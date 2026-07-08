#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace hire_sfc_lite {

struct Box2D {
  double xmin = 0.0;
  double ymin = 0.0;
  double xmax = 0.0;
  double ymax = 0.0;
};

inline Box2D normalize(Box2D box) {
  if (box.xmin > box.xmax) {
    std::swap(box.xmin, box.xmax);
  }
  if (box.ymin > box.ymax) {
    std::swap(box.ymin, box.ymax);
  }
  return box;
}

inline bool intersects(const Box2D& a, const Box2D& b) {
  return !(a.xmax < b.xmin || b.xmax < a.xmin ||
           a.ymax < b.ymin || b.ymax < a.ymin);
}

inline void expand(Box2D& target, const Box2D& value) {
  target.xmin = std::min(target.xmin, value.xmin);
  target.ymin = std::min(target.ymin, value.ymin);
  target.xmax = std::max(target.xmax, value.xmax);
  target.ymax = std::max(target.ymax, value.ymax);
}

struct RecordInput {
  std::size_t id = 0;
  double zmin = 0.0;
  double zmax = 0.0;
  Box2D box;
};

struct QueryStats {
  std::size_t block_checks = 0;
  std::size_t visited_leaves = 0;
  std::size_t skipped_zmax_leaves = 0;
  std::size_t skipped_mbr_leaves = 0;
  std::size_t records_scanned = 0;
  std::size_t buffer_records_scanned = 0;
  std::size_t mbr_candidates = 0;
};

struct DebugStats {
  std::size_t leaf_count = 0;
  std::size_t model_leaf_count = 0;
  std::size_t legacy_leaf_count = 0;
  std::size_t buffer_records = 0;
  std::size_t tombstone_records = 0;
  std::size_t local_rebuild_count = 0;
  std::size_t directory_log_entries = 0;
  std::size_t directory_rebuild_count = 0;
  std::size_t deleted_slot_reuse_count = 0;
  std::size_t cost_retrain_trigger_count = 0;
  std::size_t legacy_transform_count = 0;
  std::size_t pending_rebuild_count = 0;
  std::size_t background_recalibration_count = 0;
  double avg_model_error = 0.0;
  double max_model_error = 0.0;
};

enum class LeafKind { Model, Legacy };

class HireSfcLiteIndex {
 public:
  explicit HireSfcLiteIndex(std::size_t object_count)
      : config_(Config::from_env()), records_(object_count) {}

  void bulk_load(const std::vector<RecordInput>& entries) {
    initialized_ = true;
    live_count_ = 0;
    leaves_.clear();
    directory_min_zmin_.clear();
    directory_max_zmin_.clear();
    directory_log_.clear();
    local_rebuild_count_ = 0;
    directory_rebuild_count_ = 0;
    deleted_slot_reuse_count_ = 0;
    cost_retrain_trigger_count_ = 0;
    legacy_transform_count_ = 0;
    background_recalibration_count_ = 0;
    ensure_record_size(max_id_plus_one(entries));

    std::vector<std::size_t> ids;
    ids.reserve(entries.size());
    for (const RecordInput& input : entries) {
      store_record(input);
      records_[input.id].alive = true;
      ids.push_back(input.id);
      ++live_count_;
    }
    sort_ids(ids);
    build_bulk_loaded_leaves(ids);
    rebuild_record_leaf_refs();
    rebuild_directory();
  }

  bool insert(const RecordInput& input) {
    initialized_ = true;
    ensure_record_size(input.id + 1);
    if (records_[input.id].alive) {
      return false;
    }
    const std::size_t previous_leaf_index = records_[input.id].leaf_index;
    store_record(input);
    records_[input.id].alive = true;
    ++live_count_;
    apply_pending_rebuilds();
    if (leaves_.empty()) {
      Leaf leaf;
      leaf.record_ids.push_back(input.id);
      rebuild_leaf_metadata(leaf);
      leaves_.push_back(std::move(leaf));
      rebuild_record_leaf_refs();
      rebuild_directory();
      return true;
    }

    std::size_t leaf_index = find_leaf_for_zmin(input.zmin);
    if (previous_leaf_index < leaves_.size()) {
      if (try_reuse_buffer_slot(previous_leaf_index, input.id)) {
        maybe_cost_retrain_leaf(previous_leaf_index);
        return true;
      }
      if (try_reuse_deleted_slot(previous_leaf_index, input.id)) {
        maybe_cost_retrain_leaf(previous_leaf_index);
        return true;
      }
    }
    if (try_reuse_buffer_slot(leaf_index, input.id)) {
      maybe_cost_retrain_leaf(leaf_index);
      return true;
    }
    if (try_reuse_deleted_slot(leaf_index, input.id)) {
      maybe_cost_retrain_leaf(leaf_index);
      return true;
    }
    Leaf& leaf = leaves_[leaf_index];
    leaf.buffer_ids.push_back(input.id);
    records_[input.id].leaf_index = leaf_index;
    expand_leaf_on_insert(leaf, input);
    note_directory_update(leaf_index);

    if (leaf.buffer_ids.size() >= config_.buffer_limit) {
      schedule_or_rebuild_leaf(leaf_index);
    } else {
      maybe_cost_retrain_leaf(leaf_index);
    }
    return true;
  }

