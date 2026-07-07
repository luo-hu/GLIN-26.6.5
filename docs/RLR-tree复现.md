正常。你当前的 `RLR_LITE_CS` 只是 **RL ChooseSubtree-only**，它只能影响插入路径，不能直接改变 split 造成的 MBR overlap。对于 ZGAP_MIXED 这类 fat-object / overlap-heavy workload，候选数主要由节点分裂后的 MBR 重叠决定，所以 `mbr_candidates` 和 Boost 接近是合理结果。

下一步做 `RLR_LITE_CS_SPLIT` 是对的。目标不是复现 2025 那篇 `RL-RTree` 的 SGAT + PPO 查询策略效果，而是做一个更接近 **RLR-style R-tree reinforcement baseline** 的轻量版本：

```text
RLR_LITE_CS:
    RL ChooseSubtree + heuristic split + standard query

RLR_LITE_CS_SPLIT:
    RL ChooseSubtree + RL-lite split + standard query
```

下面是给 Codex 的修改步骤。

---

# 1. 总体目标

保留当前 `RLR_LITE_CS`，不要覆盖它。新增一个 index 名称：

```text
RLR_LITE_CS_SPLIT
```

两者区别：

```text
RLR_LITE_CS:
    插入路径 ChooseSubtree 用当前 RL-lite
    节点溢出 split 仍用 heuristic quadratic split

RLR_LITE_CS_SPLIT:
    插入路径 ChooseSubtree 用当前 RL-lite
    节点溢出 split 改为 RL-lite / bandit split
```

查询阶段仍然必须是标准 R-tree traversal：

```text
query MBR
    ↓
R-tree intersects traversal
    ↓
candidate ids
    ↓
GEOS exact refine
```

不要让 RL 参与查询剪枝，否则可能产生 false negative。

---

# 2. 给 Codex 的直接任务说明

可以直接把下面这段给 Codex：

```text
请在当前 GLIN-26.6.5 仓库中，在已有 RLR_LITE_CS 的基础上新增 RLR_LITE_CS_SPLIT。

要求：
1. 保留 RLR_LITE_CS，不要破坏已有结果。
2. 新增 index 字符串 RLR_LITE_CS_SPLIT。
3. RLR_LITE_CS_SPLIT 使用当前已有的 RL ChooseSubtree。
4. RLR_LITE_CS_SPLIT 在节点 overflow 时使用 RL-lite split policy，而不是固定 quadratic split。
5. RL split 不使用深度学习框架，只实现 C++ 内部 lightweight contextual bandit / linear scoring。
6. Query 阶段保持标准 R-tree MBR intersects traversal，不允许 RL 剪枝。
7. Delete 仍然使用当前 lazy delete。
8. Correctness 必须继续以 Boost_Rtree + GEOS 为 oracle，CHECK_CORRECTNESS=1 时 answers_match_boost=1, missing_count=0, extra_count=0。
9. 主 summary CSV schema 不要破坏；如需调试信息，单独写 rlr_lite_debug.csv。
10. 注释和日志中说明：RLR_LITE_CS_SPLIT 是 RLR-inspired ChooseSubtree+Split lightweight baseline，不是完整 RLR-Tree，也不是 2025 RL-RTree。
```

---

# 3. 代码结构建议

如果你现在已有：

```text
src/benchmark/rlr_lite/
```

就在里面新增或修改：

```text
src/benchmark/rlr_lite/rl_split_agent.h
src/benchmark/rlr_lite/split_candidate.h
src/benchmark/rlr_lite/rtree_lite.h
src/benchmark/rlr_lite/rlr_lite_index.h
```

如果目前是单头文件实现，也可以直接在当前 `rlr_lite_index.h` 里加，但建议至少逻辑上分成：

```text
ChooseSubtree policy
Split candidate generator
Split policy
Reward updater
Debug logger
```

---

# 4. 新增配置项

新增环境变量：

