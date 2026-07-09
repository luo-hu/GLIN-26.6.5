# DELI-Adaptive-PRL-Fusion 设计草案

最后更新日期：2026-07-09

## 1. 观察结论

实验目录：

```text
results/dynamic_compare_mixed_0.05m_cost_dp_prl_zgapmixed10
results/dynamic_compare_mixed_0.05m_cost_dp_prl_zgapmixed11
results/dynamic_compare_mixed_0.05m_cost_dp_prl_zgapmixed12
```

三组 summary 共 2016 行，覆盖：

```text
datasets: AW, LW, ZGAP_MIXED
selectivity: 0p001pct, 0p01pct, 0p1pct, 1pct
profiles: read_heavy, balanced, write_heavy
```

PRL-only 结果的平均排名显示：

```text
avg_query_ms:
  DELI_ALEX_HYBRID_COST_PRL 最好。
  DELI_ALEX_HYBRID_SINGLE_STORE_COST_PRL 次之。
  Boost_Rtree_PRL 在部分低选择性 LW 场景明显更好。

overall_ops_tps:
  DELI_ALEX_HYBRID_COST_PRL 最好。
  DELI_ALEX_HYBRID_SINGLE_STORE_COST_PRL 次之。

insert_tps:
  DELI_ALEX_HYBRID_SINGLE_STORE_PRL 最好。
  SingleStore-Cost 次之。

delete_tps:
  RLR_LITE_CS_PRL / RLR_LITE_CS_SPLIT_PRL 最好。
  HIRE_SFC_LITE_PRL 次之。

mbr_candidates / exact_calls:
  Boost_Rtree_PRL 最少。
  DELI-Cost 系列次之。
```

因此，下一版不应该只优化 learned model，而应该同时处理：

```text
1. query latency:
   降低 block checks 和 records scanned，尤其是低选择性、query-heavy 或 balanced 场景。

2. insert throughput:
   保留 SingleStore 的单副本写入路径，避免 ALEX data copy 带来的写入成本。

3. delete throughput:
   引入 RLR/HIRE 风格的 mask/delete-slot reuse，减少即时结构维护。

4. exact predicate bottleneck:
   对 fat-object/ZGAP workload 控制 false positives 和 exact calls。
```

## 2. 新版本名称

建议新增 index：

```text
DELI_ADAPTIVE_PRL_FUSION
```

论文中可以称为：

```text
DELI-Fusion
```

定位：

```text
An adaptive dynamic learned spatial index that combines DELI-Cost's
predicate-aware block maintenance, SingleStore's single-copy update path,
RLR-style lazy deletion, and HIRE-inspired local recalibration.
```

## 3. 核心设计

### 3.1 双模式 leaf/block

每个 block 维护一个轻量状态：

```cpp
enum class BlockMode {
  LearnedCompact,   // DELI-Cost: compact sorted records + PRL summary
  SingleStoreDelta, // SingleStore: single-copy overlay + cheaper insert
  SpatialGuard,     // RLR/R-tree guard: small local spatial guard index
  ColdLazy          // lazy delete / sparse tombstone, delay compaction
};
```

每个 block 根据近期统计切换模式，而不是全局固定一种维护策略。

### 3.2 代价模型升级

当前 DELI-Cost 主要估计 local compaction 是否值得做。Fusion 版把决策扩展为：

```text
score_query =
  scan_cost * scanned_records
  + mbr_cost * mbr_candidates
  + exact_cost * exact_calls
  + block_cost * visited_blocks

score_update =
  insert_cost * delta_size
  + delete_cost * tombstones
  + rebuild_cost * compact_records

score_memory =
  bytes_cost * extra_guard_bytes
```

每个 checkpoint 或每 N 次操作重新估计：

```text
keep LearnedCompact
switch to SingleStoreDelta
build/drop SpatialGuard
compact now
delay compaction
reuse deleted slots
```

### 3.3 Query-hot block 使用 SpatialGuard

Boost R-tree 在低选择性 LW 场景胜出的原因不是 update 更强，而是 query path 少扫大量 learned blocks。Fusion 可以只给少数 query-hot 或 false-positive-heavy block 建一个局部 guard：

```text
per block optional guard:
  small R-tree / packed MBR vector / grid over record MBRs

trigger:
  visited_blocks high
  records_scanned high
  mbr_candidates low relative to scan
  recent query_count high

query:
  learned directory 定位候选 block
  block summary 过滤
  if guard exists:
      guard.query(query_mbr) 得到 record candidates
  else:
      scan compact + delta records
  PRL / shortcut / GEOS refine
```

这吸收 R-tree/RLR 的优点，但只在局部引入，避免全局 R-tree 的写入和内存成本。

### 3.3.1 Global SpatialGuard 快路径

Stage C/D/E 后新增一个全局 SpatialGuard，用于解决 AW/LW 低选择性下 learned directory 仍需检查较多 block 的问题：

