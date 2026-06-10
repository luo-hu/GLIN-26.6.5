# IntervalOverlapIndex 1000000 全量实验分析

本文档分析：

```text
results/interval_overlap_full_1000000
figures/interval_overlap_full_1000000
```

实验包含：

```text
数据集：
AW, LW, ROADS, PARKS, UNIF_S, DIAG_S, ZGAP_WIDE

注意：
当前 result_dir 里没有 UNIF_L / DIAG_L 的 raw CSV。

selectivity：
0.001%, 0.01%, 0.1%, 1%

IntervalOverlapIndex block size：
512, 1024, 2048

对比方法：
IntervalOverlapIndex
GLIN_PIECEWISE
Boost_Rtree
GEOS_Quadtree
```

## 1. 总体结论

当前改进是成功的，但成功点不是“全面超过 Boost R-tree”。

更准确的结论是：

```text
IntervalOverlapIndex 成功解决了 GLIN_PIECEWISE 在 Intersects 查询中的候选膨胀问题。
它在所有数据集和所有 selectivity 上都保持 answers 与 Boost_Rtree 一致。
它稳定快于 GLIN_PIECEWISE。
它大多数情况下快于 GEOS_Quadtree。
它与 Boost_Rtree 接近，但不是所有情况下都超过 Boost_Rtree。
```

可以作为论文创新点，但论文表述要克制：

```text
可以说：
提出 interval-overlap-aware safe pruning，用 [Zmin, Zmax] overlap + block maxZmax + block MBR
减少 GLIN-style Zmin-only Intersects 查询的候选膨胀。

不能说：
全面击败 R-tree。
已经是完整 learned spatial index。
已经支持动态更新。
已经在所有 synthetic 数据上验证完整。
```

## 2. 正确性

所有统计组：

```text
7 个数据集
4 个 selectivity
共 28 个 dataset-selectivity 组合
```

结果：

```text
answers_match_boost = 1
```

解释：

```text
IntervalOverlapIndex 与 Boost_Rtree 的最终答案数一致。
这说明当前 block maxZmax 和 block MBR 剪枝没有造成 false negative。
```

这里的术语：

```text
false negative：
本来应该返回的答案被错误过滤掉。
论文里这是最严重的 correctness 错误。
```

## 3. 查询性能总览

下面使用每个 dataset-selectivity 下最快的 IntervalOverlapIndex block size。

### 3.1 对 GLIN_PIECEWISE

结果非常稳定：

```text
全部 28 / 28 组都快于 GLIN_PIECEWISE。
几何平均 speedup = 2.57x。
如果排除 ZGAP_WIDE，几何平均 speedup = 2.99x。
真实数据 AW/LW/ROADS/PARKS 上，几何平均 speedup = 2.31x。
```

解释：

```text
GLIN_PIECEWISE 为了支持 Intersects，会产生 query augmentation 带来的候选膨胀。
IntervalOverlapIndex 用 interval overlap 和 block-level safe pruning 把候选数压低。
所以它相对 GLIN_PIECEWISE 的提升是稳定的。
```

这就是目前最强的论文证据。

### 3.2 对 GEOS_Quadtree

结果也比较好：

```text
全部 28 组中，IntervalOverlapIndex 有 23 组快于 GEOS_Quadtree。
排除 ZGAP_WIDE 后，24 组中有 23 组快于 GEOS_Quadtree。
排除 ZGAP_WIDE 后，几何平均 speedup = 1.45x。
```

解释：

```text
在真实数据和普通 synthetic 数据上，IntervalOverlapIndex 的剪枝效果通常比 GEOS_Quadtree 更好。
但 ZGAP_WIDE 是压力数据集，里面对象覆盖范围很大，GEOS_Quadtree 反而更快。
```

ZGAP_WIDE 不适合拿来证明平均性能优势，但很适合用来说明边界情况。

### 3.3 对 Boost_Rtree

结果要谨慎：

