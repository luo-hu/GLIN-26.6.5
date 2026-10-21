#!/usr/bin/env python3
"""Validate normalization and benchmark-compatible WKT prefix accounting."""

import copy
import csv
from pathlib import Path
import subprocess
import tempfile
import unittest

from characterize_deli_datasets import table_text, validate

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build_current/characterize_deli_datasets"
SOURCE = ROOT / "results/deli_dataset_characterization_main8/dataset_characterization.csv"


class CharacterizationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with SOURCE.open(newline="") as stream:
            cls.rows = list(csv.DictReader(stream))

    def test_closed_interval_algorithm(self):
        subprocess.run([str(BINARY), "--self_test"], check=True, capture_output=True)

    def test_loading_and_mbr_area_fixture(self):
        text = ('id,description\n"POLYGON ((0 0,1 0,1 1,0 1,0 0))"\tmetadata\n'
                'POINT EMPTY\nLINESTRING (2 0,3 1)\nPOLYGON ((bad))\nPOINT (3 1)\n'
                'POINT (90 30)\n')
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "data.csv"
            out = Path(directory) / "stats.csv"
            source.write_text(text)
            subprocess.run([str(BINARY), "--data_file", str(source), "--dataset_name", "TEST",
                            "--limit", "3", "--output_csv", str(out)], check=True, capture_output=True)
            with out.open(newline="") as stream:
                row = next(csv.DictReader(stream))
            validate(row, 3)
            self.assertEqual(row["lines_seen"], "6")
            self.assertEqual(row["parse_errors"], "1")
            self.assertEqual(row["empty_geometries"], "1")
            self.assertEqual(row["coordinate_count"], "8")
            self.assertAlmostEqual(float(row["mean_normalized_mbr_area"]), 2 / 9)
            self.assertEqual(row["bbox_maxx"], "3")

    def test_normalization_and_geometry_labels(self):
        source_rows = {"AW": 2292766, "LW": 5838339,
                       "PARKS": 9961896, "ROADS": 72339945}
        for row in self.rows:
            validate(row, 500000)
            self.assertEqual(int(row["source_rows"]), source_rows[row["dataset"]])
        table = table_text(self.rows)
        self.assertIn("Source rows & Avg. vertices", table)
        self.assertNotIn("Pool objects", table)
        self.assertIn("72,339,945 & 11.1", table)
        self.assertIn(r"Polygon (>99.9\%)", table)
        self.assertIn(r"Polygon (79.3\%)", table)
        self.assertIn(r"LineString (97.6\%)", table)
        self.assertNotIn("Mixed", table)

    def test_reject_wrong_count_domain_and_encoding(self):
        for field, value in (("loaded_count", "5"), ("mean_normalized_mbr_area", "2"),
                             ("mean_normalized_sfc_span", "nan"), ("cell_size", "1"),
                             ("coordinate_count", "0"), ("pair_overlap_fraction", "0.5"),
                             ("source_rows", "499999")):
            row = copy.deepcopy(self.rows[0])
            row[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate(row, 500000)


if __name__ == "__main__":
    unittest.main()
