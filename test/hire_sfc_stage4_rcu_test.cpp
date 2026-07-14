#include "../src/benchmark/hire_sfc_lite_index.h"

#include <algorithm>
#include <atomic>
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
  return RecordInput{id, key, key + 1.0,
                     Box2D{key, 0.0, key + 1.0, 1.0}};
}

void configure_rcu() {
  setenv("HIRE_SFC_LEAF_SIZE", "16", 1);
  setenv("HIRE_SFC_MODEL_LEAF_SIZE", "64", 1);
  setenv("HIRE_SFC_MIN_MODEL_LEAF", "4", 1);
  setenv("HIRE_SFC_EPSILON", "1000000", 1);
  setenv("HIRE_SFC_FORCE_LEGACY", "0", 1);
  setenv("HIRE_SFC_ENABLE_LEGACY_TRANSFORM", "0", 1);
  setenv("HIRE_SFC_ENABLE_COST_RETRAIN", "0", 1);
  setenv("HIRE_SFC_ENABLE_BACKGROUND_RECALIBRATION", "1", 1);
  setenv("HIRE_SFC_ENABLE_RCU_RECALIBRATION", "1", 1);
  setenv("HIRE_SFC_BACKGROUND_TEST_DELAY_US", "50000", 1);
  setenv("HIRE_SFC_COST_SAMPLE_EVERY", "1", 1);
  setenv("HIRE_SFC_BUFFER_LIMIT", "3", 1);
  setenv("HIRE_SFC_TOMBSTONE_REBUILD_RATIO", "2.0", 1);
  setenv("HIRE_SFC_QUERY_WINDOW_BUCKETS", "4", 1);
  setenv("HIRE_SFC_INTERNAL_FANOUT", "4", 1);
  setenv("HIRE_SFC_ENABLE_LOCAL_LEAF_BLOCKS", "1", 1);
  setenv("HIRE_SFC_ENABLE_LOCAL_LEAF_SIMD", "1", 1);
  setenv("HIRE_SFC_LOCAL_READ_BLOCK_SIZE", "4", 1);
}

std::vector<std::size_t> query_all(const HireSfcLiteIndex& index) {
  std::vector<std::size_t> result;
  index.range_query(-1000.0, 1000.0,
                    Box2D{-1000.0, -1000.0, 1000.0, 1000.0}, result,
                    nullptr);
  std::sort(result.begin(), result.end());
  return result;
}

void test_nonblocking_recalibration_and_log_replay() {
  configure_rcu();
  std::vector<RecordInput> bulk;
  std::vector<char> live(256, 0);
  for (std::size_t id = 0; id < 32; ++id) {
    bulk.push_back(make_record(id, static_cast<double>(id)));
    live[id] = 1;
  }

  HireSfcLiteIndex index(256, true);
  index.bulk_load(bulk);
  require(index.insert(make_record(100, 15.1)), "insert 100 failed");
  live[100] = 1;
  require(index.insert(make_record(101, 15.2)), "insert 101 failed");
  live[101] = 1;
  require(index.insert(make_record(102, 15.3)), "trigger insert failed");
  live[102] = 1;

  DebugStats stats = index.debug_stats();
  require(stats.background_job_count == 1,
          "passive trigger did not enqueue one background job");
  require(stats.pap_snapshot_count == 1 && stats.last_pap_levels > 0,
          "background job did not snapshot a PAP");
  require(stats.last_pap_sigma > 0,
          "retraining split bound did not produce sigma");

  require(index.insert(make_record(103, 15.4)), "logged insert 103 failed");
  live[103] = 1;
  require(index.insert(make_record(104, 15.5)), "logged insert 104 failed");
  live[104] = 1;
  require(index.erase(5), "logged main-list erase failed");
  live[5] = 0;
  require(index.erase(101), "logged buffer erase failed");
  live[101] = 0;

  std::vector<std::size_t> expected;
  for (std::size_t id = 0; id < live.size(); ++id) {
    if (live[id]) {
      expected.push_back(id);
    }
  }

  std::atomic<bool> readers_ok{true};
  std::vector<std::thread> readers;
  for (std::size_t thread_id = 0; thread_id < 4; ++thread_id) {
    readers.emplace_back([&]() {
      for (std::size_t iteration = 0; iteration < 500; ++iteration) {
        if (query_all(index) != expected) {
          readers_ok.store(false, std::memory_order_release);
          return;
        }
      }
    });
  }

  require(index.wait_for_background_tasks(5000),
          "background RCU retraining did not finish");
  for (std::thread& reader : readers) {
    reader.join();
  }
  require(readers_ok.load(std::memory_order_acquire),
          "reader observed a partial or stale MLS installation");
  require(query_all(index) == expected,
          "post-install result differs from brute-force oracle");

  stats = index.debug_stats();
  require(stats.mls_update_log_entries >= 4,
          "updates during retraining were not captured by the MLS log");
  require(stats.mls_update_replay_count >= 4,
          "MLS updates were not replayed before installation");
  require(stats.mls_update_log_entries == stats.mls_update_replay_count,
          "MLS update log was not replayed exactly once");
  require(stats.mls_install_count == 1,
          "retrained MLS was not installed exactly once");
  require(stats.background_job_abort_count == 0,
          "background retraining unexpectedly aborted");
  require(stats.rcu_full_root_publish_count == 1 &&
              stats.rcu_snapshot_publish_count == 1,
          "local MLS RCU republished the complete index root");
  require(stats.rcu_mls_pointer_swap_count >= 1,
          "MLS installation did not swap its stable parent-child slot");
  require(stats.rcu_local_leaf_publish_count >= 4,
          "foreground updates did not publish local leaf versions");
  require(stats.rcu_delta_append_count == 0 && stats.rcu_delta_entries == 0,
          "local MLS RCU leaked updates into a global ReadView delta");
  require(stats.rcu_retired_snapshot_count > 0 &&
              stats.rcu_reclaimed_snapshot_count > 0,
          "RCU grace-period retirement was not observed");
  require(stats.rcu_active_reader_count == 0,
          "reader epoch counter did not drain");
  require(stats.model_scan_sample_count > 0 &&
              stats.buffer_scan_sample_count > 0,
          "RCU readers did not feed measured CPU scan costs to Stage 3");
  require(stats.pending_rebuild_count == 0,
          "installed leaf remained marked under retraining");
  require(stats.broken_sibling_link_count == 0,
          "MLS installation broke sibling links");
}

