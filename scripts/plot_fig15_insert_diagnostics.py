#!/usr/bin/env python3
"""Plot diagnostic charts for Fig.15 insertion anomalies."""

import argparse
import os
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", "figures/.matplotlib_cache")

import matplotlib.pyplot as plt
import pandas as pd


DATASET_ORDER = ["AW", "LW", "ROADS", "PARKS"]
INDEX_ORDER = [
    "GLIN",
    "GLIN_BUFFERED",
    "GLIN_LSM",
    "GLIN_LSM_ASYNC",
    "GLIN_PIECEWISE",
    "Boost_Rtree",
    "Boost_Rtree_linear",
    "Boost_Rtree_quadratic",
    "Boost_Rtree_rstar",
    "GEOS_Quadtree",
]
INDEX_LABELS = {
    "GLIN": "GLIN",
    "GLIN_BUFFERED": "GLIN-buffered",
    "GLIN_LSM": "GLIN-LSM",
    "GLIN_LSM_ASYNC": "GLIN-LSM-async",
    "GLIN_PIECEWISE": "GLIN-piecewise",
    "Boost_Rtree": "Boost linear",
    "Boost_Rtree_linear": "Boost linear",
    "Boost_Rtree_quadratic": "Boost quadratic",
    "Boost_Rtree_rstar": "Boost rstar",
    "GEOS_Quadtree": "GEOS Quadtree",
}
INDEX_COLORS = {
    "GLIN": "#3B6EA8",
    "GLIN_BUFFERED": "#2F9C95",
    "GLIN_LSM": "#5B8F2A",
    "GLIN_LSM_ASYNC": "#9A8F2A",
    "GLIN_PIECEWISE": "#70A37F",
    "Boost_Rtree": "#C46A4A",
    "Boost_Rtree_linear": "#C46A4A",
    "Boost_Rtree_quadratic": "#D89C45",
    "Boost_Rtree_rstar": "#B64A6A",
    "GEOS_Quadtree": "#8C6BB1",
}
MARKERS = {
    "GLIN": "o",
    "GLIN_BUFFERED": "X",
    "GLIN_LSM": "*",
    "GLIN_LSM_ASYNC": "h",
    "GLIN_PIECEWISE": "s",
    "Boost_Rtree": "^",
    "Boost_Rtree_linear": "^",
    "Boost_Rtree_quadratic": "v",
    "Boost_Rtree_rstar": "D",
    "GEOS_Quadtree": "P",
}
REQUIRED_COLUMNS = {
    "dataset",
    "index",
    "operation",
    "loaded_count",
    "insert_order",
    "boost_strategy",
    "cell_size",
    "throughput_ops_per_sec",
    "avg_update_ns",
    "failed_count",
}


def parse_args():
    parser = argparse.ArgumentParser(
        description="Plot insertion diagnostic sweep results."
    )
    parser.add_argument("--boost_csv", required=True)
    parser.add_argument("--order_csv", required=True)
    parser.add_argument("--cell_csv", required=True)
    parser.add_argument(
        "--output_dir",
        default="figures/fig15_insert_diagnostics",
    )
    parser.add_argument(
        "--summary_csv",
        default="results/fig15_insert_diagnostics/fig15_insert_diagnostics_summary.csv",
    )
    parser.add_argument("--dpi", type=int, default=180)
    return parser.parse_args()


def load_csv(path, experiment):
    path = Path(path)
    if str(path) == ".":
        raise ValueError(
            f"{experiment} CSV path is empty; pass an explicit CSV file path."
        )
    if not path.exists():
        raise FileNotFoundError(path)
    if path.is_dir():
        raise ValueError(f"{experiment} CSV path is a directory, not a file: {path}")
    df = pd.read_csv(path)
    missing = REQUIRED_COLUMNS.difference(df.columns)
    if missing:
        raise ValueError(f"{path} missing columns: {sorted(missing)}")
    df["experiment"] = experiment
    df["source_csv"] = str(path)
    for column in ["loaded_count", "cell_size", "throughput_ops_per_sec",
                   "avg_update_ns", "failed_count"]:
        df[column] = pd.to_numeric(df[column])
    return df


def ordered(values, preferred):
    found = list(dict.fromkeys(values))
    result = [value for value in preferred if value in found]
    result.extend(sorted(value for value in found if value not in result))
    return result


