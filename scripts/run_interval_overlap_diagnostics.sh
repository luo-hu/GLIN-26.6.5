#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  cat <<'USAGE'
用法：
  ./scripts/run_interval_overlap_diagnostics.sh

这个脚本用于跑 IO_BLOCK_MBR / IO_OVERFLOW 的 Intersects 查询实验，并和
GLIN_PIECEWISE、Boost_Rtree、GEOS_Quadtree 做对比。

重要提醒：
  1. 这个脚本主要跑 Intersects。
  2. 原始 GLIN 在当前仓库里对应 contains，不是 intersects，所以默认不放进排名表。
  3. 运行参数通过“环境变量=值”的方式写在命令前面。

最常用命令：

  只跑已有 1% query，不重新生成 query：
    RESET_RESULTS=1 SELECTIVITY_TAGS=1pct BLOCK_SIZES=1024 \
      ./scripts/run_interval_overlap_diagnostics.sh

  跑 4 种选择性，并自动生成缺失 query：
    RESET_RESULTS=1 AUTO_GENERATE_QUERIES=1 JTS_JAVA_HEAP=20g \
    SELECTIVITY_TAGS="0p001pct 0p01pct 0p1pct 1pct" \
    BLOCK_SIZES="256 512 1024 2048 4096" \
      ./scripts/run_interval_overlap_diagnostics.sh

  只根据已有 CSV 重新汇总和画图，不重新跑实验：
    RUN_BENCHMARKS=0 RESET_RESULTS=0 \
    RESULT_DIR=results/interval_overlap_2000000_with_quadtree \
    FIGURE_DIR=figures/interval_overlap_2000000_with_quadtree \
      ./scripts/run_interval_overlap_diagnostics.sh

  只绘图/汇总，不重新跑 benchmark，并且排除 ZGAP_WIDE：
    RUN_BENCHMARKS=0 RESET_RESULTS=0 \
    EXCLUDE_DATASETS=ZGAP_WIDE \
    RESULT_DIR=results/interval_overlap_full_1000000 \
    FIGURE_DIR=figures/interval_overlap_full_1000000_no_zgap \
      ./scripts/run_interval_overlap_diagnostics.sh
    注意：EXCLUDE_DATASETS=ZGAP_WIDE 只是不把它写进新的 summary 和图里，不会删除原始 raw CSV。

  这个命令和上面那个其实是一个意思，都是只绘图（只绘制UNIF_S和DIAG_S两个数据集的图像）
  python3 scripts/plot_interval_overlap_diagnostics.py \
  --input results/interval_overlap_full_1000000/interval_overlap_summary.csv \
  --output_dir figures/interval_overlap_full_1000000_synthetic_only \
  --figure_prefix interval_overlap \
  --exclude_datasets "AW LW ROADS PARKS ZGAP_WIDE"



  跑合成矩形数据，如果数据和 query 不存在就自动生成：
    RESET_RESULTS=1 DATASETS="UNIF_S UNIF_L DIAG_S DIAG_L" \
    LIMIT=1000000 QUERY_LIMIT=1000000 \
    PREPARE_DATA=1 AUTO_GENERATE_QUERIES=1 SYNTHETIC_KIND=rectangles \
    QUERY_ROOT=queries/interval_overlap_synthetic_1000000 \
    RESULT_DIR=results/interval_overlap_synthetic_1000000 \
    FIGURE_DIR=figures/interval_overlap_synthetic_1000000 \
    SELECTIVITY_TAGS="0p001pct 0p01pct 0p1pct 1pct" \
    BLOCK_SIZES="512 1024 2048" \
      ./scripts/run_interval_overlap_diagnostics.sh

  跑完整的测试：
    RESET_RESULTS=1 \
    OVERWRITE=1 \
    PREPARE_DATA=1 \
    AUTO_GENERATE_QUERIES=1 \
    JTS_JAVA_HEAP=20g \
    DATASETS="ROADS PARKS UNIF_S UNIF_L DIAG_S DIAG_L ZGAP_WIDE ZGAP_MIXED" \
    LIMIT=1000000 \
    QUERY_LIMIT=1000000 \
    SYNTHETIC_KIND=rectangles \
    SELECTIVITY_TAGS="0p001pct 0p01pct 0p1pct 1pct" \
    BLOCK_SIZES="512 1024 2048" \
    INCLUDE_QUADTREE=1 \
    RESULT_DIR=results/interval_overlap_full_1000000 \
    FIGURE_DIR=figures/interval_overlap_full_1000000 \
    QUERY_ROOT=queries/interval_overlap_full_1000000 \
    ./scripts/run_interval_overlap_diagnostics.sh

