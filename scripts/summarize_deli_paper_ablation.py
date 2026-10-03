#!/usr/bin/env python3
"""Summarize matched final-checkpoint DELI ablations, not independent repeats."""

import argparse
import csv
import math
from pathlib import Path
from statistics import median

VARIANTS = ("full", "fixed_layout", "conditional_refresh", "fixed_budget", "fixed_layout_no_prl")
BASELINES = {variant: "full" for variant in VARIANTS}
BASELINES["fixed_layout_no_prl"] = "fixed_layout"
LABELS = {
    "full": "Full DELI",
    "fixed_layout": "Fixed layout",
    "conditional_refresh": "Conditional refresh",
    "fixed_budget": "Fixed budget",
    "fixed_layout_no_prl": "Fixed layout, no PRL",
}
INDEX = "DELI_ALEX_HYBRID_SINGLE_STORE_COST"
METRICS = (
    "p95_query_ms", "p95_insert_ms", "p95_delete_ms",
    "query_tps", "insert_tps", "delete_tps", "overall_ops_tps",
    "build_ms", "index_mb_estimate", "block_count", "avg_beta", "avg_tau",
    "summary_rebuild_count_total", "summary_rebuild_ns_total",
    "local_compaction_count_total", "local_compaction_ns_total",
    "compact_records_scanned", "delta_records_scanned",
    "candidates", "predicate_shortcuts", "exact_calls", "geos_exact_ms",
)
TABLE_METRICS = (
    "p95_query_ms", "p95_insert_ms", "p95_delete_ms", "build_ms",
    "index_mb_estimate", "summary_rebuild_ns_total", "exact_calls",
)


def read_variant(path):
    with path.open(newline="", encoding="utf-8") as stream:
        rows = [row for row in csv.DictReader(stream) if row["index"] == INDEX]
    if not rows:
        raise ValueError(f"No {INDEX} rows in {path}")
    checkpoints = {}
    for row in rows:
        key = (row["dataset"], row["mixed_profile"], row["selectivity_tag"],
               int(row["operation_count"]))
        if key in checkpoints:
            raise ValueError(f"Duplicate checkpoint {key} in {path}")
        if row["answers_match_boost"] not in {"-1", "1"} or row["validate_ok"] != "1":
            raise ValueError(f"Mismatch or self-validation failure: {key} in {path}")
        for metric in METRICS:
            value = float(row[metric])
            if not math.isfinite(value) or value < 0:
                raise ValueError(f"Invalid {metric}: {key} in {path}")
        checkpoints[key] = row
    last_operation = max(key[-1] for key in checkpoints)
    final = {key[:3]: row for key, row in checkpoints.items() if key[-1] == last_operation}
    expected = {key + (operation,) for key in final
                for operation in {key[-1] for key in checkpoints}}
    if set(checkpoints) != expected:
        raise ValueError(f"Incomplete checkpoint matrix in {path}")
    return final, checkpoints


def validate_pair(variant, current, reference):
    if set(current) != set(reference):
        raise ValueError(f"Checkpoint set differs for {variant}")
    for key, row in current.items():
        base = reference[key]
        for field in ("seed", "checkpoint_ops", "loaded_count", "live_count",
                      "query_count", "answers"):
            if row[field] != base[field]:
                raise ValueError(f"Unmatched {field} for {variant}: {key}")
        expected_prl = "0" if variant == "fixed_layout_no_prl" else "1"
        if row["predicate_shortcuts_enabled"] != expected_prl:
            raise ValueError(f"Incorrect PRL setting for {variant}: {key}")
        if variant == "fixed_layout_no_prl" and row["predicate_shortcuts"] != "0":
            raise ValueError(f"PRL-off run still reports shortcuts: {key}")


