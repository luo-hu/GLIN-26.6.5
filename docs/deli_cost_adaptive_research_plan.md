# DELI-Cost 自适应动态空间学习索引研究计划

最后更新日期：2026-06-24

本文档用于整理 `DELI_ALEX_HYBRID_LOCAL_BOUNDED` 的下一步研究方向：把当前固定参数版本升级为一个 **cost-driven adaptive spatial index model**，中文可称为：

```text
DELI-Cost：代价驱动的自适应动态范围感知学习索引
```

核心目标不是继续手动试：

```text
BLOCK_SIZE = 512
LOCAL_DELTA_BOUND = 128 / 256 / 512
DELETE_COMPACT_FRACTION = 0.25
```

而是建立一套可以写进 VLDB 论文的方法体系：

```text
统一优化模型
+ 自适应 block 划分
+ block 级 beta / tau 自动选择
+ 收益-成本驱动 local compaction
+ 动态正确性不变式
+ mixed workload 稳定性实验
```

一句话判断：

> 这个方向可行，而且比继续调 `LOCAL_DELTA_BOUND` 更像论文创新；但要把 Bayesian Optimization 定位为外层校准器，而不是核心算法本身。核心贡献应该是代价模型和自适应维护机制。

---

## 1. 固定英文术语中文解释

### 1.1 `block size`：块大小

`block size` 中文叫 **块大小**。

在 DELI 中，所有对象按 `Zmin` 排序后，被划成一段一段连续区间。每一段就是一个 `block`，中文叫 **块** 或 **查询块**。

如果：

```text
block_size = 512
```

意思是：

```text
一个 block 里大约放 512 条主区记录。
```

当前固定块大小只是工程简化。理论版不必固定，可以定义：

```text
b_min <= |B| <= b_max
```

也就是每个 block 的大小可以不同，由代价模型自动决定。

### 1.2 `compact`：紧凑主区

`compact` 在这里不是压缩文件，而是指 **已经整理好的主区**。

一个 block 可以理解成：

```text
Block B
  compact part: 主区，按 Zmin 排好序，查询扫描快
  delta part: 增量缓存，新插入对象先放这里
  tombstone: 墓碑记录，被删除但还没有物理清理的记录
```

### 1.3 `compaction`：局部压实 / 局部整理

`compaction` 中文叫 **压实** 或 **局部整理**。

它做四件事：

```text
1. 删除 tombstone 记录。
2. 把 delta 里的新记录合并进 compact 主区。
3. 重新按 Zmin 排序。
4. 重新计算 block summary。
```

这里的重点是 **local compaction**，也就是只整理被污染的一个 block，不全局重建整个索引。

### 1.4 `delta buffer`：增量缓存

`delta buffer` 中文叫 **增量缓存**。

插入新对象时，不马上把它插入 compact 主区中间，而是先追加到目标 block 的局部缓存：

```text
insert object g
-> find target block B by Zmin
-> append g to local delta Delta_B
```

这样插入更快，但查询时要额外扫描 delta。

### 1.5 `beta`：局部增量比例

`beta` 写作 `β`，中文叫 **局部增量比例**。

定义：

```text
beta_B = max_delta_size(B) / compact_size(B)
```

如果：

```text
compact_size(B) = 512
beta_B = 0.25
```

那么这个 block 最多允许：

```text
512 * 0.25 = 128
```

条 delta 记录。

直观含义：

```text
beta 大：插入快，compaction 少，但查询要多扫 delta。
beta 小：查询干净，但插入更容易触发 compaction。
```

### 1.6 `tau`：墓碑比例 / 延迟删除比例

`tau` 写作 `τ`，中文叫 **墓碑比例** 或 **延迟删除比例**。

删除对象时，不马上物理删除，而是先标记：

```text
alive = false
```

这个被逻辑删除但仍留在 block 里的记录叫：

```text
tombstone
墓碑记录
```

定义：

```text
tau_B = max_tombstone_size(B) / compact_size(B)
```

如果：

```text
compact_size(B) = 512
tau_B = 0.25
```

那么这个 block 最多允许约 128 条 tombstone。

直观含义：

```text
tau 大：删除快，但查询会扫到更多无效记录。
tau 小：查询更干净，但删除更容易触发 compaction。
```

### 1.7 `block summary`：块摘要

`block summary` 中文叫 **块摘要**，可以理解成贴在 block 外面的标签。

