#pragma once

#include "hire_bulk_loading.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace hire_sfc_lite {

// Dynamic HIRE-style internal layer. Internal nodes and leaf children use
// stable IDs, so local leaf replacement never invalidates directory pointers.
class HireInternalDirectory {
 public:
  using NodeId = std::size_t;

  struct SearchStats {
    std::size_t visited_nodes = 0;
    std::size_t model_searches = 0;
    std::size_t fallback_searches = 0;
    std::size_t log_entries_scanned = 0;
  };

  struct DebugStats {
    std::size_t internal_nodes = 0;
    std::size_t levels = 0;
    std::size_t primary_entries = 0;
    std::size_t gap_slots = 0;
    std::size_t log_entries = 0;
    std::size_t log_compactions = 0;
    std::size_t boundary_updates = 0;
    std::size_t internal_split_count = 0;
    std::size_t internal_merge_count = 0;
    std::size_t internal_redistribution_count = 0;
    std::size_t gap_insert_count = 0;
    std::size_t log_insert_count = 0;
    std::size_t masked_delete_count = 0;
    bool inter_level_bulk_enabled = false;
    std::size_t bulk_optimized_levels = 0;
    std::size_t bulk_boundaries_considered = 0;
    std::size_t bulk_boundaries_shifted = 0;
    std::size_t bulk_candidate_evaluations = 0;
    std::size_t bulk_rls_updates = 0;
    std::size_t bulk_max_shift = 0;
    std::uint64_t bulk_build_ns = 0;
    double avg_model_error = 0.0;
    double max_model_error = 0.0;
  };

  struct PathNodeSnapshot {
    NodeId node_id = invalid_id();
    NodeId parent_id = invalid_id();
    bool children_are_leaves = false;
    std::size_t child_count = 0;
    std::size_t free_slots = 0;
  };

  struct PotentiallyAffectedPath {
    NodeId target_leaf_id = invalid_id();
    NodeId mls_root_id = invalid_id();
    std::size_t initial_sigma = 0;
    std::size_t residual_sigma = 0;
    std::vector<PathNodeSnapshot> leaf_to_root;
    std::vector<NodeId> covered_leaf_ids;
  };

  HireInternalDirectory() : config_(Config::from_env()) {}

  static NodeId invalid_id() {
    return std::numeric_limits<NodeId>::max();
  }

  void clear() {
    nodes_.clear();
    leaf_parent_.clear();
    root_id_ = invalid_id();
    next_internal_id_ = 1;
    log_compactions_ = 0;
    boundary_updates_ = 0;
    internal_split_count_ = 0;
    internal_merge_count_ = 0;
    internal_redistribution_count_ = 0;
    gap_insert_count_ = 0;
    log_insert_count_ = 0;
    masked_delete_count_ = 0;
    bulk_optimization_stats_ = bulk_loading::OptimizationStats{};
    bulk_optimized_levels_ = 0;
    bulk_build_ns_ = 0;
  }

  void bulk_load(
      const std::vector<std::pair<NodeId, double>>& leaf_max_keys) {
    clear();
    const auto build_start = std::chrono::steady_clock::now();
    if (leaf_max_keys.empty()) {
      bulk_build_ns_ = elapsed_ns(build_start);
      return;
    }

    std::vector<Entry> children;
    children.reserve(leaf_max_keys.size());
    for (const auto& leaf : leaf_max_keys) {
      children.push_back(Entry{leaf.second, leaf.first, true});
    }
    sort_entries(children);

    bool children_are_leaves = true;
    while (true) {
      std::vector<Entry> parents;
      const std::size_t target = bulk_target_children();
      std::vector<std::size_t> ends;
      for (std::size_t end = target; end < children.size(); end += target) {
        ends.push_back(end);
      }
      ends.push_back(children.size());
      if (config_.enable_inter_level_bulk_load && ends.size() > 1) {
        bulk_loading::OptimizationStats level_stats;
        ends = bulk_loading::optimize_partition_ends(
            children.size(), ends, target, config_.fanout,
            bulk_seed_children(), config_.bulk_delta,
            [&](std::size_t index) { return children[index].max_key; },
            [&](std::size_t begin, std::size_t end) {
              const std::size_t count = end - begin;
              return count <= config_.fanout &&
                     (count >= minimum_children() || end == children.size());
            },
            level_stats);
        if (level_stats.boundaries_considered > 0) {
          ++bulk_optimized_levels_;
        }
        bulk_optimization_stats_.add(level_stats);
      }
      std::size_t begin = 0;
      for (std::size_t end : ends) {
        std::vector<Entry> group(children.begin() + static_cast<long>(begin),
                                 children.begin() + static_cast<long>(end));
        const NodeId node_id = create_node(group, children_are_leaves,
                                           invalid_id());
        parents.push_back(Entry{node_max_key(node_id), node_id, true});
        begin = end;
      }
      if (parents.size() == 1) {
        root_id_ = parents.front().child;
        nodes_.at(root_id_).parent = invalid_id();
        bulk_build_ns_ = elapsed_ns(build_start);
        return;
      }
      children = std::move(parents);
      children_are_leaves = false;
    }
  }

