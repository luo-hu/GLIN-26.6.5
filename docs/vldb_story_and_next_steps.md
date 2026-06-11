# VLDB 故事线与下一步代码路线

本文档用于整理当前 GLIN 改进工作的主线、已有代码资产、还缺的动态维护机制，以及下一步应该怎么继续实现。重点是避免把工作讲成一堆零散 variant，而是收束成一个可以写论文、可以继续编码验证的系统方向。

## 0. 一句话研究问题

现有 GLIN-style learned spatial index 把复杂 geometry 主要当成 `Zmin` 点 key 来索引，容易出现 extent blindness，也就是索引层看不到对象的空间范围。我们希望把 learned ordered index 从 point-key records 扩展为 extent entries，并在动态更新后仍然维护 false-negative-free 的空间剪枝摘要。

可以暂定论文方法名：

```text
DELI: Dynamic Extent-aware Learned Index
```

这里的名字只是论文层命名。代码里现有的 `IO_BLOCK_MBR`、`IO_OVERFLOW`、`GLIN_LSM_BG` 等名字不用马上全部改掉，但论文叙事里不能把它们并列成一堆主方法。

## 1. 当前结论

目前查询方向已经有可用基础：

- `IO_BLOCK_MBR` 已实现 `Zmin/Zmax interval overlap + block MBR + record MBR + exact refinement`。
- `IO_OVERFLOW` 已实现 `main interval/block index + fat-object overflow R-tree`。
- `ZGAP_MIXED` 已新增，用来验证“少量 fat objects 污染 block summaries”这一类场景。
- `run_interval_overlap_diagnostics.sh` 已能跑真实数据、合成数据、不同 selectivity、不同 block size、不同 overflow fraction，并和 `GLIN_PIECEWISE`、`Boost_Rtree`、`GEOS_Quadtree` 对比。

但如果只停在查询优化，论文容易被认为是：

```text
在 GLIN 上加了 maxZmax 和 block MBR 的工程补丁。
```

为了形成更完整的系统贡献，下一步应该把动态维护纳入主线：

```text
DELI = extent-entry representation
     + safe interval-overlap pruning
     + one-sided dynamic summary maintenance
     + exact refinement
     + query/update/mixed workload evaluation
```

动态维护不是另一个可选 LSM 分支，而是 DELI 是否成立的关键。

## 2. 现在应该改 GLIN 还是改 ALEX/GRE？

短期建议：继续在当前 GLIN 项目里改，不要直接切到 `/home/lh/Code/2026.5.18/GRE`。

原因：

- 当前 GLIN 项目已经有 WKT geometry 加载、GEOS exact predicate、Boost R-tree、GEOS Quadtree、GLIN-piecewise、interval overlap diagnostics 和绘图脚本。
- DELI 的核心问题是 spatial extent maintenance，不是单纯 ALEX 的一维 key 插入策略。
- GRE 项目主要适合作为动态指标和 benchmark 设计参考，例如 throughput、latency percentiles、memory、split/retrain/resize 计数。
- 如果直接在 GRE/ALEX 上改，短期会丢失复杂 geometry、Intersects query、GEOS correctness check 这些已经搭好的空间实验环境。

所以推荐路线是：

```text
实现位置：
  当前 GLIN-26.6.5 项目

参考来源：
  GRE 的 benchmark 指标体系和 ALEX 结构调整统计

未来可选：
  如果 DELI-Dynamic 在当前项目跑通，再考虑把更底层的 ALEX leaf hook 接得更深。
```

换句话说，不是“改 ALEX 还是改 GLIN”的二选一，而是：

```text
先在 GLIN 项目实现 DELI 的空间层和动态语义；
再按需要借鉴或暴露 ALEX 的底层动态统计。
```

## 3. ALEX、GLIN、DELI 的区别

不要写：

```text
We improve ALEX.
```

更准确的写法是：

```text
We build on an ALEX-style dynamic learned ordered layout, but extend the indexed record from a point-like key to a spatial extent entry.
```

三者区别如下：

| 方法 | 索引单位 | 查询逻辑 | 动态维护对象 |
|---|---|---|---|
| ALEX | `<key, payload>` | point lookup / 1D range scan | 一维 key 的有序布局、gap、model、split/retrain |
| GLIN | `<Zmin, geometry>` | Zmin probing + query augmentation + exact refinement | Zmin index、leaf MBR、piecewise query augmentation |
| DELI | `<lower, upper, object_id, entry_id>` | interval overlap pruning + block/record MBR + exact refinement | extent entries、max_upper、block MBR、object table、stale-large summaries |

