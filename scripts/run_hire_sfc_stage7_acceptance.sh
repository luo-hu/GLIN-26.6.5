#!/usr/bin/env bash SDF S SADFSZDFVS 

if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
  printf '%s\n' \
    'Error: run this acceptance script as a child process; do not source it.' \
    'Use: ./scripts/run_hire_sfc_stage7_acceptance.sh' >&2
  return 2
fi

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

RESULT_DIR="${RESULT_DIR:-results/hire_sfc_stage7_acceptance}"
BUILD_DIR="${BUILD_DIR:-build_stage7_release}"
ASAN_BUILD_DIR="${ASAN_BUILD_DIR:-build_stage7_asan}"
TSAN_BUILD_DIR="${TSAN_BUILD_DIR:-build_stage7_tsan}"
STRESS_RUNS="${STRESS_RUNS:-10}"
RUN_ASAN="${RUN_ASAN:-1}"
RUN_TSAN="${RUN_TSAN:-0}"
TSAN_REQUIRED="${TSAN_REQUIRED:-0}"
RUN_BENCHMARK_SMOKE="${RUN_BENCHMARK_SMOKE:-0}"

mkdir -p "$RESULT_DIR"
REPORT="$RESULT_DIR/acceptance.log"
: > "$REPORT"
exec > >(tee -a "$REPORT") 2>&1

echo "HIRE SFC Stage 7 acceptance"
echo "timestamp=$(date --iso-8601=seconds)"
echo "root=$ROOT_DIR"
echo "git_commit=$(git rev-parse HEAD 2>/dev/null || echo unknown)"
echo "compiler=$(c++ --version | head -n 1)"
echo "kernel=$(uname -srmo)"
echo "stress_runs=$STRESS_RUNS"

echo "[1/4] Release build and regression tests"
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j"${BUILD_JOBS:-2}"
ctest --test-dir "$BUILD_DIR" --output-on-failure

echo "[2/4] Repeated deterministic oracle/RCU acceptance"
for ((run = 1; run <= STRESS_RUNS; ++run)); do
  echo "stage7_stress_run=$run"
  "$BUILD_DIR/test_hire_sfc_stage7_acceptance"
done

if [[ "$RUN_ASAN" == "1" ]]; then
  echo "[3/4] ASan/UBSan regression tests"
  cmake -S . -B "$ASAN_BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
  cmake --build "$ASAN_BUILD_DIR" \
    --target test_hire_internal_directory test_hire_sfc_stage2 \
             test_hire_sfc_stage3_cost test_hire_sfc_stage4_rcu \
             test_hire_sfc_stage5_transform test_hire_sfc_stage6_bulk \
             test_hire_sfc_stage7_acceptance \
    -j"${BUILD_JOBS:-2}"
  ASAN_OPTIONS="detect_leaks=0:halt_on_error=1" \
  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
    ctest --test-dir "$ASAN_BUILD_DIR" --output-on-failure \
      -R 'test_hire_'
else
  echo "[3/4] ASan/UBSan skipped (RUN_ASAN=0)"
fi

if [[ "$RUN_TSAN" == "1" ]]; then
  echo "[4/4] ThreadSanitizer concurrency tests"
  set +e
  cmake -S . -B "$TSAN_BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
  tsan_status=$?
  if [[ $tsan_status -eq 0 ]]; then
    cmake --build "$TSAN_BUILD_DIR" \
      --target test_hire_sfc_stage4_rcu test_hire_sfc_stage7_acceptance \
      -j"${BUILD_JOBS:-2}"
    tsan_status=$?
  fi
  if [[ $tsan_status -eq 0 ]]; then
    TSAN_OPTIONS="halt_on_error=1" \
      ctest --test-dir "$TSAN_BUILD_DIR" --output-on-failure \
        -R 'test_hire_sfc_stage(4_rcu|7_acceptance)'
    tsan_status=$?
  fi
  set -e
  echo "tsan_status=$tsan_status"
  if [[ $tsan_status -ne 0 && "$TSAN_REQUIRED" == "1" ]]; then
    exit "$tsan_status"
  fi
else
  echo "[4/4] TSan skipped (RUN_TSAN=0)"
fi

if [[ "$RUN_BENCHMARK_SMOKE" == "1" ]]; then
  echo "[optional] AW mixed correctness smoke"
  RESET_RESULTS=1 OVERWRITE=1 AUTO_BUILD=0 \
  PREDICATE_SHORTCUTS=1 INDEXES="HIRE_SFC_FULL Boost_Rtree" \
  CHECK_CORRECTNESS=1 WORKLOAD_MODE=mixed MIXED_PROFILES=balanced \
  MIXED_OPERATIONS="${SMOKE_OPERATIONS:-2000}" \
  MIXED_CHECKPOINT_INTERVAL="${SMOKE_CHECKPOINT_INTERVAL:-500}" \
  DATASETS=AW LIMIT="${SMOKE_LIMIT:-5000}" QUERY_LIMIT="${SMOKE_LIMIT:-5000}" \
  QUERY_ROOT="${SMOKE_QUERY_ROOT:-queries/hire_stage7_smoke_5000}" \
  RESULT_DIR="$RESULT_DIR/benchmark" FIGURE_DIR="$RESULT_DIR/figures" \
  SELECTIVITY_TAGS="${SMOKE_SELECTIVITY_TAGS:-0p01pct}" QUERY_COUNT=50 \
  AUTO_GENERATE_QUERIES=1 BUILD_DIR="$BUILD_DIR" \
    ./scripts/run_dynamic_compare_diagnostics.sh
fi

echo "acceptance_status=PASS"
echo "report=$REPORT"
