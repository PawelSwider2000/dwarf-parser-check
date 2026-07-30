#!/usr/bin/env python3
"""Extract Address → Source Line from VTune's SQLite DB and the DWARF line program.

Outputs two CSVs for comparison:
  - vtune_addr_srcline.csv   (from VTune DB, one row per unique address)
  - dwarf_addr_srcline.csv   (from raw .debug_line via pyelftools)

Usage:
  python3 extract_addr_srcline.py [results_dir]
  results_dir defaults to the directory containing this script.
"""

import argparse
import csv
import json
import os
import re
import subprocess
import sys
import struct
import sqlite3
from pathlib import Path


def _parse_args() -> Path:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("results_dir", nargs="?", default=None,
                   help="Path to the results directory containing result.csv and vtune_results/")
    args = p.parse_args()
    return Path(args.results_dir) if args.results_dir else Path(__file__).parent


RESULTS_DIR = _parse_args()
VTUNE_DB = RESULTS_DIR / "vtune_results/sqlite-db/dicer.db"


def find_zebin(results_dir: Path, explicit: Path | None = None) -> Path:
    """Find the .zebin file under results_dir/vtune_results/archive/binaries/."""
    if explicit is not None:
        return explicit
    search_root = results_dir / "vtune_results" / "archive" / "binaries"
    zebins = sorted(p for p in search_root.rglob("*.zebin") if p.is_file())
    data_zebins = [p for p in zebins if "data.0" in p.parts]
    if len(data_zebins) == 1:
        return data_zebins[0]
    if len(zebins) != 1:
        raise ValueError(
            f"expected exactly one .zebin under {search_root}, found {len(zebins)}"
        )
    return zebins[0]


def run_ocloc_disasm(zebin: Path, ocloc: str = "ocloc") -> Path:
    """Run `ocloc disasm` on zebin and return the dump directory.

    Skips re-running if .debug_line already exists in the dump dir.
    Uses -skip-asm-translation since we only need the debug sections.
    """
    dump_dir = zebin.parent / "dump"
    if not (dump_dir / ".debug_line").exists():
        subprocess.run(
            [ocloc, "disasm", "-file", str(zebin), "-dump", str(dump_dir),
             "-skip-asm-translation"],
            check=True,
            capture_output=True,
        )
    return dump_dir


# ---------------------------------------------------------------------------
# 1. VTune DB approach
# ---------------------------------------------------------------------------
def extract_vtune(addr_filter: set[int] | None = None) -> tuple[list[tuple[int, int, str]], dict[int, int]]:
    """Return (sorted [(display_address, line, filepath)], {display_address: rva}) from VTune DB.

    When multiple source lines exist for one address (inlining), picks the
    innermost non-zero line (smallest src_loc rowid = leaf of inline chain).
    """
    conn = sqlite3.connect(VTUNE_DB)
    rows = conn.execute("""
        SELECT cl.display_address, cl.rva, sl.line, sf.path
        FROM dd_code_location cl
        LEFT JOIN dd_source_location sl ON cl.src_loc = sl.rowid
        LEFT JOIN dd_source_file sf ON sl.src_file = sf.rowid
        ORDER BY cl.display_address, cl.rowid
    """).fetchall()
    conn.close()

    seen: dict[int, int] = {}
    seen_file: dict[int, str] = {}
    rva_map: dict[int, int] = {}  # display_address → rva
    for addr, rva, line, fpath in rows:
        if addr is None:
            continue
        if addr_filter and addr not in addr_filter:
            continue
        line = line or 0
        if addr not in seen or (line and not seen[addr]):
            seen[addr] = line
            seen_file[addr] = os.path.normpath(fpath) if fpath else ""
        if addr not in rva_map and rva is not None:
            rva_map[addr] = rva

    return [(a, l, seen_file.get(a, "")) for a, l in sorted(seen.items(), key=lambda x: x[0])], rva_map


