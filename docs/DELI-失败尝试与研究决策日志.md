# DELI 非点空间对象索引：失败尝试与研究决策日志

最后更新：2026-07-28

当前冻结基线：`21fe6a406283d89eaf5d7b6ef743a2e59b6c7293`

## 1. 文档目的

本项目围绕线、多边形等具有空间范围的非点对象，尝试构建一种在动态混合负载下能够
稳定超过 Boost R-tree 的 SFC/学习型空间索引。研究过程中实现并验证了大量结构、路由、
分区、表示和过滤方案，但多数方案没有达到预先设定的正确性、查询延迟、更新、内存或
研究归因门槛。

本文档记录：

1. 尝试过什么；
2. 当时希望解决什么问题；
3. 实验观察到了什么；
4. 为什么停止；
5. 哪些方向已被较强证据排除；
6. 哪些机制仍然有价值，但不应被包装成已经解决全部问题；
7. 未来重新启动某个方向前，必须先满足什么条件。

这里的“失败”主要表示 **research NO-GO**，不等于实现错误。很多原型已经通过正确性
测试，并在部分指标或特定场景中有效；停止它们是因为收益不足以覆盖结构复杂度，或者
收益无法归因于学习型/SFC 组件。

## 2. 证据状态与复现边界

代码已回退到 `21fe6a4`。回退前生成的许多 Oracle 源码、脚本、结果 CSV 和 decision 文件
已经不在当前工作树中。因此本文将证据分成三类：

- **当前可追溯**：仍记录在 `docs/` 或 Git 历史中；
- **会话结论**：实验已经完成并分析过，但原始结果随回退丢失；
- **定性结论**：只足以说明方向风险，不应用于论文中的精确数值声明。

除本文明确保留的数字外，未来若要在论文中使用任何已回退实验的结果，必须从冻结提交
或独立实验分支恢复实现，重新运行并保存：Git commit、完整命令、raw CSV、summary、
decision、机器配置和三次独立进程重复。

## 3. 最初目标与逐步暴露的核心矛盾

原始目标可以写成：

> 在不依赖一棵完整竞争 R-tree 的前提下，让基于 SFC 和一维学习索引的动态非点空间索引，
> 在低选择性、中高选择性以及读写混合负载下综合超过 Boost R-tree。

实验逐步表明，瓶颈不是单一的 `lower_bound`、模型误差或 GEOS 调用，而是以下成本共同
决定的：

```text
Geometry/MBR
  -> extent-to-1D representation loss
  -> interval overlap and access-path traversal
  -> candidate materialization
  -> second-dimensional MBR filtering
  -> exact geometry predicate
  -> dynamic maintenance and stale-state control
```

R-tree 在低选择性查询中强，是因为它在层级结构中始终联合保留 X/Y 的 MBR 约束。把 MBR
压缩成一个或多个一维 key/interval 后，模型只能更快地定位一维位置，不能自动恢复映射时
丢失的另一维信息。

## 4. 尝试总览