  bool erase(std::size_t id) {
    if (id >= records_.size() || !records_[id].alive) {
      return false;
    }
    apply_pending_rebuilds();
    records_[id].alive = false;
    --live_count_;
    const std::size_t leaf_index = records_[id].leaf_index;
    if (leaf_index < leaves_.size()) {
      Leaf& leaf = leaves_[leaf_index];
      if (leaf.live_count > 0) {
        --leaf.live_count;
      }
      ++leaf.tombstone_count;
      if (should_rebuild_for_tombstones(leaf)) {
        schedule_or_rebuild_leaf(leaf_index);
      }
    }
    return true;
  }

  void range_query(double raw_qmin, double raw_qmax, const Box2D& raw_query_box,
                   std::vector<std::size_t>& candidate_ids,
                   QueryStats* stats) const {
    candidate_ids.clear();
    if (leaves_.empty()) {
      return;
    }
    double qmin = raw_qmin;
    double qmax = raw_qmax;
    if (qmin > qmax) {
      std::swap(qmin, qmax);
    }
    const Box2D query_box = normalize(raw_query_box);
    const std::size_t last_leaf = find_last_leaf_for_qmax(qmax);
    if (last_leaf == npos()) {
      return;
    }
    for (std::size_t i = 0; i <= last_leaf && i < leaves_.size(); ++i) {
      const Leaf& leaf = leaves_[i];
      if (stats != nullptr) {
        ++stats->block_checks;
      }
      if (leaf.live_count == 0) {
        continue;
      }
      if (leaf.min_zmin > qmax) {
        break;
      }
      if (config_.enable_zmax_skip && leaf.max_zmax < qmin) {
        if (stats != nullptr) {
          ++stats->skipped_zmax_leaves;
        }
        continue;
      }
      if (config_.enable_mbr_skip && !intersects(leaf.mbr, query_box)) {
        if (stats != nullptr) {
          ++stats->skipped_mbr_leaves;
        }
        continue;
      }
      if (stats != nullptr) {
        ++stats->visited_leaves;
      }
      ++leaf.query_count_recent;
      scan_sorted_ids(leaf.record_ids, qmin, qmax, query_box, candidate_ids,
                      stats, false);
      scan_unsorted_ids(leaf.buffer_ids, qmin, qmax, query_box, candidate_ids,
                        stats);
    }
  }

  std::size_t leaf_count() const { return leaves_.size(); }
  std::size_t node_count() const { return leaves_.empty() ? 0 : leaves_.size() + 1; }
  std::size_t height() const { return leaves_.empty() ? 0 : (leaves_.size() == 1 ? 1 : 2); }
  std::size_t live_count() const { return live_count_; }
  std::size_t local_rebuild_count() const { return local_rebuild_count_; }

  DebugStats debug_stats() const {
    DebugStats stats;
    stats.leaf_count = leaves_.size();
    stats.local_rebuild_count = local_rebuild_count_;
    stats.directory_log_entries = directory_log_.size();
    stats.directory_rebuild_count = directory_rebuild_count_;
    stats.deleted_slot_reuse_count = deleted_slot_reuse_count_;
    stats.cost_retrain_trigger_count = cost_retrain_trigger_count_;
    stats.legacy_transform_count = legacy_transform_count_;
    stats.background_recalibration_count = background_recalibration_count_;
    double error_sum = 0.0;
    for (const Leaf& leaf : leaves_) {
      if (leaf.kind == LeafKind::Model) {
        ++stats.model_leaf_count;
        error_sum += leaf.max_model_error;
        stats.max_model_error =
            std::max(stats.max_model_error, leaf.max_model_error);
      } else {
        ++stats.legacy_leaf_count;
      }
      stats.buffer_records += leaf.buffer_ids.size();
      stats.tombstone_records += leaf.tombstone_count;
      if (leaf.pending_rebuild) {
        ++stats.pending_rebuild_count;
      }
    }
    stats.avg_model_error =
        stats.model_leaf_count == 0
            ? 0.0
            : error_sum / static_cast<double>(stats.model_leaf_count);
    return stats;
  }

