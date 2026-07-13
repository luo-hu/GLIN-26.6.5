#include "../src/benchmark/hire_sfc_lite_index.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using hire_sfc_lite::Box2D;
using hire_sfc_lite::DebugStats;
using hire_sfc_lite::HireSfcLiteIndex;
using hire_sfc_lite::RecordInput;

struct Query {
  double qmin = 0.0;
  double qmax = 0.0;
  Box2D box;
};

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void configure(bool rcu) {
  setenv("HIRE_SFC_LEAF_SIZE", "16", 1);
  setenv("HIRE_SFC_MODEL_LEAF_SIZE", "64", 1);
  setenv("HIRE_SFC_MIN_MODEL_LEAF", "4", 1);
  setenv("HIRE_SFC_EPSILON", "1.0", 1);
  setenv("HIRE_SFC_FORCE_LEGACY", "0", 1);
  setenv("HIRE_SFC_ENABLE_DELETED_SLOT_REUSE", "1", 1);
  setenv("HIRE_SFC_ENABLE_MBR_SKIP", "1", 1);
  setenv("HIRE_SFC_ENABLE_COST_RETRAIN", "0", 1);
  setenv("HIRE_SFC_ENABLE_LEGACY_TRANSFORM", "1", 1);
  setenv("HIRE_SFC_ENABLE_BACKGROUND_RECALIBRATION", rcu ? "1" : "0", 1);
  setenv("HIRE_SFC_ENABLE_RCU_RECALIBRATION", rcu ? "1" : "0", 1);
  setenv("HIRE_SFC_BACKGROUND_TEST_DELAY_US", "0", 1);
  setenv("HIRE_SFC_BUFFER_LIMIT", "8", 1);
  setenv("HIRE_SFC_TOMBSTONE_REBUILD_RATIO", "0.25", 1);
  setenv("HIRE_SFC_INTERNAL_FANOUT", "4", 1);
  setenv("HIRE_SFC_INTERNAL_GAP_FRACTION", "0.20", 1);
  setenv("HIRE_SFC_INTERNAL_LOG_FRACTION", "0.25", 1);
  setenv("HIRE_SFC_INTERNAL_MIN_FILL", "0.40", 1);
  setenv("HIRE_SFC_ENABLE_INTER_LEVEL_BULK_LOAD", "1", 1);
  setenv("HIRE_SFC_BULK_DELTA", "3", 1);
  setenv("HIRE_SFC_BULK_SEED_FRACTION", "0.25", 1);
}

RecordInput make_record(std::size_t id, double key, double span = 1.0) {
  const double y = static_cast<double>(id % 7) * 0.25;
  const double width = 0.2 + static_cast<double>(id % 5) * 0.05;
  return RecordInput{id, key, key + span,
                     Box2D{key - width, y, key + width, y + 0.4}};
}