| 方向 | 主要假设 | 最终结论 |
|---|---|---|
| RLR-Lite ChooseSubtree | 学习插入路由可降低树重叠 | 只改 ChooseSubtree 不足，作为 baseline 保留 |
| RLR-Lite-CS-Split | 学习 split 可进一步降低 MBR overlap | 删除性能强，但未形成稳定全面优势；作为 baseline 保留 |
| HIRE-SFC-Lite/Full | 一维动态学习索引机制可直接增强 SFC extent wrapper | 功能复现成功，空间 extent 查询性能不佳 |
| DELI-Fusion Stage A-E | 融合 Cost、SingleStore、RLR/HIRE 更新机制 | 综合性能强，但主要收益来自 global Guard |
| Fusion Phase 0 路由归因 | SFC 与 Guard 持续互补 | Adaptive 大多退化为 Guard fast path |
| Guard-only | 去掉无查询贡献的 SFC 维护 | 证明核心收益接近 Base/Delta R-tree + bitmap + PRL |
| Paired Oracle | 查询级路由仍可能产生上限收益 | SFC 在部分场景有效，但低选择性明显落后 Guard |
| micro-segment | 块内摘要可显著减少扫描 | 局部扫描下降不足以跨越 Boost 差距 |
| bounded Z-cover | 多区间覆盖可减少单一长 extent 空洞 | 位置查找和区间碎片成本抵消收益 |
| learned Z-cover directory | 学习目录可消除多次位置查找 | 优化的不是主要瓶颈，未达到 gate |
| block hierarchy H0 | `max_end + MBR` 层级剪枝可替代 R-tree 部分能力 | 查询访问有改善，更新维护成本和候选下限仍然过高 |
| H0.1 lazy delete | 内部摘要 stale-high 可降低删除成本 | 删除更轻，但查询主差距未消失 |
| H1 extent overflow | 大 extent 分流可减少污染 | H1A attribution 表明 H1B 不值得实现 |
| H2 fragmentation | 一个 geometry 映射多个紧凑片段可降低空洞 | replication 与多路径访问抵消候选收益 |
| H2.1 fragment access | 更好的 fragment 访问结构可挽救 H2 | Oracle 仍未通过 |
| H3 Guard leaf access | Guard 候选可由 learned/page layout 加速 | SFC 路径仍不能超过 compact Boost |
| Cost-gated 5-box | 只在收益足够时做局部分区 | 初始 Oracle 局部可行，集成后低选择性仍落后 |
| Selective 5-box+ | causal gate 可接近 perfect partition oracle | perfect 与 causal 存在差距，生产收益不足 |
| S1/S1.1 5-box 动态版 | 直接 handle 和紧凑引用可消除更新扫描 | 更新改善，低选择性查询仍低约 3.8%--6.9% |
| S1.3 boundary oracle | 分裂边界 fanout 是剩余低选择性成本 | 可回收收益不足 5%，不值得重做目录 |
| P0 predicate refinement | 剩余差距主要来自 GEOS exact calls | 否定；主要差距在非 GEOS 访问路径 |
| E0 Empty-Space Oracle | 极稀疏查询可被廉价空结果 gate 拦截 | 安全覆盖率/收益不足，NO-GO |
| D0 4D dominance | MBR overlap 可转为四维 dominance 并高效访问 | NO-GO |
| D0.1 Fair Attribution | D0 失败可能来自不公平 Boost 输出成本 | compact-ID/count-only 后仍 NO-GO |
| P1 Packed BVH | 紧凑 workload-aware hierarchy 可超过 Boost | 工程上可能有价值，但偏离学习型/SFC 研究定位 |
| Extent-SFC S0 | 四维 endpoint-aware SFC 可改善表示 | perfect bounded-run 上限仍不足 |
| S0.1 QOPT | 粗量化正交查询直接查 run template | 表膨胀与扫描膨胀，NO-GO |
| S0.2 Adaptive Trie | 自适应 plan trie 可逼近 perfect upper plan | 仍无法达到 Boost 延迟门槛，NO-GO |
| A0 Dual-Axis | X/Y 两个区间索引加 perfect-axis 路由 | 正确但候选膨胀严重，NO-GO |

## 5. RLR 与 HIRE 基线复现

### 5.1 RLR-Lite-CS 与 RLR-Lite-CS-Split

`RLR_LITE_CS` 只学习 ChooseSubtree。它能改变插入路径，却不能直接修正 split 后产生的 MBR
重叠。在 fat-object 和 overlap-heavy workload 中，候选数主要由 split 质量决定，所以它与
Boost 的候选数接近并不意外。

`RLR_LITE_CS_SPLIT` 继续将 split candidate 纳入 RL-lite 策略。该版本在删除延迟方面是很强
的对比方法，也证明原生空间层级对动态删除具有优势。但它没有成为 DELI 的解决方案：

