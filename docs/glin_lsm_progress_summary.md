# GLIN Dynamic Update Improvement Summary

Last updated: 2026-06-08

This document records what has been added around GLIN dynamic updates, how to
run the experiments, what the current results mean, and which variants are
usable for the paper.

## 1. Research Question

Original GLIN is weak under random dynamic insertion because its underlying
ALEX-like learned index stores ordered arrays with gaps. Random arrivals cause
data movement, node resizing, split/retrain, and leaf MBR updates. However,
when data are inserted in Zmin order, the update path becomes close to append or
near-append, and GLIN can outperform Boost R-tree.

The current research question is:

> Can we preserve GLIN's advantage on ordered Z-order keys while making random
> dynamic updates practical through buffering, delta indexes, segmentation, and
> background maintenance?

## 2. Files Changed or Added

Main benchmark code:

- `src/benchmark/bench_update_wkt.cpp`
  - Added insert-only variants:
    - `GLIN_BUFFERED`
    - `GLIN_LSM`
    - `GLIN_LSM_ASYNC`
  - Added update metrics:
    - `maintenance_ns`
    - `merge_count`
    - `delta_pending`
    - `buffer_size`
    - `delta_size`

- `src/benchmark/bench_hybrid_wkt.cpp`
  - Added hybrid workload variants:
    - `GLIN_LSM_ASYNC`
    - `GLIN_LSM_SEGMENTED`
    - `GLIN_LSM_SEGMENTED4`
    - `GLIN_LSM_BG`
  - Added hybrid metrics:
    - `maintenance_ns`
    - `drain_ns`
    - `total_elapsed_ns`
    - `throughput_transactions_per_sec`
    - `total_throughput_transactions_per_sec`
    - `candidate_answer_ratio`
    - `answers_match_boost`
    - `answers_delta_vs_boost`
    - `delta_pending`
    - `segment_count`
    - `segments_pruned_total`

Scripts:

- `scripts/run_fig15_insert_diagnostics.sh`
  - Supports:
    - `INCLUDE_BUFFERED_GLIN`
    - `BUFFER_SIZE`
    - `INCLUDE_LSM_GLIN`
    - `INCLUDE_LSM_ASYNC_GLIN`
    - `DELTA_SIZE`

- `scripts/run_fig17_hybrid_1m.sh`
  - Supports:
    - `INCLUDE_LSM_ASYNC_GLIN`
    - `INCLUDE_LSM_SEGMENTED_GLIN`
    - `INCLUDE_LSM_SEGMENTED4_GLIN`
    - `INCLUDE_LSM_BG_GLIN`
    - `DELTA_SIZE`

- `scripts/plot_fig17_hybrid.py`
  - Added progress panels:
    - `fig17_hybrid_delta_pending_panel.png`
    - `fig17_hybrid_segment_count_panel.png`
    - `fig17_hybrid_maintenance_ns_panel.png`
  - Added a beginner-friendly diagnostic text file:
    - `fig17_hybrid_summary_diagnostics.txt`

## 3. Variants Implemented

### 3.1 GLIN_BUFFERED

Location:

- `src/benchmark/bench_update_wkt.cpp`

Idea:

Random inserts are buffered in micro-batches. Each batch is sorted by Zmin
before insertion into GLIN.

Path:

```text
random arrival -> buffer -> sort by Zmin -> GLIN insert
```

Parameters:

- `--include_buffered_glin 1`
- `--buffer_size N`

Strength:

- Simple and easy to explain.
- Does not rebuild the whole index.
- Directly tests the hypothesis that GLIN benefits from sorted update order.

Weakness:

- Only implemented for insert diagnostics, not as a full hybrid query/update
  system.
- Sacrifices real-time insertion latency.
- In current results it is not clearly strong enough to be the final method.

Current paper status:

- Useful as a diagnostic or ablation.
- Not recommended as the main contribution.

### 3.2 GLIN_LSM

Location:

- `src/benchmark/bench_update_wkt.cpp`

Idea:

Keep a main GLIN and a delta R-tree. Inserts go to the delta. When the delta is
full, merge all records and rebuild the main GLIN.

Path:

```text
main GLIN + delta R-tree
delta full -> sort all main+delta by Zmin -> rebuild main GLIN
```

Strength:

- Conceptually clean.
- Correctly exposes the full rebuild cost through `maintenance_ns`.

Weakness:

- Full rebuild is too expensive.
- If the rebuild is counted in foreground time, write throughput becomes poor.
- If `delta_size` is small, rebuild happens too often.
- If `delta_size` is large, queries must check a large delta index.

