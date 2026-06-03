#!/usr/bin/env python3
"""Summarize raw benchmark CSVs for the 1M all-dataset GLIN experiment."""

import argparse
import csv
import re
from pathlib import Path


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
REL_ORDER = ["contains", "intersects"]
INDEX_ORDER = ["GLIN", "GLIN_PIECEWISE", "Boost_Rtree", "GEOS_Quadtree"]

SELECTIVITY_TAGS = {
    "0p001pct": "0.001%",
    "0p01pct": "0.01%",
    "0p1pct": "0.1%",
    "1pct": "1%",
}

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


def parse_args():
    parser = argparse.ArgumentParser(
        description="Summarize all-dataset 1M raw benchmark CSVs."
    )
    parser.add_argument(
        "--glob",
        default="results/all_1m/*.csv",
        help="Input raw benchmark CSV glob. Default: results/all_1m/*.csv",
    )
    parser.add_argument(
        "--output",
        default="results/all_1m_summary.csv",
        help="Output summary CSV. Default: results/all_1m_summary.csv",
    )
    return parser.parse_args()


def selectivity_from_name(path):
    match = re.search(r"_(0p001pct|0p01pct|0p1pct|1pct)_", path.name)
    if not match:
        return "unknown"
    return SELECTIVITY_TAGS[match.group(1)]


def avg(rows, column):
    return sum(float(row[column]) for row in rows) / len(rows)


def summarize_file(path):
    with path.open(newline="") as f:
        reader = csv.DictReader(f)
        if reader.fieldnames is None:
            raise ValueError(f"Empty CSV: {path}")
        missing = REQUIRED_COLUMNS.difference(reader.fieldnames)
        if missing:
            missing_text = ", ".join(sorted(missing))
            raise ValueError(f"{path} is not a raw benchmark CSV; missing: {missing_text}")
        rows = list(reader)

    if not rows:
        raise ValueError(f"Empty CSV: {path}")

    first = rows[0]
    avg_candidates = avg(rows, "candidates")
    avg_answers = avg(rows, "answers")
    return {
        "selectivity": selectivity_from_name(path),
        "dataset": first["dataset"],
        "index": first["index"],
        "relationship": first["relationship"],
        "loaded_count": first["loaded_count"],
        "query_rows": len(rows),
        "build_ms": float(first["build_ns"]) / 1e6,
        "avg_probe_ns": avg(rows, "probe_ns"),
        "avg_refine_ns": avg(rows, "refine_ns"),
        "avg_total_us": avg(rows, "total_ns") / 1e3,
        "avg_candidates": avg_candidates,
        "avg_answers": avg_answers,
        "avg_candidate_ratio": avg_candidates / avg_answers if avg_answers > 0 else 0.0,
        "source_csv": str(path),
    }


def ordered_index(value, order):
    return order.index(value) if value in order else len(order)


def sort_key(row):
    return (
        ordered_index(row["dataset"], DATASET_ORDER),
        ordered_index(row["relationship"], REL_ORDER),
        ordered_index(row["selectivity"], SELECTIVITY_ORDER),
        ordered_index(row["index"], INDEX_ORDER),
    )


def main():
    args = parse_args()
    paths = [
        path for path in sorted(Path(".").glob(args.glob))
        if path.suffix == ".csv"
        and "summary" not in path.name
        and selectivity_from_name(path) != "unknown"
    ]
    if not paths:
        raise SystemExit(f"No raw benchmark CSV files found for glob: {args.glob}")

    rows = [summarize_file(path) for path in paths]
    rows.sort(key=sort_key)

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fields = [
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
        "source_csv",
    ]
    with output_path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)

    print(f"Wrote {output_path}")
    print(f"Summarized files: {len(paths)}")
    print(f"Summary rows: {len(rows)}")


if __name__ == "__main__":
    main()