- 它本质仍是 R-tree family；
- split reward 的训练和候选评估增加插入成本；
- 局部 split 改善不保证在所有真实数据、选择性和负载下减少全树重叠；
- 即使它超过 DELI，也只能说明 MBR hierarchy 有效，不能证明 SFC/学习映射有效。

决策：两个版本作为 baseline 保留，不继续把它们融合进纯 SFC 主路径。

### 5.2 HIRE-SFC-Lite 与 HIRE-SFC-Full

HIRE-SFC-Full 按 Stage 1--7 复现了多层 learned internal nodes、混合叶、日志更新、成本触发、
PAP/MLS、RCU 重校准、legacy/model transformation 和 inter-level bulk loading。功能与并发
正确性工作本身是成功的。

性能不理想的原因不是某一个参数：

1. HIRE 原始目标是一维动态索引，不是非点对象 extent overlap；
2. AW 上大量叶最终成为 legacy leaf，模型叶优势有限；
3. SFC extent wrapper 需要额外 `max_zmax/MBR`，否则必须扫描长前缀；
4. 完整机制增加 shared state、日志、RCU 发布、转换和内存成本；
5. 即使加入 fanout 256、SoA、SIMD fallback、MLS-local pointer swap、subtree MBR 和 leaf
   blocks，命中记录仍要执行空间过滤；
6. Full 的查询和内存通常不如 Lite，说明一维更新机制的完整性不能自动转化为空间查询收益。

决策：HIRE-Full 是有价值的“机制迁移失败”证据和 baseline，不再作为 DELI 低选择性优化
来源。不能用“继续对齐 HIRE 参数”解释空间表示问题。

## 6. DELI-Fusion：性能成功，但归因没有通过

### 6.1 Stage A-E

Fusion 以 SingleStore-Cost-PRL 为底座，逐步加入：

- block 级访问/更新统计与模式切换；
- deleted-slot reuse；
- 局部 SpatialGuard；
- 更轻的 insert append 和 lazy delete；
- Base SpatialGuard、Delta SpatialGuard 与 live bitmap；
- budgeted recalibration 和查询路由。

这些优化曾显著降低查询、插入和删除延迟。尤其在 AW 大规模 read-heavy 实验中，现存设计
文档记录了 Fusion 平均查询延迟约比 Boost 低 8%，同时更新吞吐更高，但内存约高 56.8%。

这不是虚假结果；问题在于 **为什么快**。

### 6.2 Global Guard fast path

Global Guard 包含：

```text
immutable bulk-loaded Base R-tree
+ mutable Delta R-tree
+ live bitmap lazy deletion
+ PRL / exact refinement
```

当 warm-up 判断 SFC 较慢后，Adaptive 几乎永久切换到 Guard。此时查询接近直接执行 Boost
R-tree，而 SFC blocks、directory、object-to-block 和 delta IDs 仍可能被构建或维护。

Phase 0 增加 Adaptive、Force SFC、Force Guard、Guard-only 和 Boost 五组消融后，核心结论是：

- Guard 在低选择性场景承担主要查询；
- Guard-only 能剥离 Force Guard 中仍存在的 SFC 构建和更新成本；
- SFC path 在部分高选择性或大结果集场景有价值，但不是持续协同的主导路径；
- Fusion 的强查询结果不能主要归因于 learned/SFC directory。

因此 Fusion 不是性能失败，而是 **论文贡献归因风险**：若声称它是纯学习型空间索引，审稿人
可以合理地认为主要收益来自 R-tree Guard。

### 6.3 Paired probe 与路由

Paired Oracle/Force SFC 实验说明两条路径确有场景差异：Guard 擅长极低选择性，SFC 在较大
结果集、高重叠或顺序扫描友好场景可能更好。但当前 Adaptive 是 warm-up 后的一次性开关，
不是持续成本路由。

