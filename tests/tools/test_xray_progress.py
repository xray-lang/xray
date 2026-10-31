"""Exercise immutable smoke identities, paired repairs and real Git deletions."""

from copy import deepcopy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import xray_progress as progress
import merge_slice
import xray_l1

ROOT = Path(__file__).resolve().parents[2]


class MetricsTests(unittest.TestCase):
    def setUp(self):
        self.baseline = json.loads((ROOT / "tests/smoke/original-baseline.json").read_text(encoding="utf-8"))
        self.cases = deepcopy(self.baseline["cases"])
        self.cases.append({"id": "demos/new.xr", "path": "demos/new.xr", "mode": "run", "expect": "pass"})
        self.results = []
        for row in self.cases:
            stages = ["build", "run"] if row["mode"] == "build-run" else [row["mode"]]
            self.results.append({"id": row["id"], "status": row["expect"], "executed": True,
                                 "commands": [{"stage": stage, "exit_code": 0, "timed_out": False}
                                              for stage in stages]})

    def metrics(self):
        return progress.fixed_metrics(self.baseline, self.cases, self.results)

    def test_added_demo_does_not_repair_an_original_failure(self):
        value = self.metrics()
        self.assertEqual((value["original_pass"], value["original_total"]), (3, 60))
        self.assertEqual(value["original_failed_demo_repaired"], 0)

    def test_vm_only_or_unexecuted_native_does_not_count_as_repaired(self):
        name = "demos/04-functional/functional.xr"
        next(row for row in self.cases if row["id"] == name)["expect"] = "pass"
        next(row for row in self.results if row["id"] == name)["status"] = "pass"
        self.assertEqual(self.metrics()["original_failed_demo_repaired"], 0)
        self.cases.append({"id": "native:" + name, "path": name, "mode": "build-run", "expect": "pass"})
        native = {"id": "native:" + name, "status": "pass", "executed": False,
                  "commands": [{"stage": stage, "exit_code": 0, "timed_out": False}
                               for stage in ("build", "run")]}
        self.results.append(native)
        self.assertEqual(self.metrics()["original_failed_demo_repaired"], 0)
        native["executed"] = True
        self.assertEqual(self.metrics()["repaired_demo_ids"], [name])
        self.cases[-1]["path"] = "demos/new.xr"
        self.assertEqual(self.metrics()["original_failed_demo_repaired"], 0)
        self.cases[-1]["path"] = name
        native["commands"][1]["timed_out"] = True
        self.assertEqual(self.metrics()["original_failed_demo_repaired"], 0)

    def test_removed_replaced_or_demoted_original_is_rejected(self):
        for edit in ("remove", "replace", "demote"):
            with self.subTest(edit=edit):
                cases = deepcopy(self.cases)
                if edit == "remove":
                    cases.pop(0)
                elif edit == "replace":
                    cases[0]["path"] = "new-demo.xr"
                else:
                    next(row for row in cases if row["id"] == "demos/01-basics/hello.xr")["expect"] = "gap"
                with self.assertRaises(ValueError):
                    progress.fixed_metrics(self.baseline, cases, self.results)

    def test_r4_weekly_requires_both_product_gain_and_legacy_reduction(self):
        old = {"stage": "R4", "week_ending": "2026-10-31", "legacy": {"c_h_lines": 100},
               "metrics": {"original_failed_demo_repaired": 0, "repaired_demo_ids": []}}
        new = {"stage": "R4", "week_ending": "2026-11-07", "legacy": {"c_h_lines": 99},
               "metrics": {"original_failed_demo_repaired": 1, "repaired_demo_ids": ["original"]}}
        progress.weekly_progress(old, new)
        for field in ("legacy", "metrics", "week_ending"):
            with self.subTest(field=field):
                bad = deepcopy(new)
                bad[field] = old[field]
                with self.assertRaises(ValueError):
                    progress.weekly_progress(old, bad)
        done = deepcopy(new)
        done["legacy"]["c_h_lines"] = 0
        done["metrics"] = {"original_failed_demo_repaired": 20, "repaired_demo_ids": [str(i) for i in range(20)]}
        later = deepcopy(done)
        later["week_ending"] = "2026-11-14"
        progress.weekly_progress(done, later)
        later["legacy"]["c_h_lines"] = 1
        with self.assertRaises(ValueError):
            progress.weekly_progress(done, later)

    def test_r3_freezes_growth_but_keeps_weekly_original_demo_gain(self):
        old = {"stage": "R3", "week_ending": "2026-10-31", "legacy": {"c_h_lines": 100},
               "metrics": {"original_failed_demo_repaired": 0, "repaired_demo_ids": []}}
        new = {"stage": "R3", "week_ending": "2026-11-07", "legacy": {"c_h_lines": 100},
               "metrics": {"original_failed_demo_repaired": 1, "repaired_demo_ids": ["original"]}}
        progress.weekly_progress(old, new)
        new["legacy"]["c_h_lines"] = 99
        progress.weekly_progress(old, new)
        for lines in (100, 99):
            bad = deepcopy(new)
            bad["legacy"]["c_h_lines"] = lines
            bad["metrics"] = deepcopy(old["metrics"])
            with self.assertRaises(ValueError):
                progress.weekly_progress(old, bad)
        new["legacy"]["c_h_lines"] = 101
        with self.assertRaises(ValueError):
            progress.weekly_progress(old, new)

    def test_weekly_stage_is_explicit_and_cannot_go_backwards(self):
        old = {"stage": "R3", "week_ending": "2026-10-31", "legacy": {"c_h_lines": 100},
               "metrics": {"original_failed_demo_repaired": 0, "repaired_demo_ids": []}}
        new = {"stage": "R4", "week_ending": "2026-11-07", "legacy": {"c_h_lines": 99},
               "metrics": {"original_failed_demo_repaired": 1, "repaired_demo_ids": ["original"]}}
        progress.weekly_progress(old, new)
        for previous_stage, stage in ((None, "R3"), ("R3", None), ("R3", "R2"), ("R4", "R3")):
            previous, current = deepcopy(old), deepcopy(new)
            previous["stage"], current["stage"] = previous_stage, stage
            with self.assertRaises(ValueError):
                progress.weekly_progress(previous, current)
        new["legacy"]["c_h_lines"] = 100
        with self.assertRaises(ValueError):
            progress.weekly_progress(old, new)

    def test_manifest_cannot_drop_smoke_from_usability_policy(self):
        plan = json.loads((ROOT / "scripts/l1-v0.json").read_text(encoding="utf-8"))
        xray_l1.manifest(json.dumps(plan).encode())
        plan["tests"].remove("smoke")
        with self.assertRaises(ValueError):
            xray_l1.manifest(json.dumps(plan).encode())

    def test_document_check_rejects_completed_source_left_unchecked(self):
        with tempfile.TemporaryDirectory(prefix="xray-checklist-") as directory:
            docs = Path(directory)
            task = docs / progress.TASK
            task.parent.mkdir(parents=True)
            text = "- [x] R1.2 stdlib\n- [x] R1.3 BOM\n- [x] R1.6 CLI\n"
            task.write_text(text, encoding="utf-8")
            self.assertTrue(progress.document_state(ROOT, docs)["R1.3"])
            for old, new in (("[x] R1.2", "[ ] R1.2"), ("[x] R1.3", "[ ] R1.3"),
                             ("[x] R1.6", "[ ] R1.6")):
                task.write_text(text.replace(old, new), encoding="utf-8")
                with self.assertRaises(ValueError):
                    progress.document_state(ROOT, docs)


class GitTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="xray-progress-")
        self.root = Path(self.temporary.name)
        self.git("init", "-b", "main")
        self.git("config", "user.name", "Progress Fixture")
        self.git("config", "user.email", "fixture@example.com")
        self.git("config", "core.autocrlf", "false")
        (self.root / "src/ir").mkdir(parents=True)
        (self.root / "src/ir/old.c").write_text("one\ntwo\n", encoding="utf-8")
        self.commit()
        self.base = self.git("rev-parse", "HEAD")

    def tearDown(self):
        self.temporary.cleanup()

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.root), *args],
                                       stderr=subprocess.DEVNULL, text=True).strip()

    def commit(self):
        self.git("add", ".")
        self.git("commit", "-m", "Change fixture")

    def test_line_deletion_allowed_but_rewrite_rejected(self):
        file = self.root / "src/ir/old.c"
        file.write_text("one\n", encoding="utf-8")
        self.commit()
        self.assertEqual(progress.legacy_deletions(self.root, self.base)["c_h_lines"], 1)
        file.write_text("replacement\n", encoding="utf-8")
        self.commit()
        with self.assertRaises(ValueError):
            progress.legacy_deletions(self.root, self.base)

    def test_empty_new_file_and_move_out_are_rejected(self):
        (self.root / "src/ir/empty.h").touch()
        self.commit()
        with self.assertRaises(ValueError):
            progress.legacy_deletions(self.root, self.base)
        base = self.git("rev-parse", "HEAD")
        self.git("mv", "src/ir/old.c", "retained.c")
        self.commit()
        with self.assertRaises(ValueError):
            progress.legacy_deletions(self.root, base)

    def test_whole_file_deletion_and_review_scope(self):
        (self.root / "src/ir/old.c").unlink()
        self.commit()
        self.assertEqual(progress.legacy_deletions(self.root, self.base)["files"], 0)
        with self.assertRaises(ValueError):
            merge_slice.review_scope(self.root, self.base, "HEAD", {"authorRole": "a", "reviewerRole": "a"})
        self.assertTrue(merge_slice.review_scope(self.root, self.base, "HEAD",
                                                {"authorRole": "a", "reviewerRole": "b"})["independent"])

    def test_test_and_script_deletions_also_require_independent_review(self):
        for path in ("tests/old_driver.c", "scripts/old_gate.py"):
            file = self.root / path
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text("old\n", encoding="utf-8")
        self.commit()
        base = self.git("rev-parse", "HEAD")
        for path in ("tests/old_driver.c", "scripts/old_gate.py"):
            (self.root / path).unlink()
        self.commit()
        with self.assertRaises(ValueError):
            merge_slice.review_scope(self.root, base, "HEAD", {"authorRole": "a", "reviewerRole": "a"})

    def test_leaf_review_allowed_but_header_and_owner_crossing_require_independence(self):
        (self.root / "src/frontend").mkdir()
        (self.root / "src/frontend/leaf.c").write_text("leaf\n", encoding="utf-8")
        self.commit()
        review = {"authorRole": "a", "reviewerRole": "a"}
        self.assertFalse(merge_slice.review_scope(self.root, self.base, "HEAD", review)["independent"])
        base = self.git("rev-parse", "HEAD")
        (self.root / "src/frontend/interface.h").write_text("interface\n", encoding="utf-8")
        self.commit()
        with self.assertRaises(ValueError):
            merge_slice.review_scope(self.root, base, "HEAD", review)
        base = self.git("rev-parse", "HEAD")
        (self.root / "src/frontend/leaf.c").write_text("updated\n", encoding="utf-8")
        (self.root / "src/vm").mkdir()
        (self.root / "src/vm/leaf.c").write_text("consumer\n", encoding="utf-8")
        self.commit()
        with self.assertRaises(ValueError):
            merge_slice.review_scope(self.root, base, "HEAD", review)


if __name__ == "__main__":
    unittest.main()
