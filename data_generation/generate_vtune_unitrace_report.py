#!/usr/bin/env python3
"""Generate a VTune vs unitrace experiment report from dwarf-check results.

Usage:
  python3 generate_vtune_unitrace_report.py --artifact-dir artifacts \
      --runs artifacts/vtune_unitrace_runs.tsv --output artifacts/EXPERIMENT_RESULTS.md

Each row of the runs TSV (workload, configuration, label, status, exit code,
log path) names one results directory, artifacts/results/<workload>/<configuration>.
The report is built from the files dwarf-check.sh writes there: vtune_manifest.json,
unitrace_manifest.json, kernel_debug.json, and vtune_vs_unitrace/*.csv.
"""

import argparse
import csv
import json
import re
import sys
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

from compare_assembly_locations import SourceLocation


STATUSES = (
    "same source location",
    "different source location",
    "VTune-only",
    "unitrace-only",
    "neither tool",
)
# Display labels for the comparator statuses; each status is about what the tools sampled.
STATUS_LABELS = {
    "same source location": "sampled by both, same source location",
    "different source location": "sampled by both, different source location",
    "VTune-only": "sampled by VTune only",
    "unitrace-only": "sampled by Unitrace only",
    "neither tool": "listed by VTune, sampled by neither",
}
# Statuses of instructions that at least one tool sampled; the percentages use these.
SAMPLED_STATUSES = STATUSES[:-1]
MAX_DIFFERENCE_EXAMPLES = 10


@dataclass
class KernelResult:
    name: str
    display_name: str
    # Every instruction in VTune's disassembly listing of the kernel, sampled or not.
    vtune_listed: int = 0
    vtune_mapped: int = 0
    # Instructions with a VTune "Stall Count" > 0; None for results analyzed before
    # dwarf-check.sh requested that column.
    vtune_stall_sampled: int | None = None
    vtune_stall_sampled_mapped: int | None = None
    unitrace_sampled: int = 0
    unitrace_mapped: int = 0
    unitrace_unmapped: int = 0
    unitrace_native_mapped: int = 0
    native_summary: dict[str, int] | None = None
    dwarf_summary: dict[str, int] | None = None
    differences: list[list[str]] = field(default_factory=list)
    # Stall-sampled IP overlap: {"both", "vtune_only", "unitrace_only"}; None without VTune stall data.
    overlap: dict[str, int] | None = None


@dataclass
class ConfigurationResult:
    workload: str
    configuration: str
    label: str
    status: str
    exit_code: str
    log_path: str
    kernels: list[KernelResult] = field(default_factory=list)
    problems: list[str] = field(default_factory=list)

    def total(self, attribute: str) -> int:
        return sum(getattr(kernel, attribute) or 0 for kernel in self.kernels)

    def has_vtune_stall_data(self) -> bool:
        return all(
            kernel.vtune_stall_sampled is not None for kernel in self.kernels if kernel.vtune_listed
        )

    def overlap_total(self) -> dict[str, int]:
        totals = {"both": 0, "vtune_only": 0, "unitrace_only": 0}
        for kernel in self.kernels:
            for key, count in (kernel.overlap or {}).items():
                totals[key] += count
        return totals

    def summary_total(self, attribute: str) -> dict[str, int]:
        totals = {status: 0 for status in STATUSES}
        for kernel in self.kernels:
            summary = getattr(kernel, attribute)
            for status in STATUSES:
                totals[status] += (summary or {}).get(status, 0)
        return totals


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--artifact-dir", type=Path, required=True, help="dwarf-check artifact root")
    parser.add_argument("--runs", type=Path, required=True, help="runs TSV written by run_vtune_unitrace_report.sh")
    parser.add_argument("--output", type=Path, required=True, help="Markdown report path")
    parser.add_argument("--json-output", type=Path, help="machine-readable summary path")
    parser.add_argument("--unitrace-bin", default="", help="unitrace executable used for collection")
    parser.add_argument("--unitrace-version", default="", help="unitrace --version output")
    parser.add_argument("--vtune-bin", default="", help="VTune executable used for collection")
    parser.add_argument("--vtune-version", default="", help="vtune --version output")
    parser.add_argument("--git-commit", default="", help="dwarf-parser-check commit description")
    parser.add_argument("--command", default="", help="command line that produced the results")
    return parser.parse_args()