def write_figure(aggregates, variants, out, stem="deli_paper_ablation_ratios"):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    compared = [variant for variant in variants if variant != "full"]
    if not compared:
        return
    names = ("Fixed\nlayout", "Conditional\nrefresh", "Fixed\nbudget", "No PRL\n(fixed layout)")
    titles = ("P95 query latency", "P95 deletion latency", "Cumulative summary time", "Build time")
    metrics = ("p95_query_ms", "p95_delete_ms", "summary_rebuild_ns_total", "build_ms")
    style = {v: (name, color, hatch) for v, name, color, hatch in zip(
        VARIANTS[1:], names, ("#4C8CBF", "#C87631", "#8B6DA7", "#B45360"),
        ("//", "xx", "..", "\\\\"))}
    fig, axes = plt.subplots(2, 2, figsize=(7.0, 4.0))
    for axis, metric, title in zip(axes.flat, metrics, titles):
        values = [aggregates[(variant, metric)] for variant in compared]
        if any(value is None for value in values):
            raise ValueError("Required latency/build ratio is missing")
        for position, (variant, value) in enumerate(zip(compared, values)):
            _, color, hatch = style[variant]
            axis.bar(position, value, color=color, hatch=hatch, edgecolor="#303030",
                     linewidth=0.4, width=0.65)
        axis.axhline(1, color="#444444", linestyle="--", linewidth=0.7)
        axis.set_xticks(range(len(compared)))
        axis.set_xticklabels([style[v][0] for v in compared], fontsize=7)
        axis.set_ylabel("Ratio to paired reference", fontsize=8)
        axis.set_title(title, fontsize=9)
        axis.set_ylim(0, max(1.15, max(values) * 1.15))
        axis.tick_params(axis="y", labelsize=7)
        axis.spines[["top", "right"]].set_visible(False)
    fig.tight_layout()
    fig.savefig(out / f"{stem}.pdf")
    fig.savefig(out / f"{stem}.png", dpi=220)
    plt.close(fig)



def write_design_summary(aggregates, variants, out):
    """Keep structural attribution separate from generic predicate refinement."""
    structural = [variant for variant in variants if variant != "fixed_layout_no_prl"]
    write_figure(aggregates, structural, out, stem="deli_paper_design_ratios")
    metrics = TABLE_METRICS[:-1]
    table = [
        r"\begin{tabular}{@{}lrrrrrr@{}}", r"\toprule",
        r"Variant & Query P95 & Insert P95 & Delete P95 & Build & Index & Summary time \\",
        r"\midrule",
    ]
    for variant in structural:
        values = [aggregates[(variant, metric)] for metric in metrics]
        cells = ["--" if value is None else f"{value:.3f}" for value in values]
        table.append(" & ".join([LABELS[variant]] + cells) + r" \\")
    table.extend([r"\bottomrule", r"\end{tabular}"])
    (out / "deli_paper_design_table.tex").write_text(
        "\n".join(table) + "\n", encoding="utf-8")


