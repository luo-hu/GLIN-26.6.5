# HIRE-SFC-Lite baseline 复现计划

最后更新日期：2026-07-08

本文档回答一个具体问题：HIRE 能否作为“非点空间对象先通过空间填充曲线线性化，再套用一维 learned index”的 baseline，并给出当前 GLIN-26.6.5 项目中可落地的简化复现路线。

结论先写清楚：

```text
HIRE 可以作为一维 learned index 后端的设计参考，
但不能直接作为空间索引 baseline。
```

更准确的 baseline 名称建议是：

```text
HIRE_SFC_LITE
```

它表示：

```text
geometry -> SFC interval / key -> HIRE-inspired 1D learned ordered index -> GEOS exact refinement
```

不要在论文或实验中声称这是完整 HIRE。更稳妥的说法是：

```text
HIRE-SFC-Lite is a HIRE-inspired one-dimensional learned-index baseline
wrapped with space-filling-curve linearization and conservative spatial refinement.
```

## 1. 从 HIRE 论文中能借什么

论文路径：

```text
/home/lh/Documents/references/HIRE A Hybrid Learned Index for Robust and Efficient Indexing.pdf
```

HIRE 的核心对象是一维有序 key-value 记录：

```text
record = <key, value>
```

论文目标是让 learned index 在 mixed workloads 下保持稳定的 range query、insert、delete 性能。它的主要机制包括：

1. balanced-tree framework。
2. hybrid leaf nodes：线性好拟合的区域用 model-based leaf，不好拟合的区域退回 legacy B+-tree-like leaf。
3. model-accelerated internal nodes。
4. log/buffer-based updates。
5. mask-based deletion。
6. cost-driven non-blocking recalibration。
7. inter-level optimized bulk loading。

这些机制和我们的需求有一个契合点：

```text
如果空间对象已经被映射成一维 key，HIRE 可以作为一维有序索引后端。
```

但是 HIRE 论文不是空间索引论文。它没有直接解决以下问题：

1. polygon / linestring 怎么变成不会漏答案的一维范围。
2. query window 在线性化后可能变成多个不连续 SFC interval。
3. SFC key 只是空间相交的必要条件，不是充分条件。
4. exact Intersects 仍然需要 GEOS filter-and-refine。

因此，HIRE 本身不能替代 R-tree baseline。它只能作为下面这种组合 baseline 的 1D index 部分：

```text
SFC linearization + HIRE-like 1D learned index + conservative candidate filtering + GEOS refinement
```

## 2. 当前项目里的相关基础

当前项目已经有以下可复用组件：

```text
src/core/projection.h
```

其中 `curve_shape_projection(...)` 可以把 geometry 或 query envelope 映射成 Z-order / Hilbert-like 区间：

```text
geometry MBR -> [zmin, zmax]
query MBR    -> [qmin, qmax]
```

当前项目也已经有 interval-overlap 方向的实现和文档：

```text
src/benchmark/bench_interval_overlap_wkt.cpp
src/benchmark/bench_dynamic_extent_wkt.cpp
docs/interval_overlap_index_design.md
```

这些实现已经说明，对复杂 geometry 做安全空间剪枝时，只用 `zmin` 不够。为了不漏答案，至少要保留：

```text
record.zmin
record.zmax
record.MBR
record.geometry_id
```

查询时必须满足必要条件：

```text
record.zmin <= query.zmax
record.zmax >= query.zmin
record.MBR intersects query.MBR
```

最后再用 GEOS exact predicate：

```text
geometry.intersects(query_geometry)
```

## 3. 为什么不能做成纯 HIRE-on-zmin

一个看似简单的 baseline 是：

```text
key = geometry.zmin
HIRE.range_query(query.zmin, query.zmax)
GEOS refine
```

这个版本不安全，会漏答案。

原因是：一个长线或大 polygon 可能满足：

```text
geometry.zmin < query.zmin
geometry.zmax >= query.zmin
```

它可能真实和 query 相交，但它的 `zmin` 不在 `[query.zmin, query.zmax]` 内。如果只按 `zmin` 范围查 HIRE，就会漏掉它。

所以 HIRE-SFC-Lite 必须至少采用 extent-aware wrapper：

