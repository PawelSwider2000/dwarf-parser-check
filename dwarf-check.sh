#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

DEBUG_MODE=${DEBUG_MODE:-g}
OPT_LEVEL=${OPT_LEVEL:-O0}
DEVICE_CODE=${DEVICE_CODE:-jit}
DEVICE_TARGET=${DEVICE_TARGET:-bmg,pvc}
ARTIFACT_DIR=${ARTIFACT_DIR:-"$SCRIPT_DIR/artifacts"}
WORKLOAD=${WORKLOAD:-gemm}
BUILD_DIR=${BUILD_DIR:-"$SCRIPT_DIR/build"}
BUILD_TYPE=${BUILD_TYPE:-Debug}
CMAKE_GENERATOR=${CMAKE_GENERATOR:-Ninja}
GDB_ADDR2LINE=${GDB_ADDR2LINE:-"$SCRIPT_DIR/../applications.debuggers.gdb-build-intelgt/binutils/addr2line"}
VTUNE_BIN=${VTUNE_BIN:-vtune}
VTUNE_TARGET_GPU=${VTUNE_TARGET_GPU:-}
VTUNE_COMPUTING_TASK=${VTUNE_COMPUTING_TASK:-}
UNITRACE_BIN=${UNITRACE_BIN:-unitrace}
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
UNITRACE_SHADER_DUMP_DIR=${UNITRACE_SHADER_DUMP_DIR:-}

log() {
  printf '[dwarf-check] %s\n' "$*"
}

die() {
  printf 'dwarf-check: %s\n' "$*" >&2
  exit 1
}

usage() {
  cat <<EOF
Usage: $(basename "$0") [global-options] <command> [command-options] ...

Global options must appear before the first command:
  --workload NAME       Workload under data_generation/workloads (default: gemm)
  --debug MODE          Device debug information: g, gline, or none (default: g)
  --opt LEVEL           Workload optimization: O0, O1, or O2 (default: O0)
  --device-code MODE    Device compilation: jit or aot (default: jit)
  --device-target LIST  Comma-separated AOT targets: bmg, pvc (default: bmg,pvc)
  --artifact-dir PATH   Workload artifact root (default: artifacts)
  --build-dir PATH      dwarf-parser-check CMake build directory (default: build)
  --tool NAME           Collector to use: vtune, unitrace, or both (default: both)
  --clean-results       Remove the selected results directory before run or all
  --help, -h            Show this help text

Commands:
  adapter build         Configure and build dwarf-parser-check.
    --adapter NAME      Build only the named adapter(s) (gimli-rust, iga, gdb-intel);
                        may be repeated; omit to build all.
  workload build        Build the selected SYCL workload.
  build                 Build the workload and dwarf-parser-check.
  run                   Run the workload, write its manifest, and collect selected samples.
  analyze               Generate selected references and run resolver adapters.
  all                   Build workload, collect selected samples, and analyze with all adapters.
    --adapters LIST     Comma-separated adapter names, or all (default: all).
  test                  Build the adapter, run CTest, then run Rust adapter tests.

Run options:
  --kernel-debug-json PATH  Workload manifest output path.
  --target-gpu PCI_IDS      VTune GPU adapter IDs.
  --result-dir PATH         VTune result directory.
  --unitrace-result-dir PATH  Unitrace result directory.
  --unitrace-shader-dump-dir PATH  IGC shader dump directory for unitrace.

Analyze options:
  --result-dir PATH         Existing VTune result directory.
  --unitrace-result-dir PATH  Existing unitrace result directory.
  --reference-csv PATH      VTune address-to-source CSV output path.
  --adapters LIST           Comma-separated adapter names, or all.
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

validate_device_target() {
  local target
  local -a targets
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
  if [[ -z "$UNITRACE_SHADER_DUMP_DIR" ]]; then
    UNITRACE_SHADER_DUMP_DIR="$WORKLOAD_RESULTS_DIR/unitrace_shader_dump"
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

  local enable_gimli=ON enable_iga=OFF enable_gdb=OFF
  if [[ -n "$adapters_requested" ]]; then
    enable_gimli=OFF enable_iga=OFF enable_gdb=OFF
    IFS=',' read -ra _adapter_list <<< "$adapters_requested"
    for _a in "${_adapter_list[@]}"; do
      case "$_a" in
        gimli-rust) enable_gimli=ON ;;
        iga)        enable_iga=ON ;;
        gdb-intel)  enable_gdb=ON ;;
        *) die "unknown adapter: $_a; expected gimli-rust, iga, or gdb-intel" ;;
      esac
    done
  fi

  if [[ "$enable_gdb" == ON ]] && [[ ! -x "$GDB_ADDR2LINE" ]]; then
    die "IntelGT addr2line executable not found: $GDB_ADDR2LINE"
  fi

  local cmake_args=(
    -S "$SCRIPT_DIR"
    -B "$BUILD_DIR"
    -G "$CMAKE_GENERATOR"
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DDPC_ENABLE_GIMLI_ADAPTER="$enable_gimli"
    -DDPC_ENABLE_IGA_ADAPTER="$enable_iga"
    -DDPC_ENABLE_GDB_INTEL_ADAPTER="$enable_gdb"
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
  if ! command -v icpx >/dev/null 2>&1; then
    die "icpx was not found in PATH; initialize the oneAPI compiler environment first"
  fi

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
  cmake -S "$workload_project_dir" -B "$WORKLOAD_BUILD_DIR" \
    -DCMAKE_CXX_COMPILER=icpx \
    -DDPC_WORKLOAD="$WORKLOAD" \
    -DDPC_WORKLOAD_DEBUG="$DEBUG_MODE" \
    -DDPC_WORKLOAD_OPT="$OPT_LEVEL" \
    -DDPC_WORKLOAD_DEVICE_CODE="$DEVICE_CODE" \
    -DDPC_WORKLOAD_AOT_TARGET="$DEVICE_TARGET"
  if tool_selected unitrace && [[ "$DEVICE_CODE" == aot ]]; then
    rm -rf "$UNITRACE_SHADER_DUMP_DIR"
    mkdir -p "$UNITRACE_SHADER_DUMP_DIR"
    log "workload build: writing AOT unitrace shader dump to $UNITRACE_SHADER_DUMP_DIR"
    IGC_ShaderDumpEnable=1 IGC_DumpToCustomDir="$UNITRACE_SHADER_DUMP_DIR" \
      cmake --build "$WORKLOAD_BUILD_DIR" --clean-first
  else
    cmake --build "$WORKLOAD_BUILD_DIR"
  fi
  log "workload build: binary: $BIN_PATH"
}

