#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

DEBUG_MODE=${DEBUG_MODE:-g}
OPT_LEVEL=${OPT_LEVEL:-O0}
DEVICE_CODE=${DEVICE_CODE:-jit}
DEVICE_TARGET=${DEVICE_TARGET:-auto}
DRM_SYSFS_ROOT=${DRM_SYSFS_ROOT:-/sys/class/drm}
ARTIFACT_DIR=${ARTIFACT_DIR:-"$SCRIPT_DIR/artifacts"}
WORKLOAD=${WORKLOAD:-gemm}
BUILD_DIR=${BUILD_DIR:-"$SCRIPT_DIR/build"}
BUILD_TYPE=${BUILD_TYPE:-Debug}
CMAKE_GENERATOR=${CMAKE_GENERATOR:-Ninja}
GDB_ADDR2LINE=${GDB_ADDR2LINE:-"$SCRIPT_DIR/../applications.debuggers.gdb-build-intelgt/binutils/addr2line"}
VTUNE_BIN=${VTUNE_BIN:-vtune}
VTUNE_TARGET_GPU=${VTUNE_TARGET_GPU:-}
VTUNE_COMPUTING_TASK=${VTUNE_COMPUTING_TASK:-}
VTUNE_MAX_ATTEMPTS=${VTUNE_MAX_ATTEMPTS:-3}
DEFAULT_UNITRACE_BIN=$(realpath -m -- "$SCRIPT_DIR/../applications.analyzers.profilingtoolsinterfaces.sdk/tools/unitrace/build/unitrace")
UNITRACE_BIN=${UNITRACE_BIN:-"$DEFAULT_UNITRACE_BIN"}
UNITRACE_MAX_ATTEMPTS=${UNITRACE_MAX_ATTEMPTS:-3}
# Stall sampling interval in us; 0.5 matches VTune's default, unitrace's own default is 50.
UNITRACE_SAMPLING_INTERVAL=${UNITRACE_SAMPLING_INTERVAL:-0.5}
TOOL=${TOOL:-both}
ADAPTERS=${ADAPTERS:-rust-gimli}
CLEAN_RESULTS=false

workload_configuration_from_environment=${WORKLOAD_CONFIGURATION+x}
workload_build_dir_from_environment=${WORKLOAD_BUILD_DIR+x}
workload_results_dir_from_environment=${WORKLOAD_RESULTS_DIR+x}
kernel_debug_json_from_environment=${KERNEL_DEBUG_JSON+x}
vtune_result_dir_from_environment=${VTUNE_RESULT_DIR+x}
vtune_reference_csv_from_environment=${VTUNE_REFERENCE_CSV+x}
unitrace_result_dir_from_environment=${UNITRACE_RESULT_DIR+x}
WORKLOAD_CONFIGURATION=${WORKLOAD_CONFIGURATION:-}
WORKLOAD_BUILD_DIR=${WORKLOAD_BUILD_DIR:-}
WORKLOAD_RESULTS_DIR=${WORKLOAD_RESULTS_DIR:-}
BIN_PATH="$WORKLOAD_BUILD_DIR/bin/$WORKLOAD"
DEFAULT_KERNEL_DEBUG_JSON="$WORKLOAD_RESULTS_DIR/kernel_debug.json"
KERNEL_DEBUG_JSON=${KERNEL_DEBUG_JSON:-}
DEFAULT_VTUNE_RESULT_DIR="$WORKLOAD_RESULTS_DIR/vtune_results"
VTUNE_RESULT_DIR=${VTUNE_RESULT_DIR:-}
VTUNE_REFERENCE_CSV=${VTUNE_REFERENCE_CSV:-}
UNITRACE_RESULT_DIR=${UNITRACE_RESULT_DIR:-}

log() {
  printf '[dwarf-check] %s\n' "$*"
}

CURRENT_COMMAND=

die() {
  local context=${CURRENT_COMMAND:+$CURRENT_COMMAND}
  if [[ -n "$WORKLOAD_CONFIGURATION" ]]; then
    context+="${context:+ }$WORKLOAD/$WORKLOAD_CONFIGURATION"
  fi
  printf 'dwarf-check%s: %s\n' "${context:+ [$context]}" "$*" >&2
  exit 1
}

report_unexpected_failure() {
  local rc=$1 line=$2 failed_command=$3
  printf 'dwarf-check%s: command failed with exit code %s at %s:%s\n  command: %s\n' \
    "${CURRENT_COMMAND:+ [$CURRENT_COMMAND${WORKLOAD_CONFIGURATION:+ $WORKLOAD/$WORKLOAD_CONFIGURATION}]}" \
    "$rc" "${BASH_SOURCE[1]:-$0}" "$line" "$failed_command" >&2
}
trap 'report_unexpected_failure $? $LINENO "$BASH_COMMAND"' ERR

# Resolves an external tool and logs the executable path that will be used.
# Usage: require_tool LABEL VAR_NAME VALUE [DEFAULT]
require_tool() {
  local label=$1 var_name=$2 value=$3 default=${4:-}
  local resolved
  if ! resolved=$(command -v -- "$value" 2>/dev/null) || [[ ! -x "$resolved" ]]; then
    local message="$label executable not found: '$value' (from $var_name"
    [[ -n "$default" ]] && message+="; default: $default"
    message+="). Set $var_name to the $label executable or add it to PATH"
    die "$message"
  fi
  log "using $label: $resolved"
}

# Prints a shell-quoted command line.
format_command() {
  local quoted
  printf -v quoted '%q ' "$@"
  printf '%s' "${quoted% }"
}

# Logs the files a collector left in its result directory.
log_result_dir_contents() {
  local label=$1 result_dir=$2
  if [[ ! -d "$result_dir" ]]; then
    log "run: $label result dir was not created: $result_dir"
    return 0
  fi
  log "run: $label result dir contents ($result_dir):"
  local listing
  listing=$(find "$result_dir" -mindepth 1 -maxdepth 2 -printf '  %P (%s bytes)\n' | head -n 40)
  if [[ -n "$listing" ]]; then
    printf '%s\n' "$listing"
  else
    log "run:   (empty)"
  fi
}

