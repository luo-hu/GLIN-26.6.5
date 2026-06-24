我基于你已经有**固定块大小、固定 delta/tombstone 阈值、基础 compaction** 的 DELI 基础版本，给你一套**增量式、可验证、低风险**的代码实现计划。

Codex 2026-06-24 修订结论：

```text
这条路线总体可行，但不能把 5 个阶段一次性全塞进主代码。
当前最稳的实现顺序是：
  先做 DELI-Cost-Lite v1：
    per-block 统计
    per-block beta/tau 闭式自适应
    benefit-cost local compaction

  暂缓：
    离线 DP 自适应 block 划分
    在线 split/merge 自适应
    Limbo / Bayesian Optimization 集成
```

原因：

```text
1. per-block beta/tau 和 benefit-cost compaction 改动局部，能直接和固定 LocalBounded 对比。
2. DP block partition 会改变 block layout，和 beta/tau 同时改会很难判断收益来源。
3. 在线 split/merge 会牵涉 object_to_block、directory 顺序和 ALEX 写路径，风险更高。
4. BO 如果过早接入，容易把论文故事变成“黑盒调参”，不如先用确定性 cost model。
```

本轮代码已经新增：

```text
DELI_ALEX_HYBRID_COST
```

它是在 `DELI_ALEX_HYBRID_LOCAL_BOUNDED` 旁边新增的独立对比方法，不会污染原始固定参数版本。

当前执行路径：

```text
runner:
  scripts/run_dynamic_compare_diagnostics.sh

C++ benchmark:
  src/benchmark/bench_dynamic_compare_wkt.cpp

新增 index 名：
  DELI_ALEX_HYBRID_COST

绘图脚本：
  scripts/plot_dynamic_compare_diagnostics.py
```

运行示例：

```bash
RESET_RESULTS=1 OVERWRITE=1 \
INDEXES="DELI_ALEX_HYBRID_LOCAL_BOUNDED DELI_ALEX_HYBRID_COST Boost_Rtree GLIN_PIECEWISE" \
WORKLOAD_MODE=mixed \
MIXED_PROFILES="read_heavy balanced write_heavy" \
MIXED_OPERATIONS=50000 \
MIXED_CHECKPOINT_INTERVAL=1000 \
DATASETS="AW PARKS" \
LIMIT=500000 QUERY_LIMIT=500000 \
QUERY_ROOT=queries/interval_overlap_full_500000 \
RESULT_DIR=results/dynamic_compare_cost_0.5m \
FIGURE_DIR=figures/dynamic_compare_cost_0.5m \
SELECTIVITY_TAGS="0p1pct" \
QUERY_COUNT=200 \
CHECK_CORRECTNESS=0 \
AUTO_BUILD=1 BUILD_DIR=build_current \
  ./scripts/run_dynamic_compare_diagnostics.sh
```

新增可调参数：

```text
COST_EMA_ALPHA
  中文：热度统计的指数移动平均平滑系数。
  默认 0.10。

COST_BETA_MIN / COST_BETA_MAX
  中文：beta，也就是局部增量比例的下界和上界。
  默认 0.25 / 0.50。
  0.25 对应 fixed LocalBounded 的 LOCAL_DELTA_BOUND=128、BLOCK_SIZE=512。
  当前 Cost 默认只允许比 fixed LocalBounded 更宽松，不默认做更激进的压实。

COST_TAU_MIN / COST_TAU_MAX
  中文：tau，也就是墓碑比例的下界和上界。
  默认 0.25 / 0.50。
  0.25 对应 fixed LocalBounded 的 DELETE_COMPACT_FRACTION=0.25。

COST_SCAN_PER_ENTRY
  中文：扫描一条 entry 的相对成本。
  默认 1.0。

COST_COMPACT_PER_ENTRY
  中文：压实一条 entry 的相对成本。
  默认 5.0。

COST_COMPACTION_HORIZON
  中文：benefit-cost compaction 估计未来收益时看的操作窗口。
  默认 0。
  0 表示关闭主动提前压实，只保留阈值触发压实。
  如果显式设为正数，才启用热 block 的 benefit-cost 提前压实实验。

COST_MIN_COMPACT_INTERVAL
  中文：同一个 block 两次 local compaction 之间至少间隔多少次操作。
  默认 64。
```