  std::size_t estimate_bytes() const {
    std::size_t bytes = sizeof(*this);
    bytes += records_.size() * sizeof(Record);
    bytes += leaves_.size() * sizeof(Leaf);
    bytes += directory_min_zmin_.size() * sizeof(double);
    bytes += directory_max_zmin_.size() * sizeof(double);
    bytes += directory_log_.size() * sizeof(DirectoryLogEntry);
    for (const Leaf& leaf : leaves_) {
      bytes += leaf.record_ids.capacity() * sizeof(std::size_t);
      bytes += leaf.buffer_ids.capacity() * sizeof(std::size_t);
    }
    return bytes;
  }

 private:
  struct Config {
    std::size_t leaf_size = 512;
    std::size_t model_leaf_size = 0;
    double epsilon = 32.0;
    std::size_t min_model_leaf = 128;
    std::size_t buffer_limit = 128;
    std::size_t active_query_threshold = 32;
    std::size_t active_buffer_threshold = 32;
    std::size_t directory_log_limit = 256;
    std::size_t legacy_transform_max_leaves = 4;
    double tombstone_rebuild_ratio = 0.25;
    bool enable_zmax_skip = true;
    bool enable_mbr_skip = true;
    bool force_legacy = false;
    bool enable_directory_log = true;
    bool enable_deleted_slot_reuse = true;
    bool enable_cost_retrain = true;
    bool enable_legacy_transform = true;
    bool enable_background_recalibration = false;

    static Config from_env() {
      Config config;
      config.leaf_size = std::max<std::size_t>(
          16, env_size("HIRE_SFC_LEAF_SIZE", config.leaf_size));
      config.model_leaf_size = std::max<std::size_t>(
          config.leaf_size,
          env_size("HIRE_SFC_MODEL_LEAF_SIZE", config.leaf_size * 4));
      config.epsilon = env_double("HIRE_SFC_EPSILON", config.epsilon);
      config.min_model_leaf =
          env_size("HIRE_SFC_MIN_MODEL_LEAF", config.min_model_leaf);
      config.buffer_limit = std::max<std::size_t>(
          1, env_size("HIRE_SFC_BUFFER_LIMIT", config.buffer_limit));
      config.active_query_threshold =
          env_size("HIRE_SFC_ACTIVE_QUERY_THRESHOLD",
                   config.active_query_threshold);
      config.active_buffer_threshold =
          env_size("HIRE_SFC_ACTIVE_BUFFER_THRESHOLD",
                   config.active_buffer_threshold);
      config.directory_log_limit =
          env_size("HIRE_SFC_DIRECTORY_LOG_LIMIT",
                   config.directory_log_limit);
      config.legacy_transform_max_leaves = std::max<std::size_t>(
          1, env_size("HIRE_SFC_LEGACY_TRANSFORM_MAX_LEAVES",
                      config.legacy_transform_max_leaves));
      config.tombstone_rebuild_ratio =
          env_double("HIRE_SFC_TOMBSTONE_REBUILD_RATIO",
                     config.tombstone_rebuild_ratio);
      config.enable_zmax_skip =
          env_bool("HIRE_SFC_ENABLE_ZMAX_SKIP", config.enable_zmax_skip);
      config.enable_mbr_skip =
          env_bool("HIRE_SFC_ENABLE_MBR_SKIP", config.enable_mbr_skip);
      config.force_legacy =
          env_bool("HIRE_SFC_FORCE_LEGACY", config.force_legacy);
      config.enable_directory_log =
          env_bool("HIRE_SFC_ENABLE_DIRECTORY_LOG",
                   config.enable_directory_log);
      config.enable_deleted_slot_reuse =
          env_bool("HIRE_SFC_ENABLE_DELETED_SLOT_REUSE",
                   config.enable_deleted_slot_reuse);
      config.enable_cost_retrain =
          env_bool("HIRE_SFC_ENABLE_COST_RETRAIN",
                   config.enable_cost_retrain);
      config.enable_legacy_transform =
          env_bool("HIRE_SFC_ENABLE_LEGACY_TRANSFORM",
                   config.enable_legacy_transform);
      config.enable_background_recalibration =
          env_bool("HIRE_SFC_ENABLE_BACKGROUND_RECALIBRATION",
                   config.enable_background_recalibration);
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
      std::string value(raw);
      return value == "1" || value == "true" || value == "TRUE" ||
             value == "on" || value == "ON";
    }
  };

  struct Record {
    double zmin = 0.0;
    double zmax = 0.0;
    Box2D box;
    bool alive = false;
    std::size_t leaf_index = npos();
  };

  struct Leaf {
    LeafKind kind = LeafKind::Legacy;
    std::vector<std::size_t> record_ids;
    std::vector<std::size_t> buffer_ids;
    double min_zmin = std::numeric_limits<double>::infinity();
    double max_zmin = -std::numeric_limits<double>::infinity();
    double max_zmax = -std::numeric_limits<double>::infinity();
    Box2D mbr;
    double slope = 0.0;
    double intercept = 0.0;
    double max_model_error = 0.0;
    std::size_t live_count = 0;
    std::size_t tombstone_count = 0;
    mutable std::size_t query_count_recent = 0;
    bool pending_rebuild = false;
  };