Current paper status:

- Do not use as final method.
- Useful only to show why naive LSM/full rebuild is insufficient.

### 3.3 GLIN_LSM_ASYNC

Locations:

- `src/benchmark/bench_update_wkt.cpp`
- `src/benchmark/bench_hybrid_wkt.cpp`

Idea:

Separate foreground delta writes from maintenance time. In the update-only
benchmark, `update_ns` counts only delta writes and `maintenance_ns` records
merge/rebuild time separately.

Strength:

- Shows the best-case write path if rebuild/merge can be moved away from the
  foreground.
- Helpful for understanding why async maintenance matters.

Weakness:

- In the update-only benchmark, it is not a complete online system.
- The hybrid version still suffers from full rebuild or large delta double-read
  cost.
- With small `DELTA_SIZE`, rebuild is too frequent; with large `DELTA_SIZE`,
  query cost grows.

Current paper status:

- Do not claim it as the final method.
- Use as a motivation experiment: "foreground writes are cheap, but maintenance
  must be scheduled carefully."

### 3.4 GLIN_LSM_SEGMENTED

Location:

- `src/benchmark/bench_hybrid_wkt.cpp`

Idea:

Avoid full rebuild. When the delta is full, build a new GLIN segment. Queries
check base GLIN, all segments, and current delta. Segments are compacted in an
LSM-like manner.

Strength:

- Avoids rebuilding the whole main GLIN on every flush.
- More realistic than `GLIN_LSM_ASYNC`.

Weakness:

- Too many segments increase query cost.
- Synchronous flush and compaction still block foreground workload.
- Without strong pruning, query has to scan too many segment indexes.

Current paper status:

- Not recommended as the final method.
- Useful as an intermediate design step.

### 3.5 GLIN_LSM_SEGMENTED4

Location:

- `src/benchmark/bench_hybrid_wkt.cpp`

Idea:

Use fanout-4 size-tiered compaction and segment pruning. A query can skip a
segment if the segment envelope or Zmin/Zmax range cannot intersect the query.

Path:

```text
delta R-tree -> GLIN segment
4 same-level segments -> compact into 1 higher-level segment
query -> base GLIN + pruned segments + delta
```

Strength:

- Correctness is good in current Fig.17 tests:
  - `answers_match_boost=1`
- Better than the first segmented version.
- Good ablation to show the benefit of fanout=4 and segment pruning.

Weakness:

- Flush and compaction are still synchronous.
- Foreground throughput is much worse than `GLIN_LSM_BG`.

Current paper status:

- Good ablation baseline.
- Not the final system variant.

### 3.6 GLIN_LSM_BG

Location:

- `src/benchmark/bench_hybrid_wkt.cpp`

Idea:

Make segmented GLIN-LSM asynchronous. Foreground writes go to a delta R-tree.
When the delta is full, it becomes a sealed pending delta. A background job
builds GLIN segments and compacts segments. Queries check:

```text
base GLIN
+ compacted GLIN segments
+ pending delta R-trees
+ current delta R-tree
```

Current implementation details:

- Uses `std::async` for one background maintenance job.
- Uses fanout=4.
- Uses segment pruning by envelope and Zmin/Zmax.
- Uses grouped delta flush:
  - During workload, wait until 4 sealed deltas are available before building a
    GLIN segment.
  - During final drain, flush the remaining partial group.
- Tracks both foreground throughput and total throughput.

Strength:

- Best current system variant.
- Correctness matches Boost R-tree in current hybrid results:
  - `answers_match_boost=1`
- Foreground write-intensive throughput beats Boost R-tree in current 2M tests.
- Maintenance cost is lower than synchronous segmented4.

Weakness:

- It is multi-threaded/asynchronous, so it is not a fair drop-in replacement for
  single-threaded baselines unless clearly labeled.
- `drain_ns` is still non-trivial.
- In write-intensive workloads, total throughput can still be lower than Boost
  R-tree after counting final background drain.

Current paper status:

- This is the most usable GLIN extension.
- It should be positioned as a "system-level asynchronous variant", not as the
  original GLIN paper baseline.
- It supports a measured claim about foreground throughput, not a universal
  claim that GLIN beats R-tree on every metric.

## 4. How to Run Experiments

### 4.1 Build

```bash
cmake --build build --target bench_update_wkt bench_update_wkt_piece -j2
cmake --build build --target bench_hybrid_wkt_piece -j2
```

GEOS and Boost warnings are expected. The important part is that the build exits
with code 0.

