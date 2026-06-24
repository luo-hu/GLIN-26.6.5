#!/usr/bin/env python3
"""Plot unified dynamic comparison summaries."""

import argparse
import csv
import math
import os
import re
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", "figures/.matplotlib_cache")

import matplotlib.pyplot as plt


INDEX_ORDER = [
    "DELI_DYNAMIC_SINGLE",
    "DELI_ALEX",
    "DELI_ALEX_HYBRID",
    "DELI_ALEX_HYBRID_BUF",
    "DELI_ALEX_HYBRID_BOUNDED",
    "DELI_ALEX_HYBRID_LOCAL_BOUNDED",
    "DELI_ALEX_HYBRID_COST",
    "GLIN_PIECEWISE",
    "Boost_Rtree",
    "GEOS_Quadtree",
]
COLORS = {
    "DELI_DYNAMIC_SINGLE": "#2F6F73",
    "DELI_ALEX": "#3C8DBC",
    "DELI_ALEX_HYBRID": "#1F9E89",
    "DELI_ALEX_HYBRID_BUF": "#8BAE3F",
    "DELI_ALEX_HYBRID_BOUNDED": "#D49A2A",
    "DELI_ALEX_HYBRID_LOCAL_BOUNDED": "#E76F51",
    "DELI_ALEX_HYBRID_COST": "#C43C7A",
    "GLIN_PIECEWISE": "#6D8F3F",
    "Boost_Rtree": "#B86442",
    "GEOS_Quadtree": "#7C5FB3",
}
LABELS = {
    "DELI_DYNAMIC_SINGLE": "DELI-Dynamic",
    "DELI_ALEX": "DELI-ALEX",
    "DELI_ALEX_HYBRID": "DELI-ALEX-Hybrid",
    "DELI_ALEX_HYBRID_BUF": "DELI-ALEX-Hybrid-Buf",
    "DELI_ALEX_HYBRID_BOUNDED": "DELI-ALEX-Hybrid-Bounded",
    "DELI_ALEX_HYBRID_LOCAL_BOUNDED": "DELI-ALEX-Hybrid-LocalBounded",
    "DELI_ALEX_HYBRID_COST": "DELI-ALEX-Hybrid-Cost",
    "GLIN_PIECEWISE": "GLIN-piece",
    "Boost_Rtree": "Boost R-tree",
    "GEOS_Quadtree": "GEOS Quadtree",
}


def parse_args():
    parser = argparse.ArgumentParser(description="绘制统一动态对比图。")
    parser.add_argument("--input", required=True)
    parser.add_argument("--output_dir", required=True)
    parser.add_argument("--figure_prefix", default="dynamic_compare")
    parser.add_argument("--exclude_datasets", default="")
    parser.add_argument("--dpi", type=int, default=180)
    return parser.parse_args()


def split_names(value):
    if not value:
        return set()
    return {item for item in re.split(r"[,\s]+", value.strip()) if item}


def as_float(row, field):
    try:
        return float(row.get(field) or 0)
    except ValueError:
        return math.nan


def load_rows(path):
    with Path(path).open(newline="") as handle:
        return list(csv.DictReader(handle))


def order_key(row):
    index = row["index"]
    return INDEX_ORDER.index(index) if index in INDEX_ORDER else len(INDEX_ORDER)


def plot_metric(rows, output_dir, prefix, checkpoint, metric, ylabel, filename, dpi, log_y=False):
    subset = [row for row in rows if row["checkpoint"] == checkpoint]
    if metric == "answers_match_boost":
        subset = [row for row in subset if row.get(metric) != "-1"]
    if not subset:
        return None
    groups = sorted({(row["dataset"], row.get("selectivity", "")) for row in subset})
    indexes = sorted({row["index"] for row in subset}, key=lambda x: INDEX_ORDER.index(x) if x in INDEX_ORDER else 99)
    fig, ax = plt.subplots(figsize=(max(8, len(groups) * 1.8), 4.6))
    group_width = 0.78
    bar_width = group_width / max(1, len(indexes))
    centers = range(len(groups))
    for i, index in enumerate(indexes):
        xs = [c - group_width / 2 + bar_width / 2 + i * bar_width for c in centers]
        ys = []
        for dataset, selectivity in groups:
            row = next((r for r in subset if r["dataset"] == dataset and r.get("selectivity", "") == selectivity and r["index"] == index), None)
            ys.append(as_float(row, metric) if row else math.nan)
        ax.bar(xs, ys, width=bar_width * 0.92, color=COLORS.get(index), edgecolor="black", linewidth=0.45, label=LABELS.get(index, index))
    ax.set_xticks(list(centers))
    ax.set_xticklabels([f"{d}\n{s}" for d, s in groups])
    ax.set_ylabel(ylabel)
    ax.set_title(f"{checkpoint}: {ylabel}")
    ax.grid(axis="y", linestyle="--", alpha=0.32)
    if log_y:
        ax.set_yscale("log")
    ax.legend(frameon=False, ncol=min(4, len(indexes)))
    fig.tight_layout()
    path = output_dir / f"{prefix}_{checkpoint}_{filename}.png"
    fig.savefig(path, dpi=dpi)
    plt.close(fig)
    return path