usage() {
  cat <<EOF
Usage: $(basename "$0") [global-options] <command> [command-options] ...

Global options must appear before the first command:
  --workload NAME       Workload under data_generation/workloads (default: gemm)
  --debug MODE          Device debug information: g, gline, or none (default: g)
  --opt LEVEL           Workload optimization: O0, O1, or O2 (default: O0)
  --device-code MODE    Device compilation: jit or aot (default: jit)
  --device-target LIST  Comma-separated AOT targets: bmg, pvc, or auto for the GPUs
                        on this machine (default: auto)
  --artifact-dir PATH   Workload artifact root (default: artifacts)
  --build-dir PATH      dwarf-parser-check CMake build directory (default: build)
  --tool NAME           Collector to use: vtune, unitrace, or both (default: both)
  --clean-results       Remove the selected results directory before run or all
  --help, -h            Show this help text

Commands:
  adapter build         Configure and build dwarf-parser-check.
    --adapter NAME      Build only the named adapter(s) (gimli-rust, iga, gdb-intel, pti-elf-parser);
                        may be repeated; omit to build all.
  workload build        Build the selected SYCL workload.
  build                 Build the workload and dwarf-parser-check.
  device-target         Print the AOT device target(s), with auto resolved to this machine's GPUs.
  run                   Run the workload, write its manifest, and collect selected samples.
  analyze               Generate selected references and run resolver adapters.
  all                   Build workload, collect selected samples, and analyze with all adapters.
    --adapters LIST     Comma-separated adapter names, all, or none to skip DWARF adapters (default: all).
  test                  Build the adapter, run CTest, then run Rust adapter tests.

Run options:
  --kernel-debug-json PATH  Workload manifest output path.
  --target-gpu PCI_IDS      VTune GPU adapter IDs.
  --result-dir PATH         VTune result directory.
  --unitrace-result-dir PATH  Unitrace result directory.

Analyze options:
  --result-dir PATH         Existing VTune result directory.
  --unitrace-result-dir PATH  Existing unitrace result directory.
  --reference-csv PATH      VTune address-to-source CSV output path.
  --adapters LIST           Comma-separated adapter names, all, or none to skip DWARF adapters.
  --kernel-debug-json PATH  Existing workload manifest path.
  --output-dir PATH         Adapter report output directory.

Examples:
  $(basename "$0") build run
  $(basename "$0") analyze --adapters iga
  $(basename "$0") --workload gemm --debug g --opt O0 --device-code aot workload build
EOF
}

validate_debug_mode() {
  case "$DEBUG_MODE" in
    g|gline|none)
      ;;
    *)
      die "unsupported debug mode: $DEBUG_MODE; expected g, gline, or none"
      ;;
  esac
}

validate_opt_level() {
  OPT_LEVEL=${OPT_LEVEL#-}
  case "$OPT_LEVEL" in
    O0|O1|O2)
      ;;
    *)
      die "unsupported optimization level: $OPT_LEVEL; expected O0, O1, or O2"
      ;;
  esac
}

validate_device_code() {
  case "$DEVICE_CODE" in
    jit|aot)
      ;;
    *)
      die "unsupported device compilation mode: $DEVICE_CODE; expected jit or aot"
      ;;
  esac
}

