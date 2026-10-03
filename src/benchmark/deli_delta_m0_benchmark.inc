// M0 Delta Structure Attribution. Included inside bench_dynamic_compare_wkt's
// anonymous namespace so it can reuse the production DELI SingleStore overlay.

struct M0EntryRef {
  ObjectId logical_id = 0;
  std::uint32_t version = 0;
  bool valid = false;
};

struct M0ObjectState {
  std::uint32_t current_version = 0;
  ObjectId current_physical_id = 0;
  bool live = false;
  bool in_delta = false;
};

struct M0DeltaQueryStats {
  std::size_t records_scanned = 0;
  std::size_t block_checks = 0;
  std::size_t visited_blocks = 0;
  std::size_t candidates = 0;
};

class M0DeltaIndex {
 public:
  virtual ~M0DeltaIndex() = default;
  virtual bool insert(ObjectId physical_id) = 0;
  virtual bool erase(ObjectId physical_id) = 0;
  virtual void query_candidates(const QueryCase& query,
                                std::vector<ObjectId>* output,
                                M0DeltaQueryStats& stats) const = 0;
  virtual void enumerate_entries(std::vector<ObjectId>& output) const = 0;
  virtual std::size_t memory_bytes() const = 0;
  virtual bool validate(std::string* message) const = 0;
  virtual std::size_t local_compaction_count() const { return 0; }
  virtual long long local_compaction_ns() const { return 0; }
};

class M0VectorLogDelta final : public M0DeltaIndex {
 public:
  explicit M0VectorLogDelta(const std::vector<GeometryMeta>& metadata)
      : metadata_(metadata) {}

  bool insert(ObjectId physical_id) override {
    if (physical_id >= metadata_.size()) {
      return false;
    }
    entries_.push_back(physical_id);
    return true;
  }

  bool erase(ObjectId) override { return true; }

  void query_candidates(const QueryCase& query,
                        std::vector<ObjectId>* output,
                        M0DeltaQueryStats& stats) const override {
    const geos::geom::Envelope envelope =
        *query.geometry->getEnvelopeInternal();
    for (ObjectId physical_id : entries_) {
      ++stats.records_scanned;
      if (!envelopes_intersect(metadata_[physical_id].envelope, envelope)) {
        continue;
      }
      ++stats.candidates;
      if (output != nullptr) {
        output->push_back(physical_id);
      }
    }
  }

  void enumerate_entries(std::vector<ObjectId>& output) const override {
    output.insert(output.end(), entries_.begin(), entries_.end());
  }

  std::size_t memory_bytes() const override {
    return entries_.capacity() * sizeof(ObjectId);
  }

  bool validate(std::string*) const override { return true; }

 private:
  const std::vector<GeometryMeta>& metadata_;
  std::vector<ObjectId> entries_;
};

class M0BoostDelta final : public M0DeltaIndex {
 public:
  M0BoostDelta(const std::vector<GeometryMeta>& metadata, bool eager_delete)
      : metadata_(metadata), eager_delete_(eager_delete) {}

  bool insert(ObjectId physical_id) override {
    if (physical_id >= metadata_.size()) {
      return false;
    }
    tree_.insert(std::make_pair(
        boost_box_from_envelope(metadata_[physical_id].envelope), physical_id));
    ++physical_entries_;
    return true;
  }

  bool erase(ObjectId physical_id) override {
    if (!eager_delete_) {
      return true;
    }
    if (physical_id >= metadata_.size()) {
      return false;
    }
    const std::size_t removed = tree_.remove(std::make_pair(
        boost_box_from_envelope(metadata_[physical_id].envelope), physical_id));
    if (removed > 0 && physical_entries_ > 0) {
      --physical_entries_;
    }
    return removed > 0;
  }

  void query_candidates(const QueryCase& query,
                        std::vector<ObjectId>* output,
                        M0DeltaQueryStats& stats) const override {
    const BoostBox box = boost_box_from_envelope(
        *query.geometry->getEnvelopeInternal());
    const auto predicates = bgi::intersects(box);
    for (auto it = tree_.qbegin(predicates); it != tree_.qend(); ++it) {
      ++stats.records_scanned;
      ++stats.candidates;
      if (output != nullptr) {
        output->push_back(it->second);
      }
    }
  }

  void enumerate_entries(std::vector<ObjectId>& output) const override {
    output.reserve(output.size() + tree_.size());
    for (const RTreeValue& value : tree_) {
      output.push_back(value.second);
    }
  }

