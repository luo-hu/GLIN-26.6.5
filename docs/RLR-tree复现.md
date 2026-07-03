下面这套步骤是给 **Codex 直接改 GLIN-26.6.5 仓库**用的。目标不是完整复现原始 RLR-Tree，而是实现一个可跑、可解释、可和 GLIN / Boost R-tree 公平比较的 **RLR-ChooseSubtree-Lite**，索引名统一写成：

```text
RLR_LITE_CS
```

不要写成 `RLR_Tree`，因为原始 RLR-Tree 同时用 RL 学习 `ChooseSubtree` 和 `Split`，而这里先只复现 `ChooseSubtree`。原始 RLR 的核心思想确实是用强化学习替换 R-tree 插入过程中的子树选择和节点分裂启发式，同时不改变 R-tree 的结构和查询算法。([arXiv][1])

---

# 1. 总体复现路线

你的 GLIN 仓库当前实验重点已经是 WKT 复杂几何、GEOS 精确谓词 refinement、动态插入/删除，以及和 Boost R-tree、GEOS Quadtree 等方法统一比较。([GitHub][2]) 所以 RLR-Lite 也必须走同一条路径：

```text
polygon / multipolygon WKT
        ↓
GEOS envelope / MBR
        ↓
RLR_LITE_CS 自实现轻量 R-tree，索引 MBR + geometry id
        ↓
query rectangle 做 MBR intersects
        ↓
返回 candidate geometry ids
        ↓
复用现有 GEOS exact refine
        ↓
输出 answers / exact_calls / mbr_candidates / avg_query_ns 等指标
```

强化学习只参与 **插入时的 ChooseSubtree**，不要参与查询剪枝。这样不会引入 false negative，正确性仍然由标准 R-tree MBR 遍历和 GEOS refinement 保证。

---

# 2. 给 Codex 的直接任务说明

可以把下面这一段直接交给 Codex：

```text
请在当前 GLIN-26.6.5 仓库中实现一个 RLR-inspired baseline，名字为 RLR_LITE_CS。

目标：
1. 不修改 GLIN/DELI 现有算法逻辑。
2. 新增一个自实现的轻量 2D R-tree baseline，索引对象为 geometry envelope/MBR + object id。
3. 该 R-tree 在插入时使用 RL-lite policy 替代传统 ChooseSubtree 规则。
4. 节点 split 暂时使用确定性的 quadratic split 或 linear split，不做 RL split。
5. 查询阶段保持标准 R-tree range/intersects traversal，不允许 RL 剪枝。
6. 查询返回候选 geometry ids 后，复用 bench_dynamic_compare_wkt.cpp 中现有 GEOS exact refinement 与统计逻辑。
7. 在 INDEXES 中支持字符串 RLR_LITE_CS，使脚本可以通过 INDEXES="Boost_Rtree RLR_LITE_CS GLIN_PIECEWISE" 运行。
8. CHECK_CORRECTNESS=1 时，RLR_LITE_CS 必须与 Boost_Rtree oracle 结果一致：answers_match_boost=1, missing_count=0, extra_count=0。
9. 输出 CSV schema 不要改坏，现有 summary/plot 脚本应继续工作。
10. 不要声称实现完整 RLR-Tree；注释中写明这是 RLR-inspired ChooseSubtree-only lightweight baseline。
```

---

# 3. 需要新增的代码文件

建议新增目录：

```text
src/benchmark/rlr_lite/
```

新增文件：

```text
src/benchmark/rlr_lite/box2d.h
src/benchmark/rlr_lite/rl_agent.h
src/benchmark/rlr_lite/rtree_lite.h
src/benchmark/rlr_lite/rlr_lite_index.h
```

如果项目不喜欢多头文件，也可以合并成一个：

```text
src/benchmark/rlr_lite/rlr_lite_index.h
```

但建议拆开，后面调试方便。

---

# 4. 数据结构设计

## 4.1 Box2D

`box2d.h` 中定义：

```cpp
struct Box2D {
    double xmin;
    double ymin;
    double xmax;
    double ymax;
};
```

必须实现这些函数：

```cpp
inline double area(const Box2D& b);
inline double perimeter(const Box2D& b);
inline Box2D combine(const Box2D& a, const Box2D& b);
inline bool intersects(const Box2D& a, const Box2D& b);
inline double intersection_area(const Box2D& a, const Box2D& b);
inline double enlargement_area(const Box2D& old_box, const Box2D& new_box);
inline double enlargement_perimeter(const Box2D& old_box, const Box2D& new_box);
```

