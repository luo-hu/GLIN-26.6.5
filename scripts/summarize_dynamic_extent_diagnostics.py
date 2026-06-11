#!/usr/bin/env python3
"""Summarize DELI-Dynamic-Single diagnostic CSV files."""

import argparse
import csv
import math
import re
from pathlib import Path


SELECTIVITY_TAGS = {
    "0p001pct": "0.001%",
    "0p01pct": "0.01%",
    "0p1pct": "0.1%",
    "1pct": "1%",
}


def parse_args():
    parser = argparse.ArgumentParser(
        description="汇总 DELI-Dynamic-Single 动态维护实验的 raw CSV。"
    )
    parser.add_argument("--result_dir", required=True, help="raw CSV 所在目录。")
    parser.add_argument("--output_csv", required=True, help="summary CSV 输出路径。")
    parser.add_argument(
        "--exclude_datasets",
        default="",
        help="汇总时排除的数据集，多个值用逗号或空格分开，例如：ZGAP_WIDE DIAG_L。",
    )
    return parser.parse_args()


def split_names(value):
    if not value:
        return set()
    return {item for item in re.split(r"[,\s]+", value.strip()) if item}


def selectivity_from_name(path):
    match = re.search(r"_(0p001pct|0p01pct|0p1pct|1pct)_", path.name)
    if not match:
        return ""
    return SELECTIVITY_TAGS[match.group(1)]


def selectivity_tag_from_name(path):
    match = re.search(r"_(0p001pct|0p01pct|0p1pct|1pct)_", path.name)
    return match.group(1) if match else ""


def stale_tag_from_name(path):
    match = re.search(r"_st([0-9p]+)_deli_dynamic\.csv$", path.name)
    return match.group(1) if match else ""


def as_float(row, field):
    value = row.get(field, "")
    if value == "":
        return 0.0
    try:
        return float(value)
    except ValueError:
        return 0.0


def as_int(row, field):
    return int(round(as_float(row, field)))


def safe_div(numerator, denominator):
    if denominator == 0:
        return 0.0
    return numerator / denominator


def load_rows(path):
    with path.open(newline="") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            return []
        return list(reader)


def enrich_row(row, path):
    enriched = dict(row)
    enriched["source_file"] = path.name
    enriched["selectivity_tag"] = selectivity_tag_from_name(path)
    enriched["selectivity"] = selectivity_from_name(path)
    enriched["stale_threshold_tag"] = stale_tag_from_name(path)

    stale = as_float(enriched, "stale_threshold_fraction")
    enriched["stale_threshold_label"] = f"{stale:g}"
    enriched["avg_query_ms"] = as_float(enriched, "avg_query_ns") / 1e6
    enriched["p50_query_ms"] = as_float(enriched, "p50_query_ns") / 1e6
    enriched["p95_query_ms"] = as_float(enriched, "p95_query_ns") / 1e6
    enriched["p99_query_ms"] = as_float(enriched, "p99_query_ns") / 1e6
    enriched["summary_rebuild_ms"] = as_float(enriched, "summary_rebuild_ns") / 1e6
    enriched["boost_rebuild_ms"] = as_float(enriched, "boost_rebuild_ns") / 1e6
    enriched["boost_query_ms"] = as_float(enriched, "boost_query_ns") / 1e6
    enriched["insert_ms"] = as_float(enriched, "insert_ns") / 1e6
    enriched["delete_ms"] = as_float(enriched, "delete_ns") / 1e6
    enriched["exact_calls_per_query"] = safe_div(
        as_float(enriched, "exact_calls"), as_float(enriched, "query_count")
    )
    enriched["records_scanned_per_query"] = safe_div(
        as_float(enriched, "records_scanned"), as_float(enriched, "query_count")
    )
    return enriched


