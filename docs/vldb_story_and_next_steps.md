这次的结果也出来了，但是我发现IO-overflow的效果不稳定，不能完全地超越Rtree，不同的查询选择性，不同的BLOCK_SIZES，不同的OVERFLOW_FRACTIONS，都会对IO-overflow有影响，要是能自动调参就好了，这个也不知道是否可行，下一步该做什么呢？目前为止我们做了很多改进，在docs文件夹中有总结性文档，我想发VLDB会议，但是不知道怎么讲好一个故事，我感觉目前审稿人会质疑我们目前的改进还是在GLIN上小修小补，在现有基础上增加KNN查询是否可行？总之，我现在思路很乱，不知道该怎么办了，需要你的帮助
# VLDB 投稿主线与下一步决策

最后更新日期：2026-06-10

本文档用于回答当前最关键的问题：

```text
我们已经做了很多 GLIN 改进，但哪些能形成 VLDB 论文主线？
IO-overflow 参数不稳定怎么办？
要不要继续做 auto-tuning？
要不要加入 KNN 查询？
下一步到底先做什么？
```

固定术语说明：

- `VLDB`: Very Large Data Bases，数据库领域顶级会议之一。它看重清晰的问题、可靠的系统设计、完整实验和克制的结论。
- `GLIN`: 当前项目里的 learned spatial index，核心是把空间对象映射到 Z-order key。
- `Z-order`: 空间填充曲线，把二维空间映射到一维 key。
- `Zmin/Zmax`: 一个 geometry 映射到 Z-order 后覆盖区间的最小/最大 key。
- `Intersects`: 空间相交查询，判断 query window 是否和对象相交。
- `KNN`: k-nearest neighbors，k 近邻查询。注意：当前 JTS STRtree KNN query workload 主要用于生成 query window，不等于我们已经支持 KNN 查询。
- `auto-tuning`: 自动调参。这里指自动选择 `block_size`、`overflow_fraction` 等参数。
- `oracle tuning`: 看完测试结果以后再选最佳参数。这不公平，论文里只能作为上界分析，不能当正式方法。
- `training queries`: 训练查询集，用来选参数。
- `test queries`: 测试查询集，用来报告最终性能。正式论文中不能用 test queries 来调参。

## 1. 当前一句话研究问题

当前最适合投稿的研究问题是：

```text
GLIN 类 Zmin-only learned spatial index 在复杂 geometry 的 Intersects 查询中会产生候选膨胀；
我们能否显式利用 [Zmin, Zmax] interval overlap，构建安全剪枝机制，让 learned Z-order index 支持精确 Intersects 查询？
```

这比“我要全面超过 R-tree”更稳。

原因：

```text
1. 当前结果已经证明 IO_BLOCK_MBR 稳定解决 GLIN-piecewise 的候选膨胀。
2. answers_match_boost=1，正确性证据清楚。
3. candidate/answer ratio 接近 1，机制证据清楚。
4. 真实数据上 skipped block ratio 很高，能解释为什么有效。
5. 对 Boost R-tree 是接近或部分超过，而不是全面超过；这个结论更可信。
```

## 2. 当前已有资产

### 2.1 查询侧改进

已经实现：

```text
IO_BLOCK_MBR
IO_OVERFLOW
```

位置：

```text
src/benchmark/bench_interval_overlap_wkt.cpp
scripts/run_interval_overlap_diagnostics.sh
scripts/plot_interval_overlap_diagnostics.py
```

当前最强证据：

```text
IO_BLOCK_MBR 在真实数据和合成数据上稳定快于 GLIN_PIECEWISE。
candidate/answer ratio 接近 1。
answers_match_boost=1。
```

`IO_OVERFLOW` 当前状态：

```text
它有时能进一步提升，但对 block_size 和 overflow_fraction 敏感。
它不能直接作为“默认稳定方法”来主张。
它更适合作为 robustness extension，或者作为 auto-tuning 的候选机制。
```

### 2.2 动态更新侧改进

已经实现：

```text
GLIN_BUFFERED
GLIN_LSM
GLIN_LSM_ASYNC
GLIN_LSM_SEGMENTED
GLIN_LSM_SEGMENTED4
GLIN_LSM_BG
```

当前判断：

