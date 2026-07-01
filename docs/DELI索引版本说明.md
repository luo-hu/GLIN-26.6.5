# DELI 学习型空间索引项目版本说明

本文档整理 DELI 动态学习型空间索引项目中的主要索引版本。DELI 面向复杂几何对象的 exact spatial intersects 查询，核心设计从早期的 extent-aware 动态原型，逐步演进到 ALEX 写入层、compact query overlay、局部有界维护、代价驱动自适应策略、谓词感知精化层 PRL，以及 SingleStore 单存储优化。

术语约定：

- `PRL`: predicate-aware refinement layer，谓词感知精化层。它不是独立索引，而是可叠加在 DELI/GLIN/Boost/Quadtree 等方法前的保守谓词精化层。
- `extent entry`: 范围条目，记录 `zmin/zmax/MBR/geometry/id` 等摘要。
- `local delta`: 局部增量缓存，新插入对象先进入 block-local delta。
- `tombstone deletion`: 墓碑删除，删除时先标记 dead，不立即物理移除。
- `local compaction`: 局部压实，只整理被污染的 block。
- `DP block partition`: 动态规划自适应块划分。
- `stale-large summary`: 删除后保留偏大的保守摘要，只会多扫候选，不会漏答案。

## 一、版本演进总览

### 1. 早期原型：证明动态 extent summary 的正确性

**代表版本：`DELI-Dynamic`**

最早的 DELI-Dynamic 原型不深度绑定 ALEX，而是围绕 append-only records、stable record id、block-local sorted vectors、object-to-record 映射、tombstone deletion 和 block summary 维护展开。

它解决的核心问题是：

- 动态插入/删除后，`zmin/zmax + block MBR + exact refinement` 仍然 exact。
- 删除后 stale-large summary 是否安全。
- block split、local rebuild、tombstone compact 是否破坏正确性。

这个版本的论文价值主要是 correctness 和 dynamic invariant，不是最终性能主方法。

### 2. 中间迭代版：把 ALEX 动态写入能力接入 DELI

**代表版本：`DELI-ALEX`、`DELI-ALEX-Hybrid`、`DELI-ALEX-Hybrid-Buf`、`DELI-ALEX-Hybrid-Bounded`**

这一阶段尝试把 ALEX 的动态有序索引能力用于空间 extent key 的写入维护。

演进逻辑如下：

1. `DELI-ALEX`  
   直接在 ALEX leaf 上维护 extent summary。写入能力较好，但查询需要扫描大量 ALEX leaf / slots，cache locality 和 query latency 不理想。

2. `DELI-ALEX-Hybrid`  
   引入 compact query overlay。ALEX 负责动态写入布局，查询侧使用更接近 IntervalOverlapIndex 的 compact blocks。目标是恢复连续扫描和 block summary pruning 的优势。

3. `DELI-ALEX-Hybrid-Buf`  
   使用全局或较粗粒度 delta buffer 改善插入吞吐。它验证了 append delta 的写入优势，但查询要额外扫描不断增长的 delta，query latency 容易变差。

4. `DELI-ALEX-Hybrid-Bounded`  
   给 delta 加上 bounded compaction，控制查询退化。但如果 compaction 粒度过大，会显著拉低插入吞吐。它是从 global delta 走向 block-local delta 的过渡原型。

这些版本是工程探索链条，不建议作为论文主方法。

### 3. 正式实验版：局部有界维护、代价驱动和单存储

**代表版本：`DELI_ALEX_HYBRID_LOCAL_BOUNDED`、`DELI_ALEX_HYBRID_COST`、`DELI_ALEX_HYBRID_SINGLE_STORE`、`DELI_ALEX_HYBRID_SINGLE_STORE_COST`**

这一阶段形成当前最重要的四个版本：

1. `DELI_ALEX_HYBRID_LOCAL_BOUNDED`  
   固定参数的强工程基线。每个 compact block 维护 local delta 和 tombstone，达到阈值后只 local compaction。它查询稳定、写入较强，是所有自适应版本必须超过或至少接近的固定参数 baseline。

