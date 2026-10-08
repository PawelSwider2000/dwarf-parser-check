#!/usr/bin/env python3
"""Convert unitrace stall-sampling CSV files into resolver input artifacts."""

import argparse
import csv
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

from annotate_zebin_asm import line_locations


# Unitrace (2.4+) writes asm/<device>.<pid>.<kernel id>.asm for every kernel it ran.
ASM_KERNEL_HEADER = re.compile(r"^//\s*(Kernel|Mangled name):\s*(.+)$")
ASM_INSTRUCTION = re.compile(r"^/\*\s*#\d+\s+\[([0-9A-Fa-f]+)\]\s*\*/\s?(.*)$")
ASM_SOURCE_RANGE = re.compile(r"^//\s*([0-9A-Fa-f]+)-([0-9A-Fa-f]+)\s+(.+?)(?:\s+\[[^\]]*\])?\s*$")
ASM_SOURCE_TABLE = "// Source locations, by offset in the kernel:"

def safe_name(value: str) -> str:
    return re.sub(r"[^a-zA-Z0-9_-]", "_", value)


def normalized_kernel_name(value: str) -> str:
    return value.strip().strip('"')


def parse_ip(value: str) -> int:
    value = value.strip()
    if value.lower().startswith("0x"):
        value = value[2:]
    return int(value, 16)


def parse_source_location(value: str) -> tuple[str, int] | None:
    """Parses a unitrace ``file:line[:column]`` Source field."""
    parts = value.strip().split(":")
    while len(parts) > 1 and not parts[-1].isdigit():
        parts.pop()
    if len(parts) >= 3 and parts[-1].isdigit() and parts[-2].isdigit():
        parts.pop()  # drop the column
    if len(parts) < 2 or not parts[-1].isdigit():
        return None
    source_file = ":".join(parts[:-1])
    if not source_file or source_file == "NA":
        return None
    return source_file, int(parts[-1])


def collect_samples(
    result_dir: Path,
) -> tuple[dict[str, set[int]], dict[str, dict[int, tuple[str, int]]]]:
    """Returns sampled IPs and unitrace-reported source locations per kernel.

    Two metrics layouts are supported:
      * flat: one header with ``Kernel`` and ``IP`` columns;
      * sectioned (unitrace 2.x): a ``Kernel,PID,KernelID`` header and kernel row,
        followed by a ``Source,Offset,Instruction,...`` table for that kernel.
    """
    reports = sorted(result_dir.rglob("metrics_*.csv"))
    if not reports:
        raise ValueError(f"no unitrace metrics_*.csv files found under {result_dir}")

    samples: dict[str, set[int]] = defaultdict(set)
    sources: dict[str, dict[int, tuple[str, int]]] = defaultdict(dict)
    for report in reports:
        with report.open(encoding="utf-8", errors="replace", newline="") as stream:
            reader = csv.reader(stream, skipinitialspace=True)
            kernel_index = None
            ip_index = None
            source_index = None
            section_kernel_index = None
            section_kernel = None
            for row in reader:
                fields = [field.strip() for field in row]
                if not any(fields):
                    continue
                ip_columns = [
                    index for index, field in enumerate(fields)
                    if field == "IP" or field.startswith("IP[")
                ]
                if "Kernel" in fields and ip_columns:
                    kernel_index = fields.index("Kernel")
                    ip_index = ip_columns[0]
                    source_index = fields.index("Source") if "Source" in fields else None
                    section_kernel_index = None
                    continue
                if "Kernel" in fields and not ip_columns and "Offset" not in fields:
                    section_kernel_index = fields.index("Kernel")
                    section_kernel = None
                    kernel_index = ip_index = None
                    continue
                if section_kernel_index is not None and section_kernel is None:
                    if len(row) > section_kernel_index:
                        section_kernel = normalized_kernel_name(row[section_kernel_index])
                    continue
                if "Offset" in fields and "Source" in fields and section_kernel is not None:
                    kernel_index = None
                    ip_index = fields.index("Offset")
                    source_index = fields.index("Source")
                    continue
                if ip_index is None:
                    continue
                if kernel_index is not None:
                    if len(row) <= max(kernel_index, ip_index):
                        continue
                    kernel = normalized_kernel_name(row[kernel_index])
                else:
                    if len(row) <= ip_index:
                        continue
                    kernel = section_kernel
                ip = row[ip_index].strip()
                if not kernel or not ip:
                    continue
                try:
                    offset = parse_ip(ip)
                except ValueError as error:
                    raise ValueError(f"invalid IP '{ip}' in {report}") from error
                samples[kernel].add(offset)
                if source_index is not None and len(row) > source_index:
                    location = parse_source_location(row[source_index])
                    if location is not None:
                        sources[kernel].setdefault(offset, location)

    if not samples:
        raise ValueError(
            f"no unitrace stall samples found in {result_dir}; "
            f"parsed {len(reports)} metrics file(s): {', '.join(str(report) for report in reports)}"
        )
    return samples, sources