  std::size_t memory_bytes() const override {
    return physical_entries_ * sizeof(RTreeValue) * 2;
  }

  bool validate(std::string*) const override {
    return tree_.size() == physical_entries_;
  }

 private:
  const std::vector<GeometryMeta>& metadata_;
  bool eager_delete_ = false;
  RTree tree_;
  std::size_t physical_entries_ = 0;
};

class M0DeliSingleStoreDelta final : public M0DeltaIndex {
 public:
  M0DeliSingleStoreDelta(const Options& options,
                         const std::vector<GeometryPtr>& geometries,
                         const std::vector<GeometryMeta>& metadata,
                         const std::vector<QueryCase>& queries)
      : index_(options, geometries, metadata, true, &queries, true, false) {
    index_.bulk_load(std::vector<ObjectId>());
  }

  bool insert(ObjectId physical_id) override {
    return index_.insert(physical_id);
  }

  bool erase(ObjectId physical_id) override {
    return index_.erase(physical_id);
  }

  void query_candidates(const QueryCase& query,
                        std::vector<ObjectId>* output,
                        M0DeltaQueryStats& stats) const override {
    index_.collect_candidates(query, output, stats.block_checks,
                              stats.visited_blocks, stats.records_scanned,
                              stats.candidates);
  }

  void enumerate_entries(std::vector<ObjectId>& output) const override {
    index_.enumerate_live_ids(output);
  }

  std::size_t memory_bytes() const override {
    return index_.index_bytes_estimate();
  }

  bool validate(std::string* message) const override {
    return index_.validate_index(message);
  }

  std::size_t local_compaction_count() const override {
    return index_.local_compaction_count();
  }

  long long local_compaction_ns() const override {
    return index_.local_compaction_ns();
  }

 private:
  DeliAlexHybridLocalBoundedIndex index_;
};

struct M0QueryScratch {
  std::vector<ObjectId> base_candidates;
  std::vector<ObjectId> delta_candidates;
  std::vector<ObjectId> unique_physical_ids;
  std::vector<ObjectId> answer_logical_ids;
  std::vector<std::uint32_t> seen_epochs;
  std::uint32_t epoch = 0;
};

struct M0QueryMeasurement {
  long long total_ns = 0;
  long long base_query_ns = 0;
  long long delta_query_ns = 0;
  long long union_ns = 0;
  long long refinement_ns = 0;
  long long geos_ns = 0;
  std::size_t base_candidates = 0;
  std::size_t delta_candidates = 0;
  std::size_t unique_candidates = 0;
  std::size_t stale_candidates = 0;
  std::size_t duplicate_candidates = 0;
  std::size_t exact_calls = 0;
  std::size_t predicate_shortcuts = 0;
  std::size_t delta_records_scanned = 0;
  std::size_t delta_block_checks = 0;
  std::size_t delta_visited_blocks = 0;
};

struct M0SummaryRow {
  std::string dataset;
  std::string variant;
  std::string selectivity_tag;
  std::size_t repeat_id = 0;
  std::uint64_t seed = 0;
  std::size_t loaded_count = 0;
  std::size_t base_count = 0;
  std::size_t live_delta_count = 0;
  std::size_t live_count = 0;
  double requested_delta_ratio = 0.0;
  double actual_delta_ratio = 0.0;
  double churn_fraction = 0.0;
  std::size_t insert_count = 0;
  std::size_t delete_count = 0;
  std::size_t replace_count = 0;
  std::size_t query_count = 0;
  std::size_t query_repetitions = 0;
  long long base_build_ns = 0;
  long long delta_init_ns = 0;
  long long avg_insert_ns = 0;
  long long p95_insert_ns = 0;
  long long avg_delete_ns = 0;
  long long p95_delete_ns = 0;
  long long avg_replace_ns = 0;
  long long p95_replace_ns = 0;
  long long avg_delta_count_only_ns = 0;
  long long avg_total_query_ns = 0;
  long long p95_total_query_ns = 0;
  long long p99_total_query_ns = 0;
  long long avg_base_query_ns = 0;
  long long avg_delta_query_ns = 0;
  long long avg_union_ns = 0;
  long long avg_refinement_ns = 0;
  long long avg_geos_ns = 0;
  double avg_base_candidates = 0.0;
  double avg_delta_candidates = 0.0;
  double avg_unique_candidates = 0.0;
  double avg_stale_candidates = 0.0;
  double avg_duplicate_candidates = 0.0;
  double avg_exact_calls = 0.0;
  double avg_predicate_shortcuts = 0.0;
  double avg_delta_records_scanned = 0.0;
  double avg_delta_block_checks = 0.0;
  double avg_delta_visited_blocks = 0.0;
  long long delta_enumeration_ns = 0;
  long long full_rebuild_ns = 0;
  long long state_publish_ns = 0;
  std::size_t enumerated_physical_entries = 0;
  std::size_t enumerated_live_entries = 0;
  std::size_t base_bytes_estimate = 0;
  std::size_t delta_bytes_estimate = 0;
  std::size_t shared_state_bytes = 0;
  std::size_t total_bytes_estimate = 0;
  std::size_t peak_rebuild_bytes_estimate = 0;
  std::size_t local_compaction_count = 0;
  long long local_compaction_ns = 0;
  int validate_ok = 0;
  int correctness_checked = 0;
  std::size_t correctness_mismatches = 0;
};

