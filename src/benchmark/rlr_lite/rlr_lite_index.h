#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace rlr_lite {

struct Box2D {
  double xmin = 0.0;
  double ymin = 0.0;
  double xmax = 0.0;
  double ymax = 0.0;
};

inline Box2D normalize(Box2D b) {
  if (!std::isfinite(b.xmin)) b.xmin = 0.0;
  if (!std::isfinite(b.ymin)) b.ymin = 0.0;
  if (!std::isfinite(b.xmax)) b.xmax = b.xmin;
  if (!std::isfinite(b.ymax)) b.ymax = b.ymin;
  if (b.xmin > b.xmax) std::swap(b.xmin, b.xmax);
  if (b.ymin > b.ymax) std::swap(b.ymin, b.ymax);
  return b;
}

inline double width(const Box2D& b) {
  return std::max(0.0, b.xmax - b.xmin);
}

inline double height(const Box2D& b) {
  return std::max(0.0, b.ymax - b.ymin);
}

inline double area(const Box2D& b) { return width(b) * height(b); }

inline double perimeter(const Box2D& b) {
  return 2.0 * (width(b) + height(b));
}

inline Box2D combine(const Box2D& a, const Box2D& b) {
  return Box2D{std::min(a.xmin, b.xmin), std::min(a.ymin, b.ymin),
               std::max(a.xmax, b.xmax), std::max(a.ymax, b.ymax)};
}

inline bool intersects(const Box2D& a, const Box2D& b) {
  return !(a.xmax < b.xmin || b.xmax < a.xmin || a.ymax < b.ymin ||
           b.ymax < a.ymin);
}

inline double intersection_area(const Box2D& a, const Box2D& b) {
  const double xmin = std::max(a.xmin, b.xmin);
  const double ymin = std::max(a.ymin, b.ymin);
  const double xmax = std::min(a.xmax, b.xmax);
  const double ymax = std::min(a.ymax, b.ymax);
  if (xmax < xmin || ymax < ymin) {
    return 0.0;
  }
  return (xmax - xmin) * (ymax - ymin);
}

inline double enlargement_area(const Box2D& old_box, const Box2D& new_box) {
  return area(combine(old_box, new_box)) - area(old_box);
}

inline double enlargement_perimeter(const Box2D& old_box,
                                    const Box2D& new_box) {
  return perimeter(combine(old_box, new_box)) - perimeter(old_box);
}

struct QueryStats {
  std::size_t visited_nodes = 0;
  std::size_t leaf_entries_checked = 0;
  std::size_t mbr_candidates = 0;
};

struct RLRFeature {
  double delta_area = 0.0;
  double delta_perimeter = 0.0;
  double delta_overlap = 0.0;
  double occupancy = 0.0;
};

class RLRLiteAgent {
 public:
  explicit RLRLiteAgent(std::uint64_t seed)
      : rng_(seed), weights_{-0.40, -0.10, -0.40, -0.10} {
    epsilon_ = env_double("RLR_EPSILON", 0.10);
    epsilon_min_ = env_double("RLR_EPSILON_MIN", 0.02);
    lr_ = env_double("RLR_LR", 0.01);
  }

  int select_action(const std::vector<RLRFeature>& candidates, bool training) {
    if (candidates.empty()) {
      return 0;
    }
    if (training) {
      std::uniform_real_distribution<double> probability(0.0, 1.0);
      if (probability(rng_) < epsilon_) {
        std::uniform_int_distribution<int> pick(
            0, static_cast<int>(candidates.size() - 1));
        return pick(rng_);
      }
    }
    int best = 0;
    double best_q = q(candidates[0]);
    for (std::size_t i = 1; i < candidates.size(); ++i) {
      const double value = q(candidates[i]);
      if (value > best_q) {
        best_q = value;
        best = static_cast<int>(i);
      }
    }
    return best;
  }

  void observe_transition(const RLRFeature& chosen_feature) {
    last_feature_ = chosen_feature;
    has_last_feature_ = true;
  }

  void update(double reward) {
    if (!has_last_feature_) {
      return;
    }
    const double prediction = q(last_feature_);
    const double error = reward - prediction;
    weights_[0] += lr_ * error * last_feature_.delta_area;
    weights_[1] += lr_ * error * last_feature_.delta_perimeter;
    weights_[2] += lr_ * error * last_feature_.delta_overlap;
    weights_[3] += lr_ * error * last_feature_.occupancy;
    epsilon_ = std::max(epsilon_min_, epsilon_ * 0.999);
  }

