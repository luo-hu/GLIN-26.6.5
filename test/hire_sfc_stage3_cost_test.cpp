#include "../src/benchmark/hire_sfc_lite_index.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
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
  return RecordInput{id, key, key + 1.0, Box2D{key, 0.0, key + 1.0, 1.0}};
}

void configure_model_index() {
  setenv("HIRE_SFC_LEAF_SIZE", "16", 1);
  setenv("HIRE_SFC_MODEL_LEAF_SIZE", "64", 1);
  setenv("HIRE_SFC_MIN_MODEL_LEAF", "4", 1);
  setenv("HIRE_SFC_EPSILON", "1000000", 1);
  setenv("HIRE_SFC_FORCE_LEGACY", "0", 1);
  setenv("HIRE_SFC_ENABLE_LEGACY_TRANSFORM", "0", 1);
  setenv("HIRE_SFC_ENABLE_BACKGROUND_RECALIBRATION", "0", 1);
  setenv("HIRE_SFC_ENABLE_COST_RETRAIN", "1", 1);
  setenv("HIRE_SFC_TOMBSTONE_REBUILD_RATIO", "2.0", 1);
  setenv("HIRE_SFC_QUERY_WINDOW_BUCKETS", "4", 1);
  setenv("HIRE_SFC_COST_EMA_ALPHA", "0.5", 1);
}

std::vector<RecordInput> make_bulk() {
  std::vector<RecordInput> bulk;
  for (std::size_t id = 0; id < 16; ++id) {
    bulk.push_back(make_record(id, static_cast<double>(id)));
  }
  return bulk;
}

void run_queries(HireSfcLiteIndex& index, std::size_t count) {
  std::vector<std::size_t> candidates;
  const Box2D query_box{-1000.0, -1000.0, 1000.0, 1000.0};
  for (std::size_t i = 0; i < count; ++i) {
    index.range_query(-1.0, 1000.0, query_box, candidates, nullptr);
  }
}

void append_buffer_records(HireSfcLiteIndex& index, std::size_t begin_id,
                           std::size_t count) {
  for (std::size_t offset = 0; offset < count; ++offset) {
    require(index.insert(make_record(begin_id + offset, 7.1 + offset * 0.1)),
            "buffer insertion failed");
  }
}

void test_active_trigger() {
  configure_model_index();
  setenv("HIRE_SFC_QUERY_WINDOW_US", "1000000", 1);
  setenv("HIRE_SFC_ACTIVE_QUERY_THRESHOLD", "3", 1);
  setenv("HIRE_SFC_ACTIVE_BUFFER_THRESHOLD", "2", 1);
  setenv("HIRE_SFC_BUFFER_LIMIT", "64", 1);
  setenv("HIRE_SFC_COST_SAMPLE_EVERY", "1000", 1);
  setenv("HIRE_SFC_COST_BUFFER_NS_PER_ENTRY", "100", 1);
  setenv("HIRE_SFC_COST_MODEL_NS_PER_ENTRY", "1", 1);
  setenv("HIRE_SFC_COST_MERGE_NS_PER_RECORD", "0.01", 1);
  setenv("HIRE_SFC_COST_FIT_NS_PER_RECORD", "0.01", 1);

  HireSfcLiteIndex index(256, true);
  index.bulk_load(make_bulk());
  append_buffer_records(index, 100, 2);
  run_queries(index, 5);
  append_buffer_records(index, 102, 1);

  const DebugStats stats = index.debug_stats();
  require(stats.active_retrain_trigger_count == 1,
          "query-driven active trigger did not fire");
  require(stats.passive_retrain_trigger_count == 0,
          "active test unexpectedly fired passive trigger");
  require(stats.last_retrain_trigger_reason == 1,
          "active trigger reason was not recorded");
  require(stats.last_estimated_gain_ns > stats.last_estimated_retrain_ns,
          "active trigger violated the cost decision boundary");
  require(stats.merge_sample_count > 0 && stats.fit_sample_count > 0,
          "retraining did not sample merge and fit costs");
  require(stats.last_actual_retrain_ns > 0.0,
          "actual retraining cost was not measured");
  require(stats.buffer_records == 0,
          "active retraining did not merge the model buffer");
}