常用参数：
  DATASETS
    要跑哪些数据集。默认是 "ROADS PARKS"。
    可选：AW LW ROADS PARKS OSM_AU_POINTS UNIF_S UNIF_L DIAG_S DIAG_L ZGAP_WIDE ZGAP_MIXED

  LIMIT
    每个数据集最多加载多少条 geometry。默认 2000000。

  QUERY_LIMIT
    生成 query 时按多少条数据来生成。默认等于 LIMIT。

  SELECTIVITY_TAGS
    选择性，也可以理解为 query 大小/答案规模。
    可选：0p001pct 0p01pct 0p1pct 1pct
    默认：1pct

  BLOCK_SIZES
    IntervalOverlapIndex 每个 block 放多少条记录。
    默认：1024

  AUTO_GENERATE_QUERIES
    0：缺 query 文件就报错。
    1：缺 query 文件就自动调用 JTS query generator 生成。
    默认：0

  PREPARE_DATA
    0：不自动生成数据。
    1：如果 synthetic 数据不存在，就自动生成。
    默认：0

  SYNTHETIC_KIND
    rectangles：生成矩形数据，适合 Intersects。
    points：生成点数据，适合 pipeline/smoke，不太适合作为 Intersects 主实验。
    默认：rectangles

  RUN_BENCHMARKS
    1：正常跑实验。
    0：不跑实验，只汇总已有 CSV 和画图。
    默认：1

  EXCLUDE_DATASETS
    汇总和画图时排除哪些数据集。多个值用空格分开。
    例如：EXCLUDE_DATASETS=ZGAP_WIDE
    或：EXCLUDE_DATASETS="ZGAP_WIDE DIAG_L"

  PLOT_RESULTS
    1：汇总后画图。
    0：只生成 summary CSV，不画图。
    默认：1

  RESET_RESULTS
    1：运行前删除 RESULT_DIR 下旧 CSV，适合正式重跑。
    0：保留旧 CSV，适合只画图或续跑。
    默认：1

  OVERWRITE
    0：如果某个 raw CSV 已存在，就跳过对应 benchmark。
    1：即使 raw CSV 已存在，也重新跑。
    默认：0

  INCLUDE_QUADTREE
    1：加入 GEOS_Quadtree 对比。
    0：不跑 GEOS_Quadtree。
    默认：1

  INCLUDE_IO_BLOCK_MBR
    1：运行 IO_BLOCK_MBR，也就是当前的 maxZmax + block MBR + record MBR 版本。
    默认：1

  INCLUDE_IO_OVERFLOW
    1：运行 IO_OVERFLOW，也就是 main index + fat-object overflow R-tree 版本。
    默认：0

  OVERFLOW_FRACTIONS
    IO_OVERFLOW 中按 Zmax-Zmin 跨度分流到 overflow 的对象比例。
    多个值用空格分开，例如 "0.001 0.01 0.05"。
    默认：0.01

  INCLUDE_GLIN_CONTAINS
    1：额外跑原始 GLIN contains sanity check。
    注意：它不是 Intersects，不能放进 Intersects 排名。
    默认：0

  ZGAP_WIDE
    这是 Zmin/Zmax gap 压力数据集。
    如果没有它，不影响默认 ROADS/PARKS。
    如果想跑它：
      DATASETS=ZGAP_WIDE PREPARE_DATA=1 AUTO_GENERATE_QUERIES=1 \
        ./scripts/run_interval_overlap_diagnostics.sh

  ZGAP_MIXED
    这是专门给 IO_OVERFLOW 准备的 mixed fat-object 数据集。
    它不是所有对象都很大，而是大多数小对象 + 少量长/胖对象。
    如果想跑它：
      DATASETS=ZGAP_MIXED PREPARE_DATA=1 AUTO_GENERATE_QUERIES=1 \
      INCLUDE_IO_OVERFLOW=1 OVERFLOW_FRACTIONS="0.001 0.01 0.05" \
        ./scripts/run_interval_overlap_diagnostics.sh

