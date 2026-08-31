# DWARF Parser Experiment Results

## What Are the Experiments

The project evaluates Intel GPU address-to-source resolution from device DWARF.
Five SYCL workloads cover GEMM, control flow, multiple source files, inline
chains, and multi-file inline chains. The recorded matrix uses full debug
information (`-g`), optimization levels `-O0`, `-O1`, and `-O2`, and both
runtime JIT and AOT device code. VTune GPU PC samples provide the primary
reference. Unitace stall samples are additionally resolved through the matching
native zebin DWARF and compared with IGC assembly source markers.

## What Are the Results

The committed experiment summary contains 30 VTune comparisons: 29 passed and
one failed. The main resolver matched the DWARF-derived VTune reference for the
validated cases; for example, the BMG AOT GEMM `-O0` run resolved all 2,553
DWARF-mapped IPs. Eight final GEMM IPs were correctly retained as unmapped,
because they lie beyond the DWARF line-program end sequence.

Unitace and VTune produced the same DWARF locations for the 2,408 shared IPs in
the focused BMG AOT GEMM `-O0` run. IGC assembly markers differed at 848 of
those locations after path normalization, showing that marker attribution is
not interchangeable with the primary DWARF reference.

## Conclusions and Further Work

DWARF is a practical primary source of GPU instruction-to-source mapping for
the tested Intel GPU binaries, and the adapter comparison workflow can expose
both resolver disagreement and uncovered IPs. VTune and Unitace should use the
same zebin DWARF reference when their samples are compared.

Next, fix Unitace compilation-unit selection by associating each kernel with
its ELF text section and the corresponding DWARF CU, rather than choosing a CU
only by overlapping zero-based offsets. Re-run the complete matrix after
enabling VTune ptrace access and on hardware that supports Unitace
EuStallSampling. Add BMG-only and PVC validation, plus no-debug behavior and
line-table-only coverage if supported by the compiler.

## Limitations and Good Points of the Solutions

**Good points:** the workloads exercise control flow, source-file boundaries,
and inlining; JIT and AOT outputs are isolated; comparisons preserve unmapped
IPs instead of hiding debug-information gaps; and the generated manifest keeps
each kernel's binary, symbol, and sampled addresses together.

**Limitations:** the current Unitace annotation can choose the wrong CU when
multiple kernel line programs begin at address zero. IGC source markers can
refer to an inlined helper or call site and require lexical path normalization.
The committed matrix includes BMG+PVC AOT fat binaries, but a BMG-only native
image is required when using its assembly as a source-marker reference. Finally, the current compiler
does not support Intel SYCL device `-gline-tables-only`, so the collected matrix
uses full debug information only.