```text
全部 28 组中，IntervalOverlapIndex 有 13 组快于 Boost_Rtree。
排除 ZGAP_WIDE 后，24 组中有 9 组快于 Boost_Rtree。
真实数据 AW/LW/ROADS/PARKS 上，16 组中有 8 组快于 Boost_Rtree。
真实数据上几何平均 speedup = 0.98x，基本接近持平。
```

按 selectivity 看更清楚：

```text
排除 ZGAP_WIDE 后：

0.001%：
IntervalOverlapIndex 0/6 组快于 Boost_Rtree。
几何平均 speedup = 0.86x。

0.01%：
IntervalOverlapIndex 2/6 组快于 Boost_Rtree。
几何平均 speedup = 0.90x。

0.1%：
IntervalOverlapIndex 3/6 组快于 Boost_Rtree。
几何平均 speedup = 0.95x。

1%：
IntervalOverlapIndex 4/6 组快于 Boost_Rtree。
几何平均 speedup = 1.07x。
```

解释：

```text
小 selectivity 查询返回答案少，Boost_Rtree 的树查询开销很低，因此 Boost 更容易赢。
selectivity 变大时，候选/refine 成本上升，IntervalOverlapIndex 的候选控制优势开始体现。
```

论文里可以说：

```text
IntervalOverlapIndex narrows the gap to Boost_Rtree and can outperform it on larger query windows,
but it is not uniformly faster than Boost_Rtree on highly selective queries.
```

中文意思：

```text
IntervalOverlapIndex 缩小了与 Boost_Rtree 的差距，并且在较大查询窗口上可以超过它；
但在非常小的查询窗口上，它不一定比 Boost_Rtree 快。
```

## 4. Candidate / answer ratio

`candidate/answer ratio` 的意思：

```text
候选数 / 最终答案数。
越接近 1，说明送给 GEOS 精确判断的无效候选越少。
```

IntervalOverlapIndex：

```text
所有组平均 candidate/answer ratio = 1.001。
最大值 = 1.015。
```

这非常好，说明：

```text
IntervalOverlapIndex 几乎只把真正会相交的对象送进 exact GEOS intersects。
```

对比 GLIN_PIECEWISE 的典型候选膨胀：

```text
ROADS 0.001%：
GLIN_PIECEWISE ratio = 12.396
IntervalOverlapIndex ratio = 1.001

PARKS 0.001%：
GLIN_PIECEWISE ratio = 4.418
IntervalOverlapIndex ratio = 1.007

LW 0.001%：
GLIN_PIECEWISE ratio = 6.964
IntervalOverlapIndex ratio = 1.015

AW 0.001%：
GLIN_PIECEWISE ratio = 3.405
IntervalOverlapIndex ratio = 1.003
```

这是论文里最应该强调的机制证据：

```text
性能提升不是偶然的，而是候选膨胀被系统性压下来了。
```

## 5. Block pruning 效果

`skipped block ratio` 的意思：

```text
被 block maxZmax / block MBR 安全跳过的 block 比例。
越高，说明 block summary 越有效。
```

真实数据：

```text
AW/LW/ROADS/PARKS 平均 skipped block ratio = 0.983。
也就是平均跳过约 98.3% 的 block。
```

普通 synthetic：

```text
UNIF_S/DIAG_S 平均 skipped block ratio = 0.305。
```

ZGAP_WIDE：

```text
平均 skipped block ratio = 0.216。
```

解释：

```text
真实数据中对象空间分布更有局部性，block summary 很有效。
UNIF/DIAG 小合成数据上 query 结果很小，Boost_Rtree 也很轻，IntervalOverlapIndex 的优势不明显。
ZGAP_WIDE 是故意制造 Zmin/Zmax gap 的压力数据，很多对象跨很大 Z-order 范围，block 很难被跳过。
```

这可以支撑一个很好的系统解释：

```text
IntervalOverlapIndex 的优势来自真实空间数据上的局部性和 block-level safe pruning。
当对象跨度很大或查询极小，优势会减弱。
```

## 6. Block size sensitivity

当前测试了：

```text
512, 1024, 2048
```

最佳 block size 分布：

```text
全部 28 组：
b512  最优 20 次
b1024 最优 4 次
b2048 最优 4 次
```