# ---------------------------------------------------------------------------
# 2. DWARF .debug_line approach (pyelftools)
# ---------------------------------------------------------------------------
def extract_dwarf(
    rva_map: dict[int, int],
    zebin: Path,
    debug_line: Path,
    extra_display_addrs: set[int] | None = None,
) -> list[tuple[int, int]]:
    """Return sorted [(display_address, line)] by parsing DWARF via pyelftools.

    Iterates all .debug_line CUs directly (not via .debug_info DW_AT_stmt_list,
    which is broken for Intel GPU zebin — all CUs point to offset 0). Picks the
    CU whose rva entries best overlap the VTune rva_map keys.
    Also resolves extra_display_addrs absent from VTune DB using the computed
    kernel base offset (display_address - rva = constant).
    """
    try:
        from elftools.elf.elffile import ELFFile
    except ImportError:
        print("pyelftools not installed — skipping DWARF extraction", file=sys.stderr)
        return []

    rva_keys = set(rva_map.values())
    # Infer kernel base offset from any known entry; use it for extra addresses
    kernel_base = next(iter(disp - rva for disp, rva in rva_map.items()), 0) if rva_map else 0
    extra_rvas: dict[int, int] = {}  # rva → display_address for extras
    for disp in (extra_display_addrs or set()):
        rva = disp - kernel_base
        if rva >= 0:
            extra_rvas[rva] = disp
    all_rva_keys = rva_keys | set(extra_rvas)

    with open(zebin, "rb") as f:
        elf = ELFFile(f)
        # relocations are no-ops for Intel GPU zebin (all zero-valued)
        dwarf = elf.get_dwarf_info(relocate_dwarf_sections=False)
        cu0_structs = next(dwarf.iter_CUs()).structs

        # Find .debug_line CU offsets by scanning unit_length headers
        # Read from ocloc disasm dump (avoids pyelftools stream issues)
        debug_line_data = debug_line.read_bytes()
        cu_offsets: list[int] = []
        pos = 0
        while pos < len(debug_line_data):
            unit_len = struct.unpack_from("<I", debug_line_data, pos)[0]
            if unit_len == 0:
                break
            cu_offsets.append(pos)
            pos += 4 + unit_len

        # Pick the CU whose rva entries have the most overlap with VTune's rva values
        best_entries: list[tuple[int, int]] = []
        best_score = -1
        for offset in cu_offsets:
            lp = dwarf._parse_line_program_at_offset(offset, cu0_structs)
            entries = [(e.state.address, e.state.line)
                       for e in lp.get_entries()
                       if e.state and not e.state.end_sequence]
            score = sum(1 for a, _ in entries if a in rva_keys)
            if score > best_score:
                best_score = score
                best_entries = entries

    # For each rva, propagate the last known DWARF line (handles gaps between entries)
    best_entries.sort()
    rva_to_display = {v: k for k, v in rva_map.items()}
    rva_to_display.update(extra_rvas)
    results: dict[int, int] = {}
    last_line = 0
    ei = 0
    for rva in sorted(all_rva_keys):
        while ei < len(best_entries) and best_entries[ei][0] <= rva:
            last_line = best_entries[ei][1] or last_line
            ei += 1
        disp = rva_to_display.get(rva)
        if disp is not None and last_line:
            results[disp] = last_line

    return sorted(results.items(), key=lambda x: x[0])


# ---------------------------------------------------------------------------
# 3. VTune reference CSV (per kernel, same format as correlate_vtune_report output)
# ---------------------------------------------------------------------------
def generate_vtune_reference(ref_csv: Path, out_dir: Path) -> list[dict]:
    """Write one vtune_reference_<kernel>.csv per kernel found in the VTune DB.

    Columns: Address, Source File, Source Line, Assembly, <any extra cols from ref_csv>.
    Rows come from the DB (nested_level=None = raw DWARF entry); assembly and extra
    counter columns are filled from ref_csv where available.
    """
    conn = sqlite3.connect(VTUNE_DB)
    # Fetch raw DWARF entries only (nested_level IS NULL) to stay true to .debug_line
    db_rows = conn.execute("""
        SELECT cl.display_address, cl.rva, sl.line, sf.path, ms.gpu_kernel_name
        FROM dd_code_location cl
        LEFT JOIN dd_source_location sl  ON cl.src_loc  = sl.rowid
        LEFT JOIN dd_source_file     sf  ON sl.src_file = sf.rowid
        LEFT JOIN dd_module_segment  ms  ON cl.mod_seg  = ms.rowid
        WHERE cl.nested_level IS NULL
        ORDER BY ms.gpu_kernel_name, cl.display_address
    """).fetchall()
    conn.close()

    # Group by kernel name; store (display_address, rva, line, path)
    by_kernel: dict[str, list[tuple[int, int, int, str]]] = {}
    for addr, rva, line, path, kernel in db_rows:
        if addr is None:
            continue
        key = kernel or "unknown"
        norm_path = os.path.normpath(path) if path else ""
        by_kernel.setdefault(key, []).append((addr, rva or 0, line or 0, norm_path))

    # Load extra columns (assembly, stall counts) from ref_csv keyed by address
    extra_cols: list[str] = []
    extra_data: dict[int, dict[str, str]] = {}
    if ref_csv.exists():
        with open(ref_csv, newline="") as f:
            reader = csv.DictReader(f)
            if reader.fieldnames:
                extra_cols = [c for c in reader.fieldnames
                              if c not in ("Address", "Source File", "Source Line")]
            for row in reader:
                try:
                    extra_data[int(row["Address"], 16)] = {c: row.get(c, "") for c in extra_cols}
                except (ValueError, KeyError):
                    pass

    out_dir.mkdir(parents=True, exist_ok=True)
    manifest_kernels: list[dict] = []
    for kernel, entries in sorted(by_kernel.items()):
        safe = re.sub(r"[^a-zA-Z0-9_-]", "_", kernel)
        out_path = out_dir / f"vtune_reference_{safe}.csv"
        # section_file_offset = kernel base = display_address - rva (constant per kernel)
        section_file_offset = entries[0][0] - entries[0][1] if entries else 0
        with open(out_path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["Kernel Offset", "Source File", "Source Line"] + extra_cols)
            for addr, rva, line, path in entries:
                extra = extra_data.get(addr, {c: "" for c in extra_cols})
                w.writerow([hex(rva), path, line if line else ""] +
                           [extra.get(c, "") for c in extra_cols])
        print(f"Written {len(entries)} rows → {out_path}")
        manifest_kernels.append({
            "name": kernel,
            "reference_csv": str(out_path.resolve()),
            "section_file_offset": hex(section_file_offset),
        })
    return manifest_kernels


