#!/usr/bin/env python3
"""Join addressed GPU assembly with VTune and unitrace source locations.

Usage:
  python3 compare_assembly_locations.py ASSEMBLY_IP VTUNE_REFERENCE \
      UNITRACE_REFERENCE --vtune-sampled-ips VTUNE_SAMPLED_IPS --output COMPARISON.csv

VTUNE_REFERENCE lists every instruction of the kernel binary that ran, sampled or
not; VTUNE_SAMPLED_IPS holds the kernel offsets VTune stall-sampled; UNITRACE_REFERENCE
holds the instructions unitrace sampled. Each reference CSV must contain
``Kernel Offset``, ``Source File``, and ``Source Line`` columns.

The output contains every instruction in VTUNE_REFERENCE plus any unitrace-sampled
instruction missing from it, in offset order. ASSEMBLY_IP only supplies the
assembly text. An empty IP/location pair means that the corresponding tool did not
sample that instruction. A companion summary CSV categorizes each instruction as
sampled by both with a matching or mismatched location, sampled by VTune only,
sampled by unitrace only, or sampled by neither.
"""

import argparse
import csv
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path


IP_MARKER = re.compile(r"^/\* \[([0-9A-Fa-f]{8})\] \*/\s?(.*)$")


@dataclass(frozen=True)
class SourceLocation:
    file: str
    line: int

    def display(self) -> str:
        return f"{self.file}:{self.line}"

    def matches(self, other: "SourceLocation") -> bool:
        if self.line != other.line:
            return False
        own_file, other_file = os.path.normpath(self.file), os.path.normpath(other.file)
        if own_file == other_file:
            return True
        # unitrace may report a bare file name; then only the file names can be compared.
        if os.path.dirname(own_file) and os.path.dirname(other_file):
            return False
        return os.path.basename(own_file) == os.path.basename(other_file)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("assembly", type=Path, help="addressed .asm.ip listing")
    parser.add_argument("vtune_reference", type=Path, help="VTune reference CSV")
    parser.add_argument("unitrace_reference", type=Path, help="unitrace reference CSV")
    parser.add_argument(
        "--vtune-sampled-ips",
        type=Path,
        required=True,
        help="kernel offsets VTune stall-sampled, one per line",
    )
    parser.add_argument("--output", type=Path, required=True, help="comparison CSV path")
    parser.add_argument(
        "--summary-output",
        type=Path,
        help="summary CSV path (default: OUTPUT with _summary before .csv)",
    )
    return parser.parse_args()


def parse_locations(path: Path) -> dict[int, SourceLocation]:
    with path.open(encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream)
        expected = {"Kernel Offset", "Source File", "Source Line"}
        if reader.fieldnames is None or not expected.issubset(reader.fieldnames):
            raise ValueError(
                f"{path} must contain columns: {', '.join(sorted(expected))}"
            )
        locations: dict[int, SourceLocation] = {}
        for row_number, row in enumerate(reader, start=2):
            try:
                offset = int(row["Kernel Offset"], 0)
                line = int(row["Source Line"])
            except (KeyError, TypeError, ValueError) as error:
                raise ValueError(f"invalid location in {path}:{row_number}") from error
            source_file = row["Source File"].strip()
            if not source_file:
                raise ValueError(f"empty source file in {path}:{row_number}")
            location = SourceLocation(source_file, line)
            if offset in locations and locations[offset] != location:
                raise ValueError(f"conflicting source locations for {offset:#x} in {path}")
            locations[offset] = location
    return locations


def read_ip_list(path: Path) -> set[int]:
    with path.open(encoding="utf-8") as stream:
        return {int(token, 16) for token in stream.read().split()}


def assembly_instructions(path: Path) -> dict[int, str]:
    instructions: dict[int, str] = {}
    with path.open(encoding="utf-8", errors="replace") as stream:
        for line_number, raw_line in enumerate(stream, start=1):
            match = IP_MARKER.match(raw_line.rstrip("\n"))
            if match is None:
                continue
            assembly = match.group(2).strip()
            if not assembly or assembly.endswith(":"):
                continue
            instructions[int(match.group(1), 16)] = assembly
    if not instructions:
        raise ValueError(f"no IP-prefixed instructions found in {path}")
    return instructions


def tool_columns(offset: int, location: SourceLocation | None) -> tuple[str, str]:
    if location is None:
        return "", ""
    return f"{offset:#x}", location.display()


def write_comparison(
    output: Path,
    instructions: dict[int, str],
    vtune_listing: dict[int, SourceLocation],
    vtune_sampled: set[int],
    unitrace_locations: dict[int, SourceLocation],
) -> dict[str, int]:
    output.parent.mkdir(parents=True, exist_ok=True)
    summary = {
        "same source location": 0,
        "different source location": 0,
        "VTune-only": 0,
        "unitrace-only": 0,
        "neither tool": 0,
    }
    with output.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(
            ["VTune IP", "VTune Location", "unitrace IP", "unitrace Location", "Assembly Code"]
        )
        for offset in sorted(vtune_listing.keys() | unitrace_locations.keys()):
            vtune_source_location = vtune_listing.get(offset) if offset in vtune_sampled else None
            unitrace_source_location = unitrace_locations.get(offset)
            vtune_ip, vtune_location = tool_columns(offset, vtune_source_location)
            unitrace_ip, unitrace_location = tool_columns(offset, unitrace_source_location)
            assembly = instructions.get(offset, "")
            if vtune_source_location and unitrace_source_location:
                status = (
                    "same source location"
                    if vtune_source_location.matches(unitrace_source_location)
                    else "different source location"
                )
            elif vtune_source_location:
                status = "VTune-only"
            elif unitrace_source_location:
                status = "unitrace-only"
            else:
                status = "neither tool"
            summary[status] += 1
            writer.writerow([vtune_ip, vtune_location, unitrace_ip, unitrace_location, assembly])
    return summary


def default_summary_path(output: Path) -> Path:
    return output.with_name(f"{output.stem}_summary{output.suffix}")


def write_summary(path: Path, summary: dict[str, int]) -> None:
    total = sum(summary.values())
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["Status", "Instruction Count", "Percentage"])
        for status, count in summary.items():
            percentage = 100 * count / total if total else 0
            writer.writerow([status, count, f"{percentage:.2f}"])
        writer.writerow(["total", total, "100.00"])


def main() -> int:
    arguments = parse_arguments()
    try:
        instructions = assembly_instructions(arguments.assembly)
        vtune_listing = parse_locations(arguments.vtune_reference)
        vtune_sampled = read_ip_list(arguments.vtune_sampled_ips)
        unitrace_locations = parse_locations(arguments.unitrace_reference)
        summary = write_comparison(
            arguments.output, instructions, vtune_listing, vtune_sampled, unitrace_locations
        )
        summary_output = arguments.summary_output or default_summary_path(arguments.output)
        write_summary(summary_output, summary)
        print(
            f"wrote {sum(summary.values())} instructions; "
            f"same={summary['same source location']}, "
            f"different={summary['different source location']}, "
            f"VTune-only={summary['VTune-only']}, "
            f"unitrace-only={summary['unitrace-only']} -> {arguments.output}"
        )
        print(f"wrote summary -> {summary_output}")
        return 0
    except (OSError, ValueError) as error:
        print(f"compare_assembly_locations: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())