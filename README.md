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

`build` compiles the workload and `dwarf-parser-check`. `run` collects a
`gpu-hotspots` VTune result using source-analysis stall sampling; the workload
creates `kernel_debug.json` during collection. `analyze` uses the existing
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

Build the PTI GPU ELF parser adapter. Its MIT-licensed PTI parser source is
vendored under `src/adapters/pti/gpu_elf_parser`, so no PTI SDK checkout is
required:

```bash
./dwarf-check adapter build --adapter pti-elf-parser
```

The workload, VTune collection, and resolver stages are independent. After a
successful collection, rerun `analyze` to iterate on adapters without
repeating the hardware profiling run.

## Configuration

Global options must precede the first command:

```bash
./dwarf-check --workload gemm --debug g --opt O0 --device-code jit --artifact-dir artifacts build run
./dwarf-check --workload gemm --debug g --opt O2 --device-code aot --device-target bmg all
./dwarf-check analyze --adapters all
```

Defaults:

| Setting | Default |
| --- | --- |
| `--workload` | `gemm` |
| `--debug` | `g` (`g`, `gline`, or `none`) |
| `--opt` | `O0` (`O0`, `O1`, or `O2`) |
| `--device-code` | `jit` (`jit` or `aot`) |
| `--device-target` | `bmg,pvc` (comma-separated `bmg` and/or `pvc`; used by AOT) |
| `--artifact-dir` | `artifacts` |
| `--build-dir` | `build` |

These paths are derived from the selected workload and configuration:

```text
WORKLOAD_BUILD_DIR=<artifact-dir>/build/<workload>/<device-code>-<device-target>-<debug>-<opt>
WORKLOAD_RESULTS_DIR=<artifact-dir>/results/<workload>/<device-code>-<device-target>-<debug>-<opt>
KERNEL_DEBUG_JSON=<results-dir>/kernel_debug.json
VTUNE_RESULT_DIR=<results-dir>/vtune_results
VTUNE_REFERENCE_CSV=<results-dir>/vtune_reference.csv
```

JIT paths omit `<device-target>`; for example, `jit-g-O2`. AOT paths include it;
for example, `aot-bmg-pvc-g-O2`. This keeps runtime-JIT and AOT artifacts separate.

`WORKLOAD_BUILD_DIR`, `WORKLOAD_RESULTS_DIR`, `KERNEL_DEBUG_JSON`,
`VTUNE_RESULT_DIR`, and `VTUNE_REFERENCE_CSV` remain usable as environment
overrides. `BUILD_TYPE`, `CMAKE_GENERATOR`, `GDB_ADDR2LINE`, `VTUNE_BIN`,
`VTUNE_TARGET_GPU`, `VTUNE_COMPUTING_TASK`, `VTUNE_MAX_ATTEMPTS`,
`UNITRACE_MAX_ATTEMPTS`, and `READELF_BIN` are also
environment-controlled.

By default, analysis emits an address report for every kernel captured in
`kernel_debug.json`. Set `VTUNE_COMPUTING_TASK` to report only one named kernel.

`--device-code jit` is the default and embeds portable SPIR-V for runtime
compilation. `--device-code aot` uses `spir64_gen` and OCLOC during the build.
By default it embeds native images for both `bmg` and `pvc`; select one with
`--device-target bmg` or `--device-target pvc`. An AOT binary must run on a GPU
matching one of its embedded images. The local Arc B580 uses `bmg`.

Run the two modes as separate workflows:

```bash
# Portable SPIR-V compiled by the driver at runtime.
./dwarf-check --debug g --opt O2 --device-code jit all

# Native Battlemage and Ponte Vecchio code compiled during the build.
# The Arc B580 selects the embedded BMG image at runtime.
./dwarf-check --debug g --opt O2 --device-code aot all

# Build only here; execution requires a Ponte Vecchio GPU.
./dwarf-check --debug g --opt O2 --device-code aot --device-target pvc workload build
```

`--debug gline` passes `-gline-tables-only`. oneAPI DPC++ 2026.1 does not support
that option for Intel SYCL device targets, so use `--debug g` for device DWARF.

