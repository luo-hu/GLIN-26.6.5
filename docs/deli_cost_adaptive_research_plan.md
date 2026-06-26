# DELI-Cost 自适应动态空间学习索引研究计划

最后更新日期：2026-06-25

本文档用于整理当前论文主线：以 `DELI_ALEX_HYBRID_COST` 作为主方法，把固定参数版本 `DELI_ALEX_HYBRID_LOCAL_BOUNDED` 定位为消融基线，并将 PRL 作为可叠加的 predicate-aware refinement layer。整体方向是一个 **cost-driven adaptive spatial index model**，中文可称为：

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
+ predicate-aware refinement layer
+ 动态正确性不变式
+ mixed workload 稳定性实验
```

一句话判断：

> 这个方向可行，而且比继续调 `LOCAL_DELTA_BOUND` 更像论文创新；但要把 Bayesian Optimization 定位为外层校准器，而不是核心算法本身。核心贡献应该是代价模型、自适应维护机制和保守谓词精化层。论文叙事上，DELI-Cost 是主方法，DELI-LB 是固定参数基线，PRL 是可叠加层。

## 0. 2026-06-25 最终论文主线决策

2026-06-25 进一步收敛后的最终主线：

```text
DELI-PRL is a cost-driven predicate execution framework for exact spatial
relationship queries under dynamic updates.
```

中文：

```text
DELI-PRL 是一个面向动态更新下精确空间关系查询的
代价驱动保守谓词执行框架。
```

这比只写：

```text
DELI-Cost 做自适应 block / beta / tau；
DELI-PRL 做 GEOS shortcut。
```

更强。原因是它把所有模块统一成一个优化问题：

```text
给定数据 D、查询负载 Q、保守谓词层级 P 和维护策略 theta，
选择最优谓词执行路径 pi 与结构维护策略 theta，
最小化 query + exact refinement + update maintenance + memory 的总期望代价。
```

因此论文里不要把 `block size`、`beta`、`tau`、`DP`、`BO` 写成互相独立的模块，而要写成：

```text
它们都是同一个 predicate execution policy 的决策变量或求解器。
```

最终统一变量：

```text
theta = (partition, beta policy, tau policy, compaction policy)
pi    = predicate ordering / predicate execution path
```

模块定位：

| 模块 | 在统一模型中的角色 |
|---|---|
| adaptive block partition / DP | 优化结构粒度，降低 block checks 和 record scan |
| beta policy | 控制局部 delta 预算，平衡 insert 与 query scan |
| tau policy | 控制 tombstone 保留预算，平衡 delete 与 query scan |
| benefit-cost compaction | 局部维护操作选择 |
| PRL conservative predicates | 降低 GEOS entry rate |
| Bayesian Optimization | 可选的高层代价权重校准器，不是核心算法 |

最小公理集：

```text
Axiom 1, Conservative correctness:
  拒绝型谓词不能拒绝真答案；
  接受型谓词不能接受假答案。

Axiom 2, Measurable cost:
  每个谓词、扫描操作和维护操作都有可估计成本；
  每个谓词的判定率可以由 workload statistics 估计。
```

所有算法和定理都从这两个公理出发。这样论文的核心不再是“很多工程优化”，而是：

```text
Spatial query processing is a multi-stage conservative predicate filtering
problem under cost constraints.
```

中文：

```text
空间查询处理是在成本约束下的多阶段保守谓词过滤问题。
```

当前论文不要写成：

```text
我们发现了一个矩形包含 shortcut，所以查询变快。
```

也不要写成：

```text
我们用 Bayesian Optimization 自动调参数。
```

更强、更稳的主线应写成：

```text
DELI 是一个面向复杂几何 exact spatial predicate 的动态 learned access path。
它由两层机制组成：

1. Structural layer，结构层：
   extent entry
   one-sided conservative block summaries
   adaptive block partition
   beta/tau local bounded maintenance
   benefit-cost local compaction

2. Predicate layer，谓词层：
   Conservative Predicate Hierarchy Refinement Layer，简称 DELI-PRL
   用拒绝型和接受型保守谓词减少 GEOS exact calls
   所有 shortcuts 必须保持 exact correctness
```

论文类型：

```text
Technique Paper，带 New Setting framing。
```

一句话故事：

```text
DELI treats each complex geometry as a dynamic spatial extent and
optimizes a conservative predicate execution policy that jointly controls
structural scanning, dynamic maintenance, and exact predicate refinement.
```

中文：

```text
DELI 把复杂几何对象作为动态空间范围维护，并优化一套保守谓词执行策略，
同时控制结构扫描、动态维护和精确谓词精化成本。
```

当前最重要的边界：

```text
1. Cost-DP 是结构层贡献，主要减少 block_checks / visited_blocks / compact_records_scanned。
2. PRL 是谓词层贡献，主要减少 exact_calls / mbr_candidates。
3. 二者不是互相替代，而是解决不同阶段的成本。
4. 当前矩形 containment shortcut 只是 PRL 的一个安全实例，不要宣称它天然适用于任意 polygon query。
5. centroid / area / type 不能作为 intersects hard pruning，只能作为选择性估计或诊断特征。
6. CHECK_CORRECTNESS=0 的实验只能说明性能趋势，不能声明 correctness。
```

建议在 `main.tex` 中采用的章节主线：

```text
1 Introduction
2 Background and Motivation
3 Problem Definition and Design Goals
4 Unified Cost-driven Predicate Execution Model
5 DELI Overview
6 Query Processing and DELI-PRL
7 Dynamic Maintenance
8 Correctness
9 Cost Model and Adaptive Policies
10 Implementation and Experiments
11 Related Work
12 Discussion
```

最关键的实验消融：

```text
LocalBounded
Cost
LocalBounded + PRL
Cost + PRL

通过 PREDICATE_SHORTCUTS=0/1 隔离 PRL；
通过 LOCAL_BOUNDED vs COST 隔离 structural adaptive partition。
```

2026-06-25 更新：

最新实验表明，DP 自适应 block 划分确实降低了 `block_checks`、`visited_blocks` 和 `compact_records_scanned`，但没有显著降低 `mbr_candidates` 和 `exact_calls`。当前查询瓶颈已经从 index scan 层转移到 GEOS exact refinement 层。

因此 DELI-Cost 的论文主线应升级为两层模型：

```text
Structural layer:
  adaptive block partition
  beta/tau
  local delta
  tombstone
  local compaction

Predicate layer:
  conservative predicate-aware refinement
  safe accept/reject shortcuts
  GEOS call reduction
```

当前已经实现并验证的 predicate layer 实例是：

```text
query 矩形 envelope 完全包含 object envelope
-> object 必然与 query 矩形相交
-> 直接接受答案，跳过 GEOS exact intersects
```

这个优化不是启发式，也不是近似。它是一个接受型保守谓词，在当前矩形 range query workload 下保持 exact correctness。

2026-06-25 技能评估后的执行结论：

```text
idea-evaluator:
  Verdict = Accept with Revisions。
  方向值得继续，但必须收窄为 DELI-Cost-Lite + DELI-PRL。
  不要把论文写成 Bayesian Optimization 调参论文。