```text
GLIN_LSM_BG 是最可用的动态更新系统变体。
但它是 system-level asynchronous variant，不是原始 GLIN baseline。
它适合做“系统扩展”或“未来章节”，不建议压过 Intersects 查询主线。
```

原因：

```text
1. 多线程/后台维护容易被审稿人质疑公平性。
2. foreground throughput 和 total throughput 需要同时报告。
3. drain_ns 仍然说明后台有欠账。
4. 如果和 IO_BLOCK_MBR 主线混在一起，论文会显得目标太散。
```

## 3. IO-overflow 不稳定怎么办

### 3.1 目前观察

从 `results/interval_overlap_overflow_full_1000000/interval_overlap_summary.csv` 看：

```text
真实 1M overflow-full 结果包含：
AW, LW, ROADS, PARKS, ZGAP_WIDE
每个 loaded_count = 1000000
```

如果对每个 dataset-selectivity 事后选择最佳 `block_size + overflow_fraction`：

```text
IO_OVERFLOW best-vs-Boost: 16 / 20 组超过 Boost_Rtree
IO_OVERFLOW best-vs-IO_BLOCK_MBR: 14 / 20 组超过 IO_BLOCK_MBR
```

这说明：

```text
IO_OVERFLOW 有潜力。
但它不是一个不用调参就稳定超过所有方法的机制。
```

### 3.2 不能怎么写

不能写：

```text
IO_OVERFLOW 全面超过 Boost R-tree。
IO_OVERFLOW 是最终默认方法。
选择每组最佳参数后报告主结果。
```

原因：

```text
这是 oracle tuning。
审稿人会问：测试前你怎么知道该选 0.001、0.01 还是 0.05？
```

### 3.3 可以怎么写

更稳的写法：

```text
IO_BLOCK_MBR 是主方法。
IO_OVERFLOW 是面向 fat-object pollution 的可选增强。
它在存在少量长区间对象时提供收益，但需要自动参数选择。
```

中文解释：

```text
fat-object pollution 是指少量 Zmax-Zmin 跨度很大的对象把 block.maxZmax 或 block MBR 拉大，
导致主索引不能有效跳过 block。
```

## 4. Auto-tuning 是否可行

可行，但要非常小心。

### 4.1 推荐做法：训练查询集调参

流程：

```text
1. 对每个数据集生成两套 query：
   training queries：用于选参数。
   test queries：用于正式报告。

2. 在 training queries 上尝试小范围参数：
   block_size ∈ {512, 1024, 2048}
   overflow_fraction ∈ {0, 0.001, 0.01, 0.05}

3. 选择 training avg_total_ns 最小的配置。

4. 在 test queries 上只跑这个配置，报告最终结果。

5. 额外报告 tuning overhead：
   自动调参本身花了多久。
```

这个版本可以叫：

```text
IO_ADAPTIVE
```

中文意思：

```text
自适应 IO 方法，也就是通过训练查询自动选择参数。
```

### 4.2 为什么这比手动调参公平

因为：

```text
测试集没有参与选参数。
每个方法的最终结果都是在 test queries 上测出来的。
```

论文里要明确：

```text
Auto-tuning uses a small held-out calibration workload and is not run on the evaluation queries.
```

中文：

```text
自动调参只使用独立校准查询集，不使用最终评测查询集。
```

### 4.3 更轻量的版本：固定默认参数

如果时间紧，不做 auto-tuning，也可以：

```text
主方法固定 IO_BLOCK_MBR b512。
IO_OVERFLOW 只作为 sensitivity / stress-test。
```

这是最稳的 VLDB 路线。

缺点：

```text
少了一个“自适应系统”的亮点。
```

优点：

```text
故事更清楚，风险更低。
```

### 4.4 不建议现在做复杂 cost model

暂时不建议直接做很复杂的代价模型。

原因：

```text
1. 需要解释模型为什么可靠。
2. 需要处理 GEOS exact refinement 成本，这个成本和 geometry 复杂度有关。
3. 如果模型预测不准，会变成新的审稿攻击点。
```

更推荐先做：

```text
sample-based auto-tuning
```

也就是用少量 training queries 直接测几个配置，选最快的。

## 5. 是否应该加入 KNN 查询

结论：