```text
RLR_SPLIT_POLICY=rl              # rl / quadratic
RLR_SPLIT_EPSILON=0.10
RLR_SPLIT_EPSILON_MIN=0.02
RLR_SPLIT_LR=0.01
RLR_SPLIT_MAX_CANDIDATES=96
RLR_SPLIT_STEP=1
RLR_SPLIT_QUERY_SAMPLE=16
RLR_SPLIT_REWARD_CAND_WEIGHT=0.60
RLR_SPLIT_REWARD_NODE_WEIGHT=0.40
RLR_SPLIT_SEED=42
```

默认参数建议：

```text
RLR_NODE_CAPACITY=64
RLR_MIN_FILL_RATIO=0.4
RLR_TOPK=4
RLR_REWARD_INTERVAL=2048
RLR_TRAIN_QUERY_LIMIT=64

RLR_SPLIT_EPSILON=0.10
RLR_SPLIT_EPSILON_MIN=0.02
RLR_SPLIT_LR=0.01
RLR_SPLIT_MAX_CANDIDATES=96
RLR_SPLIT_STEP=1
RLR_SPLIT_QUERY_SAMPLE=16
```

index 名称映射：

```cpp
if (index_name == "RLR_LITE_CS") {
    choose_policy = ChoosePolicy::RL_LITE;
    split_policy  = SplitPolicy::QUADRATIC;
}

if (index_name == "RLR_LITE_CS_SPLIT") {
    choose_policy = ChoosePolicy::RL_LITE;
    split_policy  = SplitPolicy::RL_LITE;
}
```

---

# 5. Split 候选生成

节点 overflow 时，假设当前节点有 `M + 1` 个 entries，例如 `node_capacity=64` 时有 65 个 entries。

不要让 RL 直接输出 arbitrary partition。应先生成一批合法 split candidates，然后让 RL 从候选中选择。

## 5.1 候选生成方式

对 entries 按多个 key 排序：

```text
xmin
xmax
xcenter
ymin
ymax
ycenter
area
```

每种排序下，枚举 split position：

```text
s ∈ [min_fill, n - min_fill]
```

例如：

```cpp
for key in sort_keys:
    sorted = entries sorted by key
    for s from min_fill to n - min_fill step RLR_SPLIT_STEP:
        left  = sorted[0:s]
        right = sorted[s:n]
        add candidate(left, right)
```

如果候选数量超过 `RLR_SPLIT_MAX_CANDIDATES`，就均匀采样或保留质量较好的前若干个。

必须保证：

```text
left.size()  >= min_fill
right.size() >= min_fill
left ∪ right = original entries
left ∩ right = empty
```

如果由于参数问题没有生成候选，则 fallback 到当前 quadratic split。

---

# 6. Split candidate 特征

每个 split candidate 计算以下特征：

```text
f0 = overlap_norm
f1 = total_area_norm
f2 = total_perimeter_norm
f3 = balance_norm
f4 = query_cross_norm
```

定义如下。

## 6.1 overlap_norm

```text
left_box  = MBR(left)
right_box = MBR(right)

overlap = intersection_area(left_box, right_box)
```

归一化：

```text
overlap_norm = overlap / max(parent_area, eps)
```

这是最重要的特征。目标是降低两个 child MBR 的重叠。

## 6.2 total_area_norm

```text
total_area = area(left_box) + area(right_box)
total_area_norm = total_area / max(parent_area, eps)
```

目标是避免 split 后两个 child MBR 总面积过大。

## 6.3 total_perimeter_norm

```text
total_perimeter = perimeter(left_box) + perimeter(right_box)
total_perimeter_norm = total_perimeter / max(parent_perimeter, eps)
```

对应 R-tree/R*-tree 中常用的 margin / perimeter split criterion。

## 6.4 balance_norm

```text
balance_norm = abs(left.size() - right.size()) / double(n)
```

目标是避免极端不平衡 split。

## 6.5 query_cross_norm

如果当前 `RLR_LITE_CS` 已经保存训练 query pool，则从里面抽 `RLR_SPLIT_QUERY_SAMPLE` 个 query box：

```text
query_cross = count(q intersects left_box && q intersects right_box) / sample_count
```

