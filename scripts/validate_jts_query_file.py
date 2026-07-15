#!/usr/bin/env python3
"""Validate a generated JTS query CSV against its selectivity tag."""

import argparse
import csv
import math
from pathlib import Path


def fraction_from_tag(tag):
    if not tag.endswith("pct"):
        raise ValueError(f"unsupported selectivity tag: {tag}")
    percent = float(tag[:-3].replace("p", "."))
    if percent <= 0.0 or percent > 100.0:
        raise ValueError(f"selectivity tag must be in (0, 100]: {tag}")
    return percent / 100.0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--query_file", required=True)
    parser.add_argument("--expected_tag", required=True)
    parser.add_argument("--expected_count", type=int, default=0)
    args = parser.parse_args()

    path = Path(args.query_file)
    if not path.is_file() or path.stat().st_size == 0:
        raise SystemExit(f"missing or empty query file: {path}")

    expected = fraction_from_tag(args.expected_tag)
    with path.open(newline="") as handle:
        reader = csv.DictReader(handle)
        if not reader.fieldnames or "selectivity" not in reader.fieldnames:
            raise SystemExit(f"query file has no selectivity column: {path}")
        count = 0
        for line_number, row in enumerate(reader, start=2):
            try:
                actual = float(row["selectivity"])
            except (TypeError, ValueError):
                raise SystemExit(
                    f"invalid selectivity at {path}:{line_number}"
                )
            if not math.isclose(actual, expected, rel_tol=1e-12, abs_tol=1e-15):
                raise SystemExit(
                    f"selectivity mismatch in {path}: tag {args.expected_tag} "
                    f"expects {expected:.17g}, file contains {actual:.17g}"
                )
            count += 1

    if count == 0:
        raise SystemExit(f"query file contains no query rows: {path}")
    if args.expected_count > 0 and count < args.expected_count:
        raise SystemExit(
            f"query file has {count} rows, expected at least "
            f"{args.expected_count}: {path}"
        )


if __name__ == "__main__":
    main()
