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
| `--device-target` | `auto` (the GPUs on this machine; or comma-separated `bmg` and/or `pvc`; used by AOT) |
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
for example, `aot-bmg-g-O2`. This keeps runtime-JIT and AOT artifacts separate.

`WORKLOAD_BUILD_DIR`, `WORKLOAD_RESULTS_DIR`, `KERNEL_DEBUG_JSON`,
`VTUNE_RESULT_DIR`, and `VTUNE_REFERENCE_CSV` remain usable as environment
overrides. `BUILD_TYPE`, `CMAKE_GENERATOR`, `GDB_ADDR2LINE`, `VTUNE_BIN`,
`VTUNE_TARGET_GPU`, `VTUNE_COMPUTING_TASK`, `VTUNE_MAX_ATTEMPTS`,
`UNITRACE_BIN`, `UNITRACE_MAX_ATTEMPTS`, `UNITRACE_SAMPLING_INTERVAL`, and
`READELF_BIN` are also environment-controlled. `UNITRACE_SAMPLING_INTERVAL` is
the Unitrace stall-sampling interval in microseconds. It defaults to `0.5`,
which matches VTune's default; Unitrace's own default of 50 us leaves short
kernels with only a few samples. `UNITRACE_BIN` defaults to the sibling PTI SDK build at
`../applications.analyzers.profilingtoolsinterfaces.sdk/tools/unitrace/build/unitrace`.

Before collecting, `run` resolves each tool and logs the executable path, then
logs the exact command for each attempt. If a tool is missing, the run fails
right away and names the variable to set. If an attempt fails, the log shows
the exit code or the reason no stall metrics were found, plus the files in the
result directory. The final error lists the tool, workload, result directory,
and full command.

By default, analysis emits an address report for every kernel captured in
`kernel_debug.json`. Set `VTUNE_COMPUTING_TASK` to report only one named kernel.
Each kernel's report uses both `-source-object` and `-filter` for its computing
task. `-source-object` on its own selects the listing but still counts the
samples of every kernel in the module, plus samples outside any task.

`--device-code jit` is the default and embeds portable SPIR-V for runtime
compilation. `--device-code aot` uses `spir64_gen` and OCLOC during the build.
By default (`--device-target auto`) it builds only for the Intel GPUs on this
machine, detected from their PCI device IDs in `/sys/class/drm` (`0xe2xx` is
`bmg`, `0x0bd0`-`0x0bdb` is `pvc`); the Arc B580 gives `bmg`. Pass
`--device-target bmg`, `pvc`, or `bmg,pvc` to choose the targets yourself, and
`./dwarf-check device-target` prints what `auto` resolves to. An AOT binary must
run on a GPU matching one of its embedded images.

Run the two modes as separate workflows:

```bash
# Portable SPIR-V compiled by the driver at runtime.
./dwarf-check --debug g --opt O2 --device-code jit all

# Native code for this machine's GPU (BMG on the Arc B580), compiled during the build.
./dwarf-check --debug g --opt O2 --device-code aot all

# Build only here; execution requires a Ponte Vecchio GPU.
./dwarf-check --debug g --opt O2 --device-code aot --device-target pvc workload build
```

`--debug gline` passes `-gline-tables-only`. oneAPI DPC++ 2026.1 does not support
that option for Intel SYCL device targets, so use `--debug g` for device DWARF.

`run` requires an already-built workload binary and unrestricted ptrace access
for VTune. `analyze` requires both an existing VTune result and manifest. VTune
collection retries up to `VTUNE_MAX_ATTEMPTS=3` times after transient failures.
Unitrace stall collection requires the Xe observation policy to be unrestricted:
`sudo sysctl -w dev.xe.observation_paranoid=0`. The runner accepts a Unitrace
capture only when it contains populated `metrics_*.csv` data and retries it up
to `UNITRACE_MAX_ATTEMPTS=3` times to recover from transient metric-streamer
read failures.

## VTune vs Unitrace Comparison

To check whether unitrace reports the same stall samples and source locations
as VTune, without running any DWARF adapter:

```bash
sudo apt install python3-pyelftools               # needed by the unitrace extraction (or pip install pyelftools)
./dwarf-check.sh workload build
./dwarf-check.sh --clean-results run
./dwarf-check.sh analyze --adapters none
```

`--adapters none` skips the resolver adapters (no cargo build needed). When both
tools are selected (the default `--tool both`), `analyze` writes
`<results>/vtune_vs_unitrace/` with, per kernel:

- `vtune_vs_unitrace_<kernel>.csv`: every instruction of the kernel, with its
  VTune IP and source location when VTune sampled it, beside unitrace's own
  reported location when unitrace sampled it (from its `metrics_*.csv` `Source`
  column, or the source table of its asm dump when that column is absent).
- `vtune_vs_unitrace_dwarf_<kernel>.csv`: the same, with unitrace IPs mapped
  through the zebin DWARF line table.
- `*_summary.csv`: counts of instructions sampled by both tools with the
  same or a different location, sampled by VTune only, sampled by unitrace
  only, and listed by VTune but sampled by neither.

The instruction text and offsets come from the asm dump unitrace (2.4 or newer)
writes for every kernel it ran, `unitrace_results/unitrace.<pid>/asm/*.asm`.
That dump is taken from the binary that ran on the GPU, so JIT and AOT are handled
the same way and no IGC shader dump or extra build is needed.

When unitrace reports only a file name, locations are matched by file name and line.

### One-command VTune vs Unitrace report

`./run_vtune_unitrace_report.sh` runs `dwarf-check.sh ... --clean-results all
--adapters none` for every workload/configuration (all five workloads, `O0`-`O2`,
JIT and AOT for this machine's GPU by default), then writes `artifacts/EXPERIMENT_RESULTS.md`
and `artifacts/vtune_unitrace_summary.json` from that run's results. It loads
oneAPI when `icpx` or `vtune` is not on PATH and checks for pyelftools and
`dev.xe.observation_paranoid=0` before starting. Per-run logs go to
`artifacts/logs/`; failed runs are listed in the report and make the script exit
non-zero.

```bash
./run_vtune_unitrace_report.sh                                    # full matrix
./run_vtune_unitrace_report.sh --workloads gemm,control_flow --opt-levels O0 --device-code jit
./run_vtune_unitrace_report.sh --report-only                      # regenerate the report only
```

Use `--unitrace-bin PATH` (or `UNITRACE_BIN`) to select the unitrace build,
`--oneapi PATH` for the oneAPI `setvars.sh`, and `--report PATH` to write the
report elsewhere. Run `--help` for all options. See
[VTUNE_UNITRACE_REPORT.md](VTUNE_UNITRACE_REPORT.md) for the full guide,
including output files and troubleshooting.

## Experiment Matrix

`./run_experiments.sh` runs the `g` debug mode at every optimization level for
all five workloads as both JIT and AOT: 30 experiments by default. Each AOT
experiment builds for the GPUs on this machine (`AOT_TARGETS=auto`), while mode-specific artifact paths keep
its build and result files separate from the JIT experiment. Every experiment
builds the selected workload, runs it once under VTune, generates the reference,
and compares the selected adapters. Set `AOT_TARGETS=bmg`, `pvc`, or `bmg,pvc`
to choose the AOT targets. For a smaller matrix, set `WORKLOADS`,
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
contract as VTune. The source table of unitrace's asm dump is written to a
separate `unitrace_asm_reference_*.csv`, recorded in the manifest as
`marker_reference_csv`, and unitrace's `metrics_*.csv` `Source` column to
`unitrace_native_reference_*.csv`.

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