void test_passive_trigger() {
  configure_model_index();
  setenv("HIRE_SFC_QUERY_WINDOW_US", "1000000", 1);
  setenv("HIRE_SFC_ACTIVE_QUERY_THRESHOLD", "1000000", 1);
  setenv("HIRE_SFC_ACTIVE_BUFFER_THRESHOLD", "1000000", 1);
  setenv("HIRE_SFC_BUFFER_LIMIT", "3", 1);
  setenv("HIRE_SFC_COST_SAMPLE_EVERY", "64", 1);

  HireSfcLiteIndex index(256, true);
  index.bulk_load(make_bulk());
  append_buffer_records(index, 110, 3);

  const DebugStats stats = index.debug_stats();
  require(stats.passive_retrain_trigger_count == 1,
          "buffer-capacity passive trigger did not fire");
  require(stats.active_retrain_trigger_count == 0,
          "passive test unexpectedly fired active trigger");
  require(stats.last_retrain_trigger_reason == 2,
          "passive trigger reason was not recorded");
  require(stats.buffer_records == 0,
          "passive retraining did not merge the model buffer");
}

void test_cost_rejection_and_online_samples() {
  configure_model_index();
  setenv("HIRE_SFC_QUERY_WINDOW_US", "1000000", 1);
  setenv("HIRE_SFC_ACTIVE_QUERY_THRESHOLD", "2", 1);
  setenv("HIRE_SFC_ACTIVE_BUFFER_THRESHOLD", "2", 1);
  setenv("HIRE_SFC_BUFFER_LIMIT", "64", 1);
  setenv("HIRE_SFC_COST_SAMPLE_EVERY", "1", 1);
  setenv("HIRE_SFC_COST_MERGE_NS_PER_RECORD", "1000000", 1);
  setenv("HIRE_SFC_COST_FIT_NS_PER_RECORD", "1000000", 1);

  HireSfcLiteIndex index(256, true);
  index.bulk_load(make_bulk());
  append_buffer_records(index, 120, 2);
  run_queries(index, 3);
  append_buffer_records(index, 122, 1);

  const DebugStats stats = index.debug_stats();
  require(stats.active_retrain_trigger_count == 0,
          "costly retraining should have been rejected");
  require(stats.cost_retrain_rejected_count == 1,
          "cost-model rejection was not counted");
  require(stats.last_rejected_gain_ns <= stats.last_rejected_retrain_ns,
          "rejected retraining had a positive decision boundary");
  require(stats.buffer_scan_sample_count > 0 &&
              stats.model_scan_sample_count > 0,
          "online query cost samples were not collected");
  require(stats.buffer_records == 3,
          "rejected retraining unexpectedly consumed the buffer");
}

void test_query_window_expiration() {
  configure_model_index();
  setenv("HIRE_SFC_QUERY_WINDOW_US", "1000", 1);
  setenv("HIRE_SFC_QUERY_WINDOW_BUCKETS", "2", 1);
  setenv("HIRE_SFC_ACTIVE_QUERY_THRESHOLD", "2", 1);
  setenv("HIRE_SFC_ACTIVE_BUFFER_THRESHOLD", "2", 1);
  setenv("HIRE_SFC_BUFFER_LIMIT", "64", 1);
  setenv("HIRE_SFC_COST_SAMPLE_EVERY", "1000", 1);
  setenv("HIRE_SFC_COST_BUFFER_NS_PER_ENTRY", "100", 1);
  setenv("HIRE_SFC_COST_MODEL_NS_PER_ENTRY", "1", 1);
  setenv("HIRE_SFC_COST_MERGE_NS_PER_RECORD", "0.01", 1);
  setenv("HIRE_SFC_COST_FIT_NS_PER_RECORD", "0.01", 1);

  HireSfcLiteIndex index(256, true);
  index.bulk_load(make_bulk());
  append_buffer_records(index, 130, 2);
  run_queries(index, 3);
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  append_buffer_records(index, 132, 1);

  const DebugStats stats = index.debug_stats();
  require(stats.query_window_total == 0,
          "expired query buckets remained in the active window");
  require(stats.active_retrain_trigger_count == 0,
          "expired query heat incorrectly triggered retraining");
  require(stats.cost_retrain_rejected_count == 0,
          "expired window should fail before the cost decision");
}

}  // namespace

int main() {
  test_active_trigger();
  test_passive_trigger();
  test_cost_rejection_and_online_samples();
  test_query_window_expiration();
  std::cout << "HIRE SFC Stage 3 cost-model tests passed\n";
  return 0;
}
