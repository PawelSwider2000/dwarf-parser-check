#!/usr/bin/env python3
"""Extract Address → Source Line from VTune's SQLite DB and the DWARF line program.

Outputs two CSVs for comparison:
  - vtune_addr_srcline.csv   (from VTune DB, one row per unique address)
    - vtune_addr_srcline_<kernel>.csv (from VTune DB, grouped per GPU kernel)
  - dwarf_addr_srcline.csv   (from raw .debug_line via pyelftools)
    - vtune_ips_<kernel>.txt   (raw VTune display IPs, one per line)

Usage:
  python3 extract_addr_srcline.py [results_dir]
  results_dir defaults to the directory containing this script.
"""

import argparse
import csv
import json
import itertools
import os
import re
import subprocess
import sys
import struct
import sqlite3
import tempfile
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


def find_zebins(results_dir: Path) -> list[Path]:
    """Find the .zebin files under results_dir/vtune_results/archive/binaries/."""
    search_root = results_dir / "vtune_results" / "archive" / "binaries"
    zebins = sorted(p for p in search_root.rglob("*.zebin") if p.is_file())
    if not zebins:
        raise ValueError(f"no .zebin files found under {search_root}")
    return zebins


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
def extract_vtune_all() -> tuple[list[tuple[int, int | None, int, str]], dict[int, int]]:
    """Return (all rows [(addr, nested_level, line, path)], {addr: rva}) from VTune DB.

    Includes every resolution level per address (NULL = raw DWARF entry, 0+ = inline depth).
    Sorted by address ascending, NULL nested_level first within each address.
    """
    conn = sqlite3.connect(VTUNE_DB)
    rows = conn.execute("""
        SELECT cl.display_address, cl.rva, cl.nested_level, sl.line, sf.path
        FROM dd_code_location cl
        LEFT JOIN dd_source_location sl ON cl.src_loc = sl.rowid
        LEFT JOIN dd_source_file sf ON sl.src_file = sf.rowid
        ORDER BY cl.display_address,
                 cl.nested_level IS NOT NULL,
                 cl.nested_level
    """).fetchall()
    conn.close()

    results: list[tuple[int, int | None, int, str]] = []
    rva_map: dict[int, int] = {}
    for addr, rva, nested_level, line, fpath in rows:
        if addr is None:
            continue
        norm_path = os.path.normpath(fpath) if fpath else ""
        results.append((addr, nested_level, line or 0, norm_path))
        if addr not in rva_map and rva is not None:
            rva_map[addr] = rva

    return results, rva_map


def extract_vtune_by_kernel() -> dict[str, list[tuple[int, int | None, int, str]]]:
    """Return VTune location rows grouped by GPU kernel name."""
    conn = sqlite3.connect(VTUNE_DB)
    rows = conn.execute("""
        SELECT ms.gpu_kernel_name, cl.display_address, cl.nested_level, sl.line, sf.path
        FROM dd_code_location cl
        LEFT JOIN dd_module_segment ms ON cl.mod_seg = ms.rowid
        LEFT JOIN dd_source_location sl ON cl.src_loc = sl.rowid
        LEFT JOIN dd_source_file sf ON sl.src_file = sf.rowid
        ORDER BY ms.gpu_kernel_name,
                 cl.display_address,
                 cl.nested_level IS NOT NULL,
                 cl.nested_level
    """).fetchall()
    conn.close()

    by_kernel: dict[str, list[tuple[int, int | None, int, str]]] = {}
    for kernel, address, nested_level, line, source_file in rows:
        if address is None:
            continue
        by_kernel.setdefault(kernel or "unknown", []).append(
            (address, nested_level, line or 0,
             os.path.normpath(source_file) if source_file else "")
        )
    return by_kernel


