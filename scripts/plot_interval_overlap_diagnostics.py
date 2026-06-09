#!/usr/bin/env python3
"""Plot IntervalOverlapIndex formal query diagnostics."""

import argparse
import csv
import math
import os
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", "figures/.matplotlib_cache")

import matplotlib.pyplot as plt


INDEX_ORDER = ["IntervalOverlapIndex", "GLIN_PIECEWISE", "Boost_Rtree"]
INDEX_LABELS = {
    "IntervalOverlapIndex": "IntervalOverlapIndex",
    "GLIN_PIECEWISE": "GLIN-piecewise",
    "Boost_Rtree": "Boost R-tree",
}
INDEX_COLORS = {
    "IntervalOverlapIndex": "#2F6F73",
    "GLIN_PIECEWISE": "#6D8F3F",
    "Boost_Rtree": "#B86442",
}
DATASET_ORDER = ["ROADS", "PARKS", "AW", "LW"]

REQUIRED_COLUMNS = {
    "dataset",
    "index",
    "avg_total_ns",
    "candidate_answer_ratio",
    "records_scanned",
    "prefix_records",
    "skipped_block_ratio",
    "answers_match_boost",
}


def parse_args():
    parser = argparse.ArgumentParser(
        description="Plot IntervalOverlapIndex, GLIN-piecewise, and Boost R-tree diagnostics."
    )
    parser.add_argument(
        "--input",
        default="results/interval_overlap_2000000/interval_overlap_summary.csv",
        help="Input summary CSV.",
    )
    parser.add_argument(
        "--output_dir",
        default="figures/interval_overlap_2000000",
        help="Output figure directory.",
    )
    parser.add_argument(
        "--figure_prefix",
        default="interval_overlap",
        help="Output figure filename prefix.",
    )
    parser.add_argument("--dpi", type=int, default=180, help="Output image DPI.")
    return parser.parse_args()


def as_float(row, column):
    value = row.get(column, "")
    if value == "":
        return math.nan
    return float(value)


def load_rows(path):
    with path.open(newline="") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            raise ValueError(f"Empty CSV: {path}")
        missing = REQUIRED_COLUMNS.difference(reader.fieldnames)
        if missing:
            raise ValueError(f"{path} missing columns: {', '.join(sorted(missing))}")

        rows = []
        for row in reader:
            parsed = dict(row)
            parsed["avg_total_ms"] = as_float(row, "avg_total_ns") / 1e6
            parsed["candidate_answer_ratio"] = as_float(
                row, "candidate_answer_ratio"
            )
            parsed["records_scanned"] = as_float(row, "records_scanned")
            parsed["prefix_records"] = as_float(row, "prefix_records")
            parsed["skipped_block_ratio"] = as_float(row, "skipped_block_ratio")
            parsed["answers_match_boost"] = as_float(row, "answers_match_boost")
            rows.append(parsed)

    if not rows:
        raise ValueError(f"No rows found in {path}")
    return rows


def ordered_values(found, preferred):
    ordered = [value for value in preferred if value in found]
    ordered.extend(sorted(found.difference(ordered)))
    return ordered


def find_row(rows, dataset, index):
    for row in rows:
        if row["dataset"] == dataset and row["index"] == index:
            return row
    return None


def plot_grouped_bars(rows, output_dir, figure_prefix, metric, ylabel, filename,
                      dpi, log_y=False):
    datasets = ordered_values({row["dataset"] for row in rows}, DATASET_ORDER)
    indexes = ordered_values({row["index"] for row in rows}, INDEX_ORDER)
    fig, ax = plt.subplots(figsize=(max(8.0, len(datasets) * 1.6), 4.8))
    group_width = 0.78
    bar_width = group_width / max(1, len(indexes))
    centers = list(range(len(datasets)))

    for index_i, index in enumerate(indexes):
        x_values = [
            center - group_width / 2 + bar_width / 2 + index_i * bar_width
            for center in centers
        ]
        y_values = []
        for dataset in datasets:
            row = find_row(rows, dataset, index)
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

    ax.set_xticks(centers)
    ax.set_xticklabels(datasets)
    ax.set_ylabel(ylabel)
    ax.grid(axis="y", linestyle="--", alpha=0.32)
    if log_y:
        ax.set_yscale("log")
    ax.legend(frameon=False, ncol=min(3, len(indexes)))
    fig.tight_layout()

    path = output_dir / f"{figure_prefix}_{filename}.png"
    fig.savefig(path, dpi=dpi)
    plt.close(fig)
    return path