tech-paper-template:
  论文类型 = Technique Paper，带 New Setting framing。
  主线应是“复杂几何 exact predicate 的动态 learned spatial index”，
  不是单纯的 benchmark，也不是单纯的参数自适应。

intro-drafter:
  Introduction 必须围绕一个 running example 展开：
  query 矩形与 roads/parks/AW/LW 等复杂对象做 intersects。
  限制、挑战、模块、贡献要一一对应，不能超过三个挑战。

vibe-research-workflow:
  当前项目处在 mixed phase：
  coding + experiment + figure + writing 并行推进。
  AI 只能辅助代码、图表和文字组织；核心判断、结论和引用必须人工确认。
```

当前最重要的科学判断：

```text
DELI-Cost/DP:
  主要降低 structural cost：
    block_checks
    visited_blocks
    compact_records_scanned

DELI-PRL:
  主要降低 predicate cost：
    exact_calls / mbr_candidates
    GEOS refinement time

二者不是互相替代。
Cost-DP 说明系统不是固定参数工程实现；
PRL 解释当前实验中最大的查询收益来源。
```

最新写作判断：

```text
Cost-DP 的作用不能再被包装成“查询时间主要来源”。
它应该被定位为 structural predicate cost optimizer。

PRL shortcut 的作用不能被写成孤立 trick。
它应该被定位为 conservative predicate hierarchy 中的一个高 p_i/c_i 接受型谓词。
```

## 0.1 从 `manuscript26.6.12.docx` 整合进当前主线的内容

旧 Word 初稿虽然完成度不高，但有几类内容很有价值，应整合进 `main.tex`。

第一类是 **extent blindness 动机**。旧稿反复说明：复杂 geometry 不是点 key，`Zmin` 只能表示一维排序位置，不能表示对象的空间跨度。对于 `Intersects` 查询，一个 `Zmin` 很小、`Zmax` 很大的道路或水体 polygon 可能与右侧 query window 相交。这个观察应作为 Introduction 和 Background 的核心动机，而不是只作为 GLIN 的实现细节。

第二类是 **query augmentation 的失败机制**。旧稿指出，Zmin-only 访问路径为了不漏答案，需要把查询左边界向左扩展，或者从更早位置开始连续扫描。这个机制在长区间对象、fat objects 和动态插入后容易扩大候选范围。当前论文应把它写成“为什么 extent entry 是必要的”，而不要写成简单地批评某个 baseline。

第三类是 **块级安全摘要**。旧稿里的 `max_upper`、`min_lower`、`block_MBR` 对应当前代码和论文中的 `max_zmax`、`min_zmin`、`block_mbr`。它们应统一为 block summary，并与 one-sided conservative invariant 放在一起解释。

第四类是 **单侧安全动态维护协议**。旧稿中“摘要可以偏大，不能偏小”的解释非常适合保留。插入必须扩张 summary，删除可以 tombstone 并延迟收缩，local rebuild/compaction 精确重算 live summary。这是动态正确性的底层逻辑。

第五类是 **对象元数据表**。旧稿提到 object metadata table 支持多条目对象删除、结果去重和 MBR 缓存。当前实现虽然主要是 single extent entry，但论文可以把 object table 定位为 stable object id 到 live record/geometry/envelope 的元数据映射，并说明它为未来 multi-interval/fat-object splitting 预留空间。

需要从旧稿中删除或改写的内容：

```text
1. “第一个”“彻底解决”“全面优于 R-tree”“永远不会全表扫描”这类强表述不能直接保留。
2. “DELI 在查询上比 GLIN 快 2-5 倍、长区间场景快 10 倍、比 R-tree 小 60%-80%”等数字需要当前 CSV 重新支撑，否则只能写为待验证 hypothesis。
3. “V 一定 <= K”不严谨。更安全的表述是：DELI 避免 query augmentation 导致的强制连续扫描，并用 block summaries 支持整块跳过；但低选择性查询或大窗口查询仍可能访问大量 blocks。
4. “动态更新代价 O(log N)”需要谨慎。当前 LocalBounded/Cost overlay 的实际代价包括 ALEX update、local delta append、summary refresh、local compaction 和可能 split，论文应报告前台实测成本和摊还 local compaction 成本。
5. 旧稿里的自适应拆分、溢出分流、多区间覆盖如果当前代码没有完整实现，不应放进主贡献。可作为 future work 或 appendix roadmap。
```

整合到 `main.tex` 的状态：

```text
已整合：
  extent blindness 动机；
  GLIN-style query augmentation 的候选膨胀解释；
  旧稿观察的保留与修正；
  block summary 和 one-sided conservative invariant；
  参数表中对 b、beta、tau、pi、theta、C_geo、C_maint、C_space 的解释。
  单区间查询流程；
  多区间查询优化作为扩展路线；
  插入、删除、local compaction、split/merge 的动态协议；
  长区间对象的 adaptive splitting / overflow routing 作为 future extension；
  四个核心定理：保守区间覆盖、块级安全剪枝、动态单侧不变式、动态更新代价有界性。

还需要补：
  Figure 2: Zmin-only query augmentation vs DELI extent block pruning；
  Table 1: GLIN/R-tree/Quadtree/DELI-LB/DELI-Cost/DELI-PRL 能力对比；
  动机实验或诊断图，展示 long interval/fat object 对 query augmentation 和 exact calls 的影响。
```

### 0.1.1 从旧 docx 继承下来的理论模块

旧稿中最值得保留的不是具体实验数字，而是以下理论模块。它们已经被改写为当前 `main.tex` 的 correctness / cost model 章节。

| 旧稿概念 | 当前论文表述 | 是否作为主贡献 |
|---|---|---|
| Extent 条目（范围条目） | `extent entry = <zmin,zmax,MBR,oid>` | 是 |
| 保守 Z 区间覆盖 | 保守覆盖集合 `I(g)`，相交必有区间重叠 | 是 |
| 块级安全摘要 | `min_zmin/max_zmax/block_mbr/live/dead` | 是 |
| 单侧安全不变式 | summary 可以 stale-large，不能 stale-small | 是 |
| 插入协议 | append delta + expand summary + local compact when bounded | 是 |
| 删除协议 | tombstone + optional summary refresh + local compact | 是 |
| 块分裂/合并协议 | split/merge 后从 live entries 精确重建 summary | 是，但 merge 当前是扩展 |
| 长区间对象处理 | adaptive splitting / overflow routing | 未来扩展，不作为当前主实验 claim |
| 单区间查询流程 | block pruning -> record filtering -> PRL -> GEOS | 是 |
| 多区间查询优化 | interval-list traversal + range merge + dedup | 未来扩展或 appendix |

### 0.1.2 当前 `main.tex` 中的四个核心定理

旧稿里的定理已经统一整理为更防守的版本：

```text
Theorem 1 保守区间覆盖的相交不变性：
  如果 geometry 和 query 在二维空间真实相交，
  并且二者都有保守 Z 区间覆盖，
  则至少存在一对 Z 区间相交。

