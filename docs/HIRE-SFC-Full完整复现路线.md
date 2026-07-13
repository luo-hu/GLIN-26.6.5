# HIRE-SFC-Full 完整复现路线

## 1. 目标与命名

本项目保留两个彼此独立的实验标签：

```text
HIRE_SFC_LITE
HIRE_SFC_FULL
```

`HIRE_SFC_LITE` 是已经稳定运行的 HIRE-inspired baseline，不再把新的复杂机制
直接叠加到该标签上。`HIRE_SFC_FULL` 是按论文逐阶段实现的完整复现路径。在所有
阶段完成以前，论文和图例必须标注当前阶段，不能把开发中的版本表述为完整 HIRE。

原始 HIRE 是一维有序索引。本项目处理线、面等非点对象，因此完整实验结构仍是：

```text
geometry
  -> SFC extent <zmin, zmax>
  -> HIRE one-dimensional ordered index on zmin
  -> conservative zmax/MBR filtering
  -> GEOS exact refinement
```

SFC extent wrapper 与 GEOS refinement 属于空间适配层；混合叶、内部节点、动态维护、
重校准和 bulk loading 才是 HIRE 复现主体。

## 2. 论文机制与当前状态

| 论文组件 | Lite 状态 | Full 状态 | 完整复现要求 |
|---|---|---|---|
| Hybrid leaves | Model/Legacy 近似 | Stage 2 已完成 | buffer hashmap、legacy in-place split/merge、sibling scan |
| Model leaf deletion | mask/tombstone | 已接入 | 保持 slot 与模型位置不变，buffer 删除采用 swap-last |
| Deleted-slot reuse | 已接入 | 已接入 | 按预测位置与有序约束复用 |
| Learned internal nodes | 单层扁平 directory | Stage 1A/1B 已完成 | 多层模型、gap array、节点级 log、split/merge 上推 |
| Cost model | `query_count * buffer_size` 近似 | Stage 3 已完成 | 时间窗、主动/被动触发、实测 gain/cost |
| Non-blocking recalibration | 前台 pending 标记 | Stage 4 已完成 | PAP/MLS snapshot、更新日志、后台线程、原子安装 |
| RCU | 无 | Stage 4 已完成 | immutable reader view、shared_ptr grace period、安全回收旧版本 |
| Legacy transformation | 相邻 legacy 尝试合并 | Stage 5 已完成 | forward/backward transformation、系数筛选、低填充降级、RCU 安装 |
| Inter-level bulk loading | 大叶优先拟合近似 | Stage 6 已完成 | delta-window partition、在线 RLS、逐层 O(N) 构建 |

## 3. 分阶段实现顺序

### Stage 0：基线冻结与可观测性

状态：已完成。

- 保留 `HIRE_SFC_LITE` 的默认行为。
- 新增独立标签 `HIRE_SFC_FULL`。
- correctness 仍以 Boost R-tree exact answer 为 oracle。
- Full 的新增结构统计写入 `hire_sfc_debug.csv`。

### Stage 1：真实内部节点

#### Stage 1A：层级查询、gap array 与节点日志

状态：已完成并通过 smoke test。

- 自底向上构建多层 internal nodes。
- 每个节点存储带空槽的 key-pointer array。
- 每个节点拟合线性模型并记录最大误差。
- 模型误差小于半个节点容量时走 model prediction + correction，否则走保守扫描。
- 查询同时检查 primary slots 和 node-local log，选择覆盖 key 的最紧 lower-bound child。
- 叶边界扩张写入对应 internal node log，并逐层传播父边界。
- log 超过节点 fanout 比例后只 compact 当前节点，不重建整棵 directory。

实现文件：

```text
src/benchmark/hire_internal_directory.h
src/benchmark/hire_sfc_lite_index.h
```

#### Stage 1B：结构更新

状态：已完成并通过确定性结构测试、mixed correctness smoke 和 ASan。