输出：
  RESULT_DIR/interval_overlap_summary.csv
  FIGURE_DIR/interval_overlap_*_avg_total_ms.png
  FIGURE_DIR/interval_overlap_*_candidate_answer_ratio.png
  FIGURE_DIR/interval_overlap_*_pruning_detail.png
USAGE
  exit 0
fi

# =========================
# 1. 实验规模和数据集
# =========================
# LIMIT：每个数据集最多加载多少条 geometry。真实 2M 实验默认 2000000。
LIMIT="${LIMIT:-2000000}"

# QUERY_LIMIT：生成 query 文件时用多少条数据。通常等于 LIMIT。
QUERY_LIMIT="${QUERY_LIMIT:-$LIMIT}"

# DATASETS：要跑哪些数据集。
# 默认只跑真实数据 ROADS/PARKS，避免没准备 synthetic 时一上来就报错。
# 可选：AW LW ROADS PARKS OSM_AU_POINTS UNIF_S UNIF_L DIAG_S DIAG_L ZGAP_WIDE ZGAP_MIXED
DATASETS="${DATASETS:-ROADS PARKS}"

# DATA_ROOT：真实数据所在目录。ROADS/PARKS 默认从 /mnt/hgfs 读取。
DATA_ROOT="${DATA_ROOT:-/mnt/hgfs}"

# QUERY_ROOT：query CSV 所在目录。文件名形如 ROADS_jts_strtree_knn_1pct.csv。
QUERY_ROOT="${QUERY_ROOT:-queries/fig17_hybrid_${QUERY_LIMIT}}"

# RESULT_DIR：每个方法的 raw CSV 和 summary CSV 输出目录。
RESULT_DIR="${RESULT_DIR:-results/interval_overlap_${LIMIT}}"

# =========================
# 2. IntervalOverlapIndex 参数
# =========================
# BLOCK_SIZES：每个 block 放多少条记录。多个值用空格分开。
# 例如 BLOCK_SIZES="256 512 1024 2048 4096"
BLOCK_SIZES="${BLOCK_SIZES:-${BLOCK_SIZE:-1024}}"

# SELECTIVITY_TAGS：选择性，也就是 query 大小/答案规模。
# 可选：0p001pct 0p01pct 0p1pct 1pct
# 例：SELECTIVITY_TAGS="0p001pct 0p01pct 0p1pct 1pct"
SELECTIVITY_TAGS="${SELECTIVITY_TAGS:-1pct}"

# PIECE_LIMIT：GLIN_PIECEWISE 的 piece 大小，不是 IntervalOverlapIndex 的 block size。
PIECE_LIMIT="${PIECE_LIMIT:-10000}"

# CELL_SIZE：Z-order 网格大小，保持和 GLIN 论文/现有实验一致。
CELL_SIZE="${CELL_SIZE:-0.0000005}"

# SEED：生成 query 或 synthetic 数据时的随机种子。
SEED="${SEED:-42}"

# =========================
# 3. 是否重跑、是否画图
# =========================
# RUN_BENCHMARKS=0 时不会跑 C++ benchmark，只汇总已有 CSV 并画图。
RUN_BENCHMARKS="${RUN_BENCHMARKS:-1}"

# RESET_RESULTS=1 会先删除 RESULT_DIR 下旧 CSV。正式重跑建议设为 1。
# 只画图时必须设为 0，否则会把已有结果删掉。
RESET_RESULTS="${RESET_RESULTS:-1}"

# OVERWRITE=0 表示如果某个 raw CSV 已经存在，就跳过它，方便续跑。
OVERWRITE="${OVERWRITE:-0}"

