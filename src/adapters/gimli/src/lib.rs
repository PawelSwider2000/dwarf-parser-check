use std::cell::RefCell;
use std::ffi::{CStr, CString};
use std::fs;
use std::os::raw::{c_char, c_int};
use std::ptr;

use object::{Object, ObjectSection, ObjectSymbol, SymbolKind};

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::new("no error").expect("valid error buffer"));
}

type Addr2LineContext = addr2line::Context<gimli::EndianRcSlice<gimli::RunTimeEndian>>;
const INSTRUCTION_ALIGNMENT: u64 = 16;
const MIN_INSTRUCTION_SIZE: usize = 8;

fn load_kernel_text_bytes(path: &str, kernel_name: &str) -> Result<Vec<u8>, String> {
    let file_bytes = fs::read(path).map_err(|e| format!("failed to read {path}: {e}"))?;
    let object = object::File::parse(&*file_bytes)
        .map_err(|e| format!("failed to parse ELF for text extraction: {e}"))?;

    let section_name = format!(".text.{kernel_name}");
    if let Some(section) = object.section_by_name(&section_name) {
        if let Ok(data) = section.data() {
            return Ok(data.to_vec());
        }
    }

    for symbol in object.symbols() {
        if symbol.kind() == SymbolKind::Text
            && symbol.name().ok() == Some(kernel_name)
            && symbol.size() > 0
        {
            if let Some(idx) = symbol.section_index() {
                if let Ok(section) = object.section_by_index(idx) {
                    if let Ok(data) = section.data() {
                        let start = symbol.address().saturating_sub(section.address()) as usize;
                        let end = start.saturating_add(symbol.size() as usize);
                        if end <= data.len() {
                            return Ok(data[start..end].to_vec());
                        }
                    }
                }
            }
        }
    }

    Ok(Vec::new())
}

// Returns 8 for a compacted instruction (bit 29 of first dword set), else 16.
fn instruction_step(kernel_bytes: &[u8], offset: usize) -> usize {
    if offset + 4 <= kernel_bytes.len() && (kernel_bytes[offset + 3] & 0x20) != 0 {
        MIN_INSTRUCTION_SIZE
    } else {
        INSTRUCTION_ALIGNMENT as usize
    }
}

#[repr(C)]
pub struct DpcAddr2LineKernelLocation {
    pub offset: u64,
    pub file: *mut c_char,
    pub function_name: *mut c_char,
    pub line: u64,
    pub column: u64,
    pub has_line: u8,
    pub has_column: u8,
}

#[repr(C)]
pub struct DpcAddr2LineKernelLocations {
    pub values: *mut DpcAddr2LineKernelLocation,
    pub len: usize,
}

struct ResolvedLocation {
    offset: u64,
    file: Option<String>,
    function_name: Option<String>,
    line: Option<u64>,
    column: Option<u64>,
}

fn set_last_error(message: impl Into<String>) {
    let sanitized = message.into().replace('\0', " ");
    let c_string = CString::new(sanitized).unwrap_or_else(|_| CString::new("unknown error").expect("static string"));
    LAST_ERROR.with(|slot| {
        *slot.borrow_mut() = c_string;
    });
}

fn build_context(path: &str, kernel_name: &str) -> Result<Option<Addr2LineContext>, String> {
    let file_bytes = fs::read(path).map_err(|error| format!("failed to read {path}: {error}"))?;
    let object = object::File::parse(&*file_bytes)
        .map_err(|error| format!("failed to parse object file: {error}"))?;
    let has_kernel_symbol = object
        .symbols()
        .any(|symbol| {
            symbol.kind() == SymbolKind::Text
                && symbol.size() != 0
                && symbol.name().ok() == Some(kernel_name)
        });
    if !has_kernel_symbol {
        return Ok(None);
    }

    addr2line::Context::new(&object)
        .map(Some)
        .map_err(|error| format!("failed to build addr2line context: {error}"))
}

fn canonicalize_gpu_address(address: u64) -> u64 {
    const ADDRESS_MASK: u64 = (1_u64 << 48) - 1;
    const SIGN_BIT: u64 = 1_u64 << 47;
    address & ADDRESS_MASK | if address & SIGN_BIT != 0 { !ADDRESS_MASK } else { 0 }
}

fn resolve_address(
    context: &Addr2LineContext,
    address: u64,
    offset: u64,
) -> Result<Option<ResolvedLocation>, String> {
    let found = context
        .find_location(address)
        .map_err(|error| format!("find_location failed for 0x{address:x}: {error}"))?;
    let Some(found) = found else {
        return Ok(None);
    };

    let mut function_name = None;
    let mut frames = context
        .find_frames(address)
        .skip_all_loads()
        .map_err(|error| format!("find_frames failed for 0x{address:x}: {error}"))?;
    while let Some(frame) = frames
        .next()
        .map_err(|error| format!("find_frames failed for 0x{address:x}: {error}"))?
    {
        if let Some(function) = frame.function {
            if let Ok(name) = function.raw_name() {
                function_name = Some(name.into_owned());
                break;
            }
        }
    }

    Ok(Some(ResolvedLocation {
        offset,
        file: found.file.map(str::to_owned),
        function_name,
        line: found.line.map(u64::from),
        column: found.column.map(u64::from),
    }))
}

#[no_mangle]
pub extern "C" fn dpc_addr2line_last_error() -> *const c_char {
    LAST_ERROR.with(|slot| slot.borrow().as_ptr())
}