 private:
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

  double q(const RLRFeature& feature) const {
    return weights_[0] * feature.delta_area +
           weights_[1] * feature.delta_perimeter +
           weights_[2] * feature.delta_overlap +
           weights_[3] * feature.occupancy;
  }

  std::mt19937_64 rng_;
  double weights_[4];
  double epsilon_ = 0.10;
  double epsilon_min_ = 0.02;
  double lr_ = 0.01;
  RLRFeature last_feature_;
  bool has_last_feature_ = false;
};

struct RTreeLiteNode {
  struct Entry {
    Box2D box;
    std::size_t id = 0;
    RTreeLiteNode* child = nullptr;
  };

  bool leaf = true;
  Box2D mbr;
  RTreeLiteNode* parent = nullptr;
  std::vector<Entry> entries;
};

enum class ChoosePolicy { Heuristic, RLLite };

class RTreeLite {
 public:
  RTreeLite(std::size_t node_capacity, double min_fill_ratio,
            std::size_t top_k, std::uint64_t seed, ChoosePolicy policy)
      : node_capacity_(std::max<std::size_t>(4, node_capacity)),
        min_fill_(std::max<std::size_t>(
            2, static_cast<std::size_t>(
                   std::floor(min_fill_ratio *
                              static_cast<double>(node_capacity_))))),
        top_k_(std::max<std::size_t>(1, top_k)),
        policy_(policy),
        agent_(seed) {
    min_fill_ = std::min(min_fill_, node_capacity_ / 2);
    root_ = make_node(true);
  }

  void insert(const Box2D& raw_box, std::size_t id, bool training) {
    const Box2D box = normalize(raw_box);
    RTreeLiteNode* leaf = choose_leaf(root_, box, training);
    leaf->entries.push_back(RTreeLiteNode::Entry{box, id, nullptr});
    adjust_after_insert(leaf);
  }

  void range_query(const Box2D& raw_query, const std::vector<unsigned char>& alive,
                   std::vector<std::size_t>& candidate_ids,
                   QueryStats* stats) const {
    const Box2D query = normalize(raw_query);
    range_query_node(root_, query, alive, candidate_ids, stats);
  }

  QueryStats range_query_stats(const Box2D& raw_query,
                               const std::vector<unsigned char>& alive) const {
    QueryStats stats;
    std::vector<std::size_t> scratch;
    range_query(raw_query, alive, scratch, &stats);
    return stats;
  }

  std::size_t node_count() const { return nodes_.size(); }

  std::size_t height() const {
    std::size_t h = 0;
    const RTreeLiteNode* node = root_;
    while (node != nullptr) {
      ++h;
      if (node->leaf || node->entries.empty()) {
        break;
      }
      node = node->entries.front().child;
    }
    return h;
  }

  std::size_t entry_count() const {
    std::size_t total = 0;
    for (const auto& node : nodes_) {
      total += node->entries.size();
    }
    return total;
  }

  std::size_t estimate_bytes() const {
    return sizeof(*this) + nodes_.size() * sizeof(RTreeLiteNode) +
           entry_count() * sizeof(RTreeLiteNode::Entry);
  }

  RLRLiteAgent& agent() { return agent_; }

 private:
  RTreeLiteNode* make_node(bool leaf) {
    std::unique_ptr<RTreeLiteNode> node(new RTreeLiteNode());
    node->leaf = leaf;
    RTreeLiteNode* ptr = node.get();
    nodes_.push_back(std::move(node));
    return ptr;
  }

  RTreeLiteNode* choose_leaf(RTreeLiteNode* node, const Box2D& box,
                             bool training) {
    while (!node->leaf) {
      node = choose_child(node, box, training);
      if (node == nullptr) {
        throw std::runtime_error("RLR_LITE_CS internal error: choose_child returned null");
      }
    }
    return node;
  }

  RTreeLiteNode* choose_child(RTreeLiteNode* node, const Box2D& box,
                              bool training) {
    std::vector<std::size_t> order(node->entries.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
      order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
      if (lhs >= node->entries.size() || rhs >= node->entries.size()) {
        throw std::runtime_error("RLR_LITE_CS internal error: child order index out of range");
      }
      const auto& a = node->entries[lhs];
      const auto& b = node->entries[rhs];
      if (a.child == nullptr || b.child == nullptr) {
        throw std::runtime_error("RLR_LITE_CS internal error: internal entry has null child");
      }
      const double da = enlargement_area(a.box, box);
      const double db = enlargement_area(b.box, box);
      if (da != db) return da < db;
      const double oa = overlap_delta(node, lhs, box);
      const double ob = overlap_delta(node, rhs, box);
      if (oa != ob) return oa < ob;
      return area(a.box) < area(b.box);
    });

