#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

RESULT_ROOT="${RESULT_ROOT:-results/deli_paper_ablation_main8}"
FIGURE_ROOT="${FIGURE_ROOT:-manuscript/2026.8.28/pvldbstyle-master/figures/ablation_main8}"
ABLATION_VARIANTS="${ABLATION_VARIANTS:-full fixed_layout conditional_refresh fixed_budget fixed_layout_no_prl}"
ABLATION_FIXED_BUDGET="${ABLATION_FIXED_BUDGET:-0.25}"
export DATASETS="${DATASETS:-AW ROADS}"
export SELECTIVITY_TAGS="${SELECTIVITY_TAGS:-0p01pct 0p1pct 1pct}"
export MIXED_PROFILES="${MIXED_PROFILES:-read_heavy write_heavy}"
export WORKLOAD_MODE="${WORKLOAD_MODE:-mixed}"
export LIMIT="${LIMIT:-500000}"
export QUERY_LIMIT="${QUERY_LIMIT:-$LIMIT}"
export QUERY_ROOT="${QUERY_ROOT:-queries/interval_overlap_full_$QUERY_LIMIT}"
export QUERY_COUNT="${QUERY_COUNT:-500}"
export MIXED_OPERATIONS="${MIXED_OPERATIONS:-200000}"
export MIXED_CHECKPOINT_INTERVAL="${MIXED_CHECKPOINT_INTERVAL:-10000}"
export BLOCK_SIZE="${BLOCK_SIZE:-512}"
export LOCAL_DELTA_BOUND="${LOCAL_DELTA_BOUND:-128}"
export DELETE_COMPACT_FRACTION="${DELETE_COMPACT_FRACTION:-0.25}"
export COST_COMPACTION_HORIZON="${COST_COMPACTION_HORIZON:-0}"
export INITIAL_FRACTION="${INITIAL_FRACTION:-0.5}"
export SEED="${SEED:-42}"
export AUTO_GENERATE_QUERIES="${AUTO_GENERATE_QUERIES:-0}"

if [[ "$WORKLOAD_MODE" != "mixed" ]]; then
  echo "This matched-checkpoint ablation requires WORKLOAD_MODE=mixed." >&2
  exit 2
fi
if ! awk -v horizon="$COST_COMPACTION_HORIZON" -v budget="$ABLATION_FIXED_BUDGET" \
    -v local="$LOCAL_DELTA_BOUND" -v block="$BLOCK_SIZE" \
    -v dead="$DELETE_COMPACT_FRACTION" \
    'BEGIN { exit !(horizon == 0 && block > 0 && budget > 0 && budget <= 1 &&
                   (local == 0 ? 0.25 : local / block) <= budget && dead <= budget) }'; then
  echo "Require zero compaction horizon and a fixed budget >= base Delta/dead fractions." >&2
  exit 2
fi

for variant in $ABLATION_VARIANTS; do
  case "$variant" in
    full|fixed_layout|conditional_refresh|fixed_budget|fixed_layout_no_prl) ;;
    *) echo "Unknown ablation variant: $variant" >&2; exit 2 ;;
  esac
done

auto_build="${AUTO_BUILD:-1}"
for variant in $ABLATION_VARIANTS; do
  adaptive_partition=1
  defer_refresh=1
  predicate_shortcuts=1
  beta_min="${COST_BETA_MIN:-0.25}"
  beta_max="${COST_BETA_MAX:-0.50}"
  tau_min="${COST_TAU_MIN:-0.25}"
  tau_max="${COST_TAU_MAX:-0.50}"
  case "$variant" in
    fixed_layout) adaptive_partition=0 ;;
    conditional_refresh) defer_refresh=0 ;;
    fixed_budget)
      beta_min="$ABLATION_FIXED_BUDGET"
      beta_max="$ABLATION_FIXED_BUDGET"
      tau_min="$ABLATION_FIXED_BUDGET"
      tau_max="$ABLATION_FIXED_BUDGET"
      ;;
    fixed_layout_no_prl)
      adaptive_partition=0
      predicate_shortcuts=0
      ;;
  esac

  echo "DELI ablation: $variant (partition=$adaptive_partition, defer=$defer_refresh, PRL=$predicate_shortcuts)"
  RESULT_DIR="$RESULT_ROOT/$variant" \
  FIGURE_DIR="$FIGURE_ROOT/$variant" \
  INDEXES="DELI_ALEX_HYBRID_SINGLE_STORE_COST" \
  COST_ADAPTIVE_PARTITION="$adaptive_partition" \
  DEFER_DELETE_SUMMARY_REFRESH="$defer_refresh" \
  COST_BETA_MIN="$beta_min" COST_BETA_MAX="$beta_max" \
  COST_TAU_MIN="$tau_min" COST_TAU_MAX="$tau_max" \
  PREDICATE_SHORTCUTS="$predicate_shortcuts" \
  PREDICATE_SHORTCUTS_LIST="$predicate_shortcuts" \
  AUTO_BUILD="$auto_build" \
  PLOT_RESULTS=0 \
  BATCH_MIXED_PROFILES=0 \
  CHECK_CORRECTNESS="${CHECK_CORRECTNESS:-0}" \
  ./scripts/run_dynamic_compare_diagnostics.sh
  auto_build=0
done

python scripts/summarize_deli_paper_ablation.py \
  --result-root "$RESULT_ROOT" --figure-dir "$FIGURE_ROOT" \
  --fixed-budget "$ABLATION_FIXED_BUDGET" --variants $ABLATION_VARIANTS