典型字段：

```text
min_zmin
max_zmin
max_zmax
block_mbr
live_count
delta_count
tombstone_count
```

查询时先看摘要。如果摘要已经说明这个 block 不可能有答案，就直接跳过。

### 1.8 `one-sided conservative invariant`：单侧保守不变式

这是 DELI 动态正确性的核心。

中文解释：

```text
块摘要可以偏大，不能偏小。
```

例如：

```text
stored max_zmax >= true max_zmax
stored block_mbr 包含 true block_mbr
stored min_zmin <= true min_zmin
```

这样做的结果是：

```text
自适应调整只会影响性能，不会影响正确性。
```

即使 beta 很大、tau 很大、block 很粗，最多只是多扫描候选，不会漏答案。

---

## 2. Idea-Evaluator 评估结果

### 2.1 第一印象

论文类型：

```text
Technique Paper，以 New Problem/Setting framing 做加强。
```

一句话故事：

> DELI-Cost 将 learned spatial index 从固定参数的动态工程实现，升级为一个代价驱动的自适应空间索引框架，使复杂 geometry 的 exact spatial predicate 查询在动态插入、删除和 mixed workload 下仍能保持安全剪枝和受控维护成本。

### 2.2 Fatal flaws 审查

| # | 风险 | 严重程度 | 防守方式 |
|---|---|---|---|
| 1 | 范围过大：如果同时承诺 exact polygon query、learned index、ALEX、BO、自适应分区、动态证明、系统实现，容易变成一锅粥 | MAJOR | 主线收窄成 `DELI-Cost-Lite`，BO 只作为可选校准，不作为核心贡献 |
| 2 | 被认为只是调参：如果写成“用 BO 找最佳参数”，贡献会显得弱 | MAJOR | 写成“统一代价模型 + 闭式阈值 + 自适应分区 + benefit-cost compaction”，BO 只校准少数高层权重 |

没有不可修复的 CRITICAL fatal flaw，因此不需要 pivot。

### 2.3 五维评分

| 维度 | 分数 | 依据 | 如何加强 |
|---|---:|---|---|
| Higher，有效性 | 7 | `Zmin/Zmax + block MBR + exact refinement` 已经在查询侧减少 false positives | 加入 adaptive block partition，证明剪枝效率进一步提升 |
| Faster，效率 | 8 | LocalBounded 已经改善写入，DELI-Cost 进一步减少不必要 compaction | mixed workload 中展示 query/update 综合吞吐 |
| Stronger，鲁棒性 | 8 | 自适应 beta/tau 和 benefit-cost compaction 能应对读写比例变化 | 加负载漂移实验，证明固定参数会偏，而自适应更稳 |
| Cheaper，成本 | 7 | 减少人工调参，减少全局 rebuild | 报告 calibration overhead 和 maintenance_ns |
| Broader，泛化/统一 | 8 | 把 spatial extent、learned layout、dynamic maintenance 统一到一个 cost model | 和 R-tree、Quadtree、GLIN-piece、DELI fixed variants 做统一框架对比 |

评价：

```text
Accept with Revisions
```

不是因为方向弱，而是因为必须把范围收窄。最推荐先做：

```text
DELI-Cost-Lite
```

再决定是否做：

```text
DELI-Cost-Bayes
```

### 2.4 范式转变潜力

| Probe | 判断 | 理由 |
|---|---|---|
| First Principles | Yes | 挑战了 learned spatial index 只把复杂对象压成 point-like key 的隐含假设 |
| Elephant in the Room | Yes | 复杂 geometry + exact predicate + learned index + dynamic update 的组合很少被完整处理 |
| Technology Cycle | Partial | learned indexes 和 self-tuning systems 已成熟，但空间复杂几何场景还未充分融合 |
| Hamming's Rule | Partial | 如果做成，会给 learned spatial index 提供一条从查询优化走向动态系统的路径 |

结论：

```text
有 disruptive seed，但论文应以稳健技术贡献为主，不要过度宣传为范式革命。
```

---

## 3. 推荐研究定位

不要写成：

```text
We use Bayesian Optimization to tune DELI parameters.
```

建议写成：

```text
We propose DELI-Cost, a cost-driven adaptive dynamic learned spatial index for exact spatial relationship queries over complex geometries.
```

中文：

```text
我们提出 DELI-Cost，一个面向复杂几何精确空间关系查询的代价驱动自适应动态学习型空间索引。
```