  std::size_t find_leaf(double key, SearchStats* stats = nullptr) const {
    NodeId node_id = root_id_;
    while (node_id != invalid_id()) {
      const auto node_it = nodes_.find(node_id);
      if (node_it == nodes_.end()) {
        return invalid_id();
      }
      if (stats != nullptr) {
        ++stats->visited_nodes;
      }
      const Node& node = node_it->second;
      const Entry entry = find_entry(node, key, stats);
      if (!entry.active) {
        return invalid_id();
      }
      if (node.children_are_leaves) {
        return entry.child;
      }
      node_id = entry.child;
    }
    return invalid_id();
  }

  std::size_t find_leaf_for_insert(double key) const {
    NodeId node_id = root_id_;
    while (node_id != invalid_id()) {
      const auto node_it = nodes_.find(node_id);
      if (node_it == nodes_.end()) {
        return invalid_id();
      }
      const Node& node = node_it->second;
      const Entry entry = find_entry_for_insert(node, key);
      if (!entry.active) {
        return invalid_id();
      }
      if (node.children_are_leaves) {
        return entry.child;
      }
      node_id = entry.child;
    }
    return invalid_id();
  }

  void update_leaf_boundary(NodeId leaf_id, double new_max_key) {
    const auto parent_it = leaf_parent_.find(leaf_id);
    if (parent_it == leaf_parent_.end()) {
      return;
    }
    if (update_child_key(parent_it->second, leaf_id, new_max_key)) {
      ++boundary_updates_;
    }
  }

  bool insert_leaf_after(NodeId left_leaf_id, NodeId new_leaf_id,
                         double left_max_key, double new_max_key) {
    if (leaf_parent_.find(new_leaf_id) != leaf_parent_.end()) {
      return false;
    }
    const auto parent_it = leaf_parent_.find(left_leaf_id);
    if (parent_it == leaf_parent_.end()) {
      return false;
    }
    const NodeId parent_id = parent_it->second;
    update_child_key(parent_id, left_leaf_id, left_max_key);
    insert_child(parent_id, Entry{new_max_key, new_leaf_id, true});
    return true;
  }

  bool remove_leaf(NodeId leaf_id) {
    const auto parent_it = leaf_parent_.find(leaf_id);
    if (parent_it == leaf_parent_.end()) {
      return false;
    }
    const NodeId parent_id = parent_it->second;
    leaf_parent_.erase(parent_it);
    remove_child(parent_id, leaf_id);
    return true;
  }

  std::size_t node_count() const { return nodes_.size(); }

  bool inter_level_bulk_enabled() const {
    return config_.enable_inter_level_bulk_load;
  }

  std::size_t bulk_target_children_for_planning() const {
    return bulk_target_children();
  }

  std::size_t bulk_seed_children_for_planning() const {
    return bulk_seed_children();
  }

  std::size_t bulk_slot_count_for_planning() const { return config_.fanout; }

  std::size_t bulk_delta_for_planning() const { return config_.bulk_delta; }

