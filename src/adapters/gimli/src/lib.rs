use std::borrow::Cow;
use std::cell::RefCell;
use std::collections::BTreeSet;
use std::ffi::{CStr, CString};
use std::fs;
use std::os::raw::{c_char, c_int};
use std::ptr;
use std::rc::Rc;

use gimli::Reader;
use object::read::{Object, ObjectSection, ObjectSymbol};
use object::SymbolKind;

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::new("no error").expect("valid error buffer"));
}

type Addr2LineContext = addr2line::Context<gimli::EndianRcSlice<gimli::RunTimeEndian>>;
pub(crate) type DwarfReader = gimli::EndianRcSlice<gimli::RunTimeEndian>;
type RawDwarf = gimli::Dwarf<DwarfReader>;

#[repr(C)]
pub struct DpcAddr2LineLocation {
    pub address: u64,
    pub file: *mut c_char,
    pub function_name: *mut c_char,
    pub line: u64,
    pub column: u64,
    pub has_line: u8,
    pub has_column: u8,
}

#[repr(C)]
pub struct DpcAddr2LineAddresses {
    pub values: *mut u64,
    pub len: usize,
}

pub struct DpcAddr2LineContext {
    _bytes: &'static [u8],
    _object: object::File<'static>,
    pub(crate) user_subprograms: Vec<UserSubprogram>,
    context: Addr2LineContext,
}

const INTEL_GPU_INSTRUCTION_SIZE: u64 = 16;
const SYSTEM_PATH_PREFIXES: [&str; 7] = [
    "/usr/",
    "/lib/",
    "/lib64/",
    "/bin/",
    "/sbin/",
    "/opt/intel/",
    "/opt/compiler/",
];

pub(crate) fn is_system_path(path: &str) -> bool {
    SYSTEM_PATH_PREFIXES.iter().any(|prefix| path.starts_with(prefix))
}

#[derive(Clone)]
struct SymbolCandidate {
    address: u64,
    size: u64,
    name: String,
    user_line: Option<u64>,
    user_file: Option<String>,
}

#[derive(Clone)]
pub(crate) struct AddressRange {
    pub(crate) begin: u64,
    pub(crate) end: u64,
}

#[derive(Clone)]
pub(crate) struct UserSubprogram {
    pub(crate) name: String,
    pub(crate) file: String,
    pub(crate) decl_line: u64,
    pub(crate) ranges: Vec<AddressRange>,
}

pub(crate) fn numeric_attr_value<R: Reader>(value: gimli::AttributeValue<R>) -> Option<u64> {
    match value {
        gimli::AttributeValue::Data1(value) => Some(u64::from(value)),
        gimli::AttributeValue::Data2(value) => Some(u64::from(value)),
        gimli::AttributeValue::Data4(value) => Some(u64::from(value)),
        gimli::AttributeValue::Data8(value) => Some(value),
        gimli::AttributeValue::Udata(value) => Some(value),
        gimli::AttributeValue::FileIndex(value) => Some(value),
        _ => None,
    }
}

fn attr_string_lossy(dwarf: &RawDwarf, unit: &gimli::Unit<DwarfReader>, value: gimli::AttributeValue<DwarfReader>) -> Result<String, String> {
    dwarf
        .attr_string(unit, value)
        .map_err(|error| format!("failed to read DWARF string: {error}"))?
        .to_string_lossy()
        .map(|text| text.into_owned())
        .map_err(|error| format!("failed to decode DWARF string: {error}"))
}

fn resolve_file_path(
    dwarf: &RawDwarf,
    unit: &gimli::Unit<DwarfReader>,
    file_index: u64,
) -> Result<Option<String>, String> {
    let Some(program) = unit.line_program.as_ref() else {
        return Ok(None);
    };

    let header = program.header();
    let Some(file) = header.file(file_index) else {
        return Ok(None);
    };

    let path_name = attr_string_lossy(dwarf, unit, file.path_name())?;
    if path_name.is_empty() {
        return Ok(None);
    }
    if path_name.starts_with('/') {
        return Ok(Some(path_name));
    }

    let directory = file
        .directory(header)
        .map(|value| attr_string_lossy(dwarf, unit, value))
        .transpose()?;

    match directory {
        Some(prefix) if !prefix.is_empty() => Ok(Some(format!("{prefix}/{path_name}"))),
        _ => Ok(Some(path_name)),
    }
}