新增输出指标：

```text
avg_beta
  当前所有 block 的平均 beta。

avg_tau
  当前所有 block 的平均 tau。

avg_adaptive_delta_bound
  当前每个 block 自动算出的 delta 上限的平均值。

avg_adaptive_delete_bound
  当前每个 block 自动算出的 tombstone 上限的平均值。
```

这些指标的作用：

```text
如果 read-heavy 下 avg_beta / avg_tau 自动下降，
说明方法在主动保护查询。

如果 write-heavy 下 avg_beta / avg_tau 自动上升，
说明方法在减少 compaction，保护写入吞吐。

如果它们不随 workload 改变，说明 cost model 参数还需要校准。
```

---

# 整体实现路线总览

| 阶段 | 内容                                          | 代码改动量 | 核心价值                         | 优先级 |
| ---- | --------------------------------------------- | ---------- | -------------------------------- | ------ |
| 1    | 基础设施：per-block 统计埋点 + 统一代价常量   | 小         | 所有自适应逻辑的地基             | 最高   |
| 2    | 核心功能1：per-block 自适应 β / τ           | 中         | 摆脱固定阈值，核心论文贡献       | 最高   |
| 3    | 核心功能2：Benefit-Cost 驱动的 compaction     | 中         | 从「阈值触发」升级为「代价触发」 | 高     |
| 4    | 进阶功能：离线DP自适应块划分（bulk-load阶段） | 中         | 自适应块划分，补充理论完整性     | 中     |
| 5    | 可选进阶：在线局部分裂/合并                   | 大         | 全动态自适应，锦上添花           | 低     |

> 核心原则：**先做改动小、收益大的，每一步都保留正确性校验，每一步都能出实验结果**。前3个阶段做完，DELI-Cost-Lite的核心贡献就已经完整了，足够支撑论文主体。

---

## 阶段1：基础设施改造 —— 统计埋点 + 统一代价常量

**目标**：给每个block加上热度统计能力，定义好全局单位成本常量，为后面的自适应计算提供数据基础。
**预计工作量**：2-3天

### 1.1 给 Block 结构体新增统计字段

在你的 `Block`类/结构体里，新增以下字段（都放在块元数据里，不占主数据空间）：

```cpp
struct Block {
    // ========== 原有字段 ==========
    vector<ExtentEntry> compact;  // 主数据区
    vector<ExtentEntry> delta;    // 增量缓存
    int tombstone_count;          // 墓碑数量
    BlockSummary summary;         // 块摘要（max_zmax, mbr等）

    // ========== 新增统计字段 ==========
    // 热度统计（指数移动平均 EMA，反映近期负载）
    double query_hits_ema;    // 被查询访问的频率（单位时间次数）
    double insert_hits_ema;   // 插入命中的频率
    double delete_hits_ema;   // 删除命中的频率

    // 自适应参数（每个block自己算）
    double beta;    // 局部增量比例
    double tau;     // 墓碑比例

    // 辅助统计
    uint64_t last_compact_ts;  // 上次压实时间戳（微秒）
    int compaction_count;      // 压实次数（用于实验统计）
};
```

### 1.2 关键实现细节：用 EMA 做平滑统计

不要用累计总次数，要用**指数移动平均（EMA）** 统计频率，好处是：

- 能反映**近期的负载变化**，不会被远古数据拖累；
- 计算极快，O(1)，不需要存历史数据；
- 符合「自适应」的直觉：最近的负载才是决定当前参数的依据。

更新公式（每次操作触发一次）：

```cpp
// alpha 是平滑系数，一般取 0.05 ~ 0.2，值越小越平滑
void update_ema(double &stat, double alpha = 0.1) {
    stat = (1 - alpha) * stat + alpha * 1.0;
}
```

- 每次查询访问到这个block，就调用 `update_ema(query_hits_ema)`
- 每次插入命中这个block，就调用 `update_ema(insert_hits_ema)`
- 每次删除命中这个block，就调用 `update_ema(delete_hits_ema)`

### 1.3 定义全局单位成本常量

在全局配置里定义好单位代价，后面所有计算都复用这一套：