### 4.2 Fig.15 Insert Diagnostics

Default insert-order and Boost strategy diagnostics:

```bash
RESET_RESULTS=1 ./scripts/run_fig15_insert_diagnostics.sh
```

Run buffered GLIN:

```bash
INCLUDE_BUFFERED_GLIN=1 BUFFER_SIZE=10000 ./scripts/run_fig15_insert_diagnostics.sh
```

Run update-only LSM variants:

```bash
INCLUDE_LSM_GLIN=1 INCLUDE_LSM_ASYNC_GLIN=1 DELTA_SIZE=100000 ./scripts/run_fig15_insert_diagnostics.sh
```

Outputs:

- `results/fig15_insert_diagnostics/fig15_insert_diagnostics_summary.csv`
- `results/fig15_insert_diagnostics/insert_order_sweep.csv`
- `figures/fig15_insert_diagnostics/insert_order_sweep.png`
- `figures/fig15_insert_diagnostics/boost_split_strategy_sweep.png`
- `figures/fig15_insert_diagnostics/cell_size_sweep_glin.png`

### 4.3 Fig.17 Hybrid Workload

Current recommended command:

```bash
RESET_RESULTS=1 INCLUDE_LSM_BG_GLIN=1 INCLUDE_LSM_SEGMENTED4_GLIN=1 LIMIT=2000000 DELTA_SIZE=100000 ./scripts/run_fig17_hybrid_1m.sh
```

Useful smaller smoke command:

```bash
RESET_RESULTS=1 DATASETS=ROADS WORKLOAD=write LIMIT=200000 QUERY_LIMIT=200000 INCLUDE_LSM_BG_GLIN=1 INCLUDE_LSM_SEGMENTED4_GLIN=1 DELTA_SIZE=10000 PROGRESS_STEP_PERCENT=10 ./scripts/run_fig17_hybrid_1m.sh
```

Outputs:

- `results/fig17_hybrid_2000000/fig17_hybrid_summary.csv`
- `results/fig17_hybrid_2000000/fig17_hybrid_progress.csv`
- `figures/fig17_hybrid_2000000/fig17_hybrid_curves_panel.png`
- `figures/fig17_hybrid_2000000/fig17_hybrid_delta_pending_panel.png`
- `figures/fig17_hybrid_2000000/fig17_hybrid_segment_count_panel.png`
- `figures/fig17_hybrid_2000000/fig17_hybrid_maintenance_ns_panel.png`
- `figures/fig17_hybrid_2000000/fig17_hybrid_summary_diagnostics.txt`

## 5. How to Read the New Metrics

- `foreground throughput`
  - CSV field: `throughput_transactions_per_sec`
  - Meaning: throughput seen during the workload. For async variants, this is
    the online query/update experience.

- `total throughput`
  - CSV field: `total_throughput_transactions_per_sec`
  - Meaning: throughput after adding `drain_ns` to workload time. This is a
    stricter metric because it counts final background cleanup.

- `maintenance_ns`
  - Time spent building GLIN segments or doing merge/compaction.
  - High value means the method creates heavy background/index-maintenance work.

- `drain_ns`
  - Time spent waiting after the workload for unfinished background jobs.
  - High value means the background worker did not keep up.

- `delta_pending`
  - Number of records still in current or sealed delta R-trees.
  - High value means queries may pay more double-read cost.

- `segment_count`
  - Number of GLIN segments outside the base GLIN.
  - High value means each query may touch more indexes.

- `candidate_answer_ratio`
  - `candidates_total / answers_total`.
  - Closer to 1 means better filtering.

- `answers_match_boost`
  - `1` means the answer count matches Boost R-tree.
  - `0` means either the index has a correctness issue or the baseline/result
    accounting must be inspected.

Important note:

`GLIN_PIECEWISE` has `answers_match_boost=0` in the current Fig.17 summary.
Therefore, do not use GLIN-piecewise as the correctness oracle for these hybrid
results. Use Boost R-tree as the correctness reference.

## 6. Current Results

### 6.1 Insert-Order Diagnostic

Result file:

- `results/fig15_insert_diagnostics/insert_order_sweep.csv`

Main observation:

Random insertion hurts GLIN, but Zmin insertion often helps GLIN over Boost
R-tree.

Selected throughput results:

