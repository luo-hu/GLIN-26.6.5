#!/usr/bin/env python3
"""Plot GLIN-piecewise piece_limit sweep figures from a summary CSV."""

import argparse
import csv
import os
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", "figures/.matplotlib_cache")

import matplotlib.pyplot as plt


SELECTIVITY_ORDER = ["0.001%", "0.01%", "0.1%", "1%"]
SELECTIVITY_COLORS = {
    "0.001%": "#3B6EA8",
    "0.01%": "#70A37F",
    "0.1%": "#C46A4A",
    "1%": "#8C6BB1",
}
SELECTIVITY_MARKERS = {
    "0.001%": "o",
    "0.01%": "s",
    "0.1%": "^",
    "1%": "D",
}

REQUIRED_COLUMNS = {
    "selectivity",
    "dataset",
    "relationship",
    "loaded_count",
    "query_rows",
    "piece_limit",
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
        description="Plot GLIN-piecewise piece_limit sweep figures."
    )
    parser.add_argument(
        "--input",
        default="results/parks_1m_jts_strtree_knn_piece_limit_summary.csv",
        help="Input summary CSV.",
    )
    parser.add_argument(
        "--output_dir",
        default="figures",
        help="Output figure directory.",
    )
    parser.add_argument(
        "--figure_prefix",
        default="parks_1m_jts_strtree_knn_piece_limit",
        help="Output figure filename prefix.",
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
                "piece_limit",
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


def ordered_selectivities(rows):
    found = {row["selectivity"] for row in rows}
    ordered = [value for value in SELECTIVITY_ORDER if value in found]
    ordered.extend(sorted(found.difference(ordered)))
    return ordered


def rows_for_selectivity(rows, selectivity):
    selected = [row for row in rows if row["selectivity"] == selectivity]
    return sorted(selected, key=lambda row: row["piece_limit"])


def plot_metric(rows, output_dir, figure_prefix, metric, ylabel, filename,
                dpi, use_log_y=True):
    fig, ax = plt.subplots(figsize=(7.6, 4.8))

    for selectivity in ordered_selectivities(rows):
        cur = rows_for_selectivity(rows, selectivity)
        if not cur:
            continue
        x_values = [row["piece_limit"] for row in cur]
        y_values = [row[metric] for row in cur]
        ax.plot(
            x_values,
            y_values,
            label=selectivity,
            color=SELECTIVITY_COLORS.get(selectivity),
            marker=SELECTIVITY_MARKERS.get(selectivity, "o"),
            linewidth=2.0,
            markersize=6.0,
        )

    dataset = rows[0]["dataset"]
    relationship = rows[0]["relationship"]
    ax.set_title(f"{dataset} {relationship.capitalize()} GLIN-piecewise")
    ax.set_xlabel("piece_limit")
    ax.set_ylabel(ylabel)
    ax.set_xscale("log")
    if use_log_y:
        ax.set_yscale("log")
    ax.grid(axis="both", linestyle="--", alpha=0.35)
    ax.legend(title="Selectivity", frameon=False)
    fig.tight_layout()

    path = output_dir / f"{figure_prefix}_{filename}.png"
    fig.savefig(path, dpi=dpi)
    plt.close(fig)
    return path


def plot_probe_refine(rows, output_dir, figure_prefix, dpi):
    selectivities = ordered_selectivities(rows)
    fig, axes = plt.subplots(2, 2, figsize=(10.8, 7.4), sharex=True)
    axes = axes.flatten()

    for ax, selectivity in zip(axes, selectivities):
        cur = rows_for_selectivity(rows, selectivity)
        x_values = [row["piece_limit"] for row in cur]
        probe_us = [row["avg_probe_ns"] / 1000.0 for row in cur]
        refine_us = [row["avg_refine_ns"] / 1000.0 for row in cur]

        ax.plot(
            x_values,
            probe_us,
            label="Probe",
            color="#3B6EA8",
            marker="o",
            linewidth=2.0,
        )
        ax.plot(
            x_values,
            refine_us,
            label="Refine",
            color="#C46A4A",
            marker="s",
            linewidth=2.0,
        )
        ax.set_title(f"Selectivity {selectivity}")
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xlabel("piece_limit")
        ax.set_ylabel("Time (microseconds)")
        ax.grid(axis="both", linestyle="--", alpha=0.35)
        ax.legend(frameon=False)

    for ax in axes[len(selectivities):]:
        ax.axis("off")

    fig.suptitle(f"{rows[0]['dataset']} Probe vs Refine by piece_limit")
    fig.tight_layout()
    path = output_dir / f"{figure_prefix}_probe_refine.png"
    fig.savefig(path, dpi=dpi)
    plt.close(fig)
    return path


def print_best(rows):
    print("Best piece_limit by average total query time:")
    for selectivity in ordered_selectivities(rows):
        cur = rows_for_selectivity(rows, selectivity)
        best = min(cur, key=lambda row: row["avg_total_us"])
        print(
            f"  {selectivity}: PL={int(best['piece_limit'])} "
            f"total_us={best['avg_total_us']:.3f} "
            f"candidates={best['avg_candidates']:.2f} "
            f"ratio={best['avg_candidate_ratio']:.3f}"
        )


def main():
    args = parse_args()
    input_path = Path(args.input)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    rows = load_rows(input_path)
    figure_paths = [
        plot_metric(
            rows,
            output_dir,
            args.figure_prefix,
            "avg_probe_ns",
            "Average probe time (nanoseconds, log scale)",
            "probe_time",
            args.dpi,
        ),
        plot_metric(
            rows,
            output_dir,
            args.figure_prefix,
            "avg_total_us",
            "Average query time (microseconds, log scale)",
            "query_time",
            args.dpi,
        ),
        plot_metric(
            rows,
            output_dir,
            args.figure_prefix,
            "avg_candidates",
            "Average candidates per query (log scale)",
            "candidates",
            args.dpi,
        ),
        plot_metric(
            rows,
            output_dir,
            args.figure_prefix,
            "avg_candidate_ratio",
            "Candidates / answers",
            "candidate_ratio",
            args.dpi,
            use_log_y=False,
        ),
        plot_metric(
            rows,
            output_dir,
            args.figure_prefix,
            "build_ms",
            "Build time (milliseconds)",
            "build_time",
            args.dpi,
            use_log_y=False,
        ),
        plot_probe_refine(rows, output_dir, args.figure_prefix, args.dpi),
    ]

    print_best(rows)
    print("\nWrote figures:")
    for path in figure_paths:
        print(f"  {path}")


if __name__ == "__main__":
    main()