def safe_name(name: str) -> str:
    # Matches the kernel file-name sanitizing in dwarf-check.sh compare_vtune_unitrace.
    return re.sub(r"[^a-zA-Z0-9_-]", "_", name)


def load_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def read_summary(path: Path) -> dict[str, int] | None:
    if not path.is_file():
        return None
    with path.open(encoding="utf-8", newline="") as stream:
        return {row["Status"]: int(row["Instruction Count"]) for row in csv.DictReader(stream)}


def parse_location(text: str) -> SourceLocation | None:
    source_file, _, line = text.rpartition(":")
    if not source_file or not line.isdigit():
        return None
    return SourceLocation(source_file, int(line))


def read_differences(path: Path) -> list[list[str]]:
    """Return VTune IP, VTune location, unitrace location, and assembly for differing rows."""
    if not path.is_file():
        return []
    differences = []
    with path.open(encoding="utf-8", newline="") as stream:
        for row in csv.DictReader(stream):
            vtune_location = parse_location(row["VTune Location"])
            unitrace_location = parse_location(row["unitrace Location"])
            if vtune_location and unitrace_location and not vtune_location.matches(unitrace_location):
                differences.append(
                    [row["VTune IP"], vtune_location.display(), unitrace_location.display(), row["Assembly Code"].strip()]
                )
    return differences


def read_ip_list(path_text: str | None) -> set[int] | None:
    if not path_text or not Path(path_text).is_file():
        return None
    return {int(line, 16) for line in Path(path_text).read_text(encoding="utf-8").split() if line}


def demangled_names(path: Path) -> dict[str, str]:
    if not path.is_file():
        return {}
    names = {}
    for kernel in load_json(path).get("kernels", []):
        demangled = kernel.get("demangled_name") or kernel["name"]
        function_name, delimiter, template_arguments = demangled.partition("<")
        names[kernel["name"]] = function_name.rsplit("::", 1)[-1] + delimiter + template_arguments
    return names


def load_configuration(artifact_dir: Path, row: list[str]) -> ConfigurationResult:
    workload, configuration, label, status, exit_code, log_path = row
    result = ConfigurationResult(workload, configuration, label, status, exit_code, log_path)
    result_dir = artifact_dir / "results" / workload / configuration
    vtune_manifest = result_dir / "vtune_manifest.json"
    unitrace_manifest = result_dir / "unitrace_manifest.json"
    for path in (vtune_manifest, unitrace_manifest):
        if not path.is_file():
            result.problems.append(f"missing {path}")
    if result.problems:
        return result

    names = demangled_names(result_dir / "kernel_debug.json")
    kernels: dict[str, KernelResult] = {}

    def kernel_result(name: str) -> KernelResult:
        if name not in kernels:
            kernels[name] = KernelResult(name, names.get(name, name))
        return kernels[name]

    vtune_ips: dict[str, set[int] | None] = {}
    unitrace_ips: dict[str, set[int] | None] = {}
    try:
        for kernel in load_json(vtune_manifest)["kernels"]:
            if kernel.get("sampled_ip_count", 0) == 0:
                continue
            entry = kernel_result(kernel["name"])
            entry.vtune_listed = kernel.get("sampled_ip_count", 0)
            entry.vtune_mapped = kernel.get("mapped_ip_count", 0)
            entry.vtune_stall_sampled = kernel.get("stall_sampled_ip_count")
            entry.vtune_stall_sampled_mapped = kernel.get("stall_sampled_mapped_ip_count")
            vtune_ips[entry.name] = read_ip_list(kernel.get("sampled_ip_list"))
        for kernel in load_json(unitrace_manifest)["kernels"]:
            entry = kernel_result(kernel["name"])
            entry.unitrace_sampled = kernel.get("sampled_ip_count", 0)
            entry.unitrace_mapped = kernel.get("mapped_ip_count", 0)
            entry.unitrace_unmapped = kernel.get("unmapped_ip_count", 0)
            entry.unitrace_native_mapped = kernel.get("native_mapped_ip_count", 0)
            unitrace_ips[entry.name] = read_ip_list(kernel.get("ip_list"))
    except (KeyError, OSError, ValueError) as error:
        result.problems.append(f"unreadable manifest: {error}")
        return result

    comparison_dir = result_dir / "vtune_vs_unitrace"
    for entry in kernels.values():
        stem = safe_name(entry.name)
        entry.native_summary = read_summary(comparison_dir / f"vtune_vs_unitrace_{stem}_summary.csv")
        entry.dwarf_summary = read_summary(comparison_dir / f"vtune_vs_unitrace_dwarf_{stem}_summary.csv")
        vtune_sampled_ips, unitrace_sampled_ips = vtune_ips.get(entry.name), unitrace_ips.get(entry.name)
        if vtune_sampled_ips is not None and unitrace_sampled_ips is not None:
            entry.overlap = {
                "both": len(vtune_sampled_ips & unitrace_sampled_ips),
                "vtune_only": len(vtune_sampled_ips - unitrace_sampled_ips),
                "unitrace_only": len(unitrace_sampled_ips - vtune_sampled_ips),
            }
        if entry.vtune_listed and entry.unitrace_sampled and entry.native_summary is None:
            result.problems.append(f"missing VTune vs unitrace comparison for {entry.display_name}")
        entry.differences = read_differences(comparison_dir / f"vtune_vs_unitrace_{stem}.csv")
    result.kernels = sorted(kernels.values(), key=lambda kernel: kernel.display_name)
    return result


