#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  cat <<'USAGE'
用法：
  ./scripts/run_dynamic_compare_diagnostics.sh

这个脚本用于跑统一动态对比：
  DELI-Dynamic-Single
  DELI-ALEX
  DELI-ALEX-Hybrid
  DELI-ALEX-Hybrid-Buf
  DELI-ALEX-Hybrid-Bounded
  DELI-ALEX-Hybrid-LocalBounded
  DELI-ALEX-Hybrid-Cost
  Boost R-tree
  GEOS Quadtree
  GLIN-piece

所有方法使用同一套 workload：
  bulk-load 50% -> insert 20% -> query -> delete 10% -> query

也可以跑单线程 mixed workload：
  bulk-load 50%
  然后按同一条操作序列 interleave query/insert/delete。
  典型 profile：
    read_heavy:  90% query + 5% insert + 5% delete
    balanced:    70% query + 15% insert + 15% delete
    write_heavy: 50% query + 25% insert + 25% delete

默认 DELI 参数固定为：
  block_size=512
  stale_threshold_fraction=0.05

常用 smoke 命令：
  RESET_RESULTS=1 OVERWRITE=1 \
  DATASETS=ZGAP_MIXED \
  LIMIT=1234 QUERY_LIMIT=1234 \
  QUERY_ROOT=queries/interval_overlap_mixed_smoke_1234 \
  RESULT_DIR=results/dynamic_compare_smoke_1234 \
  FIGURE_DIR=figures/dynamic_compare_smoke_1234 \
  SELECTIVITY_TAGS=0p01pct QUERY_COUNT=20 \
    ./scripts/run_dynamic_compare_diagnostics.sh

正式 ZGAP_MIXED 命令：
  RESET_RESULTS=1 OVERWRITE=1 \
  DATASETS=ZGAP_MIXED \
  LIMIT=1000000 QUERY_LIMIT=1000000 \
  QUERY_ROOT=queries/interval_overlap_mixed_1000000 \
  RESULT_DIR=results/dynamic_compare_mixed_1000000 \
  FIGURE_DIR=figures/dynamic_compare_mixed_1000000 \
  SELECTIVITY_TAGS="0p001pct 0p01pct 0p1pct 1pct" \
    ./scripts/run_dynamic_compare_diagnostics.sh