  struct DirectoryLogEntry {
    std::size_t leaf_index = 0;
    double min_zmin = std::numeric_limits<double>::infinity();
    double max_zmin = -std::numeric_limits<double>::infinity();
  };

  static std::size_t npos() {
    return std::numeric_limits<std::size_t>::max();
  }

  static std::size_t max_id_plus_one(const std::vector<RecordInput>& entries) {
    std::size_t result = 0;
    for (const RecordInput& input : entries) {
      result = std::max(result, input.id + 1);
    }
    return result;
  }

  void ensure_record_size(std::size_t size) {
    if (records_.size() < size) {
      records_.resize(size);
    }
  }

  void store_record(const RecordInput& input) {
    Record& record = records_[input.id];
    record.zmin = input.zmin;
    record.zmax = input.zmax;
    if (record.zmin > record.zmax) {
      std::swap(record.zmin, record.zmax);
    }
    record.box = normalize(input.box);
  }

  void sort_ids(std::vector<std::size_t>& ids) const {
    std::sort(ids.begin(), ids.end(), [&](std::size_t lhs, std::size_t rhs) {
      const Record& a = records_[lhs];
      const Record& b = records_[rhs];
      if (a.zmin != b.zmin) {
        return a.zmin < b.zmin;
      }
      if (a.zmax != b.zmax) {
        return a.zmax < b.zmax;
      }
      return lhs < rhs;
    });
  }

  Leaf make_leaf_from_range(const std::vector<std::size_t>& ids,
                            std::size_t begin, std::size_t end) {
    Leaf leaf;
    leaf.record_ids.assign(ids.begin() + begin, ids.begin() + end);
    rebuild_leaf_metadata(leaf);
    return leaf;
  }

  void build_bulk_loaded_leaves(const std::vector<std::size_t>& ids) {
    leaves_.clear();
    for (std::size_t begin = 0; begin < ids.size();) {
      std::size_t chosen_end =
          std::min(ids.size(), begin + config_.leaf_size);
      Leaf chosen = make_leaf_from_range(ids, begin, chosen_end);

      if (!config_.force_legacy && ids.size() - begin >= config_.min_model_leaf) {
        const std::size_t max_end =
            std::min(ids.size(), begin + config_.model_leaf_size);
        for (std::size_t end = max_end;
             end >= begin + config_.min_model_leaf;) {
          Leaf candidate = make_leaf_from_range(ids, begin, end);
          if (candidate.kind == LeafKind::Model) {
            chosen_end = end;
            chosen = std::move(candidate);
            break;
          }
          if (end <= begin + config_.leaf_size) {
            break;
          }
          end -= std::min(config_.leaf_size, end - begin);
        }
      }

      leaves_.push_back(std::move(chosen));
      begin = chosen_end;
    }
    if (config_.enable_legacy_transform) {
      for (std::size_t i = 0; i < leaves_.size();) {
        if (try_transform_legacy_run(i)) {
          continue;
        }
        ++i;
      }
    }
  }

  void rebuild_leaf_metadata(Leaf& leaf) {
    sort_ids(leaf.record_ids);
    leaf.buffer_ids.clear();
    leaf.min_zmin = std::numeric_limits<double>::infinity();
    leaf.max_zmin = -std::numeric_limits<double>::infinity();
    leaf.max_zmax = -std::numeric_limits<double>::infinity();
    leaf.live_count = 0;
    leaf.tombstone_count = 0;
    leaf.pending_rebuild = false;
    leaf.query_count_recent = 0;
    bool have_box = false;
    for (std::size_t id : leaf.record_ids) {
      const Record& record = records_[id];
      if (!record.alive) {
        ++leaf.tombstone_count;
        continue;
      }
      ++leaf.live_count;
      leaf.min_zmin = std::min(leaf.min_zmin, record.zmin);
      leaf.max_zmin = std::max(leaf.max_zmin, record.zmin);
      leaf.max_zmax = std::max(leaf.max_zmax, record.zmax);
      if (!have_box) {
        leaf.mbr = record.box;
        have_box = true;
      } else {
        expand(leaf.mbr, record.box);
      }
    }
    if (!have_box) {
      leaf.mbr = Box2D{};
    }
    fit_leaf_model(leaf);
  }

