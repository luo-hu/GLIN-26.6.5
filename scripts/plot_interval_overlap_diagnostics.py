#!/usr/bin/env python3
"""Plot IntervalOverlapIndex formal query diagnostics."""

import argparse
import csv
import math
import os
import re
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", "figures/.matplotlib_cache")

import matplotlib.pyplot as plt


INDEX_ORDER = [
    "IO_BLOCK_MBR",
    "IO_OVERFLOW",
    "GLIN_PIECEWISE",
    "Boost_Rtree",
    "GEOS_Quadtree",
]
INDEX_LABELS = {
    "IntervalOverlapIndex": "IO-blockMBR",
    "IO_BLOCK_MBR": "IO-blockMBR",
    "IO_OVERFLOW": "IO-overflow",
    "GLIN_PIECEWISE": "GLIN-piecewise",
    "Boost_Rtree": "Boost R-tree",
    "GEOS_Quadtree": "GEOS Quadtree",
}
INDEX_COLORS = {
    "IntervalOverlapIndex": "#2F6F73",
    "IO_BLOCK_MBR": "#2F6F73",
    "IO_OVERFLOW": "#2D9CDB",
    "GLIN_PIECEWISE": "#6D8F3F",
    "Boost_Rtree": "#B86442",
    "GEOS_Quadtree": "#7C5FB3",
}
OVERFLOW_COLORS = {
    0.001: "#1F77B4",
    0.005: "#17A2A4",
    0.01: "#2CA02C",
    0.02: "#FF7F0E",
    0.05: "#D62728",
    0.10: "#9467BD",
}
IO_INDEXES = {"IntervalOverlapIndex", "IO_BLOCK_MBR", "IO_OVERFLOW"}
BLOCK_COLORS = {
    128: "#4E79A7",
    256: "#59A14F",
    512: "#F28E2B",
    1024: "#E15759",
    2048: "#76B7B2",
    4096: "#B07AA1",
}
BLOCK_MARKERS = {
    128: "o",
    256: "s",
    512: "^",
    1024: "D",
    2048: "P",
    4096: "X",
}
BLOCK_HATCHES = {
    128: "",
    256: "//",
    512: "\\\\",
    1024: "xx",
    2048: "..",
    4096: "++",
}
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
    "ZGAP_WIDE",
    "ZGAP_MIXED",
]