曾考虑加入 rolling router、Selective Guard 和 learned cardinality router，最终均未直接进入
生产版本，原因是：在训练 router 前必须先证明两条底层路径各自具有足够竞争力。后续 Oracle
反复表明 SFC 的低选择性访问路径本身不够强，router 不能把一条慢路径变快。

决策：Fusion 可以作为诚实命名的 hybrid dynamic spatial index；不能仅凭综合性能把 Guard
贡献写成 learned/SFC 贡献。

## 7. SFC 访问结构增强

### 7.1 micro-segment

假设：在 block 内按小段保存 `min_zmin/max_zmax/MBR`，先跳过不可能命中的 micro-segment，
可降低 pre-MBR records scanned。

观察：局部扫描确实可下降，但总延迟没有达到相对 Boost 的门槛。低选择性查询中，主要问题
不只在 block 内扫描，还包括前置 block/segment 访问、候选输出和另一维过滤。

结论：micro-segment 可作为局部工程优化，不是突破性访问结构。

### 7.2 bounded multi-interval Z-cover

假设：把一个长 SFC extent 替换为有限多个 Z-runs，减少曲线空洞。

观察：更多 runs 带来多次定位、去重、边界扫描和 interval fragmentation；对于大 polygon，
还会引入 replication。binary 和 learned directory 版本均未通过端到端 gate。

结论：只优化每次位置查找的 PGM/RadixSpline 不能解决 run 数量和候选物化成本。

### 7.3 block hierarchy H0 与 H0.1

H0 为 block directory 增加层级 `max_end + MBR` 剪枝；H0.1 让内部 occupancy/summary 在删除
后保持 conservative stale-high，延迟刷新祖先，以保护删除吞吐。

观察：

- H0 增加了层级剪枝能力，但没有显著降低最终 records scanned；
- H0.1 降低删除摘要维护成本，却不改变查询候选下限；
- 结构越接近联合 MBR hierarchy，越接近重新实现一棵 R-tree，同时还要承担 SFC overlay 成本。

结论：层级摘要不是无用，而是收益不足以支撑一套并行于 R-tree 的动态目录。

### 7.4 H1 large-extent overflow

H1A 先做 extent attribution + threshold oracle，测试大对象是否主导候选污染，以及将它们移入
overflow 是否存在足够上限。

观察：没有足够对象同时满足“污染大、数量少、复制/overflow 访问成本低”。阈值很难跨数据集
稳定，H1B 生产 overflow 因此没有继续实现。

结论：以后不得仅凭“大 polygon 看起来很坏”重启 overflow；必须先在 held-out workload 上
重新通过 attribution gate。

### 7.5 H2 geometry fragmentation 与 H2.1 access oracle

假设：把 geometry/MBR 分成 2/4/8 个更紧凑 fragment，可减少单一 extent 的空洞。

观察：fragment 越细，单片候选可能越少，但 replication、多入口访问、去重和动态维护同步
增长。H2.1 即使只测试更理想的 fragment access path，仍没有获得足够端到端上限。

结论：fragmentation 不是免费的表示增强。未来若没有严格 replication、update fanout 和 held-out
latency gate，不应重做。

### 7.6 H3 Guard Leaf Access Oracle

H3 测试不同 leaf/page size 的紧凑 Guard candidate access，并加入 Boost compact-ID 对比。
结果进一步说明，SFC 路径无法仅靠 page layout 超过 Boost。访问页更紧凑不能弥补表示和剪枝
结构的差异。

## 8. Selective 5-box 分区

### 8.1 Cost-gated 5-box Oracle

该方向借鉴 Adaptive Indexing of Objects with Spatial Extent：查询与不规则叶部分重叠时，沿
查询边界和最长维度最多生成五个盒子。项目没有直接实现完整动态分区，而是先测试：

