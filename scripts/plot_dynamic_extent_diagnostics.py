#!/usr/bin/env python3
"""Plot DELI-Dynamic-Single dynamic-maintenance diagnostics."""

import argparse
import csv
import math
import os
import re
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", "figures/.matplotlib_cache")

import matplotlib.pyplot as plt


DATASET_ORDER = [
    "AW",
    "LW",
    "ROADS",
    "PARKS",
    "OSM_AU_POINTS",
    "UNIF_S",
    "UNIF_L",
    "DIAG_S",
    "DIAG_L",
    "ZGAP_WIDE",
    "ZGAP_MIXED",
]

BLOCK_COLORS = {
    16: "#4E79A7",
    32: "#59A14F",
    64: "#F28E2B",
    128: "#E15759",
    256: "#76B7B2",
    512: "#B07AA1",
    1024: "#9C755F",
    2048: "#2F6F73",
    4096: "#6D8F3F",
}

BLOCK_MARKERS = {
    16: "o",
    32: "s",
    64: "^",
    128: "D",
    256: "P",
    512: "X",
    1024: "v",
    2048: "<",
    4096: ">",
}

METRICS = [
    ("avg_query_ms", "Average query time (ms)", "avg_query_ms", False),
    ("p95_query_ms", "P95 query time (ms)", "p95_query_ms", False),
    ("p99_query_ms", "P99 query time (ms)", "p99_query_ms", False),
    ("delete_tps", "Delete throughput (ops/s)", "delete_tps", False),
    ("summary_rebuild_count", "Summary rebuild count", "summary_rebuild_count", False),
    ("summary_rebuild_ms", "Summary rebuild time (ms)", "summary_rebuild_ms", False),
    ("dead_entry_ratio", "Dead entry ratio", "dead_entry_ratio", False),
    ("stale_block_count", "Stale block count", "stale_block_count", False),
    ("skipped_block_ratio", "Skipped block ratio", "skipped_block_ratio", False),
    ("candidate_answer_ratio", "Candidate / answer ratio", "candidate_answer_ratio", True),
]

BAR_METRICS = {
    "avg_query_ms",
    "delete_tps",
    "summary_rebuild_count",
    "summary_rebuild_ms",
    "skipped_block_ratio",
}

REQUIRED_COLUMNS = {
    "dataset",
    "selectivity",
    "checkpoint",
    "block_size",
    "stale_threshold_fraction",
    "answers_match_boost",
    "validate_ok",
}


def parse_args():
    parser = argparse.ArgumentParser(
        description="绘制 DELI-Dynamic-Single stale threshold 动态维护诊断图。"
    )
    parser.add_argument("--input", required=True, help="dynamic_extent_summary.csv。")
    parser.add_argument("--output_dir", required=True, help="图片输出目录。")
    parser.add_argument("--figure_prefix", default="dynamic_extent")
    parser.add_argument(
        "--checkpoint",
        default="after_delete",
        help="默认画删除后的 checkpoint，因为 stale threshold 主要影响删除后状态。",
    )
    parser.add_argument("--dpi", type=int, default=180)
    parser.add_argument(
        "--exclude_datasets",
        default="",
        help="画图时排除的数据集，多个值用逗号或空格分开。",
    )
    return parser.parse_args()


def split_names(value):
    if not value:
        return set()
    return {item for item in re.split(r"[,\s]+", value.strip()) if item}


def as_float(row, field):
    value = row.get(field, "")
    if value == "":
        return math.nan
    try:
        return float(value)
    except ValueError:
        return math.nan


def as_int(row, field):
    value = as_float(row, field)
    if math.isnan(value):
        return 0
    return int(round(value))


def load_rows(path):
    with path.open(newline="") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            raise ValueError(f"Empty CSV: {path}")
        missing = REQUIRED_COLUMNS.difference(reader.fieldnames)
        if missing:
            raise ValueError(f"{path} missing columns: {', '.join(sorted(missing))}")
        rows = []
        for row in reader:
            parsed = dict(row)
            parsed["block_size_i"] = as_int(parsed, "block_size")
            parsed["stale_threshold_f"] = as_float(parsed, "stale_threshold_fraction")
            rows.append(parsed)
    if not rows:
        raise ValueError(f"No rows found in {path}")
    return rows


