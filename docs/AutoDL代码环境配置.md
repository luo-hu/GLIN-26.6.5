我租用了一台 AutoDL 服务器，标准目录分区如下

1. 系统盘 /root（关机镜像保存，容量 30G）
   plaintext
   /root
   ├── autodl-pub      # 公共示例代码
   ├── miniconda3      # conda基础环境（占系统盘，建议迁移到autodl-tmp）
   ├── autodl-tmp -> 数据盘挂载点
   ├── rcad s.rar
   └── tf-logs
2. 数据盘 /root/autodl-tmp（关机数据不丢失，不随镜像保存，50G）
   plaintext
   /root/autodl-tmp
   ├── Code            # 你的GLIN项目代码存放目录
   │   └── GLIN-26.6.5 # 主工程
   ├── Datasets        # 数据集压缩包（zip/rar）
   ├── Software        # 第三方源码压缩包（boost、geos等依赖）
   └── glin_env26.6.29 # 项目虚拟环境（推荐放在数据盘，节省系统盘）

(base) root@autodl-container-bbf34ea8fa-3da9223e:~/autodl-tmp# tree -L 2
.
├── Code
│   └── GLIN-26.6.5
├── Datasets
│   ├── AREAWATER.zip
│   ├── LINEARWATER.rar
│   ├── parks.rar
│   └── roads.rar
├── Software
│   ├── boost_1_67_0.tar.gz
│   ├── boost_1_79_0.tar.gz
│   └── geos-3.8.4.tar.bz2
└── glin_env26.6.29
    ├── bin
    ├── compiler_compat
    ├── conda-meta
    ├── include
    ├── lib
    ├── man
    ├── share
    ├── ssl
    └── x86_64-conda-linux-gnu
这是服务器的分区目录，我需要你指导如何在这个服务器上复现GLIN-26.6.5这个项目


可以。你这台 AutoDL 的目录规划是合理的： **系统盘 `/root` 尽量少放东西，项目、环境、数据、第三方依赖都放 `/root/autodl-tmp`** 。下面给你一套比较稳的复现流程。

**目标目录**

```
/root/autodl-tmp
├── Code/GLIN-26.6.5
├── Datasets
├── Software
└── glin_env26.6.29
```

**1. 进入数据盘**

```
cd /root/autodl-tmp
```

确认项目存在：

```
ls Code/GLIN-26.6.5
```

**2. 激活你的 conda 环境**
你已经把环境放在数据盘了，推荐直接激活：

```
source /root/miniconda3/etc/profile.d/conda.sh
conda activate /root/autodl-tmp/glin_env26.6.29
```

确认：

```
which python
which g++
python --version
g++ --version
```

如果环境激活成功，`which python` 应该指向：

```
/root/autodl-tmp/glin_env26.6.29/bin/python
```

**3. 解压数据集**
进入数据集目录：

```
cd /root/autodl-tmp/Datasets
```

安装解压工具：

```
apt update
apt install -y unzip unrar
```

解压：

```
unzip AREAWATER.zip
unrar x LINEARWATER.rar
unrar x parks.rar
unrar x roads.rar
```

然后确认文件名：

```
ls -lh /root/autodl-tmp/Datasets
```

你后面 runner 里可以这样指定：

```
DATA_ROOT=/root/autodl-tmp/Datasets
```

如果解压后文件名不是 `AREAWATER.csv`、`LINEARWATER.csv`、`parks`、`roads`，需要告诉我实际文件名，我帮你改 runner 的数据路径。

**4. 安装/确认依赖**
先看环境里有没有 GEOS 和 Boost：

```
conda list | grep -E "geos|boost|cmake"
```

如果缺少，可以优先用 conda 安装：

```
conda install -y -c conda-forge cmake make gxx_linux-64 boost-cpp geos
```

确认：

```
which cmake
geos-config --version
```

如果 `geos-config` 能输出版本，比如 `3.8.4` 或更高，一般就可以。

**5. 编译项目**
进入项目：

```
cd /root/autodl-tmp/Code/GLIN-26.6.5
```

新建 build 目录：

```
mkdir -p build_current
cd build_current
```

配置：

```
cmake ..
```

如果 CMake 找不到 GEOS，可以显式指定：

```
cmake .. \
  -DGEOS_INCLUDE_DIR=/root/autodl-tmp/glin_env26.6.29/include \
  -DGEOS_LIBRARY=/root/autodl-tmp/glin_env26.6.29/lib/libgeos.so
```

如果 CMake 配置阶段能看到类似：

```
-- GEOS_INCLUDE_DIR=/root/autodl-tmp/glin_env26.6.29/include
-- GEOS_LIBRARY=/root/autodl-tmp/glin_env26.6.29/lib/libgeos.so;/root/autodl-tmp/glin_env26.6.29/lib/libgeos_c.so
```

但编译时报：

```
fatal error: geos/geom/Envelope.h: No such file or directory
```

这通常不是 GEOS 没安装，而是 CMake target 的 include directories 没有正确传给编译命令。先检查头文件是否真实存在：

```
ls /root/autodl-tmp/glin_env26.6.29/include/geos/geom/Envelope.h
```

如果文件存在，清理旧的 build 缓存并重新配置：