  void fit_leaf_model(Leaf& leaf) const {
    leaf.slope = 0.0;
    leaf.intercept = 0.0;
    leaf.max_model_error = 0.0;
    leaf.kind = LeafKind::Legacy;
    if (config_.force_legacy ||
        leaf.live_count < config_.min_model_leaf ||
        leaf.record_ids.size() < 2) {
      return;
    }

    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_xx = 0.0;
    double sum_xy = 0.0;
    std::size_t n = 0;
    for (std::size_t pos = 0; pos < leaf.record_ids.size(); ++pos) {
      const Record& record = records_[leaf.record_ids[pos]];
      if (!record.alive) {
        continue;
      }
      const double x = record.zmin;
      const double y = static_cast<double>(pos);
      sum_x += x;
      sum_y += y;
      sum_xx += x * x;
      sum_xy += x * y;
      ++n;
    }
    if (n < 2) {
      return;
    }
    const double dn = static_cast<double>(n);
    const double denom = dn * sum_xx - sum_x * sum_x;
    if (std::fabs(denom) > 1e-12) {
      leaf.slope = (dn * sum_xy - sum_x * sum_y) / denom;
      leaf.intercept = (sum_y - leaf.slope * sum_x) / dn;
    } else {
      leaf.slope = 0.0;
      leaf.intercept = sum_y / dn;
    }
    for (std::size_t pos = 0; pos < leaf.record_ids.size(); ++pos) {
      const Record& record = records_[leaf.record_ids[pos]];
      if (!record.alive) {
        continue;
      }
      const double predicted = leaf.slope * record.zmin + leaf.intercept;
      leaf.max_model_error =
          std::max(leaf.max_model_error,
                   std::fabs(predicted - static_cast<double>(pos)));
    }
    if (leaf.max_model_error <= config_.epsilon) {
      leaf.kind = LeafKind::Model;
    }
  }

  void expand_leaf_on_insert(Leaf& leaf, const RecordInput& input) {
    const Record& record = records_[input.id];
    if (leaf.live_count == 0) {
      leaf.min_zmin = record.zmin;
      leaf.max_zmin = record.zmin;
      leaf.max_zmax = record.zmax;
      leaf.mbr = record.box;
    } else {
      leaf.min_zmin = std::min(leaf.min_zmin, record.zmin);
      leaf.max_zmin = std::max(leaf.max_zmin, record.zmin);
      leaf.max_zmax = std::max(leaf.max_zmax, record.zmax);
      expand(leaf.mbr, record.box);
    }
    ++leaf.live_count;
  }

  bool should_rebuild_for_tombstones(const Leaf& leaf) const {
    const std::size_t total = leaf.live_count + leaf.tombstone_count;
    if (total == 0) {
      return true;
    }
    return static_cast<double>(leaf.tombstone_count) /
               static_cast<double>(total) >=
           config_.tombstone_rebuild_ratio;
  }

  void schedule_or_rebuild_leaf(std::size_t leaf_index) {
    if (leaf_index >= leaves_.size()) {
      return;
    }
    if (config_.enable_background_recalibration) {
      leaves_[leaf_index].pending_rebuild = true;
      ++background_recalibration_count_;
      return;
    }
    rebuild_leaf_range(leaf_index);
  }

  void apply_pending_rebuilds() {
    if (!config_.enable_background_recalibration) {
      return;
    }
    for (std::size_t i = 0; i < leaves_.size(); ++i) {
      if (leaves_[i].pending_rebuild) {
        rebuild_leaf_range(i);
        return;
      }
    }
  }

  void maybe_cost_retrain_leaf(std::size_t leaf_index) {
    if (!config_.enable_cost_retrain || leaf_index >= leaves_.size()) {
      return;
    }
    Leaf& leaf = leaves_[leaf_index];
    if (leaf.pending_rebuild ||
        leaf.buffer_ids.size() < config_.active_buffer_threshold ||
        leaf.query_count_recent < config_.active_query_threshold) {
      return;
    }
    const double gain =
        static_cast<double>(leaf.query_count_recent) *
        static_cast<double>(leaf.buffer_ids.size());
    const double cost =
        static_cast<double>(std::max<std::size_t>(
            1, leaf.live_count + leaf.buffer_ids.size()));
    if (gain > cost) {
      ++cost_retrain_trigger_count_;
      leaf.query_count_recent = 0;
      schedule_or_rebuild_leaf(leaf_index);
    }
  }

  bool can_place_in_sorted_slot(const Leaf& leaf, std::size_t slot,
                                double zmin) const {
    double prev = -std::numeric_limits<double>::infinity();
    for (std::size_t i = slot; i > 0; --i) {
      const std::size_t id = leaf.record_ids[i - 1];
      if (id < records_.size() && records_[id].alive) {
        prev = records_[id].zmin;
        break;
      }
    }
    double next = std::numeric_limits<double>::infinity();
    for (std::size_t i = slot + 1; i < leaf.record_ids.size(); ++i) {
      const std::size_t id = leaf.record_ids[i];
      if (id < records_.size() && records_[id].alive) {
        next = records_[id].zmin;
        break;
      }
    }
    return prev <= zmin && zmin <= next;
  }

