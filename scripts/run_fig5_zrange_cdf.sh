#!/usr/bin/env bash
set -euo pipefail

# Reproduce Fig. 5: CDFs of different datasets based on Zmin and Zmax.
#
# Common overrides:
#   LIMIT=1000000
#   DATASETS="AW LW ROADS PARKS OSM_AU_POINTS UNIF_S UNIF_L DIAG_S DIAG_L"
#   CDF_POINTS=1000
#   SYN_WORK_DIR=data/synthetic/fig14_rectangles
#   OVERWRITE=1
#   DRY_RUN=1

LIMIT=${LIMIT:-1000000} # 单个数据集最多读取有效几何体数量；LIMIT=10000000 代表单数据集加载 1000 万条
CDF_POINTS=${CDF_POINTS:-1000} # CDF 曲线采样分位数点数：在 0~100% 区间均匀取 N 个采样点，点数越大绘制的 CDF 曲线越平滑
SEED=${SEED:-42} # 全局随机种子，固定保证实验结果可复现，随机采样数据时用
DATA_ROOT=${DATA_ROOT:-/mnt/hgfs}  # 原始真实数据集根目录：AREAWATER、ROADS、PARKS 等原始 CSV / 二进制所在 VM 共享目录
REAL_WORK_DIR=${REAL_WORK_DIR:-data/real} # 转换后真实 WKT 文件存放目录：OSM 澳洲点位二进制会转为OSM_AU_POINTS.wkt存在此处
SYN_WORK_DIR=${SYN_WORK_DIR:-data/synthetic/fig14_rectangles} # 4 种人工合成矩形数据集（UNIF/DIAG）WKT 自动生成到此目录
DATASETS=${DATASETS:-"AW LW ROADS PARKS OSM_AU_POINTS UNIF_S UNIF_L DIAG_S DIAG_L"}
RESULT_CSV=${RESULT_CSV:-results/fig5_zrange_cdf_${LIMIT}.csv}  # CDF 分位数统计结果输出文件，文件名携带 LIMIT 区分数据量
FIGURE_DIR=${FIGURE_DIR:-figures/fig5_zrange_cdf_${LIMIT}}    # Python 绘图生成的 Fig5 曲线图保存文件夹
CELL_XMIN=${CELL_XMIN:--180}  # 经度空间左边界（全球地理经度最小值）
CELL_YMIN=${CELL_YMIN:--90}   # 纬度空间下边界（全球地理纬度最小值）
CELL_SIZE=${CELL_SIZE:-0.0000005} # GLIN 全局网格单元格边长，用于把原始经纬度归一化到[0,1]区间，计算 Zmin/Zmax 相对跨度
OVERWRITE=${OVERWRITE:-0}   # 功能开关，默认值是0，   1：运行前删除旧 CSV 结果，从头重新统计；0：新数据集结果追加进原有 CSV
DRY_RUN=${DRY_RUN:-0}       # 1=干跑调试：只打印执行命令、不编译、不生成文件、不跑计算，仅预览流程
AUTO_BUILD=${AUTO_BUILD:-1}  # 1 = 自动 cmake 编译 3 个 C++ 程序；0 = 跳过编译（如何已经编译过用代码没有修改，可以跳过编译）
PREPARE_DATA=${PREPARE_DATA:-1} # 1 = 自动预处理：OSM 二进制 bin→WKT；②缺失合成数据集自动生成；0 = 跳过预处理，要求所有 WKT 文件已提前就绪
PLOT=${PLOT:-1} # 1 = 数据跑完自动调用 Python 绘制 CDF 图片；0 = 仅输出 CSV，不绘图

#用到内置子程序说明：
#convert_binary_points_to_wkt：二进制点.bin → WKT 单点极小矩形（OSM_AU_POINTS 专用，和你之前导出的 OSM/ENC point.bin 格式兼容）
#generate_synthetic_rectangles：生成 4 类人工矩形 WKT 数据集
#export_zrange_cdf_wkt：主计算程序：读取 WKT 几何→提取每个 AABB 的 min/max→归一化计算 Zmin/Zmax→按分位数输出 CDF 数据到 CSV

#run_fig5_zrange_cdf常用运行命令示例
#1、1000 万数据，仅跑 4 个真实数据集
#LIMIT=10000000 DATASETS="AW LW ROADS PARKS" bash run_fig5_zrange.sh

#2、只跑 OSM+4 组合成数据集，不重新编译
#LIMIT=2000000 AUTO_BUILD=0 DATASETS="OSM_AU_POINTS UNIF_S UNIF_L DIAG_S DIAG_L" bash run_fig5_zrange.sh

#3、清空历史结果、只导出 CSV 不画图
#OVERWRITE=1 PLOT=0 LIMIT=1000000 bash run_fig5_zrange.sh

#4、调试预览（不实际执行任何操作）
#DRY_RUN=1 bash run_fig5_zrange.sh


run_cmd() {
  if [[ "$DRY_RUN" == "1" ]]; then
    printf '[dry-run]'
    printf ' %q' "$@"
    printf '\n'
  else
    "$@"
  fi
}

