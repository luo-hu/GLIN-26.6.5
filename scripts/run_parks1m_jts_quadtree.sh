# SELECTIVITY（查询选择性 = 查询框面积占全域比例）
# 0p001pct = 0.001%、0p01pct=0.01%、0p1pct=0.1%、1pct=1%
# 百分比越小 → 查询框越小，命中的公园面越少（小范围点查）；
# 百分比越大 → 查询框越大，大面积框选海量面（大范围面查）；
# 4 档用来测试：小查询 / 大查询场景下四叉树性能变化。
# RELATIONSHIPS（GIS 两大核心空间谓词）
# intersects：相交（只要和查询框有重叠就命中，最常用空间查询）
# contains：包含（目标公园面完全落在查询框内部才算命中）
# 总共组合：4×2 = 8 组对照实验，每组独立跑一次测试、单独生成结果 CSV。
# ./run_parks1m_jts_quadtree.sh    默认参数（数据路径 /mnt/hgfs/parks，读取前 100 万条）

#!/usr/bin/env bash
set -euo pipefail

# Run GEOS Quadtree on PARKS 1M with the fixed JTS STRtree KNN query windows.
#
# Optional environment overrides:
#   DATA_FILE=/mnt/hgfs/parks
#   LIMIT=1000000
#   DATASET_NAME=PARKS
#   QUERY_PREFIX=queries/parks_1m_jts_strtree_knn
#   OUTPUT_PREFIX=results/parks_1m_jts_strtree_knn

DATA_FILE=${DATA_FILE:-/mnt/hgfs/parks}
LIMIT=${LIMIT:-1000000}
DATASET_NAME=${DATASET_NAME:-PARKS}
QUERY_PREFIX=${QUERY_PREFIX:-queries/parks_1m_jts_strtree_knn}
OUTPUT_PREFIX=${OUTPUT_PREFIX:-results/parks_1m_jts_strtree_knn}

SELECTIVITY_TAGS=(0p001pct 0p01pct 0p1pct 1pct)
RELATIONSHIPS=(contains intersects)

if [[ ! -x ./build/bench_geos_quadtree_wkt ]]; then
  echo "Error: ./build/bench_geos_quadtree_wkt not found. Run cmake --build build --target bench_geos_quadtree_wkt first." >&2
  exit 1
fi

mkdir -p "$(dirname "$OUTPUT_PREFIX")"

for selectivity in "${SELECTIVITY_TAGS[@]}"; do
  query_file="${QUERY_PREFIX}_${selectivity}.csv"
  if [[ ! -f "$query_file" ]]; then
    echo "Error: missing query file: $query_file" >&2
    exit 1
  fi

  for relationship in "${RELATIONSHIPS[@]}"; do
    output_csv="${OUTPUT_PREFIX}_${selectivity}_geos_quadtree_${relationship}.csv"
    echo "Running GEOS Quadtree ${relationship} selectivity=${selectivity}"
    ./build/bench_geos_quadtree_wkt \
      --data_file "$DATA_FILE" \
      --dataset_name "$DATASET_NAME" \
      --limit "$LIMIT" \
      --query_file "$query_file" \
      --relationship "$relationship" \
      --output_csv "$output_csv"
  done
done

echo "Done. Raw CSV prefix: ${OUTPUT_PREFIX}_*_geos_quadtree_*.csv"
