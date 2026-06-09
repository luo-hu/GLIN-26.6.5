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

## 13. ROADS/PARKS 2M 正式 query workload 对比

已经新增正式对比脚本：

```text
scripts/run_interval_overlap_diagnostics.sh
scripts/summarize_interval_overlap_diagnostics.py
```

运行命令：

```bash
RESET_RESULTS=1 DATASETS="ROADS PARKS" LIMIT=2000000 QUERY_LIMIT=2000000 BLOCK_SIZE=1024 bash scripts/run_interval_overlap_diagnostics.sh
```

对比对象：

```text
IntervalOverlapIndex
GLIN_PIECEWISE
Boost_Rtree
```

三者使用同一批 query 文件：

```text
queries/fig17_hybrid_2000000/ROADS_jts_strtree_knn_1pct.csv
queries/fig17_hybrid_2000000/PARKS_jts_strtree_knn_1pct.csv
```

输出：

```text
results/interval_overlap_2000000/ROADS_interval_overlap.csv
results/interval_overlap_2000000/ROADS_glin_piecewise.csv
results/interval_overlap_2000000/ROADS_boost_rtree.csv
results/interval_overlap_2000000/PARKS_interval_overlap.csv
results/interval_overlap_2000000/PARKS_glin_piecewise.csv
results/interval_overlap_2000000/PARKS_boost_rtree.csv
results/interval_overlap_2000000/interval_overlap_summary.csv
```

### 13.1 正确性

当前结果：

```text
ROADS:
IntervalOverlapIndex answers = 2,490,128
GLIN_PIECEWISE answers       = 2,490,128
Boost_Rtree answers          = 2,490,128

PARKS:
IntervalOverlapIndex answers = 3,249,257
GLIN_PIECEWISE answers       = 3,249,257
Boost_Rtree answers          = 3,249,257
```

解释：

```text
IntervalOverlapIndex 在 ROADS/PARKS 的 2M 固定 query workload 上和 Boost R-tree 答案一致。
这说明当前 block maxZmax + MBR pruning 没有产生 false negative。
```

### 13.2 查询时间

| Dataset | Index | avg_total_ns | avg_total_ms | answers_match_boost |
|---|---|---:|---:|---:|
| ROADS | IntervalOverlapIndex | 13,656,971.68 | 13.657 | 1 |
| ROADS | GLIN_PIECEWISE | 15,650,507.52 | 15.651 | 1 |
| ROADS | Boost_Rtree | 16,250,759.32 | 16.251 | 1 |
| PARKS | IntervalOverlapIndex | 18,368,633.63 | 18.369 | 1 |
| PARKS | GLIN_PIECEWISE | 20,838,326.03 | 20.838 | 1 |
| PARKS | Boost_Rtree | 20,518,349.75 | 20.518 | 1 |

相对提升：

```text
ROADS:
IntervalOverlapIndex 比 GLIN_PIECEWISE 快约 1.146x。
IntervalOverlapIndex 比 Boost_Rtree 快约 1.190x。

PARKS:
IntervalOverlapIndex 比 GLIN_PIECEWISE 快约 1.134x。
IntervalOverlapIndex 比 Boost_Rtree 快约 1.117x。
```

注意：

```text
这是 100 个 1% JTS STRtree KNN query window 上的平均查询时间。
当前还不是完整论文结论，后续还需要更多 selectivity、更多 seed 和更多数据集。
```

### 13.3 候选数量

| Dataset | Index | candidates | answers | candidate_answer_ratio |
|---|---|---:|---:|---:|
| ROADS | IntervalOverlapIndex | 2,490,131 | 2,490,128 | 1.000001 |
| ROADS | GLIN_PIECEWISE | 2,742,009 | 2,490,128 | 1.101152 |
| ROADS | Boost_Rtree | 2,490,131 | 2,490,128 | 1.000001 |
| PARKS | IntervalOverlapIndex | 3,249,304 | 3,249,257 | 1.000014 |
| PARKS | GLIN_PIECEWISE | 3,551,775 | 3,249,257 | 1.093104 |
| PARKS | Boost_Rtree | 3,249,304 | 3,249,257 | 1.000014 |

解释：

```text
IntervalOverlapIndex 的 exact_calls/candidates 与 Boost_Rtree 几乎一致。
GLIN_PIECEWISE 的候选数比 IntervalOverlapIndex 多：
ROADS 多约 10.1%。
PARKS 多约 9.3%。
```

这正好对应 GLIN 的 query augmentation 问题：

```text
GLIN_PIECEWISE 为了避免漏答案，会多扫一些不必要候选。
IntervalOverlapIndex 通过 [Zmin, Zmax] overlap 和 block maxZmax，把这部分候选压了下来。
```