    if (policy_ == ChoosePolicy::Heuristic || order.size() <= 1) {
      return node->entries[order.front()].child;
    }

    const std::size_t count = std::min(top_k_, order.size());
    std::vector<RLRFeature> features;
    features.reserve(count);
    double max_da = 0.0;
    double max_dp = 0.0;
    double max_do = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
      const std::size_t entry_index = order[i];
      const auto& entry = node->entries[entry_index];
      RLRFeature feature;
      feature.delta_area = std::max(0.0, enlargement_area(entry.box, box));
      feature.delta_perimeter =
          std::max(0.0, enlargement_perimeter(entry.box, box));
      feature.delta_overlap = std::max(0.0, overlap_delta(node, entry_index, box));
      feature.occupancy =
          entry.child == nullptr
              ? 0.0
              : static_cast<double>(entry.child->entries.size()) /
                    static_cast<double>(node_capacity_);
      max_da = std::max(max_da, feature.delta_area);
      max_dp = std::max(max_dp, feature.delta_perimeter);
      max_do = std::max(max_do, feature.delta_overlap);
      features.push_back(feature);
    }
    const double eps = 1e-12;
    for (RLRFeature& feature : features) {
      feature.delta_area /= (max_da + eps);
      feature.delta_perimeter /= (max_dp + eps);
      feature.delta_overlap /= (max_do + eps);
    }
    const int action = agent_.select_action(features, training);
    const std::size_t selected = order[static_cast<std::size_t>(
        std::max(0, std::min(action, static_cast<int>(count - 1))))];
    if (training) {
      agent_.observe_transition(features[static_cast<std::size_t>(
          std::max(0, std::min(action, static_cast<int>(count - 1))))]);
    }
    return node->entries[selected].child;
  }

  double overlap_delta(const RTreeLiteNode* node, std::size_t entry_index,
                       const Box2D& box) const {
    const Box2D old_box = node->entries[entry_index].box;
    const Box2D new_box = combine(old_box, box);
    double old_overlap = 0.0;
    double new_overlap = 0.0;
    for (std::size_t i = 0; i < node->entries.size(); ++i) {
      if (i == entry_index) {
        continue;
      }
      old_overlap += intersection_area(old_box, node->entries[i].box);
      new_overlap += intersection_area(new_box, node->entries[i].box);
    }
    return new_overlap - old_overlap;
  }

  void adjust_after_insert(RTreeLiteNode* node) {
    while (node != nullptr) {
      recompute_mbr(node);
      update_parent_entry(node);
      if (node->entries.size() <= node_capacity_) {
        node = node->parent;
        continue;
      }
      RTreeLiteNode* sibling = split_node(node);
      if (node->parent == nullptr) {
        RTreeLiteNode* new_root = make_node(false);
        node->parent = new_root;
        sibling->parent = new_root;
        recompute_mbr(node);
        recompute_mbr(sibling);
        new_root->entries.push_back(RTreeLiteNode::Entry{node->mbr, 0, node});
        new_root->entries.push_back(
            RTreeLiteNode::Entry{sibling->mbr, 0, sibling});
        recompute_mbr(new_root);
        root_ = new_root;
        return;
      }
      RTreeLiteNode* parent = node->parent;
      sibling->parent = parent;
      update_parent_entry(node);
      parent->entries.push_back(RTreeLiteNode::Entry{sibling->mbr, 0, sibling});
      node = parent;
    }
  }

  RTreeLiteNode* split_node(RTreeLiteNode* node) {
    std::vector<RTreeLiteNode::Entry> entries;
    entries.swap(node->entries);
    RTreeLiteNode* sibling = make_node(node->leaf);

    std::size_t seed_a = 0;
    std::size_t seed_b = 1;
    double best_waste = -std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < entries.size(); ++i) {
      for (std::size_t j = i + 1; j < entries.size(); ++j) {
        const double waste =
            area(combine(entries[i].box, entries[j].box)) -
            area(entries[i].box) - area(entries[j].box);
        if (waste > best_waste) {
          best_waste = waste;
          seed_a = i;
          seed_b = j;
        }
      }
    }

    std::vector<unsigned char> assigned(entries.size(), 0);
    add_split_entry(node, entries[seed_a]);
    add_split_entry(sibling, entries[seed_b]);
    assigned[seed_a] = 1;
    assigned[seed_b] = 1;

    std::size_t remaining = entries.size() - 2;
    while (remaining > 0) {
      if (node->entries.size() + remaining == min_fill_) {
        for (std::size_t i = 0; i < entries.size(); ++i) {
          if (!assigned[i]) {
            add_split_entry(node, entries[i]);
            assigned[i] = 1;
            --remaining;
          }
        }
        break;
      }
      if (sibling->entries.size() + remaining == min_fill_) {
        for (std::size_t i = 0; i < entries.size(); ++i) {
          if (!assigned[i]) {
            add_split_entry(sibling, entries[i]);
            assigned[i] = 1;
            --remaining;
          }
        }
        break;
      }

      std::size_t next = 0;
      bool found = false;
      double best_diff = -1.0;
      for (std::size_t i = 0; i < entries.size(); ++i) {
        if (assigned[i]) {
          continue;
        }
        const double da = enlargement_area(node->mbr, entries[i].box);
        const double db = enlargement_area(sibling->mbr, entries[i].box);
        const double diff = std::fabs(da - db);
        if (!found || diff > best_diff) {
          found = true;
          best_diff = diff;
          next = i;
        }
      }

      const double da = enlargement_area(node->mbr, entries[next].box);
      const double db = enlargement_area(sibling->mbr, entries[next].box);
      RTreeLiteNode* target = node;
      if (db < da ||
          (db == da && area(sibling->mbr) < area(node->mbr)) ||
          (db == da && area(sibling->mbr) == area(node->mbr) &&
           sibling->entries.size() < node->entries.size())) {
        target = sibling;
      }
      add_split_entry(target, entries[next]);
      assigned[next] = 1;
      --remaining;
    }

    recompute_mbr(node);
    recompute_mbr(sibling);
    return sibling;
  }

  void add_split_entry(RTreeLiteNode* node, const RTreeLiteNode::Entry& entry) {
    node->entries.push_back(entry);
    if (!node->leaf && entry.child != nullptr) {
      entry.child->parent = node;
    }
    if (node->entries.size() == 1) {
      node->mbr = entry.box;
    } else {
      node->mbr = combine(node->mbr, entry.box);
    }
  }

  void update_parent_entry(RTreeLiteNode* node) {
    if (node->parent == nullptr) {
      return;
    }
    for (auto& entry : node->parent->entries) {
      if (entry.child == node) {
        entry.box = node->mbr;
        return;
      }
    }
  }

  void recompute_mbr(RTreeLiteNode* node) {
    if (node->entries.empty()) {
      node->mbr = Box2D{};
      return;
    }
    Box2D box = node->entries.front().box;
    for (std::size_t i = 1; i < node->entries.size(); ++i) {
      box = combine(box, node->entries[i].box);
    }
    node->mbr = box;
  }

  void range_query_node(const RTreeLiteNode* node, const Box2D& query,
                        const std::vector<unsigned char>& alive,
                        std::vector<std::size_t>& candidate_ids,
                        QueryStats* stats) const {
    if (node == nullptr) {
      return;
    }
    if (stats != nullptr) {
      ++stats->visited_nodes;
    }
    for (const auto& entry : node->entries) {
      if (!intersects(entry.box, query)) {
        continue;
      }
      if (node->leaf) {
        if (stats != nullptr) {
          ++stats->leaf_entries_checked;
        }
        if (entry.id < alive.size() && alive[entry.id]) {
          candidate_ids.push_back(entry.id);
          if (stats != nullptr) {
            ++stats->mbr_candidates;
          }
        }
      } else {
        range_query_node(entry.child, query, alive, candidate_ids, stats);
      }
    }
  }

  std::size_t node_capacity_ = 64;
  std::size_t min_fill_ = 25;
  std::size_t top_k_ = 4;
  ChoosePolicy policy_ = ChoosePolicy::RLLite;
  RTreeLiteNode* root_ = nullptr;
  std::vector<std::unique_ptr<RTreeLiteNode>> nodes_;
  RLRLiteAgent agent_;
};