常用参数：
  DATASETS
    默认 ZGAP_MIXED。可选：AW LW ROADS PARKS UNIF_S DIAG_S ZGAP_WIDE ZGAP_MIXED。

  LIMIT
    每个数据集最多加载多少条 geometry。默认 1000000。

  SELECTIVITY_TAGS
    query 选择性。默认 1pct。

  BLOCK_SIZE
    DELI block size。默认 512。

  STALE_THRESHOLD
    DELI stale_threshold_fraction。默认 0.05。

  LOCAL_DELTA_BOUND
    DELI-ALEX-Hybrid-LocalBounded 的每个 block 局部 delta 上限。
    默认 0，表示自动使用 max(64, BLOCK_SIZE/4)，把每个 block 的额外扫描量控制在约 25%。

  DELETE_COMPACT_FRACTION
    DELI-ALEX-Hybrid-LocalBounded 的删除物理压缩阈值。
    默认 0.25，表示每个 block 最多容忍约 25% tombstone 后才做 physical compaction。

  COST_EMA_ALPHA / COST_BETA_MIN / COST_BETA_MAX / COST_TAU_MIN / COST_TAU_MAX
    DELI-ALEX-Hybrid-Cost 的自适应参数。
    beta 是局部增量比例，tau 是墓碑比例。
    默认 beta/tau 都限制在 0.25 到 0.50 之间，避免 Cost 版在当前数据上过度压实。
    如果想测试激进读优化，可以显式设置 COST_BETA_MIN=0.05 COST_TAU_MIN=0.05。

  COST_SCAN_PER_ENTRY / COST_COMPACT_PER_ENTRY / COST_COMPACTION_HORIZON
    DELI-ALEX-Hybrid-Cost 的代价模型相对权重。
    scan_per_entry 表示扫描一条记录的成本，compact_per_entry 表示整理一条记录的成本。
    compaction_horizon 表示估计未来收益时看的操作窗口。
    默认 0，表示不做提前 compaction；Cost 版只根据负载比例放宽局部阈值。

  COST_ADAPTIVE_PARTITION
    是否为 DELI-ALEX-Hybrid-Cost 启用 DP 自适应 block 划分。默认 1。
    这个开关只影响 Cost 版，不影响 LocalBounded 固定分区基线。

  COST_PARTITION_MIN_BLOCK_SIZE / COST_PARTITION_MAX_BLOCK_SIZE
    DP 划分时每个 block 的最小/最大记录数。
    默认 0 表示自动使用 BLOCK_SIZE/2 和 2*BLOCK_SIZE。

  COST_PARTITION_STEP
    DP 候选切分点步长。默认 0 表示自动使用 BLOCK_SIZE/8。
    步长越小，划分越细，但 build 时间越长。

  COST_PARTITION_QUERY_SAMPLE
    用多少条 query 样本估计 block 的查询代价。默认 128。

  PREDICATE_SHORTCUTS
    是否启用 DELI predicate-aware shortcut。默认 1。
    当前实现只使用严格安全的矩形查询 shortcut：
    如果 query 矩形的 envelope 完全包含对象 envelope，则该对象必然与 query 矩形相交，
    可以直接返回答案并跳过 GEOS exact intersects。
    设为 0 可以复现实验中的 DELI v1，不使用该 predicate-aware 层。

  INDEXES
    只跑指定索引，避免每次都把所有方法重建一遍。默认 all。
    例子：
      INDEXES="DELI_ALEX_HYBRID_LOCAL_BOUNDED Boost_Rtree"
    可用名字：
      DELI_DYNAMIC_SINGLE 
      DELI_ALEX 
      DELI_ALEX_HYBRID
      DELI_ALEX_HYBRID_BUF 
      DELI_ALEX_HYBRID_BOUNDED
      DELI_ALEX_HYBRID_LOCAL_BOUNDED 
      DELI_ALEX_HYBRID_COST
      Boost_Rtree
      GEOS_Quadtree 
      GLIN_PIECEWISE

  CHECK_CORRECTNESS
    是否在 checkpoint 上用 Boost R-tree oracle 检查答案正确性。默认 1。
    设为 0 时跳过 oracle，answers_match_boost 会写成 -1，适合先快速看性能趋势。

  CORRECTNESS_EVERY_N
    每隔多少个 checkpoint 做一次正确性检查。默认 1，表示每次都查。
    例子：CORRECTNESS_EVERY_N=5 表示只检查第 5、10、15... 个 checkpoint。
    设为 0 等价于 CHECK_CORRECTNESS=0。

  INITIAL_FRACTION / INSERT_FRACTION / DELETE_FRACTION
    默认 0.5 / 0.2 / 0.1。

  WORKLOAD_MODE  混合负载模式，区别于GLIN的stage负载模式，混合负载更能体现真实场景
    staged：默认，bulk-load -> insert -> query -> delete -> query。
    mixed：单线程交错 mixed workload。 查询和插入删除是依次交替执行的，也就是说，系统会先生成一条操作序列，例如：
    query
    query
    insert
    query
    delete
    query
    query
    insert
    ...
    然后所有方法都按照这条完全相同的序列一个操作一个操作地执行，所以它不是：
    一个线程一直query
    一个线程一直insrt
    一个线程一直delete
    而是：
    同一个线程里query/insert/delete交错出现
    这样做的好处是公平／容易复现，也不会引入多线程锁，一致性／后台维护这些复杂问题

    那么问题来了，混合负载是如何保证不同的索引，在查询和插入删除时的对象是相同的？并保证结果可重复？
      因为 mixed workload 里有随机过程，例如：
      插入对象的顺序
      删除对象的选择
      query/insert/delete 操作序列
      这些随机过程不是用真正不可控的随机数，而是用伪随机数生成器：
      std::mt19937_64 rng(options.seed);
      只要：
      seed 一样
      数据集一样
      QUERY_COUNT 一样
      MIXED_OPERATIONS 一样
      比例一样
      代码一样
    查询对象是一样是因为query文件是一致的，但是我不理解它是怎么保证不同方法的插入和删除的对象是一样的呢？
      生成mixed workload 混合查询负载，这个会生成一个固定的操作序列，如：

      1: query query[0]
      2: query query[1]
      3: insert object 812345
      4: query query[2]
      5: delete object 12345
      6: query query[3]
      ...
      100000: delete object 456789


      所有方法会复用这个操作序列，所以能保证
      所以不同方法的：
      第几步是 query
      第几步是 insert
      插入哪个 object_id
      第几步是 delete
      删除哪个 object_id
      都是一样的。
      为什么删除对象也能一样？因为生成操作序列时，代码内部维护了一个“模拟 live set”。
      简单说：
      初始 live set = bulk-load 的 50% 对象
      insert 时：从未插入对象池里取一个 object_id，并加入 live set
      delete 时：从当前 live set 里选一个 object_id，并从 live set 删除
      这个过程只发生在生成 workload 时。生成完后，delete 的 object_id 已经固定了。后面每个方法只是照着执行：
      delete object 12345
      而不是自己再随机选要删谁。
      所以公平性来自两层：
      1. seed 固定 -> 生成出来的 operations 固定
      2. operations 先生成好 -> 所有方法复用同一条操作序列


  MIXED_PROFILES    profile可以理解成“负载模板或者是读写场景配置”
    WORKLOAD_MODE=mixed 时一次跑哪些 profile。
    默认：read_heavy balanced write_heavy。

  BATCH_MIXED_PROFILES
    默认 1。mixed 模式下把 MIXED_PROFILES 合并到同一个 C++ 进程里跑，
    这样同一个 dataset + selectivity 只加载一次 WKT，能明显减少重复加载成本。
    设为 0 时恢复旧行为：每个 profile 单独启动一次 benchmark。

  MIXED_OPERATIONS   总操作次数
    每个 mixed profile 的交错操作总数即查询，插入和删除次数的总和。默认 100000。
    总操作数是10万，它会按设置好的读写场景配置来分配次数，比如read_heavy：90%query，5%insert,5%delete,那么就是9万次query,5000次插入，5000次删除
    


  MIXED_CHECKPOINT_INTERVAL
    每隔多少个 mixed 操作输出 checkpoint。默认 10000。  checkpoint就是“中途拍一次快照”，mixed workload是一边查询，一边插入，一边删除，如果只看最后的结果，就不会知道索引是不是中途越来越慢。
    所以每隔一段操作，停下来记录一次当前状态，比如：
    当前已经执行了多少操作
    这一段 query 平均延迟是多少
    P95/P99延迟是多少
    insert/delete 吞吐量是多少
    答案是否仍然正确
    tombstone 有没有堆积

    如果MIXED_OPERATIONS=100000，MIXED_CHECKPOINT_INTERVAL=10000,查询插入及删除的总操作数是10万，但是每执行一万次操作就会执行一次checkpoint快照，会输出10个checkpoint:
    mixed_10000,mixed_20000,mixed_30000...,这个快照的频率越高，实验运行时间肯定越长

  AUTO_GENERATE_QUERIES
    0：缺 query 文件就报错。
    1：缺 query 文件时调用 JTS STRtree KNN query generator 自动生成。
    默认：0。

  QUERY_COUNT 是“查询样本池大小”
    因为mixed workload里有很多query操作，但是我们不想每次query都现场随机生成一个查询窗口，这样不稳定，也不方便和其它实验对齐所以脚本会先从query文件里读取固定数量的query，例如
    QUERY_COUNT=200
    表示读取200条query,形成一个query pool:
    query[0]
    query[1]
    ...
    query[199]
    mixed workload运行时，每遇到一个query操作，就从这个pool里，取一个query用。
    如果query操作超过200次，就循环使用：
    第1次query用query[0]
    第200次query用query[199]
    第201次query又用query[0]
    为什么要这样做？
        1.保证公平：所有方法查询的是同一批query
        2.保证复现：同一seed,同一query文件，结果可重复
        3.控制实验规模，如果QUERY_COUNT太大，correctness检查会很慢
        4.稳定统计：如果QUERY_COUNT太小，P95/P99容易被少数query偶然影响，因为不具有代表性，有的范围大，有的范围小


  QUERY_COUNT的取值怎么确定呢？它和数据集的大小，查询选择性，工作负载有关系吗？其实是有的，但是没有一个固定的关系，只能说当查询选择性较低时，或者数据集的量比较小，QUERY_COUNT要取大一些比如500，1000

  
  query文件是用脚本生成的scripts/generate_jts_strtree_knn_queries.sh，它的核心思想是用JTS STRTree在数据集上找邻近对象，然后生成能达到目标selectivity的查询窗口，比如
    0p1pct
    1pct
  分别表示希望query的答案规模大约对应数据集一定的比例。这样比随机生成随机框更稳定，因为不同数据集大小和空间分布不同。

