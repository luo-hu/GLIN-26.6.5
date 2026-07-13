#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

namespace hire_sfc_lite {
namespace bulk_loading {

struct OptimizationStats {
  std::size_t boundaries_considered = 0;
  std::size_t boundaries_shifted = 0;
  std::size_t candidate_evaluations = 0;
  std::size_t rls_updates = 0;
  std::size_t max_shift = 0;

  void add(const OptimizationStats& other) {
    boundaries_considered += other.boundaries_considered;
    boundaries_shifted += other.boundaries_shifted;
    candidate_evaluations += other.candidate_evaluations;
    rls_updates += other.rls_updates;
    max_shift = std::max(max_shift, other.max_shift);
  }
};

class RecursiveLeastSquares {
 public:
  void fit(const std::vector<std::pair<double, double>>& samples) {
    double sx = 0.0;
    double sy = 0.0;
    double sxx = 0.0;
    double sxy = 0.0;
    for (const auto& sample : samples) {
      sx += sample.first;
      sy += sample.second;
      sxx += sample.first * sample.first;
      sxy += sample.first * sample.second;
    }
    const double n = static_cast<double>(samples.size());
    const double ridge = 1e-9;
    const double a00 = n + ridge;
    const double a01 = sx;
    const double a11 = sxx + ridge;
    const double determinant = a00 * a11 - a01 * a01;
    if (samples.empty()) {
      intercept_ = 0.0;
      slope_ = 0.0;
      p00_ = 1e9;
      p01_ = 0.0;
      p11_ = 1e9;
      return;
    }
    if (std::fabs(determinant) <= 1e-15) {
      intercept_ = sy / n;
      slope_ = 0.0;
      p00_ = 1.0 / a00;
      p01_ = 0.0;
      p11_ = 1e9;
      return;
    }
    p00_ = a11 / determinant;
    p01_ = -a01 / determinant;
    p11_ = a00 / determinant;
    intercept_ = p00_ * sy + p01_ * sxy;
    slope_ = p01_ * sy + p11_ * sxy;
  }

  double predict(double x) const { return intercept_ + slope_ * x; }

  void update(double x, double y) {
    const double px0 = p00_ + p01_ * x;
    const double px1 = p01_ + p11_ * x;
    const double denominator = 1.0 + px0 + x * px1;
    if (std::fabs(denominator) <= 1e-15) {
      return;
    }
    const double gain0 = px0 / denominator;
    const double gain1 = px1 / denominator;
    const double residual = y - predict(x);
    intercept_ += gain0 * residual;
    slope_ += gain1 * residual;

    const double old_p00 = p00_;
    const double old_p01 = p01_;
    const double old_p11 = p11_;
    p00_ = old_p00 - gain0 * (old_p00 + x * old_p01);
    p01_ = old_p01 - gain0 * (old_p01 + x * old_p11);
    p11_ = old_p11 - gain1 * (old_p01 + x * old_p11);
  }

 private:
  double intercept_ = 0.0;
  double slope_ = 0.0;
  double p00_ = 1e9;
  double p01_ = 0.0;
  double p11_ = 1e9;
};

template <typename KeyAt, typename ValidPartition>
std::vector<std::size_t> optimize_partition_ends(
    std::size_t item_count,
    const std::vector<std::size_t>& initial_ends,
    std::size_t parent_capacity,
    std::size_t parent_slot_count,
    std::size_t seed_count,
    std::size_t delta,
    KeyAt key_at,
    ValidPartition valid_partition,
    OptimizationStats& stats) {
  std::vector<std::size_t> ends = initial_ends;
  if (item_count == 0 || ends.size() < 2 || delta == 0) {
    return ends;
  }
  parent_capacity = std::max<std::size_t>(2, parent_capacity);
  parent_slot_count = std::max<std::size_t>(2, parent_slot_count);
  seed_count = std::max<std::size_t>(2, seed_count);

  for (std::size_t group_begin = 0; group_begin < ends.size();
       group_begin += parent_capacity) {
    const std::size_t group_end =
        std::min(ends.size(), group_begin + parent_capacity);
    const std::size_t group_size = group_end - group_begin;
    if (group_size <= 2) {
      continue;
    }
    const std::size_t seeds = std::min(seed_count, group_size);
    const double first_key = key_at(ends[group_begin] - 1);
    const double last_key = key_at(ends[group_end - 1] - 1);
    const double key_scale = std::max(1.0, std::fabs(last_key - first_key));
    const auto normalized_key = [&](std::size_t end) {
      return (key_at(end - 1) - first_key) / key_scale;
    };
    const auto target_rank = [&](std::size_t partition_index) {
      const std::size_t local = partition_index - group_begin;
      return static_cast<double>(local * (parent_slot_count - 1)) /
             static_cast<double>(group_size - 1);
    };

    std::vector<std::pair<double, double>> samples;
    samples.reserve(seeds);
    for (std::size_t index = group_begin;
         index < group_begin + seeds; ++index) {
      samples.emplace_back(normalized_key(ends[index]), target_rank(index));
    }
    RecursiveLeastSquares model;
    model.fit(samples);

    for (std::size_t index = group_begin + seeds; index < group_end;
         ++index) {
      const std::size_t original = ends[index];
      if (original == item_count) {
        model.update(normalized_key(original), target_rank(index));
        ++stats.rls_updates;
        continue;
      }
      const std::size_t previous = ends[index - 1];
      const std::size_t next = ends[index + 1];
      const std::size_t lower =
          std::max(previous + 1, original > delta ? original - delta : 1);
      const std::size_t upper = std::min(next - 1, original + delta);
      std::size_t chosen = original;
      double best_distance = std::numeric_limits<double>::infinity();
      std::size_t best_shift = std::numeric_limits<std::size_t>::max();
      ++stats.boundaries_considered;
      for (std::size_t candidate = lower; candidate <= upper; ++candidate) {
        if (!valid_partition(previous, candidate) ||
            !valid_partition(candidate, next)) {
          continue;
        }
        ++stats.candidate_evaluations;
        const double distance = std::fabs(
            model.predict(normalized_key(candidate)) - target_rank(index));
        const std::size_t shift = candidate > original
                                      ? candidate - original
                                      : original - candidate;
        if (distance < best_distance ||
            (distance == best_distance && shift < best_shift)) {
          best_distance = distance;
          best_shift = shift;
          chosen = candidate;
        }
      }
      ends[index] = chosen;
      if (chosen != original) {
        ++stats.boundaries_shifted;
        stats.max_shift = std::max(stats.max_shift, best_shift);
      }
      model.update(normalized_key(chosen), target_rank(index));
      ++stats.rls_updates;
    }
  }
  return ends;
}

}  // namespace bulk_loading
}  // namespace hire_sfc_lite