注意处理空 envelope、NaN、坐标反转。插入前统一保证：

```cpp
xmin <= xmax
ymin <= ymax
```

如果遇到点状 envelope，即 `xmin == xmax` 或 `ymin == ymax`，不要丢弃。面积为 0 时要用 `eps` 防止除零。

---

## 4.2 R-tree Node

`rtree_lite.h` 中定义轻量节点：

```cpp
struct RTreeLiteNode {
    bool leaf = true;
    Box2D mbr;
    RTreeLiteNode* parent = nullptr;

    struct Entry {
        Box2D box;
        size_t id = 0;
        RTreeLiteNode* child = nullptr;
    };

    std::vector<Entry> entries;
};
```

节点所有权建议由 `std::vector<std::unique_ptr<RTreeLiteNode>> nodes_` 管理，entry 里只保存 raw pointer。

默认参数：

```cpp
node_capacity = 64
min_fill = 0.4 * node_capacity
top_k = 4
seed = 42
```

通过环境变量覆盖：

```text
RLR_NODE_CAPACITY=64
RLR_MIN_FILL_RATIO=0.4
RLR_TOPK=4
RLR_SEED=42
```

---

# 5. ChooseSubtree-Lite 策略

在内部节点插入一个新 MBR 时，先对所有 child 计算候选特征。

每个 child 的特征为：

| 特征                | 含义                                  |
| ------------------- | ------------------------------------- |
| `delta_area`      | 插入该 child 后，child MBR 面积增加量 |
| `delta_perimeter` | 插入该 child 后，child MBR 周长增加量 |
| `delta_overlap`   | 插入后与兄弟 child 的 overlap 增加量  |
| `occupancy`       | child 当前 entry 数 / node_capacity   |

计算逻辑：

```text
new_child_box = combine(child.box, insert_box)

delta_area =
    area(new_child_box) - area(child.box)

delta_perimeter =
    perimeter(new_child_box) - perimeter(child.box)

old_overlap =
    sum intersection_area(child.box, sibling.box)

new_overlap =
    sum intersection_area(new_child_box, sibling.box)

delta_overlap =
    new_overlap - old_overlap

occupancy =
    child.entries.size() / node_capacity
```

然后按传统启发式先排序，取 top-k：

```text
sort by:
1. delta_area ascending
2. delta_overlap ascending
3. area(child.box) ascending
```

只对 top-k 做 RL action。这样动作空间固定较小，复现稳定。

---

# 6. RL Agent 设计

不要引入 PyTorch / Stable-Baselines3。这里做 C++ 内部轻量版本即可，定义为 **contextual bandit / linear Q-learning**。

`rl_agent.h` 中定义：

```cpp
struct RLRFeature {
    double delta_area;
    double delta_perimeter;
    double delta_overlap;
    double occupancy;
};

class RLRLiteAgent {
public:
    int select_action(const std::vector<RLRFeature>& candidates, bool training);
    void observe_transition(const RLRFeature& chosen_feature);
    void update(double reward);
};
```

线性 Q 函数：

```text
Q(f) = w0 * delta_area
     + w1 * delta_perimeter
     + w2 * delta_overlap
     + w3 * occupancy
```

选择动作：

```text
with probability epsilon:
    random among top-k
otherwise:
    choose candidate with max Q(f)
```

推荐初始权重：

```text
w = {-0.40, -0.10, -0.40, -0.10}
```

这些负权重表示：面积扩张、周长扩张、重叠扩张、过高 occupancy 都是不利因素。

默认超参数：

```text
RLR_EPSILON=0.10
RLR_EPSILON_MIN=0.02
RLR_LR=0.01
RLR_REWARD_INTERVAL=2048
RLR_TRAIN_QUERY_LIMIT=64
```

更新规则：

```text
error = reward - Q(chosen_feature)
w = w + lr * error * chosen_feature
```

为了稳定，所有特征必须归一化：

```text
feature_norm = feature / max(feature among top-k + eps)
```

`occupancy` 本身已经在 `[0,1]`，不需要再归一化。

---

# 7. Reward 设计

不要用 wall-clock time 做 reward。时间噪声太大，会受到 cache、GEOS refine、CPU 抖动影响。

