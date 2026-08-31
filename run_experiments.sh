#!/usr/bin/env bash
# Run dwarf-parser-check experiments across all combinations of workloads,
# debug modes, optimization levels, and device compilation modes.
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
DWARF_CHECK=${DWARF_CHECK:-"$SCRIPT_DIR/dwarf-check.sh"}

# ── Experiment matrix ──────────────────────────────────────────────────────────
WORKLOADS=(${WORKLOADS:-gemm control_flow multifile inline_chain multifile_inline_chain})
DEBUG_MODES=(g)
OPT_LEVELS=(O0 O1 O2)
DEVICE_CODE_MODES=(jit aot)

# AOT builds produce one fat binary containing native images for each target.
AOT_TARGETS=${AOT_TARGETS:-bmg,pvc}

# Comma-separated adapter names to build and run (rust-gimli, iga, gdb-intel).
# Defaults to rust-gimli because gdb-intel requires an external addr2line binary.
ADAPTERS=${ADAPTERS:-rust-gimli}

# Map runtime adapter names (used by the dwarf-parser-check binary) to the
# cmake build-time names (used by adapter build --adapter).
runtime_to_cmake_adapter() {
  case "$1" in
    rust-gimli) echo "gimli-rust" ;;
    *)          echo "$1" ;;
  esac
}

# ── Optional overrides ─────────────────────────────────────────────────────────
# Pass any extra dwarf-check.sh global flags via EXTRA_FLAGS, e.g.:
#   EXTRA_FLAGS="--artifact-dir /tmp/artifacts" ./run_experiments.sh
EXTRA_FLAGS=${EXTRA_FLAGS:-}
ARTIFACT_DIR=${ARTIFACT_DIR:-"$SCRIPT_DIR/artifacts"}
EXPERIMENT_SUMMARY_FILE=${EXPERIMENT_SUMMARY_FILE:-"$ARTIFACT_DIR/experiment_summary.json"}
EXPERIMENT_RECORDS_FILE=$(mktemp "${TMPDIR:-/tmp}/dwarf-check-experiments.XXXXXX")
trap 'rm -f "$EXPERIMENT_RECORDS_FILE"' EXIT

write_experiment_record() {
  local status=$1
  local name=$2
  local workload=$3
  local configuration=$4
  local result_dir="$ARTIFACT_DIR/results/$workload/$configuration"
  local comparison_file="$result_dir/adapter_rust-gimli_vtune_comparison.json"
  local unitrace_comparison_file="$result_dir/unitrace/adapter_rust-gimli_unitrace_comparison.json"
  local vtune_manifest="$result_dir/vtune_manifest.json"
  local unitrace_manifest="$result_dir/unitrace_manifest.json"

  python3 - "$EXPERIMENT_RECORDS_FILE" "$status" "$name" "$comparison_file" "$unitrace_comparison_file" "$vtune_manifest" "$unitrace_manifest" <<'PY'
import json
import pathlib
import sys

records_path, status, name, vtune_comparison_path, unitrace_comparison_path, vtune_manifest_path, unitrace_manifest_path = sys.argv[1:]
record = {"name": name, "status": status, "comparison": None, "unitrace_comparison": None}
summary_keys = (
  "compared_offsets",
  "matches",
  "file_mismatches",
  "line_mismatches",
  "column_mismatches",
  "missing_in_reference",
  "missing_in_backend",
)

def aggregate_comparison(path):
  comparisons = json.loads(path.read_text())["comparisons"]
  summary = {key: 0 for key in summary_keys}
  mismatch_count = 0
  for comparison in comparisons:
    mismatch_count += comparison["mismatch_count"]
    for key in summary_keys:
      summary[key] += comparison["summary"][key]
  return {"mismatch_count": mismatch_count, "summary": summary}, comparisons

def collector_coverage(path_text):
  path = pathlib.Path(path_text)
  if not path.is_file():
    return None
  try:
    kernels = json.loads(path.read_text())["kernels"]
    coverage_keys = ("sampled_ip_count", "mapped_ip_count", "unmapped_ip_count")
    if not any(any(key in kernel for key in coverage_keys) for kernel in kernels):
      return None
    return {
        "sampled_ips": sum(kernel.get("sampled_ip_count", 0) for kernel in kernels),
        "mapped_ips": sum(kernel.get("mapped_ip_count", 0) for kernel in kernels),
        "unmapped_ips": sum(kernel.get("unmapped_ip_count", 0) for kernel in kernels),
    }
  except (KeyError, OSError, ValueError, json.JSONDecodeError):
    return None

def store_comparison(field, error_field, path_text, manifest_path):
  path = pathlib.Path(path_text)
  if not path.is_file():
    record[error_field] = f"comparison file not found: {path}"
    return None
  try:
    aggregate, comparisons = aggregate_comparison(path)
    coverage = collector_coverage(manifest_path)
    if coverage is not None:
      aggregate["coverage"] = coverage
    record[field] = aggregate
    return comparisons
  except (KeyError, OSError, ValueError, json.JSONDecodeError) as error:
    record[error_field] = str(error)
    return None

vtune_comparisons = store_comparison("comparison", "comparison_error", vtune_comparison_path, vtune_manifest_path)
store_comparison("unitrace_comparison", "unitrace_comparison_error", unitrace_comparison_path, unitrace_manifest_path)
if status == "PASS" and vtune_comparisons and all(
    comparison.get("status") == "skipped" for comparison in vtune_comparisons
):
  record["status"] = "SKIP"
  record["skip_reasons"] = [
      comparison.get("skip_reason", "") for comparison in vtune_comparisons
  ]

with open(records_path, "a", encoding="utf-8") as records_file:
  records_file.write(json.dumps(record) + "\n")
print(record["status"])
PY
}