2. `DELI_ALEX_HYBRID_COST`  
   在 LocalBounded 上增加 cost-driven 自适应机制，包括 per-block `beta/tau`、DP block partition、benefit-cost compaction。它的目标不是简单调参，而是解释不同 workload 下 block/delta/tombstone 如何自适应。

3. `DELI_ALEX_HYBRID_SINGLE_STORE`  
   去掉 ALEX 数据副本，只保留 compact query overlay 作为主存储。目标是解决双结构带来的内存、插入吞吐和 cache locality 短板。

4. `DELI_ALEX_HYBRID_SINGLE_STORE_COST`  
   SingleStore + Cost。保留单存储低内存/高写入优势，同时加入自适应 `beta/tau` 和 cost policy。

PRL 可以开/关叠加在这些版本上，用于评估 predicate-aware refinement 对 GEOS exact calls 的削减效果。

### 4. 基线方法：GLIN-piece 与传统空间索引

**代表版本：`GLIN-piece`**

GLIN-piece 是项目继承的 learned spatial index baseline。它支持 piecewise learned mapping 和动态插入/删除，是论文里必须保留的 learned baseline。

注意：`GLIN-piece` 也可叠加 PRL。PRL 作为通用保守谓词层，对 GLIN-piece、Boost R-tree、GEOS Quadtree 都应该公平启用/关闭。

## 二、核心版本多维度对比表

| 代码原名 | 论文/正式命名 | 核心架构 | 核心特性 | PRL 支持情况 | 论文中定位 | 核心性能标签 |
|---|---|---|---|---|---|---|
| `DELI-Dynamic` / `DELI_DYNAMIC_SINGLE` | DELI-Dynamic-Single | 单结构动态 extent prototype；append-only records + block-local sorted ids | stable record id、block summary、tombstone deletion、local rebuild/split | 可接入 exact refinement；不是主要 PRL 版本 | 早期原型 / correctness 证明基线 | 正确性清晰，动态 invariant 强，性能不是主打 |
| `DELI-ALEX` | DELI-ALEX | ALEX leaf 作为主要动态结构 | ALEX leaf summary、leaf/group directory、动态插入删除 | 可叠加 | 中间迭代原型 | 写入自然，但查询 leaf scan 成本高 |
| `DELI-ALEX-Hybrid` | DELI-Hybrid | 双结构：ALEX write layout + compact query overlay | ALEX 负责写入，overlay 负责查询；compact blocks 改善连续扫描 | 可叠加 | 中间迭代原型 / 消融 | 查询较 DELI-ALEX 改善，仍有双结构开销 |
| `DELI-ALEX-Hybrid-Buf` | DELI-Hybrid-Buf | 双结构 + buffer/delta | 中间迭代原型，推测为缓冲区优化版本，用于验证 append delta 对写入吞吐的收益 | 可叠加 | 工程验证原型 | 插入快，但 query 可能受 delta 膨胀拖累 |
| `DELI-ALEX-Hybrid-Bounded` | DELI-Hybrid-Bounded | 双结构 + bounded delta | 对 delta 做 bounded compaction，控制查询退化；早期 compaction 粒度偏重 | 可叠加 | 工程验证原型 | 查询受控，但插入吞吐可能下降 |
| `DELI_ALEX_HYBRID_LOCAL_BOUNDED` | DELI-LB / DELI-LocalBounded | 双结构：ALEX write layout + compact query overlay | 固定 block size；per-block local delta；tombstone deletion；local compaction；固定 `LOCAL_DELTA_BOUND` 和 `DELETE_COMPACT_FRACTION` | 支持开/关 | 强消融基线 / 固定参数正式版本 | 查询稳定，局部维护有界，工程表现强 |
| `DELI_ALEX_HYBRID_COST` | DELI-Cost | 双结构：ALEX write layout + compact query overlay + cost policy | 在 LocalBounded 上加入 per-block `beta/tau`、DP block partition、benefit-cost compaction | 支持开/关 | 代价驱动主方法或自适应消融版本 | 自适应能力强，解释性好；常规负载下未必超过强固定基线 |
| `DELI_ALEX_HYBRID_SINGLE_STORE` | DELI-SingleStore | 单结构：compact query overlay 是主存储；移除 ALEX 数据副本 | 保留 LocalBounded 的 block/local delta/tombstone/local compaction，但不维护 ALEX leaf 数据 | 支持开/关 | 新主线候选 / 架构优化版本 | 内存低，插入吞吐高，查询保持 DELI+PRL 优势 |
| `DELI_ALEX_HYBRID_SINGLE_STORE_COST` | DELI-SingleStore-Cost | 单结构 + cost policy | SingleStore + adaptive `beta/tau` / cost decision；目标是低内存、高写入、自适应维护 | 支持开/关 | 当前最值得推进的主方法候选 | 综合平衡最好：低内存、高插入、可自适应 |
| `GLIN-piece` / `GLIN_PIECEWISE` | GLIN-piece | learned spatial baseline；piecewise learned mapping | 原始 GLIN piecewise 动态机制 | 支持开/关 | 对比基线 | learned baseline，通常作为必须比较对象 |