def ordered_values(found, preferred):
    ordered = [value for value in preferred if value in found]
    ordered.extend(sorted(found.difference(ordered)))
    return ordered


def panel_title(dataset, selectivity):
    return f"{dataset} / {selectivity or 'unknown selectivity'}"


def plot_metric(rows, output_dir, figure_prefix, checkpoint, metric, ylabel, filename, log_y, dpi):
    rows = [row for row in rows if row.get("checkpoint") == checkpoint]
    if not rows:
        return None
    panels = []
    for dataset in ordered_values({row["dataset"] for row in rows}, DATASET_ORDER):
        selectivities = sorted({row.get("selectivity", "") for row in rows if row["dataset"] == dataset})
        for selectivity in selectivities:
            subset = [
                row
                for row in rows
                if row["dataset"] == dataset and row.get("selectivity", "") == selectivity
            ]
            if subset:
                panels.append((dataset, selectivity, subset))

    if not panels:
        return None

    cols = min(3, len(panels))
    rows_n = math.ceil(len(panels) / cols)
    fig, axes = plt.subplots(
        rows_n,
        cols,
        figsize=(max(5.2, cols * 4.6), max(3.8, rows_n * 3.35)),
        squeeze=False,
    )

    for ax in axes.flat:
        ax.set_visible(False)

    for ax, (dataset, selectivity, subset) in zip(axes.flat, panels):
        ax.set_visible(True)
        block_sizes = sorted({row["block_size_i"] for row in subset})
        for block_size in block_sizes:
            series = sorted(
                [row for row in subset if row["block_size_i"] == block_size],
                key=lambda row: row["stale_threshold_f"],
            )
            x_values = [row["stale_threshold_f"] for row in series]
            y_values = [as_float(row, metric) for row in series]
            ax.plot(
                x_values,
                y_values,
                marker=BLOCK_MARKERS.get(block_size, "o"),
                linewidth=1.8,
                markersize=5,
                color=BLOCK_COLORS.get(block_size),
                label=f"b{block_size}",
            )
        ax.set_title(panel_title(dataset, selectivity))
        ax.set_xlabel("stale_threshold_fraction")
        ax.set_ylabel(ylabel)
        ax.grid(True, linestyle="--", alpha=0.32)
        if log_y:
            positive_values = [
                value for value in [as_float(row, metric) for row in subset]
                if not math.isnan(value) and value > 0
            ]
            if positive_values:
                ax.set_yscale("log")

    handles, labels = axes.flat[0].get_legend_handles_labels()
    if handles:
        fig.legend(handles, labels, frameon=False, ncol=min(6, len(labels)), loc="upper center")
    fig.suptitle(f"{checkpoint}: {ylabel}", y=0.995)
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    path = output_dir / f"{figure_prefix}_{checkpoint}_{filename}.png"
    fig.savefig(path, dpi=dpi)
    plt.close(fig)
    return path