# AUTO_BUILD=1 会自动编译所需 target。
AUTO_BUILD="${AUTO_BUILD:-1}"

# PLOT_RESULTS=1 会在 summary 后自动画图。
PLOT_RESULTS="${PLOT_RESULTS:-1}"

# EXCLUDE_DATASETS：只影响 summary/plot，不删除 raw CSV。
# 例如 EXCLUDE_DATASETS=ZGAP_WIDE 可以画图时排除压力数据集。
EXCLUDE_DATASETS="${EXCLUDE_DATASETS:-}"

# FIGURE_DIR：图片输出目录。
FIGURE_DIR="${FIGURE_DIR:-figures/interval_overlap_${LIMIT}}"

# INCLUDE_QUADTREE=1：加入 GEOS_Quadtree 作为 baseline。
INCLUDE_QUADTREE="${INCLUDE_QUADTREE:-1}"

# INCLUDE_IO_BLOCK_MBR=1：运行当前基础增强版 IO。
INCLUDE_IO_BLOCK_MBR="${INCLUDE_IO_BLOCK_MBR:-1}"

# INCLUDE_IO_OVERFLOW=1：运行 main index + fat-object overflow R-tree 版本。
INCLUDE_IO_OVERFLOW="${INCLUDE_IO_OVERFLOW:-0}"

# OVERFLOW_FRACTIONS：长对象分流比例。
OVERFLOW_FRACTIONS="${OVERFLOW_FRACTIONS:-0.01}"

# INCLUDE_GLIN_CONTAINS 只做原始 GLIN 的 contains sanity check。
# 注意：原始 GLIN 当前不是 Intersects，不能和 Intersects 方法放在同一张排名表。
# RUN_CONTAINS 是为了兼容 scripts/run_all_1m.sh 的旧参数名。
INCLUDE_GLIN_CONTAINS="${INCLUDE_GLIN_CONTAINS:-${RUN_CONTAINS:-0}}"

# =========================
# 4. 自动准备数据和 query
# =========================
# PREPARE_DATA=1：如果 synthetic 数据不存在，就自动生成。
# 默认 0，避免真实数据实验时意外生成大文件。
PREPARE_DATA="${PREPARE_DATA:-0}"

# AUTO_GENERATE_QUERIES=1：缺 query CSV 时自动生成。
# 默认 0，所以缺 query 文件会报错并提醒你打开这个开关。
AUTO_GENERATE_QUERIES="${AUTO_GENERATE_QUERIES:-0}"

# QUERY_COUNT：每个 selectivity 生成多少个 query window。
QUERY_COUNT="${QUERY_COUNT:-100}"

# SYNTHETIC_KIND：合成数据类型。
# rectangles 更适合 Intersects；points 主要适合 pipeline/smoke。
SYNTHETIC_KIND="${SYNTHETIC_KIND:-rectangles}"

# REAL_WORK_DIR：由二进制真实点数据转换出的 WKT 放这里。
REAL_WORK_DIR="${REAL_WORK_DIR:-data/real}"

# SYN_WORK_DIR：UNIF/DIAG synthetic WKT 放这里。
SYN_WORK_DIR="${SYN_WORK_DIR:-data/synthetic/rectangles}"

# ZGAP_WORK_DIR：Zmin/Zmax gap 压力数据集放这里。
ZGAP_WORK_DIR="${ZGAP_WORK_DIR:-data/synthetic/zrange_gap}"

mkdir -p "$RESULT_DIR"

if [[ "$RUN_BENCHMARKS" == "0" && "$RESET_RESULTS" == "1" ]]; then
  echo "Error: RUN_BENCHMARKS=0 时不能使用 RESET_RESULTS=1。" >&2
  echo "原因：只画图需要保留已有 CSV，RESET_RESULTS=1 会删除它们。" >&2
  echo "请改成：RUN_BENCHMARKS=0 RESET_RESULTS=0 ..." >&2
  exit 1
fi

