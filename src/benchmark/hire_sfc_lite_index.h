#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <condition_variable>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "hire_internal_directory.h"

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
  std::size_t internal_nodes_visited = 0;
  std::size_t internal_model_searches = 0;
  std::size_t internal_fallback_searches = 0;
  std::size_t internal_log_entries_scanned = 0;
  std::size_t leaf_sibling_hops = 0;
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
  std::size_t internal_node_count = 0;
  std::size_t internal_levels = 0;
  std::size_t internal_primary_entries = 0;
  std::size_t internal_gap_slots = 0;
  std::size_t internal_log_entries = 0;
  std::size_t internal_log_compactions = 0;
  std::size_t internal_boundary_updates = 0;
  std::size_t internal_split_count = 0;
  std::size_t internal_merge_count = 0;
  std::size_t internal_redistribution_count = 0;
  std::size_t internal_gap_insert_count = 0;
  std::size_t internal_log_insert_count = 0;
  std::size_t internal_masked_delete_count = 0;
  std::size_t leaf_split_count = 0;
  std::size_t leaf_merge_count = 0;
  std::size_t leaf_redistribution_count = 0;
  std::size_t buffer_hash_entries = 0;
  std::size_t buffer_swap_delete_count = 0;
  std::size_t legacy_slots_used = 0;
  std::size_t legacy_slot_capacity = 0;
  std::size_t sibling_link_count = 0;
  std::size_t broken_sibling_link_count = 0;
  std::size_t unsorted_main_leaf_count = 0;
  std::size_t first_unsorted_leaf = 0;
  std::size_t first_unsorted_previous_id = 0;
  std::size_t first_unsorted_current_id = 0;
  double first_unsorted_previous_zmin = 0.0;
  double first_unsorted_current_zmin = 0.0;
  std::size_t out_of_order_leaf_count = 0;
  std::size_t stale_leaf_summary_count = 0;
  std::size_t first_out_of_order_leaf = 0;
  double first_out_of_order_left_min = 0.0;
  double first_out_of_order_right_min = 0.0;
  double first_out_of_order_left_max = 0.0;
  double first_out_of_order_right_max = 0.0;
  std::size_t first_out_of_order_left_id = 0;
  std::size_t first_out_of_order_right_id = 0;
  std::size_t first_out_of_order_left_live = 0;
  std::size_t first_out_of_order_right_live = 0;
  int first_out_of_order_left_kind = 0;
  int first_out_of_order_right_kind = 0;
  std::size_t active_retrain_trigger_count = 0;
  std::size_t passive_retrain_trigger_count = 0;
  std::size_t cost_retrain_rejected_count = 0;
  std::size_t query_window_total = 0;
  std::size_t query_window_max = 0;
  std::size_t buffer_scan_sample_count = 0;
  std::size_t model_scan_sample_count = 0;
  std::size_t merge_sample_count = 0;
  std::size_t fit_sample_count = 0;
  std::size_t pap_snapshot_count = 0;
  std::size_t pap_max_levels = 0;
  std::size_t mls_install_count = 0;
  std::size_t mls_update_log_entries = 0;
  std::size_t mls_update_replay_count = 0;
  std::size_t mls_final_validation_repair_count = 0;
  std::size_t rcu_snapshot_publish_count = 0;
  std::size_t rcu_retired_snapshot_count = 0;
  std::size_t rcu_reclaimed_snapshot_count = 0;
  std::size_t rcu_active_reader_count = 0;
  std::size_t background_job_count = 0;
  std::size_t background_job_abort_count = 0;
  std::size_t last_pap_levels = 0;
  std::size_t last_pap_sigma = 0;
  std::size_t last_mls_covered_leaves = 0;
  std::size_t legacy_forward_attempt_count = 0;
  std::size_t legacy_forward_success_count = 0;
  std::size_t legacy_backward_attempt_count = 0;
  std::size_t legacy_backward_success_count = 0;
  std::size_t model_downgrade_count = 0;
  std::size_t legacy_transform_abort_count = 0;
  std::size_t legacy_coefficient_reject_count = 0;
  std::size_t last_transform_input_leaves = 0;
  std::size_t last_transform_input_records = 0;
  bool inter_level_bulk_enabled = false;
  std::size_t bulk_leaf_boundaries_considered = 0;
  std::size_t bulk_leaf_boundaries_shifted = 0;
  std::size_t bulk_leaf_candidate_evaluations = 0;
  std::size_t bulk_leaf_rls_updates = 0;
  std::size_t bulk_leaf_max_shift = 0;
  std::size_t bulk_internal_optimized_levels = 0;
  std::size_t bulk_internal_boundaries_considered = 0;
  std::size_t bulk_internal_boundaries_shifted = 0;
  std::size_t bulk_internal_candidate_evaluations = 0;
  std::size_t bulk_internal_rls_updates = 0;
  std::size_t bulk_internal_max_shift = 0;
  std::uint64_t bulk_load_ns = 0;
  std::uint64_t bulk_internal_build_ns = 0;
  int last_recalibration_job_kind = 0;
  double buffer_scan_ns_per_entry_ema = 0.0;
  double model_scan_ns_per_entry_ema = 0.0;
  double merge_ns_per_record_ema = 0.0;
  double fit_ns_per_record_ema = 0.0;
  double last_estimated_gain_ns = 0.0;
  double last_estimated_retrain_ns = 0.0;
  double last_rejected_gain_ns = 0.0;
  double last_rejected_retrain_ns = 0.0;
  double last_actual_retrain_ns = 0.0;
  double last_retrain_error_before = 0.0;
  double last_retrain_error_after = 0.0;
  int last_retrain_trigger_reason = 0;
  double max_internal_model_error = 0.0;
  double avg_internal_model_error = 0.0;
  double avg_model_error = 0.0;
  double max_model_error = 0.0;
};

struct DebugRecordState {
  bool alive = false;
  std::size_t leaf_index = std::numeric_limits<std::size_t>::max();
  std::size_t mutable_main_occurrences = 0;
  std::size_t mutable_buffer_occurrences = 0;
  std::size_t mutable_tombstone_occurrences = 0;
  std::size_t snapshot_main_occurrences = 0;
  std::size_t snapshot_buffer_occurrences = 0;
};

enum class LeafKind { Model, Legacy };

class HireSfcLiteIndex {
 private:
  struct ReadSnapshot;
  enum class UpdateKind : int;

 public:
  explicit HireSfcLiteIndex(std::size_t object_count,
                            bool enable_full_internal_directory = false)
      : config_(Config::from_env()),
        full_internal_directory_(enable_full_internal_directory),
        records_(object_count) {}

  ~HireSfcLiteIndex() { stop_background_worker(); }

  HireSfcLiteIndex(const HireSfcLiteIndex&) = delete;
  HireSfcLiteIndex& operator=(const HireSfcLiteIndex&) = delete;

  bool wait_for_background_tasks(std::size_t timeout_ms = 5000) {
    if (!rcu_enabled()) {
      return true;
    }
    std::unique_lock<std::mutex> lock(job_mutex_);
    return idle_cv_.wait_for(
        lock, std::chrono::milliseconds(timeout_ms),
        [&]() { return job_queue_.empty() && !worker_busy_; });
  }

  void bulk_load(const std::vector<RecordInput>& entries) {
    stop_background_worker();
    std::unique_lock<std::mutex> lock(writer_mutex_, std::defer_lock);
    if (rcu_enabled()) {
      lock.lock();
    }
    bulk_load_unlocked(entries);
    publish_read_snapshot_locked();
    if (lock.owns_lock()) {
      lock.unlock();
    }
    start_background_worker();
  }

  bool insert(const RecordInput& input) {
    std::unique_lock<std::mutex> lock(writer_mutex_, std::defer_lock);
    if (rcu_enabled()) {
      lock.lock();
    }
    const bool inserted = insert_unlocked(input);
    if (inserted && full_internal_directory_ &&
        config_.enable_legacy_transform && input.id < records_.size() &&
        records_[input.id].leaf_index < leaves_.size()) {
      const std::size_t leaf_index = records_[input.id].leaf_index;
      if (leaves_[leaf_index].kind == LeafKind::Legacy) {
        maybe_schedule_legacy_transformation(leaf_index);
      }
    }
    if (inserted && rcu_enabled()) {
      append_mls_update_locked(UpdateKind::Insert, input, input.id);
      publish_read_snapshot_locked();
    }
    return inserted;
  }

  bool erase(std::size_t id) {
    std::unique_lock<std::mutex> lock(writer_mutex_, std::defer_lock);
    if (rcu_enabled()) {
      lock.lock();
    }
    RecordInput deleted;
    if (id < records_.size()) {
      deleted = RecordInput{id, records_[id].zmin, records_[id].zmax,
                            records_[id].box};
    }
    const bool erased = erase_unlocked(id);
    if (erased && full_internal_directory_ &&
        config_.enable_legacy_transform && !leaves_.empty()) {
      const std::size_t leaf_index = find_leaf_for_zmin(deleted.zmin);
      if (leaf_index < leaves_.size() &&
          leaves_[leaf_index].kind == LeafKind::Legacy) {
        maybe_schedule_legacy_transformation(leaf_index);
      }
    }
    if (erased && rcu_enabled()) {
      append_mls_update_locked(UpdateKind::Erase, deleted, id);
      publish_read_snapshot_locked();
    }
    return erased;
  }

  void range_query(double raw_qmin, double raw_qmax,
                   const Box2D& raw_query_box,
                   std::vector<std::size_t>& candidate_ids,
                   QueryStats* stats) const {
    if (rcu_enabled()) {
      const std::shared_ptr<const ReadSnapshot> snapshot =
          std::atomic_load_explicit(&read_snapshot_,
                                    std::memory_order_acquire);
      if (snapshot) {
        range_query_snapshot(*snapshot, raw_qmin, raw_qmax, raw_query_box,
                             candidate_ids, stats);
        return;
      }
    }
    range_query_mutable(raw_qmin, raw_qmax, raw_query_box, candidate_ids,
                        stats);
  }

  void bulk_load_unlocked(const std::vector<RecordInput>& entries) {
    const std::uint64_t bulk_start = steady_now_ns();
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
    pap_snapshot_count_ = 0;
    pap_max_levels_ = 0;
    mls_install_count_ = 0;
    mls_update_log_entries_ = 0;
    mls_update_replay_count_ = 0;
    mls_final_validation_repair_count_ = 0;
    rcu_snapshot_publish_count_ = 0;
    rcu_retired_snapshot_count_ = 0;
    rcu_reclaimed_snapshot_count_ = 0;
    background_job_count_ = 0;
    background_job_abort_count_ = 0;
    last_pap_levels_ = 0;
    last_pap_sigma_ = 0;
    last_mls_covered_leaves_ = 0;
    legacy_forward_attempt_count_ = 0;
    legacy_forward_success_count_ = 0;
    legacy_backward_attempt_count_ = 0;
    legacy_backward_success_count_ = 0;
    model_downgrade_count_ = 0;
    legacy_transform_abort_count_ = 0;
    legacy_coefficient_reject_count_ = 0;
    last_transform_input_leaves_ = 0;
    last_transform_input_records_ = 0;
    last_recalibration_job_kind_ = RecalibrationKind::ModelRetrain;
    bulk_leaf_optimization_stats_ = bulk_loading::OptimizationStats{};
    bulk_load_ns_ = 0;
    next_update_sequence_ = 1;
    active_recalibrations_.clear();
    read_leaf_cache_.clear();
    retired_read_snapshots_.clear();
    leaf_split_count_ = 0;
    leaf_merge_count_ = 0;
    leaf_redistribution_count_ = 0;
    buffer_swap_delete_count_ = 0;
    active_retrain_trigger_count_ = 0;
    passive_retrain_trigger_count_ = 0;
    cost_retrain_rejected_count_ = 0;
    buffer_scan_cost_ = CostEma{};
    model_scan_cost_ = CostEma{};
    merge_cost_ = CostEma{};
    fit_cost_ = CostEma{};
    rcu_model_scan_ns_.store(0, std::memory_order_relaxed);
    rcu_model_scan_entries_.store(0, std::memory_order_relaxed);
    rcu_model_scan_samples_.store(0, std::memory_order_relaxed);
    rcu_buffer_scan_ns_.store(0, std::memory_order_relaxed);
    rcu_buffer_scan_entries_.store(0, std::memory_order_relaxed);
    rcu_buffer_scan_samples_.store(0, std::memory_order_relaxed);
    last_estimated_gain_ns_ = 0.0;
    last_estimated_retrain_ns_ = 0.0;
    last_rejected_gain_ns_ = 0.0;
    last_rejected_retrain_ns_ = 0.0;
    last_actual_retrain_ns_ = 0.0;
    last_retrain_error_before_ = 0.0;
    last_retrain_error_after_ = 0.0;
    last_retrain_trigger_reason_ = TriggerReason::None;
    next_leaf_node_id_ = 1;
    directory_ready_ = false;
    leaf_id_to_index_.clear();
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
    bulk_load_ns_ = steady_now_ns() - bulk_start;
  }

  bool insert_unlocked(const RecordInput& input) {
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
      assign_leaf_node_id(leaf);
      leaf.record_ids.push_back(input.id);
      rebuild_leaf_metadata(leaf);
      leaves_.push_back(std::move(leaf));
      rebuild_record_leaf_refs();
      rebuild_directory();
      return true;
    }

    std::size_t leaf_index = find_leaf_for_insert(input.zmin);
    if (previous_leaf_index == leaf_index &&
        previous_leaf_index < leaves_.size()) {
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
    if (full_internal_directory_ &&
        leaves_[leaf_index].kind == LeafKind::Legacy) {
      insert_into_legacy_leaf(leaf_index, input.id);
      return true;
    }
    Leaf& leaf = leaves_[leaf_index];
    append_model_buffer(leaf_index, input.id);
    expand_leaf_on_insert(leaf, input);
    note_directory_update(leaf_index);

    if (full_internal_directory_) {
      maybe_cost_retrain_leaf(leaf_index);
    } else if (leaf.buffer_ids.size() >= config_.buffer_limit) {
      schedule_or_rebuild_leaf(leaf_index);
    } else {
      maybe_cost_retrain_leaf(leaf_index);
    }
    return true;
  }

  bool erase_unlocked(std::size_t id) {
    if (id >= records_.size() || !records_[id].alive) {
      return false;
    }
    apply_pending_rebuilds();
    records_[id].alive = false;
    --live_count_;
    const std::size_t leaf_index = records_[id].leaf_index;
    if (leaf_index < leaves_.size()) {
      if (full_internal_directory_ &&
          leaves_[leaf_index].kind == LeafKind::Legacy) {
        erase_from_legacy_leaf(leaf_index, id);
        return true;
      }
      if (full_internal_directory_ && records_[id].in_buffer &&
          erase_from_model_buffer(leaf_index, id)) {
        if (maybe_schedule_model_downgrade(leaf_index)) {
          return true;
        }
        maybe_cost_retrain_leaf(leaf_index);
        return true;
      }
      Leaf& leaf = leaves_[leaf_index];
      if (leaf.live_count > 0) {
        --leaf.live_count;
      }
      leaf.tombstone_ids.insert(id);
      ++leaf.tombstone_count;
      touch_leaf(leaf);
      if (full_internal_directory_ &&
          maybe_schedule_model_downgrade(leaf_index)) {
        return true;
      }
      if (should_rebuild_for_tombstones(leaf)) {
        schedule_or_rebuild_leaf(leaf_index);
      } else if (full_internal_directory_) {
        maybe_cost_retrain_leaf(leaf_index);
      }
    }
    return true;
  }

