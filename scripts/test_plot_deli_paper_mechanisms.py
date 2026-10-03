#!/usr/bin/env python3
"""Check paper figure derivations and reject broken measurement accounting."""

import copy
import csv
from pathlib import Path
from statistics import median
import tempfile
import unittest

from plot_deli_paper_interim import BOOST, DELI, load_final_rows
from plot_deli_paper_mechanisms import (
    ALL_UPDATE_METRICS, SERVICE_METRICS, UPDATE_METRICS, attribution_records,
    numeric, process_records, update_figure, update_records,
)

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "results/deli_main8_compare_0.5m/dynamic_compare_summary.csv"
OUT = ROOT / "manuscript/2026.8.28/pvldbstyle-master/figures/full_matrix_main8"


class MechanismFiguresTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.final, _, cls.datasets, cls.tags, _, _ = load_final_rows(SOURCE)
        with SOURCE.open(newline="") as stream:
            cls.raw = list(csv.DictReader(stream))

    def test_update_medians_and_ranges(self):
        records = update_records(self.final, self.datasets, self.tags)
        self.assertEqual(len(records), 72)
        for row in records:
            for field, _, _ in ALL_UPDATE_METRICS:
                values = [float(self.final[(row["dataset"], row["mixed_profile"], tag,
                                            row["index"])][field]) for tag in self.tags]
                self.assertEqual(row[field + "_median"], median(values))
                self.assertEqual(row[field + "_min"], min(values))
                self.assertEqual(row[field + "_max"], max(values))
            self.assertEqual(row["selectivities_aggregated"], 4)

    def test_update_latency_and_service_figures_are_separate(self):
        self.assertEqual([field for field, _, _ in UPDATE_METRICS],
                         ["p95_insert_ms", "p95_delete_ms", "p99_insert_ms", "p99_delete_ms"])
        self.assertEqual([field for field, _, _ in SERVICE_METRICS],
                         ["insert_tps", "delete_tps"])
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            for stem, metrics in (("latency", UPDATE_METRICS), ("service", SERVICE_METRICS)):
                update_figure(self.final, out, self.datasets, self.tags, stem, metrics=metrics)
                self.assertGreater((out / (stem + ".pdf")).stat().st_size, 0)
                self.assertGreater((out / (stem + ".png")).stat().st_size, 0)

    def test_query_counts_and_mean_time(self):
        records = attribution_records(self.final, self.datasets, self.tags)
        self.assertEqual(len(records), 96)
        for row in records:
            raw = self.final[(row["dataset"], row["mixed_profile"],
                              row["selectivity_tag"], row["index"])]
            queries = float(raw["query_count"])
            self.assertEqual(row["mean_query_us"], float(raw["avg_query_ns"]) / 1000)
            self.assertEqual(row["geos_us_per_query"], float(raw["geos_exact_ns"]) / queries / 1000)
            self.assertAlmostEqual(row["other_query_us"] + row["geos_us_per_query"],
                                   row["mean_query_us"])
            for output, source in (("mbr_candidates_per_query", "mbr_candidates"),
                                   ("exact_calls_per_query", "exact_calls"),
                                   ("shortcuts_per_query", "predicate_shortcuts")):
                self.assertEqual(row[output], float(raw[source]) / queries)
            if row["index"] == BOOST:
                self.assertIsNone(row["compact_scanned_per_query"])
                self.assertIsNone(row["delta_scanned_per_query"])
            else:
                self.assertEqual(row["compact_scanned_per_query"],
                                 float(raw["compact_records_scanned"]) / queries)
                self.assertEqual(row["delta_scanned_per_query"],
                                 float(raw["delta_records_scanned"]) / queries)

    def test_process_windows_sum_to_cumulative_totals(self):
        records = process_records(SOURCE, self.datasets, self.tags)
        self.assertEqual(len(records), 960)
        groups = {}
        for row in records:
            key = (row["dataset"], row["mixed_profile"], row["selectivity_tag"])
            groups.setdefault(key, []).append(row)
            deli = next(r for r in self.raw if r["index"] == DELI
                        and (r["dataset"], r["mixed_profile"], r["selectivity_tag"]) == key
                        and int(r["operation_count"]) == row["operation_count"])
            self.assertEqual(row["compactions_window"], int(deli["local_compaction_count_stage"]))
            self.assertEqual(row["compaction_ms_window"], float(deli["local_compaction_ns_stage"]) / 1e6)
        for rows in groups.values():
            self.assertEqual(len(rows), 20)
            self.assertEqual([r["operation_count"] for r in rows], list(range(10000, 200001, 10000)))
            self.assertEqual(sum(r["compactions_window"] for r in rows), rows[-1]["compactions_total"])
            self.assertAlmostEqual(sum(r["compaction_ms_window"] for r in rows),
                                   rows[-1]["compaction_ms_total"])

    def test_exported_complete_metrics_preserve_source(self):
        with (OUT / "complete_final_metrics.csv").open(newline="") as stream:
            rows = list(csv.DictReader(stream))
        self.assertEqual(len(rows), 288)
        for row in rows:
            raw = self.final[(row["dataset"], row["mixed_profile"], row["selectivity_tag"], row["index"])]
            for field in row:
                if field in ("dataset", "mixed_profile", "selectivity_tag", "index"):
                    self.assertEqual(row[field], raw[field])
                else:
                    self.assertEqual(float(row[field]), float(raw[field]))

    def test_reject_nonfinite_and_negative_counts(self):
        for bad in ("nan", "inf", "-1"):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                numeric({"count": bad}, "count")
        with self.assertRaises(ValueError):
            numeric({"count": "0"}, "count", positive=True)

    def test_reject_impossible_geos_and_refinement_accounting(self):
        key = next(key for key in self.final if key[-1] == DELI)
        for field, value in (("geos_exact_ns", "1e30"), ("exact_calls", "0")):
            final = copy.deepcopy(self.final)
            final[key][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                attribution_records(final, self.datasets, self.tags)

    def test_reject_duplicate_missing_and_inconsistent_windows(self):
        base = [r for r in self.raw if r["index"] in (DELI, BOOST)]
        variants = [base + [base[0]], base[1:]]
        for field, value in (("local_compaction_count_stage", "99999"),
                             ("local_compaction_ns_total", "0"),
                             ("avg_local_delta_size", "1e30"),
                             ("live_count", "-1")):
            broken = copy.deepcopy(base)
            row = next(r for r in broken if r["index"] == DELI and int(r["operation_count"]) == 200000)
            row[field] = value
            variants.append(broken)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "broken.csv"
            for rows in variants:
                with path.open("w", newline="") as stream:
                    writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
                    writer.writeheader()
                    writer.writerows(rows)
                with self.assertRaises(ValueError):
                    process_records(path, self.datasets, self.tags)


if __name__ == "__main__":
    unittest.main()