- leaf/internal node 使用稳定 node id，不依赖 vector ordinal。
- legacy leaf overflow 后立即 split，并把新 key-pointer 上推父节点。
- 预测 slot 是合法 gap 时原位写入，否则 append 到 log。
- primary + log child count 超过 fanout 时 split internal node，并递归上推。
- child merge 时优先删除 log entry；primary child 使用 mask 删除。
- internal underflow 根据相邻节点总 child 数执行 redistribution 或 merge。
- internal split/merge/redistribution 后重新拟合模型并 remap slots，生成新 gaps。
- model leaf local rebuild、legacy transformation 和 leaf split/merge 均使用增量父节点更新。
- Full 运行期不再调用全局 `rebuild_directory()`；该计数只在 bulk-load 初始化时为 1。

确定性测试位于：

```text
test/hire_internal_directory_test.cpp
```

它使用 fanout=4，逐操作对照有序 lower-bound reference，并强制覆盖 internal split、
merge、redistribution、gap/log insert 和 masked delete。随机结构压力测试还覆盖了 leaf
split、merge 和 redistribution。

### Stage 2：论文级混合叶更新

状态：已完成并通过确定性测试、三组 mixed correctness smoke 和 ASan。

- Full model leaf buffer 使用 `vector + unordered_map<oid, position>`；Lite
  继续保持原有 buffer 行为。
- buffer insert 同步写入位置哈希，buffer delete 通过哈希定位后执行
  swap-last，并修正被移动记录的位置，平均复杂度为 O(1)。
- buffer delete 只递减 live count，保留原有 `min/max z` 与 MBR 作为保守 stale
  上界，避免在前台重新扫描整叶；下一次 local rebuild 会恢复精确摘要。
- model 主数组仍采用 lazy tombstone 和 deleted-slot reuse，物理 buffer 删除不制造
  tombstone。
- Full legacy leaf 使用预分配的紧凑有序数组，容量固定为
  `HIRE_SFC_LEAF_SIZE`；普通插入/删除原位移位，不触发叶内重新分配。
- legacy 满载插入只在临时数组中合并新记录，然后生成两个固定容量叶并把 separator
  增量上推；underflow 继续复用 Stage 1B 的 merge/redistribution。
- 每个 Full leaf 维护稳定 `prev_leaf_id/next_leaf_id`。split、merge、rebuild 和
  legacy transformation 后统一重连，并用稳定 node id 解析当前叶地址。
- Full 范围查询先由内部 directory 定位 `qmax` 所在叶，再沿 `prev_leaf_id` 扫描前缀。
  这是空间 extent wrapper 所需的保守方向，可覆盖 `zmin < qmin <= zmax` 的对象。
- 重复 SFC key 继续使用稳定 `(zmin, zmax, oid)` 顺序。

确定性测试位于：

```text
test/hire_sfc_stage2_test.cpp
```

测试分别强制 model/legacy 两类叶，覆盖 buffer hashmap、swap-last、固定容量、leaf
split/merge、双向 sibling 链和 brute-force range-query 对照。

### Stage 3：成本驱动触发器

状态：已完成并通过确定性触发测试、mixed correctness smoke 和 ASan。

- 每个 Full leaf 使用固定数量时间桶维护滑动窗口 `Tq` 内的查询计数 `Ql`。桶数组
  仅在叶第一次被查询时分配，空间上界固定，不保存逐查询时间戳。
- 主动触发要求同时满足 `Ql >= Qth`、`Bl >= Bth`，并通过下面的成本边界。
- 被动触发在 `Bl >= tau` 时无条件执行，其中 `tau=HIRE_SFC_BUFFER_LIMIT`。
- 插入、model 主数组删除和 model buffer 删除后都会重新评估成本模型。
- 每隔 `HIRE_SFC_COST_SAMPLE_EVERY` 次叶访问在线采样 buffer scan 和 model data-list
  scan；使用每项纳秒成本 EMA 降低计时噪声。
- local rebuild 分别计时 buffer merge/sort 与 model fit/segmentation，并按输入记录数
  维护 `merge_ns_per_record` 和 `fit_ns_per_record` EMA。
- model range scan 的每项实测成本乘以 `2*ceil(error)+1`，作为论文中常数级
  `c_model` correction cost 的近似；buffer 成本按当前 `Bl` 线性外推。
- 使用论文边界：

