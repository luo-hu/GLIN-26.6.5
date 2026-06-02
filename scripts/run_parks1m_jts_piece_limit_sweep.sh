#!/usr/bin/env bash
set -euo pipefail

# Run GLIN-piecewise Intersects with the same JTS STRtree KNN query windows
# while sweeping the piece_limit parameter used in the GLIN paper.
#
# Optional environment overrides:
#   DATA_FILE=/mnt/hgfs/parks
#   LIMIT=1000000
#   DATASET_NAME=PARKS
#   QUERY_PREFIX=queries/parks_1m_jts_strtree_knn
#   OUTPUT_PREFIX=results/parks_1m_jts_strtree_knn

DATA_FILE=${DATA_FILE:-/mnt/hgfs/parks}
LIMIT=${LIMIT:-1000000}
DATASET_NAME=${DATASET_NAME:-PARKS}
QUERY_PREFIX=${QUERY_PREFIX:-queries/parks_1m_jts_strtree_knn}
OUTPUT_PREFIX=${OUTPUT_PREFIX:-results/parks_1m_jts_strtree_knn}

PIECE_LIMITS=(100 1000 10000 100000)
SELECTIVITY_TAGS=(0p001pct 0p01pct 0p1pct 1pct)

if [[ ! -x ./build/bench_glin_wkt_piece ]]; then
  echo "Error: ./build/bench_glin_wkt_piece not found. Run ./build.sh first." >&2
  exit 1
fi

mkdir -p "$(dirname "$OUTPUT_PREFIX")"

for piece_limit in "${PIECE_LIMITS[@]}"; do
  for selectivity in "${SELECTIVITY_TAGS[@]}"; do
    query_file="${QUERY_PREFIX}_${selectivity}.csv"
    output_csv="${OUTPUT_PREFIX}_pl${piece_limit}_${selectivity}_glin_piece_intersects.csv"

    if [[ ! -f "$query_file" ]]; then
      echo "Error: missing query file: $query_file" >&2
      exit 1
    fi

    echo "Running PL=${piece_limit} selectivity=${selectivity}"
    ./build/bench_glin_wkt_piece \
      --data_file "$DATA_FILE" \
      --dataset_name "$DATASET_NAME" \
      --limit "$LIMIT" \
      --piece_limit "$piece_limit" \
      --query_file "$query_file" \
      --output_csv "$output_csv"
  done
done

echo "Done. Raw CSV prefix: ${OUTPUT_PREFIX}_pl*_glin_piece_intersects.csv"
