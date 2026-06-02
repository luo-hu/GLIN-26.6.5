#!/usr/bin/env python3
"""Plot GLIN-piecewise piece_limit sweep against Boost R-tree Intersects."""

import argparse
import csv
import math
import os
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", "figures/.matplotlib_cache")

import matplotlib.pyplot as plt


SELECTIVITY_ORDER = ["0.001%", "0.01%", "0.1%", "1%"]
PIECE_LIMIT_ORDER = [100, 1000, 10000, 100000]
SERIES_ORDER = ["Boost R-tree"] + [f"PL={value}" for value in PIECE_LIMIT_ORDER]

SERIES_COLORS = {
    "Boost R-tree": "#C46A4A",
    "PL=100": "#3B6EA8",
    "PL=1000": "#70A37F",
    "PL=10000": "#8C6BB1",
    "PL=100000": "#D9A441",
}

REQUIRED_PIECE_COLUMNS = {
    "selectivity",
    "dataset",
    "relationship",
    "loaded_count",
    "piece_limit",
    "avg_probe_ns",
    "avg_total_us",
    "avg_candidates",
    "avg_answers",
}

REQUIRED_BASELINE_COLUMNS = {
    "selectivity",
    "dataset",
    "index",
    "relationship",
    "loaded_count",
    "avg_probe_ns",
    "avg_total_us",
    "avg_candidates",
    "avg_answers",
}


def parse_args():
    parser = argparse.ArgumentParser(
        description="Plot GLIN-piecewise PL sweep against Boost R-tree Intersects."
    )
    parser.add_argument(
        "--piece_summary",
        default="results/parks_1m_jts_strtree_knn_piece_limit_summary.csv",
        help="Input GLIN-piecewise piece_limit summary CSV.",
    )
    parser.add_argument(
        "--baseline_summary",
        default="results/parks_1m_jts_strtree_knn_summary.csv",
        help="Input summary CSV containing Boost_Rtree intersects rows.",
    )
    parser.add_argument(
        "--output_dir",
        default="figures",
        help="Output figure directory.",
    )
    parser.add_argument(
        "--figure_prefix",
        default="parks_1m_jts_strtree_knn_pl_vs_boost",
        help="Output figure filename prefix.",
    )
    parser.add_argument(
        "--combined_csv",
        default="results/parks_1m_jts_strtree_knn_pl_vs_boost_intersects.csv",
        help="Output combined comparison CSV.",
    )
    parser.add_argument("--dpi", type=int, default=180, help="Output image DPI.")
    return parser.parse_args()


def require_columns(reader, required, path):
    if reader.fieldnames is None:
        raise ValueError(f"Empty CSV: {path}")
    missing = required.difference(reader.fieldnames)
    if missing:
        raise ValueError(f"{path} missing columns: {', '.join(sorted(missing))}")


def load_piece_rows(path):
    rows = []
    with path.open(newline="") as f:
        reader = csv.DictReader(f)
        require_columns(reader, REQUIRED_PIECE_COLUMNS, path)
        for row in reader:
            if row["relationship"] != "intersects":
                continue
            rows.append(
                {
                    "selectivity": row["selectivity"],
                    "dataset": row["dataset"],
                    "relationship": row["relationship"],
                    "loaded_count": int(float(row["loaded_count"])),
                    "series": f"PL={int(float(row['piece_limit']))}",
                    "piece_limit": int(float(row["piece_limit"])),
                    "avg_probe_ns": float(row["avg_probe_ns"]),
                    "avg_total_us": float(row["avg_total_us"]),
                    "avg_candidates": float(row["avg_candidates"]),
                    "avg_answers": float(row["avg_answers"]),
                }
            )
    return rows


def load_boost_rows(path):
    rows = []
    with path.open(newline="") as f:
        reader = csv.DictReader(f)
        require_columns(reader, REQUIRED_BASELINE_COLUMNS, path)
        for row in reader:
            if row["relationship"] != "intersects" or row["index"] != "Boost_Rtree":
                continue
            rows.append(
                {
                    "selectivity": row["selectivity"],
                    "dataset": row["dataset"],
                    "relationship": row["relationship"],
                    "loaded_count": int(float(row["loaded_count"])),
                    "series": "Boost R-tree",
                    "piece_limit": 0,
                    "avg_probe_ns": float(row["avg_probe_ns"]),
                    "avg_total_us": float(row["avg_total_us"]),
                    "avg_candidates": float(row["avg_candidates"]),
                    "avg_answers": float(row["avg_answers"]),
                }
            )
    return rows


def sort_key(row):
    selectivity = row["selectivity"]
    return (
        SELECTIVITY_ORDER.index(selectivity)
        if selectivity in SELECTIVITY_ORDER
        else len(SELECTIVITY_ORDER),
        SERIES_ORDER.index(row["series"])
        if row["series"] in SERIES_ORDER
        else len(SERIES_ORDER),
    )


