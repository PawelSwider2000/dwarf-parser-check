#!/usr/bin/env python3
"""Compare unitrace IGC shader dumps with the zebin captured by VTune.

The IGC ``.asm.ip`` files carry source-marker annotations but not machine-code
bytes. This helper pairs each annotated dump with its sibling IGC kernel ELF,
then compares its ``.text.<kernel>`` section to the same section in the VTune
zebin. It also preserves an ``ocloc disasm`` dump of the final zebin.

Usage:
  python3 compare_unitrace_asm_vtune_zebin.py RESULTS_DIR
  python3 compare_unitrace_asm_vtune_zebin.py RESULTS_DIR --zebin PATH
"""

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path


KERNEL_MARKER = re.compile(r"^//\.kernel\s+(\S+)\s*$")
IP_MARKER = re.compile(r"^/\* \[([0-9A-Fa-f]{8})\] \*/\s+(.*)$")
SECTION_LINE = re.compile(
    r"^\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+\S+\s+([0-9A-Fa-f]+)\s+([0-9A-Fa-f]+)"
)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results_dir", type=Path, help="Experiment result directory")
    parser.add_argument("--zebin", type=Path, help="VTune zebin to compare")
    parser.add_argument("--ocloc", default="ocloc", help="ocloc executable (default: ocloc)")
    parser.add_argument(
        "--output-dir",
        type=Path,
        help="Directory for report and final zebin disassembly (default: RESULTS_DIR/unitrace_zebin_comparison)",
    )
    return parser.parse_args()


def read_kernel_name(assembly: Path) -> str:
    with assembly.open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            match = KERNEL_MARKER.match(line)
            if match is not None:
                return match.group(1)
    raise ValueError(f"no //.kernel marker found in {assembly}")


def annotation_coverage(assembly: Path) -> dict[str, int]:
    instruction_count = 0
    maximum_end = 0
    with assembly.open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            match = IP_MARKER.match(line)
            if match is None:
                continue
            instruction_count += 1
            instruction_pointer = int(match.group(1), 16)
            instruction_size = 0x8 if "Compacted" in match.group(2) else 0x10
            maximum_end = max(maximum_end, instruction_pointer + instruction_size)
    if instruction_count == 0:
        raise ValueError(f"no IP-prefixed instructions found in {assembly}")
    return {"instruction_count": instruction_count, "end_offset": maximum_end}


def read_sections(path: Path) -> dict[str, tuple[int, int]]:
    result = subprocess.run(
        ["readelf", "-SW", str(path)],
        check=True,
        text=True,
        capture_output=True,
    )
    sections: dict[str, tuple[int, int]] = {}
    for line in result.stdout.splitlines():
        match = SECTION_LINE.match(line)
        if match is not None:
            sections[match.group(1)] = (int(match.group(2), 16), int(match.group(3), 16))
    return sections


def section_bytes(path: Path, section_name: str) -> bytes:
    section = read_sections(path).get(section_name)
    if section is None:
        raise ValueError(f"section {section_name} not found in {path}")
    offset, size = section
    with path.open("rb") as stream:
        stream.seek(offset)
        data = stream.read(size)
    if len(data) != size:
        raise ValueError(f"failed to read {size} bytes from {section_name} in {path}")
    return data


def find_vtune_zebin(results_dir: Path, explicit_zebin: Path | None) -> Path:
    if explicit_zebin is not None:
        return explicit_zebin
    candidates = sorted((results_dir / "vtune_results" / "data.0").glob("*.zebin"))
    if len(candidates) == 1:
        return candidates[0]
    candidates = sorted((results_dir / "vtune_results" / "archive" / "binaries").rglob("*.zebin"))
    if len(candidates) == 1:
        return candidates[0]
    raise ValueError(
        f"expected one VTune zebin under {results_dir / 'vtune_results'}; pass --zebin to choose one"
    )