```text
primary learned key = zmin
candidate condition = zmin <= query.zmax && zmax >= query.zmin
```

这仍然是一维 learned index baseline，因为索引定位和有序布局基于 `zmin`，但记录 payload 中保存了 spatial extent，用于安全过滤。

## 4. 推荐 baseline 定义

建议新增 index 字符串：

```text
HIRE_SFC_LITE
```

定义：

```text
HIRE_SFC_LITE:
    Space filling curve: Z-order, reuse current curve_shape_projection
    Primary key: zmin
    Payload: {zmin, zmax, MBR, object_id}
    1D index: HIRE-inspired hybrid learned ordered leaf directory
    Query: interval-overlap filter + MBR filter + GEOS exact refinement
    Update: insert into leaf delta/buffer; lazy delete; local rebuild when needed
```

可选第二个名字：

```text
HIRE_SFC_ZMIN_UNSAFE
```

这个只用于 debug，不建议进入论文图。它用纯 `zmin` range query，会有 false negatives，不能作为 correctness baseline。

## 5. 实现阶段划分

不要一口气复现完整 HIRE。完整 HIRE 包含 RCU、后台线程、internal log、cost-driven recalibration 和 inter-level optimized bulk-loading，工程量很大，而且和空间 baseline 的关键问题不是同一个层面。

推荐分四阶段实现。

### Stage 0：确认线性化和安全过滤

目标：

```text
确认 SFC extent wrapper 的 correctness。
```

可以直接参考：

```text
src/benchmark/bench_interval_overlap_wkt.cpp
src/benchmark/bench_dynamic_extent_wkt.cpp
```

记录结构：

```cpp
struct HireSfcRecord {
  double zmin;
  double zmax;
  Envelope mbr;
  std::size_t id;
  bool alive;
};
```

查询流程：

```text
1. query geometry/envelope -> [qmin, qmax]
2. 找到所有 zmin <= qmax 的候选范围
3. 对 block/leaf 做 summary skip:
      leaf.max_zmax < qmin -> skip
      leaf.mbr does not intersect query.mbr -> skip
4. 对 leaf 内记录做 record-level filter:
      record.zmin > qmax -> skip/break
      record.zmax < qmin -> skip
      record.MBR not intersect query.MBR -> skip
5. GEOS exact refinement
```

正确性不变式：

```text
任何被 summary 跳过的 leaf 一定不包含真答案。
```

summary 更新时必须遵守：

```text
max_zmax 可以 stale-large，不能 stale-small。
MBR 可以 stale-large，不能 stale-small。
```

### Stage 1：HIRE-like leaf layer

目标：

```text
把 sorted vector / binary-search baseline 换成 HIRE-inspired hybrid leaves。
```

叶子节点：

```cpp
enum class HireLeafKind {
  Model,
  Legacy
};

struct HireSfcLeaf {
  HireLeafKind kind;
  std::vector<std::size_t> record_ids;
  std::vector<std::size_t> buffer_ids;
  double min_zmin;
  double max_zmin;
  double max_zmax;
  Envelope mbr;
  double slope;
  double intercept;
  double max_error;
  std::size_t live_count;
  std::size_t tombstone_count;
};
```

构建规则：

```text
1. 所有 live records 按 zmin 排序。
2. 从左到右尝试拟合 zmin -> position 的 linear model。
3. 如果当前 segment 的 max error <= HIRE_SFC_EPSILON，建 Model leaf。
4. 如果拟合失败或 segment 太小，建 Legacy leaf。
5. 每个 leaf 维护 max_zmax 和 MBR summary。
```

第一版可以简化为：

```text
leaf_size 固定，例如 512 / 1024。
每个 leaf 尝试拟合一个线性模型。
拟合好则 kind=Model，否则 kind=Legacy。
```

Model leaf 的查询：

```text
predict(qmin/qmax) -> local correction -> leaf-local scan
```

Legacy leaf 的查询：

```text
binary search / linear scan
```

注意：对空间查询来说，leaf-level summary skip 往往比单条 key 的预测更重要。第一版不需要追求 HIRE 论文中的所有内部优化。

### Stage 2：HIRE-like directory

目标：

```text
用 learned directory 加速定位 zmin <= qmax 的 leaf 边界。
```

