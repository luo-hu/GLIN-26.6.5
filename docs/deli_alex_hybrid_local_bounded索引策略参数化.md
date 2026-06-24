学习型尺寸分离策略，它的核心思想不是“手动试参数”，而是：**先把索引策略参数化，再定义查询代价函数，再用 BO/GP 或近似代价模型快速寻找较优配置**。文档里把查询代价拆成过滤成本和扫描成本，并进一步指出真正和配置有关的主要是扫描假阳结果的代价；它还说明直接网格搜索复杂度高，所以用贝叶斯优化和 GSM 近似代价函数来快速重配置。

你这个思路可以借鉴，但不能直接照搬。尺寸分离策略优化的是“不同 MBR 尺寸对象如何分区”，而 DELI 要优化的是：

```text
Zmin 有序空间中如何划分 block；
每个 block 允许多少 delta；
每个 block 允许多少 tombstone；
什么时候 local compaction；
什么时候 split / merge block。
```

所以我建议把 DELI 升级成一个新的模型：

```text
DELI-Cost:
Cost-driven Adaptive Dynamic Extent-aware Learned Index
基于代价模型驱动的自适应动态范围感知学习索引
```

下面我把它整理成可以写进 VLDB 方法章节的版本。

---

# 1. 先把术语讲清楚

## 1.1 block：块 / 查询块

`block` 中文叫：

```text
块
```

在 DELI 里，所有对象先按 `Zmin` 排序，然后连续分成一段一段。每一段就是一个 block。

例如：

```text
block 0: 第 0 ~ 511 个对象
block 1: 第 512 ~ 1023 个对象
block 2: 第 1024 ~ 1535 个对象
```

每个 block 维护自己的摘要：

```text
min_zmin      这个 block 里最小的 Zmin
max_zmax      这个 block 里最大的 Zmax
block_mbr     这个 block 里所有对象 MBR 的并集
live_count    当前有效对象数
```

查询时先看 block summary。如果整个 block 不可能命中，就直接跳过。

---

## 1.2 block size：块大小

`block size` 中文叫：

```text
块大小
```

意思是：

```text
一个 block 里大约放多少条对象记录。
```

你现在默认：

```text
BLOCK_SIZE = 512
```

意思是每个 compact block 大约放 512 条记录。你之前的文档也把它解释成查询扫描局部性和维护成本之间的平衡。

但是在更强的理论版本里，block size 不应该必须固定。它可以变成：

```text
b_min <= |B| <= b_max
```

也就是说，每个 block 大小可以不同，由代价模型自动决定。

---

## 1.3 compact：紧凑主区 / 已整理区

`compact` 中文可以叫：

```text
紧凑主区
```

或者：

```text
已整理区
```

它的意思不是“压缩文件”，而是：

```text
这一部分记录已经按 Zmin 排好序，
没有空洞，
没有过多 dead records，
适合顺序扫描。
```

你可以把一个 block 想成：

```text
Block B
 ├── compact part：正式区，排好序，查询快
 ├── delta part：临时区，新插入对象先放这里
 └── tombstone：已删除但还没物理清理的记录
```

---

## 1.4 compaction：局部压实 / 局部整理

`compaction` 中文叫：

```text
压实
```

在 DELI 里更准确叫：

```text
局部压实
```

它做的事情是：

```text
1. 删除 tombstone 对象；
2. 把 delta 里的新对象合并进 compact 主区；
3. 按 Zmin 重新排序；
4. 重算 block summary。
```

你的文档里也明确说，local compaction 只重建受影响的一个 block，不全局重建整个索引；它会删除 dead ids、合并 compact_ids 和 delta_ids、保持 Zmin 有序、重算 block summary。

---

## 1.5 delta：局部增量缓存

`delta` 中文叫：

```text
增量缓存
```

或者：

```text
局部缓存
```

插入新对象时，不马上把它插入到 compact 主区中间，因为这会搬移很多元素。DELI 先把它 append 到目标 block 的 local delta 里。

```text
insert object g
→ 找到对应 block B
→ append 到 Δ_B
```