# Prints the AOT targets of the Intel GPUs on this machine, from their PCI device IDs.
detect_device_target() {
  local card vendor device target targets=()
  for card in "$DRM_SYSFS_ROOT"/card*/device; do
    [[ -r "$card/vendor" && -r "$card/device" ]] || continue
    vendor=$(<"$card/vendor")
    [[ "${vendor,,}" == 0x8086 ]] || continue
    device=$(<"$card/device")
    case "${device,,}" in
      0xe2??) target=bmg ;;
      0x0bd[0-9ab]) target=pvc ;;
      *) continue ;;
    esac
    [[ " ${targets[*]} " == *" $target "* ]] || targets+=("$target")
    log "detected $target GPU (PCI device ID $device) at ${card%/device}" >&2
  done
  [[ ${#targets[@]} -gt 0 ]] || \
    die "no BMG or PVC GPU found under $DRM_SYSFS_ROOT; pass --device-target bmg, pvc, or bmg,pvc"
  local IFS=,
  printf '%s\n' "${targets[*]}"
}

validate_device_target() {
  local target
  local -a targets
  if [[ "$DEVICE_TARGET" == auto ]]; then
    [[ "$DEVICE_CODE" == aot ]] || return 0
    DEVICE_TARGET=$(detect_device_target) || exit 1
  fi
  IFS=',' read -ra targets <<< "$DEVICE_TARGET"
  [[ ${#targets[@]} -gt 0 ]] || die "at least one AOT device target is required"
  for target in "${targets[@]}"; do
    case "$target" in
      bmg|pvc)
        ;;
      *)
        die "unsupported AOT device target: $target; expected bmg or pvc"
        ;;
    esac
  done
}

validate_tool() {
  case "$TOOL" in
    vtune|unitrace|both)
      ;;
    *)
      die "unsupported tool: $TOOL; expected vtune, unitrace, or both"
      ;;
  esac
}

tool_selected() {
  [[ "$TOOL" == both || "$TOOL" == "$1" ]]
}

refresh_derived_paths() {
  if [[ -z "$workload_configuration_from_environment" ]]; then
    if [[ "$DEVICE_CODE" == aot ]]; then
      WORKLOAD_CONFIGURATION="aot-${DEVICE_TARGET//,/-}-$DEBUG_MODE-$OPT_LEVEL"
    else
      WORKLOAD_CONFIGURATION="jit-$DEBUG_MODE-$OPT_LEVEL"
    fi
  fi
  if [[ -z "$workload_build_dir_from_environment" ]]; then
    WORKLOAD_BUILD_DIR="$ARTIFACT_DIR/build/$WORKLOAD/$WORKLOAD_CONFIGURATION"
  fi
  if [[ -z "$workload_results_dir_from_environment" ]]; then
    WORKLOAD_RESULTS_DIR="$ARTIFACT_DIR/results/$WORKLOAD/$WORKLOAD_CONFIGURATION"
  fi
  BIN_PATH="$WORKLOAD_BUILD_DIR/bin/$WORKLOAD"
  DEFAULT_KERNEL_DEBUG_JSON="$WORKLOAD_RESULTS_DIR/kernel_debug.json"
  if [[ -z "$kernel_debug_json_from_environment" ]]; then
    KERNEL_DEBUG_JSON="$DEFAULT_KERNEL_DEBUG_JSON"
  fi
  DEFAULT_VTUNE_RESULT_DIR="$WORKLOAD_RESULTS_DIR/vtune_results"
  if [[ -z "$vtune_result_dir_from_environment" ]]; then
    VTUNE_RESULT_DIR="$DEFAULT_VTUNE_RESULT_DIR"
  fi
  if [[ -z "$vtune_reference_csv_from_environment" ]]; then
    VTUNE_REFERENCE_CSV="$WORKLOAD_RESULTS_DIR/vtune_reference.csv"
  fi
  VTUNE_MANIFEST_JSON="$WORKLOAD_RESULTS_DIR/vtune_manifest.json"
  DEFAULT_UNITRACE_RESULT_DIR="$WORKLOAD_RESULTS_DIR/unitrace_results"
  if [[ -z "$unitrace_result_dir_from_environment" ]]; then
    UNITRACE_RESULT_DIR="$DEFAULT_UNITRACE_RESULT_DIR"
  fi
  UNITRACE_MANIFEST_JSON="$WORKLOAD_RESULTS_DIR/unitrace_manifest.json"
}

parse_global_options() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --workload)
        [[ $# -ge 2 ]] || die "--workload requires a name"
        WORKLOAD=$2
        shift 2
        ;;
      --debug)
        [[ $# -ge 2 ]] || die "--debug requires g, gline, or none"
        DEBUG_MODE=$2
        shift 2
        ;;
      --opt)
        [[ $# -ge 2 ]] || die "--opt requires O0, O1, or O2"
        OPT_LEVEL=$2
        shift 2
        ;;
      --device-code)
        [[ $# -ge 2 ]] || die "--device-code requires jit or aot"
        DEVICE_CODE=$2
        shift 2
        ;;
      --device-target)
        [[ $# -ge 2 ]] || die "--device-target requires a comma-separated list of bmg and/or pvc"
        DEVICE_TARGET=$2
        shift 2
        ;;
      --artifact-dir)
        [[ $# -ge 2 ]] || die "--artifact-dir requires a path"
        ARTIFACT_DIR=$2
        shift 2
        ;;
      --build-dir)
        [[ $# -ge 2 ]] || die "--build-dir requires a path"
        BUILD_DIR=$2
        shift 2
        ;;
      --tool)
        [[ $# -ge 2 ]] || die "--tool requires vtune, unitrace, or both"
        TOOL=$2
        shift 2
        ;;
      --clean-results)
        CLEAN_RESULTS=true
        shift
        ;;
      --help|-h|help)
        usage
        exit 0
        ;;
      --*)
        die "unknown global option: $1"
        ;;
      *)
        break
        ;;
    esac
  done

  validate_debug_mode
  validate_opt_level
  validate_device_code
  validate_device_target
  validate_tool
  refresh_derived_paths
  PARSED_ARGUMENTS=("$@")
}

adapter_build() {
  local adapters_requested=
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --adapter)
        [[ $# -ge 2 ]] || die "adapter build --adapter requires a name"
        adapters_requested="${adapters_requested:+$adapters_requested,}$2"
        shift 2
        ;;
      *)
        die "unknown adapter build option: $1"
        ;;
    esac
  done

  local enable_gimli=ON enable_iga=OFF enable_gdb=OFF enable_pti_elf_parser=OFF
  if [[ -n "$adapters_requested" ]]; then
    enable_gimli=OFF enable_iga=OFF enable_gdb=OFF enable_pti_elf_parser=OFF
    IFS=',' read -ra _adapter_list <<< "$adapters_requested"
    for _a in "${_adapter_list[@]}"; do
      case "$_a" in
        gimli-rust) enable_gimli=ON ;;
        iga)        enable_iga=ON ;;
        gdb-intel)  enable_gdb=ON ;;
        pti-elf-parser) enable_pti_elf_parser=ON ;;
        *) die "unknown adapter: $_a; expected gimli-rust, iga, gdb-intel, or pti-elf-parser" ;;
      esac
    done
  fi

  if [[ "$enable_gdb" == ON ]]; then
    require_tool "IntelGT addr2line" GDB_ADDR2LINE "$GDB_ADDR2LINE"
  fi
  if [[ "$enable_gimli" == ON ]]; then
    command -v cargo >/dev/null 2>&1 || \
      die "cargo executable not found in PATH; the gimli-rust (rust-gimli) adapter needs it to build. Install Rust (https://rustup.rs) or add ~/.cargo/bin to PATH, then rerun 'dwarf-check adapter build'"
    log "adapter build: using cargo: $(command -v cargo)"
  fi
  local cmake_args=(
    -S "$SCRIPT_DIR"
    -B "$BUILD_DIR"
    -G "$CMAKE_GENERATOR"
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DDPC_ENABLE_GIMLI_ADAPTER="$enable_gimli"
    -DDPC_ENABLE_IGA_ADAPTER="$enable_iga"
    -DDPC_ENABLE_GDB_INTEL_ADAPTER="$enable_gdb"
    -DDPC_ENABLE_PTI_ELF_PARSER_ADAPTER="$enable_pti_elf_parser"
  )
  if [[ "$enable_gdb" == ON ]]; then
    cmake_args+=(-DDPC_GDB_ADDR2LINE_EXECUTABLE="$GDB_ADDR2LINE")
  fi

  log "adapter build: configuring $BUILD_DIR"
  cmake "${cmake_args[@]}"
  log "adapter build: compiling dwarf-parser-check"
  cmake --build "$BUILD_DIR"
}