Theorem 2 块级安全剪枝定理：
  如果 block.max_zmax < query_zmin，
  或 block.min_zmin > query_zmax，
  或 block_mbr 与 query_mbr 不相交，
  则该 block 可以安全跳过。

Theorem 3 单侧安全摘要的动态不变性：
  insert 扩张 summary；
  delete tombstone 并允许 stale-large summary；
  local compaction / split / merge 从 live entries 精确重建 summary；
  因此任意动态更新序列后 summary 仍然 conservative。

Theorem 4 动态更新的代价有界性：
  local delta 和 tombstone 都被 block-level budget 约束；
  local compaction 只作用于一个 block；
  因此更新引起的额外扫描量由 visited blocks 和 beta/tau budgets 局部控制。
```

注意：旧稿里的 “DELI 永远不会全表扫描” 已经删除。当前更稳的说法是：

```text
DELI 不依赖离线 query augmentation 保持正确性；
动态更新导致的 delta/tombstone 污染由局部预算约束；
但大窗口、低选择性或高度重叠数据仍可能访问大量 blocks。
```

### 0.1.3 旧稿内容的实现状态边界

为了避免论文过度承诺，后续写作必须区分：

```text
已经实现并可实验：
  DELI-LB / DELI-Cost compact blocks
  local delta
  tombstone
  local compaction
  adaptive partition / DP
  PRL rectangle containment shortcut
  mixed workload runner
  correctness oracle sampling

当前主要作为理论兼容协议：
  block merge
  multi-interval query traversal
  object splitting with multi-entry dedup
  overflow routing for extremely long intervals

不应作为当前主结果强 claim：
  长区间对象一定被完全解决
  DELI 全面优于 R-tree
  查询永远不会接近全表扫描
  动态更新一定是 O(log N)
  memory 一定比 R-tree 小 60%-80%
```

后续如果要把 long interval / multi-interval 写进主贡献，必须补代码和实验：

```text
1. object_id -> multiple extent entries 的 metadata table；
2. multi-entry delete；
3. query answer dedup；
4. split threshold / overflow threshold 的 cost model；
5. long-interval stress dataset；
6. exact correctness check with Boost / brute force。
```

## 0.2 `tech-paper-template` 给出的论文骨架

论文类型：

```text
Technique Paper，带 New Setting framing。
```

理由：

```text
核心贡献是一个新的动态 learned spatial access path 和维护/谓词执行机制；
“复杂 geometry + exact predicate + dynamic update + learned layout”的组合用于加强问题定位，
但论文不应写成纯 benchmark 或纯问题定义论文。
```

Thinking template：

| Stage | 当前论文应写成的内容 |
|---|---|
| Research background | 复杂 polygon、linestring、roads、parks、water bodies 等 geospatial objects 需要 exact `Intersects` 查询和动态更新。 |
| Limitation 1 | Zmin-centric learned spatial index 存在 extent blindness，不能显式使用 `Zmax` 和 MBR 做安全剪枝。 |
| Limitation 2 | 动态 learned index 主要服务一维 key，缺少针对 extent summaries、local delta、tombstone 和 local compaction 的统一维护模型。 |
| Limitation 3 | 当结构扫描成本下降后，GEOS/JTS exact refinement 成为瓶颈；不保守的 centroid/area/type hard filter 会破坏 exact correctness。 |
| Key Idea | 将复杂 geometry 表示为 extent entry，用代价模型维护结构层，并在 GEOS 前加入保守谓词精化层。 |
| Challenge 1 | 自适应 block 划分、insert/delete、summary refresh 和 compaction 不能破坏 false-negative-free pruning。 |
| Challenge 2 | 固定 `block_size`、`beta`、`tau` 容易变成手动调参，难以解释不同 workload 下的 trade-off。 |
| Challenge 3 | 必须减少 exact calls，但只能使用保守接受/拒绝谓词，不能用启发式特征直接跳过 GEOS。 |
| Module A | Extent entry + one-sided conservative invariant。 |
| Module B | Cost-driven adaptive partition、beta/tau budget 和 benefit-cost local compaction。 |
| Module C | DELI-PRL conservative predicate hierarchy。 |
| Contribution 1 | 范围感知动态 learned spatial access path 和 correctness invariant。 |
| Contribution 2 | 代价驱动结构维护策略。 |
| Contribution 3 | 保守谓词精化层和 exact-call reduction。 |
| Contribution 4 | staged/mixed/drift workload 下与 Boost R-tree、GEOS Quadtree、GLIN-piece 和固定参数 DELI 的统一实验。 |

自一致性检查：

```text
Limitations -> Key Idea: pass
Key Idea -> Challenges: pass
Challenges -> Methodology: pass
Methodology -> Contributions: pass
```

需要继续防守的边界：

```text
1. 如果 Cost-DP 只降低 scan 指标，不能说它是查询性能最大来源。
2. 如果 PRL shortcut 只在矩形 query 上验证，不能说它自动适用于任意 polygon query。
3. 如果 CHECK_CORRECTNESS=0，不能用该实验声明 correctness。
```

## 0.3 `pre-submission-reviewer` 对当前 `main.tex` 的审查结论

当前草稿还不是可投稿稿，但主线已经比旧 docx 清楚。审查结果如下。

CRITICAL：

```text
1. 实验结论仍是阶段性文字，缺少最终 CSV 数字和表格支撑。
2. 贡献与实验还需要一一绑定：Cost-DP、LocalBounded、PRL、correctness、memory 分别需要独立图表。
3. 任意 polygon query 下的 PRL 边界必须在 Method 和 Discussion 中反复说明。
```

MAJOR：

```text
1. `main.tex` 仍是单文件大草稿，正式论文建议拆成 sections/。
2. 当前 Figure 1 是 fbox 占位图，投稿前必须替换为矢量结构图。
3. 算法伪代码目前用 figure/table 伪装，正式版建议换成 algorithm2e 或 ACM 兼容算法环境。
4. `index_mb_estimate` 是估算内存，不能作为强 memory claim。
5. 中英文混写适合当前内部草稿，但英文投稿前必须统一英文术语和句式。
```

MINOR：

```text
1. 部分段落偏长，需要拆成 topic sentence + evidence + takeaway。
2. `exact`、`predicate`、`refinement`、`summary` 等术语需要第一次出现时给中文解释。
3. 引文需要补齐 DBLP BibTeX，尤其 GLIN、learned spatial index、filter-and-refine 和 spatial database 相关工作。
```

禁用词和过强表述处理：

```text
旧 docx 中“第一个”“彻底解决”“永远不会全表扫描”“全面优于”等表述不应进入投稿版。
当前 main.tex 已经用“不是全面替代 R-tree”“balanced design point”“当前实现依赖矩形 query”等更稳妥表述替代。
```

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

### 1.9 `predicate-aware refinement layer`：谓词感知精化层

`predicate-aware refinement layer` 可以缩写为 `PRL`，中文叫 **谓词感知精化层**。

它位于：

```text
block pruning / record MBR pruning
和
GEOS exact predicate
之间
```

作用是：

```text
用非常便宜、严格保守的空间谓词，
提前拒绝不可能相交的对象，
或者提前接受一定相交的对象，
从而减少昂贵的 GEOS exact calls。
```

注意：

```text
PRL 不是 learned classifier。
PRL 不能用不保守的启发式特征直接丢弃候选。
```

例如，`centroid`、`area_bucket`、`geometry_type` 可以作为诊断特征或选择性估计特征，但不能直接作为 `intersects` 的 hard pruning 条件。否则可能产生 false negatives。

### 1.10 `conservative predicate`：保守谓词

`conservative predicate` 中文叫 **保守谓词**。

它分两类。

第一类是 **拒绝型保守谓词**：

```text
如果它说“不可能相交”，那就一定不相交。
```

例子：

```text
Z 区间不重叠
MBR 不相交
block MBR 不相交
```

第二类是 **接受型保守谓词**：

```text
如果它说“一定相交”，那就一定相交。
```

当前实现的例子：

```text
query 是矩形；
query envelope 完全包含 object envelope；
则 object geometry 一定落在 query 矩形内；
因此 object 一定与 query 相交。
```

### 1.11 `predicate shortcut`：谓词捷径 / 精化捷径

`predicate shortcut` 中文可叫 **谓词捷径** 或 **精化捷径**。

它不是为了改变答案，而是为了减少 GEOS 调用：

```text
MBR candidate
-> conservative predicate shortcut
-> accepted answer 或 rejected non-answer
-> 只有剩余候选进入 GEOS exact refinement
```

当前代码中的开关是：

```text
PREDICATE_SHORTCUTS=1  启用
PREDICATE_SHORTCUTS=0  关闭，用于 ablation
```

---

## 2. Idea-Evaluator 评估结果

### 2.1 第一印象

论文类型：

```text
Technique Paper，以 New Problem/Setting framing 做加强。
```

一句话故事：

> DELI-Cost 将 learned spatial index 从固定参数的动态工程实现，升级为一个代价驱动的自适应空间索引框架；进一步通过 DELI-PRL 在 GEOS exact refinement 之前引入保守谓词精化层，使复杂 geometry 的 exact spatial predicate 查询同时具备安全动态维护和低精化成本。

更短的投稿版本：

```text
DELI-Cost is a dynamic learned spatial index that treats complex geometries
as spatial extents, adapts its structural maintenance by cost, and reduces
exact predicate refinement through conservative predicate shortcuts.
```

### 2.2 Fatal flaws 审查

| # | 风险 | 严重程度 | 防守方式 |
|---|---|---|---|
| 1 | 范围过大或过度包装：如果同时承诺 exact polygon query、learned index、ALEX、BO、自适应分区、PRL、动态证明、系统实现，容易变成一锅粥；如果把矩形 shortcut 直接包装成任意 polygon 通用优化，也会被质疑 | MAJOR | 主线收窄成 `DELI-Cost-Lite + PRL`；BO 只作为可选校准；PRL 只声明 conservative predicate hierarchy，当前矩形包含只是一个安全实例 |
| 2 | 被认为只是调参：如果写成“用 BO 找最佳参数”，贡献会显得弱 | MAJOR | 写成“统一代价模型 + 闭式阈值 + 自适应分区 + benefit-cost compaction”，BO 只校准少数高层权重 |

没有不可修复的 CRITICAL fatal flaw，因此不需要 pivot。

防守边界：

```text
必须显式承认：
  Cost-DP 主要减少 scan/metadata 层成本；
  当前最大查询收益来自 PRL 减少 GEOS exact calls；
  当前 PRL shortcut 只对矩形 query workload 是直接安全的。