核心区别：

| 弱故事 | 强故事 |
|---|---|
| 用 BO 调参数 | 用代价模型推导维护策略 |
| 试几个 block size | 用 DP 或近似 DP 自适应划分 block |
| 固定 delta bound | 每个 block 根据读写热度自动选 beta |
| 固定 delete fraction | 每个 block 根据删除热度和查询代价自动选 tau |
| delta 满了就整理 | 未来收益大于整理成本才 compaction |

---

## 4. 统一优化模型

定义 DELI 配置：

```text
Theta = (P, {beta_B}, {tau_B})
```

中文解释：

```text
Theta: 整个索引配置
P: block 划分方式
beta_B: block B 的局部增量比例
tau_B: block B 的墓碑比例
```

目标函数：

```text
J(Theta)
= w_q * E[T_query(Theta)]
+ w_i * E[T_insert(Theta)]
+ w_d * E[T_delete(Theta)]
+ w_m * E[T_maintenance(Theta)]
+ w_s * Space(Theta)
```

中文解释：

```text
w_q: 查询权重
w_i: 插入权重
w_d: 删除权重
w_m: 维护权重
w_s: 空间权重
```

对于 mixed workload，可以直接由操作比例给出初始权重：

```text
read-heavy:  w_q=0.90, w_i=0.05, w_d=0.05
balanced:    w_q=0.70, w_i=0.15, w_d=0.15
write-heavy: w_q=0.50, w_i=0.25, w_d=0.25
```

这一步很重要：

```text
没有 workload 权重，就不存在唯一最佳参数。
```

所谓“最佳”，必须是相对于查询、插入、删除、维护和空间开销的综合目标。

---

## 5. 自适应 block 划分

### 5.1 为什么 block size 不应该永远固定

固定 `block_size=512` 的问题是：

```text
密集复杂区域可能需要更小 block，提升剪枝精度。
稀疏简单区域可以用更大 block，减少元数据和维护成本。
```

block 太大：

```text
Z 区间跨度变大；
max_zmax 更容易被大对象拉高；
block_mbr 更容易变宽；
查询更难剪枝；
扫描候选更多。
```

block 太小：

```text
block 数量变多；
summary 检查次数变多；
metadata 内存增加；
插入和 compaction 的管理开销变大。
```

所以 block 划分本质是在：

```text
剪枝精度 vs 元数据/维护开销
```

之间找平衡。

### 5.2 DP 自适应划分模型

对象按 `Zmin` 排序后，block 必须是连续区间。

令：

```text
Cost(i, j)
```

表示把第 `i` 到第 `j` 条记录划成一个 block 的代价。

动态规划：

```text
DP[j] = min_i { DP[i-1] + Cost(i, j) + split_penalty }
```

约束：

```text
b_min <= j - i + 1 <= b_max
```

### 5.3 Cost(i, j) 应该包含什么

建议第一版包含：

```text
1. query_hit_cost: 校准查询会访问这个 block 的次数。
2. scan_cost: 估计会扫描多少 compact/delta/tombstone 记录。
3. exact_cost: 估计 GEOS exact predicate 次数。
4. metadata_cost: 多一个 block 带来的 summary 内存和检查成本。
5. maintenance_cost: 这个 block 未来 insert/delete/compaction 的成本。
```

可以用校准查询集 `Q_cal` 估计：

```text
Cost_query(i, j)
= sum over q in Q_cal of I(B_ij, q) * estimated_scan_cost(B_ij, q)
```

其中 `I(B_ij, q)=1` 表示这个 block 无法被 query q 剪枝掉。

### 5.4 Theorem 1 草案

**Theorem 1，连续 block 划分最优性。**

在以下条件下：

```text
1. records 已按 Zmin 排序；
2. 每个 block 必须是连续 Zmin 区间；
3. 总代价是各 block 代价之和；
4. 每个 block 满足 b_min <= |B| <= b_max；
```

动态规划：

```text
DP[j] = min_i { DP[i-1] + Cost(i, j) }
```

可以找到全局最优的连续 block partition。

证明思路：

```text
最优划分的最后一个 block 必然是某个连续区间 [i, j]。
其前缀 [1, i-1] 必须也是最优划分，否则替换成更优前缀会得到更低总代价，矛盾。
因此满足最优子结构，DP 正确。
```