## 三、重点版本详细说明

### 3.1 `DELI_ALEX_HYBRID_LOCAL_BOUNDED`

论文建议命名：**DELI-LB** 或 **DELI-LocalBounded**。

#### 设计目标

LocalBounded 的目标是用最简单、可解释、前台计成本的方式解决动态维护问题：

- 插入不立即打乱 compact records，而是进入 block-local delta。
- 删除不立即物理移除，而是 tombstone deletion。
- delta/tombstone 超过阈值后，只对被污染 block 做 local compaction。
- block summary 允许 stale-large，从而保证删除路径轻量且 correctness 不受影响。

#### 与基础版本的核心差异

相较于 `DELI-ALEX-Hybrid`，LocalBounded 不再只是一个 compact query overlay，而是显式引入：

- per-block local delta；
- per-block tombstone count；
- local compaction；
- `LOCAL_DELTA_BOUND`；
- `DELETE_COMPACT_FRACTION`；
- lazy ALEX delete；
- deferred summary refresh。

#### 优势与适用场景

优势：

- 机制简单，实验稳定。
- 是强固定参数 baseline。
- 对 staged workload 和 mixed workload 都容易解释。
- 单线程前台维护成本透明，不涉及异步公平性争议。

适用场景：

- 作为所有 Cost/SingleStore 方法的固定参数对照。
- 做 PRL on/off 消融。
- 做 local delta / tombstone / local compaction 有效性验证。

#### 典型实验表现

已知结论：

- `LOCAL_DELTA_BOUND=128`、`DELETE_COMPACT_FRACTION=0.25` 是当前较强经验设置。
- 在 PRL 开启时，复杂几何 exact calls 可大幅下降，部分实验中 PRL 可削减 95%+ GEOS 调用。
- 主要短板是双结构开销：ALEX write layout + compact query overlay 重复维护元数据，导致内存和插入吞吐不如 SingleStore。

### 3.2 `DELI_ALEX_HYBRID_COST`

论文建议命名：**DELI-Cost**。

#### 设计目标

Cost 版的目标是把 LocalBounded 从固定参数策略升级为 cost-driven adaptive spatial index model：

- 自动选择 per-block `beta`，即局部增量预算比例。
- 自动选择 per-block `tau`，即墓碑压实比例。
- 使用 DP block partition，根据查询代价和数据分布自适应划分 block。
- 使用 benefit-cost 规则判断是否提前 local compaction。

#### 与基础版本的核心差异

相较于 LocalBounded：

- LocalBounded 使用固定 `LOCAL_DELTA_BOUND` 和 `DELETE_COMPACT_FRACTION`。
- Cost 使用 `beta/tau` 和 workload signal 推导局部维护预算。
- Cost 可以启用 DP block partition。
- Cost 能在负载漂移或局部污染变重时调整维护策略。

#### 优势与适用场景