```text
KNN 可行，但不建议作为当前 VLDB 主线的下一步。
```

### 5.1 为什么 KNN 有吸引力

KNN 看起来能扩展论文覆盖面：

```text
Point/range/intersects/update 之外，再支持 nearest-neighbor。
```

如果做得好，确实能增强系统完整性。

### 5.2 为什么现在风险很高

KNN 不是简单加一个 benchmark。

原因：

```text
1. Z-order 近邻不等于真实欧氏距离近邻。
2. 必须有正确性保证，不能漏掉真正最近邻。
3. 需要 distance lower bound、迭代扩大搜索半径、候选验证等完整机制。
4. baseline 要换成 R-tree KNN、STRtree nearest-neighbor 等。
5. 评价指标也会变成 query latency、distance computations、candidate expansions、recall/correctness。
```

尤其要注意：

```text
当前 JTS STRtree KNN workload 主要用于生成 query window。
它不是说我们已经实现了真正的 KNN 查询方法。
```

### 5.3 推荐定位

当前不建议把 KNN 放进主线。

可以在论文 discussion/future work 写：

```text
The same interval-aware design suggests future extensions to distance queries,
but exact KNN requires distance-bound reasoning beyond Z-order overlap.
```

中文：

```text
我们的 interval-aware 设计未来可能扩展到距离查询，
但精确 KNN 需要额外的距离下界推理，不能直接由 Z-order overlap 保证。
```

如果一定要做 KNN，建议作为单独分支：

```text
先做 prototype，不影响当前 Intersects 论文主线。
```

## 6. VLDB 论文应该怎么讲故事

### 6.1 不推荐的故事

不建议讲：

```text
我们在 GLIN 上做了很多小修小补：
buffered insert、LSM、background compaction、interval overlap、overflow、synthetic mixed dataset、KNN ...
```

这样会让审稿人感觉：

```text
目标分散。
主贡献不清楚。
像工程补丁集合，而不是一个系统性思想。
```

### 6.2 推荐的故事

推荐主线：

```text
Learned spatial indexes built on Z-order keys are efficient for point/range-like access,
but exact Intersects queries over complex geometries expose a mismatch:
Zmin-only indexing loses the object's spatial extent and causes candidate inflation.

We show that representing each geometry as a Z-address interval [Zmin, Zmax]
enables deterministic, false-negative-free block pruning.

We design IO_BLOCK_MBR, an interval-overlap-aware block index that combines:
Zmin-ordered layout,
block maxZmax,
block MBR,
record MBR,
and exact GEOS refinement.
```

中文：

```text
GLIN 类方法把空间对象压到 Z-order key 上以后，适合点查和范围查；
但是复杂 geometry 的 Intersects 查询需要知道对象跨度。
只索引 Zmin 会丢掉 Zmax 信息，导致查询必须扩大范围，从而产生大量候选。

我们的核心思想是把对象看成 [Zmin, Zmax] 区间，
用 interval overlap 做安全剪枝。
```

### 6.3 可写成的贡献

贡献 1：工作负载特征分析

```text
我们系统分析 GLIN-piecewise 在 Intersects 查询中的 candidate inflation 问题。
```

需要证据：

```text
candidate/answer ratio
exact_calls
GLIN-piecewise vs Boost_Rtree / IO_BLOCK_MBR
```

贡献 2：interval-overlap-aware safe pruning

```text
提出 [Zmin, Zmax] overlap + block maxZmax + block MBR 的 false-negative-free pruning。
```

需要证据：

```text
answers_match_boost=1
safe pruning argument
ablation: maxZmax only, block MBR only, both
```

贡献 3：面向 fat objects 的鲁棒性扩展

```text
提出 IO_OVERFLOW，用 overflow R-tree 分流长区间对象。
```

建议定位：

```text
robustness extension / sensitivity study
```

不要定位：

```text
最终默认方法，除非 auto-tuning 在 held-out test 上稳定有效。
```

贡献 4：动态更新扩展

```text
GLIN_LSM_BG 说明 interval/GLIN 思想可以和异步增量维护结合。
```

建议定位：

```text
system extension 或 discussion。
```

如果篇幅不够，动态更新可以弱化，不要抢主线。

## 7. 下一步优先级