def unitrace_asm_by_kernel(result_dir: Path) -> dict[str, Path]:
    """Maps kernel names (mangled and demangled) to unitrace's asm dump of that kernel.

    A kernel launched several times has one file per launch; they hold the same code,
    so the first one is used.
    """
    by_kernel: dict[str, Path] = {}
    for assembly_file in sorted(result_dir.rglob("asm/*.asm")):
        with assembly_file.open(encoding="utf-8", errors="replace") as stream:
            for line in stream:
                match = ASM_KERNEL_HEADER.match(line.rstrip("\n"))
                if match is None:
                    if not line.startswith("//"):
                        break
                    continue
                by_kernel.setdefault(normalized_kernel_name(match.group(2)), assembly_file)
    return by_kernel


def parse_unitrace_asm(
    assembly_file: Path,
) -> tuple[dict[int, str], dict[int, tuple[str, int]]]:
    """Returns the instructions and their source locations from a unitrace asm dump.

    Instructions look like ``/*   #3 [0030]  */ (W) add ...``; the source table at the
    end lists ``// <start>-<end> <file>:<line>[:<column>] [not a statement]`` ranges.
    """
    instructions: dict[int, str] = {}
    ranges: list[tuple[int, int, tuple[str, int]]] = []
    in_source_table = False
    with assembly_file.open(encoding="utf-8", errors="replace") as stream:
        for raw_line in stream:
            line = raw_line.rstrip("\n")
            if (match := ASM_INSTRUCTION.match(line)) is not None:
                instructions[int(match.group(1), 16)] = match.group(2).rstrip()
            elif line.strip() == ASM_SOURCE_TABLE:
                in_source_table = True
            elif in_source_table:
                match = ASM_SOURCE_RANGE.match(line)
                if match is None:
                    in_source_table = False
                    continue
                location = parse_source_location(match.group(3))
                if location is not None:
                    ranges.append((int(match.group(1), 16), int(match.group(2), 16), location))

    if not instructions:
        raise ValueError(f"no instructions found in unitrace asm dump {assembly_file}")
    locations = {
        offset: location
        for start, end, location in ranges
        for offset in instructions
        if start <= offset < end
    }
    return instructions, locations