优势：

- 理论解释性更强。
- 能支撑论文里的统一代价模型。
- 适合用于 workload drift、skew、fat-object 或异常 block 场景。

适用场景：

- 负载漂移实验。
- 代价模型消融。
- DP block partition 是否降低 structural scan cost 的诊断实验。

#### 典型实验表现

已知结论：

- Cost/DP 能降低 `block_checks`、`visited_blocks`、`compact_records_scanned` 等 structural scan 指标。
- 当 PRL 打开后，主要瓶颈可能从 scan 转移到 PRL 后剩余 exact path、metadata scan 或内存访问，导致 Cost 端到端收益不一定超过 LocalBounded。
- 当前 Cost 不应被表述为“所有指标显著超过 LocalBounded”，更稳妥的说法是：它提供可解释的自适应机制，在负载变化和局部异常时更有论文价值。

### 3.3 `DELI_ALEX_HYBRID_SINGLE_STORE`

论文建议命名：**DELI-SingleStore**。

#### 设计目标

SingleStore 目标是解决 DELI-LB/Cost 最明确的工程短板：

```text
ALEX write layout
+ compact query overlay
= 双结构重复维护
```

SingleStore 去掉 ALEX 数据副本，让 compact query overlay 成为唯一主存储。

#### 与基础版本的核心差异

相较于 LocalBounded：

- 不再 bulk-load ALEX leaf 数据。
- insert 不再写入 ALEX。
- delete 不再从 ALEX 删除。
- memory estimate 只计算 compact overlay。
- 查询逻辑仍复用 compact blocks + local delta + tombstone + PRL。

#### 优势与适用场景

优势：

- 显著降低内存。
- 显著改善插入吞吐。
- 叙事更干净：一个 compact extent store，而不是两个重复索引。
- 更接近“新系统抽象”。

适用场景：

- 证明双结构是 DELI-LB 的主要工程短板。
- 和 Boost R-tree 比较 memory/update/query trade-off。
- 作为下一阶段主方法候选。

#### 典型实验表现

在 `ZGAP_MIXED 50k, write-heavy, PRL=1` smoke 中：

- DELI-LB memory 约 3.48 MB，SingleStore 约 0.77 MB。
- SingleStore 插入吞吐从十万级提升到百万级。
- 查询仍保持 DELI+PRL 相对 Boost R-tree 的优势。

该结果说明：SingleStore 能直接改善内存和写入短板。但当前仍需更大规模实验和 correctness oracle 验证。

### 3.4 `DELI_ALEX_HYBRID_SINGLE_STORE_COST`

论文建议命名：**DELI-SingleStore-Cost**。

#### 设计目标

SingleStore-Cost 是当前最值得推进的主方法候选：

- 用 SingleStore 消除双结构内存和写入开销。
- 用 Cost policy 保留自适应能力。
- 用 PRL 降低 GEOS exact refinement cost。

#### 与基础版本的核心差异

相较于 SingleStore：

- 增加 per-block adaptive `beta/tau`。
- 保留 Cost 版 workload-aware budget。
- 可叠加 DP block partition 和 benefit-cost compaction。

相较于 DELI-Cost：

- 不维护 ALEX 数据副本。
- 内存和插入吞吐显著改善。
- 更适合作为最终系统抽象。

#### 优势与适用场景

优势：

- 内存低。
- 插入吞吐高。
- 删除吞吐可保持较高水平。
- 具备自适应策略和论文理论叙事。

适用场景：

- 当前主方法候选。
- 与 DELI-LB、DELI-Cost、Boost R-tree、GLIN-piece 做正式动态对比。
- 做 workload drift 和 fat-object stress workload。

#### 典型实验表现

在 `ZGAP_MIXED 50k, write-heavy, PRL=1` smoke 的最终 checkpoint 中，观察到：

- `DELI-SingleStore-Cost` memory 约 0.78 MB，明显低于 DELI-LB 的 3.48 MB，也低于 Boost R-tree 的 1.73 MB。
- 插入吞吐达到百万级，显著高于 DELI-LB 和 Boost R-tree。
- 查询延迟保持在 DELI+PRL 的优势区间。