核心区别一句话：

```text
ALEX is update-aware but extent-blind.
GLIN is geometry-aware but still Zmin-centric.
DELI aims to be both update-aware and extent-aware.
```

中文解释：

```text
ALEX 能动态维护一维 key，但不知道 geometry 的范围。
GLIN 知道 geometry，但主索引仍以 Zmin 为中心，Intersects 依赖 query augmentation。
DELI 把索引单位升级为 extent entry，并直接维护 interval-overlap pruning 所需的动态安全摘要。
```

## 4. 论文里哪些说法要谨慎

需要避免的过强表述：

```text
DELI 一定比 R-tree 快。
DELI 一定比 GLIN 快 2-5 倍。
DELI 在 fat-object 场景一定数量级提升。
这个改动完全够 VLDB。
```

更安全的表述：

```text
DELI targets workloads where Zmin-centric learned spatial indexes suffer from extent blindness.
It is not a universal replacement for R-tree, but a principled extension of learned ordered indexes to exact spatial relationship queries.
```

中文：

```text
DELI 不是万能 R-tree 替代品。
它主要解决 GLIN-style learned spatial index 在复杂 geometry Intersects 查询和动态维护中的 extent blindness。
```

论文贡献应当绑定证据：

| Claim | 需要的证据 |
|---|---|
| 查询剪枝有效 | selectivity sweep、block size sensitivity、candidate/answer ratio、exact correctness |
| fat object 分流有效 | `ZGAP_MIXED`、overflow fraction sensitivity、overflow cost breakdown |
| 动态维护正确 | insert/delete/update 后 `answers_match_boost = 1` |
| 动态维护成本可控 | update throughput、summary rebuild count、stale ratio、p95/p99 latency |
| 不是调参取巧 | train/test 分离或固定协议、多个数据集、多种 workload |

## 5. 当前代码机制如何映射到论文主线

| 当前代码名 | 论文中建议定位 | 是否主线 |
|---|---|---|
| `IO_BLOCK_MBR` | `DELI-Single`：single interval extent entry + block summaries | 是，作为查询基础版 |
| `IO_OVERFLOW` | `DELI-Overflow`：fat-object overflow optimization | 可作为 ablation/optional optimization |
| `ZGAP_MIXED` | mixed fat-object stress dataset | 是，用于解释适用场景 |
| `GLIN_BUFFERED` | micro-batch sorted insert baseline/extension | 不作为主方法 |
| `GLIN_LSM_ASYNC` | system-level async write-path variant | 不作为主方法，公平性需说明 |
| `GLIN_LSM_SEGMENTED/BG` | buffered/compaction 系统扩展 | 最多放 appendix 或 extension |
| `Boost_Rtree` | 标准空间索引 baseline | 必须保留 |
| `GEOS_Quadtree` | 标准空间索引 baseline | 必须保留 |
| `GLIN_PIECEWISE` | learned spatial index baseline | 必须保留 |

重要原则：

```text
IO_BLOCK_MBR / IO_OVERFLOW 属于 extent-aware query path。
BUFFERED / LSM / ASYNC / BG 属于 update policy/system extension。
不要把它们全都作为并列主方法。
```

## 6. DELI-Dynamic 第一版设计

第一版不要急着做 multi-interval cover，也不要急着接真正后台 compaction。先做 single-entry dynamic extent index：

```text
Record:
  object_id
  record_id
  zmin
  zmax
  envelope
  geometry pointer / geometry table id
  alive flag

Index:
  records 是 append-only stable record table
  record_id 永远稳定，不会因为插入而变化
  每个 block 保存一组 record_id
  block 内 record_id 按 records[record_id].zmin 排序
  directory 保存 block 指针，并按 block 的 zmin range 排列

Block summary:
  block_id
  ids: sorted record_id list
  min_zmin
  max_zmin
  max_zmax
  block_mbr
  live_count
  dead_count
  stale_flag

Object table:
  object_id -> current live record_id
```

第一版可以先不直接修改 ALEX leaf 内部，而是做一个独立 `DELI-Single-Dynamic` prototype：

