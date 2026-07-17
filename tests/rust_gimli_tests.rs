use std::ffi::{CStr, CString};
use std::path::PathBuf;
use std::ptr;

use dpc_addr2line::{
    dpc_addr2line_addresses_dispose, dpc_addr2line_context_free, dpc_addr2line_context_new,
    dpc_addr2line_enumerate_kernel_ips, dpc_addr2line_last_error, dpc_addr2line_location_dispose,
    dpc_addr2line_resolve_address, DpcAddr2LineAddresses, DpcAddr2LineLocation,
};

fn sample_dwarf_path() -> CString {
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../../artifacts/_ZTSN12_GLOBAL__N_117PrimaryGEMMKernelE.dwarf");
    CString::new(path.to_string_lossy().into_owned()).expect("sample path should not contain NUL")
}

fn last_error_text() -> String {
    unsafe {
        let error = dpc_addr2line_last_error();
        if error.is_null() {
            return String::new();
        }
        CStr::from_ptr(error).to_string_lossy().into_owned()
    }
}

fn sample_context() -> *mut dpc_addr2line::DpcAddr2LineContext {
    let path = sample_dwarf_path();
    let context = dpc_addr2line_context_new(path.as_ptr());
    assert!(!context.is_null(), "{}", last_error_text());
    context
}

fn empty_location() -> DpcAddr2LineLocation {
    DpcAddr2LineLocation {
        address: 0,
        file: ptr::null_mut(),
        function_name: ptr::null_mut(),
        line: 0,
        column: 0,
        has_line: 0,
        has_column: 0,
    }
}

fn resolve_address(context: *mut dpc_addr2line::DpcAddr2LineContext, address: u64) -> DpcAddr2LineLocation {
    let mut location = empty_location();
    let status = dpc_addr2line_resolve_address(context, address, &mut location);
    assert_eq!(status, 1, "{}", last_error_text());
    location
}

#[test]
fn null_context_path_reports_error() {
    let context = dpc_addr2line_context_new(ptr::null());

    assert!(context.is_null());
    assert!(last_error_text().contains("path was null"));
}

#[test]
fn invalid_context_path_reports_error() {
    let path = CString::new("/definitely/missing/file.dwarf").expect("static string should be valid");

    let context = dpc_addr2line_context_new(path.as_ptr());

    assert!(context.is_null());
    assert!(last_error_text().contains("failed to read"));
}

#[test]
fn invalid_utf8_path_reports_error() {
    let invalid_path = CStr::from_bytes_with_nul(b"\xff\0").expect("bytes should be NUL terminated");

    let context = dpc_addr2line_context_new(invalid_path.as_ptr());

    assert!(context.is_null());
    assert!(last_error_text().contains("valid UTF-8"));
}

#[test]
fn sample_dwarf_primary_kernel_enumerates_real_user_body() {
    let kernel = CString::new("_ZTSN12_GLOBAL__N_117PrimaryGEMMKernelE")
        .expect("static string should be valid");
    let context = sample_context();

    let mut addresses = DpcAddr2LineAddresses {
        values: std::ptr::null_mut(),
        len: 0,
    };
    let status = dpc_addr2line_enumerate_kernel_ips(context, kernel.as_ptr(), &mut addresses);

    assert_eq!(status, 1, "{}", last_error_text());
    assert!(addresses.len > 0);

    let values = unsafe { std::slice::from_raw_parts(addresses.values, addresses.len) };
    assert_eq!(values[0], 0xffff8000fff80000);

    dpc_addr2line_addresses_dispose(&mut addresses);
    dpc_addr2line_context_free(context);
}

#[test]
fn null_enumeration_arguments_are_rejected() {
    let kernel = CString::new("_ZTSN12_GLOBAL__N_117PrimaryGEMMKernelE")
        .expect("static string should be valid");
    let context = sample_context();
    let mut addresses = DpcAddr2LineAddresses {
        values: ptr::null_mut(),
        len: 0,
    };

    assert_eq!(dpc_addr2line_enumerate_kernel_ips(ptr::null_mut(), kernel.as_ptr(), &mut addresses), -1);
    assert!(last_error_text().contains("was null"));

    assert_eq!(dpc_addr2line_enumerate_kernel_ips(context, ptr::null(), &mut addresses), -1);
    assert!(last_error_text().contains("was null"));

    assert_eq!(dpc_addr2line_enumerate_kernel_ips(context, kernel.as_ptr(), ptr::null_mut()), -1);
    assert!(last_error_text().contains("was null"));

    dpc_addr2line_context_free(context);
}

#[test]
fn unknown_kernel_returns_no_addresses_with_descriptive_error() {
    let kernel = CString::new("DefinitelyMissingKernel").expect("static string should be valid");
    let context = sample_context();
    let mut addresses = DpcAddr2LineAddresses {
        values: ptr::null_mut(),
        len: 0,
    };

    let status = dpc_addr2line_enumerate_kernel_ips(context, kernel.as_ptr(), &mut addresses);

    assert_eq!(status, 0);
    assert_eq!(addresses.len, 0);
    assert!(last_error_text().contains("no text symbols matched kernel"));

    dpc_addr2line_context_free(context);
}

#[test]
fn invalid_utf8_kernel_name_is_rejected() {
    let kernel = CStr::from_bytes_with_nul(b"\xff\0").expect("bytes should be NUL terminated");
    let context = sample_context();
    let mut addresses = DpcAddr2LineAddresses {
        values: ptr::null_mut(),
        len: 0,
    };

    let status = dpc_addr2line_enumerate_kernel_ips(context, kernel.as_ptr(), &mut addresses);

    assert_eq!(status, -1);
    assert!(last_error_text().contains("kernel name was not valid UTF-8"));

    dpc_addr2line_context_free(context);
}

#[test]
fn sample_dwarf_primary_kernel_ip_resolves_to_source() {
    let context = sample_context();

    let mut location = resolve_address(context, 0xffff8000fff80000);
    assert_eq!(location.has_line, 1);
    assert_eq!(location.line, 202);

    let file = unsafe { CStr::from_ptr(location.file) }.to_string_lossy().into_owned();
    assert!(file.ends_with("simple_sycl_vtune.cpp"));

    dpc_addr2line_location_dispose(&mut location);
    dpc_addr2line_context_free(context);
}

#[test]
fn null_resolution_arguments_are_rejected() {
    let context = sample_context();
    let mut location = empty_location();

    assert_eq!(dpc_addr2line_resolve_address(ptr::null_mut(), 0xffff8000fff80000, &mut location), -1);
    assert!(last_error_text().contains("context or location was null"));

    assert_eq!(dpc_addr2line_resolve_address(context, 0xffff8000fff80000, ptr::null_mut()), -1);
    assert!(last_error_text().contains("context or location was null"));

    dpc_addr2line_context_free(context);
}

#[test]
fn unknown_address_returns_no_location() {
    let context = sample_context();
    let mut location = empty_location();

    let status = dpc_addr2line_resolve_address(context, 0x1, &mut location);

    assert_eq!(status, 0);
    assert!(location.file.is_null());
    assert!(location.function_name.is_null());

    dpc_addr2line_context_free(context);
}