workload_build() {
  local workload_project_dir="$SCRIPT_DIR/data_generation"
  local workload_cache_file="$WORKLOAD_BUILD_DIR/CMakeCache.txt"
  if [[ ! -f "$workload_project_dir/CMakeLists.txt" ]]; then
    die "workload CMake project not found: $workload_project_dir"
  fi
  command -v icpx >/dev/null 2>&1 || \
    die "icpx executable not found in PATH; initialize the oneAPI compiler environment first (source <oneAPI install>/setvars.sh)"
  log "workload build: using icpx: $(command -v icpx)"

  log "workload build: configuring $WORKLOAD (debug=$DEBUG_MODE, opt=$OPT_LEVEL, device-code=$DEVICE_CODE, device-target=$DEVICE_TARGET)"
  mkdir -p "$ARTIFACT_DIR"
  if [[ -f "$workload_cache_file" ]]; then
    local configured_source_dir
    configured_source_dir=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$workload_cache_file")
    if [[ -n "$configured_source_dir" && "$configured_source_dir" != "$workload_project_dir" ]]; then
      log "workload build: removing stale CMake build directory from $configured_source_dir"
      rm -rf "$WORKLOAD_BUILD_DIR"
    fi
  fi
  # JIT builds ignore the AOT target (it may still be auto); drop it from the cache.
  local cmake_target_args=(-UDPC_WORKLOAD_AOT_TARGET)
  if [[ "$DEVICE_CODE" == aot ]]; then
    cmake_target_args=(-DDPC_WORKLOAD_AOT_TARGET="$DEVICE_TARGET")
  fi
  cmake -S "$workload_project_dir" -B "$WORKLOAD_BUILD_DIR" \
    -DCMAKE_CXX_COMPILER=icpx \
    -DDPC_WORKLOAD="$WORKLOAD" \
    -DDPC_WORKLOAD_DEBUG="$DEBUG_MODE" \
    -DDPC_WORKLOAD_OPT="$OPT_LEVEL" \
    -DDPC_WORKLOAD_DEVICE_CODE="$DEVICE_CODE" \
    "${cmake_target_args[@]}"
  cmake --build "$WORKLOAD_BUILD_DIR"
  log "workload build: binary: $BIN_PATH"
}

require_workload_binary() {
  if [[ ! -x "$BIN_PATH" ]]; then
    die "workload binary not found or not executable: $BIN_PATH; run 'dwarf-check workload build' first"
  fi
  local missing_libraries
  missing_libraries=$(ldd "$BIN_PATH" 2>/dev/null | awk '/not found/ { print $1 }' | paste -sd, -) || true
  if [[ -n "$missing_libraries" ]]; then
    die "workload binary $BIN_PATH cannot load shared libraries: $missing_libraries; initialize the oneAPI runtime environment first (source <oneAPI install>/setvars.sh)"
  fi
}

workload_execute() {
  require_workload_binary
  log "run: executing workload $WORKLOAD"
  mkdir -p "$(dirname "$KERNEL_DEBUG_JSON")"
  (
    cd "$ARTIFACT_DIR"
    ZE_ENABLE_TRACING_LAYER=${ZE_ENABLE_TRACING_LAYER:-1} \
      "$BIN_PATH" "$KERNEL_DEBUG_JSON"
  )
  log "run: kernel debug manifest: $KERNEL_DEBUG_JSON"
}

vtune_collect() {
  require_workload_binary
  [[ "$VTUNE_MAX_ATTEMPTS" =~ ^[1-9][0-9]*$ ]] || \
    die "VTUNE_MAX_ATTEMPTS must be a positive integer: $VTUNE_MAX_ATTEMPTS"
  require_tool VTune VTUNE_BIN "$VTUNE_BIN"
  local ptrace_scope_file=/proc/sys/kernel/yama/ptrace_scope
  if [[ -r "$ptrace_scope_file" ]] && [[ $(<"$ptrace_scope_file") != 0 ]]; then
    die "VTune profiling requires unrestricted ptrace access (kernel.yama.ptrace_scope=$(<"$ptrace_scope_file")). Run: sudo sysctl -w kernel.yama.ptrace_scope=0"
  fi

  mkdir -p "$(dirname "$KERNEL_DEBUG_JSON")"

  local vtune_cmd=(
    "$VTUNE_BIN"
    -collect gpu-hotspots
    -knob gpu-profiling-mode=source-analysis
    -knob source-analysis=stall-sampling
    -result-dir "$VTUNE_RESULT_DIR"
  )
  if [[ -n "$VTUNE_TARGET_GPU" ]]; then
    vtune_cmd+=(-knob "target-gpu=$VTUNE_TARGET_GPU")
  fi
  vtune_cmd+=(-- "$BIN_PATH" "$KERNEL_DEBUG_JSON")

  local attempt rc=0
  for ((attempt = 1; attempt <= VTUNE_MAX_ATTEMPTS; ++attempt)); do
    if [[ -e "$VTUNE_RESULT_DIR" ]]; then
      log "run: removing previous VTune result: $VTUNE_RESULT_DIR"
      rm -rf "$VTUNE_RESULT_DIR"
    fi
    log "run: collecting VTune samples into $VTUNE_RESULT_DIR (attempt $attempt/$VTUNE_MAX_ATTEMPTS)"
    log "run: command: $(format_command "${vtune_cmd[@]}")"
    rc=0
    ZE_ENABLE_TRACING_LAYER=${ZE_ENABLE_TRACING_LAYER:-1} "${vtune_cmd[@]}" || rc=$?
    if [[ $rc -eq 0 ]]; then
      log "run: VTune result: $VTUNE_RESULT_DIR"
      return 0
    fi
    log "run: VTune attempt $attempt/$VTUNE_MAX_ATTEMPTS failed: exit code $rc; tool=$VTUNE_BIN; result dir=$VTUNE_RESULT_DIR"
    log_result_dir_contents VTune "$VTUNE_RESULT_DIR"
  done
  die "VTune collection failed after $VTUNE_MAX_ATTEMPTS attempts (last exit code $rc)
  tool:       $VTUNE_BIN
  workload:   $BIN_PATH
  result dir: $VTUNE_RESULT_DIR
  command:    $(format_command "${vtune_cmd[@]}")
  hint: check GPU availability and the VTune output above"
}

