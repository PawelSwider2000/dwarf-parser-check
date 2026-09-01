# DWARF Parser Experiment Results

## Scope

This report summarizes the current Intel Arc B580 experiment matrix. It covers
five SYCL workloads (`gemm`, `control_flow`, `multifile`, `inline_chain`, and
`multifile_inline_chain`), full device debug information (`-g`), optimization
levels `-O0`, `-O1`, and `-O2`, and both runtime JIT and BMG+PVC AOT device
code. Each configuration runs the `rust-gimli` and `pti-elf-parser` resolvers.

The source data is the published
[experiment summary](published-results/experiment_summary.json). The chart
index is [published-results/README.md](published-results/README.md), with a
complete seven-chart suite per adapter.

## Matrix Outcome

All 30 workload/configuration runs completed successfully. Both adapters passed
the primary VTune comparison in every configuration, for 60 passing
adapter/configuration results with no failures or skips. Unitace stall sampling
is an independent sample population and is evaluated against the same native
zebin DWARF model.

| Collector | Sampled IPs | DWARF-mapped IPs | Coverage | PTI mismatches | `rust-gimli` mismatches |
| --- | ---: | ---: | ---: | ---: | ---: |
| VTune | 94,732 | 94,732 | 100.0% | 0 | 88 |
| Unitace | 60,698 | 47,008 | 77.4% | 0 | 7 |

**Coverage** VTune maps all 94,732 sampled instruction IPs. Unitace maps
47,008 of 60,698 sampled stall IPs (77.4%). Nearly all of Unitace's 13,690
unmapped IPs are JIT `-O0` samples attributed to a caller kernel even though
they execute in shared module code outside that kernel's DWARF range. They
cannot be assigned a caller source line without producing a false location.

**Mismatches** PTI matches every DWARF-mapped IP for both collectors.
`rust-gimli` reports 88 VTune mismatches and seven Unitace mismatches, all of
which are line-only representation differences: the reference preserves DWARF
line `0`, while Gimli represents that location as an absent line. The kernel,
instruction offset, and source file agree in every case, so these are not
incorrect source mappings. Treating an absent line and line `0` as equivalent
in the comparator would reduce both mismatch counts to zero without changing a
resolver location.

### Mismatches

All 95 `rust-gimli` mismatches (88 from VTune and seven from Unitace) have the
same cause: DWARF line `0` is represented as an absent line by Gimli. PTI
retains the numeric zero. The resolved kernel, instruction offset, and source
file match the reference, so this is a comparison-policy difference rather than
an incorrect source location.

**Possible fix.** In the comparator, treat an absent resolver line as equal to
a reference line of `0`. Do not apply this rule to nonzero reference lines, so
an actually missing source line remains a mismatch. This would remove all 95
reported mismatches without altering adapter resolution results.

### Coverage

Unitace samples stall events rather than VTune's filtered executable-instruction
population, so its denominator represents a different set of PCs. It maps
47,008 of 60,698 sampled IPs; 13,690 remain outside the named kernel's
kernel-specific DWARF range.

The dominant case is JIT `-O0` multi-kernel modules. Each native zebin contains
a `.text.<kernel>` section and a module-level executable
`.text.Intel_Symbol_Table_Void_Program` section. Unitace attributes PCs from
that shared program to the active caller kernel, while reference generation
uses the caller's `.text.<kernel>` section and matching compilation unit. A
shared-program PC therefore has no valid caller-kernel source location.

| JIT `-O0` caller kernel | Unitace mapped / sampled IPs | IPs outside kernel DWARF range |
| --- | ---: | ---: |
| Control flow `SwitchKernel` | 554 / 4,771 | 4,217 |
| Multifile `HeaderImplementedKernel` | 49 / 1,390 | 1,341 |
| Inline chain `InlineChainMultiplyKernel` | 58 / 1,070 | 1,012 |
| Multifile-inline-chain reduction kernel | 1,713 / 8,814 | 7,101 |

These four caller records account for 13,671 of the 13,690 unmapped Unitace
IPs. The remaining 19 are small boundary gaps distributed across the other
configurations. Since every mapped Unitace IP agrees with PTI, treating the
shared-program PCs as caller-kernel code would only create false source
locations. Section-level attribution from Unitace would be needed to resolve
them against the shared program's own DWARF information.

The retained native zebins show that this coverage loss can be reduced, but not
eliminated. The shared-program CUs provide one unambiguous alternate source
location for 7,904 of the 13,671 dominant JIT `-O0` exclusions. Resolving only
those unique locations would raise overall Unitace coverage from 77.4% to
approximately 90.5% (54,912 of 60,698 IPs) without assigning a caller-kernel
source line to shared code.

The remaining 5,786 IPs cannot be resolved correctly from the current Unitace
CSV and DWARF data: 4,570 have no decoded source line in any CU, 1,197 have
more than one possible CU location, and 19 are the existing minor boundary
gaps. Unitace reports a caller kernel and local IP but no executable-section or
module-address identity, so the ambiguous PCs cannot be attributed safely. Full
coverage would require Unitace to provide section-level attribution, or an
equivalent module-relative GPU PC, in its sampling output.

## Summary

Both adapters are giving good results.

The in-tree PTI gpu-elf-parser solution could be much easier to use and start using. On the other hand it will require maintentance and potential bug fixes.

The `rust-gimli` solution will be potentially easier to maintain but will require adding Rust for building PTI which will increase PTI complexity.
