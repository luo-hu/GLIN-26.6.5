#!/usr/bin/env python3
"""Plot figures from results/aw1m_knn_summary.csv.

这个脚本专门读取 summarize_aw1m_knn.py 生成的汇总表，不再读取逐 query CSV。

用法：
  python3 scripts/plot_aw1m_knn_summary.py

也可以手动指定输入和输出目录：
  python3 scripts/plot_aw1m_knn_summary.py \
    --input results/aw1m_knn_summary.csv \
    --output_dir figures
"""

import argparse
import csv
import os
from pathlib import Path

# 避免某些虚拟机里 ~/.config/matplotlib 不可写导致运行时警告。
os.environ.setdefault("MPLCONFIGDIR", "figures/.matplotlib_cache")

import matplotlib.pyplot as plt


SELECTIVITY_ORDER = ["0.001%", "0.01%", "0.1%", "1%"]
RELATIONSHIP_ORDER = ["contains", "intersects"]
INDEX_ORDER = ["GLIN", "GLIN_PIECEWISE", "Boost_Rtree", "GEOS_Quadtree"]

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

INDEX_MARKERS = {
    "GLIN": "o",
    "GLIN_PIECEWISE": "s",
    "Boost_Rtree": "^",
    "GEOS_Quadtree": "D",
}

REQUIRED_COLUMNS = {
    "selectivity",
    "dataset",
    "index",
    "relationship",
    "loaded_count",
    "query_rows",
    "build_ms",
    "avg_probe_ns",
    "avg_refine_ns",
    "avg_total_us",
    "avg_candidates",
    "avg_answers",
}


def parse_args():
    parser = argparse.ArgumentParser(
        description="Plot AREAWATER 1M KNN-selectivity benchmark summary."
    )
    parser.add_argument(
        "--input",
        default="results/aw1m_knn_summary.csv",
        help="Input summary CSV. Default: results/aw1m_knn_summary.csv",
    )
    parser.add_argument(
        "--output_dir",
        default="figures",
        help="Output figure directory. Default: figures",
    )
    parser.add_argument(
        "--figure_prefix",
        default="",
        help="Output figure filename prefix. Default: derived from input filename.",
    )
    parser.add_argument(
        "--dpi",
        type=int,
        default=180,
        help="Output image DPI. Default: 180",
    )
    return parser.parse_args()


def load_summary(path):
    with path.open(newline="") as f:
        reader = csv.DictReader(f)
        if reader.fieldnames is None:
            raise ValueError(f"Empty CSV: {path}")

        missing = REQUIRED_COLUMNS.difference(reader.fieldnames)
        if missing:
            raise ValueError(f"{path} missing columns: {', '.join(sorted(missing))}")

        rows = []
        for row in reader:
            parsed = dict(row)
            for column in [
                "loaded_count",
                "query_rows",
                "build_ms",
                "avg_probe_ns",
                "avg_refine_ns",
                "avg_total_us",
                "avg_candidates",
                "avg_answers",
            ]:
                parsed[column] = float(row[column])
            if parsed["avg_answers"] > 0:
                parsed["avg_candidate_ratio"] = (
                    parsed["avg_candidates"] / parsed["avg_answers"]
                )
            else:
                parsed["avg_candidate_ratio"] = 0.0
            rows.append(parsed)

    if not rows:
        raise ValueError(f"No rows found in {path}")
    return rows


def ordered_selectivities(rows):
    found = {row["selectivity"] for row in rows}
    ordered = [value for value in SELECTIVITY_ORDER if value in found]
    ordered.extend(sorted(found.difference(ordered)))
    return ordered


def ordered_indexes(rows, relationship):
    found = {row["index"] for row in rows if row["relationship"] == relationship}
    ordered = [value for value in INDEX_ORDER if value in found]
    ordered.extend(sorted(found.difference(ordered)))
    return ordered


def row_lookup(rows, relationship, index, selectivity):
    for row in rows:
        if (
            row["relationship"] == relationship
            and row["index"] == index
            and row["selectivity"] == selectivity
        ):
            return row
    return None


def setup_axis(ax, title, ylabel, use_log_y=True):
    ax.set_title(title)
    ax.set_ylabel(ylabel)
    ax.set_xlabel("Selectivity")
    if use_log_y:
        ax.set_yscale("log")
    ax.grid(axis="y", linestyle="--", alpha=0.35)


def plot_metric_by_relationship(rows, output_dir, metric, ylabel, title, filename,
                                dpi, use_log_y=True):
    selectivities = ordered_selectivities(rows)
    fig, axes = plt.subplots(1, 2, figsize=(11.0, 4.6), sharex=True)

    for ax, relationship in zip(axes, RELATIONSHIP_ORDER):
        indexes = ordered_indexes(rows, relationship)
        x_values = list(range(len(selectivities)))

        for index in indexes:
            y_values = []
            for selectivity in selectivities:
                row = row_lookup(rows, relationship, index, selectivity)
                y_values.append(row[metric] if row else float("nan"))

            ax.plot(
                x_values,
                y_values,
                label=INDEX_LABELS.get(index, index),
                color=INDEX_COLORS.get(index),
                marker=INDEX_MARKERS.get(index, "o"),
                linewidth=2.0,
                markersize=6.0,
            )

        setup_axis(ax, relationship.capitalize(), ylabel, use_log_y=use_log_y)
        ax.set_xticks(x_values)
        ax.set_xticklabels(selectivities)
        ax.legend(frameon=False)

    fig.suptitle(title)
    fig.tight_layout()
    path = output_dir / filename
    fig.savefig(path, dpi=dpi)
    plt.close(fig)
    return path


