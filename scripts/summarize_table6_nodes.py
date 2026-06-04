#!/usr/bin/env python3
"""Generate TABLE VI style node-count tables from bench_index_size_wkt CSV."""

from __future__ import annotations

import argparse
from pathlib import Path

import pandas as pd


INDEX_ORDER = ["GLIN", "Boost_Rtree", "GEOS_Quadtree"]
INDEX_LABELS = {
    "GLIN": "GLIN",
    "Boost_Rtree": "Boost R-tree",
    "GEOS_Quadtree": "QuadTree",
}
DATASET_ORDER = ["AW", "LW", "ROADS", "PARKS", "OSM_AU_POINTS", "DIAG_S", "DIAG_L", "UNIF_S", "UNIF_L"]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Summarize TABLE VI: Number of nodes in compared approaches."
    )
    parser.add_argument(
        "--input_csv",
        default="results/fig8_index_size_1000000.csv",
        help="CSV produced by bench_index_size_wkt or run_fig8_index_size.sh.",
    )
    parser.add_argument(
        "--output_csv",
        default="results/table6_nodes_1000000.csv",
        help="Output compact CSV table.",
    )
    parser.add_argument(
        "--output_md",
        default="results/table6_nodes_1000000.md",
        help="Output Markdown table.",
    )
    return parser.parse_args()


def ordered(values: list[str], preferred: list[str]) -> list[str]:
    seen = list(dict.fromkeys(values))
    return [v for v in preferred if v in seen] + sorted(v for v in seen if v not in preferred)


def load_rows(path: Path) -> pd.DataFrame:
    df = pd.read_csv(path)
    required = {"dataset", "index", "node_count", "loaded_count"}
    missing = required - set(df.columns)
    if missing:
        raise SystemExit(f"Missing required columns in {path}: {sorted(missing)}")

    df = df.copy()
    df["node_count"] = pd.to_numeric(df["node_count"], errors="coerce")
    df["loaded_count"] = pd.to_numeric(df["loaded_count"], errors="coerce")
    df = df.dropna(subset=["node_count", "loaded_count"])
    df["node_count"] = df["node_count"].astype("int64")
    df["loaded_count"] = df["loaded_count"].astype("int64")
    return df


def build_table(df: pd.DataFrame) -> pd.DataFrame:
    datasets = ordered(df["dataset"].astype(str).tolist(), DATASET_ORDER)
    indexes = ordered(df["index"].astype(str).tolist(), INDEX_ORDER)

    pivot = (
        df.groupby(["dataset", "index"], as_index=False)["node_count"]
        .mean()
        .pivot(index="dataset", columns="index", values="node_count")
        .reindex(index=datasets, columns=indexes)
    )
    pivot = pivot.rename(columns=INDEX_LABELS)

    loaded = (
        df.groupby("dataset", as_index=True)["loaded_count"]
        .max()
        .reindex(datasets)
        .rename("Loaded records")
    )
    table = (
        pd.concat([loaded, pivot], axis=1)
        .reset_index()
        .rename(columns={"index": "Dataset", "dataset": "Dataset"})
    )
    for col in table.columns:
        if col != "Dataset":
            table[col] = table[col].round(0).astype("Int64")
    return table


def write_markdown(table: pd.DataFrame, path: Path) -> None:
    lines = []
    lines.append("# TABLE VI: Number of nodes in compared approaches")
    lines.append("")
    lines.append("| Dataset | Loaded records | GLIN | Boost R-tree | QuadTree |")
    lines.append("|---|---:|---:|---:|---:|")
    for _, row in table.iterrows():
        lines.append(
            f"| {row['Dataset']} | {int(row['Loaded records']):,} | "
            f"{int(row.get('GLIN', 0)):,} | "
            f"{int(row.get('Boost R-tree', 0)):,} | "
            f"{int(row.get('QuadTree', 0)):,} |"
        )
    lines.append("")
    lines.append(
        "Note: this table uses the same node-count instrumentation as Fig. 8. "
        "GLIN counts ALEX model/data nodes; Boost R-tree counts internal and leaf "
        "nodes from a Boost internal visitor; QuadTree counts GEOS quadtree nodes."
    )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    args = parse_args()
    input_csv = Path(args.input_csv)
    output_csv = Path(args.output_csv)
    output_md = Path(args.output_md)
    output_csv.parent.mkdir(parents=True, exist_ok=True)
    output_md.parent.mkdir(parents=True, exist_ok=True)

    df = load_rows(input_csv)
    table = build_table(df)
    table.to_csv(output_csv, index=False)
    write_markdown(table, output_md)

    print(table.to_string(index=False))
    print(f"csv={output_csv}")
    print(f"markdown={output_md}")


if __name__ == "__main__":
    main()
