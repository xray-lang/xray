"""Test G13 mode oracles, then optionally execute a real VM/native/error sample."""
import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("spec_snippet_gate", ROOT / "scripts/check_spec_xray_snippets.py")
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)
REAL_BINARY = None


def block(markers="@check", code="print(1)", output=None):
    text = f"```xray {markers}\n{code}\n```\n"
    if output is not None:
        text += "\n```output\n" + output + "```\n"
    parsed, errors = gate.parse_document(text, "spec.md")
    assert not errors and len(parsed) == 1
    return parsed[0]


class FakeCompiler:
    def __init__(self, vm=(0, "1\n", ""), native=(0, "1\n", ""), build_code=0, produce=True, error=None):
        self.vm, self.native, self.build_code, self.produce, self.error = vm, native, build_code, produce, error
        self.commands = []

    def __call__(self, command, cwd):
        self.commands.append(command)
        if len(command) == 1:
            code, stdout, stderr = self.native
        elif command[1] == "build":
            code, stdout, stderr = self.build_code, "", "" if not self.build_code else "native compiler unavailable"
            if code == 0 and self.produce:
                Path(command[-1]).write_bytes(b"owned native fixture")
        elif command[1] == "check":
            code, stdout, stderr = (0, "", "") if self.error is None else self.error(command[-1])
        else:
            code, stdout, stderr = self.vm
        return dict(command=command, cwd=str(cwd), exit=code, stdout=stdout, stderr=stderr,
                    utf8_valid=True, binary_sha256_before="same", binary_sha256_after="same")