---

## 6. beta / tau 的闭式选择

### 6.1 beta 的代价模型

对 block B，beta 控制 delta 大小。

可以写：

```text
J_B(beta) = A_B * beta + B_B / beta
```

中文解释：

```text
A_B * beta:
  beta 越大，delta 越大，查询额外扫描越多。

B_B / beta:
  beta 越小，delta 越快满，compaction 越频繁，维护越贵。
```

其中：

```text
A_B 与 block 的 query_hits、scan_cost 有关。
B_B 与 insert_hits、local_compaction_cost 有关。
```

最优解：

```text
beta*_B = sqrt(B_B / A_B)
```

再裁剪到安全范围：

```text
beta_B = clip(beta*_B, beta_min, beta_max)
```

例如：

```text
beta_min = 0.05
beta_max = 0.50
```

这不是手调，而是工程安全边界。

### 6.2 tau 的代价模型

tau 控制 tombstone 数量。

同理：

```text
J_B(tau) = C_B * tau + D_B / tau
```

中文解释：

```text
C_B * tau:
  tau 越大，查询扫到的 dead records 越多。

D_B / tau:
  tau 越小，删除越容易触发 compaction，维护越贵。
```

最优解：

```text
tau*_B = sqrt(D_B / C_B)
tau_B = clip(tau*_B, tau_min, tau_max)
```

### 6.3 Theorem 2 草案

**Theorem 2，block 级维护预算的闭式最优解。**

对于：

```text
J(x) = A*x + B/x
```

当：

```text
A > 0, B > 0, x > 0
```

有：

```text
J'(x) = A - B/x^2
```

令 `J'(x)=0`，得到：

```text
x* = sqrt(B/A)
```

且：

```text
J''(x) = 2B/x^3 > 0
```

因此 `x*` 是唯一全局最优解。

对应到 DELI：

```text
beta*_B = sqrt(B_B / A_B)
tau*_B  = sqrt(D_B / C_B)
```

### 6.4 推论：平摊维护代价有界

如果每个 block 在 delta 达到 `beta_B * |B|` 时做 local compaction，则一次 compaction 成本约为：

```text
O(|B| + |Delta_B|)
```

摊还到 `beta_B * |B|` 次插入上：

```text
amortized insert maintenance cost
= O((|B| + beta_B |B|) / (beta_B |B|))
= O(1 / beta_B)
```

当 `beta_B` 被裁剪到：

```text
beta_B >= beta_min > 0
```

则平摊维护成本是常数级上界：

```text
O(1 / beta_min)
```

tau 同理。

这个推论很重要，因为它把“参数自动选择”和“维护成本有界”连接起来。

---

## 7. Benefit-Cost Local Compaction

当前规则是：

```text
delta 超过阈值就 compaction
tombstone 超过阈值就 compaction
```

DELI-Cost 可以升级成：

```text
如果未来查询省下的代价 >= 当前 compaction 代价，就 compaction。
```

对 delta：

```text
Benefit_delta(B)
= query_rate(B) * horizon * scan_cost_per_record * |Delta_B|
```

对 tombstone：

```text
Benefit_tombstone(B)
= query_rate(B) * horizon * scan_cost_per_record * |T_B|
```

compaction 成本：

```text
Cost_compact(B)
= compact_cost_per_record * (|C_B| + |Delta_B|)
```

触发规则：

```text
if Benefit_delta(B) + Benefit_tombstone(B) >= Cost_compact(B):
    compact(B)
```

中文解释：

```text
如果这个 block 很热，未来会被查很多次，那么早点整理是值得的。
如果这个 block 很冷，就先不整理，避免把维护成本浪费在冷区。
```

### Theorem 3 草案

**Theorem 3，收益-成本触发的局部代价单调不升。**

若 `compact(B)` 只在：

```text
EstimatedBenefit(B) >= EstimatedCost(B)
```

时执行，则在估计模型无偏或误差有界的情况下，执行 compaction 不会提高预测窗口内的期望总代价。

证明思路：

```text
执行前成本 = future_query_overhead_without_compaction
执行后成本 = compaction_cost + future_query_overhead_after_compaction

EstimatedBenefit = 两者的未来查询开销差值。
当 EstimatedBenefit >= EstimatedCost 时，执行后预测成本不高于执行前。
```

这个定理可以先写成 model-level guarantee，不要过度承诺真实系统一定单调，因为真实查询分布会漂移。