简化目录：

```cpp
struct HireSfcDirectory {
  std::vector<HireSfcLeaf*> leaves;
  std::vector<double> leaf_max_zmin;
  double slope;
  double intercept;
  double max_error;
};
```

查询上界定位：

```text
leaf_id = model.predict(qmax)
在 [leaf_id - max_error, leaf_id + max_error] 内 correction
得到最后一个 max_zmin <= qmax 的 leaf
```

如果模型误差过大：

```text
fallback 到 binary search over leaf_max_zmin
```

这对应 HIRE 的思想：

```text
model-accelerated internal node, but keep worst-case fallback.
```

### Stage 3：动态更新

目标：

```text
支持 mixed workload 下 insert/delete/query。
```

插入：

```text
1. 新 geometry -> {zmin, zmax, MBR, id}
2. 用 directory 找到 zmin 所属 leaf
3. append 到 leaf.buffer_ids
4. 更新 leaf summary:
      max_zmax = max(max_zmax, record.zmax)
      mbr.expand(record.mbr)
5. buffer 超过阈值后 local rebuild leaf
```

删除：

```text
1. alive[id] = false
2. leaf.tombstone_count++
3. 不立即缩小 max_zmax / MBR
4. tombstone ratio 超过阈值后 local rebuild leaf
```

local rebuild：

```text
1. 合并 leaf.record_ids 和 leaf.buffer_ids
2. 过滤 dead records
3. 重新按 zmin 排序
4. 重新拟合 Model/Legacy leaf
5. 重新计算 exact summary
```

第一版不做 HIRE 的 RCU / background retraining。原因：

```text
当前 benchmark 是单线程前台执行，先验证 baseline 行为更重要。
```

### Stage 4：更接近 HIRE 的可选增强

如果 Stage 1-3 已经跑通，再考虑：

```text
1. internal node log
2. model leaf deleted-slot reuse
3. cost-driven retraining trigger
4. legacy leaf -> model leaf transformation
5. inter-level optimized bulk-loading
```

这些增强可以作为后续优化，不应该挡住第一版 baseline。

当前代码已经实现一组可开关的 Stage 4 近似增强：

```text
1. internal node log：
   directory update log 记录被 insert 扩展过的 leaf 边界，查询定位时把 learned directory
   和 log 的结果合并；log 超过阈值后 rebuild directory。

2. model leaf deleted-slot reuse：
   model leaf 中保留 mask/tombstone 风格的 deleted slot。如果新插入对象的 zmin
   不破坏 leaf 内有序性，则直接复用 tombstone slot，避免进入 delta buffer。

3. cost-driven retraining trigger：
   leaf 记录 recent query count 和 buffer size。当 active query * buffer size
   超过重建成本的简化估计时，触发 local rebuild。

4. legacy leaf -> model leaf transformation：
   local rebuild 或 bulk loading 后，会尝试把相邻 legacy leaf 合并拟合成一个
   model leaf。拟合误差满足 HIRE_SFC_EPSILON 才转换。

5. inter-level optimized bulk-loading：
   bulk loading 时先尝试更大的 model leaf segment；如果拟合失败，再回退到
   HIRE_SFC_LEAF_SIZE 固定叶大小。

6. HIRE RCU / background recalibration：
   当前 benchmark 是单线程，所以没有实现真正并发 RCU。代码提供
   pending_rebuild 近似：触发重建时先标记 leaf，后续 update 逐步 apply 一个
   pending rebuild。这个只用于模拟 non-blocking recalibration 的效果。
```

新增参数：

```text
HIRE_SFC_MODEL_LEAF_SIZE=2048
HIRE_SFC_ACTIVE_QUERY_THRESHOLD=32
HIRE_SFC_ACTIVE_BUFFER_THRESHOLD=32
HIRE_SFC_DIRECTORY_LOG_LIMIT=256
HIRE_SFC_LEGACY_TRANSFORM_MAX_LEAVES=4
HIRE_SFC_ENABLE_DIRECTORY_LOG=1
HIRE_SFC_ENABLE_DELETED_SLOT_REUSE=1
HIRE_SFC_ENABLE_COST_RETRAIN=1
HIRE_SFC_ENABLE_LEGACY_TRANSFORM=1
HIRE_SFC_ENABLE_BACKGROUND_RECALIBRATION=0
```

