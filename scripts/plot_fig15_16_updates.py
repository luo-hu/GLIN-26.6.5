#!/usr/bin/env python3
"""Plot Fig.15/Fig.16 style update-throughput charts."""

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
REQUIRED_COLUMNS = {
    "dataset",
    "index",
    "operation",
    "loaded_count",
    "initial_count",
    "update_count",
    "avg_update_ns",
    "throughput_ops_per_sec",
    "success_count",
    "failed_count",
}


def parse_args():
    parser = argparse.ArgumentParser(
        description="Plot GLIN Fig.15/Fig.16 update benchmark results."
    )
    parser.add_argument(
        "--inputs",
        nargs="+",
        default=[
            "results/fig15_16_aw1m_updates.csv",
            "results/fig15_16_lw1m_updates.csv",
            "results/fig15_16_roads1m_updates.csv",
            "results/fig15_16_parks1m_updates.csv",
        ],
        help="Input update CSV files.",
    )
    parser.add_argument(
        "--output_dir",
        default="figures/fig15_16_updates",
        help="Output figure directory.",
    )
    parser.add_argument(
        "--summary_csv",
        default="results/fig15_16_1m_updates_summary.csv",
        help="Merged summary CSV path.",
    )
    parser.add_argument("--dpi", type=int, default=180)
    parser.add_argument(
        "--log_y",
        action="store_true",
        help="Use log-scale y axis for throughput/latency charts.",
    )
    return parser.parse_args()


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
        "initial_count",
        "update_count",
        "avg_update_ns",
        "throughput_ops_per_sec",
        "success_count",
        "failed_count",
    ]:
        df[column] = pd.to_numeric(df[column])

    return df


def ordered_values(values, preferred):
    found = list(dict.fromkeys(values))
    ordered = [value for value in preferred if value in found]
    ordered.extend(sorted(value for value in found if value not in ordered))
    return ordered


def plot_grouped_bars(df, operation, metric, ylabel, title, path, log_y, dpi):
    sub = df[df["operation"] == operation].copy()
    datasets = ordered_values(sub["dataset"], DATASET_ORDER)
    indexes = ordered_values(sub["index"], INDEX_ORDER)
    if not datasets or not indexes:
        return None

    fig, ax = plt.subplots(figsize=(max(9.0, len(datasets) * 1.35), 5.2))
    group_width = 0.78
    bar_width = group_width / len(indexes)
    x_centers = list(range(len(datasets)))

    for index_i, index in enumerate(indexes):
        x_values = [
            center - group_width / 2 + bar_width / 2 + index_i * bar_width
            for center in x_centers
        ]
        y_values = []
        for dataset in datasets:
            rows = sub[(sub["dataset"] == dataset) & (sub["index"] == index)]
            y_values.append(float(rows[metric].iloc[0]) if not rows.empty else float("nan"))

        ax.bar(
            x_values,
            y_values,
            width=bar_width * 0.92,
            label=INDEX_LABELS.get(index, index),
            color=INDEX_COLORS.get(index, "#999999"),
            edgecolor="black",
            linewidth=0.45,
        )

    ax.set_title(title)
    ax.set_xlabel("Dataset")
    ax.set_ylabel(ylabel)
    ax.set_xticks(x_centers)
    ax.set_xticklabels(datasets)
    if log_y:
        ax.set_yscale("log")
    ax.grid(axis="y", linestyle="--", alpha=0.35)
    ax.legend(frameon=False, ncol=2)
    fig.tight_layout()
    fig.savefig(path, dpi=dpi)
    plt.close(fig)
    return path


