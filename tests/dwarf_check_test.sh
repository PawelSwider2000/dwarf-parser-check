#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PROJECT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
DWARF_CHECK="$PROJECT_DIR/dwarf-check.sh"

fail() {
  printf 'dwarf_check_test: %s\n' "$*" >&2
  exit 1
}

expect_failure() {
  local expected=$1
  shift
  local output
  if output=$("$@" 2>&1); then
    fail "expected command to fail: $*"
  fi
  [[ "$output" == *"$expected"* ]] || fail "missing error text: $expected"
}

help_output=$("$DWARF_CHECK" --help)
[[ "$help_output" == *"adapter build"* ]] || fail "missing adapter build help"
[[ "$help_output" == *"workload build"* ]] || fail "missing workload build help"
[[ "$help_output" == *"default: both"* ]] || fail "missing both collector default"
expect_failure "unsupported debug mode: invalid" "$DWARF_CHECK" --debug invalid build
expect_failure "unsupported device compilation mode: invalid" "$DWARF_CHECK" --device-code invalid build
expect_failure "unsupported AOT device target: invalid" "$DWARF_CHECK" --device-target invalid build
expect_failure "unknown run option: --unexpected" "$DWARF_CHECK" run --unexpected

temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/dwarf-check-test.XXXXXX")
trap 'rm -rf "$temporary_dir"' EXIT
expect_failure "$temporary_dir/artifacts/build/gemm/jit-gline-O0/bin/gemm" \
  "$DWARF_CHECK" --artifact-dir "$temporary_dir/artifacts" --debug gline run
expect_failure "$temporary_dir/artifacts/build/gemm/aot-bmg-pvc-g-O0/bin/gemm" \
  "$DWARF_CHECK" --artifact-dir "$temporary_dir/artifacts" --device-code aot run
expect_failure "$temporary_dir/artifacts/build/gemm/aot-pvc-g-O0/bin/gemm" \
  "$DWARF_CHECK" --artifact-dir "$temporary_dir/artifacts" --device-code aot --device-target pvc run

mkdir -p "$temporary_dir/bin" "$temporary_dir/build" "$temporary_dir/result"
manifest="$temporary_dir/kernel_debug.json"
reference_csv="$temporary_dir/vtune_reference.csv"
output_dir="$temporary_dir/adapter_reports"
adapter_arguments="$temporary_dir/adapter_arguments.txt"
printf '{}\n' > "$manifest"

vtune_instruction_report="$temporary_dir/vtune-instruction-report.csv"
vtune_address_only_report="$temporary_dir/vtune-address-only-report.csv"
printf 'Address,Assembly\n0x10,nop\n0x20,Block 1:\n0x30,illegal\n' > "$vtune_instruction_report"
printf 'Address\n0x40\n0x50\n' > "$vtune_address_only_report"
filtered_vtune_addresses=$(python3 - "$vtune_instruction_report" "$vtune_address_only_report" \
  "$PROJECT_DIR/data_generation/extract_addr_srcline.py" <<'PY'
import importlib.util
import sys
from pathlib import Path

instruction_report, address_only_report, module_path = map(Path, sys.argv[1:])
sys.argv = [str(module_path)]
spec = importlib.util.spec_from_file_location("extract_addr_srcline", module_path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
instruction_addresses = sorted(module.report_instruction_addresses(instruction_report))
address_only_addresses = sorted(module.report_instruction_addresses(address_only_report))
print(
    ",".join(hex(address) for address in instruction_addresses)
    + ";"
    + ",".join(hex(address) for address in address_only_addresses)
)
PY
)
[[ "$filtered_vtune_addresses" == "0x10;0x40,0x50" ]] || \
  fail "VTune instruction filtering did not exclude pseudo-instruction addresses"

cat > "$temporary_dir/bin/vtune" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
application=()
while [[ $# -gt 0 ]]; do
  if [[ $1 == -report-output ]]; then
    printf 'Address,Assembly\n0x0,nop\n' > "$2"
    exit 0
  fi
  if [[ $1 == -- ]]; then
    shift
    application=("$@")
    break
  fi
  shift
done
if [[ -n ${DPC_TEST_VTUNE_ATTEMPTS:-} ]]; then
  printf 'attempt\n' >> "$DPC_TEST_VTUNE_ATTEMPTS"
  [[ $(wc -l < "$DPC_TEST_VTUNE_ATTEMPTS") -ge 2 ]] || exit 1
fi
exec "${application[@]}"
EOF
cat > "$temporary_dir/bin/unitrace" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
result_dir=
application=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --result-dir)
      result_dir=$2
      shift 2
      ;;
    --stall-sampling)
      shift
      ;;
    *)
      application=("$@")
      break
      ;;
  esac
