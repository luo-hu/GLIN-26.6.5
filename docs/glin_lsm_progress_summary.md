# GLIN 动态更新改进进展总结

最后更新日期：2026-06-08

本文档记录目前围绕 GLIN 动态更新做过哪些改进、如何测试、增加了哪些实验指标、当前效果如何，以及哪些内容可以作为论文创新点。

固定英文术语说明：

- `GLIN`: 当前项目中的 learned spatial index，基于空间对象的 Z-order/Zmin 编码进行索引。
- `ALEX`: Adaptive Learned Index，是 GLIN 底层使用或借鉴的动态 learned index 结构。
- `Zmin`: 空间对象映射到 Z-order 曲线后的最小 key。按 `Zmin` 插入通常更接近顺序插入。
- `LSM`: Log-Structured Merge-tree，日志结构合并树。这里借用它的思想：先写 delta，再批量合并。
- `delta`: 临时增量区，专门接收新插入的数据。
- `segment`: 一个已经从 delta 批量构建出来的小 GLIN 索引段。
- `compaction`: 合并多个 segment，减少查询时需要访问的索引数量。
- `foreground`: 前台路径，指用户查询/插入正在执行时直接等待的部分。
- `background`: 后台路径，指异步维护任务，比如建 segment 或 compaction。
- `drain`: workload 结束后等待后台任务收尾。

## 1. 研究问题

原始 GLIN 在随机动态插入下表现不好，主要原因是底层 ALEX/GLIN 的数据节点接近“有序数组 + gap”的结构。

随机插入容易触发：

```text
1. 叶子数组内部搬移元素。
2. 扩容 resize。
3. 节点 split/retrain。
4. leaf MBR 更新。
```

但是，如果按 `Zmin` 顺序插入，插入过程更像 append 或 near-append，也就是追加或接近追加。此时 GLIN 的有序数组结构反而有优势。

因此当前研究问题是：

> 能不能保留 GLIN 在 Z-order 有序数据上的优势，同时通过 buffer、delta index、segment 和 background compaction，让它也能处理随机动态更新？

## 2. 当前改了哪些文件

### 2.1 插入诊断 benchmark

文件：

```text
src/benchmark/bench_update_wkt.cpp
```

新增变体：

```text
GLIN_BUFFERED
GLIN_LSM
GLIN_LSM_ASYNC
```

新增指标：

```text
maintenance_ns
merge_count
delta_pending
buffer_size
delta_size
```

指标解释：

- `maintenance_ns`: 维护时间，例如 rebuild、merge、compaction 花了多久。
- `merge_count`: 发生了多少次 merge 或 rebuild。
- `delta_pending`: 还有多少数据留在 delta 里。
- `buffer_size`: micro-batch buffer 的大小。
- `delta_size`: delta 满多少条后触发 flush/merge。

### 2.2 Hybrid 查询 + 更新 benchmark

文件：

```text
src/benchmark/bench_hybrid_wkt.cpp
```

新增变体：

```text
GLIN_LSM_ASYNC
GLIN_LSM_SEGMENTED
GLIN_LSM_SEGMENTED4
GLIN_LSM_BG
```

新增指标：

```text
maintenance_ns
drain_ns
total_elapsed_ns
throughput_transactions_per_sec
total_throughput_transactions_per_sec
candidate_answer_ratio
answers_match_boost
answers_delta_vs_boost
delta_pending
segment_count
segments_pruned_total
```

指标解释：

- `throughput_transactions_per_sec`: 前台事务吞吐，只看 workload 正在执行期间的速度。
- `total_throughput_transactions_per_sec`: 总吞吐，把最后 `drain_ns` 也算进去。
- `drain_ns`: workload 结束后，为了等后台任务完成又花了多久。
- `candidate_answer_ratio`: 候选数量 / 真答案数量，越接近 1，说明过滤越准。
- `answers_match_boost`: 是否和 Boost R-tree 的答案数量一致。`1` 表示一致。
- `answers_delta_vs_boost`: 和 Boost R-tree 的答案数量差值。
- `segment_count`: 当前有多少个 GLIN segment。
- `segments_pruned_total`: 查询时被剪枝跳过的 segment 总数。