def write_combined_csv(path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = [
        "selectivity",
        "dataset",
        "relationship",
        "loaded_count",
        "series",
        "piece_limit",
        "avg_probe_ns",
        "avg_total_us",
        "avg_candidates",
        "avg_answers",
        "candidate_ratio",
        "total_vs_boost",
        "probe_vs_boost",
    ]

    boost_by_selectivity = {
        row["selectivity"]: row for row in rows if row["series"] == "Boost R-tree"
    }
    output_rows = []
    for row in rows:
        parsed = dict(row)
        parsed["candidate_ratio"] = (
            parsed["avg_candidates"] / parsed["avg_answers"]
            if parsed["avg_answers"] > 0
            else 0.0
        )
        boost = boost_by_selectivity.get(parsed["selectivity"])
        if boost:
            parsed["total_vs_boost"] = parsed["avg_total_us"] / boost["avg_total_us"]
            parsed["probe_vs_boost"] = parsed["avg_probe_ns"] / boost["avg_probe_ns"]
        else:
            parsed["total_vs_boost"] = math.nan
            parsed["probe_vs_boost"] = math.nan
        output_rows.append(parsed)

    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(output_rows)


def lookup(rows, selectivity, series):
    for row in rows:
        if row["selectivity"] == selectivity and row["series"] == series:
            return row
    return None


def plot_grouped_bars(rows, output_dir, figure_prefix, metric, ylabel, filename,
                      title_suffix, dpi):
    selectivities = [value for value in SELECTIVITY_ORDER if any(
        row["selectivity"] == value for row in rows
    )]
    series_names = [value for value in SERIES_ORDER if any(
        row["series"] == value for row in rows
    )]

    x_centers = list(range(len(selectivities)))
    group_width = 0.84
    bar_width = group_width / len(series_names)

    fig, ax = plt.subplots(figsize=(10.8, 5.2))
    for series_index, series in enumerate(series_names):
        x_values = [
            center - group_width / 2 + bar_width / 2 + series_index * bar_width
            for center in x_centers
        ]
        y_values = []
        for selectivity in selectivities:
            row = lookup(rows, selectivity, series)
            y_values.append(row[metric] if row else float("nan"))
        ax.bar(
            x_values,
            y_values,
            width=bar_width * 0.92,
            label=series,
            color=SERIES_COLORS.get(series),
            edgecolor="black",
            linewidth=0.45,
        )

    dataset = rows[0]["dataset"]
    ax.set_title(f"{dataset} Intersects: {title_suffix}")
    ax.set_xlabel("Selectivity")
    ax.set_ylabel(ylabel)
    ax.set_xticks(x_centers)
    ax.set_xticklabels(selectivities)
    ax.set_yscale("log")
    ax.grid(axis="y", linestyle="--", alpha=0.35)
    ax.legend(frameon=False, ncol=3)
    fig.tight_layout()

    path = output_dir / f"{figure_prefix}_{filename}.png"
    fig.savefig(path, dpi=dpi)
    plt.close(fig)
    return path


def print_brief(rows):
    print("GLIN-piecewise total time relative to Boost R-tree:")
    for selectivity in SELECTIVITY_ORDER:
        boost = lookup(rows, selectivity, "Boost R-tree")
        if not boost:
            continue
        parts = []
        for piece_limit in PIECE_LIMIT_ORDER:
            series = f"PL={piece_limit}"
            row = lookup(rows, selectivity, series)
            if row:
                parts.append(
                    f"{series}:{row['avg_total_us'] / boost['avg_total_us']:.2f}x"
                )
        print(f"  {selectivity}: " + " | ".join(parts))


def main():
    args = parse_args()
    piece_summary = Path(args.piece_summary)
    baseline_summary = Path(args.baseline_summary)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    rows = load_boost_rows(baseline_summary) + load_piece_rows(piece_summary)
    rows.sort(key=sort_key)
    if not rows:
        raise SystemExit("No comparison rows found")

    combined_csv = Path(args.combined_csv)
    write_combined_csv(combined_csv, rows)

    figure_paths = [
        plot_grouped_bars(
            rows,
            output_dir,
            args.figure_prefix,
            metric="avg_probe_ns",
            ylabel="Index probing time (nanoseconds, log scale)",
            filename="fig6_probe_time",
            title_suffix="Index Probing Time",
            dpi=args.dpi,
        ),
        plot_grouped_bars(
            rows,
            output_dir,
            args.figure_prefix,
            metric="avg_total_us",
            ylabel="Query response time (microseconds, log scale)",
            filename="fig7_query_time",
            title_suffix="Query Response Time",
            dpi=args.dpi,
        ),
        plot_grouped_bars(
            rows,
            output_dir,
            args.figure_prefix,
            metric="avg_candidates",
            ylabel="Candidates per query (log scale)",
            filename="candidate_count",
            title_suffix="Candidate Count",
            dpi=args.dpi,
        ),
    ]

    print(f"Wrote {combined_csv}")
    print_brief(rows)
    print("\nWrote figures:")
    for path in figure_paths:
        print(f"  {path}")


if __name__ == "__main__":
    main()