done
if [[ -n ${DPC_TEST_UNITRACE_ATTEMPTS:-} ]]; then
  printf 'attempt\n' >> "$DPC_TEST_UNITRACE_ATTEMPTS"
  [[ $(wc -l < "$DPC_TEST_UNITRACE_ATTEMPTS") -ge 2 ]] || exit 0
fi
mkdir -p "$result_dir/unitrace.test/metrics"
printf 'Kernel,IP[Address]\ntest,0\n' > "$result_dir/unitrace.test/metrics/metrics_0.csv"
exec "${application[@]}"
EOF
cat > "$temporary_dir/bin/python3" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
results_dir=$2
mkdir -p "$results_dir"
printf 'Address,Source File,Source Line\n0x0,source.cpp,1\n' > "$(dirname "$results_dir")/vtune_reference.csv"
EOF
cat > "$temporary_dir/build/dwarf-parser-check" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$@" > "$DPC_TEST_ADAPTER_ARGUMENTS"
EOF
chmod +x "$temporary_dir/bin/vtune" "$temporary_dir/bin/python3" \
  "$temporary_dir/bin/unitrace" \
  "$temporary_dir/build/dwarf-parser-check"

single_run_artifact_dir="$temporary_dir/single-run-artifacts"
single_run_binary="$single_run_artifact_dir/build/gemm/jit-g-O0/bin/gemm"
single_run_log="$temporary_dir/workload-runs.log"
mkdir -p "$(dirname "$single_run_binary")"
cat > "$single_run_binary" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf 'run\n' >> "$DPC_TEST_WORKLOAD_LOG"
mkdir -p "$(dirname "$1")"
printf '{"kernels": []}\n' > "$1"
EOF
chmod +x "$single_run_binary"

DPC_TEST_WORKLOAD_LOG="$single_run_log" \
UNITRACE_BIN="$temporary_dir/bin/unitrace" \
"$DWARF_CHECK" --tool unitrace --artifact-dir "$single_run_artifact_dir" run

[[ $(wc -l < "$single_run_log") -eq 1 ]] || fail "run did not execute the workload under unitrace"

retry_artifact_dir="$temporary_dir/retry-run-artifacts"
retry_binary="$retry_artifact_dir/build/gemm/jit-g-O0/bin/gemm"
retry_log="$temporary_dir/retry-workload-runs.log"
unitrace_attempts="$temporary_dir/unitrace-attempts.log"
mkdir -p "$(dirname "$retry_binary")"
cp "$single_run_binary" "$retry_binary"

DPC_TEST_WORKLOAD_LOG="$retry_log" \
DPC_TEST_UNITRACE_ATTEMPTS="$unitrace_attempts" \
UNITRACE_MAX_ATTEMPTS=2 \
UNITRACE_BIN="$temporary_dir/bin/unitrace" \
"$DWARF_CHECK" --tool unitrace --artifact-dir "$retry_artifact_dir" run

[[ $(wc -l < "$unitrace_attempts") -eq 2 ]] || fail "unitrace did not retry an empty metrics result"
[[ $(wc -l < "$retry_log") -eq 1 ]] || fail "unitrace retry did not run the workload"

vtune_retry_artifact_dir="$temporary_dir/vtune-retry-run-artifacts"
vtune_retry_binary="$vtune_retry_artifact_dir/build/gemm/jit-g-O0/bin/gemm"
vtune_retry_log="$temporary_dir/vtune-retry-workload-runs.log"
vtune_attempts="$temporary_dir/vtune-attempts.log"
mkdir -p "$(dirname "$vtune_retry_binary")"
cp "$single_run_binary" "$vtune_retry_binary"