fn build_user_subprogram_index(dwarf: &RawDwarf) -> Result<Vec<UserSubprogram>, String> {
    let mut subprograms = Vec::new();
    let mut units = dwarf.units();

    while let Some(header) = units
        .next()
        .map_err(|error| format!("failed to iterate DWARF units: {error}"))?
    {
        let unit = dwarf
            .unit(header)
            .map_err(|error| format!("failed to parse DWARF unit: {error}"))?;
        let mut entries = unit.entries();

        while let Some((_, entry)) = entries
            .next_dfs()
            .map_err(|error| format!("failed to walk DWARF entries: {error}"))?
        {
            if entry.tag() != gimli::DW_TAG_subprogram {
                continue;
            }

            let Some(file_index) = entry
                .attr_value(gimli::DW_AT_decl_file)
                .map_err(|error| format!("failed to read DW_AT_decl_file: {error}"))?
                .and_then(numeric_attr_value)
            else {
                continue;
            };

            let Some(decl_line) = entry
                .attr_value(gimli::DW_AT_decl_line)
                .map_err(|error| format!("failed to read DW_AT_decl_line: {error}"))?
                .and_then(numeric_attr_value)
                .filter(|line| *line != 0)
            else {
                continue;
            };

            let Some(file) = resolve_file_path(dwarf, &unit, file_index)? else {
                continue;
            };
            if file.is_empty() || is_system_path(&file) {
                continue;
            }

            let mut ranges = dwarf
                .die_ranges(&unit, entry)
                .map_err(|error| format!("failed to read subprogram ranges: {error}"))?;
            let mut collected_ranges = Vec::new();
            while let Some(range) = ranges
                .next()
                .map_err(|error| format!("failed to iterate subprogram ranges: {error}"))?
            {
                if range.end <= range.begin {
                    continue;
                }

                collected_ranges.push(AddressRange {
                    begin: range.begin,
                    end: range.end,
                });
            }

            if collected_ranges.is_empty() {
                continue;
            }

            let name = entry
                .attr_value(gimli::DW_AT_linkage_name)
                .map_err(|error| format!("failed to read DW_AT_linkage_name: {error}"))?
                .map(|value| attr_string_lossy(dwarf, &unit, value))
                .transpose()?
                .or_else(|| {
                    entry
                        .attr_value(gimli::DW_AT_name)
                        .ok()
                        .flatten()
                        .and_then(|value| attr_string_lossy(dwarf, &unit, value).ok())
                })
                .unwrap_or_default();

            subprograms.push(UserSubprogram {
                name,
                file,
                decl_line,
                ranges: collected_ranges,
            });
        }
    }

    subprograms.sort_by(|left, right| {
        left.file
            .cmp(&right.file)
            .then_with(|| left.decl_line.cmp(&right.decl_line))
            .then_with(|| left.name.cmp(&right.name))
    });
    Ok(subprograms)
}

pub(crate) fn find_owning_subprograms(
    subprograms: &[UserSubprogram],
    kernel_name: &str,
    file: &str,
    line: u64,
) -> Vec<UserSubprogram> {
    let matching_subprograms: Vec<&UserSubprogram> = subprograms
        .iter()
        .filter(|candidate| candidate.file == file)
        .collect();

    let has_kernel_wrapper_at_line = matching_subprograms.iter().any(|candidate| {
        candidate.decl_line == line && candidate.name.contains(kernel_name)
    });

    let target_decl_line = if has_kernel_wrapper_at_line {
        matching_subprograms
            .iter()
            .filter(|candidate| candidate.decl_line < line)
            .map(|candidate| candidate.decl_line)
            .max()
    } else {
        matching_subprograms
            .iter()
            .filter(|candidate| candidate.decl_line <= line)
            .map(|candidate| candidate.decl_line)
            .max()
    };

    let Some(best_decl_line) = target_decl_line else {
        return Vec::new();
    };

    matching_subprograms
        .into_iter()
        .filter(|candidate| candidate.decl_line == best_decl_line)
        .cloned()
        .collect()
}

