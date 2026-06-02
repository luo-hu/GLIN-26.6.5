#!/usr/bin/env python3
"""Plot GLIN vs Boost R-tree benchmark CSVs.

用法示例：
  python3 scripts/plot_glin_vs_boost.py \
    --input results/aw60k_glin_contains_queries.csv \
    --input results/aw60k_glin_piece_intersects_queries.csv \
    --input results/aw60k_boost_rtree_contains_queries.csv \
    --input results/aw60k_boost_rtree_intersects_queries.csv \
    --output_dir figures

这个脚本读取 bench_glin_wkt / bench_boost_rtree_wkt 输出的逐 query CSV，
自动按 dataset、index、relationship 分组，计算平均查询时间、平均候选数、
平均答案数、构建时间，然后画柱状图。
"""

import argparse
import csv
import math
import os
from collections import OrderedDict
from pathlib import Path

# 有些虚拟机环境里 ~/.config/matplotlib 不可写。这里把 matplotlib 缓存放到项目输出目录，
# 避免每次运行都打印一大段环境警告。
os.environ.setdefault("MPLCONFIGDIR", "figures/.matplotlib_cache")

import matplotlib.pyplot as plt


REQUIRED_COLUMNS = {
    "dataset",
    "index",
    "relationship",
    "loaded_count",
    "build_ns",
    "probe_ns",
    "refine_ns",
    "total_ns",
    "candidates",
    "answers",
}


INDEX_LABELS = {
    "GLIN": "GLIN",
    "GLIN_PIECEWISE": "GLIN-piecewise",
    "Boost_Rtree": "Boost R-tree",
    "GEOS_Quadtree": "GEOS Quadtree",
}


INDEX_COLORS = {
    "GLIN": "#3B6EA8",
    "GLIN_PIECEWISE": "#70A37F",
    "Boost_Rtree": "#C46A4A",
    "GEOS_Quadtree": "#8C6BB1",
}


def parse_args():
    parser = argparse.ArgumentParser(
        description="Draw bar charts from GLIN and Boost R-tree benchmark CSVs."
    )
    parser.add_argument(
        "--input",
        action="append",
        dest="inputs",
        help="Input CSV path. Can be passed multiple times.",
    )
    parser.add_argument(
        "--glob",
        default="results/*_queries.csv",
        help="CSV glob used when --input is omitted. Default: results/*_queries.csv",
    )
    parser.add_argument(
        "--output_dir",
        default="figures",
        help="Directory for PNG figures and summary CSV. Default: figures",
    )
    parser.add_argument(
        "--dpi",
        type=int,
        default=180,
        help="Output image DPI. Default: 180",
    )
    return parser.parse_args()


def to_float(row, column, path):
    try:
        return float(row[column])
    except (KeyError, ValueError) as exc:
        raise ValueError(f"Bad numeric value for column {column!r} in {path}") from exc


def load_rows(paths):
    rows = []
    for path in paths:
        with path.open(newline="") as f:
            reader = csv.DictReader(f)
            if reader.fieldnames is None:
                raise ValueError(f"Empty CSV: {path}")

            missing = REQUIRED_COLUMNS.difference(reader.fieldnames)
            if missing:
                missing_text = ", ".join(sorted(missing))
                raise ValueError(f"{path} is missing columns: {missing_text}")

            for row in reader:
                row["_path"] = str(path)
                rows.append(row)

    if not rows:
        raise ValueError("No data rows found in input CSVs")
    return rows


