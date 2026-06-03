# Synthetic Point Data for GLIN

`/home/lh/Code/learnedbench` 的 synthetic 数据是无 header 的二进制
`double` 流：每个点连续写入 `dim` 个 double。当前 GLIN WKT benchmark
不能直接读取这种 binary 文件，需要先转换成 WKT，或者直接生成 WKT。

## Build

```bash
cmake -S . -B build
cmake --build build --target generate_synthetic_points convert_binary_points_to_wkt
```

## Generate GLIN-Readable WKT

```bash
N=1000000 FORMAT=wkt OUT_DIR=data/synthetic/Default \
  scripts/prepare_synthetic_points.sh
```

This writes:

```text
data/synthetic/Default/uniform_1000000_2_1.wkt
data/synthetic/Default/gaussian_1000000_2_1.wkt
data/synthetic/Default/lognormal_1000000_2_1.wkt
```

The distributions match learnedbench's point generators:

```text
uniform:   each dimension samples U(0, scale)
gaussian:  each dimension samples N(0, scale^2)
lognormal: each dimension samples lognormal(mean=0, sigma=scale)
```

To generate learnedbench-style binary and WKT together:

```bash
N=1000000 FORMAT=both OUT_DIR=data/synthetic/Default \
  scripts/prepare_synthetic_points.sh
```

For 2D data, the binary and WKT outputs use the same seed and point order.

## Generate GLIN-Style Geo Synthetic Points

For the four synthetic point datasets named like the GLIN paper:

```text
UNIF_S
UNIF_L
DIAG_S
DIAG_L
```

run:

```bash
SMALL_N=1000000 LARGE_N=1000000 OUT_DIR=data/synthetic/glin_geo \
  scripts/prepare_glin_synthetic_geo_points.sh
```

This maps unit-square points to the longitude/latitude range:

```text
x: [-180, 180]
y: [-90, 90]
```

For paper-like synthetic scale:

```bash
SMALL_N=10000000 LARGE_N=40000000 OUT_DIR=data/synthetic/glin_geo \
  scripts/prepare_glin_synthetic_geo_points.sh
```

`UNIF_*` uses independent uniform points. `DIAG_*` samples points near the
diagonal in unit space and then maps them to longitude/latitude.

## Convert Existing learnedbench Binary

```bash
./build/convert_binary_points_to_wkt \
  --input_file /home/lh/Code/learnedbench/data/synthetic/Default/uniform_20m_2_1 \
  --output_file data/synthetic/Default/uniform_20m_2_1.wkt \
  --num 20000000 \
  --dim 2
```

## Generate Query Windows

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

## Run Benchmarks

```bash
./build/bench_glin_wkt \
  --data_file data/synthetic/Default/uniform_1000000_2_1.wkt \
  --dataset_name SYN_UNIFORM \
  --limit 1000000 \
  --query_file queries/synthetic_uniform_1m_geom_knn_1pct.csv \
  --output_csv results/synthetic_uniform_1m_1pct_glin_contains.csv
```

Use `bench_boost_rtree_wkt` and `bench_geos_quadtree_wkt` with the same
`--query_file` for comparable baselines.

## OSM Binary Note

`/mnt/hgfs/osm_australia_2m_point.bin` is shaped like a 2M 2D raw point file.
Its size is 32,000,000 bytes:

```text
32,000,000 bytes = 4,000,000 doubles = 2,000,000 points * 2 dimensions
```

Convert the first 1M points for comparable 1M experiments:

```bash
./build/convert_binary_points_to_wkt \
  --input_file /mnt/hgfs/osm_australia_2m_point.bin \
  --output_file data/real/osm_australia_1m_point.wkt \
  --num 1000000 \
  --dim 2
```

## Synthetic Rectangles for Intersects

When the paper's `DIAG_S`, `DIAG_L`, `UNIF_S`, and `UNIF_L` datasets are not
available, use the local rectangle generator:

```bash
cmake --build build --target generate_synthetic_rectangles

SMALL_N=1000000 LARGE_N=4000000 OUT_DIR=data/synthetic/rectangles \
  scripts/prepare_synthetic_rectangles.sh
```

This writes:

```text
data/synthetic/rectangles/UNIF_S.wkt
data/synthetic/rectangles/UNIF_L.wkt
data/synthetic/rectangles/DIAG_S.wkt
data/synthetic/rectangles/DIAG_L.wkt
```

To mimic the paper's scale more closely:

```bash
SMALL_N=10000000 LARGE_N=40000000 OUT_DIR=data/synthetic/rectangles \
  scripts/prepare_synthetic_rectangles.sh
```

The generator writes axis-aligned `POLYGON` rectangles in the default longitude
and latitude range `[-180, 180] x [-90, 90]`. You can override the range and
rectangle sizes:

```bash
MIN_WIDTH=0.0001 MAX_WIDTH=0.001 \
MIN_HEIGHT=0.0001 MAX_HEIGHT=0.001 \
XMIN=-180 YMIN=-90 XMAX=180 YMAX=90 \
scripts/prepare_synthetic_rectangles.sh
```

For Intersects tests, generate fixed query windows and run all indexes with the
same query files:

```bash
./build/generate_wkt_queries \
  --data_file data/synthetic/rectangles/DIAG_S.wkt \
  --limit 1000000 \
  --query_count 100 \
  --mode geom_knn \
  --selectivities 1%,0.1%,0.01%,0.001% \
  --output_prefix queries/synthetic_diag_s_geom_knn \
  --seed 42

./build/bench_glin_wkt_piece \
  --data_file data/synthetic/rectangles/DIAG_S.wkt \
  --dataset_name DIAG_S \
  --limit 1000000 \
  --query_file queries/synthetic_diag_s_geom_knn_1pct.csv \
  --output_csv results/synthetic_diag_s_1pct_glin_piece_intersects.csv
```

Use `bench_boost_rtree_wkt --relationship intersects` and
`bench_geos_quadtree_wkt --relationship intersects` with the same query file.

## Note

These learnedbench-style synthetic datasets are point datasets. They are useful
for testing GLIN's pipeline and point query behavior, but they are not the same
as the GLIN paper's polygon-heavy synthetic datasets used for Intersects
experiments. The rectangle generator above is closer to the paper's Intersects
synthetic setup, although it is still a local approximation unless the authors'
exact generator and seeds are available.
