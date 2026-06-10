#!/usr/bin/env python3
"""Summarize IntervalOverlapIndex, GLIN-piecewise, and Boost R-tree query CSVs."""

import argparse
import csv
import re
from pathlib import Path


SELECTIVITY_TAGS = {
    "0p001pct": "0.001%",
    "0p01pct": "0.01%",
    "0p1pct": "0.1%",
    "1pct": "1%",
}

IO_INDEXES = {"IntervalOverlapIndex", "IO_BLOCK_MBR", "IO_OVERFLOW"}


def parse_args():
    parser = argparse.ArgumentParser(
        description="Summarize interval-overlap diagnostic CSV files."
    )
    parser.add_argument("--result_dir", required=True)
    parser.add_argument("--output_csv", required=True)
    parser.add_argument(
        "--exclude_datasets",
        default="",
        help="Datasets to exclude, separated by comma or spaces. Example: ZGAP_WIDE",
    )
    return parser.parse_args()


def as_float(row, field):
    try:
        return float(row.get(field) or 0)
    except ValueError:
        return 0.0


def load_rows(path):
    with open(path, newline="") as handle:
        return list(csv.DictReader(handle))


def selectivity_from_name(path):
    match = re.search(r"_(0p001pct|0p01pct|0p1pct|1pct)_", path.name)
    if not match:
        return "1%"
    return SELECTIVITY_TAGS[match.group(1)]


def summarize_file(path):
    rows = load_rows(path)
    if not rows:
        return None

    first = rows[0]
    dataset = first.get("dataset", "")
    index = first.get("index", path.stem)
    relationship = first.get("relationship", "intersects")
    loaded_count = int(as_float(first, "loaded_count"))
    query_count = len(rows)

    build_ns = as_float(first, "build_ns")
    load_ns = as_float(first, "load_ns")
    block_size = int(as_float(first, "block_size"))
    block_count = int(as_float(first, "block_count"))
    overflow_count = int(as_float(first, "overflow_count"))
    overflow_fraction = as_float(first, "overflow_fraction")
    overflow_span_threshold = as_float(first, "overflow_span_threshold")

    probe_ns = sum(as_float(row, "probe_ns") for row in rows)
    refine_ns = sum(as_float(row, "refine_ns") for row in rows)
    total_ns = sum(as_float(row, "total_ns") for row in rows)
    if total_ns == 0:
        total_ns = probe_ns + refine_ns

    answers = sum(int(as_float(row, "answers")) for row in rows)

    if index in IO_INDEXES:
        candidates = sum(int(as_float(row, "exact_calls")) for row in rows)
        records_scanned = sum(int(as_float(row, "records_scanned")) for row in rows)
        interval_candidates = sum(
            int(as_float(row, "interval_candidates")) for row in rows
        )
        mbr_candidates = sum(int(as_float(row, "mbr_candidates")) for row in rows)
        exact_calls = candidates
        prefix_records = sum(int(as_float(row, "prefix_records")) for row in rows)
        prefix_blocks = sum(int(as_float(row, "prefix_blocks")) for row in rows)
        visited_blocks = sum(int(as_float(row, "visited_blocks")) for row in rows)
        skipped_zmax_blocks = sum(
            int(as_float(row, "skipped_zmax_blocks")) for row in rows
        )
        skipped_mbr_blocks = sum(
            int(as_float(row, "skipped_mbr_blocks")) for row in rows
        )
        overflow_probe_candidates = sum(
            int(as_float(row, "overflow_probe_candidates")) for row in rows
        )
        overflow_interval_candidates = sum(
            int(as_float(row, "overflow_interval_candidates")) for row in rows
        )
        overflow_exact_calls = sum(
            int(as_float(row, "overflow_exact_calls")) for row in rows
        )
        overflow_answers = sum(
            int(as_float(row, "overflow_answers")) for row in rows
        )
    else:
        candidates = sum(int(as_float(row, "candidates")) for row in rows)
        records_scanned = 0
        interval_candidates = 0
        mbr_candidates = candidates
        exact_calls = candidates
        prefix_records = 0
        prefix_blocks = 0
        visited_blocks = int(sum(as_float(row, "visited_leaf") for row in rows))
        skipped_zmax_blocks = 0
        skipped_mbr_blocks = 0
        overflow_probe_candidates = 0
        overflow_interval_candidates = 0
        overflow_exact_calls = 0
        overflow_answers = 0

    candidate_answer_ratio = 0 if answers == 0 else candidates / answers
    avg_probe_ns = 0 if query_count == 0 else probe_ns / query_count
    avg_refine_ns = 0 if query_count == 0 else refine_ns / query_count
    avg_total_ns = 0 if query_count == 0 else total_ns / query_count
    skipped_block_ratio = (
        0
        if prefix_blocks == 0
        else (skipped_zmax_blocks + skipped_mbr_blocks) / prefix_blocks
    )

    return {
        "selectivity": selectivity_from_name(path),
        "dataset": dataset,
        "index": index,
        "relationship": relationship,
        "loaded_count": loaded_count,
        "query_count": query_count,
        "block_size": block_size,
        "block_count": block_count,
        "overflow_count": overflow_count,
        "overflow_fraction": overflow_fraction,
        "overflow_span_threshold": overflow_span_threshold,
        "load_ns": int(load_ns),
        "build_ns": int(build_ns),
        "probe_ns": int(probe_ns),
        "refine_ns": int(refine_ns),
        "total_ns": int(total_ns),
        "avg_probe_ns": avg_probe_ns,
        "avg_refine_ns": avg_refine_ns,
        "avg_total_ns": avg_total_ns,
        "candidates": candidates,
        "answers": answers,
        "candidate_answer_ratio": candidate_answer_ratio,
        "records_scanned": records_scanned,
        "interval_candidates": interval_candidates,
        "mbr_candidates": mbr_candidates,
        "exact_calls": exact_calls,
        "prefix_records": prefix_records,
        "prefix_blocks": prefix_blocks,
        "visited_blocks": visited_blocks,
        "skipped_zmax_blocks": skipped_zmax_blocks,
        "skipped_mbr_blocks": skipped_mbr_blocks,
        "skipped_block_ratio": skipped_block_ratio,
        "overflow_probe_candidates": overflow_probe_candidates,
        "overflow_interval_candidates": overflow_interval_candidates,
        "overflow_exact_calls": overflow_exact_calls,
        "overflow_answers": overflow_answers,
        "answers_match_boost": "",
        "answers_delta_vs_boost": "",
    }


