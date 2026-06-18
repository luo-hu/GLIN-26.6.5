# DELI 动态方法对比与论文路线说明

最后更新日期：2026-06-16

本文档用于解释当前动态实验里的各个方法到底是什么、为什么要保留这些方法、各自优缺点是什么，以及它们在论文里应该怎么定位。目标是让后续即使隔一段时间再看，也能快速明白这些名字背后的设计逻辑。

## 1. 先说结论

当前最适合作为 DELI 动态主方法的是：

```text
DELI-ALEX-Hybrid-LocalBounded
```

建议默认参数：

```text
BLOCK_SIZE = 512
LOCAL_DELTA_BOUND = 128
DELETE_COMPACT_FRACTION = 0.25
```

这些参数不是“试出来哪个最好就用哪个”，而是有结构含义：

- `LOCAL_DELTA_BOUND=128`：每个 block 最多容忍约 `25%` 的局部 delta 记录。
- `DELETE_COMPACT_FRACTION=0.25`：每个 block 最多容忍约 `25%` 的 tombstone，也就是被删除但尚未物理清理的记录。
- `BLOCK_SIZE=512`：每个 compact block 约 512 条记录，保持查询扫描局部性和维护成本之间的平衡。

一句话解释：

```text
LocalBounded 不是追求单项最强，而是一个同步、单线程、公平计成本的平衡版本：
查询接近 compact overlay，插入明显快于同步 Hybrid，删除明显快于 aggressive compaction，
同时 correctness 始终用 Boost R-tree exact oracle 检查。
```

## 2. 核心名词解释

### 2.1 learned index

`learned index` 是学习型索引。它用模型学习 key 的分布，预测一个 key 大概在数组里的位置。

通俗例子：

```text
如果学生学号大致从 1 到 100000 连续增长，
模型可以猜出学号 50000 大概在中间位置。
```

传统 B-tree 靠树结构一级一级找；learned index 靠模型先猜位置，再在附近修正。

### 2.2 ALEX

`ALEX` 是 Adaptive Learned Index。它是动态 learned index，支持插入、删除和更新。

可以把 ALEX 想成：

```text
一个按 key 排序的数组系统，数组中留有 gap，方便插入。
如果局部数据分布变了，就 split、resize 或 retrain。
```

ALEX 擅长维护一维 key，比如 `Zmin`，但它不知道复杂 geometry 的空间范围。

### 2.3 Zmin / Zmax

空间对象可以映射到 Z-order 曲线。

- `Zmin`：这个对象覆盖区域在 Z-order 上的最小值。
- `Zmax`：这个对象覆盖区域在 Z-order 上的最大值。

如果只用 `Zmin`，一个很长的线或很大的多边形会被压成一个点 key，容易丢失范围信息。

### 2.4 extent

`extent` 指空间对象的范围。对一个 geometry 来说，它不只是一个点，而是一段 Z-order 区间和一个二维包围盒。

在 DELI 里，一个对象更像：

```text
object = <Zmin, Zmax, MBR, geometry>
```

其中 `MBR` 是 minimum bounding rectangle，中文是最小外接矩形。

### 2.5 block summary

`block summary` 是一个 block 的摘要信息，例如：

```text
min_zmin
max_zmin
max_zmax
block_mbr
live_count
dead_count
```

它的作用是快速判断一个 block 有没有必要继续查。

通俗例子：

```text
一个抽屉里有 512 份地图。
block summary 就是这个抽屉外面贴的标签：
里面地图的 Z 范围大概是多少，整体覆盖的地理范围在哪里。
查询时先看标签，不合适的抽屉就不用打开。
```

### 2.6 delta

`delta` 是临时增量区。新插入的数据先进入 delta，不马上插入 compact 主体。

好处：

```text
写入快。
```

坏处：

```text
查询时除了查 main，还要查 delta。
delta 越大，查询越慢。
```

### 2.7 tombstone

`tombstone` 是墓碑标记。删除对象时不马上从数组里物理移除，而是先标记为 dead。

好处：

```text
删除快。
```

坏处：

```text
数组里会留下 dead records。
查询和维护时要跳过它们。
```

### 2.8 compaction

`compaction` 是压缩整理。它会把 live records 收集起来，移除 dead records，并重新整理 block。
live records 就是当前还有效，还应该被查询返回的对象记录。比如对象 object_id=10 是一个 polygon，现在还在索引里，没有被删除，那么它对应的 record 就是：live record ＝ 活记录 ＝ 有效记录  查询时如果它和query相交了，就应该返回它。

Dead records 就是已经被删除，但物理上暂时还留在数组里的旧记录。比如删除了 object_id=10,为了避免马上移动大量数组元素，DELI不立刻把它从block里拿掉，而是标记为：alive=false;  这个记录就变成：dead record = 死记录=tombstone record = 被逻辑删除的记录，查询扫描到它时会跳过，不会返回
Tombstone delete 就是“墓碑式”删除或者逻辑删除，好处就是删除非常快，不用频繁移动数组，重排block。坏处就是dead records 多了以后，查询会多扫描一些无效记录。所以当dead records太多时，需要再做一次compaction,把dead records真正清掉

通俗例子：

```text
桌面上有很多文件，其中一些已经作废。
lazy delete 只是给作废文件打叉。
compaction 是真正把作废文件扔掉，再把剩下文件排整齐。
```

### 2.9 foreground cost

`foreground cost` 是前台成本。用户执行插入、删除、查询时必须等待的成本。

