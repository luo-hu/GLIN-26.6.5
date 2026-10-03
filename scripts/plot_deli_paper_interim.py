#!/usr/bin/env python3
"""Make clearly labeled interim paper plots from one dynamic-compare run."""

import argparse
import csv
import json
import math
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import TwoSlopeNorm

DELI = "DELI_ALEX_HYBRID_SINGLE_STORE_COST"
BOOST = "Boost_Rtree"
MEMORY_METHODS = (
    (DELI, "DELI"),
    (BOOST, "Boost R-tree"),
    ("GEOS_Quadtree", "GEOS Quadtree"),
    ("GLIN_PIECEWISE", "GLIN-Piecewise"),
    ("RLR_LITE_CS_SPLIT", "RLR-Lite-CS-Split"),
    ("HIRE_SFC_LITE", "HIRE-SFC-Lite"),
)
DATASET_ORDER = ("AW", "LW", "PARKS", "ROADS", "ZGAP_MIXED")
PROFILES = ("read_heavy", "balanced", "write_heavy")
COMPARISON_METHODS = (
    (DELI, "DELI", "#147D64", ""),
    (BOOST, "Boost R-tree", "#4E5968", "//"),
    ("GEOS_Quadtree", "GEOS Quadtree", "#B45360", "\\\\"),
    ("RLR_LITE_CS_SPLIT", "RLR-Lite", "#C87631", "xx"),
    ("HIRE_SFC_LITE", "HIRE-SFC-Lite", "#8B6DA7", ".."),
    ("GLIN_PIECEWISE", "GLIN-Piecewise", "#4C8CBF", "++"),
)
GOOD = "#126B58"
BAD = "#B44A3B"


def load_final_rows(path):
    with path.open(newline="", encoding="utf-8") as stream:
        source_rows = list(csv.DictReader(stream))
    if not source_rows:
        raise ValueError("Empty result CSV")
    methods = {method for method, _ in MEMORY_METHODS}
    rows = [row for row in source_rows if row["index"] in methods]
    if not rows:
        raise ValueError("No supported methods in result CSV")
    dataset_set = {row["dataset"] for row in rows}
    datasets = sorted(dataset_set, key=lambda name: (
        DATASET_ORDER.index(name) if name in DATASET_ORDER else len(DATASET_ORDER), name))
    selectivities = {}
    for row in rows:
        tag, raw_selectivity = row["selectivity_tag"], row["selectivity"]
        value = float(raw_selectivity.rstrip("%"))
        value /= 100 if raw_selectivity.endswith("%") else 1
        if tag in selectivities and selectivities[tag] != value:
            raise ValueError("Inconsistent selectivity for " + tag)
        selectivities[tag] = value
    tags = sorted(selectivities, key=selectivities.get)
    tag_labels = [f"{100 * selectivities[tag]:g}%" for tag in tags]
    operations = sorted({int(row["operation_count"]) for row in rows})
    final_op = operations[-1]
    selected, seen = {}, set()
    for row in rows:
        key = (row["dataset"], row["mixed_profile"], row["selectivity_tag"], row["index"])
        checkpoint_key = key + (int(row["operation_count"]),)
        if checkpoint_key in seen:
            raise ValueError("Duplicate checkpoint: " + str(checkpoint_key))
        seen.add(checkpoint_key)
        if row["answers_match_boost"] not in {"-1", "1"}:
            raise ValueError("Answer mismatch or unknown correctness status: " + str(key))
        if row.get("validate_ok") not in {None, "", "1"}:
            raise ValueError("Index self-validation failed: " + str(key))
        if int(row["operation_count"]) == final_op:
            for metric in ("build_ms", "index_mb_estimate", "p95_query_ms",
                           "p95_insert_ms", "p95_delete_ms", "overall_ops_tps"):
                value = float(row[metric])
                if not math.isfinite(value) or value <= 0:
                    raise ValueError("Invalid metric " + metric + ": " + str(key))
            selected[key] = row
    expected_keys = {
        (dataset, profile, tag, method, operation)
        for dataset in datasets for profile in PROFILES for tag in tags
        for method in methods for operation in operations
    }
    if seen != expected_keys:
        raise ValueError(f"Incomplete result matrix: missing {len(expected_keys - seen)}, "
                         f"unexpected {len(seen - expected_keys)} checkpoints")
    seeds = sorted({row["seed"] for row in rows})
    checkpoint_ops = sorted({row["checkpoint_ops"] for row in rows})
    if len(seeds) != 1 or len(checkpoint_ops) != 1:
        raise ValueError("Mixed seeds or checkpoint lengths must not be pooled")
    manifest = {
        "input": str(path), "source_rows": len(source_rows), "selected_methods_rows": len(rows),
        "final_rows": len(selected), "datasets": datasets, "profiles": list(PROFILES),
        "selectivity_tags": tags, "nominal_selectivity_labels": tag_labels,
        "methods": sorted(methods), "seed": seeds[0], "operation_count": final_op,
        "checkpoint_ops": int(checkpoint_ops[0]), "checkpoints": len(operations),
        "unchecked_rows": sum(row["answers_match_boost"] == "-1" for row in rows),
        "aggregation": "Final checkpoint only; not whole-run P95 or independent repeats",
    }
    return selected, final_op, datasets, tags, tag_labels, manifest