```text
Cgain = Ql * (c_buffer(Bl) - c_model)
trigger iff Cgain > C_retrain
```

- 将触发原因、预测收益、实际耗时和误差变化写入 debug CSV。
- Stage 3 完成的是触发决策与 CPU 成本闭环；默认配置下 local rebuild 仍在前台同步
  执行。`HIRE_SFC_ENABLE_BACKGROUND_RECALIBRATION=1` 目前也只是延迟到后续更新点处理，
  不等价于论文中的后台 worker、MLS update log 与 RCU 原子安装，这些由 Stage 4 实现。

Stage 3 参数：

```text
HIRE_SFC_QUERY_WINDOW_US=1000000
HIRE_SFC_QUERY_WINDOW_BUCKETS=8
HIRE_SFC_ACTIVE_QUERY_THRESHOLD=32
HIRE_SFC_ACTIVE_BUFFER_THRESHOLD=32
HIRE_SFC_BUFFER_LIMIT=128
HIRE_SFC_COST_SAMPLE_EVERY=64
HIRE_SFC_COST_EMA_ALPHA=0.20
HIRE_SFC_COST_BUFFER_NS_PER_ENTRY=3.0
HIRE_SFC_COST_MODEL_NS_PER_ENTRY=3.0
HIRE_SFC_COST_MERGE_NS_PER_RECORD=10.0
HIRE_SFC_COST_FIT_NS_PER_RECORD=20.0
```

最后四项只在尚无在线样本时作为 cold-start 先验；获得样本后由 EMA 替代。

确定性测试位于 `test/hire_sfc_stage3_cost_test.cpp`，覆盖 active trigger、passive
trigger、成本拒绝、窗口过期，以及四类成本采样。

### Stage 4：PAP、MLS 与非阻塞 RCU 重校准

状态：已完成并通过确定性并发测试与 ASan。

- 使用论文公式 `sigma=ceil((|data|+|buffer|)/f)-1` 估计最坏新增叶数；从目标 leaf
  parent 向上逐层扣除空余 fanout，直到 `sigma` 被吸收，得到 PAP 和最高 MLS root。
- PAP snapshot 保存稳定 internal node ID、parent ID、child 数量、空余 slot 和 MLS
  覆盖的 leaf ID 集合。
- 每个 active MLS 有独立、单调 sequence 的 index-level update log。前台 insert/delete
  先更新 live tree，再将 MLS 范围内操作写入日志。
- 单后台 worker 在 leaf 副本上执行 merge、日志重放、排序和分段准备；安装前循环检查
  sequence，只有日志完全追平才进入 writer 临界区。
- read path 使用 immutable `ReadSnapshot`。snapshot 包含 learned internal directory
  副本、stable leaf-ID map，以及按 leaf 版本复用的 immutable record view。
- query 只执行一次原子 `shared_ptr` load，不持有 writer/MLS 锁；仍通过 learned internal
  nodes 定位 leaf，并沿快照中的有序 leaf view 反向扫描。
- 安装时短暂持有单 writer lock，增量更新 live internal directory，然后用
  `atomic_exchange(shared_ptr)` 发布完整新版本。旧 snapshot 在最后一个 reader 释放引用后
  自动结束 grace period并回收。
- RCU query 使用原子时间桶记录热度，并以无锁原子累计 model/buffer scan 的实测 CPU
  成本，因此 Stage 3 的主动触发在 Stage 4 模式下仍然有效。

当前实现与论文在发布粒度上有一个明确区别：论文交换 MLS parent-child pointer；本项目
交换 index-level immutable read-root pointer。后者原子性更粗、会额外复制 internal
directory 元数据，但不会复制未变化 leaf 的记录，并能在现有 vector-based writer tree
上提供等价的无锁 reader 与 grace-period 语义。

Stage 4 开关：

```text
HIRE_SFC_ENABLE_BACKGROUND_RECALIBRATION=1
HIRE_SFC_ENABLE_RCU_RECALIBRATION=1
```

`HIRE_SFC_BACKGROUND_TEST_DELAY_US` 仅用于确定性并发测试，正式实验保持 `0`。