# Checks that unitrace wrote a metrics_*.csv with at least one data row.
# On failure, stores the reason in UNITRACE_SAMPLE_PROBLEM.
unitrace_has_stall_samples() {
  UNITRACE_SAMPLE_PROBLEM=
  if [[ ! -d "$UNITRACE_RESULT_DIR" ]]; then
    UNITRACE_SAMPLE_PROBLEM="result dir was not created: $UNITRACE_RESULT_DIR"
    return 1
  fi
  local metrics_file
  metrics_file=$(find "$UNITRACE_RESULT_DIR" -type f -name 'metrics_*.csv' -print -quit)
  if [[ -z "$metrics_file" ]]; then
    UNITRACE_SAMPLE_PROBLEM="no metrics_*.csv file in $UNITRACE_RESULT_DIR"
    return 1
  fi
  metrics_file=$(find "$UNITRACE_RESULT_DIR" -type f -name 'metrics_*.csv' -size +1c -print -quit)
  if [[ -z "$metrics_file" ]] ||
     ! awk 'NR > 1 && NF { found = 1; exit } END { exit !found }' "$metrics_file"; then
    UNITRACE_SAMPLE_PROBLEM="metrics_*.csv in $UNITRACE_RESULT_DIR is empty or has only a header (no stall samples)"
    return 1
  fi
  if [[ -z $(find "$UNITRACE_RESULT_DIR" -type f -path '*/asm/*.asm' -print -quit) ]]; then
    UNITRACE_SAMPLE_PROBLEM="no asm/*.asm kernel dump in $UNITRACE_RESULT_DIR; unitrace 2.4 or newer is required"
    return 1
  fi
}

unitrace_paranoid_hint() {
  local paranoid_file=/proc/sys/dev/xe/observation_paranoid
  if [[ -r "$paranoid_file" ]] && [[ $(<"$paranoid_file") == 0 ]]; then
    printf 'dev.xe.observation_paranoid=0 (OK)'
  elif [[ -r "$paranoid_file" ]]; then
    printf 'dev.xe.observation_paranoid=%s but must be 0; run: sudo sysctl -w dev.xe.observation_paranoid=0' "$(<"$paranoid_file")"
  else
    printf 'ensure dev.xe.observation_paranoid=0 (sudo sysctl -w dev.xe.observation_paranoid=0)'
  fi
}

unitrace_collect() {
  require_workload_binary
  [[ "$UNITRACE_MAX_ATTEMPTS" =~ ^[1-9][0-9]*$ ]] || \
    die "UNITRACE_MAX_ATTEMPTS must be a positive integer: $UNITRACE_MAX_ATTEMPTS"
  require_tool unitrace UNITRACE_BIN "$UNITRACE_BIN" "$DEFAULT_UNITRACE_BIN"
  mkdir -p "$(dirname "$KERNEL_DEBUG_JSON")"

  local unitrace_cmd=(
    "$UNITRACE_BIN" --stall-sampling --sampling-interval "$UNITRACE_SAMPLING_INTERVAL"
    --result-dir "$UNITRACE_RESULT_DIR"
    "$BIN_PATH" "$KERNEL_DEBUG_JSON"
  )
  local unitrace_env=(ZE_ENABLE_TRACING_LAYER="${ZE_ENABLE_TRACING_LAYER:-1}")

  local attempt rc=0 problem=
  for ((attempt = 1; attempt <= UNITRACE_MAX_ATTEMPTS; ++attempt)); do
    if [[ -e "$UNITRACE_RESULT_DIR" ]]; then
      log "run: removing previous unitrace result: $UNITRACE_RESULT_DIR"
      rm -rf "$UNITRACE_RESULT_DIR"
    fi
    log "run: collecting unitrace stall samples into $UNITRACE_RESULT_DIR (attempt $attempt/$UNITRACE_MAX_ATTEMPTS)"
    log "run: command: $(format_command env "${unitrace_env[@]}" "${unitrace_cmd[@]}")"
    rc=0
    env "${unitrace_env[@]}" "${unitrace_cmd[@]}" || rc=$?
    if [[ $rc -ne 0 ]]; then
      problem="unitrace exited with code $rc"
    elif unitrace_has_stall_samples; then
      log "run: unitrace result: $UNITRACE_RESULT_DIR"
      return 0
    else
      problem=$UNITRACE_SAMPLE_PROBLEM
    fi
    log "run: unitrace attempt $attempt/$UNITRACE_MAX_ATTEMPTS failed: $problem; tool=$UNITRACE_BIN"
    log_result_dir_contents unitrace "$UNITRACE_RESULT_DIR"
  done
  die "Unitrace stall sampling failed after $UNITRACE_MAX_ATTEMPTS attempts: $problem
  tool:        $UNITRACE_BIN
  workload:    $BIN_PATH
  result dir:  $UNITRACE_RESULT_DIR
  command:     $(format_command env "${unitrace_env[@]}" "${unitrace_cmd[@]}")
  hint: $(unitrace_paranoid_hint); also check GPU availability and the unitrace output above"
}

collect_selected_tools() {
  if tool_selected vtune; then
    vtune_collect
  fi
  if tool_selected unitrace; then
    unitrace_collect
  fi
}

