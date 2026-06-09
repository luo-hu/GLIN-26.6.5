# GLIN Interval-Overlap 优化设计草案

最后更新日期：2026-06-08

本文档说明一个新的 GLIN 优化方向：把 `Intersects` 查询从 GLIN 当前的 `Zmin-only + query augmentation` 思路，改成 `interval-overlap-aware learned index`，也就是显式利用 `[Zmin, Zmax]` 区间重叠条件做安全剪枝。

固定术语解释：

- `Zmin`: geometry 的 Z-order 区间起点。
- `Zmax`: geometry 的 Z-order 区间终点。
- `query interval [L, R]`: 查询窗口映射到 Z-order 后的区间。
- `interval overlap`: 区间重叠。geometry `[Zmin, Zmax]` 和 query `[L, R]` 可能相交的必要条件是 `Zmin <= R && Zmax >= L`。
- `safe pruning`: 安全剪枝。被跳过的数据一定不包含真答案，也就是不会产生 false negative。
- `false negative`: 漏答案。
- `false positive`: 多返回候选，后续还需要 exact refinement 过滤。
- `exact refinement`: 用 GEOS 的 `intersects` 等精确几何谓词做最终判断。

## 1. 这个创新点针对 GLIN 的哪个痛点

当前 GLIN 的主要问题是：主索引主要建立在 `Zmin` 上，对 `Zmax` 利用弱。

当前 GLIN 的查询路径大致是：

```text
geometry -> [Zmin, Zmax]
query -> [L, R]
主索引按 Zmin 查找
Intersects 时通过 piecewise query augmentation 扩大左边界
扫描候选
GEOS exact refinement
```

问题是：

```text
为了不漏掉 Zmin 很小但 Zmax 很大的对象，GLIN 必须把查询左边界向左扩展。
这个扩展是安全的，但比较粗，会带来很多额外候选和 false positives。
```

你的新想法是把查询条件显式改成 interval overlap：

```text
geometry interval: [Zmin, Zmax]
query interval:    [L, R]

candidate condition:
Zmin <= R && Zmax >= L
```

这比只看 `Zmin` 更接近 `Intersects` 查询本质。

## 2. 当前代码中的 GLIN 查询执行路径

相关文件：

```text
glin/glin.h
src/core/projection.h
src/benchmark/bench_glin_wkt.cpp
src/benchmark/bench_hybrid_wkt.cpp
```

当前路径：

```text
1. benchmark 入口：
   bench_glin_wkt.cpp 或 bench_hybrid_wkt.cpp

2. 数据加载：
   WKT -> GEOS Geometry

3. key 计算：
   curve_shape_projection(geometry, "z", ...) 计算 geometry 的 [Zmin, Zmax]

4. 建索引：
   glin_bulk_load(...)
   loadCurve(...) 中按 Zmin bulk_load 到 ALEX/GLIN
   PIECE 模式下还会构建 piecewise augmentation 信息

5. 查询：
   glin_find(...)
   index_probe_curve(...) 先计算 query 的 [L, R]
   PIECE 模式下通过 pieces 把 L 向左扩展
   refine_with_curveseg(...) 扫描候选并做 leaf MBR skip + GEOS intersects

6. 输出：
   CSV 记录 probe_ns、refine_ns、candidates、answers、visited_leaf 等指标
```

这说明新方法有两个可实现入口：

```text
入口 A：先做一个独立 benchmark 原型，不改 GLIN 内核。
入口 B：后续把 block summary 接入 glin/glin.h 的 data node / iterator。
```

我建议先走入口 A。

## 3. 推荐的第一阶段实现：Block Interval Index 原型

第一阶段目标不是马上做完整 learned dynamic index，而是先验证核心假设：

> 用 block-level maxZmax 能不能比 GLIN 的 query augmentation 少扫候选？

### 3.1 数据结构

先实现一个独立 benchmark，例如：

```text
src/benchmark/bench_interval_overlap_wkt.cpp
```

新增 CMake target：

```text
bench_interval_overlap_wkt
```

索引结构：

```cpp
struct IntervalRecord {
  double zmin;
  double zmax;
  geos::geom::Envelope envelope;
  Geometry* geometry;
  std::size_t id;
};

struct IntervalBlock {
  std::size_t begin;
  std::size_t end;
  double min_zmin;
  double max_zmin;
  double max_zmax;
  geos::geom::Envelope mbr;
};
```

整体结构：

```text
records: 按 zmin 排序的 IntervalRecord 数组
blocks: 每 block_size 条 record 建一个 IntervalBlock summary
```

`block_size` 可以先设成：

```text
1024 或 4096
```

并暴露参数：

```text
--block_size 1024
```