### Stage 5：Legacy 双向转换

状态：已完成。

- forward transformation：legacy leaf 更新后，临时拟合它与左侧 model leaf 的局部回归
  系数；斜率和首键预测偏移接近时，将两者快照为同一个 MLS job，并对合并数据执行
  `epsilon` 拟合。只有全部记录可由单个 model leaf 表示时才安装新版本。
- backward transformation：发现至少两个连续 legacy leaves 且相邻局部系数接近后，使用
  greedy PLA 从最长可行前缀开始切分；每个输出段同时满足 `[alpha, beta]` 和 `epsilon`
  约束，成功后生成一个或多个 model leaves。
- model downgrade：删除使 model leaf 的 live count 低于 `alpha` 时，异步转换为定长
  legacy leaf，保留 stable node ID 和 sibling links。
- 拟合失败、目标 leaf 消失或 stable IDs 不再连续时不修改当前树，只清除 pending 标记并
  记录 abort；reader 始终读取完整旧 snapshot 或完整新 snapshot。
- 所有转换复用 Stage 4 的 PAP snapshot、MLS update log/replay、后台 worker 和 RCU 原子
  发布。internal directory 只对替换区间执行 boundary update、remove 和 insert，不做全局
  rebuild。

Stage 5 参数：

```text
HIRE_SFC_LEGACY_TRANSFORM_MAX_LEAVES=4
HIRE_SFC_LEGACY_BACKWARD_MIN_LEAVES=2
HIRE_SFC_LEGACY_SLOPE_TOLERANCE=0.25
HIRE_SFC_LEGACY_INTERCEPT_TOLERANCE=64
```

`HIRE_SFC_LEGACY_INTERCEPT_TOLERANCE` 比较的是各 leaf 首键处的局部预测偏移，不是会随
key 区间平移的原始回归截距。最终正确性门槛始终是合并数据上的 `epsilon`，系数阈值只用于
廉价候选过滤。

`last_recalibration_job_kind` 编码为 `1=ModelRetrain`、`2=ForwardMerge`、
`3=BackwardMerge`、`4=ModelDowngrade`。

### Stage 6：Inter-level optimized bulk loading

状态：已完成。

- 先按 `epsilon/alpha/beta` 规则线性扫描有序数据，只生成候选叶分段位置；候选试探不再
  分配 stable node ID，最终边界确定后才实例化 leaves。
- 每个父节点使用前 `ceil(fanout * seed_fraction)` 个 child separators 拟合初始线性模型，
  默认等价于论文的 `fanout/4`。
- 后续原始切分点只在 `+/- delta` key window 内搜索，目标为最小化
  `abs(F(key) - mapped_slot)`；候选必须保证左右 leaves 仍满足容量限制，超过普通 leaf
  容量时还必须满足 `alpha/beta/epsilon` 模型约束。
- 每接受一个 separator 后使用二维 Recursive Least Squares 更新截距、斜率和协方差矩阵。
- 同一个 partition optimizer 递归应用到所有 internal levels；internal 候选同时受
  min-fill 和 fanout 限制，节点数量及树高不因优化增加。
- `delta` 是固定系统参数，因此每个 separator 至多检查 `2 * delta + 1` 个候选，总复杂度
  保持 `O(N)`。

Stage 6 参数：

```text
HIRE_SFC_ENABLE_INTER_LEVEL_BULK_LOAD=1
HIRE_SFC_BULK_DELTA=32
HIRE_SFC_BULK_SEED_FRACTION=0.25
```

该开关只改变 `HIRE_SFC_FULL` 的初始构建分区，设为 `0` 可执行普通 bottom-up bulk-load
消融。实验同时报告平均和最大 internal model error；论文优化目标对应跨层平均误差，不能
只挑其中一个指标报告。

### Stage 7：复现验收

状态：功能、随机 oracle、RCU 压力回归和 ASan/UBSan 验收已完成；TSan 仍受当前
虚拟机地址映射环境限制，论文性能消融需在正式实验机执行。

功能验收：