本文档讨论的 LocalBounded 是同步前台维护，不把成本藏到后台线程里，因此更公平、也更容易解释。

## 3. 各方法总览

| 方法 | 核心思想 | 查询性能 | 插入性能 | 删除性能 | 正确性定位 | 论文定位 |
|---|---|---|---|---|---|---|
| `DELI-Dynamic` | 独立 blocked-vector 动态原型 | 中等 | 中等 | 中等 | 应为 1 | 正确性原型 |
| `DELI-ALEX` | ALEX 动态布局 + leaf/group summary | 一般 | 很强 | 很强 | 应为 1 | ALEX 接入基础版 |
| `DELI-ALEX-Hybrid` | ALEX 写入 + compact query overlay 同步维护 | 好 | 一般 | 一般 | 应为 1 | 查询优化同步版 |
| `DELI-ALEX-Hybrid-Buf` | 全局 delta，写入尽量 append | 较差 | 很强 | 很强 | 应为 1 | 写入优化 ablation |
| `DELI-ALEX-Hybrid-Bounded` | 全局 delta 满后全量 promotion/compaction | 好 | 较差 | 视情况 | 应为 1 | 证明 full compaction 太重 |
| `DELI-ALEX-Hybrid-LocalBounded` | 每个 block 局部 delta + bounded tombstone | 好 | 强 | 强 | 应为 1 | 当前主方法 |
| `GLIN-piece` | GLIN + piecewise query augmentation | 视数据而定 | 强 | 强 | 当前动态下可能不稳定 | learned baseline |
| `Boost R-tree` | 传统 R-tree 空间索引 | 强 | 中等 | 中等 | oracle/baseline | 必须 baseline |
| `GEOS Quadtree` | GEOS 四叉树 | 数据相关 | 中等 | 较弱或中等 | baseline | 必须 baseline |

注意：

```text
answers_match_boost = 1
```

表示这个方法的最终 exact answer set 和 Boost R-tree oracle 一致。对于 DELI 主线，这是必须满足的基本条件。

## 4. 各方法详细解释

### 4.1 DELI-Dynamic

全名可以理解为：

```text
DELI-Dynamic-Single
```

它是一个独立的动态原型，不深度依赖 ALEX。

结构：

```text
append-only records
+ block-local sorted ids
+ object_id -> live record
+ tombstone delete
+ one-sided conservative summaries
```

它主要证明：

```text
DELI 的动态维护机制在 insert/delete 后仍然正确。
```

优点：

- 数据结构清楚，适合解释 correctness。
- 容易验证 invariant，也就是不变式。
- 可以说明 DELI 不只是查询补丁，而有动态维护语义。

缺点：

- 不是最终性能版。
- 没有充分利用 ALEX 的动态 learned layout。


论文定位：

```text
正确性原型，不作为最终最高性能方法。
```

名词解释：

Append-Only Records
append-only records 是说主记录表只追加，不在中间插入、不随便移动。

例如：
```text
records[0]
records[1]
records[2]
records[3]
```
新对象来了，只做：
```text
records.push_back(new_record)
```
好处是 record_id 永远稳定。比如 record_id=100 一旦分配，就一直指向同一条记录，不会因为 vector 中间插入导致位置变化。

Block-Local Sorted IDs
DELI 不把所有 records 真的排成一个巨大数组，而是每个 block 里保存一组 record id：
```text
block 0: [record_id 3, record_id 8, record_id 20, ...]
block 1: [record_id 1, record_id 7, record_id 30, ...]
```
这些 id 在 block 内按 zmin 排序。

意思是：
```text
records 本身不移动
block 里只移动 record_id
这比移动完整 geometry / record 便宜，也避免 record_id 失效。
```

object_id -> live record   
object_id 是对象身份，比如：object_id=10, record_id 是它当前在 records 表里的记录编号。

映射：
```text
object_id -> live record
```
意思是：
```text
object_id=10 当前有效版本是 record_id=135
```
如果删除 object_id=10，就通过这个表找到它当前活着的 record，然后标记 dead。

如果 update 一个对象，可以理解为：
```text
旧 record 标记 dead
新 geometry append 成一个新 record
object_id 指向新 record

举例说明：可以把object_id和record_id分别想成“身份证号”和“档案编号”的区别。

`object_id` 是对象本身的身份，通常不变。  
比如一个湖泊、一个公园、一条道路，它的编号是 `object_id=10`。不管它以后被更新多少次，它还是同一个对象。

`record_id` 是这个对象某一次版本在内部记录表里的位置。  
因为我们的动态结构是 append-only，也就是新版本不覆盖旧版本，而是追加一条新记录。所以同一个 `object_id` 可能历史上对应过多个 `record_id`。

例如一开始：

```text
records[135] = object_id=10 的旧 geometry
object_to_record[10] = 135
```

意思是：

```text
object_id=10 当前活着的版本是 record_id=135
```

如果删除 `object_id=10`：

```text
查 object_to_record[10] 得到 135
把 records[135].alive = false
从 object_to_record 删除 10
```

如果更新 `object_id=10`，比如它的 geometry 变了：

```text
records[135].alive = false        // 旧版本作废
records.push_back(new geometry)   // 新版本追加到 records 表尾
新 record_id = 200
object_to_record[10] = 200        // object_id=10 现在指向新版本
```

所以这句话：

```text
object_id -> live record
```

就是：

```text
对象编号 -> 当前有效的那条内部记录
```

为什么要这样做？因为 `records` 里的旧记录不移动，`record_id` 稳定；删除和更新只改 `alive` 标记和映射表，不需要大规模移动数组，也不容易把 block 里的位置搞乱。


```
One-Sided Conservative Summaries
这是最关键的正确性机制。

