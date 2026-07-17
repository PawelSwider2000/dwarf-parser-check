#[path = "../src/adapters/gimli/src/lib.rs"]
mod rust_impl;

use std::collections::BTreeSet;
use std::path::PathBuf;

use gimli::AttributeValue;
use rust_impl::{
    build_context, dpc_addr2line_last_error, enumerate_kernel_ips_impl, find_owning_subprograms,
    insert_instruction_addresses, is_system_path, numeric_attr_value, set_last_error,
    AddressRange, UserSubprogram,
};

fn sample_dwarf_path() -> String {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../../dwarf_files/PrimaryGEMMKernel.dwarf")
        .to_string_lossy()
        .into_owned()
}

fn sample_range(begin: u64, end: u64) -> AddressRange {
    AddressRange { begin, end }
}

#[test]
fn system_path_detection_matches_expected_prefixes() {
    assert!(is_system_path("/usr/include/c++/v1/vector"));
    assert!(is_system_path("/opt/intel/oneapi/compiler/latest/include/foo.hpp"));
    assert!(!is_system_path("/localdisk/project/main.cc"));
    assert!(!is_system_path("main.cc"));
}

#[test]
fn numeric_attr_value_accepts_supported_forms() {
    assert_eq!(numeric_attr_value::<rust_impl::DwarfReader>(AttributeValue::Data1(7)), Some(7));
    assert_eq!(numeric_attr_value::<rust_impl::DwarfReader>(AttributeValue::Data2(70)), Some(70));
    assert_eq!(numeric_attr_value::<rust_impl::DwarfReader>(AttributeValue::Data4(700)), Some(700));
    assert_eq!(numeric_attr_value::<rust_impl::DwarfReader>(AttributeValue::Data8(7000)), Some(7000));
    assert_eq!(numeric_attr_value::<rust_impl::DwarfReader>(AttributeValue::Udata(77)), Some(77));
}

#[test]
fn numeric_attr_value_rejects_non_numeric_forms() {
    assert_eq!(
        numeric_attr_value::<rust_impl::DwarfReader>(AttributeValue::Addr(0x10)),
        None
    );
}

#[test]
fn owning_subprograms_step_back_from_same_line_kernel_wrapper() {
    let subprograms = vec![
        UserSubprogram {
            name: "_Z4GEMM".to_string(),
            file: "/tmp/main.cc".to_string(),
            decl_line: 63,
            ranges: vec![sample_range(0x100, 0x140)],
        },
        UserSubprogram {
            name: "_ZTS17PrimaryGEMMKernel".to_string(),
            file: "/tmp/main.cc".to_string(),
            decl_line: 100,
            ranges: vec![sample_range(0x200, 0x240)],
        },
        UserSubprogram {
            name: "_ZTS19SecondaryGEMMKernel".to_string(),
            file: "/tmp/main.cc".to_string(),
            decl_line: 100,
            ranges: vec![sample_range(0x300, 0x340)],
        },
    ];

    let owners = find_owning_subprograms(&subprograms, "PrimaryGEMM", "/tmp/main.cc", 100);

    assert_eq!(owners.len(), 1);
    assert_eq!(owners[0].name, "_Z4GEMM");
    assert_eq!(owners[0].decl_line, 63);
}

#[test]
fn owning_subprograms_return_all_matches_for_direct_line_lookup() {
    let subprograms = vec![
        UserSubprogram {
            name: "first".to_string(),
            file: "/tmp/main.cc".to_string(),
            decl_line: 63,
            ranges: vec![sample_range(0x100, 0x140)],
        },
        UserSubprogram {
            name: "second".to_string(),
            file: "/tmp/main.cc".to_string(),
            decl_line: 63,
            ranges: vec![sample_range(0x200, 0x240)],
        },
    ];

    let owners = find_owning_subprograms(&subprograms, "PrimaryGEMM", "/tmp/main.cc", 63);

    assert_eq!(owners.len(), 2);
    assert_eq!(owners[0].decl_line, 63);
    assert_eq!(owners[1].decl_line, 63);
}

#[test]
fn insert_instruction_addresses_enumerates_multiple_ranges() {
    let mut addresses = BTreeSet::new();

    insert_instruction_addresses(
        &mut addresses,
        "kernel_body",
        &[sample_range(0x100, 0x130), sample_range(0x200, 0x220)],
    )
    .expect("aligned ranges should enumerate");

    assert_eq!(
        addresses.into_iter().collect::<Vec<_>>(),
        vec![0x100, 0x110, 0x120, 0x200, 0x210]
    );
}

#[test]
fn insert_instruction_addresses_rejects_misaligned_ranges() {
    let mut addresses = BTreeSet::new();

    let error = insert_instruction_addresses(&mut addresses, "kernel_body", &[sample_range(0x100, 0x138)])
        .expect_err("misaligned range should fail");

    assert!(error.contains("not aligned"));
}

#[test]
fn last_error_sanitizes_embedded_nuls() {
    set_last_error("alpha\0beta");

    let error = unsafe { std::ffi::CStr::from_ptr(dpc_addr2line_last_error()) }
        .to_string_lossy()
        .into_owned();

    assert_eq!(error, "alpha beta");
}

#[test]
fn build_context_indexes_real_sample_data() {
    let context = build_context(&sample_dwarf_path()).expect("sample dwarf should load");

    assert!(!context.user_subprograms.is_empty());
    assert!(context
        .user_subprograms
        .iter()
        .all(|item| !item.ranges.is_empty() && item.ranges.iter().all(|range| range.end > range.begin)));
}

#[test]
fn enumerate_kernel_ips_impl_rejects_empty_kernel_name() {
    let context = build_context(&sample_dwarf_path()).expect("sample dwarf should load");

    let error = enumerate_kernel_ips_impl(&context, "").expect_err("empty kernel name should fail");

    assert!(error.contains("kernel name was empty"));
}

#[test]
fn enumerate_kernel_ips_impl_rejects_unknown_kernel_name() {
    let context = build_context(&sample_dwarf_path()).expect("sample dwarf should load");

    let error = enumerate_kernel_ips_impl(&context, "DefinitelyMissingKernel")
        .expect_err("missing kernel should fail");

    assert!(error.contains("no text symbols matched kernel"));
}

#[test]
fn enumerate_kernel_ips_impl_returns_sorted_unique_primary_gemm_body() {
    let context = build_context(&sample_dwarf_path()).expect("sample dwarf should load");

    let addresses = enumerate_kernel_ips_impl(&context, "PrimaryGEMM")
        .expect("primary kernel addresses should enumerate");

    assert!(!addresses.is_empty());
    assert_eq!(addresses[0], 0xffff8000fff80000);
    assert!(addresses.windows(2).all(|pair| pair[0] < pair[1]));
    assert!(addresses.iter().all(|address| address % 0x10 == 0));
}