建议同时维护两个树：

```text
rl_tree   = 使用 RLR_LITE_CS ChooseSubtree
ref_tree  = 使用传统 min-area enlargement ChooseSubtree
```

两棵树使用相同 split、相同插入、相同删除。区别只有 ChooseSubtree。

每插入 `RLR_REWARD_INTERVAL` 个对象后，用固定训练查询集评估：

```text
reward_nodes =
    (visited_nodes_ref - visited_nodes_rl) / max(1, visited_nodes_ref)

reward_candidates =
    (candidates_ref - candidates_rl) / max(1, candidates_ref)

reward =
    0.7 * reward_nodes + 0.3 * reward_candidates
```

然后裁剪：

```text
reward = clamp(reward, -1.0, 1.0)
```

这样 reward 直接优化 R-tree 的结构质量：节点访问更少、MBR 候选更少就奖励更高。

训练查询集来源：

1. 首选：单独提供 `RLR_TRAIN_QUERY_FILE`。
2. 没有单独文件时，从当前 query pool 前 `RLR_TRAIN_QUERY_LIMIT` 个 query 中取一小部分作为训练查询，并在日志里打印 warning。
3. 最终论文实验中，建议训练 query 和测试 query 分开，避免 query workload 泄漏。

---

# 8. Split 策略

第一版不要做 RL split。

实现一个确定性的 quadratic split 即可：

```text
1. 从溢出节点 entries 中选择两个 seed：
   使 combine(ei.box, ej.box) 的 wasted area 最大。

2. 其余 entry 逐个分配：
   对两个 group 分别计算 area enlargement。
   放入 enlargement 更小的 group。
   tie-break:
       group area smaller
       group size smaller

3. 保证 min_fill。
```

根节点 split 后创建新 root。非根节点 split 后向 parent 插入新 child entry。如果 parent 继续 overflow，递归 split。

---

# 9. 删除策略

第一版用懒删除，避免完整 R-tree condense delete 的工程量。

```cpp
std::vector<uint8_t> alive_;
```

插入：

```cpp
alive_[id] = 1;
```

删除：

```cpp
alive_[id] = 0;
```

查询返回 candidate 时：

```cpp
if (alive_[id]) candidates.push_back(id);
```

注意：ref_tree 和 rl_tree 都要同步 lazy delete。否则 reward 比较不公平。

如果后续删除比例很高，可以增加：

```text
RLR_REBUILD_DELETED_RATIO=0.3
```

当 deleted ratio 超过阈值时重建。但第一版先不做，避免引入额外变量。

---

# 10. 查询接口

RLR-Lite 查询必须返回两类信息：

```cpp
struct QueryStats {
    size_t visited_nodes = 0;
    size_t leaf_entries_checked = 0;
    size_t mbr_candidates = 0;
};

void range_query(const Box2D& query_box,
                 std::vector<size_t>& candidate_ids,
                 QueryStats* stats);
```

遍历逻辑：

```text
DFS(root):
    visited_nodes++
    for entry in node.entries:
        if !intersects(entry.box, query_box):
            continue

        if node.leaf:
            leaf_entries_checked++
            if alive[entry.id]:
                candidate_ids.push_back(entry.id)
        else:
            DFS(entry.child)
```

RLR-Lite 不做 exact predicate。exact predicate 继续由 `bench_dynamic_compare_wkt.cpp` 中现有 GEOS refinement 完成。

---

# 11. 接入 bench_dynamic_compare_wkt.cpp

仓库 README 显示当前动态实验主入口是 `bench_dynamic_compare_wkt`，主脚本是 `scripts/run_dynamic_compare_diagnostics.sh`。([GitHub][2]) CMake 中已经有 `bench_dynamic_compare_wkt` target。([GitHub][3])

Codex 修改步骤：

1. 在 `bench_dynamic_compare_wkt.cpp` 中搜索：

```text
Boost_Rtree
GEOS_Quadtree
GLIN_PIECEWISE
DELI_ALEX
INDEXES
```

2. 找到 index string parser，新增：

```cpp
else if (name == "RLR_LITE_CS") {
    ...
}
```

3. 找到 Boost R-tree benchmark wrapper，按同样接口新增 `RLRLiteIndexWrapper`。
4. Wrapper 至少提供：