### 3.2 查询流程

查询输入是 WKT query window。

流程：

```text
1. 计算 query 的 [L, R]。
2. 在 records 中用 upper_bound 找到最后一个 zmin <= R 的位置。
3. 只考虑这些 block。
4. 对每个 block 做安全剪枝：
      if block.max_zmax < L:
          skip
      if block.mbr 不和 query envelope 相交:
          skip
5. 对没有跳过的 block，扫描 leaf records：
      if record.zmin > R:
          continue/break
      if record.zmax < L:
          continue
      if record.envelope 不和 query envelope 相交:
          continue
      GEOS intersects 精确判断
6. 输出 candidates、answers、skipped_blocks、visited_blocks、exact_calls。
```

关键点：

```text
block.max_zmax < L 是 safe pruning。
它不会漏答案。
```

因为如果 block 里有真答案 `g`，那么它一定满足：

```text
Zmax_g >= L
```

所以：

```text
block.max_zmax >= Zmax_g >= L
```

因此 `block.max_zmax < L` 的 block 不可能包含答案。

### 3.3 为什么第一阶段先不用 learned model

第一阶段可以先用 `upper_bound(zmin <= R)`，不用马上接 ALEX。

原因：

```text
1. 先验证 interval skipping 是否真的减少候选。
2. 避免同时调 learned model、block summary、dynamic update，问题太多。
3. 如果 block skipping 有明显收益，再把 upper_bound 替换成 learned model。
```

论文里可以把这个第一阶段称为：

```text
oracle-free interval-aware block index prototype
```

但在最终系统中，应该把定位部分替换为：

```text
Learned Zmin directory
```

## 4. 第二阶段：接入 Learned Zmin Directory

第一阶段证明有效后，再把 `upper_bound` 替换成 learned index 定位。

可选做法：

```text
方案 A：复用 ALEX/GLIN 的 Zmin bulk_load。
方案 B：自己实现一个轻量 learned block directory。
方案 C：先用 std::vector + binary search 作为 baseline，再加 learned variant。
```

推荐顺序：

```text
1. vector + binary search
2. block directory
3. learned block directory
4. dynamic update
```

不要一开始就把所有东西做进 GLIN 内核，否则很难 debug。

## 5. 第三阶段：Fat Object Overflow

`max_zmax` 的问题是会被长区间对象污染。

例子：

```text
一个 block 里 999 个小对象，1 个超长对象。
这个超长对象让 block.max_zmax 很大。
结果很多 query 都无法跳过这个 block。
```

解决办法是把 fat object 单独分流。

`fat object` 的意思是 Z-order 区间跨度很大的对象：

```text
span_z = Zmax - Zmin
```

推荐架构：

```text
Main Interval Index:
  存普通对象
  按 Zmin 排序
  每个 block 维护 max_zmax / mbr

Overflow Index:
  存 fat objects
  可以先用 vector scan
  后续可以换成 R-tree / interval tree
```

查询时：

```text
1. 查 main interval index。
2. 查 overflow index。
3. 合并去重。
```

阈值不要只用平均 span。更稳的是 percentile：

```text
span_z > quantile(span_z, 1 - rho)
```

例如：

```text
rho = 0.1%, 0.5%, 1%, 5%
```

后续做 sensitivity study，也就是敏感性实验，看不同 rho 下性能是否稳定。

## 6. 第四阶段：Fully Dynamic 安全摘要

如果要主张 `fully dynamic`，必须说明 summary 怎么更新。

核心原则：

```text
summary 可以 stale-large，不能 stale-small。
```

中文解释：

```text
summary 过大只会少剪枝，不会漏答案。
summary 过小可能错误跳过包含答案的 block，会漏答案。
```

插入规则：

```text
如果 new_zmax <= block.max_zmax:
    不需要向上传播。

如果 new_zmax > block.max_zmax:
    必须立即更新 block.max_zmax。
    如果有上层 directory，也要更新成不小于真实值。
```

删除规则：

```text
如果 deleted_zmax < block.max_zmax:
    不需要更新。

如果 deleted_zmax == block.max_zmax:
    可以先保留旧 max_zmax。
    这会让 summary 偏大，不会漏答案。
    stale_count 增加，超过阈值后局部重算。
```

这可以形成论文里的 dynamic correctness argument。

## 7. 不能踩的正确性坑

### 7.1 P90/P95 不能直接整块跳过

`P90 Zmax`、`P95 Zmax`、`body_maxZmax` 这些统计量不能直接用于 exact query 的整块跳过。

反例：

```text
Block 内 Zmax = [10, 11, 12, 100000]
body_maxZmax = 12
global_maxZmax = 100000
query L = 100
```

