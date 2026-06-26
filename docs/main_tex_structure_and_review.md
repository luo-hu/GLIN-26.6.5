# main.tex 结构组织与预投稿审查

最后更新：2026-06-25

本文档根据 `tech-paper-template` 和 `pre-submission-reviewer` 对 `manuscript/pvldbstyle-master/main.tex` 做结构整理和审查。当前已经把论文主线收敛为：

```text
DELI-Cost 是主方法；
DELI-LB 是固定参数消融基线；
PRL 是可叠加到 DELI-LB / DELI-Cost 上的谓词精化层。
```

## 1. Tech-paper-template 结构定位

### 1.1 Paper-type positioning

- Type: Technique Paper with New Setting framing.
- Rationale: 查询语义 `Intersects` 和 baseline 都是已有空间数据库问题，但本文的新技术点是把动态 learned ordered index 扩展到复杂 geometry exact predicate，并在动态更新下维护 extent summaries 与 predicate refinement policy。

### 1.2 Thinking template

| Stage | Your content |
|---|---|
| Research background | 复杂 polygon、linestring、道路、水体和公园对象广泛存在于 GIS、城市分析、遥感和移动计算中。这些对象需要 exact spatial relationship query，而不能只返回近似候选。传统 R-tree/Quadtree 支持 filter-and-refine，但不是 learned ordered layout；learned indexes 紧凑且更新友好，但多数面向点或一维 range；GLIN 把 learned index 引入复杂 geometry，但采用 Zmin-centric 表示。 |
| Limitation 1 | Zmin-centric learned spatial indexes 存在 extent blindness：索引只看到一维排序 key，却没有把 `Zmax` 和 MBR 作为可维护信息。 |
| Limitation 2 | 动态更新后，若辅助 summary、query augmentation 或 block statistics 不能保守维护，就会在 correctness 和 update throughput 之间失衡。 |
| Limitation 3 | 当结构层剪枝已经有效时，瓶颈会转移到 GEOS/JTS exact predicate refinement；继续调 block size 或 delta bound 只能带来有限收益。 |
| Key Idea / Our Goal | DELI-Cost treats each complex geometry as a dynamic spatial extent and optimizes a conservative predicate execution policy that jointly controls structural scanning, dynamic maintenance, and exact predicate refinement. |
| Challenge 1 | 如何把复杂 geometry 表示为 learned ordered index 可以维护的 extent entry，并保证 block pruning 在 insert/delete/compaction 后仍然 false-negative-free。 |
| Challenge 2 | 如何让动态维护局部化，避免全局重排、global delta 膨胀或 foreground full compaction。 |
| Challenge 3 | 如何在不引入 false negatives 或 false positives 的前提下减少 GEOS exact calls。 |
| Methodology topic sentence | DELI-Cost is a cost-driven dynamic learned spatial index with conservative extent maintenance and predicate-aware refinement. |
| Module A | Extent entry and conservative summaries: use `<Zmin, Zmax, MBR, oid>` and one-sided conservative block summaries to make learned layout range-aware. |
| Module B | Cost-driven local bounded maintenance: use adaptive block partition, beta/tau budgets, local delta, tombstones, and local compaction to keep update cost local. |
| Module C | PRL predicate refinement layer: place conservative accept/reject predicates before GEOS to reduce exact refinement calls without changing the answer set. |
| Contribution 1 | 提出 extent blindness 视角、extent entry 抽象和 one-sided conservative invariant。（Sections 2--4） |
| Contribution 2 | 提出 DELI-Cost 动态主索引结构与 cost-driven local bounded maintenance。（Sections 5--10） |
| Contribution 3 | 提出 PRL 保守谓词精化层，并给出 exact correctness 条件和谓词排序代价分析。（Section 7） |
| Contribution 4 | 在 staged/mixed/drift workloads 下与 GLIN-piecewise、Boost R-tree、GEOS Quadtree 对比，并报告 correctness、latency、throughput、memory 和 exact-call reduction。（Sections 11--13） |

### 1.3 Self-consistency checks

- Check 1 Limitations -> Key Idea: pass. `extent blindness`、动态维护和 GEOS refinement 都被 DELI-Cost + PRL 覆盖。
- Check 2 Key Idea -> Challenges: pass. 三个挑战自然来自实现 cost-driven conservative predicate execution policy。
- Check 3 Challenges -> Methodology: pass. Extent summaries、local bounded maintenance、PRL 分别对应三个挑战。
- Check 4 Methodology -> Contributions: pass with caveat. 贡献已经覆盖三大模块，但实验贡献需要最终 CSV 和图表支撑。

