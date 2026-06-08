# All 1M Dataset Runner 使用说明

本文档说明 `scripts/run_all_1m.sh` 这个一键脚本的作用、运行方式和常用参数。

`All 1M Dataset Runner` 的意思是：把多个数据集都按 `1M` 规模跑一遍的统一实验入口。这里的 `1M` 表示一百万条有效几何数据。

## 1. 这个脚本做什么

`scripts/run_all_1m.sh` 是当前 GLIN 复现实验的一键运行脚本。它会自动完成数据准备、查询生成、索引 benchmark、结果汇总和画图。

默认覆盖这些数据集：

```text
AW               AREAWATER，面状水域数据
LW               LINEARWATER，线状水域数据
ROADS            道路数据
PARKS            公园/区域类数据
OSM_AU_POINTS    OSM Australia point，澳大利亚 OSM 点数据
UNIF_S           Uniform Small，均匀分布小规模合成数据
UNIF_L           Uniform Large，均匀分布大规模合成数据
DIAG_S           Diagonal Small，对角线分布小规模合成数据
DIAG_L           Diagonal Large，对角线分布大规模合成数据
```

脚本执行流程：

```text
1. 编译需要用到的 C++ 工具。
2. 如果需要，把 OSM Australia binary point 转换成 WKT。
3. 如果需要，生成 UNIF/DIAG 合成 geo point WKT。
4. 生成 JTS STRtree KNN 查询窗口。
5. 运行 GLIN Contains 查询实验。
6. 运行 GLIN-piecewise Intersects 查询实验。
7. 运行 Boost R-tree Contains/Intersects 查询实验。
8. 运行 GEOS Quadtree Contains/Intersects 查询实验。
9. 汇总原始 CSV 结果。
10. 绘制实验图。
```

术语解释：

- `WKT`: Well-Known Text，几何对象的文本格式，例如 `POINT`、`POLYGON`。
- `Contains`: 空间关系，表示查询窗口是否包含目标几何对象。
- `Intersects`: 空间关系，表示查询窗口是否和目标几何对象相交。
- `JTS STRtree`: Java Topology Suite 里的 STRtree 空间索引，这里用来生成较稳定的 KNN 查询窗口。
- `KNN`: k-nearest neighbors，k 近邻。这里不是直接做 KNN 查询，而是用 KNN 思路生成查询窗口。

## 2. 完整运行

直接运行：

```bash
scripts/run_all_1m.sh
```

默认输出路径：

```text
queries/all_1m/              生成的查询文件
results/all_1m/              每个实验的原始 CSV 结果
results/all_1m_summary.csv   汇总后的总表
figures/all_1m/              绘制出来的图
```

## 3. Smoke Test 小规模测试

`Smoke Test` 的意思是“小烟测”，也就是先跑一个很小规模的实验，确认脚本、数据路径、编译和画图流程都没问题。

推荐命令：

```bash
DATASETS="UNIF_S" LIMIT=1000 QUERY_COUNT=5 \
RESULT_DIR=results/all_1m_smoke \
QUERY_DIR=queries/all_1m_smoke \
FIGURE_DIR=figures/all_1m_smoke \
SUMMARY_CSV=results/all_1m_smoke_summary.csv \
OVERWRITE=1 \
scripts/run_all_1m.sh
```

这个命令只跑 `UNIF_S`，只加载 1000 条数据，只生成 5 个查询窗口，适合检查流程。

## 4. Dry Run 只打印命令

`Dry Run` 的意思是“空跑”。它不会真正执行实验，只会打印将要运行的命令。

```bash
DRY_RUN=1 DATASETS="AW" LIMIT=1000 QUERY_COUNT=3 \
scripts/run_all_1m.sh
```

用途：

- 检查脚本会不会跑错数据集。
- 检查输出路径是否正确。
- 检查参数拼接是否符合预期。

## 5. 常用参数

```text
LIMIT=1000000
每个数据集加载多少条有效记录。默认 1000000，也就是 1M。

QUERY_COUNT=100
每种 selectivity 生成多少个查询窗口。

SEED=42
随机种子。固定 seed 可以保证实验可复现。

DATASETS="AW PARKS"
只跑指定数据集。多个数据集之间用空格分隔。

OVERWRITE=1
重新生成已有输出。不开这个参数时，脚本可能复用已有文件。

AUTO_BUILD=0
跳过 cmake 编译步骤。适合代码没有变化、只想重跑实验时使用。

RUN_CONTAINS=0
跳过 Contains 查询实验。

RUN_INTERSECTS=0
跳过 Intersects 查询实验。

GENERATE_QUERIES=0
不重新生成 query 文件，复用已有查询文件。

RUN_BENCHMARKS=0
不跑 benchmark，只准备数据、query、summary 或 plot。

SUMMARIZE=0
跳过结果汇总。

PLOT=0
跳过画图。
```

## 6. 一个重要注意点

对于点数据集，低 selectivity 的 KNN 查询窗口可能非常小，甚至接近退化窗口。

这会导致：

```text
Contains 查询可能返回 0 个答案。
Intersects 查询通常更适合点数据窗口测试。
```

原因是：

- `Contains` 要求查询 polygon 真正包含点。
- 如果查询窗口很小，边界判断容易让答案变成 0。
- `Intersects` 只要求相交，点落在窗口边界时也更自然。

所以在点数据实验中，如果 `Contains` 结果异常少，不要马上认为索引错了，要先看查询窗口是否太小，以及是否应该使用 `Intersects`。

