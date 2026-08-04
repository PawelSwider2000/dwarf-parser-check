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

cat > "$temporary_dir/bin/vtune" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
while [[ $# -gt 0 ]]; do
  if [[ $1 == -report-output ]]; then
    printf 'Address,Assembly\n0x0,nop\n' > "$2"
    exit 0
  fi
  if [[ $1 == -- ]]; then
    shift
    exec "$@"
  fi
  shift
done
exit 0
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
VTUNE_BIN="$temporary_dir/bin/vtune" \
"$DWARF_CHECK" --artifact-dir "$single_run_artifact_dir" run

[[ $(wc -l < "$single_run_log") -eq 1 ]] || fail "run executed the workload outside VTune"

DPC_TEST_ADAPTER_ARGUMENTS="$adapter_arguments" \
PATH="$temporary_dir/bin:$PATH" \
VTUNE_BIN="$temporary_dir/bin/vtune" \
"$DWARF_CHECK" --artifact-dir "$temporary_dir/artifacts" --build-dir "$temporary_dir/build" analyze \
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
EOF
chmod +x "$fake_dwarf_check"

DPC_TEST_CLEAN_LOG="$matrix_clean_log" \
ARTIFACT_DIR="$matrix_artifact_dir" \
DWARF_CHECK="$fake_dwarf_check" \
"$PROJECT_DIR/run_experiments.sh" --clean-results

[[ $(wc -l < "$matrix_clean_log") -eq 24 ]] || fail "cleanup option was not forwarded to every experiment"
python3 - "$matrix_artifact_dir/experiment_summary.json" <<'PY'
import json
import sys

summary = json.load(open(sys.argv[1]))
if summary["summary"] != {"total": 24, "passed": 0, "skipped": 24, "failed": 0}:
    raise SystemExit("incorrect experiment summary")
if any(experiment["status"] != "SKIP" for experiment in summary["experiments"]):
    raise SystemExit("expected every experiment to be skipped")
PY