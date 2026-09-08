import hashlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from _support import load_module

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))
gate = load_module("allocation_native_gate", ROOT / "scripts/check_xr_program_allocation_native.py")


class AllocationNativeGateTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="xr-allocation-gate-test-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)

    def fixture(self, source=None, directory=None):
        directory = directory or self.directory
        if source is None:
            source = ("struct XrAotType16 { int f0; };\n"
                      "struct XrAotType17 { int f0; };\n"
                      "struct XrAotType18 { int f0; };\n"
                      "XrAotOutcome xr_aot_fn_0(void *context) { return 0; }\n" +
                      '_Static_assert(1, "allocation payload alignment is unsupported");\n' * 4)
        data = source.encode("utf-8")
        facts = {**gate.EXPECTED_FACTS, "entry": 0, "plain_type": 16, "inner_type": 17,
                 "capture_type": 18, "sha256": hashlib.sha256(data).hexdigest()}
        (directory / "allocation.generated.c").write_bytes(data)
        (directory / "allocation.facts.json").write_text(json.dumps(facts), encoding="utf-8")
        return facts, data

    def test_valid_facts_bind_generated_bytes_and_header_identities(self):
        facts, source = self.fixture()
        self.assertEqual(gate.load_fixture(self.directory), (facts, source))
        header = gate.facts_header(facts)
        self.assertIn("XR_FIXTURE_ENTRY xr_aot_fn_0", header)
        self.assertIn("XR_FIXTURE_CAPTURE XrAotType18", header)

    def test_missing_empty_and_stale_output_do_not_qualify(self):
        with self.assertRaises(OSError):
            gate.load_fixture(self.directory)
        self.fixture()
        source = self.directory / "allocation.generated.c"
        source.write_bytes(b"")
        with self.assertRaises(gate.QualificationError):
            gate.load_fixture(self.directory)
        self.fixture()
        source.write_bytes(source.read_bytes() + b" ")
        with self.assertRaisesRegex(gate.QualificationError, "digest"):
            gate.load_fixture(self.directory)

    def test_missing_unknown_duplicate_and_wrong_producer_facts_fail(self):
        mutations = [lambda facts: facts.pop("capturing_pack"),
                     lambda facts: facts.update(unknown=1),
                     lambda facts: facts.update(producer="other"),
                     lambda facts: facts.update(schema=True),
                     lambda facts: facts.update(capturing_pack=0),
                     lambda facts: facts.update(callable_copy=2),
                     lambda facts: facts.update(existential_pack=0),
                     lambda facts: facts.update(allocations=0),
                     lambda facts: facts.update(entry=-1),
                     lambda facts: facts.update(entry=4),
                     lambda facts: facts.update(plain_type=True),
                     lambda facts: facts.update(capture_type=16)]
        for mutate in mutations:
            with self.subTest(mutate=mutate):
                facts, _ = self.fixture()
                mutate(facts)
                (self.directory / "allocation.facts.json").write_text(json.dumps(facts), encoding="utf-8")
                with self.assertRaises(gate.QualificationError):
                    gate.load_fixture(self.directory)
        facts, _ = self.fixture()
        duplicate = json.dumps(facts)[:-1] + ', "schema": 1}'
        (self.directory / "allocation.facts.json").write_text(duplicate, encoding="utf-8")
        with self.assertRaisesRegex(gate.QualificationError, "duplicate"):
            gate.load_fixture(self.directory)

    def test_missing_declarations_entry_and_alignment_proofs_fail(self):
        for old, new in (("struct XrAotType18", "struct Other"),
                         ("xr_aot_fn_0", "xr_aot_fn_5"),
                         ("allocation payload alignment is unsupported", "removed proof")):
            _, source = self.fixture()
            self.fixture(source.decode().replace(old, new))
            with self.subTest(old=old), self.assertRaises(gate.QualificationError):
                gate.load_fixture(self.directory)

    def test_gnu_expressions_cannot_hide_behind_whitespace_or_comments(self):
        for residue in ("({ 1; })", "( \n { 1; })", "( /* comment */ { 1; })",
                        "( // comment\n { 1; })", "typeof(int)", "__attribute__((aligned(8)))",
                        "max_align_t value;", "# pragma pack(1)"):
            _, source = self.fixture()
            self.fixture(source.decode() + residue)
            with self.subTest(residue=residue), self.assertRaisesRegex(
                    gate.QualificationError, "nonportable"):
                gate.load_fixture(self.directory)

    def test_unavailable_msvc_is_not_a_successful_skip(self):
        with mock.patch.object(gate, "os", SimpleNamespace(name="nt")), \
             mock.patch.object(gate.sanitizer, "activate_windows_msvc_environment", return_value=True), \
             mock.patch.object(gate.shutil, "which", return_value=None):
            with self.assertRaisesRegex(gate.QualificationError, "NOT_QUALIFIED"):
                gate.providers()

    def test_discovered_clang_cl_is_also_required(self):
        with mock.patch.object(gate, "os", SimpleNamespace(name="nt")), \
             mock.patch.object(gate.sanitizer, "activate_windows_msvc_environment", return_value=True), \
             mock.patch.object(gate.sanitizer, "resolve_compiler_command", return_value="clang-cl"), \
             mock.patch.object(gate.shutil, "which", side_effect=["cl.exe", "clang-cl.exe"]):
            self.assertEqual(gate.providers(), [("msvc", "cl.exe"), ("clang-cl", "clang-cl.exe")])

    def test_each_provider_compiles_strict_c11_runs_and_checks_symbols(self):
        for name in ("msvc", "clang-cl"):
            with self.subTest(name=name), redirect_stdout(io.StringIO()), \
                    mock.patch.object(gate, "run_checked", side_effect=[
                    subprocess.CompletedProcess([], 0, b"", b""),
                    subprocess.CompletedProcess([], 0, gate.EXPECTED_STDOUT, b"")]) as run, \
                    mock.patch.object(gate.native, "load_symbol_inventory", return_value=("main", "")) as symbols:
                gate.qualify_provider(name, "compiler", self.directory)
                command = run.call_args_list[0].args[0]
                for flag in ("/TC", "/std:c11", "/W4", "/WX", "/utf-8"):
                    self.assertIn(flag, command)
                self.assertEqual("/clang:-pedantic-errors" in command, name == "clang-cl")
                self.assertIn(str(gate.HARNESS), command)
                self.assertEqual(len(run.call_args_list), 2)
                symbols.assert_called_once()

    def test_exit_zero_without_complete_native_checks_is_failure(self):
        for stdout, stderr in ((b"", b""), (b"false\n", b""), (gate.EXPECTED_STDOUT, b"error")):
            with self.subTest(stdout=stdout, stderr=stderr), \
                 mock.patch.object(gate, "run_checked", side_effect=[
                     subprocess.CompletedProcess([], 0, b"", b""),
                     subprocess.CompletedProcess([], 0, stdout, stderr)]), \
                 self.assertRaises(gate.QualificationError):
                gate.qualify_provider("msvc", "compiler", self.directory)

    def test_symbol_failure_or_compiler_residue_fails(self):
        for inventory in ((None, "no map"), ("xr_program_validate", "")):
            with self.subTest(inventory=inventory), \
                 mock.patch.object(gate, "run_checked", return_value=subprocess.CompletedProcess(
                     [], 0, gate.EXPECTED_STDOUT, b"")), \
                 mock.patch.object(gate.native, "load_symbol_inventory", return_value=inventory), \
                 self.assertRaises(gate.QualificationError):
                gate.qualify_provider("msvc", "compiler", self.directory)

    def test_qualification_generates_one_complete_fixture_and_runs_all_providers(self):
        writer = self.directory / "writer.exe"
        writer.write_bytes(b"fixture writer stand-in")

        def generate(command, directory):
            self.assertEqual(command[0], str(writer.resolve()))
            self.assertEqual(command[1::2], ["--output", "--facts"])
            self.fixture(directory=directory)

        with redirect_stdout(io.StringIO()), \
             mock.patch.object(gate, "providers", return_value=[("msvc", "cl"), ("clang-cl", "clang")]), \
             mock.patch.object(gate, "run_checked", side_effect=generate) as run, \
             mock.patch.object(gate, "qualify_provider") as qualify:
            gate.qualify(writer)
            run.assert_called_once()
            self.assertEqual([call.args[0] for call in qualify.call_args_list], ["msvc", "clang-cl"])

    def test_writer_failure_prevents_native_qualification(self):
        writer = self.directory / "writer.exe"
        writer.write_bytes(b"fixture writer stand-in")
        with mock.patch.object(gate, "providers", return_value=[("msvc", "cl")]), \
             mock.patch.object(gate, "run_checked", side_effect=gate.QualificationError("writer failed")), \
             mock.patch.object(gate, "qualify_provider") as qualify, \
             self.assertRaisesRegex(gate.QualificationError, "writer failed"):
            gate.qualify(writer)
        qualify.assert_not_called()

    def test_incomplete_providers_or_modified_generated_c_do_not_pass(self):
        writer = self.directory / "writer.exe"
        writer.write_bytes(b"fixture writer stand-in")

        def generate(command, directory):
            self.fixture(directory=directory)

        def reject_second(name, compiler, directory):
            if name == "clang-cl":
                raise gate.QualificationError("clang-cl qualification failed")

        def mutate_source(name, compiler, directory):
            source = directory / "allocation.generated.c"
            source.write_bytes(source.read_bytes() + b"\n")

        for effect, error in ((reject_second, "clang-cl qualification failed"),
                              (mutate_source, "changed the generated C")):
            output = io.StringIO()
            with self.subTest(effect=effect), redirect_stdout(output), \
                 mock.patch.object(gate, "providers", return_value=[("msvc", "cl"), ("clang-cl", "clang")]), \
                 mock.patch.object(gate, "run_checked", side_effect=generate), \
                 mock.patch.object(gate, "qualify_provider", side_effect=effect), \
                 self.assertRaisesRegex(gate.QualificationError, error):
                gate.qualify(writer)
            self.assertNotIn("allocation native qualification: PASS", output.getvalue())

    def test_empty_duplicate_unknown_and_partial_provider_selection_fails(self):
        writer = self.directory / "writer.exe"
        writer.write_bytes(b"fixture writer stand-in")
        for selected in ([], [("clang-cl", "clang")], [("msvc", "cl"), ("msvc", "cl")],
                         [("msvc", "cl"), ("unknown", "cc")]):
            with self.subTest(selected=selected), mock.patch.object(gate, "providers", return_value=selected), \
                 mock.patch.object(gate, "run_checked") as run, self.assertRaises(gate.QualificationError):
                gate.qualify(writer)
            run.assert_not_called()

    def test_failed_command_keeps_error_code_and_diagnostic(self):
        with mock.patch.object(gate.subprocess, "run", return_value=subprocess.CompletedProcess(
                [], 7, b"output", b"bad \xff")), self.assertRaisesRegex(
                    gate.QualificationError, r"command failed \(7\)"):
            gate.run_checked(["compiler"], self.directory)

    def test_cli_missing_writer_or_required_qualification_is_nonzero(self):
        output = io.StringIO()
        with redirect_stdout(output), redirect_stderr(output):
            code = gate.main(["--writer", str(self.directory / "missing.exe")])
        self.assertEqual(code, 1)
        self.assertNotIn("PASS", output.getvalue())
        for arguments in ([], ["old-positional.exe"], ["--unknown", "writer.exe"],
                          ["--writer", "one.exe", "--writer", "two.exe"]):
            with self.subTest(arguments=arguments), redirect_stderr(io.StringIO()), \
                 self.assertRaises(SystemExit) as caught:
                gate.main(arguments)
            self.assertEqual(caught.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