### 2.3 实验脚本

文件：

```text
scripts/run_fig15_insert_diagnostics.sh
```

新增环境变量：

```text
INCLUDE_BUFFERED_GLIN
BUFFER_SIZE
INCLUDE_LSM_GLIN
INCLUDE_LSM_ASYNC_GLIN
DELTA_SIZE
```

文件：

```text
scripts/run_fig17_hybrid_1m.sh
```

新增环境变量：

```text
INCLUDE_LSM_ASYNC_GLIN
INCLUDE_LSM_SEGMENTED_GLIN
INCLUDE_LSM_SEGMENTED4_GLIN
INCLUDE_LSM_BG_GLIN
DELTA_SIZE
```

文件：

```text
scripts/plot_fig17_hybrid.py
```

新增输出图：

```text
fig17_hybrid_delta_pending_panel.png
fig17_hybrid_segment_count_panel.png
fig17_hybrid_maintenance_ns_panel.png
```

新增诊断报告：

```text
fig17_hybrid_summary_diagnostics.txt
```

这个诊断报告会用中文解释 `foreground throughput`、`maintenance_ns`、`drain_ns` 等指标，方便以后直接查看。

## 3. 各个 GLIN 改进版本说明

### 3.1 GLIN_BUFFERED

实现位置：

```text
src/benchmark/bench_update_wkt.cpp
```

核心思想：

随机插入不是一条来一条立刻插入，而是先缓存一小批。每批先按 `Zmin` 排序，再插入 GLIN。

流程：

```text
random arrival -> buffer -> sort by Zmin -> GLIN insert
```

参数：

```text
--include_buffered_glin 1
--buffer_size N
```

优点：

```text
1. 实现简单。
2. 很容易解释。
3. 能直接验证“按 Zmin 插入有利于 GLIN”这个假设。
4. 不需要 full rebuild。
```

缺点：

```text
1. 目前只用于 insert diagnostics，没有做成完整的 query/update 系统。
2. 会牺牲实时性，因为数据要先等在 buffer 里。
3. 当前结果没有强到足够作为最终方法。
```

论文定位：

```text
可以作为动机实验或 ablation。
不建议作为最终创新方法。
```

### 3.2 GLIN_LSM

实现位置：

```text
src/benchmark/bench_update_wkt.cpp
```

核心思想：

维护一个 main GLIN 和一个 delta R-tree。新插入数据先进 delta。delta 满了以后，把 main + delta 的所有数据重新按 `Zmin` 排序，然后重建 main GLIN。

流程：

```text
main GLIN + delta R-tree
delta full -> sort all main+delta by Zmin -> rebuild main GLIN
```

优点：

```text
1. 思路清楚。
2. 能验证 delta buffer + rebuild 的基本想法。
3. maintenance_ns 可以清楚暴露 full rebuild 成本。
```

缺点：

```text
1. full rebuild 太重。
2. DELTA_SIZE 小时 rebuild 太频繁。
3. DELTA_SIZE 大时查询要查很大的 delta，query 变慢。
4. 作为系统方案不稳定。
```

论文定位：

```text
不能作为最终方法。
可以用来说明 naive LSM/full rebuild 不够好。
```

### 3.3 GLIN_LSM_ASYNC

实现位置：

```text
src/benchmark/bench_update_wkt.cpp
src/benchmark/bench_hybrid_wkt.cpp
```

核心思想：

把前台写入时间和维护时间拆开统计。也就是 `update_ns` 只统计写入 delta 的时间，rebuild/merge 的时间单独记到 `maintenance_ns`。

优点：

```text
1. 能看到“如果只看前台写 delta，写入可以很快”。
2. 能说明 async maintenance 有必要。
3. 是理解后续 GLIN_LSM_BG 的铺垫。
```

缺点：

```text
1. update-only 版本不是完整在线系统。
2. hybrid 版本仍然受 full rebuild 或大 delta 双读影响。
3. 如果只报 update_ns，会被质疑隐藏了 maintenance 成本。
```

论文定位：

```text
不能作为最终方法。
适合作为动机实验：前台写入便宜，但维护调度必须认真设计。
```