orrectness oracle 可以理解成“标准答案生成器”。
  这里我们用 Boost R-tree + GEOS exact intersects 当 oracle。意思是：
  1. 用 Boost R-tree 找候选对象；
  2. 再用 GEOS 精确判断 intersects；
  3. 得到一组标准答案；
  4. 拿 DELI / GLIN-piece / Quadtree 的答案和它对比。
  如果结果一致：
  answers_match_boost = 1
  如果不一致：
  answers_match_boost = 0
  如果你设置：
  CHECK_CORRECTNESS=0
  那就不做这个标准答案检查，所以：
  answers_match_boost = -1
  这不是错，只是表示“这次为了省时间，没有检查正确性”
  
输出：
  RESULT_DIR/dynamic_compare_summary.csv
  FIGURE_DIR/dynamic_compare_*.png
USAGE
  exit 0
fi

LIMIT="${LIMIT:-1000000}"
QUERY_LIMIT="${QUERY_LIMIT:-$LIMIT}"
DATASETS="${DATASETS:-ZGAP_MIXED}"
DATA_ROOT="${DATA_ROOT:-/mnt/hgfs}"
ZGAP_WORK_DIR="${ZGAP_WORK_DIR:-data/synthetic/zrange_gap}"
ZGAP_MIXED_WORK_DIR="${ZGAP_MIXED_WORK_DIR:-data/synthetic/zrange_gap_mixed_${LIMIT}}"
QUERY_ROOT="${QUERY_ROOT:-queries/dynamic_compare_${QUERY_LIMIT}}"
RESULT_DIR="${RESULT_DIR:-results/dynamic_compare_${LIMIT}}"
FIGURE_DIR="${FIGURE_DIR:-figures/dynamic_compare_${LIMIT}}"
SELECTIVITY_TAGS="${SELECTIVITY_TAGS:-1pct}"
BUILD_DIR="${BUILD_DIR:-build}"