---

## 8. Bayesian Optimization 应该怎么用

你提到的 Limbo 是 C++11 的 Bayesian Optimization / Gaussian Process 库，适合优化梯度未知、单次评估昂贵的函数。它可以借鉴，但不建议现在直接把 Limbo 接进主代码。

原因：

```text
1. 直接 BO 搜每个 block 的 block size、beta、tau 会变成黑盒调参。
2. 参数维度太高，评估成本高。
3. 审稿人会问：为什么不是你手调的另一种形式？
```

推荐定位：

```text
DELI-Cost-Lite:
  核心算法，确定性代价模型，闭式 beta/tau，DP block partition。

DELI-Cost-Bayes:
  可选扩展，只校准少数高层权重。
```

BO 只优化少数全局参数，例如：

```text
w_q, w_i, w_d, w_m, w_s
lambda_split
lambda_merge
horizon
scan_cost_per_record
compact_cost_per_record
```

不要让 BO 优化每个 block 的细粒度参数。

论文里可以写：

```text
Bayesian calibration is used only to tune a small set of workload-level weights; all per-block decisions are produced by the cost model.
```

---

## 9. 技术型论文结构，tech-paper-template 版本

### 9.1 Paper type

```text
Technique Paper
```

理由：

```text
核心贡献是一个新的索引机制和动态维护算法，而不是一个纯 benchmark。
```

### 9.2 Thinking template

| Stage | 内容 |
|---|---|
| Research background | 复杂 polygon、linestring、rectangle 等 geospatial objects 在 GIS 中常见；传统 R-tree 支持复杂 geometry 和动态更新，但不是 learned；learned spatial indexes 追求低内存和高吞吐，但对复杂 geometry 的 extent 和动态维护支持不足。 |
| Limitation 1 | 现有 learned spatial index 常将复杂 geometry 压成 `Zmin` 等 point-like key，存在 extent blindness。 |
| Limitation 2 | 现有动态 learned index 主要面向一维 key，对 exact spatial predicate query 所需的 `Zmax`、MBR 和 conservative summaries 缺乏动态维护模型。 |
| Limitation 3 | 当前固定阈值的 delta/tombstone 维护容易依赖手调参数，在读写比例变化时不稳定。 |
| Key Idea | 将复杂 geometry 表示为 extent entry，并用代价模型自适应决定 block 划分、delta 预算、tombstone 预算和 local compaction 时机。 |
| Challenge 1 | 如何在自适应 block 划分后仍保证 exact query 不漏答案。 |
| Challenge 2 | 如何避免 beta/tau 变成人工调参，并让维护代价有理论上界。 |
| Challenge 3 | 如何在 mixed workload 和 workload drift 下保持查询、插入、删除性能稳定。 |
| Module A | 单侧保守不变式：所有自适应操作只改变性能，不改变安全剪枝正确性。 |
| Module B | 代价驱动自适应维护：DP block partition、闭式 beta/tau、benefit-cost compaction。 |
| Module C | 动态 mixed workload 执行和诊断：记录 query drift、tail latency、local compaction、delta/tombstone 控制效果。 |
| Contribution 1 | 提出 DELI-Cost 的 extent-aware learned spatial index 抽象和动态正确性不变式。 |
| Contribution 2 | 提出代价驱动自适应 block partition 和 block 级 beta/tau 闭式选择规则。 |
| Contribution 3 | 提出收益-成本驱动的 local compaction，并证明其预测窗口下的维护代价合理性。 |
| Contribution 4 | 在真实和合成 complex geometry workload 上，与 Boost R-tree、GEOS Quadtree、GLIN-piece 和固定参数 DELI variants 做 query/update/mixed 对比。 |

### 9.3 自一致性检查

```text
Limitations -> Key Idea: pass
Key Idea -> Challenges: pass
Challenges -> Methodology: pass
Methodology -> Contributions: pass
```

需要注意：

```text
如果实际代码只完成 DELI-Cost-Lite，不要在贡献里承诺 BO 或在线学习。
BO 最多写 optional calibration 或 future work。
```

---

## 10. Introduction 草稿结构，intro-drafter 版本

### Paragraph 1：背景与动机

目的：

```text
说明复杂 geospatial objects 的 exact spatial predicate query 很重要。
```

建议 running example：

