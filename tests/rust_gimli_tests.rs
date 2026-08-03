use std::ffi::{CStr, CString};
use std::path::PathBuf;
use std::ptr;

use dpc_addr2line::{
    dpc_addr2line_kernel_locations_dispose, dpc_addr2line_last_error,
    dpc_addr2line_resolve_addresses, DpcAddr2LineKernelLocations,
};

fn sample_dwarf_path() -> CString {
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../../artifacts/results/gemm/g-O2/_ZTSN12_GLOBAL__N_117PrimaryGEMMKernelE.dwarf");
    CString::new(path.to_string_lossy().into_owned()).expect("sample path should not contain NUL")
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
    let addresses = [0xffff8000ffbb0980];
    let mut locations = empty_locations();

    assert_eq!(
        unsafe {
            dpc_addr2line_resolve_addresses(
                ptr::null(), 0xffff8000ffbb0900, addresses.as_ptr(), addresses.len(), &mut locations,
            )
        },
        -1
    );
    assert!(last_error_text().contains("was null"));

    assert_eq!(
        unsafe {
            dpc_addr2line_resolve_addresses(
                dwarf_path.as_ptr(), 0xffff8000ffbb0900, ptr::null(), addresses.len(), &mut locations,
            )
        },
        -1
    );
    assert!(last_error_text().contains("was null"));

    assert_eq!(
        unsafe {
            dpc_addr2line_resolve_addresses(
                dwarf_path.as_ptr(), 0xffff8000ffbb0900, addresses.as_ptr(), addresses.len(), ptr::null_mut(),
            )
        },
        -1
    );
    assert!(last_error_text().contains("was null"));
}

#[test]
fn invalid_dwarf_path_reports_error() {
    let dwarf_path = CString::new("/definitely/missing/file.dwarf").expect("static path should be valid");
    let addresses = [0xffff8000ffbb0980];
    let mut locations = empty_locations();

    assert_eq!(
        unsafe {
            dpc_addr2line_resolve_addresses(
                dwarf_path.as_ptr(), 0xffff8000ffbb0900, addresses.as_ptr(), addresses.len(), &mut locations,
            )
        },
        -1
    );
    assert!(last_error_text().contains("failed to read"));
}

#[test]
fn invalid_utf8_dwarf_path_is_rejected() {
    let invalid_value = c"\xff";
    let addresses = [0xffff8000ffbb0980];
    let mut locations = empty_locations();

    assert_eq!(
        unsafe {
            dpc_addr2line_resolve_addresses(
                invalid_value.as_ptr(), 0xffff8000ffbb0900, addresses.as_ptr(), addresses.len(), &mut locations,
            )
        },
        -1
    );
    assert!(last_error_text().contains("DWARF path was not valid UTF-8"));
}

#[test]
fn supplied_address_resolves_to_source_and_returns_its_offset() {
    let dwarf_path = sample_dwarf_path();
    if !PathBuf::from(dwarf_path.to_str().expect("sample path should be UTF-8")).exists() {
        return;
    }

    let addresses = [0xffff8000ffbb0980];
    let mut locations = empty_locations();
    assert_eq!(
        unsafe {
            dpc_addr2line_resolve_addresses(
                dwarf_path.as_ptr(), 0xffff8000ffbb0900, addresses.as_ptr(), addresses.len(), &mut locations,
            )
        },
        1,
        "{}",
        last_error_text()
    );
    assert_eq!(locations.len, 1);

    let values = unsafe { std::slice::from_raw_parts(locations.values, locations.len) };
    assert_eq!(values[0].offset, 0x80);
    assert_eq!(values[0].has_line, 1);
    let file = unsafe { CStr::from_ptr(values[0].file) }.to_string_lossy().into_owned();
    assert!(file.ends_with("gemm_main.cpp"));

    unsafe { dpc_addr2line_kernel_locations_dispose(&mut locations) };
    assert!(locations.values.is_null());
    assert_eq!(locations.len, 0);
}