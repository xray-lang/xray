import hashlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

from _support import load_module

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))
gate = load_module("coroutine_graph_native_gate",
                   ROOT / "scripts/check_xr_program_coroutine_graph_native.py")


class CoroutineGraphNativeTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="xr-graph-gate-test-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)

    def fixture(self, scenario="cleanup", directory=None, extra=""):
        directory = directory or self.directory
        source = ("typedef struct XrAotEntryCoroutineFrame { int state; } XrAotEntryCoroutineFrame;\n"
                  "XrAotOutcome xr_aot_fn_0_step(void *frame) { return 0; }\n"
                  "const XrBackendNativeDescriptor xr_aot_entry_coroutine_descriptor = {0};\n" + extra).encode()
        facts = {**gate.COMMON, **gate.EXPECTED[scenario], "scenario": scenario,
                 "execution_id": ("01" if scenario == "cleanup" else "02") * 32,
                 "sha256": hashlib.sha256(source).hexdigest()}
        (directory / "graph.generated.c").write_bytes(source)
        (directory / "graph.facts.json").write_text(json.dumps(facts), encoding="utf-8")
        return facts, source

    def test_both_graphs_bind_counts_identity_source_and_harness(self):
        for scenario in gate.EXPECTED:
            with self.subTest(scenario=scenario):
                facts, source = self.fixture(scenario)
                self.assertEqual(gate.load_fixture(self.directory, scenario), (facts, source))
                header = gate.facts_header(facts)
                self.assertIn(f"XR_GRAPH_CASES {gate.EXPECTED[scenario]['cases']}", header)
                self.assertEqual(header.count("UINT8_C("), 32)
                self.assertIn(f"XR_GRAPH_CLEANUP {int(scenario == 'cleanup')}", header)

    def test_missing_unknown_duplicate_wrong_counts_and_boolean_numbers_fail(self):
        mutations = [lambda facts: facts.pop("yields"), lambda facts: facts.update(unknown=0),
                     lambda facts: facts.update(scenario="branch"),
                     lambda facts: facts.update(producer="old"), lambda facts: facts.update(cases=0),
                     lambda facts: facts.update(functions=True), lambda facts: facts.update(entry=1),
                     lambda facts: facts.update(self_edges=0), lambda facts: facts.update(owner_drops=3),
                     lambda facts: facts.update(execution_id="0" * 64),
                     lambda facts: facts.update(execution_id=2)]
        for mutation in mutations:
            facts, _ = self.fixture()
            mutation(facts)
            (self.directory / "graph.facts.json").write_text(json.dumps(facts), encoding="utf-8")
            with self.subTest(mutation=mutation), self.assertRaises(gate.QualificationError):
                gate.load_fixture(self.directory, "cleanup")
        facts, _ = self.fixture()
        (self.directory / "graph.facts.json").write_text(
            json.dumps(facts)[:-1] + ', "entry": 0}', encoding="utf-8")
        with self.assertRaisesRegex(gate.QualificationError, "duplicate"):
            gate.load_fixture(self.directory, "cleanup")

    def test_missing_empty_stale_and_unknown_scenario_fail(self):
        with self.assertRaises(OSError):
            gate.load_fixture(self.directory, "cleanup")
        with self.assertRaisesRegex(gate.QualificationError, "unknown"):
            gate.load_fixture(self.directory, "unregistered")
        self.fixture()
        (self.directory / "graph.generated.c").write_bytes(b"")
        with self.assertRaises(gate.QualificationError):
            gate.load_fixture(self.directory, "cleanup")
        _, source = self.fixture()
        (self.directory / "graph.generated.c").write_bytes(source + b" ")
        with self.assertRaisesRegex(gate.QualificationError, "digest"):
            gate.load_fixture(self.directory, "cleanup")

    def test_missing_entry_and_descriptor_fail(self):
        for missing in ("xr_aot_fn_0_step", "xr_aot_entry_coroutine_descriptor", "XrAotEntryCoroutineFrame"):
            facts, source = self.fixture()
            source = source.replace(missing.encode(), b"removed")
            facts["sha256"] = hashlib.sha256(source).hexdigest()
            (self.directory / "graph.generated.c").write_bytes(source)
            (self.directory / "graph.facts.json").write_text(json.dumps(facts), encoding="utf-8")
            with self.subTest(missing=missing), self.assertRaisesRegex(gate.QualificationError, "missing"):
                gate.load_fixture(self.directory, "cleanup")

    def test_nonportable_executor_and_unobserved_heap_dependencies_fail(self):
        for extra in ("( /* hidden */ { 1; })", "( // comment\n { 1; })", "typeof(int)",
                      "__attribute__((aligned(8)))", "# pragma pack(1)", "xr_vm_execute();",
                      "xr_program_validate();", "xr_backend_ir_build();", "malloc(8);", "free(p);"):
            self.fixture(extra=extra)
            with self.subTest(extra=extra), self.assertRaises(gate.QualificationError):
                gate.load_fixture(self.directory, "cleanup")

    def test_repeated_writer_keeps_exact_bytes_mtime_and_identity(self):
        calls = []

        def generate(command, directory):
            calls.append(command)
            if len(calls) == 1:
                self.fixture(directory=directory)
            return subprocess.CompletedProcess(command, 0, b"", b"")

        with mock.patch.object(gate, "run_checked", side_effect=generate):
            facts, _ = gate.generate_fixture(self.directory / "writer.exe", "cleanup", self.directory)
        self.assertEqual(len(calls), 2)
        self.assertEqual(calls[0], calls[1])
        self.assertEqual(calls[0][1::2], ["--scenario", "--output", "--facts"])
        self.assertEqual((self.directory / "graph.facts.h").read_text(), gate.facts_header(facts))

    def test_same_bytes_rewrite_and_nondeterministic_writer_fail(self):
        for rewrite_only in (True, False):
            calls = 0

            def generate(command, directory):
                nonlocal calls
                calls += 1
                if calls == 1:
                    self.fixture(directory=directory)
                elif not rewrite_only:
                    self.fixture(directory=directory, extra="/* changed */")
                return subprocess.CompletedProcess(command, 0, b"", b"")

            identities = [(0, 0, 1), (0, 0, 2), (1, 0, 1), (0, 0, 2)]
            with self.subTest(rewrite_only=rewrite_only), \
                 mock.patch.object(gate, "run_checked", side_effect=generate), \
                 mock.patch.object(gate, "stable_file_identity", side_effect=identities), \
                 self.assertRaises(gate.QualificationError):
                gate.generate_fixture(self.directory / "writer.exe", "cleanup", self.directory)

    def test_each_provider_uses_strict_c11_execution_and_symbol_checks(self):
        for name in ("msvc", "clang-cl"):
            with self.subTest(name=name), redirect_stdout(io.StringIO()), \
                 mock.patch.object(gate, "run_checked", side_effect=[
                     subprocess.CompletedProcess([], 0, b"", b""),
                     subprocess.CompletedProcess([], 0, gate.EXPECTED_STDOUT["branch"], b"")]) as run, \
                 mock.patch.object(gate.native, "load_symbol_inventory", return_value=("main", "")) as symbols:
                gate.qualify_provider(name, "compiler", "branch", self.directory)
                command = run.call_args_list[0].args[0]
                for flag in ("/TC", "/std:c11", "/W4", "/WX", "/utf-8"):
                    self.assertIn(flag, command)
                self.assertEqual("/clang:-pedantic-errors" in command, name == "clang-cl")
                self.assertEqual(run.call_count, 2)
                symbols.assert_called_once()

    def test_exit_zero_without_exact_trace_or_with_stderr_is_failure(self):
        expected = gate.EXPECTED_STDOUT["cleanup"]
        for output, error in ((b"", b""), (b"false\n", b""), (expected.replace(b"71", b"72"), b""),
                              (expected, b"warning"), (gate.EXPECTED_STDOUT["branch"], b"")):
            with self.subTest(output=output, error=error), \
                 mock.patch.object(gate, "run_checked", side_effect=[
                     subprocess.CompletedProcess([], 0, b"", b""),
                     subprocess.CompletedProcess([], 0, output, error)]), \
                 self.assertRaises(gate.QualificationError):
                gate.qualify_provider("msvc", "compiler", "cleanup", self.directory)

    def test_symbol_failure_or_old_executor_residue_fails(self):
        for symbols in ((None, "no inventory"), ("xr_vm_execute", "")):
            with self.subTest(symbols=symbols), \
                 mock.patch.object(gate, "run_checked", return_value=subprocess.CompletedProcess(
                     [], 0, gate.EXPECTED_STDOUT["branch"], b"")), \
                 mock.patch.object(gate.native, "load_symbol_inventory", return_value=symbols), \
                 self.assertRaises(gate.QualificationError):
                gate.qualify_provider("msvc", "compiler", "branch", self.directory)

    def test_both_graphs_and_all_discovered_providers_are_required(self):
        writer = self.directory / "writer.exe"
        writer.write_bytes(b"stand-in")

        def generate(writer, scenario, directory):
            return self.fixture(scenario, directory)

        with redirect_stdout(io.StringIO()), \
             mock.patch.object(gate, "providers", return_value=[("msvc", "cl"), ("clang-cl", "clang")]), \
             mock.patch.object(gate, "generate_fixture", side_effect=generate), \
             mock.patch.object(gate, "qualify_provider") as qualify:
            gate.qualify(writer)
        self.assertEqual([(call.args[2], call.args[0]) for call in qualify.call_args_list],
                         [("cleanup", "msvc"), ("cleanup", "clang-cl"),
                          ("branch", "msvc"), ("branch", "clang-cl")])

    def test_incomplete_or_duplicate_providers_and_writer_failures_do_not_pass(self):
        writer = self.directory / "writer.exe"
        writer.write_bytes(b"stand-in")
        for selected in ([], [("clang-cl", "clang")], [("msvc", "cl"), ("msvc", "cl")]):
            with self.subTest(selected=selected), mock.patch.object(gate, "providers", return_value=selected), \
                 mock.patch.object(gate, "generate_fixture") as generate, \
                 self.assertRaises(gate.QualificationError):
                gate.qualify(writer)
            generate.assert_not_called()
        with mock.patch.object(gate, "providers", return_value=[("msvc", "cl")]), \
             mock.patch.object(gate, "generate_fixture", side_effect=gate.QualificationError("writer failed")), \
             mock.patch.object(gate, "qualify_provider") as qualify, self.assertRaises(gate.QualificationError):
            gate.qualify(writer)
        qualify.assert_not_called()

    def test_second_provider_failure_prevents_complete_pass(self):
        writer = self.directory / "writer.exe"
        writer.write_bytes(b"stand-in")

        def qualify(name, compiler, scenario, directory):
            if name == "clang-cl":
                raise gate.QualificationError("clang-cl failed")

        output = io.StringIO()
        with redirect_stdout(output), \
             mock.patch.object(gate, "providers", return_value=[("msvc", "cl"), ("clang-cl", "clang")]), \
             mock.patch.object(gate, "generate_fixture", side_effect=lambda writer, scenario, directory:
                               self.fixture(scenario, directory)), \
             mock.patch.object(gate, "qualify_provider", side_effect=qualify), \
             self.assertRaises(gate.QualificationError):
            gate.qualify(writer)
        self.assertNotIn("coroutine graph native qualification: PASS", output.getvalue())

    def test_cli_requires_one_writer_and_does_not_offer_partial_scenarios(self):
        output = io.StringIO()
        with redirect_stdout(output), redirect_stderr(output):
            self.assertEqual(gate.main(["--writer", str(self.directory / "missing.exe")]), 1)
        self.assertNotIn("PASS", output.getvalue())
        for args in ([], ["old.exe"], ["--writer", "a", "--writer", "b"],
                     ["--writer", "a", "--scenario", "branch"]):
            with self.subTest(args=args), redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as caught:
                gate.main(args)
            self.assertEqual(caught.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