pub(crate) fn insert_instruction_addresses(addresses: &mut BTreeSet<u64>, name: &str, ranges: &[AddressRange]) -> Result<(), String> {
    for range in ranges {
        let size = range.end - range.begin;
        if size == 0 {
            addresses.insert(range.begin);
            continue;
        }

        if size % INTEL_GPU_INSTRUCTION_SIZE != 0 {
            return Err(format!(
                "range for {name} has size {size} which is not aligned to {INTEL_GPU_INSTRUCTION_SIZE}-byte instructions"
            ));
        }

        let instruction_count = size / INTEL_GPU_INSTRUCTION_SIZE;
        for index in 0..instruction_count {
            addresses.insert(range.begin + (index * INTEL_GPU_INSTRUCTION_SIZE));
        }
    }

    Ok(())
}

fn resolve_frame_metadata(
    context: &Addr2LineContext,
    address: u64,
) -> Result<(Option<String>, Option<u64>, Option<u64>, Option<String>), String> {
    let mut function_name: Option<String> = None;
    let mut first_frame_location: Option<(String, u64, Option<u64>)> = None;
    let mut preferred_frame_location: Option<(String, u64, Option<u64>)> = None;

    let mut frames = context
        .find_frames(address)
        .skip_all_loads()
        .map_err(|error| format!("find_frames failed for 0x{address:x}: {error}"))?;

    while let Some(frame) = frames
        .next()
        .map_err(|error| format!("find_frames iteration failed for 0x{address:x}: {error}"))?
    {
        if function_name.is_none() {
            if let Some(function) = frame.function {
                if let Ok(name) = function.raw_name() {
                    function_name = Some(name.into_owned());
                }
            }
        }

        if let Some(frame_location) = frame.location {
            let Some(line) = frame_location.line.filter(|line| *line != 0) else {
                continue;
            };

            let file = frame_location.file.unwrap_or_default().to_string();
            let column = frame_location.column.map(Into::into);

            if first_frame_location.is_none() {
                first_frame_location = Some((file.clone(), line.into(), column));
            }

            if !file.is_empty() && !is_system_path(&file) {
                preferred_frame_location = Some((file, line.into(), column));
            }
        }
    }

    let (file, line, column) = preferred_frame_location
        .or(first_frame_location)
        .map_or((None, None, None), |(file, line, column)| {
            (Some(file), Some(line), column)
        });

    Ok((file, line, column, function_name))
}