if [[ "$AUTO_BUILD" == "1" && ( "$RUN_BENCHMARKS" == "1" || "$PREPARE_DATA" == "1" ) ]]; then
  cmake --build build --target \
    bench_interval_overlap_wkt \
    bench_glin_wkt \
    bench_glin_wkt_piece \
    bench_boost_rtree_wkt \
    bench_geos_quadtree_wkt \
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
  [ZGAP_MIXED]="$ZGAP_WORK_DIR/ZGAP_MIXED.wkt"
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

should_run_file() {
  local path="$1"
  [[ "$OVERWRITE" == "1" || ! -s "$path" ]]
}

query_file_for_dataset() {
  local dataset="$1"
  local tag="$2"
  echo "$QUERY_ROOT/${dataset}_jts_strtree_knn_${tag}.csv"
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
      if [[ " $DATASETS " == *" $synthetic "* && ! -s "$(data_file_for_dataset "$synthetic")" ]]; then
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
    if [[ ! -s "$zgap_wkt" ]]; then
      NUM="$LIMIT" OUT_DIR="$ZGAP_WORK_DIR" NAME=ZGAP_WIDE SEED="$SEED" AUTO_BUILD=0 \
        scripts/prepare_zrange_gap_dataset.sh
    fi
  fi

  if [[ " $DATASETS " == *" ZGAP_MIXED "* ]]; then
    local mixed_wkt="$ZGAP_WORK_DIR/ZGAP_MIXED.wkt"
    if [[ ! -s "$mixed_wkt" ]]; then
      NUM="$LIMIT" OUT_DIR="$ZGAP_WORK_DIR" NAME=ZGAP_MIXED SEED="$SEED" AUTO_BUILD=0 \
        scripts/prepare_zrange_mixed_dataset.sh
    fi
  fi
}

