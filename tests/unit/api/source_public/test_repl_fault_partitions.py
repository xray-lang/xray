"""Reject incomplete or forged full-prefix reports independently of execution."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

import run_repl_fault_partitions as driver


INPUTS = {"kind": "inputs", "mode": "history", "stdlib": "X:/stdlib",
          "rooted": "X:/rooted", "file": "X:/file", "allocated_cap": 67108864,
          "live_cap": 8388608, "work_cap": 128000000}
NORMAL = {"kind": "baseline", "point": 0, "sites": 17, "status": 0, "attempts": 17,
          "empty_output": False, "compiler_blocks": 0, "compiler_bytes": 0}
PARTS = [[1, 9, 17], [2, 10], [3, 11], [4, 12], [5, 13], [6, 14], [7, 15], [8, 16]]


def completion(index, count):
    return {"kind": "complete", "sites": 17, "points": count, "index": index,
            "workers": 8, "single_point": False, "compiler_blocks": 0, "compiler_bytes": 0}


def partition(index):
    rows = [copy.deepcopy(INPUTS), copy.deepcopy(NORMAL)]
    for ordinal in PARTS[index]:
        rows.append({"kind": "point", "point": ordinal, "sites": 17, "status": 6,
                     "attempts": ordinal, "empty_output": True,
                     "compiler_blocks": 0, "compiler_bytes": 0})
    rows.append(completion(index, len(PARTS[index])))
    return rows


class Reports(unittest.TestCase):
    def test_exact_eight_process_union(self):
        driver.inputs(INPUTS, "history")
        self.assertEqual(driver.verify_baseline([INPUTS, NORMAL, completion(0, 0)], INPUTS), 17)
        points = []
        for index, expected in enumerate(PARTS):
            actual = driver.verify_partition(partition(index), INPUTS, 17, index)
            self.assertEqual(actual, expected)
            points.extend(actual)
        self.assertEqual(sorted(points), list(range(1, 18)))
        self.assertEqual(len(points), len(set(points)))

    def test_missing_and_duplicate_points_despite_consistent_totals(self):
        for points in ([1, 9], [1, 9, 9, 17], [9, 1, 17], [1, 10, 17], [1, 9, 18]):
            with self.subTest(points=points), self.assertRaises(RuntimeError):
                rows = partition(0)
                template = rows[2]
                rows = rows[:2] + [{**template, "point": p, "attempts": p} for p in points]
                rows.append(completion(0, len(points)))
                driver.verify_partition(rows, INPUTS, 17, 0)

    def test_normal_execution_is_not_a_fault(self):
        for field, value in (("status", 0), ("status", 7), ("attempts", 0),
                             ("empty_output", False), ("compiler_blocks", 1),
                             ("compiler_bytes", 1), ("sites", 16)):
            with self.subTest(field=field, value=value), self.assertRaises(RuntimeError):
                rows = partition(0)
                rows[2][field] = value
                driver.verify_partition(rows, INPUTS, 17, 0)

    def test_workers_reobserve_the_whole_denominator(self):
        rows = partition(0)
        rows[1].update({"sites": 16, "attempts": 16})
        with self.assertRaises(RuntimeError):
            driver.verify_partition(rows, INPUTS, 17, 0)

    def test_no_completion_or_partial_mode_is_not_success(self):
        for field, value in (("points", 2), ("index", 1), ("workers", 7),
                             ("single_point", True), ("single_point", 0),
                             ("compiler_bytes", 8), ("sites", 16)):
            with self.subTest(field=field, value=value), self.assertRaises(RuntimeError):
                rows = partition(0)
                rows[-1][field] = value
                driver.verify_partition(rows, INPUTS, 17, 0)
        with self.assertRaises(RuntimeError):
            driver.verify_partition(partition(0)[:-1], INPUTS, 17, 0)

    def test_malformed_numeric_kinds_are_rejected(self):
        for value in (True, 1.0, "1", None):
            for field in ("point", "status", "attempts", "compiler_blocks", "compiler_bytes"):
                with self.subTest(field=field, value=value), self.assertRaises(RuntimeError):
                    rows = partition(0)
                    rows[2][field] = value
                    driver.verify_partition(rows, INPUTS, 17, 0)

    def test_compiled_paths_and_limits_cannot_be_substituted(self):
        for field, value in (("kind", "point"), ("mode", "write"), ("stdlib", ""),
                             ("rooted", None), ("allocated_cap", 67108865),
                             ("live_cap", 8388609), ("work_cap", 128000001)):
            with self.subTest(field=field), self.assertRaises(RuntimeError):
                row = {**INPUTS, field: value}
                driver.inputs(row, "history")
        rows = partition(0)
        rows[0]["stdlib"] = "X:/other"
        with self.assertRaises(RuntimeError):
            driver.verify_partition(rows, INPUTS, 17, 0)

    def test_baseline_requires_the_owned_normal_result(self):
        for field, value in (("sites", 0), ("status", 6), ("attempts", 18),
                             ("empty_output", True), ("compiler_bytes", 1)):
            with self.subTest(field=field), self.assertRaises(RuntimeError):
                driver.verify_baseline([INPUTS, {**NORMAL, field: value}, completion(0, 0)], INPUTS)

    def test_truncated_and_non_object_json_do_not_qualify(self):
        with tempfile.TemporaryDirectory() as name:
            path = Path(name) / "actual.jsonl"
            for content in ('{"kind":', '[]\n', 'null\n'):
                with self.subTest(content=content), self.assertRaises((RuntimeError, json.JSONDecodeError)):
                    path.write_text(content, encoding="utf-8")
                    driver.records(path)


if __name__ == "__main__":
    unittest.main()