def number(value: int) -> str:
    return f"{value:,}"


def percent(part: int, whole: int) -> str:
    return f"{100 * part / whole:.1f}%" if whole else "n/a"


def optional_number(value: int, available: bool) -> str:
    return number(value) if available else "n/a"


def table(header: list[str], alignments: str, rows: list[list[str]]) -> list[str]:
    separators = ["---:" if alignment == "r" else "---" for alignment in alignments]
    lines = ["| " + " | ".join(header) + " |", "| " + " | ".join(separators) + " |"]
    lines += ["| " + " | ".join(cell.replace("|", "\\|") for cell in row) + " |" for row in rows]
    return lines


def configuration_name(result: ConfigurationResult) -> str:
    return f"`{result.workload}` `{result.configuration}`"


def build_report(arguments: argparse.Namespace, results: list[ConfigurationResult]) -> tuple[str, dict]:
    with_data = [result for result in results if result.kernels]
    passed = [result for result in results if result.status == "PASS"]
    failed = [result for result in results if result.status != "PASS"]

    vtune_listed = sum(result.total("vtune_listed") for result in with_data)
    vtune_mapped = sum(result.total("vtune_mapped") for result in with_data)
    vtune_stall_available = bool(with_data) and all(result.has_vtune_stall_data() for result in with_data)
    vtune_stall_sampled = sum(result.total("vtune_stall_sampled") for result in with_data)
    vtune_stall_mapped = sum(result.total("vtune_stall_sampled_mapped") for result in with_data)
    overlap_totals = {"both": 0, "vtune_only": 0, "unitrace_only": 0}
    for result in with_data:
        for key, count in result.overlap_total().items():
            overlap_totals[key] += count
    unitrace_sampled = sum(result.total("unitrace_sampled") for result in with_data)
    unitrace_mapped = sum(result.total("unitrace_mapped") for result in with_data)
    unitrace_unmapped = sum(result.total("unitrace_unmapped") for result in with_data)
    unitrace_native = sum(result.total("unitrace_native_mapped") for result in with_data)
    native_totals = {status: 0 for status in STATUSES}
    dwarf_totals = {status: 0 for status in STATUSES}
    for result in with_data:
        for status, count in result.summary_total("native_summary").items():
            native_totals[status] += count
        for status, count in result.summary_total("dwarf_summary").items():
            dwarf_totals[status] += count

    workloads = sorted({result.workload for result in results})
    configurations = sorted({result.configuration for result in results})
    lines = [
        "# VTune vs Unitrace Experiment Results",
        "",
        "This report is generated by `run_vtune_unitrace_report.sh`; every number below",
        "is read from the result files of the run it describes.",
        "",
        "## Scope",
        "",
        f"- Generated: {datetime.now().astimezone().strftime('%Y-%m-%d %H:%M %Z')}",
    ]
    if arguments.git_commit:
        lines.append(f"- dwarf-parser-check commit: `{arguments.git_commit}`")
    if arguments.unitrace_bin:
        version = f" ({arguments.unitrace_version})" if arguments.unitrace_version else ""
        lines.append(f"- Unitrace: `{arguments.unitrace_bin}`{version}")
    if arguments.vtune_bin:
        version = f" ({arguments.vtune_version})" if arguments.vtune_version else ""
        lines.append(f"- VTune: `{arguments.vtune_bin}`{version}")
    lines += [
        f"- Workloads: {', '.join(f'`{workload}`' for workload in workloads)}",
        f"- Configurations: {', '.join(f'`{configuration}`' for configuration in configurations)}",
    ]
    if arguments.command:
        lines.append(f"- Command: `{arguments.command}`")

    lines += ["", "## Matrix Outcome", ""]
    lines.append(
        f"{len(passed)} of {len(results)} workload/configuration runs completed successfully."
    )
    if failed:
        lines += ["", "Failed runs:", ""]
        lines += [
            f"- {configuration_name(result)}: exit code {result.exit_code}; log `{result.log_path}`"
            for result in failed
        ]
    incomplete = [result for result in results if result.problems]
    if incomplete:
        lines += ["", "Runs with missing or incomplete data (excluded from the totals where absent):", ""]
        lines += [f"- {configuration_name(result)}: {'; '.join(result.problems)}" for result in incomplete]

    lines += ["", "## Collector Coverage", ""]
    lines += table(
        ["Collector", "Stall-sampled IPs", "Sampled IPs with source location", "Coverage"],
        "lrrr",
        [
            ["VTune", optional_number(vtune_stall_sampled, vtune_stall_available),
             optional_number(vtune_stall_mapped, vtune_stall_available),
             percent(vtune_stall_mapped, vtune_stall_sampled) if vtune_stall_available else "n/a"],
            ["Unitrace, via zebin DWARF", number(unitrace_sampled), number(unitrace_mapped),
             percent(unitrace_mapped, unitrace_sampled)],
            ["Unitrace, own `Source` column", number(unitrace_sampled), number(unitrace_native),
             percent(unitrace_native, unitrace_sampled)],
        ],
    )
    lines += [
        "",
        "Both tools use GPU stall sampling. *Stall-sampled IPs* are the",
        "instructions that received at least one stall sample (VTune `Stall Count` > 0;",
        "a row in Unitrace's `metrics_*.csv`). *Coverage* is the share of stall-sampled IPs",
        "that map to a source location.",
    ]
    if not vtune_stall_available:
        lines += [
            "",
            "VTune stall counts are missing for some runs (results analyzed before",
            "`Stall Count` was requested); re-run `./dwarf-check.sh <options> analyze",
            "--adapters none` for those configurations to add them.",
        ]

    lines += ["", "## Stall Sample Overlap", ""]
    if vtune_stall_available:
        union = sum(overlap_totals.values())
        lines += [
            "Instructions that received at least one stall sample, compared per kernel.",
            "The percentages are shares of the instructions sampled by at least one tool.",
            "",
        ]
        lines += table(
            ["Stall-sampled instructions", "Count", "% of all sampled"],
            "lrr",
            [
                ["sampled by both tools", number(overlap_totals["both"]), percent(overlap_totals["both"], union)],
                ["sampled by VTune only", number(overlap_totals["vtune_only"]),
                 percent(overlap_totals["vtune_only"], union)],
                ["sampled by Unitrace only", number(overlap_totals["unitrace_only"]),
                 percent(overlap_totals["unitrace_only"], union)],
            ],
        )
    else:
        lines.append("Not available: VTune stall counts are missing (see Collector Coverage).")

    lines += ["", "## VTune vs Unitrace Source Locations", ""]
    lines += [
        "Every instruction that either tool sampled is classified by what each tool",
        "sampled and, when both did, by whether their source locations match.",
        "*Unitrace-reported* uses Unitrace's own `Source` column; *DWARF-mapped* maps",
        "Unitrace IPs through the zebin DWARF line table used for VTune. The percentages",
        "are shares of the instructions sampled by at least one tool; the last row counts",
        "the instructions in VTune's listing that neither tool sampled.",
        "",
    ]
    native_total = sum(native_totals[status] for status in SAMPLED_STATUSES)
    dwarf_total = sum(dwarf_totals[status] for status in SAMPLED_STATUSES)
    lines += table(
        ["Instruction status", "Unitrace-reported", "%", "DWARF-mapped", "%"],
        "lrrrr",
        [
            [STATUS_LABELS[status], number(native_totals[status]), percent(native_totals[status], native_total),
             number(dwarf_totals[status]), percent(dwarf_totals[status], dwarf_total)]
            for status in SAMPLED_STATUSES
        ] + [[STATUS_LABELS["neither tool"], number(native_totals["neither tool"]), "",
              number(dwarf_totals["neither tool"]), ""]],
    )
    both_native = native_totals["same source location"] + native_totals["different source location"]
    lines += [
        "",
        f"Of the {number(both_native)} instructions sampled by both tools,",
        f"{number(native_totals['same source location'])} ({percent(native_totals['same source location'], both_native)})"
        " have the same source location in VTune and in Unitrace's own report, and",
        f"{number(native_totals['different source location'])} differ.",
    ]

    lines += ["", "## Per-Configuration Results", ""]
    rows = []
    for result in results:
        if not result.kernels:
            rows.append([configuration_name(result), result.status] + ["n/a"] * 6)
            continue
        summary = result.summary_total("native_summary")
        stall_available = result.has_vtune_stall_data()
        overlap = result.overlap_total()
        rows.append([
            configuration_name(result),
            result.status,
            optional_number(result.total("vtune_stall_sampled"), stall_available),
            number(result.total("unitrace_sampled")),
            optional_number(overlap["both"], stall_available),
            percent(result.total("unitrace_mapped"), result.total("unitrace_sampled")),
            number(summary["same source location"]),
            number(summary["different source location"]),
        ])
    lines += table(
        ["Configuration", "Status", "VTune stall-sampled",
         "Unitrace stall-sampled", "Sampled by both", "Unitrace DWARF coverage", "Same", "Different"],
        "llrrrrrr",
        rows,
    )
    lines += ["", "Same/Different compare source locations using the Unitrace-reported locations."]

    lines += ["", "## Unitrace Coverage Gaps", ""]
    dominant = []
    for result in with_data:
        if not result.configuration.startswith("jit-") or not result.configuration.endswith("-O0"):
            continue
        kernel = max(result.kernels, key=lambda entry: entry.unitrace_unmapped)
        if kernel.unitrace_unmapped > 0:
            dominant.append((result, kernel))
    if dominant:
        rows = [
            [f"`{result.workload}` `{kernel.display_name}`",
             f"{number(kernel.unitrace_mapped)} / {number(kernel.unitrace_sampled)}",
             number(kernel.unitrace_unmapped)]
            for result, kernel in dominant
        ]
        dominant_mapped = sum(kernel.unitrace_mapped for _, kernel in dominant)
        dominant_sampled = sum(kernel.unitrace_sampled for _, kernel in dominant)
        dominant_unmapped = sum(kernel.unitrace_unmapped for _, kernel in dominant)
        rows.append([
            "**Total dominant exclusions**",
            f"**{number(dominant_mapped)} / {number(dominant_sampled)}**",
            f"**{number(dominant_unmapped)}**",
        ])
        lines += table(
            ["JIT `-O0` caller kernel", "Unitrace mapped / sampled IPs", "IPs outside caller-kernel DWARF range"],
            "lrr",
            rows,
        )
        lines += [
            "",
            "Each row is the kernel with the most unmapped Unitrace IPs in its JIT `-O0` run.",
            f"Together these rows account for {number(dominant_unmapped)} of the",
            f"{number(unitrace_unmapped)} unmapped Unitrace IPs; the remaining",
            f"{number(unitrace_unmapped - dominant_unmapped)} are spread across the other kernels and configurations.",
        ]
    elif unitrace_unmapped:
        lines.append(
            f"No JIT `-O0` kernel has unmapped Unitrace IPs; {number(unitrace_unmapped)} unmapped IPs "
            "occur in other configurations."
        )
    else:
        lines.append("Every sampled Unitrace IP maps to a source location through the zebin DWARF.")
    gap_rows = [
        [configuration_name(result), f"`{kernel.display_name}`",
         f"{number(kernel.unitrace_mapped)} / {number(kernel.unitrace_sampled)}", number(kernel.unitrace_unmapped)]
        for result in with_data
        for kernel in result.kernels
        if kernel.unitrace_unmapped > 0
    ]
    if gap_rows:
        lines += ["", "All kernels with unmapped Unitrace IPs:", ""]
        lines += table(["Configuration", "Kernel", "Mapped / sampled", "Unmapped"], "llrr", gap_rows)

    lines += ["", "## Source Location Differences", ""]
    differing = [
        (result, kernel) for result in with_data for kernel in result.kernels if kernel.differences
    ]
    if not differing:
        lines.append("VTune and Unitrace report the same source location for every instruction both sampled.")
    for index, (result, kernel) in enumerate(differing):
        lines += [
            *([""] if index else []),
            f"### {configuration_name(result)} `{kernel.display_name}`",
            "",
            f"{number(len(kernel.differences))} " + ("instruction differs" if len(kernel.differences) == 1 else "instructions differ")
            + (f"; the first {MAX_DIFFERENCE_EXAMPLES} are shown." if len(kernel.differences) > MAX_DIFFERENCE_EXAMPLES else "."),
            "",
        ]
        lines += table(
            ["IP", "VTune location", "Unitrace location", "Assembly"],
            "llll",
            [[f"`{ip}`", f"`{Path(vtune).name}`", f"`{Path(unitrace).name}`", f"`{assembly}`"]
             for ip, vtune, unitrace, assembly in kernel.differences[:MAX_DIFFERENCE_EXAMPLES]],
        )

    summary = {
        "runs": {"total": len(results), "passed": len(passed), "failed": len(failed)},
        "coverage": {
            "vtune": {
                "listed_instructions": vtune_listed,
                "listed_mapped_instructions": vtune_mapped,
                "stall_sampled_ips": vtune_stall_sampled if vtune_stall_available else None,
                "stall_sampled_mapped_ips": vtune_stall_mapped if vtune_stall_available else None,
            },
            "unitrace": {
                "sampled_ips": unitrace_sampled,
                "dwarf_mapped_ips": unitrace_mapped,
                "unmapped_ips": unitrace_unmapped,
                "native_mapped_ips": unitrace_native,
            },
        },
        "stall_sample_overlap": overlap_totals if vtune_stall_available else None,
        "comparison": {"unitrace_reported": native_totals, "dwarf_mapped": dwarf_totals},
        "experiments": [
            {
                "workload": result.workload,
                "configuration": result.configuration,
                "label": result.label,
                "status": result.status,
                "exit_code": result.exit_code,
                "log": result.log_path,
                "problems": result.problems,
                "kernels": [
                    {
                        "name": kernel.name,
                        "display_name": kernel.display_name,
                        "vtune_listed_instructions": kernel.vtune_listed,
                        "vtune_listed_mapped_instructions": kernel.vtune_mapped,
                        "vtune_stall_sampled_ips": kernel.vtune_stall_sampled,
                        "vtune_stall_sampled_mapped_ips": kernel.vtune_stall_sampled_mapped,
                        "stall_sample_overlap": kernel.overlap,
                        "unitrace_sampled_ips": kernel.unitrace_sampled,
                        "unitrace_dwarf_mapped_ips": kernel.unitrace_mapped,
                        "unitrace_unmapped_ips": kernel.unitrace_unmapped,
                        "unitrace_native_mapped_ips": kernel.unitrace_native_mapped,
                        "unitrace_reported_comparison": kernel.native_summary,
                        "dwarf_mapped_comparison": kernel.dwarf_summary,
                        "different_locations": len(kernel.differences),
                    }
                    for kernel in result.kernels
                ],
            }
            for result in results
        ],
    }
    return "\n".join(lines) + "\n", summary


def main() -> int:
    arguments = parse_arguments()
    try:
        with arguments.runs.open(encoding="utf-8", newline="") as stream:
            rows = [row for row in csv.reader(stream, delimiter="\t") if row]
        if not rows:
            raise ValueError(f"no runs listed in {arguments.runs}")
        results = [load_configuration(arguments.artifact_dir, row) for row in rows]
        report, summary = build_report(arguments, results)
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(report, encoding="utf-8")
        print(f"generate_vtune_unitrace_report: report written to {arguments.output}")
        if arguments.json_output:
            arguments.json_output.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
            print(f"generate_vtune_unitrace_report: summary written to {arguments.json_output}")
    except (OSError, ValueError) as error:
        print(f"generate_vtune_unitrace_report: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