排除 ZGAP_WIDE 后：

```text
b512  最优 18 次
b1024 最优 4 次
b2048 最优 2 次
```

结论：

```text
b512 当前最稳。
但不能说 block size 不敏感，因为仍有少数组合 b1024/b2048 更好。
```

论文里建议：

```text
主实验用 b512 或 b1024 都可以。
如果追求当前 full_1000000 结果的最优表现，b512 更合适。
如果想保持更保守、更少 block summary 开销，b1024 也可以作为默认值。
```

## 7. 各类数据集表现

### 7.1 真实数据 AW/LW/ROADS/PARKS

这是最重要的部分。

结论：

```text
IntervalOverlapIndex 稳定快于 GLIN_PIECEWISE。
大多数情况下快于 GEOS_Quadtree。
与 Boost_Rtree 接近，1% 查询时更容易超过 Boost_Rtree。
```

真实数据上的几何平均：

```text
vs GLIN_PIECEWISE: 2.31x
vs GEOS_Quadtree: 1.33x
vs Boost_Rtree:   0.98x
```

解释：

```text
这说明 IntervalOverlapIndex 是一个强 GLIN 改进，而不是一个已经全面取代 R-tree 的方案。
```

### 7.2 UNIF_S / DIAG_S

结论：

```text
IntervalOverlapIndex 稳定快于 GLIN_PIECEWISE 和 GEOS_Quadtree。
但多数情况下慢于 Boost_Rtree。
```

几何平均：

```text
vs GLIN_PIECEWISE: 4.99x
vs GEOS_Quadtree: 1.73x
vs Boost_Rtree:   0.87x
```

解释：

```text
UNIF_S/DIAG_S 当前答案数量很小，很多查询只有 100 个 answers。
这种情况下 Boost_Rtree 查询路径非常轻，IntervalOverlapIndex 的扫描和 block 逻辑不一定占优。
```

注意：

```text
当前 full_1000000 目录没有 UNIF_L / DIAG_L。
如果论文要写 synthetic 完整实验，应该补 UNIF_L / DIAG_L。
```

### 7.3 ZGAP_WIDE

结论：

```text
IntervalOverlapIndex 快于 Boost_Rtree 和 GLIN_PIECEWISE。
但明显慢于 GEOS_Quadtree。
```

几何平均：

```text
vs Boost_Rtree:   1.23x
vs GLIN_PIECEWISE: 1.03x
vs GEOS_Quadtree: 0.71x
```

解释：

```text
ZGAP_WIDE 是压力数据，不是普通真实数据。
它故意让 Zmin/Zmax 跨度很大，block pruning 的跳过率只有约 21.6%。
这说明当对象跨度巨大时，IntervalOverlapIndex 的 block pruning 会变弱。
```

这个结果不坏，反而有价值：

```text
它说明方法边界在哪里。
论文可以把它作为 stress test，而不是主结果。
```

## 8. 能否作为论文创新点

可以，但应该定位为：

```text
Query-side innovation for GLIN-style learned spatial indexes.
```

中文：

```text
面向 GLIN 类 learned spatial index 的查询侧改进。
```

创新点可以写成三层：

```text
1. 问题发现：
   GLIN_PIECEWISE 的 Intersects 查询存在 Zmin-only / query augmentation 导致的候选膨胀。

2. 方法设计：
   将对象表示为 [Zmin, Zmax] interval；
   按 Zmin 排序；
   在 block 上维护 maxZmax 和 MBR；
   查询时用 interval overlap 和 block MBR 做 safe pruning。

3. 实验证据：
   answers 与 Boost_Rtree 一致；
   candidate/answer ratio 接近 1；
   真实数据上 skipped block ratio 约 98.3%；
   相比 GLIN_PIECEWISE 稳定提速；
   在较大 query window 上可超过 Boost_Rtree。
```

推荐论文表述：

```text
Our interval-overlap pruning reduces the candidate inflation of GLIN-piecewise
for spatial intersection queries while preserving exact answers. On real
geospatial datasets, it achieves near-R-tree candidate quality and substantially
improves over the GLIN-piecewise baseline.
```

