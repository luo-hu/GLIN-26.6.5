#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  cat <<'USAGE'
用法：
  ./scripts/run_dynamic_compare_diagnostics.sh

这个脚本用于跑统一动态对比：
  DELI-Dynamic-Single
  DELI-ALEX
  DELI-ALEX-Hybrid
  DELI-ALEX-Hybrid-Buf
  DELI-ALEX-Hybrid-Bounded
  DELI-ALEX-Hybrid-LocalBounded
  Boost R-tree
  GEOS Quadtree
  GLIN-piece

所有方法使用同一套 workload：
  bulk-load 50% -> insert 20% -> query -> delete 10% -> query

默认 DELI 参数固定为：
  block_size=512
  stale_threshold_fraction=0.05

常用 smoke 命令：
  RESET_RESULTS=1 OVERWRITE=1 \
  DATASETS=ZGAP_MIXED \
  LIMIT=1234 QUERY_LIMIT=1234 \
  QUERY_ROOT=queries/interval_overlap_mixed_smoke_1234 \
  RESULT_DIR=results/dynamic_compare_smoke_1234 \
  FIGURE_DIR=figures/dynamic_compare_smoke_1234 \
  SELECTIVITY_TAGS=0p01pct QUERY_COUNT=20 \
    ./scripts/run_dynamic_compare_diagnostics.sh

正式 ZGAP_MIXED 命令：
  RESET_RESULTS=1 OVERWRITE=1 \
  DATASETS=ZGAP_MIXED \
  LIMIT=1000000 QUERY_LIMIT=1000000 \
  QUERY_ROOT=queries/interval_overlap_mixed_1000000 \
  RESULT_DIR=results/dynamic_compare_mixed_1000000 \
  FIGURE_DIR=figures/dynamic_compare_mixed_1000000 \
  SELECTIVITY_TAGS="0p001pct 0p01pct 0p1pct 1pct" \
    ./scripts/run_dynamic_compare_diagnostics.sh

常用参数：
  DATASETS
    默认 ZGAP_MIXED。可选：AW LW ROADS PARKS UNIF_S DIAG_S ZGAP_WIDE ZGAP_MIXED。

  LIMIT
    每个数据集最多加载多少条 geometry。默认 1000000。

  SELECTIVITY_TAGS
    query 选择性。默认 1pct。

  BLOCK_SIZE
    DELI block size。默认 512。

  STALE_THRESHOLD
    DELI stale_threshold_fraction。默认 0.05。

  LOCAL_DELTA_BOUND
    DELI-ALEX-Hybrid-LocalBounded 的每个 block 局部 delta 上限。
    默认 0，表示自动使用 max(64, BLOCK_SIZE/4)，把每个 block 的额外扫描量控制在约 25%。

  DELETE_COMPACT_FRACTION
    DELI-ALEX-Hybrid-LocalBounded 的删除物理压缩阈值。
    默认 0.25，表示每个 block 最多容忍约 25% tombstone 后才做 physical compaction。

  INITIAL_FRACTION / INSERT_FRACTION / DELETE_FRACTION
    默认 0.5 / 0.2 / 0.1。

  AUTO_GENERATE_QUERIES
    0：缺 query 文件就报错。
    1：缺 query 文件时调用 JTS STRtree KNN query generator 自动生成。
    默认：0。

输出：
  RESULT_DIR/dynamic_compare_summary.csv
  FIGURE_DIR/dynamic_compare_*.png
USAGE
  exit 0
fi

LIMIT="${LIMIT:-1000000}"
QUERY_LIMIT="${QUERY_LIMIT:-$LIMIT}"
DATASETS="${DATASETS:-ZGAP_MIXED}"
DATA_ROOT="${DATA_ROOT:-/mnt/hgfs}"
ZGAP_WORK_DIR="${ZGAP_WORK_DIR:-data/synthetic/zrange_gap}"
ZGAP_MIXED_WORK_DIR="${ZGAP_MIXED_WORK_DIR:-data/synthetic/zrange_gap_mixed_${LIMIT}}"
QUERY_ROOT="${QUERY_ROOT:-queries/dynamic_compare_${QUERY_LIMIT}}"
RESULT_DIR="${RESULT_DIR:-results/dynamic_compare_${LIMIT}}"
FIGURE_DIR="${FIGURE_DIR:-figures/dynamic_compare_${LIMIT}}"
SELECTIVITY_TAGS="${SELECTIVITY_TAGS:-1pct}"

