#!/usr/bin/env python3
"""Measure the four main8 WKT prefixes and render a dataset characterization table."""

import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
DATA_FILES = {"AW": "AREAWATER.csv", "LW": "LINEARWATER.csv", "PARKS": "parks", "ROADS": "roads"}
GEOMETRY_TYPES = {"Point", "MultiPoint", "LineString", "MultiLineString", "Polygon",
                  "MultiPolygon", "GeometryCollection"}


def scientific(value):
    mantissa, exponent = f"{value:.2e}".split("e")
    return r"\(" + mantissa + r"\!\times\!10^{" + str(int(exponent)) + r"}\)"


def validate(row, limit):
    if int(row["loaded_count"]) != limit or limit < 2:
        raise ValueError("Wrong prefix size")
    types = dict(item.split(":") for item in row["geometry_types"].split(";"))
    if not set(types) <= GEOMETRY_TYPES or sum(map(int, types.values())) != limit:
        raise ValueError("Invalid geometry-type accounting")
    for field in ("avg_vertices", "mean_normalized_mbr_area", "mean_normalized_sfc_span",
                  "mean_interval_overlaps", "pair_overlap_fraction"):
        value = float(row[field])
        if not math.isfinite(value) or value < 0:
            raise ValueError("Invalid statistic: " + field)
    for field in ("mean_normalized_mbr_area", "mean_normalized_sfc_span", "pair_overlap_fraction"):
        if float(row[field]) > 1:
            raise ValueError("Normalization outside [0,1]")
    if not math.isclose(float(row["mean_interval_overlaps"]) / (limit - 1),
                        float(row["pair_overlap_fraction"]), abs_tol=1e-12):
        raise ValueError("Overlap normalization differs")
    if not math.isclose(int(row["coordinate_count"]) / limit, float(row["avg_vertices"]),
                        rel_tol=1e-12):
        raise ValueError("Vertex accounting differs")
    if (float(row["cell_xmin"]), float(row["cell_ymin"]), float(row["cell_size"])) != (-180, -90, 5e-7):
        raise ValueError("Encoding differs from main8")
    return types


def table_text(rows):
    lines = [r"\begin{tabular}{@{}llrrrrr@{}}", r"\toprule",
             r"Dataset & Dominant geometry & Objects & Avg. vertices & Norm. MBR area & Norm. SFC span & Avg. overlaps \\",
             r"\midrule"]
    for row in rows:
        types = validate(row, int(row["loaded_count"]))
        dominant = max(types, key=lambda kind: int(types[kind]))
        fraction = int(types[dominant]) / int(row["loaded_count"])
        share = "100" if fraction == 1 else (">99.9" if fraction > 0.999 else f"{fraction * 100:.1f}")
        geometry = dominant + " (" + share + r"\%)"
        cells = [row["dataset"], geometry, f"{int(row['loaded_count']) / 1000:g}K",
                 f"{float(row['avg_vertices']):.1f}", scientific(float(row["mean_normalized_mbr_area"])),
                 scientific(float(row["mean_normalized_sfc_span"])),
                 f"{float(row['mean_interval_overlaps']):.1f}"]
        lines.append(" & ".join(cells) + r" \\")
    lines.append(r"\bottomrule\end{tabular}")
    return "\n".join(lines) + "\n"


def prefix_fingerprint(path, lines):
    digest = hashlib.sha256()
    bytes_read = 0
    with path.open("rb") as stream:
        for _ in range(lines):
            line = stream.readline()
            if not line:
                raise ValueError("Prefix changed while characterizing")
            digest.update(line)
            bytes_read += len(line)
    return {"data_file": str(path), "file_bytes": path.stat().st_size,
            "mtime_ns": path.stat().st_mtime_ns, "consumed_lines": lines,
            "consumed_bytes": bytes_read, "consumed_prefix_sha256": digest.hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-root", type=Path, default=Path("/mnt/hgfs"))
    parser.add_argument("--limit", type=int, default=500000)
    parser.add_argument("--build-dir", type=Path, default=Path("build_current"))
    parser.add_argument("--geos-prefix", type=Path, default=Path.home() / "anaconda3")
    parser.add_argument("--out", type=Path, default=Path("results/deli_dataset_characterization_main8"))
    parser.add_argument("--table", type=Path,
                        default=Path("manuscript/2026.8.28/pvldbstyle-master/figures/dataset_characterization_main8/dataset_characterization.tex"))
    args = parser.parse_args()
    if args.limit < 2:
        raise ValueError("Need at least two objects for prefix statistics")
    os.nice(15)
    args.build_dir.mkdir(parents=True, exist_ok=True)
    args.out.mkdir(parents=True, exist_ok=True)
    binary = args.build_dir / "characterize_deli_datasets"
    compile_command = ["c++", "-std=c++14", "-O2", "-Wall", "-Wextra",
                       "-I" + str(args.geos_prefix / "include"),
                       str(ROOT / "scripts/characterize_deli_datasets.cpp"),
                       "-L" + str(args.geos_prefix / "lib"),
                       "-Wl,-rpath," + str(args.geos_prefix / "lib"), "-lgeos", "-o", str(binary)]
    subprocess.run(compile_command, check=True)
    subprocess.run([str(binary), "--self_test"], check=True)
    rows, inputs, commands = [], [], []
    for dataset, name in DATA_FILES.items():
        source = args.data_root / name
        raw = args.out / (dataset + ".csv")
        command = [str(binary), "--data_file", str(source), "--dataset_name", dataset,
                   "--limit", str(args.limit), "--output_csv", str(raw)]
        subprocess.run(command, check=True)
        with raw.open(newline="") as stream:
            loaded = list(csv.DictReader(stream))
        if len(loaded) != 1 or loaded[0]["dataset"] != dataset:
            raise ValueError("Invalid dataset export")
        row = loaded[0]
        validate(row, args.limit)
        rows.append(row)
        inputs.append(prefix_fingerprint(source, int(row["lines_seen"])))
        commands.append(command)
        print(dataset, row["geometry_types"], "vertices=" + row["avg_vertices"],
              "overlaps=" + row["mean_interval_overlaps"], flush=True)
    with (args.out / "dataset_characterization.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    args.table.parent.mkdir(parents=True, exist_ok=True)
    args.table.write_text(table_text(rows))
    manifest = {"inputs": inputs, "compile_command": compile_command, "commands": commands,
                "tool_sha256": hashlib.sha256((ROOT / "scripts/characterize_deli_datasets.cpp").read_bytes()).hexdigest(),
                "mean_mbr_area": "Mean object MBR coordinate area / prefix bounding-box coordinate area",
                "mean_sfc_span": "Mean (zmax-zmin) / (max(zmax)-min(zmin)); benchmark double endpoints",
                "mean_interval_overlaps": "Mean count of other closed intervals intersecting each object interval",
                "vertices": "Stored coordinate count, including closing polygon-ring coordinates",
                "scope": "Full valid object pool prefix, not final live workload state or query-conditioned overlap"}
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("Dataset table:", args.table)


if __name__ == "__main__":
    main()
