#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  cat <<'USAGE'
用法：
  ./scripts/run_dynamic_extent_diagnostics.sh

这个脚本用于批量测试 DELI-Dynamic-Single 动态维护原型。
它会一次跑完多个 stale_threshold_fraction，并自动汇总 CSV、画图。

最常用命令：

  小规模 smoke 测试：
    RESET_RESULTS=1 OVERWRITE=1 \
    DATASETS=ZGAP_MIXED \
    LIMIT=1234 QUERY_LIMIT=1234 \
    QUERY_ROOT=queries/interval_overlap_mixed_smoke_1234 \
    RESULT_DIR=results/dynamic_extent_smoke_1234 \
    FIGURE_DIR=figures/dynamic_extent_smoke_1234 \
    SELECTIVITY_TAGS=0p01pct \
    BLOCK_SIZES="16 128" \
    STALE_THRESHOLDS="0 0.05 0.1 0.2" \
    QUERY_COUNT=20 \
    INITIAL_FRACTION=0.2 \
    INSERT_FRACTION=0.6 \
    DELETE_FRACTION=0.2 \
    VALIDATE_EVERY=1 \
      ./scripts/run_dynamic_extent_diagnostics.sh

  正式一点的 mixed fat-object 实验：
    RESET_RESULTS=1 OVERWRITE=1 \
    DATASETS=ZGAP_MIXED \
    LIMIT=1000000 QUERY_LIMIT=1000000 \
    PREPARE_DATA=1 AUTO_GENERATE_QUERIES=1 \
    QUERY_ROOT=queries/interval_overlap_mixed_1000000 \
    RESULT_DIR=results/dynamic_extent_mixed_1000000 \
    FIGURE_DIR=figures/dynamic_extent_mixed_1000000 \
    SELECTIVITY_TAGS="0p001pct 0p01pct 0p1pct 1pct" \
    BLOCK_SIZES="256 512 1024 2048" \
    STALE_THRESHOLDS="0 0.05 0.1 0.2" \
      ./scripts/run_dynamic_extent_diagnostics.sh

  只根据已有 raw CSV 重新汇总和画图，不重新跑 benchmark：
    RUN_BENCHMARKS=0 RESET_RESULTS=0 \
    RESULT_DIR=results/dynamic_extent_mixed_1000000 \
    FIGURE_DIR=figures/dynamic_extent_mixed_1000000 \
      ./scripts/run_dynamic_extent_diagnostics.sh

常用参数：
  DATASETS
    要跑哪些数据集。默认 ZGAP_MIXED。
    可选：AW LW ROADS PARKS OSM_AU_POINTS UNIF_S UNIF_L DIAG_S DIAG_L ZGAP_WIDE ZGAP_MIXED

  LIMIT
    每个数据集最多加载多少条 geometry。默认 1000000。

  QUERY_LIMIT
    生成 query 时按多少条数据来生成。默认等于 LIMIT。

  SELECTIVITY_TAGS
    query 选择性，也可以粗略理解为 query 大小/答案规模。
    可选：0p001pct 0p01pct 0p1pct 1pct。
    默认：1pct。

  BLOCK_SIZES
    DynamicExtentIndex 每个 block 的目标记录数，多个值用空格分开。
    默认：512。

  STALE_THRESHOLDS
    stale_threshold_fraction 列表。
    含义：删除后允许 block 中 dead record 积累到多少比例才局部重建。
    例如 "0 0.05 0.1 0.2" 会一次跑完四种策略。
    默认：0 0.05 0.1 0.2。

  INITIAL_FRACTION
    初始 bulk-load 的数据比例。默认 0.5。

  INSERT_FRACTION
    bulk-load 后继续插入的数据比例。默认 0.2。

  DELETE_FRACTION
    插入后删除 live objects 的比例。默认 0.1。

  QUERY_COUNT
    每个 checkpoint 最多执行多少个 query。默认 100。

  VALIDATE_EVERY
    每隔多少次更新做一次 validate_index 内部不变式检查。
    0 表示只在 checkpoint 检查；小规模调试可以设 1。默认 0。

  STOP_ON_MISMATCH
    1：一旦 DELI 与 Boost exact 答案不一致就停止。
    0：记录错误但继续跑。默认 1。

  CHECK_CORRECTNESS
    1：每个 checkpoint 都重建 Boost R-tree 并比较最终答案集合，最严格但很慢。
    0：跳过 Boost answer-set check，只保留 DELI 自身查询和 validate_index。
    建议：先用 1 跑小规模确认正确性，再用 0 做大规模性能扫参。默认 1。

  AUTO_BUILD
    1：自动编译 bench_dynamic_extent_wkt。默认 1。

  PREPARE_DATA
    1：如果 synthetic 数据不存在或行数不足，就自动生成。默认 0。

  AUTO_GENERATE_QUERIES
    1：缺 query CSV 时自动生成。默认 0。

  REGENERATE_QUERIES
    1：即使 query CSV 已存在，也强制重新生成。默认 0。

  RUN_BENCHMARKS
    1：跑 C++ benchmark。
    0：只汇总已有 CSV 并画图。默认 1。

  RESET_RESULTS
    1：运行前删除 RESULT_DIR 下旧 CSV。只画图时必须设为 0。默认 1。

  OVERWRITE
    0：raw CSV 已存在就跳过，方便续跑。
    1：已有 raw CSV 也重新跑。默认 0。

  EXCLUDE_DATASETS
    汇总和画图时排除哪些数据集，不会删除 raw CSV。

