"""Exercise the actual test command and its shared Program execution owner."""

import argparse
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
            for expected, options, diagnosis in (
                (2, ("--jobs", "0"), "parallel jobs must be at least one"),
                (2, ("--jobs", "2"), "parallel test execution is temporarily unavailable"),
                (1, ("--sandbox",), "sandbox execution is temporarily unavailable"),
                (2, ("--report", str(report)), "test outcome reports are temporarily unavailable"),
            ):
                with self.subTest(options=options):
                    self.assertEqual(self.invoke(files, expected, *options, contains=(diagnosis,)), "")
            self.assertFalse(report.exists())

    def test_quiet_preserves_guest_output(self):
        self.assertEqual(self.invoke({"main_test.xr": '''
@test
fn selected() { print("guest output"); assert(false, "hidden runner diagnostic") }
@after_all
fn teardown() { print("guest teardown") }
'''}, 1, "--quiet", "--verbose",
            absent=("hidden runner diagnostic", "xray test:")), "guest output\nguest teardown\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--xray", type=Path, required=True)
    arguments, remaining = parser.parse_known_args()
    XRAY = arguments.xray.resolve()
    ROOT = Path(__file__).resolve().parents[2]
    unittest.main(argv=[__file__, *remaining])
