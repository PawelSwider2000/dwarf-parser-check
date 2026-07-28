#!/usr/bin/env bash
set -euo pipefail

DEBUG_MODE=g
OPT_LEVEL=O0

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ARTIFACT_DIR=${ARTIFACT_DIR:-"$SCRIPT_DIR/../artifacts"}
WORKLOAD=${WORKLOAD:-gemm}
WORKLOAD_DIR="$SCRIPT_DIR/workloads/$WORKLOAD"
WORKLOAD_BUILD_DIR="$ARTIFACT_DIR/build/$WORKLOAD/$DEBUG_MODE-$OPT_LEVEL"
BIN_PATH="$WORKLOAD_BUILD_DIR/bin/$WORKLOAD"
JSON_PATH="$ARTIFACT_DIR/simple_sycl_vtune_kernel_debug.json"
VTUNE_RESULT_DIR=${VTUNE_RESULT_DIR:-"$ARTIFACT_DIR/vtune_pc_sampling"}
VTUNE_COMPUTING_TASK=${VTUNE_COMPUTING_TASK:-PrimaryGEMMKernel}
VTUNE_REFERENCE_CSV=${VTUNE_REFERENCE_CSV:-"$ARTIFACT_DIR/result_vtune_reference.csv"}
VTUNE_SOURCE_LOCATIONS_JSON=${VTUNE_SOURCE_LOCATIONS_JSON:-"$ARTIFACT_DIR/source_locations.json"}
VTUNE_TARGET_GPU=${VTUNE_TARGET_GPU:-}
USER_SOURCE_ROOT=${USER_SOURCE_ROOT:-"$SCRIPT_DIR"}

log() {
  printf '[make_reference] %s\n' "$*"
}

usage() {
  cat <<EOF
Usage: $(basename "$0") [--debug <g|gline|none>] [--opt <O0|O1|O2>] <command> [<command> ...]

Commands execute in the order provided:
  build      Build the SYCL sample.
  run        Run the sample and write its kernel debug metadata.
  vtune_run  Profile the sample with VTune GPU PC sampling.
  analyze    Generate a VTune address-to-source-line CSV report.

Options:
  --debug MODE       Debug information: g, gline, or none (default: g)
  --opt LEVEL        Optimization: O0, O1, or O2 (default: O0)

Configuration is provided through environment variables:
  ARTIFACT_DIR      Generated-artifact directory
  WORKLOAD          Workload directory name (default: gemm)
  WORKLOAD_BUILD_DIR
                      CMake build directory (default: artifacts/build/<debug>-<opt>)
  VTUNE_RESULT_DIR  VTune result directory
  VTUNE_COMPUTING_TASK
                      GPU computing task to analyze (default: PrimaryGEMMKernel)
  VTUNE_REFERENCE_CSV
                      Output CSV path (default: artifacts/result_vtune_reference.csv)
  VTUNE_SOURCE_LOCATIONS_JSON
                      Per-IP highest user locations JSON output path
  VTUNE_TARGET_GPU  Comma-separated PCI GPU adapter IDs to profile
  USER_SOURCE_ROOT    Root directory used to identify user source files
EOF
}

build() {
  if [[ ! -f "$WORKLOAD_DIR/CMakeLists.txt" ]]; then
    echo "workload CMake project not found: $WORKLOAD_DIR" >&2
    return 1
  fi

  log "build: configuring workload $WORKLOAD (debug=$DEBUG_MODE, opt=$OPT_LEVEL)"
  mkdir -p "$ARTIFACT_DIR"

  cmake -S "$WORKLOAD_DIR" -B "$WORKLOAD_BUILD_DIR" \
    -DCMAKE_CXX_COMPILER=icpx \
    -DDPC_WORKLOAD_DEBUG="$DEBUG_MODE" \
    -DDPC_WORKLOAD_OPT="$OPT_LEVEL"
  cmake --build "$WORKLOAD_BUILD_DIR"

  log "build: binary: $BIN_PATH"
}

run() {
  log "run: executing workload $WORKLOAD"
  if [[ ! -x "$BIN_PATH" ]]; then
    echo "sample binary not found or not executable: $BIN_PATH; run 'build' first" >&2
    return 1
  fi

  (
    cd "$ARTIFACT_DIR"
    ZE_ENABLE_TRACING_LAYER=${ZE_ENABLE_TRACING_LAYER:-1} \
    "$BIN_PATH" "$JSON_PATH"
  )

  log "run: json: $JSON_PATH"
}