void test_query_hotness_active_trigger() {
  configure_rcu();
  setenv("HIRE_SFC_BACKGROUND_TEST_DELAY_US", "0", 1);
  setenv("HIRE_SFC_ENABLE_COST_RETRAIN", "1", 1);
  setenv("HIRE_SFC_BUFFER_LIMIT", "64", 1);
  setenv("HIRE_SFC_ACTIVE_QUERY_THRESHOLD", "3", 1);
  setenv("HIRE_SFC_ACTIVE_BUFFER_THRESHOLD", "2", 1);
  setenv("HIRE_SFC_COST_SAMPLE_EVERY", "100000", 1);
  setenv("HIRE_SFC_COST_BUFFER_NS_PER_ENTRY", "100", 1);
  setenv("HIRE_SFC_COST_MODEL_NS_PER_ENTRY", "1", 1);
  setenv("HIRE_SFC_COST_MERGE_NS_PER_RECORD", "0.01", 1);
  setenv("HIRE_SFC_COST_FIT_NS_PER_RECORD", "0.01", 1);

  std::vector<RecordInput> bulk;
  for (std::size_t id = 0; id < 16; ++id) {
    bulk.push_back(make_record(id, static_cast<double>(id)));
  }
  HireSfcLiteIndex index(128, true);
  index.bulk_load(bulk);
  require(index.insert(make_record(100, 7.1)), "active insert 100 failed");
  require(index.insert(make_record(101, 7.2)), "active insert 101 failed");
  for (std::size_t query = 0; query < 5; ++query) {
    query_all(index);
  }
  require(index.insert(make_record(102, 7.3)),
          "active trigger insert failed");
  require(index.wait_for_background_tasks(5000),
          "active background retraining did not finish");

  const DebugStats stats = index.debug_stats();
  require(stats.active_retrain_trigger_count == 1,
          "RCU query window did not fire the active trigger");
  require(stats.passive_retrain_trigger_count == 0,
          "active RCU test unexpectedly fired the passive trigger");
  require(stats.pap_snapshot_count == 1 && stats.mls_install_count == 1,
          "active trigger did not complete PAP/MLS installation");
  require(stats.last_estimated_gain_ns > stats.last_estimated_retrain_ns,
          "active RCU trigger violated the measured cost boundary");
}