每个 block 会保存摘要 summary，比如：

min_zmin
max_zmax
block_mbr
这些 summary 用来快速跳过不可能命中的 block。

one-sided conservative 的意思是：

summary 可以偏大，但不能偏小。
比如 block 里真实最大 zmax=100，summary 可以还是：

max_zmax = 120
这会导致多扫一点，但不会漏答案。

但绝对不能变成：max_zmax = 80
因为如果 query 需要找 z=90 的对象，系统可能错误跳过这个 block，造成漏答案。

所以 DELI 的安全原则是：宁愿多查，不能漏查。

整个机制连起来
DELI 动态维护大概是这样：
```text
1. 新对象插入：
   append 到 records
   record_id 插入对应 block 的 id 列表
   扩大 block summary

2. 删除对象：
   object_id 找到当前 live record
   标记 alive=false
   summary 不急着缩小
   查询时跳过 dead record

3. dead records 太多：
   对这个 block 做 local compaction
   清掉 dead records
   重新计算 summary

4. 查询：
   先用 block summary 剪枝
   再扫 live records
   最后用 GEOS exact intersects 精确判断
```

一句话：
```text
DELI 用 append-only + tombstone 保证更新简单，
用 block-local ids 保证局部有序，
用 conservative summaries 保证查询不会漏答案，
用 local compaction 控制 dead records 不会无限变多。
```
所以它不是“删除后马上重建整个索引”，而是：
```text
先逻辑删除，保证快；
摘要保持保守，保证正确；
局部积累太多 dead records 后，再清理一个 block。
```

Append-only records这个records具体是什么东西，它是多边形对象的Zmin吗？append-only records 是说主记录表只追加，不在中间插入、不随便移动。主记录表里records和block 里record id是什么关系 ？为什么records 本身不需要移动，只需要在block 里只移动 record_id呢？
可以，把它想成“对象仓库”和“索引目录”分开。

**records 是什么**
`records` 不是只有 `Zmin`。它是一张主记录表，每一条 `record` 表示一个空间对象的完整索引元数据，通常包含：

```text
（1）record_id：内部稳定编号。即使 block 里插入、删除、排序，record 本身在 records 表里的位置不变。这样 block 里只保存 record_id，不会因为 vector 插入导致引用失效。
（2）object_id：外部对象编号。查询最终返回的是对象集合，不是内部 record 编号。和 Boost R-tree 做 correctness check 时，也要比较 object_id。
（3）zmin：对象在 Z-order / Hilbert-like 空间映射后的最小 key。它用于排序和定位 block。也就是决定这个对象大致放在哪个 block。
（4）zmax：对象覆盖的最大 key。查询时如果：record.zmax < query_zmin，说明这个对象的 Z 区间不可能和 query 相交，可以跳过。
（5）MBR/envelope：对象的最小外接矩形。Z 区间只是粗过滤，MBR 是第二层空间过滤。查询时如果对象 MBR 和 query MBR 不相交，就不用调用昂贵的 GEOS exact predicate。
（6）geometry pointer：指向真实几何对象。最后必须做 exact refinement。query_geometry->intersects(record.geometry)
（7）alive/dead 标记：删除时不马上物理移除 record，而是 tombstone。alive=false 表示这个对象已经被删除，查询时跳过。这样删除很快，也避免频繁移动数组和更新大量位置。
（8）所在 block 指针：用于快速删除和更新。删除 object_id 时，通过：object_id -> live record_id -> record.block，直接找到所在 block，更新这个 block 的 live_count/dead_count/stale，不用全局搜索。
```


其中：

```text
zmin/zmax 是这个多边形对象映射到 Z-order 后的区间
MBR 是对象外接矩形
geometry pointer 指向真正的多边形几何
alive 表示这条记录当前是否有效
```

所以 `records[i]` 可以理解为“第 i 条对象记录”，不是单纯一个 `Zmin`。

**append-only records 是什么意思**
`append-only` 的意思是：主记录表只追加，不在中间插入，不因为排序而移动已有记录。

例如已有：

```text
records[0] = 对象A
records[1] = 对象B
records[2] = 对象C
records[3] = 对象D
```

新对象 E 来了，不管它的 `zmin` 应该排在 A 前面还是 B 后面，主表都只做：

```text
records.push_back(E)
```

于是：

```text
records[4] = 对象E
```

这个 `4` 就是稳定的 `record_id`。

**为什么 records 不按 zmin 排序**
因为如果 `records` 本身按 `zmin` 排序，新对象插入中间时会导致后面的元素整体后移。

例如：

```text
records[0]
records[1]
records[2]
records[3]
```

如果新对象应该插到 `records[1]` 前面，vector 会变成：

```text
records[0]
new_record
old records[1]
old records[2]
old records[3]
```

这样原来的 `records[1]` 变成了 `records[2]`，原来的 `records[2]` 变成了 `records[3]`。也就是说，很多对象的位置都变了。

这会让：

```text
object_id -> record position
block begin/end
block summary
```

都容易失效。

所以 DELI 的做法是：

```text
records 主表不动
排序关系放到 block 里的 record_id 列表里
```

**block 里的 record_id 是什么**
block 不是直接存完整对象，而是存一组 `record_id`：

```text
block 0: [3, 8, 20]
block 1: [1, 7, 30]
```