### 3.4 GLIN_LSM_SEGMENTED

实现位置：

```text
src/benchmark/bench_hybrid_wkt.cpp
```

核心思想：

避免每次 delta 满了就 full rebuild。delta 满后构建一个新的 GLIN segment。查询时查：

```text
base GLIN + all GLIN segments + current delta
```

优点：

```text
1. 避免频繁 full rebuild。
2. 比 GLIN_LSM_ASYNC 更像一个真实在线系统。
```

缺点：

```text
1. segment 太多时查询会变慢。
2. flush 和 compaction 还是同步执行，会挡住前台。
3. 没有强剪枝时，查询要查太多 segment。
```

论文定位：

```text
不建议作为最终方法。
可以作为中间版本说明为什么需要 pruning 和 compaction。
```

### 3.5 GLIN_LSM_SEGMENTED4

实现位置：

```text
src/benchmark/bench_hybrid_wkt.cpp
```

核心思想：

在 segmented LSM 基础上加入两个机制：

```text
1. fanout=4 size-tiered compaction
2. segment pruning
```

`fanout=4` 的意思是：同一层有 4 个 segment 后合并成上一层 1 个 segment。

`segment pruning` 的意思是：查询时，如果一个 segment 的 envelope 或 Zmin/Zmax 范围明显不可能和 query 相交，就直接跳过它。

流程：

```text
delta R-tree -> GLIN segment
4 same-level segments -> compact into 1 higher-level segment
query -> base GLIN + pruned segments + delta
```

优点：

```text
1. 当前 Fig.17 测试中 correctness 好，answers_match_boost=1。
2. 比普通 segmented 版本更稳定。
3. 很适合作为 ablation，对比 GLIN_LSM_BG。
```

缺点：

```text
1. flush 和 compaction 仍然是同步维护。
2. foreground throughput 明显低于 GLIN_LSM_BG。
```

论文定位：

```text
适合作为 ablation baseline。
不建议作为最终系统方案。
```

### 3.6 GLIN_LSM_BG

实现位置：

```text
src/benchmark/bench_hybrid_wkt.cpp
```

核心思想：

把 segmented GLIN-LSM 的维护任务放到后台。前台写入只写 delta R-tree。delta 满了以后变成 sealed pending delta。后台任务负责把 pending delta 构建成 GLIN segment，并做 compaction。

查询时需要查：

```text
base GLIN
+ compacted GLIN segments
+ pending delta R-trees
+ current delta R-tree
```

当前实现细节：

```text
1. 使用 std::async 启动一个后台维护任务。
2. 使用 fanout=4。
3. 查询时使用 envelope + Zmin/Zmax 做 segment pruning。
4. 使用 grouped delta flush：
   workload 过程中，攒够 4 个 sealed delta 才构建 GLIN segment；
   workload 结束 drain 时，再处理不足 4 个的尾批。
5. 同时报告 foreground throughput 和 total throughput。
```

优点：

```text
1. 当前最可用的 GLIN 动态更新扩展。
2. 当前 2M hybrid 结果中 answers_match_boost=1，正确性对齐 Boost R-tree。
3. 写密集 workload 中 foreground throughput 超过 Boost R-tree。
4. maintenance_ns 明显低于 GLIN_LSM_SEGMENTED4。
```

缺点：

```text
1. 它是异步/后台版本，不是原始单线程 GLIN baseline。
2. drain_ns 仍然不小，说明后台维护还会欠账。
3. 写密集 workload 中，如果看 total throughput，仍可能输给 Boost R-tree。
```

论文定位：

```text
当前最推荐保留的系统变体。
应该称为 system-level asynchronous variant。
不能说它是原始 GLIN baseline。
不能说它所有指标都超过 Boost R-tree。
```

## 4. 如何运行实验

### 4.1 编译

```bash
cmake --build build --target bench_update_wkt bench_update_wkt_piece -j2
cmake --build build --target bench_hybrid_wkt_piece -j2
```

说明：

```text
GEOS 和 Boost 的 warning 很常见。
只要最终 exit code 是 0，并且显示 Built target，就说明编译通过。
```