class ModeTests(unittest.TestCase):
    def evaluate(self, item, compiler=None):
        with tempfile.TemporaryDirectory() as temporary:
            return gate.evaluate_block(item, Path("fake-xray"), Path(temporary), ROOT, compiler or FakeCompiler())

    def test_extractor_preserves_api_and_output_boundary(self):
        text = "```xray @id=one @run\nprint(1)\n```\n```output\n1\n```\n```xray @check\nvar x=1\n```\n"
        self.assertEqual(gate.extract_blocks(text), [(2, "print(1)", ["@id=one", "@run"]), (8, "var x=1", ["@check"])])

    def test_explicit_modes_only(self):
        for markers in ("", "@id=sample", "@run @check", "@fragment @check", "@check @check", "@error=BAD", "@check @mystery"):
            compiler = FakeCompiler()
            self.assertEqual(self.evaluate(block(markers, "fn main() {}\nmain()"), compiler)["status"], "METADATA_FAIL")
            self.assertEqual(compiler.commands, [])

    def test_fragment_only_parses_and_check_does_not_run(self):
        compiler = FakeCompiler()
        self.assertEqual(self.evaluate(block("@fragment"), compiler)["status"], "PARSE_PASS")
        self.assertIn("-S", compiler.commands[0])
        compiler = FakeCompiler()
        self.assertEqual(self.evaluate(block(), compiler)["status"], "CHECK_PASS")
        self.assertEqual([command[1] for command in compiler.commands], ["check"])

    def test_error_code_line_primary_and_no_crash(self):
        item = block("@error=E0326", 'var x = [1, "x"] // error')
        for code, line, extra, exit_code, expected in [("E0326", 1, "", 1, "ERROR_PASS"),
                ("E0328", 1, "", 1, "ERROR_FAIL"), ("E0326", 2, "", 1, "ERROR_FAIL"),
                ("E0326", 1, "second", 1, "ERROR_FAIL"), ("E0326", 1, "", -1, "ERROR_FAIL"),
                ("E0326", 1, "", 0xC0000005, "ERROR_FAIL"),
                ("E0326", 1, "", 0, "ERROR_FAIL")]:
            def error(path, code=code, line=line, extra=extra, exit_code=exit_code):
                stderr = f"{path}:{line}:7: error: {code}: failure\n{path}:1:1: note: explanation\n"
                if extra:
                    stderr += f"{path}:1:3: error: E0326: second\n"
                return exit_code, "", stderr
            self.assertEqual(self.evaluate(item, FakeCompiler(error=error))["status"], expected)

    def test_error_annotation_is_a_real_single_comment(self):
        for code in ('print("// error")', 'var x=1 // error\nvar y=2 // error', 'var x=1 /* // error */'):
            self.assertEqual(self.evaluate(block("@error=E0326", code))["status"], "METADATA_FAIL")

    def test_run_requires_exact_dual_output(self):
        compiler = FakeCompiler()
        row = self.evaluate(block("@run", output="1\n"), compiler)
        self.assertEqual(row["status"], "RUN_PASS")
        self.assertEqual([command[1] if len(command)>1 else "native" for command in compiler.commands], ["run", "build", "native"])
        for compiler in (FakeCompiler(vm=(1, "", "bad")), FakeCompiler(vm=(0, "1", "")),
                         FakeCompiler(native=(0, "1 \n", "")), FakeCompiler(native=(0, "1\n", "warning")),
                         FakeCompiler(native=(1, "1\n", "")), FakeCompiler(build_code=1), FakeCompiler(produce=False)):
            self.assertEqual(self.evaluate(block("@run", output="1\n"), compiler)["status"], "RUN_FAIL")
        self.assertEqual(self.evaluate(block("@run"))["status"], "METADATA_FAIL")
        self.assertEqual(self.evaluate(block("@run", output=""), FakeCompiler(vm=(0,"",""), native=(0,"","")))["status"], "RUN_PASS")

    def test_failed_build_does_not_claim_native_execution(self):
        compiler = FakeCompiler(build_code=1)
        row = self.evaluate(block("@run", output="1\n"), compiler)
        self.assertEqual(row["native_execution"], "NOT_RUN")
        self.assertEqual(len(compiler.commands), 2)

    def test_malformed_fences_are_defects(self):
        self.assertEqual(gate.parse_document("```xray @check\nprint(1)")[1][0]["status"], "METADATA_FAIL")
        item = gate.parse_document("```xray @run\nprint(1)\n```\n```output\n1")[0][0]
        self.assertEqual(self.evaluate(item)["status"], "METADATA_FAIL")

    def test_parity_preserves_literals_and_token_boundaries(self):
        left = block("@id=x @check", 'var url="http://x" // 中文\n/* a /* nested */ b */ print(url)')
        right = block("@id=x @check", 'var url = "http://x" // English\nprint(url)')
        self.assertEqual(gate.parity([left], [right])[0]["status"], "PARITY_PASS")
        right["id"]="x-en"
        self.assertEqual(gate.parity([left], [right])[0]["status"], "PARITY_PASS")
        for code in ('var url="http://y"\nprint(url)', 'varurl="http://x"\nprint(url)'):
            self.assertEqual(gate.parity([left], [block("@id=x @check", code)])[0]["status"], "PARITY_FAIL")
        for code in ('var x = r"// raw"', 'var x="${f(\"// nested\")}"', 'var x="""\n// block contents\n"""'):
            self.assertEqual(gate.lexical_facts(code)[1], [])
        self.assertNotEqual(gate.lexical_facts("a += b")[0], gate.lexical_facts("a + = b")[0])
        self.assertEqual(gate.parity([block("@run", output="1\n")], [block("@run", output="1 \n")])[0]["status"], "PARITY_FAIL")

    def test_timeout_invalid_utf8_and_binary_change_are_not_pass(self):
        for result in (dict(exit=None, timeout_seconds=30), dict(exit=0, utf8_valid=False),
                       dict(exit=0, binary_sha256_before="a", binary_sha256_after="b")):
            self.assertFalse(gate.command_ok(result))
        with mock.patch.object(gate.subprocess, "run", return_value=subprocess.CompletedProcess([],0,b"1\r\n",b"")):
            self.assertEqual(gate.command_result(["absent-compiler"], ROOT)["stdout"], "1\n")
        with mock.patch.object(gate.subprocess, "run", return_value=subprocess.CompletedProcess([],0,b"\xff",b"")):
            self.assertFalse(gate.command_result(["absent-compiler"], ROOT)["utf8_valid"])
        with mock.patch.object(gate.subprocess, "run", side_effect=OSError("absent")):
            self.assertFalse(gate.command_ok(gate.command_result(["absent-compiler"], ROOT)))

    def test_scope_is_only_selection_and_baseline_stays_frozen(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary); spec = folder/"spec.md"; baseline = folder/"baseline.txt"; report = folder/"report.json"
            spec.write_text('<!-- xr-spec:cn -->\n```xray @id=a @check\nvar x=1\n```\n```xray @id=b @run\nprint(1)\n```\n```output\n1\n```\n<!-- /xr-spec:cn -->\n<!-- xr-spec:en -->\n```xray @id=a @check\nvar x=1\n```\n```xray @id=b @run\nprint(1)\n```\n```output\n1\n```\n<!-- /xr-spec:en -->\n',encoding="utf-8")
            frozen = "# no accepted deviations\n";baseline.write_text(frozen,encoding="utf-8")
            selected=[]
            def evaluate(item,*args):
                selected.append(item["id"])
                return dict(status="CHECK_PASS", key=gate.snippet_key(item["line"],item["code"]), source=item["source"], line=item["line"])
            args=["--xray","absent","--spec",str(spec),"--baseline",str(baseline),"--ids","a","--report",str(report)]
            with mock.patch.object(gate,"evaluate_block",evaluate), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(gate.main(args),0)
                self.assertEqual(selected,["a"])
                self.assertEqual(gate.main([*args[:-2],"--ids","missing"]),1)
            self.assertEqual(baseline.read_text(encoding="utf-8"),frozen)
            self.assertTrue(json.loads(report.read_text(encoding="utf-8"))["conforms"])
            spec.write_text(spec.read_text(encoding="utf-8").replace("@id=a ", ""),encoding="utf-8")
            anonymous_args=args.copy();anonymous_args[anonymous_args.index("--ids")+1]="spec.md:3"
            selected.clear()
            with mock.patch.object(gate,"evaluate_block",evaluate), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(gate.main(anonymous_args),0)
            self.assertEqual(selected,[None])
            empty=folder/"empty.md";empty.write_text("no fences",encoding="utf-8")
            with mock.patch.object(gate,"evaluate_block",evaluate), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(gate.main([*anonymous_args,"--peer-spec",str(empty)]),1)

    def test_baseline_records_failure_without_turning_metadata_into_pass(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder=Path(temporary);spec=folder/"spec.md";baseline=folder/"baseline.txt";report=folder/"report.json"
            spec.write_text("```xray @check\nprint(1)\n```\n",encoding="utf-8")
            key=gate.snippet_key(2,"print(1)");frozen=key+"\n";baseline.write_text(frozen,encoding="utf-8")
            args=["--xray","absent","--spec",str(spec),"--baseline",str(baseline),"--report",str(report)]
            for status,expected in [("CHECK_FAIL",0),("METADATA_FAIL",1)]:
                def evaluate(item,*unused):
                    return dict(status=status,key=key,source=item["source"],line=item["line"])
                with mock.patch.object(gate,"evaluate_block",evaluate),contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(gate.main(args),expected)
                saved=json.loads(report.read_text(encoding="utf-8"));self.assertFalse(saved["conforms"])
                self.assertEqual(saved["results"][0]["status"],status)
            self.assertEqual(baseline.read_text(encoding="utf-8"),frozen)


class RealCompilerTests(unittest.TestCase):
    def test_actual_vm_native_and_error(self):
        if REAL_BINARY is None:
            self.skipTest("requires --xray for actual VM/native validation")
        with tempfile.TemporaryDirectory() as temporary:
            folder=Path(temporary)
            row=gate.evaluate_block(block("@run",output="1\n"),REAL_BINARY,folder,ROOT)
            self.assertEqual(row["status"],"RUN_PASS",json.dumps(row,ensure_ascii=False))
            row=gate.evaluate_block(block("@error=E0326",'var values = [1, "x"] // error'),REAL_BINARY,folder,ROOT)
            self.assertEqual(row["status"],"ERROR_PASS",json.dumps(row,ensure_ascii=False))


if __name__ == "__main__":
    parser=argparse.ArgumentParser();parser.add_argument("--xray",type=Path);args,remaining=parser.parse_known_args()
    REAL_BINARY=args.xray.resolve() if args.xray else None
    unittest.main(argv=[sys.argv[0],*remaining])