```text
bulk load:
  load geometries -> compute zmin/zmax
  append records -> sort record ids by zmin -> build block-local ids

insert:
  append record to records
  find target block by zmin
  insert record_id into block-local sorted ids
  expand affected block summary
  split block if it becomes too large

delete:
  mark object dead
  remove object_id from object table
  do not shrink max_zmax/block_mbr immediately
  increase dead_count/stale_flag

local rebuild:
  if dead_count exceeds threshold, compact dead ids and rebuild affected block summary exactly

query:
  interval overlap pruning
  block MBR pruning
  record MBR pruning
  exact GEOS refinement
```

这个 prototype 的目标不是最快，而是把动态正确性、指标和实验协议跑通。

注意：不要使用 `object_id -> records position`。如果全局 `records` 用 `vector.insert()` 做中间插入，后面所有位置都会移动，`object_id -> position`、block begin/end 和 summary 都容易失效。第一版应使用 stable `record_id`。

等 prototype 验证后，再考虑更深地接入 ALEX leaf split/merge：

```text
When a leaf is split:
  recompute summaries for the two new leaves.

When leaves are merged:
  recompute summary for the merged leaf.

When entries move between leaves:
  recompute summaries for affected leaves.

When only a model node is retrained:
  no spatial summary change is needed unless leaf membership changes.
```

## 7. 动态维护的核心不变式

DELI-Dynamic 的正确性来自 one-sided conservative summaries。

对每个 block `B`，需要保证：

```text
B.max_zmax >= max zmax of all live entries in B
B.block_mbr contains union MBR of all live geometries in B
B.min_zmin <= min zmin of all live entries in B
```

直白解释：

```text
summary 可以偏大，不能偏小。
```

删除时不马上收缩 summary 是安全的：

- `max_zmax` 偏大：只会让 `max_zmax < query_lower` 更难成立，因此少跳过 block，不会漏答案。
- `block_mbr` 偏大：只会让 MBR disjoint 更难成立，也不会漏答案。
- `min_zmin` 偏小：只会让 `min_zmin > query_upper` 更难成立，仍然不会漏答案。

插入时必须立即扩张 summary：

```text
B.min_zmin = min(B.min_zmin, zmin)
B.max_zmax = max(B.max_zmax, zmax)
B.block_mbr = union(B.block_mbr, object_mbr)
```

否则 summary 可能偏小，查询可能错误跳过 block。

## 8. 需要写进论文的基本性质

这些不是复杂理论，但足够支撑系统论文的方法部分。

### Lemma 1: Conservative cover overlap

如果 geometry `g` 和 query `q` 真实相交，并且二者的 Z-order interval cover 都是 conservative cover，那么至少存在一对 intervals 发生 overlap：

```text
object interval [l, u]
query interval  [L, R]

l <= R and u >= L
```

### Lemma 2: Safe block skipping

如果 block summary 满足 one-sided conservative invariant，那么以下条件可以安全跳过 block：

```text
B.max_zmax < L
B.min_zmin > R
B.block_mbr disjoint query.MBR
```

### Lemma 3: Delete with stale-large summaries

删除 live entries 后，如果 summary 不收缩，那么 stored summary 仍然是 conservative bound，因此不会产生 false negatives。

### Lemma 4: Insert with immediate expansion

插入后立即执行 `min/max/union` 扩张 summary，可以保持 invariant。

### Theorem: Dynamic false-negative freedom

任意 insert/delete/update/local rebuild 序列之后，只要上述 invariants 成立，query pruning 不会漏掉真实相交对象；最后通过 GEOS exact predicate 过滤 false positives，因此结果 exact。

## 9. 复杂度与空间开销

记号：

```text
n    = geometry 数量
N    = interval entry 总数
rho  = N / n，entry expansion ratio
b    = block size
B    = N / b，block 数
kq   = query cover interval 数
V(q) = query 访问的 block 数
S(q) = query 扫描的 entry 数
C(q) = 进入 exact refinement 的候选数
```

查询代价：

```text
T_query(q) = O(kq * block_lookup + V(q) + S(q) + C(q) * T_exact)
```

当前 block prototype 如果还需要线性扫描 block directory，就诚实写成：

```text
block_lookup = B
```

如果以后做 segment tree / hierarchical block directory，可以变成：

```text
block_lookup = log B
```

插入代价：

```text
T_insert(g) = O(T_ordered_insert + T_summary_update)
```

第一版 single interval 中：

```text
T_summary_update = O(1)
```

删除代价：

```text
T_delete(g) = O(T_lookup + T_tombstone) + amortized local rebuild
```

如果每个 block 在 `dead_count > tau * b` 后重算 summary，每次重算 `O(b)`，则 amortized rebuild cost 约为：