### 13.4 Block skipping 效果

| Dataset | prefix_records | records_scanned | visited_blocks | skipped_zmax_blocks | skipped_mbr_blocks | skipped_block_ratio |
|---|---:|---:|---:|---:|---:|---:|
| ROADS | 121,549,950 | 4,063,962 | 4,020 | 87,568 | 27,166 | 0.966149 |
| PARKS | 117,007,639 | 4,972,435 | 4,903 | 76,696 | 32,715 | 0.957109 |

解释：

```text
prefix_records 是只按 Zmin <= R 会进入扫描范围的记录数。
records_scanned 是 block-level pruning 后真正进入 leaf 扫描的记录数。
```

ROADS：

```text
121,549,950 -> 4,063,962
扫描记录数减少到约 3.34%。
```

PARKS：

```text
117,007,639 -> 4,972,435
扫描记录数减少到约 4.25%。
```

这说明：

```text
block.maxZmax 和 block MBR 的安全剪枝在正式 workload 上有效。
```

### 13.5 当前结论

这组结果比 smoke test 更有价值，可以支持下面的初步判断：

```text
IntervalOverlapIndex 在 ROADS/PARKS 2M 真实数据和固定 query workload 上：
1. 与 Boost R-tree 答案一致；
2. 显著减少 Zmin-only 前缀扫描范围；
3. 候选数量接近 Boost R-tree；
4. 平均查询时间优于 GLIN_PIECEWISE 和 Boost_Rtree。
```

论文上可以谨慎表述为：

```text
On ROADS and PARKS with 2M geometries and 1% JTS STRtree KNN query windows,
the interval-overlap prototype preserves exactness and reduces candidate
inflation compared with GLIN-piecewise.
```

中文意思：

```text
在 ROADS/PARKS 2M 数据和 1% JTS STRtree KNN 查询窗口上，
IntervalOverlapIndex 原型保持了正确性，并减少了 GLIN-piecewise 的候选膨胀。
```

仍然不能过度声称：

```text
不能说它已经是完整 learned index。
不能说它已经 fully dynamic。
不能说所有数据和所有 selectivity 都优于 R-tree。
```

### 13.6 新增可视化输出

为了后续更容易看结果，新增绘图脚本：

```text
scripts/plot_interval_overlap_diagnostics.py
```

正式 runner 现在会在汇总 CSV 后自动画图：

```text
scripts/run_interval_overlap_diagnostics.sh
```

默认输出目录：

```text
figures/interval_overlap_2000000/
```

当前已经生成：

```text
figures/interval_overlap_2000000/interval_overlap_1pct_avg_total_ms.png
figures/interval_overlap_2000000/interval_overlap_1pct_candidate_answer_ratio.png
figures/interval_overlap_2000000/interval_overlap_1pct_pruning_detail.png
figures/interval_overlap_2000000/interval_overlap_diagnostics.txt
```

每张图的含义：

```text
interval_overlap_avg_total_ms.png
平均每个 query 花多少毫秒。越低越好。

interval_overlap_candidate_answer_ratio.png
候选数 / 最终答案数。越接近 1 越好。
它表示索引多送了多少对象给 GEOS 精确判断。

interval_overlap_pruning_detail.png
只看 IntervalOverlapIndex 的剪枝效果。
prefix_records 是如果只按 Zmin <= query.Zmax 会落入前缀范围的记录数。
records_scanned 是经过 block maxZmax + block MBR 剪枝后真正扫描的记录数。
skipped block ratio 是被安全跳过的 block 比例。

interval_overlap_diagnostics.txt
文字版简要诊断，包括相对 Boost_Rtree/GLIN-piecewise 的 speedup、
candidate/answer ratio、skipped block ratio 和 answers_match_boost。
```

如果只想重新画图，不重新跑 2M benchmark：

```bash
python3 scripts/plot_interval_overlap_diagnostics.py \
  --input results/interval_overlap_2000000/interval_overlap_summary.csv \
  --output_dir figures/interval_overlap_2000000 \
  --figure_prefix interval_overlap
```

如果想完整重新跑正式对比：

```bash
RESET_RESULTS=1 DATASETS="ROADS PARKS" LIMIT=2000000 QUERY_LIMIT=2000000 BLOCK_SIZE=1024 bash scripts/run_interval_overlap_diagnostics.sh
```

## 14. 补充 QuadTree、selectivity、block size 和 synthetic 实验

### 14.1 为什么正式 Intersects 对比里不默认放原始 GLIN

这个不是漏掉，而是因为当前仓库里的两个 GLIN WKT benchmark 语义不同：