```text
道路和公园数据上的 Intersects 查询。
例如：查询某个规划区域内相交的 road segments 和 park polygons。
对象不是点，而是长线和复杂 polygon，具有较大的 Z-order extent。
```

要点：

```text
1. GIS 系统需要在复杂 geometry 上支持 exact intersects。
2. 查询对象可能是 polygon window，数据对象可能是 roads/parks/AW/LW 等复杂形状。
3. 只用点 key 表示会隐藏对象范围。
```

### Paragraph 2：现有工作限制

限制最多三个：

```text
1. 传统 R-tree/Quadtree 支持复杂 geometry 和动态更新，但不是 learned，内存和 scan locality 不是为 learned layout 设计。
2. 多数 learned spatial index 更擅长点数据、range query 或近似过滤，对 polygon exact predicate 支持不足。
3. GLIN/ALEX-style 方法虽然具备 learned layout 和动态更新潜力，但 `Zmin` 中心设计对 `Zmax`、MBR 和动态 extent summaries 利用不足。
```

### Paragraph 3：问题本质与目标

目标句候选：

```text
Our goal is to build an update-aware learned spatial index that treats each complex geometry as a spatial extent rather than a point-like key, while preserving exact-query correctness under adaptive dynamic maintenance.
```

中文：

```text
我们的目标是构建一个更新感知的学习型空间索引，它把复杂几何对象作为空间范围而不是点状 key 来维护，并在自适应动态维护过程中保持精确查询正确性。
```

### Paragraph 4：关键挑战

```text
Challenge 1: Extent-aware pruning must remain false-negative-free under insert/delete/compaction.
Challenge 2: Dynamic maintenance must avoid fixed thresholds that only work for one workload.
Challenge 3: Query/update trade-offs must remain stable under mixed and drifting workloads.
```

### Paragraph 5：方法概览

```text
DELI-Cost represents every object as an extent entry <Zmin, Zmax, MBR, geometry>.
It maintains one-sided conservative block summaries for safe pruning.
It uses a cost model to adapt block partition, beta/tau budgets, and local compaction decisions.
It evaluates the design under staged and interleaved mixed workloads.
```

### Paragraph 6：贡献

1. 提出 `DELI-Cost` 的 extent entry 抽象和单侧保守不变式，支持复杂 geometry exact predicate 查询下的安全剪枝。
2. 提出自适应 block 划分和 block 级 `beta/tau` 闭式选择规则，减少人工调参。
3. 提出收益-成本驱动的 local compaction，在查询额外扫描和维护成本之间自动平衡。
4. 在真实与合成数据、不同 selectivity、不同读写混合负载下评估 DELI-Cost，并与 Boost R-tree、GEOS Quadtree、GLIN-piece 和固定参数 DELI variants 对比。

---

## 11. Benchmark 设计，benchmark-paper-template 思维

这篇文章不建议写成纯 benchmark paper，但实验章节要用 benchmark paper 的严谨结构。

### 11.1 Research Questions

建议 RQ：

```text
RQ1: DELI-Cost 是否保持 exact correctness？
RQ2: 自适应 block partition 是否比固定 block size 有更好的剪枝效率？
RQ3: 自适应 beta/tau 是否比固定阈值在不同 mixed workload 下更稳定？
RQ4: DELI-Cost 和 Boost R-tree、GEOS Quadtree、GLIN-piece 相比，query/update/memory trade-off 如何？
RQ5: 在 workload drift 下，DELI-Cost 是否能避免固定参数版本的性能崩坏？
```

### 11.2 Baselines

必须保留：

```text
Boost R-tree
GEOS Quadtree
GLIN-piece
DELI-ALEX-Hybrid
DELI-ALEX-Hybrid-Buf
DELI-ALEX-Hybrid-LocalBounded fixed
DELI-Cost-Lite
```

可选：

```text
DELI-Cost-Bayes
```

### 11.3 Workloads

Staged workload 用于机制隔离：

```text
bulk-load 50%
checkpoint query
insert 20%
checkpoint query
delete 10%
checkpoint query
```

Mixed workload 用于真实动态稳定性：

```text
read-heavy:  90% query, 5% insert, 5% delete
balanced:    70% query, 15% insert, 15% delete
write-heavy: 50% query, 25% insert, 25% delete
```

Drift workload 用于证明自适应价值：

```text
phase 1: read-heavy
phase 2: write-heavy
phase 3: read-heavy again
```