  std::size_t height() const {
    std::size_t result = 0;
    NodeId node_id = root_id_;
    while (node_id != invalid_id()) {
      const auto it = nodes_.find(node_id);
      if (it == nodes_.end()) {
        break;
      }
      ++result;
      if (it->second.children_are_leaves) {
        break;
      }
      const std::vector<Entry> entries = collect_entries(it->second);
      node_id = entries.empty() ? invalid_id() : entries.front().child;
    }
    return result;
  }

  PotentiallyAffectedPath snapshot_potentially_affected_path(
      NodeId leaf_id, std::size_t sigma) const {
    PotentiallyAffectedPath result;
    result.target_leaf_id = leaf_id;
    result.initial_sigma = sigma;
    result.residual_sigma = sigma;
    const auto parent_it = leaf_parent_.find(leaf_id);
    if (parent_it == leaf_parent_.end()) {
      result.mls_root_id = leaf_id;
      result.covered_leaf_ids.push_back(leaf_id);
      return result;
    }

    NodeId node_id = parent_it->second;
    while (node_id != invalid_id()) {
      const auto node_it = nodes_.find(node_id);
      if (node_it == nodes_.end()) {
        break;
      }
      const Node& node = node_it->second;
      const std::size_t children = child_count(node);
      const std::size_t free_slots =
          children >= config_.fanout ? 0 : config_.fanout - children;
      result.leaf_to_root.push_back(PathNodeSnapshot{
          node.id, node.parent, node.children_are_leaves, children,
          free_slots});
      result.mls_root_id = node.id;
      if (result.residual_sigma <= free_slots) {
        result.residual_sigma = 0;
        break;
      }
      result.residual_sigma =
          (result.residual_sigma - free_slots + config_.fanout - 1) /
          config_.fanout;
      node_id = node.parent;
    }
    collect_descendant_leaves(result.mls_root_id,
                              result.covered_leaf_ids);
    std::sort(result.covered_leaf_ids.begin(),
              result.covered_leaf_ids.end());
    result.covered_leaf_ids.erase(
        std::unique(result.covered_leaf_ids.begin(),
                    result.covered_leaf_ids.end()),
        result.covered_leaf_ids.end());
    return result;
  }

  DebugStats debug_stats() const {
    DebugStats result;
    result.internal_nodes = nodes_.size();
    result.levels = height();
    result.log_compactions = log_compactions_;
    result.boundary_updates = boundary_updates_;
    result.internal_split_count = internal_split_count_;
    result.internal_merge_count = internal_merge_count_;
    result.internal_redistribution_count = internal_redistribution_count_;
    result.gap_insert_count = gap_insert_count_;
    result.log_insert_count = log_insert_count_;
    result.masked_delete_count = masked_delete_count_;
    result.inter_level_bulk_enabled =
        config_.enable_inter_level_bulk_load;
    result.bulk_optimized_levels = bulk_optimized_levels_;
    result.bulk_boundaries_considered =
        bulk_optimization_stats_.boundaries_considered;
    result.bulk_boundaries_shifted =
        bulk_optimization_stats_.boundaries_shifted;
    result.bulk_candidate_evaluations =
        bulk_optimization_stats_.candidate_evaluations;
    result.bulk_rls_updates = bulk_optimization_stats_.rls_updates;
    result.bulk_max_shift = bulk_optimization_stats_.max_shift;
    result.bulk_build_ns = bulk_build_ns_;
    for (const auto& pair : nodes_) {
      const Node& node = pair.second;
      result.log_entries += node.log.size();
      result.max_model_error =
          std::max(result.max_model_error, node.max_model_error);
      result.avg_model_error += node.max_model_error;
      for (const Entry& entry : node.slots) {
        if (entry.active) {
          ++result.primary_entries;
        } else {
          ++result.gap_slots;
        }
      }
    }
    if (!nodes_.empty()) {
      result.avg_model_error /= static_cast<double>(nodes_.size());
    }
    return result;
  }