这个特征很重要。它直接衡量一个 query 是否会同时访问 split 后的两个 child。

如果当前代码里 split 阶段拿不到 train queries，则先设：

```text
query_cross_norm = 0
```

第一版可以不强求 query-aware split，但如果要让 ZGAP_MIXED 这种 overlap-heavy workload 有变化，建议后续加上这个特征。

---

# 7. RL Split Agent

新增一个 split agent，不要和当前 ChooseSubtree agent 混在一起。

```cpp
struct RLRSplitFeature {
    double overlap_norm;
    double total_area_norm;
    double total_perimeter_norm;
    double balance_norm;
    double query_cross_norm;
};

class RLRSplitAgent {
public:
    int select_action(const std::vector<RLRSplitFeature>& candidates,
                      bool training);

    void observe(const RLRSplitFeature& chosen);

    void update(double reward);

    std::array<double, 5> weights() const;
};
```

## 7.1 打分函数

用 cost-based linear score：

```text
score(candidate) =
    - w0 * overlap_norm
    - w1 * total_area_norm
    - w2 * total_perimeter_norm
    - w3 * balance_norm
    - w4 * query_cross_norm
```

选择 score 最大的 candidate，也就是综合 cost 最小的 split。

初始权重：

```text
w0 = 0.40   overlap
w1 = 0.25   total area
w2 = 0.20   perimeter
w3 = 0.10   balance
w4 = 0.05   query_cross
```

如果启用了 query_cross，可以改成：

```text
w0 = 0.35
w1 = 0.20
w2 = 0.15
w3 = 0.10
w4 = 0.20
```

因为 query_cross 对实际 workload 更直接。

## 7.2 ε-greedy

```cpp
if (training && rand01() < epsilon) {
    return random_candidate;
} else {
    return argmax_score;
}
```

每次 reward update 后：

```cpp
epsilon = max(epsilon_min, epsilon * 0.995);
```

---

# 8. Split agent 更新规则

当前 `RLR_LITE_CS` 的 reward 是每隔 `RLR_REWARD_INTERVAL` 次插入，用 train queries 比较 RL tree 和 heuristic ref tree。

继续使用这个机制。

区别是：现在 reward 同时更新两个 agent：

```text
ChooseSubtree agent
Split agent
```

建议 reward 改成更偏候选数：

```text
r_nodes =
    (visited_nodes_ref - visited_nodes_rl) / max(1, visited_nodes_ref)

r_candidates =
    (candidates_ref - candidates_rl) / max(1, candidates_ref)

reward =
    0.4 * r_nodes + 0.6 * r_candidates
```

裁剪：

```text
reward = clamp(reward, -1.0, 1.0)
```

## 8.1 Split 权重更新

因为 split features 都是 cost features，更新规则不要直接写成普通 Q-learning 的：

```cpp
w += lr * (reward - q) * feature
```

这个容易把 cost 特征越学越偏。

建议用下面这个简单稳定的规则：

```cpp
for each j:
    w[j] -= lr * reward * mean_chosen_feature[j];

clamp w[j] to [0.001, 5.0];
normalize weights so sum(w) = 1.0;
```

含义：

```text
reward > 0:
    说明最近 RL tree 比 ref tree 好
    降低已选择特征的惩罚权重

reward < 0:
    说明最近 RL tree 比 ref tree 差
    增加已选择特征的惩罚权重
```

为了降低开销，不需要保存所有 split transition。用 accumulator 即可：

```cpp
struct FeatureAccumulator {
    std::array<double, 5> sum;
    size_t count;

    void add(feature);
    feature mean() const;
    void clear();
};
```

每次 RL split 选中一个 candidate：

```cpp
split_agent.observe(chosen_feature);
```

每次 reward interval：

```cpp
split_agent.update(reward);
split_agent.clear_observations();
```

---

# 9. Split 接入点

当前应该已有类似逻辑：

```cpp
if (node->entries.size() > node_capacity_) {
    split_node(node);
}
```

改成：

