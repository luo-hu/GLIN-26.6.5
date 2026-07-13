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
using hire_sfc_lite::QueryStats;
using hire_sfc_lite::RecordInput;

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

RecordInput make_record(std::size_t id, double zmin) {
  RecordInput input;
  input.id = id;
  input.zmin = zmin;
  input.zmax = zmin + 10.0;
  input.box = Box2D{zmin, 0.0, zmin + 1.0, 1.0};
  return input;
}

std::vector<std::size_t> expected_candidates(
    const std::vector<RecordInput>& records, const std::vector<char>& live,
    double qmin, double qmax, const Box2D& query_box) {
  std::vector<std::size_t> result;
  for (const RecordInput& record : records) {
    if (record.id < live.size() && live[record.id] &&
        record.zmin <= qmax && record.zmax >= qmin &&
        hire_sfc_lite::intersects(record.box, query_box)) {
      result.push_back(record.id);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

void verify_query(const HireSfcLiteIndex& index,
                  const std::vector<RecordInput>& records,
                  const std::vector<char>& live, double qmin, double qmax,
                  QueryStats* stats = nullptr) {
  const Box2D query_box{-1000.0, -1000.0, 1000.0, 1000.0};
  std::vector<std::size_t> actual;
  index.range_query(qmin, qmax, query_box, actual, stats);
  std::sort(actual.begin(), actual.end());
  require(actual == expected_candidates(records, live, qmin, qmax, query_box),
          "Stage 2 range query differs from brute-force reference");
}

void configure_common() {
  setenv("HIRE_SFC_LEAF_SIZE", "16", 1);
  setenv("HIRE_SFC_INTERNAL_FANOUT", "4", 1);
  setenv("HIRE_SFC_ENABLE_COST_RETRAIN", "0", 1);
  setenv("HIRE_SFC_ENABLE_LEGACY_TRANSFORM", "0", 1);
  setenv("HIRE_SFC_ENABLE_BACKGROUND_RECALIBRATION", "0", 1);
  setenv("HIRE_SFC_BUFFER_LIMIT", "64", 1);
  setenv("HIRE_SFC_TOMBSTONE_REBUILD_RATIO", "2.0", 1);
}

void test_model_buffer_swap_delete() {
  configure_common();
  setenv("HIRE_SFC_FORCE_LEGACY", "0", 1);
  setenv("HIRE_SFC_MODEL_LEAF_SIZE", "64", 1);
  setenv("HIRE_SFC_MIN_MODEL_LEAF", "4", 1);
  setenv("HIRE_SFC_EPSILON", "1000000", 1);

  std::vector<RecordInput> records;
  std::vector<char> live(128, 0);
  std::vector<RecordInput> bulk;
  for (std::size_t id = 0; id < 32; ++id) {
    bulk.push_back(make_record(id, static_cast<double>(id)));
    records.push_back(bulk.back());
    live[id] = 1;
  }

  HireSfcLiteIndex index(128, true);
  index.bulk_load(bulk);
  for (std::size_t offset = 0; offset < 5; ++offset) {
    const RecordInput input = make_record(100 + offset, 15.1 + offset * 0.1);
    records.push_back(input);
    require(index.insert(input), "model buffer insertion failed");
    live[input.id] = 1;
  }
  DebugStats stats = index.debug_stats();
  require(stats.buffer_records == 5, "unexpected model buffer size");
  require(stats.buffer_hash_entries == 5,
          "model buffer hashmap is not synchronized");

  require(index.erase(101), "model buffer deletion failed");
  live[101] = 0;
  stats = index.debug_stats();
  require(stats.buffer_records == 4, "buffer deletion did not remove the id");
  require(stats.buffer_hash_entries == 4,
          "buffer hashmap deletion did not remove the id");
  require(stats.buffer_swap_delete_count == 1,
          "middle buffer deletion did not use swap-last");
  require(stats.tombstone_records == 0,
          "physical buffer deletion unexpectedly created a tombstone");
  require(stats.directory_rebuild_count == 1,
          "model buffer update rebuilt the global directory");
  require(stats.broken_sibling_link_count == 0,
          "model leaf sibling links are inconsistent");
  verify_query(index, records, live, 18.0, 18.0);
}

void test_fixed_legacy_and_sibling_scan() {
  configure_common();
  setenv("HIRE_SFC_FORCE_LEGACY", "1", 1);
  setenv("HIRE_SFC_LEGACY_MIN_FILL", "0.40", 1);

  std::vector<RecordInput> records;
  std::vector<char> live(128, 0);
  std::vector<RecordInput> bulk;
  for (std::size_t id = 0; id < 48; ++id) {
    bulk.push_back(make_record(id, static_cast<double>(id)));
    records.push_back(bulk.back());
    live[id] = 1;
  }

  HireSfcLiteIndex index(128, true);
  index.bulk_load(bulk);
  DebugStats stats = index.debug_stats();
  require(stats.legacy_slots_used == 48, "legacy slots lost bulk data");
  require(stats.legacy_slot_capacity == stats.leaf_count * 16,
          "legacy leaves do not expose fixed capacity");
  require(stats.sibling_link_count == 2 * (stats.leaf_count - 1),
          "initial sibling chain is incomplete");

  const RecordInput inserted = make_record(100, 7.5);
  records.push_back(inserted);
  require(index.insert(inserted), "full legacy leaf insertion failed");
  live[inserted.id] = 1;
  stats = index.debug_stats();
  require(stats.leaf_split_count == 1,
          "full fixed-capacity legacy leaf did not split");
  require(stats.legacy_slots_used == 49, "legacy split lost a record");
  require(stats.legacy_slot_capacity == stats.leaf_count * 16,
          "split legacy leaves violated fixed capacity");
  require(stats.broken_sibling_link_count == 0,
          "legacy split broke sibling links");

  for (std::size_t id = 0; id < 3; ++id) {
    require(index.erase(id), "legacy in-place deletion failed");
    live[id] = 0;
  }
  stats = index.debug_stats();
  require(stats.leaf_merge_count > 0,
          "legacy underflow did not trigger a leaf merge");
  require(stats.directory_rebuild_count == 1,
          "legacy structural maintenance rebuilt the global directory");
  require(stats.broken_sibling_link_count == 0,
          "legacy merge broke sibling links");

  QueryStats query_stats;
  verify_query(index, records, live, 30.0, 35.0, &query_stats);
  require(query_stats.leaf_sibling_hops > 0,
          "Full range query did not traverse the sibling chain");
}

}  // namespace

int main() {
  test_model_buffer_swap_delete();
  test_fixed_legacy_and_sibling_scan();
  std::cout << "HIRE SFC Stage 2 leaf tests passed\n";
  return 0;
}
