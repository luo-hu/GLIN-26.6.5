#!/usr/bin/env bash
set -euo pipefail

# One-command 1M experiment runner for real and synthetic GLIN datasets.
#
# Default datasets:
#   AW, LW, ROADS, PARKS, OSM_AU_POINTS, UNIF_S, UNIF_L, DIAG_S, DIAG_L
#
# Common overrides:
#   LIMIT=1000000
#   QUERY_COUNT=100
#   SEED=42
#   DATASETS="AW PARKS UNIF_S"
#   OVERWRITE=1
#   DRY_RUN=1
#   RUN_CONTAINS=1
#   RUN_INTERSECTS=1
#   GENERATE_QUERIES=1
#   RUN_BENCHMARKS=1
#   SUMMARIZE=1
#   PLOT=1
#   AUTO_BUILD=1
#   SYNTHETIC_KIND=points    # points or rectangles

LIMIT=${LIMIT:-1000000}
QUERY_COUNT=${QUERY_COUNT:-100}
SEED=${SEED:-42}
DATA_ROOT=${DATA_ROOT:-/mnt/hgfs}
RESULT_DIR=${RESULT_DIR:-results/all_1m}
QUERY_DIR=${QUERY_DIR:-queries/all_1m}
FIGURE_DIR=${FIGURE_DIR:-figures/all_1m}
REAL_WORK_DIR=${REAL_WORK_DIR:-data/real}
SYN_WORK_DIR=${SYN_WORK_DIR:-data/synthetic/glin_geo}
ZGAP_WORK_DIR=${ZGAP_WORK_DIR:-data/synthetic/zrange_gap}
SUMMARY_CSV=${SUMMARY_CSV:-results/all_1m_summary.csv}
DATASETS=${DATASETS:-"AW LW ROADS PARKS OSM_AU_POINTS UNIF_S UNIF_L DIAG_S DIAG_L"}
SYNTHETIC_KIND=${SYNTHETIC_KIND:-points}

OVERWRITE=${OVERWRITE:-0}
DRY_RUN=${DRY_RUN:-0}
PREPARE_DATA=${PREPARE_DATA:-1}
GENERATE_QUERIES=${GENERATE_QUERIES:-1}
RUN_BENCHMARKS=${RUN_BENCHMARKS:-1}
SUMMARIZE=${SUMMARIZE:-1}
PLOT=${PLOT:-1}
RUN_CONTAINS=${RUN_CONTAINS:-1}
RUN_INTERSECTS=${RUN_INTERSECTS:-1}
AUTO_BUILD=${AUTO_BUILD:-1}

SELECTIVITY_TAGS=(0p001pct 0p01pct 0p1pct 1pct)

run_cmd() {
  if [[ "$DRY_RUN" == "1" ]]; then
    printf '[dry-run]'
    printf ' %q' "$@"
    printf '\n'
  else
    "$@"
  fi
}

ensure_executable() {
  local path=$1
  if [[ ! -x "$path" ]]; then
    echo "Error: missing executable $path. Run cmake/build first." >&2
    exit 1
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
    ZGAP_WIDE) echo "${ZGAP_WORK_DIR}/ZGAP_WIDE.wkt" ;;
    *)
      echo "Error: unknown dataset '$1'" >&2
      exit 1
      ;;
  esac
}

query_prefix() {
  echo "${QUERY_DIR}/$1_jts_strtree_knn"
}

result_path() {
  local dataset=$1
  local selectivity=$2
  local index=$3
  local relationship=$4
  echo "${RESULT_DIR}/${dataset}_${selectivity}_${index}_${relationship}.csv"
}

should_run_file() {
  local path=$1
  [[ "$OVERWRITE" == "1" || ! -s "$path" ]]
}