def write_manifest(path: Path, zebin: Path, kernels: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w") as f:
        json.dump({"zebin": str(zebin.resolve()), "kernels": kernels}, f, indent=2)
        f.write("\n")
    print(f"Written manifest → {path}")


# ---------------------------------------------------------------------------
# 4. Write CSVs
# ---------------------------------------------------------------------------
def load_assembly(ref_csv: Path) -> dict[int, str]:
    """Return {display_address: assembly_text} from result.csv."""
    asm_map: dict[int, str] = {}
    if not ref_csv.exists():
        return asm_map
    with open(ref_csv) as f:
        for row in csv.DictReader(f):
            try:
                addr = int(row["Address"], 16)
                asm = row.get("Assembly", "").strip()
                # Skip block-header pseudo-rows (no real instruction)
                if asm and not asm.startswith("Block "):
                    asm_map[addr] = asm
            except (ValueError, KeyError):
                pass
    return asm_map


def write_csv(path: Path, rows: list, asm_map: dict[int, str] | None = None) -> None:
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        has_file = rows and len(rows[0]) == 3
        header = ["Address", "Source Line", "Source File"] if has_file else ["Address", "Source Line"]
        if asm_map is not None:
            header.append("Assembly")
        w.writerow(header)
        for row in rows:
            out = [hex(row[0])] + list(row[1:])
            if asm_map is not None:
                out.append(asm_map.get(row[0], ""))
            w.writerow(out)
    print(f"Written {len(rows)} rows → {path}")


def main() -> None:
    # Collect addresses from result.csv for optional filtering
    ref_addrs: set[int] = set()
    ref_csv = RESULTS_DIR / "result.csv"
    if ref_csv.exists():
        with open(ref_csv) as f:
            for row in csv.DictReader(f):
                try:
                    ref_addrs.add(int(row["Address"], 16))
                except (ValueError, KeyError):
                    pass

    # VTune reference CSVs + manifest (one CSV per kernel, DB-sourced)
    zebin = find_zebin(RESULTS_DIR)
    manifest_kernels = generate_vtune_reference(ref_csv, RESULTS_DIR)
    write_manifest(RESULTS_DIR / "vtune_manifest.json", zebin, manifest_kernels)

    asm_map = load_assembly(ref_csv)

    # VTune DB
    vtune_rows, rva_map = extract_vtune()
    write_csv(RESULTS_DIR / "vtune_addr_srcline.csv", vtune_rows, asm_map)

    # Addresses in result.csv but absent from VTune DB (e.g. never-sampled jumps)
    vtune_addrs = {r[0] for r in vtune_rows}
    missing_addrs = ref_addrs - vtune_addrs

    # DWARF — run ocloc disasm to get fresh debug sections, then parse .debug_line
    dump_dir = run_ocloc_disasm(zebin)
    dwarf_rows = extract_dwarf(rva_map, zebin, dump_dir / ".debug_line",
                               extra_display_addrs=missing_addrs)
    dwarf_map = dict(dwarf_rows)
    write_csv(RESULTS_DIR / "dwarf_addr_srcline.csv", dwarf_rows)

    # Quick diff against result.csv addresses
    if ref_addrs and dwarf_rows:
        vtune_map = {r[0]: r[1] for r in vtune_rows}
        print(f"\n{'Address':<12} {'result.csv':>12} {'VTune DB':>10} {'DWARF':>8}")
        print("-" * 46)
        ref_map: dict[int, int] = {}
        with open(ref_csv) as f:
            for row in csv.DictReader(f):
                try:
                    addr = int(row["Address"], 16)
                    line = int(row["Source Line"])
                    if line:
                        ref_map[addr] = line
                except (ValueError, KeyError):
                    pass
        for addr in sorted(ref_map):
            ref_line = ref_map[addr]
            v_line = vtune_map.get(addr, "?")
            d_line = dwarf_map.get(addr, "?")
            match = "✓" if (v_line == ref_line or d_line == ref_line) else "✗"
            print(f"{hex(addr):<12} {ref_line:>12} {str(v_line):>10} {str(d_line):>8}  {match}")


if __name__ == "__main__":
    main()