  std::size_t estimate_bytes() const {
    std::size_t bytes = sizeof(*this);
    bytes += nodes_.size() * (sizeof(NodeId) + sizeof(Node));
    for (const auto& pair : nodes_) {
      bytes += pair.second.slots.capacity() * sizeof(Entry);
      bytes += pair.second.log.capacity() * sizeof(Entry);
    }
    bytes += leaf_parent_.size() * sizeof(std::pair<NodeId, NodeId>);
    return bytes;
  }

 private:
  struct Config {
    std::size_t fanout = 64;
    std::size_t bulk_delta = 32;
    double gap_fraction = 0.20;
    double log_fraction = 0.10;
    double min_fill_fraction = 0.40;
    double bulk_seed_fraction = 0.25;
    bool enable_inter_level_bulk_load = true;

    static Config from_env() {
      Config config;
      config.fanout = std::max<std::size_t>(
          4, env_size("HIRE_SFC_INTERNAL_FANOUT", config.fanout));
      config.bulk_delta =
          env_size("HIRE_SFC_BULK_DELTA", config.bulk_delta);
      config.gap_fraction = std::max(
          0.0, env_double("HIRE_SFC_INTERNAL_GAP_FRACTION",
                          config.gap_fraction));
      config.log_fraction = std::max(
          0.01, env_double("HIRE_SFC_INTERNAL_LOG_FRACTION",
                           config.log_fraction));
      config.min_fill_fraction = std::min(
          0.49,
          std::max(0.10, env_double("HIRE_SFC_INTERNAL_MIN_FILL",
                                    config.min_fill_fraction)));
      config.bulk_seed_fraction = std::min(
          1.0, std::max(0.05, env_double("HIRE_SFC_BULK_SEED_FRACTION",
                                        config.bulk_seed_fraction)));
      config.enable_inter_level_bulk_load = env_bool(
          "HIRE_SFC_ENABLE_INTER_LEVEL_BULK_LOAD",
          config.enable_inter_level_bulk_load);
      return config;
    }

    static std::size_t env_size(const char* name, std::size_t fallback) {
      const char* raw = std::getenv(name);
      if (raw == nullptr || *raw == '\0') {
        return fallback;
      }
      try {
        return static_cast<std::size_t>(std::stoull(raw));
      } catch (...) {
        return fallback;
      }
    }

    static double env_double(const char* name, double fallback) {
      const char* raw = std::getenv(name);
      if (raw == nullptr || *raw == '\0') {
        return fallback;
      }
      try {
        return std::stod(raw);
      } catch (...) {
        return fallback;
      }
    }

    static bool env_bool(const char* name, bool fallback) {
      const char* raw = std::getenv(name);
      if (raw == nullptr || *raw == '\0') {
        return fallback;
      }
      const std::string value(raw);
      return value == "1" || value == "true" || value == "TRUE" ||
             value == "on" || value == "ON";
    }
  };

  struct Entry {
    double max_key = -std::numeric_limits<double>::infinity();
    NodeId child = invalid_id();
    bool active = false;
  };

  struct Node {
    NodeId id = invalid_id();
    NodeId parent = invalid_id();
    bool children_are_leaves = false;
    std::vector<Entry> slots;
    std::vector<Entry> log;
    double slope = 0.0;
    double intercept = 0.0;
    double max_model_error = 0.0;
  };

  std::size_t bulk_target_children() const {
    return std::max<std::size_t>(
        2, static_cast<std::size_t>(
               std::floor(static_cast<double>(config_.fanout) /
                          (1.0 + config_.gap_fraction))));
  }

  std::size_t minimum_children() const {
    return std::max<std::size_t>(
        2, static_cast<std::size_t>(
               std::floor(config_.fanout * config_.min_fill_fraction)));
  }

  std::size_t bulk_seed_children() const {
    return std::max<std::size_t>(
        2, static_cast<std::size_t>(
               std::ceil(config_.fanout * config_.bulk_seed_fraction)));
  }

  template <typename TimePoint>
  static std::uint64_t elapsed_ns(const TimePoint& start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
  }

