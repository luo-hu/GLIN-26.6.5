#!/usr/bin/env python3
"""Plot all-dataset 1M benchmark summary figures."""

import argparse
import csv
import math
import os
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", "figures/.matplotlib_cache")

import matplotlib.pyplot as plt


SELECTIVITY_ORDER = ["0.001%", "0.01%", "0.1%", "1%"]
DATASET_ORDER = [
    "AW",
    "LW",
    "ROADS",
    "PARKS",
    "OSM_AU_POINTS",
    "UNIF_S",
    "UNIF_L",
    "DIAG_S",
    "DIAG_L",
]

RELATIONSHIP_INDEXES = {
    "contains": ["GLIN", "Boost_Rtree", "GEOS_Quadtree"],
    "intersects": ["GLIN_PIECEWISE", "Boost_Rtree", "GEOS_Quadtree"],
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
    "avg_candidate_ratio",
}


def parse_args():
    parser = argparse.ArgumentParser(
        description="Plot all-dataset 1M benchmark summary figures."
    )
    parser.add_argument(
        "--input",
        default="results/all_1m_summary.csv",
        help="Input summary CSV. Default: results/all_1m_summary.csv",
    )
    parser.add_argument(
        "--output_dir",
        default="figures/all_1m",
        help="Output figure directory. Default: figures/all_1m",
    )
    parser.add_argument(
        "--figure_prefix",
        default="all_1m",
        help="Output figure filename prefix. Default: all_1m",
    )
    parser.add_argument("--dpi", type=int, default=180, help="Output image DPI.")
    return parser.parse_args()


def load_rows(path):
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
                "avg_candidate_ratio",
            ]:
                parsed[column] = float(row[column])
            rows.append(parsed)

    if not rows:
        raise ValueError(f"No rows found in {path}")
    return rows


def ordered_datasets(rows):
    found = {row["dataset"] for row in rows}
    ordered = [value for value in DATASET_ORDER if value in found]
    ordered.extend(sorted(found.difference(ordered)))
    return ordered


def row_lookup(rows, relationship, selectivity, dataset, index):
    for row in rows:
        if (
            row["relationship"] == relationship
            and row["selectivity"] == selectivity
            and row["dataset"] == dataset
            and row["index"] == index
        ):
            return row
    return None


def plot_metric_by_selectivity(rows, output_dir, figure_prefix, relationship,
                               metric, ylabel, filename, dpi, use_log_y=True):
    selectivities = [
        value for value in SELECTIVITY_ORDER
        if any(row["selectivity"] == value and row["relationship"] == relationship
               for row in rows)
    ]
    datasets = ordered_datasets([row for row in rows if row["relationship"] == relationship])
    indexes = [
        index for index in RELATIONSHIP_INDEXES[relationship]
        if any(row["index"] == index and row["relationship"] == relationship for row in rows)
    ]
    if not selectivities or not datasets or not indexes:
        return []

    paths = []
    for selectivity in selectivities:
        fig, ax = plt.subplots(figsize=(max(10.0, len(datasets) * 0.9), 5.2))
        x_centers = list(range(len(datasets)))
        group_width = 0.78
        bar_width = group_width / len(indexes)

        for index_i, index in enumerate(indexes):
            x_values = [
                center - group_width / 2 + bar_width / 2 + index_i * bar_width
                for center in x_centers
            ]
            y_values = []
            for dataset in datasets:
                row = row_lookup(rows, relationship, selectivity, dataset, index)
                y_values.append(row[metric] if row else math.nan)
            ax.bar(
                x_values,
                y_values,
                width=bar_width * 0.92,
                label=INDEX_LABELS.get(index, index),
                color=INDEX_COLORS.get(index),
                edgecolor="black",
                linewidth=0.45,
            )

        ax.set_title(f"{relationship.capitalize()} {selectivity}")
        ax.set_xlabel("Dataset")
        ax.set_ylabel(ylabel)
        ax.set_xticks(x_centers)
        ax.set_xticklabels(datasets, rotation=25, ha="right")
        if use_log_y:
            ax.set_yscale("log")
        ax.grid(axis="y", linestyle="--", alpha=0.35)
        ax.legend(frameon=False, ncol=min(3, len(indexes)))
        fig.tight_layout()

        path = output_dir / f"{figure_prefix}_{relationship}_{filename}_{selectivity.replace('%', 'pct').replace('.', 'p')}.png"
        fig.savefig(path, dpi=dpi)
        plt.close(fig)
        paths.append(path)

    return paths


def plot_build_time(rows, output_dir, figure_prefix, relationship, dpi):
    datasets = ordered_datasets([row for row in rows if row["relationship"] == relationship])
    indexes = [
        index for index in RELATIONSHIP_INDEXES[relationship]
        if any(row["index"] == index and row["relationship"] == relationship for row in rows)
    ]
    if not datasets or not indexes:
        return None

    grouped = {}
    for row in rows:
        if row["relationship"] != relationship:
            continue
        grouped.setdefault((row["dataset"], row["index"]), []).append(row["build_ms"])

    fig, ax = plt.subplots(figsize=(max(10.0, len(datasets) * 0.9), 5.2))
    x_centers = list(range(len(datasets)))
    group_width = 0.78
    bar_width = group_width / len(indexes)

    for index_i, index in enumerate(indexes):
        x_values = [
            center - group_width / 2 + bar_width / 2 + index_i * bar_width
            for center in x_centers
        ]
        y_values = []
        for dataset in datasets:
            values = grouped.get((dataset, index), [])
            y_values.append(sum(values) / len(values) if values else math.nan)
        ax.bar(
            x_values,
            y_values,
            width=bar_width * 0.92,
            label=INDEX_LABELS.get(index, index),
            color=INDEX_COLORS.get(index),
            edgecolor="black",
            linewidth=0.45,
        )

    ax.set_title(f"{relationship.capitalize()} average build time")
    ax.set_xlabel("Dataset")
    ax.set_ylabel("Build time (milliseconds)")
    ax.set_xticks(x_centers)
    ax.set_xticklabels(datasets, rotation=25, ha="right")
    ax.grid(axis="y", linestyle="--", alpha=0.35)
    ax.legend(frameon=False, ncol=min(3, len(indexes)))
    fig.tight_layout()

    path = output_dir / f"{figure_prefix}_{relationship}_build_time.png"
    fig.savefig(path, dpi=dpi)
    plt.close(fig)
    return path


def main():
    args = parse_args()
    input_path = Path(args.input)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    rows = load_rows(input_path)
    figure_paths = []
    for relationship in ["contains", "intersects"]:
        figure_paths.extend(
            plot_metric_by_selectivity(
                rows,
                output_dir,
                args.figure_prefix,
                relationship,
                "avg_total_us",
                "Average query time (microseconds, log scale)",
                "query_time",
                args.dpi,
            )
        )
        figure_paths.extend(
            plot_metric_by_selectivity(
                rows,
                output_dir,
                args.figure_prefix,
                relationship,
                "avg_candidates",
                "Average candidates per query (log scale)",
                "candidates",
                args.dpi,
            )
        )
        figure_paths.extend(
            plot_metric_by_selectivity(
                rows,
                output_dir,
                args.figure_prefix,
                relationship,
                "avg_candidate_ratio",
                "Candidates / answers",
                "candidate_ratio",
                args.dpi,
                use_log_y=False,
            )
        )
        build_path = plot_build_time(rows, output_dir, args.figure_prefix, relationship, args.dpi)
        if build_path is not None:
            figure_paths.append(build_path)

    print("Wrote figures:")
    for path in figure_paths:
        print(f"  {path}")


if __name__ == "__main__":
    main()