def plot_metric_bars(rows, output_dir, figure_prefix, checkpoint, metric, ylabel, filename, log_y, dpi):
    rows = [row for row in rows if row.get("checkpoint") == checkpoint]
    if not rows:
        return None
    panels = []
    for dataset in ordered_values({row["dataset"] for row in rows}, DATASET_ORDER):
        selectivities = sorted({row.get("selectivity", "") for row in rows if row["dataset"] == dataset})
        for selectivity in selectivities:
            subset = [
                row
                for row in rows
                if row["dataset"] == dataset and row.get("selectivity", "") == selectivity
            ]
            if subset:
                panels.append((dataset, selectivity, subset))

    if not panels:
        return None

    cols = min(3, len(panels))
    rows_n = math.ceil(len(panels) / cols)
    fig, axes = plt.subplots(
        rows_n,
        cols,
        figsize=(max(5.2, cols * 4.8), max(3.8, rows_n * 3.45)),
        squeeze=False,
    )

    for ax in axes.flat:
        ax.set_visible(False)

    for ax, (dataset, selectivity, subset) in zip(axes.flat, panels):
        ax.set_visible(True)
        thresholds = sorted({row["stale_threshold_f"] for row in subset})
        block_sizes = sorted({row["block_size_i"] for row in subset})
        group_width = 0.78
        bar_width = group_width / max(1, len(block_sizes))
        centers = list(range(len(thresholds)))

        for block_i, block_size in enumerate(block_sizes):
            x_values = [
                center - group_width / 2 + bar_width / 2 + block_i * bar_width
                for center in centers
            ]
            y_values = []
            for threshold in thresholds:
                row = next(
                    (
                        item
                        for item in subset
                        if item["block_size_i"] == block_size
                        and item["stale_threshold_f"] == threshold
                    ),
                    None,
                )
                y_values.append(as_float(row, metric) if row else math.nan)
            ax.bar(
                x_values,
                y_values,
                width=bar_width * 0.92,
                label=f"b{block_size}",
                color=BLOCK_COLORS.get(block_size),
                edgecolor="black",
                linewidth=0.45,
            )

        ax.set_title(panel_title(dataset, selectivity))
        ax.set_xlabel("stale_threshold_fraction")
        ax.set_ylabel(ylabel)
        ax.set_xticks(centers)
        ax.set_xticklabels([f"{value:g}" for value in thresholds])
        ax.grid(axis="y", linestyle="--", alpha=0.32)
        if log_y:
            positive_values = [
                value for value in [as_float(row, metric) for row in subset]
                if not math.isnan(value) and value > 0
            ]
            if positive_values:
                ax.set_yscale("log")

    handles, labels = axes.flat[0].get_legend_handles_labels()
    if handles:
        fig.legend(handles, labels, frameon=False, ncol=min(6, len(labels)), loc="upper center")
    fig.suptitle(f"{checkpoint}: {ylabel} (bar view)", y=0.995)
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    path = output_dir / f"{figure_prefix}_{checkpoint}_{filename}_bars.png"
    fig.savefig(path, dpi=dpi)
    plt.close(fig)
    return path


def plot_correctness(rows, output_dir, figure_prefix, checkpoint, dpi):
    rows = [row for row in rows if row.get("checkpoint") == checkpoint]
    if not rows:
        return None
    rows = sorted(
        rows,
        key=lambda row: (
            DATASET_ORDER.index(row["dataset"]) if row["dataset"] in DATASET_ORDER else len(DATASET_ORDER),
            row.get("selectivity", ""),
            row["block_size_i"],
            row["stale_threshold_f"],
        ),
    )
    labels = [
        f"{row['dataset']}\n{row.get('selectivity', '')}\nb{row['block_size_i']} s{row['stale_threshold_f']:g}"
        for row in rows
    ]
    values = []
    for row in rows:
        match = as_int(row, "answers_match_boost")
        valid = as_int(row, "validate_ok")
        if match == 1 and valid == 1:
            values.append(1.0)
        elif match == -1 and valid == 1:
            values.append(0.5)
        else:
            values.append(0.0)

    fig, ax = plt.subplots(figsize=(max(8.0, len(labels) * 0.72), 3.8))
    colors = [
        "#59A14F" if value == 1.0 else "#F2C94C" if value == 0.5 else "#D62728"
        for value in values
    ]
    ax.bar(range(len(labels)), values, color=colors, edgecolor="black", linewidth=0.45)
    ax.set_ylim(0, 1.15)
    ax.set_ylabel("1=checked ok, 0.5=Boost skipped, 0=failed")
    ax.set_title(f"{checkpoint}: answer set + invariant check")
    ax.set_xticks(range(len(labels)))
    ax.set_xticklabels(labels, rotation=70, ha="right")
    ax.grid(axis="y", linestyle="--", alpha=0.32)
    fig.tight_layout()
    path = output_dir / f"{figure_prefix}_{checkpoint}_correctness.png"
    fig.savefig(path, dpi=dpi)
    plt.close(fig)
    return path