- bulk-load、insert、delete、reinsert 后 `answers_match_boost=1`。
- 随机 point/range query 与有序 reference vector 一致。
- 重复 key、全相同 key、极端 skew、单调 append、随机 delete 均通过。
- 强制每次触发 leaf/internal split、merge、log compact 和 MLS replacement。

并发验收：

- ThreadSanitizer 无 data race。
- AddressSanitizer/UndefinedBehaviorSanitizer 无错误。
- reader 在 RCU replacement 前后只观察到完整旧版本或完整新版本。
- MLS log 中的更新全部且仅重放一次。

性能验收：

- 分开报告 foreground latency 与 background CPU cost。
- 报告 p50/p95/p99/p99.9 query/insert/delete latency。
- 对 `HIRE_SFC_LITE` 做逐阶段消融，避免把空间 wrapper 的收益误归因于 HIRE。

本阶段新增 `test_hire_sfc_stage7_acceptance`，固定随机种子为 `20260713`，覆盖重复 key、
全相同 key、极端 skew、单调 append、changed-key reinsert 和 600 次随机 delete/reinsert。
每 50 次更新都将随机 point/range query 与 brute-force spatial oracle 比较，并检查：

```text
directory_rebuild_count == 1
broken_sibling_link_count == 0
unsorted_main_leaf_count == 0
out_of_order_leaf_count == 0
stale_leaf_summary_count == 0
pending_rebuild_count == 0
mls_update_log_entries == mls_update_replay_count
```

验收过程中修复了四类此前 smoke 未覆盖的正确性问题：

1. 相同 separator 跨多个 leaves 时，query 必须从 learned directory 命中的第一个相等
   leaf 继续向右找到最后一个 `min_zmin <= qmax` 的 leaf；insert 则路由到最右相等 leaf。
2. model leaf 的 lazy deletion 改为 leaf-local tombstone identity，避免同一 ID 换 key 重插后
   全局 `alive=true` 使旧槽复活；同叶重插优先复用自己的旧槽，不合适时先移除旧空槽。
   deleted-slot placement 使用完整 `(zmin, zmax, id)` comparator，而不是只比较 `zmin`。
3. RCU 安装增加双向 final validation：不仅验证后台结果仍属于目标 MLS，还验证目标 MLS
   当前每个活记录都包含在结果中。若 sequence window 发生变化，在 writer 临界区按当前
   MLS 内容重新分段，绝不以不完整后台结果覆盖 live tree。修正次数记录在
   `mls_final_validation_repair_count`。
4. insert 在 learned-directory 路由后按相邻 leaf max boundary 做局部校正，防止重校准交错
   期间把较小 key 放入右侧 leaf；后台 job 完成或安全 abort 后局部回收非 pending 空叶，
   并通过增量 remove 维护 directory，不触发全局 rebuild。

mixed benchmark 的原始 CSV、summary 和绘图链路现已同时输出 query/insert/delete 的
`p99.9` 延迟。样本数量不足 1000 时，`p99.9` 会自然退化到接近最大值，论文中必须同时
报告每个 interval 的操作样本数。

## 4. Stage 1 参数与统计

新增参数：

```text
HIRE_SFC_INTERNAL_FANOUT=64
HIRE_SFC_INTERNAL_GAP_FRACTION=0.20
HIRE_SFC_INTERNAL_LOG_FRACTION=0.10
HIRE_SFC_INTERNAL_MIN_FILL=0.40
HIRE_SFC_LEGACY_MIN_FILL=0.40
```

新增 `hire_sfc_debug.csv` 字段：