long long m0_average_ns(const std::vector<long long>& values) {
  if (values.empty()) {
    return 0;
  }
  const long long total =
      std::accumulate(values.begin(), values.end(), 0LL);
  return total / static_cast<long long>(values.size());
}

bool m0_entry_is_current(ObjectId physical_id,
                         const std::vector<M0EntryRef>& refs,
                         const std::vector<M0ObjectState>& states) {
  if (physical_id >= refs.size() || !refs[physical_id].valid) {
    return false;
  }
  const M0EntryRef& ref = refs[physical_id];
  if (ref.logical_id >= states.size()) {
    return false;
  }
  const M0ObjectState& state = states[ref.logical_id];
  return state.live && state.current_version == ref.version &&
         state.current_physical_id == physical_id;
}

M0QueryMeasurement m0_execute_query(
    const Options& options, const QueryCase& query, const RTree& base,
    const M0DeltaIndex& delta, const std::vector<GeometryPtr>& geometries,
    const std::vector<GeometryMeta>& metadata,
    const std::vector<M0EntryRef>& refs,
    const std::vector<M0ObjectState>& states, M0QueryScratch& scratch) {
  M0QueryMeasurement measurement;
  scratch.base_candidates.clear();
  scratch.delta_candidates.clear();
  scratch.unique_physical_ids.clear();
  scratch.answer_logical_ids.clear();
  if (++scratch.epoch == 0) {
    std::fill(scratch.seen_epochs.begin(), scratch.seen_epochs.end(), 0);
    ++scratch.epoch;
  }

  const auto total_start = std::chrono::high_resolution_clock::now();
  const geos::geom::Envelope query_envelope =
      *query.geometry->getEnvelopeInternal();

  const auto base_start = std::chrono::high_resolution_clock::now();
  std::vector<RTreeValue> base_hits;
  base.query(bgi::intersects(boost_box_from_envelope(query_envelope)),
             std::back_inserter(base_hits));
  scratch.base_candidates.reserve(base_hits.size());
  for (const RTreeValue& hit : base_hits) {
    scratch.base_candidates.push_back(hit.second);
  }
  const auto base_end = std::chrono::high_resolution_clock::now();
  measurement.base_query_ns = ns_count(base_end - base_start);
  measurement.base_candidates = scratch.base_candidates.size();

  M0DeltaQueryStats delta_stats;
  const auto delta_start = std::chrono::high_resolution_clock::now();
  delta.query_candidates(query, &scratch.delta_candidates, delta_stats);
  const auto delta_end = std::chrono::high_resolution_clock::now();
  measurement.delta_query_ns = ns_count(delta_end - delta_start);
  measurement.delta_candidates = scratch.delta_candidates.size();
  measurement.delta_records_scanned = delta_stats.records_scanned;
  measurement.delta_block_checks = delta_stats.block_checks;
  measurement.delta_visited_blocks = delta_stats.visited_blocks;

  const auto union_start = std::chrono::high_resolution_clock::now();
  auto accept_candidate = [&](ObjectId physical_id) {
    if (!m0_entry_is_current(physical_id, refs, states)) {
      ++measurement.stale_candidates;
      return;
    }
    const ObjectId logical_id = refs[physical_id].logical_id;
    if (logical_id >= scratch.seen_epochs.size()) {
      throw std::runtime_error("M0 logical object id exceeds seen table");
    }
    if (scratch.seen_epochs[logical_id] == scratch.epoch) {
      ++measurement.duplicate_candidates;
      return;
    }
    scratch.seen_epochs[logical_id] = scratch.epoch;
    scratch.unique_physical_ids.push_back(physical_id);
  };
  for (ObjectId physical_id : scratch.base_candidates) {
    accept_candidate(physical_id);
  }
  for (ObjectId physical_id : scratch.delta_candidates) {
    accept_candidate(physical_id);
  }
  const auto union_end = std::chrono::high_resolution_clock::now();
  measurement.union_ns = ns_count(union_end - union_start);
  measurement.unique_candidates = scratch.unique_physical_ids.size();

  const auto refinement_start = std::chrono::high_resolution_clock::now();
  for (ObjectId physical_id : scratch.unique_physical_ids) {
    const M0EntryRef& ref = refs[physical_id];
    const GeometryMeta& meta = metadata[physical_id];
    if (!envelopes_intersect(meta.envelope, query_envelope)) {
      continue;
    }
    std::size_t envelope_shortcuts = 0;
    std::size_t representative_shortcuts = 0;
    if (account_rectangle_shortcut(
            rectangle_query_candidate_shortcut(
                options, options.predicate_shortcuts, query_envelope,
                meta.envelope, geometries[physical_id].get()),
            measurement.predicate_shortcuts, envelope_shortcuts,
            representative_shortcuts)) {
      scratch.answer_logical_ids.push_back(ref.logical_id);
      continue;
    }
    if (timed_geos_intersects(query.geometry, geometries[physical_id].get(),
                              measurement.exact_calls,
                              measurement.geos_ns)) {
      scratch.answer_logical_ids.push_back(ref.logical_id);
    }
  }
  const auto refinement_end = std::chrono::high_resolution_clock::now();
  measurement.refinement_ns = ns_count(refinement_end - refinement_start);
  measurement.total_ns = ns_count(refinement_end - total_start);
  return measurement;
}

