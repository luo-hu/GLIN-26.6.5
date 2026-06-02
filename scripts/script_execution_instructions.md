完成了：我已经新增脚本、跑完 **PARKS 1M + JTS STRtree KNN + GLIN-piecewise Intersects + PL sweep**，并生成了汇总表和图。
新增脚本：

- [run_parks1m_jts_piece_limit_sweep.sh](/home/lh/Code/2026.5.28/GLIN/scripts/run_parks1m_jts_piece_limit_sweep.sh:1)：运行 `PL=100/1000/10000/100000`
- [summarize_piece_limit_sweep.py](/home/lh/Code/2026.5.28/GLIN/scripts/summarize_piece_limit_sweep.py:1)：汇总 16 个 raw CSV
- [plot_piece_limit_sweep.py](/home/lh/Code/2026.5.28/GLIN/scripts/plot_piece_limit_sweep.py:1)：画 probe、total、candidate、candidate ratio、build time 图

输出结果：

- 汇总表：[parks_1m_jts_strtree_knn_piece_limit_summary.csv](/home/lh/Code/2026.5.28/GLIN/results/parks_1m_jts_strtree_knn_piece_limit_summary.csv:1)
- 关键图：
  - [query_time](/home/lh/Code/2026.5.28/GLIN/figures/parks_1m_jts_strtree_knn_piece_limit_query_time.png)
  - [probe_time](/home/lh/Code/2026.5.28/GLIN/figures/parks_1m_jts_strtree_knn_piece_limit_probe_time.png)
  - [candidates](/home/lh/Code/2026.5.28/GLIN/figures/parks_1m_jts_strtree_knn_piece_limit_candidates.png)
  - [candidate_ratio](/home/lh/Code/2026.5.28/GLIN/figures/parks_1m_jts_strtree_knn_piece_limit_candidate_ratio.png)
  - [probe_refine](/home/lh/Code/2026.5.28/GLIN/figures/parks_1m_jts_strtree_knn_piece_limit_probe_refine.png)

这次结果的核心结论是：**PARKS 1M 这个 workload 下，GLIN-piecewise 候选数偏多不是简单因为 `piece_limit` 没调好。**

候选放大倍数基本稳定：

| Selectivity | Candidates / Answers |
|---:|---:|
| 0.001% | 约 4.4x |
| 0.01% | 约 2.28x |
| 0.1% | 约 1.32x |
| 1% | 约 1.10x |

最佳 total time 对应的 PL：

| Selectivity | 最快 PL | Avg total |
|---:|---:|---:|
| 0.001% | 100 | 503 us |
| 0.01% | 100 | 658 us |
| 0.1% | 1000 | 1449 us |
| 1% | 1000 | 8463 us |

所以目前更像是：低 selectivity 下 GLIN-piecewise 的 Z-order interval/query augmentation 本身会带来候选放大；`piece_limit` 会影响 probe 和总时间，但不能从根上消除候选偏多。论文默认 `PL=10000` 在这组数据上不是绝对最优，但也在合理区间内。

验证已完成：

```bash
python3 -m py_compile scripts/summarize_piece_limit_sweep.py scripts/plot_piece_limit_sweep.py
scripts/run_parks1m_jts_piece_limit_sweep.sh
python3 scripts/summarize_piece_limit_sweep.py ...
python3 scripts/plot_piece_limit_sweep.py ...
```

下一步最合理的是把这组 PL sweep 和 **Boost R-tree intersects** 放到同一张图里，形成论文 Fig. 6/Fig. 7 风格：GLIN-piecewise 不同 PL vs Boost R-tree。这样就能直接判断每个 selectivity 下 GLIN 离 R-tree 差多少。





脚本：plot_piece_limit_vs_boost.py

它会读取：

- [parks_1m_jts_strtree_knn_piece_limit_summary.csv](/home/lh/Code/2026.5.28/GLIN/results/parks_1m_jts_strtree_knn_piece_limit_summary.csv:1)
- [parks_1m_jts_strtree_knn_summary.csv](/home/lh/Code/2026.5.28/GLIN/results/parks_1m_jts_strtree_knn_summary.csv:1)

然后生成 combined CSV：

[parks_1m_jts_strtree_knn_pl_vs_boost_intersects.csv](/home/lh/Code/2026.5.28/GLIN/results/parks_1m_jts_strtree_knn_pl_vs_boost_intersects.csv:1)

以及三张图：

- Fig. 6 风格 probing time：  
  [parks_1m_jts_strtree_knn_pl_vs_boost_fig6_probe_time.png](/home/lh/Code/2026.5.28/GLIN/figures/parks_1m_jts_strtree_knn_pl_vs_boost_fig6_probe_time.png)

