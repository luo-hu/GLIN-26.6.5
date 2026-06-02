#!/usr/bin/env python3
"""Summarize GLIN-piecewise piece_limit sweep raw benchmark CSVs."""

import argparse
import csv
import re
from pathlib import Path


SELECTIVITY_ORDER = ["0.001%", "0.01%", "0.1%", "1%"]
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
    "piece_limit",
    "build_ns",
    "probe_ns",
    "refine_ns",
    "total_ns",
    "candidates",
    "answers",
}


def parse_args():
    parser = argparse.ArgumentParser(
        description="Summarize GLIN-piecewise piece_limit sweep CSVs."
    )
    parser.add_argument(
        "--glob",
        default="results/parks_1m_jts_strtree_knn_pl*_glin_piece_intersects.csv",
        help="Input raw CSV glob.",
    )
    parser.add_argument(
        "--output",
        default="results/parks_1m_jts_strtree_knn_piece_limit_summary.csv",
        help="Output summary CSV.",
    )
    return parser.parse_args()


def metadata_from_name(path):
    match = re.search(r"_pl(\d+)_(0p001pct|0p01pct|0p1pct|1pct)_", path.name)
    if not match:
      return None
    piece_limit = int(match.group(1))
    selectivity = SELECTIVITY_TAGS[match.group(2)]
    return piece_limit, selectivity


def avg(rows, column):
    return sum(float(row[column]) for row in rows) / len(rows)


def summarize_file(path):
    metadata = metadata_from_name(path)
    if metadata is None:
        raise ValueError(f"Cannot parse piece_limit/selectivity from filename: {path}")
    piece_limit_from_name, selectivity = metadata

    with path.open(newline="") as f:
        reader = csv.DictReader(f)
        if reader.fieldnames is None:
            raise ValueError(f"Empty CSV: {path}")
        missing = REQUIRED_COLUMNS.difference(reader.fieldnames)
        if missing:
            missing_text = ", ".join(sorted(missing))
            raise ValueError(f"{path} missing columns: {missing_text}")
        rows = list(reader)

    if not rows:
        raise ValueError(f"Empty CSV: {path}")

    first = rows[0]
    piece_limit = int(float(first["piece_limit"]))
    if piece_limit != piece_limit_from_name:
        raise ValueError(
            f"{path} filename PL={piece_limit_from_name}, CSV PL={piece_limit}"
        )

    avg_candidates = avg(rows, "candidates")
    avg_answers = avg(rows, "answers")
    avg_candidate_ratio = avg_candidates / avg_answers if avg_answers > 0 else 0.0

    return {
        "selectivity": selectivity,
        "dataset": first["dataset"],
        "index": first["index"],
        "relationship": first["relationship"],
        "loaded_count": first["loaded_count"],
        "query_rows": len(rows),
        "piece_limit": piece_limit,
        "build_ms": float(first["build_ns"]) / 1e6,
        "avg_probe_ns": avg(rows, "probe_ns"),
        "avg_refine_ns": avg(rows, "refine_ns"),
        "avg_total_us": avg(rows, "total_ns") / 1e3,
        "avg_candidates": avg_candidates,
        "avg_answers": avg_answers,
        "avg_candidate_ratio": avg_candidate_ratio,
    }


def sort_key(row):
    selectivity = row["selectivity"]
    return (
        SELECTIVITY_ORDER.index(selectivity)
        if selectivity in SELECTIVITY_ORDER
        else len(SELECTIVITY_ORDER),
        row["piece_limit"],
    )


def main():
    args = parse_args()
    paths = [
        path for path in sorted(Path(".").glob(args.glob))
        if path.suffix == ".csv" and "summary" not in path.name
    ]
    if not paths:
        raise SystemExit(f"No raw benchmark CSV files found for glob: {args.glob}")

    summaries = [summarize_file(path) for path in paths]
    summaries.sort(key=sort_key)

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fields = [
        "selectivity",
        "dataset",
        "index",
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
    ]

    with output_path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(summaries)

    print(f"Wrote {output_path}")
    for row in summaries:
        print(
            f"{row['selectivity']} PL={row['piece_limit']} "
            f"total_us={row['avg_total_us']:.3f} "
            f"probe_ns={row['avg_probe_ns']:.1f} "
            f"candidates={row['avg_candidates']:.2f} "
            f"ratio={row['avg_candidate_ratio']:.3f}"
        )


if __name__ == "__main__":
    main()