意思是：

```text
block 0 里有 records[3], records[8], records[20]
block 1 里有 records[1], records[7], records[30]
```

这些 `record_id` 在 block 内按 `records[id].zmin` 排序。

比如：

```text
records[3].zmin  = 10
records[8].zmin  = 15
records[20].zmin = 22
```

所以：

```text
block 0: [3, 8, 20]
```

**为什么只移动 record_id 更好**
因为 `record_id` 只是一个整数，移动它很便宜。

假设新对象 `records[100]` 的 `zmin=16`，应该插到 block 0 中间：

```text
原来:
block 0: [3, 8, 20]

插入后:
block 0: [3, 8, 100, 20]
```

这里只移动 block 里的整数 id，不移动 `records[3]`、`records[8]`、`records[20]` 本身。

主表仍然是：

```text
records[3]  还在原位置
records[8]  还在原位置
records[20] 还在原位置
records[100] 是新追加的
```

所以 `record_id=20` 永远还是指向 `records[20]`，不会失效。

**一句话理解**
`records` 是稳定的对象仓库；`block.ids` 是按 `zmin` 排序的索引目录。

```text
records:
  存完整记录，不移动

block.ids:
  存 record_id，负责排序和查询扫描
```

这样 DELI 既能支持动态插入删除，又不会因为 vector 中间插入导致大量位置失效。



### 4.2 DELI-ALEX

它把 DELI 的空间 summary 接到 ALEX/GLIN-style 动态布局上。

结构：

```text
ALEX leaves
+ leaf summary
+ leaf group directory
```

优点：

- 插入和删除吞吐通常很好。
- 能利用 ALEX 的动态插入能力。
- 内存通常比 Hybrid overlay 版本低。

缺点：

- 查询时可能要扫很多 ALEX leaf。
- ALEX leaf 内部有 gap、slot、稀疏布局，cache locality 不如连续 compact block。

通俗解释：

```text
ALEX 像一个动态书架，书架方便插书和删书；
但要查空间范围时，书架里的分隔和空位会让扫描不够顺滑。
```

论文定位：

```text
ALEX 接入基础版，用来说明只加 leaf summary 还不够。
```

名词解释：
summary 可以理解成“摘要信息”或“索引块的安全边界”,作用就是快速判断一组对象有没有必要继续查。对一个 leaf/block 里的很多 geometry，DELI 不想每次查询都逐个看，所以给这一组对象维护几个摘要：
```text
min_zmin   这一组对象里最小的 zmin
max_zmin   这一组对象里最大的 zmin
max_zmax   这一组对象里最大的 zmax
MBR        这一组对象整体外接矩形
```
Leaf Summary
leaf 是 ALEX 底层的数据节点。你可以把它理解成 ALEX 里的一个叶子数组，里面存了一批按 zmin 排列的对象。
leaf summary 就是给每个ALEX leaf 维护一份DELI summary:它会记录这一组对象的zmin范围，最大zmax，还有这一组对象的最小包围框MBR

Leaf Group Directory
如果ALEX有很多leaf，比如3万个，查询时即使只查询leaf summary,还是要看3万个，成本还是有点高。
所以再加一层leaf group directory:
```text
每若干个leaf合成一个grouup
每个group再维护一个group summary
```
结构大概是：
```text
group 0: leaf 0 - leaf 31 的 summary
group 1: leaf 32 - leaf 63 的 summary
group 2: leaf 64 - leaf 95 的 summary
...
```
查询时先看group summary，如果整个group不相交，直接跳过这一组leaf，从而减少需要检查的leaf summary数量

DELI-ALEX是怎么利用ALEX的动态插入能力的？
ALEX 本身是一个 learned index，按一维 key 管理数据。这里的 key 通常是：zmin
插入一个geometry时：
```text
1. 计算 geometry 的 zmin/zmax/MBR
2. 把 geometry 按 zmin 插入 ALEX
3. ALEX 自动决定放到哪个 leaf
4. ALEX 负责 leaf 扩容、split、局部调整
5. DELI 只刷新相关 leaf 的 summary
```
也就是说：ALEX负责动态数据布局  DELI负责空间查询剪枝信息

ALEX里的Gap/Slot/Sparse Layout是什么意思？
ALEX Leaf 不是一个紧密连续数组。为了方便插入，它会预留空位。
可以想象成：
```text
[对象][空位][对象][对象][空位][空位][对象]
```
这些空位就是gap
slot 是leaf数组里位置。有的slot有对象，有的是空的。
sparse layout就是这种“不完全填满”的稀疏布局。
这的好处是插入时不用每次搬动很多对象，坏处是查询扫描时要跳过空slot,CPU cache利用率差一些

Cache Locality
cache locality（缓存局部性），它理解为“CPU读取内存是否顺手”
如果数据是连续放的：
```text
对象0 对象1 对象2 对象3 对象4
```
CPU 读对象0时，通常会顺手把后面的对象也加载进 cache，所以扫描很快。
如果数据中间有空洞、指针跳转、分散存储：
```text
对象0 空位 空位 对象1 指针跳到别处 对象2
```
CPU 就没那么容易连续读取，扫描会慢。
所以：
```text
cache locality 好 = 数据排得紧、连续、扫描顺
cache locality 差 = 数据稀疏、跳来跳去、扫描不顺
```