```cpp
// 单位成本常量（可以通过校准微调，默认值按操作耗时比例设定）
const double COST_SCAN_PER_ENTRY = 1.0;      // 扫描一条记录的成本（基准单位）
const double COST_COMPACT_PER_ENTRY = 5.0;   // 压实一条记录的成本（比扫描贵）
const double COST_EXACT_CHECK = 20.0;        // 一次GEOS精确相交校验的成本
const double COST_BLOCK_META = 10.0;         // 一个block的元数据固定成本
```

> 这些常量是相对值，不需要绝对准确，只要比例符合真实耗时就行。后面如果加BO，就是校准这些权重。

### 1.4 阶段验证标准

1. 跑一遍标准查询+插入+删除测试，每个block的统计字段都有合理的数值；
2. 正确性校验：查询结果和原版完全一致，没有假阴性；
3. 性能开销：统计埋点带来的额外耗时 < 1%，几乎无感知。

---

## 阶段2：核心功能 —— per-block 自适应 β / τ

**目标**：把原来全局固定的 `LOCAL_DELTA_BOUND`和 `DELETE_COMPACT_FRACTION`，改成每个block根据自身热度自动计算。
**预计工作量**：3-4天

> 这是整个DELI-Cost性价比最高的一步，改动最小，理论和实验收益最大。

### 2.1 实现 β 的闭式计算

对每个block，根据自身的查询频率和插入频率，自动计算最优β：

$$
\beta_B^* = \sqrt{\frac{B_B}{A_B}}
$$

其中：

- $A_B = 0.5 \times q_B \times c_s$ （查询侧代价系数，0.5是delta平均大小的系数）
- $B_B = i_B \times c_c$ （维护侧代价系数）
- $q_B$ 就是 `query_hits_ema`，$i_B$ 就是 `insert_hits_ema`
- $c_s$ 是单条扫描成本，$c_c$ 是单条压实成本

代码实现：

```cpp
double compute_optimal_beta(const Block &b) {
    double q = b.query_hits_ema;
    double i = b.insert_hits_ema;
  
    // 边界保护：如果没有查询，β可以设大一点，优先插入性能
    if (q < 1e-6) return BETA_MAX;
    // 边界保护：如果没有插入，β设最小，优先查询性能
    if (i < 1e-6) return BETA_MIN;
  
    double A = 0.5 * q * COST_SCAN_PER_ENTRY;
    double B = i * COST_COMPACT_PER_ENTRY;
  
    double beta_opt = sqrt(B / A);
  
    // 裁剪到合理范围，防止极端值
    return clamp(beta_opt, BETA_MIN, BETA_MAX);
}
```

### 2.2 实现 τ 的闭式计算

和β完全对称，用删除频率替换插入频率：

$$
\tau_B^* = \sqrt{\frac{D_B}{C_B}}
$$

- $C_B = 0.5 \times q_B \times c_s$
- $D_B = d_B \times c_c$
- $d_B$ 就是 `delete_hits_ema`

### 2.3 参数更新策略：定时更新，不是每次操作都算

不用每次插入删除都重算β和τ，那样开销没必要。

- 策略：每个block每累计N次操作（比如100次），或者每隔固定时间（比如1秒），重算一次β和τ；
- 好处：计算开销可以忽略，同时参数能跟上负载变化。

### 2.4 替换原来的固定阈值判断

把原来的：

```cpp
if (delta.size() >= LOCAL_DELTA_BOUND) {
    local_compact(block);
}
```

替换成：

```cpp
int delta_threshold = static_cast<int>(block.compact.size() * block.beta);
if (block.delta.size() >= delta_threshold) {
    local_compact(block);
}
```

tombstone的阈值同理替换。

### 2.5 阶段验证标准

1. 正确性：查询结果完全正确，零假阴性；
2. 功能验证：读密集场景下，热点block的β自动变小；写密集场景下，热点block的β自动变大；
3. 性能对比：在混合负载下，自适应版本的综合吞吐（查询+更新）比固定阈值版本高10%-20%。

---

## 阶段3：核心功能 —— Benefit-Cost 驱动的 Compaction