中文意思：

```text
我们的 interval-overlap 剪枝降低了 GLIN-piecewise 在空间相交查询中的候选膨胀，
同时保持精确答案。在真实地理数据上，它获得了接近 R-tree 的候选质量，
并显著优于 GLIN-piecewise baseline。
```

## 9. 目前不能声称什么

不能声称：

```text
1. 全面超过 Boost_Rtree。
   因为小 selectivity 和部分 synthetic 上 Boost_Rtree 更快。

2. 全面超过 GEOS_Quadtree。
   因为 ZGAP_WIDE 上 GEOS_Quadtree 更快。

3. 已经是完整 GLIN 替代方案。
   当前还是独立 prototype，不是完全接入 GLIN/ALEX 的最终结构。

4. 参数完全不敏感。
   b512 当前最好，但 b1024/b2048 在少数组合上更好。

5. synthetic 完整覆盖。
   当前 full_1000000 结果没有 UNIF_L / DIAG_L。
```

## 10. 下一步建议

论文优先补：

```text
1. 补 UNIF_L / DIAG_L。
2. 做 3 次 seed repeat，至少对 ROADS/PARKS/AW/LW。
3. 增加 memory/build time 表。
4. 增加 ablation：
   only maxZmax
   only block MBR
   maxZmax + block MBR
5. 固定一个默认 block size。
   当前建议 b512；如果想更保守可用 b1024。
6. 把 ZGAP_WIDE 单独放 stress test，不和普通数据混在主图平均里。
```

## 11. 命名修正和 IO_OVERFLOW

之前图和 CSV 里的：

```text
IntervalOverlapIndex
```

容易造成误解，因为当前实现不只是基础的 `maxZmax`，还包含：

```text
block.maxZmax 剪枝
block MBR 剪枝
record MBR filter
GEOS exact intersects
```

所以现在代码里把它明确命名为：

```text
IO_BLOCK_MBR
```

中文意思：

```text
Interval-overlap index with block MBR pruning。
也就是：区间重叠索引 + block MBR 剪枝版本。
```

新增版本：

```text
IO_OVERFLOW
```

中文意思：

```text
main index + fat-object overflow R-tree。
也就是：主索引存普通对象，把 Zmax-Zmin 跨度最大的长对象分流到 overflow R-tree。
```

### 11.1 如何运行 IO_OVERFLOW

小规模测试：

```bash
RESET_RESULTS=1 OVERWRITE=1 \
DATASETS=ZGAP_WIDE LIMIT=10000 QUERY_LIMIT=1000000 \
QUERY_ROOT=queries/interval_overlap_full_1000000 \
RESULT_DIR=results/interval_overlap_overflow_zgap_smoke_10000 \
FIGURE_DIR=figures/interval_overlap_overflow_zgap_smoke_10000 \
SELECTIVITY_TAGS=1pct BLOCK_SIZES=512 \
INCLUDE_IO_BLOCK_MBR=1 \
INCLUDE_IO_OVERFLOW=1 \
OVERFLOW_FRACTIONS="0.01 0.05" \
INCLUDE_QUADTREE=1 AUTO_BUILD=0 \
./scripts/run_interval_overlap_diagnostics.sh
```

正式测试建议：

```bash
RESET_RESULTS=1 OVERWRITE=1 \
DATASETS="AW LW ROADS PARKS ZGAP_WIDE" \
LIMIT=1000000 QUERY_LIMIT=1000000 \
QUERY_ROOT=queries/interval_overlap_full_1000000 \
RESULT_DIR=results/interval_overlap_overflow_full_1000000 \
FIGURE_DIR=figures/interval_overlap_overflow_full_1000000 \
SELECTIVITY_TAGS="0p001pct 0p01pct 0p1pct 1pct" \
BLOCK_SIZES="512 1024 2048" \
INCLUDE_IO_BLOCK_MBR=1 \
INCLUDE_IO_OVERFLOW=1 \
OVERFLOW_FRACTIONS="0.001 0.01 0.05" \
INCLUDE_QUADTREE=1 \
./scripts/run_interval_overlap_diagnostics.sh
```