def ratio(rows, dataset, profile, tag, metric):
    left = float(rows[(dataset, profile, tag, DELI)][metric])
    right = float(rows[(dataset, profile, tag, BOOST)][metric])
    if right <= 0:
        raise ValueError(f"Nonpositive Boost value for {metric}: {dataset}, {profile}, {tag}")
    return left / right


def save_tradeoff(rows, out, datasets, final_op):
    metrics = (
        ("p95_query_ms", "P95 query latency"),
        ("p95_insert_ms", "P95 insert latency"),
        ("p95_delete_ms", "P95 delete latency"),
        ("index_mb_estimate", "Estimated index memory"),
    )
    fig, axes = plt.subplots(2, 2, figsize=(7.0, 4.1), sharey=True)
    for axis, (metric, title) in zip(axes.flat, metrics):
        values = [ratio(rows, dataset, "read_heavy", "0p1pct", metric) for dataset in datasets]
        for position, value in enumerate(values):
            axis.barh(position, value, color=GOOD if value < 1 else BAD, height=0.63)
            axis.text(
                min(max(value * 1.04, 0.10), 4.7),
                position,
                f"{value:.2f}x",
                va="center",
                ha="left",
                fontsize=7,
            )
        axis.axvline(1, color="#303030", linewidth=0.8, linestyle="--")
        axis.set_xlim(0, 3.3)
        axis.set_yticks(range(len(datasets)))
        axis.set_yticklabels(datasets)
        axis.set_title(title, fontsize=9)
        axis.set_xlabel("DELI / Boost (lower is better)", fontsize=7.5)
        axis.tick_params(labelsize=7)
        axis.spines["top"].set_visible(False)
        axis.spines["right"].set_visible(False)
    axes.flat[0].set_ylim(len(datasets) - 0.5, -0.5)
    fig.suptitle(f"Read-heavy, nominal 0.1%, final {final_op // 1000}K-operation checkpoint", fontsize=9)
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    fig.savefig(out / "deli_interim_tradeoff.pdf")
    fig.savefig(out / "deli_interim_tradeoff.png", dpi=220)
    plt.close(fig)