不能声称：
  centroid / area / geometry type 可以直接过滤 intersects；
  当前矩形 envelope shortcut 自动适用于任意 polygon query；
  Bayesian Optimization 是核心贡献。
```

### 2.3 五维评分

| 维度 | 分数 | 依据 | 如何加强 |
|---|---:|---|---|
| Higher，有效性 | 8 | `Zmin/Zmax + block MBR + exact refinement` 保持 exact answers；PRL 的接受型保守谓词不改变答案，只减少 GEOS 调用 | 用 `answers_match_boost=1` 和 `missing/extra=0` 防守正确性 |
| Faster，效率 | 9 | LocalBounded 改善写入；DP 降低 scan；PRL 将 `exact_calls / mbr_candidates` 从 100% 降到约 0.4%-1.6% | 做 `PREDICATE_SHORTCUTS=0/1` 消融，单独量化 GEOS reduction |
| Stronger，鲁棒性 | 8 | 自适应 beta/tau 和 benefit-cost compaction 应对读写变化；PRL 保守谓词不依赖 learned classifier，因此不会破坏正确性 | 加负载漂移实验，并明确任意 polygon query 下需要替换保守谓词 |
| Cheaper，成本 | 7 | 减少人工调参，减少全局 rebuild | 报告 calibration overhead 和 maintenance_ns |
| Broader，泛化/统一 | 8 | 把 spatial extent、learned layout、dynamic maintenance 和 predicate refinement 统一到一个 cost model | 和 R-tree、Quadtree、GLIN-piece、DELI fixed variants 做统一框架对比 |

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

### 2.5 Lifecycle 和可行性匹配

| 项目 | 判断 |
|---|---|
| Idea category | Innovative Technique + Data-intensive system evaluation |
| 预估生命周期 | 6-9 个月形成稳定投稿稿件；如果继续加入 BO/在线 split-merge，容易扩展到 12 个月以上 |
| 当前能力匹配 | 绿色偏黄：代码和实验基础已经具备，但论文故事必须收敛 |
| Compute 风险 | 中等，主要风险不是算力，而是 WKT 加载、oracle correctness 和多数据集重复实验耗时 |
| Data 风险 | 低，已有 AW/LW/PARKS/ROADS 和 query workload |
| Engineering 风险 | 中等，PRL 必须严格保守；不能为了减少 GEOS 调用引入 false negatives |
| Timeline 风险 | 中等，若把 BO、任意 polygon PRL、在线 split/merge 都纳入主线，会超范围 |

最终 verdict：

```text
Accept with Revisions。
先完成 DELI-Cost-Lite + DELI-PRL + ablation + correctness sampling。
Bayesian Optimization 和任意 polygon predicate hierarchy 作为扩展或 future work。
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

### 4.0 DELI-PRL 的统一形式化

更收敛的论文版本应把 DELI-Cost 和 DELI-PRL 写成一个系统：

```text
S = (D, Q, P, C)
```

中文解释：

```text
D: 空间数据集
Q: 查询和更新负载
P: 保守谓词层级
C: 代价模型
```

谓词集合：

```text
P = {P_1, P_2, ..., P_k}
```

每个谓词都有三个属性：

```text
c_i: computation cost，计算成本
p_i: decision probability，成功判定候选的概率
s_i: selectivity reduction，候选缩减能力
```

注意这里的 `p_i` 对拒绝型谓词表示成功拒绝概率，对接受型谓词表示成功接受概率。它们都必须满足保守性：