  bool try_reuse_buffer_slot(std::size_t leaf_index, std::size_t new_id) {
    if (leaf_index >= leaves_.size()) {
      return false;
    }
    Leaf& leaf = leaves_[leaf_index];
    if (std::find(leaf.buffer_ids.begin(), leaf.buffer_ids.end(), new_id) ==
        leaf.buffer_ids.end()) {
      return false;
    }
    if (leaf.tombstone_count > 0) {
      --leaf.tombstone_count;
    }
    expand_leaf_on_insert(leaf, RecordInput{new_id, records_[new_id].zmin,
                                            records_[new_id].zmax,
                                            records_[new_id].box});
    note_directory_update(leaf_index);
    return true;
  }

  bool try_reuse_deleted_slot(std::size_t leaf_index, std::size_t new_id) {
    if (!config_.enable_deleted_slot_reuse || leaf_index >= leaves_.size()) {
      return false;
    }
    Leaf& leaf = leaves_[leaf_index];
    if (leaf.kind != LeafKind::Model || leaf.tombstone_count == 0) {
      return false;
    }
    const double zmin = records_[new_id].zmin;
    std::size_t best_slot = npos();
    double best_distance = std::numeric_limits<double>::infinity();
    const double predicted = leaf.slope * zmin + leaf.intercept;
    for (std::size_t slot = 0; slot < leaf.record_ids.size(); ++slot) {
      const std::size_t old_id = leaf.record_ids[slot];
      if (old_id < records_.size() &&
          (old_id == new_id || !records_[old_id].alive) &&
          can_place_in_sorted_slot(leaf, slot, zmin)) {
        const double distance =
            std::fabs(static_cast<double>(slot) - predicted);
        if (distance < best_distance) {
          best_distance = distance;
          best_slot = slot;
        }
      }
    }
    if (best_slot == npos()) {
      return false;
    }
    leaf.record_ids[best_slot] = new_id;
    records_[new_id].leaf_index = leaf_index;
    if (leaf.tombstone_count > 0) {
      --leaf.tombstone_count;
    }
    expand_leaf_on_insert(leaf, RecordInput{new_id, records_[new_id].zmin,
                                            records_[new_id].zmax,
                                            records_[new_id].box});
    ++deleted_slot_reuse_count_;
    note_directory_update(leaf_index);
    return true;
  }

  void rebuild_leaf_range(std::size_t leaf_index) {
    if (leaf_index >= leaves_.size()) {
      return;
    }
    std::vector<std::size_t> ids = leaves_[leaf_index].record_ids;
    ids.insert(ids.end(), leaves_[leaf_index].buffer_ids.begin(),
               leaves_[leaf_index].buffer_ids.end());
    std::vector<std::size_t> live_ids;
    live_ids.reserve(ids.size());
    for (std::size_t id : ids) {
      if (id < records_.size() && records_[id].alive) {
        live_ids.push_back(id);
      }
    }
    sort_ids(live_ids);

    std::vector<Leaf> replacement;
    for (std::size_t begin = 0; begin < live_ids.size();) {
      const std::size_t end =
          std::min(live_ids.size(), begin + config_.leaf_size);
      Leaf leaf;
      leaf.record_ids.assign(live_ids.begin() + begin, live_ids.begin() + end);
      rebuild_leaf_metadata(leaf);
      replacement.push_back(std::move(leaf));
      begin = end;
    }

    leaves_.erase(leaves_.begin() + static_cast<long>(leaf_index));
    leaves_.insert(leaves_.begin() + static_cast<long>(leaf_index),
                   std::make_move_iterator(replacement.begin()),
                   std::make_move_iterator(replacement.end()));
    ++local_rebuild_count_;
    if (leaf_index < leaves_.size()) {
      try_transform_legacy_run(leaf_index);
    }
    rebuild_record_leaf_refs();
    rebuild_directory();
  }