def safe_name(value):
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", value or "unknown")


def plot_mixed_metric(rows, output_dir, prefix, metric, ylabel, filename, dpi, log_y=False):
    subset = [row for row in rows if row.get("workload_mode") == "mixed"]
    if not subset:
        return []
    paths = []
    groups = sorted({
        (row.get("mixed_profile", "custom"), row["dataset"], row.get("selectivity", ""))
        for row in subset
    })
    for profile, dataset, selectivity in groups:
        group_rows = [
            row for row in subset
            if row.get("mixed_profile", "custom") == profile
            and row["dataset"] == dataset
            and row.get("selectivity", "") == selectivity
        ]
        indexes = sorted({row["index"] for row in group_rows}, key=lambda x: INDEX_ORDER.index(x) if x in INDEX_ORDER else 99)
        fig, ax = plt.subplots(figsize=(7.6, 4.6))
        for index in indexes:
            series = [row for row in group_rows if row["index"] == index]
            series.sort(key=lambda row: as_float(row, "operation_count"))
            xs = [as_float(row, "operation_count") for row in series]
            ys = [as_float(row, metric) for row in series]
            ax.plot(xs, ys, marker="o", linewidth=1.5, markersize=3.5,
                    color=COLORS.get(index), label=LABELS.get(index, index))
        ax.set_xlabel("Operations")
        ax.set_ylabel(ylabel)
        ax.set_title(f"{profile} {dataset} {selectivity}: {ylabel}")
        ax.grid(axis="both", linestyle="--", alpha=0.32)
        if log_y:
            ax.set_yscale("log")
        ax.legend(frameon=False, ncol=2, fontsize=8)
        fig.tight_layout()
        path = output_dir / (
            f"{prefix}_mixed_{safe_name(profile)}_{safe_name(dataset)}_"
            f"{safe_name(selectivity)}_{filename}.png"
        )
        fig.savefig(path, dpi=dpi)
        plt.close(fig)
        paths.append(path)
    return paths


