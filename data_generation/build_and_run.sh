#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ARTIFACT_DIR=${ARTIFACT_DIR:-"$SCRIPT_DIR/artifacts"}
BIN_PATH="$ARTIFACT_DIR/simple_sycl_vtune"
JSON_PATH="$ARTIFACT_DIR/simple_sycl_vtune_kernel_debug.json"
TOTAL_LOOPS=${1:-1}
MATRIX_SIZE=${2:-128}

mkdir -p "$ARTIFACT_DIR"

pushd "$SCRIPT_DIR" >/dev/null
icpx \
  -fsycl -std=c++20 -g -O0 \
  simple_sycl_vtune.cpp \
  kernel_debug_info.cpp \
  -lze_loader \
  -o "$BIN_PATH"
popd >/dev/null

(
  cd "$ARTIFACT_DIR"
  ZE_ENABLE_TRACING_LAYER=${ZE_ENABLE_TRACING_LAYER:-1} \
  "$BIN_PATH" "$TOTAL_LOOPS" "$MATRIX_SIZE" "$JSON_PATH"
)

echo "binary: $BIN_PATH"
echo "json:   $JSON_PATH"