```text
bulk load:
  使用 workload 原始初始顺序构建全局 R-tree guard。
  overlay 仍然按 z-order 排序和分 block，二者互不影响。

query routing:
  前 4 次 query 同时测 learned path 和 global guard probe。
  如果 global guard 平均耗时更低，则打开 global_guard_fast_path。
  fast path 直接执行 R-tree MBR query + PRL shortcut + GEOS exact refine。

update:
  insert/delete 同步维护 global guard 和 overlay live mask。
  因此查询收益会带来额外更新维护成本。
```

当前 smoke 结果：

```text
results/smoke_fusion_global_guard_order

AW/read_heavy:
  0p001pct: avg_query_ms 比 Boost_Rtree 低约 9.9%
  0p01pct:  avg_query_ms 比 Boost_Rtree 低约 8.8%
  1pct:     avg_query_ms 比 Boost_Rtree 低约 16.5%

LW/read_heavy:
  0p001pct: avg_query_ms 比 Boost_Rtree 高约 4.9%
  0p01pct:  avg_query_ms 比 Boost_Rtree 高约 7.1%
  1pct:     avg_query_ms 比 Boost_Rtree 低约 48.7%
```

结论：

```text
Global SpatialGuard 可以补齐 AW 和高选择性 LW 的查询短板。
LW 极低/低选择性下，查询本身只有约 0.01 ms，Fusion 多维护 learned overlay、
block 统计和 global guard 的更新成本会被放大。
下一步 ablation 应该验证：
  global_guard_fast_path 打开后，降低 overlay recalibration / mode stats / local guard 维护预算；
  或新增 guard-dominant mode，只保留 correctness 必需的 live mask 和延迟 overlay 修复。
```

### 3.4 Write-hot block 使用 SingleStoreDelta

SingleStore 的优势在 insert throughput。Fusion 中：

```text
if block insert_rate high and query_heat not high:
  use SingleStoreDelta
  append only once
  summary stale-large
  postpone exact sorted compact layout
```

当该 block 后续变成 query-hot，再切回 LearnedCompact 或建立 SpatialGuard。

### 3.5 Delete-hot block 使用 RLR/HIRE 风格 lazy mask

RLR 的 delete TPS 很强，HIRE 的 deleted-slot reuse 也适合动态 workload。Fusion 中：

```text
delete:
  mark alive=false
  tombstone_count++
  no immediate shrink summary

insert:
  if compatible deleted slot exists:
      reuse slot
  else:
      append delta

compact trigger:
  cost-driven, not fixed threshold only
```

### 3.6 HIRE-inspired local recalibration

不做完整 HIRE RCU。当前 benchmark 单线程，先实现：

```text
pending_rebuild queue
per checkpoint apply limited rebuild budget
query-hot block 优先 recalibrate
write-hot block 延迟 recalibrate
```

这比一次性 local compaction 更稳定，有助于 tail latency。

## 4. 实现阶段

### Stage A：Fusion Policy Shell

先不改数据结构，只增加统计和决策日志：

```text
block_query_count
block_insert_count
block_delete_count
records_scanned
mbr_candidates
exact_calls
tombstone_count
delta_size
mode_decision
```

新增 index 名称：

```text
DELI_ADAPTIVE_PRL_FUSION
```

第一版可以复用：

```text
DELI_ALEX_HYBRID_SINGLE_STORE_COST_PRL
```

作为底座。

### Stage B：SingleStore / Cost 自适应切换

目标：

```text
write-heavy 场景接近 SingleStore insert_tps，
read-heavy 场景保留 Cost query latency。
```

先实现 block-level policy：

```text
write_hot -> SingleStoreDelta
query_hot -> LearnedCompact
```

当前 Stage A+B 实现状态：

```text
index name:
  DELI_ADAPTIVE_PRL_FUSION

base:
  DELI_ALEX_HYBRID_SINGLE_STORE_COST

enabled behavior:
  1. 每个 LocalBounded block 维护 FusionBlockMode：
       LearnedCompact
       SingleStoreDelta
       ColdLazy

  2. 每个 block 记录：
       fusion_query_count
       fusion_insert_count
       fusion_delete_count
       fusion_records_scanned
       fusion_mbr_candidates
       fusion_exact_calls
       fusion_mode_switches

  3. mode policy：
       query-hot / low dirty block -> LearnedCompact
       insert/update-hot delta block -> SingleStoreDelta
       delete-heavy stale block -> ColdLazy

  4. mode 会影响 compaction threshold：
       LearnedCompact 更积极 compact，保护查询延迟。
       SingleStoreDelta 延迟 insert compaction，保护写入吞吐。
       ColdLazy 延迟 delete compaction，保护删除吞吐。

debug output:
  deli_fusion_debug.csv
```

当前实现状态：

```text
1. Stage A/B: block-level mode stats + mode switching 已实现。
2. Stage C: deleted-slot reuse 已实现，只复用不会破坏 compact_ids z-order 的 tombstone slot。
3. Stage D: local SpatialGuard 已实现为 block-local Boost R-tree guard，当前只覆盖 compact_ids；delta_ids 仍按原路径扫描。
4. Stage E: budgeted background recalibration 已实现为单线程前台模拟的 pending rebuild queue，不包含 HIRE 的 RCU/background thread。
```