```text
Reject_i(q, o) = true  =>  intersects(q, o) = false
Accept_i(q, o) = true  =>  intersects(q, o) = true
```

统一优化目标应写成：

```text
J(pi, theta)
= E_q [
    sum_{P_i in pi(q)} c_i
    + C_exact * |R_final(q, pi, theta)|
  ]
  + C_maintenance(theta)
  + C_space(theta)
```

中文解释：

```text
pi(q): 查询 q 实际执行的谓词路径和谓词顺序
theta: 结构和维护策略，包括 block partition、beta、tau、compaction policy
R_final: 进入 GEOS exact refinement 的剩余候选集合
C_exact: 单次 GEOS exact predicate 的平均成本
```

这条公式统一了目前所有模块：

```text
DP adaptive partition:
  改变 theta 中的 partition，降低 block/scan predicate cost。

beta/tau:
  改变 theta 中的 local update budget，平衡 query scan 与 update maintenance。

benefit-cost local compaction:
  改变 theta 中的 maintenance operator timing。

PRL:
  改变 pi(q) 中的 predicate hierarchy，降低 |R_final|。

BO:
  可选地校准 C 中的少数高层权重，不直接搜索每个 block 的参数。
```

这一节要强调一个关键洞察：

```text
DP / block optimization 只能降低 structural predicate cost；
如果 mbr_candidates 和 exact_calls 不变，GEOS exact cost 仍是结构性下界；
PRL 正是为了突破这个 exact refinement 下界。
```

为了避免符号冲突，后续建议用 `theta` 表示结构维护策略，用 `P` 表示 predicate hierarchy。旧版 DELI 配置可以改写为：

```text
theta = (partition, {beta_B}, {tau_B}, compaction_policy)
```

中文解释：

```text
theta: 整个结构与维护策略
partition: block 划分方式
beta_B: block B 的局部增量比例
tau_B: block B 的墓碑比例
compaction_policy: 何时对某个 block 做 local compaction
```

目标函数：

```text
J(pi, theta)
= w_q * E[T_query(pi, theta)]
+ w_i * E[T_insert(theta)]
+ w_d * E[T_delete(theta)]
+ w_m * E[T_maintenance(theta)]
+ w_s * Space(theta)
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

查询代价应进一步拆成：

```text
T_query
= T_block_pruning
+ T_record_scan
+ T_predicate_filter
+ T_exact_refinement
```

中文解释：

```text
T_block_pruning:
  检查 block summary 的成本。

T_record_scan:
  扫 compact records、delta records、tombstone 的成本。

T_predicate_filter:
  执行 PRL 保守谓词的成本。

T_exact_refinement:
  GEOS exact intersects 的成本。
```

这个拆分非常关键。

最新实验说明：

```text
DP adaptive partition 主要降低 T_block_pruning 和 T_record_scan。
PRL shortcut 主要降低 T_exact_refinement。
```

因此 DELI-Cost 的完整模型不能只看 scan cost，还必须显式建模 exact refinement cost。

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

## 7.5 DELI-PRL：谓词感知分层精化层

### 7.5.1 为什么需要从 index layer 换到 predicate layer

最新诊断结果显示：

```text
DP adaptive partition:
  block_checks 下降
  visited_blocks 下降
  compact_records_scanned 下降

但是：
  mbr_candidates 基本不变
  exact_calls 基本不变
```

这说明 DELI 已经进入：

```text
predicate-bound regime
谓词瓶颈阶段
```

中文解释：

```text
索引扫描已经不是唯一瓶颈；
真正昂贵的是 MBR candidates 进入 GEOS exact intersects。
```

因此下一层优化不应该继续只调 block，而应该在：

```text
MBR candidate
和
GEOS exact refinement
```

之间增加一层保守谓词精化。

### 7.5.2 Conservative Predicate Hierarchy

对于相交查询：

```text
intersects(q, o)
```

定义两类保守谓词。

**拒绝型保守谓词**：

```text
Reject_i(q, o) = true  =>  intersects(q, o) = false
```

如果它拒绝一个对象，这个对象一定不是答案。

**接受型保守谓词**：

```text
Accept_i(q, o) = true  =>  intersects(q, o) = true
```

如果它接受一个对象，这个对象一定是答案。

DELI-PRL 的查询管线是：

```text
L1 block-level reject:
  Z interval + block MBR

L2 record-level reject:
  object Z interval + object MBR

L3 predicate-level accept/reject:
  cheap conservative predicate shortcuts

L4 exact refinement:
  GEOS exact intersects
```

当前实现的 L3 是：

```text
query 是 axis-aligned rectangle；
query envelope contains object envelope；
=> object geometry 必然在 query rectangle 内；
=> intersects(q, o) 必然为 true；
=> 直接加入答案，跳过 GEOS。
```

这个谓词的代价是几次数值比较，但能避免一次昂贵的 GEOS 多边形精确计算。

### 7.5.3 Lemma 4：PRL 保守正确性

**Lemma 4，PRL 保守正确性。**

如果 PRL 中所有拒绝型谓词都满足：

```text
Reject_i(q, o) = true  =>  intersects(q, o) = false
```

并且所有接受型谓词都满足：

```text
Accept_i(q, o) = true  =>  intersects(q, o) = true
```

那么 DELI-PRL 返回的答案集与对所有 live objects 执行 GEOS exact intersects 的答案集一致。

证明思路：

```text
1. 被拒绝型谓词跳过的对象一定不是答案，因此不会漏真答案。
2. 被接受型谓词直接加入的对象一定是答案，因此不会引入错误答案。
3. 既未拒绝也未接受的对象继续进入下一层，最终由 GEOS exact intersects 判定。
4. 因此最终结果与全量 exact refinement 等价。
```

这个 lemma 是 PRL 的正确性底线。

注意：

```text
centroid mismatch
area mismatch
geometry type mismatch
```

默认都不是 `intersects` 的保守谓词，不能作为 hard pruning。它们只能用于：

```text
1. 选择性估计；
2. cost model 特征；
3. 诊断分析；
4. learned routing；
```

不能直接决定跳过 GEOS。

### 7.5.4 Theorem 5：谓词层级最优排序

对一个保守谓词 `P_i`，定义：

```text
c_i: 执行该谓词的平均成本
p_i: 该谓词成功判定候选的概率
     对拒绝型谓词，是成功拒绝概率；
     对接受型谓词，是成功接受概率。
```

如果当前候选未被 `P_i` 判定，则进入下一层，代价为：

```text
C_{i+1}
```

则该层的期望代价是：

```text
C_i = c_i + (1 - p_i) * C_{i+1}
```

**Theorem 5，最优层级排序。**

对于多个相互独立或近似独立的保守谓词，按照：

```text
p_i / c_i
```

从高到低排序，可以最小化期望精化代价。

两两交换证明：

```text
A before B:
  C_AB = c_A + (1 - p_A)c_B + (1 - p_A)(1 - p_B)C_next

B before A:
  C_BA = c_B + (1 - p_B)c_A + (1 - p_A)(1 - p_B)C_next

