# IntervalOverlapIndex 实验脚本中文速查

这个文档解释：

```text
scripts/run_interval_overlap_diagnostics.sh
```

它用于跑 IntervalOverlapIndex 的 `Intersects` 查询实验，并和下面方法对比：

```text
GLIN_PIECEWISE
Boost_Rtree
GEOS_Quadtree
```

注意：

```text
原始 GLIN 在当前仓库里跑的是 contains，不是 intersects。
所以 Intersects 正式对比里默认不加入原始 GLIN。
```

## 1. 先看脚本帮助

```bash
./scripts/run_interval_overlap_diagnostics.sh --help
```

如果忘了参数是什么意思，先跑这个。

## 2. 只跑已有 1% 查询

适合你已经有：

```text
queries/fig17_hybrid_2000000/ROADS_jts_strtree_knn_1pct.csv
queries/fig17_hybrid_2000000/PARKS_jts_strtree_knn_1pct.csv
```

命令：

```bash
RESET_RESULTS=1 \
DATASETS="ROADS PARKS" \
SELECTIVITY_TAGS=1pct \
BLOCK_SIZES=1024 \
./scripts/run_interval_overlap_diagnostics.sh
```

## 3. 只画图，不重新跑实验

这个最容易写错。

必须写：

```text
RUN_BENCHMARKS=0
RESET_RESULTS=0
```

命令：

```bash
RUN_BENCHMARKS=0 \
RESET_RESULTS=0 \
RESULT_DIR=results/interval_overlap_2000000_with_quadtree \
FIGURE_DIR=figures/interval_overlap_2000000_with_quadtree \
./scripts/run_interval_overlap_diagnostics.sh
```

解释：

```text
RUN_BENCHMARKS=0 表示不跑 C++ benchmark。
RESET_RESULTS=0 表示不要删除已有 CSV。
```

如果写成 `RUN_BENCHMARKS=0 RESET_RESULTS=1`，脚本会报错保护你，因为这会删除已有结果。

## 4. 跑不同 selectivity

`selectivity` 可以理解为 query 窗口大小/答案规模：

```text
0p001pct  很小
0p01pct   较小
0p1pct    中等
1pct      较大
```

命令：

```bash
RESET_RESULTS=1 \
AUTO_GENERATE_QUERIES=1 \
JTS_JAVA_HEAP=20g \
DATASETS="ROADS PARKS" \
SELECTIVITY_TAGS="0p001pct 0p01pct 0p1pct 1pct" \
BLOCK_SIZES=1024 \
RESULT_DIR=results/interval_overlap_selectivity_2000000 \
FIGURE_DIR=figures/interval_overlap_selectivity_2000000 \
./scripts/run_interval_overlap_diagnostics.sh
```

解释：

```text
AUTO_GENERATE_QUERIES=1 表示缺 query CSV 就自动生成。
JTS_JAVA_HEAP=20g 表示给 Java query generator 更多内存。
```

## 5. 跑 block size sensitivity

`block size` 是 IntervalOverlapIndex 里每个 block 放多少条记录。

```text
小 block：剪枝更细，但 block 数更多。
大 block：block 数更少，但剪枝更粗。
```

命令：

```bash
RESET_RESULTS=1 \
DATASETS="ROADS PARKS" \
SELECTIVITY_TAGS=1pct \
BLOCK_SIZES="256 512 1024 2048 4096" \
RESULT_DIR=results/interval_overlap_block_sweep_2000000 \
FIGURE_DIR=figures/interval_overlap_block_sweep_2000000 \
./scripts/run_interval_overlap_diagnostics.sh
```

如果对应 query 不存在，加：

```text
AUTO_GENERATE_QUERIES=1
```

## 6. 跑合成数据

合成数据不会默认跑。

你要显式指定：

```text
DATASETS="UNIF_S UNIF_L DIAG_S DIAG_L"
PREPARE_DATA=1
AUTO_GENERATE_QUERIES=1
```

命令：

```bash
RESET_RESULTS=1 \
DATASETS="UNIF_S UNIF_L DIAG_S DIAG_L" \
LIMIT=1000000 \
QUERY_LIMIT=1000000 \
PREPARE_DATA=1 \
AUTO_GENERATE_QUERIES=1 \
SYNTHETIC_KIND=rectangles \
QUERY_ROOT=queries/interval_overlap_synthetic_1000000 \
RESULT_DIR=results/interval_overlap_synthetic_1000000 \
FIGURE_DIR=figures/interval_overlap_synthetic_1000000 \
SELECTIVITY_TAGS="0p001pct 0p01pct 0p1pct 1pct" \
BLOCK_SIZES="512 1024 2048" \
./scripts/run_interval_overlap_diagnostics.sh
```

解释：

```text
SYNTHETIC_KIND=rectangles 表示生成矩形合成数据。
Intersects 查询更适合用 rectangles，而不是 points。
```

## 7. ZGAP_WIDE 没有怎么办

`ZGAP_WIDE` 是压力数据集，不是默认实验必须的数据。

如果你不跑它，就不用管：

```bash
DATASETS="ROADS PARKS"
```

如果你想跑它，而且文件不存在：