# 这里故意固定为论文默认参数，避免每个数据集单独调参。
BLOCK_SIZE="${BLOCK_SIZE:-512}"
STALE_THRESHOLD="${STALE_THRESHOLD:-0.05}"
LOCAL_DELTA_BOUND="${LOCAL_DELTA_BOUND:-0}"
DELETE_COMPACT_FRACTION="${DELETE_COMPACT_FRACTION:-0.25}"
PIECE_LIMIT="${PIECE_LIMIT:-10000}"

INITIAL_FRACTION="${INITIAL_FRACTION:-0.5}"
INSERT_FRACTION="${INSERT_FRACTION:-0.2}"
DELETE_FRACTION="${DELETE_FRACTION:-0.1}"
QUERY_COUNT="${QUERY_COUNT:-100}"
CELL_SIZE="${CELL_SIZE:-0.0000005}"
SEED="${SEED:-42}"

AUTO_BUILD="${AUTO_BUILD:-1}"
RUN_BENCHMARKS="${RUN_BENCHMARKS:-1}"
RESET_RESULTS="${RESET_RESULTS:-1}"
OVERWRITE="${OVERWRITE:-0}"
PLOT_RESULTS="${PLOT_RESULTS:-1}"
EXCLUDE_DATASETS="${EXCLUDE_DATASETS:-}"
AUTO_GENERATE_QUERIES="${AUTO_GENERATE_QUERIES:-0}"
REGENERATE_QUERIES="${REGENERATE_QUERIES:-0}"

if [[ "$RUN_BENCHMARKS" == "0" && "$RESET_RESULTS" == "1" ]]; then
  echo "Error: RUN_BENCHMARKS=0 时不能使用 RESET_RESULTS=1，否则会删掉已有结果。" >&2
  exit 1
fi