def plot_probe_refine_breakdown(rows, output_dir, figure_prefix, dpi):
    selectivities = ordered_selectivities(rows)
    figure_paths = []

    for relationship in RELATIONSHIP_ORDER:
        indexes = ordered_indexes(rows, relationship)
        labels = []
        probe_us = []
        refine_us = []
        colors = []

        for selectivity in selectivities:
            for index in indexes:
                row = row_lookup(rows, relationship, index, selectivity)
                if not row:
                    continue
                labels.append(f"{selectivity}\n{INDEX_LABELS.get(index, index)}")
                probe_us.append(row["avg_probe_ns"] / 1000.0)
                refine_us.append(row["avg_refine_ns"] / 1000.0)
                colors.append(INDEX_COLORS.get(index))

        fig, ax = plt.subplots(figsize=(11.5, 5.0))
        x_values = list(range(len(labels)))
        ax.bar(
            x_values,
            probe_us,
            label="Probe",
            color=colors,
            edgecolor="black",
            linewidth=0.5,
        )
        ax.bar(
            x_values,
            refine_us,
            bottom=probe_us,
            label="Refine",
            color="white",
            edgecolor="black",
            linewidth=0.8,
            hatch="//",
        )
        ax.set_title(f"{relationship.capitalize()} Probe/Refine Breakdown")
        ax.set_ylabel("Time (microseconds, log scale)")
        ax.set_yscale("log")
        ax.set_xticks(x_values)
        ax.set_xticklabels(labels)
        ax.grid(axis="y", linestyle="--", alpha=0.35)
        ax.legend(frameon=False)
        fig.tight_layout()

        path = output_dir / f"{figure_prefix}_{relationship}_probe_refine.png"
        fig.savefig(path, dpi=dpi)
        plt.close(fig)
        figure_paths.append(path)

    return figure_paths


def plot_build_time(rows, output_dir, figure_prefix, dpi):
    # 同一个 index 在不同 selectivity 下会重复建索引。这里画平均 build time，主要看量级。
    groups = {}
    for row in rows:
        groups.setdefault(row["index"], []).append(row["build_ms"])

    indexes = [index for index in INDEX_ORDER if index in groups]
    indexes.extend(sorted(set(groups).difference(indexes)))
    values = [sum(groups[index]) / len(groups[index]) for index in indexes]

    fig, ax = plt.subplots(figsize=(7.2, 4.4))
    bars = ax.bar(
        [INDEX_LABELS.get(index, index) for index in indexes],
        values,
        color=[INDEX_COLORS.get(index) for index in indexes],
        edgecolor="black",
        linewidth=0.5,
    )
    ax.set_title("Average Build Time")
    ax.set_ylabel("Build time (milliseconds)")
    ax.grid(axis="y", linestyle="--", alpha=0.35)
    for bar in bars:
        height = bar.get_height()
        ax.annotate(
            f"{height:.1f}",
            xy=(bar.get_x() + bar.get_width() / 2, height),
            xytext=(0, 3),
            textcoords="offset points",
            ha="center",
            va="bottom",
            fontsize=8,
        )
    fig.tight_layout()

    path = output_dir / f"{figure_prefix}_build_time.png"
    fig.savefig(path, dpi=dpi)
    plt.close(fig)
    return path


def print_brief(rows):
    print("Loaded summary rows:")
    for relationship in RELATIONSHIP_ORDER:
        for selectivity in ordered_selectivities(rows):
            parts = []
            for index in ordered_indexes(rows, relationship):
                row = row_lookup(rows, relationship, index, selectivity)
                if row:
                    label = INDEX_LABELS.get(index, index)
                    parts.append(
                        f"{label}={row['avg_total_us']:.3f}us/"
                        f"{row['avg_candidates']:.2f}cand"
                    )
            if parts:
                print(f"  {relationship} {selectivity}: " + " | ".join(parts))


def main():
    args = parse_args()
    input_path = Path(args.input)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    rows = load_summary(input_path)
    figure_prefix = args.figure_prefix
    if not figure_prefix:
        figure_prefix = input_path.stem
        if figure_prefix.endswith("_summary"):
            figure_prefix = figure_prefix[:-len("_summary")]

    dataset_label = rows[0]["dataset"]
    figure_paths = [
        plot_metric_by_relationship(
            rows,
            output_dir,
            metric="avg_total_us",
            ylabel="Average query time (microseconds, log scale)",
            title=f"{dataset_label} KNN Query Time",
            filename=f"{figure_prefix}_query_time.png",
            dpi=args.dpi,
        ),
        plot_metric_by_relationship(
            rows,
            output_dir,
            metric="avg_candidates",
            ylabel="Average candidates per query (log scale)",
            title=f"{dataset_label} Candidate Count",
            filename=f"{figure_prefix}_candidates.png",
            dpi=args.dpi,
        ),
        plot_metric_by_relationship(
            rows,
            output_dir,
            metric="avg_candidate_ratio",
            ylabel="Candidates / answers",
            title=f"{dataset_label} Candidate Amplification",
            filename=f"{figure_prefix}_candidate_ratio.png",
            dpi=args.dpi,
            use_log_y=False,
        ),
        plot_metric_by_relationship(
            rows,
            output_dir,
            metric="avg_answers",
            ylabel="Average answers per query (log scale)",
            title=f"{dataset_label} Answer Count",
            filename=f"{figure_prefix}_answers.png",
            dpi=args.dpi,
        ),
        plot_build_time(rows, output_dir, figure_prefix, args.dpi),
    ]
    figure_paths.extend(
        plot_probe_refine_breakdown(rows, output_dir, figure_prefix, args.dpi)
    )

    print_brief(rows)
    print("\nWrote figures:")
    for path in figure_paths:
        print(f"  {path}")


if __name__ == "__main__":
    main()