Compact Block
compact block 就是紧凑块。
例如 IntervalOverlapIndex 或 DELI-ALEX-Hybrid 里的 overlay 会把对象 id 按 zmin 连续排好，然后每 512 个组成一个 block：
```text
block 0: id0 id1 id2 ... id511
block 1: id512 id513 ... id1023
```
这个 block 是紧凑的：没有 gap 没有空 slot 按 zmin 连续排列 扫描时很顺
所以查询时它比 ALEX leaf 更适合扫描。

为什么 ALEX Leaf 不如 Compact Block
一句话：
```text
ALEX leaf 更适合动态插入； compact block 更适合查询扫描。
```
ALEX leaf：
```text
有 gap，方便插入
可能比较稀疏
扫描时要跳空位
cache locality 较差
```
compact block：
```text
连续存储
没有空洞
summary 更规整
扫描效率高
```
所以它们是两个方向的取舍：
```text
ALEX leaf：写入友好
compact block：查询友好
```
这也是为什么后来要做 DELI-ALEX-Hybrid 和 LocalBounded：
```text
ALEX 负责动态写入
compact overlay/block 负责高效查询
```
也就是希望同时拿到：
```text
ALEX 的动态更新能力
● compact block 的查询效率
```
### 4.3 DELI-ALEX-Hybrid

Hybrid 的意思是混合。

它把写入和查询拆开：

```text
ALEX 负责动态写入。
compact query overlay 负责查询。
```

这里的 `overlay` 可以理解为一层查询用的辅助目录。

优点：

- 查询明显比 DELI-ALEX 更好。
- compact block 连续扫描，cache locality 好。
- correctness 仍然可以用 Boost exact oracle 检查。

缺点：

- 每次插入都要同步更新 compact overlay。
- 插入吞吐不如 Buf 或纯 DELI-ALEX。

通俗例子：

```text
ALEX 是日常收纳用的柜子；
overlay 是专门为快速查找做的一本目录。
每次新增文件，都要同时更新柜子和目录。
```

论文定位：

```text
查询优化同步版。说明 compact query layout 对复杂 geometry exact query 很重要。
```

### 4.4 DELI-ALEX-Hybrid-Buf

Buf 是 buffer 的缩写。

它的思想是：

```text
新插入对象先 append 到全局 delta buffer。
查询时查 main compact overlay + global delta。
```

优点：

- 插入吞吐很高。
- 删除也很快，因为维护很少。

缺点：

- global delta 越大，查询越慢。
- after_insert / after_delete 的 p95、p99 容易变差。

通俗解释：

```text
它像把新文件都先堆在桌面上。
放文件很快，但找文件时既要查柜子，又要翻桌面。
桌面越乱，查询越慢。
```

论文定位：

```text
写入优化 ablation。
证明只追求 append delta 会伤害查询。
```

### 4.5 DELI-ALEX-Hybrid-Bounded

Bounded 的意思是有上界。

它尝试限制 global delta：

```text
delta 到达上界后，把 main + delta 合并，重建 compact blocks。
```

优点：

- 查询可以恢复。
- 证明 bounded delta 思路是有效的。

缺点：

- full compaction 太重。
- 插入阶段会被全局重建拖慢。

通俗解释：

```text
桌面文件太多以后，把整个柜子和桌面全部重新整理一遍。
整理完很好查，但整理过程太累。
```

论文定位：

```text
global full compaction 反例或 ablation。
证明需要局部维护，而不是全局重建。
```

### 4.6 DELI-ALEX-Hybrid-LocalBounded
它的目标是：
```text
ALEX 负责动态写入能力；
compact query blocks 负责快速 exact spatial query；
local delta 负责降低插入维护成本；
tombstone + local compaction 负责降低删除维护成本。
````
这是当前最推荐的主方法。

结构：

```text
ALEX write layout            负责快速登记新书，删书
+ compact query blocks       负责把书按编号整理到书架上，方便查找
+ per-block local delta       每个书架旁边的小临时篮子，先放新来的书
+ bounded tombstone           删除时先贴“已下架标签”，不马上搬走
+ local compaction            篮子或者废书太多时，只整理这个书架
```

插入：

```text
1. ALEX 插入。
2. 找到对应 compact block。
3. append 到这个 block 的 local delta。
4. local delta 超过 LOCAL_DELTA_BOUND 后，只 compact 这个 block。
```

删除：

```text
1. 先 tombstone，不立刻物理删除。
2. 如果删除对象影响 block summary，只 refresh summary。
3. tombstone 超过 DELETE_COMPACT_FRACTION 后，才 physical compact 这个 block。
```

优点：

- 查询远好于 Hybrid-Buf。
- 插入远好于同步 Hybrid。
- 删除在 `DELETE_COMPACT_FRACTION=0.25` 后明显改善。
- 不使用后台线程，公平性好解释。
- correctness 仍然用 Boost exact oracle 检查。

缺点：

- 不是所有单项指标都第一。
- 内存估算仍需补真实 RSS。
- 对参数需要做 sensitivity 证明，但默认值可以用固定规则解释。

通俗解释：

```text
不是把所有新文件堆到一个大桌面上，
而是每个抽屉旁边有自己的小临时夹。
某个抽屉的小临时夹满了，只整理这个抽屉。
删除时先打叉，打叉太多了才真正清理这个抽屉。
```

论文定位：

```text
当前 DELI 动态主方法。
```

名词解释
ALEX write layout
这是底层动态 learned index。它按 zmin 这个一维 key 组织对象，主要服务于插入和删除。也就是说，新对象来了，先交给ALEX登记，删除对象时，也先从ALEX那边删除。它更偏向“写入维护层”

在当前实现里，ALEX 不是主要查询路径。也就是说，查询不直接扫 ALEX leaf，因为 ALEX leaf 有 gap、slot、动态布局开销，复杂几何查询会比较慢。

compact query blocks

这是查询用的紧凑 block 结构。它把对象按 zmin 排序，每个 block 大约放 BLOCK_SIZE 个对象，比如 512 个。

每个 block 保存 summary：
```text
min_zmin
max_zmin
max_zmax
block MBR
live_count
```
查询时先看 block summary 能不能跳过整个 block，不能跳过才进入 block 内部检查对象。作用就是让查询少扫对象，查得快

per-block local delta

每个 compact block 自己带一个小的临时区buffer，叫 local delta。

新插入对象不会立刻插入到 block 的有序数组中，因为那样会搬移很多元素。它会先 append 到对应 block 的 local delta 里，即先放入这个blockd旁边的小篮子里。

例如：
```text
block 10 原来有 512 个排好序的对象
现在来了 1 个新对象，本来应该插到 block 10 中间