### 1.4 Severity summary

- CRITICAL: 0 for logic skeleton after current rewrite.
- MAJOR: 2 remaining risks.
- MINOR: 4 polish issues.

Top three fixes:

1. 用最终实验数字替换 “初步实验表明” 和 “当前证据仍有不足”。
2. 把 `需要补充的算法、结构图、图表和数据` 移到 internal plan 或 appendix，不应留在投稿正文。
3. 把中文草稿改成英文，且保证 section names 与 contribution names 一致。

## 2. Pre-submission-reviewer 审查

### Summary

- CRITICAL: 3
- MAJOR: 8
- MINOR: 7
- Submission recommendation: Needs major revision before submission.

当前稿子已经有清楚主线，但仍是中文技术草稿，不是可投稿版本。最大问题不是方法方向，而是实验证据、图表、正文施工清单和最终英文写作。

### Dimension 1: Macro logic

| # | Finding | Severity | Suggested fix |
|---|---|---|---|
| 1 | `本节目前只写可防守的阶段性结论，投稿前必须用最终 CSV 和图表替换成具体数字。` | CRITICAL | Section 13 不能作为最终论文结果。用最终实验表、图和具体数字替换该段；若实验未完成，不能投稿。 |
| 2 | `本节是中文草稿的施工清单。投稿前可删除，或改写为实验计划和 appendix roadmap。` | CRITICAL | 把该 section 移出正文，放入 docs 或 appendix plan。正文必须只保留已完成算法、图表和实验。 |
| 3 | `当前内存指标中的 index_mb_estimate 是估算值，只能用于判断数量级。` | MAJOR | 若论文 claim memory advantage，必须补 peak RSS 或严格 memory accounting。否则只能写 “estimated memory footprint”。 |
| 4 | `\DELICost 是本文主方法；\DELILB 是固定参数版本，用作消融基线；\PRL 是正交的 predicate refinement layer` | MINOR | 主线已经清楚。建议 Figure 1 和 Table 1 也使用完全相同的三个名称，避免后文再出现旧版本名。 |

### Dimension 2: Writing details

| # | Finding | Severity | Suggested fix |
|---|---|---|---|
| 1 | Abstract 有三大段，每段信息密度很高，例如 `系统将 ALEX-style write layout 与 compact query blocks 解耦...` | MAJOR | 改成 VLDB abstract 的五句式：问题、缺口、挑战、方法、结果。最终必须给具体数字。 |
| 2 | `当前最稳妥的结论是：DELI-Cost 不是 R-tree 的万能替代品...` | MAJOR | 这是讨论性文字，不适合作为 Conclusion 的核心结尾。改成技术结论和实验发现，再把边界条件放到 Discussion。 |
| 3 | 多处出现 `当前`、`正式论文`、`投稿前`。 | MAJOR | 这些是内部施工语气。正文投稿版必须删除或改为 paper voice。 |
| 4 | `本文不主张 DELI 全面替代这些结构` 在 Related Work 中出现。 | MINOR | 可以保留克制态度，但建议移到 Discussion，Related Work 中重点写技术差异。 |

### Dimension 3: English grammar

当前主体是中文草稿，因此英语语法审查只能覆盖英文混写片段。

| # | Finding | Severity | Suggested fix |
|---|---|---|---|
| 1 | `a dynamic learned spatial index with conservative extent maintenance and predicate-aware refinement` 这类英文句子可作为最终英文主线。 | MINOR | 英文版保留该句，但统一术语大小写。 |
| 2 | `query augmentation`、`exact predicate refinement`、`foreground update throughput` 等英文术语混在中文正文里。 | MINOR | 英文投稿版中统一改写；中文草稿阶段可以保留。 |
| 3 | 未来英文版需重点检查 G1 article usage 和 G4 sentence complexity。 | MINOR | 尤其注意 “a learned spatial index”、 “an extent entry”、 “a conservative predicate”。 |

### Dimension 4: LaTeX format