def plot_speedup_vs_boost(df, operation, path, dpi):
    sub = df[df["operation"] == operation].copy()
    rows = []
    for dataset in ordered_values(sub["dataset"], DATASET_ORDER):
        boost = sub[
            (sub["dataset"] == dataset) & (sub["index"] == "Boost_Rtree")
        ]
        if boost.empty:
            continue
        boost_throughput = float(boost["throughput_ops_per_sec"].iloc[0])
        for index in ["GLIN", "GLIN_PIECEWISE", "GEOS_Quadtree"]:
            cur = sub[(sub["dataset"] == dataset) & (sub["index"] == index)]
            if cur.empty:
                continue
            rows.append(
                {
                    "dataset": dataset,
                    "index": index,
                    "speedup": float(cur["throughput_ops_per_sec"].iloc[0])
                    / boost_throughput,
                }
            )

    speedup = pd.DataFrame(rows)
    if speedup.empty:
        return None

    datasets = ordered_values(speedup["dataset"], DATASET_ORDER)
    indexes = ordered_values(speedup["index"], ["GLIN", "GLIN_PIECEWISE", "GEOS_Quadtree"])
    fig, ax = plt.subplots(figsize=(max(9.0, len(datasets) * 1.35), 4.8))
    group_width = 0.72
    bar_width = group_width / len(indexes)
    x_centers = list(range(len(datasets)))

    for index_i, index in enumerate(indexes):
        x_values = [
            center - group_width / 2 + bar_width / 2 + index_i * bar_width
            for center in x_centers
        ]
        y_values = []
        for dataset in datasets:
            cur = speedup[(speedup["dataset"] == dataset) & (speedup["index"] == index)]
            y_values.append(float(cur["speedup"].iloc[0]) if not cur.empty else float("nan"))
        ax.bar(
            x_values,
            y_values,
            width=bar_width * 0.92,
            label=INDEX_LABELS.get(index, index),
            color=INDEX_COLORS.get(index, "#999999"),
            edgecolor="black",
            linewidth=0.45,
        )

    ax.axhline(1.0, color="black", linewidth=1.0, linestyle="--", label="Boost R-tree")
    ax.set_title(f"{operation.capitalize()} throughput relative to Boost R-tree")
    ax.set_xlabel("Dataset")
    ax.set_ylabel("Speedup over Boost R-tree")
    ax.set_xticks(x_centers)
    ax.set_xticklabels(datasets)
    ax.grid(axis="y", linestyle="--", alpha=0.35)
    ax.legend(frameon=False, ncol=2)
    fig.tight_layout()
    fig.savefig(path, dpi=dpi)
    plt.close(fig)
    return path


def main():
    args = parse_args()
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    Path(args.summary_csv).parent.mkdir(parents=True, exist_ok=True)

    df = load_results(args.inputs)
    df = df.sort_values(["operation", "dataset", "index"]).reset_index(drop=True)
    df.to_csv(args.summary_csv, index=False)

    bad_rows = df[df["failed_count"] != 0]
    if not bad_rows.empty:
        print("Warning: some update rows have failed_count != 0")
        print(bad_rows[["dataset", "index", "operation", "failed_count"]].to_string(index=False))

    paths = []
    paths.append(
        plot_grouped_bars(
            df,
            "insert",
            "throughput_ops_per_sec",
            "Throughput (ops/sec)",
            "Fig.15-style insertion throughput, 1M records",
            output_dir / "fig15_insert_throughput.png",
            args.log_y,
            args.dpi,
        )
    )
    paths.append(
        plot_grouped_bars(
            df,
            "delete",
            "throughput_ops_per_sec",
            "Throughput (ops/sec)",
            "Fig.16-style deletion throughput, 1M records",
            output_dir / "fig16_delete_throughput.png",
            args.log_y,
            args.dpi,
        )
    )
    paths.append(
        plot_grouped_bars(
            df,
            "insert",
            "avg_update_ns",
            "Average update time (ns/op)",
            "Insertion latency, 1M records",
            output_dir / "fig15_insert_avg_update_ns.png",
            args.log_y,
            args.dpi,
        )
    )
    paths.append(
        plot_grouped_bars(
            df,
            "delete",
            "avg_update_ns",
            "Average update time (ns/op)",
            "Deletion latency, 1M records",
            output_dir / "fig16_delete_avg_update_ns.png",
            args.log_y,
            args.dpi,
        )
    )
    paths.append(
        plot_speedup_vs_boost(
            df,
            "insert",
            output_dir / "fig15_insert_speedup_vs_boost.png",
            args.dpi,
        )
    )
    paths.append(
        plot_speedup_vs_boost(
            df,
            "delete",
            output_dir / "fig16_delete_speedup_vs_boost.png",
            args.dpi,
        )
    )

    print(f"Wrote merged summary: {args.summary_csv}")
    for path in paths:
        if path is not None:
            print(f"Wrote figure: {path}")


if __name__ == "__main__":
    main()
