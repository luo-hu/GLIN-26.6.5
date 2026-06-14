#!/usr/bin/env python3
"""Summarize unified dynamic comparison CSV files."""

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


def parse_args():
    parser = argparse.ArgumentParser(description="汇总统一动态对比实验。")
    parser.add_argument("--result_dir", required=True)
    parser.add_argument("--output_csv", required=True)
    parser.add_argument("--exclude_datasets", default="")
    return parser.parse_args()


def split_names(value):
    if not value:
        return set()
    return {item for item in re.split(r"[,\s]+", value.strip()) if item}


def selectivity_from_name(path):
    match = re.search(r"_(0p001pct|0p01pct|0p1pct|1pct)_", path.name)
    return SELECTIVITY_TAGS.get(match.group(1), "") if match else ""


def selectivity_tag_from_name(path):
    match = re.search(r"_(0p001pct|0p01pct|0p1pct|1pct)_", path.name)
    return match.group(1) if match else ""


def as_float(row, field):
    try:
        return float(row.get(field) or 0)
    except ValueError:
        return 0.0


def enrich(row, path):
    out = dict(row)
    out["source_file"] = path.name
    out["selectivity"] = selectivity_from_name(path)
    out["selectivity_tag"] = selectivity_tag_from_name(path)
    out["avg_query_ms"] = as_float(out, "avg_query_ns") / 1e6
    out["p95_query_ms"] = as_float(out, "p95_query_ns") / 1e6
    out["p99_query_ms"] = as_float(out, "p99_query_ns") / 1e6
    out["build_ms"] = as_float(out, "build_ns") / 1e6
    out["insert_ms"] = as_float(out, "insert_ns") / 1e6
    out["delete_ms"] = as_float(out, "delete_ns") / 1e6
    out["index_mb_estimate"] = as_float(out, "index_bytes_estimate") / (1024 * 1024)
    return out


def field_order(rows):
    preferred = [
        "selectivity",
        "selectivity_tag",
        "dataset",
        "index",
        "checkpoint",
        "loaded_count",
        "live_count",
        "query_count",
        "avg_query_ms",
        "p95_query_ms",
        "p99_query_ms",
        "insert_tps",
        "delete_tps",
        "index_mb_estimate",
        "answers_match_boost",
        "missing_count",
        "extra_count",
        "candidate_answer_ratio",
        "exact_calls",
        "answers",
        "build_ms",
        "insert_ms",
        "delete_ms",
    ]
    all_fields = set()
    for row in rows:
        all_fields.update(row.keys())
    ordered = [field for field in preferred if field in all_fields]
    ordered.extend(sorted(all_fields.difference(ordered)))
    return ordered


def main():
    args = parse_args()
    result_dir = Path(args.result_dir)
    output_csv = Path(args.output_csv)
    excluded = split_names(args.exclude_datasets)
    rows = []
    for path in sorted(result_dir.glob("*_dynamic_compare.csv")):
        with path.open(newline="") as handle:
            for row in csv.DictReader(handle):
                if row.get("dataset", "") in excluded:
                    continue
                rows.append(enrich(row, path))

    output_csv.parent.mkdir(parents=True, exist_ok=True)
    fields = field_order(rows) if rows else ["dataset", "index", "checkpoint"]
    with output_csv.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)

    print(f"写出 summary: {output_csv}")
    bad = [r for r in rows if r.get("answers_match_boost") != "1"]
    if bad:
        print("警告：存在 answers_match_boost != 1 的行：")
        for row in bad[:20]:
            print(
                f"  {row.get('dataset')} {row.get('selectivity')} "
                f"{row.get('index')} {row.get('checkpoint')} "
                f"missing={row.get('missing_count')} extra={row.get('extra_count')}"
            )
    else:
        print("correctness: 所有行 answers_match_boost=1")


if __name__ == "__main__":
    main()
