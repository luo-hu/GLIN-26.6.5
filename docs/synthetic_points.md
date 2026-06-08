# GLIN 合成点数据说明

本文档说明如何为 GLIN WKT benchmark 生成合成点数据、转换 learnedbench 的 binary 数据，以及如何生成 query 和运行 benchmark。

固定英文术语说明：

- `synthetic`: 合成数据，不是真实地图数据。
- `point`: 点数据。
- `binary`: 二进制文件。
- `WKT`: Well-Known Text，几何对象文本格式，例如 `POINT (x y)`。
- `benchmark`: 性能测试。
- `query window`: 查询窗口，通常是一个矩形范围。

## 1. learnedbench 数据格式

`/home/lh/Code/learnedbench` 里的 synthetic 数据是无 header 的二进制 `double` 流。

含义是：

```text
每个点连续写入 dim 个 double。
如果 dim=2，那么每个点就是两个 double: x 和 y。
```

当前 GLIN WKT benchmark 不能直接读取这种 binary 文件。因此有两种办法：

```text
1. 先把 binary 文件转换成 WKT。
2. 直接生成 WKT 文件。
```

## 2. 编译相关工具

```bash
cmake -S . -B build
cmake --build build --target generate_synthetic_points convert_binary_points_to_wkt
```

工具含义：

- `generate_synthetic_points`: 生成合成点数据。
- `convert_binary_points_to_wkt`: 把 binary 点数据转换成 WKT 点数据。

## 3. 生成 GLIN 可读取的 WKT

生成 100 万条 WKT 点数据：

```bash
N=1000000 FORMAT=wkt OUT_DIR=data/synthetic/Default \
  scripts/prepare_synthetic_points.sh
```

输出文件：

```text
data/synthetic/Default/uniform_1000000_2_1.wkt
data/synthetic/Default/gaussian_1000000_2_1.wkt
data/synthetic/Default/lognormal_1000000_2_1.wkt
```

三个分布的含义：

```text
uniform:
每个维度都从 U(0, scale) 均匀分布采样。

gaussian:
每个维度都从 N(0, scale^2) 高斯分布采样。

lognormal:
每个维度都从 lognormal(mean=0, sigma=scale) 对数正态分布采样。
```

如果想同时生成 learnedbench 风格的 binary 和 GLIN 可读的 WKT：

```bash
N=1000000 FORMAT=both OUT_DIR=data/synthetic/Default \
  scripts/prepare_synthetic_points.sh
```

对于 2D 数据，binary 和 WKT 使用相同的 seed 和点顺序，因此可以互相对照。

## 4. 生成 GLIN 论文风格的 Geo Synthetic Points

GLIN 论文里常见的四个合成数据名：

```text
UNIF_S
UNIF_L
DIAG_S
DIAG_L
```

含义：

- `UNIF`: Uniform，均匀分布。
- `DIAG`: Diagonal，对角线分布。
- `S`: Small，小规模。
- `L`: Large，大规模。

生成命令：

```bash
SMALL_N=1000000 LARGE_N=1000000 OUT_DIR=data/synthetic/glin_geo \
  scripts/prepare_glin_synthetic_geo_points.sh
```

这个脚本会把单位正方形里的点映射到经纬度范围：

```text
x: [-180, 180]
y: [-90, 90]
```

如果要更接近论文规模：

```bash
SMALL_N=10000000 LARGE_N=40000000 OUT_DIR=data/synthetic/glin_geo \
  scripts/prepare_glin_synthetic_geo_points.sh
```

分布说明：

- `UNIF_*`: x/y 独立均匀采样。
- `DIAG_*`: 点集中在单位空间的对角线附近，然后映射到经纬度范围。

## 5. 转换已有 learnedbench binary 文件

示例：把 learnedbench 的 2000 万 uniform binary 文件转换成 WKT。

```bash
./build/convert_binary_points_to_wkt \
  --input_file /home/lh/Code/learnedbench/data/synthetic/Default/uniform_20m_2_1 \
  --output_file data/synthetic/Default/uniform_20m_2_1.wkt \
  --num 20000000 \
  --dim 2
```

参数解释：

- `--input_file`: 输入 binary 文件路径。
- `--output_file`: 输出 WKT 文件路径。
- `--num`: 转换多少个点。
- `--dim`: 点的维度。二维点就是 `2`。

## 6. 生成 Query Windows

生成查询窗口：

```bash
./build/generate_wkt_queries \
  --data_file data/synthetic/Default/uniform_1000000_2_1.wkt \
  --limit 1000000 \
  --query_count 100 \
  --mode geom_knn \
  --selectivities 1%,0.1%,0.01%,0.001% \
  --output_prefix queries/synthetic_uniform_1m_geom_knn \
  --seed 42
```

