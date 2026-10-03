#!/usr/bin/env python3
"""Compare updates and expose measured query/maintenance work, without rerunning indexes."""

import argparse
import csv
import math
from pathlib import Path
from statistics import median

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

from plot_deli_paper_interim import BOOST, COMPARISON_METHODS, DELI, PROFILES, load_final_rows

PROFILE_NAMES = {"read_heavy": "Read-heavy", "balanced": "Balanced", "write_heavy": "Write-heavy"}
UPDATE_METRICS = (
    ("p95_insert_ms", "Insert P95 ($\\mu$s)", 1000),
    ("p95_delete_ms", "Delete P95 ($\\mu$s)", 1000),
    ("p99_insert_ms", "Insert P99 ($\\mu$s)", 1000),
    ("p99_delete_ms", "Delete P99 ($\\mu$s)", 1000),
)
SERVICE_METRICS = (
    ("insert_tps", "Insert rate (Kops/s)", 0.001),
    ("delete_tps", "Delete rate (Kops/s)", 0.001),
)
ALL_UPDATE_METRICS = UPDATE_METRICS + SERVICE_METRICS
METHOD_STYLE = {method: (name, color, hatch) for method, name, color, hatch in COMPARISON_METHODS}


def numeric(row, field, positive=False):
    value = float(row[field])
    if not math.isfinite(value) or value < 0 or (positive and value == 0):
        raise ValueError("Invalid " + field + ": " + str(row))
    return value


def write_csv(path, records):
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(records[0]))
        writer.writeheader()
        writer.writerows(records)


def save(fig, out, stem):
    fig.savefig(out / (stem + ".pdf"))
    fig.savefig(out / (stem + ".png"), dpi=220)
    plt.close(fig)


def update_records(final, datasets, tags):
    records = []
    for dataset in datasets:
        for profile in PROFILES:
            for method, _, _, _ in COMPARISON_METHODS:
                record = {"dataset": dataset, "mixed_profile": profile, "index": method,
                          "selectivities_aggregated": len(tags), "statistic": "scenario median"}
                for metric, _, _ in ALL_UPDATE_METRICS:
                    values = [numeric(final[(dataset, profile, tag, method)], metric, True)
                              for tag in tags]
                    record[metric + "_median"] = median(values)
                    record[metric + "_min"] = min(values)
                    record[metric + "_max"] = max(values)
                records.append(record)
    return records


def update_figure(final, out, datasets, tags, stem, selectivity=None,
                  metrics=UPDATE_METRICS):
    fig, axes = plt.subplots(len(metrics), 3, figsize=(7.2, 1.43 * len(metrics)),
                             squeeze=False)
    for r, (metric, title, scale) in enumerate(metrics):
        values_for = lambda d, p, m: (
            numeric(final[(d, p, selectivity, m)], metric, True) * scale if selectivity else
            median(numeric(final[(d, p, tag, m)], metric, True) for tag in tags) * scale)
        all_values = [values_for(d, p, m) for d in datasets for p in PROFILES
                      for m, _, _, _ in COMPARISON_METHODS]
        for c, profile in enumerate(PROFILES):
            axis = axes[r, c]
            for n, (method, label, color, hatch) in enumerate(COMPARISON_METHODS):
                axis.bar([x + (n - 2.5) * 0.13 for x in range(len(datasets))],
                         [values_for(d, profile, method) for d in datasets], width=0.13,
                         color=color, hatch=hatch, label=label, edgecolor="#333333", linewidth=0.3)
            axis.set_yscale("log")
            axis.set_ylim(min(all_values) / 1.3, max(all_values) * 1.5)
            axis.set_xticks(range(len(datasets)))
            axis.set_xticklabels(datasets, fontsize=7)
            axis.tick_params(axis="y", labelsize=7)
            axis.grid(axis="y", color="#DDDDDD", linewidth=0.5)
            axis.set_axisbelow(True)
            axis.spines[["top", "right"]].set_visible(False)
            if c == 0:
                axis.set_ylabel(title, fontsize=7.5)
            if r == 0:
                axis.set_title(PROFILE_NAMES[profile], fontsize=8)
    handles, labels = axes[0, 0].get_legend_handles_labels()
    fig.legend(handles, labels, ncol=3, frameon=False, fontsize=7,
               loc="upper center", bbox_to_anchor=(0.51, 1))
    fig.tight_layout(rect=(0, 0, 1, 0.935), h_pad=0.7, w_pad=0.7)
    save(fig, out, stem)


