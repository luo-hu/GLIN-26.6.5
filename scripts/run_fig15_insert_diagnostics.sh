#!/usr/bin/env bash
set -euo pipefail

LIMIT="${LIMIT:-1000000}"
SEED="${SEED:-42}"
PIECE_LIMIT="${PIECE_LIMIT:-10000}"
INCLUDE_BUFFERED_GLIN="${INCLUDE_BUFFERED_GLIN:-0}"
BUFFER_SIZE="${BUFFER_SIZE:-10000}"
INCLUDE_LSM_GLIN="${INCLUDE_LSM_GLIN:-0}"
INCLUDE_LSM_ASYNC_GLIN="${INCLUDE_LSM_ASYNC_GLIN:-0}"
DELTA_SIZE="${DELTA_SIZE:-100000}"
DATASETS="${DATASETS:-AW LW ROADS PARKS}"
RESULT_DIR="${RESULT_DIR:-results/fig15_insert_diagnostics}"
FIGURE_DIR="${FIGURE_DIR:-figures/fig15_insert_diagnostics}"
AUTO_BUILD="${AUTO_BUILD:-1}"
# ========== 新增：仅绘图开关，默认0(正常跑数据)，设为1则只绘图 ==========
PLOT_ONLY="${PLOT_ONLY:-0}"

BOOST_CSV="$RESULT_DIR/boost_strategy_sweep.csv"
ORDER_CSV="$RESULT_DIR/insert_order_sweep.csv"
CELL_CSV="$RESULT_DIR/cell_size_sweep.csv"
SUMMARY_CSV="$RESULT_DIR/fig15_insert_diagnostics_summary.csv"

# 创建目录（绘图也需要目录存在，保留）
mkdir -p "$RESULT_DIR" "$FIGURE_DIR"

# ========== 改动1：仅非绘图模式，才执行编译 ==========
if [[ "$PLOT_ONLY" == "0" && "$AUTO_BUILD" == "1" ]]; then
  cmake --build build --target bench_update_wkt bench_update_wkt_piece -j2
fi

# ========== 改动2：仅非绘图模式，才清空旧CSV（防止覆盖已有数据） ==========
if [[ "$PLOT_ONLY" == "0" ]]; then
  : > "$BOOST_CSV"
  : > "$ORDER_CSV"
  : > "$CELL_CSV"
fi

data_file_for_dataset() {
  case "$1" in
    AW) echo "/mnt/hgfs/AREAWATER.csv" ;;
    LW) echo "/mnt/hgfs/LINEARWATER.csv" ;;
    ROADS) echo "/mnt/hgfs/roads" ;;
    PARKS) echo "/mnt/hgfs/parks" ;;
    *)
      echo "Unknown dataset: $1" >&2
      return 1
      ;;
  esac
}

append_args_for_file() {
  local file="$1"
  if [[ -s "$file" ]]; then
    echo "--append_csv"
  fi
}

# ========== 改动3：仅非绘图模式，才执行性能测试主循环 ==========
if [[ "$PLOT_ONLY" == "0" ]]; then
  for dataset in $DATASETS; do
    data_file="$(data_file_for_dataset "$dataset")"

    # 1. Boost split strategy sweep: linear/quadratic/rstar in one process.
    # GLIN and GEOS Quadtree are included once as reference rows.
    ./build/bench_update_wkt \
      --data_file "$data_file" \
      --dataset_name "$dataset" \
      --limit "$LIMIT" \
      --mode insert \
      --seed "$SEED" \
      --piece_limit "$PIECE_LIMIT" \
      --boost_strategy all \
      --insert_order random \
      --include_buffered_glin "$INCLUDE_BUFFERED_GLIN" \
      --buffer_size "$BUFFER_SIZE" \
    --include_lsm_glin "$INCLUDE_LSM_GLIN" \
    --include_lsm_async_glin "$INCLUDE_LSM_ASYNC_GLIN" \
    --delta_size "$DELTA_SIZE" \
      --output_csv "$BOOST_CSV" \
      $(append_args_for_file "$BOOST_CSV")

    # 2. Insert order sweep: random, original file order, and Zmin order.
    for insert_order in random file zmin; do
      ./build/bench_update_wkt \
        --data_file "$data_file" \
        --dataset_name "$dataset" \
        --limit "$LIMIT" \
        --mode insert \
        --seed "$SEED" \
        --piece_limit "$PIECE_LIMIT" \
        --boost_strategy linear \
        --insert_order "$insert_order" \
        --include_buffered_glin "$INCLUDE_BUFFERED_GLIN" \
        --buffer_size "$BUFFER_SIZE" \
      --include_lsm_glin "$INCLUDE_LSM_GLIN" \
      --include_lsm_async_glin "$INCLUDE_LSM_ASYNC_GLIN" \
      --delta_size "$DELTA_SIZE" \
        --output_csv "$ORDER_CSV" \
        $(append_args_for_file "$ORDER_CSV")

      ./build/bench_update_wkt_piece \
        --data_file "$data_file" \
        --dataset_name "$dataset" \
        --limit "$LIMIT" \
        --mode insert \
        --seed "$SEED" \
        --piece_limit "$PIECE_LIMIT" \
        --insert_order "$insert_order" \
        --output_csv "$ORDER_CSV" \
        --append_csv
    done

    # 3. GLIN cell size sweep. Baselines are skipped because Boost/Quadtree do
    # not use GLIN's Z-order cell size.
    #for cell_size in 5e-5 5e-6 5e-7 5e-8; do  综合来看网格划分尺寸设置为5e-5更全面
    for cell_size in 5e-5; do
      ./build/bench_update_wkt \
        --data_file "$data_file" \
        --dataset_name "$dataset" \
        --limit "$LIMIT" \
        --mode insert \
        --seed "$SEED" \
        --piece_limit "$PIECE_LIMIT" \
        --cell_size "$cell_size" \
        --insert_order random \
        --include_baselines 0 \
        --include_buffered_glin "$INCLUDE_BUFFERED_GLIN" \
        --buffer_size "$BUFFER_SIZE" \
      --include_lsm_glin "$INCLUDE_LSM_GLIN" \
      --include_lsm_async_glin "$INCLUDE_LSM_ASYNC_GLIN" \
      --delta_size "$DELTA_SIZE" \
        --output_csv "$CELL_CSV" \
        $(append_args_for_file "$CELL_CSV")

      ./build/bench_update_wkt_piece \
        --data_file "$data_file" \
        --dataset_name "$dataset" \
        --limit "$LIMIT" \
        --mode insert \
        --seed "$SEED" \
        --piece_limit "$PIECE_LIMIT" \
        --cell_size "$cell_size" \
        --insert_order random \
        --output_csv "$CELL_CSV" \
        --append_csv
    done
  done
fi

# 无论哪种模式，最后都执行绘图（核心保留）
python3 scripts/plot_fig15_insert_diagnostics.py \
  --boost_csv "$BOOST_CSV" \
  --order_csv "$ORDER_CSV" \
  --cell_csv "$CELL_CSV" \
  --output_dir "$FIGURE_DIR" \
  --summary_csv "$SUMMARY_CSV"