def field_order(rows):
    preferred = [
        "selectivity",
        "selectivity_tag",
        "dataset",
        "index",
        "checkpoint",
        "checkpoint_id",
        "loaded_count",
        "query_count",
        "initial_count",
        "insert_count",
        "delete_count",
        "live_count",
        "total_records",
        "dead_records",
        "dead_entry_ratio",
        "block_size",
        "block_count",
        "stale_threshold_fraction",
        "stale_threshold_label",
        "stale_threshold_tag",
        "stale_block_count",
        "summary_rebuild_count",
        "summary_rebuild_ns",
        "summary_rebuild_ms",
        "block_split_count",
        "avg_query_ns",
        "avg_query_ms",
        "p50_query_ns",
        "p50_query_ms",
        "p95_query_ns",
        "p95_query_ms",
        "p99_query_ns",
        "p99_query_ms",
        "records_scanned",
        "records_scanned_per_query",
        "visited_blocks",
        "skipped_zmax_blocks",
        "skipped_mbr_blocks",
        "skipped_block_ratio",
        "interval_candidates",
        "mbr_candidates",
        "exact_calls",
        "exact_calls_per_query",
        "answers",
        "candidate_answer_ratio",
        "zero_answer_queries",
        "answers_match_boost",
        "missing_count",
        "extra_count",
        "boost_rebuild_ns",
        "boost_rebuild_ms",
        "boost_query_ns",
        "boost_query_ms",
        "insert_ns",
        "insert_ms",
        "delete_ns",
        "delete_ms",
        "insert_tps",
        "delete_tps",
        "validate_ok",
        "seed",
        "lines_seen",
        "parse_errors",
        "source_file",
    ]
    seen = set()
    ordered = []
    all_fields = set()
    for row in rows:
        all_fields.update(row.keys())
    for field in preferred:
        if field in all_fields:
            ordered.append(field)
            seen.add(field)
    ordered.extend(sorted(all_fields - seen))
    return ordered


def print_console_summary(rows):
    print("动态维护实验汇总：")
    if not rows:
        print("  没有找到可汇总的 raw CSV。")
        return

    after_delete = [row for row in rows if row.get("checkpoint") == "after_delete"]
    target_rows = after_delete or rows
    target_rows = sorted(
        target_rows,
        key=lambda row: (
            row.get("dataset", ""),
            row.get("selectivity", ""),
            as_int(row, "block_size"),
            as_float(row, "stale_threshold_fraction"),
            row.get("checkpoint", ""),
        ),
    )

    for row in target_rows:
        dataset = row.get("dataset", "")
        selectivity = row.get("selectivity", "")
        checkpoint = row.get("checkpoint", "")
        block_size = as_int(row, "block_size")
        stale = as_float(row, "stale_threshold_fraction")
        match = as_int(row, "answers_match_boost")
        valid = as_int(row, "validate_ok")
        avg_ms = as_float(row, "avg_query_ms")
        delete_tps = as_float(row, "delete_tps")
        dead_ratio = as_float(row, "dead_entry_ratio")
        rebuilds = as_int(row, "summary_rebuild_count")
        print(
            "  "
            f"{dataset} {selectivity} {checkpoint} "
            f"b={block_size} stale={stale:g} "
            f"avg_query_ms={avg_ms:.4f} delete_tps={delete_tps:.2f} "
            f"dead_ratio={dead_ratio:.4f} rebuilds={rebuilds} "
            f"match={match} validate={valid}"
        )

    bad = [
        row
        for row in rows
        if as_int(row, "answers_match_boost") not in (-1, 1)
        or as_int(row, "validate_ok") != 1
    ]
    if bad:
        print("警告：发现 correctness 或 validate 失败的行，请优先检查：")
        for row in bad[:20]:
            print(
                "  "
                f"{row.get('source_file', '')} checkpoint={row.get('checkpoint', '')} "
                f"missing={row.get('missing_count', '')} extra={row.get('extra_count', '')} "
                f"validate_ok={row.get('validate_ok', '')}"
            )


def main():
    args = parse_args()
    result_dir = Path(args.result_dir)
    output_csv = Path(args.output_csv)
    excluded = split_names(args.exclude_datasets)

    rows = []
    for path in sorted(result_dir.glob("*_deli_dynamic.csv")):
        for row in load_rows(path):
            if row.get("dataset", "") in excluded:
                continue
            rows.append(enrich_row(row, path))

    output_csv.parent.mkdir(parents=True, exist_ok=True)
    fields = field_order(rows) if rows else ["dataset", "index", "checkpoint"]
    with output_csv.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)

    print(f"写出 summary: {output_csv}")
    print_console_summary(rows)


if __name__ == "__main__":
    main()
