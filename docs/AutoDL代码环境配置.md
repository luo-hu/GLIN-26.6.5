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

这一步要先判断是“头文件不存在”，还是“头文件存在但 include path 没传进编译命令”。项目代码直接使用 GEOS C++ 的 `geos/geom/Envelope.h`，所以只看到 `GEOS_INCLUDE_DIR=.../include` 还不够，必须确认这个文件真实存在：

```
ls -lh /root/autodl-tmp/glin_env26.6.29/include/geos/geom/Envelope.h
ls -lh /root/autodl-tmp/glin_env26.6.29/include/geos/geom/Geometry.h
ls -lh /root/autodl-tmp/glin_env26.6.29/include/geos/geom/CoordinateArraySequence.h
```

如果报错是：

```
fatal error: geos/geom/CoordinateArraySequence.h: No such file or directory
```

但 `Geometry.h` 已经能被找到，例如编译日志里出现：

```
/root/autodl-tmp/glin_env26.6.29/include/geos/geom/Geometry.h
```

这说明 **不是 include path 没生效**，而是当前环境里的 GEOS C++ 头文件版本和本项目代码不匹配。GLIN-26.6.5 目前直接使用 GEOS C++ 旧接口：

```
geos/geom/CoordinateArraySequence.h
new geos::geom::CoordinateArraySequence()
```

部分较新的 conda-forge GEOS 包可能不再安装这个旧头文件，或者头文件组织方式已经变化。此时不要继续反复 `export C_INCLUDE_PATH` / `CPLUS_INCLUDE_PATH`，因为路径已经生效了，缺的是兼容的 GEOS 头文件。

推荐固定使用你已经下载的 GEOS 3.8.4 源码包安装到当前环境：

```
cd /root/autodl-tmp/Software
rm -rf geos-3.8.4
tar -xjf geos-3.8.4.tar.bz2
cd geos-3.8.4
mkdir -p build
cd build

cmake .. \
  -DCMAKE_INSTALL_PREFIX=/root/autodl-tmp/glin_env26.6.29 \
  -DCMAKE_BUILD_TYPE=Release

cmake --build . -j2
cmake --install .
```

安装后必须检查这三个文件都存在：

```
ls -lh /root/autodl-tmp/glin_env26.6.29/include/geos/geom/CoordinateArraySequence.h
ls -lh /root/autodl-tmp/glin_env26.6.29/lib/libgeos.so
ls -lh /root/autodl-tmp/glin_env26.6.29/lib/libgeos_c.so
```

然后必须清空 GLIN 的旧 CMake 缓存重新配置。

注意这里有两个容易踩坑的点：

1. 项目的 `cmake/FindGEOS.cmake` 识别的是 `GEOS_C_LIB`，不是 `GEOS_CXX_LIBRARY`。
   如果写成 `-DGEOS_CXX_LIBRARY=.../libgeos_c.so`，这个变量会被忽略。
2. 不要手动传一个不存在的 `-DGEOS_LIBRARY=/path/libgeos.so`。
   如果安装目录里只有 `libgeos-3.8.4.so`，没有 `libgeos.so` 软链接，Make 会报：

```
No rule to make target '/root/autodl-tmp/local_geos384/lib/libgeos.so'
```

先检查真实文件名：

```
ls -lh /root/autodl-tmp/local_geos384/lib/libgeos*
ls -lh /root/autodl-tmp/local_geos384/include/geos/geom/Geometry.h
ls -lh /root/autodl-tmp/local_geos384/include/geos/geom/Envelope.h
```

如果缺少 `libgeos.so` 或 `libgeos_c.so` 软链接，可以重装 GEOS，或手动补软链接：

```
cd /root/autodl-tmp/local_geos384/lib
ln -sf libgeos-3.8.4.so libgeos.so
ln -sf libgeos_c.so.1.13.1 libgeos_c.so   # 这里的真实版本号以 ls 输出为准
```

更推荐的配置方式是让 `FindGEOS.cmake` 自己从前缀目录找 `geos` 和 `geos_c`：

```
cd /root/autodl-tmp/Code/GLIN-26.6.5
rm -rf build_current

cmake -S . -B build_current \
  -DCMAKE_PREFIX_PATH="/root/autodl-tmp/local_geos384;/root/autodl-tmp/local_boost179" \
  -DGEOS_DIR=/root/autodl-tmp/local_geos384 \
  -DBOOST_ROOT=/root/autodl-tmp/local_boost179 \
  -DBoost_NO_SYSTEM_PATHS=ON

cmake --build build_current --target bench_dynamic_compare_wkt -j2
```