**目标**：从「达到阈值才压实」升级为「收益大于成本就压实」，让冷block少压实省资源，热block早压实提性能。
**预计工作量**：3-4天

### 3.1 实现收益-成本判断公式

对每个block，在每次插入/删除后，判断是否需要主动压实：

$$
\text{Benefit} = \hat{q}_B \times H \times c_s \times |\Delta_B|
$$

$$
\text{Cost} = c_c \times |B|
$$

当 $\text{Benefit} \ge \text{Cost}$ 时，执行压实。

参数说明：

- $\hat{q}_B$：预测未来的查询频率，直接用 `query_hits_ema` 即可；
- $H$：未来收益时间窗口（单位：操作次数/时间），比如设为1000次查询；
- $|\Delta_B|$：当前delta的大小（或tombstone的数量）；
- $|B|$：block的总大小（compact + delta）。

### 3.2 双重触发机制：阈值兜底 + 收益主动

不要完全扔掉阈值判断，采用**双重触发**，更稳定：

```cpp
bool should_compact_delta(const Block &b) {
    int threshold = static_cast<int>(b.compact.size() * b.beta);
  
    // 条件1：达到自适应阈值，兜底触发
    if (b.delta.size() >= threshold) return true;
  
    // 条件2：收益大于成本，主动触发（热block提前整理）
    double benefit = b.query_hits_ema * H_WINDOW * COST_SCAN_PER_ENTRY * b.delta.size();
    double cost = COST_COMPACT_PER_ENTRY * (b.compact.size() + b.delta.size());
    if (benefit >= cost) return true;
  
    return false;
}
```

### 3.3 加冷却时间，防止抖动

防止刚压实完，插入几条数据又触发压实，加一个最小压实间隔：

```cpp
// 两次压实之间至少间隔 MIN_COMPACT_INTERVAL 次操作/微秒
if (current_ts - b.last_compact_ts < MIN_COMPACT_INTERVAL) {
    return false;
}
```

### 3.4 阶段验证标准

1. 正确性：结果正确，无数据丢失；
2. 行为验证：热block的compaction更频繁，delta更小；冷block的compaction很少，delta积累到阈值才整理；
3. 性能对比：和纯阈值版本比，相同查询延迟下，更新吞吐更高；或者相同更新吞吐下，查询延迟更低。

---

## 阶段4：进阶功能 —— 离线DP自适应块划分（仅bulk-load）

**目标**：批量构建索引时，用动态规划自动划分block，替代固定大小切分。
**预计工作量**：1周

> 只改批量构建逻辑，在线查询更新完全不动，风险低，理论加分多。

### 4.1 第一步：预处理前缀和数组

所有extent entry按Zmin排好序后，先算好前缀和数组，O(1)获取任意区间的统计值：

```cpp
// 前缀和数组，prefix_sum[i] 表示前i条记录的累计统计值
struct PrefixStats {
    uint64_t count;          // 记录数
    uint64_t max_zmax;       // 区间内最大zmax
    double total_zspan;      // z区间跨度总和
    double avg_mbr_area;     // 平均MBR面积
    double estimated_query_cost;  // 预估查询代价（用校准数据预计算）
};

vector<PrefixStats> prefix;
```

### 4.2 第二步：实现单块代价函数

输入区间 `[i, j]`，输出这个区间作为一个block的总代价：

```cpp
double compute_segment_cost(int i, int j) {
    auto stats = get_range_stats(i, j); // 用前缀和O(1)计算
  
    // 代价 = 查询代价 + 维护代价 + 空间元数据代价
    double cost_query = stats.estimated_query_cost;
    double cost_maintain = stats.count * COST_COMPACT_PER_ENTRY * 0.01; // 平摊维护成本
    double cost_space = COST_BLOCK_META; // 每个块的固定元数据开销
  
    return cost_query + cost_maintain + cost_space;
}
```

> 查询代价的预估，可以用校准查询集提前算好每个位置的命中权重；如果没有校准数据，就用 `max_zmax - min_zmin`近似，跨度越大，剪枝效率越低，查询代价越高。

### 4.3 第三步：实现DP递推

标准一维分段DP：

