#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

route_modes="${FUSION_ROUTE_MODES:-adaptive force_sfc force_guard}"
update_mode="${FUSION_UPDATE_MODE:-light}"
initial_reset="${RESET_RESULTS:-1}"

if [[ "$update_mode" == "coupled" ]]; then
  echo "Error: Phase 0 route ablation requires FUSION_UPDATE_MODE=light or full." >&2
  echo "Using coupled would change both query routing and update maintenance." >&2
  exit 1
fi

first_run=1
for route_mode in $route_modes; do
  reset_results=0
  if [[ "$first_run" == "1" ]]; then
    reset_results="$initial_reset"
    first_run=0
  fi
  echo "Phase 0 route=$route_mode update_mode=$update_mode"
  indexes="DELI_ADAPTIVE_PRL_FUSION"
  if [[ "$route_mode" == "force_guard" ]]; then
    indexes="DELI_ADAPTIVE_PRL_FUSION DELI_FUSION_GUARD_ONLY Boost_Rtree"
  fi
  env \
    INDEXES="$indexes" \
    FUSION_ROUTE_MODE="$route_mode" \
    FUSION_UPDATE_MODE="$update_mode" \
    RESET_RESULTS="$reset_results" \
    ./scripts/run_dynamic_compare_diagnostics.sh
done