```cpp
build(initial_records)
insert(record_id)
erase(record_id)
query(query_box, candidate_ids, stats)
estimate_size_bytes()
name() -> "RLR_LITE_CS"
```

5. query 后的 exact refine 逻辑必须复用现有代码，不要另写一套 GEOS predicate。
6. 输出字段映射建议：

```text
mbr_candidates           = candidate_ids.size()
visited_blocks           = stats.visited_nodes
block_checks             = stats.visited_nodes
compact_records_scanned  = stats.leaf_entries_checked
delta_records_scanned    = 0
exact_calls              = 现有 GEOS refine exact calls
answers                  = 现有 GEOS refine result count
index_mb_estimate        = estimate_size_bytes() / 1024 / 1024
```

7. 正确性检查仍然以 Boost R-tree + GEOS 为 oracle。README 中 smoke test 要求正确性检查时 `answers_match_boost=1, missing_count=0, extra_count=0`。([GitHub][2])

---

# 12. CMake 修改

如果只新增 header，并且 `bench_dynamic_compare_wkt.cpp` include 它们，则 CMake 不需要新增 target。

如果新增了 `.cpp` 文件，比如：

```text
src/benchmark/rlr_lite/rtree_lite.cpp
src/benchmark/rlr_lite/rl_agent.cpp
```

则修改 `CMakeLists.txt`：

```cmake
add_executable(bench_dynamic_compare_wkt
    src/benchmark/bench_dynamic_compare_wkt.cpp
    src/benchmark/rlr_lite/rtree_lite.cpp
    src/benchmark/rlr_lite/rl_agent.cpp
    glin/hilbert/hilbert.h
    glin/hilbert/hilbert.cpp
)
```

保留：

```cmake
target_compile_definitions(bench_dynamic_compare_wkt PRIVATE PIECE)
target_link_libraries(bench_dynamic_compare_wkt PRIVATE ${GEOS_LIBRARY} ${Boost_SYSTEM_LIBRARY})
target_include_directories(bench_dynamic_compare_wkt PUBLIC ${GLIN_WKT_INCLUDE_DIRS})
```

---

# 13. 构建步骤

仓库 README 推荐的 benchmark 构建方式是：([GitHub][2])

```bash
cmake -S . -B build_current -DCMAKE_BUILD_TYPE=Release
cmake --build build_current --target bench_dynamic_compare_wkt -j2
```

修改后先只编译主 target：

```bash
cmake -S . -B build_current -DCMAKE_BUILD_TYPE=Release
cmake --build build_current --target bench_dynamic_compare_wkt -j2
```

---

# 14. Smoke Test

先跑小数据，目标是验证编译、查询、插入、删除、CSV 输出和正确性。

```bash
RESET_RESULTS=1 OVERWRITE=1 AUTO_BUILD=1 \
DATASETS="AW" \
LIMIT=10000 QUERY_LIMIT=10000 \
QUERY_ROOT=queries/smoke_10000 \
RESULT_DIR=results/smoke_rlr_lite \
FIGURE_DIR=figures/smoke_rlr_lite \
SELECTIVITY_TAGS="1pct" QUERY_COUNT=20 \
AUTO_GENERATE_QUERIES=1 CHECK_CORRECTNESS=1 \
INDEXES="Boost_Rtree RLR_LITE_CS" \
RLR_NODE_CAPACITY=64 \
RLR_TOPK=4 \
RLR_EPSILON=0.10 \
RLR_LR=0.01 \
RLR_REWARD_INTERVAL=1024 \
RLR_TRAIN_QUERY_LIMIT=20 \
RLR_SEED=42 \
BUILD_DIR=build_current \
./scripts/run_dynamic_compare_diagnostics.sh
```

仓库 README 中 smoke test 原本就是用 `run_dynamic_compare_diagnostics.sh` 自动构建、生成 query、跑 staged workload、检查 Boost oracle 正确性和输出结果。([GitHub][2])

检查输出：

```bash
cat results/smoke_rlr_lite/dynamic_compare_summary.csv | grep RLR_LITE_CS
```

必须满足：

```text
answers_match_boost=1
missing_count=0
extra_count=0
```

---

# 15. 主实验：staged workload

通过 smoke test 后，跑 staged 动态实验。README 中 staged workload 是 bulk-load 50%、query checkpoint、insert 20%、query checkpoint、delete 10%、query checkpoint。([GitHub][2])

