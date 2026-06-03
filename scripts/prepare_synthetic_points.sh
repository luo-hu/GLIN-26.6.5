#!/usr/bin/env bash
set -euo pipefail

# Generate learnedbench-style synthetic 2D point datasets for GLIN WKT benchmarks.
#
# Defaults are intentionally small enough for quick experiments. To match
# learnedbench's default 20M points, run:
#   N=20000000 scripts/prepare_synthetic_points.sh
#
# Optional environment overrides:
#   N=1000000
#   DIM=2
#   SCALE=1
#   SEED=42
#   FORMAT=wkt          # wkt, binary, or both
#   OUT_DIR=data/synthetic/Default

N=${N:-1000000}
DIM=${DIM:-2}
SCALE=${SCALE:-1}
SEED=${SEED:-42}
FORMAT=${FORMAT:-wkt}
OUT_DIR=${OUT_DIR:-data/synthetic/Default}

if [[ ! -x ./build/generate_synthetic_points ]]; then
  echo "Error: ./build/generate_synthetic_points not found. Run cmake --build build --target generate_synthetic_points first." >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

for dist in uniform gaussian lognormal; do
  base="${dist}_${N}_${DIM}_${SCALE}"
  if [[ "$FORMAT" == "both" ]]; then
    ./build/generate_synthetic_points \
      --dist "$dist" \
      --num "$N" \
      --dim "$DIM" \
      --scale "$SCALE" \
      --seed "$SEED" \
      --format both \
      --wkt_file "${OUT_DIR}/${base}.wkt" \
      --binary_file "${OUT_DIR}/${base}.bin"
  else
    suffix="wkt"
    if [[ "$FORMAT" == "binary" ]]; then
      suffix="bin"
    fi
    ./build/generate_synthetic_points \
      --dist "$dist" \
      --num "$N" \
      --dim "$DIM" \
      --scale "$SCALE" \
      --seed "$SEED" \
      --format "$FORMAT" \
      --output_file "${OUT_DIR}/${base}.${suffix}"
  fi
done

echo "Done. Synthetic data directory: $OUT_DIR"