```cpp
if (node->entries.size() > node_capacity_) {
    split_node(node, tree_role);
}
```

其中 `tree_role` 用来区分：

```text
RL tree
Ref tree
```

对于 `RLR_LITE_CS_SPLIT`：

```text
RL tree:
    ChooseSubtree = RL-lite
    Split         = RL-lite

Ref tree:
    ChooseSubtree = heuristic
    Split         = heuristic quadratic
```

不要让 ref tree 也用 RL split。否则 reward 没有基准。

伪代码：

```cpp
SplitResult split_entries(const std::vector<Entry>& entries,
                          bool is_leaf,
                          bool use_rl_split,
                          bool training) {
    if (!use_rl_split) {
        return quadratic_split(entries);
    }

    auto candidates = generate_split_candidates(entries);

    if (candidates.empty()) {
        return quadratic_split(entries);
    }

    auto features = compute_split_features(candidates);

    int action = split_agent_.select_action(features, training);

    split_agent_.observe(features[action]);

    return candidates[action].to_split_result();
}
```

---

# 10. Debug 统计必须加

否则你跑完后仍然不知道 RL split 有没有起作用。

不要改坏主 summary CSV。单独输出：

```text
rlr_lite_debug.csv
```

建议字段：

```text
dataset
index
phase
checkpoint
inserted
split_count
split_rl_count
split_random_count
split_greedy_count
split_disagree_with_quadratic
split_reward_updates
last_reward
avg_reward
train_ref_visited
train_rl_visited
train_ref_candidates
train_rl_candidates
cs_weight_0
cs_weight_1
cs_weight_2
cs_weight_3
split_weight_overlap
split_weight_area
split_weight_perimeter
split_weight_balance
split_weight_query_cross
```

关键指标：

```text
split_rl_count > 0
split_disagree_with_quadratic > 0
split_reward_updates > 0
split weights 有变化
train_rl_candidates 和 train_ref_candidates 不完全一样
```

如果这些都没有变化，说明 RL split 虽然写了，但实际上没有影响树结构。

---

# 11. 正确性测试

Codex 改完后，先不要直接跑大实验。按这个顺序测。

## 11.1 Split 单元测试

随机生成 1000 次 overflow entries，检查：

```text
left.size() >= min_fill
right.size() >= min_fill
left.size() + right.size() == original.size()
每个 entry 恰好出现一次
left_box / right_box 坐标合法
```

## 11.2 R-tree MBR 查询正确性

生成随机 rectangles：

```text
insert 10000 rectangles
query 1000 rectangles
RLR_LITE_CS_SPLIT result == brute-force MBR intersects result
```

## 11.3 Delete 正确性

```text
insert 10000 rectangles
lazy delete 2000 rectangles
query 1000 rectangles
RLR_LITE_CS_SPLIT result == brute-force alive rectangles
```

## 11.4 和 Boost oracle 对齐

用 benchmark smoke test：

```bash
RESET_RESULTS=1 OVERWRITE=1 AUTO_BUILD=1 \
DATASETS="AW" \
LIMIT=10000 QUERY_LIMIT=10000 \
QUERY_ROOT=queries/smoke_10000 \
RESULT_DIR=results/smoke_rlr_lite_split \
FIGURE_DIR=figures/smoke_rlr_lite_split \
SELECTIVITY_TAGS="1pct" QUERY_COUNT=20 \
AUTO_GENERATE_QUERIES=1 CHECK_CORRECTNESS=1 \
INDEXES="Boost_Rtree RLR_LITE_CS RLR_LITE_CS_SPLIT" \
RLR_NODE_CAPACITY=64 \
RLR_TOPK=4 \
RLR_EPSILON=0.10 \
RLR_LR=0.01 \
RLR_REWARD_INTERVAL=1024 \
RLR_TRAIN_QUERY_LIMIT=20 \
RLR_SPLIT_EPSILON=0.10 \
RLR_SPLIT_LR=0.01 \
RLR_SPLIT_MAX_CANDIDATES=96 \
RLR_SPLIT_QUERY_SAMPLE=8 \
RLR_SEED=42 \
RLR_SPLIT_SEED=42 \
BUILD_DIR=build_current \
./scripts/run_dynamic_compare_diagnostics.sh
```