```text
bench_glin_wkt        -> index=GLIN           -> relationship=contains
bench_glin_wkt_piece  -> index=GLIN_PIECEWISE -> relationship=intersects
```

中文解释：

```text
原始 GLIN 在当前代码路径里跑的是 contains。
contains 的意思是 query window 完全包含 candidate geometry。
intersects 的意思是 query window 和 candidate geometry 有交集即可。

这两个不是同一个查询任务。
如果把原始 GLIN 放进 Intersects 排名表，它的 answers 不应该和 Boost_Rtree 对齐，
这样比较会误导读者。
```

所以：

```text
Intersects 正式对比：
IntervalOverlapIndex
GLIN_PIECEWISE
Boost_Rtree
GEOS_Quadtree

Contains 正式对比：
GLIN
Boost_Rtree
GEOS_Quadtree
```

如果只是想做 sanity check，可以打开：

```bash
INCLUDE_GLIN_CONTAINS=1 bash scripts/run_interval_overlap_diagnostics.sh
```

但它只能说明原始 GLIN 的 contains 表现，不能作为 Intersects 方法排名。

### 14.2 为什么要加 GEOS_Quadtree

`GEOS_Quadtree` 是应该加的，因为它是空间索引里常见的 tree baseline。

新增 runner 现在默认：

```text
INCLUDE_QUADTREE=1
```

也就是会自动跑：

```text
IntervalOverlapIndex
GLIN_PIECEWISE
Boost_Rtree
GEOS_Quadtree
```

### 14.3 ROADS/PARKS 2M + QuadTree 正式结果

运行命令：

```bash
RESET_RESULTS=1 DATASETS="ROADS PARKS" LIMIT=2000000 QUERY_LIMIT=2000000 \
QUERY_ROOT=queries/fig17_hybrid_2000000 \
RESULT_DIR=results/interval_overlap_2000000_with_quadtree \
FIGURE_DIR=figures/interval_overlap_2000000_with_quadtree \
SELECTIVITY_TAGS=1pct BLOCK_SIZES=1024 INCLUDE_QUADTREE=1 AUTO_BUILD=0 \
bash scripts/run_interval_overlap_diagnostics.sh
```

输出：

```text
results/interval_overlap_2000000_with_quadtree/interval_overlap_summary.csv
figures/interval_overlap_2000000_with_quadtree/interval_overlap_1pct_avg_total_ms.png
figures/interval_overlap_2000000_with_quadtree/interval_overlap_1pct_candidate_answer_ratio.png
figures/interval_overlap_2000000_with_quadtree/interval_overlap_1pct_pruning_detail.png
```

当前结果：

| Dataset | Index | avg_total_ms | candidates | answers | candidate_answer_ratio | answers_match_boost |
|---|---|---:|---:|---:|---:|---:|
| ROADS | IntervalOverlapIndex | 13.736 | 2,490,131 | 2,490,128 | 1.000001 | 1 |
| ROADS | Boost_Rtree | 15.998 | 2,490,131 | 2,490,128 | 1.000001 | 1 |
| ROADS | GLIN_PIECEWISE | 16.346 | 2,742,009 | 2,490,128 | 1.101152 | 1 |
| ROADS | GEOS_Quadtree | 22.026 | 2,561,859 | 2,490,128 | 1.028857 | 1 |
| PARKS | IntervalOverlapIndex | 18.697 | 3,249,304 | 3,249,257 | 1.000014 | 1 |
| PARKS | Boost_Rtree | 20.263 | 3,249,304 | 3,249,257 | 1.000014 | 1 |
| PARKS | GLIN_PIECEWISE | 21.410 | 3,551,775 | 3,249,257 | 1.093104 | 1 |
| PARKS | GEOS_Quadtree | 22.281 | 3,541,049 | 3,249,257 | 1.089802 | 1 |

解释：

```text
IntervalOverlapIndex 在这轮 ROADS/PARKS 2M、1% fixed query workload 上：
1. answers 与 Boost_Rtree 完全一致；
2. candidate/answer ratio 接近 1；
3. 平均 query time 同时优于 GLIN_PIECEWISE、Boost_Rtree、GEOS_Quadtree。
```

仍然要注意：

```text
这还只是 1% query window。
论文里不能只靠这一组结果下最终结论。
```

### 14.4 Selectivity sensitivity 怎么跑

`selectivity` 是查询窗口大小/答案规模的控制变量。

中文理解：

```text
0.001% / 0.01% / 0.1% / 1%
可以理解为 query 越来越大、返回答案越来越多。
```

运行：