def run_ocloc(ocloc: str, zebin: Path, output_dir: Path) -> Path:
    dump_dir = output_dir / "vtune_zebin_disassembly"
    if dump_dir.exists():
        shutil.rmtree(dump_dir)
    subprocess.run([ocloc, "disasm", "-file", str(zebin), "-dump", str(dump_dir)], check=True)
    return dump_dir


def first_difference(left: bytes, right: bytes) -> int | None:
    for index, (left_byte, right_byte) in enumerate(zip(left, right)):
        if left_byte != right_byte:
            return index
    return None


def compare_kernel(assembly_ip: Path, zebin: Path) -> dict[str, object]:
    kernel_name = read_kernel_name(assembly_ip)
    assembly = assembly_ip.with_suffix("")
    kernel_elf = assembly.with_suffix(".elf")
    if not kernel_elf.is_file():
        raise ValueError(f"IGC kernel ELF not found next to {assembly_ip}: {kernel_elf}")

    section_name = f".text.{kernel_name}"
    igc_code = section_bytes(kernel_elf, section_name)
    zebin_code = section_bytes(zebin, section_name)
    difference = first_difference(igc_code, zebin_code)
    common_prefix = difference if difference is not None else min(len(igc_code), len(zebin_code))
    if igc_code == zebin_code:
        status = "exact"
    elif difference is None:
        status = "matching-prefix"
    else:
        status = "mismatch"

    coverage = annotation_coverage(assembly_ip)
    return {
        "kernel": kernel_name,
        "status": status,
        "assembly_ip": str(assembly_ip.resolve()),
        "igc_kernel_elf": str(kernel_elf.resolve()),
        "section": section_name,
        "igc_text_size": len(igc_code),
        "vtune_zebin_text_size": len(zebin_code),
        "common_prefix_size": common_prefix,
        "first_difference_offset": difference,
        "igc_text_sha256": hashlib.sha256(igc_code).hexdigest(),
        "vtune_zebin_text_sha256": hashlib.sha256(zebin_code).hexdigest(),
        "annotated_instruction_count": coverage["instruction_count"],
        "annotation_end_offset": coverage["end_offset"],
        "annotation_covers_vtune_text": coverage["end_offset"] >= len(zebin_code),
    }


def main() -> int:
    arguments = parse_arguments()
    try:
        results_dir = arguments.results_dir.resolve()
        shader_dump_dir = results_dir / "unitrace_shader_dump"
        assembly_files = sorted(shader_dump_dir.glob("*.asm.ip"))
        if not assembly_files:
            raise ValueError(f"no unitrace .asm.ip files found in {shader_dump_dir}")
        zebin = find_vtune_zebin(results_dir, arguments.zebin)
        if not zebin.is_file():
            raise ValueError(f"VTune zebin not found: {zebin}")

        output_dir = arguments.output_dir or results_dir / "unitrace_zebin_comparison"
        output_dir.mkdir(parents=True, exist_ok=True)
        disassembly_dir = run_ocloc(arguments.ocloc, zebin, output_dir)
        report = {
            "vtune_zebin": str(zebin.resolve()),
            "vtune_zebin_sha256": hashlib.sha256(zebin.read_bytes()).hexdigest(),
            "ocloc_disassembly_dir": str(disassembly_dir.resolve()),
            "kernels": [compare_kernel(assembly, zebin) for assembly in assembly_files],
        }
        report_path = output_dir / "report.json"
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"unitrace-zebin comparison report: {report_path}")
        for kernel in report["kernels"]:
            first_difference_offset = kernel["first_difference_offset"]
            first_difference = (
                "none" if first_difference_offset is None else f"{first_difference_offset:#x}"
            )
            print(
                f"{kernel['kernel']}: {kernel['status']}; "
                f"IGC={kernel['igc_text_size']:#x}, VTune={kernel['vtune_zebin_text_size']:#x}, "
                f"common-prefix={kernel['common_prefix_size']:#x}, first-difference={first_difference}"
            )
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"compare_unitrace_asm_vtune_zebin: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())