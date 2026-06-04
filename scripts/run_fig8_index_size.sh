#!/usr/bin/env bash
set -euo pipefail

# Reproduce Fig. 8 style index-size measurements:
#   GLIN vs Boost R-tree vs GEOS Quadtree, excluding the GLIN piecewise function.
#
# Common overrides:
#   LIMIT=1000000
#   DATASETS="AW LW ROADS PARKS"
#   DATA_ROOT=/mnt/hgfs
#   RESULT_CSV=results/fig8_index_size_1m.csv
#   FIGURE_DIR=figures/fig8_index_size_1m
#   OVERWRITE=1
#   DRY_RUN=1

LIMIT=${LIMIT:-1000000}
SEED=${SEED:-42}
DATA_ROOT=${DATA_ROOT:-/mnt/hgfs}
REAL_WORK_DIR=${REAL_WORK_DIR:-data/real}
SYN_WORK_DIR=${SYN_WORK_DIR:-data/synthetic/glin_geo}
DATASETS=${DATASETS:-"AW LW ROADS PARKS"}
RESULT_CSV=${RESULT_CSV:-results/fig8_index_size_${LIMIT}.csv}
FIGURE_DIR=${FIGURE_DIR:-figures/fig8_index_size_${LIMIT}}
PIECE_LIMIT=${PIECE_LIMIT:-10000}
CELL_XMIN=${CELL_XMIN:--180}
CELL_YMIN=${CELL_YMIN:--90}
CELL_SIZE=${CELL_SIZE:-0.0000005}
OVERWRITE=${OVERWRITE:-0}
DRY_RUN=${DRY_RUN:-0}
AUTO_BUILD=${AUTO_BUILD:-1}
PLOT=${PLOT:-1}

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

ensure_file() {
  local dataset=$1
  local path=$2
  if [[ "$DRY_RUN" != "1" && ! -s "$path" ]]; then
    echo "Error: dataset file missing or empty for $dataset: $path" >&2
    echo "For OSM_AU_POINTS or synthetic datasets, run scripts/run_all_1m.sh once to prepare WKT files." >&2
    exit 1
  fi
}

main() {
  echo "=== GLIN Fig.8 index-size runner ==="
  echo "LIMIT=$LIMIT DATASETS=$DATASETS"
  echo "RESULT_CSV=$RESULT_CSV FIGURE_DIR=$FIGURE_DIR"

  if [[ "$AUTO_BUILD" == "1" ]]; then
    run_cmd cmake -S . -B build
    run_cmd cmake --build build --target bench_index_size_wkt -j2
  fi
  if [[ "$DRY_RUN" != "1" && ! -x ./build/bench_index_size_wkt ]]; then
    echo "Error: missing ./build/bench_index_size_wkt" >&2
    exit 1
  fi

  mkdir -p "$(dirname "$RESULT_CSV")" "$FIGURE_DIR"
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

    echo "[fig8] $dataset -> $RESULT_CSV"
    run_cmd ./build/bench_index_size_wkt \
      --data_file "$data_file" \
      --dataset_name "$dataset" \
      --limit "$LIMIT" \
      --seed "$SEED" \
      --piece_limit "$PIECE_LIMIT" \
      --cell_xmin "$CELL_XMIN" \
      --cell_ymin "$CELL_YMIN" \
      --cell_size "$CELL_SIZE" \
      --output_csv "$RESULT_CSV" \
      "${append_flag[@]}"
  done

  if [[ "$PLOT" == "1" ]]; then
    run_cmd python3 scripts/plot_fig8_index_size.py \
      --input_csv "$RESULT_CSV" \
      --output_dir "$FIGURE_DIR" \
      --prefix fig8_index_size
  fi
}

main "$@"
