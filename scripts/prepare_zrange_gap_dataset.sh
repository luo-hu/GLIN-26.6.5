#!/usr/bin/env bash
set -euo pipefail

# Generate a synthetic rectangle dataset where Zmin and Zmax are deliberately far apart.
#
# The default dataset uses large rectangles over the normal longitude/latitude
# range. It is designed as a stress workload for GLIN-piecewise Intersects:
# large MBRs make each record's Z-address interval long, which can increase
# query augmentation and candidate counts.
#
# Common overrides:
#   NUM=1000000
#   OUT_DIR=data/synthetic/zrange_gap
#   NAME=ZGAP_WIDE
#   MIN_WIDTH=30 MAX_WIDTH=120
#   MIN_HEIGHT=15 MAX_HEIGHT=60
#   AVOID_CLAMP=1
#   SEED=42
#   AUTO_BUILD=1

NUM=${NUM:-1000000}
OUT_DIR=${OUT_DIR:-data/synthetic/zrange_gap}
NAME=${NAME:-ZGAP_WIDE}
SEED=${SEED:-42}
XMIN=${XMIN:--180}
YMIN=${YMIN:--90}
XMAX=${XMAX:-180}
YMAX=${YMAX:-90}
MIN_WIDTH=${MIN_WIDTH:-30}
MAX_WIDTH=${MAX_WIDTH:-120}
MIN_HEIGHT=${MIN_HEIGHT:-15}
MAX_HEIGHT=${MAX_HEIGHT:-60}
DIST=${DIST:-uniform}
AVOID_CLAMP=${AVOID_CLAMP:-1}
AUTO_BUILD=${AUTO_BUILD:-1}
BUILD_DIR=${BUILD_DIR:-build}

main() {
  echo "=== Generate Zmin/Zmax gap stress dataset ==="
  echo "NAME=$NAME NUM=$NUM OUT_DIR=$OUT_DIR DIST=$DIST"
  echo "WIDTH=[$MIN_WIDTH,$MAX_WIDTH] HEIGHT=[$MIN_HEIGHT,$MAX_HEIGHT]"
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
    --dist "$DIST" \
    --num "$NUM" \
    --output_file "$output" \
    --xmin "$XMIN" \
    --ymin "$YMIN" \
    --xmax "$XMAX" \
    --ymax "$YMAX" \
    --min_width "$MIN_WIDTH" \
    --max_width "$MAX_WIDTH" \
    --min_height "$MIN_HEIGHT" \
    --max_height "$MAX_HEIGHT" \
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