vtune_run() {
  log "vtune_run: collecting GPU stall reasons into $VTUNE_RESULT_DIR"
  if [[ ! -x "$BIN_PATH" ]]; then
    echo "sample binary not found or not executable: $BIN_PATH; run 'build' first" >&2
    return 1
  fi

  local ptrace_scope_file=/proc/sys/kernel/yama/ptrace_scope
  if [[ -r "$ptrace_scope_file" ]] && [[ $(<"$ptrace_scope_file") != 0 ]]; then
    echo "VTune profiling requires unrestricted ptrace access. Run: sudo sysctl -w kernel.yama.ptrace_scope=0" >&2
    return 1
  fi

  mkdir -p "$ARTIFACT_DIR"
  if [[ -e "$VTUNE_RESULT_DIR" ]]; then
    log "vtune_run: removing previous result: $VTUNE_RESULT_DIR"
    rm -rf "$VTUNE_RESULT_DIR"
  fi

  local vtune_args=(
    -collect gpu-hotspots \
    -knob gpu-profiling-mode=source-analysis \
    -knob source-analysis=stall-sampling \
    -result-dir "$VTUNE_RESULT_DIR" \
  )
  if [[ -n "$VTUNE_TARGET_GPU" ]]; then
    vtune_args+=(-knob "target-gpu=$VTUNE_TARGET_GPU")
  fi
  vtune_args+=(-- "$BIN_PATH" "$JSON_PATH")

  vtune "${vtune_args[@]}"

  log "vtune_run: VTune result: $VTUNE_RESULT_DIR"
}

analyze() {
  log "analyze: generating source correlation for $VTUNE_COMPUTING_TASK"
  if [[ ! -d "$VTUNE_RESULT_DIR" ]]; then
    echo "VTune result directory not found: $VTUNE_RESULT_DIR; run 'vtune_run' first" >&2
    return 1
  fi

  mkdir -p "$(dirname "$VTUNE_REFERENCE_CSV")"
  local raw_report
  raw_report=$(mktemp "${TMPDIR:-/tmp}/vtune-reference.XXXXXX.csv")
  vtune \
    -report hotspots \
    -result-dir "$VTUNE_RESULT_DIR" \
    -source-object "computing-task=$VTUNE_COMPUTING_TASK" \
    -group-by address \
    -format csv \
    -csv-delimiter comma \
    -report-width 0 \
    -report-output "$raw_report"

  local correlate_args=(
    --input "$raw_report"
    --output "$VTUNE_REFERENCE_CSV"
    --result-dir "$VTUNE_RESULT_DIR"
    --source-locations-output "$VTUNE_SOURCE_LOCATIONS_JSON"
    --user-source-root "$USER_SOURCE_ROOT"
  )
  if ! python3 "$SCRIPT_DIR/correlate_vtune_report.py" "${correlate_args[@]}"; then
    rm -f "$raw_report"
    return 1
  fi
  rm -f "$raw_report"

  log "analyze: CSV report: $VTUNE_REFERENCE_CSV"
  log "analyze: user source locations: $VTUNE_SOURCE_LOCATIONS_JSON"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --debug)
      if [[ $# -lt 2 ]]; then
        echo "--debug requires g, gline, or none" >&2
        exit 1
      fi
      DEBUG_MODE=$2
      case "$DEBUG_MODE" in
        g|gline|none)
          ;;
        *)
          echo "unsupported debug mode: $DEBUG_MODE; expected g, gline, or none" >&2
          exit 1
          ;;
      esac
      shift 2
      ;;
    --opt)
      if [[ $# -lt 2 ]]; then
        echo "--opt requires O0, O1, or O2" >&2
        exit 1
      fi
      OPT_LEVEL=${2#-}
      case "$OPT_LEVEL" in
        O0|O1|O2)
          ;;
        *)
          echo "unsupported optimization level: $OPT_LEVEL; expected O0, O1, or O2" >&2
          exit 1
          ;;
      esac
      shift 2
      ;;
    --)
      shift
      break
      ;;
    --*)
      echo "unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
    *)
      break
      ;;
  esac
done
if [[ $# -eq 0 ]]; then
  usage >&2
  exit 1
fi

for command in "$@"; do
  case "$command" in
    build|run|vtune_run|analyze)
      log "starting command: $command"
      "$command"
      ;;
    --help|-h|help)
      usage
      ;;
    *)
      echo "unknown command: $command" >&2
      usage >&2
      exit 1
      ;;
  esac
done
