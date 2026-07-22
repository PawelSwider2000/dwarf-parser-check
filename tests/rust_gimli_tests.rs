use std::ffi::{CStr, CString};
use std::path::PathBuf;
use std::ptr;

use dpc_addr2line::{
    dpc_addr2line_kernel_locations_dispose, dpc_addr2line_last_error, dpc_addr2line_resolve_kernel,
    DpcAddr2LineKernelLocations,
};

fn sample_dwarf_path() -> CString {
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../../artifacts/_ZTSN12_GLOBAL__N_117PrimaryGEMMKernelE.dwarf");
    CString::new(path.to_string_lossy().into_owned()).expect("sample path should not contain NUL")
}

fn primary_kernel_name() -> CString {
    CString::new("_ZTSN12_GLOBAL__N_117PrimaryGEMMKernelE")
        .expect("static kernel name should not contain NUL")
}

fn empty_locations() -> DpcAddr2LineKernelLocations {
    DpcAddr2LineKernelLocations {
        values: ptr::null_mut(),
        len: 0,
    }
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

#[test]
fn null_resolution_arguments_are_rejected() {
    let dwarf_path = sample_dwarf_path();
    let kernel_name = primary_kernel_name();
    let mut locations = empty_locations();

    assert_eq!(
        unsafe {
            dpc_addr2line_resolve_kernel(
                ptr::null(),
                kernel_name.as_ptr(),
                0xffff8000fff80000,
                81152,
                &mut locations,
            )
        },
        -1
    );
    assert!(last_error_text().contains("was null"));

    assert_eq!(
        unsafe {
            dpc_addr2line_resolve_kernel(
                dwarf_path.as_ptr(),
                ptr::null(),
                0xffff8000fff80000,
                81152,
                &mut locations,
            )
        },
        -1
    );
    assert!(last_error_text().contains("was null"));

    assert_eq!(
        unsafe {
            dpc_addr2line_resolve_kernel(
                dwarf_path.as_ptr(),
                kernel_name.as_ptr(),
                0xffff8000fff80000,
                81152,
                ptr::null_mut(),
            )
        },
        -1
    );
    assert!(last_error_text().contains("was null"));
}

#[test]
fn invalid_dwarf_path_reports_error() {
    let dwarf_path = CString::new("/definitely/missing/file.dwarf").expect("static path should be valid");
    let kernel_name = primary_kernel_name();
    let mut locations = empty_locations();

    assert_eq!(
        unsafe {
            dpc_addr2line_resolve_kernel(
                dwarf_path.as_ptr(),
                kernel_name.as_ptr(),
                0xffff8000fff80000,
                81152,
                &mut locations,
            )
        },
        -1
    );
    assert!(last_error_text().contains("failed to read"));
}

#[test]
fn invalid_utf8_arguments_are_rejected() {
    let dwarf_path = sample_dwarf_path();
    let kernel_name = primary_kernel_name();
    let invalid_value = c"\xff";
    let mut locations = empty_locations();

    assert_eq!(
        unsafe {
            dpc_addr2line_resolve_kernel(
                invalid_value.as_ptr(),
                kernel_name.as_ptr(),
                0xffff8000fff80000,
                81152,
                &mut locations,
            )
        },
        -1
    );
    assert!(last_error_text().contains("DWARF path was not valid UTF-8"));

    assert_eq!(
        unsafe {
            dpc_addr2line_resolve_kernel(
                dwarf_path.as_ptr(),
                invalid_value.as_ptr(),
                0xffff8000fff80000,
                81152,
                &mut locations,
            )
        },
        -1
    );
    assert!(last_error_text().contains("kernel name was not valid UTF-8"));
}

#[test]
fn unknown_kernel_returns_no_locations_with_descriptive_error() {
    let dwarf_path = sample_dwarf_path();
    let kernel_name = CString::new("DefinitelyMissingKernel").expect("static kernel name should be valid");
    let mut locations = empty_locations();

    assert_eq!(
        unsafe {
            dpc_addr2line_resolve_kernel(
                dwarf_path.as_ptr(),
                kernel_name.as_ptr(),
                0xffff8000fff80000,
                81152,
                &mut locations,
            )
        },
        0
    );
    assert_eq!(locations.len, 0);
    assert!(last_error_text().contains("no text symbols matched kernel"));
}

#[test]
fn sample_dwarf_whole_kernel_resolves_to_source_and_returns_offsets() {
    let dwarf_path = sample_dwarf_path();
    let kernel_name = primary_kernel_name();
    let mut locations = empty_locations();

    assert_eq!(
        unsafe {
            dpc_addr2line_resolve_kernel(
                dwarf_path.as_ptr(),
                kernel_name.as_ptr(),
                0xffff8000fff80000,
                81152,
                &mut locations,
            )
        },
        1,
        "{}",
        last_error_text()
    );
    assert!(locations.len > 0);

    let values = unsafe { std::slice::from_raw_parts(locations.values, locations.len) };
    assert_eq!(values[0].offset, 0);
    assert_eq!(values[0].has_line, 1);
    assert_eq!(values[0].line, 213);
    let file = unsafe { CStr::from_ptr(values[0].file) }.to_string_lossy().into_owned();
    assert!(file.ends_with("simple_sycl_vtune.cpp"));

    unsafe { dpc_addr2line_kernel_locations_dispose(&mut locations) };
    assert!(locations.values.is_null());
    assert_eq!(locations.len, 0);
}
