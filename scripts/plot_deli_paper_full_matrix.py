#!/usr/bin/env python3
"""Generate coverage-complete paper views from archived main8 results."""

import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
from statistics import median

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import Normalize, TwoSlopeNorm

from plot_deli_paper_interim import (
    BOOST, COMPARISON_METHODS, DELI, PROFILES, load_final_rows, ratio,
)
from summarize_deli_paper_ablation import read_variant, validate_pair
from plot_deli_paper_mechanisms import build_mechanism_views
from characterize_deli_datasets import validate as validate_dataset

PROFILE_NAMES = ("Read-heavy", "Balanced", "Write-heavy")
METRICS = (
    ("p95_query_ms", 1000), ("p95_insert_ms", 1000), ("p95_delete_ms", 1000),
    ("query_tps", 0.001), ("insert_tps", 0.001), ("delete_tps", 0.001),
    ("overall_ops_tps", 0.001), ("build_ms", 1), ("index_mb_estimate", 1),
)


def fingerprint(path):
    return {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def save(fig, out, stem):
    fig.savefig(out / (stem + ".pdf"))
    fig.savefig(out / (stem + ".png"), dpi=220)
    plt.close(fig)


def comparison_grid(rows, out, datasets, tags, tag_labels, metric, scale, ylabel, stem):
    height = 4.15 if len(datasets) == 2 else 6.35
    fig, axes = plt.subplots(len(datasets), 3, figsize=(7.2, height), squeeze=False)
    for r, dataset in enumerate(datasets):
        row_values = [float(rows[(dataset, p, t, method)][metric]) * scale
                      for p in PROFILES for t in tags
                      for method, _, _, _ in COMPARISON_METHODS]
        for c, profile in enumerate(PROFILES):
            axis = axes[r, c]
            for m, (method, label, color, hatch) in enumerate(COMPARISON_METHODS):
                values = [float(rows[(dataset, profile, tag, method)][metric]) * scale
                          for tag in tags]
                axis.bar([x + (m - 2.5) * 0.13 for x in range(len(tags))], values,
                         width=0.13, label=label, color=color, hatch=hatch,
                         edgecolor="#333333", linewidth=0.3, zorder=3)
            axis.set_yscale("log")
            axis.set_ylim(min(row_values) / 1.3, max(row_values) * 1.5)
            axis.set_xticks(range(len(tags)))
            axis.set_xticklabels(tag_labels, fontsize=7)
            axis.tick_params(axis="y", labelsize=7)
            axis.grid(axis="y", which="major", color="#DDDDDD", linewidth=0.5, zorder=0)
            axis.spines[["top", "right"]].set_visible(False)
            axis.set_title(dataset + " / " + PROFILE_NAMES[c], fontsize=8)
            if c == 0:
                axis.set_ylabel(ylabel, fontsize=7.5)
            if r == len(datasets) - 1:
                axis.set_xlabel("Nominal selectivity", fontsize=7.5)
    handles, names = axes[0, 0].get_legend_handles_labels()
    fig.legend(handles, names, loc="upper center", ncol=3, fontsize=7,
               frameon=False, bbox_to_anchor=(0.51, 1), columnspacing=1.4)
    fig.tight_layout(rect=(0, 0, 1, 0.91 if len(datasets) == 2 else 0.935),
                     h_pad=0.9, w_pad=0.6)
    save(fig, out, stem)


def ratio_maps(rows, out, datasets, tags, tag_labels, metrics, titles, stem):
    query = len(metrics) == 1
    fig, axes = plt.subplots(len(metrics), 3, figsize=(7.2, 2.75 if query else 4.1),
                             squeeze=False)
    values = [ratio(rows, d, p, t, metric) for metric in metrics
              for d in datasets for p in PROFILES for t in tags]
    if query:
        norm, cmap, extend = TwoSlopeNorm(vmin=0.5, vcenter=1, vmax=2), "RdYlGn_r", "both"
    else:
        norm, cmap, extend = Normalize(vmin=0, vmax=max(1, max(values))), "YlGn_r", "neither"
    for r, (metric, title) in enumerate(zip(metrics, titles)):
        for c, profile in enumerate(PROFILES):
            axis = axes[r, c]
            matrix = [[ratio(rows, d, profile, t, metric) for t in tags] for d in datasets]
            picture = axis.imshow(matrix, cmap=cmap, norm=norm, aspect="auto")
            axis.set_xticks(range(len(tags)))
            axis.set_xticklabels(tag_labels, fontsize=7)
            axis.set_yticks(range(len(datasets)))
            axis.set_yticklabels(datasets, fontsize=7)
            axis.tick_params(length=0)
            axis.set_title(PROFILE_NAMES[c] + (" / " + title if not query else ""), fontsize=8)
            if r == len(metrics) - 1:
                axis.set_xlabel("Nominal selectivity", fontsize=7.5)
            for y, row in enumerate(matrix):
                for x, value in enumerate(row):
                    red, green, blue, _ = plt.get_cmap(cmap)(norm(value))
                    luminance = 0.2126 * red + 0.7152 * green + 0.0722 * blue
                    foreground = "white" if luminance < 0.42 else "#111111"
                    axis.text(x, y, f"{value:.2f}", ha="center", va="center",
                              fontsize=7, color=foreground)

    fig.subplots_adjust(left=0.09, right=0.99, top=0.90, bottom=0.32 if query else 0.23,
                        wspace=0.35, hspace=0.48)
    color_axis = fig.add_axes((0.32, 0.14 if query else 0.10, 0.39, 0.025))
    bar = fig.colorbar(picture, cax=color_axis, orientation="horizontal", extend=extend)
    bar.set_label("P95 latency: DELI / Boost (below 1 favors DELI)", fontsize=7.5)
    bar.ax.tick_params(labelsize=7)
    save(fig, out, stem)


def resource_tables(rows, out, datasets, tags):
    resources = []
    for method, name, _, _ in COMPARISON_METHODS:
        for dataset in datasets:
            resource = {"dataset": dataset, "index": method, "method": name,
                        "scenarios": len(PROFILES) * len(tags)}
            for field in ("build_ms", "index_mb_estimate"):
                values = [float(rows[(dataset, profile, tag, method)][field])
                          for profile in PROFILES for tag in tags]
                resource.update({field + "_median": median(values),
                                 field + "_min": min(values), field + "_max": max(values)})
            resources.append(resource)
    lookup = {(row["index"], row["dataset"]): row for row in resources}
    lines = [r"\begin{tabular}{@{}l" + "rr" * len(datasets) + "@{}}", r"\toprule",
             "Method & " + " & ".join(r"\multicolumn{2}{c}{" + d + "}" for d in datasets) + r" \\",
             " & " + " & ".join([r"Build (ms) & Index (MiB)"] * len(datasets)) + r" \\",
             r"\midrule"]
    for method, name, _, _ in COMPARISON_METHODS:
        cells = [name]
        for dataset in datasets:
            row = lookup[(method, dataset)]
            cells += [f"{row['build_ms_median']:.1f}", f"{row['index_mb_estimate_median']:.2f}"]
        lines.append(" & ".join(cells) + r" \\")
    lines += [r"\bottomrule", r"\end{tabular}"]
    (out / "deli_all_method_resources.tex").write_text("\n".join(lines) + "\n")
    with (out / "resource_scenario_ranges.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(resources[0]))
        writer.writeheader()
        writer.writerows(resources)
    return resources


def maintenance_figure(root, out):
    variants = ("full", "fixed_layout", "conditional_refresh", "fixed_budget")
    sources = [root / variant / "dynamic_compare_summary.csv" for variant in variants]
    loaded = {variant: read_variant(path) for variant, path in zip(variants, sources)}
    reference, checkpoints = loaded["full"]
    for variant, (_, current) in loaded.items():
        validate_pair(variant, current, checkpoints)
    expected = {(d, p, t) for d in ("AW", "ROADS")
                for p in ("read_heavy", "write_heavy")
                for t in ("0p01pct", "0p1pct", "1pct")}
    if set(reference) != expected:
        raise ValueError("Maintenance scenario matrix is not the expected 12 pairs")
    fields = ("summary_rebuild_count_total", "local_compaction_count_total",
              "summary_rebuild_ns_total")
    titles = ("Summary rebuild count", "Compaction count", "Cumulative summary time")
    fig, axes = plt.subplots(1, 3, figsize=(7.2, 2.25))
    records = []
    for axis, field, title in zip(axes, fields, titles):
        for n, variant in enumerate(variants):
            paired = []
            for key, base in reference.items():
                denominator = float(base[field])
                if denominator <= 0:
                    raise ValueError("Undefined maintenance ratio: " + str(key))
                paired.append(float(loaded[variant][0][key][field]) / denominator)
            value = median(paired)
            records.append({"variant": variant, "metric": field, "matched_scenarios": 12,
                            "median_scenario_ratio": value})
            axis.bar(n, value, color=("#147D64", "#4C8CBF", "#C87631", "#8B6DA7")[n],
                     edgecolor="#333333", linewidth=0.4, width=0.65)
            axis.text(n, value + 0.04, f"{value:.2f}", ha="center", fontsize=7)
        axis.axhline(1, color="#444444", linestyle="--", linewidth=0.7)
        axis.set_xticks(range(4))
        axis.set_xticklabels(["Full", "Fixed\nlayout", "Conditional\nrefresh", "Fixed\nbudget"],
                            fontsize=7)
        axis.tick_params(axis="y", labelsize=7)
        axis.set_title(title, fontsize=8)
        axis.set_ylim(0, max(axis.get_ylim()[1] * 1.05, 1.3))
        axis.spines[["top", "right"]].set_visible(False)
    axes[0].set_ylabel("Ratio to paired full DELI", fontsize=7.5)
    fig.tight_layout(w_pad=0.8)
    save(fig, out, "deli_maintenance_frequency")
    with (out / "maintenance_frequency_ratios.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(records[0]))
        writer.writeheader()
        writer.writerows(records)
    return [fingerprint(path) for path in sources]


def complete_tables(rows, out, datasets, tags, tag_labels):
    records = []
    for profile, profile_name in zip(PROFILES, PROFILE_NAMES):
        columns = [r"Dataset & Nominal & Method & Q95 & I95 & D95 & Query & Insert & Delete & Overall & Build & Index \\",
                   r" & (\%) & & (\(\mu\)s) & (\(\mu\)s) & (\(\mu\)s) & (Kops/s) & (Kops/s) & (Kops/s) & (Kops/s) & (ms) & (MiB) \\"]
        lines = [r"\begin{longtable}{@{}lllrrrrrrrrr@{}}",
                 r"\caption{" + profile_name + r": all 16 dataset/selectivity scenarios and six methods.}\label{tab:" + profile + r"}\\",
                 r"\toprule"] + columns + [r"\midrule", r"\endfirsthead",
                 r"\multicolumn{12}{c}{" + profile_name + r" (continued)}\\", r"\toprule"] + columns + [
                 r"\midrule", r"\endhead", r"\midrule",
                 r"\multicolumn{12}{r}{Continued on next page}\\", r"\endfoot",
                 r"\bottomrule", r"\endlastfoot"]
        for dataset in datasets:
            for tag, tag_label in zip(tags, tag_labels):
                for method, name, _, _ in COMPARISON_METHODS:
                    source = rows[(dataset, profile, tag, method)]
                    cells = [dataset, tag_label.rstrip("%"), name]
                    record = {"dataset": dataset, "mixed_profile": profile, "selectivity_tag": tag,
                              "index": method, "seed": source["seed"],
                              "operation_count": source["operation_count"],
                              "checkpoint_ops": source["checkpoint_ops"],
                              "answers_match_boost": source["answers_match_boost"]}
                    for field, scale in METRICS:
                        value = float(source[field])
                        record[field] = value
                        cells.append(f"{value * scale:.3g}")
                    records.append(record)
                    lines.append(" & ".join(cells) + r" \\")
                lines.append(r"\addlinespace[2pt]")
        lines.append(r"\end{longtable}")
        (out / ("complete_" + profile + ".tex")).write_text("\n".join(lines) + "\n")
    with (out / "complete_final_metrics.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(records[0]))
        writer.writeheader()
        writer.writerows(records)
    return len(records)


def companion(path, out, resources, manifest, dataset_stats):
    relative = out.relative_to(path.parent).as_posix()
    lines = [r"% Generated by scripts/plot_deli_paper_full_matrix.py; edit the generator.",
             r"\documentclass[10pt]{article}",
             r"\usepackage[letterpaper,landscape,margin=0.6in]{geometry}",
             r"\usepackage{booktabs,longtable,graphicx}",
             r"\usepackage[hidelinks]{hyperref}",
             r"\renewcommand{\thetable}{S\arabic{table}}",
             r"\renewcommand{\thefigure}{S\arabic{figure}}",
             r"\setlength{\tabcolsep}{4pt}",
             r"\begin{document}",
             r"\section*{DELI: Complete Experimental Matrix}",
             r"Companion to Section 8 of the manuscript. This file exposes all 48 settings",
             r"(four datasets, three workloads, four nominal selectivities), six methods,",
             r"and nine final-checkpoint metrics: 288 method/scenario records.",
             r"\paragraph{Protocol and interpretation.}",
             r"The pool contains 500K objects; 250K are initially indexed. Runs use seed 42,",
             r"200K mixed operations, 10K-operation windows, and a 500-query pool.",
             r"Read-heavy, balanced, and write-heavy query/insert/delete ratios are",
             r"90/5/5, 70/15/15, and 50/25/25, respectively.",
             r"P95/P99 and rates describe the final window, not the entire run. Query, insertion,",
             r"and deletion rates divide each type's window operation count by its measured",
             r"service time; overall rate includes all three types' measured service time.",
             r"Build time is initial construction time. Index MiB is a method-specific estimate",
             r"excluding shared geometry storage, not process RSS or a uniform allocator audit.",
             r"The selectivities are nominal pool settings and can drift after updates.",
             r"These are same-process, single-seed comparisons without independent timing repeats.",
             r"All 5,760 main-matrix checkpoints have disabled answer-set checks;",
             r"index self-validation is not a substitute for answer-set correctness.",
             r"HIRE-SFC-Lite is an SFC-wrapped reproduction; RLR-Lite denotes RLR-Lite-CS-Split.",
             r"Tables use three significant digits; unrounded values are in",
             r"\texttt{figures/full\_matrix\_main8/complete\_final\_metrics.csv}.",
             r"Scenario medians and ranges aggregate different workload/selectivity settings,",
             r"not repeated estimates of one setting. No significance claims are inferred.",
             r"\paragraph{Source.}",
             r"\texttt{results/deli\_main8\_compare\_0.5m/dynamic\_compare\_summary.csv}",
             r"(5,760 rows; 20 checkpoints per method/scenario). The source fingerprint and",
             r"per-view coverage are recorded in \texttt{coverage\_manifest.json}."]
    for profile in PROFILES:
        lines += [r"\clearpage", r"\scriptsize", r"\input{" + relative + "/complete_" + profile + "}"]
    lines += [r"\clearpage", r"\normalsize", r"\section*{Remaining Query Panels}",
              r"The main paper displays AW and ROADS across all three mixes. The panels below",
              r"complete the six-method absolute query comparison for LW and PARKS.",
              r"\begin{figure}[h]\centering",
              r"\includegraphics[width=0.76\textwidth]{" + relative + "/deli_query_lw_parks.pdf}",
              r"\caption{LW and PARKS query P95 across all mixes and nominal selectivities.",
              r"Logarithmic vertical axes; final-window values.}\end{figure}",
              r"\clearpage", r"\section*{Construction and Index Estimates}",
              r"\begin{table}[h]\centering\small",
              r"\caption{Medians and observed ranges across twelve different scenarios per dataset/method.",
              r"Ranges are not repeat uncertainty. Index estimates use method-specific accounting.}",
              r"\begin{tabular}{@{}llrr@{}}\toprule",
              r"Dataset & Method & Build ms: median [min., max.] & Index MiB: median [min., max.]\\\midrule"]
    for dataset in manifest["datasets"]:
        for resource in [row for row in resources if row["dataset"] == dataset]:
            cells = [dataset, resource["method"]]
            for field, precision in (("build_ms", 1), ("index_mb_estimate", 2)):
                cells.append(f"{resource[field + '_median']:.{precision}f} "
                             f"[{resource[field + '_min']:.{precision}f}, "
                             f"{resource[field + '_max']:.{precision}f}]")
            lines.append(" & ".join(cells) + r" \\")
        lines.append(r"\addlinespace")
    lines += [r"\bottomrule\end{tabular}\end{table}", r"\clearpage",
              r"\section*{Maintenance and Refinement Details}",
              r"The matched component study uses AW and ROADS, read-heavy and write-heavy mixes,",
              r"and nominal 0.01\%, 0.1\%, and 1\%: twelve scenarios per configuration.",
              r"\begin{table}[h]\centering\small",
              r"\caption{Matched ablation configurations. Structural variants use full DELI as reference.",
              r"No PRL uses fixed-layout DELI to keep candidate access work matched.",
              r"Workload budgets are initialized for the mix; online budget recalibration is disabled.}",
              r"\label{tab:ablation_configs}",
              r"\begin{tabular}{@{}lllll@{}}\toprule",
              r"Variant & Layout & Summary & Budget & PRL \\",
              r"\midrule",
              r"Full & Cost-guided & Deferred & Workload & On \\",
              r"Fixed layout & Fixed & Deferred & Workload & On \\",
              r"Refresh & Cost-guided & Conditional & Workload & On \\",
              r"Fixed budget & Cost-guided & Deferred & 0.25 & On \\",
              r"No PRL & Fixed & Deferred & Workload & Off \\",
              r"\bottomrule\end{tabular}\end{table}",
              r"Each configuration runs in a separate process. Across five configurations'",
              r"1,200 checkpoints, all 240 scheduled answer checks match Boost; 960 are unchecked.",
              r"These paired scenario ratios are not independent process repeats.",
              r"\begin{figure}[h]\centering",
              r"\includegraphics[width=0.76\textwidth]{figures/ablation_main8/deli_paper_design_ratios.pdf}",
              r"\caption{Structural attribution across all twelve pairs. Ratios are variant/full DELI.",
              r"P95 is final-window latency; summary time is cumulative.}\end{figure}",
              r"\clearpage", r"\section*{Exact-safe Predicate Refinement}",
              r"PRL is isolated with fixed blocks on both sides. Candidate, scan, and structural",
              r"counts match at every paired checkpoint. Ratios are no PRL / PRL.",
              r"\begin{figure}[h]\centering",
              r"\includegraphics[width=0.76\textwidth]{figures/ablation_main8/deli_paper_ablation_prl.pdf}",
              r"\caption{All twelve PRL pairs. Exact calls and GEOS time have logarithmic axes.",
              r"Call-count reduction is not the end-to-end query speedup.}\end{figure}",
              r"\begin{table}[h]\centering\scriptsize",
              r"\caption{Absolute PRL diagnostics for the complete read-heavy slice.",
              r"Calls and GEOS time are per-query means; query P95 is a latency quantile.}",
              r"\input{figures/ablation_main8/deli_paper_ablation_prl_table.tex}\end{table}",
              r"All twelve absolute pairs, including write-heavy, are available in",
              r"\texttt{figures/ablation\_main8/deli\_paper\_ablation\_prl\_details.csv}.",
              r"The main paper reports P95/P99 update latency. The service-rate panels below",
              r"retain the four-selectivity scenario-median aggregation; the following",
              r"per-selectivity panels show final-window absolute values.",
              r"\begin{figure}[h]\centering",
              r"\includegraphics[width=0.76\textwidth]{" + relative + "/deli_update_service_rates.pdf}",
              r"\caption{Insertion and deletion service rates for all six methods.",
              r"Each bar is the median of four selectivity scenarios for one dataset/mix;",
              r"logarithmic axes. These rates use measured operation service time,",
              r"not wall-clock elapsed time.}\end{figure}"]
    for tag, label in zip(manifest["selectivity_tags"], manifest["nominal_selectivity_labels"]):
        lines += [r"\clearpage", r"\section*{All-method Update Comparison: " + label.replace("%", r"\%") + "}",
                  r"\begin{figure}[h]\centering",
                  r"\includegraphics[width=0.64\textwidth]{" + relative + "/deli_update_absolute_" + tag + ".pdf}",
                  r"\caption{Insertion/deletion P95 and P99 at nominal "
                  + label.replace("%", r"\%") + r". Rows are metrics; columns are workloads.",
                  r"Each panel compares six methods on all four datasets. Logarithmic axes;",
                  r"final-window absolute values, not scenario medians.}\end{figure}",
                  r"\begin{figure}[h]\centering",
                  r"\includegraphics[width=0.64\textwidth]{" + relative + "/deli_update_service_absolute_" + tag + ".pdf}",
                  r"\caption{Insertion and deletion service rates at nominal "
                  + label.replace("%", r"\%") + r" for the same methods and workloads.",
                  r"Logarithmic axes; final-window absolute values (Kops/s).}\end{figure}"]
    lines += [r"\clearpage", r"\section*{Query Attribution: Remaining Datasets}",
              r"\begin{figure}[h]\centering",
              r"\includegraphics[width=0.76\textwidth]{" + relative + "/deli_query_attribution_lw_parks.pdf}",
              r"\caption{LW/PARKS read-heavy attribution at all four selectivities.",
              r"Scan counts are instrumented DELI IDs, not Boost traversal counts.",
              r"Time shares use measured mean query/GEOS times; other work includes encoding,",
              r"filtering, shortcuts, materialization, and instrumentation. Labels below",
              r"selectivity give DELI/Boost mean query times in microseconds.}\end{figure}",
              r"\clearpage", r"\section*{Dynamic Process: Remaining Datasets}",
              r"\begin{figure}[h]\centering",
              r"\includegraphics[width=0.76\textwidth]{" + relative + "/deli_dynamic_process_lw_parks.pdf}",
              r"\caption{LW/PARKS write-heavy, nominal 0.1\%: all twenty 10K-operation windows.",
              r"Delta statistics count live IDs. Compaction counts and time are per-window,",
              r"not cumulative; summary time is not added to compaction time.}\end{figure}",
              r"Unrounded query diagnostics cover all 48 scenarios for DELI/Boost in",
              r"\texttt{query\_attribution\_all.csv}; all 960 DELI checkpoint states and",
              r"matched Boost P95 values are in \texttt{dynamic\_process\_all.csv}.",
              r"Boost scan counters and GLIN GEOS time are uninstrumented, not zero-cost measurements.",
              r"\clearpage", r"\section*{Dataset Characterization Details}",
              r"The main table characterizes the first 500K valid nonempty WKT objects per dataset.",
              r"PARKS requires 500,006 input lines because six empty objects are skipped.",
              r"Areas are coordinate-space MBR areas, not geodesic surface areas.",
              r"Each is divided by the prefix envelope area; SFC spans are divided by",
              r"$\max_i z_i^+-\min_i z_i^-$. Closed-interval overlaps count other object",
              r"intervals with endpoint contact included. They do not condition on the query pool.",
              r"GeometryCollections retain their WKT container type rather than being reassigned",
              r"to polygons or lines. Coordinate counts include ring-closing coordinates.",
              r"\begin{table}[h]\centering\small",
              r"\caption{Complete WKT type composition of the four loaded prefixes.}",
              r"\label{tab:dataset_types}",
              r"\begin{tabular}{@{}llrr@{}}\toprule",
              r"Dataset & WKT type & Objects & Share (\%) \\",
              r"\midrule"]
    for row in dataset_stats:
        types = validate_dataset(row, 500000)
        for kind, count in types.items():
            lines.append(row["dataset"] + " & " + kind + " & " + count + " & "
                         + f"{int(count) / 500000 * 100:.4g}" + r" \\")
        lines.append(r"\addlinespace")
    lines += [r"\bottomrule\end{tabular}\end{table}",
              r"Unrounded statistics and the input-prefix fingerprints are retained in",
              r"\texttt{results/deli\_dataset\_characterization\_main8/}.",
              r"The statistics reuse the benchmark Encoder with origin $(-180,-90)$",
              r"and cell size $5\times10^{-7}$, including its double interval endpoints.",
              r"Dataset overlap averages alone do not explain the measured sparse-query ratios.",
              r"\end{document}"]
    path.write_text("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--summary", type=Path,
                        default=Path("results/deli_main8_compare_0.5m/dynamic_compare_summary.csv"))
    parser.add_argument("--ablation-root", type=Path, default=Path("results/deli_paper_ablation_main8"))
    parser.add_argument("--out", type=Path,
                        default=Path("manuscript/2026.8.28/pvldbstyle-master/figures/full_matrix_main8"))
    parser.add_argument("--companion", type=Path,
                        default=Path("manuscript/2026.8.28/pvldbstyle-master/experiment_matrix.tex"))
    parser.add_argument("--dataset-characterization", type=Path,
                        default=Path("results/deli_dataset_characterization_main8/dataset_characterization.csv"))
    args = parser.parse_args()
    with args.dataset_characterization.open(newline="") as stream:
        dataset_stats = list(csv.DictReader(stream))
    if [row["dataset"] for row in dataset_stats] != ["AW", "LW", "PARKS", "ROADS"]:
        raise ValueError("Expected all four characterized main8 prefixes")
    for row in dataset_stats:
        validate_dataset(row, 500000)
    rows, _, datasets, tags, tag_labels, manifest = load_final_rows(args.summary)
    manifest["dataset_characterization_source"] = fingerprint(args.dataset_characterization)
    if (datasets != ["AW", "LW", "PARKS", "ROADS"]
            or tags != ["0p001pct", "0p01pct", "0p1pct", "1pct"]
            or len(rows) != 288 or manifest["operation_count"] != 200000
            or manifest["checkpoint_ops"] != 10000 or manifest["seed"] != "42"
            or manifest["checkpoints"] != 20 or manifest["unchecked_rows"] != 5760
            or any(int(row["loaded_count"]) != 500000 for row in rows.values())):
        raise ValueError("Expected the archived main8 protocol (48 scenarios, 200K operations)")
    companion_assets = (
        "deli_paper_design_ratios.pdf", "deli_paper_ablation_prl.pdf",
        "deli_paper_ablation_prl_table.tex", "deli_paper_ablation_prl_details.csv",
    )
    asset_root = args.companion.parent / "figures/ablation_main8"
    for name in companion_assets:
        if not (asset_root / name).is_file():
            raise ValueError("Generate matched ablation assets first: " + name)
    # Validate all displayed fields before emitting any figure or table.
    for key, row in rows.items():
        for field, _ in METRICS:
            value = float(row[field])
            if not math.isfinite(value) or value <= 0:
                raise ValueError("Invalid final metric: " + field + str(key))
    args.out.mkdir(parents=True, exist_ok=True)
    plt.rcParams.update({"font.family": "DejaVu Sans", "pdf.fonttype": 42,
                         "ps.fonttype": 42, "hatch.linewidth": 0.35})
    table_rows = complete_tables(rows, args.out, datasets, tags, tag_labels)
    for subset, stem in ((["AW", "ROADS"], "deli_query_aw_roads"),
                         (["LW", "PARKS"], "deli_query_lw_parks")):
        comparison_grid(rows, args.out, subset, tags, tag_labels, "p95_query_ms", 1000,
                        "Query P95 ($\\mu$s)", stem)
    comparison_grid(rows, args.out, datasets, tags, tag_labels, "overall_ops_tps", 0.001,
                    "Overall rate (Kops/s)", "deli_throughput_full_matrix")
    ratio_maps(rows, args.out, datasets, tags, tag_labels, ["p95_query_ms"], ["Query"],
               "deli_query_boundary")
    ratio_maps(rows, args.out, datasets, tags, tag_labels,
               ["p95_insert_ms", "p95_delete_ms"], ["Insert", "Delete"], "deli_update_p95")
    resources = resource_tables(rows, args.out, datasets, tags)
    manifest["source_fingerprint"] = fingerprint(args.summary)
    manifest["maintenance_sources"] = maintenance_figure(args.ablation_root, args.out)
    manifest["mechanism_views"] = build_mechanism_views(
        args.summary, rows, args.out, datasets, tags, tag_labels)
    manifest["views"] = {
        "main_query": {"datasets": ["AW", "ROADS"], "scenarios": 24, "methods": 6},
        "companion_query": {"datasets": ["LW", "PARKS"], "scenarios": 24, "methods": 6},
        "main_throughput": {"scenarios": 48, "methods": 6},
        "main_query_boundary": {"scenarios": 48, "methods": [DELI, BOOST]},
        "main_update_comparison": {"source_scenarios_per_metric": 48,
                                   "metrics": ["p95_insert_ms", "p95_delete_ms",
                                               "p99_insert_ms", "p99_delete_ms"],
                                   "methods": 6,
                                   "aggregation": "Scenario median over four selectivities"},
        "companion_update_rates": {"metrics": ["insert_tps", "delete_tps"],
                                   "aggregation": "Scenario median plus final-window per-selectivity views"},
        "main_resources": {"methods": 6, "datasets": 4, "scenario_aggregates_per_cell": 12},
        "complete_tables": {"rows": table_rows, "metrics": [metric[0] for metric in METRICS]},
    }
    manifest["companion"] = str(args.companion)
    manifest["companion_assets"] = [fingerprint(asset_root / name) for name in companion_assets]
    manifest["deli_vs_boost"] = {}
    for field, _ in METRICS:
        scenarios = [ratio(rows, d, p, t, field) for d in datasets for p in PROFILES for t in tags]
        higher_is_better = field.endswith("_tps")
        wins = lambda values: sum(v > 1 if higher_is_better else v < 1 for v in values)
        manifest["deli_vs_boost"][field] = {
            "min_ratio": min(scenarios), "max_ratio": max(scenarios),
            "better_scenarios": wins(scenarios), "scenarios": len(scenarios),
            "better_by_tag": {tag: wins([ratio(rows, d, p, tag, field)
                                         for d in datasets for p in PROFILES]) for tag in tags},
        }
    (args.out / "coverage_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    companion(args.companion, args.out, resources, manifest, dataset_stats)
    print(f"Validated {len(rows)} final records: 48 scenarios x 6 methods, 9 metrics each.")
    print("Figures and tables:", args.out)
    print("Complete companion:", args.companion)


if __name__ == "__main__":
    main()