REQUIRED_COLUMNS = {
    "dataset",
    "index",
    "selectivity",
    "block_size",
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
    parser.add_argument(
        "--exclude_datasets",
        default="",
        help="Datasets to exclude, separated by comma or spaces. Example: ZGAP_WIDE",
    )
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
            block_size = row.get("block_size", "0") or "0"
            overflow_fraction = row.get("overflow_fraction", "0") or "0"
            parsed["plot_index"] = row["index"]
            if row["index"] == "IO_OVERFLOW":
                fraction_tag = str(float(overflow_fraction)).replace(".", "p")
                parsed["plot_index"] = (
                    f"IO_OVERFLOW f{fraction_tag} b{int(float(block_size))}"
                )
            elif row["index"] in IO_INDEXES:
                parsed["plot_index"] = f"{row['index']} b{int(float(block_size))}"
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


def split_names(value):
    if not value:
        return set()
    return {item for item in re.split(r"[,\s]+", value.strip()) if item}


def ordered_values(found, preferred):
    ordered = [value for value in preferred if value in found]
    ordered.extend(sorted(found.difference(ordered)))
    return ordered


def plot_index_order(value):
    if value.startswith("IntervalOverlapIndex") or value.startswith("IO_BLOCK_MBR") or value.startswith("IO_OVERFLOW"):
        match = re.search(r"b(\d+)", value)
        block_size = int(match.group(1)) if match else 0
        variant_order = 1 if value.startswith("IO_OVERFLOW") else 0
        fraction_match = re.search(r"f([0-9p]+)", value)
        fraction = (
            float(fraction_match.group(1).replace("p", "."))
            if fraction_match
            else 0.0
        )
        return (variant_order, fraction, block_size)
    base = value
    return (INDEX_ORDER.index(base) if base in INDEX_ORDER else len(INDEX_ORDER), value)


def find_row(rows, dataset, index):
    for row in rows:
        if row["dataset"] == dataset and row["plot_index"] == index:
            return row
    return None


def label_for_plot_index(index):
    if index.startswith("IntervalOverlapIndex b"):
        return index.replace("IntervalOverlapIndex", "IO-block")
    if index.startswith("IO_BLOCK_MBR b"):
        return index.replace("IO_BLOCK_MBR", "IO-block")
    if index.startswith("IO_OVERFLOW b"):
        return index.replace("IO_OVERFLOW", "IO-overflow")
    if index.startswith("IO_OVERFLOW f"):
        match = re.search(r"IO_OVERFLOW f([0-9p]+) b(\d+)", index)
        if match:
            fraction = match.group(1).replace("p", ".")
            return f"IO-overflow of={fraction} b{match.group(2)}"
        return index.replace("IO_OVERFLOW", "IO-overflow").replace(" f", " of=")
    return INDEX_LABELS.get(index, index)


def color_for_plot_index(index):
    if index.startswith("IntervalOverlapIndex") or index.startswith("IO_BLOCK_MBR") or index.startswith("IO_OVERFLOW"):
        match = re.search(r"b(\d+)", index)
        if match:
            if index.startswith("IO_OVERFLOW"):
                fraction = overflow_fraction_for_plot_index(index)
                return color_for_overflow_fraction(fraction)
            return BLOCK_COLORS.get(int(match.group(1)), INDEX_COLORS["IO_BLOCK_MBR"])
        return INDEX_COLORS["IO_BLOCK_MBR"]
    return INDEX_COLORS.get(index)


def overflow_fraction_for_plot_index(index):
    fraction_match = re.search(r"f([0-9p]+)", index)
    if not fraction_match:
        return 0.0
    return float(fraction_match.group(1).replace("p", "."))


def color_for_overflow_fraction(fraction):
    if fraction in OVERFLOW_COLORS:
        return OVERFLOW_COLORS[fraction]
    palette = [
        "#1F77B4",
        "#FF7F0E",
        "#2CA02C",
        "#D62728",
        "#9467BD",
        "#8C564B",
        "#E377C2",
        "#7F7F7F",
        "#BCBD22",
        "#17BECF",
    ]
    return palette[abs(hash(round(fraction, 6))) % len(palette)]


def hatch_for_plot_index(index):
    match = re.search(r"b(\d+)", index)
    if not match:
        return ""
    return BLOCK_HATCHES.get(int(match.group(1)), "")


def plot_grouped_bars(rows, output_dir, figure_prefix, selectivity, metric, ylabel, filename,
                      dpi, log_y=False):
    datasets = ordered_values({row["dataset"] for row in rows}, DATASET_ORDER)
    indexes = sorted({row["plot_index"] for row in rows}, key=plot_index_order)
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
            label=label_for_plot_index(index),
            color=color_for_plot_index(index),
            hatch=hatch_for_plot_index(index),
            edgecolor="black",
            linewidth=0.45,
        )

    ax.set_title(f"Intersects {selectivity}")
    ax.set_xticks(centers)
    ax.set_xticklabels(datasets)
    ax.set_ylabel(ylabel)
    ax.grid(axis="y", linestyle="--", alpha=0.32)
    if log_y:
        ax.set_yscale("log")
    ax.legend(frameon=False, ncol=min(3, len(indexes)))
    fig.tight_layout()

    tag = selectivity.replace("%", "pct").replace(".", "p")
    path = output_dir / f"{figure_prefix}_{tag}_{filename}.png"
    fig.savefig(path, dpi=dpi)
    plt.close(fig)
    return path


def block_size_of(row):
    return int(float(row["block_size"]))


def plot_block_sensitivity(rows, output_dir, figure_prefix, selectivity, dpi):
    interval_rows = [row for row in rows if row["index"] in IO_INDEXES]
    if not interval_rows:
        return None

    datasets = ordered_values({row["dataset"] for row in interval_rows}, DATASET_ORDER)
    fig, ax = plt.subplots(figsize=(max(8.0, len(datasets) * 1.35), 4.8))

    for dataset in datasets:
        dataset_rows = sorted(
            [row for row in interval_rows if row["dataset"] == dataset],
            key=block_size_of,
        )
        if not dataset_rows:
            continue
        x_values = [block_size_of(row) for row in dataset_rows]
        y_values = [row["avg_total_ms"] for row in dataset_rows]
        marker = BLOCK_MARKERS.get(x_values[0], "o")
        ax.plot(
            x_values,
            y_values,
            marker=marker,
            linewidth=1.8,
            markersize=5.5,
            label=dataset,
        )
        best_row = min(dataset_rows, key=lambda row: row["avg_total_ms"])
        ax.scatter(
            [block_size_of(best_row)],
            [best_row["avg_total_ms"]],
            s=72,
            edgecolor="black",
            linewidth=0.8,
            zorder=4,
        )

    ax.set_title(f"IO block sensitivity {selectivity}")
    ax.set_xlabel("Block size")
    ax.set_ylabel("Average query time (ms)")
    ax.grid(True, linestyle="--", alpha=0.32)
    ax.legend(frameon=False, ncol=min(3, len(datasets)))
    fig.tight_layout()

    tag = selectivity.replace("%", "pct").replace(".", "p")
    path = output_dir / f"{figure_prefix}_{tag}_block_sensitivity.png"
    fig.savefig(path, dpi=dpi)
    plt.close(fig)
    return path