重点看固定参数是否在某个阶段偏掉，以及 DELI-Cost 是否自动回到较好 trade-off。

### 11.4 指标

正确性：

```text
answers_match_boost
missing_count
extra_count
```

查询：

```text
avg_query_ms
p95_query_ms
p99_query_ms
candidate_answer_ratio
exact_calls
records_scanned
blocks_visited
blocks_pruned
```

更新：

```text
insert_tps
delete_tps
p95_insert_ms
p99_insert_ms
p95_delete_ms
p99_delete_ms
```

维护：

```text
local_compaction_count
local_compaction_ns
avg_local_delta_size
max_local_delta_size
tombstone_ratio
block_split_count
```

空间：

```text
index_mb_estimate
records_memory
summary_memory
delta_memory
metadata_memory
```

综合：

```text
query_tps
overall_ops_tps
foreground_time
maintenance_time
```

---

## 12. 论文图表设计，figure-designer 版本

### Figure 1：Motivated Example

类型：

```text
Running Example plus Failure Case
```

布局：

```text
Panel A: 一个真实 query polygon，覆盖 roads/parks 中一片区域。
Panel B: Zmin-only learned index 把长线/大 polygon 当成点 key，导致必须 query augmentation 或扫大量候选。
Panel C: DELI-Cost 用 <Zmin, Zmax, MBR> extent entry 和 block summary 安全跳过无关 block。
```

核心信息：

```text
复杂对象不是点；extent blindness 会造成候选膨胀；DELI 直接维护 extent。
```

工具：

```text
PowerPoint 初稿，Figma 或 draw.io 精修，导出 PDF。
```

### Figure 2：DELI-Cost Overview

类型：

```text
System Architecture / Multi-layer
```

布局：

```text
Offline / calibration layer:
  load records -> compute extent entries -> adaptive block partition

Online update layer:
  insert -> local delta
  delete -> tombstone
  benefit-cost compaction

Query layer:
  block summary pruning -> record pruning -> GEOS exact refinement
```

### Figure 3：Block 内部结构图

内容：

```text
compact records
local delta
tombstone
block summary
local compaction before/after
```

这张图专门解释 beta、tau 和 compaction。

### Figure 4：自适应 block 划分图

内容：

```text
按 Zmin 排序的 records；
密集/复杂区域切小 block；
稀疏/简单区域切大 block；
标注 Cost(i,j)。
```

### Figure 5：beta/tau 自动选择图

类型：

```text
曲线图
```

内容：

```text
J(beta) = A beta + B / beta
查询代价随 beta 增大而升高
维护代价随 beta 增大而降低
总成本在 sqrt(B/A) 处最小
```

tau 同理，可以放在同图右侧。

### Figure 6：Mixed Workload Query Drift

类型：

```text
line chart
```

横轴：

```text
operation count / checkpoint
```

纵轴：

```text
avg query latency 或 p95 query latency
```

曲线：

```text
fixed small threshold
fixed large threshold
DELI-Cost-Lite
Boost R-tree
GLIN-piece
```

核心信息：

```text
固定参数在 workload drift 下偏掉，自适应版本波动更小。
```

### Figure 7：Query/Update Trade-off

类型：

```text
scatter plot
```

横轴：

```text
update throughput
```

纵轴：

```text
query latency，越低越好
```

可以用反向 y 轴或标注 lower is better。

### Figure 8：Memory Breakdown

类型：

```text
stacked bar
```

分段：

```text
main records
block summaries
delta
tombstone metadata
ALEX/auxiliary structures
```

目的：

```text
说明内存估计不是一个黑箱数。
```

---

## 13. Vibe Research Workflow

### 13.1 六条行为规则

1. AI 可以辅助文献整理、代码实现、debug、语言润色。
2. 研究想法、问题定义、技术路线、实验设计、核心结论必须由你自己理解和决定。
3. AI 生成的代码和文字都必须逐行或逐句验证。
4. 不允许 AI 编造引用，所有引用必须由你自己在 DBLP、Google Scholar、arXiv 或论文原文确认。
5. 不允许伪造数据、结果或实验流程。
6. 遵守学校和投稿会议关于 AI 使用的披露要求。

### 13.2 6 周执行计划