C_AB - C_BA = p_B c_A - p_A c_B
```

当：

```text
p_A / c_A >= p_B / c_B
```

时：

```text
C_AB <= C_BA
```

所以 `A` 应该排在 `B` 前面。由两两交换可得全局排序。

对于当前矩形包含 shortcut：

```text
c_i 极低：几次数值比较；
p_i 极高：实验中 predicate_shortcuts / mbr_candidates 可达到 98% 以上；
```

因此它应该排在 GEOS exact intersects 之前。

### 7.5.5 PRL 与 DELI-Cost 的关系

DELI-Cost 和 DELI-PRL 优化的是不同层。

```text
DELI-Cost:
  降低 block_checks
  降低 visited_blocks
  降低 compact_records_scanned
  控制 delta/tombstone/compaction

DELI-PRL:
  降低 exact_calls
  降低 GEOS refinement cost
```

因此论文中不能把 Cost-DP 和 PRL 混在一起报一个总收益。必须做 ablation：

```text
LocalBounded
Cost
LocalBounded + PRL
Cost + PRL
```

建议用开关：

```text
PREDICATE_SHORTCUTS=0
PREDICATE_SHORTCUTS=1
```

来隔离 PRL 的贡献。

### 7.5.6 当前 PRL 的适用边界

当前实现的接受型谓词依赖：

```text
query 是 axis-aligned rectangle
```

所以它适用于当前 interval-overlap range query workload。

如果未来 query 是任意 polygon，则不能直接使用：

```text
query envelope contains object envelope
```

作为接受型谓词。因为：

```text
object envelope 在 query envelope 内
不代表 object geometry 在 query polygon 内。
```

任意 polygon query 下可以使用更强但更贵的保守谓词，例如：

```text
query polygon contains all four corners of object envelope
并且 query polygon 覆盖 object envelope rectangle
```

或者直接用 GEOS 的 `contains` / `covers` 对 envelope rectangle 做保守接受检查。但这时 `c_i` 会变大，是否值得启用应由 `p_i / c_i` 决定。

结论：

```text
PRL 是通用框架；
矩形 envelope containment 是当前 workload 的一个低成本安全实例；
不能把这个具体实例过度宣称为任意 polygon query 的免费优化。
```

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
| Limitation 1 | 现有 learned spatial index 常将复杂 geometry 压成 `Zmin` 等 point-like key，存在 extent blindness，难以利用 `Zmax` 和对象 MBR 做安全剪枝。 |
| Limitation 2 | 现有动态 learned index 主要面向一维 key，对 exact spatial predicate query 所需的 extent summaries、delta、tombstone 和 local compaction 缺乏统一维护模型。 |
| Limitation 3 | 即使 structural scan 成本被降低，复杂 geometry 查询仍可能被 GEOS exact refinement 主导；传统 pipeline 缺少系统化的保守 predicate-aware refinement 层。 |
| Key Idea | 将复杂 geometry 表示为 extent entry，用代价模型自适应维护 block/delta/tombstone，并在 GEOS 前加入保守谓词精化层来减少 exact calls。 |
| Challenge 1 | 自适应 block 划分、插入、删除和 compaction 会改变 summary，如果 summary 不是单侧保守的，查询可能漏答案。 |
| Challenge 2 | 固定 `block_size`、`beta` 和 `tau` 只适合某一种 workload，直接手调会使系统缺乏可解释性和可迁移性。 |
| Challenge 3 | MBR candidates 进入 GEOS 后成本很高，但不保守的 centroid/area/type 过滤会破坏 exact correctness。 |
| Methodology topic sentence | DELI-Cost is a cost-driven dynamic learned spatial index with conservative extent maintenance and predicate-aware refinement. |
| Module A，addresses Challenge 1 | Extent entry + one-sided conservative invariant：维护 `<Zmin, Zmax, MBR, geometry>` 和偏大的 block summaries，保证所有自适应操作只影响性能，不影响正确性。 |
| Module B，addresses Challenge 2 | Cost-driven adaptive structural maintenance：用 DP 做 bulk-load block partition，用闭式 `beta/tau` 和 benefit-cost local compaction 控制 delta/tombstone。 |
| Module C，addresses Challenge 3 | DELI-PRL predicate-aware refinement：只使用拒绝型或接受型保守谓词，在 GEOS exact refinement 前减少 exact calls。 |
| Contribution 1 | 提出 DELI-Cost 的 extent-aware dynamic learned spatial index 抽象和单侧保守正确性不变式。（Sections 2-3） |
| Contribution 2 | 提出代价驱动的 adaptive block partition、block 级 `beta/tau` 闭式选择和 benefit-cost local compaction。（Section 4） |
| Contribution 3 | 提出 DELI-PRL 保守谓词精化层，并给出 exact correctness 条件和谓词排序原则。（Section 5） |
| Contribution 4 | 在 staged、mixed 和 drift workloads 下，与 Boost R-tree、GEOS Quadtree、GLIN-piece、固定参数 DELI variants 做 query/update/memory/correctness 对比，并用 ablation 区分 structural layer 和 predicate layer 的收益。（Section 6） |

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
如果实际实验只验证了矩形 query shortcut，不要声称 PRL 已经完整覆盖任意 polygon query。
```

---

## 10. Introduction 草稿结构，intro-drafter 版本

### 10.0 Type positioning

```text
Type: Technique Paper
Rationale:
  论文主要贡献是一个新的动态 learned spatial index 机制。
  New Setting framing 用来强调复杂 geometry + exact predicate + dynamic update 的组合空白，
  但不要把全文写成纯问题定义论文。
Implication:
  Paragraph 3 用一段清楚定义目标和约束；
  Paragraph 4/5 承担主要技术叙事。
```

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
2. 多数 learned spatial index 更擅长点数据、range query 或近似过滤，对 polygon exact predicate 和动态 extent summaries 支持不足。
3. 即使使用 extent-aware block pruning，复杂对象的 MBR candidates 仍可能被 GEOS exact refinement 主导，现有 learned spatial pipeline 缺少保守 predicate-aware refinement 层。
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
  Naively shrinking summaries after deletes or moving records during splits can make safe pruning unsafe.

Challenge 2: Dynamic maintenance must avoid fixed thresholds that only work for one workload.
  A fixed delta or tombstone budget may protect query latency in one workload but hurt update throughput in another.

Challenge 3: Predicate refinement must reduce GEOS calls without unsafe heuristic pruning.
  Centroid, area, and type features are useful diagnostics, but using them as hard filters for intersects may create false negatives.