### 4.2 Fig.15 插入诊断实验

默认插入顺序和 Boost 策略诊断：

```bash
RESET_RESULTS=1 ./scripts/run_fig15_insert_diagnostics.sh
```

运行 GLIN_BUFFERED：

```bash
INCLUDE_BUFFERED_GLIN=1 BUFFER_SIZE=10000 ./scripts/run_fig15_insert_diagnostics.sh
```

运行 update-only LSM 变体：

```bash
INCLUDE_LSM_GLIN=1 INCLUDE_LSM_ASYNC_GLIN=1 DELTA_SIZE=100000 ./scripts/run_fig15_insert_diagnostics.sh
```

输出：

```text
results/fig15_insert_diagnostics/fig15_insert_diagnostics_summary.csv
results/fig15_insert_diagnostics/insert_order_sweep.csv
figures/fig15_insert_diagnostics/insert_order_sweep.png
figures/fig15_insert_diagnostics/boost_split_strategy_sweep.png
figures/fig15_insert_diagnostics/cell_size_sweep_glin.png
```

### 4.3 Fig.17 Hybrid 查询 + 更新实验

当前推荐命令：

```bash
RESET_RESULTS=1 INCLUDE_LSM_BG_GLIN=1 INCLUDE_LSM_SEGMENTED4_GLIN=1 LIMIT=2000000 DELTA_SIZE=100000 ./scripts/run_fig17_hybrid_1m.sh
```

小规模 smoke test：

```bash
RESET_RESULTS=1 DATASETS=ROADS WORKLOAD=write LIMIT=200000 QUERY_LIMIT=200000 INCLUDE_LSM_BG_GLIN=1 INCLUDE_LSM_SEGMENTED4_GLIN=1 DELTA_SIZE=10000 PROGRESS_STEP_PERCENT=10 ./scripts/run_fig17_hybrid_1m.sh
```

输出：

```text
results/fig17_hybrid_2000000/fig17_hybrid_summary.csv
results/fig17_hybrid_2000000/fig17_hybrid_progress.csv
figures/fig17_hybrid_2000000/fig17_hybrid_curves_panel.png
figures/fig17_hybrid_2000000/fig17_hybrid_delta_pending_panel.png
figures/fig17_hybrid_2000000/fig17_hybrid_segment_count_panel.png
figures/fig17_hybrid_2000000/fig17_hybrid_maintenance_ns_panel.png
figures/fig17_hybrid_2000000/fig17_hybrid_summary_diagnostics.txt
```

## 5. 新指标怎么看

### foreground throughput

CSV 字段：

```text
throughput_transactions_per_sec
```

含义：

```text
workload 正在执行时，前台看到的吞吐。
对于 async 方法，它代表用户在线查询/写入时感受到的速度。
```

### total throughput

CSV 字段：

```text
total_throughput_transactions_per_sec
```

含义：

```text
把 workload_ns 和 drain_ns 加在一起以后计算的吞吐。
这个指标更严格，因为它把最后后台收尾时间也算进去了。
```

### maintenance_ns

含义：

```text
后台或同步维护花掉的建索引、flush、merge、compaction 时间。
数值越大，说明维护成本越重。
```

### drain_ns

含义：

```text
workload 结束后，等待后台任务完成的时间。
数值越大，说明后台没有跟上前台，欠了维护债。
```

### delta_pending

含义：

```text
还在 current delta 或 sealed pending delta 里的记录数。
太大时，query 需要额外查很多 delta R-tree。
```

### segment_count

含义：

```text
base GLIN 之外，还有多少个 GLIN segment。
太大时，query 可能要查很多小索引。
```

### candidate_answer_ratio

含义：

```text
candidates_total / answers_total
候选数量 / 真答案数量。
越接近 1，说明过滤越准。
```

### answers_match_boost

含义：

```text
1 表示答案数量和 Boost R-tree 一致。
0 表示要检查正确性或统计口径。
```

重要注意：

```text
当前 Fig.17 summary 里 GLIN_PIECEWISE 的 answers_match_boost=0。
所以不要用 GLIN-piecewise 当正确性 oracle。
当前应该用 Boost R-tree 作为正确性参照。
```

