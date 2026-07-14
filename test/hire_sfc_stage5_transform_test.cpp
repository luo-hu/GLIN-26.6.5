#include "../src/benchmark/hire_sfc_lite_index.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using hire_sfc_lite::Box2D;
using hire_sfc_lite::DebugStats;
using hire_sfc_lite::HireSfcLiteIndex;
using hire_sfc_lite::RecordInput;

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

RecordInput make_record(std::size_t id, double key) {
  return RecordInput{id, key, key + 1.0,
                     Box2D{key, 0.0, key + 1.0, 1.0}};
}

void configure(std::size_t min_model_leaf, std::size_t buffer_limit) {
  setenv("HIRE_SFC_LEAF_SIZE", "16", 1);
  setenv("HIRE_SFC_MODEL_LEAF_SIZE", "64", 1);
  setenv("HIRE_SFC_MIN_MODEL_LEAF",
         std::to_string(min_model_leaf).c_str(), 1);
  setenv("HIRE_SFC_EPSILON", "0.01", 1);
  setenv("HIRE_SFC_FORCE_LEGACY", "0", 1);
  setenv("HIRE_SFC_ENABLE_LEGACY_TRANSFORM", "1", 1);
  setenv("HIRE_SFC_ENABLE_DELETED_SLOT_REUSE", "0", 1);
  setenv("HIRE_SFC_ENABLE_COST_RETRAIN", "0", 1);
  setenv("HIRE_SFC_ENABLE_BACKGROUND_RECALIBRATION", "1", 1);
  setenv("HIRE_SFC_ENABLE_RCU_RECALIBRATION", "1", 1);
  setenv("HIRE_SFC_BACKGROUND_TEST_DELAY_US", "0", 1);
  setenv("HIRE_SFC_BUFFER_LIMIT", std::to_string(buffer_limit).c_str(), 1);
  setenv("HIRE_SFC_TOMBSTONE_REBUILD_RATIO", "2.0", 1);
  setenv("HIRE_SFC_LEGACY_TRANSFORM_MAX_LEAVES", "4", 1);
  setenv("HIRE_SFC_LEGACY_BACKWARD_MIN_LEAVES", "2", 1);
  setenv("HIRE_SFC_LEGACY_SLOPE_TOLERANCE", "0.10", 1);
  setenv("HIRE_SFC_LEGACY_INTERCEPT_TOLERANCE", "2.0", 1);
  setenv("HIRE_SFC_LEGACY_TRANSFORM_UPDATE_THRESHOLD", "1", 1);
  setenv("HIRE_SFC_LEGACY_TRANSFORM_COOLDOWN_UPDATES", "0", 1);
  setenv("HIRE_SFC_INTERNAL_FANOUT", "4", 1);
}

std::vector<RecordInput> linear_bulk(std::size_t count) {
  std::vector<RecordInput> result;
  for (std::size_t id = 0; id < count; ++id) {
    result.push_back(make_record(id, static_cast<double>(id)));
  }
  return result;
}

std::vector<std::size_t> query_all(const HireSfcLiteIndex& index) {
  std::vector<std::size_t> result;
  index.range_query(-1000.0, 10000.0,
                    Box2D{-1000.0, -1000.0, 10000.0, 1000.0}, result,
                    nullptr);
  std::sort(result.begin(), result.end());
  return result;
}

void require_ids(const HireSfcLiteIndex& index,
                 const std::vector<std::size_t>& expected,
                 const char* message) {
  require(query_all(index) == expected, message);
}

void test_forward_merge() {
  configure(4, 3);
  HireSfcLiteIndex index(256, true);
  index.bulk_load(linear_bulk(32));
  for (std::size_t offset = 0; offset < 3; ++offset) {
    require(index.insert(make_record(100 + offset, 32.0 + offset)),
            "forward setup insert failed");
  }
  require(index.wait_for_background_tasks(),
          "forward setup retraining did not finish");
  DebugStats stats = index.debug_stats();
  require(stats.legacy_leaf_count == 1,
          "forward setup did not create a short legacy tail");

  require(index.insert(make_record(103, 35.0)),
          "forward trigger insert failed");
  require(index.wait_for_background_tasks(),
          "forward merge did not finish");
  stats = index.debug_stats();
  require(stats.legacy_forward_attempt_count == 1 &&
              stats.legacy_forward_success_count == 1,
          "forward model+legacy merge did not succeed");
  require(stats.last_recalibration_job_kind == 2,
          "forward merge job kind was not recorded");
  require(stats.legacy_leaf_count == 0 && stats.model_leaf_count == 2,
          "forward merge produced the wrong leaf kinds");

  std::vector<std::size_t> expected;
  for (std::size_t id = 0; id < 32; ++id) {
    expected.push_back(id);
  }
  expected.insert(expected.end(), {100, 101, 102, 103});
  std::sort(expected.begin(), expected.end());
  require_ids(index, expected, "forward merge lost records");
}