std::unordered_set<ObjectId> m0_oracle_answers(
    const QueryCase& query, const RTree& oracle,
    const std::vector<GeometryPtr>& geometries,
    const std::vector<M0EntryRef>& refs) {
  std::vector<RTreeValue> candidates;
  oracle.query(bgi::intersects(boost_box_from_envelope(
                   *query.geometry->getEnvelopeInternal())),
               std::back_inserter(candidates));
  std::unordered_set<ObjectId> answers;
  for (const RTreeValue& candidate : candidates) {
    const ObjectId physical_id = candidate.second;
    if (query.geometry->intersects(geometries[physical_id].get())) {
      answers.insert(refs[physical_id].logical_id);
    }
  }
  return answers;
}

void write_m0_csv(const std::string& path, const M0SummaryRow& row) {
  if (path.empty()) {
    return;
  }
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("Cannot write M0 CSV: " + path);
  }
  output
      << "dataset,variant,selectivity_tag,repeat_id,seed,loaded_count,base_count,"
         "live_delta_count,live_count,requested_delta_ratio,actual_delta_ratio,"
         "churn_fraction,insert_count,delete_count,replace_count,query_count,"
         "query_repetitions,base_build_ns,delta_init_ns,avg_insert_ns,"
         "p95_insert_ns,avg_delete_ns,p95_delete_ns,avg_replace_ns,"
         "p95_replace_ns,avg_delta_count_only_ns,avg_total_query_ns,"
         "p95_total_query_ns,p99_total_query_ns,avg_base_query_ns,"
         "avg_delta_query_ns,avg_union_ns,avg_refinement_ns,avg_geos_ns,"
         "avg_base_candidates,avg_delta_candidates,avg_unique_candidates,"
         "avg_stale_candidates,avg_duplicate_candidates,avg_exact_calls,"
         "avg_predicate_shortcuts,avg_delta_records_scanned,"
         "avg_delta_block_checks,avg_delta_visited_blocks,"
         "delta_enumeration_ns,full_rebuild_ns,state_publish_ns,"
         "enumerated_physical_entries,enumerated_live_entries,"
         "base_bytes_estimate,delta_bytes_estimate,shared_state_bytes,"
         "total_bytes_estimate,peak_rebuild_bytes_estimate,"
         "local_compaction_count,local_compaction_ns,validate_ok,"
         "correctness_checked,correctness_mismatches\n";
  output << std::setprecision(17) << row.dataset << "," << row.variant << ","
         << row.selectivity_tag << "," << row.repeat_id << "," << row.seed
         << "," << row.loaded_count << ","
         << row.base_count << "," << row.live_delta_count << ","
         << row.live_count << "," << row.requested_delta_ratio << ","
         << row.actual_delta_ratio << "," << row.churn_fraction << ","
         << row.insert_count << "," << row.delete_count << ","
         << row.replace_count << "," << row.query_count << ","
         << row.query_repetitions << "," << row.base_build_ns << ","
         << row.delta_init_ns << "," << row.avg_insert_ns << ","
         << row.p95_insert_ns << "," << row.avg_delete_ns << ","
         << row.p95_delete_ns << "," << row.avg_replace_ns << ","
         << row.p95_replace_ns << "," << row.avg_delta_count_only_ns << ","
         << row.avg_total_query_ns << "," << row.p95_total_query_ns << ","
         << row.p99_total_query_ns << "," << row.avg_base_query_ns << ","
         << row.avg_delta_query_ns << "," << row.avg_union_ns << ","
         << row.avg_refinement_ns << "," << row.avg_geos_ns << ","
         << row.avg_base_candidates << "," << row.avg_delta_candidates << ","
         << row.avg_unique_candidates << "," << row.avg_stale_candidates
         << "," << row.avg_duplicate_candidates << "," << row.avg_exact_calls
         << "," << row.avg_predicate_shortcuts << ","
         << row.avg_delta_records_scanned << "," << row.avg_delta_block_checks
         << "," << row.avg_delta_visited_blocks << ","
         << row.delta_enumeration_ns << "," << row.full_rebuild_ns << ","
         << row.state_publish_ns << "," << row.enumerated_physical_entries
         << "," << row.enumerated_live_entries << ","
         << row.base_bytes_estimate << "," << row.delta_bytes_estimate << ","
         << row.shared_state_bytes << "," << row.total_bytes_estimate << ","
         << row.peak_rebuild_bytes_estimate << ","
         << row.local_compaction_count << "," << row.local_compaction_ns << ","
         << row.validate_ok << "," << row.correctness_checked << ","
         << row.correctness_mismatches << "\n";
}