其中：

[
\Delta_B
]

表示 block (B) 的 local delta。

---

## 1.6 β：局部增量比例

[
\beta
]

读作 beta，中文叫：

```text
局部增量比例
```

定义为：

[
\beta_B = \frac{|\Delta_B|_{\max}}{|C_B|}
]

其中：

```text
Δ_B = block B 的局部 delta
C_B = block B 的 compact 主区
```

如果：

```text
|C_B| = 512
β_B = 0.25
```

那么：

```text
这个 block 最多允许 512 × 0.25 = 128 条 delta 记录。
```

β 控制的是：

```text
插入速度 vs 查询速度
```

β 越大：

```text
delta 允许更大；
插入更快；
compaction 更少；
但查询要多扫 delta。
```

β 越小：

```text
delta 更小；
查询更干净；
但 compaction 更频繁；
插入更慢。
```

---

## 1.7 τ：墓碑比例 / 延迟删除比例

[
\tau
]

读作 tau，中文叫：

```text
墓碑比例
```

或者：

```text
延迟删除比例
```

删除对象时，不马上物理删除，而是先标记：

```text
alive = false
```

这个被标记的 dead record 就是：

```text
tombstone
墓碑记录
```

定义：

[
\tau_B = \frac{|T_B|_{\max}}{|C_B|}
]

其中：

```text
T_B = block B 中允许保留的 tombstone 数
```

如果：

```text
|C_B| = 512
τ_B = 0.25
```

那么最多允许：

```text
512 × 0.25 = 128 条 tombstone。
```

τ 控制的是：

```text
删除速度 vs 查询速度
```

τ 越大：

```text
删除更快；
但查询可能扫到更多 dead records。
```

τ 越小：

```text
查询更干净；
但删除时 local compaction 更频繁。
```

---

# 2. 你现在真正需要升级的是：从 DELI-Fixed 到 DELI-Cost

你现在实现的是：

```text
DELI-Fixed / DELI-LocalBounded

BLOCK_SIZE = 512
LOCAL_DELTA_BOUND = 128
DELETE_COMPACT_FRACTION = 0.25
```

它已经有结构意义：每个 block 最多容忍 25% local delta 和 25% tombstone。你之前文档也把 LocalBounded 定位成当前最合适的动态主方法，并说明它不是追求单项最强，而是同步、单线程、公平计成本的平衡版本。

但要冲 VLDB，最好再提出一个更理论化版本：

```text
DELI-Cost
```

它不再固定：

```text
b = 512
β = 0.25
τ = 0.25
```

而是自动选择：

```text
block partition P
每个 block 的 β_B
每个 block 的 τ_B
split / merge / compaction 触发时机
```

这样论文故事就从：

```text
我们选了一组好参数
```

变成：

```text
我们提出一个代价模型，并用算法自动决定分区和维护策略。
```

---

# 3. 统一优化模型

我们把 DELI 的参数写成：

[
\Theta = (P, {\beta_B}, {\tau_B})
]

其中：

```text
Θ = DELI 的配置
P = block 划分方式
β_B = block B 的局部增量比例
τ_B = block B 的墓碑比例
```

目标是最小化总代价：

[
J(\Theta)
=========

w_q \cdot \mathbb{E}[T_q(\Theta)]
+
w_i \cdot \mathbb{E}[T_i(\Theta)]
+
w_d \cdot \mathbb{E}[T_d(\Theta)]
+
w_m \cdot \mathbb{E}[T_m(\Theta)]
+
w_s \cdot Space(\Theta)
]

中文解释：

```text
T_q = 查询代价
T_i = 插入代价
T_d = 删除代价
T_m = 维护代价
Space = 空间开销
w_q, w_i, w_d, w_m, w_s = 权重
```

如果你的工作负载是：

```text
90% query
5% insert
5% delete
```

可以设：

```text
w_q = 0.90
w_i = 0.05
w_d = 0.05
```

如果是写多读少系统，则提高 (w_i) 和 (w_d)。

这一步很重要，因为“最佳参数”必须相对于某个目标函数才有意义。没有 workload 权重，就不存在唯一最佳参数。