```bash
RESET_RESULTS=1 OVERWRITE=1 AUTO_BUILD=1 \
WORKLOAD_MODE=staged \
DATASETS="AW PARKS" \
LIMIT=500000 QUERY_LIMIT=500000 \
QUERY_ROOT=queries/interval_overlap_full_500000 \
RESULT_DIR=results/rlr_lite_staged_0.5m \
FIGURE_DIR=figures/rlr_lite_staged_0.5m \
SELECTIVITY_TAGS="0p1pct 1pct" QUERY_COUNT=200 \
AUTO_GENERATE_QUERIES=1 CHECK_CORRECTNESS=1 \
INDEXES="Boost_Rtree RLR_LITE_CS GLIN_PIECEWISE DELI_ALEX_HYBRID_COST" \
RLR_NODE_CAPACITY=64 \
RLR_TOPK=4 \
RLR_EPSILON=0.10 \
RLR_EPSILON_MIN=0.02 \
RLR_LR=0.01 \
RLR_REWARD_INTERVAL=2048 \
RLR_TRAIN_QUERY_LIMIT=64 \
RLR_SEED=42 \
BUILD_DIR=build_current \
./scripts/run_dynamic_compare_diagnostics.sh
```

---

# 16. 主实验：mixed workload

README 里 mixed workload 支持 read-heavy、balanced、write-heavy 三类固定操作序列。([GitHub][2])

```bash
RESET_RESULTS=1 OVERWRITE=1 AUTO_BUILD=1 \
WORKLOAD_MODE=mixed \
MIXED_PROFILES="read_heavy balanced write_heavy" \
MIXED_OPERATIONS=50000 \
MIXED_CHECKPOINT_INTERVAL=1000 \
DATASETS="AW PARKS" \
LIMIT=500000 QUERY_LIMIT=500000 \
QUERY_ROOT=queries/interval_overlap_full_500000 \
RESULT_DIR=results/rlr_lite_mixed_0.5m \
FIGURE_DIR=figures/rlr_lite_mixed_0.5m \
SELECTIVITY_TAGS="0p1pct" QUERY_COUNT=200 \
AUTO_GENERATE_QUERIES=1 CHECK_CORRECTNESS=0 \
INDEXES="Boost_Rtree RLR_LITE_CS GLIN_PIECEWISE DELI_ALEX_HYBRID_COST" \
RLR_NODE_CAPACITY=64 \
RLR_TOPK=4 \
RLR_EPSILON=0.10 \
RLR_LR=0.01 \
RLR_REWARD_INTERVAL=4096 \
RLR_TRAIN_QUERY_LIMIT=64 \
RLR_SEED=42 \
PLOT_MIXED_ROLLING_WINDOW=5 \
PLOT_MIXED_CUMULATIVE_THROUGHPUT=1 \
BUILD_DIR=build_current \
./scripts/run_dynamic_compare_diagnostics.sh
```

最终正式结果再打开 correctness：

```bash
CHECK_CORRECTNESS=1 CORRECTNESS_EVERY_N=5
```

长实验全 checkpoint 都做 Boost oracle 会比较慢，README 也建议长探索实验可先关闭 correctness，最终再开启或降低检查频率。([GitHub][2])

---

# 17. 输出字段关注哪些

README 中动态结果 CSV 已经包含这些关键字段：`avg_query_ns`、`p95_query_ns`、`query_tps`、`insert_tps`、`delete_tps`、`mbr_candidates`、`exact_calls`、`answers`、`answers_match_boost`、`missing_count`、`extra_count` 等。([GitHub][2])

RLR_LITE_CS 重点比较：

```text
avg_query_ns
p95_query_ns
mbr_candidates
exact_calls
insert_tps
delete_tps
index_mb_estimate
answers_match_boost
missing_count
extra_count
```

其中最关键的是：

```text
mbr_candidates 是否低于 Boost_Rtree
avg_query_ns 是否接近或低于 Boost_Rtree
insert_tps 是否还能接受
answers_match_boost 是否始终为 1
```

---

# 18. 单元测试建议

让 Codex 额外加一个最小测试，哪怕不接入 CTest，也要能本地跑。

测试 1：MBR 查询正确性

```text
1. 随机生成 10000 个 rectangle。
2. 插入 RLR_LITE_CS。
3. 随机生成 1000 个 query rectangle。
4. RLR_LITE_CS 返回 candidate ids。
5. 暴力扫描所有 alive rectangle。
6. 两者集合必须完全一致。
```