int run_deli_delta_m0(const Options& options,
                      const std::vector<GeometryPtr>& geometries,
                      const std::vector<GeometryMeta>& metadata,
                      const std::vector<QueryCase>& queries) {
  const std::size_t loaded = geometries.size();
  const std::size_t base_count = std::max<std::size_t>(
      1, static_cast<std::size_t>(
             std::floor(options.m0_base_fraction * loaded)));
  const std::size_t target_delta = static_cast<std::size_t>(std::ceil(
      options.m0_delta_ratio / (1.0 - options.m0_delta_ratio) * base_count));
  const std::size_t churn_count = static_cast<std::size_t>(
      std::floor(options.m0_churn_fraction * target_delta));
  if (base_count + target_delta + churn_count > loaded) {
    throw std::runtime_error(
        "M0 physical geometry pool is too small; reduce m0_base_fraction, "
        "m0_delta_ratio, or m0_churn_fraction");
  }

  std::vector<M0EntryRef> refs(loaded);
  std::vector<M0ObjectState> states(loaded);
  std::vector<RTreeValue> base_values;
  base_values.reserve(base_count);
  for (ObjectId physical_id = 0; physical_id < base_count; ++physical_id) {
    refs[physical_id] = M0EntryRef{physical_id, 1, true};
    states[physical_id] = M0ObjectState{1, physical_id, true, false};
    base_values.emplace_back(
        boost_box_from_envelope(metadata[physical_id].envelope), physical_id);
  }
  const auto base_start = std::chrono::high_resolution_clock::now();
  RTree base(base_values.begin(), base_values.end());
  const auto base_end = std::chrono::high_resolution_clock::now();

  const auto delta_init_start = std::chrono::high_resolution_clock::now();
  std::unique_ptr<M0DeltaIndex> delta;
  if (options.m0_variant == "vector_log") {
    delta.reset(new M0VectorLogDelta(metadata));
  } else if (options.m0_variant == "boost_lazy") {
    delta.reset(new M0BoostDelta(metadata, false));
  } else if (options.m0_variant == "boost_eager") {
    delta.reset(new M0BoostDelta(metadata, true));
  } else {
    delta.reset(
        new M0DeliSingleStoreDelta(options, geometries, metadata, queries));
  }
  const auto delta_init_end = std::chrono::high_resolution_clock::now();

  std::vector<long long> insert_latencies;
  std::vector<long long> delete_latencies;
  std::vector<long long> replace_latencies;
  std::vector<ObjectId> live_delta_logicals;
  live_delta_logicals.reserve(target_delta);
  ObjectId next_physical = base_count;
  ObjectId next_logical = base_count;
  auto add_new_logical = [&]() {
    const ObjectId physical_id = next_physical++;
    const ObjectId logical_id = next_logical++;
    refs[physical_id] = M0EntryRef{logical_id, 1, true};
    states[logical_id] = M0ObjectState{1, physical_id, true, true};
    const auto start = std::chrono::high_resolution_clock::now();
    const bool ok = delta->insert(physical_id);
    const auto end = std::chrono::high_resolution_clock::now();
    if (!ok) {
      throw std::runtime_error("M0 Delta insert failed");
    }
    insert_latencies.push_back(ns_count(end - start));
    live_delta_logicals.push_back(logical_id);
  };
  for (std::size_t i = 0; i < target_delta; ++i) {
    add_new_logical();
  }

  std::mt19937_64 rng(options.seed);
  std::shuffle(live_delta_logicals.begin(), live_delta_logicals.end(), rng);
  const std::size_t replace_count = churn_count / 2;
  const std::size_t delete_insert_count = churn_count - replace_count;
  for (std::size_t i = 0; i < replace_count; ++i) {
    const ObjectId logical_id = live_delta_logicals[i];
    M0ObjectState& state = states[logical_id];
    const ObjectId old_physical = state.current_physical_id;
    const ObjectId new_physical = next_physical++;
    const std::uint32_t new_version = state.current_version + 1;
    refs[new_physical] = M0EntryRef{logical_id, new_version, true};
    const auto start = std::chrono::high_resolution_clock::now();
    const bool erased = delta->erase(old_physical);
    state.current_version = new_version;
    state.current_physical_id = new_physical;
    const bool inserted = delta->insert(new_physical);
    const auto end = std::chrono::high_resolution_clock::now();
    if (!erased || !inserted) {
      throw std::runtime_error("M0 Delta replace failed");
    }
    replace_latencies.push_back(ns_count(end - start));
  }
  for (std::size_t i = 0; i < delete_insert_count; ++i) {
    const std::size_t offset = replace_count + i;
    const ObjectId logical_id = live_delta_logicals[offset];
    M0ObjectState& state = states[logical_id];
    const ObjectId old_physical = state.current_physical_id;
    const auto delete_start = std::chrono::high_resolution_clock::now();
    const bool erased = delta->erase(old_physical);
    const auto delete_end = std::chrono::high_resolution_clock::now();
    if (!erased) {
      throw std::runtime_error("M0 Delta delete failed");
    }
    state.current_version += 1;
    state.live = false;
    delete_latencies.push_back(ns_count(delete_end - delete_start));

    const ObjectId new_physical = next_physical++;
    const ObjectId new_logical = next_logical++;
    refs[new_physical] = M0EntryRef{new_logical, 1, true};
    states[new_logical] = M0ObjectState{1, new_physical, true, true};
    const auto insert_start = std::chrono::high_resolution_clock::now();
    const bool inserted = delta->insert(new_physical);
    const auto insert_end = std::chrono::high_resolution_clock::now();
    if (!inserted) {
      throw std::runtime_error("M0 replacement insert failed");
    }
    insert_latencies.push_back(ns_count(insert_end - insert_start));
    live_delta_logicals[offset] = new_logical;
  }

  std::string validation_message;
  const bool validate_ok = delta->validate(&validation_message);
  if (!validate_ok) {
    throw std::runtime_error("M0 Delta validation failed: " +
                             validation_message);
  }

  std::vector<RTreeValue> current_values;
  current_values.reserve(base_count + target_delta);
  for (const M0ObjectState& state : states) {
    if (!state.live) {
      continue;
    }
    current_values.emplace_back(
        boost_box_from_envelope(metadata[state.current_physical_id].envelope),
        state.current_physical_id);
  }
  RTree oracle(current_values.begin(), current_values.end());

  M0QueryScratch scratch;
  scratch.seen_epochs.resize(states.size(), 0);
  const std::size_t warmup = std::min(options.m0_warmup_queries, queries.size());
  for (std::size_t i = 0; i < warmup; ++i) {
    m0_execute_query(options, queries[i], base, *delta, geometries, metadata,
                     refs, states, scratch);
  }

  std::vector<long long> count_only_latencies;
  count_only_latencies.reserve(queries.size());
  for (const QueryCase& query : queries) {
    M0DeltaQueryStats count_stats;
    const auto start = std::chrono::high_resolution_clock::now();
    delta->query_candidates(query, nullptr, count_stats);
    const auto end = std::chrono::high_resolution_clock::now();
    count_only_latencies.push_back(ns_count(end - start));
  }

  std::size_t correctness_mismatches = 0;
  if (options.check_correctness) {
    for (const QueryCase& query : queries) {
      const M0QueryMeasurement measured = m0_execute_query(
          options, query, base, *delta, geometries, metadata, refs, states,
          scratch);
      (void)measured;
      const std::unordered_set<ObjectId> expected =
          m0_oracle_answers(query, oracle, geometries, refs);
      const std::unordered_set<ObjectId> actual(
          scratch.answer_logical_ids.begin(), scratch.answer_logical_ids.end());
      if (expected != actual) {
        ++correctness_mismatches;
        if (options.stop_on_mismatch) {
          throw std::runtime_error("M0 candidate correctness mismatch");
        }
      }
    }
  }

  std::vector<long long> total_latencies;
  long long sum_base_ns = 0;
  long long sum_delta_ns = 0;
  long long sum_union_ns = 0;
  long long sum_refinement_ns = 0;
  long long sum_geos_ns = 0;
  std::size_t sum_base_candidates = 0;
  std::size_t sum_delta_candidates = 0;
  std::size_t sum_unique_candidates = 0;
  std::size_t sum_stale_candidates = 0;
  std::size_t sum_duplicate_candidates = 0;
  std::size_t sum_exact_calls = 0;
  std::size_t sum_predicate_shortcuts = 0;
  std::size_t sum_delta_records_scanned = 0;
  std::size_t sum_delta_block_checks = 0;
  std::size_t sum_delta_visited_blocks = 0;
  for (std::size_t repetition = 0;
       repetition < options.m0_query_repetitions; ++repetition) {
    for (const QueryCase& query : queries) {
      const M0QueryMeasurement measured = m0_execute_query(
          options, query, base, *delta, geometries, metadata, refs, states,
          scratch);
      total_latencies.push_back(measured.total_ns);
      sum_base_ns += measured.base_query_ns;
      sum_delta_ns += measured.delta_query_ns;
      sum_union_ns += measured.union_ns;
      sum_refinement_ns += measured.refinement_ns;
      sum_geos_ns += measured.geos_ns;
      sum_base_candidates += measured.base_candidates;
      sum_delta_candidates += measured.delta_candidates;
      sum_unique_candidates += measured.unique_candidates;
      sum_stale_candidates += measured.stale_candidates;
      sum_duplicate_candidates += measured.duplicate_candidates;
      sum_exact_calls += measured.exact_calls;
      sum_predicate_shortcuts += measured.predicate_shortcuts;
      sum_delta_records_scanned += measured.delta_records_scanned;
      sum_delta_block_checks += measured.delta_block_checks;
      sum_delta_visited_blocks += measured.delta_visited_blocks;
    }
  }

  std::vector<ObjectId> enumerated;
  const auto enumerate_start = std::chrono::high_resolution_clock::now();
  delta->enumerate_entries(enumerated);
  const auto enumerate_end = std::chrono::high_resolution_clock::now();
  std::size_t enumerated_live = 0;
  for (ObjectId physical_id : enumerated) {
    if (m0_entry_is_current(physical_id, refs, states)) {
      ++enumerated_live;
    }
  }

  const auto rebuild_start = std::chrono::high_resolution_clock::now();
  std::unique_ptr<RTree> rebuilt(
      new RTree(current_values.begin(), current_values.end()));
  const auto rebuild_end = std::chrono::high_resolution_clock::now();
  std::unique_ptr<RTree> published;
  const auto publish_start = std::chrono::high_resolution_clock::now();
  published.swap(rebuilt);
  const auto publish_end = std::chrono::high_resolution_clock::now();

  const std::size_t timed_queries = total_latencies.size();
  const double denom = static_cast<double>(std::max<std::size_t>(1, timed_queries));
  M0SummaryRow row;
  row.dataset = options.dataset_name;
  row.variant = options.m0_variant;
  row.selectivity_tag = options.m0_selectivity_tag;
  row.repeat_id = options.m0_repeat_id;
  row.seed = options.seed;
  row.loaded_count = loaded;
  row.base_count = base_count;
  row.live_delta_count = target_delta;
  row.live_count = base_count + target_delta;
  row.requested_delta_ratio = options.m0_delta_ratio;
  row.actual_delta_ratio = static_cast<double>(target_delta) /
                           static_cast<double>(row.live_count);
  row.churn_fraction = options.m0_churn_fraction;
  row.insert_count = insert_latencies.size();
  row.delete_count = delete_latencies.size();
  row.replace_count = replace_latencies.size();
  row.query_count = queries.size();
  row.query_repetitions = options.m0_query_repetitions;
  row.base_build_ns = ns_count(base_end - base_start);
  row.delta_init_ns = ns_count(delta_init_end - delta_init_start);
  row.avg_insert_ns = m0_average_ns(insert_latencies);
  row.p95_insert_ns = percentile_value(insert_latencies, 0.95);
  row.avg_delete_ns = m0_average_ns(delete_latencies);
  row.p95_delete_ns = percentile_value(delete_latencies, 0.95);
  row.avg_replace_ns = m0_average_ns(replace_latencies);
  row.p95_replace_ns = percentile_value(replace_latencies, 0.95);
  row.avg_delta_count_only_ns = m0_average_ns(count_only_latencies);
  row.avg_total_query_ns = m0_average_ns(total_latencies);
  row.p95_total_query_ns = percentile_value(total_latencies, 0.95);
  row.p99_total_query_ns = percentile_value(total_latencies, 0.99);
  row.avg_base_query_ns = static_cast<long long>(sum_base_ns / denom);
  row.avg_delta_query_ns = static_cast<long long>(sum_delta_ns / denom);
  row.avg_union_ns = static_cast<long long>(sum_union_ns / denom);
  row.avg_refinement_ns =
      static_cast<long long>(sum_refinement_ns / denom);
  row.avg_geos_ns = static_cast<long long>(sum_geos_ns / denom);
  row.avg_base_candidates = sum_base_candidates / denom;
  row.avg_delta_candidates = sum_delta_candidates / denom;
  row.avg_unique_candidates = sum_unique_candidates / denom;
  row.avg_stale_candidates = sum_stale_candidates / denom;
  row.avg_duplicate_candidates = sum_duplicate_candidates / denom;
  row.avg_exact_calls = sum_exact_calls / denom;
  row.avg_predicate_shortcuts = sum_predicate_shortcuts / denom;
  row.avg_delta_records_scanned = sum_delta_records_scanned / denom;
  row.avg_delta_block_checks = sum_delta_block_checks / denom;
  row.avg_delta_visited_blocks = sum_delta_visited_blocks / denom;
  row.delta_enumeration_ns = ns_count(enumerate_end - enumerate_start);
  row.full_rebuild_ns = ns_count(rebuild_end - rebuild_start);
  row.state_publish_ns = ns_count(publish_end - publish_start);
  row.enumerated_physical_entries = enumerated.size();
  row.enumerated_live_entries = enumerated_live;
  row.base_bytes_estimate = base_values.size() * sizeof(RTreeValue) * 2;
  row.delta_bytes_estimate = delta->memory_bytes();
  row.shared_state_bytes = refs.capacity() * sizeof(M0EntryRef) +
                           states.capacity() * sizeof(M0ObjectState);
  row.total_bytes_estimate = row.base_bytes_estimate +
                             row.delta_bytes_estimate + row.shared_state_bytes;
  row.peak_rebuild_bytes_estimate =
      row.total_bytes_estimate + current_values.size() * sizeof(RTreeValue) * 2;
  row.local_compaction_count = delta->local_compaction_count();
  row.local_compaction_ns = delta->local_compaction_ns();
  row.validate_ok = validate_ok ? 1 : 0;
  row.correctness_checked = options.check_correctness ? 1 : 0;
  row.correctness_mismatches = correctness_mismatches;
  write_m0_csv(options.output_csv, row);

  std::cout << "M0 dataset=" << row.dataset << " variant=" << row.variant
            << " delta_ratio=" << row.actual_delta_ratio
            << " query_us=" << row.avg_total_query_ns / 1000.0
            << " insert_us=" << row.avg_insert_ns / 1000.0
            << " delete_us=" << row.avg_delete_ns / 1000.0
            << " replace_us=" << row.avg_replace_ns / 1000.0
            << " delta_mb=" << row.delta_bytes_estimate / (1024.0 * 1024.0)
            << " mismatch=" << row.correctness_mismatches << "\n";
  if (!options.output_csv.empty()) {
    std::cout << "CSV: " << options.output_csv << "\n";
  }
  return correctness_mismatches == 0 ? 0 : 2;
}