参数解释：

- `--data_file`: 用哪个数据文件生成 query。
- `--limit`: 读取多少条数据参与 query 生成。
- `--query_count`: 每种 selectivity 生成多少个查询。
- `--mode geom_knn`: 用几何对象的 KNN 方式生成查询窗口。
- `--selectivities`: 查询窗口选择率，数值越小，查询窗口越小。
- `--output_prefix`: query 输出文件名前缀。
- `--seed`: 随机种子。

## 7. 运行 Benchmark

运行 GLIN：

```bash
./build/bench_glin_wkt \
  --data_file data/synthetic/Default/uniform_1000000_2_1.wkt \
  --dataset_name SYN_UNIFORM \
  --limit 1000000 \
  --query_file queries/synthetic_uniform_1m_geom_knn_1pct.csv \
  --output_csv results/synthetic_uniform_1m_1pct_glin_contains.csv
```

如果要做公平对比，Boost R-tree 和 GEOS Quadtree 必须使用同一个 `--query_file`：

```text
bench_boost_rtree_wkt
bench_geos_quadtree_wkt
```

## 8. OSM Binary 数据说明

`/mnt/hgfs/osm_australia_2m_point.bin` 看起来是一个 2M 规模的二维 raw point 文件。

文件大小：

```text
32,000,000 bytes = 4,000,000 doubles = 2,000,000 points * 2 dimensions
```

转换前 100 万个点：

```bash
./build/convert_binary_points_to_wkt \
  --input_file /mnt/hgfs/osm_australia_2m_point.bin \
  --output_file data/real/osm_australia_1m_point.wkt \
  --num 1000000 \
  --dim 2
```

## 9. 用于 Intersects 的合成矩形数据

GLIN 论文里的 `DIAG_S`、`DIAG_L`、`UNIF_S`、`UNIF_L` 很多时候是 polygon/rectangle 类型，而不是简单点数据。

如果没有论文作者的原始生成器，可以用本地矩形生成器近似：

```bash
cmake --build build --target generate_synthetic_rectangles

SMALL_N=1000000 LARGE_N=4000000 OUT_DIR=data/synthetic/rectangles \
  scripts/prepare_synthetic_rectangles.sh
```

输出：

```text
data/synthetic/rectangles/UNIF_S.wkt
data/synthetic/rectangles/UNIF_L.wkt
data/synthetic/rectangles/DIAG_S.wkt
data/synthetic/rectangles/DIAG_L.wkt
```

更接近论文规模：

```bash
SMALL_N=10000000 LARGE_N=40000000 OUT_DIR=data/synthetic/rectangles \
  scripts/prepare_synthetic_rectangles.sh
```

矩形生成器默认生成经纬度范围内的轴对齐 `POLYGON` 矩形：

```text
[-180, 180] x [-90, 90]
```

可以调整矩形大小和空间范围：

```bash
MIN_WIDTH=0.0001 MAX_WIDTH=0.001 \
MIN_HEIGHT=0.0001 MAX_HEIGHT=0.001 \
XMIN=-180 YMIN=-90 XMAX=180 YMAX=90 \
scripts/prepare_synthetic_rectangles.sh
```

## 10. Intersects 测试示例

先生成固定 query 文件：

```bash
./build/generate_wkt_queries \
  --data_file data/synthetic/rectangles/DIAG_S.wkt \
  --limit 1000000 \
  --query_count 100 \
  --mode geom_knn \
  --selectivities 1%,0.1%,0.01%,0.001% \
  --output_prefix queries/synthetic_diag_s_geom_knn \
  --seed 42
```

运行 GLIN-piecewise：

```bash
./build/bench_glin_wkt_piece \
  --data_file data/synthetic/rectangles/DIAG_S.wkt \
  --dataset_name DIAG_S \
  --limit 1000000 \
  --query_file queries/synthetic_diag_s_geom_knn_1pct.csv \
  --output_csv results/synthetic_diag_s_1pct_glin_piece_intersects.csv
```

Boost 和 Quadtree 要使用同一个 query 文件：

```bash
./build/bench_boost_rtree_wkt \
  --relationship intersects \
  --query_file queries/synthetic_diag_s_geom_knn_1pct.csv

./build/bench_geos_quadtree_wkt \
  --relationship intersects \
  --query_file queries/synthetic_diag_s_geom_knn_1pct.csv
```

## 11. 注意事项

learnedbench 风格的 synthetic 数据主要是点数据。它适合测试 GLIN pipeline 和点查询行为，但它不等同于 GLIN 论文里用于 `Intersects` 实验的 polygon-heavy synthetic datasets。

本地 rectangle generator 更接近论文的 `Intersects` 合成设置，但只要没有作者的精确生成器和 seed，它仍然只能算本地近似复现。

