#!/usr/bin/env bash
set -euo pipefail

# Reproduce the arXiv GLIN Fig.17-style hybrid workload with the current
# GLIN-ALEX/GLIN-piecewise implementation.
#
# Defaults:
#   - datasets: ROADS and PARKS
#   - limit: 1M valid geometries
#   - query workload: 1% JTS STRtree KNN windows
#   - hybrid workloads: read-intensive 90/10 and write-intensive 50/50
#   - insert transaction size: 1% of loaded records
#
# Override examples:
#   LIMIT=10000 WORKLOAD=read scripts/run_fig17_hybrid_1m.sh
#   INSERT_ORDER=zmin scripts/run_fig17_hybrid_1m.sh
#   LIMIT=5000000 DATASETS="ROADS PARKS" scripts/run_fig17_hybrid_1m.sh
#   DATASETS="LW ROADS PARKS" AUTO_GENERATE_QUERIES=1 scripts/run_fig17_hybrid_1m.sh
#   RESET_RESULTS=0 DATASETS=PARKS JTS_JAVA_HEAP=20g scripts/run_fig17_hybrid_1m.sh
#   LIMIT=10000000 QUERY_LIMIT=5000000 DATASETS=PARKS scripts/run_fig17_hybrid_1m.sh

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

export MPLCONFIGDIR="${MPLCONFIGDIR:-/tmp/matplotlib}"

LIMIT="${LIMIT:-1000000}" #数据量
QUERY_LIMIT="${QUERY_LIMIT:-$LIMIT}" # query窗口生成使用的数据量； 内存不足缩减 QUERY_LIMIT：QUERY_LIMIT=5000000 LIMIT=10000000：加载 1000 万数据建索引，只用 500 万生成查询；
DATASETS="${DATASETS:-ROADS PARKS}"  #数据集
WORKLOAD="${WORKLOAD:-both}"         #工作负载：•read：读密集负载：90% 查询 + 10% 动态插入 / 删除   write：写密集负载：50% 查询 + 50% 动态插入 / 删除  both：自动顺序跑完read+write两种负载   只跑读密集 / 只跑写密集：WORKLOAD=read QUERY_SELECTIVITIES="0.1%" ...。
INSERT_ORDER="${INSERT_ORDER:-random}"   #插入顺序，是随机插入，还是按Zmin顺序插入（这种效果最好）
INSERT_BATCH_PERCENT="${INSERT_BATCH_PERCENT:-0.01}" # 单次批量插入量 = 总数据量 × 0.01
INITIAL_FRACTION="${INITIAL_FRACTION:-0.5}"  #总有效数据LIMIT条，前50%一次性加载，构建初始索引
INSERT_FRACTION="${INSERT_FRACTION:-0.5}"    # 剩余 50% 数据作为动态数据，分批增量插入 / 删除（混合负载的更新数据源）
PIECE_LIMIT="${PIECE_LIMIT:-10000}"  # GLIN piecewise 分片单分片最大容量，满容量触发分片分裂
CELL_SIZE="${CELL_SIZE:-0.0000005}"  # GLIN 全局网格划分单元格尺寸，空间预分区参数
SEED="${SEED:-42}" # 全局随机种子，固定 = 实验可复现
QUERY_COUNT="${QUERY_COUNT:-100}"  # 每种选择性生成多少条查询语句
QUERY_SELECTIVITIES="${QUERY_SELECTIVITIES:-1%}" #查询 AABB 矩形的空间面积占整个数据集全域空间的百分比； 当前脚本一次只能执行一个选择性，
PROGRESS_STEP_PERCENT="${PROGRESS_STEP_PERCENT:-10}"
INCLUDE_LSM_ASYNC_GLIN="${INCLUDE_LSM_ASYNC_GLIN:-0}"
INCLUDE_LSM_SEGMENTED_GLIN="${INCLUDE_LSM_SEGMENTED_GLIN:-0}"
INCLUDE_LSM_SEGMENTED4_GLIN="${INCLUDE_LSM_SEGMENTED4_GLIN:-0}"
INCLUDE_LSM_BG_GLIN="${INCLUDE_LSM_BG_GLIN:-0}"
DELTA_SIZE="${DELTA_SIZE:-100000}"
DATA_ROOT="${DATA_ROOT:-/mnt/hgfs}"    # 原始数据集路径
QUERY_ROOT="${QUERY_ROOT:-queries/fig17_hybrid_${QUERY_LIMIT}}" # 查询 csv 文件存储根目录
AUTO_GENERATE_QUERIES="${AUTO_GENERATE_QUERIES:-1}"      #默认是1，就是说它会根据根据数据集和数据量大小LIMIT自动调用JTS/Java 生成 STRtree-KNN 查询文件来生成query窗口，如果设置为0，需要自己提前准备好查询窗口
FORCE_GENERATE_QUERIES="${FORCE_GENERATE_QUERIES:-0}" #0 = 查询文件已存在则复用、不重新生成；1 = 强制删除旧查询，从头生成新查询 不想重复生成查询文件（节省耗时）：默认 FORCE_GENERATE_QUERIES=0，同一 QUERY_LIMIT 下查询生成一次永久复用；
RESET_RESULTS="${RESET_RESULTS:-1}"   #RESET_RESULTS=1  是清空之前的结果，RESET_RESULTS=0是在原来的基础上继续追加结果，比如在ROADS数据跑完了，但是在PARKS数据集上中断了，后续在目前的基础上只需要再跑一次PARKS数据集
RESULT_DIR="${RESULT_DIR:-results/fig17_hybrid_${LIMIT}}"  # 性能指标 CSV 输出目录
FIGURE_DIR="${FIGURE_DIR:-figures/fig17_hybrid_${LIMIT}}"  # 绘图图片输出目录