注意：该版本当前应继续做 correctness smoke、500k/2M 扩展实验和 staged workload 接入，才能作为正式论文主方法稳定呈现。

## 四、快速查阅索引

### 4.1 按论文角色查找

#### 主方法候选

- 首选候选：`DELI_ALEX_HYBRID_SINGLE_STORE_COST`  
  论文名：DELI-SingleStore-Cost 或 DELI-Cost-SingleStore。

- 稳健备选：`DELI_ALEX_HYBRID_COST` + PRL  
  论文名：DELI-Cost + PRL。适合强调 cost-driven adaptive model，但工程指标未必总胜 LocalBounded。

#### 消融基线

- 固定参数维护基线：`DELI_ALEX_HYBRID_LOCAL_BOUNDED`
- 无 Cost 的单存储消融：`DELI_ALEX_HYBRID_SINGLE_STORE`
- PRL off 消融：所有正式 DELI 版本设置 `PREDICATE_SHORTCUTS=0`
- PRL on 完整版本：设置 `PREDICATE_SHORTCUTS=1`
- DP block partition 消融：`DELI_ALEX_HYBRID_COST` 中设置 `COST_ADAPTIVE_PARTITION=0/1`

#### 对比基线

- learned baseline：`GLIN_PIECEWISE`
- traditional baseline：Boost R-tree、GEOS Quadtree
- 早期动态正确性基线：`DELI_DYNAMIC_SINGLE`

### 4.2 按性能需求查找

| 性能需求 | 推荐版本 | 说明 |
|---|---|---|
| 查询最快 | `DELI_ALEX_HYBRID_LOCAL_BOUNDED` + PRL 或 `DELI_ALEX_HYBRID_SINGLE_STORE_COST` + PRL | 具体取决于数据集和 workload；fat-object 场景下 DELI+PRL 很强 |
| 删除吞吐高 | `DELI_ALEX_HYBRID_COST` / `DELI_ALEX_HYBRID_SINGLE_STORE_COST` | lazy delete、deferred summary refresh、local compaction 有帮助 |
| 插入吞吐高 | `DELI_ALEX_HYBRID_SINGLE_STORE` 或 `DELI_ALEX_HYBRID_SINGLE_STORE_COST` | 去掉 ALEX 数据副本后插入路径明显变短 |
| 最省内存 | `DELI_ALEX_HYBRID_SINGLE_STORE` / `DELI_ALEX_HYBRID_SINGLE_STORE_COST` | 单存储只维护 compact overlay |
| 自适应最强 | `DELI_ALEX_HYBRID_COST` 或 `DELI_ALEX_HYBRID_SINGLE_STORE_COST` | 包含 `beta/tau`、Cost policy、DP block partition |
| correctness 证明最清楚 | `DELI-Dynamic` | append-only records + stable ids + conservative summaries |
| learned baseline 对比 | `GLIN_PIECEWISE` | 保留原始 learned spatial index 基线 |

### 4.3 按实验场景查找

#### 纯结构消融

目标：不依赖 PRL，比较结构层 pruning、scan、maintenance。

推荐对比：

```text
DELI_ALEX_HYBRID_LOCAL_BOUNDED, PREDICATE_SHORTCUTS=0
DELI_ALEX_HYBRID_COST, PREDICATE_SHORTCUTS=0
DELI_ALEX_HYBRID_SINGLE_STORE, PREDICATE_SHORTCUTS=0
Boost_Rtree, PREDICATE_SHORTCUTS=0
GLIN_PIECEWISE, PREDICATE_SHORTCUTS=0
```

重点指标：

- `block_checks`
- `visited_blocks`
- `compact_records_scanned`
- `delta_records_scanned`
- `mbr_candidates`
- `insert_tps`
- `delete_tps`
- `index_mb_estimate`

#### PRL 收益消融

目标：证明 PRL 是通用层，并量化其对不同索引的 GEOS exact call reduction。