| # | Finding | Severity | Suggested fix |
|---|---|---|---|
| 1 | `latexmk -xelatex -interaction=nonstopmode main.tex` 编译通过，生成 `main.pdf`。 | pass | 编译链路可用。 |
| 2 | BibTeX warnings: `page numbers missing` for `pandey2020learnedspatial` and `wang2022glin`，以及若干 `empty address`。 | MAJOR | 补全 `deli_refs.bib` 的 venue、volume、number、pages、publisher/address。 |
| 3 | 多个 Overfull hbox，例如 line 696 附近 `PREDICATE_SHORTCUTS=0/1`。 | MINOR | 英文化后大多会消失；若仍存在，使用 shorter text、`\allowbreak` 或表格换行。 |
| 4 | `\fbox` 和 tabular 伪图用于 Figure 1。 | MAJOR | 投稿版必须替换为矢量结构图，不要用占位框。 |
| 5 | 算法用 `figure + tabular` 伪代码。 | MINOR | 可接受作草稿；正式版建议用 `algorithm2e` 或 ACM 允许的算法环境。 |

### Dimension 5: Figure quality

| # | Finding | Severity | Suggested fix |
|---|---|---|---|
| 1 | Figure 1 caption 写着 `正式版本应替换为矢量图`。 | MAJOR | 必须绘制真正的系统图：Geometry table、ALEX write layout、compact blocks、local delta/tombstones、Cost policy、PRL、GEOS。 |
| 2 | 结果图还没有内嵌到 paper。 | CRITICAL | 至少需要 main comparison、ablation、predicate refinement breakdown、mixed workload drift、memory 五类图。 |
| 3 | `Figure 10: Predicate refinement breakdown` 已在计划中定义。 | pass | 这是当前最重要的结果图，建议优先做。 |

### Banned-vocabulary and em-dash scan

Full scan scope: entire `main.tex`.

- Unicode em-dash count: 0.
- Banned AI-tone vocabulary from skill list: 0 hits.
- Citation spacing scan: no `\cite{}` without preceding `~` found.
- Reference spacing scan: no `\ref{}` without preceding `~` found.

### Integrity gate result

- Gate 1: pass. Findings quote actual lines/phrases.
- Gate 2: pass. Every CRITICAL finding has a concrete fix.
- Gate 3: pass. No fabricated quotes.
- Gate 4: pass. Severity follows taxonomy.
- Gate 5: partial. Grammar review is limited because current draft is Chinese.
- Gate 6: pass. Banned-vocabulary scan was run on entire `main.tex`.
- Gate 7: pass. Final score matches CRITICAL + MAJOR count.

### Final score

Current draft score: 5/10.

Reason: method story is now coherent, but the paper is not submission-ready because final experimental evidence, real figures, memory measurement, bib cleanup, and English rewrite are still missing.

## 3. Mainline decision

让 \DELICost 作为主线是更好的选择。

原因：

1. \DELICost 更符合论文贡献表达。它不是“固定参数工程实现”，而是 cost-driven adaptive spatial index model。
2. \DELILB 的价值仍然很高，但更适合作为消融基线。它能证明 local bounded maintenance 本身有效，也能让 Cost 的收益有参照。
3. \PRL 不应该变成另一个版本名。它是可叠加层，应通过 `PREDICATE_SHORTCUTS=0/1` 做 on/off ablation。
4. 最新实验表明，Cost-DP 主要减少 scan cost，而 PRL 主要减少 GEOS cost。二者解决不同瓶颈，不能互相替代。

推荐最终方法命名：

```text
Main method: DELI-Cost + PRL
Structural ablation: DELI-LB + PRL
Predicate ablation: DELI-Cost without PRL
Fixed baseline: DELI-LB without PRL
```

论文正文可以简称主方法为 `DELI-Cost`，并说明 PRL 是默认启用的 conservative predicate refinement layer。

## 4. 下一步修稿顺序

1. 先补最终实验矩阵：AW/LW/PARKS/ROADS/ZGAP_MIXED，0.1% 和 1% 至少先完整跑，`CHECK_CORRECTNESS` 抽样开启。
2. 做四组 ablation：`DELI-LB`、`DELI-Cost`、`DELI-LB+PRL`、`DELI-Cost+PRL`。
3. 补真实内存测量，至少 peak RSS + estimated index bytes 两列。
4. 把 Section “需要补充的算法、结构图、图表和数据” 移出投稿正文。
5. 根据最终图表重写 Abstract 和 Introduction 结果句。
6. 最后统一英文改写。