如果仍然要显式指定 GEOS 库，必须同时传 `GEOS_LIB` 和 `GEOS_C_LIB`，并且路径必须真实存在：

```
cmake -S . -B build_current \
  -DGEOS_INCLUDE_DIR=/root/autodl-tmp/local_geos384/include \
  -DGEOS_LIB=/root/autodl-tmp/local_geos384/lib/libgeos.so \
  -DGEOS_C_LIB=/root/autodl-tmp/local_geos384/lib/libgeos_c.so \
  -DBOOST_ROOT=/root/autodl-tmp/local_boost179 \
  -DBoost_NO_SYSTEM_PATHS=ON
```

如果报：

```
fatal error: geos/index/strtree/SimpleSTRtree.h: No such file or directory
```

要先确认本地和服务器实际参与编译的 GEOS 版本。本机当前能找到 `SimpleSTRtree.h` 的环境是 conda GEOS 3.9.1：

```
geos-config --version
grep -n "GEOS_VERSION" /path/to/include/geos/version.h
```

而服务器上的 GEOS 3.8.4 可能没有这个头。当前代码不实际使用 `SimpleSTRtree`，已经去掉该 include；如果服务器代码没有同步这个修改，请同步最新代码，或手动删除：

```
# glin/glin.h
#include <geos/index/strtree/SimpleSTRtree.h>
```

注意：这里使用 `GEOS_LIBRARY`，不要写成 `GEOS_LIB`。`C_INCLUDE_PATH` / `CPLUS_INCLUDE_PATH` 可以作为补救，但不要依赖它们覆盖 CMake 已经缓存下来的旧 GEOS 路径；最稳的是删除 `build_current` 后重新 `cmake -S . -B build_current ...`。

如果 `Envelope.h` 文件存在，清理旧的 build 缓存并重新配置：

```
cd /root/autodl-tmp/Code/GLIN-26.6.5
rm -rf build_current
cmake -S . -B build_current \
  -DGEOS_INCLUDE_DIR=/root/autodl-tmp/glin_env26.6.29/include \
  -DGEOS_LIBRARY=/root/autodl-tmp/glin_env26.6.29/lib/libgeos.so \
  -DGEOS_C_LIB=/root/autodl-tmp/glin_env26.6.29/lib/libgeos_c.so
cmake --build build_current --target bench_dynamic_compare_wkt -j2
```

如果 `Envelope.h` 文件不存在，但 `Geometry.h` 存在，说明当前环境里的 GEOS 头文件不满足本项目需要。此时不要继续反复 `cmake --build`，需要先修复 GEOS C++ 头文件。

优先尝试 conda-forge：

```
conda install -y -c conda-forge "geos=3.8.4" boost-cpp cmake make
```

如果 conda 解析失败，或者安装后仍然没有 `Envelope.h`，就使用你已经放在 `/root/autodl-tmp/Software` 的 GEOS 3.8.4 源码安装到当前环境：

```
cd /root/autodl-tmp/Software
tar -xjf geos-3.8.4.tar.bz2
cd geos-3.8.4
mkdir -p build
cd build
cmake .. \
  -DCMAKE_INSTALL_PREFIX=/root/autodl-tmp/glin_env26.6.29 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build . -j2
cmake --install .
```

安装后再次确认：

```
ls -lh /root/autodl-tmp/glin_env26.6.29/include/geos/geom/Envelope.h
ls -lh /root/autodl-tmp/glin_env26.6.29/lib/libgeos.so
ls -lh /root/autodl-tmp/glin_env26.6.29/lib/libgeos_c.so
```

然后重新执行上面的 `rm -rf build_current` 和 `cmake -S . -B build_current ...`。如果想让 CMake 更自然地优先从当前环境找依赖，也可以加上：

```
-DCMAKE_PREFIX_PATH=/root/autodl-tmp/glin_env26.6.29
```

编译动态对比 benchmark：

```
cmake --build . --target bench_dynamic_compare_wkt -j2
```

成功后应该有：

```
ls -lh bench_dynamic_compare_wkt
```


如果 conda 安装的GEOS不行，就用你已有的源码包安装 GEOS 3.8.4：

```
cd /root/autodl-tmp/Software
tar -xjf geos-3.8.4.tar.bz2
cd geos-3.8.4
mkdir -p build
cd build

cmake .. \
  -DCMAKE_INSTALL_PREFIX=/root/autodl-tmp/glin_env26.6.29 \
  -DCMAKE_BUILD_TYPE=Release

cmake --build . -j2
cmake --install .
```