---

# 4. 查询代价模型：借鉴“尺寸分离策略”的过滤 + 扫描思想

你上传的《学习型尺寸分离策略》里有一个很好的思想：查询代价可以拆成：

```text
过滤成本 + 扫描成本
```

并进一步把扫描成本拆成：

```text
扫描真阳结果的成本 + 扫描假阳结果的成本
```

文档里还指出，给定查询和数据后，真阳结果扫描成本基本不随配置变化，真正需要优化的是假阳扫描成本。

DELI 可以借用这个思想。

对 query (q)，DELI 查询代价写成：

[
T_q =
T_{probe}
+
T_{block}
+
T_{scan}
+
T_{exact}
]

其中：

```text
T_probe = 定位 block 的代价
T_block = 检查 block summary 的代价
T_scan = 扫描 compact / delta / tombstone 的代价
T_exact = GEOS exact intersects 的代价
```

对一个 block (B)，如果它不能被剪枝，则需要扫描：

```text
compact 主区
delta 增量区
tombstone 过滤
```

所以可以写：

[
T_q(B)
======

I(B,q)
\cdot
\left[
c_b
+
c_s \cdot S(B,q)
+
c_g \cdot G(B,q)
\right]
]

其中：

```text
I(B,q) = 1 表示 block B 会被 query q 访问；否则为 0
c_b = 访问一个 block 的固定成本
c_s = 扫描一条记录的成本
S(B,q) = query q 在 block B 中扫描的 entry 数
c_g = 一次 GEOS exact predicate 的成本
G(B,q) = query q 在 block B 中触发的 GEOS exact 次数
```

一个 block 之所以被访问，是因为下面三个剪枝条件都没能跳过它：

[
B.maxZmax \geq q.Zmin
]

[
B.minZmin \leq q.Zmax
]

[
B.MBR \cap q.MBR \neq \emptyset
]

所以：

[
I(B,q)=
\mathbf{1}
[
B.maxZmax \geq q.Zmin
\land
B.minZmin \leq q.Zmax
\land
B.MBR \cap q.MBR \neq \emptyset
]
]

总代价：

[
Cost_q(B)
=========

\sum_{q \in Q_{cal}}
T_q(B)
]

其中 (Q_{cal}) 是校准查询集。

---

# 5. 自适应 block 划分：从固定 512 改成动态规划最优分区

现在固定：

```text
每 512 条切一个 block
```

理论上不够强。更好的版本是：

```text
所有 extent entries 按 Zmin 排序后，用动态规划自动决定切分位置。
```

设所有 entries 按 Zmin 排序：

[
e_1, e_2, ..., e_n
]

一个 block 必须是连续片段：

[
B(i,j)={e_i,e_{i+1},...,e_j}
]

为什么必须连续？

因为 DELI 底层是 Zmin ordered learned index。连续 block 能保证：

```text
模型定位简单；
顺序扫描高效；
block summary 有明确语义。
```

---

## 5.1 候选 block 代价

对一个候选 block (B(i,j))，定义：

[
Cost(i,j)
=========

Cost_q(i,j)
+
Cost_u(i,j)
+
Cost_s(i,j)
]

其中：

```text
Cost_q = 查询代价
Cost_u = 更新/维护代价
Cost_s = 空间代价
```

约束：

[
b_{min} \leq j-i+1 \leq b_{max}
]

比如：

```text
b_min = 128
b_max = 2048
```

---

## 5.2 动态规划算法

```text
Algorithm 1: Cost-driven Adaptive Block Partitioning
算法 1：基于代价模型的自适应块划分

Input:
  E = sorted extent entries e1...en
  Qcal = calibration queries
  Ucal = calibration updates
  bmin, bmax

Output:
  P* = adaptive block partition

1. Sort E by Zmin.
2. For every segment B(i,j) with bmin <= j-i+1 <= bmax:
       estimate Cost(i,j)
3. DP[0] = 0
4. For j = 1 to n:
       DP[j] = min over i:
               DP[i-1] + Cost(i,j)
       subject to bmin <= j-i+1 <= bmax
5. Recover split points by backtracking.
6. Return P*
```