def write_notes(rows, output_dir, prefix):
    path = output_dir / f"{prefix}_diagnostics.txt"
    with path.open("w") as handle:
        handle.write("统一动态对比说明\n\n")
        handle.write("所有方法使用同一套 bulk-load / insert / delete / query workload。\n")
        handle.write("当 workload_mode=mixed 时，横轴 operation_count 表示单线程交错操作进度。\n")
        handle.write("mixed workload 的 avg/p95/p99 query latency 来自交错执行中真实发生的 query；correctness 在 checkpoint final state 上用固定 query set 与 Boost exact oracle 对齐。\n")
        handle.write("query_tps 只统计 query 自身耗时；overall_ops_tps 统计 checkpoint 内 query+insert+delete 的前台总耗时，不包含 correctness oracle。\n")
        handle.write("p95_insert_ms/p99_insert_ms 和 p95_delete_ms/p99_delete_ms 表示 mixed interval 内单次更新操作的尾延迟。\n")
        handle.write("answers_match_boost=1 表示该方法在该 checkpoint 的答案集合与 Boost exact oracle 一致。\n")
        handle.write("answers_match_boost=-1 表示本次 checkpoint 按 CHECK_CORRECTNESS/CORRECTNESS_EVERY_N 设置跳过了 oracle 检查，不代表答案错误。\n")
        handle.write("index_mb_estimate 是粗略估算，不等价于精确内存 profiler；用于先判断数量级。\n\n")
        for checkpoint in ["after_bulkload", "after_insert", "after_delete"]:
            handle.write(f"{checkpoint}\n")
            subset = [r for r in rows if r["checkpoint"] == checkpoint]
            for row in sorted(subset, key=order_key):
                handle.write(
                    f"  {row['dataset']} {row.get('selectivity', '')} {row['index']}: "
                    f"avg_query_ms={as_float(row, 'avg_query_ms'):.4f}, "
                    f"insert_tps={as_float(row, 'insert_tps'):.2f}, "
                    f"delete_tps={as_float(row, 'delete_tps'):.2f}, "
                    f"match={row.get('answers_match_boost')}, "
                    f"index_mb_est={as_float(row, 'index_mb_estimate'):.3f}, "
                    f"local_delta_bound={as_float(row, 'local_delta_bound'):.0f}, "
                    f"delete_compact_fraction={as_float(row, 'delete_compact_fraction'):.2f}, "
                    f"delete_compact_bound={as_float(row, 'delete_compact_bound'):.0f}, "
                    f"avg_beta={as_float(row, 'avg_beta'):.3f}, "
                    f"avg_tau={as_float(row, 'avg_tau'):.3f}, "
                    f"avg_delta_bound={as_float(row, 'avg_adaptive_delta_bound'):.1f}, "
                    f"avg_delete_bound={as_float(row, 'avg_adaptive_delete_bound'):.1f}, "
                    f"max_local_delta={as_float(row, 'max_local_delta_size'):.0f}, "
                    f"blocks_with_delta={as_float(row, 'blocks_with_delta'):.0f}, "
                    f"local_compactions_stage={as_float(row, 'local_compaction_count_stage'):.0f}, "
                    f"local_compaction_ms_stage={as_float(row, 'local_compaction_ns_stage') / 1e6:.3f}, "
                    f"avg_local_compaction_us={as_float(row, 'avg_local_compaction_us_stage'):.3f}\n"
                )
            handle.write("\n")
    return path