如果用：

```text
body_maxZmax < L
```

直接跳过整块，就会漏掉 `Zmax=100000` 的对象。

所以必须写清楚：

```text
P90/P95/body_maxZmax 只能作为 cost hint。
除非 outliers 已经被单独分离并单独查询，否则不能作为 safe pruning 条件。
```

### 7.2 不要说 zero false positives

更准确的说法是：

```text
false-negative-free pruning
```

也就是：

```text
被跳过的 block 一定没有答案。
但没有被跳过的 block 仍然可能有 false positives。
```

原因是：

```text
Z-order interval overlap 只是 Intersects 的必要条件，不是充分条件。
MBR 本身也会带来 false positives。
最终仍然需要 GEOS exact refinement。
```

## 8. 建议的实验设计

### 8.1 Baseline

需要比较：

```text
GLIN
GLIN-piecewise
Boost R-tree
GEOS Quadtree
新方法：IntervalOverlapIndex
```

### 8.2 Ablation

`ablation` 是消融实验，意思是逐个加机制，看每个机制到底有没有贡献。

建议：

```text
A0: GLIN-piecewise
A1: Zmin sorted scan / binary-search upper bound
A2: A1 + block maxZmax
A3: A2 + block MBR summary
A4: A3 + fat-object overflow
A5: A4 + dynamic safe summary
```

### 8.3 指标

必须记录：

```text
query latency
probe_ns
refine_ns
candidates
answers
candidate_answer_ratio
visited_blocks
skipped_blocks
exact_predicate_calls
overflow_ratio
build_ns
update throughput
maintenance_ns
```

重点看：

```text
1. candidates 是否减少。
2. exact_predicate_calls 是否减少。
3. skipped_blocks 是否增加。
4. answers 是否和 Boost R-tree 一致。
```

### 8.4 Long Interval Stress Test

必须专门测长区间污染问题。

建议数据：

```text
99% small + 1% fat
95% small + 5% fat
90% small + 10% fat
100% fat
```

这组实验能证明 fat-object overflow 是否有用。

## 9. 论文贡献应该怎么包装

不要直接说：

```text
the first learned interval index
```

这个说法太危险，需要完整 related work 支撑。

更稳的说法：

```text
We propose an interval-overlap-aware learned index for exact spatial
relationship queries over complex geometries, with deterministic block-level
skipping and fully dynamic safe summaries.
```

中文意思：

```text
我们提出一种面向复杂几何对象精确空间关系查询的 interval-overlap-aware learned index。
它通过确定性的 block-level skipping 和动态安全摘要减少候选膨胀。
```

更强但仍相对安全的说法：

```text
To the best of our knowledge, this is the first fully dynamic learned index
that explicitly models Z-address interval overlap for exact Intersects queries
over complex geometries.
```

中文意思：

```text
据我们所知，这是第一个显式建模 Z-address interval overlap，
并支持复杂几何 Intersects 精确查询的 fully dynamic learned index。
```

这句话后续必须通过 related work 支撑。

## 10. 推荐实现路线

### Step 1：实现静态原型

新增：

```text
src/benchmark/bench_interval_overlap_wkt.cpp
```

功能：

```text
1. 读取 WKT 数据。
2. 计算每个 geometry 的 Zmin/Zmax。
3. 按 Zmin 排序。
4. 每 block 建 max_zmax 和 MBR summary。
5. 查询时用 max_zmax + MBR 做安全剪枝。
6. 输出 candidates、answers、visited_blocks、skipped_blocks。
```

目的：

```text
先证明 interval-aware block skipping 能减少 GLIN 的 query augmentation 开销。
```

### Step 2：加入对比脚本

新增脚本：

```text
scripts/run_interval_overlap_diagnostics.sh
```

对比：

```text
GLIN-piecewise
Boost R-tree
IntervalOverlapIndex
```

### Step 3：加入 fat-object overflow

新增参数：

```text
--fat_ratio 0.01
```

或者：

```text
--fat_span_quantile 0.99
```

输出：

```text
overflow_count
overflow_ratio
main_candidates
overflow_candidates
```

### Step 4：加入动态插入

先做简单版本：

```text
delta buffer + periodic block rebuild
```

再做完整版本：

```text
one-sided safe summary update + local rebuild
```

### Step 5：再考虑 learned directory

如果 Step 1 到 Step 4 的候选减少明显，再把 binary search/block directory 替换成 learned directory。

不要一开始就把 learned model 做进去。否则如果效果不好，很难判断是 learned model 问题，还是 interval skipping 问题。

## 11. 当前判断

这个方向值得做，而且比单纯调 `DELTA_SIZE` 或继续优化 GLIN-LSM 更有论文价值。