def write_vtune_all_csv(
    path: Path,
    rows: list[tuple[int, int | None, int, str]],
    asm_map: dict[int, str],
) -> None:
    """Write vtune_addr_srcline.csv with one row per (address, nested_level) pair."""
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Address", "Nested Level", "Source File", "Source Line", "Assembly"])
        for addr, nested_level, line, fpath in rows:
            level = "" if nested_level is None else str(nested_level)
            w.writerow([hex(addr), level, fpath, line if line else "", asm_map.get(addr, "")])
    print(f"Written {len(rows)} rows → {path}")


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
        first_cu = next(dwarf.iter_CUs(), None)
        if first_cu is None:
            print("No DWARF compilation units found; skipping raw DWARF extraction",
                  file=sys.stderr)
            return []
        cu0_structs = first_cu.structs

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
def correlate_vtune_locations(
    ref_csv: Path, zebins: list[Path]
) -> dict[str, dict[int, tuple[int, str]]]:
    """Return DWARF-correlated {(kernel, display address): (line, file)} locations."""
    correlator = Path(__file__).with_name("correlate_vtune_report.py")
    with tempfile.TemporaryDirectory(prefix="dwarf-parser-check-correlation-") as temp_dir:
        temp_path = Path(temp_dir)
        locations: dict[str, dict[int, tuple[int, str]]] = {}
        for index, zebin in enumerate(zebins):
            manifest_path = temp_path / f"vtune_manifest_{index}.json"
            try:
                subprocess.run(
                    [
                        sys.executable,
                        str(correlator),
                        "--input", str(ref_csv),
                        "--output", str(temp_path / f"vtune_reference_{index}.csv"),
                        "--result-dir", str(RESULTS_DIR / "vtune_results"),
                        "--manifest-output", str(manifest_path),
                        "--zebin", str(zebin),
                    ],
                    check=True,
                )
            except subprocess.CalledProcessError as error:
                print(f"DWARF correlation failed for {zebin}: {error}", file=sys.stderr)
                continue

            manifest = json.loads(manifest_path.read_text())
            for kernel in manifest["kernels"]:
                kernel_locations: dict[int, tuple[int, str]] = {}
                with open(kernel["reference_csv"], newline="") as reference_stream:
                    for row in csv.DictReader(reference_stream):
                        try:
                            address = int(row["Address"], 16)
                            line = int(row["Source Line"])
                        except (KeyError, TypeError, ValueError):
                            continue
                        source_file = row.get("Source File", "")
                        if source_file:
                            kernel_locations[address] = (line, os.path.normpath(source_file))
                locations[kernel["name"]] = kernel_locations
    return locations


def report_instruction_addresses(path: Path) -> set[int]:
    """Return instruction addresses from a VTune CSV report.

    VTune emits a warning before the CSV header on some versions, so find the
    header in the same way as correlate_vtune_report.py. When the report
    provides an Assembly column, exclude block headers and illegal disassembly
    rows; address-only reports retain every parsable address.
    """
    with path.open(encoding="utf-8", errors="replace", newline="") as stream:
        header = next(
            (line for line in stream if "Address" in next(csv.reader([line]), [])),
            None,
        )
        reader = csv.DictReader(itertools.chain([header], stream)) if header else None
        if reader is None or reader.fieldnames is None or "Address" not in reader.fieldnames:
            raise ValueError(f"VTune report has no Address column: {path}")

        addresses = set()
        for row in reader:
            assembly = row.get("Assembly", "").strip()
            if "Assembly" in reader.fieldnames and (
                not assembly or assembly.startswith("Block ") or assembly == "illegal"
            ):
                continue
            try:
                addresses.add(int(row["Address"], 16))
            except (KeyError, TypeError, ValueError):
                continue
    return addresses


def kernel_report_path(input_csv: Path, kernel_name: str) -> Path:
    safe_kernel_name = re.sub(r"[^a-zA-Z0-9_-]", "_", kernel_name)
    candidate = input_csv.with_name(f"{input_csv.stem}_{safe_kernel_name}{input_csv.suffix}")
    return candidate if candidate.is_file() else input_csv


