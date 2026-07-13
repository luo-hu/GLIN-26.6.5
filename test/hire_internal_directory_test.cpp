#include "../src/benchmark/hire_internal_directory.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using Directory = hire_sfc_lite::HireInternalDirectory;
using LeafEntry = std::pair<std::size_t, double>;

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void sort_entries(std::vector<LeafEntry>& entries) {
  std::sort(entries.begin(), entries.end(), [](const LeafEntry& lhs,
                                               const LeafEntry& rhs) {
    if (lhs.second != rhs.second) {
      return lhs.second < rhs.second;
    }
    return lhs.first < rhs.first;
  });
}

void verify_routes(const Directory& directory,
                   const std::vector<LeafEntry>& raw_entries) {
  std::vector<LeafEntry> entries = raw_entries;
  sort_entries(entries);
  require(!entries.empty(), "reference entries must not be empty");
  for (int raw_key = 0; raw_key <= 200; ++raw_key) {
    const double key = static_cast<double>(raw_key);
    auto expected = std::lower_bound(
        entries.begin(), entries.end(), key,
        [](const LeafEntry& entry, double value) {
          return entry.second < value;
        });
    if (expected == entries.end()) {
      expected = entries.end() - 1;
    }
    require(directory.find_leaf(key) == expected->first,
            "directory lower-bound route mismatch");
  }
}

void erase_leaf(std::vector<LeafEntry>& entries, std::size_t id) {
  entries.erase(
      std::remove_if(entries.begin(), entries.end(),
                     [&](const LeafEntry& entry) { return entry.first == id; }),
      entries.end());
}

}  // namespace

int main() {
  setenv("HIRE_SFC_INTERNAL_FANOUT", "4", 1);
  setenv("HIRE_SFC_INTERNAL_GAP_FRACTION", "0.20", 1);
  setenv("HIRE_SFC_INTERNAL_LOG_FRACTION", "0.25", 1);
  setenv("HIRE_SFC_INTERNAL_MIN_FILL", "0.49", 1);

  Directory directory;
  std::vector<LeafEntry> entries;
  for (std::size_t id = 1; id <= 18; ++id) {
    entries.emplace_back(id, static_cast<double>(id * 10));
  }
  directory.bulk_load(entries);
  verify_routes(directory, entries);

  const Directory::PotentiallyAffectedPath short_pap =
      directory.snapshot_potentially_affected_path(9, 1);
  require(short_pap.target_leaf_id == 9,
          "PAP lost the target leaf id");
  require(!short_pap.leaf_to_root.empty(),
          "PAP did not snapshot the leaf parent");
  require(short_pap.mls_root_id == short_pap.leaf_to_root.back().node_id,
          "MLS root is not the highest copied PAP node");
  require(std::find(short_pap.covered_leaf_ids.begin(),
                    short_pap.covered_leaf_ids.end(), 9) !=
              short_pap.covered_leaf_ids.end(),
          "MLS coverage omitted the target leaf");

  const Directory::PotentiallyAffectedPath propagated_pap =
      directory.snapshot_potentially_affected_path(9, 64);
  require(propagated_pap.leaf_to_root.size() >= short_pap.leaf_to_root.size(),
          "larger sigma unexpectedly shortened the PAP");
  require(propagated_pap.covered_leaf_ids.size() >=
              short_pap.covered_leaf_ids.size(),
          "larger PAP unexpectedly reduced MLS leaf coverage");

  // The first bottom node underflows and merges with its right sibling.
  require(directory.remove_leaf(1), "failed to remove leaf 1");
  erase_leaf(entries, 1);
  require(directory.remove_leaf(2), "failed to remove leaf 2");
  erase_leaf(entries, 2);
  verify_routes(directory, entries);

  // The next node underflows next to the four-child merged node, forcing
  // redistribution instead of another merge.
  require(directory.remove_leaf(7), "failed to remove leaf 7");
  erase_leaf(entries, 7);
  require(directory.remove_leaf(8), "failed to remove leaf 8");
  erase_leaf(entries, 8);
  verify_routes(directory, entries);

  // Grow one leaf-parent beyond fanout and force an internal split/up-push.
  std::size_t left_id = 9;
  double left_max = 90.0;
  for (std::size_t offset = 0; offset < 5; ++offset) {
    const std::size_t new_id = 100 + offset;
    const double new_max = 91.0 + static_cast<double>(offset);
    require(directory.insert_leaf_after(left_id, new_id, left_max, new_max),
            "failed to insert split leaf");
    entries.emplace_back(new_id, new_max);
    left_id = new_id;
    left_max = new_max;
  }
  verify_routes(directory, entries);

  const Directory::DebugStats stats = directory.debug_stats();
  require(stats.internal_split_count > 0, "internal split was not triggered");
  require(stats.internal_merge_count > 0, "internal merge was not triggered");
  require(stats.internal_redistribution_count > 0,
          "internal redistribution was not triggered");
  require(stats.masked_delete_count > 0,
          "primary child deletion was not masked");
  require(stats.gap_insert_count + stats.log_insert_count > 0,
          "neither gap nor log insertion was triggered");
  std::cout << "internal_nodes=" << stats.internal_nodes
            << " levels=" << stats.levels
            << " splits=" << stats.internal_split_count
            << " merges=" << stats.internal_merge_count
            << " redistributions=" << stats.internal_redistribution_count
            << '\n';
  return 0;
}