不马上插入排序
先放到 block 10 的 delta 里
```

查询 block 10 时，就查两部分：
```text
block 10 的正式区 compact_ids
block 10 的临时区 delta_ids
```
这样插入快很多，因为append 比中间插入排序更便宜

bounded tombstone

删除时不立刻从数组里移走对象，而是标记为 dead，也叫 tombstone。

这样删除不需要马上移动数组、重排 block、重算所有结构。

bounded 的意思是 tombstone 不能无限增长。超过阈值后才清理，例如：
```text
DELETE_COMPACT_FRACTION = 0.25
BLOCK_SIZE = 512
delete_compact_bound = 128
```
也就是一个 block 里 dead 记录积累到约 128 个后，才 physical compact。


local compaction

这是局部压缩。只重建受影响的一个 block，不全局重建整个索引。

它会做：
```text
1. 删除 dead ids，即去年tombstone对象
2. 合并 compact_ids 和 delta_ids，即把local delta里的新对象合并到对应的block（正式区）
3. 按 zmin 保持有序
4. 重算 block summary
```
例子：
```text
block 8:
  正式区有 512 个对象
  delta 里有 128 个新对象
  tombstone 有 80 个已删除对象

触发 local compaction 后：
  删除 tombstone
  合并 delta
  重新排序
  得到一个干净的新 block 8
```
注意：它只整理block8,不动别的block。
这就是LocalBounded的关键价值：
```text
避免全局 rebuild
避免 delta 无限变大
把维护成本限制在局部 block 内
```

插入流程

插入一个 geometry 时：
```text
1. 计算它的 zmin / zmax / MBR。
2. 插入 ALEX。
3. 根据 zmin 找到对应 compact block。
4. 把对象 id append 到这个 block 的 delta_ids。
5. 更新 block summary，让它保持 conservative。
6. 如果 delta_ids 超过 LOCAL_DELTA_BOUND，就 compact 这个 block。
```
所以插入的大多数时候是：
``text
ALEX insert + local delta append
```
不是每次都重排block

删除流程

删除一个对象时：
```text
1. 从 ALEX 删除。
2. 在 query overlay 里把对象标记为 dead。
3. 如果它在 delta 里，也标记 delta live 数减少。
4. 如果它影响 block summary，比如它贡献了 max_zmax 或 MBR 边界，就 refresh summary。
5. 如果 dead_count 超过 DELETE_COMPACT_FRACTION 对应阈值，才 physical compact block。
```
关键点是：
```text
删除后 summary 偏大是安全的。
```
比如删除了一个很大的对象，block MBR 还保留原来的大范围，只会导致查询多看一些对象，不会漏答案。

所以删除不需要每次都物理清理。

查询流程
查询一个 query geometry 时：
```text
1. 计算 query 的 zmin / zmax / MBR。
2. 顺序扫描 compact query blocks。
3. 如果 block.min_zmin > query_zmax，可以停止。
4. 如果 block.max_zmax < query_zmin，跳过这个 block。
5. 如果 block MBR 和 query MBR 不相交，跳过这个 block。
6. 对不能跳过的 block：
   查 compact_ids
   查 delta_ids
7. 对候选对象做：
   z interval check
   object MBR check
   GEOS exact intersects
8. 返回 exact answers。
```
所以查询最终一定经过GEOS exact refinement。前面的block summary,Z interval,MBR都只是过滤候选对象

整体流程举例说明：
假设：
```text
block_size=512
LOCAL_DELTA_BOUND=128
DELETE_COMPACT_FRACTION=0.25
```
插入1个对象：
```text
1.ALEX记录这个对象
2.根据zmin找到block 20
3.把对象放进 block 20 的delta
4.如果delta 数量还没到128，不整理
```
删除1个对象：
```text
1.ALEX删除这个对象
2.在query block里把它标成tombstone
3.如果它影响 block summary，就只重算summary
4.如果tombstone还没到128，不整理
5.到128后，只compact这个block
```

查询时：
```text:
1.先看block summary,能跳过就跳过
2.对可能命中的block,查正式区compact_ids
3.再查这个block的delta_ids
4.跳过tombstone，这里都是被删除的对象
5.最后用GEOS exact intersects 判断真假相交
```