```text
O(1 / tau)
```

空间开销：

```text
records:        O(N)
object table:   O(N + n)
block summary:  O(N / b)
directory:      O(N / b) if used
```

如果每个对象最多拆成 `c` 个 entries：

```text
N <= c * n
Space = O(cn)
```

## 10. GRE 项目中最值得借鉴的指标

`/home/lh/Code/2026.5.18/GRE` 不建议直接作为 DELI 的实现起点，但它的 benchmark 指标很值得迁移。

GRE 当前关注：

- `throughput`：总吞吐。
- `memory_consumption`：索引内存。
- latency percentiles：`p50/p90/p95/p99/p99.9/p99.99/max/avg`。
- `latency_variance`：延迟波动。
- ALEX 结构调整计数：`num_expand_and_scales`、`num_expand_and_retrains`、`num_downward_splits`、`num_sideways_splits`、`num_model_node_splits`。
- 操作正确性计数：`success_read`、`success_insert`、`success_remove`、`scan_not_enough`。

DELI-Dynamic 应该增加自己的动态指标：

| 指标 | 中文解释 | 用途 |
|---|---|---|
| `insert_tps` | 每秒插入数 | 写入吞吐 |
| `delete_tps` | 每秒删除数 | 删除吞吐 |
| `query_tps` | 每秒查询数 | 查询吞吐 |
| `avg_query_ns` | 平均查询延迟 | 基础性能 |
| `p95_query_ns`, `p99_query_ns` | 高尾延迟 | 看是否有局部 rebuild 或 fat objects 造成抖动 |
| `answers_match_boost` | 是否与 Boost R-tree 答案一致 | 正确性底线 |
| `candidate_answer_ratio` | 候选数 / 正确答案数 | false positive 程度 |
| `records_scanned` | 扫描记录数 | 剪枝是否有效 |
| `skipped_block_ratio` | 跳过 block 比例 | block summary 是否有效 |
| `summary_update_ns` | 插入时扩张 summary 的时间 | 动态摘要维护成本 |
| `summary_rebuild_ns` | 局部重算 summary 的时间 | stale cleanup 成本 |
| `summary_rebuild_count` | 局部重算次数 | 删除维护压力 |
| `dead_entry_count` | tombstone/dead records 数量 | 删除堆积程度 |
| `dead_entry_ratio` | dead entries / total entries | stale 程度 |
| `stale_block_count` | stale block 数量 | 查询退化风险 |
| `max_dead_count_per_block` | 单个 block 最大 dead count | 尾延迟风险 |
| `memory_bytes` | 索引内存 | 与 R-tree/GLIN 对比 |

对初学者来说，可以先记住三类指标：

```text
1. 正确性：answers_match_boost
2. 前台性能：query/update throughput, avg/p95/p99 latency
3. 维护代价：summary_rebuild_count, summary_rebuild_ns, dead_entry_ratio
```

## 11. 下一步代码任务

### P0：整理现有查询实验

目标：确认查询侧已经足够作为 DELI-Single / DELI-Overflow 的基础。

要做：

- 保留 `IO_BLOCK_MBR` 和 `IO_OVERFLOW` 的代码名，但 summary/plot 文档中标注论文名映射。
- 正式重跑 `ZGAP_MIXED` 1M，确保 `loaded_count = 1000000`。
- 输出每个 selectivity 下的：
  - avg query time
  - candidate/answer ratio
  - skipped block ratio
  - overflow count
  - answers_match_boost

### P1：新增 DELI-Dynamic-Single prototype

已新增文件：

```text
src/benchmark/bench_dynamic_extent_wkt.cpp
```

该文件是独立 benchmark，不影响 `bench_interval_overlap_wkt.cpp` 和 `bench_update_wkt.cpp`。

第一版支持：

```text
bulk_load initial ratio
insert ratio
delete ratio
query-after-update checkpoints
stale_threshold
block_size
```

输出 CSV 至少包含：

```text
dataset,index,operation,loaded_count,live_count,total_entries,dead_entries,
block_size,stale_threshold,queries,updates,deletes,
avg_query_ns,p95_query_ns,p99_query_ns,
insert_tps,delete_tps,mixed_tps,
records_scanned,skipped_block_ratio,candidate_answer_ratio,
summary_update_ns,summary_rebuild_ns,summary_rebuild_count,
stale_block_count,dead_entry_ratio,answers_match_boost
```

### P2：动态正确性实验

最小实验：