# 这里故意固定为论文默认参数，避免每个数据集单独调参。
BLOCK_SIZE="${BLOCK_SIZE:-512}"   #只有DELI_ALEX_HYBRID_COST这个方法在bulk-load阶段用动态规划进行block分区,其它方法还是默认每个block的容量是512
STALE_THRESHOLD="${STALE_THRESHOLD:-0.05}"
LOCAL_DELTA_BOUND="${LOCAL_DELTA_BOUND:-0}"
if [[ -z "${DELETE_COMPACT_FRACTION+x}" && -n "${DELETE_COMPAPACT_FRACTION:-}" ]]; then
  echo "Warning: DELETE_COMPAPACT_FRACTION 拼写有误，已按 DELETE_COMPACT_FRACTION 处理。" >&2
  DELETE_COMPACT_FRACTION="$DELETE_COMPAPACT_FRACTION"
fi
DELETE_COMPACT_FRACTION="${DELETE_COMPACT_FRACTION:-0.25}"
COST_EMA_ALPHA="${COST_EMA_ALPHA:-0.10}"
COST_BETA_MIN="${COST_BETA_MIN:-0.25}"
COST_BETA_MAX="${COST_BETA_MAX:-0.50}"
COST_TAU_MIN="${COST_TAU_MIN:-0.25}"
COST_TAU_MAX="${COST_TAU_MAX:-0.50}"
COST_SCAN_PER_ENTRY="${COST_SCAN_PER_ENTRY:-1.0}"
COST_COMPACT_PER_ENTRY="${COST_COMPACT_PER_ENTRY:-5.0}"
COST_COMPACTION_HORIZON="${COST_COMPACTION_HORIZON:-0}"
COST_MIN_COMPACT_INTERVAL="${COST_MIN_COMPACT_INTERVAL:-64}"
COST_ADAPTIVE_PARTITION="${COST_ADAPTIVE_PARTITION:-1}"
COST_PARTITION_MIN_BLOCK_SIZE="${COST_PARTITION_MIN_BLOCK_SIZE:-0}"
COST_PARTITION_MAX_BLOCK_SIZE="${COST_PARTITION_MAX_BLOCK_SIZE:-0}"
COST_PARTITION_STEP="${COST_PARTITION_STEP:-0}"
COST_PARTITION_QUERY_SAMPLE="${COST_PARTITION_QUERY_SAMPLE:-128}"
PREDICATE_SHORTCUTS="${PREDICATE_SHORTCUTS:-1}"
PIECE_LIMIT="${PIECE_LIMIT:-10000}"