  static void sort_entries(std::vector<Entry>& entries) {
    std::sort(entries.begin(), entries.end(), [](const Entry& lhs,
                                                 const Entry& rhs) {
      if (lhs.max_key != rhs.max_key) {
        return lhs.max_key < rhs.max_key;
      }
      return lhs.child < rhs.child;
    });
  }

  NodeId create_node(const std::vector<Entry>& entries,
                     bool children_are_leaves, NodeId parent) {
    const NodeId id = next_internal_id_++;
    Node node;
    node.id = id;
    node.parent = parent;
    node.children_are_leaves = children_are_leaves;
    remap_node(node, entries);
    nodes_.emplace(id, std::move(node));
    set_child_parents(id);
    return id;
  }

  void remap_node(Node& node, const std::vector<Entry>& raw_entries) const {
    std::vector<Entry> entries = raw_entries;
    sort_entries(entries);
    node.slots.assign(config_.fanout, Entry{});
    node.log.clear();
    const std::size_t count = entries.size();
    for (std::size_t i = 0; i < count; ++i) {
      const std::size_t slot =
          count == 1 ? config_.fanout / 2
                     : (i * (config_.fanout - 1)) / (count - 1);
      node.slots[slot] = entries[i];
    }
    fit_model(node);
  }

  void fit_model(Node& node) const {
    node.slope = 0.0;
    node.intercept = 0.0;
    node.max_model_error = 0.0;
    double sx = 0.0;
    double sy = 0.0;
    double sxx = 0.0;
    double sxy = 0.0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < node.slots.size(); ++i) {
      if (!node.slots[i].active) {
        continue;
      }
      const double x = node.slots[i].max_key;
      const double y = static_cast<double>(i);
      sx += x;
      sy += y;
      sxx += x * x;
      sxy += x * y;
      ++count;
    }
    if (count == 0) {
      return;
    }
    const double n = static_cast<double>(count);
    const double denominator = n * sxx - sx * sx;
    if (std::fabs(denominator) > 1e-12) {
      node.slope = (n * sxy - sx * sy) / denominator;
      node.intercept = (sy - node.slope * sx) / n;
    } else {
      node.intercept = sy / n;
    }
    for (std::size_t i = 0; i < node.slots.size(); ++i) {
      if (!node.slots[i].active) {
        continue;
      }
      const double predicted =
          node.slope * node.slots[i].max_key + node.intercept;
      node.max_model_error =
          std::max(node.max_model_error,
                   std::fabs(predicted - static_cast<double>(i)));
    }
  }

  std::vector<Entry> collect_entries(const Node& node) const {
    std::unordered_map<NodeId, Entry> by_child;
    for (const Entry& entry : node.slots) {
      if (entry.active) {
        by_child[entry.child] = entry;
      }
    }
    for (const Entry& entry : node.log) {
      if (entry.active) {
        by_child[entry.child] = entry;
      } else {
        by_child.erase(entry.child);
      }
    }
    std::vector<Entry> result;
    result.reserve(by_child.size());
    for (const auto& pair : by_child) {
      result.push_back(pair.second);
    }
    sort_entries(result);
    return result;
  }

  void collect_descendant_leaves(
      NodeId node_id, std::vector<NodeId>& leaves) const {
    const auto node_it = nodes_.find(node_id);
    if (node_it == nodes_.end()) {
      return;
    }
    const Node& node = node_it->second;
    const std::vector<Entry> entries = collect_entries(node);
    if (node.children_are_leaves) {
      for (const Entry& entry : entries) {
        leaves.push_back(entry.child);
      }
      return;
    }
    for (const Entry& entry : entries) {
      collect_descendant_leaves(entry.child, leaves);
    }
  }

  Entry find_entry(const Node& node, double key,
                   SearchStats* stats) const {
    const bool use_model =
        node.max_model_error < static_cast<double>(config_.fanout) / 2.0;
    if (stats != nullptr) {
      if (use_model) {
        ++stats->model_searches;
      } else {
        ++stats->fallback_searches;
      }
      stats->log_entries_scanned += node.log.size();
    }

    // The current implementation uses the model to select the search mode but
    // completes correction over one cache-sized node to preserve lower-bound
    // correctness after arbitrary log updates.
    const std::vector<Entry> entries = collect_entries(node);
    if (entries.empty()) {
      return Entry{};
    }
    auto it = std::lower_bound(
        entries.begin(), entries.end(), key,
        [](const Entry& entry, double value) { return entry.max_key < value; });
    if (it == entries.end()) {
      return entries.back();
    }
    return *it;
  }

  Entry find_entry_for_insert(const Node& node, double key) const {
    const std::vector<Entry> entries = collect_entries(node);
    if (entries.empty()) {
      return Entry{};
    }
    auto it = std::lower_bound(
        entries.begin(), entries.end(), key,
        [](const Entry& entry, double value) { return entry.max_key < value; });
    if (it == entries.end()) {
      return entries.back();
    }
    if (it->max_key == key) {
      while (it + 1 != entries.end() && (it + 1)->max_key == key) {
        ++it;
      }
    }
    return *it;
  }

  bool primary_contains(const Node& node, NodeId child) const {
    for (const Entry& entry : node.slots) {
      if (entry.active && entry.child == child) {
        return true;
      }
    }
    return false;
  }

  bool child_exists(const Node& node, NodeId child) const {
    const std::vector<Entry> entries = collect_entries(node);
    return std::any_of(entries.begin(), entries.end(), [&](const Entry& entry) {
      return entry.child == child;
    });
  }

  std::size_t child_count(const Node& node) const {
    return collect_entries(node).size();
  }

  double node_max_key(NodeId node_id) const {
    const auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
      return -std::numeric_limits<double>::infinity();
    }
    const std::vector<Entry> entries = collect_entries(it->second);
    return entries.empty() ? -std::numeric_limits<double>::infinity()
                           : entries.back().max_key;
  }

  std::size_t log_limit() const {
    return std::max<std::size_t>(
        1, static_cast<std::size_t>(
               std::ceil(config_.fanout * config_.log_fraction)));
  }

  bool try_gap_insert(Node& node, const Entry& entry) {
    if (node.slots.empty()) {
      return false;
    }
    long predicted = static_cast<long>(
        std::llround(node.slope * entry.max_key + node.intercept));
    predicted = std::max<long>(
        0, std::min<long>(predicted,
                          static_cast<long>(node.slots.size()) - 1));
    Entry& slot = node.slots[static_cast<std::size_t>(predicted)];
    if (slot.active) {
      return false;
    }
    double previous = -std::numeric_limits<double>::infinity();
    for (long i = predicted - 1; i >= 0; --i) {
      if (node.slots[static_cast<std::size_t>(i)].active) {
        previous = node.slots[static_cast<std::size_t>(i)].max_key;
        break;
      }
    }
    double next = std::numeric_limits<double>::infinity();
    for (std::size_t i = static_cast<std::size_t>(predicted + 1);
         i < node.slots.size(); ++i) {
      if (node.slots[i].active) {
        next = node.slots[i].max_key;
        break;
      }
    }
    if (entry.max_key < previous || entry.max_key > next) {
      return false;
    }
    slot = entry;
    ++gap_insert_count_;
    return true;
  }

  void append_or_replace_log(Node& node, const Entry& entry,
                             bool count_insert) {
    for (Entry& existing : node.log) {
      if (existing.child == entry.child) {
        existing = entry;
        return;
      }
    }
    node.log.push_back(entry);
    if (count_insert) {
      ++log_insert_count_;
    }
  }

  void compact_log(NodeId node_id) {
    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
      return;
    }
    Node& node = it->second;
    const NodeId id = node.id;
    const NodeId parent = node.parent;
    const bool children_are_leaves = node.children_are_leaves;
    const std::vector<Entry> entries = collect_entries(node);
    remap_node(node, entries);
    node.id = id;
    node.parent = parent;
    node.children_are_leaves = children_are_leaves;
    set_child_parents(node_id);
    ++log_compactions_;
  }

  void set_child_parents(NodeId node_id) {
    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
      return;
    }
    const Node& node = it->second;
    for (const Entry& entry : collect_entries(node)) {
      if (node.children_are_leaves) {
        leaf_parent_[entry.child] = node_id;
      } else {
        auto child_it = nodes_.find(entry.child);
        if (child_it != nodes_.end()) {
          child_it->second.parent = node_id;
        }
      }
    }
  }

  void insert_child(NodeId node_id, const Entry& entry) {
    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
      return;
    }
    Node& node = it->second;
    if (child_exists(node, entry.child)) {
      update_child_key(node_id, entry.child, entry.max_key);
      return;
    }
    if (!try_gap_insert(node, entry)) {
      append_or_replace_log(node, entry, true);
    }
    if (node.children_are_leaves) {
      leaf_parent_[entry.child] = node_id;
    } else {
      auto child_it = nodes_.find(entry.child);
      if (child_it != nodes_.end()) {
        child_it->second.parent = node_id;
      }
    }
    if (child_count(node) > config_.fanout) {
      split_internal(node_id);
      return;
    }
    if (node.log.size() > log_limit()) {
      compact_log(node_id);
    }
    propagate_boundary(node_id);
  }

  bool update_child_key(NodeId node_id, NodeId child, double new_max_key) {
    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
      return false;
    }
    const std::vector<Entry> entries = collect_entries(it->second);
    const auto current = std::find_if(
        entries.begin(), entries.end(),
        [&](const Entry& entry) { return entry.child == child; });
    if (current == entries.end() || current->max_key == new_max_key) {
      return false;
    }
    Node& node = it->second;
    append_or_replace_log(node, Entry{new_max_key, child, true}, false);
    if (node.log.size() > log_limit()) {
      compact_log(node_id);
    }
    propagate_boundary(node_id);
    return true;
  }

  void propagate_boundary(NodeId node_id) {
    const auto it = nodes_.find(node_id);
    if (it == nodes_.end() || it->second.parent == invalid_id()) {
      return;
    }
    update_child_key(it->second.parent, node_id, node_max_key(node_id));
  }

  void split_internal(NodeId node_id) {
    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
      return;
    }
    Node& left = it->second;
    const NodeId old_parent = left.parent;
    const bool children_are_leaves = left.children_are_leaves;
    const std::vector<Entry> entries = collect_entries(left);
    const std::size_t split = entries.size() / 2;
    std::vector<Entry> left_entries(entries.begin(),
                                    entries.begin() + static_cast<long>(split));
    std::vector<Entry> right_entries(entries.begin() + static_cast<long>(split),
                                     entries.end());
    remap_node(left, left_entries);
    left.id = node_id;
    left.parent = old_parent;
    left.children_are_leaves = children_are_leaves;
    set_child_parents(node_id);
    const NodeId right_id =
        create_node(right_entries, children_are_leaves, old_parent);
    ++internal_split_count_;

    if (old_parent == invalid_id()) {
      const NodeId new_root = create_node(
          {Entry{node_max_key(node_id), node_id, true},
           Entry{node_max_key(right_id), right_id, true}},
          false, invalid_id());
      root_id_ = new_root;
      nodes_.at(node_id).parent = new_root;
      nodes_.at(right_id).parent = new_root;
      return;
    }
    update_child_key(old_parent, node_id, node_max_key(node_id));
    insert_child(old_parent,
                 Entry{node_max_key(right_id), right_id, true});
  }

  void remove_child(NodeId node_id, NodeId child) {
    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
      return;
    }
    Node& node = it->second;
    const bool in_primary = primary_contains(node, child);
    node.log.erase(
        std::remove_if(node.log.begin(), node.log.end(),
                       [&](const Entry& entry) { return entry.child == child; }),
        node.log.end());
    if (in_primary) {
      node.log.push_back(Entry{0.0, child, false});
      ++masked_delete_count_;
    }

    const std::size_t count = child_count(node);
    if (node_id == root_id_) {
      if (count == 0) {
        nodes_.erase(node_id);
        root_id_ = invalid_id();
      } else if (!node.children_are_leaves && count == 1) {
        const NodeId child_root = collect_entries(node).front().child;
        nodes_.at(child_root).parent = invalid_id();
        nodes_.erase(node_id);
        root_id_ = child_root;
      } else if (node.log.size() > log_limit()) {
        compact_log(node_id);
      }
      return;
    }
    if (count < minimum_children()) {
      rebalance_internal(node_id);
      return;
    }
    if (node.log.size() > log_limit()) {
      compact_log(node_id);
    }
    propagate_boundary(node_id);
  }

  void rebalance_internal(NodeId node_id) {
    auto node_it = nodes_.find(node_id);
    if (node_it == nodes_.end() || node_it->second.parent == invalid_id()) {
      return;
    }
    const NodeId parent_id = node_it->second.parent;
    auto parent_it = nodes_.find(parent_id);
    if (parent_it == nodes_.end()) {
      return;
    }
    const std::vector<Entry> siblings = collect_entries(parent_it->second);
    auto position = std::find_if(
        siblings.begin(), siblings.end(),
        [&](const Entry& entry) { return entry.child == node_id; });
    if (position == siblings.end() || siblings.size() < 2) {
      return;
    }
    const std::size_t index =
        static_cast<std::size_t>(position - siblings.begin());
    const NodeId sibling_id =
        index > 0 ? siblings[index - 1].child : siblings[index + 1].child;
    const NodeId left_id = index > 0 ? sibling_id : node_id;
    const NodeId right_id = index > 0 ? node_id : sibling_id;
    auto left_it = nodes_.find(left_id);
    auto right_it = nodes_.find(right_id);
    if (left_it == nodes_.end() || right_it == nodes_.end()) {
      return;
    }

    std::vector<Entry> combined = collect_entries(left_it->second);
    const std::vector<Entry> right_entries = collect_entries(right_it->second);
    combined.insert(combined.end(), right_entries.begin(), right_entries.end());
    sort_entries(combined);
    if (combined.size() <= config_.fanout) {
      const bool children_are_leaves = left_it->second.children_are_leaves;
      remap_node(left_it->second, combined);
      left_it->second.id = left_id;
      left_it->second.parent = parent_id;
      left_it->second.children_are_leaves = children_are_leaves;
      set_child_parents(left_id);
      nodes_.erase(right_id);
      ++internal_merge_count_;
      update_child_key(parent_id, left_id, node_max_key(left_id));
      remove_child(parent_id, right_id);
      return;
    }

    const std::size_t split = combined.size() / 2;
    std::vector<Entry> new_left(combined.begin(),
                                combined.begin() + static_cast<long>(split));
    std::vector<Entry> new_right(combined.begin() + static_cast<long>(split),
                                 combined.end());
    const bool children_are_leaves = left_it->second.children_are_leaves;
    remap_node(left_it->second, new_left);
    left_it->second.id = left_id;
    left_it->second.parent = parent_id;
    left_it->second.children_are_leaves = children_are_leaves;
    remap_node(right_it->second, new_right);
    right_it->second.id = right_id;
    right_it->second.parent = parent_id;
    right_it->second.children_are_leaves = children_are_leaves;
    set_child_parents(left_id);
    set_child_parents(right_id);
    ++internal_redistribution_count_;
    update_child_key(parent_id, left_id, node_max_key(left_id));
    update_child_key(parent_id, right_id, node_max_key(right_id));
  }

  Config config_;
  std::unordered_map<NodeId, Node> nodes_;
  std::unordered_map<NodeId, NodeId> leaf_parent_;
  NodeId root_id_ = invalid_id();
  NodeId next_internal_id_ = 1;
  std::size_t log_compactions_ = 0;
  std::size_t boundary_updates_ = 0;
  std::size_t internal_split_count_ = 0;
  std::size_t internal_merge_count_ = 0;
  std::size_t internal_redistribution_count_ = 0;
  std::size_t gap_insert_count_ = 0;
  std::size_t log_insert_count_ = 0;
  std::size_t masked_delete_count_ = 0;
  bulk_loading::OptimizationStats bulk_optimization_stats_;
  std::size_t bulk_optimized_levels_ = 0;
  std::uint64_t bulk_build_ns_ = 0;
};

}  // namespace hire_sfc_lite