原因：

```text
1. 它直接攻击 GLIN 的 Intersects 核心短板。
2. 它不是简单工程调参，而是改变查询建模方式。
3. 它有清楚的正确性证明：max_zmax safe pruning 不漏答案。
4. 它有明确的实验指标：candidate reduction、exact call reduction、skipped block count。
5. 它可以和传统 interval tree / segment tree / R-tree / GLIN 做清晰对比。
```

但第一版不要贪大。最合理的第一版是：

```text
Static IntervalOverlapIndex:
  sorted Zmin records
  block maxZmax
  block MBR
  exact GEOS refinement
```

这个版本做出来以后，如果 candidates 和 exact calls 明显下降，再继续做：

```text
fat-object overflow
dynamic safe summaries
learned block directory
```

## 12. 当前已实现的第一版原型

已经新增独立 benchmark：

```text
src/benchmark/bench_interval_overlap_wkt.cpp
```

已经新增 CMake target：

```text
bench_interval_overlap_wkt
```

编译命令：

```bash
cmake -S . -B build
cmake --build build --target bench_interval_overlap_wkt -j2
```

当前原型实现了：

```text
1. 读取 WKT 数据。
2. 计算每个 geometry 的 [Zmin, Zmax]。
3. 按 Zmin 排序。
4. 按 block_size 分块。
5. 每个 block 维护 max_zmax 和 MBR。
6. 查询时使用 upper_bound 找到 Zmin <= R 的前缀范围。
7. 使用 block.max_zmax < L 做 safe pruning。
8. 使用 block MBR 和 record MBR 做安全过滤。
9. 最后使用 GEOS intersects 做 exact refinement。
```

当前输出指标：

```text
prefix_records
prefix_blocks
visited_blocks
skipped_zmax_blocks
skipped_mbr_blocks
records_scanned
interval_candidates
mbr_candidates
exact_calls
answers
candidate_answer_ratio
```

这些指标的用途：

- `prefix_records`: 只按 `Zmin <= R` 会进入前缀范围的记录数。
- `visited_blocks`: 真正扫描的 block 数。
- `skipped_zmax_blocks`: 被 `maxZmax < L` 安全跳过的 block 数。
- `records_scanned`: block 剪枝后真正进入 leaf 扫描的记录数。
- `exact_calls`: 调用了多少次 GEOS exact predicate。
- `answers`: 真答案数。

Smoke test 命令：

```bash
./build/bench_interval_overlap_wkt \
  --data_file /tmp/glin_lsm_hybrid_smoke.wkt \
  --query_file /tmp/glin_lsm_hybrid_query_1pct.csv \
  --dataset_name INTERVAL_SMOKE \
  --limit 1000 \
  --block_size 64 \
  --output_csv /tmp/interval_overlap_smoke.csv
```

关闭 block skip 的对照命令：

```bash
./build/bench_interval_overlap_wkt \
  --data_file /tmp/glin_lsm_hybrid_smoke.wkt \
  --query_file /tmp/glin_lsm_hybrid_query_1pct.csv \
  --dataset_name INTERVAL_SMOKE_NO_BLOCK_SKIP \
  --limit 1000 \
  --block_size 64 \
  --disable_zmax_skip 1 \
  --disable_block_mbr_skip 1 \
  --output_csv /tmp/interval_overlap_smoke_noskip.csv
```

Boost R-tree 正确性对照：

```bash
./build/bench_boost_rtree_wkt \
  --data_file /tmp/glin_lsm_hybrid_smoke.wkt \
  --query_file /tmp/glin_lsm_hybrid_query_1pct.csv \
  --dataset_name INTERVAL_SMOKE \
  --limit 1000 \
  --relationship intersects \
  --output_csv /tmp/interval_overlap_boost_smoke.csv
```

Smoke test 结果：

```text
IntervalOverlapIndex:
answers = 20
visited_blocks = 20
skipped_zmax_blocks = 153
records_scanned = 698
exact_calls = 20

No block skip 对照：
answers = 20
visited_blocks = 173
records_scanned = 10490
exact_calls = 20

Boost R-tree:
answers = 20
```

解释：

```text
1. IntervalOverlapIndex 和 Boost R-tree 的 answers 一致，说明 smoke test 没有漏答案。
2. 开启 block maxZmax skip 后，visited_blocks 从 173 降到 20。
3. records_scanned 从 10490 降到 698。
4. 这说明 block-level interval skipping 的核心方向是可行的。
```

注意：

```text
这个 smoke test 规模很小，不能作为论文结论。
下一步需要在 ROADS/PARKS/AW/LW 和固定 query workload 上系统验证。
```
