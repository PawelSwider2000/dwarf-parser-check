#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BUILD_DIR=${BUILD_DIR:-"$SCRIPT_DIR/build"}
BUILD_TYPE=${BUILD_TYPE:-Debug}
CMAKE_GENERATOR=${CMAKE_GENERATOR:-Ninja}
DEFAULT_KERNEL_DEBUG_JSON="$SCRIPT_DIR/artifacts/simple_sycl_vtune_kernel_debug.json"
KERNEL_DEBUG_JSON=${KERNEL_DEBUG_JSON:-"$DEFAULT_KERNEL_DEBUG_JSON"}
OUTPUT_CSV=${OUTPUT_CSV:-"$SCRIPT_DIR/artifacts/result_dwarf_parser.csv"}
OUTPUT_JSON=${OUTPUT_JSON:-"$SCRIPT_DIR/artifacts/adapter_vtune_comparison.json"}
REFERENCE_FILE=${REFERENCE_FILE:-"$SCRIPT_DIR/artifacts/result_vtune_reference.csv"}
ADAPTERS=${ADAPTERS:-rust-gimli}

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
  run <args>  Build and run dwarf-parser-check. Pass resolver options such as --ip.
  all <args>  Clean, build, test, then run with the supplied resolver options.

Environment:
  BUILD_DIR          CMake build directory (default: build)
  BUILD_TYPE         CMake build type (default: Debug)
  CMAKE_GENERATOR    CMake generator (default: Ninja)
  KERNEL_DEBUG_JSON  Kernel debug manifest for run (default: artifacts/simple_sycl_vtune_kernel_debug.json)
  OUTPUT_CSV         CSV output path for run (default: artifacts/result_dwarf_parser.csv)
  OUTPUT_JSON        Pretty JSON comparison report (default: artifacts/adapter_vtune_comparison.json)
  REFERENCE_FILE     Optional VTune source_locations.json or address-report CSV
  ADAPTERS           Adapter selection for run (default: rust-gimli)

Examples:
  $(basename "$0") clean
  $(basename "$0") build
  $(basename "$0") test
  $(basename "$0") run --ip 0xffff8000fff80000
  $(basename "$0") all --ip 0xffff8000fff80000
EOF
}

clean() {
  log "clean: removing $BUILD_DIR"
  rm -rf "$BUILD_DIR"
}

build() {
  log "build: configuring $BUILD_DIR"
  cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -G "$CMAKE_GENERATOR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
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

  mkdir -p "$(dirname "$OUTPUT_CSV")"
  mkdir -p "$(dirname "$OUTPUT_JSON")"
  local resolver_args=(
    --kernel-debug-json "$KERNEL_DEBUG_JSON"
    --adapters "$ADAPTERS"
    --output-csv "$OUTPUT_CSV"
    --output-json "$OUTPUT_JSON"
  )
  if [[ -n "$REFERENCE_FILE" ]]; then
    if [[ ! -f "$REFERENCE_FILE" ]]; then
      echo "reference file not found: $REFERENCE_FILE" >&2
      return 1
    fi
    resolver_args+=(--reference "$REFERENCE_FILE")
  fi

  "$BUILD_DIR/dwarf-parser-check" "${resolver_args[@]}" \
    "$@" \
    >/dev/null

  local kernel_count
  kernel_count=$(grep -c '"mangled_name"' "$KERNEL_DEBUG_JSON" || true)
  local resolved_address_count
  resolved_address_count=$(awk 'END { print (NR > 0 ? NR - 1 : 0) }' "$OUTPUT_CSV")

  log "run: kernel metadata JSON: $KERNEL_DEBUG_JSON ($kernel_count kernel(s))"
  log "run: resolved $resolved_address_count address(es) with $ADAPTERS"
  log "run: CSV saved to $OUTPUT_CSV"
  log "run: adapter/VTune comparison JSON saved to $OUTPUT_JSON"
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