| Dataset | Order | GLIN ops/s | Boost R-tree ops/s | GLIN-piecewise ops/s | Observation |
|---|---:|---:|---:|---:|---|
| AW | random | 582,709 | 904,195 | 563,943 | GLIN loses on random insert |
| AW | zmin | 2,206,970 | 1,883,200 | 2,202,650 | GLIN wins on Zmin insert |
| LW | random | 591,586 | 926,645 | 574,606 | GLIN loses on random insert |
| LW | zmin | 2,217,880 | 1,903,880 | 2,192,780 | GLIN wins on Zmin insert |
| ROADS | random | 732,469 | 921,380 | 707,036 | GLIN loses on random insert |
| ROADS | zmin | 1,559,280 | 1,389,160 | 1,544,810 | GLIN wins on Zmin insert |
| PARKS | random | 386,030 | 959,662 | 382,799 | GLIN loses badly on random insert |
| PARKS | zmin | 1,309,360 | 1,164,110 | 1,328,000 | GLIN wins on Zmin insert |

Paper value:

- This is a strong workload characterization result.
- It motivates buffering/LSM designs.
- It is not by itself a system contribution.

### 6.2 Hybrid Workload at 2M Records

Result file:

- `results/fig17_hybrid_2000000/fig17_hybrid_summary.csv`

Current command:

```bash
RESET_RESULTS=1 INCLUDE_LSM_BG_GLIN=1 INCLUDE_LSM_SEGMENTED4_GLIN=1 LIMIT=2000000 DELTA_SIZE=100000 ./scripts/run_fig17_hybrid_1m.sh
```

#### Read-Intensive Workload

| Dataset | Index | Foreground tps | Total tps | maintenance_s | drain_s | correctness |
|---|---|---:|---:|---:|---:|---:|
| ROADS | GLIN-LSM-bg | 71.201 | 66.922 | 2.203 | 0.449 | OK |
| ROADS | Boost R-tree | 67.846 | 67.846 | 0 | 0 | OK |
| ROADS | GLIN-LSM-seg4 | 45.540 | 45.540 | 3.916 | 0 | OK |
| PARKS | GLIN-LSM-bg | 55.528 | 52.723 | 2.376 | 0.479 | OK |
| PARKS | Boost R-tree | 52.748 | 52.748 | 0 | 0 | OK |
| PARKS | GLIN-LSM-seg4 | 38.092 | 38.092 | 4.221 | 0 | OK |

Read-intensive interpretation:

- `GLIN_LSM_BG` foreground throughput is slightly better than Boost R-tree.
- Total throughput is close to Boost R-tree.
- It is much better than synchronous `GLIN_LSM_SEGMENTED4`.

#### Write-Intensive Workload

| Dataset | Index | Foreground tps | Total tps | maintenance_s | drain_s | correctness |
|---|---|---:|---:|---:|---:|---:|
| ROADS | GLIN-LSM-bg | 51.607 | 34.921 | 2.110 | 0.926 | OK |
| ROADS | Boost R-tree | 37.869 | 37.869 | 0 | 0 | OK |
| ROADS | GLIN-LSM-seg4 | 17.214 | 17.214 | 3.894 | 0 | OK |
| PARKS | GLIN-LSM-bg | 46.321 | 31.807 | 2.323 | 0.985 | OK |
| PARKS | Boost R-tree | 36.281 | 36.281 | 0 | 0 | OK |
| PARKS | GLIN-LSM-seg4 | 15.137 | 15.137 | 4.376 | 0 | OK |

Write-intensive interpretation:

- `GLIN_LSM_BG` foreground throughput beats Boost R-tree:
  - ROADS: about 36 percent higher than Boost R-tree.
  - PARKS: about 28 percent higher than Boost R-tree.
- But total throughput is lower than Boost R-tree after counting `drain_ns`.
- This means the foreground path is good, but the single background worker still
  accumulates maintenance debt.

### 6.3 Maintenance Cost

`GLIN_LSM_BG` reduces maintenance cost compared with `GLIN_LSM_SEGMENTED4`.

Examples from 2M hybrid results:

| Dataset | Workload | seg4 maintenance_s | bg maintenance_s | Reduction |
|---|---|---:|---:|---:|
| ROADS | read | 3.916 | 2.203 | about 44 percent |
| ROADS | write | 3.894 | 2.110 | about 46 percent |
| PARKS | read | 4.221 | 2.376 | about 44 percent |
| PARKS | write | 4.376 | 2.323 | about 47 percent |

This supports the grouped flush + background scheduling design.

## 7. What Is a Paper Innovation?

### Strongest Paper Story

The strongest current story is:

1. Workload characterization:
   - GLIN is highly sensitive to insertion order.
   - Random insertion is bad, but Zmin-ordered insertion is strong.