```text
internal_node_count
internal_levels
internal_primary_entries
internal_gap_slots
internal_log_entries
internal_log_compactions
internal_boundary_updates
internal_split_count
internal_merge_count
internal_redistribution_count
internal_gap_insert_count
internal_log_insert_count
internal_masked_delete_count
leaf_split_count
leaf_merge_count
leaf_redistribution_count
buffer_hash_entries
buffer_swap_delete_count
legacy_forward_attempt_count
legacy_forward_success_count
legacy_backward_attempt_count
legacy_backward_success_count
model_downgrade_count
legacy_transform_abort_count
legacy_coefficient_reject_count
last_transform_input_leaves
last_transform_input_records
last_recalibration_job_kind
inter_level_bulk_enabled
bulk_leaf_boundaries_considered
bulk_leaf_boundaries_shifted
bulk_leaf_candidate_evaluations
bulk_leaf_rls_updates
bulk_leaf_max_shift
bulk_internal_optimized_levels
bulk_internal_boundaries_considered
bulk_internal_boundaries_shifted
bulk_internal_candidate_evaluations
bulk_internal_rls_updates
bulk_internal_max_shift
bulk_load_ns
bulk_internal_build_ns
avg_internal_model_error
legacy_slots_used
legacy_slot_capacity
sibling_link_count
broken_sibling_link_count
unsorted_main_leaf_count
out_of_order_leaf_count
stale_leaf_summary_count
active_retrain_trigger_count
passive_retrain_trigger_count
cost_retrain_rejected_count
query_window_total
query_window_max
buffer_scan_sample_count
model_scan_sample_count
merge_sample_count
fit_sample_count
buffer_scan_ns_per_entry_ema
model_scan_ns_per_entry_ema
merge_ns_per_record_ema
fit_ns_per_record_ema
last_estimated_gain_ns
last_estimated_retrain_ns
last_rejected_gain_ns
last_rejected_retrain_ns
last_actual_retrain_ns
last_retrain_error_before
last_retrain_error_after
last_retrain_trigger_reason
pap_snapshot_count
pap_max_levels
mls_install_count
mls_update_log_entries
mls_update_replay_count
mls_final_validation_repair_count
rcu_snapshot_publish_count
rcu_retired_snapshot_count
rcu_reclaimed_snapshot_count
rcu_active_reader_count
background_job_count
background_job_abort_count
last_pap_levels
last_pap_sigma
last_mls_covered_leaves
max_internal_model_error
```

## 5. Stage 1/2 smoke 命令

```bash
RESET_RESULTS=1 OVERWRITE=1 AUTO_BUILD=1 \
PREDICATE_SHORTCUTS=1 \
INDEXES="HIRE_SFC_FULL HIRE_SFC_LITE Boost_Rtree" \
CHECK_CORRECTNESS=1 \
WORKLOAD_MODE=mixed MIXED_PROFILES="balanced" \
MIXED_OPERATIONS=4000 MIXED_CHECKPOINT_INTERVAL=1000 \
DATASETS="AW" LIMIT=50000 QUERY_LIMIT=50000 \
QUERY_ROOT=queries/interval_overlap_full_50000 \
SELECTIVITY_TAGS="0p1pct" QUERY_COUNT=20 \
RESULT_DIR=results/smoke_hire_sfc_full_stage1 \
FIGURE_DIR=figures/smoke_hire_sfc_full_stage1 \
BUILD_DIR=build_current \
./scripts/run_dynamic_compare_diagnostics.sh
```

验收时不能只看 `answers_match_boost`。还要确认：

```text
internal_levels >= 2
internal_node_count > 1
internal_boundary_updates > 0
internal_log_compactions > 0
missing_answers = 0
extra_answers = 0
```

Stage 2 叶层确定性测试：

```bash
cmake --build build_current --target test_hire_sfc_stage2 -j2
./build_current/test_hire_sfc_stage2
```

Stage 3 成本模型确定性测试：

```bash
cmake --build build_current --target test_hire_sfc_stage3_cost -j2
./build_current/test_hire_sfc_stage3_cost
```

Stage 4 并发 RCU 测试：

```bash
cmake --build build_current --target test_hire_sfc_stage4_rcu -j2
./build_current/test_hire_sfc_stage4_rcu
```

该测试在同一 MLS 重训练窗口内制造 model-buffer insert、main-list delete 和 buffer
delete，并让四个 reader 跨越安装点持续查询。验收要求最终 oracle 一致、
`mls_update_log_entries == mls_update_replay_count`、`background_job_abort_count == 0`、
`pending_rebuild_count == 0`，且 retire/reclaim 均发生。测试还覆盖 RCU 原子时间桶驱动的
active trigger，确保 Stage 3 成本模型在 Stage 4 模式下保持工作。

