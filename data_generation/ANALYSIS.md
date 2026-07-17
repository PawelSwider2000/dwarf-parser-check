# DWARF Collection

`build_and_run.sh [total_loops] [matrix_size]` builds and runs the SYCL GEMM
sample, saving artifacts under `artifacts/`.

The Level Zero `zeKernelCreate` callback collects one record per created kernel:

- kernel name, mangled name, and demangled name
- raw module and kernel handle values
- runtime kernel base address from `zexKernelGetBaseAddress`
- module native-binary size
- raw ELF/DWARF module returned by `zetModuleGetDebugInfo`

Each raw module is saved as `<kernel-name>.dwarf`; the JSON summary records its
absolute path and the collected metadata.

The collector does not parse ELF/DWARF, extract ISA, or resolve source lines.
Those operations belong to the main project. The saved module is suitable for
source-line resolution because it is the unmodified
`ZET_MODULE_DEBUG_INFO_FORMAT_ELF_DWARF` payload.

`module_handle_address` and `kernel_handle_address` are opaque Level Zero handle
values. `runtime_kernel_address` is the captured code base address.