```

### Paragraph 5：方法概览

```text
DELI-Cost represents every object as an extent entry <Zmin, Zmax, MBR, geometry>.
It maintains one-sided conservative block summaries for safe pruning.
It uses a cost model to adapt block partition, beta/tau budgets, and local compaction decisions.
It adds a predicate-aware refinement layer that uses cheap conservative accept/reject predicates before invoking GEOS.
It evaluates the design under staged and interleaved mixed workloads.
```

Challenge-to-module mapping:

```text
Challenge 1 -> Extent entry + one-sided conservative invariant.
Challenge 2 -> Cost-driven block partition, beta/tau, and local compaction.
Challenge 3 -> DELI-PRL conservative predicate hierarchy.
```

### Paragraph 6：贡献

1. 提出 `DELI-Cost` 的 extent entry 抽象和单侧保守不变式，支持复杂 geometry exact predicate 查询下的安全剪枝。
2. 提出自适应 block 划分、block 级 `beta/tau` 闭式选择和收益-成本驱动 local compaction，减少人工阈值依赖。
3. 提出 DELI-PRL 谓词感知精化层，用接受型/拒绝型保守谓词减少 GEOS exact calls，同时保持 exact correctness。
4. 在真实与合成数据、不同 selectivity、staged/mixed/drift 负载下评估 DELI-Cost/PRL，并与 Boost R-tree、GEOS Quadtree、GLIN-piece 和固定参数 DELI variants 对比。

### Flowchart consistency check

```text
Running example loop:
  pass。Paragraph 1 的 roads/parks intersects 例子会在 Method 和 Figure 1 中复用。

Limitations -> Challenges:
  pass。三个 limitation 分别对应 extent blindness、动态维护、GEOS refinement。

Challenges -> Modules:
  pass。三个 challenge 分别对应 invariant、cost-driven structural maintenance、PRL。

Contributions -> Sections:
  pass。贡献数量控制在四个，每个都能映射到 Problem/Method/Experiment 章节。
```

---

## 11. Benchmark 设计，benchmark-paper-template 思维

这篇文章不建议写成纯 benchmark paper，但实验章节要用 benchmark paper 的严谨结构。

### 11.1 Research Questions

建议 RQ：

```text
RQ1: DELI-Cost 是否保持 exact correctness？
RQ2: 自适应 block partition 是否比固定 block size 有更好的剪枝效率？
RQ3: 自适应 beta/tau 是否比固定阈值在不同 mixed workload 下更稳定？
RQ4: DELI-PRL 是否能在保持 exact correctness 的前提下显著减少 GEOS exact calls？
RQ5: DELI-Cost 和 Boost R-tree、GEOS Quadtree、GLIN-piece 相比，query/update/memory trade-off 如何？
RQ6: 在 workload drift 下，DELI-Cost 是否能避免固定参数版本的性能崩坏？
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
DELI-Cost-Lite + PRL
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
predicate_shortcuts
predicate_shortcuts_enabled
exact_calls / mbr_candidates
records_scanned
blocks_visited
blocks_pruned
```

PRL 消融必须报告：

```text
PREDICATE_SHORTCUTS=0
PREDICATE_SHORTCUTS=1
```

并分别比较：

```text
mbr_candidates
predicate_shortcuts
exact_calls
exact_calls / mbr_candidates
avg_query_ms
p95_query_ms
p99_query_ms
answers_match_boost
missing_count
extra_count
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

### Figure 0：Unified Cost-driven Predicate Execution Model

类型：

```text
Conceptual Model / Optimization Framework
```

布局：

```text
Left:
  Dataset D
  Workload Q
  Conservative predicate set P
  Cost model C

Middle:
  Predicate execution path pi(q)
  Structural maintenance policy theta
    partition
    beta/tau
    compaction policy

Right:
  scan cost
  exact GEOS cost
  maintenance cost
  space cost
```

核心信息：

```text
DP、beta/tau、local compaction、PRL 不是模块堆砌；
它们都是 J(pi, theta) 的不同求解部分。
```

建议配套一个短算法：

```text
Algorithm 0: Cost-driven Predicate Policy Selection
Input:
  workload statistics
  block statistics
  conservative predicate candidates
Output:
  theta = partition + beta/tau + compaction policy
  pi = predicate ordering
Steps:
  1. estimate predicate cost c_i and decision rate p_i
  2. order conservative predicates by p_i / c_i
  3. compute adaptive partition by DP
  4. derive beta/tau from local cost balance
  5. trigger local compaction by benefit-cost rule
```

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
  block summary pruning -> record pruning -> PRL conservative shortcuts -> GEOS exact refinement
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

### Figure 8：Predicate Refinement Breakdown

类型：

```text
stacked bar 或 waterfall chart
```

内容：

```text
MBR candidates
-> predicate_shortcuts accepted
-> remaining exact_calls
```

或者直接画：

```text
exact_calls / mbr_candidates
```

对比：

```text
PREDICATE_SHORTCUTS=0
PREDICATE_SHORTCUTS=1
Boost R-tree
GEOS Quadtree
GLIN-piece
```

核心信息：

```text
DELI-PRL 的主要收益来自降低 GEOS entry rate，
不是来自减少 MBR candidates。
```

### Figure 9：Memory Breakdown

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

### 13.2 Phase classification

当前阶段不是单纯 coding，也不是单纯 writing，而是 mixed phase：

```text
Vibe Coding:
  继续完善 benchmark、ablation、correctness sampling 和诊断指标。

Vibe Figure:
  把 structural layer 和 predicate layer 的收益拆成图。

Vibe Writing:
  把 DELI-Cost + DELI-PRL 写成一条克制、可防守的 VLDB 技术论文故事。
```

### 13.3 6 周执行计划

| 周次 | 阶段 | 任务 | 产物 | 工具 |
|---|---|---|---|---|
| Week 1 | Writing + Design | 锁定 DELI-Cost + PRL 理论规格，写清楚术语、目标函数、Lemma/Theorem；把任意 polygon PRL 放入 future work | `docs/deli_cost_theory_spec.md` | Codex + 你手动审阅 |
| Week 2 | Coding + Experiment | 做 PRL on/off ablation：`PREDICATE_SHORTCUTS=0/1`，同时记录 `exact_calls/mbr_candidates` | ablation CSV + smoke correctness | Codex + shell |
| Week 3 | Experiment | 跑 fixed LocalBounded vs Cost-DP vs PRL 组合实验，确认 structural layer 和 predicate layer 的独立贡献 | results + summary table | shell + Python |
| Week 4 | Experiment | 跑 correctness sampling：`CHECK_CORRECTNESS=1`、`CORRECTNESS_EVERY_N`，至少覆盖最终 checkpoint 和多数据集 smoke | correctness log | shell |
| Week 5 | Figure | 画 Figure 1/2/3/8：running example、系统结构、block delta/tombstone、predicate breakdown | PDF/PNG 图草稿 | Matplotlib + draw.io/Figma |
| Week 6 | Writing | 写 Introduction、Methodology、Experiment Setup 和 Threats to Validity 初稿 | LaTeX 初稿 | 本地 LaTeX/Overleaf + AI 语言润色 |

每个阶段的用户检查点：

```text
Coding:
  你必须确认每个新增 shortcut 都是 conservative predicate。
  不能只因为性能变好就接受 hard pruning。

Experiment:
  你必须确认每个图的 CSV 来源、命令、seed、dataset、query_count。
  CHECK_CORRECTNESS=0 的结果只能用于性能趋势，不能用于正确性声明。

Figure:
  所有实验图必须由真实 CSV 生成，不能补点、改点或手动美化数值。

Writing:
  AI 可以帮你组织文字，但每个 claim 必须能回到代码、定理或 CSV。
```

