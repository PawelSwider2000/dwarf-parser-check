#!/usr/bin/env python3
"""Convert unitrace stall-sampling CSV files into resolver input artifacts."""

import argparse
import csv
import json
import re
import sys
from collections import defaultdict
from pathlib import Path


FILE_MARKER = re.compile(r"^//\s*File:\s*(.+)$")
LINE_MARKER = re.compile(r"^//\s*Line\s+(\d+):")


def safe_name(value: str) -> str:
    return re.sub(r"[^a-zA-Z0-9_-]", "_", value)


def normalized_kernel_name(value: str) -> str:
    return value.strip().strip('"')


def parse_ip(value: str) -> int:
    value = value.strip()
    if value.lower().startswith("0x"):
        value = value[2:]
    return int(value, 16)


def collect_samples(result_dir: Path) -> dict[str, set[int]]:
    reports = sorted(result_dir.rglob("metrics_*.csv"))
    if not reports:
        raise ValueError(f"no unitrace metrics_*.csv files found under {result_dir}")

    samples: dict[str, set[int]] = defaultdict(set)
    for report in reports:
        with report.open(encoding="utf-8", errors="replace", newline="") as stream:
            reader = csv.reader(stream, skipinitialspace=True)
            kernel_index = None
            ip_index = None
            for row in reader:
                fields = [field.strip() for field in row]
                ip_columns = [
                    index for index, field in enumerate(fields)
                    if field == "IP" or field.startswith("IP[")
                ]
                if "Kernel" in fields and ip_columns:
                    kernel_index = fields.index("Kernel")
                    ip_index = ip_columns[0]
                    continue
                if kernel_index is None or ip_index is None:
                    continue
                if len(row) <= max(kernel_index, ip_index):
                    continue
                kernel = normalized_kernel_name(row[kernel_index])
                ip = row[ip_index].strip()
                if not kernel or not ip:
                    continue
                try:
                    samples[kernel].add(parse_ip(ip))
                except ValueError as error:
                    raise ValueError(f"invalid IP '{ip}' in {report}") from error

    if not samples:
        raise ValueError(f"no unitrace stall samples found in {result_dir}")
    return samples


def is_instruction(line: str) -> bool:
    return not line.startswith("//") and "//" in line


def shader_dump_for_kernel(shader_dump_dir: Path, kernel_name: str) -> Path:
    matching_files = []
    kernel_marker = f"//.kernel {kernel_name}"
    for assembly_file in sorted(shader_dump_dir.glob("*.asm")):
        with assembly_file.open(encoding="utf-8", errors="replace") as stream:
            if any(line.rstrip("\n") == kernel_marker for line in stream):
                matching_files.append(assembly_file)

    if not matching_files:
        raise ValueError(f"no IGC shader dump found for {kernel_name} in {shader_dump_dir}")
    if len(matching_files) > 1:
        raise ValueError(f"multiple IGC shader dumps found for {kernel_name}: {matching_files}")
    return matching_files[0]


def write_addressed_shader_dump(assembly_file: Path) -> Path:
    addressed_file = assembly_file.with_suffix(assembly_file.suffix + ".ip")
    instruction_pointer = 0
    with assembly_file.open(encoding="utf-8", errors="replace") as source, addressed_file.open(
        "w", encoding="utf-8"
    ) as output:
        for raw_line in source:
            line = raw_line.rstrip("\n")
            if is_instruction(line):
                output.write(f"/* [{instruction_pointer:08X}] */ {raw_line}")
                instruction_pointer += 0x8 if "Compacted" in line else 0x10
            else:
                output.write(raw_line)
    return addressed_file


def shader_locations(assembly_file: Path) -> dict[int, tuple[str, int]]:

    locations: dict[int, tuple[str, int]] = {}
    current_file: str | None = None
    current_line: int | None = None
    instruction_pointer = 0
    with assembly_file.open(encoding="utf-8", errors="replace") as stream:
        for raw_line in stream:
            line = raw_line.rstrip("\n")
            file_marker = FILE_MARKER.match(line)
            if file_marker is not None:
                current_file = file_marker.group(1)
                continue
            line_marker = LINE_MARKER.match(line)
            if line_marker is not None:
                current_line = int(line_marker.group(1))
                continue
            if not is_instruction(line):
                continue
            if current_file is not None and current_line is not None:
                locations[instruction_pointer] = (current_file, current_line)
            instruction_pointer += 0x8 if "Compacted" in line else 0x10

    if not locations:
        raise ValueError(f"no source locations found in {assembly_file}")
    return locations


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--result-dir", type=Path, required=True)
    parser.add_argument("--kernel-debug-json", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--manifest-output", type=Path, required=True)
    parser.add_argument("--shader-dump-dir", type=Path, required=True)
    arguments = parser.parse_args()

    try:
        samples = collect_samples(arguments.result_dir)
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
            shader_dump = shader_dump_for_kernel(arguments.shader_dump_dir, kernel_name)
            addressed_shader_dump = write_addressed_shader_dump(shader_dump)
            locations = shader_locations(shader_dump)

            references = []
            for offset in offsets:
                location = locations.get(offset)
                if location is not None:
                    references.append((offset, *location))
            if not references:
                raise ValueError(f"no unitrace sample offsets mapped to IGC shader source for {kernel_name}")

            safe_kernel_name = safe_name(kernel_name)
            reference_path = arguments.output_dir / f"unitrace_reference_{safe_kernel_name}.csv"
            ip_list_path = arguments.output_dir / f"unitrace_ips_{safe_kernel_name}.txt"
            with reference_path.open("w", encoding="utf-8", newline="") as stream:
                writer = csv.writer(stream)
                writer.writerow(["Kernel Offset", "Source File", "Source Line"])
                writer.writerows((hex(offset), source_file, source_line) for offset, source_file, source_line in references)
            with ip_list_path.open("w", encoding="utf-8") as stream:
                for offset, _, _ in references:
                    stream.write(f"{offset:#x}\n")
            manifest_kernels.append(
                {
                    "name": kernel_name,
                    "reference_csv": str(reference_path.resolve()),
                    "ip_list": str(ip_list_path.resolve()),
                    "addressed_shader_dump": str(addressed_shader_dump.resolve()),
                    "section_file_offset": "0x0",
                }
            )
            print(f"extract_unitrace_stalls: wrote {len(references)} samples for {kernel_name}")

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