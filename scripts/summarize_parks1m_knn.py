#!/usr/bin/env python3
"""Summarize parks 1M KNN-selectivity benchmark CSVs."""

import argparse
import csv
from pathlib import Path


SELECTIVITY_ORDER = ["0.001%", "0.01%", "0.1%", "1%"]
INDEX_ORDER = ["GLIN", "GLIN_PIECEWISE", "Boost_Rtree", "GEOS_Quadtree"]
REL_ORDER = ["contains", "intersects"]
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


def selectivity_from_name(path):
    name = path.name
    if "_0p001pct_" in name:
        return "0.001%"
    if "_0p01pct_" in name:
        return "0.01%"
    if "_0p1pct_" in name:
        return "0.1%"
    if "_1pct_" in name:
        return "1%"
    return "unknown"


def parse_args():
    parser = argparse.ArgumentParser(
        description="Summarize parks 1M KNN-selectivity benchmark CSVs."
    )
    parser.add_argument(
        "--glob",
        default="results/parks_1m_geom_knn_*.csv",
        help="Input raw benchmark CSV glob. Default: results/parks_1m_geom_knn_*.csv",
    )
    parser.add_argument(
        "--output",
        default="results/parks_1m_geom_knn_summary.csv",
        help="Output summary CSV. Default: results/parks_1m_geom_knn_summary.csv",
    )
    return parser.parse_args()


def sort_key(row):
    return (
        SELECTIVITY_ORDER.index(row["selectivity"])
        if row["selectivity"] in SELECTIVITY_ORDER
        else len(SELECTIVITY_ORDER),
        REL_ORDER.index(row["relationship"])
        if row["relationship"] in REL_ORDER
        else len(REL_ORDER),
        INDEX_ORDER.index(row["index"])
        if row["index"] in INDEX_ORDER
        else len(INDEX_ORDER),
    )


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

    def avg(column):
        return sum(float(row[column]) for row in rows) / len(rows)

    first = rows[0]
    return {
        "selectivity": selectivity_from_name(path),
        "dataset": first["dataset"],
        "index": first["index"],
        "relationship": first["relationship"],
        "loaded_count": first["loaded_count"],
        "query_rows": len(rows),
        "build_ms": float(first["build_ns"]) / 1e6,
        "avg_probe_ns": avg("probe_ns"),
        "avg_refine_ns": avg("refine_ns"),
        "avg_total_us": avg("total_ns") / 1e3,
        "avg_candidates": avg("candidates"),
        "avg_answers": avg("answers"),
    }


def main():
    args = parse_args()
    paths = [
        path for path in sorted(Path(".").glob(args.glob))
        if selectivity_from_name(path) != "unknown"
        and "summary" not in path.name
        and path.suffix == ".csv"
    ]
    if not paths:
        raise SystemExit(f"No raw benchmark CSV files found for glob: {args.glob}")

    summaries = [summarize_file(path) for path in paths]
    summaries.sort(key=sort_key)

    output_path = Path(args.output)
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
    ]

    with output_path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(summaries)

    print(f"Wrote {output_path}")
    for row in summaries:
        print(
            f"{row['selectivity']} {row['relationship']} {row['index']} "
            f"total_us={row['avg_total_us']:.3f} "
            f"candidates={row['avg_candidates']:.2f} "
            f"answers={row['avg_answers']:.2f}"
        )


if __name__ == "__main__":
    main()