DPC_TEST_WORKLOAD_LOG="$vtune_retry_log" \
DPC_TEST_VTUNE_ATTEMPTS="$vtune_attempts" \
VTUNE_MAX_ATTEMPTS=2 \
VTUNE_BIN="$temporary_dir/bin/vtune" \
"$DWARF_CHECK" --tool vtune --artifact-dir "$vtune_retry_artifact_dir" run

[[ $(wc -l < "$vtune_attempts") -eq 2 ]] || fail "VTune did not retry a failed collection"
[[ $(wc -l < "$vtune_retry_log") -eq 1 ]] || fail "VTune retry did not run the workload"

DPC_TEST_ADAPTER_ARGUMENTS="$adapter_arguments" \
PATH="$temporary_dir/bin:$PATH" \
VTUNE_BIN="$temporary_dir/bin/vtune" \
"$DWARF_CHECK" --tool vtune --artifact-dir "$temporary_dir/artifacts" --build-dir "$temporary_dir/build" analyze \
  --result-dir "$temporary_dir/result" \
  --kernel-debug-json "$manifest" \
  --reference-csv "$reference_csv" \
  --output-dir "$output_dir"

grep -Fx -- "--reference" "$adapter_arguments" >/dev/null || fail "adapter did not receive a reference"
grep -Fx -- "$reference_csv" "$adapter_arguments" >/dev/null || fail "adapter did not receive vtune_reference.csv"
if grep -Fq 'source_locations.json' "$adapter_arguments"; then
  fail "adapter received source_locations.json instead of vtune_reference.csv"
fi

matrix_artifact_dir="$temporary_dir/matrix-artifacts"
matrix_clean_log="$temporary_dir/matrix-clean.log"
fake_dwarf_check="$temporary_dir/fake-dwarf-check"
cat > "$fake_dwarf_check" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

if [[ $1 == adapter ]]; then
  exit 0
fi

[[ $1 == --clean-results ]] || exit 1
printf '%s\n' "$*" >> "$DPC_TEST_CLEAN_LOG"
shift
while [[ $# -gt 0 ]]; do
  case "$1" in
    --workload|--debug|--opt|--device-code|--device-target)
      case "$1" in
        --workload) workload=$2 ;;
        --debug) debug=$2 ;;
        --opt) opt=$2 ;;
        --device-code) device_code=$2 ;;
        --device-target) device_target=$2 ;;
      esac
      shift 2
      ;;
    all)
      [[ ${2:-} == --adapters ]] || exit 1
      shift 3
      ;;
    *)
      exit 1
      ;;
  esac
done

if [[ $device_code == aot ]]; then
  configuration="aot-${device_target//,/-}-${debug}-${opt}"
else
  configuration="jit-${debug}-${opt}"
fi
output_dir="$ARTIFACT_DIR/results/$workload/$configuration"
mkdir -p "$output_dir"
cat > "$output_dir/adapter_rust-gimli_vtune_comparison.json" <<'JSON'
{"comparisons":[{"backend":"rust-gimli","kernel":"test-kernel","status":"skipped","skip_reason":"VTune reference contains no source locations","mismatch_count":0,"summary":{"compared_offsets":0,"matches":0,"file_mismatches":0,"line_mismatches":0,"column_mismatches":0,"missing_in_reference":0,"missing_in_backend":0}}]}
JSON
cat > "$output_dir/adapter_pti-elf-parser_vtune_comparison.json" <<'JSON'
{"comparisons":[{"backend":"pti-elf-parser","kernel":"test-kernel","status":"skipped","skip_reason":"VTune reference contains no source locations","mismatch_count":0,"summary":{"compared_offsets":0,"matches":0,"file_mismatches":0,"line_mismatches":0,"column_mismatches":0,"missing_in_reference":0,"missing_in_backend":0}}]}
JSON
mkdir -p "$output_dir/unitrace"
cat > "$output_dir/unitrace/adapter_rust-gimli_unitrace_comparison.json" <<'JSON'
{"comparisons":[{"backend":"rust-gimli","kernel":"test-kernel","status":"compared","mismatch_count":2,"summary":{"compared_offsets":3,"matches":1,"file_mismatches":1,"line_mismatches":1,"column_mismatches":0,"missing_in_reference":0,"missing_in_backend":0}}]}
JSON
cat > "$output_dir/unitrace/adapter_pti-elf-parser_unitrace_comparison.json" <<'JSON'
{"comparisons":[{"backend":"pti-elf-parser","kernel":"test-kernel","status":"compared","mismatch_count":4,"summary":{"compared_offsets":5,"matches":1,"file_mismatches":2,"line_mismatches":2,"column_mismatches":0,"missing_in_reference":0,"missing_in_backend":0}}]}
JSON
cat > "$output_dir/vtune_manifest.json" <<'JSON'
{"kernels":[{"sampled_ip_count":8,"mapped_ip_count":5,"unmapped_ip_count":3}]}
JSON
cat > "$output_dir/unitrace_manifest.json" <<'JSON'
{"kernels":[{"sampled_ip_count":7,"mapped_ip_count":3,"unmapped_ip_count":4}]}
JSON
EOF
chmod +x "$fake_dwarf_check"