一句话总结
DELI-ALEX-Hybrid-LocalBounded的思想是：
```text
ALEX 负责写入；
compact blocks 负责快速查询；
local delta 让插入不用频繁重排；
tombstone 让删除不用频繁物理清理；
local compaction 把维护成本限制在单个 block 内。
```
它是一个折中版本：
```text
DELI-ALEX-Hybrid：查询快但插入/删除要维护 compact blocks，写入不够轻
DELI-ALEX-Hybrid-Buf:写入快，但是global delta越来越大，查询变慢
DELI-ALEX-Hybrid-LocalBounded：新对象先进每个block的小delta，删除先tombtone，超过阈值才局部整理
```
所以它试图做到：
```text
写入不太重
查询不崩
维护成本局部化
```


## 5. Baseline 方法说明

### 5.1 GLIN-piece

`GLIN-piece` 是 learned spatial index baseline。

它通过 piecewise query augmentation 来改善 GLIN 查询。

优点：

- 是最相关的 learned spatial index baseline。
- 某些情况下插入/删除吞吐很高。
- 必须保留，否则审稿人会问为什么不和 GLIN 对比。

缺点：

- 在当前动态 workload 下，部分结果出现过 `answers_match_boost != 1`。
- 这不应简单写成“GLIN 错了”，更谨慎的说法是：

```text
Under our dynamic update protocol, the GLIN-piecewise implementation does not always preserve exact answer equivalence after updates unless the augmentation structure is rebuilt or carefully maintained.
```

中文：

```text
在我们的动态更新协议下，如果 piecewise augmentation 没有被完整维护或重建，
GLIN-piece 更新后可能与 exact oracle 不一致。
```

论文定位：

```text
最重要 learned baseline，但 correctness 结果要谨慎报告。
```

### 5.2 Boost R-tree

Boost R-tree 是传统空间索引 baseline。

优点：

- 支持复杂 geometry 的 envelope candidate search。
- 动态插入删除成熟。
- 通常查询很强。
- 可以作为 correctness oracle 的基础。

缺点：

- 不是 learned index。
- 内存和节点结构成本可能较高。
- 在某些动态吞吐上不如 DELI。

论文定位：

```text
标准强 baseline，必须保留。
```

### 5.3 GEOS Quadtree

Quadtree 是四叉树。

核心思想：

```text
递归把空间切成四块。
对象放到对应空间区域里。
```

优点：

- 传统空间索引 baseline。
- 某些均匀数据上表现不错。
- 实现来自 GEOS，可信度高。

缺点：

- 对复杂分布和大对象敏感。
- 删除/更新性能不一定稳定。
- 查询 candidate 数可能较多。

论文定位：

```text
传统空间索引补充 baseline。
```

## 6. 为什么 DELETE_COMPACT_FRACTION=0.25 合理

从 `results/dynamic_compare_delete_frac_*_2m` 看，`0.25` 是一个合理默认值。

观察到的趋势：

```text
DELETE_COMPACT_FRACTION 越大：
  delete_tps 通常越高；
  local_compaction_count_stage 越低；
  query after_delete 有时略变慢，但没有 correctness 问题。
```

`0.05` 的问题：

```text
block_size=512 时，0.05B 约等于 26。
每个 block 约 26 个 tombstone 就 compact，一次删除阶段会触发几千次 compaction。
删除吞吐被维护成本拖慢。
```

`0.50` 的问题：

```text
几乎完全偏 lazy delete。
删除吞吐可能更高，但更像写入优先配置。
query 和 stale records 的风险更难解释。
```

`0.25` 的好处：

```text
每个 block 最多容忍约 25% tombstone。
这是一条清楚的结构规则。
它显著减少物理 compaction，同时保留较好的 after_delete query latency。
```

因此论文默认可以写：

```text
By default, each compact block tolerates up to 25% local delta entries and 25% tombstones before local physical compaction.
```

中文：

```text
默认情况下，每个 compact block 最多容忍 25% 的局部 delta 和 25% 的 tombstone，
超过后才触发局部物理压缩。
```

## 7. 当前动态实验能证明什么

目前动态实验已经可以支持以下结论：

### 7.1 DELI 动态维护正确

证据：

```text
answers_match_boost = 1
```

含义：

```text
经过 bulk-load、insert、delete 后，
DELI 的最终 exact answers 与 Boost R-tree oracle 一致。
```

这是论文里最重要的底线。

### 7.2 LocalBounded 是比全局 delta 更平衡的设计

对比逻辑：

```text
Hybrid-Buf:
  写入快，但 query 变慢。

Hybrid-Bounded:
  query 恢复，但 full compaction 太重。

LocalBounded:
  局部 delta + 局部 compaction，兼顾 query 和 update。
```

这是一条很清楚的 ablation story。

### 7.3 DELI 不是只会查询，也能动态维护

它不只是：

```text
给 GLIN 查询加一个 maxZmax 补丁。
```

而是：

```text
extent entry
+ interval-overlap pruning
+ dynamic one-sided summaries
+ bounded local maintenance
+ exact refinement
```

这比单纯查询优化更像一个系统。

## 8. 目前还不能过度声称什么

不要写：

```text
DELI 全面超过 R-tree。
DELI 所有指标都最好。
DELI 已经完全证明可以冲 VLDB。
```

更稳妥的写法：

```text
DELI targets exact spatial relationship queries over complex geometries in dynamic settings.
It extends learned ordered layouts with extent-aware records and bounded local maintenance.
It is not a universal replacement for R-tree, but provides a learned-index design point with exact correctness and competitive dynamic performance.
```

中文：