### 11.2 已验证的 smoke 结果

UNIF_S 1000 条、1% 查询：

```text
IO_BLOCK_MBR answers = 1029
IO_OVERFLOW  answers = 1029
Boost_Rtree  answers = 1029
answers_match_boost = 1
```

ZGAP_WIDE 10000 条、1% 查询：

```text
IO_BLOCK_MBR             avg_total_ns = 1,188,681
IO_OVERFLOW fraction=1%  avg_total_ns = 1,169,915
IO_OVERFLOW fraction=5%  avg_total_ns = 1,129,507
Boost_Rtree              avg_total_ns = 1,336,399
GEOS_Quadtree            avg_total_ns = 1,113,794
GLIN_PIECEWISE           avg_total_ns = 1,258,900
```

解释：

```text
IO_OVERFLOW 在这个小 ZGAP smoke 上比 IO_BLOCK_MBR 更快，
说明 fat-object overflow 的方向是有希望的。

但它仍略慢于 GEOS_Quadtree。
这符合之前判断：ZGAP_WIDE 是极端压力数据，长对象太多时，
overflow 只能缓解主索引污染，不能消除海量答案枚举成本。
```

最推荐的论文结构：

```text
主图：
真实数据 AW/LW/ROADS/PARKS，4 个 selectivity，b512 或 b1024。

候选数图：
candidate/answer ratio，突出 GLIN_PIECEWISE 的候选膨胀被压到接近 1。

机制图：
prefix_records -> records_scanned，展示 block pruning。

敏感性图：
block size sensitivity。

压力测试：
ZGAP_WIDE，说明方法边界。
```


提问：interval_overlap_full_1000000文件夹下：interval_overlap_0p1pct_avg_total_ms.png，interval_overlap_0p01pct_avg_total_ms.png，interval_overlap_0p01pct_avg_total_ms.png这三张图片好像为什么只有ZGAP_WIDE数据集能看到明显的柱状图，可能是AW LW ROADS PARKS的数值太小了，所以不明显，interval_overlap_full_1000000_no_zgap文件夹下这三张图因为去除了ZGAP_WIDGE数据，所以其它数据集的数据看起来比较明显。但是UNIF_S和DIAG_S好像确实是没有数据，所以图中根本绘制不出相应的柱状图，但是这两个在data文件夹中是有数据集的，为什么还会这样呢？interval_overlap_0p1pct_pruning_detail.png这张图是在做什么，我没看明白，它的效果如何呢，该怎么看这张图，它有什么用？interval_overlap_0p1pct_candidate_answer_ratio.png，interval_overlap_0p01pct_candidate_answer_ratio.png，interval_overlap_0p001pct_candidate_answer_ratio.png，interval_overlap_1pct_candidate_answer_ratio.png这四张图是在测试候选对象与正确答案的比例，从图中我发现在不同数据集不同比例下，IO b512 IO b1024 IO b2048 Boost Rtree这四个方法的ratio好像是几乎一样的，这是正常的吗？还有一点，为什么只有在UNIF_S和DIAG_S这两个合成数据集上，Quadtree的ratio能够和IO、Boost Rtree保持在同一水平呢？

回答：你判断得对：`avg_total_ms` 那几张图里，不是 AW/LW/ROADS/PARKS 没有数据，而是 `ZGAP_WIDE` 太大，把 y 轴拉爆了。

比如 `0.1%` 左右：

```text
ZGAP_WIDE IO: 约 350 ms
PARKS IO:    约 1.17 ms
ROADS IO:    约 0.62 ms
UNIF_S IO:   约 0.0006 ms
DIAG_S IO:   约 0.0006 ms
```

所以在同一张线性坐标柱状图里：

```text
ZGAP_WIDE 是几百 ms；
真实数据是 0.x 到几 ms；
UNIF_S / DIAG_S 是 0.001 ms 级别。
```