### P0：先修正 ZGAP_MIXED 正式数据规模

当前发现：

```text
results/interval_overlap_mixed_1000000/interval_overlap_summary.csv
里面 loaded_count = 10000
data/synthetic/zrange_gap/ZGAP_MIXED.wkt 也是 10000 行
```

这说明：

```text
目录名虽然写 1000000，但实际数据还是之前 smoke test 的 10000 条。
```

原因：

```text
PREPARE_DATA=1 只在文件不存在时生成。
如果旧的 ZGAP_MIXED.wkt 已经存在，它不会自动覆盖。
```

正式重跑前必须：

```text
1. 重新生成 1M ZGAP_MIXED 到新的文件名或新目录。
2. 或者显式删除旧的 10k 文件后再生成。
3. 最好用独立 OUT_DIR，避免 smoke 数据污染正式实验。
```

推荐命令另写，不要覆盖已有 smoke：

```bash
NUM=1000000 \
OUT_DIR=data/synthetic/zrange_gap_mixed_1000000 \
NAME=ZGAP_MIXED \
AUTO_BUILD=0 \
scripts/prepare_zrange_mixed_dataset.sh
```

然后跑：

```bash
RESET_RESULTS=1 OVERWRITE=1 \
DATASETS=ZGAP_MIXED \
DATA_FILE_ZGAP_MIXED=data/synthetic/zrange_gap_mixed_1000000/ZGAP_MIXED.wkt \
LIMIT=1000000 QUERY_LIMIT=1000000 \
AUTO_GENERATE_QUERIES=1 \
QUERY_ROOT=queries/interval_overlap_mixed_1000000_real \
RESULT_DIR=results/interval_overlap_mixed_1000000_real \
FIGURE_DIR=figures/interval_overlap_mixed_1000000_real \
SELECTIVITY_TAGS="0p001pct 0p01pct 0p1pct 1pct" \
BLOCK_SIZES="512 1024 2048" \
INCLUDE_IO_BLOCK_MBR=1 INCLUDE_IO_OVERFLOW=1 \
OVERFLOW_FRACTIONS="0.001 0.01 0.05" \
INCLUDE_QUADTREE=1 \
./scripts/run_interval_overlap_diagnostics.sh
```

### P1：先做 ablation，不要先做 KNN

必须补：

```text
A0: GLIN-piecewise
A1: Zmin upper_bound + scan
A2: A1 + block maxZmax
A3: A2 + block MBR
A4: A3 + record MBR
A5: A4 + overflow
```

目的：

```text
证明每个机制到底贡献了什么。
```

这是 VLDB 审稿人最关心的。

### P2：做 held-out auto-tuning 原型

如果 P1 后还有时间，再做：

```text
IO_ADAPTIVE
```

最小可行版本：

```text
1. training queries 上选择参数。
2. test queries 上报告性能。
3. 报告 tuning overhead。
4. 对比固定 b512 的 IO_BLOCK_MBR。
```

如果 auto-tuning 结果稳定，就作为贡献。

如果不稳定，就作为负结果：

```text
简单 sample-based tuning 不能可靠超过 R-tree，
说明 learned Z-order interval index 的瓶颈不是单纯参数选择。
```

负结果也有价值，但不要让它成为主线。

### P3：补 memory/build time/tail latency

VLDB 系统论文不能只看平均查询时间。

建议补：

```text
build time
index memory
p50/p95/p99 query latency
exact predicate calls
candidate/answer ratio
answers_match_boost
```

### P4：KNN 先放 future work 或单独分支

除非主线实验已经很稳，否则不要现在把 KNN 加进主论文。

## 8. 当前最推荐的论文范围

推荐范围：

```text
主题：Exact Intersects queries over GLIN-style learned Z-order indexes.

主方法：IO_BLOCK_MBR。

增强：IO_OVERFLOW / IO_ADAPTIVE 作为 robustness extension。

动态更新：GLIN_LSM_BG 作为可选系统扩展或 discussion。

KNN：future work，不进入主线。
```

一句话版本：

```text
不要试图写“我们做了一个全能 learned spatial index”。
要写“我们解决了 GLIN-style learned spatial index 在 exact Intersects 查询上的核心候选膨胀问题”。
```

这条线更容易讲清楚，也更容易防守。