递推公式：

[
DP[j]
=====

\min_{i:\ b_{min} \leq j-i+1 \leq b_{max}}
\left(
DP[i-1]+Cost(i,j)
\right)
]

最终：

[
P^* = DP[n]
]

---

## 5.3 Theorem 1：自适应 block 划分最优性

**定理 1：在给定代价模型下，Algorithm 1 返回最优连续 block 划分。**

假设：

1. entries 已按 Zmin 排序；
2. 每个 block 必须是连续片段；
3. 总代价是各 block 代价之和；
4. block 大小满足 (b_{min} \leq |B| \leq b_{max})。

则 Algorithm 1 返回所有合法连续划分中总代价最小的划分。

**证明思路：**

考虑最优解的最后一个 block。假设它是：

[
B(i,j)
]

那么前缀：

[
e_1,\ldots,e_{i-1}
]

也必须是最优划分。否则可以用更优前缀替换它，使总代价更小，和最优性矛盾。因此最优解满足：

[
OPT[j]
======

\min_i
OPT[i-1]+Cost(i,j)
]

这正是 DP 递推式。所以算法返回全局最优连续划分。

这个定理能回答审稿人：

> block 是不是拍脑袋划的？

回答：

> 不是。DELI-Cost 把 block partition 建模为一维有序分段优化问题，并用动态规划求解给定代价模型下的最优划分。

---

# 6. 代价估计不能太慢：借鉴 GSM 近似思想

动态规划需要计算很多 (Cost(i,j))。如果每个候选 block 都跑真实 query，那太慢。

你上传的尺寸分离策略文档也遇到了类似问题：原始代价函数复杂，不能满足快速重配置，所以它用 GSM 设计近似代价函数。GSM 的思想是把 MBR 尺寸映射到网格，每个网格单元维护平均 size 和对象数量，再用近似公式快速估计查询成本。

DELI 可以借用这个思想，设计：

```text
Z-GSM: Z-order Grid Summary Map
Z 序网格摘要图
```

每个 Z-order 桶维护：

```text
count          对象数量
avg_zspan      平均 Z 区间跨度
avg_mbr_area   平均 MBR 面积
max_zmax       最大 Zmax
mbr_union      MBR 并集近似
query_hits     校准查询访问次数
insert_hits    校准插入命中次数
delete_hits    校准删除命中次数
```

这样候选 block (B(i,j)) 的查询访问频率、假阳估计、更新频率都可以快速估计，不需要每次真实跑 GEOS。

这就对应你文档里的思路：

```text
真实代价太贵；
用统计摘要构造近似代价；
再用 BO 或 DP 优化配置。
```

---

# 7. β 和 τ 的自动推导：不是枚举参数，而是闭式最优

现在解决你最关心的问题：

```text
β 和 τ 不是手动选，而是自动算。
```

---

## 7.1 β 的代价函数

β 控制 local delta。

对 block (B)，设：

```text
q_B = block B 被查询访问的频率
i_B = 插入落到 block B 的频率
|B| = block 大小
c_s = 扫描一条 entry 的成本
c_c = local compaction 单位成本
```

当 β 变大，delta 变大，查询会多扫 delta。
delta 从 0 增长到 (\beta |B|)，平均大小约为：

[
\frac{\beta |B|}{2}
]

所以 query 增量代价约为：

[
A_B \beta
]

其中：

[
A_B = \frac{1}{2}q_B c_s |B|
]

另一方面，β 越小，compaction 越频繁。插入维护代价近似为：

[
\frac{B_B}{\beta}
]

其中：

[
B_B = i_B c_c
]

所以：

[
J_B(\beta)
==========

A_B\beta
+
\frac{B_B}{\beta}
]

---

## 7.2 β 的闭式最优解

求导：

[
\frac
=====

A_B-\frac{B_B}{\beta^2}
]

令导数为 0：

[
A_B=\frac{B_B}{\beta^2}
]

得到：

