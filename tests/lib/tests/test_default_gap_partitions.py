"""Independent rejection checks for complete default-gap partition reports."""
import copy
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("default_gap_runner", ROOT / "tests/unit/xir/run_default_gap_compiler.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def baseline():
    return dict(kind="baseline", mode=0, sites=3, ordinal=0, axis=0, status=0, attempts=3,
                injected=False, empty_output=False, ledger_oom=True, allocated=1024, peak=512, held=256,
                memory_mode=0, memory_limit=0, profile_allocations=True,
                **{key: 0 for key in runner.PHYSICAL})


def complete(index=0, workers=1, points=0, axes=0):
    return dict(kind="complete", mode=0, index=index, workers=workers, sites=3, points=points,
                axes=axes, **{key: 0 for key in runner.PHYSICAL})


def shard(index=0):
    first = baseline()
    points = [dict(first, kind="point", ordinal=n, attempts=n+1, status=7,
                   injected=True, empty_output=True) for n in range(index, 3, 2)]
    return [first, *points, complete(index, 2, len(points))]


class Protocol(unittest.TestCase):
    def test_complete_ordinal_set(self):
        first = baseline()
        self.assertEqual(runner.baseline(first, 0), 3)
        parts = [runner.verify_shard(shard(i), first, i, 2) for i in range(2)]
        self.assertEqual(runner.verify_union(parts, 3), [0, 1, 2])

    def test_missing_duplicate_reordered_and_foreign_points(self):
        valid = shard()
        for change in (lambda r: r.pop(1), lambda r: r.insert(1, copy.deepcopy(r[1])),
                       lambda r: r.__setitem__(slice(1, 3), r[1:3][::-1]),
                       lambda r: r[1].update(ordinal=1), lambda r: r[1].update(ordinal=True)):
            rows = copy.deepcopy(valid)
            change(rows)
            with self.assertRaises(RuntimeError):
                runner.verify_shard(rows, baseline(), 0, 2)
        for parts in ([[0, 2], [1, 2]], [[0], [1]]):
            with self.assertRaises(RuntimeError):
                runner.verify_union(parts, 3)

    def test_denominator_memory_mode_or_fresh_baseline_changes(self):
        for key, value in (("sites", 4), ("sites", 0), ("memory_mode", 1),
                           ("memory_limit", 1), ("profile_allocations", False),
                           ("ledger_oom", False), ("attempts", 2), ("held", 1025)):
            row = baseline()
            row[key] = value
            with self.assertRaises(RuntimeError):
                if key == "sites" and value == 4:
                    runner.verify_shard([row, *shard()[1:]], baseline(), 0, 2)
                else:
                    runner.baseline(row, 0)

    def test_missing_completion_and_physical_residue(self):
        with self.assertRaises(RuntimeError):
            runner.verify_shard(shard()[:-1], baseline(), 0, 2)
        for key in runner.PHYSICAL:
            for index in (1, -1):
                rows = shard()
                rows[index][key] = 1
                with self.assertRaises(RuntimeError):
                    runner.verify_shard(rows, baseline(), 0, 2)

    def test_actual_hit_status_and_empty_output(self):
        for key, value in (("status", 0), ("injected", False), ("empty_output", False), ("attempts", 0)):
            rows = shard()
            rows[1][key] = value
            with self.assertRaises(RuntimeError):
                runner.verify_shard(rows, baseline(), 0, 2)

    def test_held_exact_minus_one_and_plus_one(self):
        first = baseline()
        rows = [first, *(dict(first, kind="axis", axis=n, status=12 if n == 1 else 0,
                              empty_output=n == 1, injected=False, ledger_oom=False) for n in range(3)),
                complete(axes=3)]
        runner.verify_bounds(rows, first)
        for axis in range(3):
            damaged = copy.deepcopy(rows)
            damaged[axis+1]["status"] = 0 if axis == 1 else 12
            with self.assertRaises(RuntimeError):
                runner.verify_bounds(damaged, first)

    def test_sanitizer_output_is_not_success(self):
        self.assertTrue(runner.sanitizer_report(b"ERROR: AddressSanitizer: heap-use-after-free"))
        self.assertTrue(runner.sanitizer_report(b"AddressSanitizer:DEADLYSIGNAL"))
        self.assertFalse(runner.sanitizer_report(b"ordinary success"))


if __name__ == "__main__":
    unittest.main()