  bool try_transform_legacy_run(std::size_t center_index) {
    if (!config_.enable_legacy_transform || config_.force_legacy ||
        center_index >= leaves_.size() ||
        leaves_[center_index].kind != LeafKind::Legacy) {
      return false;
    }
    std::size_t begin = center_index;
    while (begin > 0 &&
           center_index - begin + 1 < config_.legacy_transform_max_leaves &&
           leaves_[begin - 1].kind == LeafKind::Legacy) {
      --begin;
    }
    std::size_t end = center_index + 1;
    while (end < leaves_.size() &&
           end - begin < config_.legacy_transform_max_leaves &&
           leaves_[end].kind == LeafKind::Legacy) {
      ++end;
    }

    std::vector<std::size_t> ids;
    for (std::size_t i = begin; i < end; ++i) {
      ids.insert(ids.end(), leaves_[i].record_ids.begin(),
                 leaves_[i].record_ids.end());
      ids.insert(ids.end(), leaves_[i].buffer_ids.begin(),
                 leaves_[i].buffer_ids.end());
    }
    std::vector<std::size_t> live_ids;
    live_ids.reserve(ids.size());
    for (std::size_t id : ids) {
      if (id < records_.size() && records_[id].alive) {
        live_ids.push_back(id);
      }
    }
    if (live_ids.size() < config_.min_model_leaf ||
        live_ids.size() > config_.model_leaf_size) {
      return false;
    }
    sort_ids(live_ids);
    Leaf candidate;
    candidate.record_ids = std::move(live_ids);
    rebuild_leaf_metadata(candidate);
    if (candidate.kind != LeafKind::Model) {
      return false;
    }
    leaves_.erase(leaves_.begin() + static_cast<long>(begin),
                  leaves_.begin() + static_cast<long>(end));
    leaves_.insert(leaves_.begin() + static_cast<long>(begin),
                   std::move(candidate));
    ++legacy_transform_count_;
    rebuild_record_leaf_refs();
    rebuild_directory();
    return true;
  }

  void rebuild_record_leaf_refs() {
    for (std::size_t leaf_index = 0; leaf_index < leaves_.size();
         ++leaf_index) {
      Leaf& leaf = leaves_[leaf_index];
      for (std::size_t id : leaf.record_ids) {
        if (id < records_.size()) {
          records_[id].leaf_index = leaf_index;
        }
      }
      for (std::size_t id : leaf.buffer_ids) {
        if (id < records_.size()) {
          records_[id].leaf_index = leaf_index;
        }
      }
    }
  }

  void rebuild_directory() {
    directory_min_zmin_.clear();
    directory_max_zmin_.clear();
    directory_log_.clear();
    directory_min_zmin_.reserve(leaves_.size());
    directory_max_zmin_.reserve(leaves_.size());
    for (const Leaf& leaf : leaves_) {
      directory_min_zmin_.push_back(leaf.min_zmin);
      directory_max_zmin_.push_back(leaf.max_zmin);
    }
    fit_directory_model();
    ++directory_rebuild_count_;
  }

  void note_directory_update(std::size_t leaf_index) {
    if (leaf_index >= leaves_.size()) {
      return;
    }
    if (!config_.enable_directory_log) {
      rebuild_directory();
      return;
    }
    DirectoryLogEntry entry;
    entry.leaf_index = leaf_index;
    entry.min_zmin = leaves_[leaf_index].min_zmin;
    entry.max_zmin = leaves_[leaf_index].max_zmin;
    directory_log_.push_back(entry);
    if (directory_log_.size() > config_.directory_log_limit) {
      rebuild_directory();
    }
  }