## 6. 当前实验结果

### 6.1 插入顺序诊断

结果文件：

```text
results/fig15_insert_diagnostics/insert_order_sweep.csv
```

核心观察：

```text
随机插入时 GLIN 吃亏。
按 Zmin 顺序插入时 GLIN 往往超过 Boost R-tree。
```

部分结果：

| Dataset | Order | GLIN ops/s | Boost R-tree ops/s | GLIN-piecewise ops/s | 观察 |
|---|---:|---:|---:|---:|---|
| AW | random | 582,709 | 904,195 | 563,943 | GLIN 随机插入较弱 |
| AW | zmin | 2,206,970 | 1,883,200 | 2,202,650 | GLIN 按 Zmin 插入更强 |
| LW | random | 591,586 | 926,645 | 574,606 | GLIN 随机插入较弱 |
| LW | zmin | 2,217,880 | 1,903,880 | 2,192,780 | GLIN 按 Zmin 插入更强 |
| ROADS | random | 732,469 | 921,380 | 707,036 | GLIN 随机插入较弱 |
| ROADS | zmin | 1,559,280 | 1,389,160 | 1,544,810 | GLIN 按 Zmin 插入更强 |
| PARKS | random | 386,030 | 959,662 | 382,799 | GLIN 随机插入明显吃亏 |
| PARKS | zmin | 1,309,360 | 1,164,110 | 1,328,000 | GLIN 按 Zmin 插入更强 |

论文价值：

```text
这是很有价值的 workload characterization。
它说明 GLIN 的动态插入性能和插入顺序强相关。
它可以作为后续 buffer/LSM 设计的动机。
但它本身不是完整系统创新。
```

### 6.2 2M Hybrid 结果

结果文件：

```text
results/fig17_hybrid_2000000/fig17_hybrid_summary.csv
```

运行命令：

```bash
RESET_RESULTS=1 INCLUDE_LSM_BG_GLIN=1 INCLUDE_LSM_SEGMENTED4_GLIN=1 LIMIT=2000000 DELTA_SIZE=100000 ./scripts/run_fig17_hybrid_1m.sh
```

#### 读密集 workload

读密集的意思是：查询比例高，更新比例低。当前脚本中大致是 90% query + 10% insert transaction。

| Dataset | Index | Foreground tps | Total tps | maintenance_s | drain_s | 正确性 |
|---|---|---:|---:|---:|---:|---:|
| ROADS | GLIN-LSM-bg | 71.201 | 66.922 | 2.203 | 0.449 | OK |
| ROADS | Boost R-tree | 67.846 | 67.846 | 0 | 0 | OK |
| ROADS | GLIN-LSM-seg4 | 45.540 | 45.540 | 3.916 | 0 | OK |
| PARKS | GLIN-LSM-bg | 55.528 | 52.723 | 2.376 | 0.479 | OK |
| PARKS | Boost R-tree | 52.748 | 52.748 | 0 | 0 | OK |
| PARKS | GLIN-LSM-seg4 | 38.092 | 38.092 | 4.221 | 0 | OK |

解释：

```text
GLIN_LSM_BG 的 foreground throughput 略高于 Boost R-tree。
如果看 total throughput，则和 Boost R-tree 接近。
它明显好于同步维护的 GLIN_LSM_SEGMENTED4。
```

#### 写密集 workload

写密集的意思是：查询和更新比例接近，当前脚本中大致是 50% query + 50% insert transaction。

| Dataset | Index | Foreground tps | Total tps | maintenance_s | drain_s | 正确性 |
|---|---|---:|---:|---:|---:|---:|
| ROADS | GLIN-LSM-bg | 51.607 | 34.921 | 2.110 | 0.926 | OK |
| ROADS | Boost R-tree | 37.869 | 37.869 | 0 | 0 | OK |
| ROADS | GLIN-LSM-seg4 | 17.214 | 17.214 | 3.894 | 0 | OK |
| PARKS | GLIN-LSM-bg | 46.321 | 31.807 | 2.323 | 0.985 | OK |
| PARKS | Boost R-tree | 36.281 | 36.281 | 0 | 0 | OK |
| PARKS | GLIN-LSM-seg4 | 15.137 | 15.137 | 4.376 | 0 | OK |