def save_boundary(rows, out, datasets, tags, tag_labels):
    fig, axes = plt.subplots(1, 3, figsize=(7.0, 2.9), sharey=True)
    norm = TwoSlopeNorm(vmin=0.5, vcenter=1.0, vmax=2.0)
    image = None
    for axis, profile in zip(axes, PROFILES):
        matrix = [
            [ratio(rows, dataset, profile, tag, "p95_query_ms") for tag in tags]
            for dataset in datasets
        ]
        image = axis.imshow(matrix, cmap="RdYlGn_r", norm=norm, aspect="auto")
        axis.set_xticks(range(len(tags)))
        axis.set_xticklabels(tag_labels, fontsize=7)
        axis.set_yticks(range(len(datasets)))
        axis.set_yticklabels(datasets, fontsize=7)
        axis.set_title(profile.replace("_", "-"), fontsize=9)
        axis.set_xlabel("Nominal selectivity", fontsize=7.5)
        for row_index, values in enumerate(matrix):
            for col_index, value in enumerate(values):
                axis.text(col_index, row_index, f"{value:.2f}", ha="center", va="center", fontsize=7)
        axis.tick_params(length=0)
    color_axis = fig.add_axes((0.32, 0.12, 0.36, 0.045))
    bar = fig.colorbar(image, cax=color_axis, orientation="horizontal")
    bar.set_label("P95 query latency: DELI / Boost (below 1 favors DELI)", fontsize=7.5)
    bar.ax.tick_params(labelsize=7)
    fig.subplots_adjust(left=0.095, right=0.99, top=0.90, bottom=0.31, wspace=0.16)
    fig.savefig(out / "deli_interim_boundary.pdf")
    fig.savefig(out / "deli_interim_boundary.png", dpi=220)
    plt.close(fig)



def save_memory(rows, out):
    dataset, profile, tag = "AW", "read_heavy", "0p1pct"
    values = [
        float(rows[(dataset, profile, tag, method)]["index_mb_estimate"])
        for method, _ in MEMORY_METHODS
    ]
    labels = [label for _, label in MEMORY_METHODS]
    fig, axis = plt.subplots(figsize=(6.8, 2.8))
    for position, (value, label) in enumerate(zip(values, labels)):
        axis.barh(position, value, color=GOOD if label == "DELI" else "#858F98", height=0.66)
        axis.text(value + 0.5, position, f"{value:.1f}", va="center", fontsize=8)
    axis.set_yticks(range(len(labels)))
    axis.set_yticklabels(labels, fontsize=8)
    axis.set_ylim(len(labels) - 0.5, -0.5)
    axis.set_xlim(0, max(values) * 1.14)
    axis.set_xlabel("Method-specific estimated index size (MiB)", fontsize=8)
    axis.set_title("AW, read-heavy, nominal 0.1%, final checkpoint", fontsize=9)
    axis.spines["top"].set_visible(False)
    axis.spines["right"].set_visible(False)
    axis.tick_params(axis="x", labelsize=7)
    fig.tight_layout()
    fig.savefig(out / "deli_interim_memory.pdf")
    fig.savefig(out / "deli_interim_memory.png", dpi=220)
    plt.close(fig)