```bash
RESET_RESULTS=1 DATASETS="ROADS PARKS" LIMIT=2000000 QUERY_LIMIT=2000000 \
QUERY_ROOT=queries/fig17_hybrid_2000000 \
RESULT_DIR=results/interval_overlap_selectivity_2000000 \
FIGURE_DIR=figures/interval_overlap_selectivity_2000000 \
SELECTIVITY_TAGS="0p001pct 0p01pct 0p1pct 1pct" \
BLOCK_SIZES=1024 INCLUDE_QUADTREE=1 \
bash scripts/run_interval_overlap_diagnostics.sh
```

如果某些 query CSV 还没有生成，可以打开：

```bash
AUTO_GENERATE_QUERIES=1
```

注意：

```text
QUERY_ROOT 下需要有：
ROADS_jts_strtree_knn_0p001pct.csv
ROADS_jts_strtree_knn_0p01pct.csv
ROADS_jts_strtree_knn_0p1pct.csv
ROADS_jts_strtree_knn_1pct.csv
PARKS_jts_strtree_knn_...
```

### 14.5 Block size sensitivity 怎么跑

`block size` 是 IntervalOverlapIndex 里每个 block 放多少条按 Zmin 排序的记录。

中文理解：

```text
block size 小：
block 更多，summary 更细，剪枝可能更准，但 block 管理开销更大。

block size 大：
block 更少，summary 更粗，管理开销小，但可能少剪枝、多扫描。
```

运行：

```bash
RESET_RESULTS=1 DATASETS="ROADS PARKS" LIMIT=2000000 QUERY_LIMIT=2000000 \
QUERY_ROOT=queries/fig17_hybrid_2000000 \
RESULT_DIR=results/interval_overlap_block_sweep_2000000 \
FIGURE_DIR=figures/interval_overlap_block_sweep_2000000 \
SELECTIVITY_TAGS=1pct \
BLOCK_SIZES="256 512 1024 2048 4096" \
INCLUDE_QUADTREE=1 \
bash scripts/run_interval_overlap_diagnostics.sh
```

看图时重点看：

```text
avg_total_ms:
不同 block size 下平均查询时间。

pruning_detail:
prefix_records 到 records_scanned 的下降幅度。

candidate_answer_ratio:
是否仍然接近 1。
```

### 14.6 Synthetic 数据怎么跑

当前仓库里有合成数据生成器：

```text
scripts/prepare_synthetic_rectangles.sh
scripts/prepare_glin_synthetic_geo_points.sh
```

对于 Intersects，优先用 rectangle synthetic：

```text
SYNTHETIC_KIND=rectangles
```

原因：

```text
Intersects 更关心面/矩形/线段这类有空间范围的对象。
point synthetic 可以测 pipeline，但对 Intersects 的挑战性不如 rectangle synthetic。
```

小规模 smoke test：

```bash
RESET_RESULTS=1 DATASETS=UNIF_S LIMIT=1000 QUERY_LIMIT=1000 \
QUERY_ROOT=queries/interval_overlap_smoke_1000 \
RESULT_DIR=results/interval_overlap_smoke_1000 \
FIGURE_DIR=figures/interval_overlap_smoke_1000 \
PREPARE_DATA=1 AUTO_GENERATE_QUERIES=1 \
SELECTIVITY_TAGS="0p1pct 1pct" BLOCK_SIZES="128 256" \
SYNTHETIC_KIND=rectangles \
bash scripts/run_interval_overlap_diagnostics.sh
```

这条 smoke 已经跑通：

```text
UNIF_S 1000 条 synthetic rectangle。
IntervalOverlapIndex、GLIN_PIECEWISE、Boost_Rtree、GEOS_Quadtree 都能跑。
answers_match_boost = 1。
```

正式 synthetic 建议：

```bash
RESET_RESULTS=1 DATASETS="UNIF_S UNIF_L DIAG_S DIAG_L" LIMIT=1000000 QUERY_LIMIT=1000000 \
QUERY_ROOT=queries/interval_overlap_synthetic_1000000 \
RESULT_DIR=results/interval_overlap_synthetic_1000000 \
FIGURE_DIR=figures/interval_overlap_synthetic_1000000 \
PREPARE_DATA=1 AUTO_GENERATE_QUERIES=1 \
SELECTIVITY_TAGS="0p001pct 0p01pct 0p1pct 1pct" \
BLOCK_SIZES="512 1024 2048" \
SYNTHETIC_KIND=rectangles INCLUDE_QUADTREE=1 \
bash scripts/run_interval_overlap_diagnostics.sh
```

论文里 synthetic 的用法：

```text
真实数据 ROADS/PARKS 说明方法对实际地图对象有效。
Synthetic UNIF/DIAG 说明方法在可控分布下是否稳定。
Block size sensitivity 说明参数不是靠碰运气调出来的。
Selectivity sensitivity 说明 query 大小变化时方法是否仍然有效。
```
