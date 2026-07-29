#!/usr/bin/env bash
# Run dwarf-parser-check experiments across all combinations of
# workloads, debug modes, and optimization levels.
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
DWARF_CHECK="$SCRIPT_DIR/dwarf-check.sh"

# ── Experiment matrix ──────────────────────────────────────────────────────────
WORKLOADS=(gemm)
DEBUG_MODES=(g gline)
OPT_LEVELS=(O0 O1 O2)

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

# ── Build the adapter once (shared across all experiments) ─────────────────────
log() { printf '[experiments] %s\n' "$*"; }

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
total=$(( ${#WORKLOADS[@]} * ${#DEBUG_MODES[@]} * ${#OPT_LEVELS[@]} ))
count=0
failed=()

for workload in "${WORKLOADS[@]}"; do
  for debug in "${DEBUG_MODES[@]}"; do
    for opt in "${OPT_LEVELS[@]}"; do
      (( count++ )) || true
      label="$workload/debug=$debug/opt=$opt"
      log "[$count/$total] experiment: $label"

      # shellcheck disable=SC2086
      if "$DWARF_CHECK" $EXTRA_FLAGS \
          --workload "$workload" \
          --debug "$debug" \
          --opt "$opt" \
          all --adapters "$ADAPTERS"; then
        log "[$count/$total] PASS: $label"
      else
        log "[$count/$total] FAIL: $label"
        failed+=("$label")
      fi
    done
  done
done

# ── Summary ────────────────────────────────────────────────────────────────────
echo
log "results: $((count - ${#failed[@]}))/$count passed"
if [[ ${#failed[@]} -gt 0 ]]; then
  log "failed experiments:"
  for f in "${failed[@]}"; do
    log "  - $f"
  done
  exit 1
fi