class RLRLiteIndex {
 public:
  explicit RLRLiteIndex(std::size_t object_count,
                        const std::vector<Box2D>& train_queries)
      : config_(Config::from_env()),
        alive_(object_count, 0),
        rl_tree_(config_.node_capacity, config_.min_fill_ratio, config_.top_k,
                 config_.seed, ChoosePolicy::RLLite),
        ref_tree_(config_.node_capacity, config_.min_fill_ratio, config_.top_k,
                  config_.seed, ChoosePolicy::Heuristic),
        train_queries_(train_queries) {
    if (train_queries_.size() > config_.train_query_limit) {
      train_queries_.resize(config_.train_query_limit);
    }
  }

  void bulk_load(const std::vector<std::pair<Box2D, std::size_t>>& entries) {
    for (const auto& entry : entries) {
      insert(entry.first, entry.second);
    }
  }

  bool insert(const Box2D& box, std::size_t id) {
    ensure_alive_size(id + 1);
    if (alive_[id]) {
      return false;
    }
    alive_[id] = 1;
    rl_tree_.insert(box, id, true);
    ref_tree_.insert(box, id, false);
    ++insert_since_reward_;
    if (config_.reward_interval > 0 &&
        insert_since_reward_ >= config_.reward_interval) {
      update_reward();
      insert_since_reward_ = 0;
    }
    return true;
  }