```
cd /root/autodl-tmp/Code/GLIN-26.6.5
rm -rf build_current
cmake -S . -B build_current \
  -DGEOS_INCLUDE_DIR=/root/autodl-tmp/glin_env26.6.29/include \
  -DGEOS_LIBRARY=/root/autodl-tmp/glin_env26.6.29/lib/libgeos.so \
  -DGEOS_C_LIB=/root/autodl-tmp/glin_env26.6.29/lib/libgeos_c.so
cmake --build build_current --target bench_dynamic_compare_wkt -j2
```

如果 `Envelope.h` 文件不存在，说明当前 conda 的 GEOS 包没有提供 C++ 头文件，建议改装 conda-forge 的 GEOS/Boost：

```
conda install -y -c conda-forge geos boost-cpp cmake make
```

然后重新执行上面的 `rm -rf build_current` 和 `cmake -S . -B build_current ...`。

编译动态对比 benchmark：

```
cmake --build . --target bench_dynamic_compare_wkt -j2
```

成功后应该有：

```
ls -lh bench_dynamic_compare_wkt
```

**6. 先跑一个 smoke test**
不要一上来跑 200 万数据，先跑 1 万确认流程通。

```
cd /root/autodl-tmp/Code/GLIN-26.6.5
```

如果 query 文件还没有，可以让脚本自动生成：

```
RESET_RESULTS=1 OVERWRITE=1 AUTO_BUILD=0 \
DATA_ROOT=/root/autodl-tmp/Datasets \
DATASETS="AW" \
LIMIT=10000 QUERY_LIMIT=10000 \
QUERY_ROOT=queries/smoke_10000 \
RESULT_DIR=results/smoke_autodl \
FIGURE_DIR=figures/smoke_autodl \
SELECTIVITY_TAGS="0p1pct" \
QUERY_COUNT=20 \
AUTO_GENERATE_QUERIES=1 \
CHECK_CORRECTNESS=1 \
INDEXES="DELI_ALEX_HYBRID_LOCAL_BOUNDED DELI_ALEX_HYBRID_COST Boost_Rtree" \
BUILD_DIR=build_current \
./scripts/run_dynamic_compare_diagnostics.sh
```

如果成功，重点看输出里有没有：

```
answers_match_boost=1
```

以及：

```
correctness: 所有行 answers_match_boost=1
```

**7. 跑正式小规模 mixed workload**
确认 smoke 没问题后，可以跑 50 万数据：

```
RESET_RESULTS=1 OVERWRITE=1 AUTO_BUILD=0 \
DATA_ROOT=/root/autodl-tmp/Datasets \
INDEXES="DELI_ALEX_HYBRID_LOCAL_BOUNDED DELI_ALEX_HYBRID_COST Boost_Rtree GEOS_Quadtree GLIN_PIECEWISE" \
CHECK_CORRECTNESS=0 \
PREDICATE_SHORTCUTS_LIST="0 1" \
WORKLOAD_MODE=mixed \
MIXED_PROFILES="write_heavy" \
MIXED_OPERATIONS=500000 \
MIXED_CHECKPOINT_INTERVAL=5000 \
DATASETS="AW" \
LIMIT=500000 QUERY_LIMIT=500000 \
QUERY_ROOT=queries/interval_overlap_full_500000 \
RESULT_DIR=results/dynamic_compare_autodl_0.5m \
FIGURE_DIR=figures/dynamic_compare_autodl_0.5m \
SELECTIVITY_TAGS="0p01pct" \
QUERY_COUNT=500 \
AUTO_GENERATE_QUERIES=1 \
LOCAL_DELTA_BOUND=128 \
DELETE_COMPACT_FRACTION=0.25 \
LAZY_ALEX_DELETE=1 \
DEFER_DELETE_SUMMARY_REFRESH=1 \
BUILD_DIR=build_current \
./scripts/run_dynamic_compare_diagnostics.sh
```

**8. 推荐的正式实验顺序**
建议你按这个顺序来，不要直接开最大规模：

```
第一步：AW, LIMIT=10000, CHECK_CORRECTNESS=1
第二步：AW, LIMIT=500000, CHECK_CORRECTNESS=0
第三步：AW + PARKS, LIMIT=500000
第四步：AW + PARKS, LIMIT=2000000
第五步：AW + LW + PARKS + ROADS, LIMIT=2000000
```

`CHECK_CORRECTNESS=1` 很慢，正式大规模 mixed workload 可以关掉，但每个新版本代码至少要跑一次小规模 correctness。

**9. AutoDL 上的注意事项**
不要把大结果放 `/root`，全部放：

```
/root/autodl-tmp/Code/GLIN-26.6.5/results
/root/autodl-tmp/Code/GLIN-26.6.5/figures
/root/autodl-tmp/Code/GLIN-26.6.5/queries
```

如果系统盘快满，检查：

```
df -h
du -sh /root/*
```

如果 conda 缓存太大：

```
conda clean -a -y
```

下一步你可以先在服务器上跑到第 5 步。如果 CMake 或数据路径报错，把完整报错和下面两条命令输出发我：

```
pwd
ls -lh /root/autodl-tmp/Datasets
```
