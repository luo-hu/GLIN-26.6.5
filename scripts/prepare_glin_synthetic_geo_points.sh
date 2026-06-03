#!/usr/bin/env bash
set -euo pipefail

# Generate four GLIN synthetic point datasets mapped to longitude/latitude:
#   UNIF_S, UNIF_L, DIAG_S, DIAG_L
#
# Defaults are set to 1M for practical local benchmarking. To use the paper-like
# scale, run for example:
#   SMALL_N=10000000 LARGE_N=40000000 scripts/prepare_glin_synthetic_geo_points.sh
#
# Optional environment overrides:
#   SMALL_N=1000000
#   LARGE_N=1000000
#   SCALE=1
#   SEED=42
#   FORMAT=wkt          # wkt, binary, or both
#   OUT_DIR=data/synthetic/glin_geo
#   DIAG_NOISE=0.01

SMALL_N=${SMALL_N:-1000000}
LARGE_N=${LARGE_N:-1000000}
SCALE=${SCALE:-1}
SEED=${SEED:-42}
FORMAT=${FORMAT:-wkt}
OUT_DIR=${OUT_DIR:-data/synthetic/glin_geo}
DIAG_NOISE=${DIAG_NOISE:-0.01}

if [[ ! -x ./build/generate_synthetic_points ]]; then
  echo "Error: ./build/generate_synthetic_points not found. Run cmake --build build --target generate_synthetic_points first." >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

generate_one() {
  local name=$1
  local dist=$2
  local n=$3

  if [[ "$FORMAT" == "both" ]]; then
    ./build/generate_synthetic_points \
      --dist "$dist" \
      --num "$n" \
      --dim 2 \
      --scale "$SCALE" \
      --seed "$SEED" \
      --format both \
      --map_geo \
      --diag_noise "$DIAG_NOISE" \
      --wkt_file "${OUT_DIR}/${name}.wkt" \
      --binary_file "${OUT_DIR}/${name}.bin"
  else
    local suffix="wkt"
    if [[ "$FORMAT" == "binary" ]]; then
      suffix="bin"
    fi
    ./build/generate_synthetic_points \
      --dist "$dist" \
      --num "$n" \
      --dim 2 \
      --scale "$SCALE" \
      --seed "$SEED" \
      --format "$FORMAT" \
      --map_geo \
      --diag_noise "$DIAG_NOISE" \
      --output_file "${OUT_DIR}/${name}.${suffix}"
  fi
}

generate_one UNIF_S uniform "$SMALL_N"
generate_one UNIF_L uniform "$LARGE_N"
generate_one DIAG_S diag "$SMALL_N"
generate_one DIAG_L diag "$LARGE_N"

echo "Done. GLIN synthetic geo point data directory: $OUT_DIR"
