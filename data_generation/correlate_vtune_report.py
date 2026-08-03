#!/usr/bin/env python3

import argparse
import bisect
import csv
import json
import os
import re
import subprocess
import sys
from pathlib import Path


LINE_ROW = re.compile(r"^(.+?)\s+(\d+|-)\s+((?:0x)?[0-9a-fA-F]+)(?:\s+|$)")
COMP_DIR = re.compile(r"DW_AT_comp_dir\s+:\s+(?:\([^)]*\)\s+)?(.+)$")
CU_BOUNDARY = re.compile(r"^\s*Compilation Unit @ offset")
SECTION_ROW = re.compile(
    r"^\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+([0-9a-fA-F]+)\s+"
    r"([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+\S+\s+([^\s]+)"
)
SECTION_ROW_WITH_IDX = re.compile(
    r"^\s*\[\s*(\d+)\]\s+(\S+)\s+\S+\s+([0-9a-fA-F]+)\s+"
    r"([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+\S+\s+([^\s]+)"
)


def run_readelf(readelf: str, *arguments: str, allow_nonzero_output: bool = False) -> str:
    result = subprocess.run(
        [readelf, *arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if result.returncode != 0 and not (allow_nonzero_output and result.stdout):
        raise subprocess.CalledProcessError(
            result.returncode,
            result.args,
            output=result.stdout,
            stderr=result.stderr,
        )
    return result.stdout


def find_zebin(result_dir: Path, explicit_zebin: Path | None) -> Path:
    if explicit_zebin is not None:
        return explicit_zebin

    zebins = sorted(path for path in result_dir.rglob("*.zebin") if path.is_file())
    data_zebins = [path for path in zebins if "data.0" in path.parts]
    if len(data_zebins) == 1:
        return data_zebins[0]
    if len(zebins) != 1:
        raise ValueError(
            f"expected exactly one .zebin in {result_dir}, found {len(zebins)}; "
            "pass --zebin to select one"
        )
    return zebins[0]


def kernel_text_sections(readelf: str, zebin: Path) -> list[tuple[str, int, int]]:
    """Return (kernel_name, address, elf_section_index) for every executable
    .text.<name> section, excluding Intel_Symbol_Table, in section-table order."""
    output = run_readelf(readelf, "-SW", str(zebin))
    result = []
    for line in output.splitlines():
        m = SECTION_ROW_WITH_IDX.match(line)
        if m is None:
            continue
        idx, name, address, _offset, flags = m.groups()
        if name.startswith(".text.") and "X" in flags and "Intel_Symbol_Table" not in name:
            # text_address = -(file offset) so that:
            #   dwarf_addr = vtune_addr + text_address = vtune_addr - file_offset
            # which converts VTune's file-relative addresses to section-relative
            # DWARF addresses.
            result.append((name[len(".text."):], -int(_offset, 16), int(idx)))
    return result


def compilation_directories(readelf: str, zebin: Path) -> list[Path | None]:
    """Return DW_AT_comp_dir for each CU in the zebin, in debug_info order."""
    output = run_readelf(readelf, "--debug-dump=info", "--wide", str(zebin),
                         allow_nonzero_output=True)
    result: list[Path | None] = []
    current: str | None = None
    in_cu = False
    for line in output.splitlines():
        if CU_BOUNDARY.match(line):
            if in_cu:
                result.append(Path(current) if current else None)
            in_cu = True
            current = None
        elif in_cu and current is None:
            m = COMP_DIR.search(line)
            if m:
                current = m.group(1).strip()
    if in_cu:
        result.append(Path(current) if current else None)
    return result


def split_by_cu(readelf: str, zebin: Path) -> list[list[str]]:
    """Split readelf decodedline output into one list-of-lines per CU, in order."""
    output = run_readelf(readelf, "--debug-dump=decodedline", "--wide", str(zebin))
    blocks: list[list[str]] = []
    current: list[str] = []
    for line in output.splitlines():
        if re.match(r"^CU:\s", line):
            if current:
                blocks.append(current)
            current = [line]
        elif current:
            current.append(line)
    if current:
        blocks.append(current)
    return blocks


def compilation_directory(readelf: str, zebin: Path) -> Path:
    output = run_readelf(
        readelf,
        "--debug-dump=info",
        "--wide",
        str(zebin),
        allow_nonzero_output=True,
    )
    directories = {
        match.group(1).strip()
        for line in output.splitlines()
        if (match := COMP_DIR.search(line)) is not None
    }
    if len(directories) != 1:
        raise ValueError(
            f"expected exactly one DWARF compilation directory in {zebin}, "
            f"found {len(directories)}"
        )
    return Path(directories.pop())


def decoded_lines(
    readelf: str,
    zebin: Path,
    comp_dir: Path,
) -> tuple[list[int], list[tuple[str, int] | None]]:
    output = run_readelf(readelf, "--debug-dump=decodedline", "--wide", str(zebin))
    return decoded_lines_from_block(output.splitlines(), comp_dir, str(zebin))


def decoded_lines_from_block(
    lines: list[str],
    comp_dir: Path,
    label: str = "",
) -> tuple[list[int], list[tuple[str, int] | None]]:
    """Parse decoded line table from a pre-split block of readelf decodedline output."""
    rows = []
    current_file: Path | None = None
    for line in lines:
        stripped = line.strip()
        heading = None
        if stripped.startswith("CU: ") and stripped.endswith(":"):
            heading = stripped[4:-1]
        elif (stripped.startswith("/") or stripped.startswith("./") or stripped.startswith("../")):
            if stripped.endswith(":[++]"):
                heading = stripped[:-5]
            elif stripped.endswith(":"):
                heading = stripped[:-1]

        if heading is not None:
            path = Path(heading)
            if not path.is_absolute():
                path = comp_dir / path
            current_file = Path(os.path.normpath(path))
            continue

        match = LINE_ROW.match(line)
        if match is None:
            continue
        source_file, source_line, address = match.groups()
        location = None
        if source_line != "-":
            path = current_file
            if path is None or path.name != source_file.strip():
                path = Path(os.path.normpath(comp_dir / source_file.strip()))
            location = (str(path), int(source_line))
        rows.append((int(address, 16), location))

    if not rows:
        raise ValueError(f"no decoded DWARF line rows found{' in ' + label if label else ''}")

    rows.sort(key=lambda row: row[0])
    return [row[0] for row in rows], [row[1] for row in rows]


def correlate(
    input_csv: Path,
    output_csv: Path,
    text_address: int,
    line_addresses: list[int],
    line_locations: list[tuple[str, int] | None],
) -> tuple[int, int]:
    mapped = 0
    instruction_count = 0
    with (
        input_csv.open(encoding="utf-8", errors="replace", newline="") as input_stream,
        output_csv.open("w", encoding="utf-8", newline="") as output_stream,
    ):
        reader = csv.DictReader(input_stream)
        if reader.fieldnames is None or "Address" not in reader.fieldnames:
            raise ValueError(f"VTune report has no Address column: {input_csv}")

        remaining_fields = [
            field for field in reader.fieldnames if field not in ("Address", "Source File", "Source Line")
        ]
        fieldnames = ["Address", "Source File", "Source Line", *remaining_fields]
        writer = csv.DictWriter(output_stream, fieldnames=fieldnames)
        writer.writeheader()

        for row in reader:
            source_file = ""
            source_line = ""
            assembly = row.get("Assembly", "")
            if assembly and not assembly.startswith("Block ") and assembly != "illegal":
                instruction_count += 1
                dwarf_address = int(row["Address"], 16) + text_address
                index = bisect.bisect_right(line_addresses, dwarf_address) - 1
                location = line_locations[index] if index >= 0 else None
                if location is not None:
                    source_file, line = location
                    source_line = str(line)
                    mapped += 1

            row["Source File"] = source_file
            row["Source Line"] = source_line
            writer.writerow({field: row.get(field, "") for field in fieldnames})

    return mapped, instruction_count


def kernel_input_csv(input_csv: Path, kernel_name: str) -> Path:
    safe_name = re.sub(r"[^a-zA-Z0-9_-]", "_", kernel_name)[:80]
    candidate = input_csv.with_name(
        f"{input_csv.stem}_{safe_name}{input_csv.suffix}"
    )
    return candidate if candidate.is_file() else input_csv


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--result-dir", type=Path, required=True)
    parser.add_argument("--manifest-output", type=Path,
                        help="Write a per-kernel manifest JSON (always written for "
                             "multi-section zebins; optional for single-section).")
    parser.add_argument("--zebin", type=Path)
    parser.add_argument("--readelf", default="readelf")
    arguments = parser.parse_args()

    try:
        zebin = find_zebin(arguments.result_dir, arguments.zebin)
        sections = kernel_text_sections(arguments.readelf, zebin)
        if not sections:
            raise ValueError(f"no kernel text sections found in {zebin}")

        if len(sections) == 1:
            # ── Single-section: original behaviour ─────────────────────────
            kernel_name, text_address, _elf_idx = sections[0]
            comp_dir = compilation_directory(arguments.readelf, zebin)
            line_addresses, line_locations = decoded_lines(
                arguments.readelf, zebin, comp_dir
            )
            mapped, instruction_count = correlate(
                kernel_input_csv(arguments.input, kernel_name), arguments.output,
                text_address, line_addresses, line_locations,
            )
            print(
                f"correlate_vtune_report: mapped {mapped}/{instruction_count} "
                f"instructions using {zebin}"
            )
            if arguments.manifest_output is not None:
                _file_offset = -text_address  # text_address = -file_offset
                _write_manifest(
                    arguments.manifest_output, zebin,
                    [(kernel_name, arguments.output, _file_offset)],
                )
        else:
            # ── Multi-section: one output CSV per kernel ────────────────────
            cu_blocks = split_by_cu(arguments.readelf, zebin)
            cu_dirs = compilation_directories(arguments.readelf, zebin)

            out_dir = arguments.output.parent
            out_stem = arguments.output.stem
            out_suffix = arguments.output.suffix

            produced: list[tuple[str, Path, int]] = []

            for i, (kernel_name, text_address, elf_idx) in enumerate(sections):
                safe = re.sub(r"[^a-zA-Z0-9_-]", "_", kernel_name)[:80]
                kernel_csv = out_dir / f"{out_stem}_{safe}{out_suffix}"

                cu_block = cu_blocks[i] if i < len(cu_blocks) else None
                comp_dir = cu_dirs[i] if i < len(cu_dirs) else None

                if cu_block is None or comp_dir is None:
                    print(
                        f"correlate_vtune_report: [{kernel_name}] skipped: "
                        f"no CU data (cu_block={cu_block is not None}, "
                        f"comp_dir={comp_dir})",
                        file=sys.stderr,
                    )
                    continue

                try:
                    line_addresses, line_locations = decoded_lines_from_block(
                        cu_block, comp_dir, kernel_name
                    )
                    mapped, instruction_count = correlate(
                        kernel_input_csv(arguments.input, kernel_name), kernel_csv,
                        text_address, line_addresses, line_locations,
                    )
                    file_offset = -text_address  # text_address = -file_offset
                    produced.append((kernel_name, kernel_csv, file_offset))
                    print(
                        f"correlate_vtune_report: [{kernel_name}] mapped "
                        f"{mapped}/{instruction_count} instructions using {zebin}"
                    )
                except (OSError, subprocess.CalledProcessError, ValueError) as err:
                    print(
                        f"correlate_vtune_report: [{kernel_name}] {err}",
                        file=sys.stderr,
                    )

            if not produced:
                print(
                    "correlate_vtune_report: no sections processed successfully",
                    file=sys.stderr,
                )
                return 1

            manifest_path = (
                arguments.manifest_output
                if arguments.manifest_output is not None
                else out_dir / "vtune_manifest.json"
            )
            _write_manifest(manifest_path, zebin, produced)

    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        print(f"correlate_vtune_report: {error}", file=sys.stderr)
        return 1

    return 0


def _write_manifest(
    path: Path,
    zebin: Path,
    kernels: list[tuple[str, Path, int]],
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    entries = [
        {
            "name": name,
            "reference_csv": str(ref_csv),
            "section_file_offset": hex(file_offset),
        }
        for name, ref_csv, file_offset in kernels
    ]
    with path.open("w") as stream:
        json.dump({"zebin": str(zebin), "kernels": entries}, stream, indent=2)
        stream.write("\n")
    print(f"correlate_vtune_report: manifest written to {path}")


if __name__ == "__main__":
    sys.exit(main())