#!/usr/bin/env bash
set -euo pipefail

LIMIT="${LIMIT:-1000000}" # 单个数据集最多读取有效几何体数量；LIMIT=10000000 代表单数据集加载 1000 万条
SEED="${SEED:-42}"   # 全局随机种子，固定保证实验结果可复现，随机采样数据时用
PROGRESS_STEP_PERCENT="${PROGRESS_STEP_PERCENT:-10}" # 进度采样步长 %：每完成总更新量的 N%，记录一次瞬时性能写入PROGRESS_CSV；设为 10 即每 10% 打点，折线图数据更密集
PIECE_LIMIT="${PIECE_LIMIT:-10000}" # 仅作用分段 GLIN (GLIN_PIECEWISE)：单个分片存储上限，满容量自动分裂分片，GLIN 分片超参
DATASETS="${DATASETS:-AW LW ROADS PARKS}"  # 数据集
SUMMARY_CSV="${SUMMARY_CSV:-results/fig15_16_1m_updates_with_progress.csv}" # 汇总结果文件：存储全量更新最终平均耗时、吞吐，用于生成柱状图
PROGRESS_CSV="${PROGRESS_CSV:-results/fig15_16_1m_update_progress.csv}" # 分步时序结果文件：每 X% 更新进度的实时性能，用于生成更新曲线折线图
FIGURE_DIR="${FIGURE_DIR:-figures/fig15_16_update_curves}"   # 图片保存路径 折线图图片输出目录；末尾两个_SUMMARY_CSV是 python 绘图中间汇总表
BAR_FIGURE_DIR="${BAR_FIGURE_DIR:-figures/fig15_16_updates}" # 图片保存路径 柱状图图片输出目录；末尾两个_SUMMARY_CSV是 python 绘图中间汇总表
PROGRESS_SUMMARY_CSV="${PROGRESS_SUMMARY_CSV:-results/fig15_16_1m_update_progress_summary.csv}"

BAR_SUMMARY_CSV="${BAR_SUMMARY_CSV:-results/fig15_16_1m_updates_summary.csv}"
AUTO_BUILD="${AUTO_BUILD:-1}" # 1 = 自动 cmake 编译 3 个 C++ 程序；0 = 跳过编译（如何已经编译过用代码没有修改，可以跳过编译）

#如果你要输入指定数据集应该加上引号  LIMIT=5000000 DATASETS="ROADS PARKS" PROGRESS_STEP_PERCENT=5 AUTO_BUILD=0 ./scripts/run_fig15_16_update_progress_1m.sh

if [[ "$AUTO_BUILD" == "1" ]]; then
  cmake --build build --target bench_update_wkt bench_update_wkt_piece -j2
fi

mkdir -p "$(dirname "$SUMMARY_CSV")" "$(dirname "$PROGRESS_CSV")" "$FIGURE_DIR"
: > "$SUMMARY_CSV"
: > "$PROGRESS_CSV"

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

first_write=1
for dataset in $DATASETS; do
  data_file="$(data_file_for_dataset "$dataset")"
  append_args=()
  if [[ "$first_write" == "0" ]]; then
    append_args=(--append_csv)
  fi

  ./build/bench_update_wkt \
    --data_file "$data_file" \
    --dataset_name "$dataset" \
    --limit "$LIMIT" \
    --mode both \
    --seed "$SEED" \
    --piece_limit "$PIECE_LIMIT" \
    --progress_step_percent "$PROGRESS_STEP_PERCENT" \
    --output_csv "$SUMMARY_CSV" \
    --progress_csv "$PROGRESS_CSV" \
    "${append_args[@]}"

  ./build/bench_update_wkt_piece \
    --data_file "$data_file" \
    --dataset_name "$dataset" \
    --limit "$LIMIT" \
    --mode both \
    --seed "$SEED" \
    --piece_limit "$PIECE_LIMIT" \
    --progress_step_percent "$PROGRESS_STEP_PERCENT" \
    --output_csv "$SUMMARY_CSV" \
    --progress_csv "$PROGRESS_CSV" \
    --append_csv

  first_write=0
done

python3 scripts/plot_fig15_16_update_curves.py \
  --inputs "$PROGRESS_CSV" \
  --output_dir "$FIGURE_DIR" \
  --summary_csv "$PROGRESS_SUMMARY_CSV"

python3 scripts/plot_fig15_16_updates.py \
  --inputs "$SUMMARY_CSV" \
  --output_dir "$BAR_FIGURE_DIR" \
  --summary_csv "$BAR_SUMMARY_CSV"