prepare_data() {
  mkdir -p "$REAL_WORK_DIR" "$SYN_WORK_DIR" "$RESULT_DIR" "$QUERY_DIR" "$FIGURE_DIR"

  if [[ " $DATASETS " == *" OSM_AU_POINTS "* ]]; then
    local osm_wkt
    osm_wkt=$(dataset_file OSM_AU_POINTS)
    if should_run_file "$osm_wkt"; then
      echo "[prepare] Convert OSM Australia binary points -> $osm_wkt"
      run_cmd ./build/convert_binary_points_to_wkt \
        --input_file "${DATA_ROOT}/osm_australia_2m_point.bin" \
        --output_file "$osm_wkt" \
        --num "$LIMIT" \
        --dim 2
    fi
  fi

  if [[ " $DATASETS " == *" UNIF_S "* || " $DATASETS " == *" UNIF_L "* || \
        " $DATASETS " == *" DIAG_S "* || " $DATASETS " == *" DIAG_L "* ]]; then
    local missing=0
    for synthetic in UNIF_S UNIF_L DIAG_S DIAG_L; do
      if should_run_file "$(dataset_file "$synthetic")"; then
        missing=1
      fi
    done
    if [[ "$missing" == "1" ]]; then
      if [[ "$SYNTHETIC_KIND" == "points" ]]; then
        echo "[prepare] Generate synthetic geo point datasets -> $SYN_WORK_DIR"
        run_cmd env SMALL_N="$LIMIT" LARGE_N="$LIMIT" OUT_DIR="$SYN_WORK_DIR" \
          SEED="$SEED" FORMAT=wkt scripts/prepare_glin_synthetic_geo_points.sh
      elif [[ "$SYNTHETIC_KIND" == "rectangles" ]]; then
        echo "[prepare] Generate synthetic rectangle datasets -> $SYN_WORK_DIR"
        run_cmd env SMALL_N="$LIMIT" LARGE_N="$LIMIT" OUT_DIR="$SYN_WORK_DIR" \
          SEED="$SEED" scripts/prepare_synthetic_rectangles.sh
      else
        echo "Error: SYNTHETIC_KIND must be points or rectangles" >&2
        exit 1
      fi
    fi
  fi

  if [[ " $DATASETS " == *" ZGAP_WIDE "* ]]; then
    local zgap_wkt
    zgap_wkt=$(dataset_file ZGAP_WIDE)
    if should_run_file "$zgap_wkt"; then
      echo "[prepare] Generate Zmin/Zmax gap stress dataset -> $zgap_wkt"
      run_cmd env NUM="$LIMIT" OUT_DIR="$ZGAP_WORK_DIR" NAME=ZGAP_WIDE \
        SEED="$SEED" AUTO_BUILD=0 scripts/prepare_zrange_gap_dataset.sh
    fi
  fi
}

generate_queries_for_dataset() {
  local dataset=$1
  local data_file
  data_file=$(dataset_file "$dataset")
  if [[ "$DRY_RUN" != "1" ]]; then
    data_file=$(realpath "$data_file")
  fi
  local prefix
  prefix=$(query_prefix "$dataset")

  local all_queries_exist=1
  for tag in "${SELECTIVITY_TAGS[@]}"; do
    if [[ ! -s "${prefix}_${tag}.csv" ]]; then
      all_queries_exist=0
    fi
  done
  if [[ "$OVERWRITE" != "1" && "$all_queries_exist" == "1" ]]; then
    return
  fi

  echo "[query] $dataset -> ${prefix}_*.csv"
  run_cmd scripts/generate_jts_strtree_knn_queries.sh \
    "$data_file" \
    "$prefix" \
    "$LIMIT" \
    "$QUERY_COUNT" \
    "$SEED"
}

run_contains_for_selectivity() {
  local dataset=$1
  local data_file=$2
  local query_file=$3
  local tag=$4

  local glin_csv
  glin_csv=$(result_path "$dataset" "$tag" glin contains)
  if should_run_file "$glin_csv"; then
    echo "[bench] $dataset contains GLIN $tag"
    run_cmd ./build/bench_glin_wkt \
      --data_file "$data_file" \
      --dataset_name "$dataset" \
      --limit "$LIMIT" \
      --query_file "$query_file" \
      --seed "$SEED" \
      --output_csv "$glin_csv"
  fi

  local boost_csv
  boost_csv=$(result_path "$dataset" "$tag" boost_rtree contains)
  if should_run_file "$boost_csv"; then
    echo "[bench] $dataset contains Boost R-tree $tag"
    run_cmd ./build/bench_boost_rtree_wkt \
      --data_file "$data_file" \
      --dataset_name "$dataset" \
      --limit "$LIMIT" \
      --query_file "$query_file" \
      --relationship contains \
      --seed "$SEED" \
      --output_csv "$boost_csv"
  fi

  local quad_csv
  quad_csv=$(result_path "$dataset" "$tag" geos_quadtree contains)
  if should_run_file "$quad_csv"; then
    echo "[bench] $dataset contains GEOS Quadtree $tag"
    run_cmd ./build/bench_geos_quadtree_wkt \
      --data_file "$data_file" \
      --dataset_name "$dataset" \
      --limit "$LIMIT" \
      --query_file "$query_file" \
      --relationship contains \
      --seed "$SEED" \
      --output_csv "$quad_csv"
  fi
}

