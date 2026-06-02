# JTS STRtree KNN Query Generator

这个小工具用于更贴近 GLIN 论文的 query workload 生成方式：

1. 读取 WKT 文本数据。
2. 用 JTS `STRtree` 建空间索引。
3. 随机选择一个 geometry。
4. 用 JTS `STRtree.nearestNeighbour(..., k)` 找 KNN。
5. 合并 K 个近邻 geometry 的 MBR，输出 query window CSV。

输出 CSV 的前 6 列与现有 C++ benchmark 兼容：

```text
query_id,xmin,ymin,xmax,ymax,source_geometry_id,selectivity,k,mode
```

## 环境安装

如果系统没有 Java/Maven，推荐直接在终端执行：

```bash
sudo apt update
sudo apt install -y openjdk-17-jdk maven
```

检查：

```bash
java -version
mvn -version
```

## 编译

在仓库根目录执行：

```bash
cd java/jts-query-generator
mvn -q -DskipTests package
```

第一次 Maven 会下载 `org.locationtech.jts:jts-core:1.20.0`。

## 推荐入口：从仓库根目录生成 Query

我在仓库根目录加了一个包装脚本，装好 Java/Maven 后可以直接用：

```bash
scripts/generate_jts_strtree_knn_queries.sh \
  /mnt/hgfs/parks \
  queries/parks_1m_jts_strtree_knn \
  1000000 \
  100 \
  42
```

参数含义：

```text
DATA_FILE      WKT 数据文件
OUTPUT_PREFIX 生成 query CSV 的路径前缀
LIMIT          读入多少条有效 geometry
QUERY_COUNT    每个 selectivity 生成多少个 query window，论文是 100
SEED           随机种子
```

## 生成 PARKS 1M Query

```bash
cd java/jts-query-generator
mvn -q exec:java -Dexec.args="\
  --data_file /mnt/hgfs/parks \
  --limit 1000000 \
  --query_count 100 \
  --selectivities 1%,0.1%,0.01%,0.001% \
  --output_prefix ../../queries/parks_1m_jts_strtree_knn \
  --seed 42"
```
第二种方式是用脚本：
scripts/generate_jts_strtree_knn_queries.sh \
  /mnt/hgfs/parks \
  queries/parks_1m_jts_strtree_knn \
  1000000 \
  100 \
  42

生成文件：

```text
queries/parks_1m_jts_strtree_knn_1pct.csv
queries/parks_1m_jts_strtree_knn_0p1pct.csv
queries/parks_1m_jts_strtree_knn_0p01pct.csv
queries/parks_1m_jts_strtree_knn_0p001pct.csv
```

## 生成 AREAWATER 1M Query

```bash
cd java/jts-query-generator
mvn -q exec:java -Dexec.args="\
  --data_file /mnt/hgfs/AREAWATER.csv \
  --limit 1000000 \
  --query_count 100 \
  --selectivities 1%,0.1%,0.01%,0.001% \
  --output_prefix ../../queries/areawater_1m_jts_strtree_knn \
  --seed 42"
```
或者用第二种脚本的方式：
scripts/generate_jts_strtree_knn_queries.sh \
  /mnt/hgfs/AREAWATER.csv \
  queries/aw_1m_jts_strtree_knn \
  1000000 \
  100 \
  42

## 用生成的 Query 跑 C++ Benchmark

回到仓库根目录：

```bash
cd ../..
```

Boost R-tree contains：

```bash
for sel in 0p001pct 0p01pct 0p1pct 1pct; do
  ./build/bench_boost_rtree_wkt \
    --data_file /mnt/hgfs/parks \
    --dataset_name PARKS \
    --limit 1000000 \
    --query_file queries/parks_1m_jts_strtree_knn_${sel}.csv \
    --relationship contains \
    --output_csv results/parks_1m_jts_strtree_knn_${sel}_boost_rtree_contains.csv
done
```

