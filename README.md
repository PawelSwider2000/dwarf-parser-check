# dwarf-parser-check

`dwarf-parser-check` resolves Intel GPU DWARF information to source locations
and compares resolver output with VTune GPU PC-sampling data. The root
[`dwarf-check`](dwarf-check) script is the single entry point for building the
sample, collecting VTune data, and running resolver adapters.

## Basic Workflow

Build the selected SYCL workload and the resolver, collect VTune samples, then
analyze the existing result:

```bash
./dwarf-check build run
./dwarf-check analyze
```

`build` compiles the workload and `dwarf-parser-check`. `run` executes the
workload to create `kernel_debug.json`, then collects a `gpu-hotspots` VTune
result using source-analysis stall sampling. `analyze` uses the existing
manifest and VTune result to generate a direct address-to-source reference and
run the selected adapters. It never starts VTune collection implicitly.

Run `./dwarf-check --help` for the complete command reference.

## Commands

```bash
./dwarf-check adapter build    # Configure and build dwarf-parser-check.
./dwarf-check workload build   # Build the selected SYCL workload only.
./dwarf-check build            # Build the workload and resolver.
./dwarf-check run              # Run the workload and collect VTune samples.
./dwarf-check analyze          # Generate the VTune reference and adapter reports.
./dwarf-check test             # Run CTest and Rust gimli adapter tests.
```

The workload, VTune collection, and resolver stages are independent. After a
successful collection, rerun `analyze` to iterate on adapters without
repeating the hardware profiling run.

## Configuration

Global options must precede the first command:

```bash
./dwarf-check --workload gemm --debug g --opt O0 --artifact-dir artifacts build run
./dwarf-check analyze --adapters all
```

Defaults:

| Setting | Default |
| --- | --- |
| `--workload` | `gemm` |
| `--debug` | `g` (`g`, `gline`, or `none`) |
| `--opt` | `O0` (`O0`, `O1`, or `O2`) |
| `--artifact-dir` | `artifacts` |
| `--build-dir` | `build` |

These paths are derived from the selected workload and configuration:

```text
WORKLOAD_BUILD_DIR=<artifact-dir>/build/<workload>/<debug>-<opt>
WORKLOAD_RESULTS_DIR=<artifact-dir>/results/<workload>/<debug>-<opt>
KERNEL_DEBUG_JSON=<results-dir>/kernel_debug.json
VTUNE_RESULT_DIR=<results-dir>/vtune_results
VTUNE_REFERENCE_CSV=<results-dir>/vtune_reference.csv
```

`WORKLOAD_BUILD_DIR`, `WORKLOAD_RESULTS_DIR`, `KERNEL_DEBUG_JSON`,
`VTUNE_RESULT_DIR`, and `VTUNE_REFERENCE_CSV` remain usable as environment
overrides. `BUILD_TYPE`, `CMAKE_GENERATOR`, `GDB_ADDR2LINE`, `VTUNE_BIN`,
`VTUNE_TARGET_GPU`, `VTUNE_COMPUTING_TASK`, and `READELF_BIN` are also
environment-controlled.

`run` requires an already-built workload binary and unrestricted ptrace access
for VTune. `analyze` requires both an existing VTune result and manifest.

## Output

`analyze` creates `vtune_reference.csv` and passes it to the resolver as the
sole comparison reference. It also retains `source_locations.json` as a
diagnostic sidecar, but does not use that JSON file for comparisons.

For each selected adapter, the output directory contains a resolution CSV and
a comparison JSON report. The default output directory is
`WORKLOAD_RESULTS_DIR`; override it with `analyze --output-dir PATH`.

## Direct Resolver CLI

The compiled resolver accepts a manifest, adapter selection, optional
reference, and report output directory:

```bash
./build/dwarf-parser-check \
  --kernel-debug-json artifacts/results/gemm/g-O0/kernel_debug.json \
  --adapters all \
  --output-dir artifacts/results/gemm/g-O0 \
  --reference artifacts/results/gemm/g-O0/vtune_reference.csv
```

The manifest contains each kernel's demangled display name, mangled symbol
selector, ELF/DWARF path, and runtime metadata. Resolved instruction addresses
are reported as offsets from the beginning of each kernel.

## Tests

```bash
./dwarf-check test
```

This builds the resolver, runs CTest with output on failure, and then runs the
Rust gimli adapter tests directly. The shell-level CTest verifies command
parsing, derived paths, required-input failures, and that adapter comparisons
use `vtune_reference.csv` rather than `source_locations.json`.

## Prerequisites

- CMake and Ninja
- a C++20 compiler
- Rust and Cargo for the gimli adapter
- oneAPI `icpx` and Level Zero for workload builds
- VTune for `run` and `analyze`

## Project Layout

```text
.
├── CMakeLists.txt              # CMake build and test configuration
├── README.md                   # Project and workflow documentation
├── dwarf-check                 # Unified build, collection, analysis, and test entry point
├── include/                    # Public C++ interfaces
├── src/                        # Resolver CLI, core logic, comparison, and adapters
│   └── adapters/               # gimli, IGA, and GDB Intel adapter implementations
├── data_generation/
│   ├── correlate_vtune_report.py  # VTune address-to-source correlation
│   ├── kernel_debug_info.*        # Workload manifest generation
│   └── workloads/                 # SYCL workloads, including gemm
├── tests/                      # C++, Rust, and shell regression tests
└── artifacts/                  # Generated workload, VTune, and adapter reports
```