def summarize(rows):
    groups = OrderedDict()
    for row in rows:
        key = (
            row["dataset"],
            row["relationship"],
            row["index"],
            int(float(row["loaded_count"])),
        )
        if key not in groups:
            groups[key] = {
                "dataset": row["dataset"],
                "relationship": row["relationship"],
                "index": row["index"],
                "loaded_count": int(float(row["loaded_count"])),
                "rows": 0,
                "build_ns": to_float(row, "build_ns", row["_path"]),
                "probe_ns_sum": 0.0,
                "refine_ns_sum": 0.0,
                "total_ns_sum": 0.0,
                "candidates_sum": 0.0,
                "answers_sum": 0.0,
            }

        group = groups[key]
        group["rows"] += 1
        group["probe_ns_sum"] += to_float(row, "probe_ns", row["_path"])
        group["refine_ns_sum"] += to_float(row, "refine_ns", row["_path"])
        group["total_ns_sum"] += to_float(row, "total_ns", row["_path"])
        group["candidates_sum"] += to_float(row, "candidates", row["_path"])
        group["answers_sum"] += to_float(row, "answers", row["_path"])

    summary = []
    for group in groups.values():
        n = group["rows"]
        summary.append(
            {
                "dataset": group["dataset"],
                "relationship": group["relationship"],
                "index": group["index"],
                "loaded_count": group["loaded_count"],
                "query_rows": n,
                "build_ms": group["build_ns"] / 1e6,
                "avg_probe_ns": group["probe_ns_sum"] / n,
                "avg_refine_ns": group["refine_ns_sum"] / n,
                "avg_total_ns": group["total_ns_sum"] / n,
                "avg_total_us": group["total_ns_sum"] / n / 1e3,
                "avg_candidates": group["candidates_sum"] / n,
                "avg_answers": group["answers_sum"] / n,
            }
        )
    return summary


def relationship_order(summary):
    preferred = ["contains", "intersects"]
    found = {row["relationship"] for row in summary}
    ordered = [rel for rel in preferred if rel in found]
    ordered.extend(sorted(found.difference(ordered)))
    return ordered


def index_order(summary):
    preferred = ["GLIN", "GLIN_PIECEWISE", "Boost_Rtree", "GEOS_Quadtree"]
    found = {row["index"] for row in summary}
    ordered = [idx for idx in preferred if idx in found]
    ordered.extend(sorted(found.difference(ordered)))
    return ordered


def write_summary_csv(summary, output_dir):
    path = output_dir / "glin_vs_boost_summary.csv"
    fields = [
        "dataset",
        "relationship",
        "index",
        "loaded_count",
        "query_rows",
        "build_ms",
        "avg_probe_ns",
        "avg_refine_ns",
        "avg_total_ns",
        "avg_total_us",
        "avg_candidates",
        "avg_answers",
    ]
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for row in summary:
            writer.writerow(row)
    return path


def add_bar_labels(ax, bars, fmt="{:.2f}", rotation=0):
    for bar in bars:
        height = bar.get_height()
        if not math.isfinite(height):
            continue
        ax.annotate(
            fmt.format(height),
            xy=(bar.get_x() + bar.get_width() / 2, height),
            xytext=(0, 3),
            textcoords="offset points",
            ha="center",
            va="bottom",
            fontsize=8,
            rotation=rotation,
        )


def plot_grouped_metric(summary, output_dir, metric, ylabel, title, filename, value_fmt):
    rels = relationship_order(summary)
    indexes = index_order(summary)
    width = 0.22
    x_positions = list(range(len(rels)))

    fig, ax = plt.subplots(figsize=(8.0, 4.8))
    for offset_id, index in enumerate(indexes):
        values = []
        for rel in rels:
            matched = [
                row for row in summary
                if row["relationship"] == rel and row["index"] == index
            ]
            values.append(matched[0][metric] if matched else float("nan"))

        offset = (offset_id - (len(indexes) - 1) / 2) * width
        bars = ax.bar(
            [x + offset for x in x_positions],
            values,
            width,
            label=INDEX_LABELS.get(index, index),
            color=INDEX_COLORS.get(index),
            edgecolor="black",
            linewidth=0.5,
        )
        add_bar_labels(ax, bars, fmt=value_fmt)

    ax.set_title(title)
    ax.set_ylabel(ylabel)
    ax.set_xticks(x_positions)
    ax.set_xticklabels(rels)
    ax.legend(frameon=False)
    ax.grid(axis="y", linestyle="--", alpha=0.35)
    fig.tight_layout()

    path = output_dir / filename
    fig.savefig(path)
    plt.close(fig)
    return path


