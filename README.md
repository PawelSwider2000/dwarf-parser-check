# dwarf-parser-check

`dwarf-parser-check` is a command-line tool for resolving Intel GPU DWARF information back to source locations and comparing backend behavior.

The repository currently contains:

- a real Rust `gimli`/`addr2line` adapter
- a dummy adapter used for scaffolding and tests
- a common CLI, comparison layer, and adapter interface
- C++ and Rust tests wired into CTest

## What The Tool Does

Given a DWARF-bearing GPU module, a kernel name, and either explicit IPs or `--all-ips`, the tool:

- resolves instruction addresses to file, line, and optional column
- reports the backend used for resolution
- can compare results against a VTune-style reference file
- allows multiple adapters to be selected at runtime

For the Rust `gimli` adapter, `--all-ips` can enumerate kernel IPs from DWARF and symbol information, then resolve them back to source lines.

## Current CLI

The program currently accepts:

- `--dwarf-file <PATH>`: path to the DWARF/object file
- `--kernel <NAME>`: demangled kernel name used for reporting and reference matching
- `--mangled-kernel <NAME>`: mangled symbol selector used by adapters for kernel-IP enumeration
- `--ip <HEX_OR_DEC>`: instruction pointer to resolve, repeatable
- `--all-ips`: resolve all enumerated IPs for the selected kernel
- `--adapters <LIST>`: comma-separated adapter names or `all`
- `--reference <PATH>`: optional reference file for comparison
- `--help`, `-h`: show usage

`--kernel` and `--mangled-kernel` are both required.

If no `--ip` values are provided, `--all-ips` must be used.

## Why Both Kernel Names Exist

The tool now treats these as separate concepts:

- `--kernel` is the user-facing, demangled name shown in output and used by comparison logic
- `--mangled-kernel` is the exact raw selector used by adapters when they need to enumerate kernel IPs from symbols

This split exists because the user-facing kernel label is not always enough to derive a unique raw symbol.

In the sample DWARF file, the demangled display name is `PrimaryGEMM`, but the most direct mangled selector for the real body is:

```text
_Z4GEMMPKfS0_PfjN4sycl3_V12idILi2EEE
```

The sample also contains wrapper-related symbols such as:

```text
_ZTS17PrimaryGEMMKernel
_ZTSN4sycl3_V16detail19__pf_kernel_wrapperI17PrimaryGEMMKernelEE
```

Those are not equivalent selectors. The real body selector and wrapper selectors can produce different results.

## Example Commands

Resolve all IPs for the sample DWARF using the Rust `gimli` adapter:

```bash
./build/dwarf-parser-check \
  --dwarf-file dwarf_files/PrimaryGEMMKernel.dwarf \
  --kernel PrimaryGEMM \
  --mangled-kernel _Z4GEMMPKfS0_PfjN4sycl3_V12idILi2EEE \
  --adapters rust-gimli \
  --all-ips
```

Resolve specific IPs:

```bash
./build/dwarf-parser-check \
  --dwarf-file dwarf_files/PrimaryGEMMKernel.dwarf \
  --kernel PrimaryGEMM \
  --mangled-kernel _Z4GEMMPKfS0_PfjN4sycl3_V12idILi2EEE \
  --ip 0x8000ffd50060 \
  --ip 0x8000ffd50b20
```

Run with all compiled adapters:

```bash
./build/dwarf-parser-check \
  --dwarf-file dwarf_files/PrimaryGEMMKernel.dwarf \
  --kernel PrimaryGEMM \
  --mangled-kernel _Z4GEMMPKfS0_PfjN4sycl3_V12idILi2EEE \
  --adapters all \
  --all-ips
```

## Output Shape

The tool prints results grouped by backend.

Current output includes:

- backend name
- kernel name
- warnings, if any
- resolved locations in the form `IP -> file:line[:column]`
- optional backend notes such as the resolved function name

Example shape:

```text
backend: rust-gimli
kernel: PrimaryGEMM
  - 0x8000ffd50060 -> /path/to/main.cc:63
    note: resolved function: _Z4GEMMPKfS0_PfjN4sycl3_V12idILi2EEE
```

The tool no longer prints or exposes a `user/system/unknown` path kind. Source file and line resolution still comes from DWARF and `addr2line`, but no additional path-kind label is reported in the common output.

## Current Adapter Behavior

### rust-gimli

The Rust adapter lives under [src/adapters/gimli](src/adapters/gimli).

Current behavior:

- builds an `addr2line::Context` from the input object
- resolves source locations with `find_frames`
- enumerates kernel IPs for `--all-ips`
- uses DWARF subprogram ownership logic to step from wrapper-adjacent symbols back to user code when needed

Important implementation notes:

- source file/line selection is driven by DWARF and `addr2line`
- `--mangled-kernel` is used for symbol matching during IP enumeration
- the sample `PrimaryGEMM` body selector is `_Z4GEMMPKfS0_PfjN4sycl3_V12idILi2EEE`

### dummy

The dummy adapter returns synthetic locations and exists to keep the adapter interface and tests simple while evolving the rest of the tool.

## Adapter Selection

The CLI supports dynamic adapter selection based on compiled adapters.

Examples:

```bash
--adapters rust-gimli
--adapters dummy
--adapters rust-gimli,dummy
--adapters all
```

Whitespace around comma-separated adapter names is accepted.

## Reference Comparison

When `--reference` is provided, the tool loads reference locations for the demangled `--kernel` name and compares them against the selected backend output.

The comparison layer checks:

- matching IPs
- file path equality
- line equality
- optional column equality
- missing entries on either side

## Build

Prerequisites:

- CMake
- Ninja is recommended
- a C++20-capable compiler
- Rust toolchain for the Rust `gimli` adapter

Typical commands:

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Show CLI help:

```bash
./build/dwarf-parser-check --help
```

## Tests

The repository currently includes:

- C++ GTests for CLI, adapter selection, and end-to-end adapter behavior
- Rust integration tests for the `gimli` adapter FFI surface
- Rust internal tests for helper logic and DWARF ownership behavior
- CTest integration that runs the Rust test suite

Run everything:

```bash
ctest --test-dir build --output-on-failure
cargo test --manifest-path src/adapters/gimli/Cargo.toml --tests
```

## Sample DWARF Notes

The sample file in this repository is:

```text
dwarf_files/PrimaryGEMMKernel.dwarf
```

Useful symbol spellings observed in that file:

- display kernel name: `PrimaryGEMM`
- real body selector: `_Z4GEMMPKfS0_PfjN4sycl3_V12idILi2EEE`
- wrapper/type-related symbol: `_ZTS17PrimaryGEMMKernel`
- wrapper helper symbol: `_ZTSN4sycl3_V16detail19__pf_kernel_wrapperI17PrimaryGEMMKernelEE`

If the goal is to resolve the real computation body, the recommended selector for the sample is:

```text
_Z4GEMMPKfS0_PfjN4sycl3_V12idILi2EEE
```

## Project Layout

```text
.
├── CMakeLists.txt
├── README.md
├── include/
├── src/
│   ├── main.cpp
│   ├── cli.cpp
│   ├── core.cpp
│   ├── compare.cpp
│   └── adapters/
├── tests/
└── dwarf_files/
```