void test_backward_merge() {
  configure(10, 9);
  HireSfcLiteIndex index(256, true);
  index.bulk_load(linear_bulk(32));
  for (std::size_t offset = 0; offset < 9; ++offset) {
    require(index.insert(make_record(100 + offset, 100.0 + 2.0 * offset)),
            "backward setup insert failed");
  }
  require(index.wait_for_background_tasks(),
          "backward setup retraining did not finish");
  for (std::size_t offset = 9; offset < 17; ++offset) {
    require(index.insert(make_record(100 + offset, 100.0 + 2.0 * offset)),
            "backward run insert failed");
  }
  require(index.wait_for_background_tasks(),
          "backward PLA merge did not finish");

  const DebugStats stats = index.debug_stats();
  if (stats.legacy_backward_attempt_count != 1 ||
      stats.legacy_backward_success_count != 1) {
    std::cerr << "backward diagnostics: attempts="
              << stats.legacy_backward_attempt_count
              << " successes=" << stats.legacy_backward_success_count
              << " rejects=" << stats.legacy_coefficient_reject_count
              << " aborts=" << stats.legacy_transform_abort_count
              << " gate_skips=" << stats.legacy_transform_gate_skip_count
              << " cache_refreshes="
              << stats.legacy_regression_cache_refresh_count
              << " leaves=" << stats.leaf_count
              << " model=" << stats.model_leaf_count
              << " legacy=" << stats.legacy_leaf_count
              << " splits=" << stats.leaf_split_count << '\n';
  }
  require(stats.legacy_backward_attempt_count == 1 &&
              stats.legacy_backward_success_count == 1,
          "backward legacy-run merge did not succeed");
  require(stats.last_recalibration_job_kind == 3,
          "backward merge job kind was not recorded");
  require(stats.legacy_coefficient_reject_count > 0,
          "backward setup never exercised coefficient rejection");
  require(stats.broken_sibling_link_count == 0,
          "backward merge broke sibling links");

  std::vector<std::size_t> expected;
  for (std::size_t id = 0; id < 32; ++id) {
    expected.push_back(id);
  }
  for (std::size_t id = 100; id < 117; ++id) {
    expected.push_back(id);
  }
  std::sort(expected.begin(), expected.end());
  require_ids(index, expected, "backward merge lost records");
}

void test_failed_forward_preserves_tree() {
  configure(4, 3);
  setenv("HIRE_SFC_LEGACY_SLOPE_TOLERANCE", "1000", 1);
  setenv("HIRE_SFC_LEGACY_INTERCEPT_TOLERANCE", "1000", 1);
  HireSfcLiteIndex index(256, true);
  index.bulk_load(linear_bulk(32));
  for (std::size_t offset = 0; offset < 3; ++offset) {
    require(index.insert(make_record(100 + offset, 100.0 + offset)),
            "abort setup insert failed");
  }
  require(index.wait_for_background_tasks(),
          "abort setup retraining did not finish");
  require(index.insert(make_record(103, 103.0)),
          "abort trigger insert failed");
  require(index.wait_for_background_tasks(),
          "failed forward job did not drain");

  const DebugStats stats = index.debug_stats();
  require(stats.legacy_forward_attempt_count == 1,
          "failed forward candidate was not attempted");
  require(stats.legacy_forward_success_count == 0 &&
              stats.legacy_transform_abort_count == 1,
          "nonlinear forward merge was not rejected");
  require(stats.pending_rebuild_count == 0,
          "aborted transformation left pending leaves");

  std::vector<std::size_t> expected;
  for (std::size_t id = 0; id < 32; ++id) {
    expected.push_back(id);
  }
  expected.insert(expected.end(), {100, 101, 102, 103});
  std::sort(expected.begin(), expected.end());
  require_ids(index, expected, "failed forward merge changed the tree");
}

void test_model_downgrade() {
  configure(4, 64);
  HireSfcLiteIndex index(128, true);
  index.bulk_load(linear_bulk(16));
  for (std::size_t id = 0; id < 13; ++id) {
    require(index.erase(id), "downgrade delete failed");
  }
  require(index.wait_for_background_tasks(),
          "model downgrade did not finish");

  const DebugStats stats = index.debug_stats();
  require(stats.model_downgrade_count == 1,
          "low-fill model did not downgrade");
  require(stats.last_recalibration_job_kind == 4,
          "downgrade job kind was not recorded");
  require(stats.model_leaf_count == 0 && stats.legacy_leaf_count == 1,
          "downgrade produced the wrong leaf kind");
  require_ids(index, std::vector<std::size_t>{13, 14, 15},
              "model downgrade lost live records");
}

}  // namespace

int main() {
  test_forward_merge();
  test_backward_merge();
  test_failed_forward_preserves_tree();
  test_model_downgrade();
  std::cout << "HIRE SFC Stage 5 transformation tests passed\n";
  return 0;
}