### 13.4 工具建议

| 阶段 | 主工具 | 替代工具 | 理由 |
|---|---|---|---|
| Coding | Codex | Cursor / Claude Code | 当前项目已经在 Codex 工作区内，适合小步 patch、build、smoke test |
| Figure | Matplotlib + draw.io/Figma | PowerPoint | 实验图必须可复现，结构图可以先 draw.io 再 Figma 精修 |
| Writing | 本地 LaTeX / Overleaf | ChatGPT/Claude 语言润色 | 论文逻辑由你定，AI 只做表达和结构检查 |
| Literature | DBLP / Google Scholar / arXiv | AI 辅助整理 | 引用不能由 AI 编造，必须你确认 |

### 13.5 什么时候再做 Bayesian Optimization

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

当前代码状态：

```text
已经完成：
  DELI_ALEX_HYBRID_LOCAL_BOUNDED
  DELI_ALEX_HYBRID_COST
  DP adaptive block partition
  per-block beta/tau 统计输出
  PREDICATE_SHORTCUTS=0/1
  predicate_shortcuts / exact_calls / mbr_candidates 诊断指标
```

因此近期任务不再是从零实现 Cost-Lite，而是做三组可防守实验。

### 14.1 Structural layer ablation

```text
目的：
  证明 Cost-DP 不是主要 GEOS 优化，但确实降低 structural scan cost。

对比：
  DELI_ALEX_HYBRID_LOCAL_BOUNDED
  DELI_ALEX_HYBRID_COST

必须报告：
  block_checks
  visited_blocks
  compact_records_scanned
  delta_records_scanned
  avg_query_ms
  p95_query_ms
  p99_query_ms

预期结论：
  Cost-DP 降低 block/scan 层成本；
  但如果 mbr_candidates 和 exact_calls 不下降，说明瓶颈转移到 predicate layer。
```

### 14.2 Predicate layer ablation

```text
目的：
  证明 PRL shortcut 是当前查询收益的主要来源，并且不破坏 correctness。

对比：
  PREDICATE_SHORTCUTS=0
  PREDICATE_SHORTCUTS=1

必须报告：
  mbr_candidates
  predicate_shortcuts
  exact_calls
  exact_calls / mbr_candidates
  avg_query_ms
  p95_query_ms
  p99_query_ms
  answers_match_boost
  missing_count
  extra_count

最低正确性要求：
  每个 dataset + selectivity 至少跑一个小规模 CHECK_CORRECTNESS=1 smoke。
  正式性能实验可以 CHECK_CORRECTNESS=0，但不能用它声明 correctness。
```

推荐 smoke 命令模板：

```bash
RESET_RESULTS=1 OVERWRITE=1 AUTO_BUILD=0 \
PREDICATE_SHORTCUTS=1 \
INDEXES="DELI_ALEX_HYBRID_LOCAL_BOUNDED DELI_ALEX_HYBRID_COST" \
CHECK_CORRECTNESS=1 CORRECTNESS_EVERY_N=1 \
WORKLOAD_MODE=mixed MIXED_PROFILES="read_heavy balanced write_heavy" \
MIXED_OPERATIONS=5000 MIXED_CHECKPOINT_INTERVAL=2500 \
DATASETS="AW" LIMIT=100000 QUERY_LIMIT=500000 \
QUERY_ROOT=queries/interval_overlap_full_500000 \
RESULT_DIR=/tmp/deli_pr_correctness_results \
FIGURE_DIR=/tmp/deli_pr_correctness_figures \
SELECTIVITY_TAGS="1pct" QUERY_COUNT=10 \
LOCAL_DELTA_BOUND=128 DELETE_COMPACT_FRACTION=0.25 \
BUILD_DIR=build_current PLOT_RESULTS=0 \
./scripts/run_dynamic_compare_diagnostics.sh
```

### 14.3 Combined system comparison

```text
目的：
  证明完整系统在 query/update/memory trade-off 上有竞争力。

对比：
  DELI_ALEX_HYBRID_LOCAL_BOUNDED
  DELI_ALEX_HYBRID_COST
  Boost_Rtree
  GEOS_Quadtree
  GLIN_PIECEWISE

建议参数：
  LIMIT=500000 或 1000000
  QUERY_COUNT=100 或 200
  MIXED_OPERATIONS=100000
  MIXED_CHECKPOINT_INTERVAL=5000
  SELECTIVITY_TAGS="0p1pct 1pct"

快速调试可以 QUERY_COUNT=10，
但论文结论不要依赖 QUERY_COUNT=10 的 p95/p99。
```

### 14.4 Drift workload

```text
目的：
  证明自适应维护不是静态调参，而是能随负载变化保持稳定。

负载：
  phase 1: read-heavy
  phase 2: write-heavy
  phase 3: read-heavy

对比：
  fixed small threshold
  fixed large threshold
  LocalBounded fixed
  Cost-DP
  Cost-DP + PRL

重点看：
  avg_query_ms drift
  p95/p99 drift
  local_compaction_count_stage
  avg_beta / avg_tau
  exact_calls / mbr_candidates
```

### 14.5 PRL 系统化边界

```text
1. 保留当前矩形 query envelope contains object envelope 的安全接受型 shortcut。
2. 增加 PREDICATE_SHORTCUTS=0/1 消融实验。
3. 在 correctness sampling 下验证 answers_match_boost。
4. 报告 predicate_shortcuts、exact_calls、exact_calls/mbr_candidates。
5. 只把 centroid/area/type 作为选择性估计特征，不作为 hard pruning。
6. 对任意 polygon query 单独设计新的保守谓词，不复用矩形 envelope shortcut。
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
  predicate-aware refinement layer
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
5. PRL 将 exact_calls / mbr_candidates 显著降低；
6. workload drift 下比固定参数版本更稳；
```

那么它就比当前 `DELI_ALEX_HYBRID_LOCAL_BOUNDED` 更像一个完整 VLDB 级系统贡献。

当前阶段的判断：

```text
DP/Cost 是结构层贡献，证明 DELI 不是固定参数工程实现；
PRL 是谓词层贡献，解释最新实验中最大的查询收益来源；
二者应该组合成完整系统，而不是互相替代。
```

---

## 16. 参考线索

本文档没有把以下工作当作已完整验证的 related work，只作为后续文献梳理入口：

- Limbo: C++11 Bayesian Optimization / Gaussian Process library，用于昂贵黑盒函数优化。适合作为 DELI-Cost-Bayes 的高层权重校准参考。
- ALEX: Updatable Adaptive Learned Index，说明 learned index 可以支持动态更新，但主要面向一维 key。
- Waffle / QUASII / workload-aware spatial indexing：可作为自适应空间索引和动态 workload 相关工作入口。
- R-tree / Quadtree / LSM-R-tree：作为传统空间索引和动态维护 baseline。
- GLIN / learned spatial index / learned space-filling curve：作为 learned spatial index 相关工作入口。