dataset_file() {
  case "$1" in
    AW) echo "${DATA_ROOT}/AREAWATER.csv" ;;
    LW) echo "${DATA_ROOT}/LINEARWATER.csv" ;;
    ROADS) echo "${DATA_ROOT}/roads" ;;
    PARKS) echo "${DATA_ROOT}/parks" ;;
    OSM_AU_POINTS) echo "${REAL_WORK_DIR}/osm_australia_1m_point.wkt" ;;
    UNIF_S) echo "${SYN_WORK_DIR}/UNIF_S.wkt" ;;
    UNIF_L) echo "${SYN_WORK_DIR}/UNIF_L.wkt" ;;
    DIAG_S) echo "${SYN_WORK_DIR}/DIAG_S.wkt" ;;
    DIAG_L) echo "${SYN_WORK_DIR}/DIAG_L.wkt" ;;
    *)
      echo "Error: unknown dataset '$1'" >&2
      exit 1
      ;;
  esac
}

prepare_data() {
  mkdir -p "$REAL_WORK_DIR" "$SYN_WORK_DIR"

  if [[ " $DATASETS " == *" OSM_AU_POINTS "* ]]; then
    local osm_wkt
    osm_wkt=$(dataset_file OSM_AU_POINTS)
    if [[ ! -s "$osm_wkt" || "$OVERWRITE" == "1" ]]; then
      echo "[prepare] Convert OSM Australia binary points -> $osm_wkt"
      run_cmd ./build/convert_binary_points_to_wkt \
        --input_file "${DATA_ROOT}/osm_australia_2m_point.bin" \
        --output_file "$osm_wkt" \
        --num "$LIMIT" \
        --dim 2
    fi
  fi

  local need_synthetic=0
  for dataset in UNIF_S UNIF_L DIAG_S DIAG_L; do
    if [[ " $DATASETS " == *" $dataset "* ]]; then
      local path
      path=$(dataset_file "$dataset")
      if [[ ! -s "$path" ]]; then
        need_synthetic=1
      fi
    fi
  done
  if [[ "$need_synthetic" == "1" ]]; then
    echo "[prepare] Generate synthetic rectangle WKT datasets -> $SYN_WORK_DIR"
    run_cmd env SMALL_N="$LIMIT" LARGE_N="$LIMIT" OUT_DIR="$SYN_WORK_DIR" \
      SEED="$SEED" scripts/prepare_synthetic_rectangles.sh
  fi
}

ensure_file() {
  local dataset=$1
  local path=$2
  if [[ "$DRY_RUN" != "1" && ! -s "$path" ]]; then
    echo "Error: dataset file missing or empty for $dataset: $path" >&2
    exit 1
  fi
}

main() {
  echo "=== GLIN Fig.5 Zmin/Zmax CDF runner ==="
  echo "LIMIT=$LIMIT CDF_POINTS=$CDF_POINTS DATASETS=$DATASETS"
  echo "RESULT_CSV=$RESULT_CSV FIGURE_DIR=$FIGURE_DIR"
  echo "SYN_WORK_DIR=$SYN_WORK_DIR"

  if [[ "$AUTO_BUILD" == "1" ]]; then
    run_cmd cmake -S . -B build
    run_cmd cmake --build build --target \
      export_zrange_cdf_wkt \
      convert_binary_points_to_wkt \
      generate_synthetic_rectangles \
      -j2
  fi
  if [[ "$DRY_RUN" != "1" && ! -x ./build/export_zrange_cdf_wkt ]]; then
    echo "Error: missing ./build/export_zrange_cdf_wkt" >&2
    exit 1
  fi

  mkdir -p "$(dirname "$RESULT_CSV")" "$FIGURE_DIR"
  if [[ "$PREPARE_DATA" == "1" ]]; then
    prepare_data
  fi
  if [[ "$OVERWRITE" == "1" && "$DRY_RUN" != "1" ]]; then
    rm -f "$RESULT_CSV"
  fi

  local append_flag=()
  for dataset in $DATASETS; do
    local data_file
    data_file=$(dataset_file "$dataset")
    ensure_file "$dataset" "$data_file"
    if [[ -s "$RESULT_CSV" ]]; then
      append_flag=(--append_csv)
    else
      append_flag=()
    fi

    echo "[fig5] $dataset -> $RESULT_CSV"
    run_cmd ./build/export_zrange_cdf_wkt \
      --data_file "$data_file" \
      --dataset_name "$dataset" \
      --limit "$LIMIT" \
      --cdf_points "$CDF_POINTS" \
      --seed "$SEED" \
      --cell_xmin "$CELL_XMIN" \
      --cell_ymin "$CELL_YMIN" \
      --cell_size "$CELL_SIZE" \
      --output_csv "$RESULT_CSV" \
      "${append_flag[@]}"
  done

  if [[ "$PLOT" == "1" ]]; then
    run_cmd python3 scripts/plot_fig5_zrange_cdf.py \
      --input_csv "$RESULT_CSV" \
      --output_dir "$FIGURE_DIR" \
      --prefix fig5_zrange_cdf
  fi
}

main "$@"