输出：
  RESULT_DIR/dynamic_extent_summary.csv
  FIGURE_DIR/dynamic_extent_after_delete_*.png
  FIGURE_DIR/dynamic_extent_diagnostics.txt
USAGE
  exit 0
fi

# =========================
# 1. 实验规模和数据集
# =========================
# LIMIT：每个数据集最多加载多少条 geometry。
LIMIT="${LIMIT:-1000000}"

# QUERY_LIMIT：生成 query 文件时用多少条数据。通常等于 LIMIT。
QUERY_LIMIT="${QUERY_LIMIT:-$LIMIT}"

# DATASETS：要跑哪些数据集。动态维护实验默认先看 mixed fat-object 数据。
DATASETS="${DATASETS:-ZGAP_MIXED}"

# DATA_ROOT：真实数据所在目录。ROADS/PARKS 默认从 /mnt/hgfs 读取。
DATA_ROOT="${DATA_ROOT:-/mnt/hgfs}"

# QUERY_ROOT：query CSV 所在目录。文件名形如 ZGAP_MIXED_jts_strtree_knn_1pct.csv。
QUERY_ROOT="${QUERY_ROOT:-queries/dynamic_extent_${QUERY_LIMIT}}"

# RESULT_DIR：每个参数组合的 raw CSV 和 summary CSV 输出目录。
RESULT_DIR="${RESULT_DIR:-results/dynamic_extent_${LIMIT}}"

# FIGURE_DIR：图片和中文诊断说明输出目录。
FIGURE_DIR="${FIGURE_DIR:-figures/dynamic_extent_${LIMIT}}"

# =========================
# 2. 动态索引参数
# =========================
# BLOCK_SIZES：每个 block 的目标记录数。多个值用空格分开。
BLOCK_SIZES="${BLOCK_SIZES:-${BLOCK_SIZE:-512}}"

# STALE_THRESHOLDS：删除后 dead record 比例达到多少才触发 local rebuild。
# 0 表示尽快重建；0.2 表示最多容忍约 20% tombstone 后再重建。
STALE_THRESHOLDS="${STALE_THRESHOLDS:-0 0.05 0.1 0.2}"

# INITIAL_FRACTION：bulk-load 阶段加载的数据比例。
INITIAL_FRACTION="${INITIAL_FRACTION:-0.5}"

# INSERT_FRACTION：bulk-load 后插入的数据比例。
INSERT_FRACTION="${INSERT_FRACTION:-0.2}"

# DELETE_FRACTION：插入后删除 live objects 的比例。
DELETE_FRACTION="${DELETE_FRACTION:-0.1}"

# CELL_SIZE：Z-order 网格大小，保持和 GLIN/IO 实验一致。
CELL_SIZE="${CELL_SIZE:-0.0000005}"

# SEED：插入/删除顺序、query 生成、synthetic 数据生成使用的随机种子。
SEED="${SEED:-42}"

# QUERY_COUNT：每个 checkpoint 最多跑多少个 query。
QUERY_COUNT="${QUERY_COUNT:-100}"

# VALIDATE_EVERY：每隔多少次更新检查一次内部不变式。0 表示只在 checkpoint 检查。
VALIDATE_EVERY="${VALIDATE_EVERY:-0}"