对应 debug 字段写入 `hire_sfc_debug.csv`：

```text
directory_log_entries
directory_rebuild_count
deleted_slot_reuse_count
cost_retrain_trigger_count
legacy_transform_count
pending_rebuild_count
background_recalibration_count
```

## 6. 接入当前 benchmark

建议新增文件：

```text
src/benchmark/hire_sfc_lite_index.h
```

在下面文件中接入 index 字符串：

```text
src/benchmark/bench_dynamic_compare_wkt.cpp
```

新增：

```text
INDEXES="HIRE_SFC_LITE"
```

输出字段复用现有 summary schema：

```text
avg_query_ms
p95_query_ms
p99_query_ms
query_tps
insert_tps
delete_tps
mbr_candidates
exact_calls
answers_match_boost
tree_nodes / block_count
index_bytes_estimate
```

不要改主 summary CSV schema。额外 debug 信息写：

```text
hire_sfc_debug.csv
```

建议 debug 字段：

```text
dataset
index
checkpoint
leaf_count
model_leaf_count
legacy_leaf_count
avg_model_error
max_model_error
buffer_records
tombstone_records
local_rebuild_count
skipped_zmax_leaves
skipped_mbr_leaves
visited_leaves
records_scanned
```

## 7. 参数建议

环境变量：

```text
HIRE_SFC_LEAF_SIZE=512
HIRE_SFC_EPSILON=32
HIRE_SFC_MIN_MODEL_LEAF=128
HIRE_SFC_BUFFER_LIMIT=128
HIRE_SFC_TOMBSTONE_REBUILD_RATIO=0.25
HIRE_SFC_CURVE=z
HIRE_SFC_ENABLE_ZMAX_SKIP=1
HIRE_SFC_ENABLE_MBR_SKIP=1
HIRE_SFC_FORCE_LEGACY=0
HIRE_SFC_SEED=42
```

消融实验：

```text
HIRE_SFC_FORCE_LEGACY=1
```

这相当于：

```text
SFC + B+-tree-like leaves + extent summary
```

可以用来判断 learned leaf/directory 是否真的带来收益。

## 8. Correctness 测试

第一组 smoke：

```bash
RESET_RESULTS=1 OVERWRITE=1 AUTO_BUILD=1 \
DATASETS="AW" LIMIT=10000 QUERY_LIMIT=10000 \
QUERY_ROOT=queries/smoke_hire_sfc_lite \
RESULT_DIR=results/smoke_hire_sfc_lite \
FIGURE_DIR=figures/smoke_hire_sfc_lite \
SELECTIVITY_TAGS="1pct" QUERY_COUNT=20 \
AUTO_GENERATE_QUERIES=1 CHECK_CORRECTNESS=1 \
INDEXES="Boost_Rtree HIRE_SFC_LITE" \
HIRE_SFC_LEAF_SIZE=512 HIRE_SFC_EPSILON=32 \
HIRE_SFC_BUFFER_LIMIT=128 \
BUILD_DIR=build_current \
./scripts/run_dynamic_compare_diagnostics.sh
```

必须满足：

```text
answers_match_boost=1
missing_count=0
extra_count=0
```

第二组 mixed smoke：

```bash
RESET_RESULTS=1 OVERWRITE=1 AUTO_BUILD=1 \
WORKLOAD_MODE=mixed MIXED_PROFILES="read_heavy" \
MIXED_OPERATIONS=20000 MIXED_CHECKPOINT_INTERVAL=5000 \
DATASETS="LW PARKS" LIMIT=50000 QUERY_LIMIT=50000 \
QUERY_ROOT=queries/interval_overlap_full_50000 \
RESULT_DIR=results/hire_sfc_lite_mixed_0.05m \
FIGURE_DIR=figures/hire_sfc_lite_mixed_0.05m \
SELECTIVITY_TAGS="0p01pct 1pct" QUERY_COUNT=100 \
AUTO_GENERATE_QUERIES=1 CHECK_CORRECTNESS=1 \
INDEXES="Boost_Rtree HIRE_SFC_LITE DELI_ALEX_HYBRID_COST" \
HIRE_SFC_LEAF_SIZE=512 HIRE_SFC_EPSILON=32 \
HIRE_SFC_BUFFER_LIMIT=128 \
BUILD_DIR=build_current \
./scripts/run_dynamic_compare_diagnostics.sh
```