- 预测扫描下降；
- replication；
- fanout；
- 分区构建和更新代价；
- perfect oracle 与 causal route 差距。

初始结果显示 AW 0.001% 的局部情况可能值得分区，而 AW 0.1% 和 ZGAP 应回退 SFC。这证明
cost gate 的思路有意义，但没有证明生产结构已经可行。

### 8.2 Selective 5-box+、S0 与 S1

后续加入更严格 gate、冻结 causal router，并实现 S1 integrated prototype。主要问题是：

- 非点对象跨多个 partition，复制率和更新 fanout 上升；
- causal route 无法达到 perfect oracle 的决策上限；
- 查询路径增加 partition probe、兄弟分支、空叶和 overflow 访问；
- 动态 split/merge 和对象迁移使删除成本恶化。

### 8.3 S1.1 update handles 与 S1.3 boundary attribution

S1.1 为对象保存稳定 leaf/overflow handles，将删除从递归扫描改成按引用失效，并压缩节点和
对象引用。更新性能得到改善，但 AW 低选择性查询相对目标仍低约：

```text
0.001%: 约 6.9%
0.01%:  约 3.8%
```

S1.3 进一步统计 boundary 引起的 sibling branch、empty leaf 和 overflow visit。结果表明，即使
理想消除这些边界开销，也不足以稳定回收预设的 5% 延迟。

结论：剩余差距不主要来自 5-box 的某一个边界实现细节。继续设计 learned leaf directory 或
更复杂 split policy 的预期回报过低。

## 9. 稀疏查询专项 Oracle

### 9.1 Predicate Refinement Oracle

曾怀疑 DELI 相对 Boost 的差距主要来自 GEOS exact predicate。对 shortcut、exact calls 和
单次 GEOS CPU 成本做归因后发现，PRL 已经能安全跳过大量 exact calls，剩余低选择性差距
主要来自非 GEOS 路径：目录、块访问、候选扫描和物化。

结论：继续围绕 GEOS refinement 优化不会解决主要问题。PRL 本身仍应保留。

### 9.2 E0 Empty-Space Oracle

E0 测试一个廉价、保守的 empty gate，并预注册了严格门槛：correctness mismatch 为 0、AW
空结果覆盖率、probe p95、复制率、更新 cell p99，以及同时报告 Boost + Empty Gate。

结果没有同时满足安全覆盖率和净收益门槛。若一种通用 empty gate 同样能加速 Boost，其收益
也不能归因于 learned index。

结论：E0 NO-GO。不得降低门槛后重新包装同一结果。

### 9.3 D0 4D Dominance 与 D0.1 Fair Attribution

MBR overlap 可写为四个不等式，因此尝试把对象编码为四维点并执行 dominance query。D0
正式结果为 NO-GO。

为排除基线不公平，D0.1 做了：

- Boost compact-ID output，不复制完整 `RTreeValue`；
- 分离 traversal 与 candidate materialization；
- count-only sink；
- 固定 `cyclic/32/8`；
- 保留原门槛，不事后放宽。

D0.1 仍为 NO-GO，说明收益不是被 Boost 输出对象的额外复制掩盖。四维 dominance access
path 本身没有显示足够优势。

### 9.4 P1 Packed BVH

P1 比较 compact Boost、heuristic packed BVH 和 workload-aware packed BVH，使用前 20% 查询
训练、后 80% held-out、无 GEOS。

该方向的重要价值是说明：直接保留 MBR hierarchy 的 packed layout 更接近问题本质。但用户
最终决定不沿 BVH 推进，因为它偏离“非点对象学习型/SFC 索引”的论文定位。

结论：P1 是 scope NO-GO，不应简单写成算法性能失败。如果未来论文转向 workload-aware
spatial hierarchy，可以重新评估；在当前 SFC 主线下停止。

## 10. Extent-aware SFC 表示 Oracle

### 10.1 S0 Extent-SFC Representation Oracle