def split_names(value):
    if not value:
        return set()
    return {item for item in re.split(r"[,\s]+", value.strip()) if item}


def main():
    args = parse_args()
    result_dir = Path(args.result_dir)
    output_csv = Path(args.output_csv)
    excluded_datasets = split_names(args.exclude_datasets)

    summaries = []
    for path in sorted(result_dir.glob("*_*.csv")):
        if path.name == output_csv.name or path.name.endswith("_summary.csv"):
            continue
        summary = summarize_file(path)
        if summary:
            if summary["dataset"] in excluded_datasets:
                continue
            summaries.append(summary)

    boost_answers = {}
    for summary in summaries:
        if summary["index"] == "Boost_Rtree":
            boost_answers[(summary["dataset"], summary["selectivity"])] = summary[
                "answers"
            ]

    for summary in summaries:
        expected = boost_answers.get((summary["dataset"], summary["selectivity"]))
        if expected is not None:
            delta = summary["answers"] - expected
            summary["answers_delta_vs_boost"] = delta
            summary["answers_match_boost"] = 1 if delta == 0 else 0

    fieldnames = [
        "selectivity",
        "dataset",
        "index",
        "relationship",
        "loaded_count",
        "query_count",
        "block_size",
        "block_count",
        "overflow_count",
        "overflow_fraction",
        "overflow_span_threshold",
        "load_ns",
        "build_ns",
        "probe_ns",
        "refine_ns",
        "total_ns",
        "avg_probe_ns",
        "avg_refine_ns",
        "avg_total_ns",
        "candidates",
        "answers",
        "candidate_answer_ratio",
        "records_scanned",
        "interval_candidates",
        "mbr_candidates",
        "exact_calls",
        "prefix_records",
        "prefix_blocks",
        "visited_blocks",
        "skipped_zmax_blocks",
        "skipped_mbr_blocks",
        "skipped_block_ratio",
        "overflow_probe_candidates",
        "overflow_interval_candidates",
        "overflow_exact_calls",
        "overflow_answers",
        "answers_match_boost",
        "answers_delta_vs_boost",
    ]

    output_csv.parent.mkdir(parents=True, exist_ok=True)
    with open(output_csv, "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(summaries)

    print(f"Summary CSV: {output_csv}")
    for summary in summaries:
        print(
            f"{summary['dataset']} {summary['selectivity']} {summary['index']} "
            f"block={summary['block_size']} "
            f"overflow_fraction={summary['overflow_fraction']:.4g} "
            f"overflow_count={summary['overflow_count']} "
            f"answers={summary['answers']} candidates={summary['candidates']} "
            f"ratio={summary['candidate_answer_ratio']:.3f} "
            f"avg_total_ns={summary['avg_total_ns']:.1f} "
            f"match_boost={summary['answers_match_boost']}"
        )


if __name__ == "__main__":
    main()
