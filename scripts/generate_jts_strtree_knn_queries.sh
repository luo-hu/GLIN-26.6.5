#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 3 ]]; then
  cat >&2 <<'USAGE'
Usage:
  scripts/generate_jts_strtree_knn_queries.sh DATA_FILE OUTPUT_PREFIX LIMIT [QUERY_COUNT] [SEED]

Example:
  scripts/generate_jts_strtree_knn_queries.sh \
    /mnt/hgfs/parks \
    queries/parks_1m_jts_strtree_knn \
    1000000 \
    100 \
    42

This writes:
  OUTPUT_PREFIX_1pct.csv
  OUTPUT_PREFIX_0p1pct.csv
  OUTPUT_PREFIX_0p01pct.csv
  OUTPUT_PREFIX_0p001pct.csv
USAGE
  exit 2
fi

DATA_FILE=$1
OUTPUT_PREFIX=$2
LIMIT=$3
QUERY_COUNT=${4:-100}
SEED=${5:-42}
REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
GENERATOR_DIR="$REPO_ROOT/java/jts-query-generator"

if ! command -v java >/dev/null 2>&1; then
  echo "Error: java is not installed. Run: sudo apt install -y openjdk-17-jdk maven" >&2
  exit 1
fi

if ! command -v mvn >/dev/null 2>&1; then
  echo "Error: mvn is not installed. Run: sudo apt install -y openjdk-17-jdk maven" >&2
  exit 1
fi

mkdir -p "$(dirname "$REPO_ROOT/$OUTPUT_PREFIX")"

cd "$GENERATOR_DIR"
mvn -q -DskipTests package
mvn -q exec:java -Dexec.args="--data_file $DATA_FILE --limit $LIMIT --query_count $QUERY_COUNT --selectivities 1%,0.1%,0.01%,0.001% --output_prefix ../../$OUTPUT_PREFIX --seed $SEED"