def attribution_records(final, datasets, tags):
    records = []
    for dataset in datasets:
        for profile in PROFILES:
            for tag in tags:
                for method in (DELI, BOOST):
                    row = final[(dataset, profile, tag, method)]
                    queries = numeric(row, "query_count", True)
                    mean_us = numeric(row, "avg_query_ns", True) / 1000
                    geos_us = numeric(row, "geos_exact_ns") / queries / 1000
                    if geos_us > mean_us:
                        raise ValueError("GEOS time exceeds total query time")
                    candidates = numeric(row, "mbr_candidates")
                    exact, shortcuts = numeric(row, "exact_calls"), numeric(row, "predicate_shortcuts")
                    if candidates != exact + shortcuts:
                        raise ValueError("Refinement accounting differs from MBR candidates")
                    records.append({
                        "dataset": dataset, "mixed_profile": profile, "selectivity_tag": tag,
                        "index": method, "query_count": int(queries), "mean_query_us": mean_us,
                        "geos_us_per_query": geos_us, "other_query_us": mean_us - geos_us,
                        "geos_fraction_of_mean": geos_us / mean_us,
                        "mbr_candidates_per_query": candidates / queries,
                        "exact_calls_per_query": exact / queries,
                        "shortcuts_per_query": shortcuts / queries,
                        "compact_scanned_per_query": numeric(row, "compact_records_scanned") / queries
                        if method == DELI else None,
                        "delta_scanned_per_query": numeric(row, "delta_records_scanned") / queries
                        if method == DELI else None,
                    })
    return records