检查：

```bash
grep "RLR_LITE_CS_SPLIT" results/smoke_rlr_lite_split/dynamic_compare_summary.csv
```

必须满足：

```text
answers_match_boost=1
missing_count=0
extra_count=0
```

---

# 12. 小规模对比实验

先跑 ZGAP_MIXED 小规模，确认 split 是否真的改变候选数。

```bash
RESET_RESULTS=1 OVERWRITE=1 AUTO_BUILD=1 \
WORKLOAD_MODE=mixed \
MIXED_PROFILES="read_heavy" \
MIXED_OPERATIONS=10000 \
MIXED_CHECKPOINT_INTERVAL=1000 \
DATASETS="ZGAP_MIXED" \
LIMIT=100000 QUERY_LIMIT=100000 \
QUERY_ROOT=queries/zgap_mixed_100000 \
RESULT_DIR=results/rlr_split_zgap_small \
FIGURE_DIR=figures/rlr_split_zgap_small \
SELECTIVITY_TAGS="0p01pct" QUERY_COUNT=100 \
AUTO_GENERATE_QUERIES=1 CHECK_CORRECTNESS=1 \
INDEXES="Boost_Rtree RLR_LITE_CS RLR_LITE_CS_SPLIT" \
RLR_NODE_CAPACITY=64 \
RLR_TOPK=4 \
RLR_EPSILON=0.10 \
RLR_EPSILON_MIN=0.02 \
RLR_LR=0.01 \
RLR_REWARD_INTERVAL=2048 \
RLR_TRAIN_QUERY_LIMIT=64 \
RLR_SPLIT_EPSILON=0.10 \
RLR_SPLIT_EPSILON_MIN=0.02 \
RLR_SPLIT_LR=0.01 \
RLR_SPLIT_MAX_CANDIDATES=96 \
RLR_SPLIT_STEP=1 \
RLR_SPLIT_QUERY_SAMPLE=16 \
RLR_SEED=42 \
RLR_SPLIT_SEED=42 \
BUILD_DIR=build_current \
./scripts/run_dynamic_compare_diagnostics.sh
```

如果你的脚本里 0.01% tag 不是 `0p01pct`，换成你当前实际使用的 tag。

重点看：

```text
RLR_LITE_CS_SPLIT vs RLR_LITE_CS:

mbr_candidates 是否下降
exact_calls 是否下降
visited_blocks / block_checks 是否下降
avg_query_ms 是否至少不明显恶化
insert throughput 下降多少
```

第一目标不是立刻超过 Boost，而是让：

```text
RLR_LITE_CS_SPLIT 的 mbr_candidates < RLR_LITE_CS
```

如果候选数还是完全一样，说明 split policy 没有形成结构差异。

---

# 13. 正式 staged 实验

```bash
RESET_RESULTS=1 OVERWRITE=1 AUTO_BUILD=1 \
WORKLOAD_MODE=staged \
DATASETS="ZGAP_MIXED AW PARKS" \
LIMIT=500000 QUERY_LIMIT=500000 \
QUERY_ROOT=queries/interval_overlap_full_500000 \
RESULT_DIR=results/rlr_lite_cs_split_staged_0.5m \
FIGURE_DIR=figures/rlr_lite_cs_split_staged_0.5m \
SELECTIVITY_TAGS="0p01pct 0p1pct 1pct" QUERY_COUNT=200 \
AUTO_GENERATE_QUERIES=1 CHECK_CORRECTNESS=1 \
INDEXES="Boost_Rtree RLR_LITE_CS RLR_LITE_CS_SPLIT GLIN_PIECEWISE DELI_ALEX_HYBRID_COST" \
RLR_NODE_CAPACITY=64 \
RLR_TOPK=4 \
RLR_EPSILON=0.10 \
RLR_EPSILON_MIN=0.02 \
RLR_LR=0.01 \
RLR_REWARD_INTERVAL=2048 \
RLR_TRAIN_QUERY_LIMIT=64 \
RLR_SPLIT_EPSILON=0.10 \
RLR_SPLIT_EPSILON_MIN=0.02 \
RLR_SPLIT_LR=0.01 \
RLR_SPLIT_MAX_CANDIDATES=96 \
RLR_SPLIT_STEP=1 \
RLR_SPLIT_QUERY_SAMPLE=16 \
RLR_SEED=42 \
RLR_SPLIT_SEED=42 \
BUILD_DIR=build_current \
./scripts/run_dynamic_compare_diagnostics.sh
```