def main():
    args = parse_args()
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    rows = load_rows(args.input)
    excluded = split_names(args.exclude_datasets)
    if excluded:
        rows = [row for row in rows if row["dataset"] not in excluded]
    paths = []
    paths.append(plot_metric(rows, output_dir, args.figure_prefix, "after_insert", "insert_tps", "Insert throughput (ops/s)", "insert_tps", args.dpi))
    paths.append(plot_metric(rows, output_dir, args.figure_prefix, "after_delete", "delete_tps", "Delete throughput (ops/s)", "delete_tps", args.dpi))
    paths.append(plot_metric(rows, output_dir, args.figure_prefix, "after_insert", "avg_query_ms", "Average query time after insert (ms)", "avg_query_ms", args.dpi))
    paths.append(plot_metric(rows, output_dir, args.figure_prefix, "after_delete", "avg_query_ms", "Average query time after delete (ms)", "avg_query_ms", args.dpi))
    paths.append(plot_metric(rows, output_dir, args.figure_prefix, "after_insert", "p95_query_ms", "P95 query latency after insert (ms)", "p95_query_ms", args.dpi))
    paths.append(plot_metric(rows, output_dir, args.figure_prefix, "after_delete", "p95_query_ms", "P95 query latency after delete (ms)", "p95_query_ms", args.dpi))
    paths.append(plot_metric(rows, output_dir, args.figure_prefix, "after_insert", "p99_query_ms", "P99 query latency after insert (ms)", "p99_query_ms", args.dpi))
    paths.append(plot_metric(rows, output_dir, args.figure_prefix, "after_delete", "p99_query_ms", "P99 query latency after delete (ms)", "p99_query_ms", args.dpi))
    paths.append(plot_metric(rows, output_dir, args.figure_prefix, "after_delete", "index_mb_estimate", "Estimated index size (MB)", "index_mb_estimate", args.dpi))
    paths.append(plot_metric(rows, output_dir, args.figure_prefix, "after_delete", "answers_match_boost", "Answers match Boost oracle", "correctness", args.dpi))
    paths.extend(plot_mixed_metric(rows, output_dir, args.figure_prefix, "avg_query_ms", "Average query latency in mixed workload (ms)", "avg_query_ms", args.dpi))
    paths.extend(plot_mixed_metric(rows, output_dir, args.figure_prefix, "p95_query_ms", "P95 query latency in mixed workload (ms)", "p95_query_ms", args.dpi))
    paths.extend(plot_mixed_metric(rows, output_dir, args.figure_prefix, "p99_query_ms", "P99 query latency in mixed workload (ms)", "p99_query_ms", args.dpi))
    paths.extend(plot_mixed_metric(rows, output_dir, args.figure_prefix, "block_checks", "Block summary checks per interval", "block_checks", args.dpi))
    paths.extend(plot_mixed_metric(rows, output_dir, args.figure_prefix, "visited_blocks", "Visited blocks per interval", "visited_blocks", args.dpi))
    paths.extend(plot_mixed_metric(rows, output_dir, args.figure_prefix, "compact_records_scanned", "Compact records scanned per interval", "compact_records_scanned", args.dpi))
    paths.extend(plot_mixed_metric(rows, output_dir, args.figure_prefix, "delta_records_scanned", "Delta records scanned per interval", "delta_records_scanned", args.dpi))
    paths.extend(plot_mixed_metric(rows, output_dir, args.figure_prefix, "mbr_candidates", "MBR candidates per interval", "mbr_candidates", args.dpi))
    paths.extend(plot_mixed_metric(rows, output_dir, args.figure_prefix, "insert_tps", "Interval insert throughput (ops/s)", "insert_tps", args.dpi))
    paths.extend(plot_mixed_metric(rows, output_dir, args.figure_prefix, "delete_tps", "Interval delete throughput (ops/s)", "delete_tps", args.dpi))
    paths.extend(plot_mixed_metric(rows, output_dir, args.figure_prefix, "query_tps", "Interval query throughput (queries/s)", "query_tps", args.dpi))
    paths.extend(plot_mixed_metric(rows, output_dir, args.figure_prefix, "overall_ops_tps", "Overall foreground throughput (ops/s)", "overall_ops_tps", args.dpi))
    paths.extend(plot_mixed_metric(rows, output_dir, args.figure_prefix, "p95_insert_ms", "P95 insert latency in mixed workload (ms)", "p95_insert_ms", args.dpi))
    paths.extend(plot_mixed_metric(rows, output_dir, args.figure_prefix, "p99_insert_ms", "P99 insert latency in mixed workload (ms)", "p99_insert_ms", args.dpi))
    paths.extend(plot_mixed_metric(rows, output_dir, args.figure_prefix, "p95_delete_ms", "P95 delete latency in mixed workload (ms)", "p95_delete_ms", args.dpi))
    paths.extend(plot_mixed_metric(rows, output_dir, args.figure_prefix, "p99_delete_ms", "P99 delete latency in mixed workload (ms)", "p99_delete_ms", args.dpi))
    paths.extend(plot_mixed_metric(rows, output_dir, args.figure_prefix, "local_compaction_count_stage", "Local compactions per interval", "local_compaction_count_stage", args.dpi))
    paths.extend(plot_mixed_metric(rows, output_dir, args.figure_prefix, "local_compaction_ns_stage", "Local compaction time per interval (ns)", "local_compaction_ns_stage", args.dpi))
    paths.extend(plot_mixed_metric(rows, output_dir, args.figure_prefix, "avg_local_delta_size", "Average local delta size", "avg_local_delta_size", args.dpi))
    paths.extend(plot_mixed_metric(rows, output_dir, args.figure_prefix, "max_local_delta_size", "Max local delta size", "max_local_delta_size", args.dpi))
    paths.extend(plot_mixed_metric(rows, output_dir, args.figure_prefix, "tombstone_ratio", "Tombstone ratio", "tombstone_ratio", args.dpi))
    paths.extend(plot_mixed_metric(rows, output_dir, args.figure_prefix, "avg_beta", "Average beta (local delta ratio)", "avg_beta", args.dpi))
    paths.extend(plot_mixed_metric(rows, output_dir, args.figure_prefix, "avg_tau", "Average tau (tombstone ratio)", "avg_tau", args.dpi))
    paths.extend(plot_mixed_metric(rows, output_dir, args.figure_prefix, "avg_adaptive_delta_bound", "Average adaptive delta bound", "avg_adaptive_delta_bound", args.dpi))
    paths.extend(plot_mixed_metric(rows, output_dir, args.figure_prefix, "avg_adaptive_delete_bound", "Average adaptive delete bound", "avg_adaptive_delete_bound", args.dpi))
    paths.append(write_notes(rows, output_dir, args.figure_prefix))
    for path in paths:
        if path:
            print(f"Wrote {path}")


if __name__ == "__main__":
    main()