[
\beta_B^*
=========

\sqrt{\frac{B_B}{A_B}}
]

再加上下界和上界：

[
\beta_B
=======

clip
\left(
\sqrt{\frac{B_B}{A_B}},
\beta_{min},
\beta_{max}
\right)
]

中文解释：

```text
如果 block 查询很频繁，q_B 大，A_B 大，β_B 会变小。
因为要保持查询干净。

如果 block 插入很频繁，i_B 大，B_B 大，β_B 会变大。
因为要减少 compaction，提升写入。
```

这就不是调参，而是根据 workload 统计自动算。

---

## 7.3 τ 的代价函数

τ 控制 tombstone。

设：

```text
d_B = 删除命中 block B 的频率
```

查询因为 tombstone 多扫的代价：

[
C_B\tau
]

删除因为 compaction 产生的维护代价：

[
\frac{D_B}{\tau}
]

其中：

[
C_B = \frac{1}{2}q_B c_s |B|
]

[
D_B = d_B c_c
]

所以：

[
J_B(\tau)
=========

C_B\tau
+
\frac{D_B}{\tau}
]

最优：

[
\tau_B^*
========

\sqrt{\frac{D_B}{C_B}}
]

加上下界：

[
\tau_B
======

clip
\left(
\sqrt{\frac{D_B}{C_B}},
\tau_{min},
\tau_{max}
\right)
]

中文解释：

```text
如果 block 查询频繁，τ_B 小一点，减少 tombstone 扫描。
如果 block 删除频繁，τ_B 大一点，减少物理删除成本。
```

---

## 7.4 Theorem 2：局部预算最优性

**定理 2：对于代价函数 (J(x)=Ax+B/x)，最优预算有闭式解。**

若：

[
A>0,\ B>0,\ x>0
]

且：

[
J(x)=Ax+\frac{B}{x}
]

则无约束最优解为：

[
x^*=\sqrt{\frac{B}{A}}
]

在约束区间 ([x_{min},x_{max}]) 上，最优解为：

[
clip(x^*,x_{min},x_{max})
]

**证明：**

一阶导数：

[
J'(x)=A-\frac{B}{x^2}
]

令其为 0 得：

[
x^2=\frac{B}{A}
]

所以：

[
x^*=\sqrt{\frac{B}{A}}
]

二阶导数：

[
J''(x)=\frac{2B}{x^3}>0
]

因此 (J(x)) 是凸函数，一阶驻点是全局最优点。有上下界时，把无约束最优点投影到区间即可。

把 (x) 替换为 (\beta_B) 或 (\tau_B)，即可得到每个 block 的最优局部 delta 比例和 tombstone 比例。

---

# 8. 什么时候缓存区更新到主索引？

在 DELI-Cost 里，不再写死：

```text
LOCAL_DELTA_BOUND = 128
```

而是：

```text
if |Δ_B| >= β_B · |B|:
    LocalCompact(B)
```

其中 (\beta_B) 是自动算出来的。

更理论化的触发规则是收益-成本规则。

---

## 8.1 Delta compaction 的收益-成本规则

如果不 compact，未来查询会继续多扫 delta。设未来时间窗口为 (H)，block (B) 的查询频率为 (\hat{q}_B)，扫描一条 entry 成本为 (c_s)，当前 delta 大小为 (|\Delta_B|)。

未来查询损失估计为：

[
Benefit_(B)
===========

\hat{q}_B \cdot H \cdot c_s \cdot |\Delta_B|
]

local compaction 成本：

[
Cost_{compact}(B)=c_c |B|
]

如果：

[
Benefit_{\Delta}(B) \geq Cost_{compact}(B)
]

则执行：

```text
LocalCompact(B)
```

中文：

```text
如果继续保留 delta 对未来查询造成的损失，
已经超过现在整理这个 block 的代价，
就应该 compact。
```

---

# 9. 什么时候真正删除 tombstone？

同理，删除不再写死：

```text
DELETE_COMPACT_FRACTION = 0.25
```

而是：

```text
if |T_B| >= τ_B · |B|:
    LocalCompact(B)
```

