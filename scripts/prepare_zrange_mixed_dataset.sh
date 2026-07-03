#!/usr/bin/env bash
set -euo pipefail

# Generate a mixed Zmin/Zmax gap dataset for IO_OVERFLOW.
#
# Unlike ZGAP_WIDE, this dataset is not all wide rectangles. It uses mostly
# small rectangles plus a small fraction of fat rectangles. That is the case
# where overflow separation should help: fat objects pollute block summaries,
# while normal objects should remain easy to prune in the main index.
#
# Common overrides:
#   NUM=1000000
#   OUT_DIR=data/synthetic/zrange_gap
#   NAME=ZGAP_MIXED
#   FAT_FRACTION=0.01
#   FAT_MIN_WIDTH=30 FAT_MAX_WIDTH=120
#   FAT_MIN_HEIGHT=15 FAT_MAX_HEIGHT=60
#   MIN_WIDTH=0.0001 MAX_WIDTH=0.001
#   MIN_HEIGHT=0.0001 MAX_HEIGHT=0.001
#   AVOID_CLAMP=1
#   SEED=42

NUM=${NUM:-1000000}
OUT_DIR=${OUT_DIR:-data/synthetic/zrange_gap_mixed_${NUM}}
NAME=${NAME:-ZGAP_MIXED}
SEED=${SEED:-42}
XMIN=${XMIN:--180}
YMIN=${YMIN:--90}
XMAX=${XMAX:-180}
YMAX=${YMAX:-90}
MIN_WIDTH=${MIN_WIDTH:-0.0001}
MAX_WIDTH=${MAX_WIDTH:-0.001}
MIN_HEIGHT=${MIN_HEIGHT:-0.0001}
MAX_HEIGHT=${MAX_HEIGHT:-0.001}
FAT_FRACTION=${FAT_FRACTION:-0.01}
FAT_MIN_WIDTH=${FAT_MIN_WIDTH:-30}
FAT_MAX_WIDTH=${FAT_MAX_WIDTH:-120}
FAT_MIN_HEIGHT=${FAT_MIN_HEIGHT:-15}
FAT_MAX_HEIGHT=${FAT_MAX_HEIGHT:-60}
AVOID_CLAMP=${AVOID_CLAMP:-1}
AUTO_BUILD=${AUTO_BUILD:-1}
BUILD_DIR=${BUILD_DIR:-build}

main() {
  echo "=== Generate mixed fat-object Zmin/Zmax dataset ==="
  echo "NAME=$NAME NUM=$NUM OUT_DIR=$OUT_DIR"
  echo "SMALL_WIDTH=[$MIN_WIDTH,$MAX_WIDTH] SMALL_HEIGHT=[$MIN_HEIGHT,$MAX_HEIGHT]"
  echo "FAT_FRACTION=$FAT_FRACTION FAT_WIDTH=[$FAT_MIN_WIDTH,$FAT_MAX_WIDTH] FAT_HEIGHT=[$FAT_MIN_HEIGHT,$FAT_MAX_HEIGHT]"
  echo "AVOID_CLAMP=$AVOID_CLAMP"

  if [[ "$AUTO_BUILD" == "1" ]]; then
    cmake -S . -B "$BUILD_DIR"
    cmake --build "$BUILD_DIR" --target generate_synthetic_rectangles export_zrange_cdf_wkt -j2
  fi

  if [[ ! -x "$BUILD_DIR/generate_synthetic_rectangles" ]]; then
    echo "Error: missing $BUILD_DIR/generate_synthetic_rectangles" >&2
    exit 1
  fi

  mkdir -p "$OUT_DIR"
  local output="${OUT_DIR}/${NAME}.wkt"
  local args=(
    "$BUILD_DIR/generate_synthetic_rectangles"
    --dist mixed_fat
    --num "$NUM"
    --output_file "$output"
    --xmin "$XMIN"
    --ymin "$YMIN"
    --xmax "$XMAX"
    --ymax "$YMAX"
    --min_width "$MIN_WIDTH"
    --max_width "$MAX_WIDTH"
    --min_height "$MIN_HEIGHT"
    --max_height "$MAX_HEIGHT"
    --fat_fraction "$FAT_FRACTION"
    --fat_min_width "$FAT_MIN_WIDTH"
    --fat_max_width "$FAT_MAX_WIDTH"
    --fat_min_height "$FAT_MIN_HEIGHT"
    --fat_max_height "$FAT_MAX_HEIGHT"
    --seed "$SEED"
  )
  if [[ "$AVOID_CLAMP" == "1" ]]; then
    args+=(--avoid_clamp)
  fi
  "${args[@]}"

  echo "dataset=$output"
  echo
  echo "To inspect Zmin/Zmax separation:"
  echo "  ./build/export_zrange_cdf_wkt --data_file $output --dataset_name $NAME --limit $NUM --cdf_points 1000 --output_csv results/${NAME}_zrange_cdf.csv"
  echo "  python3 scripts/plot_fig5_zrange_cdf.py --input_csv results/${NAME}_zrange_cdf.csv --output_dir figures/${NAME}_zrange_cdf --prefix ${NAME}_zrange_cdf"
}

main "$@"