std::vector<std::size_t> expected_query(
    const std::vector<RecordInput>& records,
    const std::vector<unsigned char>& live,
    const Query& query) {
  std::vector<std::size_t> result;
  for (const RecordInput& record : records) {
    if (record.id < live.size() && live[record.id] &&
        record.zmin <= query.qmax && record.zmax >= query.qmin &&
        hire_sfc_lite::intersects(record.box, query.box)) {
      result.push_back(record.id);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

void verify_query(const HireSfcLiteIndex& index,
                  const std::vector<RecordInput>& records,
                  const std::vector<unsigned char>& live,
                  const Query& query,
                  const std::string& context) {
  std::vector<std::size_t> actual;
  index.range_query(query.qmin, query.qmax, query.box, actual, nullptr);
  std::sort(actual.begin(), actual.end());
  const std::vector<std::size_t> expected =
      expected_query(records, live, query);
  if (actual != expected) {
    std::cerr << context << ": expected=" << expected.size()
              << " actual=" << actual.size() << " q=[" << query.qmin
              << "," << query.qmax << "] box=[" << query.box.xmin << ","
              << query.box.ymin << "," << query.box.xmax << ","
              << query.box.ymax << "]\n";
    std::vector<std::size_t> missing;
    std::vector<std::size_t> extra;
    std::set_difference(expected.begin(), expected.end(), actual.begin(),
                        actual.end(), std::back_inserter(missing));
    std::set_difference(actual.begin(), actual.end(), expected.begin(),
                        expected.end(), std::back_inserter(extra));
    std::cerr << "missing:";
    for (std::size_t id : missing) {
      std::cerr << " " << id;
    }
    std::cerr << " extra:";
    for (std::size_t id : extra) {
      std::cerr << " " << id;
    }
    std::cerr << "\n";
    for (std::size_t id : missing) {
      const hire_sfc_lite::DebugRecordState state =
          index.debug_record_state(id);
      std::cerr << "missing_state id=" << id << " alive=" << state.alive
                << " leaf=" << state.leaf_index
                << " mutable_main=" << state.mutable_main_occurrences
                << " mutable_buffer=" << state.mutable_buffer_occurrences
                << " mutable_tombstone="
                << state.mutable_tombstone_occurrences
                << " snapshot_main=" << state.snapshot_main_occurrences
                << " snapshot_buffer=" << state.snapshot_buffer_occurrences
                << "\n";
    }
    const DebugStats stats = index.debug_stats();
    std::cerr << "unsorted_main_leaves=" << stats.unsorted_main_leaf_count
              << " broken_sibling_links="
              << stats.broken_sibling_link_count
              << " out_of_order_leaves=" << stats.out_of_order_leaf_count
              << " stale_summaries=" << stats.stale_leaf_summary_count
              << "\n";
    if (stats.out_of_order_leaf_count > 0) {
      std::cerr << "first_out_of_order=" << stats.first_out_of_order_leaf
                << " left=[" << stats.first_out_of_order_left_min << ","
                << stats.first_out_of_order_left_max << "] right=["
                << stats.first_out_of_order_right_min << ","
                << stats.first_out_of_order_right_max << "]\n";
    }
  }
  require(actual == expected,
          context + ": range result differs from brute-force oracle");
}

void verify_workload(const HireSfcLiteIndex& index,
                     const std::vector<RecordInput>& records,
                     const std::vector<unsigned char>& live,
                     std::uint64_t seed,
                     const std::string& context) {
  double minimum = std::numeric_limits<double>::infinity();
  double maximum = -std::numeric_limits<double>::infinity();
  std::vector<std::size_t> live_ids;
  for (const RecordInput& record : records) {
    if (record.id < live.size() && live[record.id]) {
      minimum = std::min(minimum, record.zmin);
      maximum = std::max(maximum, record.zmax);
      live_ids.push_back(record.id);
    }
  }
  if (live_ids.empty()) {
    verify_query(index, records, live,
                 Query{-1.0, 1.0, Box2D{-1.0, -1.0, 1.0, 3.0}},
                 context);
    return;
  }

  verify_query(index, records, live,
               Query{minimum - 1.0, maximum + 1.0,
                     Box2D{minimum - 1.0, -1.0, maximum + 1.0, 3.0}},
               context + ": full range");

  std::mt19937_64 random(seed);
  std::uniform_real_distribution<double> key_distribution(minimum - 1.0,
                                                           maximum + 1.0);
  for (std::size_t query_id = 0; query_id < 32; ++query_id) {
    double first = key_distribution(random);
    double second = key_distribution(random);
    if (first > second) {
      std::swap(first, second);
    }
    const double box_first = key_distribution(random);
    const double box_width =
        std::max(0.01, (maximum - minimum + 1.0) /
                           static_cast<double>(2 + random() % 16));
    const double y_first = static_cast<double>(random() % 7) * 0.20;
    verify_query(index, records, live,
                 Query{first, second,
                       Box2D{box_first, y_first, box_first + box_width,
                             y_first + 0.75}},
                 context + ": random query");
  }

  for (std::size_t sample = 0;
       sample < std::min<std::size_t>(16, live_ids.size()); ++sample) {
    const RecordInput& record = records[live_ids[random() % live_ids.size()]];
    verify_query(index, records, live,
                 Query{record.zmin, record.zmin,
                       Box2D{record.box.xmin - 0.01, record.box.ymin - 0.01,
                             record.box.xmax + 0.01,
                             record.box.ymax + 0.01}},
                 context + ": point query");
  }
}

void verify_static_case(const std::string& name,
                        const std::vector<RecordInput>& records) {
  configure(false);
  std::vector<unsigned char> live(records.size(), 1);
  HireSfcLiteIndex index(records.size(), true);
  index.bulk_load(records);
  verify_workload(index, records, live, 20260713, name);
  const DebugStats stats = index.debug_stats();
  require(stats.broken_sibling_link_count == 0,
          name + ": broken sibling chain after bulk load");
  require(stats.directory_rebuild_count == 1,
          name + ": bulk load rebuilt the directory more than once");
  require(index.live_count() == records.size(),
          name + ": live count mismatch after bulk load");
}

void test_pathological_bulk_loads() {
  std::vector<RecordInput> duplicates;
  std::vector<RecordInput> identical;
  std::vector<RecordInput> skewed;
  for (std::size_t id = 0; id < 192; ++id) {
    duplicates.push_back(make_record(id, static_cast<double>(id / 4), 2.0));
    identical.push_back(make_record(id, 42.0, 1.0));
    const double skewed_key =
        id < 176 ? static_cast<double>(id) * 0.001
                 : 1000.0 + static_cast<double>(id - 176) * 100.0;
    skewed.push_back(make_record(id, skewed_key, 0.5));
  }
  verify_static_case("duplicate keys", duplicates);
  verify_static_case("identical keys", identical);
  verify_static_case("extreme skew", skewed);
}

void test_forced_split_paths() {
  configure(false);
  setenv("HIRE_SFC_FORCE_LEGACY", "1", 1);
  setenv("HIRE_SFC_ENABLE_LEGACY_TRANSFORM", "0", 1);
  constexpr std::size_t record_count = 128;
  std::vector<RecordInput> records;
  std::vector<unsigned char> live(record_count, 0);
  for (std::size_t id = 0; id < record_count; ++id) {
    records.push_back(make_record(id, static_cast<double>(id), 0.5));
  }
  HireSfcLiteIndex index(record_count, true);
  index.bulk_load(std::vector<RecordInput>(records.begin(),
                                           records.begin() + 16));
  std::fill(live.begin(), live.begin() + 16, 1);
  for (std::size_t id = 16; id < 96; ++id) {
    require(index.insert(records[id]), "forced split insert failed");
    live[id] = 1;
  }
  verify_workload(index, records, live, 424242, "forced split paths");
  const DebugStats stats = index.debug_stats();
  require(stats.leaf_split_count > 0,
          "forced workload did not exercise a leaf split");
  require(stats.internal_split_count > 0,
          "forced workload did not exercise an internal split");
  require(stats.directory_rebuild_count == 1,
          "forced split workload rebuilt the global directory");
  require(stats.broken_sibling_link_count == 0,
          "forced split workload broke sibling links");
}

void test_long_dynamic_churn() {
  configure(true);
  constexpr std::size_t capacity = 384;
  std::vector<RecordInput> records;
  records.reserve(capacity);
  for (std::size_t id = 0; id < capacity; ++id) {
    double key = 0.0;
    if (id < 160) {
      key = static_cast<double>(id / 3);
    } else if (id < 320) {
      key = 100.0 + static_cast<double>(id - 160) * 0.5;
    } else {
      key = 1000.0 + static_cast<double>(id - 320) * 50.0;
    }
    records.push_back(make_record(id, key, 0.5 + (id % 3) * 0.25));
  }

  std::vector<unsigned char> live(capacity, 0);
  std::vector<RecordInput> bulk(records.begin(), records.begin() + 96);
  for (std::size_t id = 0; id < bulk.size(); ++id) {
    live[id] = 1;
  }
  HireSfcLiteIndex index(capacity, true);
  index.bulk_load(bulk);

  for (std::size_t id = 96; id < 320; ++id) {
    require(index.insert(records[id]), "monotonic append insert failed");
    live[id] = 1;
    const DebugStats insert_stats = index.debug_stats();
    if (insert_stats.out_of_order_leaf_count > 0) {
      std::cerr << "first monotonic disorder after id=" << id
                << " leaf_splits=" << insert_stats.leaf_split_count
                << " local_rebuilds=" << insert_stats.local_rebuild_count
                << " transforms=" << insert_stats.legacy_transform_count
                << " forward="
                << insert_stats.legacy_forward_success_count
                << " backward="
                << insert_stats.legacy_backward_success_count
                << " pair_ids=" << insert_stats.first_out_of_order_left_id
                << "/" << insert_stats.first_out_of_order_right_id
                << " pair_live="
                << insert_stats.first_out_of_order_left_live << "/"
                << insert_stats.first_out_of_order_right_live
                << " pair_kind="
                << insert_stats.first_out_of_order_left_kind << "/"
                << insert_stats.first_out_of_order_right_kind << "\n";
    }
    require(insert_stats.out_of_order_leaf_count == 0,
            "monotonic append broke leaf ordering");
    if ((id + 1) % 32 == 0) {
      require(index.wait_for_background_tasks(5000),
              "background task timed out during monotonic append");
      verify_workload(index, records, live, 1000 + id,
                      "monotonic append checkpoint");
    }
  }

  for (std::size_t id = 0; id < 160; id += 2) {
    require(index.erase(id), "deterministic delete failed");
    live[id] = 0;
  }
  require(index.wait_for_background_tasks(5000),
          "background task timed out after deterministic deletes");
  verify_workload(index, records, live, 3001, "post-delete checkpoint");

  for (std::size_t id = 0; id < 80; id += 4) {
    records[id] = make_record(id, 500.0 + static_cast<double>(id) * 0.25,
                              0.75);
    require(index.insert(records[id]), "changed-key reinsert failed");
    live[id] = 1;
  }

  std::mt19937_64 random(20260713);
  for (std::size_t operation = 0; operation < 600; ++operation) {
    const std::size_t id = random() % capacity;
    if (live[id]) {
      require(index.erase(id), "random delete failed");
      live[id] = 0;
    } else {
      if (operation % 7 == 0) {
        const double shifted_key = records[id].zmin +
                                   static_cast<double>((operation % 11) + 1) *
                                       0.0001;
        records[id] = make_record(id, shifted_key, records[id].zmax -
                                                       records[id].zmin);
      }
      require(index.insert(records[id]), "random reinsert failed");
      live[id] = 1;
    }
    if ((operation + 1) % 50 == 0) {
      require(index.wait_for_background_tasks(5000),
              "background task timed out during random churn");
      verify_workload(index, records, live, 5000 + operation,
                      "random churn checkpoint");
    }
  }

  require(index.wait_for_background_tasks(5000),
          "final background task did not drain");
  verify_workload(index, records, live, 9001, "final dynamic state");

  const DebugStats stats = index.debug_stats();
  const std::size_t expected_live = static_cast<std::size_t>(
      std::count(live.begin(), live.end(), static_cast<unsigned char>(1)));
  require(index.live_count() == expected_live,
          "final dynamic live count differs from oracle");
  require(stats.directory_rebuild_count == 1,
          "dynamic churn performed a global directory rebuild");
  require(stats.mls_install_count > 0,
          "dynamic churn never installed a recalibrated MLS");
  require(stats.mls_update_log_entries == stats.mls_update_replay_count,
          "dynamic churn did not replay MLS updates exactly once");
  require(stats.background_job_abort_count == 0,
          "dynamic churn aborted a background recalibration");
  require(stats.pending_rebuild_count == 0,
          "dynamic churn left a pending leaf");
  require(stats.broken_sibling_link_count == 0,
          "dynamic churn broke sibling links");
  if (stats.unsorted_main_leaf_count != 0) {
    std::cerr << "first_unsorted leaf=" << stats.first_unsorted_leaf
              << " previous_id=" << stats.first_unsorted_previous_id
              << " previous_zmin=" << stats.first_unsorted_previous_zmin
              << " current_id=" << stats.first_unsorted_current_id
              << " current_zmin=" << stats.first_unsorted_current_zmin
              << " final_validation_repairs="
              << stats.mls_final_validation_repair_count << '\n';
  }
  require(stats.unsorted_main_leaf_count == 0,
          "dynamic churn left an unsorted main leaf");
  if (stats.out_of_order_leaf_count != 0) {
    std::cerr << "first_out_of_order leaf="
              << stats.first_out_of_order_leaf << " left_id="
              << stats.first_out_of_order_left_id << " left=["
              << stats.first_out_of_order_left_min << ","
              << stats.first_out_of_order_left_max << "] right_id="
              << stats.first_out_of_order_right_id << " right=["
              << stats.first_out_of_order_right_min << ","
              << stats.first_out_of_order_right_max << "] repairs="
              << stats.mls_final_validation_repair_count << '\n';
  }
  require(stats.out_of_order_leaf_count == 0,
          "dynamic churn broke global leaf ordering");
  require(stats.stale_leaf_summary_count == 0,
          "dynamic churn left an unsafe leaf summary");

  std::cout << "dynamic_live=" << expected_live
            << " leaf_splits=" << stats.leaf_split_count
            << " internal_splits=" << stats.internal_split_count
            << " mls_installs=" << stats.mls_install_count
            << " final_validation_repairs="
            << stats.mls_final_validation_repair_count << '\n';
}

}  // namespace

int main() {
  test_pathological_bulk_loads();
  test_forced_split_paths();
  test_long_dynamic_churn();
  std::cout << "HIRE SFC Stage 7 acceptance tests passed\n";
  return 0;
}
