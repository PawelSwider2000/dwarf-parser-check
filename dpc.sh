#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BUILD_DIR=${BUILD_DIR:-"$SCRIPT_DIR/build"}
BUILD_TYPE=${BUILD_TYPE:-Debug}
CMAKE_GENERATOR=${CMAKE_GENERATOR:-Ninja}
DEFAULT_KERNEL_DEBUG_JSON="$SCRIPT_DIR/artifacts/simple_sycl_vtune_kernel_debug.json"
KERNEL_DEBUG_JSON=${KERNEL_DEBUG_JSON:-"$DEFAULT_KERNEL_DEBUG_JSON"}
OUTPUT_DIR=${OUTPUT_DIR:-"$SCRIPT_DIR/artifacts"}
REFERENCE_FILE=${REFERENCE_FILE:-"$SCRIPT_DIR/artifacts/result_vtune_reference.csv"}
ADAPTERS=${ADAPTERS:-all}
IGA_PLATFORM=${IGA_PLATFORM:-0x02000000}
GDB_ADDR2LINE=${GDB_ADDR2LINE:-"$SCRIPT_DIR/../applications.debuggers.gdb-build-intelgt/binutils/addr2line"}

log() {
  printf '[dpc] %s\n' "$*"
}

usage() {
  cat <<EOF
Usage: $(basename "$0") <command> [options]

Commands:
  clean       Remove the CMake build directory.
  build       Configure and build dwarf-parser-check.
  test        Build and run the CTest suite.
  run         Build and run dwarf-parser-check for every kernel in the manifest.
  all         Clean, build, test, then run dwarf-parser-check.

Environment:
  BUILD_DIR          CMake build directory (default: build)
  BUILD_TYPE         CMake build type (default: Debug)
  CMAKE_GENERATOR    CMake generator (default: Ninja)
  KERNEL_DEBUG_JSON  Kernel debug manifest for run (default: artifacts/simple_sycl_vtune_kernel_debug.json)
  OUTPUT_DIR         Directory for per-adapter CSV and JSON reports (default: artifacts)
  REFERENCE_FILE     Optional VTune source_locations.json or address-report CSV
  ADAPTERS           Adapter selection for run (default: all)
  IGA_PLATFORM       IGA target platform passed to the IGA adapter (default: 0x02000000, Xe2)
  GDB_ADDR2LINE      IntelGT-aware addr2line executable used by gdb-intel

Examples:
  $(basename "$0") clean
  $(basename "$0") build
  $(basename "$0") test
  $(basename "$0") run
  ADAPTERS=all $(basename "$0") all
EOF
}

clean() {
  log "clean: removing $BUILD_DIR"
  rm -rf "$BUILD_DIR"
}

build() {
  if [[ ! -x "$GDB_ADDR2LINE" ]]; then
    echo "IntelGT addr2line executable not found: $GDB_ADDR2LINE" >&2
    return 1
  fi
  log "build: configuring $BUILD_DIR"
  cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -G "$CMAKE_GENERATOR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DDPC_GDB_ADDR2LINE_EXECUTABLE="$GDB_ADDR2LINE"
  log "build: compiling dwarf-parser-check"
  cmake --build "$BUILD_DIR"
}

test_project() {
  build
  log "test: running CTest"
  ctest --test-dir "$BUILD_DIR" --output-on-failure
}

ensure_kernel_debug_json() {
  if [[ -f "$KERNEL_DEBUG_JSON" ]]; then
    return
  fi

  if [[ "$KERNEL_DEBUG_JSON" != "$DEFAULT_KERNEL_DEBUG_JSON" ]]; then
    echo "kernel debug manifest not found: $KERNEL_DEBUG_JSON" >&2
    return 1
  fi

  log "run: generating kernel debug manifest at $KERNEL_DEBUG_JSON"
  "$SCRIPT_DIR/data_generation/make_reference" build run
}

run() {
  build
  ensure_kernel_debug_json

  mkdir -p "$OUTPUT_DIR"
  local resolver_args=(
    --kernel-debug-json "$KERNEL_DEBUG_JSON"
    --adapters "$ADAPTERS"
    --output-dir "$OUTPUT_DIR"
  )
  if [[ -n "$REFERENCE_FILE" ]]; then
    if [[ ! -f "$REFERENCE_FILE" ]]; then
      echo "reference file not found: $REFERENCE_FILE" >&2
      return 1
    fi
    resolver_args+=(--reference "$REFERENCE_FILE")
  fi

  DPC_IGA_PLATFORM="$IGA_PLATFORM" DPC_GDB_ADDR2LINE="$GDB_ADDR2LINE" \
    "$BUILD_DIR/dwarf-parser-check" "${resolver_args[@]}" \
    "$@" \
    >/dev/null

  local kernel_count
  kernel_count=$(grep -c '"mangled_name"' "$KERNEL_DEBUG_JSON" || true)
  log "run: kernel metadata JSON: $KERNEL_DEBUG_JSON ($kernel_count kernel(s))"
  log "run: per-adapter reports saved to $OUTPUT_DIR"
  if [[ -n "$REFERENCE_FILE" ]]; then
    log "run: compared with reference $REFERENCE_FILE"
  fi
}

if [[ $# -eq 0 ]]; then
  usage >&2
  exit 1
fi

command=$1
shift
case "$command" in
  clean)
    [[ $# -eq 0 ]] || { usage >&2; exit 1; }
    clean
    ;;
  build)
    [[ $# -eq 0 ]] || { usage >&2; exit 1; }
    build
    ;;
  test)
    [[ $# -eq 0 ]] || { usage >&2; exit 1; }
    test_project
    ;;
  run)
    run "$@"
    ;;
  all)
    clean
    test_project
    run "$@"
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