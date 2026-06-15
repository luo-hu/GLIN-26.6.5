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
    "GLIN_PIECEWISE",
    "Boost_Rtree",
    "GEOS_Quadtree",
]
COLORS = {
    "DELI_DYNAMIC_SINGLE": "#2F6F73",
    "DELI_ALEX": "#3C8DBC",
    "GLIN_PIECEWISE": "#6D8F3F",
    "Boost_Rtree": "#B86442",
    "GEOS_Quadtree": "#7C5FB3",
}
LABELS = {
    "DELI_DYNAMIC_SINGLE": "DELI-Dynamic",
    "DELI_ALEX": "DELI-ALEX",
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


def write_notes(rows, output_dir, prefix):
    path = output_dir / f"{prefix}_diagnostics.txt"
    with path.open("w") as handle:
        handle.write("统一动态对比说明\n\n")
        handle.write("所有方法使用同一套 bulk-load / insert / delete / query workload。\n")
        handle.write("answers_match_boost=1 表示该方法在该 checkpoint 的答案集合与 Boost exact oracle 一致。\n")
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
                    f"index_mb_est={as_float(row, 'index_mb_estimate'):.3f}\n"
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
    paths.append(plot_metric(rows, output_dir, args.figure_prefix, "after_delete", "index_mb_estimate", "Estimated index size (MB)", "index_mb_estimate", args.dpi))
    paths.append(plot_metric(rows, output_dir, args.figure_prefix, "after_delete", "answers_match_boost", "Answers match Boost oracle", "correctness", args.dpi))
    paths.append(write_notes(rows, output_dir, args.figure_prefix))
    for path in paths:
        if path:
            print(f"Wrote {path}")


if __name__ == "__main__":
    main()
