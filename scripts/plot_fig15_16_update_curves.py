#!/usr/bin/env python3
"""Plot Fig.15/Fig.16 style update throughput curves."""

import argparse
import os
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", "figures/.matplotlib_cache")

import matplotlib.pyplot as plt
import pandas as pd


DATASET_ORDER = ["AW", "LW", "ROADS", "PARKS"]
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
    "dataset",
    "index",
    "operation",
    "loaded_count",
    "checkpoint_update_count",
    "update_percent",
    "throughput_records_per_sec",
}


def parse_args():
    parser = argparse.ArgumentParser(
        description="Plot update throughput curves from --progress_csv outputs."
    )
    parser.add_argument(
        "--inputs",
        nargs="+",
        default=["results/fig15_16_1m_update_progress.csv"],
        help="Input progress CSV files.",
    )
    parser.add_argument(
        "--output_dir",
        default="figures/fig15_16_update_curves",
        help="Output figure directory.",
    )
    parser.add_argument(
        "--summary_csv",
        default="results/fig15_16_1m_update_progress_summary.csv",
        help="Merged progress summary CSV.",
    )
    parser.add_argument("--dpi", type=int, default=180)
    parser.add_argument(
        "--log_y",
        action="store_true",
        help="Use log-scale y axis.",
    )
    return parser.parse_args()


def ordered(values, preferred):
    found = list(dict.fromkeys(values))
    result = [value for value in preferred if value in found]
    result.extend(sorted(value for value in found if value not in result))
    return result


def load_results(paths):
    frames = []
    for raw_path in paths:
        path = Path(raw_path)
        if not path.exists():
            raise FileNotFoundError(f"Missing input CSV: {path}")
        frame = pd.read_csv(path)
        missing = REQUIRED_COLUMNS.difference(frame.columns)
        if missing:
            raise ValueError(f"{path} missing columns: {sorted(missing)}")
        frame["source_csv"] = str(path)
        frames.append(frame)
    df = pd.concat(frames, ignore_index=True)
    if df.empty:
        raise ValueError("No rows loaded from input CSVs")

    for column in [
        "loaded_count",
        "checkpoint_update_count",
        "update_percent",
        "throughput_records_per_sec",
    ]:
        df[column] = pd.to_numeric(df[column])
    return df


def plot_dataset_curve(df, dataset, operation, output_dir, dpi, log_y):
    sub = df[(df["dataset"] == dataset) & (df["operation"] == operation)].copy()
    if sub.empty:
        return None

    fig, ax = plt.subplots(figsize=(7.2, 4.8))
    for index in ordered(sub["index"], INDEX_ORDER):
        cur = sub[sub["index"] == index].sort_values("update_percent")
        if cur.empty:
            continue
        ax.plot(
            cur["update_percent"],
            cur["throughput_records_per_sec"],
            marker=INDEX_MARKERS.get(index, "o"),
            linewidth=2.0,
            markersize=5.5,
            color=INDEX_COLORS.get(index),
            label=INDEX_LABELS.get(index, index),
        )

    if operation == "insert":
        title = f"{dataset}: insertion throughput"
        xlabel = "Number of Inserted Records (%)"
        filename = f"fig15_{dataset.lower()}_insert_curve.png"
    else:
        title = f"{dataset}: deletion throughput"
        xlabel = "Number of Deleted Records (%)"
        filename = f"fig16_{dataset.lower()}_delete_curve.png"

    ax.set_title(title)
    ax.set_xlabel(xlabel)
    ax.set_ylabel("Throughput (Records/Sec)")
    ax.set_xlim(0, max(50.0, float(sub["update_percent"].max())))
    ax.set_xticks(sorted(sub["update_percent"].unique()))
    if log_y:
        ax.set_yscale("log")
    ax.grid(True, linestyle="--", alpha=0.35)
    ax.legend(frameon=False)
    fig.tight_layout()

    path = output_dir / filename
    fig.savefig(path, dpi=dpi)
    plt.close(fig)
    return path


def plot_panel(df, operation, output_dir, dpi, log_y):
    datasets = ordered(df[df["operation"] == operation]["dataset"], DATASET_ORDER)
    if not datasets:
        return None

    fig, axes = plt.subplots(2, 2, figsize=(12.0, 8.2), sharex=True)
    axes = axes.ravel()
    handles = []
    labels = []

    for ax, dataset in zip(axes, datasets):
        sub = df[(df["dataset"] == dataset) & (df["operation"] == operation)].copy()
        for index in ordered(sub["index"], INDEX_ORDER):
            cur = sub[sub["index"] == index].sort_values("update_percent")
            if cur.empty:
                continue
            line = ax.plot(
                cur["update_percent"],
                cur["throughput_records_per_sec"],
                marker=INDEX_MARKERS.get(index, "o"),
                linewidth=2.0,
                markersize=5.0,
                color=INDEX_COLORS.get(index),
                label=INDEX_LABELS.get(index, index),
            )[0]
            if INDEX_LABELS.get(index, index) not in labels:
                handles.append(line)
                labels.append(INDEX_LABELS.get(index, index))
        ax.set_title(dataset)
        ax.grid(True, linestyle="--", alpha=0.35)
        if log_y:
            ax.set_yscale("log")

    for ax in axes[len(datasets):]:
        ax.axis("off")

    if operation == "insert":
        xlabel = "Number of Inserted Records (%)"
        filename = "fig15_insert_curves_panel.png"
        title = "Fig.15-style insertion throughput curves"
    else:
        xlabel = "Number of Deleted Records (%)"
        filename = "fig16_delete_curves_panel.png"
        title = "Fig.16-style deletion throughput curves"

    for ax in axes:
        if ax.has_data():
            ax.set_xlabel(xlabel)
            ax.set_ylabel("Throughput (Records/Sec)")

    fig.suptitle(title, y=0.99)
    fig.legend(handles, labels, frameon=False, ncol=4, loc="upper center",
               bbox_to_anchor=(0.5, 0.945))
    fig.tight_layout(rect=(0, 0, 1, 0.92))
    path = output_dir / filename
    fig.savefig(path, dpi=dpi)
    plt.close(fig)
    return path


def main():
    args = parse_args()
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    Path(args.summary_csv).parent.mkdir(parents=True, exist_ok=True)

    df = load_results(args.inputs)
    df = df.sort_values(["operation", "dataset", "index", "update_percent"])
    df.to_csv(args.summary_csv, index=False)

    paths = []
    for operation in ["insert", "delete"]:
        paths.append(plot_panel(df, operation, output_dir, args.dpi, args.log_y))
        for dataset in ordered(df["dataset"], DATASET_ORDER):
            paths.append(plot_dataset_curve(df, dataset, operation, output_dir,
                                            args.dpi, args.log_y))

    print(f"Wrote merged progress summary: {args.summary_csv}")
    for path in paths:
        if path is not None:
            print(f"Wrote figure: {path}")


if __name__ == "__main__":
    main()
