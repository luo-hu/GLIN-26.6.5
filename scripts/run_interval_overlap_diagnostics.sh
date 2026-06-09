#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

LIMIT="${LIMIT:-2000000}"
QUERY_LIMIT="${QUERY_LIMIT:-$LIMIT}"
DATASETS="${DATASETS:-ROADS PARKS}"
DATA_ROOT="${DATA_ROOT:-/mnt/hgfs}"
QUERY_ROOT="${QUERY_ROOT:-queries/fig17_hybrid_${QUERY_LIMIT}}"
RESULT_DIR="${RESULT_DIR:-results/interval_overlap_${LIMIT}}"
BLOCK_SIZE="${BLOCK_SIZE:-1024}"
PIECE_LIMIT="${PIECE_LIMIT:-10000}"
CELL_SIZE="${CELL_SIZE:-0.0000005}"
SEED="${SEED:-42}"
RESET_RESULTS="${RESET_RESULTS:-1}"
AUTO_BUILD="${AUTO_BUILD:-1}"
PLOT_RESULTS="${PLOT_RESULTS:-1}"
FIGURE_DIR="${FIGURE_DIR:-figures/interval_overlap_${LIMIT}}"

mkdir -p "$RESULT_DIR"

if [[ "$AUTO_BUILD" == "1" ]]; then
  cmake --build build --target bench_interval_overlap_wkt bench_glin_wkt_piece bench_boost_rtree_wkt -j2
fi

if [[ "$RESET_RESULTS" == "1" ]]; then
  rm -f "$RESULT_DIR"/*.csv
fi

declare -A DATA_FILES=(
  [AW]="$DATA_ROOT/AREAWATER.csv"
  [LW]="$DATA_ROOT/LINEARWATER.csv"
  [ROADS]="$DATA_ROOT/roads"
  [PARKS]="$DATA_ROOT/parks"
)

data_file_for_dataset() {
  local dataset="$1"
  local override_var="DATA_FILE_${dataset}"
  local override_value="${!override_var:-}"
  if [[ -n "$override_value" ]]; then
    echo "$override_value"
    return
  fi
  if [[ -z "${DATA_FILES[$dataset]:-}" ]]; then
    echo "Error: unknown dataset '$dataset'." >&2
    exit 1
  fi
  echo "${DATA_FILES[$dataset]}"
}

query_file_for_dataset() {
  local dataset="$1"
  echo "$QUERY_ROOT/${dataset}_jts_strtree_knn_1pct.csv"
}

for dataset in $DATASETS; do
  data_file="$(data_file_for_dataset "$dataset")"
  query_file="$(query_file_for_dataset "$dataset")"

  if [[ ! -e "$data_file" ]]; then
    echo "Error: data file not found: $data_file" >&2
    exit 1
  fi
  if [[ ! -e "$query_file" ]]; then
    echo "Error: query file not found: $query_file" >&2
    echo "Generate it with scripts/run_fig17_hybrid_1m.sh first, or set QUERY_ROOT." >&2
    exit 1
  fi

  echo "Running IntervalOverlapIndex for $dataset"
  ./build/bench_interval_overlap_wkt \
    --data_file "$data_file" \
    --query_file "$query_file" \
    --dataset_name "$dataset" \
    --limit "$LIMIT" \
    --block_size "$BLOCK_SIZE" \
    --cell_size "$CELL_SIZE" \
    --seed "$SEED" \
    --output_csv "$RESULT_DIR/${dataset}_interval_overlap.csv"

  echo "Running GLIN-piecewise for $dataset"
  ./build/bench_glin_wkt_piece \
    --data_file "$data_file" \
    --query_file "$query_file" \
    --dataset_name "$dataset" \
    --limit "$LIMIT" \
    --piece_limit "$PIECE_LIMIT" \
    --cell_size "$CELL_SIZE" \
    --seed "$SEED" \
    --output_csv "$RESULT_DIR/${dataset}_glin_piecewise.csv"

  echo "Running Boost R-tree for $dataset"
  ./build/bench_boost_rtree_wkt \
    --data_file "$data_file" \
    --query_file "$query_file" \
    --dataset_name "$dataset" \
    --limit "$LIMIT" \
    --relationship intersects \
    --seed "$SEED" \
    --output_csv "$RESULT_DIR/${dataset}_boost_rtree.csv"
done

python3 scripts/summarize_interval_overlap_diagnostics.py \
  --result_dir "$RESULT_DIR" \
  --output_csv "$RESULT_DIR/interval_overlap_summary.csv"

if [[ "$PLOT_RESULTS" == "1" ]]; then
  python3 scripts/plot_interval_overlap_diagnostics.py \
    --input "$RESULT_DIR/interval_overlap_summary.csv" \
    --output_dir "$FIGURE_DIR" \
    --figure_prefix "interval_overlap"
fi

echo "Result dir: $RESULT_DIR"
echo "Summary:    $RESULT_DIR/interval_overlap_summary.csv"
if [[ "$PLOT_RESULTS" == "1" ]]; then
  echo "Figures:    $FIGURE_DIR"
fi
