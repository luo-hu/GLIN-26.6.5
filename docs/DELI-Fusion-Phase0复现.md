# DELI-Fusion Phase 0 路由归因与验收

最后更新日期：2026-07-15

## 1. 目标

Phase 0 不增加新的索引机制，只回答五个问题：

1. Fusion 的查询收益来自 SFC path、Global Guard path，还是二者的自适应选择？
2. Adaptive 实际把多少查询交给每条路径，warm-up probe 付出了多少额外成本？
3. PRL 的 geometry-vertex witness 是否保持 exact-safe？
4. Fusion 的索引内存分别消耗在哪些组件？
5. 去掉所有 SFC 状态后，剩余收益是否只是 Base/Delta R-tree、live bitmap 和 PRL？

## 2. 三种查询路由

通过 `FUSION_ROUTE_MODE` 选择：

```text
adaptive:    保持当前 warm-up + hysteresis 路由
force_sfc:   所有正式查询走 SFC block overlay
force_guard: 所有正式查询走 Base/Delta SpatialGuard
```

三种模式都会 bulk-load 相同的 SFC blocks、Base SpatialGuard、live bitmap 和 metadata。
forced mode 不执行备用路径 probe；Adaptive 的 probe 时间不计入单条正式 query latency，但
单独写入 `fusion_guard_probe_ns`，不能在整体成本分析中忽略。

`DELI_FUSION_GUARD_ONLY` 不是第四种 route，而是独立的结构消融。它仅维护：

```text
bulk-loaded Base R-tree
mutable Delta R-tree
live bitmap lazy deletion
与其它 wrapper 相同的 PRL + GEOS exact refinement
```

它不计算 SFC，不构建 blocks/directory/compact IDs/object-to-block，不在插入时定位 block
或追加 block delta。这样才能把 `Force Guard` 中“查询不用 SFC、更新仍维护 SFC”的隐性成本
彻底剥离。

## 3. 固定更新语义

旧实现把查询路由与更新维护耦合。为避免 route ablation 同时改变 insert/delete 路径，新增：

```text
FUSION_UPDATE_MODE=coupled  # 历史默认；更新策略跟随当前 route
FUSION_UPDATE_MODE=light    # 始终使用 update-light append/lazy-delete
FUSION_UPDATE_MODE=full     # 始终执行完整 block maintenance
```

严格查询路由消融必须让三组使用相同的 `light` 或 `full`。推荐论文主消融使用 `light`，因为
它对应当前 Fusion 获得高更新吞吐的核心配置；再用 `full` 做维护策略敏感性实验。

## 4. 一键运行

在原动态实验命令中保留数据集、选择性、负载和规模参数，将执行文件改为：

```bash
RESET_RESULTS=1 \
FUSION_UPDATE_MODE=light \
RESULT_DIR=results/deli_fusion_phase0_routes \
FIGURE_DIR=figures/deli_fusion_phase0_routes \
DATASETS="AW LW PARKS ROADS" \
SELECTIVITY_TAGS="0p001pct 0p01pct 0p1pct" \
WORKLOAD_MODE=mixed MIXED_PROFILES=read_heavy \
LIMIT=500000 QUERY_LIMIT=500000 QUERY_COUNT=100 \
MIXED_OPERATIONS=200000 MIXED_CHECKPOINT_INTERVAL=5000 \
QUERY_ROOT=queries/interval_overlap_full_500000 \
CHECK_CORRECTNESS=1 AUTO_BUILD=1 \
BUILD_DIR=build_current \
./scripts/run_deli_fusion_phase0_ablation.sh
```

wrapper 依次运行 `adaptive force_sfc force_guard`。`force_guard` 轮次还会同时运行
`DELI_FUSION_GUARD_ONLY` 和 `Boost_Rtree`。非 adaptive 文件名自动带
`fusion_force_sfc` 或 `fusion_force_guard`，不会覆盖已有 raw CSV。最终 summary 中五条线
自动命名为：

```text
DELI-Fusion Adaptive
DELI-Fusion Force SFC
DELI-Fusion Force Guard
DELI-Fusion Guard-only
Boost R-tree
```

## 5. 路由统计

主 summary 和 `deli_fusion_debug.csv` 记录。若目标目录已经存在旧版 debug header，新格式
会自动写入 `deli_fusion_phase0_debug.csv`，避免新旧列错位：

```text
fusion_route_mode, fusion_update_mode
fusion_sfc_query_count, fusion_guard_query_count, fusion_sfc_route_fraction
fusion_route_switch_count, fusion_guard_probe_count
fusion_sfc_query_ns, fusion_guard_query_ns, fusion_guard_probe_ns
fusion_sfc_query_candidates, fusion_guard_query_candidates
fusion_sfc_query_exact_calls, fusion_guard_query_exact_calls
fusion_light_update_insert_count, fusion_light_update_delete_count
```

正式比较前先确认三组的 light-update insert/delete counts 一致。否则实验同时改变了更新路径，
不能解释为纯查询路由消融。

## 6. 内存拆解

Fusion-owned estimate 被拆分为：

```text
live/delta bitmaps
object-to-block locator
directory + block headers
compact IDs + block delta IDs
predicate plan
local SpatialGuards
Base SpatialGuard + Delta SpatialGuard
global delta log + recalibration queue
```

CSV 还单独报告 shared metadata 和 geometry handle。`index_bytes_estimate` 只汇总索引拥有的
上述结构，不把共享 metadata 重复计入。GEOS geometry 内部动态分配无法由当前 C++ API
可靠遍历，因此本阶段不伪造 geometry payload 的精确拆解；论文内存主结果仍应结合进程
RSS/peak RSS 报告。

Guard-only 的以下字段必须严格为 0，否则纯结构消融失败：

```text
fusion_delta_bitmap_bytes, fusion_object_locator_bytes
fusion_directory_bytes, fusion_block_header_bytes
fusion_compact_id_bytes, fusion_block_delta_id_bytes
fusion_predicate_plan_bytes, fusion_local_guard_bytes_estimate
fusion_rebuild_queue_bytes
```

其 `index_bytes_estimate` 只由 live bitmap、Base Guard 和 Delta Guard 三部分组成。

## 7. PRL 测试

运行：

```bash
cmake --build build_current --target test_rectangle_predicate_shortcut
ctest --test-dir build_current -R test_rectangle_predicate_shortcut --output-on-failure
```

测试覆盖 envelope containment、line/polygon/geometry collection 顶点见证、边界接触、
polygon hole、empty geometry、全局关闭和单独关闭 vertex witness。

## 8. 论文判定规则

```text
Guard route share > 95% 且 Full ~= Force Guard:
  当前优势主要来自 update-light SpatialGuard，不能声称 SFC/learned routing 是主要来源。

Guard-only ~= Force Guard，且 Guard-only 的 build/update/memory 更低：
  SFC overlay 没有贡献当前 workload 的查询收益，只产生额外结构维护成本。

Guard-only 明显优于 Boost，但接近 Force Guard/Adaptive：
  可防守的创新边界是 immutable Base + mutable Delta + bitmap lazy delete + PRL，
  而不是 learned 双路径协同；论文命名和贡献陈述必须据此收缩。

Force SFC 在部分 workload 明显获胜，Adaptive 能选择对应路径：
  双路径协同有实证基础，可进入 Phase 1 持续自适应路由。

Force SFC 始终落后：
  先优化 extent-aware SFC pruning；暂不增加只优化 lower_bound 的 learned router。
```