或者收益-成本规则：

[
Benefit_T(B)
============

\hat{q}_B \cdot H \cdot c_s \cdot |T_B|
]

如果：

[
Benefit_T(B) \geq Cost_{compact}(B)
]

则执行物理清理。

这就回答了：

> 什么时候删除？

答案：

```text
不是每次 delete 都物理删除；
先 tombstone；
当 tombstone 对未来查询造成的预计损失超过 local compaction 成本时，再物理清理。
```

你当前实现里也已经是先 tombstone，只有 tombstone 超过阈值后才 physical compact；删除对象影响 block summary 时，只 refresh summary，不立即 physical compact。

---

# 10. 自适应 split / merge

你问：

> 为什么不能做自适应划分？

可以，而且 DELI-Cost 应该支持。

---

## 10.1 Split 触发条件

对一个 block (B)，考虑把它切成两个相邻 block：

[
B \rightarrow B_1,B_2
]

如果：

[
Cost(B_1)+Cost(B_2)+\lambda_{split}
<
Cost(B)
]

则 split。

其中：

```text
λ_split = split 的维护惩罚项
```

避免频繁 split。

---

## 10.2 Merge 触发条件

对两个相邻 block (B_1,B_2)，如果：

[
Cost(B_1 \cup B_2)+\lambda_{merge}
<
Cost(B_1)+Cost(B_2)
]

则 merge。

---

## 10.3 Theorem 3：局部重分区单调改进

如果 DELI 只在：

[
Cost(new)+\lambda < Cost(old)
]

时执行 split / merge / repartition，则每次重分区都会严格降低校准目标函数。

**证明：**

算法触发条件就是：

[
Cost(new)+\lambda < Cost(old)
]

因此新结构的代价严格小于旧结构，至少降低 (\lambda)。由于合法分区数量有限，持续执行这样的局部改进最终会停止在一个局部最优状态。

这不是全局最优，但可以证明：

```text
每次自适应调整都不会让校准代价变差。
```

---

# 11. 贝叶斯优化怎么引入？Limbo 应该怎么用？

你提到 Limbo。它可以用，但我建议不要让它直接搜索所有 block 边界和所有 (\beta_B,\tau_B)。那样维度太高，会变成黑箱调参。

Limbo 官方 README 说明它是一个 C++11 的 Gaussian Process 和 data-efficient optimization 库，可以用于 Bayesian optimization，并且强调灵活和高性能。([GitHub][1]) GP-UCB 这类高斯过程优化方法也有理论基础：Srinivas 等人把未知昂贵函数优化建模为 GP/RKHS bandit，并给出基于最大信息增益的 regret bound。([arXiv][2])

所以我建议这样用：

```text
内部结构优化：
  用 DP + 闭式公式。
  这是可解释、可证明的。

外层少量高层权重：
  用 BO / Limbo 调。
  这是可选增强。
```

---

## 11.1 不推荐的用法

不要让 BO 直接搜索：

```text
每个 block 的边界；
每个 block 的 β_B；
每个 block 的 τ_B。
```

原因：

```text
维度太高；
搜索空间爆炸；
结果不稳定；
审稿人会说这是黑箱调参。
```

---

## 11.2 推荐的用法

让 BO 只搜索低维高层参数：

[
\phi =
(w_q,w_i,w_d,w_s,\lambda_{split},\lambda_{merge},H)
]

其中：

```text
w_q, w_i, w_d = 查询/插入/删除权重
λ_split = split 惩罚项
λ_merge = merge 惩罚项
H = 未来收益估计窗口长度
```

对于每个 BO sample (\phi)，内部执行：

```text
1. 用 DP 计算自适应 block partition；
2. 用闭式公式计算每个 block 的 β_B 和 τ_B；
3. 在 calibration workload 上测总代价；
4. 把观测值反馈给 BO。
```

也就是说：

```text
BO 不是直接替代 DELI 的理论；
BO 只是校准 cost model 的少量高层权重。
```