mkdir -p "$RESULT_DIR"
if [[ "$RESET_RESULTS" == "1" ]]; then
  rm -f "$RESULT_DIR"/*.csv
fi

if [[ "$AUTO_BUILD" == "1" && "$RUN_BENCHMARKS" == "1" ]]; then
  cmake --build build --target bench_dynamic_compare_wkt -j2
fi

declare -A DATA_FILES=(
  [AW]="$DATA_ROOT/AREAWATER.csv"
  [LW]="$DATA_ROOT/LINEARWATER.csv"
  [ROADS]="$DATA_ROOT/roads"
  [PARKS]="$DATA_ROOT/parks"
  [UNIF_S]="data/synthetic/rectangles/UNIF_S.wkt"
  [DIAG_S]="data/synthetic/rectangles/DIAG_S.wkt"
  [ZGAP_WIDE]="$ZGAP_WORK_DIR/ZGAP_WIDE.wkt"
  [ZGAP_MIXED]="$ZGAP_MIXED_WORK_DIR/ZGAP_MIXED.wkt"
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
  local tag="$2"
  echo "$QUERY_ROOT/${dataset}_jts_strtree_knn_${tag}.csv"
}

should_run_file() {
  local path="$1"
  [[ "$OVERWRITE" == "1" || ! -s "$path" ]]
}

generate_queries_if_needed() {
  local dataset="$1"
  local data_file="$2"
  local needs_generation="$REGENERATE_QUERIES"
  local reason=""

  if [[ "$REGENERATE_QUERIES" == "1" ]]; then
    reason="REGENERATE_QUERIES=1"
  fi

  for tag in $SELECTIVITY_TAGS; do
    local query_file
    query_file="$(query_file_for_dataset "$dataset" "$tag")"
    if [[ ! -s "$query_file" ]]; then
      needs_generation=1
      reason="missing query file: $query_file"
    fi
  done

  if [[ "$needs_generation" != "1" ]]; then
    return
  fi

  if [[ "$AUTO_GENERATE_QUERIES" != "1" && "$REGENERATE_QUERIES" != "1" ]]; then
    echo "Error: query file for $dataset under $QUERY_ROOT needs generation." >&2
    echo "Reason: $reason" >&2
    echo "请先用 run_interval_overlap_diagnostics.sh 生成 query，设置正确 QUERY_ROOT，或加 AUTO_GENERATE_QUERIES=1。" >&2
    exit 1
  fi

  if [[ -n "$reason" ]]; then
    echo "Generating queries for $dataset: $reason"
  fi
  mkdir -p "$QUERY_ROOT"
  scripts/generate_jts_strtree_knn_queries.sh \
    "$(realpath "$data_file")" \
    "$QUERY_ROOT/${dataset}_jts_strtree_knn" \
    "$QUERY_LIMIT" \
    "$QUERY_COUNT" \
    "$SEED"
}

if [[ "$RUN_BENCHMARKS" == "1" ]]; then
  for dataset in $DATASETS; do
    data_file="$(data_file_for_dataset "$dataset")"
    if [[ ! -e "$data_file" ]]; then
      echo "Error: data file not found: $data_file" >&2
      exit 1
    fi
    generate_queries_if_needed "$dataset" "$data_file"

    for tag in $SELECTIVITY_TAGS; do
      query_file="$(query_file_for_dataset "$dataset" "$tag")"
      if [[ ! -e "$query_file" ]]; then
        echo "Error: query file not found: $query_file" >&2
        echo "请先用 run_interval_overlap_diagnostics.sh 生成 query，或设置正确 QUERY_ROOT。" >&2
        exit 1
      fi

      raw_csv="$RESULT_DIR/${dataset}_${tag}_dynamic_compare.csv"
      if should_run_file "$raw_csv"; then
        echo "Running dynamic compare dataset=$dataset selectivity=$tag"
        ./build/bench_dynamic_compare_wkt \
          --data_file "$data_file" \
          --query_file "$query_file" \
          --dataset_name "$dataset" \
          --limit "$LIMIT" \
          --query_count "$QUERY_COUNT" \
          --initial_fraction "$INITIAL_FRACTION" \
          --insert_fraction "$INSERT_FRACTION" \
          --delete_fraction "$DELETE_FRACTION" \
          --block_size "$BLOCK_SIZE" \
          --stale_threshold_fraction "$STALE_THRESHOLD" \
          --local_delta_bound "$LOCAL_DELTA_BOUND" \
          --delete_compact_fraction "$DELETE_COMPACT_FRACTION" \
          --piece_limit "$PIECE_LIMIT" \
          --cell_size "$CELL_SIZE" \
          --seed "$SEED" \
          --stop_on_mismatch 0 \
          --output_csv "$raw_csv"
      else
        echo "Skip existing raw CSV: $raw_csv"
      fi
    done
  done
else
  echo "RUN_BENCHMARKS=0：跳过 benchmark，只汇总和画图。"
fi

python3 scripts/summarize_dynamic_compare_diagnostics.py \
  --result_dir "$RESULT_DIR" \
  --output_csv "$RESULT_DIR/dynamic_compare_summary.csv" \
  --exclude_datasets "$EXCLUDE_DATASETS"

if [[ "$PLOT_RESULTS" == "1" ]]; then
  python3 scripts/plot_dynamic_compare_diagnostics.py \
    --input "$RESULT_DIR/dynamic_compare_summary.csv" \
    --output_dir "$FIGURE_DIR" \
    --figure_prefix dynamic_compare \
    --exclude_datasets "$EXCLUDE_DATASETS"
fi

echo "Result dir: $RESULT_DIR"
echo "Summary:    $RESULT_DIR/dynamic_compare_summary.csv"
if [[ "$PLOT_RESULTS" == "1" ]]; then
  echo "Figures:    $FIGURE_DIR"
fi