`run` requires an already-built workload binary and unrestricted ptrace access
for VTune. `analyze` requires both an existing VTune result and manifest. VTune
collection retries up to `VTUNE_MAX_ATTEMPTS=3` times after transient failures.
Unitace stall collection requires the Xe observation policy to be unrestricted:
`sudo sysctl -w dev.xe.observation_paranoid=0`. The runner accepts a Unitace
capture only when it contains populated `metrics_*.csv` data and retries it up
to `UNITRACE_MAX_ATTEMPTS=3` times to recover from transient metric-streamer
read failures.

## Experiment Matrix

`./run_experiments.sh` runs the `g` debug mode at every optimization level for
all five workloads as both JIT and AOT: 30 experiments by default. Each AOT
experiment builds a BMG+PVC fat binary, while mode-specific artifact paths keep
its build and result files separate from the JIT experiment. Every experiment
builds the selected workload, runs it once under VTune, generates the reference,
and compares the selected adapters. Set `AOT_TARGETS=bmg` or `AOT_TARGETS=pvc`
to restrict the AOT target list. For a smaller matrix, set `WORKLOADS`,
`OPT_LEVELS`, or `DEVICE_CODE_MODES`; for example,
`WORKLOADS=gemm OPT_LEVELS=O0 DEVICE_CODE_MODES=jit ./run_experiments.sh`.

Pass `--clean-results` to `dwarf-check` before `run` or `all` to remove the
selected configuration's result directory before generating new artifacts.
`./run_experiments.sh --clean-results` forwards this option to every matrix
entry. The experiment summary distinguishes `SKIP` (VTune provided no source
locations to compare) from `FAIL`.

After the matrix completes, `artifacts/experiment_summary.json` contains one
record per experiment. `adapters` identifies the requested adapters, while each
record's `adapter_results` object stores a separate `PASS`, `SKIP`, or `FAIL`
status plus VTune/DWARF and Unitace/DWARF aggregates for each adapter. Each
aggregate includes `coverage` with sampled, DWARF-mapped, and unmapped IP
counts. The record-level status summarizes its adapter statuses. Set
`ARTIFACT_DIR` or `EXPERIMENT_SUMMARY_FILE` to choose a different location.

## Charts

Generate status, mismatch, and coverage charts from a completed experiment
summary:

```bash
python3 data_generation/plot_experiment_summary.py \
	artifacts/experiment_summary.json --adapter rust-gimli
```

The default output directory is `artifacts/charts`. Every figure title, the
generated `chart_metadata.json`, and the chart README identify the adapter.
Multi-adapter summaries require `--adapter` so each chart shows one adapter.

## Output

`analyze` creates a per-kernel VTune reference and addr2line IP list, then passes
both to the resolver. Adapters resolve only the supplied IPs; they do not scan
or infer instruction boundaries. It also retains `source_locations.json` as a
diagnostic sidecar, but does not use that JSON file for comparisons.

For unitrace, `analyze` uses the sampled IPs with source locations from the
matching native zebin's DWARF line table. This tests the same address-to-DWARF
contract as VTune. The IGC `// File:` and `// Line` assembly markers are written
to a separate `unitrace_reference_*.csv`, recorded in the manifest as
`marker_reference_csv`, and remain a diagnostic: they may attribute an IP to an
inlined helper or call site rather than the primary DWARF location.

For each selected adapter, the output directory contains a resolution CSV and
a comparison JSON report. The default output directory is
`WORKLOAD_RESULTS_DIR`; override it with `analyze --output-dir PATH`.

## Direct Resolver CLI

The compiled resolver accepts a manifest, adapter selection, optional
reference, and report output directory:

```bash
./build/dwarf-parser-check \
	--kernel-debug-json artifacts/results/gemm/jit-g-O0/kernel_debug.json \
	--adapters all \
	--output-dir artifacts/results/gemm/jit-g-O0 \
	--reference artifacts/results/gemm/jit-g-O0/vtune_reference.csv
```

The manifest contains each kernel's demangled display name, mangled symbol
selector, ELF/DWARF path, runtime metadata, and matching VTune IP list.
Resolved addresses are reported as offsets from the beginning of each kernel.

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