def plot_interval_pruning(rows, output_dir, figure_prefix, dpi):
    interval_rows = [row for row in rows if row["index"] == "IntervalOverlapIndex"]
    if not interval_rows:
        return None

    datasets = ordered_values({row["dataset"] for row in interval_rows}, DATASET_ORDER)
    scanned = []
    prefix = []
    skipped = []
    for dataset in datasets:
        row = find_row(interval_rows, dataset, "IntervalOverlapIndex")
        scanned.append(row["records_scanned"])
        prefix.append(row["prefix_records"])
        skipped.append(row["skipped_block_ratio"] * 100.0)

    fig, ax1 = plt.subplots(figsize=(max(8.0, len(datasets) * 1.6), 4.8))
    centers = list(range(len(datasets)))
    ax1.bar(
        [value - 0.2 for value in centers],
        prefix,
        width=0.36,
        label="Prefix records before block pruning",
        color="#9CA3AF",
        edgecolor="black",
        linewidth=0.45,
    )
    ax1.bar(
        [value + 0.2 for value in centers],
        scanned,
        width=0.36,
        label="Records scanned after pruning",
        color=INDEX_COLORS["IntervalOverlapIndex"],
        edgecolor="black",
        linewidth=0.45,
    )
    ax1.set_yscale("log")
    ax1.set_ylabel("Records, log scale")
    ax1.set_xticks(centers)
    ax1.set_xticklabels(datasets)
    ax1.grid(axis="y", linestyle="--", alpha=0.32)

    ax2 = ax1.twinx()
    ax2.plot(
        centers,
        skipped,
        color="#222222",
        marker="o",
        linewidth=1.8,
        label="Skipped block ratio",
    )
    ax2.set_ylabel("Skipped blocks (%)")
    ax2.set_ylim(0, 105)

    handles1, labels1 = ax1.get_legend_handles_labels()
    handles2, labels2 = ax2.get_legend_handles_labels()
    ax1.legend(handles1 + handles2, labels1 + labels2, frameon=False, loc="upper left")
    fig.tight_layout()

    path = output_dir / f"{figure_prefix}_pruning_detail.png"
    fig.savefig(path, dpi=dpi)
    plt.close(fig)
    return path


def write_diagnostics(rows, output_dir, figure_prefix):
    path = output_dir / f"{figure_prefix}_diagnostics.txt"
    datasets = ordered_values({row["dataset"] for row in rows}, DATASET_ORDER)
    with path.open("w") as handle:
        for dataset in datasets:
            handle.write(f"{dataset}\n")
            boost = find_row(rows, dataset, "Boost_Rtree")
            interval = find_row(rows, dataset, "IntervalOverlapIndex")
            glin = find_row(rows, dataset, "GLIN_PIECEWISE")
            if interval and boost:
                speedup = boost["avg_total_ms"] / interval["avg_total_ms"]
                handle.write(
                    f"  IntervalOverlapIndex vs Boost_Rtree speedup: {speedup:.3f}x\n"
                )
            if interval and glin:
                speedup = glin["avg_total_ms"] / interval["avg_total_ms"]
                handle.write(
                    f"  IntervalOverlapIndex vs GLIN-piecewise speedup: {speedup:.3f}x\n"
                )
            if interval:
                handle.write(
                    "  IntervalOverlapIndex candidate/answer ratio: "
                    f"{interval['candidate_answer_ratio']:.6f}\n"
                )
                handle.write(
                    "  IntervalOverlapIndex skipped block ratio: "
                    f"{interval['skipped_block_ratio']:.3f}\n"
                )
                handle.write(
                    "  answers_match_boost: "
                    f"{int(interval['answers_match_boost'])}\n"
                )
            handle.write("\n")
    return path


def main():
    args = parse_args()
    input_path = Path(args.input)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    rows = load_rows(input_path)
    paths = [
        plot_grouped_bars(
            rows,
            output_dir,
            args.figure_prefix,
            "avg_total_ms",
            "Average query time (ms)",
            "avg_total_ms",
            args.dpi,
        ),
        plot_grouped_bars(
            rows,
            output_dir,
            args.figure_prefix,
            "candidate_answer_ratio",
            "Candidate / answer ratio",
            "candidate_answer_ratio",
            args.dpi,
        ),
        plot_interval_pruning(rows, output_dir, args.figure_prefix, args.dpi),
        write_diagnostics(rows, output_dir, args.figure_prefix),
    ]

    for path in paths:
        if path:
            print(f"Wrote {path}")


if __name__ == "__main__":
    main()