```bash
RESET_RESULTS=1 \
DATASETS=ZGAP_WIDE \
LIMIT=1000000 \
QUERY_LIMIT=1000000 \
PREPARE_DATA=1 \
AUTO_GENERATE_QUERIES=1 \
QUERY_ROOT=queries/interval_overlap_zgap_1000000 \
RESULT_DIR=results/interval_overlap_zgap_1000000 \
FIGURE_DIR=figures/interval_overlap_zgap_1000000 \
SELECTIVITY_TAGS="0p001pct 0p1pct 1pct" \
BLOCK_SIZES="512 1024 2048" \
./scripts/run_interval_overlap_diagnostics.sh
```

## 8. 输出在哪里

默认输出：

```text
results/interval_overlap_${LIMIT}/interval_overlap_summary.csv
figures/interval_overlap_${LIMIT}/
```

正式实验建议总是显式指定：

```text
RESULT_DIR=...
FIGURE_DIR=...
```

这样不同实验不会互相覆盖。



`run_interval_overlap_diagnostics.sh` 现在靠环境变量控制实验矩阵。最全面的一次运行，就是同时打开：

```text
真实数据 + 合成数据
4 种 selectivity
多个 block size
IntervalOverlapIndex + GLIN_PIECEWISE + Boost_Rtree + GEOS_Quadtree
自动生成缺失 query
自动生成 synthetic 数据
```

**最全面一条命令**
```bash
RESET_RESULTS=1 \
OVERWRITE=1 \
PREPARE_DATA=1 \
AUTO_GENERATE_QUERIES=1 \
JTS_JAVA_HEAP=20g \
DATASETS="AW LW ROADS PARKS UNIF_S UNIF_L DIAG_S DIAG_L ZGAP_WIDE" \
LIMIT=2000000 \
QUERY_LIMIT=2000000 \
SYNTHETIC_KIND=rectangles \
SELECTIVITY_TAGS="0p001pct 0p01pct 0p1pct 1pct" \
BLOCK_SIZES="256 512 1024 2048 4096" \
INCLUDE_QUADTREE=1 \
RESULT_DIR=results/interval_overlap_full_2000000 \
FIGURE_DIR=figures/interval_overlap_full_2000000 \
QUERY_ROOT=queries/interval_overlap_full_2000000 \
./scripts/run_interval_overlap_diagnostics.sh
```

这条命令会很久，因为实验数量大概是：

```text
9 个数据集
x 4 个 selectivity
x 5 个 block size 的 IntervalOverlapIndex
+ 每个数据集/selectivity 还要跑 GLIN_PIECEWISE、Boost_Rtree、GEOS_Quadtree
```

也就是：

```text
IntervalOverlapIndex: 9 x 4 x 5 = 180 次
baseline: 9 x 4 x 3 = 108 次
总共约 180+108=288次 benchmark


第一部分：IntervalOverlapIndex 运行次数该索引需要遍历：数据集 × 选择性 × 块大小:9 x 4 x 5 = 180 次 
第二部分：三组基线索引总运行次数基线共 3 个索引：
GLIN_PIECEWISE + Boost_Rtree + GEOS_Quadtree单个基线索引遍历：数据集 × 选择性（无 block size，只有IntervalOverlapIndex这个才需要）
单个基线次数：
9 x 4 = 36次
三个基线总和：3 x 36 = 108次



```

**更推荐的论文版全量**
如果担心太慢，我建议先用这个，更合理：

```bash
RESET_RESULTS=1 \
OVERWRITE=1 \
PREPARE_DATA=1 \
AUTO_GENERATE_QUERIES=1 \
JTS_JAVA_HEAP=20g \
DATASETS="ROADS PARKS UNIF_S UNIF_L DIAG_S DIAG_L ZGAP_WIDE" \
LIMIT=1000000 \
QUERY_LIMIT=1000000 \
SYNTHETIC_KIND=rectangles \
SELECTIVITY_TAGS="0p001pct 0p01pct 0p1pct 1pct" \
BLOCK_SIZES="512 1024 2048" \
INCLUDE_QUADTREE=1 \
RESULT_DIR=results/interval_overlap_full_1000000 \
FIGURE_DIR=figures/interval_overlap_full_1000000 \
QUERY_ROOT=queries/interval_overlap_full_1000000 \
./scripts/run_interval_overlap_diagnostics.sh
```

**参数含义**
```text
PREPARE_DATA=1
如果 synthetic 数据不存在，自动生成 UNIF/DIAG 合成矩形数据。

AUTO_GENERATE_QUERIES=1
如果 query CSV 不存在，自动生成 0.001%、0.01%、0.1%、1% 四种 query。

SYNTHETIC_KIND=rectangles
生成矩形合成数据，更适合 Intersects 实验。

BLOCK_SIZES
测试 block size 对 IntervalOverlapIndex 的影响。

SELECTIVITY_TAGS
测试不同查询窗口大小。

INCLUDE_QUADTREE=1
把 GEOS_Quadtree 加进对比。
```

跑完看这里：

```text
results/interval_overlap_full_1000000/interval_overlap_summary.csv
figures/interval_overlap_full_1000000/
```

建议你先跑 `LIMIT=1000000 + BLOCK_SIZES="512 1024 2048"` 这版。确认趋势后，再决定要不要上 `2M + 5 个 block size` 的超全量。