for sel in 0p001pct 0p01pct 0p1pct 1pct; do
  ./build/bench_boost_rtree_wkt \
    --data_file /mnt/hgfs/AREAWATER.csv \
    --dataset_name AREAWATER.csv \
    --limit 1000000 \
    --query_file queries/aw_1m_jts_strtree_knn_${sel}.csv \
    --relationship contains \
    --output_csv results/aw_1m_jts_strtree_knn_${sel}_boost_rtree_contains.csv
done


GLIN contains：

```bash
for sel in 0p001pct 0p01pct 0p1pct 1pct; do
  ./build/bench_glin_wkt \
    --data_file /mnt/hgfs/parks \
    --dataset_name PARKS \
    --limit 1000000 \
    --query_file queries/parks_1m_jts_strtree_knn_${sel}.csv \
    --output_csv results/parks_1m_jts_strtree_knn_${sel}_glin_contains.csv
done
```
for sel in 0p001pct 0p01pct 0p1pct 1pct; do
  ./build/bench_glin_wkt \
    --data_file /mnt/hgfs/AREAWATER.csv \
    --dataset_name AREAWATER.csv \
    --limit 1000000 \
    --query_file queries/aw_1m_jts_strtree_knn_${sel}.csv \
    --output_csv results/aw_1m_jts_strtree_knn_${sel}_glin_contains.csv
done

GLIN-piecewise intersects：

```bash
for sel in 0p001pct 0p01pct 0p1pct 1pct; do
  ./build/bench_glin_wkt_piece \
    --data_file /mnt/hgfs/parks \
    --dataset_name PARKS \
    --limit 1000000 \
    --query_file queries/parks_1m_jts_strtree_knn_${sel}.csv \
    --output_csv results/parks_1m_jts_strtree_knn_${sel}_glin_piece_intersects.csv
done
```
for sel in 0p001pct 0p01pct 0p1pct 1pct; do
  ./build/bench_glin_wkt_piece \
    --data_file /mnt/hgfs/AREAWATER.csv \
    --dataset_name AREAWATER.csv \
    --limit 1000000 \
    --query_file queries/aw_1m_jts_strtree_knn_${sel}.csv \
    --output_csv results/aw_1m_jts_strtree_knn_${sel}_glin_piece_intersects.csv
done

Boost R-tree intersects：

```bash
for sel in 0p001pct 0p01pct 0p1pct 1pct; do
  ./build/bench_boost_rtree_wkt \
    --data_file /mnt/hgfs/parks \
    --dataset_name PARKS \
    --limit 1000000 \
    --query_file queries/parks_1m_jts_strtree_knn_${sel}.csv \
    --relationship intersects \
    --output_csv results/parks_1m_jts_strtree_knn_${sel}_boost_rtree_intersects.csv
done
```
for sel in 0p001pct 0p01pct 0p1pct 1pct; do
  ./build/bench_boost_rtree_wkt \
    --data_file /mnt/hgfs/AREAWATER.csv  \
    --dataset_name AREAWATER.csv  \
    --limit 1000000 \
    --query_file queries/aw_1m_jts_strtree_knn_${sel}.csv \
    --relationship intersects \
    --output_csv results/aw_1m_jts_strtree_knn_${sel}_boost_rtree_intersects.csv
done

汇总：

```bash
python3 scripts/summarize_aw1m_knn.py \
  --glob 'results/parks_1m_jts_strtree_knn_*.csv' \
  --output results/parks_1m_jts_strtree_knn_summary.csv
```

画图：

```bash
python3 scripts/plot_aw1m_knn_summary.py \
  --input results/parks_1m_jts_strtree_knn_summary.csv \
  --output_dir figures \
  --figure_prefix parks_1m_jts_strtree_knn
```

哦哦，这个summarize_aw1m_knn.py和plot_aw1m_knn_summary.py是可以重复使用的，只要修改输入输出就行了，不用每一次都写新的脚本
