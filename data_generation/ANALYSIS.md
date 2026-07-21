# DWARF Collection

`./make_reference build run` builds and runs the SYCL GEMM sample, saving
artifacts under `artifacts/`. Commands execute in their provided order, so a
complete reference-generation invocation is:

```bash
./make_reference build run vtune_run analyze
```

Available commands are `build`, `run`, `vtune_run`, and `analyze`.
`vtune_run` collects VTune GPU PC samples using source analysis with
stall sampling. This is supported by the included Arc B580. `analyze` uses the
VTune `hotspots` report in source mode to write instruction addresses,
assembly, source lines, and stall counts to
`artifacts/result_vtune_reference.csv`.

Set `TOTAL_LOOPS` and `MATRIX_SIZE` to configure the workload; they default to
`1` and `128`. `VTUNE_BIN` and `VTUNE_RESULT_DIR` respectively override the
VTune CLI executable and its result directory. `VTUNE_COMPUTING_TASK` selects
the GPU task analyzed by `analyze` and defaults to `PrimaryGEMMKernel`.
`VTUNE_REFERENCE_CSV` overrides the report output path. If a result contains
multiple GPU binaries, `VTUNE_ZEBIN` selects the one to analyze. The sample
queries `zeKernelGetBinaryExp` for every created kernel and writes
`kernel_binary_size` in its JSON record. The field is `null` if the query
fails.

## Source Correlation

The VTune `hotspots` source report currently assigns only the kernel entry
range to the outer invocation line. The `.zebin` stored under the same VTune
result contains a more complete DWARF line table, including the inlined kernel
body and SYCL headers. The `analyze` command decodes that table with `readelf`
and uses it to fill `Source File` and `Source Line` for every assembly row.
`Source File` preserves a normalized absolute path, resolving relative paths
against the DWARF compilation directory.

VTune reports GPU addresses as offsets in the binary file, while DWARF uses
addresses relative to the kernel text section. Correlation therefore converts
each report address using the text section's file offset and virtual address.
Basic-block headings and instructions outside the DWARF kernel range remain
unmapped. `READELF_BIN` overrides the `readelf` executable if necessary.

`analyze` also writes
`artifacts/vtune_source_locations/source_locations.json`. This file contains
every instruction IP as a key and a list of highest reachable user-source
locations as its value:

```json
{
  "0xd8e0": [
    ["/path/to/simple_sycl_vtune.cpp", 204],
    ["/path/to/simple_sycl_vtune.cpp", 206]
  ]
}
```

For instructions attributed directly to user code, the list contains that
location. For oneAPI, standard-library, and compiler helper instructions, the
generator follows VTune assembly call edges back through DWARF function ranges
and records all reachable user call sites. `USER_SOURCE_ROOT` defines which
paths are user-owned and defaults to `data_generation/`.
`VTUNE_SOURCE_LOCATIONS_JSON` overrides the JSON output path. Instructions
outside the kernel function ranges, such as trailing `illegal` rows, are kept
with an empty list.

## VTune Prerequisite

VTune collection requires `kernel.yama.ptrace_scope=0`. If `vtune_run` reports
that this policy is restricted, ask an administrator to run:

```bash
sudo sysctl kernel.yama.ptrace_scope=0
```

The Level Zero `zeKernelCreate` callback collects one record per created kernel:

- kernel name, mangled name, and demangled name
- raw module and kernel handle values
- runtime kernel base address from `zexKernelGetBaseAddress`
- module native-binary size
- kernel binary size from `zeKernelGetBinaryExp`
- raw ELF/DWARF module returned by `zetModuleGetDebugInfo`

Each raw module is saved as `<kernel-name>.dwarf`; the JSON summary records its
absolute path and the collected metadata.

The collector does not parse ELF/DWARF, extract ISA, or resolve source lines.
Those operations belong to the main project. The saved module is suitable for
source-line resolution because it is the unmodified
`ZET_MODULE_DEBUG_INFO_FORMAT_ELF_DWARF` payload.

`module_handle_address` and `kernel_handle_address` are opaque Level Zero handle
values. `runtime_kernel_address` is the captured code base address.