INITIAL_FRACTION="${INITIAL_FRACTION:-0.5}"
INSERT_FRACTION="${INSERT_FRACTION:-0.2}"
DELETE_FRACTION="${DELETE_FRACTION:-0.1}"
QUERY_COUNT="${QUERY_COUNT:-100}"  # query_count 不是操作总数，而是“查询样本池大小”。
CELL_SIZE="${CELL_SIZE:-0.0000005}"
SEED="${SEED:-42}"
INDEXES="${INDEXES:-all}"
CHECK_CORRECTNESS="${CHECK_CORRECTNESS:-1}"
CORRECTNESS_EVERY_N="${CORRECTNESS_EVERY_N:-1}"
WORKLOAD_MODE="${WORKLOAD_MODE:-staged}"
MIXED_PROFILES="${MIXED_PROFILES:-read_heavy balanced write_heavy}"
BATCH_MIXED_PROFILES="${BATCH_MIXED_PROFILES:-1}"
MIXED_OPERATIONS="${MIXED_OPERATIONS:-100000}"
MIXED_CHECKPOINT_INTERVAL="${MIXED_CHECKPOINT_INTERVAL:-10000}"

AUTO_BUILD="${AUTO_BUILD:-1}"
RUN_BENCHMARKS="${RUN_BENCHMARKS:-1}"
RESET_RESULTS="${RESET_RESULTS:-1}"
OVERWRITE="${OVERWRITE:-0}"
PLOT_RESULTS="${PLOT_RESULTS:-1}"
EXCLUDE_DATASETS="${EXCLUDE_DATASETS:-}"
AUTO_GENERATE_QUERIES="${AUTO_GENERATE_QUERIES:-0}"
REGENERATE_QUERIES="${REGENERATE_QUERIES:-0}"

if [[ "$RUN_BENCHMARKS" == "0" && "$RESET_RESULTS" == "1" ]]; then
  echo "Error: RUN_BENCHMARKS=0 时不能使用 RESET_RESULTS=1，否则会删掉已有结果。" >&2
  exit 1
fi