void test_local_spatial_tree_pruning_and_expansion() {
  configure_rcu();
  setenv("HIRE_SFC_FORCE_LEGACY", "1", 1);
  setenv("HIRE_SFC_ENABLE_COST_RETRAIN", "0", 1);
  setenv("HIRE_SFC_ENABLE_LOCAL_SPATIAL_TREE", "1", 1);

  std::vector<RecordInput> bulk;
  for (std::size_t id = 0; id < 128; ++id) {
    bulk.push_back(make_record(id, static_cast<double>(id * 10)));
  }
  HireSfcLiteIndex index(256, true);
  index.bulk_load(bulk);

  std::vector<std::size_t> candidates;
  hire_sfc_lite::QueryStats stats;
  index.range_query(399.0, 412.0, Box2D{399.0, -1.0, 412.0, 2.0},
                    candidates, &stats);
  require(candidates == std::vector<std::size_t>{40, 41},
          "local spatial tree changed a narrow query answer");
  const DebugStats initial = index.debug_stats();
  require(initial.rcu_spatial_tree_node_count > 1 &&
              initial.rcu_spatial_tree_levels >= 2,
          "local spatial tree did not build a hierarchy");
  require(stats.visited_leaves < initial.leaf_count,
          "local subtree summaries did not prune any leaves");

  candidates.clear();
  stats = hire_sfc_lite::QueryStats{};
  index.range_query(-1.0, 2000.0,
                    Box2D{549.0, -1.0, 552.0, 2.0}, candidates, &stats);
  require(candidates == std::vector<std::size_t>{55},
          "local leaf block summaries changed the query answer");
  require(stats.leaf_block_checks > 0 && stats.leaf_blocks_pruned > 0,
          "local leaf block summaries did not prune a block");
  require(stats.records_scanned < 16,
          "local leaf block summaries did not reduce record scans");

  require(index.insert(make_record(200, 5000.0)),
          "out-of-summary insert failed");
  candidates.clear();
  stats = hire_sfc_lite::QueryStats{};
  index.range_query(4999.0, 5002.0,
                    Box2D{4999.0, -1.0, 5002.0, 2.0}, candidates, &stats);
  require(candidates == std::vector<std::size_t>{200},
          "subtree expansion hid a newly published record");
  require(index.debug_stats().rcu_spatial_summary_publish_count >
              initial.rcu_spatial_summary_publish_count,
          "insert did not publish an expanded subtree summary");
}

void test_local_leaf_simd_matches_scalar() {
  configure_rcu();
  setenv("HIRE_SFC_FORCE_LEGACY", "1", 1);
  setenv("HIRE_SFC_ENABLE_COST_RETRAIN", "0", 1);
  setenv("HIRE_SFC_ENABLE_LOCAL_LEAF_BLOCKS", "0", 1);

  std::vector<RecordInput> bulk;
  for (std::size_t id = 0; id < 64; ++id) {
    bulk.push_back(make_record(id, static_cast<double>(id * 10)));
  }

  setenv("HIRE_SFC_ENABLE_LOCAL_LEAF_SIMD", "0", 1);
  HireSfcLiteIndex scalar_index(128, true);
  scalar_index.bulk_load(bulk);
  std::vector<std::size_t> scalar_candidates;
  hire_sfc_lite::QueryStats scalar_stats;
  scalar_index.range_query(-1.0, 1000.0,
                           Box2D{109.0, -1.0, 142.0, 2.0},
                           scalar_candidates, &scalar_stats);

  setenv("HIRE_SFC_ENABLE_LOCAL_LEAF_SIMD", "1", 1);
  HireSfcLiteIndex simd_index(128, true);
  simd_index.bulk_load(bulk);
  std::vector<std::size_t> simd_candidates;
  hire_sfc_lite::QueryStats simd_stats;
  simd_index.range_query(-1.0, 1000.0,
                         Box2D{109.0, -1.0, 142.0, 2.0}, simd_candidates,
                         &simd_stats);

  require(simd_candidates == scalar_candidates,
          "SoA/SIMD filtering differs from the scalar fallback");
  require(simd_candidates ==
              std::vector<std::size_t>({11, 12, 13, 14}),
          "SoA/SIMD filtering returned an unexpected answer");
#if defined(__AVX2__)
  require(scalar_stats.leaf_simd_batches == 0 &&
              simd_stats.leaf_simd_batches > 0,
          "local leaf SIMD switch did not select the AVX2 path");
#endif
}

}  // namespace

int main() {
  test_nonblocking_recalibration_and_log_replay();
  test_query_hotness_active_trigger();
  test_local_spatial_tree_pruning_and_expansion();
  test_local_leaf_simd_matches_scalar();
  std::cout << "HIRE SFC Stage 4 PAP/MLS/RCU tests passed\n";
  return 0;
}