测试 2：插入/删除正确性

```text
1. 插入 10000 个 rectangle。
2. 删除其中 2000 个 id。
3. 查询 1000 个窗口。
4. RLR_LITE_CS 结果必须等于暴力扫描 alive set。
```

测试 3：和 Boost oracle 对齐

```text
跑 smoke test：
INDEXES="Boost_Rtree RLR_LITE_CS"
CHECK_CORRECTNESS=1
```

必须满足：

```text
answers_match_boost=1
missing_count=0
extra_count=0
```

---

# 19. 参数调试顺序

结果不稳定时，按这个顺序调：

| 问题                   | 优先调整                                                                         |
| ---------------------- | -------------------------------------------------------------------------------- |
| 编译或内存问题         | 先把 `RLR_NODE_CAPACITY=64` 固定                                               |
| 查询慢                 | 看 `mbr_candidates` 和 `visited_blocks`，不是先看 wall-clock                 |
| RL 不如 heuristic      | 降低 `RLR_EPSILON` 到 0.05                                                     |
| 训练太慢               | 增大 `RLR_REWARD_INTERVAL` 到 4096 或 8192                                     |
| reward 抖动大          | 降低 `RLR_TRAIN_QUERY_LIMIT` 的随机性，固定前 64 个训练 query                  |
| update throughput 太差 | 增大 `RLR_REWARD_INTERVAL`，或设置 `RLR_FREEZE_AFTER_BUILD=1` 做 frozen 版本 |

建议先固定一组默认参数，不要针对每个数据集调参：

```text
RLR_NODE_CAPACITY=64
RLR_TOPK=4
RLR_EPSILON=0.10
RLR_EPSILON_MIN=0.02
RLR_LR=0.01
RLR_REWARD_INTERVAL=2048
RLR_TRAIN_QUERY_LIMIT=64
RLR_SEED=42
```

---

# 20. 论文/汇报中的准确表述

推荐写法：

```text
We implement an RLR-inspired lightweight baseline, denoted as RLR-Lite-CS. It replaces the hand-crafted ChooseSubtree rule in R-tree insertion with a lightweight reinforcement-learning policy, while keeping node splitting and range-query traversal unchanged. Since the original RLR-Tree involves both RL-based ChooseSubtree and RL-based Split and requires intrusive modification of R-tree internals, RLR-Lite-CS serves as a practical reproducible RL-enhanced R-tree baseline for complex polygon workloads.
```

中文写法：

```text
本文实现了一个受 RLR-Tree 启发的轻量化强化学习 R-tree baseline，记为 RLR-Lite-CS。该方法仅将 R-tree 插入阶段的 ChooseSubtree 子树选择规则替换为轻量强化学习策略，节点分裂和查询遍历仍保持传统 R-tree 方式。由于完整 RLR-Tree 同时包含 RL ChooseSubtree 和 RL Split，且需要深度修改 R-tree 内部结构，本文采用 RLR-Lite-CS 作为面向复杂多边形动态 workload 的可复现实验对比。
```

这句话很重要。否则审稿人会认为你声称复现了完整 RLR-Tree。

---

# 21. 最小完成标准

Codex 改完后，至少达到这四点才算完成：

```text
1. cmake --build build_current --target bench_dynamic_compare_wkt -j2 成功。
2. INDEXES="Boost_Rtree RLR_LITE_CS" 可以正常运行。
3. CHECK_CORRECTNESS=1 时，RLR_LITE_CS 对 Boost oracle 无 missing / extra。
4. CSV summary 中出现 index=RLR_LITE_CS，且 avg_query_ns、mbr_candidates、insert_tps、delete_tps、index_mb_estimate 都有有效数值。
```

第一阶段不要追求一定超过 Boost R-tree。Boost.Geometry R-tree 是高度优化实现，自写 R-tree 即使 candidate 更少，也可能 wall-clock 不占优。RLR_LITE_CS 的主要价值是提供一个 **R-tree + 强化学习构造策略** 的可复现 baseline。

[1]: https://arxiv.org/abs/2103.04541?utm_source=chatgpt.com
[2]: https://github.com/luo-hu/GLIN-26.6.5

[3]: https://github.com/luo-hu/GLIN-26.6.5/blob/main/CMakeLists.txt