pub(crate) fn enumerate_kernel_ips_impl(
    context: &DpcAddr2LineContext,
    kernel_name: &str,
) -> Result<Vec<u64>, String> {
    if kernel_name.is_empty() {
        return Err("kernel name was empty".to_string());
    }

    let mut addresses = BTreeSet::new();
    let mut matched_symbol = false;
    let mut kernel_candidates = Vec::new();
    let mut selected_subprograms = Vec::new();

    for symbol in context._object.symbols() {
        let Ok(symbol_name) = symbol.name() else {
            continue;
        };

        if symbol.kind() != SymbolKind::Text {
            continue;
        }

        let address = symbol.address();
        if address == 0 {
            continue;
        }

        let (user_file, user_line, _, _) = resolve_frame_metadata(&context.context, address)?;
        let user_file = user_file.filter(|file| !file.is_empty() && !is_system_path(file));

        let candidate = SymbolCandidate {
            address,
            size: symbol.size(),
            name: symbol_name.to_string(),
            user_line,
            user_file,
        };

        if !symbol_name.contains(kernel_name) {
            continue;
        }

        matched_symbol = true;
        if let (Some(file), Some(line)) = (candidate.user_file.as_deref(), candidate.user_line) {
            selected_subprograms.extend(find_owning_subprograms(
                &context.user_subprograms,
                kernel_name,
                file,
                line,
            ));
        }
        kernel_candidates.push(candidate);
    }

    if !matched_symbol {
        return Err(format!("no text symbols matched kernel '{kernel_name}'"));
    }

    selected_subprograms.sort_by(|left, right| {
        left.file
            .cmp(&right.file)
            .then_with(|| left.decl_line.cmp(&right.decl_line))
            .then_with(|| left.name.cmp(&right.name))
    });
    selected_subprograms.dedup_by(|left, right| {
        left.file == right.file && left.decl_line == right.decl_line && left.name == right.name
    });

    if !selected_subprograms.is_empty() {
        for subprogram in &selected_subprograms {
            insert_instruction_addresses(&mut addresses, &subprogram.name, &subprogram.ranges)?;
        }
    } else {
        for candidate in kernel_candidates {
            let size = if candidate.size == 0 {
                addresses.insert(candidate.address);
                continue;
            } else {
                candidate.size
            };

            if size % INTEL_GPU_INSTRUCTION_SIZE != 0 {
                return Err(format!(
                    "symbol {} size {} is not aligned to {INTEL_GPU_INSTRUCTION_SIZE}-byte instructions",
                    candidate.name,
                    size,
                ));
            }

            let instruction_count = size / INTEL_GPU_INSTRUCTION_SIZE;
            for index in 0..instruction_count {
                addresses.insert(candidate.address + (index * INTEL_GPU_INSTRUCTION_SIZE));
            }
        }
    }

    if addresses.is_empty() {
        return Err(format!("no DWARF locations found for kernel '{kernel_name}'"));
    }

    Ok(addresses.into_iter().collect())
}

pub(crate) fn set_last_error(message: impl Into<String>) {
    let sanitized = message.into().replace('\0', " ");
    let c_string = CString::new(sanitized).unwrap_or_else(|_| CString::new("unknown error").expect("static string"));
    LAST_ERROR.with(|slot| {
        *slot.borrow_mut() = c_string;
    });
}

pub(crate) fn build_context(path: &str) -> Result<DpcAddr2LineContext, String> {
    let file_bytes = fs::read(path).map_err(|error| format!("failed to read {path}: {error}"))?;
    let leaked_bytes: &'static [u8] = Box::leak(file_bytes.into_boxed_slice());
    let object = object::File::parse(leaked_bytes).map_err(|error| format!("failed to parse object file: {error}"))?;
    let endian = if object.is_little_endian() {
        gimli::RunTimeEndian::Little
    } else {
        gimli::RunTimeEndian::Big
    };
    let dwarf = gimli::Dwarf::load(|id| {
        let data = object
            .section_by_name(id.name())
            .and_then(|section| section.uncompressed_data().ok())
            .unwrap_or(Cow::Borrowed(&[]));
        Ok::<_, gimli::Error>(gimli::EndianRcSlice::new(Rc::from(&*data), endian))
    })
    .map_err(|error| format!("failed to load DWARF sections: {error}"))?;
    let user_subprograms = build_user_subprogram_index(&dwarf)?;
    let context = addr2line::Context::new(&object).map_err(|error| format!("failed to build addr2line context: {error}"))?;

    Ok(DpcAddr2LineContext {
        _bytes: leaked_bytes,
        _object: object,
        user_subprograms,
        context,
    })
}

unsafe fn copy_string(value: Option<&str>) -> *mut c_char {
    match value {
        Some(text) if !text.is_empty() => CString::new(text)
            .map(CString::into_raw)
            .unwrap_or(ptr::null_mut()),
        _ => ptr::null_mut(),
    }
}

#[no_mangle]
pub extern "C" fn dpc_addr2line_last_error() -> *const c_char {
    LAST_ERROR.with(|slot| slot.borrow().as_ptr())
}