---

# 14. 正式 mixed 实验

```bash
RESET_RESULTS=1 OVERWRITE=1 AUTO_BUILD=1 \
WORKLOAD_MODE=mixed \
MIXED_PROFILES="read_heavy balanced write_heavy" \
MIXED_OPERATIONS=50000 \
MIXED_CHECKPOINT_INTERVAL=1000 \
DATASETS="ZGAP_MIXED AW PARKS" \
LIMIT=500000 QUERY_LIMIT=500000 \
QUERY_ROOT=queries/interval_overlap_full_500000 \
RESULT_DIR=results/rlr_lite_cs_split_mixed_0.5m \
FIGURE_DIR=figures/rlr_lite_cs_split_mixed_0.5m \
SELECTIVITY_TAGS="0p01pct 0p1pct 1pct" QUERY_COUNT=200 \
AUTO_GENERATE_QUERIES=1 CHECK_CORRECTNESS=0 \
INDEXES="Boost_Rtree RLR_LITE_CS RLR_LITE_CS_SPLIT GLIN_PIECEWISE DELI_ALEX_HYBRID_COST" \
RLR_NODE_CAPACITY=64 \
RLR_TOPK=4 \
RLR_EPSILON=0.10 \
RLR_EPSILON_MIN=0.02 \
RLR_LR=0.01 \
RLR_REWARD_INTERVAL=4096 \
RLR_TRAIN_QUERY_LIMIT=64 \
RLR_SPLIT_EPSILON=0.10 \
RLR_SPLIT_EPSILON_MIN=0.02 \
RLR_SPLIT_LR=0.01 \
RLR_SPLIT_MAX_CANDIDATES=96 \
RLR_SPLIT_STEP=1 \
RLR_SPLIT_QUERY_SAMPLE=16 \
RLR_SEED=42 \
RLR_SPLIT_SEED=42 \
PLOT_MIXED_ROLLING_WINDOW=5 \
PLOT_MIXED_CUMULATIVE_THROUGHPUT=1 \
BUILD_DIR=build_current \
./scripts/run_dynamic_compare_diagnostics.sh
```

最终正式数据再开 correctness：

```bash
CHECK_CORRECTNESS=1 CORRECTNESS_EVERY_N=5
```

---

# 15. 如果效果仍然不明显，按这个顺序调

## 15.1 先确认 RL split 真的被调用

看 debug：

```text
split_rl_count > 0
split_disagree_with_quadratic > 0
split_reward_updates > 0
split_weight_* 有变化
```

如果 `split_disagree_with_quadratic = 0`，说明 learned split 和 heuristic split 每次都选同一个候选，结果自然不会变。

处理：

```text
提高 RLR_SPLIT_EPSILON 到 0.20
提高 RLR_SPLIT_QUERY_SAMPLE 到 32
把 query_cross 权重提高到 0.20 或 0.30
```

## 15.2 如果 candidate 不降

说明 split objective 还不够贴近 workload。调 reward：

```text
reward = 0.3 * r_nodes + 0.7 * r_candidates
```

或者新增结构惩罚：

```text
r_overlap =
    (internal_overlap_ref - internal_overlap_rl) / max(1, internal_overlap_ref)

reward =
    0.25 * r_nodes + 0.55 * r_candidates + 0.20 * r_overlap
```

但 `internal_overlap` 统计较慢，可以只在 reward interval 上算。