S0 不再只编码中心点，而是编码 `(xmin, ymin, xmax, ymax)`，测试多种四维 bit-merge 曲线、
causal bounded runs 和 perfect bounded-run upper plan。

这是对“是否存在更适合 extent 的固定 SFC”最直接的上限测试。结果表明，即使 perfect plan
知道答案位置并在有限 run budget 下合并，扫描膨胀和规划/执行成本仍不足以超过 Boost。

根因是四维正交查询区域在任意一维排序上通常会碎裂成许多不连续 runs。更换 bit interleave
顺序能移动碎片，不能消除维度压缩的拓扑限制。

### 10.2 S0.1 Quantized Orthant Plan Table

S0.1 将四维查询阈值量化到粗 cell，每个 cell 离线保存保守 bounded runs，查询只计算 cell ID
并读取模板。每维 sweep 2/3/4 bit。

失败原因：

- cell 粗时，向上取整虽然保证不漏结果，却产生严重扫描膨胀；
- cell 细时，四维表大小指数增长；
- 保守模板无法同时适应同一 cell 内差异很大的稀疏查询；
- lookup 很便宜，但后续多余扫描远大于节省的 plan time。

结论：固定全局量化表无法解决 query-specific orthant fragmentation。

### 10.3 S0.2 Adaptive Perfect-Upper Plan Trie

S0.2 用自适应 trie 将 plan capacity 集中到常见查询区域，并 sweep 64/256/1024 leaf budgets。
它比完整均匀表更节省空间，但 held-out 查询仍无法稳定逼近 perfect plan，更没有跨过 Boost
延迟 gate。

结论：失败不是 QOPT 表分辨率选择不佳，而是低维 plan lookup 无法用有限状态表达大量
query-specific runs。

### 10.4 A0 Dual-Axis Interval Oracle

A0 建立两个紧凑静态区间索引：

```text
X path: [xmin, xmax]
Y path: [ymin, ymax]
perfect oracle: 免费选择候选较少的一轴
selected candidates: 再检查另一轴得到精确 MBR candidates
```

每一轴使用按 start 排序的 SoA、block `min_end/max_end` segment tree，并与 Boost compact-ID
比较，不调用 GEOS。正式结果的正确性 mismatch 为 0，三次重复稳定，但判定为
`NO_GO_DUAL_AXIS`。

回退前 summary 的关键中位数为：

| 场景 | perfect-axis 候选膨胀 | Oracle / Boost 平均延迟 |
|---|---:|---:|
| AW 0.001% | 42.67x | 3.44x |
| AW 0.01% | 18.67x | 4.01x |
| AW 0.1% | 7.92x | 4.01x |
| ZGAP 0.001% | 4.14x | 3.39x |
| ZGAP 0.01% | 1.92x | 2.85x |
| ZGAP 0.1% | 1.71x | 2.49x |

X/Y 的使用比例接近各半，perfect-axis 相比最佳固定轴确实快约 9%--37%。这说明 cardinality
router 有能力学会“哪一轴相对更少”，但即使路由完全正确且推理成本为 0，被选单轴仍缺少
另一维剪枝。

结论：不实现 learned cardinality router 和动态双索引。动态维护只会增加写入、内存和一致性
成本，不可能修复已失败的静态 perfect-oracle 上限。

## 11. 反复失败的共同根因

### 11.1 extent-to-1D 的表示损失

传统 SFC 优化点邻近性。线和 polygon 是范围对象，一个 MBR 映射为中心点、单区间、多区间、
四维 key 或 fragments 时，都必须在 false positives、fragmentation 和 replication 之间取舍。

### 11.2 剪枝能力没有免费午餐

要同时使用 `xmin/xmax/ymin/ymax`，最终需要联合多维摘要。加入 subtree MBR、5-box hierarchy、
BVH 或 Guard 后，结构逐渐重新获得 R-tree 的能力，也逐渐承担 R-tree-like 的复杂度。若同时
保留 SFC overlay，就可能付两套结构成本。