clean_workload_results() {
  if [[ "$CLEAN_RESULTS" != true ]]; then
    return 0
  fi
  if [[ -z "$WORKLOAD_RESULTS_DIR" || "$WORKLOAD_RESULTS_DIR" == / || "$WORKLOAD_RESULTS_DIR" == . ]]; then
    die "refusing to clean unsafe results directory: $WORKLOAD_RESULTS_DIR"
  fi
  if [[ -e "$WORKLOAD_RESULTS_DIR" ]]; then
    log "cleaning previous results: $WORKLOAD_RESULTS_DIR"
    rm -rf "$WORKLOAD_RESULTS_DIR"
  fi
}

generate_vtune_reference() {
  if [[ ! -d "$VTUNE_RESULT_DIR" ]]; then
    die "VTune result directory not found: $VTUNE_RESULT_DIR; run 'dwarf-check run' first"
  fi
  require_tool VTune VTUNE_BIN "$VTUNE_BIN"

  mkdir -p "$WORKLOAD_RESULTS_DIR"
  local vtune_report_args=(
    -report hotspots
    -result-dir "$VTUNE_RESULT_DIR"
    -group-by address
    -column Address
    -column "Source Line"
    -column Assembly
    -column "Stall Count"
    -format csv
    -csv-delimiter comma
    -report-width 0
  )
  if [[ -n "$VTUNE_COMPUTING_TASK" ]]; then
    vtune_report_args+=(
      -source-object "computing-task=$VTUNE_COMPUTING_TASK"
      -filter "computing-task=$VTUNE_COMPUTING_TASK"
      -report-output "$WORKLOAD_RESULTS_DIR/result.csv"
    )
    "$VTUNE_BIN" "${vtune_report_args[@]}"
  else
    rm -f "$WORKLOAD_RESULTS_DIR"/result_*.csv
    while IFS=$'\t' read -r kernel_name computing_task; do
      local kernel_report_args=("${vtune_report_args[@]}")
      kernel_report_args+=(
        -source-object "computing-task=$computing_task"
        -filter "computing-task=$computing_task"
        -report-output "$WORKLOAD_RESULTS_DIR/result_${kernel_name}.csv"
      )
      "$VTUNE_BIN" "${kernel_report_args[@]}"
    done < <(python3 - "$KERNEL_DEBUG_JSON" <<'PY'
import json
import sys

for kernel in json.load(open(sys.argv[1], encoding="utf-8"))["kernels"]:
  demangled_name = kernel["demangled_name"]
  function_name, delimiter, template_arguments = demangled_name.partition("<")
  computing_task = function_name.rsplit("::", 1)[-1] + delimiter + template_arguments
  print(f'{kernel["name"]}\t{computing_task}')
PY
)
  fi

  python3 "$SCRIPT_DIR/data_generation/extract_addr_srcline.py" \
    "$WORKLOAD_RESULTS_DIR"

  log "analyze: VTune reference CSVs written to $WORKLOAD_RESULTS_DIR"
  log "analyze: manifest: $VTUNE_MANIFEST_JSON"
}

generate_unitrace_reference() {
  if [[ ! -d "$UNITRACE_RESULT_DIR" ]]; then
    die "unitrace result directory not found: $UNITRACE_RESULT_DIR; run 'dwarf-check run' first"
  fi

  python3 "$SCRIPT_DIR/data_generation/extract_unitrace_stalls.py" \
    --result-dir "$UNITRACE_RESULT_DIR" \
    --kernel-debug-json "$KERNEL_DEBUG_JSON" \
    --output-dir "$WORKLOAD_RESULTS_DIR" \
    --manifest-output "$UNITRACE_MANIFEST_JSON"
  log "analyze: unitrace reference CSVs written to $WORKLOAD_RESULTS_DIR"
  log "analyze: manifest: $UNITRACE_MANIFEST_JSON"
}

# Fails early when a requested adapter was compiled out of dwarf-parser-check.
require_compiled_adapters() {
  local cache_file="$BUILD_DIR/CMakeCache.txt"
  [[ -f "$cache_file" ]] || return 0
  local adapter
  IFS=',' read -ra _requested_adapters <<< "$ADAPTERS"
  for adapter in "${_requested_adapters[@]}"; do
    if [[ "$adapter" == rust-gimli ]] &&
       grep -q '^CARGO_EXECUTABLE:FILEPATH=.*NOTFOUND' "$cache_file"; then
      die "adapter rust-gimli is not compiled into $BUILD_DIR/dwarf-parser-check: cargo was not found when the adapter was configured (CARGO_EXECUTABLE-NOTFOUND in $cache_file). Add cargo to PATH and run 'dwarf-check adapter build', or pick other adapters with --adapters"
    fi
  done
}

adapter_run() {
  if [[ ! -x "$BUILD_DIR/dwarf-parser-check" ]]; then
    adapter_build
  fi
  require_compiled_adapters
  if [[ ! -f "$KERNEL_DEBUG_JSON" ]]; then
    if [[ "$KERNEL_DEBUG_JSON" != "$DEFAULT_KERNEL_DEBUG_JSON" ]]; then
      die "kernel debug manifest not found: $KERNEL_DEBUG_JSON"
    fi
    workload_build
    workload_execute
  fi

  local output_dir=${ADAPTER_OUTPUT_DIR:-"$WORKLOAD_RESULTS_DIR"}
  local sample_manifest=${SAMPLE_MANIFEST:-}
  local comparison_label=${COMPARISON_LABEL:-vtune}
  mkdir -p "$output_dir"
  local resolver_args=(
    --kernel-debug-json "$KERNEL_DEBUG_JSON"
    --adapters "$ADAPTERS"
    --output-dir "$output_dir"
    --comparison-label "$comparison_label"
  )
  if [[ -n "$sample_manifest" ]]; then
    [[ -f "$sample_manifest" ]] || die "sample manifest not found: $sample_manifest"
    resolver_args+=(--vtune-manifest "$sample_manifest")
  elif [[ -f "$VTUNE_MANIFEST_JSON" ]]; then
    resolver_args+=(--vtune-manifest "$VTUNE_MANIFEST_JSON")
  elif [[ -n ${REFERENCE_FILE:-} ]]; then
    [[ -f "$REFERENCE_FILE" ]] || die "reference file not found: $REFERENCE_FILE"
    resolver_args+=(--reference "$REFERENCE_FILE")
  fi

  local resolver_cmd=("$BUILD_DIR/dwarf-parser-check" "${resolver_args[@]}" "$@")
  local rc=0
  DPC_GDB_ADDR2LINE="$GDB_ADDR2LINE" "${resolver_cmd[@]}" >/dev/null || rc=$?
  if [[ $rc -ne 0 ]]; then
    die "adapter run ($comparison_label) failed with exit code $rc (see the dwarf-parser-check error above; 'unknown adapter selection' means the adapter was not compiled in, so rebuild with 'dwarf-check adapter build')
  tool:       $BUILD_DIR/dwarf-parser-check
  adapters:   $ADAPTERS
  output dir: $output_dir
  command:    $(format_command "${resolver_cmd[@]}")"
  fi
  log "adapter run: per-adapter reports saved to $output_dir"
}