def attribution_figure(records, out, datasets, tags, labels, stem):
    lookup = {(r["dataset"], r["mixed_profile"], r["selectivity_tag"], r["index"]): r
              for r in records}
    fig, axes = plt.subplots(len(datasets), 3, figsize=(7.2, 4.3), squeeze=False)
    for r, dataset in enumerate(datasets):
        axis = axes[r, 0]
        scans = []
        for n, (field, label, color, hatch) in enumerate((
                ("compact_scanned_per_query", "Compact IDs", "#4C8CBF", ""),
                ("delta_scanned_per_query", "Delta IDs", "#C87631", "//"))):
            values = [lookup[(dataset, "read_heavy", tag, DELI)][field] for tag in tags]
            scans.extend(values)
            axis.bar([x + (n - 0.5) * 0.32 for x in range(len(tags))], values,
                     width=0.32, color=color, hatch=hatch, edgecolor="#333333", linewidth=0.3,
                     label=label)
        axis.set_yscale("log")
        axis.set_ylim(min(scans) / 1.5, max(scans) * 3)
        axis.set_title(dataset + " / DELI scan work", fontsize=8)
        axis.set_ylabel("IDs scanned / query", fontsize=7.5)
        axis.legend(fontsize=6, frameon=False, ncol=2, loc="upper left")
        axis = axes[r, 1]
        work = []
        for m, method in enumerate((DELI, BOOST)):
            name, color, _ = METHOD_STYLE[method]
            for k, (field, suffix, hatch) in enumerate((
                    ("mbr_candidates_per_query", "MBR", ""),
                    ("exact_calls_per_query", "exact", "xx"))):
                values = [lookup[(dataset, "read_heavy", tag, method)][field] for tag in tags]
                work.extend(values)
                axis.bar([x + (m * 2 + k - 1.5) * 0.18 for x in range(len(tags))], values,
                         width=0.18, color=color, hatch=hatch, edgecolor="#333333", linewidth=0.3,
                         label=name.replace(" R-tree", "") + " " + suffix)
        axis.set_yscale("log")
        axis.set_ylim(min(work) / 1.5, max(work) * 40)
        axis.set_title(dataset + " / refinement work", fontsize=8)
        axis.set_ylabel("Count / query", fontsize=7.5)
        axis.legend(fontsize=6, frameon=False, ncol=2, loc="upper left", columnspacing=0.6)
        axis = axes[r, 2]
        for m, method in enumerate((DELI, BOOST)):
            name, color, hatch = METHOD_STYLE[method]
            fractions = [100 * lookup[(dataset, "read_heavy", tag, method)]["geos_fraction_of_mean"]
                         for tag in tags]
            x = [n + (m - 0.5) * 0.34 for n in range(len(tags))]
            axis.bar(x, fractions, width=0.34, color="#8B6DA7", hatch=hatch,
                     edgecolor="#333333", linewidth=0.3, label="GEOS exact" if m == 0 else None)
            axis.bar(x, [100 - v for v in fractions], bottom=fractions, width=0.34,
                     color=color, hatch=hatch, edgecolor="#333333", linewidth=0.3,
                     label=name.replace(" R-tree", "") + " other")
        axis.set_ylim(0, 135)
        axis.set_yticks([0, 50, 100])
        axis.set_title(dataset + " / measured time share", fontsize=8)
        axis.set_ylabel("Share of mean query time (%)", fontsize=7.5)
        axis.legend(fontsize=6, frameon=False, ncol=3, loc="upper left", columnspacing=0.5,
                    handlelength=1)
        for c, axis in enumerate(axes[r]):
            axis.set_xticks(range(len(tags)))
            tick_labels = labels
            if c == 2:
                tick_labels = []
                for tag, label in zip(tags, labels):
                    means = [lookup[(dataset, "read_heavy", tag, method)]["mean_query_us"]
                             for method in (DELI, BOOST)]
                    tick_labels.append(label + "\n" + "\n".join(
                        prefix + " " + format(value, ".0f" if value >= 1000 else ".3g")
                        for prefix, value in zip(("D", "B"), means)))
            axis.set_xticklabels(tick_labels, fontsize=6.5)
            axis.tick_params(axis="y", labelsize=7)
            axis.spines[["top", "right"]].set_visible(False)
            if r == len(datasets) - 1:
                axis.set_xlabel("Nominal selectivity" + ("\nMean $\\mu$s: D = DELI, B = Boost" if c == 2 else ""),
                                fontsize=7)
    fig.tight_layout(h_pad=1.1, w_pad=0.5)
    save(fig, out, stem)