### Stage C：Deleted-slot Reuse

从 HIRE_SFC_LITE 迁移思想到 DELI block：

```text
compact array 中 tombstone slot 可被新 record 复用
如果复用不破坏 z-order / local ordering，则不进入 delta。
```

实现细节：

```text
try_reuse_deleted_slot(block, oid)
  只在 DELI_ADAPTIVE_PRL_FUSION 中启用。
  扫描 compact_ids 中 dead slot。
  用前后邻居 z-order 检查新 oid 是否可放入该 slot。
  成功后 live/in_delta/object_to_block/live_count/dead_count 同步更新。
  失败则回退原 SingleStore-Cost delta insert 路径。
```

预期改善：

```text
delete_tps / insert_tps
delta_records_scanned
tombstone_ratio
```

### Stage D：SpatialGuard

只给高收益 block 建局部 guard：

```text
guard_gain =
  recent_query_count * (scan_before - scan_after) * scan_cost
  - guard_build_cost
  - guard_memory_cost
```

当前近似版本：

```text
maybe_build_fusion_spatial_guard(block)
  触发条件：
    compact_ids >= 64
    live_count >= 64
    fusion_query_count >= 8
    avg_scan_per_visit >= 128
    fusion_records_scanned >= 4 * max(1, fusion_mbr_candidates)

query_compact_ids()
  guard clean 时：
    用 block-local Boost R-tree 按 query envelope 找 compact 候选。
    然后仍执行 zmin/zmax、MBR、PRL shortcut、GEOS exact 判断。
  guard dirty 时：
    回退原 compact sequential scan。

guard build policy:
  query path 不做前台 build。
  query 先记录本次延迟，然后最多为一个刚访问过的 hot block 做后台式 guard build。
  这样 SpatialGuard 的构建成本不会直接污染 avg/p95 query latency，也不会压低普通 insert/delete TPS。

guard 只覆盖 compact_ids。
delta_ids 始终保留原扫描路径，因此不会漏掉新增对象。
compact membership 变化时 guard 标 dirty；delta append 不会让 compact guard 失效。
```

预期改善：

```text
LW 低选择性 balanced/read-heavy 场景
query latency
overall throughput
records_scanned
```

### Stage E：Budgeted Background Recalibration

实现一个前台模拟的 rebuild budget：

```text
每 N ops 最多 rebuild K 个 block
优先级 = expected_query_gain / rebuild_cost
```

当前近似版本：

```text
Fusion block 达到 compact 阈值时先进入 pending rebuild queue。
更新操作之后按小预算消化 queue。
同一 block 不重复入队。
query 对 pending block 仍走保守 summary + live bit 过滤，所以 correctness 不受影响。

注意：
这是单线程 benchmark 中的 background recalibration 近似，不包含 HIRE 的 RCU、
后台线程和读写并发版本切换。
```

预期改善：

```text
p95_query_ms
p99_query_ms
overall throughput stability
```

## 5. 必要消融

至少需要以下 ablation：

```text
DELI_ADAPTIVE_PRL_FUSION
DELI_ADAPTIVE_PRL_FUSION_NO_GUARD
DELI_ADAPTIVE_PRL_FUSION_NO_SLOT_REUSE
DELI_ADAPTIVE_PRL_FUSION_NO_MODE_SWITCH
DELI_ADAPTIVE_PRL_FUSION_NO_BACKGROUND
```

必须和以下 baseline 比较：

```text
DELI_ALEX_HYBRID_COST_PRL
DELI_ALEX_HYBRID_SINGLE_STORE_COST_PRL
RLR_LITE_CS_SPLIT_PRL
HIRE_SFC_LITE_PRL
Boost_Rtree_PRL
```

## 6. 论文贡献表述

可以形成一个比“组合 baseline”更清楚的贡献：

```text
We propose an adaptive maintenance policy for dynamic learned spatial
indexes. Instead of committing to a single update path, the index switches
individual blocks among compact learned layout, single-store delta layout,
lazy slot reuse, and optional local spatial guards according to a unified
query-update cost model.
```

关键 claim：

```text
1. DELI-Fusion preserves the strong query performance of DELI-Cost-PRL
   under read-heavy workloads.

2. It approaches SingleStore's insert throughput under write-heavy workloads.

3. It reduces delete overhead using lazy tombstones and deleted-slot reuse.

4. It narrows the gap to R-tree/RLR on low-selectivity query-heavy workloads
   by selectively building local spatial guards only where scan waste is high.
```

## 7. 风险

```text
1. SpatialGuard 可能增加内存，必须报告 index_bytes_estimate。
2. 过度 mode switching 可能造成震荡，需要 hysteresis。
3. 删除 lazy summary 必须 stale-large，不能 stale-small，否则会漏答案。
4. Guard 只能作为候选过滤，最终仍然必须 GEOS exact refinement。
5. 如果 guard build 成本过高，应限制在 query-hot top-k blocks。
```