# STOP_ON_MISMATCH：答案和 Boost exact 不一致时是否立即停止。
STOP_ON_MISMATCH="${STOP_ON_MISMATCH:-1}"

# CHECK_CORRECTNESS：是否在每个 checkpoint 重建 Boost R-tree 做 exact 答案集合对比。
# 1 最严格，但大规模扫参会很慢；0 适合正确性已经验证后的性能实验。
CHECK_CORRECTNESS="${CHECK_CORRECTNESS:-1}"

# =========================
# 3. 是否重跑、是否画图
# =========================
RUN_BENCHMARKS="${RUN_BENCHMARKS:-1}"
RESET_RESULTS="${RESET_RESULTS:-1}"
OVERWRITE="${OVERWRITE:-0}"
AUTO_BUILD="${AUTO_BUILD:-1}"
PLOT_RESULTS="${PLOT_RESULTS:-1}"
EXCLUDE_DATASETS="${EXCLUDE_DATASETS:-}"

# =========================
# 4. 自动准备数据和 query
# =========================
PREPARE_DATA="${PREPARE_DATA:-0}"
AUTO_GENERATE_QUERIES="${AUTO_GENERATE_QUERIES:-0}"
REGENERATE_QUERIES="${REGENERATE_QUERIES:-0}"
REGENERATE_STALE_QUERIES="${REGENERATE_STALE_QUERIES:-1}"
SYNTHETIC_KIND="${SYNTHETIC_KIND:-rectangles}"

REAL_WORK_DIR="${REAL_WORK_DIR:-data/real}"
SYN_WORK_DIR="${SYN_WORK_DIR:-data/synthetic/rectangles}"
ZGAP_WORK_DIR="${ZGAP_WORK_DIR:-data/synthetic/zrange_gap}"
ZGAP_MIXED_WORK_DIR="${ZGAP_MIXED_WORK_DIR:-data/synthetic/zrange_gap_mixed_${LIMIT}}"

mkdir -p "$RESULT_DIR"

if [[ "$RUN_BENCHMARKS" == "0" && "$RESET_RESULTS" == "1" ]]; then
  echo "Error: RUN_BENCHMARKS=0 时不能使用 RESET_RESULTS=1。" >&2
  echo "原因：只画图需要保留已有 CSV，RESET_RESULTS=1 会删除它们。" >&2
  echo "请改成：RUN_BENCHMARKS=0 RESET_RESULTS=0 ..." >&2
  exit 1
fi

if [[ "$AUTO_BUILD" == "1" && ( "$RUN_BENCHMARKS" == "1" || "$PREPARE_DATA" == "1" ) ]]; then
  cmake --build build --target \
    bench_dynamic_extent_wkt \
    generate_synthetic_points \
    generate_synthetic_rectangles \
    convert_binary_points_to_wkt \
    -j2
fi

