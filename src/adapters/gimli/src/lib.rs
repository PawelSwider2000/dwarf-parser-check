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
pub struct DpcAddr2LineLocation {
    pub address: u64,
    pub file: *mut c_char,
    pub function_name: *mut c_char,
    pub line: u64,
    pub column: u64,
    pub has_line: u8,
    pub has_column: u8,
}

pub struct DpcAddr2LineContext {
    _bytes: &'static [u8],
    _object: object::File<'static>,
    context: Addr2LineContext,
}

fn set_last_error(message: impl Into<String>) {
    let sanitized = message.into().replace('\0', " ");
    let c_string = CString::new(sanitized).unwrap_or_else(|_| CString::new("unknown error").expect("static string"));
    LAST_ERROR.with(|slot| {
        *slot.borrow_mut() = c_string;
    });
}

fn build_context(path: &str) -> Result<DpcAddr2LineContext, String> {
    let file_bytes = fs::read(path).map_err(|error| format!("failed to read {path}: {error}"))?;
    let leaked_bytes: &'static [u8] = Box::leak(file_bytes.into_boxed_slice());
    let object = object::File::parse(leaked_bytes).map_err(|error| format!("failed to parse object file: {error}"))?;
    let context = addr2line::Context::new(&object).map_err(|error| format!("failed to build addr2line context: {error}"))?;

    Ok(DpcAddr2LineContext {
        _bytes: leaked_bytes,
        _object: object,
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
