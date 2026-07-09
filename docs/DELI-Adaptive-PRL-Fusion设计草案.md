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

### 3.3.2 Update-Light Global Guard

Stage F 引入 update-light 策略，把 RLR 的 lazy deletion 和 SingleStore 的 append-first 思路并入 global guard：

```text
delete:
  不再对 global guard 执行 RTree::remove。
  只更新 live mask，并累加 global_guard_stale_count。
  query 从 global guard 取到候选后继续用 live mask 过滤 stale entry。

insert:
  旧版本仍同步写入 overlay delta 和 global guard，保证 fast path 可以立即看到新对象。
  当 global_guard_fast_path 已经打开时，跳过 deleted-slot reuse 的 compact slot 扫描。
  这样 insert 更接近 SingleStore append 路径，避免每次为复用 tombstone 扫一个 block。

query:
  继续使用 global guard fast path。
  stale entry 只增加少量候选过滤成本，不影响正确性。
```

当前 smoke 结果：

```text
results/smoke_fusion_update_light

CHECK_CORRECTNESS=1, answers_match_boost 全部为 1。

read_heavy / mixed_20000:
  AW 0p01pct: q=0.0622 ms, insert_tps=465K, delete_tps=1.39M
  AW 0p1pct:  q=0.0854 ms, insert_tps=380K, delete_tps=1.16M
  LW 0p01pct: q=0.0183 ms, insert_tps=444K, delete_tps=1.51M
  LW 0p1pct:  q=0.0307 ms, insert_tps=492K, delete_tps=1.45M

write_heavy / mixed_20000:
  AW 0p01pct: q=0.0913 ms, insert_tps=542K, delete_tps=1.91M
  AW 0p1pct:  q=0.1063 ms, insert_tps=552K, delete_tps=1.92M
  LW 0p01pct: q=0.0175 ms, insert_tps=646K, delete_tps=2.14M
  LW 0p1pct:  q=0.0307 ms, insert_tps=733K, delete_tps=2.53M
```

结论：

```text
Update-Light Global Guard 明显改善 Fusion 的 insert/delete latency。
RLR-Lite-CS-Split 的 delete 仍然更强，因为它几乎只做 mask update；
SingleStore-Cost 的 insert 仍然更强，因为它没有同步维护 global guard。
Fusion 的折中点是：保留 global guard 查询优势，同时把 delete 从同步树删除降为 lazy mask。

后续可选增强：
  当 global_guard_stale_count / global_guard_live_count 超过阈值时，
  在 checkpoint 外或单独 ablation 中测试 global guard rebuild。
  注意不要把 rebuild 直接塞进普通 query/update latency，否则会污染 tail latency。
```

### 3.3.3 Single Delta Guard + Fast-Path Light Updates

Stage G/H 继续把 global guard 的 update path 降低到更接近 SingleStore/RLR：

```text
base global guard:
  bulk-load 后冻结，不再对每次 insert 做 RTree::insert。

insert:
  新对象追加到 fusion_global_delta_ids。
  同时插入一个独立的 mutable delta R-tree。
  delta tree 只包含新增对象，规模远小于 base global guard，
  因此避免了直接向 25 万级 base R-tree 做 per-insert 更新。

delete:
  继续只更新 live mask 和 stale counter。

query:
  base global guard query
  + single delta guard query
  + live mask / PRL shortcut / GEOS refine

fast-path light update:
  global_guard_fast_path 打开后，insert/delete 只维护 correctness 必需状态：
    live mask
    object_to_block
    local delta/dead/live counters
    conservative block summary
    global delta guard metadata
  跳过局部 compaction、mode stats、pending rebuild 和 background recalibration。
```

这样做的动机：

```text
SingleStore-Cost 的 insert 快，是因为它不维护全局空间树。
RLR 的 delete 快，是因为它基本只做 mask/lazy delete。
Fusion 若想保住 global guard 的 query 优势，就不能完全变成二者；
但可以把全局树更新从同步更新大 base R-tree 变成更新小 delta R-tree，
并在 fast path 后停止为几乎不用的 learned overlay 支付前台维护成本。
```

反例记录：

```text
不要每隔 N 次 insert 就把全部 delta ids 重新 bulk-load 成一个 delta R-tree。

在 results/dynamic_compare_mixed_0.5m_cost_dp_prl_zgapmixed34 中：
  AW/write_heavy/0p01pct/mixed_200000
  global_guard_delta_count ~= 50K
  global_guard_delta_rebuild_count = 49
  insert_tps 只有约 118K

原因是累计重建成本接近 1K + 2K + ... + 50K，规模上来后退化成近似二次成本。

第二个反例：

不要把 delta 切成过多 immutable chunks。

在 0.5M read-heavy/balanced/write-heavy 下，delta chunk 数可达到 19/29/49。
低选择性 query 本身候选很少，固定 probe 多个小 R-tree 的成本会主导 latency。
因此最终采用 single mutable delta guard：query 只 probe base + delta 两棵树。
```

当前 smoke 结果：

```text
results/smoke_fusion_single_delta_correctness
results/smoke_fusion_single_delta_0.5m_aw_read_low
results/smoke_fusion_single_delta_0.5m_aw_write

CHECK_CORRECTNESS=1, answers_match_boost 全部为 1。

0.5M AW / read_heavy / 0p001pct / mixed_100000:
  Fusion: q=0.045 ms, insert_tps=427K, delete_tps=1.50M, overall=24.5K
  Boost:  q=0.047 ms, insert_tps=384K, delete_tps=0.43M, overall=23.3K
  SingleStore-Cost: q=0.050 ms, insert_tps=1.20M, delete_tps=1.51M, overall=22.1K

0.5M AW / write_heavy / 0p01pct / mixed_100000:
  Fusion: q=0.071 ms, insert_tps=524K, delete_tps=1.84M, overall=27.6K
  Boost:  q=0.093 ms, insert_tps=408K, delete_tps=0.42M, overall=20.9K
  SingleStore-Cost: q=0.090 ms, insert_tps=1.32M, delete_tps=1.87M, overall=22.1K
```

结论：

```text
Single delta guard 优先恢复低选择性 query latency，并保留较高 overall throughput。
SingleStore-Cost 仍可能在 insert 上领先，因为它不维护任何空间 guard。
RLR 仍会在 delete 上领先，因为 Fusion 仍维护 block-level correctness counters 和全局 stale accounting。
后续若要继续追 update，可以做一个更激进的 ablation：
  guard-dominant mode 下允许 object_to_block/local counters 延迟修复，
  只用 live mask + global guard 保证查询正确性。
但这会影响 validate/debug 指标，应单独作为 ablation，不应直接替换主版本。
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