require_workload_binary() {
  if [[ ! -x "$BIN_PATH" ]]; then
    die "workload binary not found or not executable: $BIN_PATH; run 'dwarf-check workload build' first"
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
  local ptrace_scope_file=/proc/sys/kernel/yama/ptrace_scope
  if [[ -r "$ptrace_scope_file" ]] && [[ $(<"$ptrace_scope_file") != 0 ]]; then
    die "VTune profiling requires unrestricted ptrace access. Run: sudo sysctl -w kernel.yama.ptrace_scope=0"
  fi

  if [[ -e "$VTUNE_RESULT_DIR" ]]; then
    log "run: removing previous VTune result: $VTUNE_RESULT_DIR"
    rm -rf "$VTUNE_RESULT_DIR"
  fi
  mkdir -p "$(dirname "$KERNEL_DEBUG_JSON")"

  local vtune_args=(
    -collect gpu-hotspots
    -knob gpu-profiling-mode=source-analysis
    -knob source-analysis=stall-sampling
    -result-dir "$VTUNE_RESULT_DIR"
  )
  if [[ -n "$VTUNE_TARGET_GPU" ]]; then
    vtune_args+=(-knob "target-gpu=$VTUNE_TARGET_GPU")
  fi
  vtune_args+=(-- "$BIN_PATH" "$KERNEL_DEBUG_JSON")

  log "run: collecting VTune samples into $VTUNE_RESULT_DIR"
  ZE_ENABLE_TRACING_LAYER=${ZE_ENABLE_TRACING_LAYER:-1} "$VTUNE_BIN" "${vtune_args[@]}"
  log "run: VTune result: $VTUNE_RESULT_DIR"
}