```text
bulk load 50%
insert 10%
query checkpoint
insert 20%
query checkpoint
delete 10%
query checkpoint
```

每个 checkpoint 都和 Boost R-tree 对答案：

```text
answers_match_boost = 1
```

如果这个做不到，后面所有性能结果都没有意义。

### P3：动态性能实验

至少跑：

```text
read-heavy:
  90% query + 10% update

balanced:
  50% query + 50% update

write-heavy:
  10% query + 90% update
```

对比方法：

```text
DELI-Dynamic-Single
GLIN / GLIN_PIECEWISE
Boost_Rtree
GEOS_Quadtree
```

注意：`GLIN_LSM_BG` 这种 system-level async variant 可以放在扩展实验，不要作为主 protocol baseline，否则审稿人可能质疑多线程/后台维护不公平。

### P4：stale threshold sensitivity

测试：

```text
stale_threshold = 0, 0.05b, 0.1b, 0.2b, 0.5b
```

看 trade-off：

```text
threshold 小:
  rebuild 更频繁，update 慢，但 query 更稳。

threshold 大:
  update 更快，但 stale summaries 偏大，query 可能多扫。
```

这组实验是动态维护机制最关键的 ablation。

### P5：再考虑 multi-entry intervalization

只有 P1-P4 跑通后，再做：

```text
object_cover_budget = 1, 2, 4, 8, 16
```

需要新增：

```text
object_id -> entry list
query candidate object-id dedup
entry expansion ratio
```

multi-entry 是增强，不要一开始就做，否则 debugging 会很痛苦。

## 12. 论文结构建议

```text
1. Introduction
   - learned spatial indexes are lightweight and update-friendly
   - complex geometry Intersects is not a point-key problem
   - GLIN-style Zmin indexing suffers from extent blindness
   - propose DELI

2. Background and Motivation
   - ALEX-style learned ordered index
   - GLIN-style Zmin indexing
   - query augmentation and fat-object pollution
   - update-order sensitivity

3. Extent-Aware Representation
   - geometry/query conservative interval cover
   - extent entry <lower, upper, object_id, entry_id>
   - block summaries

4. Query Processing
   - interval-overlap pruning
   - block MBR / record MBR
   - exact GEOS refinement
   - correctness

5. Dynamic Maintenance
   - insert
   - delete
   - geometry update = delete + insert
   - stale-large summaries
   - local rebuild
   - split/merge summary recomputation

6. System Extensions
   - fat-object overflow
   - optional buffered/LSM-style updates

7. Evaluation
   - query performance
   - dynamic correctness
   - update and mixed workload
   - stale threshold sensitivity
   - memory/build/tail latency
   - failure cases

8. Related Work

9. Conclusion
```

## 13. 当前应避免的坑

不要继续优先做：

```text
继续调 DELTA_SIZE
继续堆 GLIN_LSM_* variant
继续调 OVERFLOW_FRACTIONS 来追单张图
现在就做 KNN
现在就做 multi-entry cover
```

优先级应该是：

```text
动态正确性 > 动态指标 > stale threshold > mixed workload > multi-entry cover
```

## 14. Citation TODO

后续写论文或正式技术报告时，需要补齐以下引用：

- ALEX paper / official repository：说明 ALEX 是 dynamic learned ordered index，支持 insert/update/delete。
- GLIN paper / repository：说明 GLIN 使用 ALEX-style index，并通过 Z-address / piecewise query augmentation 支持 complex geometry。
- PVLDB systems paper guidelines：说明系统论文强调 working prototype 和 empirical evaluation，不要求很重的理论证明。
- R-tree / Quadtree / spatial learned index related work。

当前文档先不保留不完整的 `[GitHub][1]`、`[arXiv][3]` 之类占位，避免以后误以为 citation 已经齐全。

## 15. 最终建议

下一步不要把目标写成：

```text
继续优化 GLIN 查询速度。
```

应该写成：

```text
实现并验证 DELI-Dynamic-Single：
一个在 ALEX/GLIN-style learned ordered layout 上维护 extent entries 和 one-sided summaries 的动态 learned spatial access path。
```

如果 DELI-Dynamic-Single 能证明：

```text
1. 查询后答案和 Boost R-tree 一致；
2. 插入/删除后答案仍一致；
3. stale-large summary 的性能退化可控；
4. 查询、更新、混合 workload 上有清晰 trade-off；
```

那么论文主线就会比“GLIN 上加一个查询过滤器”强很多。