当前虚拟机可成功构建 TSan 版本，但运行时在测试入口前报
`ThreadSanitizer: unexpected memory mapping`；PIE 与 non-PIE 均相同，因此暂不能把 TSan
列为已通过项。ASan 与实际四 reader 并发压力测试均已通过。

Stage 5 双向转换测试：

```bash
cmake --build build_current --target test_hire_sfc_stage5_transform -j2
./build_current/test_hire_sfc_stage5_transform
```

该测试分别覆盖 model+legacy forward merge、连续 legacy backward PLA merge、非线性候选
拟合失败后旧树保持不变，以及低填充 model downgrade。每条路径同时核对最终 record IDs、
leaf 类型、转换计数、pending 清理和 sibling link 完整性。

Stage 6 inter-level bulk-loading 测试：

```bash
cmake --build build_current --target test_hire_sfc_stage6_bulk -j2
./build_current/test_hire_sfc_stage6_bulk
```

该测试先直接检查 delta-window optimizer 的边界、分区覆盖和 RLS 更新，再对同一组非线性
keys 分别执行普通与 optimized bulk-load。验收要求答案集合、leaf 数和树高不变，至少一个
internal separator 被移动且不超过 `delta`，并确认平均 internal model error 不高于普通构建。

Stage 7 一键验收：

```bash
STRESS_RUNS=10 ./scripts/run_hire_sfc_stage7_acceptance.sh
```

默认执行 Release 全量 CTest、Stage 7 十次 RCU/oracle 复跑，以及 HIRE 测试的
ASan/UBSan。LeakSanitizer 在当前 ptrace 容器环境中无法可靠运行，因此脚本显式使用
`ASAN_OPTIONS=detect_leaks=0`；这不影响 AddressSanitizer 和 UndefinedBehaviorSanitizer。
结果和完整环境信息写入 `results/hire_sfc_stage7_acceptance/acceptance.log`。

真实 AW + Boost oracle smoke：

```bash
RUN_BENCHMARK_SMOKE=1 STRESS_RUNS=10 \
  ./scripts/run_hire_sfc_stage7_acceptance.sh
```

在允许 TSan 预留 shadow memory 的原生 Linux 实验机上执行：

```bash
RUN_TSAN=1 TSAN_REQUIRED=1 RUN_ASAN=0 \
  ./scripts/run_hire_sfc_stage7_acceptance.sh
```

当前虚拟机若出现 `FATAL: ThreadSanitizer: unexpected memory mapping`，表示 TSan runtime
无法建立其固定 shadow-memory 布局，不是已经检测到代码 data race。此项必须迁移到原生
Linux、关闭 ASLR 的隔离测试环境或兼容的容器/内核后重新验收，不能在论文中写成
“TSan passed”。

正式 mixed smoke 还应检查：

```text
buffer_hash_entries == buffer_records
legacy_slots_used <= legacy_slot_capacity
broken_sibling_link_count == 0
directory_rebuild_count == 1
```

`last_retrain_trigger_reason` 的编码为 `0=None`、`1=Active`、`2=Passive`。分析正式
实验时，active 行应满足 `last_estimated_gain_ns > last_estimated_retrain_ns`，最近一次
被拒绝的判断则应满足 `last_rejected_gain_ns <= last_rejected_retrain_ns`。

## 6. 当前结论

`HIRE_SFC_FULL` 已完成 Stage 1A/1B、Stage 2、Stage 3、Stage 4、Stage 5、Stage 6，以及
Stage 7 的功能和 sanitizer 验收。它现在具备稳定 node id、动态 child
split/merge 上推、局部 directory 更新、哈希 model buffer、swap-last buffer deletion、
固定容量 legacy leaf、稳定 sibling range scan、时间窗实测成本模型，以及 PAP/MLS
后台 RCU recalibration、论文级 legacy leaf 双向转换、逐层 RLS bulk-loading，以及
final-validated RCU installation。剩余验收项是迁移到兼容 TSan 的实验机完成 data-race
检查，并运行论文规模的逐阶段性能消融；在这两项完成前，应称为“功能复现完成”，而不是
“论文全部实验验收完成”。
