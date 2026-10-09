#!/usr/bin/env bash
# Build, run, and analyze every workload configuration with VTune and unitrace
# (no DWARF adapters), then generate a Markdown report of the comparison.
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
DWARF_CHECK=${DWARF_CHECK:-"$SCRIPT_DIR/dwarf-check.sh"}

# ── Defaults (environment variables or the flags below override them) ──────────
UNITRACE_BIN=${UNITRACE_BIN:-/home/sarbojit/workspace/src/kernel-disassembly/applications.analyzers.profilingtoolsinterfaces.sdk/tools/unitrace/build/unitrace}
ONEAPI_SETVARS=${ONEAPI_SETVARS:-/localdisk/oneapi2026.1/setvars.sh}
WORKLOADS=${WORKLOADS:-"gemm control_flow multifile inline_chain multifile_inline_chain"}
OPT_LEVELS=${OPT_LEVELS:-"O0 O1 O2"}
DEVICE_CODE_MODES=${DEVICE_CODE_MODES:-"jit aot"}
AOT_TARGETS=${AOT_TARGETS:-auto}
DEBUG_MODE=g
ARTIFACT_DIR=${ARTIFACT_DIR:-"$SCRIPT_DIR/artifacts"}
REPORT=${REPORT:-}
REPORT_ONLY=false

log() { printf '[vtune-unitrace] %s\n' "$*"; }
die() { printf 'run_vtune_unitrace_report: %s\n' "$*" >&2; exit 1; }

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Builds each workload configuration, collects VTune and unitrace samples, compares
their source locations, and writes a Markdown report of the whole run.

  --unitrace-bin PATH    unitrace executable (default: $UNITRACE_BIN)
  --oneapi PATH          oneAPI setvars.sh, sourced when icpx or vtune is not on PATH
                         (default: $ONEAPI_SETVARS)
  --workloads LIST       Space- or comma-separated workloads (default: $WORKLOADS)
  --opt-levels LIST      Optimization levels (default: $OPT_LEVELS)
  --device-code LIST     jit and/or aot (default: $DEVICE_CODE_MODES)
  --aot-targets LIST     Comma-separated AOT targets, or auto for the GPUs on this
                         machine (default: $AOT_TARGETS)
  --artifact-dir PATH    Artifact root (default: artifacts)
  --report PATH          Report path (default: <artifact-dir>/EXPERIMENT_RESULTS.md)
  --report-only          Skip build and collection; regenerate the report from the
                         results already in <artifact-dir>
  --help, -h             Show this help text
EOF
}

command_line=$(printf '%q ' "$0" "$@")
while [[ $# -gt 0 ]]; do
  case "$1" in
    --unitrace-bin|--oneapi|--workloads|--opt-levels|--device-code|--aot-targets|--artifact-dir|--report)
      [[ $# -ge 2 ]] || die "$1 requires a value"
      case "$1" in
        --unitrace-bin) UNITRACE_BIN=$2 ;;
        --oneapi)       ONEAPI_SETVARS=$2 ;;
        --workloads)    WORKLOADS=${2//,/ } ;;
        --opt-levels)   OPT_LEVELS=${2//,/ } ;;
        --device-code)  DEVICE_CODE_MODES=${2//,/ } ;;
        --aot-targets)  AOT_TARGETS=$2 ;;
        --artifact-dir) ARTIFACT_DIR=$(realpath -m -- "$2") ;;
        --report)       REPORT=$2 ;;
      esac
      shift 2
      ;;
    --report-only)
      REPORT_ONLY=true
      shift
      ;;
    --help|-h|help)
      usage
      exit 0
      ;;
    *)
      die "unknown option: $1 (see --help)"
      ;;
  esac
done
REPORT=${REPORT:-"$ARTIFACT_DIR/EXPERIMENT_RESULTS.md"}
RUNS_FILE="$ARTIFACT_DIR/vtune_unitrace_runs.tsv"
SUMMARY_JSON="$ARTIFACT_DIR/vtune_unitrace_summary.json"
METADATA_FILE="$ARTIFACT_DIR/vtune_unitrace_run_metadata.txt"
LOG_DIR="$ARTIFACT_DIR/logs"

# ── Preflight ──────────────────────────────────────────────────────────────────
preflight() {
  if ! command -v icpx >/dev/null 2>&1 || ! command -v "${VTUNE_BIN:-vtune}" >/dev/null 2>&1; then
    [[ -f "$ONEAPI_SETVARS" ]] || die "icpx/vtune not on PATH and oneAPI setvars not found: $ONEAPI_SETVARS (use --oneapi PATH)"
    log "loading oneAPI: $ONEAPI_SETVARS"
    # setvars.sh is not written for 'set -u'.
    set +u
    # shellcheck disable=SC1090
    source "$ONEAPI_SETVARS" >/dev/null
    set -u
  fi
  command -v icpx >/dev/null 2>&1 || die "icpx not found after loading oneAPI"
  command -v "${VTUNE_BIN:-vtune}" >/dev/null 2>&1 || die "vtune not found; set VTUNE_BIN to the vtune executable"
  [[ -x "$UNITRACE_BIN" ]] || die "unitrace executable not found: $UNITRACE_BIN (use --unitrace-bin PATH)"
  export UNITRACE_BIN
  python3 -c 'import elftools' 2>/dev/null ||
    die "python3 cannot import pyelftools; install it with: sudo apt install python3-pyelftools"
  local paranoid
  paranoid=$(sysctl -n dev.xe.observation_paranoid 2>/dev/null || echo unknown)
  [[ "$paranoid" == 0 ]] ||
    die "dev.xe.observation_paranoid is $paranoid; unitrace stall sampling needs: sudo sysctl -w dev.xe.observation_paranoid=0"
  log "unitrace: $UNITRACE_BIN"
  log "vtune: $(command -v "${VTUNE_BIN:-vtune}")"
}

