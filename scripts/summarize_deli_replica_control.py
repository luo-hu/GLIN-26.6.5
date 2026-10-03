#!/usr/bin/env python3
"""Audit archived same-path replica controls and generate a paper table."""

import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
from statistics import median

DELI = "DELI_ALEX_HYBRID_SINGLE_STORE_COST"
REPLICA = "DELI_ALEX_HYBRID_COST"
PAIRED_FIELDS = (
    "dataset", "selectivity_tag", "mixed_profile", "workload_mode", "seed",
    "operation_count", "checkpoint_ops", "loaded_count", "initial_count",
    "live_count", "query_count", "mixed_query_ratio", "mixed_insert_ratio",
    "mixed_delete_ratio", "block_size", "local_delta_bound",
    "delete_compact_fraction", "delete_compact_bound", "avg_beta", "avg_tau",
    "predicate_shortcuts_enabled", "representative_point_shortcuts_enabled",
    "block_count", "block_checks", "visited_blocks", "compact_records_scanned",
    "delta_records_scanned", "candidates", "mbr_candidates", "predicate_shortcuts",
    "exact_calls", "answers", "insert_count", "delete_count", "success_count",
    "failed_count", "local_compaction_count_total", "summary_rebuild_count_total",
    "block_split_count", "source_file",
)
METRICS = (
    ("index_mb_estimate", "Estimated index size"),
    ("p95_insert_ms", "Insertion P95"),
    ("p95_delete_ms", "Deletion P95"),
    ("build_ms", "Construction time"),
    ("p95_query_ms", "Query P95"),
)


def read_run(path, index):
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    if not rows or {row["index"] for row in rows} != {index}:
        raise ValueError(f"Expected only {index}: {path}")
    required = set(PAIRED_FIELDS) | {
        metric for metric, _ in METRICS
    } | {"answers_match_boost", "missing_count", "extra_count", "validate_ok"}
    if not required <= rows[0].keys():
        raise ValueError(f"Missing fields in {path}: {required - rows[0].keys()}")
    checkpoints = {}
    for row in rows:
        key = (row["dataset"], row["mixed_profile"], row["selectivity_tag"],
               int(row["operation_count"]))
        if key in checkpoints:
            raise ValueError(f"Duplicate checkpoint: {path}, {key}")
        if row["validate_ok"] != "1" or row["answers_match_boost"] not in {"1", "-1"}:
            raise ValueError(f"Failed validation: {path}, {key}")
        if int(row["missing_count"]) or int(row["extra_count"]):
            raise ValueError(f"Answer mismatch: {path}, {key}")
        if int(row["failed_count"]):
            raise ValueError(f"Failed update operation: {path}, {key}")
        for metric, _ in METRICS:
            value = float(row[metric])
            if not math.isfinite(value) or value <= 0:
                raise ValueError(f"Invalid {metric}: {path}, {key}")
        checkpoints[key] = row
    return checkpoints


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-root", type=Path, default=Path(
        "results/deli_cost_single_diagnosis_current_0p1_balanced"))
    parser.add_argument("--repeats", type=int, default=4)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    if args.repeats < 2:
        raise ValueError("At least two paired process repetitions are required")
    fingerprints, details, final_pairs = [], [], []
    checked = unchecked = 0
    first_deli = None
    for repeat in range(1, args.repeats + 1):
        runs = {}
        for index in (DELI, REPLICA):
            path = args.input_root / f"r{repeat}_{index}" / "dynamic_compare_summary.csv"
            rows = read_run(path, index)
            runs[index] = rows
            checked += sum(row["answers_match_boost"] == "1" for row in rows.values())
            unchecked += sum(row["answers_match_boost"] == "-1" for row in rows.values())
            fingerprints.append({
                "path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            })
        if runs[DELI].keys() != runs[REPLICA].keys():
            raise ValueError(f"Unmatched checkpoint matrix: repeat {repeat}")
        if first_deli is None:
            first_deli = runs[DELI]
        elif runs[DELI].keys() != first_deli.keys():
            raise ValueError(f"Checkpoint matrix changed across repeats: {repeat}")
        for key, left in runs[DELI].items():
            right = runs[REPLICA][key]
            for field in PAIRED_FIELDS:
                if left[field] != right[field] or left[field] != first_deli[key][field]:
                    raise ValueError(f"Unmatched {field}: repeat {repeat}, {key}")
        scenarios = {key[:3] for key in runs[DELI]}
        if len(scenarios) != 1:
            raise ValueError("This archived control requires one matched scenario")
        last = max(runs[DELI], key=lambda key: key[-1])
        left, right = runs[DELI][last], runs[REPLICA][last]
        if left["answers_match_boost"] != "1" or right["answers_match_boost"] != "1":
            raise ValueError(f"Unchecked final pair: repeat {repeat}")
        final_pairs.append((left, right))
        for metric, label in METRICS:
            a, b = float(left[metric]), float(right[metric])
            details.append({
                "repeat": repeat, "dataset": last[0], "mixed_profile": last[1],
                "selectivity_tag": last[2], "operation_count": last[-1],
                "metric": metric, "deli": a, "replica": b, "replica_deli_ratio": b / a,
            })

    args.output_dir.mkdir(parents=True, exist_ok=True)
    with (args.output_dir / "deli_replica_control_pairs.csv").open(
            "w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(details[0]))
        writer.writeheader()
        writer.writerows(details)
    aggregates = []
    table = [
        r"\begin{tabular}{@{}lrrr@{}}", r"\toprule",
        r"Cost & Median ratio & Min. & Max. \\", r"\midrule",
    ]
    for metric, label in METRICS:
        ratios = [record["replica_deli_ratio"] for record in details
                  if record["metric"] == metric]
        values = (median(ratios), min(ratios), max(ratios))
        aggregates.append(dict(metric=metric, median_ratio=values[0],
                               min_ratio=values[1], max_ratio=values[2]))
        table.append(" & ".join([label] + [f"{value:.3f}" for value in values]) + r" \\")
        print(f"{label}: median {values[0]:.3f}; range {values[1]:.3f}--{values[2]:.3f}")
    table.extend([r"\bottomrule", r"\end{tabular}"])
    (args.output_dir / "deli_replica_control_table.tex").write_text(
        "\n".join(table) + "\n", encoding="utf-8")
    settings = {
        field: final_pairs[0][0][field]
        for field in PAIRED_FIELDS[:20]
    }
    manifest = {
        "source_files": fingerprints, "settings": settings,
        "paired_process_repeats": args.repeats,
        "checked_checkpoints": checked, "unchecked_checkpoints": unchecked,
        "same_path_and_maintenance_counters_match": True,
        "metrics": aggregates,
        "binary_fingerprint": (args.input_root / "binary.sha256").read_text().strip(),
        "scope": "Archived one-scenario control; do not pool with main8 timings.",
        "ratio_definition": "Median of paired Replica/DELI ratios, not ratio of medians.",
        "memory_scope": "Method-specific index estimate, excluding shared geometries; not RSS.",
    }
    (args.output_dir / "deli_replica_control_manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Checked {checked}; unchecked {unchecked}; matched repetitions {args.repeats}")


if __name__ == "__main__":
    main()
