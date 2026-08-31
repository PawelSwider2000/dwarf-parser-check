#!/usr/bin/env python3
"""Annotate Intel GPU assembly with UnitRACE-style DWARF source markers.

Usage:
  python3 annotate_zebin_asm.py ASSEMBLY ZEBIN --output ANNOTATED_ASSEMBLY

ASSEMBLY may be an ``ocloc disasm`` ``.text.<kernel>.asm`` file or its ``.ip``
variant. The matching zebin supplies the DWARF line program; its ``.debug_loc``
section is required as a consistency check because it carries the corresponding
variable-location metadata, not source lines.
"""

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path


IP_MARKER = re.compile(r"^/\* \[([0-9A-Fa-f]{8})\] \*/")


@dataclass(frozen=True)
class SourceLocation:
    file: str
    line: int


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("assembly", type=Path, help="ocloc .text.<kernel>.asm listing")
    parser.add_argument("zebin", type=Path, help="zebin that supplied the assembly")
    parser.add_argument("--output", type=Path, required=True, help="annotated assembly output path")
    parser.add_argument(
        "--section",
        help="ELF text section name (default: inferred from the assembly file name)",
    )
    parser.add_argument(
        "--no-source-text",
        action="store_true",
        help="write only File and Line markers without reading the source files",
    )
    return parser.parse_args()


def section_name(assembly: Path, explicit_section: str | None) -> str:
    if explicit_section:
        return explicit_section
    name = assembly.name.removesuffix(".ip")
    if not name.endswith(".asm") or not name.startswith(".text."):
        raise ValueError("cannot infer the text section; pass --section")
    return name.removesuffix(".asm")


def instruction_offsets(assembly: Path) -> tuple[list[int], int]:
    offsets: list[int] = []
    next_offset = 0
    with assembly.open(encoding="utf-8", errors="replace") as stream:
        for raw_line in stream:
            match = IP_MARKER.match(raw_line)
            if match is not None:
                next_offset = int(match.group(1), 16)
            line = raw_line[match.end():] if match is not None else raw_line
            if not line.strip() or line.rstrip().endswith(":"):
                continue
            offsets.append(next_offset)
            next_offset += 0x8 if "Compacted" in line else 0x10
    if not offsets:
        raise ValueError(f"no instructions found in {assembly}")
    return offsets, next_offset


def decode(value: bytes) -> str:
    return value.decode("utf-8", errors="replace")


def source_path(cu, program, file_index: int) -> str:
    file_entries = program["file_entry"]
    if file_index <= 0 or file_index > len(file_entries):
        return ""
    file_entry = file_entries[file_index - 1]
    path = Path(decode(file_entry.name))
    if path.is_absolute():
        return str(path)

    directories = program["include_directory"]
    directory = Path()
    if file_entry.dir_index:
        directory = Path(decode(directories[file_entry.dir_index - 1]))
    else:
        attribute = cu.get_top_DIE().attributes.get("DW_AT_comp_dir")
        if attribute is not None:
            directory = Path(decode(attribute.value))
    return str(directory / path)