然后重新编译 GLIN：

```
cd /root/autodl-tmp/Code/GLIN-26.6.5
rm -rf build_current

cmake -S . -B build_current \
  -DCMAKE_PREFIX_PATH=/root/autodl-tmp/glin_env26.6.29 \
  -DGEOS_INCLUDE_DIR=/root/autodl-tmp/glin_env26.6.29/include \
  -DGEOS_LIBRARY=/root/autodl-tmp/glin_env26.6.29/lib/libgeos.so \
  -DGEOS_C_LIB=/root/autodl-tmp/glin_env26.6.29/lib/libgeos_c.so

cmake --build build_current --target bench_dynamic_compare_wkt -j2
```

如果这里出现下面两个错误，不是 CMake 路径问题，而是源码兼容性/同步问题：

```
error: 'class geos::geom::Envelope' has no member named 'disjoint'
error: no matching function for call to 'alex::Glin<...>::glin_find(..., predicate_shortcuts, ...)'
```

处理方法：

1. `Envelope::disjoint` 是 GEOS C++ API 版本差异。GEOS 3.8.4 里可以用 `!Envelope::intersects(...)` 表达同样的 MBR 不相交逻辑。当前代码已改成兼容写法：

```
!query_window->intersects(cur_leaf_->mbr)
```

2. `glin_find` 参数不匹配通常说明服务器上的 `glin/glin.h` 不是当前 benchmark 对应版本。`bench_dynamic_compare_wkt.cpp` 为了让 GLIN-piece 也公平启用/关闭 PRL，会调用带 `predicate_shortcuts` 的 11 参数版本。请确认服务器源码里有这个签名：

```
grep -n "predicate_shortcuts" glin/glin.h
grep -n "void glin_find" glin/glin.h
```

如果没有输出 `predicate_shortcuts`，说明只同步了部分文件。需要重新同步整个仓库，或至少同步：

```
src/benchmark/bench_dynamic_compare_wkt.cpp
glin/glin.h
src/core/alex.h
scripts/run_dynamic_compare_diagnostics.sh
```

同步后务必删除旧构建目录重新配置：

```
rm -rf build_current
cmake -S . -B build_current \
  -DCMAKE_PREFIX_PATH=/root/autodl-tmp/glin_env26.6.29 \
  -DGEOS_INCLUDE_DIR=/root/autodl-tmp/glin_env26.6.29/include \
  -DGEOS_LIBRARY=/root/autodl-tmp/glin_env26.6.29/lib/libgeos.so \
  -DGEOS_C_LIB=/root/autodl-tmp/glin_env26.6.29/lib/libgeos_c.so
cmake --build build_current --target bench_dynamic_compare_wkt -j2
```


**5.5. 安装 Java/Maven，用于自动生成 query**

动态对比脚本里的：

```
AUTO_GENERATE_QUERIES=1
```

表示如果 `QUERY_ROOT` 下没有 query CSV，就自动调用：

```
scripts/generate_jts_strtree_knn_queries.sh
```

这个脚本内部使用 `java/jts-query-generator`，依赖 Java 和 Maven。也就是说，即使 C++ benchmark 已经编译成功，只要服务器没有 `java` 或 `mvn`，自动生成 query 仍然会报错：

```
Error: java is not installed. Run: sudo apt install -y openjdk-17-jdk maven
```

AutoDL 容器里通常是 root 用户，可以直接安装：

```
apt update
apt install -y openjdk-17-jdk maven
```

检查：

```
java -version
mvn -version
```

如果 apt 源不可用，或者希望把依赖装进当前 conda 环境，也可以用 conda-forge：

```
conda install -y -c conda-forge openjdk=17 maven
```

然后再次检查：

```
which java
which mvn
java -version
mvn -version
```

如果想单独测试 query 生成器，可以在项目根目录运行：

```
scripts/generate_jts_strtree_knn_queries.sh \
  /root/autodl-tmp/Datasets/AREAWATER.csv \
  queries/smoke_10000/AW_jts_strtree_knn \
  10000 \
  20 \
  42
```

成功后应该看到类似文件：

```
queries/smoke_10000/AW_jts_strtree_knn_0p1pct.csv
queries/smoke_10000/AW_jts_strtree_knn_1pct.csv
```

注意：第一次运行 Maven 会下载 JTS 依赖，需要网络；如果服务器不能联网，可以在有网络的机器上先生成 query CSV，再把 `queries/smoke_10000` 目录拷贝到 AutoDL 服务器，并把 `AUTO_GENERATE_QUERIES=0`。


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