Stage 4 增强 smoke：

```bash
RESET_RESULTS=1 OVERWRITE=1 AUTO_BUILD=0 \
WORKLOAD_MODE=mixed MIXED_PROFILES="read_heavy" \
MIXED_OPERATIONS=5000 MIXED_CHECKPOINT_INTERVAL=2500 \
DATASETS="AW" LIMIT=10000 QUERY_LIMIT=10000 \
QUERY_ROOT=queries/smoke_hire_sfc_lite_enhanced_mixed \
RESULT_DIR=results/smoke_hire_sfc_lite_enhanced_mixed \
FIGURE_DIR=figures/smoke_hire_sfc_lite_enhanced_mixed \
SELECTIVITY_TAGS="1pct" QUERY_COUNT=20 \
AUTO_GENERATE_QUERIES=1 CHECK_CORRECTNESS=1 \
INDEXES="Boost_Rtree HIRE_SFC_LITE" \
HIRE_SFC_LEAF_SIZE=512 HIRE_SFC_MODEL_LEAF_SIZE=2048 \
HIRE_SFC_EPSILON=32 HIRE_SFC_BUFFER_LIMIT=64 \
HIRE_SFC_ACTIVE_QUERY_THRESHOLD=4 \
HIRE_SFC_ACTIVE_BUFFER_THRESHOLD=8 \
BUILD_DIR=build_current \
./scripts/run_dynamic_compare_diagnostics.sh
```

如果要专门验证 deleted-slot reuse，可以用更宽松的 model 拟合参数，并暂时关闭
cost retrain，避免 tombstone 过早被 rebuild 清掉：

```bash
RESET_RESULTS=1 OVERWRITE=1 AUTO_BUILD=0 \
WORKLOAD_MODE=mixed MIXED_PROFILES="write_heavy" \
MIXED_OPERATIONS=8000 MIXED_CHECKPOINT_INTERVAL=4000 \
DATASETS="AW" LIMIT=10000 QUERY_LIMIT=10000 \
QUERY_ROOT=queries/smoke_hire_sfc_lite_reuse \
RESULT_DIR=results/smoke_hire_sfc_lite_reuse \
FIGURE_DIR=figures/smoke_hire_sfc_lite_reuse \
SELECTIVITY_TAGS="1pct" QUERY_COUNT=20 \
AUTO_GENERATE_QUERIES=1 CHECK_CORRECTNESS=1 \
INDEXES="Boost_Rtree HIRE_SFC_LITE" \
HIRE_SFC_LEAF_SIZE=256 HIRE_SFC_MODEL_LEAF_SIZE=2048 \
HIRE_SFC_MIN_MODEL_LEAF=16 HIRE_SFC_EPSILON=1000000 \
HIRE_SFC_BUFFER_LIMIT=100000 HIRE_SFC_TOMBSTONE_REBUILD_RATIO=2.0 \
HIRE_SFC_ENABLE_COST_RETRAIN=0 \
BUILD_DIR=build_current \
./scripts/run_dynamic_compare_diagnostics.sh
```

如果要验证 background recalibration 近似路径：

```bash
RESET_RESULTS=1 OVERWRITE=1 AUTO_BUILD=0 \
WORKLOAD_MODE=mixed MIXED_PROFILES="read_heavy" \
MIXED_OPERATIONS=5000 MIXED_CHECKPOINT_INTERVAL=2500 \
DATASETS="AW" LIMIT=10000 QUERY_LIMIT=10000 \
QUERY_ROOT=queries/smoke_hire_sfc_lite_background \
RESULT_DIR=results/smoke_hire_sfc_lite_background \
FIGURE_DIR=figures/smoke_hire_sfc_lite_background \
SELECTIVITY_TAGS="1pct" QUERY_COUNT=20 \
AUTO_GENERATE_QUERIES=1 CHECK_CORRECTNESS=1 \
INDEXES="Boost_Rtree HIRE_SFC_LITE" \
HIRE_SFC_LEAF_SIZE=256 HIRE_SFC_MODEL_LEAF_SIZE=2048 \
HIRE_SFC_MIN_MODEL_LEAF=16 HIRE_SFC_EPSILON=1000000 \
HIRE_SFC_BUFFER_LIMIT=16 \
HIRE_SFC_ACTIVE_QUERY_THRESHOLD=1 \
HIRE_SFC_ACTIVE_BUFFER_THRESHOLD=1 \
HIRE_SFC_ENABLE_BACKGROUND_RECALIBRATION=1 \
BUILD_DIR=build_current \
./scripts/run_dynamic_compare_diagnostics.sh
```