generate_queries_if_needed() {
  local dataset="$1"
  local data_file="$2"
  local all_queries_exist=1
  for tag in $SELECTIVITY_TAGS; do
    if [[ ! -s "$(query_file_for_dataset "$dataset" "$tag")" ]]; then
      all_queries_exist=0
    fi
  done
  if [[ "$all_queries_exist" == "1" ]]; then
    return
  fi
  if [[ "$AUTO_GENERATE_QUERIES" != "1" ]]; then
    echo "Error: missing query file for $dataset under $QUERY_ROOT" >&2
    echo "Set AUTO_GENERATE_QUERIES=1 to generate ${dataset}_jts_strtree_knn_*.csv." >&2
    exit 1
  fi

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
      echo "如果不想跑这个数据集，请用 DATASETS=\"ROADS PARKS\" 指定要跑的数据集。" >&2
      exit 1
    fi
    generate_queries_if_needed "$dataset" "$data_file"

    for tag in $SELECTIVITY_TAGS; do
      query_file="$(query_file_for_dataset "$dataset" "$tag")"
      if [[ ! -e "$query_file" ]]; then
        echo "Error: query file not found: $query_file" >&2
        echo "请加 AUTO_GENERATE_QUERIES=1 自动生成 query，或减少 SELECTIVITY_TAGS。" >&2
        exit 1
      fi

      for block_size in $BLOCK_SIZES; do
        if [[ "$INCLUDE_IO_BLOCK_MBR" == "1" ]]; then
          block_mbr_csv="$RESULT_DIR/${dataset}_${tag}_b${block_size}_io_block_mbr.csv"
          if should_run_file "$block_mbr_csv"; then
            echo "Running IO_BLOCK_MBR for $dataset $tag block=$block_size"
            ./build/bench_interval_overlap_wkt \
              --data_file "$data_file" \
              --query_file "$query_file" \
              --dataset_name "$dataset" \
              --limit "$LIMIT" \
              --block_size "$block_size" \
              --cell_size "$CELL_SIZE" \
              --variant block_mbr \
              --seed "$SEED" \
              --output_csv "$block_mbr_csv"
          fi
        fi

        if [[ "$INCLUDE_IO_OVERFLOW" == "1" ]]; then
          for overflow_fraction in $OVERFLOW_FRACTIONS; do
            overflow_tag="${overflow_fraction//./p}"
            overflow_csv="$RESULT_DIR/${dataset}_${tag}_b${block_size}_of${overflow_tag}_io_overflow.csv"
            if should_run_file "$overflow_csv"; then
              echo "Running IO_OVERFLOW for $dataset $tag block=$block_size overflow=$overflow_fraction"
              ./build/bench_interval_overlap_wkt \
                --data_file "$data_file" \
                --query_file "$query_file" \
                --dataset_name "$dataset" \
                --limit "$LIMIT" \
                --block_size "$block_size" \
                --cell_size "$CELL_SIZE" \
                --variant overflow \
                --overflow_fraction "$overflow_fraction" \
                --seed "$SEED" \
                --output_csv "$overflow_csv"
            fi
          done
        fi
      done

      glin_piece_csv="$RESULT_DIR/${dataset}_${tag}_glin_piecewise.csv"
      if should_run_file "$glin_piece_csv"; then
        echo "Running GLIN-piecewise for $dataset $tag"
        ./build/bench_glin_wkt_piece \
          --data_file "$data_file" \
          --query_file "$query_file" \
          --dataset_name "$dataset" \
          --limit "$LIMIT" \
          --piece_limit "$PIECE_LIMIT" \
          --cell_size "$CELL_SIZE" \
          --seed "$SEED" \
          --output_csv "$glin_piece_csv"
      fi

      boost_csv="$RESULT_DIR/${dataset}_${tag}_boost_rtree.csv"
      if should_run_file "$boost_csv"; then
        echo "Running Boost R-tree for $dataset $tag"
        ./build/bench_boost_rtree_wkt \
          --data_file "$data_file" \
          --query_file "$query_file" \
          --dataset_name "$dataset" \
          --limit "$LIMIT" \
          --relationship intersects \
          --seed "$SEED" \
          --output_csv "$boost_csv"
      fi

      if [[ "$INCLUDE_QUADTREE" == "1" ]]; then
        quadtree_csv="$RESULT_DIR/${dataset}_${tag}_geos_quadtree.csv"
        if should_run_file "$quadtree_csv"; then
          echo "Running GEOS Quadtree for $dataset $tag"
          ./build/bench_geos_quadtree_wkt \
            --data_file "$data_file" \
            --query_file "$query_file" \
            --dataset_name "$dataset" \
            --limit "$LIMIT" \
            --relationship intersects \
            --seed "$SEED" \
            --output_csv "$quadtree_csv"
        fi
      fi

      if [[ "$INCLUDE_GLIN_CONTAINS" == "1" ]]; then
        glin_contains_csv="$RESULT_DIR/${dataset}_${tag}_glin_contains_not_intersects.csv"
        if should_run_file "$glin_contains_csv"; then
          echo "Running original GLIN contains sanity check for $dataset $tag"
          ./build/bench_glin_wkt \
            --data_file "$data_file" \
            --query_file "$query_file" \
            --dataset_name "$dataset" \
            --limit "$LIMIT" \
            --piece_limit "$PIECE_LIMIT" \
            --cell_size "$CELL_SIZE" \
            --seed "$SEED" \
            --output_csv "$glin_contains_csv"
        fi
      fi
    done
  done
else
  echo "RUN_BENCHMARKS=0：跳过 C++ benchmark，只汇总已有 CSV 并画图。"
fi

python3 scripts/summarize_interval_overlap_diagnostics.py \
  --result_dir "$RESULT_DIR" \
  --output_csv "$RESULT_DIR/interval_overlap_summary.csv" \
  --exclude_datasets "$EXCLUDE_DATASETS"

if [[ "$PLOT_RESULTS" == "1" ]]; then
  python3 scripts/plot_interval_overlap_diagnostics.py \
    --input "$RESULT_DIR/interval_overlap_summary.csv" \
    --output_dir "$FIGURE_DIR" \
    --figure_prefix "interval_overlap" \
    --exclude_datasets "$EXCLUDE_DATASETS"
fi

echo "Result dir: $RESULT_DIR"
echo "Summary:    $RESULT_DIR/interval_overlap_summary.csv"
if [[ "$PLOT_RESULTS" == "1" ]]; then
  echo "Figures:    $FIGURE_DIR"
fi
