# GPU DWARF Workload Corpus

This corpus compares address-to-source backends on the same Intel GPU
ELF/DWARF artifacts. It complements the existing
`PrimaryGEMMKernel` regression fixture with small kernels whose expected source
locations are easy to identify.

## Fixture Layout

Store each workload under `workloads/<case>/`:

```text
source.cpp       Exact SYCL source, with one MARK comment per expected location.
build.json       Compiler version, flags, runtime, GPU, and capture date.
manifest.json    Kernel names, runtime addresses, sizes, and DWARF file paths.
expected.json    Expected marker files, lines, and optional inline frames.
module.dwarf     ELF/DWARF blob returned by zetModuleGetDebugInfo.
module.native    Optional native binary returned by zeModuleGetNativeBinary.
disassembly.txt  Optional IGA or ocloc disassembly.
```

The manifest must preserve both the exact `mangled_name` and the display
`demangled_name`. Capture debug data using the existing Level Zero tracer in
`data_generation/kernel_debug_info.cpp`; it already records the ELF/DWARF path,
runtime kernel address, module size, and kernel binary size.

## Workloads

Every workload must launch at least two named kernels from one module. The
fixture must assert that each selected symbol resolves only to its own address
range and never returns locations from its sibling kernels.

| Case | Kernel behavior | Required markers and assertions |
| --- | --- | --- |
| `gemm` | Current `PrimaryGEMMKernel` plus a small companion kernel. | Preserve the existing GEMM result as a production-like regression baseline and verify separate symbol/range selection for both kernels. |
| `control_flow` | Multiple kernels containing separate arithmetic, `if`/`else`, ternary, and `switch` code. | Every marked branch body resolves; repeated or non-monotonic line ranges are accepted. |
| `multifile` | Multiple kernels in a `.cpp` source that use helpers declared in a header and implemented in a second source file. | Results identify the `.cpp`, header, and helper implementation paths; file IDs and paths are stable after normalization. |
| `inline_chain` | Multiple kernels calling two nested `inline` helpers in one source file. | Each leaf helper line resolves; record the inline chain when a backend supplies it. |
| `multifile_inline_chain` | Multiple kernels calling nested inline helpers spread across the main source and included headers. | Resolve locations in every participating file and retain the inline chain without crossing into a sibling kernel's range. |

## Artifact Variants

Collect the following variants for every workload. Each workload should have information from *kernel_debug_info.json and reference in different files.

| Dimension | Values to collect | Purpose |
| --- | --- | --- |
| Debug information | `-g`, `-gline-tables-only`, and no debug information | Compare full DWARF with line-table-only output; the no-debug variant must return a clean no-mapping result. |
| Optimization | `-O0`, `-O1`, and `-O2` | Detect source-range changes caused by inlining, control-flow simplification, unrolling, and instruction scheduling. |
| GPU architecture | BMG and PVC | Detect target-specific ELF layout, instruction alignment, relocation, and debug-line behavior. |
| Level Zero binary format | Legacy IGC binary and ZEBinary, when each is available | Establish whether the candidate supports the format emitted by the deployed driver/compiler stack. |

## Implementation

We should have a script that for each workload will: compile and exeucute it with debug info/different optimizations.

Calculate a reference from it.

Based on the files run the source code correlation for all the adapters.

Save the general results from comparison in the json file.