def generate_vtune_reference(
    ref_csv: Path,
    out_dir: Path,
    correlated_locations: dict[str, dict[int, tuple[int, str]]],
) -> list[dict]:
    """Write a correlateable reference CSV and raw IP list per VTune kernel.

    The IP list contains every distinct address from the per-kernel VTune result
    report. The reference CSV contains the subset with a DWARF source location;
    the database supplies only the display-address to RVA translation. Extra
    counter columns are copied from the report when available.
    """
    conn = sqlite3.connect(VTUNE_DB)
    # Fetch raw DWARF entries only (nested_level IS NULL) to stay true to .debug_line.
    # These entries establish the display-address to RVA translation, but they are
    # not a complete instruction inventory; the result report owns that inventory.
    db_rows = conn.execute("""
        SELECT cl.display_address, cl.rva, ms.gpu_kernel_name
        FROM dd_code_location cl
        LEFT JOIN dd_module_segment  ms  ON cl.mod_seg  = ms.rowid
        WHERE cl.nested_level IS NULL
        ORDER BY ms.gpu_kernel_name, cl.display_address
    """).fetchall()
    conn.close()

    # Group by kernel name and use the database only for its stable
    # display-address - RVA section offset.
    by_kernel: dict[str, dict[int, int]] = {}
    for addr, rva, kernel in db_rows:
        if addr is None or rva is None:
            continue
        key = kernel or "unknown"
        by_kernel.setdefault(key, {})[addr] = rva

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
    for kernel, rvas_by_display_address in sorted(by_kernel.items()):
        report_path = kernel_report_path(ref_csv, kernel)
        report_addresses = report_instruction_addresses(report_path) if report_path.is_file() else set()
        if report_addresses:
            section_file_offsets = {
                address - rva for address, rva in rvas_by_display_address.items()
            }
            if len(section_file_offsets) > 1:
                raise ValueError(
                    f"multiple section file offsets for kernel {kernel}: "
                    f"{', '.join(hex(offset) for offset in sorted(section_file_offsets))}"
                )
            section_file_offset = next(iter(section_file_offsets), 0)
        else:
            section_file_offset = 0
        locations = correlated_locations.get(kernel, {})
        correlateable_entries = [
            (address, address - section_file_offset, line, path)
            for address, (line, path) in sorted(locations.items())
            if address in report_addresses
        ]
        safe = re.sub(r"[^a-zA-Z0-9_-]", "_", kernel)
        out_path = out_dir / f"vtune_reference_{safe}.csv"
        ip_list_path = out_dir / f"vtune_ips_{safe}.txt"
        with open(out_path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["Kernel Offset", "Source File", "Source Line"] + extra_cols)
            for addr, rva, line, path in correlateable_entries:
                extra = extra_data.get(addr, {c: "" for c in extra_cols})
                w.writerow([hex(rva), path, line] +
                           [extra.get(c, "") for c in extra_cols])
        print(f"Written {len(correlateable_entries)} correlateable rows → {out_path}")
        with open(ip_list_path, "w") as f:
            for addr in sorted(report_addresses):
                f.write(f"{addr:#x}\n")
        print(f"Written {len(report_addresses)} VTune report IPs → {ip_list_path}")
        print(
            f"VTune coverage for {kernel}: sampled={len(report_addresses)}, "
            f"mapped={len(correlateable_entries)}, "
            f"unmapped={len(report_addresses) - len(correlateable_entries)}"
        )
        manifest_kernels.append({
            "name": kernel,
            "reference_csv": str(out_path.resolve()),
            "ip_list": str(ip_list_path.resolve()),
            "sampled_ip_count": len(report_addresses),
            "mapped_ip_count": len(correlateable_entries),
            "unmapped_ip_count": len(report_addresses) - len(correlateable_entries),
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

    # VTune samples are mapped through the raw DWARF line table because VTune's
    # SQLite source-file field can pair an inline-header line with the outer TU.
    zebins = find_zebins(RESULTS_DIR)
    zebin = zebins[0]
    correlated_locations = correlate_vtune_locations(ref_csv, zebins)
    manifest_kernels = generate_vtune_reference(
        ref_csv, RESULTS_DIR, correlated_locations
    )
    write_manifest(RESULTS_DIR / "vtune_manifest.json", zebin, manifest_kernels)

    asm_map = load_assembly(ref_csv)

    # VTune DB — all resolution levels per address
    vtune_all_rows, rva_map = extract_vtune_all()
    write_vtune_all_csv(RESULTS_DIR / "vtune_addr_srcline.csv", vtune_all_rows, asm_map)
    for kernel_name, kernel_rows in extract_vtune_by_kernel().items():
        safe_kernel_name = re.sub(r"[^a-zA-Z0-9_-]", "_", kernel_name)
        write_vtune_all_csv(
            RESULTS_DIR / f"vtune_addr_srcline_{safe_kernel_name}.csv",
            kernel_rows,
            asm_map,
        )

    # Addresses in result.csv but absent from VTune DB (e.g. never-sampled jumps)
    vtune_addrs = {r[0] for r in vtune_all_rows}
    missing_addrs = ref_addrs - vtune_addrs

    # DWARF — run ocloc disasm to get fresh debug sections, then parse .debug_line
    dump_dir = run_ocloc_disasm(zebin)
    dwarf_rows = extract_dwarf(rva_map, zebin, dump_dir / ".debug_line",
                               extra_display_addrs=missing_addrs)
    dwarf_map = dict(dwarf_rows)
    write_csv(RESULTS_DIR / "dwarf_addr_srcline.csv", dwarf_rows)

    # Quick diff against result.csv addresses
    if ref_addrs and dwarf_rows:
        # Use NULL nested_level rows (raw DWARF) as the canonical VTune resolution
        vtune_map = {r[0]: r[2] for r in vtune_all_rows if r[1] is None}
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
