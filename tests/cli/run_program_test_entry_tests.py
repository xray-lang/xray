"""Exercise the actual test command and its shared Program execution owner."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class ProgramTestCommand(unittest.TestCase):
    def invoke(self, files, expected, *options, contains=(), absent=()):
        with tempfile.TemporaryDirectory(prefix="xray-program-tests-") as directory:
            root = Path(directory)
            entries = []
            for name, source in files.items():
                path = root / name
                path.write_text(source, encoding="utf-8")
                if name.endswith("_test.xr"):
                    entries.append(str(path))
            result = subprocess.run(
                [str(XRAY), "test", *entries, *options], cwd=ROOT,
                capture_output=True, timeout=15,
                env={**os.environ, "NO_COLOR": "1"},
            )
            text = result.stdout.decode("utf-8", errors="replace")
            errors = result.stderr.decode("utf-8", errors="replace")
            self.assertEqual(result.returncode, expected, text + errors)
            self.assertNotIn("AddressSanitizer", errors)
            self.assertNotIn("runtime error:", errors)
            for value in contains:
                self.assertIn(value, text + errors)
            for value in absent:
                self.assertNotIn(value, text + errors)
            return text

    def test_module_state_hooks_skip_and_import_discovery(self):
        self.invoke({
            "library.xr": '''
const counter = Atomic(40)
export fn bump() -> i64 { return counter.fetchAdd(1) + 1 }
@test
fn importedTestMustNotRun() { assert(false) }
''',
            "main_test.xr": '''
import "./library"
var counter: i64 = 0
@before_all
fn setup() { counter = library.bump(); assert(counter == 41) }
@before_each
fn before() { counter = counter + 1 }
@after_each
fn after() { counter = counter + 1 }
@test
fn first() { assert(counter == 42); assert(library.bump() == 42) }
@test
fn second() { assert(counter == 44); assert(library.bump() == 43) }
@after_all
fn done() { assert(counter == 45) }
@test(skip)
fn skipped() { assert(false) }
''',
        }, 0, "--jobs", "1", contains=("2 passed", "1 skipped"))

    def test_assertion_message_is_reported_and_teardown_runs(self):
        self.invoke({"main_test.xr": '''
@test
fn broken() { assert(false, "exact assertion message") }
@after_all
fn done() { print("teardown completed") }
'''}, 1, "--jobs", "1", contains=("1 failed", "exact assertion message", "teardown completed"))

    def test_failure_continues_and_teardown_runs(self):
        self.invoke({"main_test.xr": '''
var counter: i64 = 0
@test
fn broken() { assert(false) }
@after_each
fn after() { counter = counter + 1 }
@test
fn next() { assert(counter == 1) }
@after_all
fn done() { assert(counter == 2); print("teardown completed") }
'''}, 1, "--jobs", "1", contains=("1 passed", "1 failed", "broken", "teardown completed"))

    def test_fail_fast_still_tears_down(self):
        self.invoke({"main_test.xr": '''
@test
fn broken() { assert(false) }
@test
fn next() { print("UNEXPECTED NEXT") }
@after_all
fn done() { print("teardown completed") }
'''}, 1, "--fail-fast", "--jobs", "1", contains=("0 passed", "teardown completed"),
                    absent=("UNEXPECTED NEXT",))

    def test_after_hooks_cannot_silently_pass(self):
        for annotation in ("after_each", "after_all"):
            with self.subTest(annotation=annotation):
                self.invoke({"main_test.xr": f'''
@test
fn okay() {{ assert(true) }}
@{annotation}
fn teardown() {{ assert(false) }}
'''}, 1, contains=("teardown", "1 failed"))

    def test_setup_failure_is_an_error_and_teardown_runs(self):
        for annotation in ("before_each", "before_all"):
            with self.subTest(annotation=annotation):
                self.invoke({"main_test.xr": f'''
@{annotation}
fn setup() {{ assert(false) }}
@test
fn never() {{ print("UNEXPECTED TEST") }}
@after_all
fn done() {{ print("teardown completed") }}
'''}, 1, contains=("setup", "teardown completed"), absent=("UNEXPECTED TEST",))

    def test_filter_and_zero_executed_rejection(self):
        source = "@test\nfn kept() { assert(true) }\n@test\nfn rejected() { assert(false) }\n"
        self.invoke({"main_test.xr": source}, 0, "--filter", "kept",
                    contains=("1 passed", "1 skipped"))
        self.invoke({"main_test.xr": source}, 1, "--filter", "absent")
        self.invoke({"main_test.xr": "@test(skip)\nfn skipped() { assert(false) }\n"}, 1)
        self.invoke({"main_test.xr": "fn ordinary() {}\n"}, 1)

    def test_files_have_isolated_instances(self):
        source = '''
var counter: i64 = 0
@test
fn isolated() { counter = counter + 1; assert(counter == 1) }
'''
        self.invoke({"first_test.xr": source, "second_test.xr": source}, 0,
                    "--jobs", "2", contains=("2 passed",))

    def job_files(self, root, *, failing=False, count=2):
        fixture = ROOT / "tests/lir/programs/test-role-jobs-rendezvous.xr"
        paths = []
        for i in range(count):
            text = fixture.read_text(encoding="utf-8")
            for marker in (101, 201, 301):
                text = text.replace(f"print({marker})", f"print({marker + i})")
            if failing:
                text = text.replace('assert(state == 1, "case state survived the rendezvous")',
                                    f'assert(false, "failure in file {i + 1}")')
            path = root / f"{i + 1}-file.xr"
            path.write_text(text, encoding="utf-8")
            paths.append(path)
        return paths

    def run_job_files(self, root, paths, options, markers, *, cancel=False, break_stdout=False):
        with subprocess.Popen([XRAY, "test", *reversed(paths), *options], cwd=root,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              env={**os.environ, "NO_COLOR": "1"}) as child:
            with ThreadPoolExecutor(max_workers=1) as reader:
                try:
                    first = [reader.submit(child.stdout.readline).result(timeout=15) for _ in markers]
                    self.assertCountEqual(first, markers)
                    self.assertIsNone(child.poll())
                    if cancel:
                        child.kill()
                    else:
                        if break_stdout:
                            child.stdout.close()
                            child.stdout = None
                        (root / "release").touch()
                    output, errors = child.communicate(timeout=15)
                finally:
                    if child.poll() is None:
                        child.kill()
                        child.wait(timeout=5)
        return child.returncode, output, errors

    def test_file_jobs_rendezvous_and_sorted_report(self):
        with tempfile.TemporaryDirectory(prefix="xray-test-job-rendezvous-") as directory:
            root = Path(directory)
            paths = self.job_files(root)
            report = root / "serial.json"
            serial = self.run_job_files(root, paths, ["--jobs", "1", "--report", report], [b"101\n"])
            self.assertEqual(serial, (0, b"201\n301\n102\n202\n302\n", b""))
            (root / "release").unlink()
            parallel_report = root / "parallel.json"
            code, output, errors = self.run_job_files(root, paths,
                ["--jobs", "2", "--report", parallel_report], [b"101\n", b"102\n"])
            self.assertEqual((code, errors), (0, b""))
            self.assertCountEqual(output.splitlines(), [b"201", b"202", b"301", b"302"])
            serial_files = json.loads(report.read_bytes())["files"]
            parallel_files = json.loads(parallel_report.read_bytes())["files"]
            self.assertEqual(serial_files, parallel_files)
            self.assertEqual([Path(row["path"]) for row in parallel_files], paths)
            self.assertEqual([(file["passed"], file["skipped"]) for file in parallel_files], [(1, 1), (1, 1)])

    def test_file_jobs_fail_fast_keeps_inflight_cleanup(self):
        with tempfile.TemporaryDirectory(prefix="xray-test-job-fail-fast-") as directory:
            root = Path(directory)
            paths = self.job_files(root, failing=True, count=3)
            report = root / "report.json"
            code, output, errors = self.run_job_files(root, paths,
                ["--jobs", "2", "--fail-fast", "--report", report], [b"101\n", b"102\n"])
            self.assertEqual((code, errors), (1, b""))
            self.assertCountEqual(output.splitlines(), [b"201", b"202", b"301", b"302"])
            rows = json.loads(report.read_bytes())["files"]
            self.assertEqual([Path(row["path"]) for row in rows], paths[:2])
            self.assertEqual([(row["passed"], row["failed"], row["errors"], row["skipped"]) for row in rows],
                             [(0, 1, 0, 1), (0, 1, 0, 1)])
            self.assertEqual([row["cases"][0]["message"] for row in rows],
                             [f"[Uncaught Panic] E0445: failure in file {i}" for i in (1, 2)])

    def test_file_jobs_keep_string_and_integer_records_whole(self):
        with tempfile.TemporaryDirectory(prefix="xray-test-job-output-") as directory:
            root = Path(directory)
            paths = self.job_files(root)
            for i, path in enumerate(paths):
                text = path.read_text(encoding="utf-8")
                payload = chr(ord("A") + i) * 64
                additional = f'    for (i in 0..750) {{ print("{payload}"); print({700 + i}) }}\n'
                text = text.replace("    assert(state == 1,", additional + "    assert(state == 1,", 1)
                path.write_text(text, encoding="utf-8")
            report = root / "report.json"
            code, output, errors = self.run_job_files(root, paths, ["--jobs", "2", "--report", report],
                [b"101\n", b"102\n"])
            self.assertEqual((code, errors), (0, b""))
            lines = output.split(b"\n")
            self.assertEqual(lines.pop(), b"")
            self.assertCountEqual(lines, [b"A" * 64, b"B" * 64, b"700", b"701"] * 750 +
                                             [b"201", b"202", b"301", b"302"])

    def test_file_jobs_broken_stdout_does_not_hold_record_lock(self):
        with tempfile.TemporaryDirectory(prefix="xray-test-job-output-failure-") as directory:
            root = Path(directory)
            paths = self.job_files(root)
            for path in paths:
                text = path.read_text(encoding="utf-8")
                text = text.replace("    assert(state == 1,", '    print("write after the pipe closes")\n'
                                    "    assert(state == 1,", 1)
                path.write_text(text, encoding="utf-8")
            report = root / "report.json"
            code, output, errors = self.run_job_files(root, paths, ["--jobs", "2", "--report", report],
                [b"101\n", b"102\n"], break_stdout=True)
            self.assertEqual((code, output, errors), (1, None, b""))
            rows = json.loads(report.read_bytes())["files"]
            self.assertEqual([Path(row["path"]) for row in rows], paths)
            self.assertEqual([(row["passed"], row["failed"], row["errors"]) for row in rows], [(0, 0, 2)] * 2)
            for row in rows:
                self.assertIn("test output failed", row["cases"][0]["message"])
                self.assertIn("hook @after_each", row["cases"][0]["message"])
                self.assertIn("hook @after_all", row["error"])

    def test_file_jobs_console_owns_diagnostics_in_serial_file_order(self):
        with tempfile.TemporaryDirectory(prefix="xray-test-job-diagnostics-") as directory:
            root = Path(directory)
            cases = {
                "1-null chosen.xr": '@test\nfn rejected() { var empty: i64? = null; print(empty!) }\n',
                "2-message.xr": '@test\nfn rejected() { assert(false, "a qualified message") }\n',
                "3-source.xr": '@test\nfn rejected() { noSuchFunction() }\n',
            }
            paths = [root / name for name in cases]
            for path, text in zip(paths, cases.values()):
                path.write_text(text, encoding="utf-8")
            runs = [subprocess.run([XRAY, "test", *reversed(paths), "--verbose", "--jobs", str(jobs)],
                                   cwd=root, capture_output=True, timeout=15,
                                   env={**os.environ, "NO_COLOR": "1"}) for jobs in (1, 2)]
            self.assertEqual([(run.returncode, run.stdout) for run in runs], [(1, b""), (1, b"")])
            self.assertEqual(runs[0].stderr, runs[1].stderr)
            for text in (b"E0413: null value unwrapped", b"E0445: a qualified message",
                         b"3-source.xr:2:17: error: name is not an initialized value",
                         b"xray test: 0 passed, 3 failed, 0 skipped"):
                self.assertIn(text, runs[1].stderr)
            if os.name == "nt":
                self.assertIn(b"\\x5c1-null chosen.xr:2:41-2:41", runs[1].stderr)

    def test_cancelled_file_jobs_keep_prior_report(self):
        with tempfile.TemporaryDirectory(prefix="xray-test-job-cancel-") as directory:
            root = Path(directory)
            paths = self.job_files(root)
            report = root / "report.json"
            report.write_bytes(b"prior report")
            code, output, errors = self.run_job_files(root, paths, ["--jobs", "2", "--report", report],
                [b"101\n", b"102\n"], cancel=True)
            self.assertNotEqual(code, 0)
            self.assertEqual((output, errors), (b"", b""))
            self.assertEqual(report.read_bytes(), b"prior report")
            self.assertEqual(sorted(root.iterdir()), [*paths, report])

    def test_timer_and_cpu_timeouts_release_the_invocation(self):
        for body in ("while (true) {}", "time.sleep(10000)"):
            with self.subTest(body=body):
                self.invoke({"main_test.xr": f'''
import time
var counter: i64 = 0
@test(timeout: 1)
fn timeout() {{ {body} }}
@after_each
fn after() {{ counter = counter + 1 }}
@test
fn next() {{ assert(counter == 1); time.sleep(1) }}
'''}, 1, "--jobs", "1", contains=("exceeded timeout", "1 passed", "1 failed"))

    def test_original_error_and_teardown_survive_failed_test(self):
        self.invoke({"main_test.xr": '''
enum Failure { Failed { message: string } }
var counter: i64 = 0
@test
fn fails() { throw Failure.Failed { message: "from-test" } }
@after_each
fn cleanup() { counter = counter + 1 }
@test
fn continues() { assert(counter == 1) }
'''}, 1, "--jobs", "1", contains=(
            '[Uncaught Error] Failure.Failed("from-test")', "1 passed", "1 failed"),
            absent=("execution failed (outcome=",))

    def test_scalar_typed_error_and_teardown(self):
        output = self.invoke({"main_test.xr": '''
enum Failure { Denied { code: i64 } }
var counter: i64 = 0
@test
fn fails() throws(Failure) { throw Failure.Denied { code: 17 } }
@after_each
fn cleanup() { counter = counter + 1 }
@test
fn continues() { assert(counter == 1); print(counter) }
'''}, 1, "--jobs", "1", contains=(
            "[Uncaught Error] Failure.Denied(17)", "1 passed", "1 failed"),
            absent=("execution failed (outcome=",))
        self.assertEqual(output, "1\n")

    def test_invalid_declarations_and_compile_errors_fail(self):
        for source in (
            "@test\nfn needsArgument(x: i64) {}\n",
            "@test\nfn generic<T>() {}\n",
            "@test\nfn returnsValue() -> i64 { return 1 }\n",
            "@test\nfn invalid() { missing() }\n",
            "@test(skip)\nfn invalidSkipped() { missing() }\n",
        ):
            with self.subTest(source=source):
                self.invoke({"main_test.xr": source}, 1, absent=("All tests passed",))

    def test_selection_precedes_backend_admission_but_checks_every_declaration(self):
        source = '''
print("initializer ran")
@test
fn kept() { print("kept ran") }
@test(timeout: 17)
fn pending() { print("unexpected deadline body") }
'''
        self.assertEqual(self.invoke({"main_test.xr": source}, 0, "--filter", "kept",
                                     contains=("1 passed, 0 failed, 1 skipped",)),
                         "initializer ran\nkept ran\n")
        self.assertEqual(self.invoke({"main_test.xr": source}, 1, "--filter", "pending",
                                     contains=("test-role startup with a deadline is not supported",)),
                         "")
        self.assertEqual(self.invoke({"main_test.xr": source + '''
@test
fn invalid() { missing() }
'''}, 1, "--filter", "kept", contains=("name is not an initialized value",)), "")

    def test_serial_files_own_state_and_exclude_imported_roles(self):
        source = '''
import "./helper" as helper
var counter: i64 = helper.initial()
@test
fn isolated() { counter = counter + 1; assert(counter == 41); print(counter) }
'''
        self.assertEqual(self.invoke({
            "helper.xr": '''
export fn initial() -> i64 { return 40 }
@test
fn imported() { print("unexpected imported test"); assert(false) }
''',
            "first_test.xr": source,
            "second_test.xr": source,
        }, 0, "--jobs", "1", contains=("2 passed, 0 failed, 0 skipped",),
            absent=("unexpected imported test",)), "41\n41\n")

    def test_failed_initializer_never_retries_or_runs_hooks(self):
        self.assertEqual(self.invoke({"main_test.xr": '''
print("initialization attempt")
assert(false, "initialization failed first")
@before_all
fn setup() { print("unexpected suite setup") }
@test
fn first() { print("unexpected first body") }
@test
fn later() { print("unexpected later body") }
@after_each
fn clean_case() { print("unexpected case teardown") }
@after_all
fn clean_suite() { print("unexpected suite teardown") }
'''}, 1, contains=("module initialization failed", "initialization failed first", "1 failed"),
            absent=("unexpected",)), "initialization attempt\n")

    def test_unsupported_execution_options_are_immediate(self):
        files = {"main_test.xr": '''
print("initializer ran")
@test
fn selected() { print("test ran") }
'''}
        with tempfile.TemporaryDirectory(prefix="xray-test-report-") as directory:
            report = Path(directory) / "test-results.json"
            report.write_bytes(b"prior report")
            for expected, options, diagnosis in (
                (2, ("--jobs", "0"), "parallel jobs must be at least one"),
                (1, ("--sandbox",), "sandbox execution is temporarily unavailable"),
            ):
                with self.subTest(options=options):
                    self.assertEqual(self.invoke(files, expected, *options, "--report", str(report),
                                                 contains=(diagnosis,)), "")
                    self.assertEqual(report.read_bytes(), b"prior report")

    def test_quiet_preserves_guest_output(self):
        self.assertEqual(self.invoke({"main_test.xr": '''
@test
fn selected() { print("guest output"); assert(false, "hidden runner diagnostic") }
@after_all
fn teardown() { print("guest teardown") }
'''}, 1, "--quiet", "--verbose",
            absent=("hidden runner diagnostic", "xray test:")), "guest output\nguest teardown\n")

    def test_report_preserves_owned_runtime_and_source_causes(self):
        with tempfile.TemporaryDirectory(prefix="xray-report-owned-") as directory:
            root = Path(directory)
            report = root / "report.json"
            report.write_bytes(b"prior report")
            actions = root / "a-actions.xr"
            actions.write_text(r'''
enum Failure { Detail { code: i64 } }
fn rejected() throws(Failure) { throw Failure.Detail { code: 42 } }
@test
fn succeeds() { print(17) }
@test(skip)
fn skipped() { assert(false) }
@test
fn panics() { assert(false, "first\n\"例\"\\") }
@test
fn asserts_error() { try! rejected() }
@after_all
fn teardown() { print(23); assert(false, "teardown cause") }
''', encoding="utf-8")
            invalid = root / "b-诊断.xr"
            invalid.write_text('fn duplicate(a: i64, a: i64) {}\n', encoding="utf-8")
            result = subprocess.run([XRAY, "test", invalid, actions, "--quiet", "--report", report],
                                    cwd=ROOT, capture_output=True, timeout=15)
            self.assertEqual((result.returncode, result.stdout, result.stderr), (1, b"17\n23\n", b""))
            data = json.loads(report.read_bytes())
            self.assertEqual(set(data), {"schema", "files", "duration_ms"})
            self.assertEqual(data["schema"], 1)
            self.assertGreaterEqual(data["duration_ms"], 0)
            outcome, diagnostic = data["files"]
            self.assertEqual(Path(outcome["path"]), actions)
            self.assertEqual(tuple(outcome[key] for key in
                ("tests", "passed", "failed", "errors", "timeouts", "skipped")), (4, 1, 2, 1, 0, 1))
            self.assertEqual(outcome["cases"], [
                {"name": "succeeds", "status": "passed"},
                {"name": "skipped", "status": "skipped"},
                {"name": "panics", "status": "failed", "message": '[Uncaught Panic] E0445: first\n"例"\\'},
                {"name": "asserts_error", "status": "failed",
                 "message": "[Uncaught Panic] E0446: try! failed: Failure.Detail(42)"},
            ])
            self.assertIn("teardown cause", outcome["error"])
            self.assertEqual(Path(diagnostic["path"]), invalid)
            self.assertEqual((diagnostic["sourceStatus"], diagnostic["sourceStage"], diagnostic["errors"],
                              diagnostic["cases"]), ("source-rejected", 4, 1, []))
            invalid.unlink()
            self.assertIn("duplicate parameter name", diagnostic["error"])
            self.assertIn("first declaration is here", diagnostic["error"])
            self.assertIn("b-诊断.xr", diagnostic["error"])

    def test_report_keeps_unstarted_cases_and_early_errors_distinct(self):
        with tempfile.TemporaryDirectory(prefix="xray-report-stages-") as directory:
            root = Path(directory)
            report = root / "report.json"
            fixture = ROOT / "tests/lir/programs/test-role-session-selected.xr"
            result = subprocess.run([XRAY, "test", fixture, "--filter", "excluded_deadline", "--report", report],
                                    cwd=ROOT, capture_output=True, timeout=15)
            self.assertEqual((result.returncode, result.stdout, result.stderr), (1, b"", b""))
            row = json.loads(report.read_bytes())["files"][0]
            self.assertEqual((row["passed"], row["failed"], row["errors"], row["timeouts"], row["cases"]),
                             (0, 0, 1, 0, []))
            self.assertIn("test-role startup with a deadline is not supported", row["error"])
            self.assertNotIn("sourceStatus", row)

            source = root / "stopped.xr"
            source.write_text('''
@test
fn first() { assert(false, "stopped") }
@test
fn later() { print(99) }
@after_all
fn cleanup() { print(17) }
''', encoding="utf-8")
            result = subprocess.run([XRAY, "test", source, "--fail-fast", "--report", report],
                                    cwd=ROOT, capture_output=True, timeout=15)
            self.assertEqual((result.returncode, result.stdout, result.stderr), (1, b"17\n", b""))
            row = json.loads(report.read_bytes())["files"][0]
            self.assertEqual((row["tests"], row["failed"], row["skipped"]), (2, 1, 0))
            self.assertEqual(row["cases"],
                             [{"name": "first", "status": "failed", "message": "[Uncaught Panic] E0445: stopped"}])

            source.write_text('''
print("initialization")
assert(false, "sticky initializer cause")
@test
fn blocked() { print(99) }
@after_all
fn cleanup() { print(17) }
''', encoding="utf-8")
            result = subprocess.run([XRAY, "test", source, "--report", report],
                                    cwd=ROOT, capture_output=True, timeout=15)
            self.assertEqual((result.returncode, result.stdout, result.stderr), (1, b"initialization\n", b""))
            row = json.loads(report.read_bytes())["files"][0]
            self.assertEqual((row["tests"], row["failed"], row["errors"], row["cases"]), (1, 0, 1, []))
            self.assertIn("module initialization failed", row["error"])
            self.assertIn("sticky initializer cause", row["error"])

    def test_report_publication_failure_is_explicit(self):
        with tempfile.TemporaryDirectory(prefix="xray-report-write-") as directory:
            root = Path(directory)
            source = root / "case.xr"
            source.write_text('@test\nfn selected() { print(17) }\n', encoding="utf-8")
            target = root / "missing/report.json"
            result = subprocess.run([XRAY, "test", source, "--report", target],
                                    cwd=ROOT, capture_output=True, timeout=15)
            self.assertEqual((result.returncode, result.stdout), (1, b"17\n"))
            self.assertIn(b"output publication failed", result.stderr)
            self.assertEqual(sorted(root.iterdir()), [source])

    def test_report_distinguishes_invalid_source_from_semantic_rejection(self):
        with tempfile.TemporaryDirectory(prefix="xray-report-source-status-") as directory:
            root = Path(directory)
            report = root / "report.json"
            for label, text, status, diagnostic in (
                ("syntax", b"fn broken() return\n", "invalid", "function body must use braces { }"),
                ("input", b"fn ordinary() {}\n\x00", "invalid", "source text contains an embedded NUL"),
                ("semantic", b"fn duplicate(a: i64, a: i64) {}\n", "source-rejected", "duplicate parameter name"),
            ):
                with self.subTest(label=label):
                    source = root / f"{label}.xr"
                    source.write_bytes(text)
                    plain = subprocess.run([XRAY, "test", source], cwd=ROOT, capture_output=True, timeout=15)
                    result = subprocess.run([XRAY, "test", source, "--report", report],
                                            cwd=ROOT, capture_output=True, timeout=15)
                    self.assertEqual((result.returncode, result.stdout, result.stderr), (1, b"", b""))
                    row = json.loads(report.read_bytes())["files"][0]
                    self.assertEqual((row["sourceStatus"], row["sourceStage"], row["errors"], row["cases"]),
                                     (status, 4, 1, []))
                    self.assertIn(diagnostic, row["error"])
                    self.assertEqual(plain.stderr, row["error"].encode("utf-8") +
                                     b"xray test: 0 passed, 1 failed, 0 skipped\n")

    def test_interrupted_report_keeps_prior_bytes(self):
        fixture = ROOT / "tests/lir/programs/test-role-report-interrupt.xr"
        with tempfile.TemporaryDirectory(prefix="xray-report-interrupt-") as directory:
            root = Path(directory)
            report = root / "report.json"
            report.write_bytes(b"prior report")
            completed = root / "a-completed.xr"
            completed.write_text('@test\nfn ran() { print("first file completed") }\n', encoding="utf-8")
            held = root / "b-held.xr"
            held.write_bytes(fixture.read_bytes())
            with subprocess.Popen([XRAY, "test", held, completed, "--report", report], cwd=ROOT,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE) as child:
                with ThreadPoolExecutor(max_workers=1) as executor:
                    try:
                        for expected in (b"first file completed\n", b"report guest started\n"):
                            marker = executor.submit(child.stdout.readline)
                            self.assertEqual(marker.result(timeout=15), expected)
                        self.assertIsNone(child.poll())
                    finally:
                        if child.poll() is None:
                            child.kill()
                remaining, errors = child.communicate(timeout=5)
            self.assertEqual((remaining, errors), (b"", b""))
            self.assertEqual(report.read_bytes(), b"prior report")
            self.assertEqual(sorted(root.iterdir()), [completed, held, report])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--xray", type=Path, required=True)
    arguments, remaining = parser.parse_known_args()
    XRAY = arguments.xray.resolve()
    ROOT = Path(__file__).resolve().parents[2]
    unittest.main(argv=[__file__, *remaining])