if [[ "$RESET_RESULTS" == "1" ]]; then
  rm -f "$RESULT_DIR"/*.csv
fi

declare -A DATA_FILES=(
  [AW]="$DATA_ROOT/AREAWATER.csv"
  [LW]="$DATA_ROOT/LINEARWATER.csv"
  [ROADS]="$DATA_ROOT/roads"
  [PARKS]="$DATA_ROOT/parks"
  [OSM_AU_POINTS]="$REAL_WORK_DIR/osm_australia_1m_point.wkt"
  [UNIF_S]="$SYN_WORK_DIR/UNIF_S.wkt"
  [UNIF_L]="$SYN_WORK_DIR/UNIF_L.wkt"
  [DIAG_S]="$SYN_WORK_DIR/DIAG_S.wkt"
  [DIAG_L]="$SYN_WORK_DIR/DIAG_L.wkt"
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

file_line_count() {
  local path="$1"
  if [[ ! -f "$path" ]]; then
    echo 0
    return
  fi
  wc -l < "$path" | tr -d '[:space:]'
}

synthetic_file_needs_generation() {
  local path="$1"
  if [[ ! -s "$path" ]]; then
    return 0
  fi
  local rows
  rows="$(file_line_count "$path")"
  [[ "$rows" -lt "$LIMIT" ]]
}

validate_synthetic_size() {
  local dataset="$1"
  local path="$2"
  case "$dataset" in
    UNIF_S|UNIF_L|DIAG_S|DIAG_L|ZGAP_WIDE|ZGAP_MIXED|OSM_AU_POINTS)
      ;;
    *)
      return
      ;;
  esac

  local rows
  rows="$(file_line_count "$path")"
  if [[ "$rows" -lt "$LIMIT" ]]; then
    echo "Error: $dataset 数据行数不足。" >&2
    echo "  data_file=$path" >&2
    echo "  file_rows=$rows, LIMIT=$LIMIT" >&2
    echo "这通常说明 smoke 小数据被正式实验复用了。" >&2
    echo "解决方法：使用 PREPARE_DATA=1 自动生成足够大的数据，或指定新的 DATA_FILE_${dataset}/工作目录。" >&2
    exit 1
  fi
}

stale_tag() {
  local value="$1"
  echo "${value//./p}"
}

prepare_data_if_needed() {
  if [[ "$PREPARE_DATA" != "1" ]]; then
    return
  fi

  if [[ " $DATASETS " == *" OSM_AU_POINTS "* ]]; then
    local osm_wkt="$REAL_WORK_DIR/osm_australia_1m_point.wkt"
    if [[ ! -s "$osm_wkt" ]]; then
      mkdir -p "$REAL_WORK_DIR"
      ./build/convert_binary_points_to_wkt \
        --input_file "$DATA_ROOT/osm_australia_2m_point.bin" \
        --output_file "$osm_wkt" \
        --num "$LIMIT" \
        --dim 2
    fi
  fi

  if [[ " $DATASETS " == *" UNIF_S "* || " $DATASETS " == *" UNIF_L "* || \
        " $DATASETS " == *" DIAG_S "* || " $DATASETS " == *" DIAG_L "* ]]; then
    local missing=0
    for synthetic in UNIF_S UNIF_L DIAG_S DIAG_L; do
      if [[ " $DATASETS " == *" $synthetic "* ]] && \
         synthetic_file_needs_generation "$(data_file_for_dataset "$synthetic")"; then
        missing=1
      fi
    done
    if [[ "$missing" == "1" ]]; then
      if [[ "$SYNTHETIC_KIND" == "rectangles" ]]; then
        mkdir -p "$SYN_WORK_DIR"
        SMALL_N="$LIMIT" LARGE_N="$LIMIT" OUT_DIR="$SYN_WORK_DIR" SEED="$SEED" \
          scripts/prepare_synthetic_rectangles.sh
      elif [[ "$SYNTHETIC_KIND" == "points" ]]; then
        mkdir -p "$SYN_WORK_DIR"
        SMALL_N="$LIMIT" LARGE_N="$LIMIT" OUT_DIR="$SYN_WORK_DIR" SEED="$SEED" FORMAT=wkt \
          scripts/prepare_glin_synthetic_geo_points.sh
      else
        echo "Error: SYNTHETIC_KIND must be rectangles or points." >&2
        exit 1
      fi
    fi
  fi

  if [[ " $DATASETS " == *" ZGAP_WIDE "* ]]; then
    local zgap_wkt="$ZGAP_WORK_DIR/ZGAP_WIDE.wkt"
    if synthetic_file_needs_generation "$zgap_wkt"; then
      NUM="$LIMIT" OUT_DIR="$ZGAP_WORK_DIR" NAME=ZGAP_WIDE SEED="$SEED" AUTO_BUILD=0 \
        scripts/prepare_zrange_gap_dataset.sh
    fi
  fi

  if [[ " $DATASETS " == *" ZGAP_MIXED "* ]]; then
    local mixed_wkt
    mixed_wkt="$(data_file_for_dataset ZGAP_MIXED)"
    if synthetic_file_needs_generation "$mixed_wkt"; then
      local mixed_dir
      local mixed_name
      mixed_dir="$(dirname "$mixed_wkt")"
      mixed_name="$(basename "$mixed_wkt" .wkt)"
      NUM="$LIMIT" OUT_DIR="$mixed_dir" NAME="$mixed_name" SEED="$SEED" AUTO_BUILD=0 \
        scripts/prepare_zrange_mixed_dataset.sh
    fi
  fi
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
    elif [[ "$REGENERATE_STALE_QUERIES" == "1" && "$query_file" -ot "$data_file" ]]; then
      needs_generation=1
      reason="stale query file older than data: $query_file"
    fi
  done
  if [[ "$needs_generation" != "1" ]]; then
    return
  fi
  if [[ "$AUTO_GENERATE_QUERIES" != "1" && "$REGENERATE_QUERIES" != "1" ]]; then
    echo "Error: query file for $dataset under $QUERY_ROOT needs generation." >&2
    echo "Reason: $reason" >&2
    echo "Set AUTO_GENERATE_QUERIES=1 to generate ${dataset}_jts_strtree_knn_*.csv." >&2
    exit 1
  fi

  echo "Generating queries for $dataset: $reason"
  mkdir -p "$QUERY_ROOT"
  scripts/generate_jts_strtree_knn_queries.sh \
    "$(realpath "$data_file")" \
    "$QUERY_ROOT/${dataset}_jts_strtree_knn" \
    "$QUERY_LIMIT" \
    "$QUERY_COUNT" \
    "$SEED"
}

prepare_data_if_needed

if [[ "$RUN_BENCHMARKS" == "1" ]]; then
  for dataset in $DATASETS; do
    data_file="$(data_file_for_dataset "$dataset")"
    if [[ ! -e "$data_file" ]]; then
      echo "Error: data file not found: $data_file" >&2
      echo "如果这是 synthetic 数据，请加 PREPARE_DATA=1 自动生成。" >&2
      exit 1
    fi
    validate_synthetic_size "$dataset" "$data_file"
    generate_queries_if_needed "$dataset" "$data_file"

    for tag in $SELECTIVITY_TAGS; do
      query_file="$(query_file_for_dataset "$dataset" "$tag")"
      if [[ ! -e "$query_file" ]]; then
        echo "Error: query file not found: $query_file" >&2
        echo "请加 AUTO_GENERATE_QUERIES=1 自动生成 query，或减少 SELECTIVITY_TAGS。" >&2
        exit 1
      fi

      for block_size in $BLOCK_SIZES; do
        for stale_threshold in $STALE_THRESHOLDS; do
          stale_name="$(stale_tag "$stale_threshold")"
          raw_csv="$RESULT_DIR/${dataset}_${tag}_b${block_size}_st${stale_name}_deli_dynamic.csv"
          if should_run_file "$raw_csv"; then
            echo "Running DELI-Dynamic-Single dataset=$dataset selectivity=$tag block=$block_size stale=$stale_threshold"
            ./build/bench_dynamic_extent_wkt \
              --data_file "$data_file" \
              --query_file "$query_file" \
              --dataset_name "$dataset" \
              --limit "$LIMIT" \
              --query_count "$QUERY_COUNT" \
              --initial_fraction "$INITIAL_FRACTION" \
              --insert_fraction "$INSERT_FRACTION" \
              --delete_fraction "$DELETE_FRACTION" \
              --block_size "$block_size" \
              --stale_threshold_fraction "$stale_threshold" \
              --cell_size "$CELL_SIZE" \
              --seed "$SEED" \
              --validate_every "$VALIDATE_EVERY" \
              --check_correctness "$CHECK_CORRECTNESS" \
              --stop_on_mismatch "$STOP_ON_MISMATCH" \
              --output_csv "$raw_csv"
          else
            echo "Skip existing raw CSV: $raw_csv"
          fi
        done
      done
    done
  done
else
  echo "RUN_BENCHMARKS=0：跳过 C++ benchmark，只汇总已有 CSV 并画图。"
fi

python3 scripts/summarize_dynamic_extent_diagnostics.py \
  --result_dir "$RESULT_DIR" \
  --output_csv "$RESULT_DIR/dynamic_extent_summary.csv" \
  --exclude_datasets "$EXCLUDE_DATASETS"

if [[ "$PLOT_RESULTS" == "1" ]]; then
  python3 scripts/plot_dynamic_extent_diagnostics.py \
    --input "$RESULT_DIR/dynamic_extent_summary.csv" \
    --output_dir "$FIGURE_DIR" \
    --figure_prefix "dynamic_extent" \
    --checkpoint "after_delete" \
    --exclude_datasets "$EXCLUDE_DATASETS"
fi

echo "Result dir: $RESULT_DIR"
echo "Summary:    $RESULT_DIR/dynamic_extent_summary.csv"
if [[ "$PLOT_RESULTS" == "1" ]]; then
  echo "Figures:    $FIGURE_DIR"
  echo "Notes:      $FIGURE_DIR/dynamic_extent_diagnostics.txt"
fi