```cpp
vector<double> dp(n+1, INF);
vector<int> prev(n+1, -1); // 记录划分点，用于回溯
dp[0] = 0;

for (int j = 1; j <= n; j++) {
    // 枚举块的左边界i，块大小在 [b_min, b_max] 之间
    for (int i = max(1, j - b_max + 1); i <= j - b_min + 1; i++) {
        double cost = dp[i-1] + compute_segment_cost(i, j);
        if (cost < dp[j]) {
            dp[j] = cost;
            prev[j] = i-1;
        }
    }
}
```

### 4.4 第四步：回溯得到划分点

```cpp
vector<int> split_points;
int cur = n;
while (cur > 0) {
    split_points.push_back(cur);
    cur = prev[cur];
}
reverse(split_points.begin(), split_points.end());
```

`split_points`里就是每个block的结束位置，按这个划分构建block即可。

### 4.5 阶段验证标准

1. 正确性：划分后的block都是连续的，大小都在[b_min, b_max]范围内；
2. 理论验证：DP计算的总代价比固定块划分低；
3. 实验对比：真实数据集上，DP划分的查询延迟比固定块大小低10%-20%。

---

## 阶段5：可选进阶 —— 在线局部分裂与合并

**目标**：在线更新过程中，自动调整块大小，适配数据分布变化。
**预计工作量**：1-2周

> 时间不够可以不做，写进论文未来工作里即可，不影响核心贡献。

### 5.1 分裂触发

当一个block大小超过 `b_max`时，计算分裂成两个子块的总代价，如果分裂后代价降低超过惩罚项，就分裂：

```cpp
bool should_split(const Block &b) {
    if (b.compact.size() + b.delta.size() < b_max) return false;
  
    double cost_old = compute_block_cost(b);
    double cost_split = compute_split_cost(b); // 找最优分裂点，算两个子块的代价和
  
    return cost_split + SPLIT_PENALTY < cost_old;
}
```

### 5.2 合并触发

当两个相邻block都很小，且合并后代价降低超过惩罚项，就合并：

```cpp
bool should_merge(const Block &left, const Block &right) {
    if (left.size() + right.size() > b_max) return false;
  
    double cost_separate = compute_block_cost(left) + compute_block_cost(right);
    double cost_merged = compute_merge_cost(left, right);
  
    return cost_merged + MERGE_PENALTY < cost_separate;
}
```

### 5.3 注意事项

- 分裂合并都要同步更新学习索引模型（ALEX的分段函数）；
- 必须加惩罚项，防止数据小幅波动导致反复分裂合并（抖动）；
- 合并只考虑相邻block，保证Zmin有序性不被破坏。

---

# 整体时间规划（按天计，共4周）

| 周数  | 工作内容                                          | 产出                                        |
| ----- | ------------------------------------------------- | ------------------------------------------- |
| 第1周 | 阶段1：统计埋点 + 阶段2：自适应β/τ              | 可运行的per-block自适应版本，正确性验证通过 |
| 第2周 | 阶段3：Benefit-Cost compaction + 基础混合负载测试 | DELI-Cost-Lite完整版本，初步性能对比数据    |
| 第3周 | 阶段4：离线DP自适应块划分 + 完整实验跑通          | 全功能DELI-Cost，固定vs自适应的对比实验结果 |
| 第4周 | 实验补全 + 数据整理 + 论文图绘制                  | 完整的实验结果，可写入论文的方法章节        |

---

# 几个关键的工程避坑提醒

1. **永远保留正确性校验**：每改完一个阶段，都跑一遍答案比对，确保查询结果和Boost R-tree完全一致。自适应只改性能，不改正确性，这是底线。
2. **统计数据要冷启动保护**：刚创建的block没有历史统计，β和τ先用默认值，等累计足够操作后再切换到自适应计算。
3. **边界条件全覆盖**：空block、全是墓碑的block、超热block、超冷block，这些极端场景都要单独测试，防止除零、溢出、死循环。
4. **代码解耦**：把代价计算、自适应参数计算做成单独的工具类，不要和block的主逻辑强耦合，后面加BO的时候直接复用。

按照这个路线走，你会非常稳：每一步都有明确的产出，每一步都能验证，前3周就能拿出核心的实验结果和完整的方法故事，完全支撑得起VLDB级别的论文主体。