| 周次 | 阶段 | 任务 | 产物 | 工具 |
|---|---|---|---|---|
| Week 1 | Writing + Design | 锁定 DELI-Cost-Lite 理论规格，写清楚术语、目标函数、Lemma/Theorem | `docs/deli_cost_theory_spec.md` | Codex + 你手动审阅 |
| Week 2 | Coding | 给当前 dynamic benchmark 增加 per-block 统计：query_hits、insert_hits、delete_hits、delta/tombstone | CSV 新字段 + smoke test | Codex |
| Week 3 | Coding | 实现自适应 beta/tau，先不做 DP block partition | `DELI_COST_LITE` 初版 | Codex |
| Week 4 | Coding | 实现简化 DP block partition，候选切分点按 64 或 128 步长枚举 | adaptive partition ablation | Codex |
| Week 5 | Experiment | 跑 staged、mixed、drift workload，对比 fixed vs adaptive | results + figures | shell + Matplotlib |
| Week 6 | Writing + Figure | 写 Introduction、Method、Experiment skeleton，画 Figure 1/2/3 | LaTeX 初稿 + 图表草稿 | Overleaf/本地 LaTeX + draw.io/Figma |

### 13.3 什么时候再做 Bayesian Optimization

先不接 Limbo。

只有当 DELI-Cost-Lite 已经跑通，并且出现下面情况时再考虑：

```text
1. 高层权重明显影响结果；
2. 默认权重在多个数据集上不稳定；
3. 用少量 calibration queries 可以显著改善结果；
```

否则 BO 放 future work 更稳。

---

## 14. 近期代码任务建议

最小可行版本：

```text
DELI-Cost-Lite v1
```

只做四件事：

1. 在 `DELI_ALEX_HYBRID_LOCAL_BOUNDED` 里记录 block 级热度：

```text
query_hits
insert_hits
delete_hits
delta_size
tombstone_count
local_compaction_ns
```

2. 用热度估计每个 block 的：

```text
A_B, B_B, C_B, D_B
```

3. 自动计算：

```text
beta_B = clip(sqrt(B_B / A_B), beta_min, beta_max)
tau_B  = clip(sqrt(D_B / C_B), tau_min, tau_max)
```

4. 把 fixed threshold 改成 per-block threshold：

```text
local_delta_bound(B) = ceil(beta_B * compact_size(B))
delete_compact_bound(B) = ceil(tau_B * compact_size(B))
```

第二阶段再做：

```text
adaptive block partition
benefit-cost compaction
workload drift adaptation
```

---

## 15. 最终建议

这个方向值得做，但建议按下面的边界推进：

```text
主线：DELI-Cost-Lite
  deterministic cost model
  adaptive block partition
  closed-form beta/tau
  benefit-cost local compaction
  exact correctness invariant

支线：DELI-Cost-Bayes
  only calibrates high-level weights
  optional or future work
```

不要把论文写成：

```text
我们用了 Bayesian Optimization 自动调参数。
```

要写成：

```text
我们提出了一个代价驱动的自适应动态空间学习索引模型。
Bayesian Optimization 只是可选的外层权重校准工具。
```

最关键的实验不是单点性能，而是：

```text
在 staged workload 中证明机制正确；
在 mixed workload 中证明真实动态稳定性；
在 workload drift 中证明自适应比固定参数更稳。
```

如果 DELI-Cost-Lite 能做到：

```text
1. answers_match_boost 始终为 1；
2. 查询接近 Boost R-tree；
3. 更新吞吐高于 Boost R-tree；
4. 内存低于或接近 R-tree；
5. workload drift 下比固定参数版本更稳；
```

那么它就比当前 `DELI_ALEX_HYBRID_LOCAL_BOUNDED` 更像一个完整 VLDB 级系统贡献。

---

## 16. 参考线索

本文档没有把以下工作当作已完整验证的 related work，只作为后续文献梳理入口：

- Limbo: C++11 Bayesian Optimization / Gaussian Process library，用于昂贵黑盒函数优化。适合作为 DELI-Cost-Bayes 的高层权重校准参考。
- ALEX: Updatable Adaptive Learned Index，说明 learned index 可以支持动态更新，但主要面向一维 key。
- Waffle / QUASII / workload-aware spatial indexing：可作为自适应空间索引和动态 workload 相关工作入口。
- R-tree / Quadtree / LSM-R-tree：作为传统空间索引和动态维护 baseline。
- GLIN / learned spatial index / learned space-filling curve：作为 learned spatial index 相关工作入口。

