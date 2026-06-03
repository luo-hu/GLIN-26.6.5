#!/usr/bin/env bash
set -euo pipefail

LIMIT="${LIMIT:-1000000}"
SEED="${SEED:-42}"
PROGRESS_STEP_PERCENT="${PROGRESS_STEP_PERCENT:-10}"
PIECE_LIMIT="${PIECE_LIMIT:-10000}"
DATASETS="${DATASETS:-AW LW ROADS PARKS}"
SUMMARY_CSV="${SUMMARY_CSV:-results/fig15_16_1m_updates_with_progress.csv}"
PROGRESS_CSV="${PROGRESS_CSV:-results/fig15_16_1m_update_progress.csv}"
FIGURE_DIR="${FIGURE_DIR:-figures/fig15_16_update_curves}"
PROGRESS_SUMMARY_CSV="${PROGRESS_SUMMARY_CSV:-results/fig15_16_1m_update_progress_summary.csv}"
BAR_FIGURE_DIR="${BAR_FIGURE_DIR:-figures/fig15_16_updates}"
BAR_SUMMARY_CSV="${BAR_SUMMARY_CSV:-results/fig15_16_1m_updates_summary.csv}"
AUTO_BUILD="${AUTO_BUILD:-1}"

if [[ "$AUTO_BUILD" == "1" ]]; then
  cmake --build build --target bench_update_wkt bench_update_wkt_piece -j2
fi

mkdir -p "$(dirname "$SUMMARY_CSV")" "$(dirname "$PROGRESS_CSV")" "$FIGURE_DIR"
: > "$SUMMARY_CSV"
: > "$PROGRESS_CSV"

data_file_for_dataset() {
  case "$1" in
    AW) echo "/mnt/hgfs/AREAWATER.csv" ;;
    LW) echo "/mnt/hgfs/LINEARWATER.csv" ;;
    ROADS) echo "/mnt/hgfs/roads" ;;
    PARKS) echo "/mnt/hgfs/parks" ;;
    *)
      echo "Unknown dataset: $1" >&2
      return 1
      ;;
  esac
}

first_write=1
for dataset in $DATASETS; do
  data_file="$(data_file_for_dataset "$dataset")"
  append_args=()
  if [[ "$first_write" == "0" ]]; then
    append_args=(--append_csv)
  fi

  ./build/bench_update_wkt \
    --data_file "$data_file" \
    --dataset_name "$dataset" \
    --limit "$LIMIT" \
    --mode both \
    --seed "$SEED" \
    --piece_limit "$PIECE_LIMIT" \
    --progress_step_percent "$PROGRESS_STEP_PERCENT" \
    --output_csv "$SUMMARY_CSV" \
    --progress_csv "$PROGRESS_CSV" \
    "${append_args[@]}"

  ./build/bench_update_wkt_piece \
    --data_file "$data_file" \
    --dataset_name "$dataset" \
    --limit "$LIMIT" \
    --mode both \
    --seed "$SEED" \
    --piece_limit "$PIECE_LIMIT" \
    --progress_step_percent "$PROGRESS_STEP_PERCENT" \
    --output_csv "$SUMMARY_CSV" \
    --progress_csv "$PROGRESS_CSV" \
    --append_csv

  first_write=0
done

python3 scripts/plot_fig15_16_update_curves.py \
  --inputs "$PROGRESS_CSV" \
  --output_dir "$FIGURE_DIR" \
  --summary_csv "$PROGRESS_SUMMARY_CSV"

python3 scripts/plot_fig15_16_updates.py \
  --inputs "$SUMMARY_CSV" \
  --output_dir "$BAR_FIGURE_DIR" \
  --summary_csv "$BAR_SUMMARY_CSV"