  void range_query_mutable(double raw_qmin, double raw_qmax,
                           const Box2D& raw_query_box,
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
    HireInternalDirectory::SearchStats internal_stats;
    const std::size_t last_leaf = find_last_leaf_for_qmax(
        qmax, stats == nullptr ? nullptr : &internal_stats);
    if (stats != nullptr) {
      stats->internal_nodes_visited += internal_stats.visited_nodes;
      stats->internal_model_searches += internal_stats.model_searches;
      stats->internal_fallback_searches += internal_stats.fallback_searches;
      stats->internal_log_entries_scanned += internal_stats.log_entries_scanned;
    }
    if (last_leaf == npos()) {
      return;
    }
    if (full_internal_directory_) {
      std::size_t leaf_id = leaves_[last_leaf].node_id;
      std::size_t hops = 0;
      while (leaf_id != npos() && hops < leaves_.size()) {
        const auto leaf_it = leaf_id_to_index_.find(leaf_id);
        if (leaf_it == leaf_id_to_index_.end()) {
          break;
        }
        scan_leaf(leaves_[leaf_it->second], qmin, qmax, query_box,
                  candidate_ids, stats);
        leaf_id = leaves_[leaf_it->second].prev_leaf_id;
        ++hops;
        if (stats != nullptr && leaf_id != npos()) {
          ++stats->leaf_sibling_hops;
        }
      }
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
                      stats, false, &leaf.tombstone_ids);
      scan_unsorted_ids(leaf.buffer_ids, qmin, qmax, query_box, candidate_ids,
                        stats);
    }
  }

  std::size_t leaf_count() const {
    std::unique_lock<std::mutex> lock(writer_mutex_, std::defer_lock);
    if (rcu_enabled()) {
      lock.lock();
    }
    return leaves_.size();
  }
  std::size_t node_count() const {
    std::unique_lock<std::mutex> lock(writer_mutex_, std::defer_lock);
    if (rcu_enabled()) {
      lock.lock();
    }
    if (leaves_.empty()) {
      return 0;
    }
    return leaves_.size() +
           (full_internal_directory_ ? internal_directory_.node_count() : 1);
  }
  std::size_t height() const {
    std::unique_lock<std::mutex> lock(writer_mutex_, std::defer_lock);
    if (rcu_enabled()) {
      lock.lock();
    }
    if (leaves_.empty()) {
      return 0;
    }
    return full_internal_directory_ ? internal_directory_.height() + 1
                                    : (leaves_.size() == 1 ? 1 : 2);
  }
  std::size_t live_count() const {
    std::unique_lock<std::mutex> lock(writer_mutex_, std::defer_lock);
    if (rcu_enabled()) {
      lock.lock();
    }
    return live_count_;
  }
  std::size_t local_rebuild_count() const {
    std::unique_lock<std::mutex> lock(writer_mutex_, std::defer_lock);
    if (rcu_enabled()) {
      lock.lock();
    }
    return local_rebuild_count_;
  }

  DebugStats debug_stats() const {
    std::unique_lock<std::mutex> lock(writer_mutex_, std::defer_lock);
    if (rcu_enabled()) {
      lock.lock();
    }
    DebugStats stats;
    stats.leaf_count = leaves_.size();
    stats.local_rebuild_count = local_rebuild_count_;
    stats.directory_log_entries = directory_log_.size();
    stats.directory_rebuild_count = directory_rebuild_count_;
    stats.deleted_slot_reuse_count = deleted_slot_reuse_count_;
    stats.cost_retrain_trigger_count = cost_retrain_trigger_count_;
    stats.active_retrain_trigger_count = active_retrain_trigger_count_;
    stats.passive_retrain_trigger_count = passive_retrain_trigger_count_;
    stats.cost_retrain_rejected_count = cost_retrain_rejected_count_;
    stats.legacy_transform_count = legacy_transform_count_;
    stats.background_recalibration_count = background_recalibration_count_;
    stats.pap_snapshot_count = pap_snapshot_count_;
    stats.pap_max_levels = pap_max_levels_;
    stats.mls_install_count = mls_install_count_;
    stats.mls_update_log_entries = mls_update_log_entries_;
    stats.mls_update_replay_count = mls_update_replay_count_;
    stats.mls_final_validation_repair_count =
        mls_final_validation_repair_count_;
    stats.rcu_snapshot_publish_count = rcu_snapshot_publish_count_;
    stats.rcu_retired_snapshot_count = rcu_retired_snapshot_count_;
    stats.rcu_reclaimed_snapshot_count = rcu_reclaimed_snapshot_count_;
    stats.rcu_active_reader_count =
        rcu_active_readers_.load(std::memory_order_acquire);
    stats.background_job_count = background_job_count_;
    stats.background_job_abort_count = background_job_abort_count_;
    stats.last_pap_levels = last_pap_levels_;
    stats.last_pap_sigma = last_pap_sigma_;
    stats.last_mls_covered_leaves = last_mls_covered_leaves_;
    stats.legacy_forward_attempt_count = legacy_forward_attempt_count_;
    stats.legacy_forward_success_count = legacy_forward_success_count_;
    stats.legacy_backward_attempt_count = legacy_backward_attempt_count_;
    stats.legacy_backward_success_count = legacy_backward_success_count_;
    stats.model_downgrade_count = model_downgrade_count_;
    stats.legacy_transform_abort_count = legacy_transform_abort_count_;
    stats.legacy_coefficient_reject_count =
        legacy_coefficient_reject_count_;
    stats.last_transform_input_leaves = last_transform_input_leaves_;
    stats.last_transform_input_records = last_transform_input_records_;
    stats.inter_level_bulk_enabled =
        full_internal_directory_ &&
        internal_directory_.inter_level_bulk_enabled();
    stats.bulk_leaf_boundaries_considered =
        bulk_leaf_optimization_stats_.boundaries_considered;
    stats.bulk_leaf_boundaries_shifted =
        bulk_leaf_optimization_stats_.boundaries_shifted;
    stats.bulk_leaf_candidate_evaluations =
        bulk_leaf_optimization_stats_.candidate_evaluations;
    stats.bulk_leaf_rls_updates = bulk_leaf_optimization_stats_.rls_updates;
    stats.bulk_leaf_max_shift = bulk_leaf_optimization_stats_.max_shift;
    stats.bulk_load_ns = bulk_load_ns_;
    stats.last_recalibration_job_kind =
        static_cast<int>(last_recalibration_job_kind_);
    if (full_internal_directory_) {
      const HireInternalDirectory::DebugStats internal =
          internal_directory_.debug_stats();
      stats.internal_node_count = internal.internal_nodes;
      stats.internal_levels = internal.levels;
      stats.internal_primary_entries = internal.primary_entries;
      stats.internal_gap_slots = internal.gap_slots;
      stats.internal_log_entries = internal.log_entries;
      stats.internal_log_compactions = internal.log_compactions;
      stats.internal_boundary_updates = internal.boundary_updates;
      stats.internal_split_count = internal.internal_split_count;
      stats.internal_merge_count = internal.internal_merge_count;
      stats.internal_redistribution_count =
          internal.internal_redistribution_count;
      stats.internal_gap_insert_count = internal.gap_insert_count;
      stats.internal_log_insert_count = internal.log_insert_count;
      stats.internal_masked_delete_count = internal.masked_delete_count;
      stats.bulk_internal_optimized_levels = internal.bulk_optimized_levels;
      stats.bulk_internal_boundaries_considered =
          internal.bulk_boundaries_considered;
      stats.bulk_internal_boundaries_shifted =
          internal.bulk_boundaries_shifted;
      stats.bulk_internal_candidate_evaluations =
          internal.bulk_candidate_evaluations;
      stats.bulk_internal_rls_updates = internal.bulk_rls_updates;
      stats.bulk_internal_max_shift = internal.bulk_max_shift;
      stats.bulk_internal_build_ns = internal.bulk_build_ns;
      stats.max_internal_model_error = internal.max_model_error;
      stats.avg_internal_model_error = internal.avg_model_error;
    }
    stats.leaf_split_count = leaf_split_count_;
    stats.leaf_merge_count = leaf_merge_count_;
    stats.leaf_redistribution_count = leaf_redistribution_count_;
    stats.buffer_scan_sample_count =
        rcu_enabled()
            ? rcu_buffer_scan_samples_.load(std::memory_order_relaxed)
            : buffer_scan_cost_.samples;
    stats.model_scan_sample_count =
        rcu_enabled()
            ? rcu_model_scan_samples_.load(std::memory_order_relaxed)
            : model_scan_cost_.samples;
    stats.merge_sample_count = merge_cost_.samples;
    stats.fit_sample_count = fit_cost_.samples;
    const std::uint64_t rcu_buffer_entries =
        rcu_buffer_scan_entries_.load(std::memory_order_relaxed);
    const std::uint64_t rcu_model_entries =
        rcu_model_scan_entries_.load(std::memory_order_relaxed);
    stats.buffer_scan_ns_per_entry_ema =
        rcu_enabled() && rcu_buffer_entries > 0
            ? static_cast<double>(rcu_buffer_scan_ns_.load(
                  std::memory_order_relaxed)) /
                  static_cast<double>(rcu_buffer_entries)
            : sampled_or_initial(
                  buffer_scan_cost_,
                  config_.initial_buffer_scan_ns_per_entry);
    stats.model_scan_ns_per_entry_ema =
        rcu_enabled() && rcu_model_entries > 0
            ? static_cast<double>(rcu_model_scan_ns_.load(
                  std::memory_order_relaxed)) /
                  static_cast<double>(rcu_model_entries)
            : sampled_or_initial(model_scan_cost_,
                                 config_.initial_model_scan_ns_per_entry);
    stats.merge_ns_per_record_ema = sampled_or_initial(
        merge_cost_, config_.initial_merge_ns_per_record);
    stats.fit_ns_per_record_ema = sampled_or_initial(
        fit_cost_, config_.initial_fit_ns_per_record);
    stats.last_estimated_gain_ns = last_estimated_gain_ns_;
    stats.last_estimated_retrain_ns = last_estimated_retrain_ns_;
    stats.last_rejected_gain_ns = last_rejected_gain_ns_;
    stats.last_rejected_retrain_ns = last_rejected_retrain_ns_;
    stats.last_actual_retrain_ns = last_actual_retrain_ns_;
    stats.last_retrain_error_before = last_retrain_error_before_;
    stats.last_retrain_error_after = last_retrain_error_after_;
    stats.last_retrain_trigger_reason =
        static_cast<int>(last_retrain_trigger_reason_);
    const std::uint64_t query_window_now = steady_now_ns();
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
      stats.buffer_hash_entries += leaf.buffer_positions.size();
      const std::size_t query_window_count =
          rcu_enabled() ? snapshot_query_count(leaf.node_id, query_window_now)
                        : query_count_in_window(leaf, query_window_now);
      stats.query_window_total += query_window_count;
      stats.query_window_max =
          std::max(stats.query_window_max, query_window_count);
      stats.tombstone_records += leaf.tombstone_count;
      if (leaf.kind == LeafKind::Legacy) {
        stats.legacy_slots_used += leaf.record_ids.size();
        stats.legacy_slot_capacity += leaf.legacy_slot_capacity;
      }
      if (leaf.prev_leaf_id != npos()) {
        ++stats.sibling_link_count;
      }
      if (leaf.next_leaf_id != npos()) {
        ++stats.sibling_link_count;
      }
      if (leaf.pending_rebuild) {
        ++stats.pending_rebuild_count;
      }
      std::size_t previous_live_id = npos();
      for (std::size_t id : leaf.record_ids) {
        if (id >= records_.size() || !records_[id].alive ||
            leaf.tombstone_ids.find(id) != leaf.tombstone_ids.end()) {
          continue;
        }
        if (previous_live_id != npos() &&
            record_id_less(id, previous_live_id)) {
          if (stats.unsorted_main_leaf_count == 0) {
            stats.first_unsorted_leaf = leaf.node_id;
            stats.first_unsorted_previous_id = previous_live_id;
            stats.first_unsorted_current_id = id;
            stats.first_unsorted_previous_zmin = records_[previous_live_id].zmin;
            stats.first_unsorted_current_zmin = records_[id].zmin;
          }
          ++stats.unsorted_main_leaf_count;
          break;
        }
        previous_live_id = id;
      }
      const auto summary_contains = [&](std::size_t id) {
        if (id >= records_.size() || !records_[id].alive ||
            leaf.tombstone_ids.find(id) != leaf.tombstone_ids.end()) {
          return true;
        }
        const Record& record = records_[id];
        return leaf.min_zmin <= record.zmin && record.zmin <= leaf.max_zmin &&
               record.zmax <= leaf.max_zmax &&
               leaf.mbr.xmin <= record.box.xmin &&
               record.box.xmax <= leaf.mbr.xmax &&
               leaf.mbr.ymin <= record.box.ymin &&
               record.box.ymax <= leaf.mbr.ymax;
      };
      bool stale_summary = false;
      for (std::size_t id : leaf.record_ids) {
        stale_summary = stale_summary || !summary_contains(id);
      }
      for (std::size_t id : leaf.buffer_ids) {
        stale_summary = stale_summary || !summary_contains(id);
      }
      if (stale_summary) {
        ++stats.stale_leaf_summary_count;
      }
    }
    stats.avg_model_error =
        stats.model_leaf_count == 0
            ? 0.0
            : error_sum / static_cast<double>(stats.model_leaf_count);
    stats.buffer_swap_delete_count = buffer_swap_delete_count_;
    if (full_internal_directory_) {
      for (std::size_t i = 0; i < leaves_.size(); ++i) {
        if (i > 0 && leaves_[i - 1].min_zmin > leaves_[i].min_zmin) {
          if (stats.out_of_order_leaf_count == 0) {
            stats.first_out_of_order_leaf = i;
            stats.first_out_of_order_left_min = leaves_[i - 1].min_zmin;
            stats.first_out_of_order_right_min = leaves_[i].min_zmin;
            stats.first_out_of_order_left_max = leaves_[i - 1].max_zmin;
            stats.first_out_of_order_right_max = leaves_[i].max_zmin;
            stats.first_out_of_order_left_id = leaves_[i - 1].node_id;
            stats.first_out_of_order_right_id = leaves_[i].node_id;
            stats.first_out_of_order_left_live = leaves_[i - 1].live_count;
            stats.first_out_of_order_right_live = leaves_[i].live_count;
            stats.first_out_of_order_left_kind =
                leaves_[i - 1].kind == LeafKind::Model ? 1 : 2;
            stats.first_out_of_order_right_kind =
                leaves_[i].kind == LeafKind::Model ? 1 : 2;
          }
          ++stats.out_of_order_leaf_count;
        }
        const std::size_t expected_prev =
            i == 0 ? npos() : leaves_[i - 1].node_id;
        const std::size_t expected_next =
            i + 1 == leaves_.size() ? npos() : leaves_[i + 1].node_id;
        if (leaves_[i].prev_leaf_id != expected_prev) {
          ++stats.broken_sibling_link_count;
        }
        if (leaves_[i].next_leaf_id != expected_next) {
          ++stats.broken_sibling_link_count;
        }
      }
    }
    return stats;
  }

  DebugRecordState debug_record_state(std::size_t id) const {
    std::unique_lock<std::mutex> lock(writer_mutex_, std::defer_lock);
    if (rcu_enabled()) {
      lock.lock();
    }
    DebugRecordState state;
    if (id < records_.size()) {
      state.alive = records_[id].alive;
      state.leaf_index = records_[id].leaf_index;
    }
    for (const Leaf& leaf : leaves_) {
      for (std::size_t record_id : leaf.record_ids) {
        if (record_id == id) {
          ++state.mutable_main_occurrences;
          if (leaf.tombstone_ids.find(id) != leaf.tombstone_ids.end()) {
            ++state.mutable_tombstone_occurrences;
          }
        }
      }
      state.mutable_buffer_occurrences +=
          static_cast<std::size_t>(
              std::count(leaf.buffer_ids.begin(), leaf.buffer_ids.end(), id));
    }
    const std::shared_ptr<const ReadSnapshot> snapshot =
        std::atomic_load_explicit(&read_snapshot_, std::memory_order_acquire);
    if (snapshot) {
      for (const std::shared_ptr<const ReadLeaf>& leaf : snapshot->leaves) {
        state.snapshot_main_occurrences +=
            static_cast<std::size_t>(std::count_if(
                leaf->records.begin(), leaf->records.end(),
                [&](const ReadRecord& record) { return record.id == id; }));
        state.snapshot_buffer_occurrences +=
            static_cast<std::size_t>(std::count_if(
                leaf->buffer.begin(), leaf->buffer.end(),
                [&](const ReadRecord& record) { return record.id == id; }));
      }
    }
    return state;
  }

  std::size_t estimate_bytes() const {
    std::unique_lock<std::mutex> lock(writer_mutex_, std::defer_lock);
    if (rcu_enabled()) {
      lock.lock();
    }
    std::size_t bytes = sizeof(*this);
    bytes += records_.size() * sizeof(Record);
    bytes += leaves_.size() * sizeof(Leaf);
    bytes += directory_min_zmin_.size() * sizeof(double);
    bytes += directory_max_zmin_.size() * sizeof(double);
    bytes += directory_log_.size() * sizeof(DirectoryLogEntry);
    if (full_internal_directory_) {
      bytes += internal_directory_.estimate_bytes();
      bytes += leaf_id_to_index_.size() *
               sizeof(std::pair<std::size_t, std::size_t>);
    }
    for (const Leaf& leaf : leaves_) {
      bytes += leaf.record_ids.capacity() * sizeof(std::size_t);
      bytes += leaf.buffer_ids.capacity() * sizeof(std::size_t);
      bytes += leaf.buffer_positions.size() *
               sizeof(std::pair<std::size_t, std::size_t>);
      bytes += leaf.tombstone_ids.size() * sizeof(std::size_t);
      bytes +=
          leaf.query_window.capacity() * sizeof(QueryWindowBucket);
    }
    if (rcu_enabled()) {
      const std::shared_ptr<const ReadSnapshot> snapshot =
          std::atomic_load_explicit(&read_snapshot_,
                                    std::memory_order_acquire);
      if (snapshot) {
        bytes += sizeof(ReadSnapshot);
        bytes += snapshot->leaves.capacity() *
                 sizeof(std::shared_ptr<const ReadLeaf>);
        bytes += snapshot->min_zmin.capacity() * sizeof(double);
        bytes += snapshot->internal_directory.estimate_bytes();
        bytes += snapshot->leaf_id_to_index.size() *
                 sizeof(std::pair<std::size_t, std::size_t>);
      }
      for (const auto& cached : read_leaf_cache_) {
        if (!cached.second.leaf) {
          continue;
        }
        bytes += sizeof(ReadLeaf);
        bytes += cached.second.leaf->records.capacity() * sizeof(ReadRecord);
        bytes += cached.second.leaf->buffer.capacity() * sizeof(ReadRecord);
        if (cached.second.leaf->runtime) {
          bytes += sizeof(LeafRuntime);
          bytes += cached.second.leaf->runtime->bucket_count *
                   sizeof(AtomicQueryBucket);
        }
      }
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
    std::size_t query_window_us = 1000000;
    std::size_t query_window_buckets = 8;
    std::size_t cost_sample_every = 64;
    std::size_t background_test_delay_us = 0;
    std::size_t directory_log_limit = 256;
    std::size_t legacy_transform_max_leaves = 4;
    std::size_t legacy_backward_min_leaves = 2;
    double tombstone_rebuild_ratio = 0.25;
    double legacy_min_fill_fraction = 0.40;
    double cost_ema_alpha = 0.20;
    double initial_buffer_scan_ns_per_entry = 3.0;
    double initial_model_scan_ns_per_entry = 3.0;
    double initial_merge_ns_per_record = 10.0;
    double initial_fit_ns_per_record = 20.0;
    double legacy_slope_tolerance = 0.25;
    double legacy_intercept_tolerance = 64.0;
    bool enable_zmax_skip = true;
    bool enable_mbr_skip = true;
    bool force_legacy = false;
    bool enable_directory_log = true;
    bool enable_deleted_slot_reuse = true;
    bool enable_cost_retrain = true;
    bool enable_legacy_transform = true;
    bool enable_background_recalibration = false;
    bool enable_rcu_recalibration = false;

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
      config.query_window_us = std::max<std::size_t>(
          1, env_size("HIRE_SFC_QUERY_WINDOW_US", config.query_window_us));
      config.query_window_buckets = std::max<std::size_t>(
          1, env_size("HIRE_SFC_QUERY_WINDOW_BUCKETS",
                      config.query_window_buckets));
      config.cost_sample_every = std::max<std::size_t>(
          1, env_size("HIRE_SFC_COST_SAMPLE_EVERY",
                      config.cost_sample_every));
      config.background_test_delay_us = env_size(
          "HIRE_SFC_BACKGROUND_TEST_DELAY_US",
          config.background_test_delay_us);
      config.directory_log_limit =
          env_size("HIRE_SFC_DIRECTORY_LOG_LIMIT",
                   config.directory_log_limit);
      config.legacy_transform_max_leaves = std::max<std::size_t>(
          1, env_size("HIRE_SFC_LEGACY_TRANSFORM_MAX_LEAVES",
                      config.legacy_transform_max_leaves));
      config.legacy_backward_min_leaves = std::max<std::size_t>(
          2, env_size("HIRE_SFC_LEGACY_BACKWARD_MIN_LEAVES",
                      config.legacy_backward_min_leaves));
      config.tombstone_rebuild_ratio =
          env_double("HIRE_SFC_TOMBSTONE_REBUILD_RATIO",
                     config.tombstone_rebuild_ratio);
      config.legacy_min_fill_fraction = std::min(
          0.49,
          std::max(0.10, env_double("HIRE_SFC_LEGACY_MIN_FILL",
                                    config.legacy_min_fill_fraction)));
      config.cost_ema_alpha = std::min(
          1.0, std::max(0.001, env_double("HIRE_SFC_COST_EMA_ALPHA",
                                          config.cost_ema_alpha)));
      config.initial_buffer_scan_ns_per_entry = std::max(
          0.0, env_double("HIRE_SFC_COST_BUFFER_NS_PER_ENTRY",
                          config.initial_buffer_scan_ns_per_entry));
      config.initial_model_scan_ns_per_entry = std::max(
          0.0, env_double("HIRE_SFC_COST_MODEL_NS_PER_ENTRY",
                          config.initial_model_scan_ns_per_entry));
      config.initial_merge_ns_per_record = std::max(
          0.0, env_double("HIRE_SFC_COST_MERGE_NS_PER_RECORD",
                          config.initial_merge_ns_per_record));
      config.initial_fit_ns_per_record = std::max(
          0.0, env_double("HIRE_SFC_COST_FIT_NS_PER_RECORD",
                          config.initial_fit_ns_per_record));
      config.legacy_slope_tolerance = std::max(
          0.0, env_double("HIRE_SFC_LEGACY_SLOPE_TOLERANCE",
                          config.legacy_slope_tolerance));
      config.legacy_intercept_tolerance = std::max(
          0.0, env_double("HIRE_SFC_LEGACY_INTERCEPT_TOLERANCE",
                          config.legacy_intercept_tolerance));
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
      config.enable_rcu_recalibration =
          env_bool("HIRE_SFC_ENABLE_RCU_RECALIBRATION",
                   config.enable_rcu_recalibration);
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

  enum class TriggerReason { None = 0, Active = 1, Passive = 2 };
  enum class UpdateKind : int { Insert = 1, Erase = 2 };
  enum class RecalibrationKind : int {
    ModelRetrain = 1,
    ForwardMerge = 2,
    BackwardMerge = 3,
    ModelDowngrade = 4
  };

  struct QueryWindowBucket {
    std::uint64_t epoch = std::numeric_limits<std::uint64_t>::max();
    std::size_t count = 0;
  };

  struct CostEma {
    double value = 0.0;
    std::size_t samples = 0;
  };

  struct Record {
    double zmin = 0.0;
    double zmax = 0.0;
    Box2D box;
    bool alive = false;
    bool in_buffer = false;
    std::size_t leaf_index = npos();
  };

  struct AtomicQueryBucket {
    std::atomic<std::uint64_t> epoch;
    std::atomic<std::size_t> count;

    AtomicQueryBucket()
        : epoch(std::numeric_limits<std::uint64_t>::max()), count(0) {}
  };

  struct LeafRuntime {
    explicit LeafRuntime(std::size_t bucket_count)
        : bucket_count(bucket_count),
          buckets(new AtomicQueryBucket[bucket_count]) {}

    std::size_t bucket_count = 0;
    std::unique_ptr<AtomicQueryBucket[]> buckets;
    std::atomic<std::size_t> cost_sample_sequence{0};
  };

  struct ReadRecord {
    std::size_t id = 0;
    double zmin = 0.0;
    double zmax = 0.0;
    Box2D box;
  };

  struct ReadLeaf {
    std::size_t node_id = npos();
    std::uint64_t version = 0;
    LeafKind kind = LeafKind::Legacy;
    double min_zmin = std::numeric_limits<double>::infinity();
    double max_zmin = -std::numeric_limits<double>::infinity();
    double max_zmax = -std::numeric_limits<double>::infinity();
    Box2D mbr;
    std::vector<ReadRecord> records;
    std::vector<ReadRecord> buffer;
    std::shared_ptr<LeafRuntime> runtime;
  };

  struct ReadSnapshot {
    std::uint64_t generation = 0;
    HireInternalDirectory internal_directory;
    std::unordered_map<std::size_t, std::size_t> leaf_id_to_index;
    std::vector<std::shared_ptr<const ReadLeaf>> leaves;
    std::vector<double> min_zmin;
  };

  struct ReadLeafCacheEntry {
    std::uint64_t version = 0;
    std::shared_ptr<const ReadLeaf> leaf;
  };

  struct MlsUpdate {
    std::uint64_t sequence = 0;
    UpdateKind kind = UpdateKind::Insert;
    RecordInput record;
    std::size_t leaf_node_id = npos();
  };

  struct RecalibrationJob {
    RecalibrationKind kind = RecalibrationKind::ModelRetrain;
    std::size_t target_leaf_id = npos();
    std::size_t mls_root_id = npos();
    std::size_t sigma = 0;
    std::vector<HireInternalDirectory::PathNodeSnapshot> pap;
    std::unordered_set<std::size_t> covered_leaf_ids;
    std::vector<std::size_t> replaced_leaf_ids;
    std::vector<RecordInput> records;
    std::vector<MlsUpdate> updates;
    std::uint64_t replayed_sequence = 0;
  };

  struct RegressionFit {
    double slope = 0.0;
    double intercept = 0.0;
    double max_error = 0.0;
  };

  struct Leaf {
    std::size_t node_id = npos();
    LeafKind kind = LeafKind::Legacy;
    std::vector<std::size_t> record_ids;
    std::vector<std::size_t> buffer_ids;
    std::unordered_map<std::size_t, std::size_t> buffer_positions;
    std::unordered_set<std::size_t> tombstone_ids;
    std::size_t legacy_slot_capacity = 0;
    std::size_t prev_leaf_id = npos();
    std::size_t next_leaf_id = npos();
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
    mutable std::vector<QueryWindowBucket> query_window;
    mutable std::size_t cost_sample_sequence = 0;
    std::uint64_t read_version = 0;
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

  bool rcu_enabled() const {
    return full_internal_directory_ &&
           config_.enable_background_recalibration &&
           config_.enable_rcu_recalibration;
  }

  void touch_leaf(Leaf& leaf) { leaf.read_version = next_read_version_++; }

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
    std::sort(ids.begin(), ids.end(),
              [&](std::size_t lhs, std::size_t rhs) {
                return record_id_less(lhs, rhs);
              });
  }

  bool record_id_less(std::size_t lhs, std::size_t rhs) const {
    const Record& a = records_[lhs];
    const Record& b = records_[rhs];
    if (a.zmin != b.zmin) {
      return a.zmin < b.zmin;
    }
    if (a.zmax != b.zmax) {
      return a.zmax < b.zmax;
    }
    return lhs < rhs;
  }

  void assign_leaf_node_id(Leaf& leaf) {
    if (full_internal_directory_ && leaf.node_id == npos()) {
      leaf.node_id = next_leaf_node_id_++;
    }
  }

  Leaf make_leaf_from_range(const std::vector<std::size_t>& ids,
                            std::size_t begin, std::size_t end) {
    Leaf leaf;
    assign_leaf_node_id(leaf);
    leaf.record_ids.assign(ids.begin() + begin, ids.begin() + end);
    rebuild_leaf_metadata(leaf);
    return leaf;
  }

  bool bulk_range_fits_model(const std::vector<std::size_t>& ids,
                             std::size_t begin,
                             std::size_t end) const {
    const std::size_t count = end - begin;
    if (config_.force_legacy || count < config_.min_model_leaf ||
        count > config_.model_leaf_size || count < 2) {
      return false;
    }
    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_xx = 0.0;
    double sum_xy = 0.0;
    for (std::size_t offset = 0; offset < count; ++offset) {
      const double x = records_[ids[begin + offset]].zmin;
      const double y = static_cast<double>(offset);
      sum_x += x;
      sum_y += y;
      sum_xx += x * x;
      sum_xy += x * y;
    }
    const double n = static_cast<double>(count);
    const double denominator = n * sum_xx - sum_x * sum_x;
    double slope = 0.0;
    double intercept = sum_y / n;
    if (std::fabs(denominator) > 1e-12) {
      slope = (n * sum_xy - sum_x * sum_y) / denominator;
      intercept = (sum_y - slope * sum_x) / n;
    }
    double max_error = 0.0;
    for (std::size_t offset = 0; offset < count; ++offset) {
      const double predicted =
          slope * records_[ids[begin + offset]].zmin + intercept;
      max_error = std::max(
          max_error, std::fabs(predicted - static_cast<double>(offset)));
    }
    return max_error <= config_.epsilon;
  }

  bool valid_bulk_leaf_range(const std::vector<std::size_t>& ids,
                             std::size_t begin,
                             std::size_t end) const {
    if (end <= begin) {
      return false;
    }
    const std::size_t count = end - begin;
    return count <= config_.leaf_size ||
           bulk_range_fits_model(ids, begin, end);
  }

  void build_bulk_loaded_leaves(const std::vector<std::size_t>& ids) {
    leaves_.clear();
    std::vector<std::size_t> ends;
    for (std::size_t begin = 0; begin < ids.size();) {
      std::size_t chosen_end =
          std::min(ids.size(), begin + config_.leaf_size);

      if (!config_.force_legacy && ids.size() - begin >= config_.min_model_leaf) {
        const std::size_t max_end =
            std::min(ids.size(), begin + config_.model_leaf_size);
        for (std::size_t end = max_end;
             end >= begin + config_.min_model_leaf;) {
          if (bulk_range_fits_model(ids, begin, end)) {
            chosen_end = end;
            break;
          }
          if (end <= begin + config_.leaf_size) {
            break;
          }
          end -= std::min(config_.leaf_size, end - begin);
        }
      }
      ends.push_back(chosen_end);
      begin = chosen_end;
    }
    if (full_internal_directory_ &&
        internal_directory_.inter_level_bulk_enabled() && ends.size() > 1) {
      ends = bulk_loading::optimize_partition_ends(
          ids.size(), ends,
          internal_directory_.bulk_target_children_for_planning(),
          internal_directory_.bulk_slot_count_for_planning(),
          internal_directory_.bulk_seed_children_for_planning(),
          internal_directory_.bulk_delta_for_planning(),
          [&](std::size_t index) { return records_[ids[index]].zmin; },
          [&](std::size_t begin, std::size_t end) {
            return valid_bulk_leaf_range(ids, begin, end);
          },
          bulk_leaf_optimization_stats_);
    }
    std::size_t begin = 0;
    for (std::size_t end : ends) {
      leaves_.push_back(make_leaf_from_range(ids, begin, end));
      begin = end;
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
    leaf.buffer_positions.clear();
    leaf.tombstone_ids.clear();
    leaf.min_zmin = std::numeric_limits<double>::infinity();
    leaf.max_zmin = -std::numeric_limits<double>::infinity();
    leaf.max_zmax = -std::numeric_limits<double>::infinity();
    leaf.live_count = 0;
    leaf.tombstone_count = 0;
    leaf.pending_rebuild = false;
    leaf.query_count_recent = 0;
    leaf.query_window.clear();
    leaf.cost_sample_sequence = 0;
    bool have_box = false;
    for (std::size_t id : leaf.record_ids) {
      const Record& record = records_[id];
      if (!record.alive) {
        leaf.tombstone_ids.insert(id);
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
    if (full_internal_directory_ && leaf.kind == LeafKind::Legacy) {
      prepare_legacy_storage(leaf);
    } else {
      leaf.legacy_slot_capacity = 0;
    }
    touch_leaf(leaf);
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
      if (!record.alive ||
          leaf.tombstone_ids.find(leaf.record_ids[pos]) !=
              leaf.tombstone_ids.end()) {
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
      if (!record.alive ||
          leaf.tombstone_ids.find(leaf.record_ids[pos]) !=
              leaf.tombstone_ids.end()) {
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
    touch_leaf(leaf);
  }

  void prepare_legacy_storage(Leaf& leaf) {
    if (!full_internal_directory_) {
      return;
    }
    std::vector<std::size_t> compact;
    compact.reserve(config_.leaf_size);
    compact.assign(leaf.record_ids.begin(), leaf.record_ids.end());
    leaf.record_ids.swap(compact);
    leaf.legacy_slot_capacity = config_.leaf_size;
  }

  void append_model_buffer(std::size_t leaf_index, std::size_t id) {
    Leaf& leaf = leaves_[leaf_index];
    if (full_internal_directory_) {
      leaf.buffer_positions[id] = leaf.buffer_ids.size();
    }
    leaf.buffer_ids.push_back(id);
    records_[id].leaf_index = leaf_index;
    records_[id].in_buffer = true;
  }

  void refresh_leaf_bounds(Leaf& leaf) {
    leaf.min_zmin = std::numeric_limits<double>::infinity();
    leaf.max_zmin = -std::numeric_limits<double>::infinity();
    leaf.max_zmax = -std::numeric_limits<double>::infinity();
    leaf.live_count = 0;
    leaf.tombstone_count = 0;
    bool have_box = false;
    const auto include_record = [&](std::size_t id) {
      if (id >= records_.size() || !records_[id].alive ||
          leaf.tombstone_ids.find(id) != leaf.tombstone_ids.end()) {
        ++leaf.tombstone_count;
        return;
      }
      const Record& record = records_[id];
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
    };
    for (std::size_t id : leaf.record_ids) {
      include_record(id);
    }
    for (std::size_t id : leaf.buffer_ids) {
      include_record(id);
    }
    if (!have_box) {
      leaf.mbr = Box2D{};
    }
    touch_leaf(leaf);
  }

  bool erase_from_model_buffer(std::size_t leaf_index, std::size_t id) {
    Leaf& leaf = leaves_[leaf_index];
    const auto position_it = leaf.buffer_positions.find(id);
    if (position_it == leaf.buffer_positions.end()) {
      return false;
    }
    const std::size_t position = position_it->second;
    const std::size_t last_position = leaf.buffer_ids.size() - 1;
    if (position != last_position) {
      const std::size_t moved_id = leaf.buffer_ids[last_position];
      leaf.buffer_ids[position] = moved_id;
      leaf.buffer_positions[moved_id] = position;
      ++buffer_swap_delete_count_;
    }
    leaf.buffer_ids.pop_back();
    leaf.buffer_positions.erase(position_it);
    records_[id].in_buffer = false;
    if (leaf.live_count > 0) {
      --leaf.live_count;
    }
    touch_leaf(leaf);
    return true;
  }

  void rebuild_as_legacy_leaf(Leaf& leaf) {
    const std::size_t node_id = leaf.node_id;
    rebuild_leaf_metadata(leaf);
    leaf.node_id = node_id;
    leaf.kind = LeafKind::Legacy;
    prepare_legacy_storage(leaf);
  }

  void insert_into_legacy_leaf(std::size_t leaf_index, std::size_t id) {
    if (leaf_index >= leaves_.size()) {
      return;
    }
    Leaf& leaf = leaves_[leaf_index];
    if (!leaf.pending_rebuild &&
        leaf.record_ids.size() >= leaf.legacy_slot_capacity) {
      split_legacy_leaf(leaf_index, id);
      return;
    }
    auto position = std::lower_bound(
        leaf.record_ids.begin(), leaf.record_ids.end(), id,
        [&](std::size_t existing_id, std::size_t inserted_id) {
          return record_id_less(existing_id, inserted_id);
        });
    leaf.record_ids.insert(position, id);
    records_[id].leaf_index = leaf_index;
    records_[id].in_buffer = false;
    expand_leaf_on_insert(leaf, RecordInput{id, records_[id].zmin,
                                            records_[id].zmax,
                                            records_[id].box});
    note_directory_update(leaf_index);
  }

  void split_legacy_leaf(std::size_t leaf_index, std::size_t inserted_id) {
    if (leaf_index >= leaves_.size() ||
        leaves_[leaf_index].record_ids.empty()) {
      return;
    }
    std::vector<std::size_t> combined = leaves_[leaf_index].record_ids;
    combined.push_back(inserted_id);
    sort_ids(combined);
    const std::size_t split = combined.size() / 2;
    Leaf right;
    assign_leaf_node_id(right);
    leaves_[leaf_index].record_ids.assign(
        combined.begin(), combined.begin() + static_cast<long>(split));
    right.record_ids.assign(combined.begin() + static_cast<long>(split),
                            combined.end());
    rebuild_as_legacy_leaf(leaves_[leaf_index]);
    rebuild_as_legacy_leaf(right);

    const std::size_t left_id = leaves_[leaf_index].node_id;
    const double left_max = leaves_[leaf_index].max_zmin;
    const std::size_t right_id = right.node_id;
    const double right_max = right.max_zmin;
    leaves_.insert(leaves_.begin() + static_cast<long>(leaf_index + 1),
                   std::move(right));
    ++leaf_split_count_;
    rebuild_record_leaf_refs();
    if (directory_ready_) {
      internal_directory_.insert_leaf_after(left_id, right_id, left_max,
                                            right_max);
    }
  }

  void erase_from_legacy_leaf(std::size_t leaf_index, std::size_t id) {
    if (leaf_index >= leaves_.size()) {
      return;
    }
    Leaf& leaf = leaves_[leaf_index];
    const auto position =
        std::find(leaf.record_ids.begin(), leaf.record_ids.end(), id);
    if (position == leaf.record_ids.end()) {
      return;
    }
    leaf.record_ids.erase(position);
    records_[id].in_buffer = false;
    refresh_leaf_bounds(leaf);
    note_directory_update(leaf_index);
    const std::size_t minimum = std::max<std::size_t>(
        1, static_cast<std::size_t>(
               std::floor(config_.leaf_size *
                          config_.legacy_min_fill_fraction)));
    if (!leaf.pending_rebuild && leaf.record_ids.size() < minimum) {
      rebalance_legacy_leaf(leaf_index);
    }
  }

  void rebalance_legacy_leaf(std::size_t leaf_index) {
    if (leaf_index >= leaves_.size() || leaves_.size() < 2 ||
        leaves_[leaf_index].kind != LeafKind::Legacy ||
        leaves_[leaf_index].pending_rebuild) {
      return;
    }
    std::size_t sibling_index = npos();
    if (leaf_index > 0 && leaves_[leaf_index - 1].kind == LeafKind::Legacy &&
        !leaves_[leaf_index - 1].pending_rebuild) {
      sibling_index = leaf_index - 1;
    } else if (leaf_index + 1 < leaves_.size() &&
               leaves_[leaf_index + 1].kind == LeafKind::Legacy &&
               !leaves_[leaf_index + 1].pending_rebuild) {
      sibling_index = leaf_index + 1;
    }
    if (sibling_index == npos()) {
      return;
    }
    const std::size_t left_index = std::min(leaf_index, sibling_index);
    const std::size_t right_index = std::max(leaf_index, sibling_index);
    std::vector<std::size_t> combined = leaves_[left_index].record_ids;
    combined.insert(combined.end(), leaves_[right_index].record_ids.begin(),
                    leaves_[right_index].record_ids.end());
    sort_ids(combined);

    const std::size_t left_id = leaves_[left_index].node_id;
    const std::size_t right_id = leaves_[right_index].node_id;
    if (combined.size() <= config_.leaf_size) {
      leaves_[left_index].record_ids = std::move(combined);
      rebuild_as_legacy_leaf(leaves_[left_index]);
      const double left_max = leaves_[left_index].max_zmin;
      leaves_.erase(leaves_.begin() + static_cast<long>(right_index));
      ++leaf_merge_count_;
      rebuild_record_leaf_refs();
      if (directory_ready_) {
        internal_directory_.update_leaf_boundary(left_id, left_max);
        internal_directory_.remove_leaf(right_id);
      }
      return;
    }

    const std::size_t split = combined.size() / 2;
    leaves_[left_index].record_ids.assign(
        combined.begin(), combined.begin() + static_cast<long>(split));
    leaves_[right_index].record_ids.assign(
        combined.begin() + static_cast<long>(split), combined.end());
    rebuild_as_legacy_leaf(leaves_[left_index]);
    rebuild_as_legacy_leaf(leaves_[right_index]);
    ++leaf_redistribution_count_;
    rebuild_record_leaf_refs();
    if (directory_ready_) {
      internal_directory_.update_leaf_boundary(
          left_id, leaves_[left_index].max_zmin);
      internal_directory_.update_leaf_boundary(
          right_id, leaves_[right_index].max_zmin);
    }
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
      if (rcu_enabled()) {
        queue_background_rebuild_locked(leaf_index);
        return;
      }
      leaves_[leaf_index].pending_rebuild = true;
      ++background_recalibration_count_;
      return;
    }
    rebuild_leaf_range(leaf_index);
  }

  void queue_background_rebuild_locked(std::size_t leaf_index) {
    queue_recalibration_job_locked(RecalibrationKind::ModelRetrain,
                                   std::vector<std::size_t>{leaf_index});
  }

  bool queue_recalibration_job_locked(
      RecalibrationKind kind, const std::vector<std::size_t>& leaf_indices) {
    if (leaf_indices.empty()) {
      return false;
    }
    for (std::size_t index : leaf_indices) {
      if (index >= leaves_.size() || leaves_[index].pending_rebuild) {
        return false;
      }
    }
    std::shared_ptr<RecalibrationJob> job =
        std::make_shared<RecalibrationJob>();
    job->kind = kind;
    job->target_leaf_id = leaves_[leaf_indices.front()].node_id;
    std::size_t total_records = 0;
    for (std::size_t index : leaf_indices) {
      Leaf& leaf = leaves_[index];
      leaf.pending_rebuild = true;
      touch_leaf(leaf);
      job->replaced_leaf_ids.push_back(leaf.node_id);
      total_records += leaf.live_count;
    }
    const std::size_t resulting_leaves =
        (total_records + config_.leaf_size - 1) / config_.leaf_size;
    job->sigma = resulting_leaves == 0 ? 0 : resulting_leaves - 1;
    const HireInternalDirectory::PotentiallyAffectedPath pap =
        internal_directory_.snapshot_potentially_affected_path(
            job->target_leaf_id, job->sigma);
    job->mls_root_id = pap.mls_root_id;
    job->pap = pap.leaf_to_root;
    job->covered_leaf_ids.insert(pap.covered_leaf_ids.begin(),
                                 pap.covered_leaf_ids.end());
    if (job->covered_leaf_ids.empty()) {
      job->covered_leaf_ids.insert(job->target_leaf_id);
    }

    const auto capture = [&](std::size_t id) {
      if (id >= records_.size() || !records_[id].alive) {
        return;
      }
      const Record& record = records_[id];
      job->records.push_back(
          RecordInput{id, record.zmin, record.zmax, record.box});
    };
    for (std::size_t index : leaf_indices) {
      for (std::size_t id : leaves_[index].record_ids) {
        if (leaves_[index].tombstone_ids.find(id) ==
            leaves_[index].tombstone_ids.end()) {
          capture(id);
        }
      }
      for (std::size_t id : leaves_[index].buffer_ids) {
        capture(id);
      }
    }

    active_recalibrations_[job->target_leaf_id] = job;
    ++pap_snapshot_count_;
    pap_max_levels_ = std::max(pap_max_levels_, job->pap.size());
    last_pap_levels_ = job->pap.size();
    last_pap_sigma_ = job->sigma;
    last_mls_covered_leaves_ = job->covered_leaf_ids.size();
    last_transform_input_leaves_ = leaf_indices.size();
    last_transform_input_records_ = total_records;
    last_recalibration_job_kind_ = kind;
    ++background_recalibration_count_;
    ++background_job_count_;
    {
      std::lock_guard<std::mutex> job_lock(job_mutex_);
      job_queue_.push_back(job);
    }
    job_cv_.notify_one();
    return true;
  }

  void append_mls_update_locked(UpdateKind kind, const RecordInput& input,
                                std::size_t id) {
    if (!rcu_enabled() || id >= records_.size()) {
      return;
    }
    const std::size_t leaf_index = records_[id].leaf_index;
    if (leaf_index >= leaves_.size()) {
      return;
    }
    const std::size_t leaf_id = leaves_[leaf_index].node_id;
    for (auto& active : active_recalibrations_) {
      RecalibrationJob& job = *active.second;
      if (job.covered_leaf_ids.find(leaf_id) ==
          job.covered_leaf_ids.end()) {
        continue;
      }
      job.updates.push_back(
          MlsUpdate{next_update_sequence_++, kind, input, leaf_id});
      ++mls_update_log_entries_;
    }
  }

  void start_background_worker() {
    if (!rcu_enabled() || worker_thread_.joinable()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(job_mutex_);
      stop_worker_ = false;
      worker_busy_ = false;
    }
    worker_thread_ = std::thread([this]() { background_worker_loop(); });
  }

  void stop_background_worker() {
    if (!worker_thread_.joinable()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(job_mutex_);
      stop_worker_ = true;
    }
    job_cv_.notify_all();
    worker_thread_.join();
    {
      std::lock_guard<std::mutex> lock(job_mutex_);
      job_queue_.clear();
      worker_busy_ = false;
    }
  }

  static bool record_input_less(const RecordInput& lhs,
                                const RecordInput& rhs) {
    if (lhs.zmin != rhs.zmin) {
      return lhs.zmin < rhs.zmin;
    }
    if (lhs.zmax != rhs.zmax) {
      return lhs.zmax < rhs.zmax;
    }
    return lhs.id < rhs.id;
  }

  void replay_job_updates(
      RecalibrationJob& job, const std::vector<MlsUpdate>& updates,
      std::unordered_map<std::size_t, RecordInput>& records_by_id) {
    for (const MlsUpdate& update : updates) {
      if (std::find(job.replaced_leaf_ids.begin(),
                    job.replaced_leaf_ids.end(),
                    update.leaf_node_id) == job.replaced_leaf_ids.end()) {
        continue;
      }
      if (update.kind == UpdateKind::Insert) {
        records_by_id[update.record.id] = update.record;
      } else {
        records_by_id.erase(update.record.id);
      }
    }
  }

  static RegressionFit fit_record_inputs(
      const std::vector<RecordInput>& records, std::size_t begin,
      std::size_t end) {
    RegressionFit fit;
    if (end <= begin) {
      return fit;
    }
    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_xx = 0.0;
    double sum_xy = 0.0;
    const std::size_t count = end - begin;
    for (std::size_t offset = 0; offset < count; ++offset) {
      const double x = records[begin + offset].zmin;
      const double y = static_cast<double>(offset);
      sum_x += x;
      sum_y += y;
      sum_xx += x * x;
      sum_xy += x * y;
    }
    const double n = static_cast<double>(count);
    const double denominator = n * sum_xx - sum_x * sum_x;
    if (std::fabs(denominator) > 1e-12) {
      fit.slope = (n * sum_xy - sum_x * sum_y) / denominator;
      fit.intercept = (sum_y - fit.slope * sum_x) / n;
    } else {
      fit.intercept = sum_y / n;
    }
    for (std::size_t offset = 0; offset < count; ++offset) {
      const double predicted =
          fit.slope * records[begin + offset].zmin + fit.intercept;
      fit.max_error =
          std::max(fit.max_error,
                   std::fabs(predicted - static_cast<double>(offset)));
    }
    return fit;
  }

  bool prepare_job_segments(const RecalibrationJob& job,
                            const std::vector<RecordInput>& sorted_records,
                            std::vector<std::size_t>& segment_sizes) const {
    segment_sizes.clear();
    if (job.kind == RecalibrationKind::ModelDowngrade ||
        job.kind == RecalibrationKind::ModelRetrain) {
      for (std::size_t begin = 0; begin < sorted_records.size();
           begin += config_.leaf_size) {
        segment_sizes.push_back(
            std::min(config_.leaf_size, sorted_records.size() - begin));
      }
      return true;
    }
    if (sorted_records.size() < config_.min_model_leaf) {
      return false;
    }
    if (job.kind == RecalibrationKind::ForwardMerge) {
      if (sorted_records.size() > config_.model_leaf_size) {
        return false;
      }
      const RegressionFit fit =
          fit_record_inputs(sorted_records, 0, sorted_records.size());
      if (fit.max_error > config_.epsilon) {
        return false;
      }
      segment_sizes.push_back(sorted_records.size());
      return true;
    }

    std::size_t begin = 0;
    while (begin < sorted_records.size()) {
      const std::size_t maximum_end =
          std::min(sorted_records.size(), begin + config_.model_leaf_size);
      std::size_t chosen_end = begin;
      for (std::size_t end = maximum_end;
           end >= begin + config_.min_model_leaf; --end) {
        const RegressionFit fit = fit_record_inputs(sorted_records, begin, end);
        if (fit.max_error <= config_.epsilon) {
          chosen_end = end;
          break;
        }
      }
      if (chosen_end == begin) {
        return false;
      }
      segment_sizes.push_back(chosen_end - begin);
      begin = chosen_end;
    }
    return true;
  }

  void clear_job_pending_locked(const RecalibrationJob& job) {
    for (std::size_t leaf_id : job.replaced_leaf_ids) {
      const auto found = leaf_id_to_index_.find(leaf_id);
      if (found != leaf_id_to_index_.end()) {
        leaves_[found->second].pending_rebuild = false;
        touch_leaf(leaves_[found->second]);
      }
    }
  }

  void prune_empty_leaves_locked() {
    bool removed = false;
    for (std::size_t index = 0; index < leaves_.size() && leaves_.size() > 1;) {
      if (leaves_[index].live_count != 0 || leaves_[index].pending_rebuild) {
        ++index;
        continue;
      }
      const std::size_t leaf_id = leaves_[index].node_id;
      leaves_.erase(leaves_.begin() + static_cast<long>(index));
      if (directory_ready_) {
        internal_directory_.remove_leaf(leaf_id);
      }
      ++leaf_merge_count_;
      removed = true;
    }
    if (removed) {
      rebuild_record_leaf_refs();
    }
  }

  bool install_retrained_leaf_locked(
      const RecalibrationJob& job,
      const std::vector<RecordInput>& sorted_records,
      const std::vector<std::size_t>& segment_sizes) {
    const auto target_it = leaf_id_to_index_.find(job.target_leaf_id);
    if (target_it == leaf_id_to_index_.end()) {
      return false;
    }
    const std::size_t leaf_index = target_it->second;
    if (leaf_index + job.replaced_leaf_ids.size() > leaves_.size()) {
      return false;
    }
    for (std::size_t offset = 0; offset < job.replaced_leaf_ids.size();
         ++offset) {
      if (leaves_[leaf_index + offset].node_id !=
          job.replaced_leaf_ids[offset]) {
        return false;
      }
    }
    last_retrain_error_before_ = leaves_[leaf_index].max_model_error;
    std::vector<RecordInput> authoritative_records;
    std::unordered_set<std::size_t> captured_ids;
    const auto capture_current = [&](std::size_t id) {
      if (id >= records_.size() || !records_[id].alive ||
          !captured_ids.insert(id).second) {
        return;
      }
      const Record& record = records_[id];
      authoritative_records.push_back(
          RecordInput{id, record.zmin, record.zmax, record.box});
    };
    for (std::size_t offset = 0; offset < job.replaced_leaf_ids.size();
         ++offset) {
      const Leaf& current = leaves_[leaf_index + offset];
      for (std::size_t id : current.record_ids) {
        if (current.tombstone_ids.find(id) ==
            current.tombstone_ids.end()) {
          capture_current(id);
        }
      }
      for (std::size_t id : current.buffer_ids) {
        capture_current(id);
      }
    }
    std::sort(authoritative_records.begin(), authoritative_records.end(),
              record_input_less);

    const auto same_record = [](const RecordInput& lhs,
                                const RecordInput& rhs) {
      return lhs.id == rhs.id && lhs.zmin == rhs.zmin &&
             lhs.zmax == rhs.zmax && lhs.box.xmin == rhs.box.xmin &&
             lhs.box.ymin == rhs.box.ymin &&
             lhs.box.xmax == rhs.box.xmax &&
             lhs.box.ymax == rhs.box.ymax;
    };
    const bool final_state_matches =
        sorted_records.size() == authoritative_records.size() &&
        std::equal(sorted_records.begin(), sorted_records.end(),
                   authoritative_records.begin(), same_record);
    std::vector<std::size_t> effective_segment_sizes = segment_sizes;
    if (!final_state_matches) {
      ++mls_final_validation_repair_count_;
      if (!prepare_job_segments(job, authoritative_records,
                                effective_segment_sizes)) {
        return false;
      }
    }

    std::vector<std::size_t> live_ids;
    live_ids.reserve(authoritative_records.size());
    for (const RecordInput& input : authoritative_records) {
      live_ids.push_back(input.id);
    }
    std::size_t segmented_records = 0;
    for (std::size_t size : effective_segment_sizes) {
      segmented_records += size;
    }
    if (segmented_records != live_ids.size()) {
      return false;
    }

    std::vector<Leaf> replacement;
    std::size_t begin = 0;
    for (std::size_t segment_size : effective_segment_sizes) {
      const std::size_t end = begin + segment_size;
      Leaf leaf;
      if (replacement.empty()) {
        leaf.node_id = job.target_leaf_id;
      } else {
        assign_leaf_node_id(leaf);
      }
      leaf.record_ids.assign(live_ids.begin() + static_cast<long>(begin),
                             live_ids.begin() + static_cast<long>(end));
      rebuild_leaf_metadata(leaf);
      if (job.kind == RecalibrationKind::ForwardMerge ||
          job.kind == RecalibrationKind::BackwardMerge) {
        if (leaf.kind != LeafKind::Model) {
          return false;
        }
      } else if (job.kind == RecalibrationKind::ModelDowngrade) {
        leaf.kind = LeafKind::Legacy;
        prepare_legacy_storage(leaf);
        touch_leaf(leaf);
      }
      replacement.push_back(std::move(leaf));
      begin = end;
    }

    leaves_.erase(
        leaves_.begin() + static_cast<long>(leaf_index),
        leaves_.begin() +
            static_cast<long>(leaf_index + job.replaced_leaf_ids.size()));
    leaves_.insert(leaves_.begin() + static_cast<long>(leaf_index),
                   std::make_move_iterator(replacement.begin()),
                   std::make_move_iterator(replacement.end()));
    ++local_rebuild_count_;
    ++mls_install_count_;
    if (job.kind == RecalibrationKind::ForwardMerge) {
      ++legacy_forward_success_count_;
      ++legacy_transform_count_;
    } else if (job.kind == RecalibrationKind::BackwardMerge) {
      ++legacy_backward_success_count_;
      ++legacy_transform_count_;
    } else if (job.kind == RecalibrationKind::ModelDowngrade) {
      ++model_downgrade_count_;
    }
    last_retrain_error_after_ = 0.0;
    for (std::size_t offset = 0; offset < replacement.size(); ++offset) {
      const Leaf& leaf = leaves_[leaf_index + offset];
      last_retrain_error_after_ =
          std::max(last_retrain_error_after_, leaf.max_model_error);
    }
    rebuild_record_leaf_refs();
    if (directory_ready_) {
      if (replacement.empty()) {
        for (std::size_t removed_id : job.replaced_leaf_ids) {
          internal_directory_.remove_leaf(removed_id);
        }
      } else {
        internal_directory_.update_leaf_boundary(
            job.target_leaf_id, leaves_[leaf_index].max_zmin);
        for (std::size_t offset = 1;
             offset < job.replaced_leaf_ids.size(); ++offset) {
          internal_directory_.remove_leaf(job.replaced_leaf_ids[offset]);
        }
        std::size_t previous_id = job.target_leaf_id;
        for (std::size_t offset = 1; offset < replacement.size(); ++offset) {
          const Leaf& previous = leaves_[leaf_index + offset - 1];
          const Leaf& current = leaves_[leaf_index + offset];
          internal_directory_.insert_leaf_after(
              previous_id, current.node_id, previous.max_zmin,
              current.max_zmin);
          previous_id = current.node_id;
        }
      }
    }
    return true;
  }

  void process_recalibration_job(
      const std::shared_ptr<RecalibrationJob>& job) {
    if (config_.background_test_delay_us > 0) {
      std::this_thread::sleep_for(
          std::chrono::microseconds(config_.background_test_delay_us));
    }
    std::unordered_map<std::size_t, RecordInput> records_by_id;
    records_by_id.reserve(job->records.size() * 2 + 1);
    for (const RecordInput& input : job->records) {
      records_by_id[input.id] = input;
    }

    for (;;) {
      std::vector<MlsUpdate> updates;
      {
        std::unique_lock<std::mutex> writer_lock(writer_mutex_);
        const auto active = active_recalibrations_.find(job->target_leaf_id);
        if (active == active_recalibrations_.end() ||
            active->second.get() != job.get()) {
          ++background_job_abort_count_;
          return;
        }
        for (const MlsUpdate& update : job->updates) {
          if (update.sequence > job->replayed_sequence) {
            updates.push_back(update);
          }
        }
        if (!updates.empty()) {
          job->replayed_sequence = updates.back().sequence;
          mls_update_replay_count_ += updates.size();
        }
      }
      replay_job_updates(*job, updates, records_by_id);

      const std::uint64_t merge_start = steady_now_ns();
      std::vector<RecordInput> sorted_records;
      sorted_records.reserve(records_by_id.size());
      for (const auto& record : records_by_id) {
        sorted_records.push_back(record.second);
      }
      std::sort(sorted_records.begin(), sorted_records.end(),
                record_input_less);
      std::vector<std::size_t> segment_sizes;
      const bool segments_ready =
          prepare_job_segments(*job, sorted_records, segment_sizes);
      const std::uint64_t merge_end = steady_now_ns();

      std::unique_lock<std::mutex> writer_lock(writer_mutex_);
      const auto active = active_recalibrations_.find(job->target_leaf_id);
      if (active == active_recalibrations_.end() ||
          active->second.get() != job.get()) {
        ++background_job_abort_count_;
        return;
      }
      const bool caught_up =
          job->updates.empty() ||
          job->updates.back().sequence <= job->replayed_sequence;
      if (!caught_up) {
        writer_lock.unlock();
        continue;
      }
      if (!segments_ready) {
        clear_job_pending_locked(*job);
        if (job->kind == RecalibrationKind::ModelRetrain) {
          ++background_job_abort_count_;
        } else {
          ++legacy_transform_abort_count_;
        }
        active_recalibrations_.erase(active);
        prune_empty_leaves_locked();
        publish_read_snapshot_locked();
        return;
      }
      const std::uint64_t fit_start = steady_now_ns();
      const bool installed =
          install_retrained_leaf_locked(*job, sorted_records, segment_sizes);
      const std::uint64_t fit_end = steady_now_ns();
      if (!installed) {
        clear_job_pending_locked(*job);
        if (job->kind == RecalibrationKind::ModelRetrain) {
          ++background_job_abort_count_;
        } else {
          ++legacy_transform_abort_count_;
        }
      } else {
        const std::size_t sampled_records =
            std::max<std::size_t>(1, sorted_records.size());
        const double merge_ns =
            static_cast<double>(merge_end - merge_start);
        const double fit_ns = static_cast<double>(fit_end - fit_start);
        update_cost_ema(merge_cost_,
                        merge_ns / static_cast<double>(sampled_records));
        update_cost_ema(fit_cost_,
                        fit_ns / static_cast<double>(sampled_records));
        last_actual_retrain_ns_ = merge_ns + fit_ns;
      }
      active_recalibrations_.erase(active);
      prune_empty_leaves_locked();
      publish_read_snapshot_locked();
      return;
    }
  }

  void background_worker_loop() {
    for (;;) {
      std::shared_ptr<RecalibrationJob> job;
      {
        std::unique_lock<std::mutex> lock(job_mutex_);
        job_cv_.wait(lock,
                     [&]() { return stop_worker_ || !job_queue_.empty(); });
        if (stop_worker_ && job_queue_.empty()) {
          return;
        }
        job = job_queue_.front();
        job_queue_.pop_front();
        worker_busy_ = true;
      }
      process_recalibration_job(job);
      {
        std::lock_guard<std::mutex> lock(job_mutex_);
        worker_busy_ = false;
        if (job_queue_.empty()) {
          idle_cv_.notify_all();
        }
      }
    }
  }

  static std::uint64_t steady_now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
  }

  std::uint64_t query_bucket_width_ns() const {
    const std::uint64_t window_ns =
        static_cast<std::uint64_t>(config_.query_window_us) * 1000ULL;
    return std::max<std::uint64_t>(
        1, (window_ns + config_.query_window_buckets - 1) /
               config_.query_window_buckets);
  }

  std::size_t query_count_in_window(const Leaf& leaf,
                                    std::uint64_t now_ns) const {
    if (leaf.query_window.empty()) {
      return 0;
    }
    const std::uint64_t current_epoch = now_ns / query_bucket_width_ns();
    std::size_t result = 0;
    for (const QueryWindowBucket& bucket : leaf.query_window) {
      if (bucket.epoch <= current_epoch &&
          current_epoch - bucket.epoch < config_.query_window_buckets) {
        result += bucket.count;
      }
    }
    return result;
  }

  void record_leaf_query(const Leaf& leaf) const {
    const std::uint64_t now_ns = steady_now_ns();
    const std::uint64_t epoch = now_ns / query_bucket_width_ns();
    if (leaf.query_window.size() != config_.query_window_buckets) {
      leaf.query_window.assign(config_.query_window_buckets,
                               QueryWindowBucket{});
    }
    QueryWindowBucket& bucket =
        leaf.query_window[epoch % config_.query_window_buckets];
    if (bucket.epoch != epoch) {
      bucket.epoch = epoch;
      bucket.count = 0;
    }
    ++bucket.count;
    leaf.query_count_recent = query_count_in_window(leaf, now_ns);
  }

  void update_cost_ema(CostEma& cost, double sample) const {
    if (!std::isfinite(sample) || sample < 0.0) {
      return;
    }
    if (cost.samples == 0) {
      cost.value = sample;
    } else {
      cost.value = config_.cost_ema_alpha * sample +
                   (1.0 - config_.cost_ema_alpha) * cost.value;
    }
    ++cost.samples;
  }

  static double sampled_or_initial(const CostEma& cost, double initial) {
    return cost.samples == 0 ? initial : cost.value;
  }

  void apply_pending_rebuilds() {
    if (!config_.enable_background_recalibration || rcu_enabled()) {
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
    if (leaf_index >= leaves_.size()) {
      return;
    }
    Leaf& leaf = leaves_[leaf_index];
    if (leaf.kind != LeafKind::Model || leaf.pending_rebuild) {
      return;
    }
    const std::size_t buffer_size = leaf.buffer_ids.size();
    if (buffer_size >= config_.buffer_limit) {
      ++cost_retrain_trigger_count_;
      ++passive_retrain_trigger_count_;
      last_retrain_trigger_reason_ = TriggerReason::Passive;
      last_estimated_gain_ns_ = 0.0;
      last_estimated_retrain_ns_ = 0.0;
      schedule_or_rebuild_leaf(leaf_index);
      return;
    }
    if (!config_.enable_cost_retrain ||
        buffer_size < config_.active_buffer_threshold) {
      return;
    }
    const std::uint64_t query_now = steady_now_ns();
    const std::size_t query_count =
        rcu_enabled() ? snapshot_query_count(leaf.node_id, query_now)
                      : query_count_in_window(leaf, query_now);
    leaf.query_count_recent = query_count;
    if (query_count < config_.active_query_threshold) {
      return;
    }

    const std::uint64_t rcu_buffer_entries =
        rcu_buffer_scan_entries_.load(std::memory_order_relaxed);
    const std::uint64_t rcu_model_entries =
        rcu_model_scan_entries_.load(std::memory_order_relaxed);
    const double buffer_scan_ns_per_entry =
        rcu_enabled() && rcu_buffer_entries > 0
            ? static_cast<double>(rcu_buffer_scan_ns_.load(
                  std::memory_order_relaxed)) /
                  static_cast<double>(rcu_buffer_entries)
            : sampled_or_initial(
                  buffer_scan_cost_,
                  config_.initial_buffer_scan_ns_per_entry);
    const double model_scan_ns_per_entry =
        rcu_enabled() && rcu_model_entries > 0
            ? static_cast<double>(rcu_model_scan_ns_.load(
                  std::memory_order_relaxed)) /
                  static_cast<double>(rcu_model_entries)
            : sampled_or_initial(model_scan_cost_,
                                 config_.initial_model_scan_ns_per_entry);
    const std::size_t correction_width = std::max<std::size_t>(
        1, std::min<std::size_t>(
               leaf.record_ids.size(),
               static_cast<std::size_t>(
                   2.0 * std::ceil(leaf.max_model_error) + 1.0)));
    const double buffer_cost =
        buffer_scan_ns_per_entry * static_cast<double>(buffer_size);
    const double model_cost =
        model_scan_ns_per_entry * static_cast<double>(correction_width);
    const double gain = static_cast<double>(query_count) *
                        std::max(0.0, buffer_cost - model_cost);
    const std::size_t retrain_records = std::max<std::size_t>(
        1, leaf.record_ids.size() + leaf.buffer_ids.size());
    const double merge_ns_per_record = sampled_or_initial(
        merge_cost_, config_.initial_merge_ns_per_record);
    const double fit_ns_per_record = sampled_or_initial(
        fit_cost_, config_.initial_fit_ns_per_record);
    const double retrain_cost =
        static_cast<double>(retrain_records) *
        (merge_ns_per_record + fit_ns_per_record);
    if (gain > retrain_cost) {
      last_estimated_gain_ns_ = gain;
      last_estimated_retrain_ns_ = retrain_cost;
      ++cost_retrain_trigger_count_;
      ++active_retrain_trigger_count_;
      last_retrain_trigger_reason_ = TriggerReason::Active;
      leaf.query_count_recent = 0;
      schedule_or_rebuild_leaf(leaf_index);
    } else {
      last_rejected_gain_ns_ = gain;
      last_rejected_retrain_ns_ = retrain_cost;
      ++cost_retrain_rejected_count_;
    }
  }

  bool can_place_in_sorted_slot(const Leaf& leaf, std::size_t slot,
                                std::size_t new_id) const {
    std::size_t previous_id = npos();
    for (std::size_t i = slot; i > 0; --i) {
      const std::size_t id = leaf.record_ids[i - 1];
      if (id < records_.size() && records_[id].alive &&
          leaf.tombstone_ids.find(id) == leaf.tombstone_ids.end()) {
        previous_id = id;
        break;
      }
    }
    std::size_t next_id = npos();
    for (std::size_t i = slot + 1; i < leaf.record_ids.size(); ++i) {
      const std::size_t id = leaf.record_ids[i];
      if (id < records_.size() && records_[id].alive &&
          leaf.tombstone_ids.find(id) == leaf.tombstone_ids.end()) {
        next_id = id;
        break;
      }
    }
    return (previous_id == npos() || !record_id_less(new_id, previous_id)) &&
           (next_id == npos() || !record_id_less(next_id, new_id));
  }

  bool try_reuse_buffer_slot(std::size_t leaf_index, std::size_t new_id) {
    if (full_internal_directory_) {
      return false;
    }
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
    const auto own_slot = std::find(leaf.record_ids.begin(),
                                    leaf.record_ids.end(), new_id);
    if (own_slot != leaf.record_ids.end() &&
        leaf.tombstone_ids.find(new_id) != leaf.tombstone_ids.end()) {
      const std::size_t slot =
          static_cast<std::size_t>(own_slot - leaf.record_ids.begin());
      if (can_place_in_sorted_slot(leaf, slot, new_id)) {
        leaf.tombstone_ids.erase(new_id);
        --leaf.tombstone_count;
        records_[new_id].leaf_index = leaf_index;
        records_[new_id].in_buffer = false;
        expand_leaf_on_insert(
            leaf, RecordInput{new_id, records_[new_id].zmin,
                              records_[new_id].zmax, records_[new_id].box});
        ++deleted_slot_reuse_count_;
        note_directory_update(leaf_index);
        return true;
      }

      leaf.record_ids.erase(own_slot);
      leaf.tombstone_ids.erase(new_id);
      --leaf.tombstone_count;
      touch_leaf(leaf);
    }
    std::size_t best_slot = npos();
    double best_distance = std::numeric_limits<double>::infinity();
    const double predicted = leaf.slope * zmin + leaf.intercept;
    for (std::size_t slot = 0; slot < leaf.record_ids.size(); ++slot) {
      const std::size_t old_id = leaf.record_ids[slot];
      if (old_id < records_.size() &&
          leaf.tombstone_ids.find(old_id) != leaf.tombstone_ids.end() &&
          can_place_in_sorted_slot(leaf, slot, new_id)) {
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
    const std::size_t replaced_id = leaf.record_ids[best_slot];
    leaf.record_ids[best_slot] = new_id;
    leaf.tombstone_ids.erase(replaced_id);
    records_[new_id].leaf_index = leaf_index;
    records_[new_id].in_buffer = false;
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
    const double error_before = leaves_[leaf_index].max_model_error;
    const std::uint64_t merge_start = steady_now_ns();
    const std::size_t old_leaf_id = leaves_[leaf_index].node_id;
    std::vector<std::size_t> ids = leaves_[leaf_index].record_ids;
    ids.insert(ids.end(), leaves_[leaf_index].buffer_ids.begin(),
               leaves_[leaf_index].buffer_ids.end());
    std::vector<std::size_t> live_ids;
    live_ids.reserve(ids.size());
    for (std::size_t id : ids) {
      if (id < records_.size() && records_[id].alive &&
          leaves_[leaf_index].tombstone_ids.find(id) ==
              leaves_[leaf_index].tombstone_ids.end()) {
        live_ids.push_back(id);
      }
    }
    sort_ids(live_ids);
    const std::uint64_t merge_end = steady_now_ns();

    const std::uint64_t fit_start = steady_now_ns();
    std::vector<Leaf> replacement;
    for (std::size_t begin = 0; begin < live_ids.size();) {
      const std::size_t end =
          std::min(live_ids.size(), begin + config_.leaf_size);
      Leaf leaf;
      if (replacement.empty() && full_internal_directory_) {
        leaf.node_id = old_leaf_id;
      } else {
        assign_leaf_node_id(leaf);
      }
      leaf.record_ids.assign(live_ids.begin() + begin, live_ids.begin() + end);
      rebuild_leaf_metadata(leaf);
      replacement.push_back(std::move(leaf));
      begin = end;
    }
    const std::uint64_t fit_end = steady_now_ns();

    const std::size_t sampled_records = std::max<std::size_t>(1, ids.size());
    const double merge_ns = static_cast<double>(merge_end - merge_start);
    const double fit_ns = static_cast<double>(fit_end - fit_start);
    update_cost_ema(merge_cost_,
                    merge_ns / static_cast<double>(sampled_records));
    update_cost_ema(fit_cost_,
                    fit_ns / static_cast<double>(sampled_records));
    last_actual_retrain_ns_ = merge_ns + fit_ns;
    last_retrain_error_before_ = error_before;
    last_retrain_error_after_ = 0.0;
    for (const Leaf& leaf : replacement) {
      last_retrain_error_after_ =
          std::max(last_retrain_error_after_, leaf.max_model_error);
    }

    leaves_.erase(leaves_.begin() + static_cast<long>(leaf_index));
    leaves_.insert(leaves_.begin() + static_cast<long>(leaf_index),
                   std::make_move_iterator(replacement.begin()),
                   std::make_move_iterator(replacement.end()));
    ++local_rebuild_count_;
    rebuild_record_leaf_refs();
    if (full_internal_directory_ && directory_ready_) {
      if (replacement.empty()) {
        internal_directory_.remove_leaf(old_leaf_id);
      } else {
        internal_directory_.update_leaf_boundary(
            old_leaf_id, leaves_[leaf_index].max_zmin);
        std::size_t previous_id = old_leaf_id;
        for (std::size_t offset = 1; offset < replacement.size(); ++offset) {
          const Leaf& previous = leaves_[leaf_index + offset - 1];
          const Leaf& current = leaves_[leaf_index + offset];
          internal_directory_.insert_leaf_after(
              previous_id, current.node_id, previous.max_zmin,
              current.max_zmin);
          previous_id = current.node_id;
        }
      }
    }
    if (leaf_index < leaves_.size()) {
      try_transform_legacy_run(leaf_index);
    }
    if (!full_internal_directory_) {
      rebuild_record_leaf_refs();
      rebuild_directory();
    }
  }

  RegressionFit fit_leaf_for_transformation(const Leaf& leaf) const {
    std::vector<RecordInput> inputs;
    inputs.reserve(leaf.record_ids.size() + leaf.buffer_ids.size());
    const auto capture = [&](std::size_t id) {
      if (id >= records_.size() || !records_[id].alive) {
        return;
      }
      const Record& record = records_[id];
      inputs.push_back(RecordInput{id, record.zmin, record.zmax, record.box});
    };
    for (std::size_t id : leaf.record_ids) {
      if (leaf.tombstone_ids.find(id) == leaf.tombstone_ids.end()) {
        capture(id);
      }
    }
    for (std::size_t id : leaf.buffer_ids) {
      capture(id);
    }
    std::sort(inputs.begin(), inputs.end(), record_input_less);
    return fit_record_inputs(inputs, 0, inputs.size());
  }

  bool coefficients_similar(const Leaf& lhs, const Leaf& rhs) const {
    if (lhs.live_count < 2 || rhs.live_count < 2) {
      return false;
    }
    const RegressionFit lhs_fit = fit_leaf_for_transformation(lhs);
    const RegressionFit rhs_fit = fit_leaf_for_transformation(rhs);
    const double slope_scale =
        std::max(1.0,
                 std::max(std::fabs(lhs_fit.slope),
                          std::fabs(rhs_fit.slope)));
    const double lhs_origin =
        lhs_fit.slope * lhs.min_zmin + lhs_fit.intercept;
    const double rhs_origin =
        rhs_fit.slope * rhs.min_zmin + rhs_fit.intercept;
    return std::fabs(lhs_fit.slope - rhs_fit.slope) <=
               config_.legacy_slope_tolerance * slope_scale &&
           std::fabs(lhs_origin - rhs_origin) <=
               config_.legacy_intercept_tolerance;
  }

  bool run_recalibration_inline_locked(
      RecalibrationKind kind, const std::vector<std::size_t>& leaf_indices) {
    if (leaf_indices.empty()) {
      return false;
    }
    RecalibrationJob job;
    job.kind = kind;
    job.target_leaf_id = leaves_[leaf_indices.front()].node_id;
    for (std::size_t index : leaf_indices) {
      if (index >= leaves_.size()) {
        return false;
      }
      job.replaced_leaf_ids.push_back(leaves_[index].node_id);
      const auto capture = [&](std::size_t id) {
        if (id < records_.size() && records_[id].alive) {
          const Record& record = records_[id];
          job.records.push_back(
              RecordInput{id, record.zmin, record.zmax, record.box});
        }
      };
      for (std::size_t id : leaves_[index].record_ids) {
        if (leaves_[index].tombstone_ids.find(id) ==
            leaves_[index].tombstone_ids.end()) {
          capture(id);
        }
      }
      for (std::size_t id : leaves_[index].buffer_ids) {
        capture(id);
      }
    }
    std::sort(job.records.begin(), job.records.end(), record_input_less);
    std::vector<std::size_t> segments;
    if (!prepare_job_segments(job, job.records, segments) ||
        !install_retrained_leaf_locked(job, job.records, segments)) {
      if (kind != RecalibrationKind::ModelRetrain) {
        ++legacy_transform_abort_count_;
      }
      return false;
    }
    last_transform_input_leaves_ = leaf_indices.size();
    last_transform_input_records_ = job.records.size();
    last_recalibration_job_kind_ = kind;
    return true;
  }

  bool maybe_schedule_model_downgrade(std::size_t leaf_index) {
    if (!config_.enable_legacy_transform || leaf_index >= leaves_.size()) {
      return false;
    }
    const Leaf& leaf = leaves_[leaf_index];
    if (leaf.kind != LeafKind::Model || leaf.pending_rebuild ||
        leaf.live_count >= config_.min_model_leaf) {
      return false;
    }
    if (rcu_enabled()) {
      return queue_recalibration_job_locked(
          RecalibrationKind::ModelDowngrade,
          std::vector<std::size_t>{leaf_index});
    }
    return run_recalibration_inline_locked(
        RecalibrationKind::ModelDowngrade,
        std::vector<std::size_t>{leaf_index});
  }

  bool maybe_schedule_legacy_transformation(std::size_t center_index) {
    if (!config_.enable_legacy_transform || config_.force_legacy ||
        center_index >= leaves_.size() ||
        leaves_[center_index].kind != LeafKind::Legacy ||
        leaves_[center_index].pending_rebuild) {
      return false;
    }

    if (center_index > 0 &&
        leaves_[center_index - 1].kind == LeafKind::Model &&
        !leaves_[center_index - 1].pending_rebuild) {
      const std::size_t combined_records =
          leaves_[center_index - 1].live_count +
          leaves_[center_index].live_count;
      if (combined_records >= config_.min_model_leaf &&
          combined_records <= config_.model_leaf_size) {
        if (coefficients_similar(leaves_[center_index - 1],
                                 leaves_[center_index])) {
          const std::vector<std::size_t> indices{center_index - 1,
                                                 center_index};
          const bool scheduled =
              rcu_enabled()
                  ? queue_recalibration_job_locked(
                        RecalibrationKind::ForwardMerge, indices)
                  : run_recalibration_inline_locked(
                        RecalibrationKind::ForwardMerge, indices);
          if (scheduled) {
            ++legacy_forward_attempt_count_;
            return true;
          }
        } else {
          ++legacy_coefficient_reject_count_;
        }
      }
    }

    std::size_t begin = center_index;
    while (begin > 0 &&
           center_index - begin + 1 < config_.legacy_transform_max_leaves &&
           leaves_[begin - 1].kind == LeafKind::Legacy &&
           !leaves_[begin - 1].pending_rebuild) {
      --begin;
    }
    std::size_t end = center_index + 1;
    while (end < leaves_.size() &&
           end - begin < config_.legacy_transform_max_leaves &&
           leaves_[end].kind == LeafKind::Legacy &&
           !leaves_[end].pending_rebuild) {
      ++end;
    }
    if (end - begin < config_.legacy_backward_min_leaves) {
      return false;
    }
    bool similar = true;
    std::size_t combined_records = 0;
    for (std::size_t index = begin; index < end; ++index) {
      combined_records += leaves_[index].live_count;
      if (index > begin &&
          !coefficients_similar(leaves_[index - 1], leaves_[index])) {
        similar = false;
      }
    }
    if (!similar) {
      ++legacy_coefficient_reject_count_;
      return false;
    }
    if (combined_records < config_.min_model_leaf) {
      return false;
    }
    std::vector<std::size_t> indices;
    for (std::size_t index = begin; index < end; ++index) {
      indices.push_back(index);
    }
    const bool scheduled =
        rcu_enabled()
            ? queue_recalibration_job_locked(
                  RecalibrationKind::BackwardMerge, indices)
            : run_recalibration_inline_locked(
                  RecalibrationKind::BackwardMerge, indices);
    if (scheduled) {
      ++legacy_backward_attempt_count_;
    }
    return scheduled;
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
    std::vector<std::size_t> removed_leaf_ids;
    if (full_internal_directory_) {
      candidate.node_id = leaves_[begin].node_id;
      for (std::size_t i = begin + 1; i < end; ++i) {
        removed_leaf_ids.push_back(leaves_[i].node_id);
      }
    }
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
    if (full_internal_directory_) {
      if (directory_ready_) {
        internal_directory_.update_leaf_boundary(
            leaves_[begin].node_id, leaves_[begin].max_zmin);
        for (std::size_t removed_id : removed_leaf_ids) {
          internal_directory_.remove_leaf(removed_id);
        }
      }
    } else {
      rebuild_directory();
    }
    return true;
  }

  void rebuild_record_leaf_refs() {
    if (full_internal_directory_) {
      leaf_id_to_index_.clear();
    }
    for (Leaf& leaf : leaves_) {
      assign_leaf_node_id(leaf);
    }
    for (std::size_t leaf_index = 0; leaf_index < leaves_.size();
         ++leaf_index) {
      Leaf& leaf = leaves_[leaf_index];
      if (full_internal_directory_) {
        leaf_id_to_index_[leaf.node_id] = leaf_index;
        leaf.prev_leaf_id =
            leaf_index == 0 ? npos() : leaves_[leaf_index - 1].node_id;
        leaf.next_leaf_id = leaf_index + 1 == leaves_.size()
                                ? npos()
                                : leaves_[leaf_index + 1].node_id;
        leaf.buffer_positions.clear();
      }
      for (std::size_t id : leaf.record_ids) {
        if (id < records_.size() &&
            leaf.tombstone_ids.find(id) == leaf.tombstone_ids.end()) {
          records_[id].leaf_index = leaf_index;
          records_[id].in_buffer = false;
        }
      }
      for (std::size_t position = 0; position < leaf.buffer_ids.size();
           ++position) {
        const std::size_t id = leaf.buffer_ids[position];
        if (id < records_.size()) {
          records_[id].leaf_index = leaf_index;
          records_[id].in_buffer = true;
          if (full_internal_directory_) {
            leaf.buffer_positions[id] = position;
          }
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
    std::vector<std::pair<std::size_t, double>> full_entries;
    full_entries.reserve(leaves_.size());
    for (const Leaf& leaf : leaves_) {
      directory_min_zmin_.push_back(leaf.min_zmin);
      directory_max_zmin_.push_back(leaf.max_zmin);
      if (full_internal_directory_) {
        full_entries.emplace_back(leaf.node_id, leaf.max_zmin);
      }
    }
    fit_directory_model();
    if (full_internal_directory_) {
      internal_directory_.bulk_load(full_entries);
      directory_ready_ = true;
    }
    ++directory_rebuild_count_;
  }

  void note_directory_update(std::size_t leaf_index) {
    if (leaf_index >= leaves_.size()) {
      return;
    }
    if (full_internal_directory_) {
      internal_directory_.update_leaf_boundary(leaves_[leaf_index].node_id,
                                               leaves_[leaf_index].max_zmin);
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
    if (full_internal_directory_) {
      const std::size_t leaf_id = internal_directory_.find_leaf(zmin);
      const auto result = leaf_id_to_index_.find(leaf_id);
      if (result != leaf_id_to_index_.end()) {
        return result->second;
      }
    }
    auto lower = std::lower_bound(directory_max_zmin_.begin(),
                                  directory_max_zmin_.end(), zmin);
    if (lower == directory_max_zmin_.end()) {
      return leaves_.empty() ? 0 : leaves_.size() - 1;
    }
    return static_cast<std::size_t>(lower - directory_max_zmin_.begin());
  }

  std::size_t find_leaf_for_insert(double zmin) const {
    std::size_t leaf_index = npos();
    if (full_internal_directory_) {
      const std::size_t leaf_id =
          internal_directory_.find_leaf_for_insert(zmin);
      const auto result = leaf_id_to_index_.find(leaf_id);
      if (result != leaf_id_to_index_.end()) {
        leaf_index = result->second;
      }
    }
    if (leaf_index == npos()) {
      auto lower = std::lower_bound(directory_max_zmin_.begin(),
                                    directory_max_zmin_.end(), zmin);
      if (lower == directory_max_zmin_.end()) {
        leaf_index = leaves_.empty() ? 0 : leaves_.size() - 1;
      } else {
        leaf_index =
            static_cast<std::size_t>(lower - directory_max_zmin_.begin());
      }
    }
    while (leaf_index > 0 && zmin < leaves_[leaf_index - 1].max_zmin) {
      --leaf_index;
    }
    while (leaf_index + 1 < leaves_.size() &&
           zmin > leaves_[leaf_index].max_zmin) {
      ++leaf_index;
    }
    if (leaf_index < leaves_.size() &&
        zmin == leaves_[leaf_index].max_zmin) {
      while (leaf_index + 1 < leaves_.size() &&
             leaves_[leaf_index + 1].max_zmin == zmin) {
        ++leaf_index;
      }
    }
    return leaf_index;
  }

  std::size_t find_last_leaf_for_qmax(
      double qmax, HireInternalDirectory::SearchStats* internal_stats) const {
    if (directory_min_zmin_.empty()) {
      return npos();
    }
    if (!leaves_.empty() && leaves_.front().min_zmin > qmax) {
      return npos();
    }
    if (full_internal_directory_) {
      const std::size_t leaf_id =
          internal_directory_.find_leaf(qmax, internal_stats);
      const auto result = leaf_id_to_index_.find(leaf_id);
      if (result != leaf_id_to_index_.end()) {
        std::size_t index = result->second;
        while (index + 1 < leaves_.size() &&
               leaves_[index + 1].min_zmin <= qmax) {
          ++index;
        }
        return index;
      }
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

  std::shared_ptr<const ReadLeaf> make_read_leaf_locked(const Leaf& leaf) {
    const auto cached = read_leaf_cache_.find(leaf.node_id);
    if (cached != read_leaf_cache_.end() &&
        cached->second.version == leaf.read_version) {
      return cached->second.leaf;
    }

    std::shared_ptr<ReadLeaf> view = std::make_shared<ReadLeaf>();
    view->node_id = leaf.node_id;
    view->version = leaf.read_version;
    view->kind = leaf.kind;
    view->min_zmin = leaf.min_zmin;
    view->max_zmin = leaf.max_zmin;
    view->max_zmax = leaf.max_zmax;
    view->mbr = leaf.mbr;
    if (cached != read_leaf_cache_.end() && cached->second.leaf) {
      view->runtime = cached->second.leaf->runtime;
    }
    if (!view->runtime) {
      view->runtime =
          std::make_shared<LeafRuntime>(config_.query_window_buckets);
    }
    view->records.reserve(leaf.record_ids.size());
    for (std::size_t id : leaf.record_ids) {
      if (id >= records_.size() || !records_[id].alive ||
          leaf.tombstone_ids.find(id) != leaf.tombstone_ids.end()) {
        continue;
      }
      const Record& record = records_[id];
      view->records.push_back(
          ReadRecord{id, record.zmin, record.zmax, record.box});
    }
    view->buffer.reserve(leaf.buffer_ids.size());
    for (std::size_t id : leaf.buffer_ids) {
      if (id >= records_.size() || !records_[id].alive) {
        continue;
      }
      const Record& record = records_[id];
      view->buffer.push_back(
          ReadRecord{id, record.zmin, record.zmax, record.box});
    }
    read_leaf_cache_[leaf.node_id] =
        ReadLeafCacheEntry{leaf.read_version, view};
    return view;
  }

  void publish_read_snapshot_locked() {
    if (!rcu_enabled()) {
      return;
    }
    std::shared_ptr<ReadSnapshot> snapshot =
        std::make_shared<ReadSnapshot>();
    snapshot->generation = ++read_snapshot_generation_;
    snapshot->internal_directory = internal_directory_;
    snapshot->leaf_id_to_index = leaf_id_to_index_;
    snapshot->leaves.reserve(leaves_.size());
    snapshot->min_zmin.reserve(leaves_.size());
    std::unordered_set<std::size_t> live_leaf_ids;
    live_leaf_ids.reserve(leaves_.size());
    for (const Leaf& leaf : leaves_) {
      snapshot->leaves.push_back(make_read_leaf_locked(leaf));
      snapshot->min_zmin.push_back(leaf.min_zmin);
      live_leaf_ids.insert(leaf.node_id);
    }
    for (auto it = read_leaf_cache_.begin(); it != read_leaf_cache_.end();) {
      if (live_leaf_ids.find(it->first) == live_leaf_ids.end()) {
        it = read_leaf_cache_.erase(it);
      } else {
        ++it;
      }
    }

    std::shared_ptr<const ReadSnapshot> immutable = snapshot;
    std::shared_ptr<const ReadSnapshot> retired =
        std::atomic_exchange_explicit(&read_snapshot_, immutable,
                                      std::memory_order_acq_rel);
    ++rcu_snapshot_publish_count_;
    if (retired) {
      retired_read_snapshots_.push_back(retired);
      ++rcu_retired_snapshot_count_;
    }
    retired_read_snapshots_.erase(
        std::remove_if(retired_read_snapshots_.begin(),
                       retired_read_snapshots_.end(),
                       [&](const std::weak_ptr<const ReadSnapshot>& old) {
                         if (!old.expired()) {
                           return false;
                         }
                         ++rcu_reclaimed_snapshot_count_;
                         return true;
                       }),
        retired_read_snapshots_.end());
  }

  void record_snapshot_leaf_query(const ReadLeaf& leaf) const {
    if (!leaf.runtime || leaf.runtime->bucket_count == 0) {
      return;
    }
    const std::uint64_t epoch = steady_now_ns() / query_bucket_width_ns();
    AtomicQueryBucket& bucket =
        leaf.runtime->buckets[epoch % leaf.runtime->bucket_count];
    std::uint64_t observed = bucket.epoch.load(std::memory_order_acquire);
    if (observed != epoch &&
        bucket.epoch.compare_exchange_strong(
            observed, epoch, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      bucket.count.store(0, std::memory_order_release);
    }
    if (bucket.epoch.load(std::memory_order_acquire) == epoch) {
      bucket.count.fetch_add(1, std::memory_order_relaxed);
    }
  }

  std::size_t snapshot_query_count(std::size_t leaf_id,
                                   std::uint64_t now_ns) const {
    const auto cached = read_leaf_cache_.find(leaf_id);
    if (cached == read_leaf_cache_.end() || !cached->second.leaf ||
        !cached->second.leaf->runtime) {
      return 0;
    }
    const std::shared_ptr<LeafRuntime>& runtime =
        cached->second.leaf->runtime;
    const std::uint64_t current_epoch = now_ns / query_bucket_width_ns();
    std::size_t result = 0;
    for (std::size_t i = 0; i < runtime->bucket_count; ++i) {
      const std::uint64_t epoch =
          runtime->buckets[i].epoch.load(std::memory_order_acquire);
      if (epoch <= current_epoch &&
          current_epoch - epoch < runtime->bucket_count) {
        result += runtime->buckets[i].count.load(std::memory_order_relaxed);
      }
    }
    return result;
  }

  void range_query_snapshot(const ReadSnapshot& snapshot, double raw_qmin,
                            double raw_qmax, const Box2D& raw_query_box,
                            std::vector<std::size_t>& candidate_ids,
                            QueryStats* stats) const {
    rcu_active_readers_.fetch_add(1, std::memory_order_acq_rel);
    candidate_ids.clear();
    if (snapshot.leaves.empty()) {
      rcu_active_readers_.fetch_sub(1, std::memory_order_acq_rel);
      return;
    }
    double qmin = raw_qmin;
    double qmax = raw_qmax;
    if (qmin > qmax) {
      std::swap(qmin, qmax);
    }
    const Box2D query_box = normalize(raw_query_box);
    if (snapshot.min_zmin.front() > qmax) {
      rcu_active_readers_.fetch_sub(1, std::memory_order_acq_rel);
      return;
    }
    HireInternalDirectory::SearchStats internal_stats;
    const std::size_t leaf_id =
        snapshot.internal_directory.find_leaf(qmax, &internal_stats);
    const auto routed = snapshot.leaf_id_to_index.find(leaf_id);
    std::size_t index = npos();
    if (routed != snapshot.leaf_id_to_index.end()) {
      index = routed->second;
      while (index + 1 < snapshot.min_zmin.size() &&
             snapshot.min_zmin[index + 1] <= qmax) {
        ++index;
      }
    } else {
      auto upper = std::upper_bound(snapshot.min_zmin.begin(),
                                    snapshot.min_zmin.end(), qmax);
      if (upper != snapshot.min_zmin.begin()) {
        index = static_cast<std::size_t>(
            upper - snapshot.min_zmin.begin() - 1);
      }
    }
    if (index == npos() || index >= snapshot.leaves.size()) {
      rcu_active_readers_.fetch_sub(1, std::memory_order_acq_rel);
      return;
    }
    if (stats != nullptr) {
      stats->internal_nodes_visited += internal_stats.visited_nodes;
      stats->internal_model_searches += internal_stats.model_searches;
      stats->internal_fallback_searches += internal_stats.fallback_searches;
      stats->internal_log_entries_scanned +=
          internal_stats.log_entries_scanned;
    }
    for (;;) {
      const ReadLeaf& leaf = *snapshot.leaves[index];
      if (stats != nullptr) {
        ++stats->block_checks;
      }
      if (leaf.max_zmax >= qmin &&
          (!config_.enable_mbr_skip || intersects(leaf.mbr, query_box))) {
        if (stats != nullptr) {
          ++stats->visited_leaves;
        }
        record_snapshot_leaf_query(leaf);
        const std::size_t sample_sequence =
            leaf.runtime
                ? leaf.runtime->cost_sample_sequence.fetch_add(
                      1, std::memory_order_relaxed) +
                      1
                : 0;
        const bool sample_cost =
            leaf.kind == LeafKind::Model && sample_sequence > 0 &&
            sample_sequence % config_.cost_sample_every == 0;
        const std::uint64_t model_start =
            sample_cost ? steady_now_ns() : 0;
        std::size_t model_scanned = 0;
        for (const ReadRecord& record : leaf.records) {
          ++model_scanned;
          if (stats != nullptr) {
            ++stats->records_scanned;
          }
          if (record.zmin > qmax) {
            break;
          }
          if (record.zmax >= qmin && intersects(record.box, query_box)) {
            candidate_ids.push_back(record.id);
            if (stats != nullptr) {
              ++stats->mbr_candidates;
            }
          }
        }
        if (sample_cost && model_scanned > 0) {
          rcu_model_scan_ns_.fetch_add(steady_now_ns() - model_start,
                                       std::memory_order_relaxed);
          rcu_model_scan_entries_.fetch_add(model_scanned,
                                            std::memory_order_relaxed);
          rcu_model_scan_samples_.fetch_add(1,
                                            std::memory_order_relaxed);
        }
        const bool sample_buffer = sample_cost && !leaf.buffer.empty();
        const std::uint64_t buffer_start =
            sample_buffer ? steady_now_ns() : 0;
        std::size_t buffer_scanned = 0;
        for (const ReadRecord& record : leaf.buffer) {
          ++buffer_scanned;
          if (stats != nullptr) {
            ++stats->buffer_records_scanned;
          }
          if (record.zmin <= qmax && record.zmax >= qmin &&
              intersects(record.box, query_box)) {
            candidate_ids.push_back(record.id);
            if (stats != nullptr) {
              ++stats->mbr_candidates;
            }
          }
        }
        if (sample_buffer && buffer_scanned > 0) {
          rcu_buffer_scan_ns_.fetch_add(steady_now_ns() - buffer_start,
                                        std::memory_order_relaxed);
          rcu_buffer_scan_entries_.fetch_add(buffer_scanned,
                                             std::memory_order_relaxed);
          rcu_buffer_scan_samples_.fetch_add(1,
                                             std::memory_order_relaxed);
        }
      } else if (stats != nullptr) {
        if (leaf.max_zmax < qmin) {
          ++stats->skipped_zmax_leaves;
        } else {
          ++stats->skipped_mbr_leaves;
        }
      }
      if (index == 0) {
        break;
      }
      --index;
      if (stats != nullptr) {
        ++stats->leaf_sibling_hops;
      }
    }
    rcu_active_readers_.fetch_sub(1, std::memory_order_acq_rel);
  }

  void scan_leaf(const Leaf& leaf, double qmin, double qmax,
                 const Box2D& query_box,
                 std::vector<std::size_t>& candidate_ids,
                 QueryStats* stats) const {
    if (stats != nullptr) {
      ++stats->block_checks;
    }
    if (leaf.live_count == 0 || leaf.min_zmin > qmax) {
      return;
    }
    if (config_.enable_zmax_skip && leaf.max_zmax < qmin) {
      if (stats != nullptr) {
        ++stats->skipped_zmax_leaves;
      }
      return;
    }
    if (config_.enable_mbr_skip && !intersects(leaf.mbr, query_box)) {
      if (stats != nullptr) {
        ++stats->skipped_mbr_leaves;
      }
      return;
    }
    if (stats != nullptr) {
      ++stats->visited_leaves;
    }
    record_leaf_query(leaf);
    ++leaf.cost_sample_sequence;
    const bool sample_cost =
        leaf.kind == LeafKind::Model &&
        leaf.cost_sample_sequence % config_.cost_sample_every == 0;

    const std::uint64_t model_start = sample_cost ? steady_now_ns() : 0;
    const std::size_t model_entries =
        scan_sorted_ids(leaf.record_ids, qmin, qmax, query_box, candidate_ids,
                        stats, false, &leaf.tombstone_ids);
    if (sample_cost && model_entries > 0) {
      const double elapsed =
          static_cast<double>(steady_now_ns() - model_start);
      update_cost_ema(model_scan_cost_,
                      elapsed / static_cast<double>(model_entries));
    }

    const bool sample_buffer = sample_cost && !leaf.buffer_ids.empty();
    const std::uint64_t buffer_start = sample_buffer ? steady_now_ns() : 0;
    const std::size_t buffer_entries =
        scan_unsorted_ids(leaf.buffer_ids, qmin, qmax, query_box,
                          candidate_ids, stats);
    if (sample_buffer && buffer_entries > 0) {
      const double elapsed =
          static_cast<double>(steady_now_ns() - buffer_start);
      update_cost_ema(buffer_scan_cost_,
                      elapsed / static_cast<double>(buffer_entries));
    }
  }

  std::size_t scan_sorted_ids(const std::vector<std::size_t>& ids,
                              double qmin, double qmax,
                              const Box2D& query_box,
                              std::vector<std::size_t>& candidate_ids,
                              QueryStats* stats,
                              bool count_as_buffer,
                              const std::unordered_set<std::size_t>*
                                  tombstone_ids = nullptr) const {
    std::size_t scanned = 0;
    for (std::size_t id : ids) {
      if (id >= records_.size()) {
        continue;
      }
      if (tombstone_ids != nullptr &&
          tombstone_ids->find(id) != tombstone_ids->end()) {
        continue;
      }
      ++scanned;
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
    return scanned;
  }

  std::size_t scan_unsorted_ids(const std::vector<std::size_t>& ids,
                                double qmin, double qmax,
                                const Box2D& query_box,
                                std::vector<std::size_t>& candidate_ids,
                                QueryStats* stats) const {
    std::size_t scanned = 0;
    for (std::size_t id : ids) {
      if (id >= records_.size()) {
        continue;
      }
      ++scanned;
      const Record& record = records_[id];
      if (stats != nullptr) {
        ++stats->buffer_records_scanned;
      }
      if (record.zmin > qmax) {
        continue;
      }
      evaluate_record(id, record, qmin, query_box, candidate_ids, stats);
    }
    return scanned;
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
  bool full_internal_directory_ = false;
  HireInternalDirectory internal_directory_;
  bool directory_ready_ = false;
  std::size_t next_leaf_node_id_ = 1;
  bool initialized_ = false;
  std::vector<Record> records_;
  std::vector<Leaf> leaves_;
  std::unordered_map<std::size_t, std::size_t> leaf_id_to_index_;
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
  std::size_t leaf_split_count_ = 0;
  std::size_t leaf_merge_count_ = 0;
  std::size_t leaf_redistribution_count_ = 0;
  std::size_t buffer_swap_delete_count_ = 0;
  std::size_t active_retrain_trigger_count_ = 0;
  std::size_t passive_retrain_trigger_count_ = 0;
  std::size_t cost_retrain_rejected_count_ = 0;
  mutable CostEma buffer_scan_cost_;
  mutable CostEma model_scan_cost_;
  CostEma merge_cost_;
  CostEma fit_cost_;
  double last_estimated_gain_ns_ = 0.0;
  double last_estimated_retrain_ns_ = 0.0;
  double last_rejected_gain_ns_ = 0.0;
  double last_rejected_retrain_ns_ = 0.0;
  double last_actual_retrain_ns_ = 0.0;
  double last_retrain_error_before_ = 0.0;
  double last_retrain_error_after_ = 0.0;
  TriggerReason last_retrain_trigger_reason_ = TriggerReason::None;
  mutable std::mutex writer_mutex_;
  std::shared_ptr<const ReadSnapshot> read_snapshot_;
  std::unordered_map<std::size_t, ReadLeafCacheEntry> read_leaf_cache_;
  std::vector<std::weak_ptr<const ReadSnapshot>> retired_read_snapshots_;
  std::uint64_t next_read_version_ = 1;
  std::uint64_t read_snapshot_generation_ = 0;
  mutable std::atomic<std::size_t> rcu_active_readers_{0};
  mutable std::atomic<std::uint64_t> rcu_model_scan_ns_{0};
  mutable std::atomic<std::uint64_t> rcu_model_scan_entries_{0};
  mutable std::atomic<std::size_t> rcu_model_scan_samples_{0};
  mutable std::atomic<std::uint64_t> rcu_buffer_scan_ns_{0};
  mutable std::atomic<std::uint64_t> rcu_buffer_scan_entries_{0};
  mutable std::atomic<std::size_t> rcu_buffer_scan_samples_{0};
  std::mutex job_mutex_;
  std::condition_variable job_cv_;
  std::condition_variable idle_cv_;
  std::deque<std::shared_ptr<RecalibrationJob>> job_queue_;
  std::thread worker_thread_;
  bool stop_worker_ = false;
  bool worker_busy_ = false;
  std::unordered_map<std::size_t, std::shared_ptr<RecalibrationJob>>
      active_recalibrations_;
  std::uint64_t next_update_sequence_ = 1;
  std::size_t pap_snapshot_count_ = 0;
  std::size_t pap_max_levels_ = 0;
  std::size_t mls_install_count_ = 0;
  std::size_t mls_update_log_entries_ = 0;
  std::size_t mls_update_replay_count_ = 0;
  std::size_t mls_final_validation_repair_count_ = 0;
  std::size_t rcu_snapshot_publish_count_ = 0;
  std::size_t rcu_retired_snapshot_count_ = 0;
  std::size_t rcu_reclaimed_snapshot_count_ = 0;
  std::size_t background_job_count_ = 0;
  std::size_t background_job_abort_count_ = 0;
  std::size_t last_pap_levels_ = 0;
  std::size_t last_pap_sigma_ = 0;
  std::size_t last_mls_covered_leaves_ = 0;
  std::size_t legacy_forward_attempt_count_ = 0;
  std::size_t legacy_forward_success_count_ = 0;
  std::size_t legacy_backward_attempt_count_ = 0;
  std::size_t legacy_backward_success_count_ = 0;
  std::size_t model_downgrade_count_ = 0;
  std::size_t legacy_transform_abort_count_ = 0;
  std::size_t legacy_coefficient_reject_count_ = 0;
  std::size_t last_transform_input_leaves_ = 0;
  std::size_t last_transform_input_records_ = 0;
  RecalibrationKind last_recalibration_job_kind_ =
      RecalibrationKind::ModelRetrain;
  bulk_loading::OptimizationStats bulk_leaf_optimization_stats_;
  std::uint64_t bulk_load_ns_ = 0;
};

}  // namespace hire_sfc_lite