DPC_TEST_CLEAN_LOG="$matrix_clean_log" \
ARTIFACT_DIR="$matrix_artifact_dir" \
ADAPTERS="rust-gimli,pti-elf-parser" \
DWARF_CHECK="$fake_dwarf_check" \
"$PROJECT_DIR/run_experiments.sh" --clean-results

[[ $(wc -l < "$matrix_clean_log") -eq 30 ]] || fail "cleanup option was not forwarded to every experiment"
python3 - "$matrix_artifact_dir/experiment_summary.json" <<'PY'
import json
import sys

summary = json.load(open(sys.argv[1]))
if summary["summary"] != {"total": 30, "passed": 0, "skipped": 30, "failed": 0}:
    raise SystemExit("incorrect experiment summary")
if any(experiment["status"] != "SKIP" for experiment in summary["experiments"]):
    raise SystemExit("expected every experiment to be skipped")
if summary["adapters"] != ["rust-gimli", "pti-elf-parser"]:
    raise SystemExit("adapter list is missing or incorrect")
if any(set(experiment["adapter_results"]) != {"rust-gimli", "pti-elf-parser"}
       for experiment in summary["experiments"]):
    raise SystemExit("adapter results are not separated")
if any(experiment["adapter_results"]["rust-gimli"]["status"] != "SKIP"
       or experiment["adapter_results"]["pti-elf-parser"]["status"] != "SKIP"
       for experiment in summary["experiments"]):
    raise SystemExit("adapter skip status is missing or incorrect")
if any(
  {key: experiment["adapter_results"]["rust-gimli"]["unitrace_comparison"][key] for key in ("mismatch_count", "summary")} != {
    "mismatch_count": 2,
    "summary": {
      "compared_offsets": 3,
      "matches": 1,
      "file_mismatches": 1,
      "line_mismatches": 1,
      "column_mismatches": 0,
      "missing_in_reference": 0,
      "missing_in_backend": 0,
    },
  }
  for experiment in summary["experiments"]
):
  raise SystemExit("unitrace comparison summary missing or incorrect")
if any(experiment["adapter_results"]["pti-elf-parser"]["unitrace_comparison"]["mismatch_count"] != 4
       for experiment in summary["experiments"]):
  raise SystemExit("PTI unitrace comparison summary missing or incorrect")
if any(experiment["adapter_results"]["rust-gimli"]["comparison"].get("coverage") != {"sampled_ips": 8, "mapped_ips": 5, "unmapped_ips": 3}
     for experiment in summary["experiments"]):
  raise SystemExit("VTune coverage summary missing or incorrect")
if any(experiment["adapter_results"]["rust-gimli"]["unitrace_comparison"].get("coverage") != {"sampled_ips": 7, "mapped_ips": 3, "unmapped_ips": 4}
     for experiment in summary["experiments"]):
  raise SystemExit("unitrace coverage summary missing or incorrect")
PY