fn into_ffi_location(location: ResolvedLocation) -> Result<DpcAddr2LineKernelLocation, String> {
    let file = location.file
        .map(CString::new)
        .transpose()
        .map_err(|error| format!("resolved file path contained a NUL byte: {error}"))?;
    let function_name = location.function_name
        .map(CString::new)
        .transpose()
        .map_err(|error| format!("resolved function name contained a NUL byte: {error}"))?;

    Ok(DpcAddr2LineKernelLocation {
        offset: location.offset,
        file: file.map_or(ptr::null_mut(), CString::into_raw),
        function_name: function_name.map_or(ptr::null_mut(), CString::into_raw),
        line: location.line.unwrap_or(0),
        column: location.column.unwrap_or(0),
        has_line: u8::from(location.line.is_some()),
        has_column: u8::from(location.column.is_some()),
    })
}

unsafe fn dispose_locations(values: &mut Vec<DpcAddr2LineKernelLocation>) {
    for location in values.drain(..) {
        if !location.file.is_null() {
            drop(CString::from_raw(location.file));
        }
        if !location.function_name.is_null() {
            drop(CString::from_raw(location.function_name));
        }
    }
}

/// Resolves every instruction address in one kernel binary range.
///
/// # Safety
/// `dwarf_path` and `mangled_kernel_name` must be valid, NUL-terminated UTF-8
/// strings. `locations` must be a valid writable pointer for the call duration.
#[no_mangle]
pub unsafe extern "C" fn dpc_addr2line_resolve_kernel(
    dwarf_path: *const c_char,
    mangled_kernel_name: *const c_char,
    runtime_kernel_address: u64,
    kernel_binary_size: usize,
    locations: *mut DpcAddr2LineKernelLocations,
) -> c_int {
    if dwarf_path.is_null() || mangled_kernel_name.is_null() || locations.is_null() {
        set_last_error("DWARF path, kernel name, or locations was null");
        return -1;
    }

    let locations = &mut *locations;
    locations.values = ptr::null_mut();
    locations.len = 0;
    let dwarf_path = match CStr::from_ptr(dwarf_path).to_str() {
        Ok(value) => value,
        Err(error) => {
            set_last_error(format!("DWARF path was not valid UTF-8: {error}"));
            return -1;
        }
    };
    let kernel_name = match CStr::from_ptr(mangled_kernel_name).to_str() {
        Ok(value) if !value.is_empty() => value,
        Ok(_) => {
            set_last_error("kernel name was empty");
            return -1;
        }
        Err(error) => {
            set_last_error(format!("kernel name was not valid UTF-8: {error}"));
            return -1;
        }
    };

    let context = match build_context(dwarf_path, kernel_name) {
        Ok(Some(context)) => context,
        Ok(None) => {
            set_last_error(format!("no text symbols matched kernel {kernel_name}"));
            return 0;
        }
        Err(error) => {
            set_last_error(error);
            return -1;
        }
    };

    let runtime_kernel_address = canonicalize_gpu_address(runtime_kernel_address);
    let kernel_binary_size = match u64::try_from(kernel_binary_size) {
        Ok(value) => value,
        Err(_) => {
            set_last_error("kernel binary size did not fit in a 64-bit address");
            return -1;
        }
    };
    let end = match runtime_kernel_address.checked_add(kernel_binary_size) {
        Some(value) => value,
        None => {
            set_last_error("kernel binary address range overflowed");
            return -1;
        }
    };
    if !runtime_kernel_address.is_multiple_of(MIN_INSTRUCTION_SIZE as u64)
        || !end.is_multiple_of(MIN_INSTRUCTION_SIZE as u64)
    {
        set_last_error("kernel binary range was not 8-byte aligned");
        return -1;
    }

    let kernel_bytes = load_kernel_text_bytes(dwarf_path, kernel_name).unwrap_or_default();

    let mut ffi_locations = Vec::new();
    let mut offset: usize = 0;
    while offset < kernel_binary_size as usize {
        let step = instruction_step(&kernel_bytes, offset);
        let address = runtime_kernel_address + offset as u64;
        match resolve_address(&context, address, offset as u64) {
            Ok(Some(location)) => match into_ffi_location(location) {
                Ok(location) => ffi_locations.push(location),
                Err(error) => {
                    dispose_locations(&mut ffi_locations);
                    set_last_error(error);
                    return -1;
                }
            },
            Ok(None) => {}
            Err(error) => {
                dispose_locations(&mut ffi_locations);
                set_last_error(error);
                return -1;
            }
        }
        offset += step;
    }

    if ffi_locations.is_empty() {
        set_last_error(format!("no source locations resolved for kernel {kernel_name}"));
        return 0;
    }

    locations.len = ffi_locations.len();
    locations.values = Box::into_raw(ffi_locations.into_boxed_slice())
        as *mut DpcAddr2LineKernelLocation;
    1
}

/// Releases a result previously returned by `dpc_addr2line_resolve_kernel`.
///
/// # Safety
/// `locations` must be null or point to a result initialized by
/// `dpc_addr2line_resolve_kernel` that has not already been disposed.
#[no_mangle]
pub unsafe extern "C" fn dpc_addr2line_kernel_locations_dispose(
    locations: *mut DpcAddr2LineKernelLocations,
) {
    if locations.is_null() {
        return;
    }

    let locations = &mut *locations;
    if locations.values.is_null() {
        return;
    }
    let mut values = Vec::from_raw_parts(locations.values, locations.len, locations.len);
    dispose_locations(&mut values);
    locations.values = ptr::null_mut();
    locations.len = 0;
}