## 15.3 如果查询 candidate 降了，但 wall-clock 仍慢

这是可以接受的。说明结构质量提升了，但自实现 R-tree + RL/ref_tree 维护开销仍高于 Boost.Geometry。论文里可以报告：

```text
RLR_LITE_CS_SPLIT reduces MBR candidates / visited nodes,
but does not outperform the highly optimized Boost R-tree in wall-clock time.
```

这比强行说“复现 RLR 效果”更稳。

## 15.4 如果 insert throughput 太低

优先调这些：

```text
RLR_REWARD_INTERVAL=8192
RLR_SPLIT_QUERY_SAMPLE=8
RLR_SPLIT_MAX_CANDIDATES=48
RLR_SPLIT_STEP=2
```

这样会牺牲训练精细度，但能降低插入开销。

---

# 16. 结果汇报时的比较方式

最终表格至少放：

```text
Boost_Rtree
RLR_LITE_CS
RLR_LITE_CS_SPLIT
GLIN / DELI 系列
```

重点不是只看 `avg_query_ms`，还要看：

```text
mbr_candidates
exact_calls
visited_blocks / block_checks
insert_tps
delete_tps
index_mb_estimate
answers_match_boost
```

推荐汇报逻辑：

```text
1. RLR_LITE_CS 相比 Boost，候选数几乎不变，说明仅替换 ChooseSubtree 不足以优化 fat-object polygon workload。
2. RLR_LITE_CS_SPLIT 进一步将 split 纳入 RL-lite 决策，尝试降低节点 MBR overlap。
3. 如果 candidates 下降，说明 split 决策确实影响了 R-tree 剪枝质量。
4. 如果 wall-clock 仍不优于 Boost，原因是 Boost.Geometry 是高度优化实现，而 RLR_Lite 需要维护 RL tree + reference tree，并执行额外训练评估。
5. 因此该 baseline 的作用是提供 RL-enhanced R-tree 对照，而不是声称复现完整 RLR-Tree 或 2025 RL-RTree。
```

论文/汇报中建议写：

```text
We further implement RLR-Lite-CS-Split, a stronger RLR-inspired baseline that extends RLR-Lite-CS by applying a lightweight bandit policy to both ChooseSubtree and node split decisions. The query traversal remains identical to a standard R-tree, and deletion is implemented lazily. This baseline is intended to evaluate whether reinforcement learning can improve MBR organization for polygon workloads, rather than to reproduce the full RLR-Tree or RL-RTree systems.
```

中文：

```text
我们进一步实现了 RLR-Lite-CS-Split，在原有 RL ChooseSubtree 的基础上，将节点分裂也纳入轻量强化学习决策。该方法保持标准 R-tree 查询遍历，删除仍采用 lazy delete。该 baseline 的目的不是复现完整 RLR-Tree 或 2025 RL-RTree 的全部效果，而是检验强化学习是否能够改善复杂多边形 workload 下的 MBR 组织质量。
```

---

# 17. 最小完成标准

`RLR_LITE_CS_SPLIT` 完成后至少满足：

```text
1. 编译通过。
2. INDEXES="Boost_Rtree RLR_LITE_CS RLR_LITE_CS_SPLIT" 能跑通。
3. CHECK_CORRECTNESS=1 时没有 missing / extra。
4. debug 中 split_rl_count > 0。
5. debug 中 split_disagree_with_quadratic > 0。
6. summary 中 RLR_LITE_CS_SPLIT 有有效 avg_query_ms、mbr_candidates、exact_calls、insert_tps。
7. 和 RLR_LITE_CS 相比，至少在部分 workload 上 mbr_candidates 或 visited_blocks 出现差异。
```

做到这一步，就可以说你实现了一个 **ChooseSubtree + Split 两阶段轻量强化学习 R-tree baseline**。不要承诺它一定超过 Boost；它的实验价值在于把“仅 RL ChooseSubtree 是否足够”和“加入 RL Split 是否能降低 MBR overlap”这两个问题拆开验证。
