#include "../src/benchmark/hire_bulk_loading.h"
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
using hire_sfc_lite::bulk_loading::OptimizationStats;

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void test_partition_optimizer() {
  std::vector<double> keys;
  for (std::size_t index = 0; index < 80; ++index) {
    keys.push_back(static_cast<double>(index * index + index));
  }
  std::vector<std::size_t> ends;
  for (std::size_t end = 8; end <= keys.size(); end += 8) {
    ends.push_back(end);
  }
  OptimizationStats stats;
  const std::vector<std::size_t> optimized =
      hire_sfc_lite::bulk_loading::optimize_partition_ends(
          keys.size(), ends, 8, 8, 2, 3,
          [&](std::size_t index) { return keys[index]; },
          [](std::size_t begin, std::size_t end) {
            const std::size_t count = end - begin;
            return count >= 4 && count <= 12;
          },
          stats);

  require(stats.boundaries_considered > 0 &&
              stats.candidate_evaluations > stats.boundaries_considered,
          "bulk optimizer did not evaluate delta-window candidates");
  require(stats.boundaries_shifted > 0 && stats.max_shift <= 3,
          "bulk optimizer did not apply a bounded separator shift");
  require(stats.rls_updates >= stats.boundaries_considered,
          "bulk optimizer did not update the RLS model online");
  std::size_t begin = 0;
  for (std::size_t end : optimized) {
    require(end > begin && end - begin >= 4 && end - begin <= 12,
            "bulk optimizer produced an invalid partition");
    begin = end;
  }
  require(begin == keys.size(), "bulk optimizer did not cover every key");
}

std::vector<RecordInput> make_records(std::size_t count) {
  std::vector<RecordInput> records;
  records.reserve(count);
  for (std::size_t id = 0; id < count; ++id) {
    const double key = static_cast<double>(id * id + 3 * id);
    records.push_back(
        RecordInput{id, key, key + 1.0, Box2D{key, 0.0, key + 1.0, 1.0}});
  }
  return records;
}

void configure(bool enabled) {
  setenv("HIRE_SFC_ENABLE_INTER_LEVEL_BULK_LOAD", enabled ? "1" : "0", 1);
  setenv("HIRE_SFC_BULK_DELTA", "3", 1);
  setenv("HIRE_SFC_BULK_SEED_FRACTION", "0.25", 1);
  setenv("HIRE_SFC_INTERNAL_FANOUT", "8", 1);
  setenv("HIRE_SFC_INTERNAL_GAP_FRACTION", "0.20", 1);
  setenv("HIRE_SFC_INTERNAL_MIN_FILL", "0.25", 1);
  setenv("HIRE_SFC_LEAF_SIZE", "16", 1);
  setenv("HIRE_SFC_MODEL_LEAF_SIZE", "64", 1);
  setenv("HIRE_SFC_MIN_MODEL_LEAF", "8", 1);
  setenv("HIRE_SFC_FORCE_LEGACY", "1", 1);
  setenv("HIRE_SFC_ENABLE_LEGACY_TRANSFORM", "0", 1);
  setenv("HIRE_SFC_ENABLE_BACKGROUND_RECALIBRATION", "0", 1);
  setenv("HIRE_SFC_ENABLE_RCU_RECALIBRATION", "0", 1);
}

std::vector<std::size_t> query_all(const HireSfcLiteIndex& index,
                                   double maximum_key) {
  std::vector<std::size_t> result;
  index.range_query(-1.0, maximum_key + 2.0,
                    Box2D{-1.0, -1.0, maximum_key + 2.0, 2.0}, result,
                    nullptr);
  std::sort(result.begin(), result.end());
  return result;
}

void test_inter_level_bulk_load() {
  const std::vector<RecordInput> records = make_records(1024);
  const double maximum_key = records.back().zmax;

  configure(false);
  HireSfcLiteIndex baseline(records.size(), true);
  baseline.bulk_load(records);
  const DebugStats baseline_stats = baseline.debug_stats();

  configure(true);
  HireSfcLiteIndex optimized(records.size(), true);
  optimized.bulk_load(records);
  const DebugStats optimized_stats = optimized.debug_stats();

  require(!baseline_stats.inter_level_bulk_enabled &&
              optimized_stats.inter_level_bulk_enabled,
          "bulk-load ablation switch was not applied");
  require(optimized_stats.bulk_internal_optimized_levels > 0 &&
              optimized_stats.bulk_internal_boundaries_considered > 0 &&
              optimized_stats.bulk_internal_candidate_evaluations > 0 &&
              optimized_stats.bulk_internal_rls_updates > 0,
          "internal levels did not use inter-level optimization");
  require(optimized_stats.bulk_internal_boundaries_shifted > 0 &&
              optimized_stats.bulk_internal_max_shift <= 3,
          "internal bulk loading did not shift a bounded separator");
  require(optimized_stats.leaf_count == baseline_stats.leaf_count &&
              optimized_stats.internal_levels == baseline_stats.internal_levels,
          "inter-level optimization changed leaf count or tree height");
  require(optimized_stats.avg_internal_model_error <=
              baseline_stats.avg_internal_model_error,
          "inter-level optimization did not reduce average internal error");
  require(optimized_stats.broken_sibling_link_count == 0,
          "inter-level bulk loading broke sibling links");
  require(optimized_stats.directory_rebuild_count == 1,
          "inter-level bulk loading rebuilt the directory more than once");

  std::vector<std::size_t> expected(records.size());
  for (std::size_t id = 0; id < expected.size(); ++id) {
    expected[id] = id;
  }
  require(query_all(baseline, maximum_key) == expected,
          "baseline bulk load lost records");
  require(query_all(optimized, maximum_key) == expected,
          "inter-level bulk load lost records");
  std::cout << "baseline_internal_error="
            << baseline_stats.max_internal_model_error
            << " optimized_internal_error="
            << optimized_stats.max_internal_model_error
            << " baseline_avg_error="
            << baseline_stats.avg_internal_model_error
            << " optimized_avg_error="
            << optimized_stats.avg_internal_model_error
            << " shifted="
            << optimized_stats.bulk_internal_boundaries_shifted
            << " levels=" << optimized_stats.internal_levels << "\n";
}

}  // namespace

int main() {
  test_partition_optimizer();
  test_inter_level_bulk_load();
  std::cout << "HIRE SFC Stage 6 bulk-loading tests passed\n";
  return 0;
}