推荐命令逻辑：

```text
PREDICATE_SHORTCUTS_LIST="0 1"
INDEXES="DELI_ALEX_HYBRID_LOCAL_BOUNDED DELI_ALEX_HYBRID_COST DELI_ALEX_HYBRID_SINGLE_STORE_COST Boost_Rtree GEOS_Quadtree GLIN_PIECEWISE"
```

重点指标：

- `mbr_candidates`
- `predicate_shortcuts`
- `exact_calls`
- `exact_calls / mbr_candidates`
- `avg_query_ms`
- `p95_query_ms`
- `p99_query_ms`

注意：PRL 是通用优化，不能只给 DELI 使用。论文中应强调公平开启/关闭。

#### 负载漂移实验

目标：验证 Cost policy 是否在 workload drift 下比固定参数更稳。

推荐对比：

```text
DELI_ALEX_HYBRID_LOCAL_BOUNDED
DELI_ALEX_HYBRID_COST
DELI_ALEX_HYBRID_SINGLE_STORE
DELI_ALEX_HYBRID_SINGLE_STORE_COST
Boost_Rtree
```

推荐场景：

```text
阶段 1: read-heavy, 90% query + 5% insert + 5% delete
阶段 2: balanced, 70% query + 15% insert + 15% delete
阶段 3: write-heavy, 50% query + 25% insert + 25% delete
```

重点指标：

- query latency drift
- p95/p99 query latency
- insert/delete throughput
- local compaction count/ns
- avg/max local delta size
- tombstone ratio
- adaptive beta/tau
- overall_ops_tps

#### fat-object stress 实验

目标：验证复杂 long interval / fat object 场景下 DELI + PRL 的协同收益。

推荐数据集：

```text
ZGAP_MIXED
ZGAP_WIDE
```

推荐对比：

```text
DELI_ALEX_HYBRID_LOCAL_BOUNDED
DELI_ALEX_HYBRID_SINGLE_STORE_COST
Boost_Rtree
GEOS_Quadtree
GLIN_PIECEWISE
PREDICATE_SHORTCUTS_LIST="0 1"
```

注意表述：

- 如果 PRL off 时 Boost 更快，不能声称 DELI 结构层全面优于 R-tree。
- 更准确的结论是：DELI 的 extent/block layout 与 PRL 在 fat-object 场景下有强协同，可显著降低 GEOS exact path 的端到端成本。

## 五、推荐论文命名策略

为了避免版本名过多，论文正文建议只保留少量正式名称：

| 论文中出现的名称 | 对应代码版本 |
|---|---|
| DELI-Dynamic | `DELI_DYNAMIC_SINGLE` / `DELI-Dynamic` |
| DELI-LB | `DELI_ALEX_HYBRID_LOCAL_BOUNDED` |
| DELI-Cost | `DELI_ALEX_HYBRID_COST` |
| DELI-SingleStore | `DELI_ALEX_HYBRID_SINGLE_STORE` |
| DELI-SingleStore-Cost | `DELI_ALEX_HYBRID_SINGLE_STORE_COST` |
| GLIN-piece | `GLIN_PIECEWISE` |
| PRL | `PREDICATE_SHORTCUTS=1`，不是独立索引 |

早期 `DELI-ALEX`、`Hybrid`、`Buf`、`Bounded` 版本建议放在 ablation 或 engineering evolution 小节中，不要全部放进主线贡献，否则论文会显得分散。

## 六、当前建议主线

当前最稳的研究叙事是：

```text
1. DELI-Dynamic 证明动态 extent summaries 的 exact correctness。
2. DELI-LB 证明 block-local bounded delta + tombstone + local compaction 是强固定参数动态机制。
3. DELI-Cost 给出代价驱动自适应策略，用于解释 workload drift 和局部污染。
4. PRL 作为通用保守谓词层，显著减少 GEOS exact calls。
5. DELI-SingleStore-Cost 去除双结构副本，改善内存和插入吞吐，是当前最值得推进的系统主方法。
```