run_intersects_for_selectivity() {
  local dataset=$1
  local data_file=$2
  local query_file=$3
  local tag=$4

  local glin_piece_csv
  glin_piece_csv=$(result_path "$dataset" "$tag" glin_piece intersects)
  if should_run_file "$glin_piece_csv"; then
    echo "[bench] $dataset intersects GLIN-piecewise $tag"
    run_cmd ./build/bench_glin_wkt_piece \
      --data_file "$data_file" \
      --dataset_name "$dataset" \
      --limit "$LIMIT" \
      --query_file "$query_file" \
      --seed "$SEED" \
      --output_csv "$glin_piece_csv"
  fi

  local boost_csv
  boost_csv=$(result_path "$dataset" "$tag" boost_rtree intersects)
  if should_run_file "$boost_csv"; then
    echo "[bench] $dataset intersects Boost R-tree $tag"
    run_cmd ./build/bench_boost_rtree_wkt \
      --data_file "$data_file" \
      --dataset_name "$dataset" \
      --limit "$LIMIT" \
      --query_file "$query_file" \
      --relationship intersects \
      --seed "$SEED" \
      --output_csv "$boost_csv"
  fi

  local quad_csv
  quad_csv=$(result_path "$dataset" "$tag" geos_quadtree intersects)
  if should_run_file "$quad_csv"; then
    echo "[bench] $dataset intersects GEOS Quadtree $tag"
    run_cmd ./build/bench_geos_quadtree_wkt \
      --data_file "$data_file" \
      --dataset_name "$dataset" \
      --limit "$LIMIT" \
      --query_file "$query_file" \
      --relationship intersects \
      --seed "$SEED" \
      --output_csv "$quad_csv"
  fi
}

run_benchmarks_for_dataset() {
  local dataset=$1
  local data_file
  data_file=$(dataset_file "$dataset")
  local prefix
  prefix=$(query_prefix "$dataset")

  for tag in "${SELECTIVITY_TAGS[@]}"; do
    local query_file="${prefix}_${tag}.csv"
    if [[ "$DRY_RUN" != "1" && ! -s "$query_file" ]]; then
      echo "Error: missing query file $query_file" >&2
      exit 1
    fi
    if [[ "$RUN_CONTAINS" == "1" ]]; then
      run_contains_for_selectivity "$dataset" "$data_file" "$query_file" "$tag"
    fi
    if [[ "$RUN_INTERSECTS" == "1" ]]; then
      run_intersects_for_selectivity "$dataset" "$data_file" "$query_file" "$tag"
    fi
  done
}

main() {
  echo "=== GLIN all 1M runner ==="
  echo "LIMIT=$LIMIT QUERY_COUNT=$QUERY_COUNT SEED=$SEED"
  echo "DATASETS=$DATASETS"
  echo "SYNTHETIC_KIND=$SYNTHETIC_KIND"
  echo "RESULT_DIR=$RESULT_DIR QUERY_DIR=$QUERY_DIR FIGURE_DIR=$FIGURE_DIR"

  if [[ "$AUTO_BUILD" == "1" ]]; then
    echo "[build] Configure and build required targets"
    run_cmd cmake -S . -B build
    run_cmd cmake --build build --target \
      bench_glin_wkt \
      bench_glin_wkt_piece \
      bench_boost_rtree_wkt \
      bench_geos_quadtree_wkt \
      convert_binary_points_to_wkt \
      generate_synthetic_points \
      generate_synthetic_rectangles \
      -j2
  fi

  ensure_executable ./build/bench_glin_wkt
  ensure_executable ./build/bench_glin_wkt_piece
  ensure_executable ./build/bench_boost_rtree_wkt
  ensure_executable ./build/bench_geos_quadtree_wkt
  ensure_executable ./build/convert_binary_points_to_wkt
  ensure_executable ./build/generate_synthetic_points
  ensure_executable ./build/generate_synthetic_rectangles

  mkdir -p "$RESULT_DIR" "$QUERY_DIR" "$FIGURE_DIR"

  if [[ "$PREPARE_DATA" == "1" ]]; then
    prepare_data
  fi

  for dataset in $DATASETS; do
    local data_file
    data_file=$(dataset_file "$dataset")
    if [[ "$DRY_RUN" != "1" && ! -s "$data_file" ]]; then
      echo "Error: dataset file missing or empty for $dataset: $data_file" >&2
      exit 1
    fi
    if [[ "$GENERATE_QUERIES" == "1" ]]; then
      generate_queries_for_dataset "$dataset"
    fi
    if [[ "$RUN_BENCHMARKS" == "1" ]]; then
      run_benchmarks_for_dataset "$dataset"
    fi
  done

  if [[ "$SUMMARIZE" == "1" ]]; then
    echo "[summary] $SUMMARY_CSV"
    run_cmd python3 scripts/summarize_all_1m.py \
      --glob "${RESULT_DIR}/*.csv" \
      --output "$SUMMARY_CSV"
  fi

  if [[ "$PLOT" == "1" ]]; then
    echo "[plot] $FIGURE_DIR"
    run_cmd python3 scripts/plot_all_1m_summary.py \
      --input "$SUMMARY_CSV" \
      --output_dir "$FIGURE_DIR" \
      --figure_prefix all_1m
  fi

  echo "Done."
}

main "$@"
