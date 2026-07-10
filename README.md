# dwarf-parser-check

This repository is intended to compare different third-party libraries for reading DWARF debug information from Intel GPU modules and extracting source-level information for a specific kernel.

The main goal is practical and user-facing: given a kernel name and instruction pointers, the tool should resolve locations back to the user's source files and lines, not to unrelated system library paths whenever user code information is available.

## Current Status

The repository is currently empty. This README defines the intended scope, architecture, and initial implementation plan.

## Problem Statement

Intel GPU debug data may be stored in compressed DWARF files generated for device modules. The tool in this repository should:

- open a file containing compressed DWARF debug information
- find debug information for a selected kernel
- translate one or more instruction pointers to source locations
- provide user source files over system or toolchain internal files when presenting results
- optionally compare produced results with reference output collected from VTune
- make it easy to plug in and evaluate multiple third-party DWARF parsing libraries

## Intended CLI

The main program should accept the following parameters:

- `--dwarf-file <PATH>`: path to the file containing compressed DWARF data
- `--kernel <NAME>`: name of the kernel to parse
- `--ip <HEX_OR_DEC>`: optional instruction pointer to resolve; may be passed multiple times
- `--all-ips`: optional flag to resolve all available instruction ranges or instruction pointers for the kernel
- `--reference <PATH>`: optional path to a file with reference results captured from VTune

Example shape:

```bash
./build/dwarf-parser-check \
  --dwarf-file ./data/kernel.debug \
  --kernel my_kernel \
  --ip 0x120 \
  --ip 0x144 \
  --reference ./refs/vtune-my-kernel.txt
```

If no `--ip` values are provided, the program should either require `--all-ips` or default to resolving all instruction locations for the selected kernel. The exact behavior should be kept explicit and documented in the implementation.

## Expected Output

For each resolved instruction pointer, the program should report at least:

- kernel name
- instruction pointer or address/range
- source file path
- line number
- optional column number
- optional inline call chain if available
- adapter or backend used for resolution

When multiple candidate file paths exist, the tool should prefer source locations that point to user code. A reasonable initial policy is:

1. prefer files inside a user-provided project root, if such an option is added later
2. otherwise prefer non-system paths over paths under directories such as `/usr`, `/lib`, or compiler/runtime install trees
3. preserve the raw backend result as debug output for troubleshooting

## Architecture

The project should be built around a small adapter abstraction so new libraries can be added without changing the CLI or comparison flow.

At a high level, the program should be split into a few stable layers:

- CLI layer: parse arguments and print results
- core layer: normalize requests and compare outputs
- adapter layer: wrap each third-party library behind a common interface
- comparison layer: match resolved locations against VTune reference results

The adapter abstraction should stay small and focus on a few shared concepts:

- a normalized resolve request that describes the DWARF input, kernel name, selected IPs, and optional reference data
- a common resolved-location result type that all backends return
- backend-specific diagnostics that can be shown without changing the common reporting flow

This keeps evaluation fair: the same request model, output format, and comparison logic can be reused across all candidate libraries.

The implementation should also keep room for mixed integration styles:

- some adapters may be native C++ code linked directly into the program
- some adapters may call external tools or helper binaries
- some adapters may come from a different language, such as a Rust-based `gimli` helper

Suggested repository layout:

```text
.
├── CMakeLists.txt
├── README.md
├── include/
│   ├── adapter.h
│   ├── cli.h
│   ├── compare.h
│   ├── core.h
│   ├── dummy_adapter.h
│   └── types.h
├── src/
│   ├── main.cpp
│   ├── cli.cpp
│   ├── core.cpp
│   ├── compare.cpp
│   └── adapters/
│       └── dummy_adapter.cpp
```

## Initial Libraries To Evaluate

### 1. gimli

One planned backend should use Rust `gimli`, integrated as a separate component or helper binary.

Why start here:

- it is a mature DWARF parsing library
- it may provide a useful comparison point against native C++ approaches
- it allows the project to compare both cross-language and in-process adapter strategies

Planned role:

- parse DWARF sections from the compressed input
- find compilation units and line programs relevant to the kernel
- map instruction addresses to file and line information

Candidate options:

- Rust helper binary built on top of `gimli`
- a native C++ backend such as `libdwarf` or LLVM DWARF support

### 2. ocloc

Another planned backend should evaluate Intel `ocloc` as an external source of debug-related information or as a companion tool in the workflow.

Planned role:

- inspect what debug information is available for Intel GPU modules
- help validate or derive symbol and kernel-related information
- serve as an alternative path for extracting or cross-checking location data

Because `ocloc` is not a direct drop-in C++ DWARF parsing library, the adapter for it will likely wrap command-line invocation and output parsing rather than in-process library calls.

### 3. IGA

Another backend candidate is Intel `IGA`.

Planned role:

- inspect instruction-level information for Intel GPU kernels
- help correlate kernel instruction addresses with debug or disassembly information
- provide another comparison point alongside `gimli` and `ocloc`

## Comparison Criteria

Each backend should be evaluated using the same inputs and the same reporting format. Useful comparison points include:

- correctness of file and line mapping
- ability to identify the requested kernel reliably
- quality of user-source selection versus system-path noise
- support for compressed DWARF input
- ease of integration
- performance on realistic debug files
- amount of backend-specific workaround logic required
- closeness to VTune reference output

## Reference Result Comparison

When a VTune reference file is provided, the program should compare:

- whether the same instruction pointers are present
- whether file paths match exactly or after normalization
- whether line numbers match
- whether the backend returns extra or missing locations

Useful comparison output should include:

- matched entries
- mismatched file paths
- mismatched line numbers
- unresolved instruction pointers
- entries only present in VTune
- entries only present in the tested backend

## Build Instructions

The initial implementation should be a C++ command-line tool.

Prerequisites:

- C++20-capable compiler such as `clang++` or `g++`
- `cmake` available in `PATH`
- `ninja` is recommended, but optional

Current state:

- the repository currently builds a dummy adapter only
- no external DWARF library is required yet
- `ocloc` is not wired in yet
- `gimli` and `IGA` are not wired in yet

Adapter-specific build strategy:

- each optional adapter should have its own CMake option
- each adapter should only be built when its dependencies are available
- this allows one backend to be enabled without forcing all toolchains or libraries to be installed

Suggested examples:

- `-DDPC_ENABLE_DUMMY_ADAPTER=ON`
- `-DDPC_ENABLE_GIMLI_ADAPTER=ON`
- `-DDPC_ENABLE_OCLOC_ADAPTER=ON`
- `-DDPC_ENABLE_IGA_ADAPTER=ON`

The long-term intent is that CMake should either skip disabled adapters or fail early with a clear message when an adapter is explicitly requested but its dependencies are missing.

Typical development commands:

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

For release-style runs:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/dwarf-parser-check --help
```

## Running Instructions

Once implemented, expected usage should look like:

```bash
./build/dwarf-parser-check \
  --dwarf-file ./data/module.debug \
  --kernel my_kernel \
  --all-ips
```

Or with explicit IP selection and reference comparison:

```bash
./build/dwarf-parser-check \
  --dwarf-file ./data/module.debug \
  --kernel my_kernel \
  --ip 0x100 \
  --ip 0x120 \
  --reference ./refs/vtune.txt
```

## Libraries And Dependencies

Expected initial C++ dependencies:

- no external libraries are required for the current draft implementation
- the CLI parser is currently implemented manually
- future parser backends may add Rust `gimli`, `libdwarf`, LLVM DWARF support, Intel `ocloc`, and/or Intel `IGA`
- future tests may add `GoogleTest` or `Catch2`

Each backend should document:

- its enabling CMake option
- required packages or toolchain components
- whether it is linked in-process or invoked as an external tool
- any backend-specific runtime requirements

## Non-Goals

At least initially, this project does not need to:

- become a full generic DWARF inspection framework
- support every possible GPU binary format
- hide backend differences completely in debug logs

The focus should stay on kernel-specific source location extraction and backend comparison.

## Next Steps

1. Replace the dummy adapter with the first real DWARF parser backend.
2. Define and document the VTune reference file format.
3. Add tests and hook them into CTest.
4. Add `gimli`, `ocloc`, and/or `IGA` adapters behind separate CMake options.
