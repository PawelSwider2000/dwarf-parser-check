use std::cell::RefCell;
use std::collections::BTreeSet;
use std::ffi::{CStr, CString};
use std::fs;
use std::os::raw::{c_char, c_int};
use std::ptr;

use gimli::{AttributeValue, Reader};
use object::{Object, ObjectSymbol, SymbolKind};

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::new("no error").expect("valid error buffer"));
}

type Addr2LineContext = addr2line::Context<gimli::EndianRcSlice<gimli::RunTimeEndian>>;
pub type DwarfReader = gimli::EndianRcSlice<gimli::RunTimeEndian>;

#[derive(Clone)]
pub struct AddressRange {
    pub begin: u64,
    pub end: u64,
}

#[derive(Clone)]
pub struct UserSubprogram {
    pub name: String,
    pub file: String,
    pub decl_line: u64,
    pub ranges: Vec<AddressRange>,
}

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
    context: Addr2LineContext,
    pub user_subprograms: Vec<UserSubprogram>,
}

pub fn set_last_error(message: impl Into<String>) {
    let sanitized = message.into().replace('\0', " ");
    let c_string = CString::new(sanitized).unwrap_or_else(|_| CString::new("unknown error").expect("static string"));
    LAST_ERROR.with(|slot| {
        *slot.borrow_mut() = c_string;
    });
}

pub fn build_context(path: &str) -> Result<DpcAddr2LineContext, String> {
    let file_bytes = fs::read(path).map_err(|error| format!("failed to read {path}: {error}"))?;
    let leaked_bytes: &'static [u8] = Box::leak(file_bytes.into_boxed_slice());
    let object = object::File::parse(leaked_bytes).map_err(|error| format!("failed to parse object file: {error}"))?;
    let context = addr2line::Context::new(&object).map_err(|error| format!("failed to build addr2line context: {error}"))?;
    let user_subprograms = object
        .symbols()
        .filter_map(|symbol| {
            if symbol.kind() != SymbolKind::Text || symbol.size() == 0 {
                return None;
            }
            let name = symbol.name().ok()?.to_string();
            Some(UserSubprogram {
                name,
                file: String::new(),
                decl_line: 0,
                ranges: vec![AddressRange {
                    begin: symbol.address(),
                    end: symbol.address().checked_add(symbol.size())?,
                }],
            })
        })
        .collect();

    Ok(DpcAddr2LineContext {
        _bytes: leaked_bytes,
        _object: object,
        context,
        user_subprograms,
    })
}

pub fn is_system_path(path: &str) -> bool {
    ["/usr/", "/lib/", "/lib64/", "/bin/", "/sbin/", "/opt/intel/", "/opt/compiler/"]
        .iter()
        .any(|prefix| path.starts_with(prefix))
}

pub fn numeric_attr_value<R: Reader>(value: AttributeValue<R>) -> Option<u64> {
    match value {
        AttributeValue::Data1(value) => Some(value.into()),
        AttributeValue::Data2(value) => Some(value.into()),
        AttributeValue::Data4(value) => Some(value.into()),
        AttributeValue::Data8(value) => Some(value),
        AttributeValue::Udata(value) => Some(value),
        _ => None,
    }
}

pub fn find_owning_subprograms(
    subprograms: &[UserSubprogram],
    kernel_name: &str,
    file: &str,
    line: u64,
) -> Vec<UserSubprogram> {
    let matches: Vec<UserSubprogram> = subprograms
        .iter()
        .filter(|subprogram| {
            subprogram.file == file
                && subprogram.decl_line == line
        })
        .cloned()
        .collect();
    let has_kernel_wrapper = matches
        .iter()
        .any(|subprogram| subprogram.name.contains(kernel_name));
    if !has_kernel_wrapper {
        return matches;
    }

    let owner_line = subprograms
        .iter()
        .filter(|subprogram| subprogram.file == file && subprogram.decl_line < line)
        .map(|subprogram| subprogram.decl_line)
        .max();
    let Some(owner_line) = owner_line else {
        return matches;
    };

    subprograms
        .iter()
        .filter(|subprogram| subprogram.file == file && subprogram.decl_line == owner_line)
        .cloned()
        .collect()
}