  void fit_directory_model() {
    directory_slope_ = 0.0;
    directory_intercept_ = 0.0;
    directory_max_error_ = 0.0;
    const std::size_t n = directory_min_zmin_.size();
    if (n < 2) {
      return;
    }
    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_xx = 0.0;
    double sum_xy = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      const double x = directory_min_zmin_[i];
      const double y = static_cast<double>(i);
      sum_x += x;
      sum_y += y;
      sum_xx += x * x;
      sum_xy += x * y;
    }
    const double dn = static_cast<double>(n);
    const double denom = dn * sum_xx - sum_x * sum_x;
    if (std::fabs(denom) > 1e-12) {
      directory_slope_ = (dn * sum_xy - sum_x * sum_y) / denom;
      directory_intercept_ = (sum_y - directory_slope_ * sum_x) / dn;
    } else {
      directory_intercept_ = sum_y / dn;
    }
    for (std::size_t i = 0; i < n; ++i) {
      const double predicted =
          directory_slope_ * directory_min_zmin_[i] + directory_intercept_;
      directory_max_error_ =
          std::max(directory_max_error_,
                   std::fabs(predicted - static_cast<double>(i)));
    }
  }

  std::size_t find_leaf_for_zmin(double zmin) const {
    auto lower = std::lower_bound(directory_max_zmin_.begin(),
                                  directory_max_zmin_.end(), zmin);
    if (lower == directory_max_zmin_.end()) {
      return leaves_.empty() ? 0 : leaves_.size() - 1;
    }
    return static_cast<std::size_t>(lower - directory_max_zmin_.begin());
  }

  std::size_t find_last_leaf_for_qmax(double qmax) const {
    if (directory_min_zmin_.empty()) {
      return npos();
    }
    if (directory_max_error_ > static_cast<double>(directory_min_zmin_.size()) ||
        directory_min_zmin_.size() < 8) {
      return find_last_leaf_binary(qmax);
    }
    double predicted = directory_slope_ * qmax + directory_intercept_;
    if (std::isnan(predicted) || std::isinf(predicted)) {
      return find_last_leaf_binary(qmax);
    }
    const long center = static_cast<long>(std::floor(predicted));
    const long radius =
        static_cast<long>(std::ceil(directory_max_error_)) + 2;
    long lo = std::max<long>(0, center - radius);
    long hi = std::min<long>(static_cast<long>(directory_min_zmin_.size()) - 1,
                             center + radius);
    while (lo > 0 && directory_min_zmin_[static_cast<std::size_t>(lo - 1)] > qmax) {
      --lo;
    }
    while (hi + 1 < static_cast<long>(directory_min_zmin_.size()) &&
           directory_min_zmin_[static_cast<std::size_t>(hi)] <= qmax) {
      ++hi;
    }
    std::size_t result = npos();
    for (long i = lo; i <= hi; ++i) {
      if (directory_min_zmin_[static_cast<std::size_t>(i)] <= qmax) {
        result = static_cast<std::size_t>(i);
      } else if (result != npos()) {
        break;
      }
    }
    if (result == npos()) {
      result = find_last_leaf_binary(qmax);
    }
    const std::size_t log_result = find_last_leaf_from_log(qmax);
    if (result == npos()) {
      return log_result;
    }
    if (log_result == npos()) {
      return result;
    }
    return std::max(result, log_result);
  }

  std::size_t find_last_leaf_binary(double qmax) const {
    auto upper = std::upper_bound(directory_min_zmin_.begin(),
                                  directory_min_zmin_.end(), qmax);
    if (upper == directory_min_zmin_.begin()) {
      return npos();
    }
    return static_cast<std::size_t>((upper - directory_min_zmin_.begin()) - 1);
  }

  std::size_t find_last_leaf_from_log(double qmax) const {
    std::size_t result = npos();
    for (const DirectoryLogEntry& entry : directory_log_) {
      if (entry.leaf_index < leaves_.size() && entry.min_zmin <= qmax) {
        result = result == npos() ? entry.leaf_index
                                  : std::max(result, entry.leaf_index);
      }
    }
    return result;
  }

  void scan_sorted_ids(const std::vector<std::size_t>& ids, double qmin,
                       double qmax, const Box2D& query_box,
                       std::vector<std::size_t>& candidate_ids,
                       QueryStats* stats, bool count_as_buffer) const {
    for (std::size_t id : ids) {
      if (id >= records_.size()) {
        continue;
      }
      const Record& record = records_[id];
      if (stats != nullptr) {
        if (count_as_buffer) {
          ++stats->buffer_records_scanned;
        } else {
          ++stats->records_scanned;
        }
      }
      if (record.zmin > qmax) {
        break;
      }
      evaluate_record(id, record, qmin, query_box, candidate_ids, stats);
    }
  }

  void scan_unsorted_ids(const std::vector<std::size_t>& ids, double qmin,
                         double qmax, const Box2D& query_box,
                         std::vector<std::size_t>& candidate_ids,
                         QueryStats* stats) const {
    for (std::size_t id : ids) {
      if (id >= records_.size()) {
        continue;
      }
      const Record& record = records_[id];
      if (stats != nullptr) {
        ++stats->buffer_records_scanned;
      }
      if (record.zmin > qmax) {
        continue;
      }
      evaluate_record(id, record, qmin, query_box, candidate_ids, stats);
    }
  }

  void evaluate_record(std::size_t id, const Record& record, double qmin,
                       const Box2D& query_box,
                       std::vector<std::size_t>& candidate_ids,
                       QueryStats* stats) const {
    if (!record.alive || record.zmax < qmin ||
        !intersects(record.box, query_box)) {
      return;
    }
    candidate_ids.push_back(id);
    if (stats != nullptr) {
      ++stats->mbr_candidates;
    }
  }

  Config config_;
  bool initialized_ = false;
  std::vector<Record> records_;
  std::vector<Leaf> leaves_;
  std::vector<double> directory_min_zmin_;
  std::vector<double> directory_max_zmin_;
  std::vector<DirectoryLogEntry> directory_log_;
  double directory_slope_ = 0.0;
  double directory_intercept_ = 0.0;
  double directory_max_error_ = 0.0;
  std::size_t live_count_ = 0;
  std::size_t local_rebuild_count_ = 0;
  std::size_t directory_rebuild_count_ = 0;
  std::size_t deleted_slot_reuse_count_ = 0;
  std::size_t cost_retrain_trigger_count_ = 0;
  std::size_t legacy_transform_count_ = 0;
  std::size_t background_recalibration_count_ = 0;
};

}  // namespace hire_sfc_lite