def plot_grouped_bars(df, x_column, x_order, index_order, title, xlabel, path, dpi):
    datasets = ordered(df["dataset"], DATASET_ORDER)
    fig, axes = plt.subplots(2, 2, figsize=(12.0, 8.0), sharey=False)
    axes = axes.ravel()
    handles = []
    labels = []

    for ax, dataset in zip(axes, datasets):
        sub = df[df["dataset"] == dataset]
        x_values = [value for value in x_order if value in set(sub[x_column].astype(str))]
        indexes = [idx for idx in index_order if idx in set(sub["index"])]
        if not x_values or not indexes:
            ax.axis("off")
            continue

        centers = list(range(len(x_values)))
        group_width = 0.78
        bar_width = group_width / len(indexes)
        for index_i, index in enumerate(indexes):
            xs = [
                center - group_width / 2 + bar_width / 2 + index_i * bar_width
                for center in centers
            ]
            ys = []
            for value in x_values:
                row = sub[(sub[x_column].astype(str) == value) & (sub["index"] == index)]
                ys.append(float(row["throughput_ops_per_sec"].iloc[0])
                          if not row.empty else float("nan"))
            bars = ax.bar(
                xs,
                ys,
                width=bar_width * 0.92,
                color=INDEX_COLORS.get(index, "#999999"),
                edgecolor="black",
                linewidth=0.45,
                label=INDEX_LABELS.get(index, index),
            )
            label = INDEX_LABELS.get(index, index)
            if label not in labels:
                handles.append(bars[0])
                labels.append(label)

        ax.set_title(dataset)
        ax.set_xlabel(xlabel)
        ax.set_ylabel("Throughput (Records/Sec)")
        ax.set_xticks(centers)
        ax.set_xticklabels(x_values, rotation=20, ha="right")
        ax.grid(axis="y", linestyle="--", alpha=0.35)

    for ax in axes[len(datasets):]:
        ax.axis("off")

    fig.suptitle(title, y=0.99)
    fig.legend(handles, labels, frameon=False, ncol=min(4, len(labels)),
               loc="upper center", bbox_to_anchor=(0.5, 0.945))
    fig.tight_layout(rect=(0, 0, 1, 0.92))
    fig.savefig(path, dpi=dpi)
    plt.close(fig)
    return path


def plot_cell_lines(df, path, dpi):
    datasets = ordered(df["dataset"], DATASET_ORDER)
    fig, axes = plt.subplots(2, 2, figsize=(12.0, 8.0), sharex=True)
    axes = axes.ravel()
    handles = []
    labels = []

    for ax, dataset in zip(axes, datasets):
        sub = df[df["dataset"] == dataset]
        for index in ["GLIN", "GLIN_BUFFERED", "GLIN_LSM",
                      "GLIN_LSM_ASYNC", "GLIN_PIECEWISE"]:
            cur = sub[sub["index"] == index].sort_values("cell_size")
            if cur.empty:
                continue
            line = ax.plot(
                cur["cell_size"],
                cur["throughput_ops_per_sec"],
                marker=MARKERS.get(index, "o"),
                linewidth=2.0,
                markersize=5.5,
                color=INDEX_COLORS.get(index),
                label=INDEX_LABELS.get(index, index),
            )[0]
            label = INDEX_LABELS.get(index, index)
            if label not in labels:
                handles.append(line)
                labels.append(label)
        ax.set_title(dataset)
        ax.set_xscale("log")
        ax.invert_xaxis()
        ax.set_xlabel("cell_size")
        ax.set_ylabel("Throughput (Records/Sec)")
        ax.grid(True, linestyle="--", alpha=0.35)

    for ax in axes[len(datasets):]:
        ax.axis("off")

    fig.suptitle("GLIN insertion throughput under cell_size sweep", y=0.99)
    fig.legend(handles, labels, frameon=False, ncol=5, loc="upper center",
               bbox_to_anchor=(0.5, 0.945))
    fig.tight_layout(rect=(0, 0, 1, 0.92))
    fig.savefig(path, dpi=dpi)
    plt.close(fig)
    return path


def main():
    args = parse_args()
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    Path(args.summary_csv).parent.mkdir(parents=True, exist_ok=True)

    boost = load_csv(args.boost_csv, "boost_strategy")
    order = load_csv(args.order_csv, "insert_order")
    cell = load_csv(args.cell_csv, "cell_size")
    summary = pd.concat([boost, order, cell], ignore_index=True)
    summary.to_csv(args.summary_csv, index=False)

    failed = summary[summary["failed_count"] != 0]
    if not failed.empty:
        print("Warning: failed update rows exist:")
        print(failed[["experiment", "dataset", "index", "failed_count"]].to_string(index=False))

    paths = []
    paths.append(
        plot_grouped_bars(
            boost[boost["operation"] == "insert"],
            "index",
            ["GLIN", "GLIN_BUFFERED", "GLIN_LSM", "GLIN_LSM_ASYNC",
             "Boost_Rtree_linear", "Boost_Rtree_quadratic",
             "Boost_Rtree_rstar", "GEOS_Quadtree"],
            ["GLIN", "GLIN_BUFFERED", "GLIN_LSM", "GLIN_LSM_ASYNC",
             "Boost_Rtree_linear", "Boost_Rtree_quadratic",
             "Boost_Rtree_rstar", "GEOS_Quadtree"],
            "Boost split strategy sweep, random insertion",
            "Index / split strategy",
            output_dir / "boost_split_strategy_sweep.png",
            args.dpi,
        )
    )
    paths.append(
        plot_grouped_bars(
            order[order["operation"] == "insert"],
            "insert_order",
            ["random", "file", "zmin"],
            ["GLIN", "GLIN_BUFFERED", "GLIN_LSM", "GLIN_LSM_ASYNC",
             "GLIN_PIECEWISE", "Boost_Rtree", "GEOS_Quadtree"],
            "Insertion order sweep",
            "Insertion order",
            output_dir / "insert_order_sweep.png",
            args.dpi,
        )
    )
    paths.append(
        plot_cell_lines(
            cell[cell["operation"] == "insert"],
            output_dir / "cell_size_sweep_glin.png",
            args.dpi,
        )
    )

    print(f"Wrote merged summary: {args.summary_csv}")
    for path in paths:
        print(f"Wrote figure: {path}")


if __name__ == "__main__":
    main()