这和你上传文档的思路一致：文档用 BO 优化尺寸分离配置，但也指出直接真实代价很贵，所以又用 GSM 设计近似代价来快速配置。

---

# 12. DELI-Cost-Bayes 完整算法

```text
Algorithm 2: DELI-Cost-Bayes
算法 2：基于贝叶斯优化的 DELI 代价模型校准

Input:
  Dataset D
  Calibration queries Qcal
  Calibration updates Ucal
  Search space Φ for high-level weights
  BO iteration budget R

Output:
  Adaptive DELI index

1. Convert each geometry g into extent entry:
       e(g) = <Zmin, Zmax, MBR, object_id>

2. Build Z-GSM summary:
       count, avg_zspan, avg_mbr_area, max_zmax, query_hits, update_hits

3. Initialize Bayesian optimizer BO over Φ.

4. for r = 1 ... R do
       φ_r = BO.suggest()

       P_r = AdaptiveBlockPartition(D, Qcal, Ucal, φ_r)

       for each block B in P_r:
           estimate q_B, i_B, d_B
           β_B = clip(sqrt(B_B / A_B), βmin, βmax)
           τ_B = clip(sqrt(D_B / C_B), τmin, τmax)

       Build temporary DELI index using P_r, β_B, τ_B.

       Run calibration workload and measure:
           query latency
           insert cost
           delete cost
           maintenance cost
           memory

       J_r = weighted total cost

       BO.observe(φ_r, J_r)

5. φ* = BO.best()
6. Build final DELI using φ*.
7. Return final index.
```

---

# 13. Theorem 4：BO 校准不是全局最优，但可给出理论防守

这里要很谨慎。

你不能说：

```text
BO 一定找到全局最优。
```

应该说：

```text
BO 在低维高层参数空间中减少昂贵评估次数；
在 GP-UCB 假设下可获得次线性 regret；
在有限候选配置下可给出近似最优泛化界。
```

给一个更容易写的有限候选证明。

设候选配置集合：

[
\Theta
]

有限，大小为 (|\Theta|)。

每个配置的代价归一化到 ([0,H])。校准 workload 有 (m) 个独立样本。

经验代价：

[
\hat{J}(\theta)
]

真实期望代价：

[
J(\theta)
]

根据 Hoeffding + union bound：

[
Pr\left[
\forall \theta \in \Theta,
|\hat{J}(\theta)-J(\theta)| \leq \epsilon
\right]
\geq 1-\delta
]

其中：

[
\epsilon =
H
\sqrt{
\frac{\ln(2|\Theta|/\delta)}{2m}
}
]

如果：

[
\hat{\theta}=\arg\min_{\theta\in\Theta}\hat{J}(\theta)
]

[
\theta^*=\arg\min_{\theta\in\Theta}J(\theta)
]

则：

[
J(\hat{\theta})
\leq
J(\theta^*)+2\epsilon
]

中文解释：

```text
用校准 workload 选出来的配置，在候选配置集合内接近最优；
误差随着校准样本数增加而下降。
```

这不是全局最优，但它比“手动试参数”强很多。

---

# 14. DELI-Cost 的最终理论体系

你可以把论文方法写成五个层次。

---

## 层次 1：范围条目

每个对象：

[
e(g)=\langle l_g,u_g,m_g,id_g\rangle
]

其中：

```text
l_g = Zmin
u_g = Zmax
m_g = MBR
id_g = object id
```

---

## 层次 2：自适应 block partition

用 DP 求：

[
P^*
===

\arg\min_P
\sum_{B\in P}Cost(B)
]

在连续 Zmin block 约束下是全局最优。

---

## 层次 3：局部预算闭式最优

每个 block 自动计算：

[
\beta_B^*=\sqrt{\frac{B_B}{A_B}}
]

[
\tau_B^*=\sqrt{\frac{D_B}{C_B}}
]

不是固定参数。

---

## 层次 4：收益-成本维护

当：

[
Benefit(B)\geq Cost(B)
]

才 local compaction。

---

## 层次 5：BO 高层校准

BO 只搜索：

```text
少量 cost model 权重
```

