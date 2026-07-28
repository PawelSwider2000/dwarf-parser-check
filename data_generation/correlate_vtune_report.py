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
SECTION_ROW = re.compile(
    r"^\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+([0-9a-fA-F]+)\s+"
    r"([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+\S+\s+([^\s]+)"
)
SYMBOL_ROW = re.compile(
    r"^\s*\d+:\s+([0-9a-fA-F]+)\s+(\d+)\s+FUNC\s+\S+\s+\S+\s+\S+\s+(.+)$"
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


def kernel_text_address(readelf: str, zebin: Path) -> int:
    sections = run_readelf(readelf, "-SW", str(zebin))
    candidates = []
    for line in sections.splitlines():
        match = SECTION_ROW.match(line)
        if match is None:
            continue
        name, address, offset, flags = match.groups()
        if name.startswith(".text.") and "X" in flags and "Intel_Symbol_Table" not in name:
            candidates.append(int(address, 16))

    if len(candidates) != 1:
        raise ValueError(
            f"expected exactly one kernel text section in {zebin}, found {len(candidates)}"
        )
    return candidates[0]


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
    rows = []
    current_file: Path | None = None
    for line in output.splitlines():
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
        raise ValueError(f"no decoded DWARF line rows found in {zebin}")

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


def function_ranges(readelf: str, zebin: Path) -> list[tuple[int, int]]:
    output = run_readelf(readelf, "-sW", str(zebin))
    ranges = []
    for line in output.splitlines():
        match = SYMBOL_ROW.match(line)
        if match is None:
            continue
        address, size, _name = match.groups()
        begin = int(address, 16)
        end = begin + int(size)
        if end > begin:
            ranges.append((begin, end))

    if not ranges:
        raise ValueError(f"no function symbols found in {zebin}")
    return sorted(ranges)


def write_user_source_locations(
    report_csv: Path,
    output_json: Path,
    user_source_root: Path,
    text_address: int,
    functions: list[tuple[int, int]],
) -> tuple[int, int]:
    with report_csv.open(encoding="utf-8", newline="") as stream:
        rows = list(csv.DictReader(stream))

    function_starts = [begin for begin, _end in functions]

    def dwarf_address(row: dict[str, str]) -> int:
        return int(row["Address"], 16) + text_address

    def containing_function(address: int) -> int | None:
        index = bisect.bisect_right(function_starts, address) - 1
        if index >= 0 and address < functions[index][1]:
            return index
        return None

    normalized_root = os.path.normpath(user_source_root)

    def is_user_location(location: tuple[str, int]) -> bool:
        try:
            return os.path.commonpath((normalized_root, location[0])) == normalized_root
        except ValueError:
            return False

    own_user_locations: dict[int, set[tuple[str, int]]] = {}
    blocks: dict[int, int] = {}
    for row in rows:
        block = re.match(r"Block (\d+):", row.get("Assembly", ""))
        if block is not None:
            blocks[int(block.group(1))] = dwarf_address(row)

        if not row.get("Source File") or not row.get("Source Line"):
            continue
        function = containing_function(dwarf_address(row))
        location = (row["Source File"], int(row["Source Line"]))
        if function is not None and is_user_location(location):
            own_user_locations.setdefault(function, set()).add(location)

    callers: dict[int, list[tuple[int, tuple[str, int]]]] = {}
    for row in rows:
        call = re.search(r"\bcall\b.*\bbb_(\d+)\b", row.get("Assembly", ""))
        if call is None or int(call.group(1)) not in blocks:
            continue
        caller = containing_function(dwarf_address(row))
        callee = containing_function(blocks[int(call.group(1))])
        if caller is None or callee is None or not row.get("Source Line"):
            continue
        location = (row["Source File"], int(row["Source Line"]))
        callers.setdefault(callee, []).append((caller, location))

    cache: dict[int, set[tuple[str, int]]] = {}

    def highest_user_locations(function: int, visiting: frozenset[int]) -> set[tuple[str, int]]:
        if function in cache:
            return cache[function]
        if function in visiting:
            return set()

        edges = callers.get(function, [])
        if not edges:
            result = set(own_user_locations.get(function, set()))
        else:
            result = set()
            for caller, call_location in edges:
                if is_user_location(call_location):
                    result.add(call_location)
                else:
                    result.update(highest_user_locations(caller, visiting | {function}))
        cache[function] = result
        return result

    locations_by_ip: dict[str, list[list[str | int]]] = {}
    resolved = 0
    for row in rows:
        assembly = row.get("Assembly", "")
        if not assembly or assembly.startswith("Block "):
            continue

        locations: set[tuple[str, int]] = set()
        if row.get("Source File") and row.get("Source Line"):
            direct_location = (row["Source File"], int(row["Source Line"]))
            if is_user_location(direct_location):
                locations.add(direct_location)
            else:
                function = containing_function(dwarf_address(row))
                if function is not None:
                    locations.update(highest_user_locations(function, frozenset()))

        ordered_locations = sorted(locations)
        if ordered_locations:
            resolved += 1
        locations_by_ip[row["Address"]] = [[file, line] for file, line in ordered_locations]

    output_json.parent.mkdir(parents=True, exist_ok=True)
    with output_json.open("w") as stream:
        json.dump(locations_by_ip, stream, indent=2)
        stream.write("\n")
    return resolved, len(locations_by_ip)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--result-dir", type=Path, required=True)
    parser.add_argument("--source-locations-output", type=Path, required=True)
    parser.add_argument("--user-source-root", type=Path)
    parser.add_argument("--zebin", type=Path)
    parser.add_argument("--readelf", default="readelf")
    arguments = parser.parse_args()

    try:
        zebin = find_zebin(arguments.result_dir, arguments.zebin)
        text_address = kernel_text_address(arguments.readelf, zebin)
        comp_dir = compilation_directory(arguments.readelf, zebin)
        line_addresses, line_locations = decoded_lines(arguments.readelf, zebin, comp_dir)
        mapped, instruction_count = correlate(
            arguments.input,
            arguments.output,
            text_address,
            line_addresses,
            line_locations,
        )
        functions = function_ranges(arguments.readelf, zebin)
        user_source_root = arguments.user_source_root or comp_dir
        user_resolved, user_ip_count = write_user_source_locations(
            arguments.output,
            arguments.source_locations_output,
            user_source_root,
            text_address,
            functions,
        )
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        print(f"correlate_vtune_report: {error}", file=sys.stderr)
        return 1

    print(
        f"correlate_vtune_report: mapped {mapped}/{instruction_count} instructions "
        f"using {zebin}"
    )
    print(
        f"correlate_vtune_report: mapped {user_resolved}/{user_ip_count} IPs "
        f"to highest user locations in {arguments.source_locations_output}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())