def process_records(source, datasets, tags):
    with source.open(newline="") as stream:
        rows = [r for r in csv.DictReader(stream) if r["index"] in (DELI, BOOST)]
    lookup = {(r["dataset"], r["mixed_profile"], r["selectivity_tag"], r["index"],
               int(r["operation_count"])): r for r in rows}
    operations = sorted({int(row["operation_count"]) for row in rows})
    expected = {(d, p, t, m, o) for d in datasets for p in PROFILES for t in tags
                for m in (DELI, BOOST) for o in operations}
    if len(lookup) != len(rows) or set(lookup) != expected:
        raise ValueError("Incomplete or duplicated process series")
    records = []
    for dataset in datasets:
        for profile in PROFILES:
            for tag in tags:
                previous_count = previous_ns = 0
                for operation in operations:
                    deli = lookup[(dataset, profile, tag, DELI, operation)]
                    boost = lookup[(dataset, profile, tag, BOOST, operation)]
                    for field in ("query_count", "live_count", "seed", "checkpoint_ops"):
                        if deli[field] != boost[field]:
                            raise ValueError("Unmatched process field: " + field)
                    count = numeric(deli, "local_compaction_count_total")
                    ns = numeric(deli, "local_compaction_ns_total")
                    if (count < previous_count or ns < previous_ns
                            or count - previous_count != numeric(deli, "local_compaction_count_stage")
                            or ns - previous_ns != numeric(deli, "local_compaction_ns_stage")):
                        raise ValueError("Invalid maintenance-window differencing")
                    average = numeric(deli, "avg_local_delta_size")
                    maximum = numeric(deli, "max_local_delta_size")
                    if average > maximum:
                        raise ValueError("Average Delta exceeds maximum")
                    records.append({
                        "dataset": dataset, "mixed_profile": profile, "selectivity_tag": tag,
                        "operation_count": operation, "checkpoint_ops": int(deli["checkpoint_ops"]),
                        "deli_query_p95_us": numeric(deli, "p95_query_ns", True) / 1000,
                        "boost_query_p95_us": numeric(boost, "p95_query_ns", True) / 1000,
                        "avg_live_delta_ids_per_block": average,
                        "max_live_delta_ids_per_block": maximum,
                        "compactions_window": int(count - previous_count),
                        "compaction_ms_window": (ns - previous_ns) / 1e6,
                        "compactions_total": int(count), "compaction_ms_total": ns / 1e6,
                    })
                    previous_count, previous_ns = count, ns
    return records


def process_figure(records, out, datasets, stem):
    fig, axes = plt.subplots(len(datasets), 4, figsize=(7.2, 3.6), squeeze=False)
    for r, dataset in enumerate(datasets):
        selected = [row for row in records if row["dataset"] == dataset
                    and row["mixed_profile"] == "write_heavy" and row["selectivity_tag"] == "0p1pct"]
        x = [row["operation_count"] / 1000 for row in selected]
        for field, label, color, marker, linestyle in (
                ("deli_query_p95_us", "DELI", "#147D64", "o", "-"),
                ("boost_query_p95_us", "Boost", "#4E5968", "s", "--")):
            axes[r, 0].plot(x, [row[field] for row in selected], label=label,
                            color=color, linewidth=1, marker=marker, markersize=3,
                            markevery=4, linestyle=linestyle)
        axes[r, 0].set_ylabel("Query P95 ($\\mu$s)", fontsize=7)
        query_values = [row[field] for row in selected
                        for field in ("deli_query_p95_us", "boost_query_p95_us")]
        low, high = min(query_values), max(query_values)
        span = max(high - low, 1)
        axes[r, 0].set_ylim(max(0, low - 0.05 * span), high + 0.25 * span)
        axes[r, 0].legend(fontsize=6, frameon=False, ncol=2, loc="upper left",
                          handlelength=1, columnspacing=0.7)
        for field, label, color, linestyle in (
                ("avg_live_delta_ids_per_block", "Mean", "#147D64", "-"),
                ("max_live_delta_ids_per_block", "Max.", "#C87631", "--")):
            axes[r, 1].plot(x, [row[field] for row in selected], label=label,
                            color=color, linewidth=1, linestyle=linestyle)
        axes[r, 1].set_ylabel("Live Delta IDs / block", fontsize=7)
        axes[r, 1].legend(fontsize=6.5, frameon=False)
        axes[r, 2].bar(x, [row["compactions_window"] for row in selected], width=6,
                       color="#4C8CBF", edgecolor="#333333", linewidth=0.3)
        axes[r, 2].set_ylabel("Compactions / window", fontsize=7)
        axes[r, 3].bar(x, [row["compaction_ms_window"] for row in selected], width=6,
                       color="#8B6DA7", edgecolor="#333333", linewidth=0.3)
        axes[r, 3].set_ylabel("Compaction ms / window", fontsize=7)
        for c, axis in enumerate(axes[r]):
            axis.set_title(dataset, fontsize=8)
            axis.set_xticks([50, 100, 150, 200])
            axis.set_xlim(4, 206)
            axis.tick_params(labelsize=7)
            axis.spines[["top", "right"]].set_visible(False)
            if c in (1, 2, 3):
                axis.set_ylim(bottom=0)
            if r == len(datasets) - 1:
                axis.set_xlabel("Operations (K)", fontsize=7)
    fig.tight_layout(h_pad=1, w_pad=0.6)
    save(fig, out, stem)