def line_locations(zebin: Path, instruction_ips: list[int]) -> dict[int, SourceLocation]:
    try:
        from elftools.elf.elffile import ELFFile
    except ImportError as error:
        raise ValueError("pyelftools is required; install it with: python3 -m pip install pyelftools") from error

    with zebin.open("rb") as stream:
        elf = ELFFile(stream)
        for section in (".debug_line", ".debug_loc"):
            if elf.get_section_by_name(section) is None:
                raise ValueError(f"{zebin} does not contain {section}")
        dwarf = elf.get_dwarf_info(relocate_dwarf_sections=False)
        candidates: list[tuple[int, list[tuple[int, int, list[tuple[int, SourceLocation]]]]]] = []
        for cu in dwarf.iter_CUs():
            program = dwarf.line_program_for_CU(cu)
            sequences: list[tuple[int, int, list[tuple[int, SourceLocation]]]] = []
            entries: list[tuple[int, SourceLocation]] = []
            for entry in program.get_entries():
                state = entry.state
                if state is None:
                    continue
                if state.end_sequence:
                    if entries:
                        sequences.append((entries[0][0], state.address, entries))
                    entries = []
                    continue
                if not state.line:
                    continue
                path = source_path(cu, program, state.file)
                if path:
                    entries.append((state.address, SourceLocation(path, state.line)))
            if sequences:
                score = sum(
                    1
                    for ip in instruction_ips
                    if any(start <= ip < end for start, end, _ in sequences)
                )
                candidates.append((score, sequences))

    if not candidates:
        raise ValueError(f"no DWARF source locations found in {zebin}")
    _, sequences = max(candidates, key=lambda candidate: candidate[0])
    locations: dict[int, SourceLocation] = {}
    for instruction_ip in instruction_ips:
        for start, end, entries in sequences:
            if not start <= instruction_ip < end:
                continue
            current = next(
                (location for address, location in reversed(entries) if address <= instruction_ip),
                None,
            )
            if current is not None:
                locations[instruction_ip] = current
            break
    return locations


def source_text(location: SourceLocation, enabled: bool) -> str:
    if not enabled:
        return ""
    try:
        return Path(location.file).read_text(encoding="utf-8").splitlines()[location.line - 1].strip()
    except (IndexError, OSError, UnicodeError):
        return ""


def annotate(
    assembly: Path,
    output: Path,
    locations: dict[int, SourceLocation],
    include_source_text: bool,
) -> tuple[int, int]:
    next_offset = 0
    previous: SourceLocation | None = None
    instruction_count = 0
    marked_instruction_count = 0
    with assembly.open(encoding="utf-8", errors="replace") as source, output.open(
        "w", encoding="utf-8"
    ) as destination:
        for raw_line in source:
            match = IP_MARKER.match(raw_line)
            if match is not None:
                next_offset = int(match.group(1), 16)
            line = raw_line[match.end():] if match is not None else raw_line
            if line.strip() and not line.rstrip().endswith(":"):
                instruction_count += 1
                location = locations.get(next_offset)
                if location is not None:
                    marked_instruction_count += 1
                    if location != previous:
                        if previous is None or location.file != previous.file:
                            destination.write(f"// File: {location.file}\n")
                        text = source_text(location, include_source_text)
                        suffix = f":  {text}" if text else ""
                        destination.write(f"// Line {location.line}{suffix}\n")
                    previous = location
                next_offset += 0x8 if "Compacted" in line else 0x10
            destination.write(raw_line)
    return instruction_count, marked_instruction_count


def main() -> int:
    arguments = parse_arguments()
    try:
        assembly = arguments.assembly.resolve()
        zebin = arguments.zebin.resolve()
        output = arguments.output.resolve()
        if not assembly.is_file():
            raise ValueError(f"assembly file not found: {assembly}")
        if not zebin.is_file():
            raise ValueError(f"zebin file not found: {zebin}")
        section = section_name(assembly, arguments.section)
        offsets, end_offset = instruction_offsets(assembly)

        from elftools.elf.elffile import ELFFile

        with zebin.open("rb") as stream:
            elf_section = ELFFile(stream).get_section_by_name(section)
            if elf_section is None:
                raise ValueError(f"section {section} not found in {zebin}")
            if end_offset > elf_section.data_size:
                raise ValueError(
                    f"assembly ends at {end_offset:#x}, beyond {section} size {elf_section.data_size:#x}"
                )
        locations = line_locations(zebin, offsets)
        output.parent.mkdir(parents=True, exist_ok=True)
        instructions, marked = annotate(
            assembly, output, locations, include_source_text=not arguments.no_source_text
        )
        print(f"annotated {marked}/{instructions} instructions -> {output}")
        return 0
    except (OSError, ValueError) as error:
        print(f"annotate_zebin_asm: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())