不直接黑箱搜索整个索引结构。

---

# 15. 你可以在论文里怎么命名

建议命名：

```text
DELI-Fixed:
  当前实现版本，固定 b=512, β=0.25, τ=0.25。

DELI-Cost:
  代价模型驱动版本，DP block partition + 闭式 β/τ。

DELI-BO:
  可选增强，使用 Limbo/BO 校准高层权重。
```

主论文可以这样安排：

```text
方法主线：DELI-Cost
实现主线：DELI-Fixed / DELI-LocalBounded
增强实验：DELI-BO 或模拟校准
```

如果你时间不够实现完整 DELI-Cost，也可以先在论文里写：

```text
We implement the fixed-budget version and evaluate sensitivity. We further present DELI-Cost as a cost-driven adaptive extension.
```

但如果想冲 VLDB，最好至少实现：

```text
per-block adaptive β/τ
```

因为这是最容易落地的自适应部分。

---

# 16. 最实际的实现路线

不要一下子实现完整 BO + DP + online split/merge。建议三步：

## Step 1：实现 per-block adaptive β/τ

保持 block size = 512 不变。

对每个 block 统计：

```text
q_B = 被 query 访问次数
i_B = insert 命中次数
d_B = delete 命中次数
```

然后计算：

[
\beta_B=\sqrt{B_B/A_B}
]

[
\tau_B=\sqrt{D_B/C_B}
]

这一步最容易实现，也最能证明不是固定参数。

---

## Step 2：实现 adaptive block partition

bulk-load 时用 DP 划分 block。

先不做 online split/merge。

---

## Step 3：实现 Limbo BO

只优化高层权重：

```text
w_q, w_i, w_d, λ_split, λ_merge
```

不要一开始让 BO 搜全结构。

---

# 17. 论文中最关键的一段话

你可以把下面这段放进方法开头：

```text
Rather than treating block size, delta capacity, and tombstone threshold as hand-tuned constants, DELI-Cost formulates index layout and maintenance as a cost-driven optimization problem. It partitions Zmin-ordered extent entries into contiguous query blocks using dynamic programming, derives per-block delta and tombstone budgets from a convex query-update maintenance model, and triggers local compaction only when its predicted query benefit exceeds its maintenance cost. Bayesian optimization is used only as an optional outer-loop calibrator for high-level cost weights, not as a black-box replacement for the index design.
```

中文：

```text
DELI-Cost 不把 block size、delta 容量和 tombstone 阈值当作手工参数，而是把索引布局和动态维护建模为代价驱动优化问题。它用动态规划对 Zmin 有序 extent entries 进行连续块划分，用凸的查询-更新维护模型推导每个 block 的 delta 和 tombstone 预算，并且只有在预测查询收益超过维护成本时才执行局部 compaction。贝叶斯优化只作为外层高层权重校准工具，而不是黑箱替代索引设计。
```

---

# 18. 最终判断

你想要的“完整理论体系”可以这样建立：

```text
1. 查询代价来自 filter + scan + exact refinement。
   借鉴学习型尺寸分离策略。

2. block partition 用动态规划。
   可以证明给定代价模型下最优。

3. β 和 τ 用 convex surrogate 推导闭式最优。
   可以证明唯一最优。

4. compaction 用 benefit-cost trigger。
   可以证明每次触发都会降低校准代价。

5. Bayesian optimization / Limbo 只校准低维权重。
   可以借 GP-UCB / BO 理论说明数据高效优化，不做高维黑箱调参。
```

这比当前固定参数版本理论深很多，而且不是空喊“自适应”。它有：

```text
Optimization model
Algorithm
Theorem
Proof
Implementation path
```

一句话总结：

> **把 DELI 写成 DELI-Cost：基于代价模型的自适应动态范围感知学习索引。固定参数版本只是 DELI-Cost 的一个特例；真正的论文贡献是自适应 block partition + per-block β/τ 闭式预算 + benefit-cost local compaction + BO 高层校准。**


[1]: https://github.com/resibots/limbo
[2]: https://arxiv.org/abs/0912.3995