unitrace_collect() {
  require_workload_binary
  if [[ -e "$UNITRACE_RESULT_DIR" ]]; then
    log "run: removing previous unitrace result: $UNITRACE_RESULT_DIR"
    rm -rf "$UNITRACE_RESULT_DIR"
  fi
  if [[ "$DEVICE_CODE" == jit ]] && [[ -e "$UNITRACE_SHADER_DUMP_DIR" ]]; then
    log "run: removing previous unitrace shader dump: $UNITRACE_SHADER_DUMP_DIR"
    rm -rf "$UNITRACE_SHADER_DUMP_DIR"
  fi
  mkdir -p "$(dirname "$KERNEL_DEBUG_JSON")"
  if [[ "$DEVICE_CODE" == jit ]]; then
    mkdir -p "$UNITRACE_SHADER_DUMP_DIR"
  elif [[ ! -d "$UNITRACE_SHADER_DUMP_DIR" ]] ||
       [[ -z $(find "$UNITRACE_SHADER_DUMP_DIR" -maxdepth 1 -name '*.asm' -print -quit) ]]; then
    die "AOT unitrace collection requires an IGC shader dump; run 'dwarf-check build' with --tool unitrace first"
  fi

  log "run: collecting unitrace stall samples into $UNITRACE_RESULT_DIR"
  if [[ "$DEVICE_CODE" == jit ]]; then
    log "run: writing unitrace shader dump to $UNITRACE_SHADER_DUMP_DIR"
    ZE_ENABLE_TRACING_LAYER=${ZE_ENABLE_TRACING_LAYER:-1} \
      IGC_ShaderDumpEnable=1 \
      IGC_DumpToCustomDir="$UNITRACE_SHADER_DUMP_DIR" \
      "$UNITRACE_BIN" --stall-sampling --result-dir "$UNITRACE_RESULT_DIR" \
      "$BIN_PATH" "$KERNEL_DEBUG_JSON"
  else
    log "run: using AOT unitrace shader dump from $UNITRACE_SHADER_DUMP_DIR"
    ZE_ENABLE_TRACING_LAYER=${ZE_ENABLE_TRACING_LAYER:-1} \
      "$UNITRACE_BIN" --stall-sampling --result-dir "$UNITRACE_RESULT_DIR" \
      "$BIN_PATH" "$KERNEL_DEBUG_JSON"
  fi
  log "run: unitrace result: $UNITRACE_RESULT_DIR"
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

  mkdir -p "$WORKLOAD_RESULTS_DIR"
  local vtune_report_args=(
    -report hotspots
    -result-dir "$VTUNE_RESULT_DIR"
    -group-by address
    -column Address
    -column "Source Line"
    -format csv
    -csv-delimiter comma
    -report-width 0
  )
  if [[ -n "$VTUNE_COMPUTING_TASK" ]]; then
    vtune_report_args+=(
      -source-object "computing-task=$VTUNE_COMPUTING_TASK"
      -report-output "$WORKLOAD_RESULTS_DIR/result.csv"
    )
    "$VTUNE_BIN" "${vtune_report_args[@]}"
  else
    rm -f "$WORKLOAD_RESULTS_DIR"/result_*.csv
    while IFS=$'\t' read -r kernel_name computing_task; do
      local kernel_report_args=("${vtune_report_args[@]}")
      kernel_report_args+=(
        -source-object "computing-task=$computing_task"
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
    --manifest-output "$UNITRACE_MANIFEST_JSON" \
    --shader-dump-dir "$UNITRACE_SHADER_DUMP_DIR"
  log "analyze: unitrace reference CSVs written to $WORKLOAD_RESULTS_DIR"
  log "analyze: manifest: $UNITRACE_MANIFEST_JSON"
}

adapter_run() {
  if [[ ! -x "$BUILD_DIR/dwarf-parser-check" ]]; then
    adapter_build
  fi
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

  DPC_GDB_ADDR2LINE="$GDB_ADDR2LINE" \
    "$BUILD_DIR/dwarf-parser-check" "${resolver_args[@]}" "$@" >/dev/null
  log "adapter run: per-adapter reports saved to $output_dir"
}

analyze_selected_tools() {
  if tool_selected vtune; then
    generate_vtune_reference
    REFERENCE_FILE=$VTUNE_REFERENCE_CSV \
      COMPARISON_LABEL=vtune \
      ADAPTER_OUTPUT_DIR="$WORKLOAD_RESULTS_DIR" \
      adapter_run
  fi
  if tool_selected unitrace; then
    generate_unitrace_reference
    SAMPLE_MANIFEST=$UNITRACE_MANIFEST_JSON \
      COMPARISON_LABEL=unitrace \
      ADAPTER_OUTPUT_DIR="$WORKLOAD_RESULTS_DIR/unitrace" \
      adapter_run
  fi
}

run_command() {
  local result_dir="$VTUNE_RESULT_DIR"
  local unitrace_result_dir="$UNITRACE_RESULT_DIR"
  local unitrace_shader_dump_dir="$UNITRACE_SHADER_DUMP_DIR"
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
      --unitrace-shader-dump-dir)
        [[ $# -ge 2 ]] || die "run --unitrace-shader-dump-dir requires a path"
        unitrace_shader_dump_dir=$2
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
  UNITRACE_SHADER_DUMP_DIR=$unitrace_shader_dump_dir
  VTUNE_TARGET_GPU=$target_gpu
  clean_workload_results
  collect_selected_tools
  REMAINING_ARGUMENTS=("$@")
}

analyze_command() {
  local result_dir="$VTUNE_RESULT_DIR"
  local unitrace_result_dir="$UNITRACE_RESULT_DIR"
  local unitrace_shader_dump_dir="$UNITRACE_SHADER_DUMP_DIR"
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
      --unitrace-shader-dump-dir)
        [[ $# -ge 2 ]] || die "analyze --unitrace-shader-dump-dir requires a path"
        unitrace_shader_dump_dir=$2
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
  UNITRACE_SHADER_DUMP_DIR=$unitrace_shader_dump_dir
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