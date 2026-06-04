#!/usr/bin/env python3
"""Plot GLIN Fig. 8 style index-size bars from bench_index_size_wkt CSV."""

from __future__ import annotations

import argparse
import os
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib")

import matplotlib.pyplot as plt
import pandas as pd


INDEX_ORDER = ["GLIN", "Boost_Rtree", "GEOS_Quadtree"]
INDEX_LABELS = {
    "GLIN": "GLIN",
    "Boost_Rtree": "Boost R-tree",
    "GEOS_Quadtree": "GEOS Quadtree",
}
COLORS = {
    "GLIN": "#3B6EA8",
    "Boost_Rtree": "#C46A4A",
    "GEOS_Quadtree": "#8C6BB1",
}
HATCHES = {
    "GLIN": "",
    "Boost_Rtree": "//",
    "GEOS_Quadtree": "\\\\",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Draw log-scale index-size bars for GLIN Fig. 8."
    )
    parser.add_argument("--input_csv", required=True, help="bench_index_size_wkt CSV")
    parser.add_argument("--output_dir", default="figures", help="Output directory")
    parser.add_argument("--prefix", default="fig8_index_size", help="Output prefix")
    return parser.parse_args()


def ordered(values: list[str], preferred: list[str]) -> list[str]:
    seen = list(dict.fromkeys(values))
    return [v for v in preferred if v in seen] + sorted(
        [v for v in seen if v not in preferred]
    )


def load_csv(path: Path) -> pd.DataFrame:
    df = pd.read_csv(path)
    required = {"dataset", "index", "index_bytes", "index_mib", "loaded_count"}
    missing = required - set(df.columns)
    if missing:
        raise SystemExit(f"Missing required columns in {path}: {sorted(missing)}")
    df = df.copy()
    df["index_bytes"] = pd.to_numeric(df["index_bytes"], errors="coerce")
    df["index_mib"] = pd.to_numeric(df["index_mib"], errors="coerce")
    df["loaded_count"] = pd.to_numeric(df["loaded_count"], errors="coerce")
    df = df.dropna(subset=["index_bytes", "index_mib"])
    df = df[df["index_bytes"] > 0]
    if df.empty:
        raise SystemExit(f"No positive index_bytes rows in {path}")
    return df


def write_table(df: pd.DataFrame, output_dir: Path, prefix: str) -> Path:
    table = df.copy()
    table["index_label"] = table["index"].map(INDEX_LABELS).fillna(table["index"])
    table["index_mib"] = table["index_mib"].round(3)
    keep = [
        "dataset",
        "index_label",
        "loaded_count",
        "index_bytes",
        "index_mib",
        "size_method",
        "node_count",
        "internal_nodes",
        "leaf_nodes",
        "entry_count",
        "depth",
        "build_ns",
    ]
    keep = [col for col in keep if col in table.columns]
    out = output_dir / f"{prefix}_table.csv"
    table[keep].to_csv(out, index=False)
    return out


