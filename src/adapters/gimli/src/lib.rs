use std::cell::RefCell;
use std::ffi::{CStr, CString};
use std::fs;
use std::os::raw::{c_char, c_int};
use std::ptr;

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::new("no error").expect("valid error buffer"));
}

type Addr2LineContext = addr2line::Context<gimli::EndianRcSlice<gimli::RunTimeEndian>>;
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

fn build_context(path: &str) -> Result<Addr2LineContext, String> {
    let file_bytes = fs::read(path).map_err(|error| format!("failed to read {path}: {error}"))?;
    let object = object::File::parse(&*file_bytes)
        .map_err(|error| format!("failed to parse object file: {error}"))?;
    addr2line::Context::new(&object)
        .map_err(|error| format!("failed to build addr2line context: {error}"))
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

/// Resolves the supplied addr2line address list.
///
/// # Safety
/// `dwarf_path` must be a valid, NUL-terminated UTF-8 string. `addresses` and
/// `locations` must be valid for the call duration.
#[no_mangle]
pub unsafe extern "C" fn dpc_addr2line_resolve_addresses(
    dwarf_path: *const c_char,
    kernel_base: u64,
    addresses: *const u64,
    address_count: usize,
    locations: *mut DpcAddr2LineKernelLocations,
) -> c_int {
    if dwarf_path.is_null() || addresses.is_null() || locations.is_null() {
        set_last_error("DWARF path, addresses, or locations was null");
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
    let context = match build_context(dwarf_path) {
        Ok(context) => context,
        Err(error) => {
            set_last_error(error);
            return -1;
        }
    };

    let mut ffi_locations = Vec::new();
    for &address in std::slice::from_raw_parts(addresses, address_count) {
        let Some(offset) = address.checked_sub(kernel_base) else {
            dispose_locations(&mut ffi_locations);
            set_last_error("addr2line IP was below the kernel base");
            return -1;
        };
        match resolve_address(&context, address, offset) {
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
    }

    if ffi_locations.is_empty() {
        set_last_error("no source locations resolved for supplied addr2line IPs");
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