print_csv_table() {
  if command -v column >/dev/null 2>&1; then
    column -s, -t "$1" | sed 's/^/  /'
  else
    sed 's/^/  /' "$1"
  fi
}

# Compares VTune and unitrace source locations per instruction, for every kernel
# present in both manifests. The primary report uses the source locations unitrace
# itself reports in metrics_*.csv (falling back to its asm dump's source table);
# the _dwarf report uses unitrace IPs mapped through the zebin DWARF line table.
compare_vtune_unitrace() {
  local comparison_dir="$WORKLOAD_RESULTS_DIR/vtune_vs_unitrace"
  [[ -f "$VTUNE_MANIFEST_JSON" ]] || die "VTune manifest not found: $VTUNE_MANIFEST_JSON"
  [[ -f "$UNITRACE_MANIFEST_JSON" ]] || die "unitrace manifest not found: $UNITRACE_MANIFEST_JSON"
  mkdir -p "$comparison_dir"

  local compared=0 kernel assembly vtune_reference vtune_sampled_ips marker_reference dwarf_reference
  while IFS=$'\t' read -r kernel assembly vtune_reference vtune_sampled_ips marker_reference dwarf_reference; do
    local safe_kernel=${kernel//[^a-zA-Z0-9_-]/_}
    log "compare: $kernel"
    python3 "$SCRIPT_DIR/data_generation/compare_assembly_locations.py" \
      "$assembly" "$vtune_reference" "$marker_reference" \
      --vtune-sampled-ips "$vtune_sampled_ips" \
      --output "$comparison_dir/vtune_vs_unitrace_${safe_kernel}.csv"
    python3 "$SCRIPT_DIR/data_generation/compare_assembly_locations.py" \
      "$assembly" "$vtune_reference" "$dwarf_reference" \
      --vtune-sampled-ips "$vtune_sampled_ips" \
      --output "$comparison_dir/vtune_vs_unitrace_dwarf_${safe_kernel}.csv"
    log "compare: unitrace-reported locations vs VTune summary:"
    print_csv_table "$comparison_dir/vtune_vs_unitrace_${safe_kernel}_summary.csv"
    log "compare: unitrace IPs via DWARF vs VTune summary:"
    print_csv_table "$comparison_dir/vtune_vs_unitrace_dwarf_${safe_kernel}_summary.csv"
    compared=$((compared + 1))
  done < <(python3 - "$VTUNE_MANIFEST_JSON" "$UNITRACE_MANIFEST_JSON" <<'PY'
import json
import sys

vtune = {kernel["name"]: kernel for kernel in json.load(open(sys.argv[1], encoding="utf-8"))["kernels"]}
for kernel in json.load(open(sys.argv[2], encoding="utf-8"))["kernels"]:
  vtune_kernel = vtune.get(kernel["name"])
  if vtune_kernel is None:
    print(f"compare: kernel {kernel['name']} has unitrace samples but no VTune samples; skipping", file=sys.stderr)
    continue
  if "sampled_ip_list" not in vtune_kernel:
    sys.exit(f"compare: VTune manifest has no stall-sampled IP list for {kernel['name']}; re-run 'dwarf-check analyze'")
  print("\t".join((
    kernel["name"],
    kernel["addressed_shader_dump"],
    vtune_kernel["reference_csv"],
    vtune_kernel["sampled_ip_list"],
    kernel.get("native_reference_csv") or kernel["marker_reference_csv"],
    kernel["reference_csv"],
  )))
PY
)
  [[ $compared -gt 0 ]] || \
    die "no kernels are present in both $VTUNE_MANIFEST_JSON and $UNITRACE_MANIFEST_JSON"
  log "compare: VTune vs unitrace reports written to $comparison_dir"
}

analyze_selected_tools() {
  if tool_selected vtune; then
    generate_vtune_reference
    if [[ "$ADAPTERS" != none ]]; then
      REFERENCE_FILE=$VTUNE_REFERENCE_CSV \
        COMPARISON_LABEL=vtune \
        ADAPTER_OUTPUT_DIR="$WORKLOAD_RESULTS_DIR" \
        adapter_run
    fi
  fi
  if tool_selected unitrace; then
    generate_unitrace_reference
    if [[ "$ADAPTERS" != none ]]; then
      SAMPLE_MANIFEST=$UNITRACE_MANIFEST_JSON \
        COMPARISON_LABEL=unitrace \
        ADAPTER_OUTPUT_DIR="$WORKLOAD_RESULTS_DIR/unitrace" \
        adapter_run
    fi
  fi
  if [[ "$TOOL" == both ]]; then
    compare_vtune_unitrace
  fi
}

run_command() {
  local result_dir="$VTUNE_RESULT_DIR"
  local unitrace_result_dir="$UNITRACE_RESULT_DIR"
  local kernel_debug_json="$KERNEL_DEBUG_JSON"
  local target_gpu="$VTUNE_TARGET_GPU"
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --kernel-debug-json)
        [[ $# -ge 2 ]] || die "run --kernel-debug-json requires a path"
        kernel_debug_json=$2
        shift 2
        ;;
      --target-gpu)
        [[ $# -ge 2 ]] || die "run --target-gpu requires PCI IDs"
        target_gpu=$2
        shift 2
        ;;
      --result-dir)
        [[ $# -ge 2 ]] || die "run --result-dir requires a path"
        result_dir=$2
        shift 2
        ;;
      --unitrace-result-dir)
        [[ $# -ge 2 ]] || die "run --unitrace-result-dir requires a path"
        unitrace_result_dir=$2
        shift 2
        ;;
      *)
        break
        ;;
    esac
  done
  if [[ $# -gt 0 ]] && [[ $1 == --* ]]; then
    die "unknown run option: $1"
  fi
  KERNEL_DEBUG_JSON=$kernel_debug_json
  VTUNE_RESULT_DIR=$result_dir
  UNITRACE_RESULT_DIR=$unitrace_result_dir
  VTUNE_TARGET_GPU=$target_gpu
  clean_workload_results
  collect_selected_tools
  REMAINING_ARGUMENTS=("$@")
}

analyze_command() {
  local result_dir="$VTUNE_RESULT_DIR"
  local unitrace_result_dir="$UNITRACE_RESULT_DIR"
  local reference_csv="$VTUNE_REFERENCE_CSV"
  local adapters="$ADAPTERS"
  local kernel_debug_json="$KERNEL_DEBUG_JSON"
  local output_dir="$WORKLOAD_RESULTS_DIR"
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --result-dir)
        [[ $# -ge 2 ]] || die "analyze --result-dir requires a path"
        result_dir=$2
        shift 2
        ;;
      --reference-csv)
        [[ $# -ge 2 ]] || die "analyze --reference-csv requires a path"
        reference_csv=$2
        shift 2
        ;;
      --unitrace-result-dir)
        [[ $# -ge 2 ]] || die "analyze --unitrace-result-dir requires a path"
        unitrace_result_dir=$2
        shift 2
        ;;
      --adapters)
        [[ $# -ge 2 ]] || die "analyze --adapters requires a list"
        adapters=$2
        shift 2
        ;;
      --kernel-debug-json)
        [[ $# -ge 2 ]] || die "analyze --kernel-debug-json requires a path"
        kernel_debug_json=$2
        shift 2
        ;;
      --output-dir)
        [[ $# -ge 2 ]] || die "analyze --output-dir requires a path"
        output_dir=$2
        shift 2
        ;;
      *)
        break
        ;;
    esac
  done
  if [[ $# -gt 0 ]] && [[ $1 == --* ]]; then
    die "unknown analyze option: $1"
  fi
  VTUNE_RESULT_DIR=$result_dir
  UNITRACE_RESULT_DIR=$unitrace_result_dir
  VTUNE_REFERENCE_CSV=$reference_csv
  KERNEL_DEBUG_JSON=$kernel_debug_json
  WORKLOAD_RESULTS_DIR=$output_dir
  VTUNE_MANIFEST_JSON="$WORKLOAD_RESULTS_DIR/vtune_manifest.json"
  UNITRACE_MANIFEST_JSON="$WORKLOAD_RESULTS_DIR/unitrace_manifest.json"
  ADAPTERS=$adapters

  [[ -f "$KERNEL_DEBUG_JSON" ]] || die "kernel debug manifest not found: $KERNEL_DEBUG_JSON; run 'dwarf-check run' first"
  analyze_selected_tools
  REMAINING_ARGUMENTS=("$@")
}

all_command() {
  local adapters="$ADAPTERS"
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --adapters)
        [[ $# -ge 2 ]] || die "all --adapters requires a list"
        adapters=$2
        shift 2
        ;;
      *)
        die "unknown all option: $1"
        ;;
    esac
  done
  clean_workload_results
  workload_build
  collect_selected_tools
  ADAPTERS=$adapters analyze_selected_tools
}

test_project() {
  adapter_build
  log "test: running CTest"
  ctest --test-dir "$BUILD_DIR" --output-on-failure
  log "test: running Rust adapter tests"
  cargo test --manifest-path "$SCRIPT_DIR/src/adapters/gimli/Cargo.toml" --tests
}

parse_global_options "$@"
set -- "${PARSED_ARGUMENTS[@]}"
[[ $# -gt 0 ]] || die "a command is required; run 'dwarf-check --help' for usage"

while [[ $# -gt 0 ]]; do
  command=$1
  CURRENT_COMMAND=$command
  shift
  case "$command" in
    adapter)
      [[ $# -gt 0 ]] || die "adapter requires a subcommand"
      adapter_command=$1
      shift
      case "$adapter_command" in
        build)
          adapter_build "$@"
          set --
          ;;
        clean)
          rm -rf "$BUILD_DIR"
          ;;
        run)
          adapter_run "$@"
          set --
          ;;
        *)
          die "unknown adapter subcommand: $adapter_command"
          ;;
      esac
      ;;
    workload)
      [[ $# -gt 0 ]] || die "workload requires a subcommand"
      workload_command=$1
      shift
      case "$workload_command" in
        build)
          workload_build
          ;;
        *)
          die "unknown workload subcommand: $workload_command"
          ;;
      esac
      ;;
    build)
      workload_build
      adapter_build
      ;;
    device-target)
      if [[ "$DEVICE_TARGET" == auto ]]; then
        DEVICE_TARGET=$(detect_device_target) || exit 1
      fi
      printf '%s\n' "$DEVICE_TARGET"
      ;;
    run)
      run_command "$@"
      set -- "${REMAINING_ARGUMENTS[@]}"
      ;;
    analyze)
      analyze_command "$@"
      set -- "${REMAINING_ARGUMENTS[@]}"
      ;;
    all)
      all_command "$@"
      set --
      ;;
    test)
      test_project
      ;;
    --help|-h|help)
      usage
      exit 0
      ;;
    *)
      die "unknown command: $command"
      ;;
  esac
done