def plot_interval_pruning(rows, output_dir, figure_prefix, selectivity, dpi):
    interval_rows = [row for row in rows if row["index"] in IO_INDEXES]
    if not interval_rows:
        return None

    interval_rows = sorted(
        interval_rows,
        key=lambda row: (
            DATASET_ORDER.index(row["dataset"])
            if row["dataset"] in DATASET_ORDER
            else len(DATASET_ORDER),
            int(float(row["block_size"])),
        ),
    )
    labels = [
        f"{row['dataset']}\n{label_for_plot_index(row['plot_index'])}" for row in interval_rows
    ]
    scanned = []
    prefix = []
    skipped = []
    for row in interval_rows:
        scanned.append(row["records_scanned"])
        prefix.append(row["prefix_records"])
        skipped.append(row["skipped_block_ratio"] * 100.0)

    fig, ax1 = plt.subplots(figsize=(max(8.0, len(labels) * 1.1), 4.8))
    centers = list(range(len(labels)))
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
    ax1.set_xticklabels(labels)
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

    tag = selectivity.replace("%", "pct").replace(".", "p")
    path = output_dir / f"{figure_prefix}_{tag}_pruning_detail.png"
    fig.savefig(path, dpi=dpi)
    plt.close(fig)
    return path


def write_diagnostics(rows, output_dir, figure_prefix):
    path = output_dir / f"{figure_prefix}_diagnostics.txt"
    selectivities = sorted({row["selectivity"] for row in rows})
    with path.open("w") as handle:
        for selectivity in selectivities:
            subset = [row for row in rows if row["selectivity"] == selectivity]
            datasets = ordered_values({row["dataset"] for row in subset}, DATASET_ORDER)
            handle.write(f"{selectivity}\n")
            for dataset in datasets:
                handle.write(f"  {dataset}\n")
                boost = find_row(subset, dataset, "Boost_Rtree")
                interval_candidates = [
                    row for row in subset
                    if row["dataset"] == dataset and row["index"] in IO_INDEXES
                ]
                interval = (
                    min(interval_candidates, key=lambda item: item["avg_total_ms"])
                    if interval_candidates
                    else None
                )
                glin = find_row(subset, dataset, "GLIN_PIECEWISE")
                if interval and boost:
                    speedup = boost["avg_total_ms"] / interval["avg_total_ms"]
                    handle.write(
                        f"    best IO block: b{block_size_of(interval)}\n"
                    )
                    handle.write(
                        f"    IO vs Boost_Rtree speedup: {speedup:.3f}x\n"
                    )
                if interval and glin:
                    speedup = glin["avg_total_ms"] / interval["avg_total_ms"]
                    handle.write(
                        f"    IO vs GLIN-piecewise speedup: {speedup:.3f}x\n"
                    )
                if interval:
                    handle.write(
                        "    IO candidate/answer ratio: "
                        f"{interval['candidate_answer_ratio']:.6f}\n"
                    )
                    handle.write(
                        "    IO skipped block ratio: "
                        f"{interval['skipped_block_ratio']:.3f}\n"
                    )
                    handle.write(
                        "    answers_match_boost: "
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
    excluded_datasets = split_names(args.exclude_datasets)
    if excluded_datasets:
        rows = [row for row in rows if row["dataset"] not in excluded_datasets]
        if not rows:
            raise ValueError(
                "No rows left after excluding datasets: "
                + ", ".join(sorted(excluded_datasets))
            )
    paths = []
    for selectivity in sorted({row["selectivity"] for row in rows}):
        subset = [row for row in rows if row["selectivity"] == selectivity]
        paths.extend(
            [
                plot_grouped_bars(
                    subset,
                    output_dir,
                    args.figure_prefix,
                    selectivity,
                    "avg_total_ms",
                    "Average query time (ms)",
                    "avg_total_ms",
                    args.dpi,
                ),
                plot_grouped_bars(
                    subset,
                    output_dir,
                    args.figure_prefix,
                    selectivity,
                    "candidate_answer_ratio",
                    "Candidate / answer ratio",
                    "candidate_answer_ratio",
                    args.dpi,
                ),
                plot_interval_pruning(
                    subset, output_dir, args.figure_prefix, selectivity, args.dpi
                ),
                plot_block_sensitivity(
                    subset, output_dir, args.figure_prefix, selectivity, args.dpi
                ),
            ]
        )
    paths.append(write_diagnostics(rows, output_dir, args.figure_prefix))

    for path in paths:
        if path:
            print(f"Wrote {path}")


if __name__ == "__main__":
    main()