  bool erase(std::size_t id) {
    if (id >= alive_.size() || !alive_[id]) {
      return false;
    }
    alive_[id] = 0;
    ++deleted_count_;
    return true;
  }

  void range_query(const Box2D& query, std::vector<std::size_t>& candidate_ids,
                   QueryStats* stats) const {
    rl_tree_.range_query(query, alive_, candidate_ids, stats);
  }

  std::size_t estimate_bytes() const {
    return rl_tree_.estimate_bytes() + ref_tree_.estimate_bytes() +
           alive_.size() * sizeof(unsigned char);
  }

  std::size_t node_count() const { return rl_tree_.node_count(); }
  std::size_t height() const { return rl_tree_.height(); }
  std::size_t live_count() const {
    std::size_t count = 0;
    for (unsigned char value : alive_) {
      if (value) {
        ++count;
      }
    }
    return count;
  }
  std::size_t dead_count() const { return deleted_count_; }

 private:
  struct Config {
    std::size_t node_capacity = 64;
    double min_fill_ratio = 0.4;
    std::size_t top_k = 4;
    std::uint64_t seed = 42;
    std::size_t reward_interval = 2048;
    std::size_t train_query_limit = 64;

    static Config from_env() {
      Config config;
      config.node_capacity = env_size("RLR_NODE_CAPACITY", 64);
      config.min_fill_ratio = env_double("RLR_MIN_FILL_RATIO", 0.4);
      config.top_k = env_size("RLR_TOPK", 4);
      config.seed = static_cast<std::uint64_t>(env_size("RLR_SEED", 42));
      config.reward_interval = env_size("RLR_REWARD_INTERVAL", 2048);
      config.train_query_limit = env_size("RLR_TRAIN_QUERY_LIMIT", 64);
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
  };

  void ensure_alive_size(std::size_t size) {
    if (alive_.size() < size) {
      alive_.resize(size, 0);
    }
  }

  void update_reward() {
    if (train_queries_.empty()) {
      return;
    }
    std::size_t rl_nodes = 0;
    std::size_t ref_nodes = 0;
    std::size_t rl_candidates = 0;
    std::size_t ref_candidates = 0;
    for (const Box2D& query : train_queries_) {
      const QueryStats rl_stats = rl_tree_.range_query_stats(query, alive_);
      const QueryStats ref_stats = ref_tree_.range_query_stats(query, alive_);
      rl_nodes += rl_stats.visited_nodes;
      ref_nodes += ref_stats.visited_nodes;
      rl_candidates += rl_stats.mbr_candidates;
      ref_candidates += ref_stats.mbr_candidates;
    }
    const double reward_nodes =
        static_cast<double>(ref_nodes) - static_cast<double>(rl_nodes);
    const double reward_candidates =
        static_cast<double>(ref_candidates) - static_cast<double>(rl_candidates);
    double reward = 0.7 * reward_nodes /
                        static_cast<double>(std::max<std::size_t>(1, ref_nodes)) +
                    0.3 * reward_candidates /
                        static_cast<double>(
                            std::max<std::size_t>(1, ref_candidates));
    reward = std::max(-1.0, std::min(1.0, reward));
    rl_tree_.agent().update(reward);
  }

  Config config_;
  std::vector<unsigned char> alive_;
  RTreeLite rl_tree_;
  RTreeLite ref_tree_;
  std::vector<Box2D> train_queries_;
  std::size_t insert_since_reward_ = 0;
  std::size_t deleted_count_ = 0;
};

}  // namespace rlr_lite