def save_method_bars(rows, out, datasets, tags, tag_labels):
    """Grouped bars compare methods; checkpoint trends remain in the artifact."""
    if len(datasets) != 4:
        raise ValueError("The paper small-multiple layout requires four datasets")
    width = 0.8 / len(COMPARISON_METHODS)
    center = (len(COMPARISON_METHODS) - 1) / 2
    fig, axes = plt.subplots(2, 2, figsize=(7.1, 4.4), sharex=True)
    for axis, dataset in zip(axes.flat, datasets):
        for method_index, (method, label, color, hatch) in enumerate(COMPARISON_METHODS):
            values = [1000 * float(rows[(dataset, "read_heavy", tag, method)]["p95_query_ms"])
                      for tag in tags]
            positions = [group + (method_index - center) * width for group in range(len(tags))]
            axis.bar(positions, values, width=width, label=label, color=color,
                     hatch=hatch, edgecolor="#292929", linewidth=0.35)
        axis.set_title(dataset, fontsize=9)
        axis.set_yscale("log")
        axis.set_xticks(range(len(tags)))
        axis.set_xticklabels(tag_labels)
        axis.tick_params(labelsize=7)
        axis.grid(axis="y", alpha=0.22)
        axis.set_axisbelow(True)
        axis.spines[["top", "right"]].set_visible(False)
    axes[0, 0].set_ylabel("P95 query latency (us)", fontsize=8)
    axes[1, 0].set_ylabel("P95 query latency (us)", fontsize=8)
    axes[1, 0].set_xlabel("Nominal selectivity", fontsize=8)
    axes[1, 1].set_xlabel("Nominal selectivity", fontsize=8)
    handles, labels = axes[0, 0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=3, fontsize=7,
               frameon=False, bbox_to_anchor=(0.5, 1.02))
    fig.tight_layout(rect=(0, 0, 1, 0.88))
    fig.savefig(out / "deli_interim_query_methods.pdf")
    fig.savefig(out / "deli_interim_query_methods.png", dpi=220)
    plt.close(fig)

    fig, axes = plt.subplots(1, 3, figsize=(7.1, 2.5), sharey=True)
    for axis, profile in zip(axes, PROFILES):
        for method_index, (method, label, color, hatch) in enumerate(COMPARISON_METHODS):
            values = [float(rows[(dataset, profile, "0p1pct", method)]["overall_ops_tps"]) / 1000
                      for dataset in datasets]
            positions = [group + (method_index - center) * width for group in range(len(datasets))]
            axis.bar(positions, values, width=width, label=label, color=color,
                     hatch=hatch, edgecolor="#292929", linewidth=0.35)
        axis.set_title(profile.replace("_", "-"), fontsize=9)
        axis.set_xticks(range(len(datasets)))
        axis.set_xticklabels(datasets)
        axis.tick_params(labelsize=7)
        axis.grid(axis="y", alpha=0.22)
        axis.set_axisbelow(True)
        axis.spines[["top", "right"]].set_visible(False)
    axes[0].set_ylabel("Overall throughput (K ops/s)", fontsize=8)
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=3, fontsize=7,
               frameon=False, bbox_to_anchor=(0.5, 1.04))
    fig.tight_layout(rect=(0, 0, 1, 0.84))
    fig.savefig(out / "deli_interim_throughput_methods.pdf")
    fig.savefig(out / "deli_interim_throughput_methods.png", dpi=220)
    plt.close(fig)



def save_source_tables(rows, out, datasets, manifest):
    """Keep table cells and provenance synchronized with the plotted CSV."""
    table_lines = [
        r"\begin{tabular}{@{}lrrrr@{}}", r"\toprule",
        r" & \multicolumn{2}{c}{Build (ms)} & \multicolumn{2}{c}{Index (MiB)} \\",
        r"Dataset & DELI & Boost & DELI & Boost \\", r"\midrule",
    ]
    for dataset in datasets:
        deli = rows[(dataset, "read_heavy", "0p1pct", DELI)]
        boost = rows[(dataset, "read_heavy", "0p1pct", BOOST)]
        table_lines.append(
            f"{dataset.replace('_', '-')} & {float(deli['build_ms']):.1f} & "
            f"{float(boost['build_ms']):.1f} & {float(deli['index_mb_estimate']):.2f} & "
            f"{float(boost['index_mb_estimate']):.2f} " + r"\\")
    table_lines.extend([r"\bottomrule", r"\end{tabular}"])
    (out / "deli_resources_table.tex").write_text("\n".join(table_lines) + "\n", encoding="utf-8")
    (out / "plot_manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    with (out / "final_checkpoint.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(next(iter(rows.values()))))
        writer.writeheader()
        writer.writerows(rows[key] for key in sorted(rows))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--input",
        type=Path,
        default=Path("results/deli_main8_compare_0.5m/dynamic_compare_summary.csv"),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("manuscript/2026.8.28/pvldbstyle-master/figures/main8_0.5m"),
    )
    args = parser.parse_args()
    rows, final_op, datasets, tags, tag_labels, manifest = load_final_rows(args.input)
    args.output.mkdir(parents=True, exist_ok=True)
    save_tradeoff(rows, args.output, datasets, final_op)
    save_boundary(rows, args.output, datasets, tags, tag_labels)
    save_memory(rows, args.output)
    save_method_bars(rows, args.output, datasets, tags, tag_labels)
    save_source_tables(rows, args.output, datasets, manifest)
    print(f"Plotted {len(rows)} matched final rows at {final_op} operations into {args.output}")


if __name__ == "__main__":
    main()
