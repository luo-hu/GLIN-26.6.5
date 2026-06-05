#!/usr/bin/env python3
"""Plot Fig. 5 style CDFs of Zmin/Zmax for real and synthetic datasets."""

from __future__ import annotations

import argparse
import os
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", "figures/.matplotlib_cache")

import matplotlib.pyplot as plt
import pandas as pd


REAL_DATASETS = ["AW", "LW", "ROADS", "PARKS", "OSM_AU_POINTS"]
SYNTHETIC_DATASETS = ["DIAG_S", "DIAG_L", "UNIF_S", "UNIF_L", "ZGAP_WIDE"]
DATASET_ORDER = REAL_DATASETS + SYNTHETIC_DATASETS
RANGE_ORDER = ["Zmin", "Zmax"]
COLORS = {
    "AW": "#3B6EA8",
    "LW": "#70A37F",
    "ROADS": "#C46A4A",
    "PARKS": "#8C6BB1",
    "OSM_AU_POINTS": "#7A7A7A",
    "DIAG_S": "#3B6EA8",
    "DIAG_L": "#1F4E79",
    "UNIF_S": "#C46A4A",
    "UNIF_L": "#8C3E2F",
    "ZGAP_WIDE": "#2F855A",
}
MARKERS = {
    "AW": "o",
    "LW": "s",
    "ROADS": "^",
    "PARKS": "D",
    "OSM_AU_POINTS": "P",
    "DIAG_S": "o",
    "DIAG_L": "s",
    "UNIF_S": "^",
    "UNIF_L": "D",
    "ZGAP_WIDE": "P",
}
LABELS = {
    "OSM_AU_POINTS": "OSM Points",
    "ZGAP_WIDE": "Z-gap Wide",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Plot GLIN Fig. 5 Zmin/Zmax CDFs."
    )
    parser.add_argument(
        "--input_csv",
        default="results/fig5_zrange_cdf_1000000.csv",
        help="CSV produced by export_zrange_cdf_wkt.",
    )
    parser.add_argument(
        "--output_dir",
        default="figures/fig5_zrange_cdf_1000000",
        help="Output figure directory.",
    )
    parser.add_argument("--prefix", default="fig5_zrange_cdf", help="Output prefix.")
    parser.add_argument(
        "--x_column",
        default="z_normalized_dataset",
        choices=["z_normalized_dataset", "z_normalized_u64", "z_value"],
        help="X-axis column. Default normalizes each dataset to [0,1].",
    )
    parser.add_argument("--dpi", type=int, default=220)
    return parser.parse_args()


def ordered(values: list[str], preferred: list[str]) -> list[str]:
    seen = list(dict.fromkeys(values))
    return [v for v in preferred if v in seen] + sorted(
        [v for v in seen if v not in preferred]
    )


def load_csv(path: Path) -> pd.DataFrame:
    df = pd.read_csv(path)
    required = {
        "dataset",
        "range_type",
        "cdf",
        "z_value",
        "z_normalized_dataset",
        "z_normalized_u64",
        "loaded_count",
    }
    missing = required - set(df.columns)
    if missing:
        raise SystemExit(f"Missing required columns in {path}: {sorted(missing)}")

    df = df.copy()
    for column in ["cdf", "z_value", "z_normalized_dataset", "z_normalized_u64", "loaded_count"]:
        df[column] = pd.to_numeric(df[column], errors="coerce")
    df = df.dropna(subset=["cdf", "z_value", "z_normalized_dataset", "z_normalized_u64"])
    if df.empty:
        raise SystemExit(f"No valid rows in {path}")
    return df


def plot_group(
    df: pd.DataFrame,
    datasets: list[str],
    title: str,
    output_path: Path,
    x_column: str,
    dpi: int,
) -> Path | None:
    sub = df[df["dataset"].isin(datasets)].copy()
    if sub.empty:
        return None

    datasets = ordered(sub["dataset"].astype(str).tolist(), datasets)
    fig, axes = plt.subplots(1, 2, figsize=(11.0, 4.5), sharey=True)
    for ax, range_type in zip(axes, RANGE_ORDER):
        cur_range = sub[sub["range_type"] == range_type]
        for dataset in datasets:
            cur = cur_range[cur_range["dataset"] == dataset].sort_values(x_column)
            if cur.empty:
                continue
            markevery = max(1, len(cur) // 12)
            ax.plot(
                cur[x_column],
                cur["cdf"],
                label=LABELS.get(dataset, dataset),
                color=COLORS.get(dataset),
                marker=MARKERS.get(dataset, "o"),
                markevery=markevery,
                markersize=3.5,
                linewidth=1.8,
            )
        ax.set_title(range_type)
        ax.set_xlabel(
            "Normalized Z-address"
            if x_column != "z_value"
            else "Z-address"
        )
        ax.grid(axis="both", linestyle=":", linewidth=0.7, alpha=0.65)
        if x_column == "z_value":
            ax.ticklabel_format(axis="x", style="sci", scilimits=(0, 0))
    axes[0].set_ylabel("CDF")
    axes[1].legend(frameon=False, loc="lower right", fontsize=9)
    fig.suptitle(title, y=1.02)
    fig.tight_layout()
    fig.savefig(output_path, dpi=dpi, bbox_inches="tight")
    plt.close(fig)
    return output_path


def write_summary(df: pd.DataFrame, output_dir: Path, prefix: str) -> Path:
    summary = (
        df.groupby(["dataset", "range_type"], as_index=False)
        .agg(
            loaded_count=("loaded_count", "max"),
            z_min=("z_value", "min"),
            z_max=("z_value", "max"),
            rows=("z_value", "size"),
        )
        .sort_values(["dataset", "range_type"])
    )
    path = output_dir / f"{prefix}_summary.csv"
    summary.to_csv(path, index=False)
    return path


def main() -> None:
    args = parse_args()
    input_csv = Path(args.input_csv)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    df = load_csv(input_csv)

    summary = write_summary(df, output_dir, args.prefix)
    outputs: list[Path] = []
    for maybe_path in [
        plot_group(
            df,
            REAL_DATASETS,
            "Fig. 5 CDFs on real datasets",
            output_dir / f"{args.prefix}_real.png",
            args.x_column,
            args.dpi,
        ),
        plot_group(
            df,
            SYNTHETIC_DATASETS,
            "Fig. 5 CDFs on synthetic rectangle datasets",
            output_dir / f"{args.prefix}_synthetic.png",
            args.x_column,
            args.dpi,
        ),
        plot_group(
            df,
            DATASET_ORDER,
            "Fig. 5 CDFs on real and synthetic datasets",
            output_dir / f"{args.prefix}_all.png",
            args.x_column,
            args.dpi,
        ),
    ]:
        if maybe_path is not None:
            outputs.append(maybe_path)

    print(f"rows={len(df)} datasets={df['dataset'].nunique()}")
    print(f"summary={summary}")
    for path in outputs:
        print(f"figure={path}")


if __name__ == "__main__":
    main()