pub fn insert_instruction_addresses(
    addresses: &mut BTreeSet<u64>,
    symbol_name: &str,
    ranges: &[AddressRange],
) -> Result<(), String> {
    for range in ranges {
        if range.begin % 16 != 0 || range.end % 16 != 0 {
            return Err(format!("text range for {symbol_name} was not aligned to 16 bytes"));
        }
        addresses.extend((range.begin..range.end).step_by(16));
    }
    Ok(())
}

pub fn enumerate_kernel_ips_impl(
    context: &DpcAddr2LineContext,
    kernel_name: &str,
) -> Result<Vec<u64>, String> {
    if kernel_name.is_empty() {
        return Err("kernel name was empty".to_string());
    }

    let matches: Vec<&UserSubprogram> = context
        .user_subprograms
        .iter()
        .filter(|subprogram| subprogram.name.contains(kernel_name))
        .collect();
    if matches.is_empty() {
        return Err(format!("no text symbols matched kernel {kernel_name}"));
    }

    let mut addresses = BTreeSet::new();
    for subprogram in matches {
        insert_instruction_addresses(&mut addresses, &subprogram.name, &subprogram.ranges)?;
    }
    Ok(addresses.into_iter().collect())
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

    match context.context.find_location(address) {
        Ok(Some(found)) => {
            location.file = unsafe { copy_string(found.file) };
            location.line = found.line.unwrap_or(0).into();
            location.column = found.column.unwrap_or(0).into();
            location.has_line = u8::from(found.line.is_some());
            location.has_column = u8::from(found.column.is_some());
        }
        Ok(None) => return 0,
        Err(error) => {
            set_last_error(format!("find_location failed for 0x{address:x}: {error}"));
            return -1;
        }
    }

    match context.context.find_frames(address).skip_all_loads() {
        Ok(mut frames) => {
            while let Ok(Some(frame)) = frames.next() {
                if let Some(function) = frame.function {
                    if let Ok(name) = function.raw_name() {
                        location.function_name = unsafe { copy_string(Some(name.as_ref())) };
                        break;
                    }
                }
            }
        }
        Err(error) => {
            set_last_error(format!("find_frames failed for 0x{address:x}: {error}"));
            return -1;
        }
    }

    1
}

#[no_mangle]
pub extern "C" fn dpc_addr2line_enumerate_kernel_ips(
    context: *mut DpcAddr2LineContext,
    mangled_kernel_name: *const c_char,
    runtime_kernel_address: u64,
    kernel_binary_size: usize,
    addresses: *mut DpcAddr2LineAddresses,
) -> c_int {
    if context.is_null() || mangled_kernel_name.is_null() || addresses.is_null() {
        set_last_error("context, kernel name, or addresses was null");
        return -1;
    }

    let addresses = unsafe { &mut *addresses };
    addresses.values = ptr::null_mut();
    addresses.len = 0;
    let kernel_name = match unsafe { CStr::from_ptr(mangled_kernel_name) }.to_str() {
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

    let context = unsafe { &*context };
    let has_kernel_symbol = context._object.symbols().any(|symbol| {
        symbol.kind() == SymbolKind::Text && symbol.name().ok() == Some(kernel_name)
    });
    if !has_kernel_symbol {
        set_last_error(format!("no text symbols matched kernel {kernel_name}"));
        return 0;
    }

    let end = match runtime_kernel_address.checked_add(kernel_binary_size as u64) {
        Some(value) => value,
        None => {
            set_last_error("kernel binary address range overflowed");
            return -1;
        }
    };
    if runtime_kernel_address % 16 != 0 || end % 16 != 0 {
        set_last_error("kernel binary range was not 16-byte aligned");
        return -1;
    }

    let values: Vec<u64> = (runtime_kernel_address..end).step_by(16).collect();
    addresses.len = values.len();
    addresses.values = Box::into_raw(values.into_boxed_slice()) as *mut u64;
    1
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