解释：

```text
如果只看 foreground throughput，GLIN_LSM_BG 超过 Boost R-tree：
ROADS 大约高 36%。
PARKS 大约高 28%。

但如果看 total throughput，GLIN_LSM_BG 仍低于 Boost R-tree。
原因是 drain_ns 仍然较大，说明后台维护没有完全跟上前台。
```

### 6.3 维护成本对比

`GLIN_LSM_BG` 相比 `GLIN_LSM_SEGMENTED4` 明显减少了维护成本。

| Dataset | Workload | seg4 maintenance_s | bg maintenance_s | 降低幅度 |
|---|---|---:|---:|---:|
| ROADS | read | 3.916 | 2.203 | 约 44% |
| ROADS | write | 3.894 | 2.110 | 约 46% |
| PARKS | read | 4.221 | 2.376 | 约 44% |
| PARKS | write | 4.376 | 2.323 | 约 47% |

这说明 grouped flush + background scheduling 是有效的。

## 7. 哪些可以算论文创新点

目前最稳的论文故事不是“GLIN-LSM-bg 全面打败 R-tree”，而是下面这个组合：

### 7.1 Workload characterization

中文意思：工作负载特征分析。

可以写：

```text
GLIN 的动态插入性能对插入顺序非常敏感。
随机插入破坏 ALEX/GLIN 的有序数组优势。
按 Zmin 顺序插入时，GLIN 的性能显著提升。
```

这是一个比较强的观察。

### 7.2 System mechanism

中文意思：系统机制设计。

可以写：

```text
使用 delta index 吸收随机写入。
将随机 arrival 转换成按 Zmin 排序的 GLIN segment。
使用 fanout=4 compaction 控制 segment 数量。
使用 segment pruning 降低查询访问的 segment 数。
使用 background maintenance 降低 foreground stall。
```

这部分是 GLIN-LSM-bg 的核心贡献。

### 7.3 Evaluation artifact

中文意思：评测方法和指标贡献。

可以写：

```text
同时报告 foreground throughput 和 total throughput。
显式报告 maintenance_ns、drain_ns、delta_pending、segment_count。
使用 Boost R-tree 做 correctness reference。
```

这很重要，因为 async 方法如果只报 foreground throughput，会被审稿人质疑不公平。

## 8. 哪些结论目前支持，哪些不支持

### 8.1 已经支持的结论

```text
1. 随机插入是 GLIN/ALEX 风格结构的弱点。
2. Zmin 顺序插入能显著提升 GLIN 插入性能。
3. naive full-rebuild LSM 不够好。
4. segmented LSM 能避免 full rebuild，但必须配合 pruning 和 compaction。
5. GLIN_LSM_BG 能提升 foreground throughput。
6. 当前 2M hybrid 结果中，GLIN_LSM_BG 和 Boost R-tree 答案一致。
```

### 8.2 部分支持的结论

```text
1. GLIN_LSM_BG 在 foreground throughput 上可以超过 Boost R-tree。
2. background maintenance 可以减少前台阻塞。
```

注意这里要写“foreground throughput”，不能写“所有吞吐指标”。

### 8.3 目前不支持的结论

```text
1. GLIN_LSM_BG 全面超过 Boost R-tree。
2. background maintenance 是免费的。
3. async 变体和单线程 baseline 完全公平。
4. GLIN-piecewise 在当前 hybrid query accounting 下可以作为正确性 oracle。
```

这些话论文里不能直接说。

## 9. 哪个版本真正可用

### 9.1 推荐作为最终系统变体

```text
GLIN_LSM_BG
```

原因：

```text
1. 当前效果最好。
2. 正确性对齐 Boost R-tree。
3. foreground throughput 有优势。
4. 维护成本比同步 segmented4 更低。
5. 机制完整：delta、segment、pruning、background maintenance 都有。
```

但要注意：

```text
它是 system-level asynchronous variant。
不能当成原始 GLIN baseline。
```

### 9.2 推荐作为 ablation