def build_mechanism_views(source, final, out, datasets, tags, tag_labels):
    aggregated = update_records(final, datasets, tags)
    attribution = attribution_records(final, datasets, tags)
    process = process_records(source, datasets, tags)
    write_csv(out / "update_scenario_medians.csv", aggregated)
    write_csv(out / "query_attribution_all.csv", attribution)
    write_csv(out / "dynamic_process_all.csv", process)
    update_figure(final, out, datasets, tags, "deli_update_all_methods")
    update_figure(final, out, datasets, tags, "deli_update_service_rates",
                  metrics=SERVICE_METRICS)
    for tag in tags:
        update_figure(final, out, datasets, tags, "deli_update_absolute_" + tag, selectivity=tag)
        update_figure(final, out, datasets, tags, "deli_update_service_absolute_" + tag,
                      selectivity=tag, metrics=SERVICE_METRICS)
    for subset, suffix in ((["AW", "ROADS"], "aw_roads"), (["LW", "PARKS"], "lw_parks")):
        attribution_figure(attribution, out, subset, tags, tag_labels, "deli_query_attribution_" + suffix)
        process_figure(process, out, subset, "deli_dynamic_process_" + suffix)
    rlr = "RLR_LITE_CS_SPLIT"
    wins = {}
    for field, _, _ in ALL_UPDATE_METRICS:
        higher = field.endswith("_tps")
        wins[field] = sum(
            numeric(final[(d, p, t, rlr)], field) > numeric(final[(d, p, t, DELI)], field)
            if higher else numeric(final[(d, p, t, rlr)], field) < numeric(final[(d, p, t, DELI)], field)
            for d in datasets for p in PROFILES for t in tags)
    return {
        "updates": {"methods": 6, "scenario_records": len(final), "aggregation_rows": len(aggregated),
                    "aggregation": "Median of four nominal-selectivity scenarios per dataset/mix/method",
                    "rlr_better_than_deli_by_metric": wins},
        "query_attribution": {"rows": len(attribution), "methods": [DELI, BOOST],
                              "main_datasets": ["AW", "ROADS"], "main_profile": "read_heavy",
                              "time_basis": "Mean query minus measured GEOS time, not a P95 decomposition",
                              "unmeasured_scans": [BOOST], "geos_uninstrumented_baseline": "GLIN_PIECEWISE"},
        "dynamic_process": {"rows": len(process), "main_datasets": ["AW", "ROADS"],
                            "profile": "write_heavy", "selectivity_tag": "0p1pct", "windows": 20,
                            "maintenance": "Differenced cumulative counters; summary and compaction times not added"},
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--summary", type=Path,
                        default=Path("results/deli_main8_compare_0.5m/dynamic_compare_summary.csv"))
    parser.add_argument("--out", type=Path,
                        default=Path("manuscript/2026.8.28/pvldbstyle-master/figures/full_matrix_main8"))
    args = parser.parse_args()
    final, _, datasets, tags, labels, _ = load_final_rows(args.summary)
    args.out.mkdir(parents=True, exist_ok=True)
    plt.rcParams.update({"font.family": "DejaVu Sans", "pdf.fonttype": 42, "hatch.linewidth": 0.35})
    report = build_mechanism_views(args.summary, final, args.out, datasets, tags, labels)
    print(report)


if __name__ == "__main__":
    main()