def plot_probe_refine_breakdown(summary, output_dir):
    labels = []
    probe_values = []
    refine_values = []
    colors = []

    for rel in relationship_order(summary):
        for index in index_order(summary):
            matched = [
                row for row in summary
                if row["relationship"] == rel and row["index"] == index
            ]
            if not matched:
                continue
            row = matched[0]
            labels.append(f"{rel}\n{INDEX_LABELS.get(index, index)}")
            probe_values.append(row["avg_probe_ns"] / 1e3)
            refine_values.append(row["avg_refine_ns"] / 1e3)
            colors.append(INDEX_COLORS.get(index))

    fig, ax = plt.subplots(figsize=(9.5, 5.0))
    x_positions = list(range(len(labels)))
    ax.bar(
        x_positions,
        probe_values,
        label="Probe",
        color=colors,
        edgecolor="black",
        linewidth=0.5,
    )
    ax.bar(
        x_positions,
        refine_values,
        bottom=probe_values,
        label="Refine",
        color="white",
        edgecolor="black",
        linewidth=0.8,
        hatch="//",
    )
    ax.set_title("Average Query Time Breakdown")
    ax.set_ylabel("Time (microseconds)")
    ax.set_xticks(x_positions)
    ax.set_xticklabels(labels)
    ax.legend(frameon=False)
    ax.grid(axis="y", linestyle="--", alpha=0.35)
    fig.tight_layout()

    path = output_dir / "glin_vs_boost_probe_refine_breakdown.png"
    fig.savefig(path)
    plt.close(fig)
    return path


def print_summary(summary):
    print("Summary:")
    print(
        "relationship,index,queries,build_ms,avg_total_us,"
        "avg_probe_ns,avg_refine_ns,avg_candidates,avg_answers"
    )
    for row in summary:
        print(
            f"{row['relationship']},{row['index']},{row['query_rows']},"
            f"{row['build_ms']:.3f},{row['avg_total_us']:.3f},"
            f"{row['avg_probe_ns']:.2f},{row['avg_refine_ns']:.2f},"
            f"{row['avg_candidates']:.2f},{row['avg_answers']:.2f}"
        )


def main():
    args = parse_args()
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    if args.inputs:
      paths = [Path(path) for path in args.inputs]
    else:
      paths = sorted(Path(".").glob(args.glob))

    if not paths:
        raise SystemExit("No input CSVs found. Use --input or adjust --glob.")

    rows = load_rows(paths)
    summary = summarize(rows)
    summary_path = write_summary_csv(summary, output_dir)

    figure_paths = [
        plot_grouped_metric(
            summary,
            output_dir,
            metric="avg_total_us",
            ylabel="Average query time (microseconds)",
            title="GLIN vs Boost R-tree Query Time",
            filename="glin_vs_boost_query_time.png",
            value_fmt="{:.2f}",
        ),
        plot_grouped_metric(
            summary,
            output_dir,
            metric="avg_candidates",
            ylabel="Average candidates per query",
            title="Average Candidate Count",
            filename="glin_vs_boost_candidates.png",
            value_fmt="{:.2f}",
        ),
        plot_grouped_metric(
            summary,
            output_dir,
            metric="build_ms",
            ylabel="Build time (milliseconds)",
            title="Index Build Time",
            filename="glin_vs_boost_build_time.png",
            value_fmt="{:.1f}",
        ),
        plot_probe_refine_breakdown(summary, output_dir),
    ]

    print_summary(summary)
    print(f"\nWrote summary: {summary_path}")
    for path in figure_paths:
        print(f"Wrote figure: {path}")


if __name__ == "__main__":
    main()
