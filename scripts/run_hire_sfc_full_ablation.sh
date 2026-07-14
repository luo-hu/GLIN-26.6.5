#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RESULT_ROOT="${HIRE_ABLATION_RESULT_ROOT:-results/hire_sfc_full_ablation}"
FIGURE_ROOT="${HIRE_ABLATION_FIGURE_ROOT:-figures/hire_sfc_full_ablation}"
STAGES="${HIRE_ABLATION_STAGES:-stage1 stage2 stage3 stage4 stage5 stage6 stage6_guard}"

run_stage() {
  local stage="$1"
  shift
  echo "Running HIRE SFC ablation: ${stage}"
  env \
    RESET_RESULTS="${RESET_RESULTS:-1}" \
    OVERWRITE="${OVERWRITE:-1}" \
    INDEXES="HIRE_SFC_FULL" \
    RESULT_DIR="${RESULT_ROOT}/${stage}" \
    FIGURE_DIR="${FIGURE_ROOT}/${stage}" \
    "$@" \
    "${PROJECT_ROOT}/scripts/run_dynamic_compare_diagnostics.sh"
}

for stage in ${STAGES}; do
  case "${stage}" in
    stage1)
      run_stage "${stage}" \
        HIRE_SFC_FORCE_LEGACY=1 \
        HIRE_SFC_ENABLE_COST_RETRAIN=0 \
        HIRE_SFC_ENABLE_BACKGROUND_RECALIBRATION=0 \
        HIRE_SFC_ENABLE_RCU_RECALIBRATION=0 \
        HIRE_SFC_ENABLE_LEGACY_TRANSFORM=0 \
        HIRE_SFC_ENABLE_INTER_LEVEL_BULK_LOAD=0 \
        HIRE_SFC_ENABLE_SPATIAL_DIRECTORY_PRUNING=0
      ;;
    stage2)
      run_stage "${stage}" \
        HIRE_SFC_FORCE_LEGACY=0 \
        HIRE_SFC_ENABLE_COST_RETRAIN=0 \
        HIRE_SFC_ENABLE_BACKGROUND_RECALIBRATION=0 \
        HIRE_SFC_ENABLE_RCU_RECALIBRATION=0 \
        HIRE_SFC_ENABLE_LEGACY_TRANSFORM=0 \
        HIRE_SFC_ENABLE_INTER_LEVEL_BULK_LOAD=0 \
        HIRE_SFC_ENABLE_SPATIAL_DIRECTORY_PRUNING=0
      ;;
    stage3)
      run_stage "${stage}" \
        HIRE_SFC_FORCE_LEGACY=0 \
        HIRE_SFC_ENABLE_COST_RETRAIN=1 \
        HIRE_SFC_ENABLE_BACKGROUND_RECALIBRATION=0 \
        HIRE_SFC_ENABLE_RCU_RECALIBRATION=0 \
        HIRE_SFC_ENABLE_LEGACY_TRANSFORM=0 \
        HIRE_SFC_ENABLE_INTER_LEVEL_BULK_LOAD=0 \
        HIRE_SFC_ENABLE_SPATIAL_DIRECTORY_PRUNING=0
      ;;
    stage4)
      run_stage "${stage}" \
        HIRE_SFC_FORCE_LEGACY=0 \
        HIRE_SFC_ENABLE_COST_RETRAIN=1 \
        HIRE_SFC_ENABLE_BACKGROUND_RECALIBRATION=1 \
        HIRE_SFC_ENABLE_RCU_RECALIBRATION=1 \
        HIRE_SFC_ENABLE_LEGACY_TRANSFORM=0 \
        HIRE_SFC_ENABLE_INTER_LEVEL_BULK_LOAD=0 \
        HIRE_SFC_ENABLE_SPATIAL_DIRECTORY_PRUNING=0
      ;;
    stage5)
      run_stage "${stage}" \
        HIRE_SFC_FORCE_LEGACY=0 \
        HIRE_SFC_ENABLE_COST_RETRAIN=1 \
        HIRE_SFC_ENABLE_BACKGROUND_RECALIBRATION=1 \
        HIRE_SFC_ENABLE_RCU_RECALIBRATION=1 \
        HIRE_SFC_ENABLE_LEGACY_TRANSFORM=1 \
        HIRE_SFC_ENABLE_INTER_LEVEL_BULK_LOAD=0 \
        HIRE_SFC_ENABLE_SPATIAL_DIRECTORY_PRUNING=0
      ;;
    stage6)
      run_stage "${stage}" \
        HIRE_SFC_FORCE_LEGACY=0 \
        HIRE_SFC_ENABLE_COST_RETRAIN=1 \
        HIRE_SFC_ENABLE_BACKGROUND_RECALIBRATION=1 \
        HIRE_SFC_ENABLE_RCU_RECALIBRATION=1 \
        HIRE_SFC_ENABLE_LEGACY_TRANSFORM=1 \
        HIRE_SFC_ENABLE_INTER_LEVEL_BULK_LOAD=1 \
        HIRE_SFC_ENABLE_SPATIAL_DIRECTORY_PRUNING=0
      ;;
    stage6_guard)
      run_stage "${stage}" \
        HIRE_SFC_FORCE_LEGACY=0 \
        HIRE_SFC_ENABLE_COST_RETRAIN=1 \
        HIRE_SFC_ENABLE_BACKGROUND_RECALIBRATION=1 \
        HIRE_SFC_ENABLE_RCU_RECALIBRATION=1 \
        HIRE_SFC_ENABLE_LEGACY_TRANSFORM=1 \
        HIRE_SFC_ENABLE_INTER_LEVEL_BULK_LOAD=1 \
        HIRE_SFC_ENABLE_SPATIAL_DIRECTORY_PRUNING=1
      ;;
    *)
      echo "Unknown ablation stage: ${stage}" >&2
      exit 2
      ;;
  esac
done