2. System mechanism:
   - Use a delta index to absorb random writes.
   - Convert random arrivals into sorted GLIN segments.
   - Use fanout-4 compaction and segment pruning to control query overhead.
   - Move segment construction/compaction to background maintenance.

3. Evaluation artifact:
   - Report both foreground throughput and total throughput.
   - Report `maintenance_ns`, `drain_ns`, `delta_pending`, `segment_count`,
     and correctness against Boost R-tree.

This is more credible than saying only "we added an LSM buffer."

### Claims That Are Currently Supported

Supported:

- Random insertion is a weakness of GLIN/ALEX-style ordered array storage.
- Zmin-ordered insertion can make GLIN competitive or better than Boost R-tree.
- Naive full-rebuild LSM is not enough.
- Segmentation avoids full rebuild but needs compaction and pruning.
- Background GLIN-LSM improves foreground throughput under hybrid workloads.
- `GLIN_LSM_BG` is correct against Boost R-tree in current 2M hybrid results.

Partially supported:

- `GLIN_LSM_BG` can outperform Boost R-tree in foreground throughput.
- Background maintenance reduces foreground stalls.

Not currently supported:

- "GLIN-LSM-bg universally beats Boost R-tree."
- "Maintenance is free."
- "The async variant is a fair single-threaded baseline comparison."
- "GLIN-piecewise is correct in current hybrid query accounting."

## 8. Which Variant Should Be Used?

Recommended final system variant:

- `GLIN_LSM_BG`

Use as ablations:

- `GLIN_LSM_SEGMENTED4`
- `GLIN_LSM_ASYNC`
- `GLIN_LSM`
- `GLIN_BUFFERED`

Do not use as final method:

- `GLIN_BUFFERED`
  - Reason: insert-only and limited as a full system.

- `GLIN_LSM`
  - Reason: full rebuild cost is too high.

- `GLIN_LSM_ASYNC`
  - Reason: useful as a write-path idealization, but not a complete fair system
    result.

- `GLIN_LSM_SEGMENTED`
  - Reason: too many segments and weak query behavior.

- `GLIN_LSM_SEGMENTED4`
  - Reason: correct and useful, but synchronous maintenance hurts foreground
    throughput.

Use carefully:

- `GLIN_PIECEWISE`
  - Reason: useful as an existing GLIN variant, but current hybrid
    `answers_match_boost=0`; do not use it as the correctness oracle.

## 9. Fairness and Paper Wording

`GLIN_LSM_BG` uses asynchronous background maintenance. Therefore, it should not
be presented as a direct replacement for the original single-threaded GLIN
baseline.

Recommended wording:

> We report GLIN-LSM-bg as a system-level asynchronous extension. Its foreground
> throughput measures the online query/update path, while total throughput
> includes the final drain cost of background maintenance.

Avoid wording:

> GLIN-LSM-bg is the new GLIN baseline.

Avoid wording:

> GLIN-LSM-bg always outperforms Boost R-tree.

Better claim:

> GLIN-LSM-bg improves foreground update throughput under hybrid workloads by
> decoupling random writes from sorted GLIN segment construction. The improvement
> comes with deferred maintenance, which is exposed through `maintenance_ns` and
> `drain_ns`.

## 10. Current Decision

Do not keep randomly changing the implementation just to chase a small number.
The current implementation is good enough to freeze as a research prototype:

- Use `GLIN_LSM_BG` as the main system extension.
- Use `GLIN_LSM_SEGMENTED4` as the main ablation.
- Keep `GLIN_BUFFERED`, `GLIN_LSM`, and `GLIN_LSM_ASYNC` as diagnostic
  variants.
- Focus next on stronger evaluation and paper framing.

## 11. Recommended Next Experiments

High priority:

1. Repeat 2M hybrid workload with multiple seeds.
2. Run larger workloads, for example 5M or 10M, to see whether `drain_ns`
   becomes smaller relative to total workload time.
3. Report foreground throughput and total throughput side by side.
4. Report `maintenance_ns`, `drain_ns`, `delta_pending`, and `segment_count`.
5. Add memory usage if possible.

Medium priority:

1. Vary `DELTA_SIZE` but present it as a sensitivity study, not manual tuning.
2. Compare query selectivities, for example 0.1 percent, 1 percent, and 10
   percent.
3. Include AW/LW if hybrid query generation is stable for those datasets.

Low priority:

1. More background workers.
2. More complicated compaction scheduling.
3. Deletion-aware segmented GLIN-LSM.

These are interesting but can easily turn into engineering work without a clear
paper payoff.