def write_prl_details(grouped, out):
    """Expose every PRL pair; the absolute-value table uses the read-heavy slice."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    on_rows, off_rows = grouped["fixed_layout"], grouped["fixed_layout_no_prl"]
    details = []
    for key in sorted(on_rows):
        on, off = on_rows[key], off_rows[key]
        query_count = int(on["query_count"])
        if query_count <= 0:
            raise ValueError("PRL diagnostics require positive query counts")
        raw_selectivity = on["selectivity"]
        percent = float(raw_selectivity.rstrip("%"))
        if not raw_selectivity.endswith("%"):
            percent *= 100
        record = {
            "dataset": key[0], "mixed_profile": key[1], "selectivity_tag": key[2],
            "nominal_selectivity_percent": percent, "query_count": query_count,
            "candidates_per_query": float(on["candidates"]) / query_count,
        }
        for metric, source, scale in (
                ("p95_query_us", "p95_query_ms", 1000),
                ("exact_calls_per_query", "exact_calls", 1 / query_count),
                ("geos_us_per_query", "geos_exact_ms", 1000 / query_count)):
            left, right = float(on[source]) * scale, float(off[source]) * scale
            record[metric + "_on"] = left
            record[metric + "_off"] = right
            record[metric + "_off_on"] = right / left if left > 0 else None
        details.append(record)
    with (out / "deli_paper_ablation_prl_details.csv").open(
            "w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(details[0]))
        writer.writeheader()
        writer.writerows(details)

    tags = sorted({row["selectivity_tag"] for row in details}, key=lambda tag: next(
        row["nominal_selectivity_percent"] for row in details if row["selectivity_tag"] == tag))
    tag_labels = [f"{next(row['nominal_selectivity_percent'] for row in details if row['selectivity_tag'] == tag):g}%"
                  for tag in tags]
    datasets = sorted({row["dataset"] for row in details})
    profiles = sorted({row["mixed_profile"] for row in details})
    groups = [(dataset, profile) for dataset in datasets for profile in profiles]
    values_by_key = {(row["dataset"], row["mixed_profile"], row["selectivity_tag"]): row
                     for row in details}
    colors = ("#147D64", "#4C8CBF", "#C87631", "#8B6DA7")
    hatches = ("", "//", "xx", "..")
    metrics = ("p95_query_us", "exact_calls_per_query", "geos_us_per_query")
    titles = ("P95 query latency", "Exact calls/query", "GEOS time/query")
    fig, axes = plt.subplots(1, 3, figsize=(7.1, 2.6))
    width = 0.8 / len(groups)
    for axis, metric, title in zip(axes, metrics, titles):
        for position, group in enumerate(groups):
            for tag_position, tag in enumerate(tags):
                value = values_by_key[group + (tag,)][metric + "_off_on"]
                x = tag_position + (position - (len(groups) - 1) / 2) * width
                label = f"{group[0]} {group[1].replace('_heavy', '')}"
                if value is None:
                    axis.text(x, 1, "n/a", rotation=90, fontsize=6)
                    continue
                axis.bar(x, value, width=width, color=colors[position % len(colors)],
                         hatch=hatches[position % len(hatches)], edgecolor="#303030",
                         linewidth=0.35, label=label if tag_position == 0 else None)
        axis.axhline(1, linestyle="--", linewidth=0.7, color="#444444")
        axis.set_xticks(range(len(tags)))
        axis.set_xticklabels(tag_labels, fontsize=7)
        axis.set_title(title, fontsize=9)
        axis.set_xlabel("Nominal selectivity", fontsize=7)
        axis.tick_params(axis="y", labelsize=7)
        axis.spines[["top", "right"]].set_visible(False)
        if metric != "p95_query_us":
            axis.set_yscale("log")
            axis.set_ylim(bottom=0.8)
    axes[0].set_ylabel("No PRL / PRL (above 1 favors PRL)", fontsize=8)
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=min(4, len(groups)),
               frameon=False, fontsize=7)
    fig.tight_layout(rect=(0, 0, 1, 0.87))
    fig.savefig(out / "deli_paper_ablation_prl.pdf")
    fig.savefig(out / "deli_paper_ablation_prl.png", dpi=220)
    plt.close(fig)

    profile = "read_heavy" if "read_heavy" in profiles else profiles[0]
    table = [
        r"\begin{tabular}{@{}llrrrrrr@{}}", r"\toprule",
        r" & & \multicolumn{2}{c}{Exact calls/query} & \multicolumn{2}{c}{GEOS time/query ($\mu$s)} & \multicolumn{2}{c}{Query P95 ($\mu$s)} \\",
        r"Dataset & Nominal sel. & On & Off & On & Off & On & Off \\", r"\midrule",
    ]
    for dataset in datasets:
        for tag in tags:
            row = values_by_key[(dataset, profile, tag)]
            cells = [f"{row['nominal_selectivity_percent']:g}" + r"\%"]
            for metric in ("exact_calls_per_query", "geos_us_per_query", "p95_query_us"):
                cells.extend(f"{row[metric + suffix]:.2f}" for suffix in ("_on", "_off"))
            table.append(" & ".join([dataset] + cells) + r" \\")
    table.extend([r"\bottomrule", r"\end{tabular}"])
    (out / "deli_paper_ablation_prl_table.tex").write_text(
        "\n".join(table) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--result-root", type=Path, required=True)
    parser.add_argument("--variants", nargs="+", choices=VARIANTS, default=list(VARIANTS))
    parser.add_argument("--fixed-budget", type=float, default=0.25)
    parser.add_argument("--figure-dir", type=Path)
    args = parser.parse_args()
    variants = args.variants
    if len(set(variants)) != len(variants):
        raise ValueError("Duplicate variants")
    needed = set(variants) | {BASELINES[v] for v in variants} | {"full"}
    grouped, checkpoints = {}, {}
    for variant in sorted(needed):
        grouped[variant], checkpoints[variant] = read_variant(
            args.result_root / variant / "dynamic_compare_summary.csv")
    for variant in needed:
        validate_pair(variant, checkpoints[variant], checkpoints["full"])
    if "fixed_layout_no_prl" in needed:
        for key, row in checkpoints["fixed_layout_no_prl"].items():
            fixed = checkpoints["fixed_layout"][key]
            for field in ("block_count", "local_compaction_count_total",
                          "summary_rebuild_count_total", "candidates",
                          "compact_records_scanned", "delta_records_scanned"):
                if row[field] != fixed[field]:
                    raise ValueError(f"PRL pair changed structural counter {field}: {key}")
    if "fixed_budget" in needed:
        for row in checkpoints["fixed_budget"].values():
            if any(not math.isclose(float(row[field]), args.fixed_budget, abs_tol=1e-6)
                   for field in ("avg_beta", "avg_tau")):
                raise ValueError("Fixed-budget run did not actually freeze beta/tau")
    keys = set(grouped["full"])
    output = args.result_root / "deli_paper_ablation_summary.csv"
    fields = ("dataset", "mixed_profile", "selectivity_tag", "variant", "reference_variant",
              "operation_count", "seed", "answers_match_boost") + METRICS + tuple(
        metric + "_vs_reference" for metric in METRICS)
    with output.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for key in sorted(keys):
            for variant in variants:
                row, base = grouped[variant][key], grouped[BASELINES[variant]][key]
                record = dict(zip(fields[:3], key))
                record.update(variant=variant, reference_variant=BASELINES[variant],
                              operation_count=row["operation_count"], seed=row["seed"],
                              answers_match_boost=row["answers_match_boost"])
                for metric in METRICS:
                    current, reference = float(row[metric]), float(base[metric])
                    record[metric] = current
                    record[metric + "_vs_reference"] = current / reference if reference > 0 else ""
                writer.writerow(record)
    aggregates = {}
    for variant in variants:
        for metric in METRICS:
            ratios = [float(grouped[variant][key][metric]) /
                      float(grouped[BASELINES[variant]][key][metric]) for key in keys
                      if float(grouped[BASELINES[variant]][key][metric]) > 0]
            aggregates[(variant, metric)] = median(ratios) if ratios else None
    aggregate_path = args.result_root / "deli_paper_ablation_aggregate.csv"
    with aggregate_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(("variant", "reference_variant", "metric", "median_scenario_ratio",
                         "matched_scenarios", "nonzero_reference_scenarios"))
        for variant in variants:
            for metric in METRICS:
                nonzero = sum(float(grouped[BASELINES[variant]][key][metric]) > 0 for key in keys)
                value = aggregates[(variant, metric)]
                writer.writerow((variant, BASELINES[variant], metric,
                                 "" if value is None else value, len(keys), nonzero))
    if args.figure_dir:
        args.figure_dir.mkdir(parents=True, exist_ok=True)
        write_figure(aggregates, variants, args.figure_dir)
        write_design_summary(aggregates, variants, args.figure_dir)
        table = [r"\begin{tabular}{@{}llrrrrrrr@{}}", r"\toprule",
                 r"Variant & Reference & Query P95 & Insert P95 & Delete P95 & Build & Index & Summary time & Exact calls \\",
                 r"\midrule"]
        for variant in variants:
            values = [aggregates[(variant, metric)] for metric in TABLE_METRICS]
            cells = ["--" if value is None else f"{value:.3f}" for value in values]
            table.append(" & ".join([LABELS[variant], LABELS[BASELINES[variant]]] + cells) + r" \\")
        table.extend([r"\bottomrule", r"\end{tabular}"])
        (args.figure_dir / "deli_paper_ablation_table.tex").write_text(
            "\n".join(table) + "\n", encoding="utf-8")
        if "fixed_layout_no_prl" in variants:
            write_prl_details(grouped, args.figure_dir)
    print(f"Ablation scenarios: {len(keys)}; summary: {output}; aggregate: {aggregate_path}")
    print("Ratios are medians of matched scenarios, not medians of independent repetitions.")
    for variant in variants:
        print(variant, "vs", BASELINES[variant])
        for metric in TABLE_METRICS:
            value = aggregates[(variant, metric)]
            print(f"  {metric}: {value:.3f}" if value is not None else f"  {metric}: n/a")


if __name__ == "__main__":
    main()