UNIF_S 和 DIAG_S 不是没数据，而是太小了，小到柱子贴着 x 轴，看起来像没画出来。CSV 里是有数据的，例如：

```text
UNIF_S 0.1% IO b512: 0.000597 ms
DIAG_S 0.1% IO b512: 0.000592 ms
```

**怎么看 avg_total_ms 图**
建议分三类看：

```text
1. full 图：
   看 ZGAP_WIDE 压力测试。

2. no_zgap 图：
   看 AW/LW/ROADS/PARKS 的真实数据表现。

3. synthetic-only 图：
   单独看 UNIF_S/DIAG_S，否则它们太小。
```

你可以这样只画 synthetic：

```bash
python3 scripts/plot_interval_overlap_diagnostics.py \
  --input results/interval_overlap_full_1000000/interval_overlap_summary.csv \
  --output_dir figures/interval_overlap_full_1000000_synthetic_only \
  --figure_prefix interval_overlap \
  --exclude_datasets "AW LW ROADS PARKS ZGAP_WIDE"
```

**pruning_detail 图是什么**
`interval_overlap_0p1pct_pruning_detail.png` 只看 `IntervalOverlapIndex` 的剪枝过程。

里面主要有三类东西：

```text
prefix_records:
只用 Zmin <= query.Zmax 时，理论上会进入扫描范围的记录数。

records_scanned:
经过 block maxZmax + block MBR 剪枝后，真正扫描的记录数。

skipped block ratio:
被安全跳过的 block 比例。
```

这张图的作用是解释：IO 为什么快。

如果：

```text
prefix_records 很高
records_scanned 很低
skipped block ratio 很高
```

说明剪枝有效。

当前效果：

```text
真实数据 AW/LW/ROADS/PARKS：
skipped block ratio 大约 95% 到 99%，效果很好。

UNIF_S / DIAG_S：
skipped block ratio 大约 30% 左右，效果一般。

ZGAP_WIDE：
skipped block ratio 大约 14% 到 30%，说明这是压力数据，block 很难跳过。
```

所以这张图很有用，论文里可以用它说明机制，不只是展示速度。

**candidate/answer ratio 为什么 IO 和 Boost 几乎一样**
这是正常的，而且是好事。

`candidate/answer ratio` 是：

```text
候选对象数 / 最终正确答案数
```

越接近 1，说明无效候选越少。

你看到：

```text
IO b512 / b1024 / b2048 / Boost_Rtree 几乎一样
```

原因是：

```text
block size 主要影响 records_scanned 和查询时间；
但最终进入 GEOS exact intersects 的候选集合基本不变。
```

也就是说：

```text
b512/b1024/b2048 改变的是“前面少扫多少”；
不是改变“最后候选答案有多少”。
```

所以比较 block size，应该看：

```text
avg_total_ms
pruning_detail
block_sensitivity
```

不要主要看 `candidate/answer ratio`。

**为什么 Quadtree 在 UNIF_S/DIAG_S 上 ratio 也接近 1**
因为这两个合成数据是规则的小矩形：

```text
对象形状简单；
MBR 基本等于真实 geometry；
空间分布也比较干净。
```

Quadtree 用 MBR 查候选。对于小矩形数据，MBR 过滤已经很准，所以 ratio 接近 1。

但真实数据不一样：

```text
ROADS / PARKS / AW / LW 有长线、多边形、复杂形状；
很多对象的 MBR 会覆盖较大区域；
MBR 相交不代表真实 geometry 相交。
```

所以 Quadtree 在真实数据上会返回更多 false positives，ratio 变高。

总结一下：

```text
IO 成功点：
候选质量接近 Boost R-tree；
明显压制 GLIN_PIECEWISE 的候选膨胀；
真实数据上 block pruning 很强。

IO 不适合夸大的点：
不是所有情况下都比 Boost R-tree 快；
UNIF_S/DIAG_S 太简单，速度图容易看不出差异；
ZGAP_WIDE 是压力测试，会拉大 y 轴。
```

论文里最好把 `ZGAP_WIDE` 单独放 stress test，把真实数据和普通 synthetic 分开画。
