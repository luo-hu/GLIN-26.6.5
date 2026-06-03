#!/usr/bin/env bash
set -euo pipefail

# Generate GLIN-style synthetic rectangle WKT datasets for Intersects tests.
#
# Defaults are practical for local experiments. To mimic the paper's scale,
# use SMALL_N=10000000 LARGE_N=40000000, but expect very large WKT files.
#
# Optional environment overrides:
#   SMALL_N=1000000
#   LARGE_N=4000000
#   OUT_DIR=data/synthetic/rectangles
#   SEED=42
#   XMIN=-180 YMIN=-90 XMAX=180 YMAX=90
#   MIN_WIDTH=0.0001 MAX_WIDTH=0.001
#   MIN_HEIGHT=0.0001 MAX_HEIGHT=0.001
#   DIAGONAL_NOISE=0.01

SMALL_N=${SMALL_N:-1000000}
LARGE_N=${LARGE_N:-4000000}
OUT_DIR=${OUT_DIR:-data/synthetic/rectangles}
SEED=${SEED:-42}
XMIN=${XMIN:--180}
YMIN=${YMIN:--90}
XMAX=${XMAX:-180}
YMAX=${YMAX:-90}
MIN_WIDTH=${MIN_WIDTH:-0.0001}
MAX_WIDTH=${MAX_WIDTH:-0.001}
MIN_HEIGHT=${MIN_HEIGHT:-0.0001}
MAX_HEIGHT=${MAX_HEIGHT:-0.001}
DIAGONAL_NOISE=${DIAGONAL_NOISE:-0.01}

if [[ ! -x ./build/generate_synthetic_rectangles ]]; then
  echo "Error: ./build/generate_synthetic_rectangles not found. Run cmake --build build --target generate_synthetic_rectangles first." >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

generate_one() {
  local name=$1
  local dist=$2
  local count=$3
  local seed=$4
  local output="${OUT_DIR}/${name}.wkt"

  ./build/generate_synthetic_rectangles \
    --dist "$dist" \
    --num "$count" \
    --output_file "$output" \
    --xmin "$XMIN" \
    --ymin "$YMIN" \
    --xmax "$XMAX" \
    --ymax "$YMAX" \
    --min_width "$MIN_WIDTH" \
    --max_width "$MAX_WIDTH" \
    --min_height "$MIN_HEIGHT" \
    --max_height "$MAX_HEIGHT" \
    --diagonal_noise "$DIAGONAL_NOISE" \
    --seed "$seed"
}

generate_one UNIF_S uniform "$SMALL_N" "$SEED"
generate_one UNIF_L uniform "$LARGE_N" "$((SEED + 1))"
generate_one DIAG_S diagonal "$SMALL_N" "$((SEED + 2))"
generate_one DIAG_L diagonal "$LARGE_N" "$((SEED + 3))"

echo "Done. Synthetic rectangle directory: $OUT_DIR"