def write_addressed_assembly(instructions: dict[int, str], output: Path) -> Path:
    """Writes instructions in the ``/* [XXXXXXXX] */ text`` form compare_assembly_locations.py reads."""
    with output.open("w", encoding="utf-8") as stream:
        for offset, text in sorted(instructions.items()):
            stream.write(f"/* [{offset:08X}] */ {text}\n")
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--result-dir", type=Path, required=True)
    parser.add_argument("--kernel-debug-json", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--manifest-output", type=Path, required=True)
    arguments = parser.parse_args()

    try:
        samples, native_sources = collect_samples(arguments.result_dir)
        assembly_by_kernel = unitrace_asm_by_kernel(arguments.result_dir)
        kernel_manifest = json.loads(arguments.kernel_debug_json.read_text(encoding="utf-8"))
        kernels = kernel_manifest.get("kernels", [])
        if not kernels:
            raise ValueError(f"no kernels found in {arguments.kernel_debug_json}")

        sample_names = set(samples)
        kernels_by_name = {
            normalized_kernel_name(kernel["demangled_name"]): kernel
            for kernel in kernels
        }
        unmatched = sorted(sample_names - set(kernels_by_name))
        if unmatched:
            print(
                "extract_unitrace_stalls: ignoring samples for kernels absent from kernel_debug.json: "
                + ", ".join(unmatched),
                file=sys.stderr,
            )

        arguments.output_dir.mkdir(parents=True, exist_ok=True)
        manifest_kernels = []
        for unitrace_name, kernel in kernels_by_name.items():
            offsets = sorted(samples.get(unitrace_name, set()))
            if not offsets:
                continue
            kernel_name = kernel["name"]
            safe_kernel_name = safe_name(kernel_name)
            assembly_file = assembly_by_kernel.get(kernel_name) or assembly_by_kernel.get(unitrace_name)
            if assembly_file is None:
                raise ValueError(
                    f"unitrace asm dump not found for {kernel_name} under {arguments.result_dir}; "
                    "unitrace 2.4 or newer is required"
                )
            instructions, asm_locations = parse_unitrace_asm(assembly_file)
            addressed_assembly = write_addressed_assembly(
                instructions, arguments.output_dir / f"unitrace_asm_{safe_kernel_name}.asm.ip"
            )
            native_zebin = Path(kernel.get("module_native_binary_path", ""))
            if not native_zebin.is_file():
                raise ValueError(
                    f"native zebin for {kernel_name} is unavailable: {native_zebin}"
                )
            dwarf_locations = line_locations(native_zebin, offsets, kernel_name)

            asm_references = [
                (offset, *asm_locations[offset]) for offset in offsets if offset in asm_locations
            ]

            native_references = [
                (offset, *native_sources[unitrace_name][offset])
                for offset in offsets
                if offset in native_sources.get(unitrace_name, {})
            ]

            dwarf_references = []
            for offset in offsets:
                location = dwarf_locations.get(offset)
                if location is not None:
                    dwarf_references.append((offset, location.file, location.line))
            if not dwarf_references:
                raise ValueError(f"no unitrace sample offsets mapped through DWARF for {kernel_name}")

            asm_reference_path = arguments.output_dir / f"unitrace_asm_reference_{safe_kernel_name}.csv"
            dwarf_reference_path = arguments.output_dir / f"unitrace_dwarf_reference_{safe_kernel_name}.csv"
            native_reference_path = arguments.output_dir / f"unitrace_native_reference_{safe_kernel_name}.csv"
            ip_list_path = arguments.output_dir / f"unitrace_ips_{safe_kernel_name}.txt"
            for reference_path, references in (
                (asm_reference_path, asm_references),
                (dwarf_reference_path, dwarf_references),
                (native_reference_path, native_references),
            ):
                with reference_path.open("w", encoding="utf-8", newline="") as stream:
                    writer = csv.writer(stream)
                    writer.writerow(["Kernel Offset", "Source File", "Source Line"])
                    writer.writerows(
                        (hex(offset), source_file, source_line)
                        for offset, source_file, source_line in references
                    )
            with ip_list_path.open("w", encoding="utf-8") as stream:
                for offset in offsets:
                    stream.write(f"{offset:#x}\n")
            manifest_kernels.append(
                {
                    "name": kernel_name,
                    "reference_csv": str(dwarf_reference_path.resolve()),
                    "marker_reference_csv": str(asm_reference_path.resolve()),
                    "native_reference_csv": str(native_reference_path.resolve()),
                    "ip_list": str(ip_list_path.resolve()),
                    "addressed_shader_dump": str(addressed_assembly.resolve()),
                    "unitrace_asm": str(assembly_file.resolve()),
                    "sampled_ip_count": len(offsets),
                    "mapped_ip_count": len(dwarf_references),
                    "unmapped_ip_count": len(offsets) - len(dwarf_references),
                    "marker_mapped_ip_count": len(asm_references),
                    "native_mapped_ip_count": len(native_references),
                    "section_file_offset": "0x0",
                }
            )
            print(
                f"extract_unitrace_stalls: wrote {len(dwarf_references)} DWARF and "
                f"{len(asm_references)} unitrace-asm and {len(native_references)} "
                f"unitrace-metrics locations for {kernel_name}"
            )

        if not manifest_kernels:
            raise ValueError("no unitrace samples matched kernels in kernel_debug.json")
        arguments.manifest_output.parent.mkdir(parents=True, exist_ok=True)
        arguments.manifest_output.write_text(
            json.dumps({"kernels": manifest_kernels}, indent=2) + "\n",
            encoding="utf-8",
        )
        print(f"extract_unitrace_stalls: manifest written to {arguments.manifest_output}")
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"extract_unitrace_stalls: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())