mkdir -p "$RESULT_DIR"
if [[ "$RESET_RESULTS" == "1" ]]; then
  rm -f "$RESULT_DIR"/*.csv
fi

if [[ "$AUTO_BUILD" == "1" && "$RUN_BENCHMARKS" == "1" ]]; then
  cmake --build "$BUILD_DIR" --target bench_dynamic_compare_wkt -j2
fi

declare -A DATA_FILES=(
  [AW]="$DATA_ROOT/AREAWATER.csv"
  [LW]="$DATA_ROOT/LINEARWATER.csv"
  [ROADS]="$DATA_ROOT/roads"
  [PARKS]="$DATA_ROOT/parks"
  [UNIF_S]="data/synthetic/rectangles/UNIF_S.wkt"
  [DIAG_S]="data/synthetic/rectangles/DIAG_S.wkt"
  [ZGAP_WIDE]="$ZGAP_WORK_DIR/ZGAP_WIDE.wkt"
  [ZGAP_MIXED]="$ZGAP_MIXED_WORK_DIR/ZGAP_MIXED.wkt"
)

data_file_for_dataset() {
  local dataset="$1"
  local override_var="DATA_FILE_${dataset}"
  local override_value="${!override_var:-}"
  if [[ -n "$override_value" ]]; then
    echo "$override_value"
    return
  fi
  if [[ -z "${DATA_FILES[$dataset]:-}" ]]; then
    echo "Error: unknown dataset '$dataset'." >&2
    exit 1
  fi
  echo "${DATA_FILES[$dataset]}"
}

query_file_for_dataset() {
  local dataset="$1"
  local tag="$2"
  echo "$QUERY_ROOT/${dataset}_jts_strtree_knn_${tag}.csv"
}

selectivity_for_tag() {
  local tag="$1"
  if [[ "$tag" == *pct ]]; then
    local percent="${tag%pct}"
    percent="${percent//p/.}"
    echo "${percent}%"
    return
  fi
  echo "$tag"
}

query_selectivities_for_tags() {
  local result=""
  local tag
  for tag in $SELECTIVITY_TAGS; do
    if [[ -n "$result" ]]; then
      result+=","
    fi
    result+="$(selectivity_for_tag "$tag")"
  done
  echo "$result"
}

mixed_profile_ratios() {
  local profile="$1"
  case "$profile" in
    read_heavy)
      echo "0.90 0.05 0.05"
      ;;
    balanced)
      echo "0.70 0.15 0.15"
      ;;
    write_heavy)
      echo "0.50 0.25 0.25"
      ;;
    custom)
      echo "${MIXED_QUERY_RATIO:-0.90} ${MIXED_INSERT_RATIO:-0.05} ${MIXED_DELETE_RATIO:-0.05}"
      ;;
    *)
      echo "Error: unknown MIXED_PROFILES item '$profile'. Use read_heavy balanced write_heavy custom." >&2
      exit 1
      ;;
  esac
}

mixed_profile_specs() {
  local specs=""
  local profile
  for profile in $MIXED_PROFILES; do
    local q i d
    read -r q i d <<<"$(mixed_profile_ratios "$profile")"
    if [[ -n "$specs" ]]; then
      specs+=","
    fi
    specs+="${profile}:${q}:${i}:${d}"
  done
  echo "$specs"
}

should_run_file() {
  local path="$1"
  [[ "$OVERWRITE" == "1" || ! -s "$path" ]]
}

generate_queries_if_needed() {
  local dataset="$1"
  local data_file="$2"
  local needs_generation="$REGENERATE_QUERIES"
  local reason=""

  if [[ "$REGENERATE_QUERIES" == "1" ]]; then
    reason="REGENERATE_QUERIES=1"
  fi

  for tag in $SELECTIVITY_TAGS; do
    local query_file
    query_file="$(query_file_for_dataset "$dataset" "$tag")"
    if [[ ! -s "$query_file" ]]; then
      needs_generation=1
      reason="missing query file: $query_file"
    fi
  done

  if [[ "$needs_generation" != "1" ]]; then
    return
  fi

  if [[ "$AUTO_GENERATE_QUERIES" != "1" && "$REGENERATE_QUERIES" != "1" ]]; then
    echo "Error: query file for $dataset under $QUERY_ROOT needs generation." >&2
    echo "Reason: $reason" >&2
    echo "请先用 run_interval_overlap_diagnostics.sh 生成 query，设置正确 QUERY_ROOT，或加 AUTO_GENERATE_QUERIES=1。" >&2
    exit 1
  fi

  if [[ -n "$reason" ]]; then
    echo "Generating queries for $dataset: $reason"
  fi
  mkdir -p "$QUERY_ROOT"
  QUERY_SELECTIVITIES="$(query_selectivities_for_tags)" \
    scripts/generate_jts_strtree_knn_queries.sh \
    "$(realpath "$data_file")" \
    "$QUERY_ROOT/${dataset}_jts_strtree_knn" \
    "$QUERY_LIMIT" \
    "$QUERY_COUNT" \
    "$SEED"
}

if [[ "$RUN_BENCHMARKS" == "1" ]]; then
  for dataset in $DATASETS; do
    data_file="$(data_file_for_dataset "$dataset")"
    if [[ ! -e "$data_file" ]]; then
      echo "Error: data file not found: $data_file" >&2
      exit 1
    fi
    generate_queries_if_needed "$dataset" "$data_file"

    for tag in $SELECTIVITY_TAGS; do
      query_file="$(query_file_for_dataset "$dataset" "$tag")"
      if [[ ! -e "$query_file" ]]; then
        echo "Error: query file not found: $query_file" >&2
        echo "请先用 run_interval_overlap_diagnostics.sh 生成 query，或设置正确 QUERY_ROOT。" >&2
        exit 1
      fi

      profiles="staged"
      if [[ "$WORKLOAD_MODE" == "mixed" ]]; then
        if [[ "$BATCH_MIXED_PROFILES" == "1" ]]; then
          profiles="__batched__"
        else
          profiles="$MIXED_PROFILES"
        fi
      fi

      for profile in $profiles; do
        mixed_query_ratio="0"
        mixed_insert_ratio="0"
        mixed_delete_ratio="0"
        mixed_profile_specs_arg=""
        raw_suffix="dynamic_compare"
        if [[ "$WORKLOAD_MODE" == "mixed" ]]; then
          if [[ "$profile" == "__batched__" ]]; then
            mixed_profile_specs_arg="$(mixed_profile_specs)"
            mixed_query_ratio="1"
            mixed_insert_ratio="0"
            mixed_delete_ratio="0"
            profile="batched"
            raw_suffix="mixed_profiles_dynamic_compare"
          else
            read -r mixed_query_ratio mixed_insert_ratio mixed_delete_ratio <<<"$(mixed_profile_ratios "$profile")"
            raw_suffix="${profile}_dynamic_compare"
          fi
        fi

        raw_csv="$RESULT_DIR/${dataset}_${tag}_${raw_suffix}.csv"
        if should_run_file "$raw_csv"; then
          echo "Running dynamic compare dataset=$dataset selectivity=$tag workload=$WORKLOAD_MODE profile=$profile"
          "$BUILD_DIR/bench_dynamic_compare_wkt" \
          --data_file "$data_file" \
          --query_file "$query_file" \
          --dataset_name "$dataset" \
          --limit "$LIMIT" \
          --query_count "$QUERY_COUNT" \
          --workload_mode "$WORKLOAD_MODE" \
          --mixed_profile "$profile" \
          --mixed_profile_specs "$mixed_profile_specs_arg" \
          --mixed_operations "$MIXED_OPERATIONS" \
          --mixed_checkpoint_interval "$MIXED_CHECKPOINT_INTERVAL" \
          --mixed_query_ratio "$mixed_query_ratio" \
          --mixed_insert_ratio "$mixed_insert_ratio" \
          --mixed_delete_ratio "$mixed_delete_ratio" \
          --initial_fraction "$INITIAL_FRACTION" \
          --insert_fraction "$INSERT_FRACTION" \
          --delete_fraction "$DELETE_FRACTION" \
          --indexes "$INDEXES" \
          --check_correctness "$CHECK_CORRECTNESS" \
          --correctness_every_n "$CORRECTNESS_EVERY_N" \
          --block_size "$BLOCK_SIZE" \
          --stale_threshold_fraction "$STALE_THRESHOLD" \
          --local_delta_bound "$LOCAL_DELTA_BOUND" \
          --delete_compact_fraction "$DELETE_COMPACT_FRACTION" \
          --cost_ema_alpha "$COST_EMA_ALPHA" \
          --cost_beta_min "$COST_BETA_MIN" \
          --cost_beta_max "$COST_BETA_MAX" \
          --cost_tau_min "$COST_TAU_MIN" \
          --cost_tau_max "$COST_TAU_MAX" \
          --cost_scan_per_entry "$COST_SCAN_PER_ENTRY" \
          --cost_compact_per_entry "$COST_COMPACT_PER_ENTRY" \
          --cost_compaction_horizon "$COST_COMPACTION_HORIZON" \
          --cost_min_compact_interval "$COST_MIN_COMPACT_INTERVAL" \
          --cost_adaptive_partition "$COST_ADAPTIVE_PARTITION" \
          --cost_partition_min_block_size "$COST_PARTITION_MIN_BLOCK_SIZE" \
          --cost_partition_max_block_size "$COST_PARTITION_MAX_BLOCK_SIZE" \
          --cost_partition_step "$COST_PARTITION_STEP" \
          --cost_partition_query_sample "$COST_PARTITION_QUERY_SAMPLE" \
          --predicate_shortcuts "$PREDICATE_SHORTCUTS" \
          --piece_limit "$PIECE_LIMIT" \
          --cell_size "$CELL_SIZE" \
          --seed "$SEED" \
          --stop_on_mismatch 0 \
          --output_csv "$raw_csv"
        else
          echo "Skip existing raw CSV: $raw_csv"
        fi
      done
    done
  done
else
  echo "RUN_BENCHMARKS=0：跳过 benchmark，只汇总和画图。"
fi

python3 scripts/summarize_dynamic_compare_diagnostics.py \
  --result_dir "$RESULT_DIR" \
  --output_csv "$RESULT_DIR/dynamic_compare_summary.csv" \
  --exclude_datasets "$EXCLUDE_DATASETS"

if [[ "$PLOT_RESULTS" == "1" ]]; then
  python3 scripts/plot_dynamic_compare_diagnostics.py \
    --input "$RESULT_DIR/dynamic_compare_summary.csv" \
    --output_dir "$FIGURE_DIR" \
    --figure_prefix dynamic_compare \
    --exclude_datasets "$EXCLUDE_DATASETS"
fi

echo "Result dir: $RESULT_DIR"
echo "Summary:    $RESULT_DIR/dynamic_compare_summary.csv"
if [[ "$PLOT_RESULTS" == "1" ]]; then
  echo "Figures:    $FIGURE_DIR"
fi