export QUERY_SELECTIVITIES

mkdir -p "$RESULT_DIR" "$FIGURE_DIR"

if [[ "$QUERY_LIMIT" != "$LIMIT" ]]; then
  echo "Warning: LIMIT=$LIMIT but QUERY_LIMIT=$QUERY_LIMIT." >&2
  echo "The index workload uses $LIMIT records, while query windows are generated from $QUERY_LIMIT records." >&2
  echo "This is useful when memory is limited, but it is not strict paper-level selectivity." >&2
fi

cmake --build build --target bench_hybrid_wkt_piece -j2

SUMMARY_CSV="$RESULT_DIR/fig17_hybrid_summary.csv"
PROGRESS_CSV="$RESULT_DIR/fig17_hybrid_progress.csv"
if [[ "$RESET_RESULTS" == "1" ]]; then
  rm -f "$SUMMARY_CSV" "$PROGRESS_CSV"
fi
append_flag=""
if [[ "$RESET_RESULTS" == "0" && ( -s "$SUMMARY_CSV" || -s "$PROGRESS_CSV" ) ]]; then
  append_flag="--append_csv"
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
    echo "Known datasets: AW LW ROADS PARKS" >&2
    echo "Or set DATA_FILE_${dataset}=/path/to/file." >&2
    exit 1
  fi
  echo "${DATA_FILES[$dataset]}"
}

query_file_for_dataset() {
  local dataset="$1"
  echo "$QUERY_ROOT/${dataset}_jts_strtree_knn_1pct.csv"
}

query_prefix_for_dataset() {
  local dataset="$1"
  echo "$QUERY_ROOT/${dataset}_jts_strtree_knn"
}

mkdir -p "$QUERY_ROOT"

for dataset in $DATASETS; do
  data_file="$(data_file_for_dataset "$dataset")"
  query_file="$(query_file_for_dataset "$dataset")"
  query_prefix="$(query_prefix_for_dataset "$dataset")"

  if [[ ! -e "$data_file" ]]; then
    echo "Error: data file not found: $data_file" >&2
    exit 1
  fi

  if [[ "$FORCE_GENERATE_QUERIES" == "1" || ! -e "$query_file" ]]; then
    if [[ "$AUTO_GENERATE_QUERIES" == "1" ]]; then
      echo "Generating JTS STRtree KNN queries for $dataset query_limit=$QUERY_LIMIT"
      scripts/generate_jts_strtree_knn_queries.sh \
        "$data_file" \
        "$query_prefix" \
        "$QUERY_LIMIT" \
        "$QUERY_COUNT" \
        "$SEED"
    else
      echo "Error: query file not found: $query_file" >&2
      echo "Set AUTO_GENERATE_QUERIES=1 or generate it manually." >&2
      exit 1
    fi
  fi

  if [[ ! -e "$query_file" ]]; then
    echo "Error: expected generated query file not found: $query_file" >&2
    exit 1
  fi

  echo "Running Fig.17 hybrid workload for $dataset with query_file=$query_file"
  ./build/bench_hybrid_wkt_piece \
    --data_file "$data_file" \
    --query_file "$query_file" \
    --dataset_name "$dataset" \
    --limit "$LIMIT" \
    --workload "$WORKLOAD" \
    --initial_fraction "$INITIAL_FRACTION" \
    --insert_fraction "$INSERT_FRACTION" \
    --insert_batch_percent "$INSERT_BATCH_PERCENT" \
    --insert_order "$INSERT_ORDER" \
    --piece_limit "$PIECE_LIMIT" \
    --cell_size "$CELL_SIZE" \
    --seed "$SEED" \
    --progress_step_percent "$PROGRESS_STEP_PERCENT" \
    --include_lsm_async_glin "$INCLUDE_LSM_ASYNC_GLIN" \
    --include_lsm_segmented_glin "$INCLUDE_LSM_SEGMENTED_GLIN" \
    --include_lsm_segmented4_glin "$INCLUDE_LSM_SEGMENTED4_GLIN" \
    --include_lsm_bg_glin "$INCLUDE_LSM_BG_GLIN" \
    --delta_size "$DELTA_SIZE" \
    --output_csv "$SUMMARY_CSV" \
    --progress_csv "$PROGRESS_CSV" \
    ${append_flag:-}

  append_flag="--append_csv"
done

python3 scripts/plot_fig17_hybrid.py \
  --input_csv "$PROGRESS_CSV" \
  --output_dir "$FIGURE_DIR" \
  --prefix "fig17_hybrid"

echo "Summary CSV:  $SUMMARY_CSV"
echo "Progress CSV: $PROGRESS_CSV"
echo "Figures:      $FIGURE_DIR"