### 11.3 候选物化是不可忽略的下限

learned directory 能减少位置查找，却不能免费输出数百、数千乃至数十万个 candidate IDs。
很多方案在 records scanned 或 lower-bound 次数上改善，但 wall-clock 没有跨过 Boost。

### 11.4 Router 只能选择路径，不能修复路径

perfect/paired oracle 多次证明路径间确有互补性，但如果每条路径在关键场景都落后 Boost，
learned router 只能选择“较慢者中较快的一个”。应先验证 path upper bound，再训练模型。

### 11.5 动态维护会吞掉静态收益

fragment、partition、双轴、multi-run 和局部 hierarchy 都需要处理 insert/delete、object owner、
split/merge、replication、stale summary 和 compaction。静态 Oracle 只略有优势时，生产动态版
几乎不可能保住收益。

### 11.6 强基线必须公平

D0.1 和 Guard-only 的最大价值，是防止把完整 value copy、通用 empty gate、PRL 或 Base/Delta
R-tree 的收益误认为 learned access path 的收益。Boost 必须使用 compact-ID、相同 predicate
语义和相同输出 sink。

## 12. 不应立即重复的方向

除非新的 Oracle 明确改变了假设，否则不应重新投入：

1. 只替换 binary search/lower_bound 的 learned directory；
2. 继续枚举 Hilbert/Z-order/四维 Morton 的 bit permutation；
3. 在没有 replication gate 时增加 fragments 或 Z-runs；
4. 在底层路径没有通过 static upper bound 前训练 router；
5. 通过放宽已看到结果后的 gate 挽救 NO-GO；
6. 把完整 R-tree Guard 隐藏在“学习型索引”名称中；
7. 继续调 HIRE 的一维参数来解释 extent overlap；
8. 只报告 visited blocks 下降，而不报告 wall-clock、candidate materialization 和 memory；
9. 在 `CHECK_CORRECTNESS=0` 的大实验上作最终正确性声明。

## 13. 仍然值得保留的资产

### 13.1 Exact-safe PRL

矩形 query 包含 object envelope 时的 positive shortcut 是正确且有效的。PRL 降低 GEOS exact
cost，不依赖某一种索引，可作为所有方法的统一执行层和消融项。

### 13.2 DELI-SingleStore-Cost

SingleStore 消除了 ALEX write layout 与 compact query overlay 的双份维护，具有低内存和高
插入吞吐。它不保证低选择性超过 Boost，但仍是 SFC/learned family 中重要的主线 baseline。

### 13.3 DELI-Fusion 的 hybrid 系统价值

Base/Delta Guard + live bitmap + PRL + update-light maintenance 是有效的动态系统组合。它可以
作为 hybrid spatial index 研究，但必须诚实报告：

- Guard 来源；
- SFC 的独立贡献；
- memory trade-off；
- stale/delta 长期退化；
- route/update 消融。

### 13.4 高选择性和大结果集下的 SFC 优势

实验并未证明 SFC 在所有场景无效。顺序布局、批量扫描和更新轻量化在中高选择性、大结果集
或写密集场景仍可能超过 Boost。应收缩 claim，而不是把“低选择性未胜”解释成所有工作无效。

### 13.5 Oracle-first 研究流程

本项目已经形成一套比继续堆代码更有价值的方法：

```text
representation upper bound
  -> access-path oracle
  -> fair compact baseline
  -> held-out validation
  -> fixed gate
  -> only then dynamic integration
```

这些 NO-GO 结果避免了实现更多无上限的动态结构。

## 14. 当前建议的研究收敛方式

### 14.1 若继续写 DELI-Fusion 系统论文

定位为：

> Predicate-cost-aware hybrid indexing for dynamic non-point spatial objects.