```text
DELI 面向复杂几何 exact spatial relationship query 的动态场景。
它不是万能替代 R-tree，而是把 learned ordered index 扩展到 extent-aware 和 update-aware 的一个设计点。
```

## 9. 论文还需要补哪些实验

当前动态实验很有价值，但若目标是 VLDB，建议继续补以下实验。

### 9.1 默认配置的正式动态对比

固定：

```text
BLOCK_SIZE=512
LOCAL_DELTA_BOUND=128
DELETE_COMPACT_FRACTION=0.25
```

数据集：

```text
AW
LW
PARKS
ROADS
至少再加一个合成 stress dataset，例如 ZGAP_MIXED
```

指标：

```text
after_insert avg/p95/p99 query latency
after_delete avg/p95/p99 query latency
insert_tps
delete_tps
answers_match_boost
candidate_answer_ratio
local_compaction_count_stage
local_compaction_ns_stage
summary_rebuild_count_stage
index memory
```

### 9.2 参数敏感性

不需要无休止调参，只需要证明固定规则合理。

建议：

```text
LOCAL_DELTA_BOUND = 128, 256, 512
DELETE_COMPACT_FRACTION = 0.05, 0.10, 0.25, 0.50
```

论文里最终只选：

```text
default = 25% local delta + 25% tombstone
```

### 9.3 真实内存测量

当前：

```text
index_mb_estimate
```

只是粗略估计，不等价于真实内存。

建议补：

```text
/usr/bin/time -v
或者进程内 peak RSS 采样
```

论文里内存图要尽量用真实 RSS 或明确说明是 estimated memory。

### 9.4 interleaved mixed workload

当前动态比较主要是 staged：

```text
bulk-load 50%
insert 20%
query
delete 10%
query
```

建议再补一种 interleaved workload：

```text
query / insert / delete 混合交错执行
```

这样更接近真实在线系统。

### 9.5 update = delete + insert

空间数据库里的 update 通常是：

```text
object_id 不变，geometry 改了
```

建议补：

```text
replace workload
```

也就是：

```text
delete old geometry + insert new geometry with same object_id
```

### 9.6 查询侧完整实验继续保留

动态实验不能替代查询实验。

仍然要保留：

```text
selectivity sweep
block size sensitivity
candidate/answer ratio
ZGAP_MIXED 或 fat-object stress
IO_BLOCK_MBR / IO_OVERFLOW 对比
Boost R-tree / Quadtree / GLIN-piece baseline
```

## 10. VLDB 初稿下一步怎么改

当前初稿如果还主要写查询优化，会显得不成体系。建议下一步按下面结构重写。

### 10.1 论文主线

建议主线：

```text
Problem:
  GLIN-style learned spatial indexes are Zmin-centric and extent-blind.

Observation:
  Complex geometries need interval and MBR-aware pruning.
  Dynamic updates require conservative summary maintenance.

Method:
  DELI extends learned ordered layouts with extent entries,
  one-sided block summaries,
  and bounded local maintenance.

Guarantee:
  summary can be stale-large but never stale-small,
  so pruning is false-negative-free;
  final GEOS exact refinement removes false positives.

Evaluation:
  query pruning,
  dynamic correctness,
  dynamic throughput,
  latency tail,
  memory,
  ablation.
```

### 10.2 建议章节

```text
1. Introduction
2. Background and Motivation
3. DELI Overview
4. Query Processing
5. Dynamic Maintenance
6. Correctness Argument
7. Experimental Setup
8. Query Evaluation
9. Dynamic Evaluation
10. Related Work
11. Conclusion
```

### 10.3 核心贡献写法

建议贡献点写成三条：

```text
1. Extent-aware learned spatial entries:
   从 Zmin point key 扩展为 <Zmin, Zmax, MBR, object_id>。

2. Safe exact query processing:
   interval-overlap pruning + block MBR + record MBR + GEOS exact refinement。

3. Dynamic bounded local maintenance:
   local delta + tombstone + one-sided conservative summary + local compaction。
```

## 11. 哪些方法是真正可用的

### 推荐作为主方法

```text
DELI-ALEX-Hybrid-LocalBounded
```

原因：

- correctness 稳。
- query/update 比较平衡。
- 单线程同步维护，公平性好解释。
- 有清晰理论和系统机制。

### 推荐作为 ablation

```text
DELI-ALEX
DELI-ALEX-Hybrid
DELI-ALEX-Hybrid-Buf
DELI-ALEX-Hybrid-Bounded
DELI-Dynamic
```

原因：

- 它们解释设计演化过程。
- 能证明为什么最终需要 LocalBounded。

### 推荐作为 baseline

```text
GLIN-piece
Boost R-tree
GEOS Quadtree
```

原因：

- GLIN-piece 是相关 learned baseline。
- Boost R-tree 和 GEOS Quadtree 是传统空间索引 baseline。

## 12. 最终建议

目前动态实验已经足以说明：

```text
DELI 的动态维护方向是成立的。
LocalBounded 是目前最合理的同步系统版本。
DELETE_COMPACT_FRACTION=0.25 是一个好默认值。
```

但如果目标是 VLDB，还需要把论文从“很多实验变体”收束成一个系统：

```text
DELI = extent-aware learned ordered index
     + exact query path
     + dynamic bounded local maintenance
```

接下来最重要的不是继续发明新方法，而是：

```text
1. 固定主方法和默认参数。
2. 补完整 query + dynamic 实验矩阵。
3. 整理论文结构和贡献点。
4. 把 ablation 讲成设计选择，而不是零散方法堆叠。
```