write_experiment_summary() {
  mkdir -p "$(dirname "$EXPERIMENT_SUMMARY_FILE")"
  python3 - "$EXPERIMENT_RECORDS_FILE" "$EXPERIMENT_SUMMARY_FILE" <<'PY'
import json
import pathlib
import sys

records_path, summary_path = map(pathlib.Path, sys.argv[1:])
records = [json.loads(line) for line in records_path.read_text().splitlines()]
summary = {
  "total": len(records),
  "passed": sum(record["status"] == "PASS" for record in records),
  "skipped": sum(record["status"] == "SKIP" for record in records),
  "failed": sum(record["status"] == "FAIL" for record in records),
}
summary_path.write_text(json.dumps({"summary": summary, "experiments": records}, indent=2) + "\n")
PY
  log "summary file: $EXPERIMENT_SUMMARY_FILE"
}

# ── Build the adapter once (shared across all experiments) ─────────────────────
log() { printf '[experiments] %s\n' "$*"; }

usage() {
  cat <<EOF
Usage: $(basename "$0") [--clean-results]

  --clean-results  Remove each experiment's results directory before it runs.
EOF
}

clean_results=false
while [[ $# -gt 0 ]]; do
  case "$1" in
    --clean-results)
      clean_results=true
      shift
      ;;
    --help|-h|help)
      usage
      exit 0
      ;;
    *)
      printf 'run_experiments: unknown option: %s\n' "$1" >&2
      exit 1
      ;;
  esac
done

clean_results_flag=()
if [[ "$clean_results" == true ]]; then
  clean_results_flag=(--clean-results)
fi

log "building dwarf-parser-check adapter (adapters: $ADAPTERS)"
# Convert comma-separated ADAPTERS into repeated --adapter flags
_adapter_flags=()
IFS=',' read -ra _adapter_list <<< "$ADAPTERS"
for _a in "${_adapter_list[@]}"; do
  _adapter_flags+=(--adapter "$(runtime_to_cmake_adapter "$_a")")
done
# shellcheck disable=SC2086
"$DWARF_CHECK" $EXTRA_FLAGS adapter build "${_adapter_flags[@]}"

# ── Run experiments ────────────────────────────────────────────────────────────
total=$(( ${#WORKLOADS[@]} * ${#DEBUG_MODES[@]} * ${#OPT_LEVELS[@]} * ${#DEVICE_CODE_MODES[@]} ))
count=0
failed=()

for workload in "${WORKLOADS[@]}"; do
  for debug in "${DEBUG_MODES[@]}"; do
    for opt in "${OPT_LEVELS[@]}"; do
      for device_code in "${DEVICE_CODE_MODES[@]}"; do
        (( count++ )) || true
        label="$workload/debug=$debug/opt=$opt/device-code=$device_code"
        arguments=(
          "${clean_results_flag[@]}"
          --workload "$workload"
          --debug "$debug"
          --opt "$opt"
          --device-code "$device_code"
        )
        if [[ "$device_code" == aot ]]; then
          label+="/device-target=$AOT_TARGETS"
          arguments+=(--device-target "$AOT_TARGETS")
          configuration="aot-${AOT_TARGETS//,/-}-$debug-$opt"
        else
          configuration="jit-$debug-$opt"
        fi
        arguments+=(all --adapters "$ADAPTERS")
        log "[$count/$total] experiment: $label"

        # shellcheck disable=SC2086
        if "$DWARF_CHECK" $EXTRA_FLAGS "${arguments[@]}"; then
          experiment_status=$(write_experiment_record PASS "$label" "$workload" "$configuration")
          log "[$count/$total] $experiment_status: $label"
        else
          log "[$count/$total] FAIL: $label"
          failed+=("$label")
          write_experiment_record FAIL "$label" "$workload" "$configuration" >/dev/null
        fi
      done
    done
  done
done

# ── Summary ────────────────────────────────────────────────────────────────────
echo
log "results: $((count - ${#failed[@]}))/$count completed"
write_experiment_summary
if [[ ${#failed[@]} -gt 0 ]]; then
  log "failed experiments:"
  for f in "${failed[@]}"; do
    log "  - $f"
  done
  exit 1
fi