def plot_index_size(df: pd.DataFrame, output_dir: Path, prefix: str) -> Path:
    datasets = ordered(df["dataset"].astype(str).tolist(), ["AW", "LW", "ROADS", "PARKS", "OSM_AU_POINTS", "DIAG_S", "DIAG_L", "UNIF_S", "UNIF_L"])
    indexes = ordered(df["index"].astype(str).tolist(), INDEX_ORDER)

    pivot = (
        df.groupby(["dataset", "index"], as_index=False)["index_mib"]
        .mean()
        .pivot(index="dataset", columns="index", values="index_mib")
        .reindex(index=datasets, columns=indexes)
    )

    fig_width = max(8.0, len(datasets) * 1.3)
    fig, ax = plt.subplots(figsize=(fig_width, 4.8))
    x = range(len(pivot.index))
    group_width = 0.78
    bar_width = group_width / max(1, len(indexes))

    for i, index in enumerate(indexes):
        offsets = [pos - group_width / 2 + bar_width / 2 + i * bar_width for pos in x]
        values = pivot[index].tolist()
        bars = ax.bar(
            offsets,
            values,
            width=bar_width,
            label=INDEX_LABELS.get(index, index),
            color=COLORS.get(index, "#666666"),
            hatch=HATCHES.get(index, ""),
            edgecolor="#222222",
            linewidth=0.6,
        )
        for bar, value in zip(bars, values):
            if pd.notna(value) and value > 0:
                ax.annotate(
                    f"{value:.1f}",
                    xy=(bar.get_x() + bar.get_width() / 2, value),
                    xytext=(0, 3),
                    textcoords="offset points",
                    ha="center",
                    va="bottom",
                    fontsize=7,
                    rotation=90,
                )

    ax.set_yscale("log")
    ax.set_ylabel("Index size (MiB, log scale)")
    ax.set_xlabel("Dataset")
    ax.set_xticks(list(x))
    ax.set_xticklabels(pivot.index, rotation=0)
    ax.grid(axis="y", which="both", linestyle=":", linewidth=0.7, alpha=0.65)
    ax.legend(frameon=False, ncol=min(3, len(indexes)), loc="upper left")
    ax.set_title("Index size without GLIN piecewise function")
    fig.tight_layout()

    out = output_dir / f"{prefix}.png"
    fig.savefig(out, dpi=220)
    plt.close(fig)
    return out


def plot_build_time(df: pd.DataFrame, output_dir: Path, prefix: str) -> Path | None:
    if "build_ns" not in df.columns:
        return None
    cur = df.copy()
    cur["build_s"] = pd.to_numeric(cur["build_ns"], errors="coerce") / 1e9
    cur = cur.dropna(subset=["build_s"])
    if cur.empty:
        return None

    datasets = ordered(cur["dataset"].astype(str).tolist(), ["AW", "LW", "ROADS", "PARKS", "OSM_AU_POINTS", "DIAG_S", "DIAG_L", "UNIF_S", "UNIF_L"])
    indexes = ordered(cur["index"].astype(str).tolist(), INDEX_ORDER)
    pivot = (
        cur.groupby(["dataset", "index"], as_index=False)["build_s"]
        .mean()
        .pivot(index="dataset", columns="index", values="build_s")
        .reindex(index=datasets, columns=indexes)
    )

    fig, ax = plt.subplots(figsize=(max(8.0, len(datasets) * 1.3), 4.6))
    x = range(len(pivot.index))
    group_width = 0.78
    bar_width = group_width / max(1, len(indexes))
    for i, index in enumerate(indexes):
        offsets = [pos - group_width / 2 + bar_width / 2 + i * bar_width for pos in x]
        ax.bar(
            offsets,
            pivot[index].tolist(),
            width=bar_width,
            label=INDEX_LABELS.get(index, index),
            color=COLORS.get(index, "#666666"),
            hatch=HATCHES.get(index, ""),
            edgecolor="#222222",
            linewidth=0.6,
        )
    ax.set_ylabel("Build time (s)")
    ax.set_xlabel("Dataset")
    ax.set_xticks(list(x))
    ax.set_xticklabels(pivot.index)
    ax.grid(axis="y", linestyle=":", linewidth=0.7, alpha=0.65)
    ax.legend(frameon=False, ncol=min(3, len(indexes)), loc="upper left")
    ax.set_title("Index initialization time captured with Fig. 8 run")
    fig.tight_layout()
    out = output_dir / f"{prefix}_build_time.png"
    fig.savefig(out, dpi=220)
    plt.close(fig)
    return out


def main() -> None:
    args = parse_args()
    input_csv = Path(args.input_csv)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    df = load_csv(input_csv)
    table = write_table(df, output_dir, args.prefix)
    size_fig = plot_index_size(df, output_dir, args.prefix)
    build_fig = plot_build_time(df, output_dir, args.prefix)

    print(f"rows={len(df)} datasets={df['dataset'].nunique()} indexes={df['index'].nunique()}")
    print(f"table={table}")
    print(f"figure={size_fig}")
    if build_fig:
        print(f"build_time_figure={build_fig}")


if __name__ == "__main__":
    main()