#[no_mangle]
pub extern "C" fn dpc_addr2line_context_new(path: *const c_char) -> *mut DpcAddr2LineContext {
    if path.is_null() {
        set_last_error("path was null");
        return ptr::null_mut();
    }

    let path = unsafe { CStr::from_ptr(path) };
    let path = match path.to_str() {
        Ok(value) => value,
        Err(error) => {
            set_last_error(format!("path was not valid UTF-8: {error}"));
            return ptr::null_mut();
        }
    };

    match build_context(path) {
        Ok(context) => Box::into_raw(Box::new(context)),
        Err(error) => {
            set_last_error(error);
            ptr::null_mut()
        }
    }
}

#[no_mangle]
pub extern "C" fn dpc_addr2line_context_free(context: *mut DpcAddr2LineContext) {
    if context.is_null() {
        return;
    }

    unsafe {
        drop(Box::from_raw(context));
    }
}

#[no_mangle]
pub extern "C" fn dpc_addr2line_resolve_address(
    context: *mut DpcAddr2LineContext,
    address: u64,
    location: *mut DpcAddr2LineLocation,
) -> c_int {
    if context.is_null() || location.is_null() {
        set_last_error("context or location was null");
        return -1;
    }

    let context = unsafe { &mut *context };
    let location = unsafe { &mut *location };

    *location = DpcAddr2LineLocation {
        address,
        file: ptr::null_mut(),
        function_name: ptr::null_mut(),
        line: 0,
        column: 0,
        has_line: 0,
        has_column: 0,
    };

    let (selected_file, selected_line, selected_column, function_name) =
        match resolve_frame_metadata(&context.context, address) {
            Ok(items) => items,
            Err(error) => {
                set_last_error(error);
                return -1;
            }
        };

    let Some(line) = selected_line else {
        return 0;
    };

    location.file = unsafe { copy_string(selected_file.as_deref()) };
    location.line = line;
    location.column = selected_column.unwrap_or(0);
    location.has_line = 1;
    location.has_column = u8::from(selected_column.is_some());
    location.function_name = unsafe { copy_string(function_name.as_deref()) };

    1
}

#[no_mangle]
pub extern "C" fn dpc_addr2line_enumerate_kernel_ips(
    context: *mut DpcAddr2LineContext,
    kernel_name: *const c_char,
    addresses: *mut DpcAddr2LineAddresses,
) -> c_int {
    if context.is_null() || kernel_name.is_null() || addresses.is_null() {
        set_last_error("context, kernel name, or addresses was null");
        return -1;
    }

    let context = unsafe { &mut *context };
    let kernel_name = unsafe { CStr::from_ptr(kernel_name) };
    let kernel_name = match kernel_name.to_str() {
        Ok(value) => value,
        Err(error) => {
            set_last_error(format!("kernel name was not valid UTF-8: {error}"));
            return -1;
        }
    };

    let addresses = unsafe { &mut *addresses };
    *addresses = DpcAddr2LineAddresses {
        values: ptr::null_mut(),
        len: 0,
    };

    match enumerate_kernel_ips_impl(context, kernel_name) {
        Ok(items) => {
            let mut items = items.into_boxed_slice();
            addresses.len = items.len();
            addresses.values = items.as_mut_ptr();
            std::mem::forget(items);
            1
        }
        Err(error) => {
            set_last_error(error);
            0
        }
    }
}

#[no_mangle]
pub extern "C" fn dpc_addr2line_location_dispose(location: *mut DpcAddr2LineLocation) {
    if location.is_null() {
        return;
    }

    let location = unsafe { &mut *location };

    if !location.file.is_null() {
        unsafe {
            drop(CString::from_raw(location.file));
        }
        location.file = ptr::null_mut();
    }

    if !location.function_name.is_null() {
        unsafe {
            drop(CString::from_raw(location.function_name));
        }
        location.function_name = ptr::null_mut();
    }
}

#[no_mangle]
pub extern "C" fn dpc_addr2line_addresses_dispose(addresses: *mut DpcAddr2LineAddresses) {
    if addresses.is_null() {
        return;
    }

    let addresses = unsafe { &mut *addresses };

    if !addresses.values.is_null() {
        unsafe {
            drop(Vec::from_raw_parts(addresses.values, addresses.len, addresses.len));
        }
        addresses.values = ptr::null_mut();
        addresses.len = 0;
    }
}