本轮验证结果：

```text
staged smoke:
  results/smoke_hire_sfc_lite_enhanced/dynamic_compare_summary.csv
  after_bulkload / after_insert / after_delete 均 answers_match_boost=1。

mixed smoke:
  results/smoke_hire_sfc_lite_enhanced_mixed/dynamic_compare_summary.csv
  mixed_2500 / mixed_5000 均 answers_match_boost=1。
  hire_sfc_debug.csv 中 cost_retrain_trigger_count 到 24。

deleted-slot reuse smoke:
  results/smoke_hire_sfc_lite_reuse/hire_sfc_debug.csv
  deleted_slot_reuse_count 到 87。

background recalibration smoke:
  results/smoke_hire_sfc_lite_background/hire_sfc_debug.csv
  background_recalibration_count 到 59，cost_retrain_trigger_count 到 56。
```

## 9. 预期结果和论文表述

HIRE-SFC-Lite 适合作为下面这个实验问题的 baseline：

```text
如果只把复杂 geometry 线性化成一维 key，
再使用一个强的一维 learned ordered index，
是否足以处理 exact spatial predicate query？
```

预期现象：

1. 在点状或小 MBR 对象上，HIRE-SFC-Lite 可能表现不错。
2. 在长线、大 polygon、ZGAP_MIXED 这类 fat-object workload 上，单纯 SFC 线性化会产生很多 false positives。
3. 如果没有 extent-aware summary，纯 zmin learned index 会不安全或需要粗糙 query augmentation。
4. 如果加上 `zmax` 和 MBR summary，它会接近当前 DELI/interval-overlap 思路，但缺少 DELI-Cost 的 adaptive partition、local compaction 和 PRL。

推荐论文表述：

```text
We include HIRE-SFC-Lite, a HIRE-inspired one-dimensional learned-index
baseline over space-filling-curve keys. Each geometry is represented by
its Z-order extent and MBR, while the learned layout is built over zmin.
The baseline uses conservative interval and MBR filters followed by GEOS
exact refinement. It is intended to test whether a robust 1D learned index
backend alone is sufficient after spatial linearization.
```

中文说明：

```text
HIRE-SFC-Lite 不是完整 HIRE，也不是原生空间索引。
它的作用是验证“空间填充曲线线性化 + 一维 learned ordered index”
这条路线在复杂 geometry exact predicate workload 下的上限和短板。
```

## 10. 和已有 baseline 的关系

对比关系建议：

```text
Boost_Rtree:
    原生空间索引 baseline。

RLR_LITE_CS / RLR_LITE_CS_SPLIT:
    R-tree 结构上加 RL-inspired 插入/分裂策略。

GLIN_PIECEWISE:
    现有 learned spatial index baseline，偏 Zmin/query augmentation。

HIRE_SFC_LITE:
    新增 baseline。SFC linearization + HIRE-inspired robust 1D learned index。

DELI_ALEX_HYBRID_COST:
    本文方法或强方法。显式维护 extent summary、动态 local maintenance 和 cost-driven policy。
```

HIRE-SFC-Lite 的实验价值不是一定要赢，而是回答：

```text
换一个更强的一维 learned index 后，
复杂 geometry workload 的瓶颈是否仍然来自空间 extent / false positives / exact refinement？
```

如果 HIRE-SFC-Lite 仍然在 `mbr_candidates`、`exact_calls` 或 tail latency 上落后 DELI-Cost，就能支持本文主张：

```text
复杂 geometry 的动态 exact spatial query 不能只靠一维 learned index 后端解决，
还需要 extent-aware maintenance 和 predicate-aware refinement。
```