- Fig. 7 风格 query response time：  
  [parks_1m_jts_strtree_knn_pl_vs_boost_fig7_query_time.png](/home/lh/Code/2026.5.28/GLIN/figures/parks_1m_jts_strtree_knn_pl_vs_boost_fig7_query_time.png)

- 候选数辅助图：  
  [parks_1m_jts_strtree_knn_pl_vs_boost_candidate_count.png](/home/lh/Code/2026.5.28/GLIN/figures/parks_1m_jts_strtree_knn_pl_vs_boost_candidate_count.png)

复现命令是：

```bash
python3 scripts/plot_piece_limit_vs_boost.py \
  --piece_summary results/parks_1m_jts_strtree_knn_piece_limit_summary.csv \
  --baseline_summary results/parks_1m_jts_strtree_knn_summary.csv \
  --combined_csv results/parks_1m_jts_strtree_knn_pl_vs_boost_intersects.csv \
  --output_dir figures \
  --figure_prefix parks_1m_jts_strtree_knn_pl_vs_boost
```

结果很清楚：

| Selectivity | GLIN-piecewise 最好 PL vs Boost |
|---:|---:|
| 0.001% | 约 2.56x 慢 |
| 0.01% | 约 1.86x 慢 |
| 0.1% | 约 1.16x 慢 |
| 1% | PL=1000 时约 0.95x，略快于 Boost |
**PARKS 1M JTS KNN workload 下，GLIN-piecewise 在低 selectivity 仍然输给 Boost，主要因为候选数更多；但在 1% 这种较大查询窗口下，probe 优势开始抵消 refinement 成本，PL=1000/10000 能接近甚至略快于 Boost。**



是的，下一步就是要把 **GEOS Quadtree** 引入基线。论文后面的 Fig. 8 到 Fig. 17 基本都不是只看 GLIN vs Boost R-tree，而是需要 **GLIN / Boost R-tree / Quadtree** 三方对比。

我已经直接补上并跑通了。

新增 Quadtree benchmark：

- [bench_geos_quadtree_wkt.cpp](/home/lh/Code/2026.5.28/GLIN/src/benchmark/bench_geos_quadtree_wkt.cpp:1)
- 已接入 [CMakeLists.txt](/home/lh/Code/2026.5.28/GLIN/CMakeLists.txt:1)
- 新增运行脚本：[run_parks1m_jts_quadtree.sh](/home/lh/Code/2026.5.28/GLIN/scripts/run_parks1m_jts_quadtree.sh:1)

我也把已有绘图/汇总脚本加上了 `GEOS_Quadtree` 图例和排序，所以后面可以直接三方对比。

已完成 PARKS 1M + JTS STRtree KNN 的 Quadtree 全量测试，结果汇总在：

[parks_1m_jts_strtree_knn_with_quadtree_summary.csv](/home/lh/Code/2026.5.28/GLIN/results/parks_1m_jts_strtree_knn_with_quadtree_summary.csv:1)

新图在：

- [query_time](/home/lh/Code/2026.5.28/GLIN/figures/parks_1m_jts_strtree_knn_with_quadtree_query_time.png)
- [candidates](/home/lh/Code/2026.5.28/GLIN/figures/parks_1m_jts_strtree_knn_with_quadtree_candidates.png)
- [candidate_ratio](/home/lh/Code/2026.5.28/GLIN/figures/parks_1m_jts_strtree_knn_with_quadtree_candidate_ratio.png)
- [contains_probe_refine](/home/lh/Code/2026.5.28/GLIN/figures/parks_1m_jts_strtree_knn_with_quadtree_contains_probe_refine.png)
- [intersects_probe_refine](/home/lh/Code/2026.5.28/GLIN/figures/parks_1m_jts_strtree_knn_with_quadtree_intersects_probe_refine.png)

初步结果很有价值：在 PARKS 1M 上，Quadtree 的候选数比 Boost R-tree 多很多；低 selectivity 下 Boost 仍最强，0.1% 附近 GLIN 开始接近，1% intersects 下 GLIN-piecewise 反而比 Boost 和 Quadtree 都快一些。

验证命令已跑通：

```bash
cmake --build build --target bench_geos_quadtree_wkt -j2
scripts/run_parks1m_jts_quadtree.sh
python3 scripts/summarize_parks1m_knn.py --glob 'results/parks_1m_jts_strtree_knn_[01]*pct_*.csv' --output results/parks_1m_jts_strtree_knn_with_quadtree_summary.csv
python3 scripts/plot_parks1m_knn_summary.py --input results/parks_1m_jts_strtree_knn_with_quadtree_summary.csv --output_dir figures --figure_prefix parks_1m_jts_strtree_knn_with_quadtree
```

下一步建议补 **Fig. 8 index size** 和 **Fig. 9 build time**。build time 已经有了，index size 还需要给 GLIN、Boost R-tree、Quadtree 写比较可靠的统计口径。

