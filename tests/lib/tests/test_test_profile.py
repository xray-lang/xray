import io
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

from _support import load_module


ROOT = Path(__file__).resolve().parents[3]
profile = load_module("test_profile_report", ROOT / "scripts" / "test_profile.py")


class TestProfileTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="xr-test-profile-")
        self.addCleanup(self.temporary.cleanup)
        self.build = Path(self.temporary.name)

    def write_ninja(self, rows: str) -> None:
        (self.build / ".ninja_log").write_text("# ninja log v5\n" + rows, encoding="utf-8")

    def write_costs(self, rows: str) -> None:
        directory = self.build / "Testing" / "Temporary"
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "CTestCostData.txt").write_text(rows, encoding="utf-8")

    def test_latest_rebuild_replaces_historical_maximum(self) -> None:
        self.write_ninja("0\t9000\t100\tobj/a.obj\ta\n"
                         "2\t502\t200\tobj/a.obj\tb\n"
                         "0\t300\t100\tobj/b.obj\tc\n")
        self.assertEqual(profile.read_build_edges(self.build),
                         [("obj/a.obj", 0.5), ("obj/b.obj", 0.3)])

    def test_zero_duration_rebuild_replaces_old_duration(self) -> None:
        self.write_ninja("0\t9000\t100\tobj/a.obj\ta\n"
                         "5\t5\t200\tobj/a.obj\ta\n")
        self.assertEqual(profile.read_build_edges(self.build), [("obj/a.obj", 0.0)])

    def test_same_basename_in_different_directories_stays_distinct(self) -> None:
        self.write_ninja("0\t1000\t100\tone/file.obj\ta\n"
                         "0\t500\t100\ttwo/file.obj\tb\n")
        self.assertEqual(profile.read_build_edges(self.build),
                         [("one/file.obj", 1.0), ("two/file.obj", 0.5)])

    def test_multi_output_and_path_spellings_are_not_guessed_edges(self) -> None:
        absolute = (self.build / "generated/a.h").as_posix()
        self.write_ninja("0\t1000\t100\tgenerated/a.h\ta\n"
                         "0\t1000\t100\tgenerated/b.h\ta\n"
                         f"0\t1000\t100\t{absolute}\ta\n")
        self.assertEqual(dict(profile.read_build_edges(self.build)),
                         {"generated/a.h": 1.0, "generated/b.h": 1.0, absolute: 1.0})

    def test_compaction_order_and_mtime_do_not_create_invocation_boundaries(self) -> None:
        first = "100\t1100\t500\tobj/a.obj\ta\n"
        second = "0\t2000\t0\tobj/b.obj\tb\n"
        self.write_ninja(first + second)
        before = profile.read_build_edges(self.build)
        self.write_ninja(second + first)
        self.assertEqual(profile.read_build_edges(self.build), before)
        with (self.build / ".ninja_log").open("a", encoding="utf-8") as output:
            output.write("0\t300\t0\tobj/a.obj\tc\n")
        self.assertEqual(profile.read_build_edges(self.build),
                         [("obj/b.obj", 2.0), ("obj/a.obj", 0.3)])

    def test_invalid_and_incomplete_records_are_not_durations(self) -> None:
        self.write_ninja("0\t500\t100\tvalid.obj\ta\n"
                         "bad\t800\t100\tbad.obj\ta\n"
                         "900\t100\t100\tnegative.obj\ta\n"
                         "-1\t800\t100\tnegative-start.obj\ta\n"
                         "0\t800\tbad\tbad-mtime.obj\ta\n"
                         "0\t800\t100\tbad-hash.obj\txx\n"
                         "0\t800\t100\t\ta\n"
                         "0\t800\t100\tmissing-hash.obj\n"
                         "0\t800\t100\tpartial.obj\ta")
        self.assertEqual(profile.read_build_edges(self.build), [("valid.obj", 0.5)])

    def test_only_costs_with_previous_runs_are_included(self) -> None:
        self.write_costs("declared 0 900\nran 2 1.5\nzero 1 0\n"
                         "reset 1 5\nreset 0 900\n")
        self.assertEqual(profile.read_test_costs(self.build), [("ran", 1.5), ("zero", 0.0)])

    def test_cost_failure_section_and_invalid_numbers_are_not_estimates(self) -> None:
        self.write_costs("valid 1 0.5\ninvalid x 4\nnegative_runs -1 4\n"
                         "infinite 1 inf\nnot_a_number 1 nan\nnegative_cost 1 -3\n"
                         "---\nfailed test name\nnot_cost 1 700\n")
        self.assertEqual(profile.read_test_costs(self.build), [("valid", 0.5)])

    def test_missing_empty_and_unrun_data_are_not_platform_claims(self) -> None:
        self.assertEqual(profile.read_build_edges(self.build), [])
        self.assertEqual(profile.read_test_costs(self.build), [])
        self.write_ninja("")
        self.write_costs("declared 0 900\n---\n")
        self.assertEqual(profile.read_build_edges(self.build), [])
        self.assertEqual(profile.read_test_costs(self.build), [])
        output = io.StringIO()
        with redirect_stdout(output):
            profile.report_tests([], 20)
            profile.report_build([], 20)
        self.assertIn("no recorded", output.getvalue())
        self.assertNotIn("not a Ninja tree", output.getvalue())
        self.assertNotIn("Makefiles", output.getvalue())

    def test_reports_label_historical_scope_and_do_not_claim_current_wall_time(self) -> None:
        output = io.StringIO()
        with redirect_stdout(output):
            profile.report_tests([("one", 1.0)], 20)
            profile.report_build([("obj/a.obj", 1.0), ("obj/b.obj", 1.0)], 20)
        text = output.getvalue()
        self.assertIn("Scheduling estimates, not the latest run's measured wall times", text)
        self.assertIn("not the latest invocation", text)
        self.assertIn("rows are not unique edges", text)
        self.assertIn("top 1 lanes", text)
        self.assertIn("the other 0 tests", text)
        self.assertNotIn("if run serially", text)
        self.assertNotIn("slowest single edge", text)


if __name__ == "__main__":
    unittest.main()