```text
GLIN_LSM_SEGMENTED4
GLIN_LSM_ASYNC
GLIN_LSM
GLIN_BUFFERED
```

`ablation` 的意思是消融实验，也就是有意去掉某些机制，看看性能为什么变好或变差。

### 9.3 不建议作为最终方法

#### GLIN_BUFFERED

原因：

```text
只适合 insert-only 诊断。
不是完整 query/update 系统。
实时性较弱。
```

#### GLIN_LSM

原因：

```text
full rebuild 成本太高。
DELTA_SIZE 难以同时兼顾写入和查询。
```

#### GLIN_LSM_ASYNC

原因：

```text
更像 write-path idealization，也就是理想化写入路径。
不是完整公平系统结果。
```

#### GLIN_LSM_SEGMENTED

原因：

```text
segment 太多时查询负担重。
缺少足够强的 pruning/compaction。
```

#### GLIN_LSM_SEGMENTED4

原因：

```text
正确性好，也适合作为对照。
但同步维护太重，foreground throughput 不好。
```

### 9.4 需要谨慎使用的版本

```text
GLIN_PIECEWISE
```

原因：

```text
它仍然是有价值的 GLIN 变体。
但当前 Fig.17 hybrid summary 中 answers_match_boost=0。
所以不能用它当 correctness oracle。
```

## 10. 论文中应该怎么写

### 推荐表述

```text
We report GLIN-LSM-bg as a system-level asynchronous extension.
Its foreground throughput measures the online query/update path,
while total throughput includes the final drain cost of background maintenance.
```

中文意思：

```text
我们把 GLIN-LSM-bg 作为系统级异步扩展来报告。
foreground throughput 反映在线查询/更新路径性能；
total throughput 则把后台维护最终收尾成本也计算进去。
```

### 不建议这样写

```text
GLIN-LSM-bg is the new GLIN baseline.
```

问题：

```text
它不是原始 GLIN baseline，而是新增的系统级异步扩展。
```

不建议这样写：

```text
GLIN-LSM-bg always outperforms Boost R-tree.
```

问题：

```text
当前 total throughput 在写密集场景下没有超过 Boost R-tree。
```

### 更稳妥的 claim

```text
GLIN-LSM-bg improves foreground update throughput under hybrid workloads by
decoupling random writes from sorted GLIN segment construction. The improvement
comes with deferred maintenance, which is exposed through maintenance_ns and
drain_ns.
```

中文意思：

```text
GLIN-LSM-bg 通过把随机写入和有序 GLIN segment 构建解耦，提高了 hybrid workload 下的前台更新吞吐。
这种提升不是免费的，它带来了延迟维护成本；我们用 maintenance_ns 和 drain_ns 显式报告这个成本。
```

## 11. 当前决策

目前不建议继续为了小幅提升盲目改实现。

推荐冻结当前实现作为研究原型：

```text
1. GLIN_LSM_BG 作为主系统扩展。
2. GLIN_LSM_SEGMENTED4 作为主要 ablation。
3. GLIN_BUFFERED、GLIN_LSM、GLIN_LSM_ASYNC 作为诊断/动机实验。
4. 后续重点转向更扎实的评测和论文表述。
```

## 12. 后续推荐实验

### 高优先级

```text
1. 多个 seed 重复 2M hybrid workload。
2. 跑更大规模，例如 5M 或 10M，看 drain_ns 相对占比是否下降。
3. foreground throughput 和 total throughput 同时报告。
4. 同时报告 maintenance_ns、drain_ns、delta_pending、segment_count。
5. 如果可以，补充 memory usage。
```

### 中优先级

```text
1. 做 DELTA_SIZE sensitivity study。
2. 比较不同 query selectivity，例如 0.1%、1%、10%。
3. 如果 AW/LW 的 hybrid query generation 稳定，可以补充 AW/LW。
```

`sensitivity study` 的意思是敏感性实验，也就是改变一个参数，看结果是否稳定。

### 低优先级

```text
1. 多后台 worker。
2. 更复杂的 compaction scheduling。
3. 删除 delete 也完整接入 segmented GLIN-LSM。
```

这些方向有价值，但容易变成大量工程工作。如果论文时间紧，不建议优先做。