# ── Matrix ─────────────────────────────────────────────────────────────────────
run_matrix() {
  mkdir -p "$LOG_DIR"
  : > "$RUNS_FILE"
  local -a workloads opt_levels device_code_modes
  read -ra workloads <<< "$WORKLOADS"
  read -ra opt_levels <<< "$OPT_LEVELS"
  read -ra device_code_modes <<< "$DEVICE_CODE_MODES"
  if [[ "$AOT_TARGETS" == auto && " ${device_code_modes[*]} " == *" aot "* ]]; then
    AOT_TARGETS=$("$DWARF_CHECK" device-target) || die "cannot detect the AOT target; pass --aot-targets"
    log "AOT target: $AOT_TARGETS"
  fi
  local total=$(( ${#workloads[@]} * ${#opt_levels[@]} * ${#device_code_modes[@]} ))
  local count=0 workload opt device_code
  for workload in "${workloads[@]}"; do
    for opt in "${opt_levels[@]}"; do
      for device_code in "${device_code_modes[@]}"; do
        (( count++ )) || true
        local label="$workload/debug=$DEBUG_MODE/opt=$opt/device-code=$device_code"
        local arguments=(
          --artifact-dir "$ARTIFACT_DIR"
          --workload "$workload"
          --debug "$DEBUG_MODE"
          --opt "$opt"
          --device-code "$device_code"
          --clean-results
        )
        local configuration
        if [[ "$device_code" == aot ]]; then
          label+="/device-target=$AOT_TARGETS"
          arguments+=(--device-target "$AOT_TARGETS")
          configuration="aot-${AOT_TARGETS//,/-}-$DEBUG_MODE-$opt"
        else
          configuration="jit-$DEBUG_MODE-$opt"
        fi
        local log_file="$LOG_DIR/$workload-$configuration.log"
        log "[$count/$total] experiment: $label (log: $log_file)"
        local rc=0
        "$DWARF_CHECK" "${arguments[@]}" all --adapters none 2>&1 | tee "$log_file" || rc=${PIPESTATUS[0]}
        local status=PASS
        [[ $rc -eq 0 ]] || status=FAIL
        log "[$count/$total] $status: $label (exit code $rc)"
        printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
          "$workload" "$configuration" "$label" "$status" "$rc" "$log_file" >> "$RUNS_FILE"
      done
    done
  done
}

# ── Report ─────────────────────────────────────────────────────────────────────
# Records the tools and command used for collection, so --report-only describes
# the run that produced the results rather than the current environment.
record_run_metadata() {
  local vtune_path
  vtune_path=$(command -v "${VTUNE_BIN:-vtune}")
  {
    printf '%s\n' "$UNITRACE_BIN"
    "$UNITRACE_BIN" --version 2>/dev/null | head -n 1 || echo
    printf '%s\n' "$vtune_path"
    "$vtune_path" --version 2>/dev/null | head -n 1 || echo
    git -C "$SCRIPT_DIR" describe --always --dirty 2>/dev/null || echo
    printf '%s\n' "${command_line% }"
  } > "$METADATA_FILE"
}

generate_report() {
  [[ -s "$RUNS_FILE" ]] || die "no run list found: $RUNS_FILE; run without --report-only first"
  local -a metadata=()
  [[ -f "$METADATA_FILE" ]] && mapfile -t metadata < "$METADATA_FILE"
  python3 "$SCRIPT_DIR/data_generation/generate_vtune_unitrace_report.py" \
    --artifact-dir "$ARTIFACT_DIR" \
    --runs "$RUNS_FILE" \
    --output "$REPORT" \
    --json-output "$SUMMARY_JSON" \
    --unitrace-bin "${metadata[0]:-}" \
    --unitrace-version "${metadata[1]:-}" \
    --vtune-bin "${metadata[2]:-}" \
    --vtune-version "${metadata[3]:-}" \
    --git-commit "${metadata[4]:-}" \
    --command "${metadata[5]:-}"
}

if [[ "$REPORT_ONLY" == false ]]; then
  preflight
  mkdir -p "$ARTIFACT_DIR"
  record_run_metadata
  run_matrix
fi
generate_report

failures=$(awk -F'\t' '$4 != "PASS"' "$RUNS_FILE" | wc -l)
log "results: $(( $(wc -l < "$RUNS_FILE") - failures ))/$(wc -l < "$RUNS_FILE") runs passed"
log "report: $REPORT"
if [[ $failures -gt 0 ]]; then
  awk -F'\t' '$4 != "PASS" { printf "[vtune-unitrace]   FAIL: %s (exit code %s; log %s)\n", $3, $5, $6 }' "$RUNS_FILE"
  exit 1
fi