def estimate_run_cost(rows):
    groups = {}
    for row in rows:
        groups.setdefault(row.get("source_file", ""), []).append(row)

    total_deli_query_ms = 0.0
    total_boost_check_ms = 0.0
    total_update_ms = 0.0
    total_maintenance_ms = 0.0
    for group_rows in groups.values():
        total_deli_query_ms += sum(
            as_float(row, "avg_query_ms") * as_float(row, "query_count")
            for row in group_rows
        )
        total_boost_check_ms += sum(
            as_float(row, "boost_rebuild_ms") + as_float(row, "boost_query_ms")
            for row in group_rows
        )
        total_update_ms += max(as_float(row, "insert_ms") for row in group_rows)
        total_update_ms += max(as_float(row, "delete_ms") for row in group_rows)
        total_maintenance_ms += max(as_float(row, "summary_rebuild_ms") for row in group_rows)

    subtotal = total_deli_query_ms + total_boost_check_ms + total_update_ms
    return {
        "raw_file_count": len(groups),
        "deli_query_ms": total_deli_query_ms,
        "boost_check_ms": total_boost_check_ms,
        "update_ms": total_update_ms,
        "maintenance_ms": total_maintenance_ms,
        "subtotal_ms": subtotal,
    }


def write_diagnostics(rows, output_dir, figure_prefix, checkpoint):
    path = output_dir / f"{figure_prefix}_diagnostics.txt"
    subset = [row for row in rows if row.get("checkpoint") == checkpoint]
    bad = [
        row
        for row in rows
        if as_int(row, "validate_ok") != 1
        or as_int(row, "answers_match_boost") not in (-1, 1)
    ]
    skipped_boost = [
        row
        for row in rows
        if as_int(row, "answers_match_boost") == -1
        and as_int(row, "validate_ok") == 1
    ]
    with path.open("w") as handle:
        handle.write("DELI-Dynamic-Single 动态维护诊断说明\n")
        handle.write("\n")
        handle.write("本次实验规模：\n")
        handle.write(f"  dataset：{', '.join(ordered_values({row['dataset'] for row in rows}, DATASET_ORDER))}\n")
        handle.write(f"  selectivity：{', '.join(sorted({row.get('selectivity', '') for row in rows}))}\n")
        handle.write(f"  block_size：{', '.join(str(value) for value in sorted({row['block_size_i'] for row in rows}))}\n")
        handle.write(f"  stale_threshold_fraction：{', '.join(f'{value:g}' for value in sorted({row['stale_threshold_f'] for row in rows}))}\n")
        handle.write(f"  raw CSV 组合数：{len({row.get('source_file', '') for row in rows})}\n")
        handle.write("\n")
        cost = estimate_run_cost(rows)
        handle.write("运行时间为什么会长：\n")
        handle.write("  下面是从 CSV 估算出来的 benchmark 内部耗时，不含 WKT 读取、索引初建、脚本调度和画图时间。\n")
        handle.write(f"  DELI 自己的 checkpoint query 总耗时约：{cost['deli_query_ms'] / 1000.0:.2f} 秒。\n")
        handle.write(f"  Boost correctness check 总耗时约：{cost['boost_check_ms'] / 1000.0:.2f} 秒。\n")
        handle.write(f"  insert/delete 更新操作总耗时约：{cost['update_ms'] / 1000.0:.2f} 秒。\n")
        handle.write(f"  其中 local summary rebuild 维护耗时约：{cost['maintenance_ms'] / 1000.0:.2f} 秒。\n")
        handle.write("  注意：Boost correctness check 是为了确认答案正确，不是 DELI 方法本身的前台查询/更新时间。\n")
        handle.write("\n")
        handle.write("核心名词解释：\n")
        handle.write("  stale_threshold_fraction：删除后允许一个 block 中 dead record 积累到多少比例才局部重建。0 表示每次删除后尽快重建。\n")
        handle.write("  dead record / tombstone：对象已经删除，但 record_id 暂时还留在 block 里，用 alive=false 标记。\n")
        handle.write("  summary_rebuild_count：局部重算 block summary 的次数，不是全局重建次数。\n")
        handle.write("  summary_rebuild_ms：局部重算 summary 的总耗时，反映维护成本。\n")
        handle.write("  dead_entry_ratio：已删除但还残留在 blocks 中的记录比例，越高通常查询越容易多扫。\n")
        handle.write("  skipped_block_ratio：query 阶段被 block summary 剪枝跳过的 block 比例。\n")
        handle.write("  candidate_answer_ratio：进入 exact intersects 判断的候选数 / 最终答案数，越大代表 refinement 压力越重。\n")
        handle.write("  answers_match_boost：DELI 的最终答案集合是否和 Boost R-tree exact baseline 一致。\n")
        handle.write("  validate_ok：内部不变式是否通过，例如 summary 是否仍然 conservative。\n")
        handle.write("\n")
        handle.write(f"当前图表主要使用 checkpoint={checkpoint}。\n")
        handle.write("原因：stale_threshold_fraction 主要影响删除后的 tombstone 积累和局部重建，所以 after_delete 最能看出 trade-off。\n")
        handle.write("\n")

        if bad:
            handle.write("需要优先检查的异常行：\n")
            for row in bad[:50]:
                handle.write(
                    "  "
                    f"{row.get('dataset', '')} {row.get('selectivity', '')} "
                    f"checkpoint={row.get('checkpoint', '')} "
                    f"b={row.get('block_size', '')} stale={as_float(row, 'stale_threshold_fraction'):g} "
                    f"match={row.get('answers_match_boost', '')} validate={row.get('validate_ok', '')} "
                    f"missing={row.get('missing_count', '')} extra={row.get('extra_count', '')}\n"
                )
        elif skipped_boost:
            handle.write("correctness 检查：validate_ok 都为 1；Boost answer-set check 被跳过，answers_match_boost=-1。\n")
        else:
            handle.write("correctness 检查：所有行 answers_match_boost=1 且 validate_ok=1。\n")

        handle.write("\n")
        handle.write("怎么看这些图：\n")
        handle.write("  correctness 图先看是否全是 1；不是 1 就先不要讨论性能。\n")
        handle.write("  summary_rebuild_count / summary_rebuild_ms 看维护成本；stale=0 通常重建最多。\n")
        handle.write("  delete_tps 看删除吞吐量；如果 stale=0 很低，说明立即重建太贵。\n")
        handle.write("  avg/p95/p99 query time 看查询是否因为 tombstone 和 stale summary 变慢。\n")
        handle.write("  skipped_block_ratio 看剪枝效果；越高说明 block summary 帮你跳过越多 block。\n")
        handle.write("  candidate_answer_ratio 在当前 ZGAP_MIXED 上基本为 1，说明这个指标暂时区分不了方法好坏。\n")
        handle.write("  折线起伏大时不要过度解读，因为这里只是单 seed、单次运行；优先看柱状图和数量级趋势。\n")
        handle.write("\n")
        handle.write("after_delete 每组最小 avg_query_ms：\n")
        groups = {}
        for row in subset:
            groups.setdefault((row.get("dataset", ""), row.get("selectivity", "")), []).append(row)
        for (dataset, selectivity), group_rows in sorted(groups.items()):
            best = min(group_rows, key=lambda row: as_float(row, "avg_query_ms"))
            handle.write(
                "  "
                f"{dataset} {selectivity}: b={best.get('block_size', '')}, "
                f"stale={as_float(best, 'stale_threshold_fraction'):g}, "
                f"avg_query_ms={as_float(best, 'avg_query_ms'):.4f}, "
                f"delete_tps={as_float(best, 'delete_tps'):.2f}, "
                f"dead_ratio={as_float(best, 'dead_entry_ratio'):.4f}\n"
            )
    return path


def main():
    args = parse_args()
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    rows = load_rows(Path(args.input))

    excluded = split_names(args.exclude_datasets)
    if excluded:
        rows = [row for row in rows if row["dataset"] not in excluded]
        if not rows:
            raise ValueError("No rows left after applying --exclude_datasets.")

    paths = []
    for metric, ylabel, filename, log_y in METRICS:
        if metric in rows[0]:
            paths.append(
                plot_metric(
                    rows,
                    output_dir,
                    args.figure_prefix,
                    args.checkpoint,
                    metric,
                    ylabel,
                    filename,
                    log_y,
                    args.dpi,
                )
            )
            if metric in BAR_METRICS:
                paths.append(
                    plot_metric_bars(
                        rows,
                        output_dir,
                        args.figure_prefix,
                        args.checkpoint,
                        metric,
                        ylabel,
                        filename,
                        log_y,
                        args.dpi,
                    )
                )
    paths.append(plot_correctness(rows, output_dir, args.figure_prefix, args.checkpoint, args.dpi))
    paths.append(write_diagnostics(rows, output_dir, args.figure_prefix, args.checkpoint))

    for path in paths:
        if path:
            print(f"Wrote {path}")


if __name__ == "__main__":
    main()