核心贡献强调 query/update/memory trade-off、Base/Delta update-light maintenance、PRL 和对不同
访问路径适用区间的系统刻画。不要声称它是一条纯 learned SFC 路径全面超过 R-tree。

### 14.2 若坚持学习型空间索引论文

将目标收缩为：

- 中高选择性；
- 大结果集；
- 写密集或动态漂移；
- 相对 GLIN/HIRE-SFC/DELI-Cost 的改进；
- 明确承认极低选择性由原生 MBR hierarchy 占优。

论文贡献可以是“识别并解释非点对象 learned indexing 的适用边界”，而不是承诺所有场景全面
超过 Boost。

### 14.3 若重新寻找新结构

新方向至少应直接保留联合二维空间关系，而不是再次把四个 MBR 端点压成一个单调 key。
但在写生产代码前，仍必须先做静态、无 GEOS、compact-ID 的 held-out Oracle。

## 15. 未来新想法的强制验收清单

任何新方案在进入动态实现前必须回答：

1. **表示上限**：perfect oracle 的 candidate inflation 是否足够低？
2. **访问上限**：无 GEOS、compact-ID、count-only 和 materialized sink 下是否均有优势？
3. **公平基线**：同一 query、同一输出语义、同一 warm-up、同一 allocator/capacity 策略？
4. **归因**：Boost 加同一个通用组件后是否得到相同收益？
5. **泛化**：前 20% train、后 80% held-out 是否仍通过？
6. **稳定性**：是否至少三次隔离进程取中位数？
7. **正确性**：candidate mismatch 是否严格为 0？
8. **内存**：是否包含 directory、metadata、replication、locator 和 scratch state？
9. **更新下限**：每次 insert/delete 需要修改多少 entries、cells、fragments 或 indexes？
10. **预注册 gate**：是否在看到正式结果前写下 GO/NO-GO 门槛？

只要 static perfect oracle 失败，就停止 learned router、后台维护和动态双结构实现。

## 16. 已回退实验目录索引

以下目录名来自实验会话，当前工作树中可能已不存在，仅用于未来从备份或 shell history 定位：

```text
results/deli_fusion_phase0_routes_r
results/deli_fusion_phase0_routes_b
results/deli_fusion_phase0_routes_w
results/deli_fusion_phase1_paired_aw
results/deli_fusion_phase2_sfc
results/deli_fusion_h0
results/deli_fusion_h01
results/deli_fusion_h1a
results/deli_fusion_h2_fragmentation
results/deli_fusion_h21_fragment_access
results/deli_fusion_h3_guard_leaf
results/deli_fusion_force_sfc_vs_boost
results/deli_cost_gated_5box_oracle
results/deli_selective_5box_plus_oracle
results/deli_d0_dominance_4d
results/deli_d01_fair_attribution
results/deli_p1_packed_bvh
results/deli_extent_sfc_s0
results/deli_extent_sfc_s01_qopt_aw_causal
results/deli_extent_sfc_s02_adaptive_aw
results/deli_dual_axis_a0
```

## 17. 最终结论

这些尝试没有证明“非点对象学习型索引永远无法超过 R-tree”。它们更具体地证明了：

1. 仅优化一维定位不能解决 extent overlap；
2. 单区间、多区间、fragment、四维 SFC、双轴和有限 plan table 都未在当前数据与门槛下提供
   足够的低选择性上限；
3. 能稳定改善低选择性查询的机制，往往重新引入联合 MBR hierarchy；
4. DELI-Fusion 的综合性能是真实的，但主要查询收益需要归因给 Guard；
5. 当前最理性的选择是收缩论文 claim、保留有效资产，并停止重复实现已被 Oracle 排除的方向。

负结果的价值在于缩小研究空间。本文档的用途不是证明项目没有希望，而是确保下一次投入
建立在一个真正改变旧假设的新观察上，而不是再次为同一种表示损失更换实现形式。
