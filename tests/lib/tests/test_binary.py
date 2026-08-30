"""Tests for artifact inspection: symbol names and disassembly parsing.

Parsing is the risky part -- symbol-body extraction has to stop at exactly the
next label, or a straight-line assertion would scan a neighbouring function and
either miss a call or invent one. These run on synthetic text, so they need no
toolchain.
"""

import unittest
from pathlib import Path
from unittest import mock

from _support import bootstrap_xraytest

bootstrap_xraytest()
from xraytest import binary  # noqa: E402


class StripUnderscoreTest(unittest.TestCase):
    def test_darwin_prefix_removed(self):
        self.assertEqual(binary.strip_underscore("_memcpy"), "memcpy")

    def test_bare_name_untouched(self):
        self.assertEqual(binary.strip_underscore("memcpy"), "memcpy")

    def test_only_leading_one_removed(self):
        self.assertEqual(binary.strip_underscore("__internal"), "_internal")


class ExtractSymbolBodyTest(unittest.TestCase):
    OBJDUMP = "\n".join([
        "0000000000001000 <_alpha>:",
        "    1000: mov  x0, x1",
        "    1004: ret",
        "0000000000001008 <_beta>:",
        "    1008: bl   _alpha",
        "    100c: ret",
    ])

    def test_extracts_first_symbol_only(self):
        body = binary.extract_symbol_body(self.OBJDUMP, "alpha")
        self.assertIn("mov  x0, x1", body)
        # Must stop before beta, or beta's `bl` would be attributed to alpha.
        self.assertNotIn("bl   _alpha", body)

    def test_extracts_last_symbol_to_end(self):
        body = binary.extract_symbol_body(self.OBJDUMP, "beta")
        self.assertIn("bl   _alpha", body)
        self.assertNotIn("mov  x0, x1", body)

    def test_missing_symbol_is_empty(self):
        self.assertEqual(binary.extract_symbol_body(self.OBJDUMP, "gamma"), "")

    def test_tolerates_underscore_prefix_either_way(self):
        # The fixture labels are `_alpha`; asking for `alpha` must still match.
        self.assertNotEqual(binary.extract_symbol_body(self.OBJDUMP, "alpha"), "")

    def test_otool_label_form(self):
        text = "\n".join([
            "_gamma:",
            "0000 mov x0, x1",
            "_delta:",
            "0004 ret",
        ])
        body = binary.extract_symbol_body(text, "gamma")
        self.assertIn("mov x0, x1", body)
        self.assertNotIn("ret", body)


class ControlFlowTest(unittest.TestCase):
    def test_detects_arm_branch_and_link(self):
        self.assertTrue(binary.has_control_flow("    1000: bl   _helper"))

    def test_detects_x86_call(self):
        self.assertTrue(binary.has_control_flow("    1000: callq  _helper"))

    def test_detects_conditional_branch(self):
        self.assertTrue(binary.has_control_flow("    1000: cbz  x0, 1010"))
        self.assertTrue(binary.has_control_flow("    1000: b.eq 1010"))

    def test_straight_line_body_has_none(self):
        body = "\n".join([
            "    1000: ldr  w0, [x1]",
            "    1004: str  w0, [x2]",
            "    1008: ret",
        ])
        self.assertFalse(binary.has_control_flow(body))

    def test_ret_alone_is_not_control_flow(self):
        # `ret` ends a straight-line body; it must not count as a branch.
        self.assertFalse(binary.has_control_flow("    1008: ret"))


class DefinedSymbolParsingTest(unittest.TestCase):
    def test_nm_definitions_exclude_undefined(self):
        text = "00000000 T _defined\n         U _missing\n00000008 D data\n"
        self.assertEqual(
            binary.parse_defined_symbol_names(text, "nm"), ["data", "defined"]
        )

    def test_dumpbin_linker_members(self):
        retired_constructor = "".join(("xray_", "vm_", "new"))
        text = f"      21A {retired_constructor}\n      31F _xr_artifact_classify\n"
        self.assertEqual(
            binary.parse_defined_symbol_names(text, "dumpbin"),
            ["xr_artifact_classify", retired_constructor],
        )

    def test_dumpbin_undefined_object_symbols(self):
        text = "\n".join([
            "00A 00000000 UNDEF  notype ()    External     | _memcpy",
            "00B 00000000 SECT3  notype ()    External     | generated_entry",
            "00C 00000000 UNDEF  notype ()    External     | xr_aot_spawn",
            "00D 00000000 UNDEF  notype       Static       | local_placeholder",
            "00E 00000000 UNDEF  notype       WeakExternal | __imp_malloc",
            "00F 00000000 DEBUG  notype       Static       | .file",
        ])
        self.assertEqual(
            binary.parse_dumpbin_undefined_symbol_names(text),
            ["_imp_malloc", "memcpy", "xr_aot_spawn"],
        )

    def test_dumpbin_malformed_external_row_fails_closed(self):
        text = "00G 00000000 UNDEF notype () External | xr_aot_spawn\n"
        self.assertIsNone(binary.parse_dumpbin_undefined_symbol_names(text))

    def test_dumpbin_missing_pipe_fails_closed(self):
        text = "00A 00000000 UNDEF notype () External xr_aot_spawn\n"
        self.assertIsNone(binary.parse_dumpbin_undefined_symbol_names(text))

    def test_dumpbin_pipe_garbage_fails_closed(self):
        self.assertIsNone(binary.parse_dumpbin_undefined_symbol_names("malformed | row\n"))

    def test_dumpbin_partial_output_fails_closed(self):
        text = "\n".join([
            "00A 00000000 UNDEF notype () External | xr_aot_spawn",
            "00B 00000000 UNDEF notype () External xrt_closure_new",
        ])
        self.assertIsNone(binary.parse_dumpbin_undefined_symbol_names(text))

    def test_dumpbin_index_only_partial_row_fails_closed(self):
        text = "00A 00000000 UNDEF notype () External | xr_aot_spawn\n00B\n"
        self.assertIsNone(binary.parse_dumpbin_undefined_symbol_names(text))

    def test_dumpbin_symbol_trailing_garbage_fails_closed(self):
        text = "00A 00000000 UNDEF notype () External | xr_aot_spawn trailing\n"
        self.assertIsNone(binary.parse_dumpbin_undefined_symbol_names(text))

    def test_nm_undefined_gnu_and_llvm_rows(self):
        text = "_memcpy U 0 0\nweak_hook w 0 0\nweak_object v - -\n"
        self.assertEqual(
            binary.parse_nm_undefined_symbol_names(text),
            ["memcpy", "weak_hook", "weak_object"],
        )

    def test_nm_undefined_darwin_rows(self):
        text = "_memcpy U 0 0\n_xr_aot_spawn U 0 0\n"
        self.assertEqual(
            binary.parse_nm_undefined_symbol_names(text), ["memcpy", "xr_aot_spawn"]
        )

    def test_nm_undefined_coff_decorated_rows(self):
        text = "__imp_memcpy U 0 0\n?provider_hook@@YAXXZ U 0 0\n"
        self.assertEqual(
            binary.parse_nm_undefined_symbol_names(text),
            ["?provider_hook@@YAXXZ", "_imp_memcpy"],
        )

    def test_nm_malformed_row_fails_closed(self):
        text = "fatal: truncated object file\n"
        self.assertIsNone(binary.parse_nm_undefined_symbol_names(text))

    def test_nm_partial_output_fails_closed(self):
        text = "xr_aot_spawn U 0 0\nxrt_closure_new U 0\n"
        self.assertIsNone(binary.parse_nm_undefined_symbol_names(text))

    def test_nm_non_posix_and_defined_rows_fail_closed(self):
        self.assertIsNone(binary.parse_nm_undefined_symbol_names("_missing\n"))
        self.assertIsNone(binary.parse_nm_undefined_symbol_names("00000000 U missing\n"))
        self.assertIsNone(binary.parse_nm_undefined_symbol_names("defined T 0 0\n"))

    def test_nm_invalid_fields_fail_closed(self):
        for text in (
            "missing UU 0 0\n",
            "missing U invalid 0\n",
            "missing U 0 invalid\n",
            "missing U 0 0 trailing\n",
        ):
            with self.subTest(text=text):
                self.assertIsNone(binary.parse_nm_undefined_symbol_names(text))

    def test_nm_empty_undefined_output_is_valid(self):
        self.assertEqual(binary.parse_nm_undefined_symbol_names("\n"), [])

    def test_undefined_symbols_fall_back_to_dumpbin(self):
        text = "00A 00000000 UNDEF notype () External | _xr_aot_spawn\n"
        nm_failure = mock.Mock(ok=False, stdout=b"")
        dumpbin_result = mock.Mock(ok=True, stdout=text.encode("utf-8"))
        artifact = Path(__file__)
        with mock.patch.object(binary, "find_nm", return_value="nm"), mock.patch.object(
            binary, "find_dumpbin", return_value="dumpbin"
        ), mock.patch.object(
            binary.proc, "run", side_effect=(nm_failure, dumpbin_result)
        ) as run:
            names = binary.undefined_symbol_names(binary=artifact)
        self.assertEqual(names, ["xr_aot_spawn"])
        self.assertEqual(
            run.call_args_list,
            [
                mock.call(["nm", "-u", "-P", artifact], timeout=120),
                mock.call(["dumpbin", "/nologo", "/symbols", artifact], timeout=120),
            ],
        )

    def test_dumpbin_parse_failure_does_not_return_partial_symbols(self):
        malformed = "00G 00000000 UNDEF notype () External | xr_aot_spawn\n"
        result = mock.Mock(ok=True, stdout=malformed.encode("utf-8"))
        with mock.patch.object(binary, "find_nm", return_value=None), mock.patch.object(
            binary, "find_dumpbin", return_value="dumpbin"
        ), mock.patch.object(binary.proc, "run", return_value=result):
            with self.assertRaises(binary.SymbolInspectionError):
                binary.undefined_symbol_names(Path(__file__))

    def test_nm_parse_failure_does_not_return_partial_symbols(self):
        malformed = "xr_aot_spawn U 0 0\nxrt_closure_new U 0\n"
        result = mock.Mock(ok=True, stdout=malformed.encode("utf-8"))
        with mock.patch.object(binary, "find_nm", return_value="nm"), mock.patch.object(
            binary, "find_dumpbin", return_value="dumpbin"
        ), mock.patch.object(binary.proc, "run", return_value=result) as run:
            with self.assertRaises(binary.SymbolInspectionError):
                binary.undefined_symbol_names(Path(__file__))
        run.assert_called_once_with(["nm", "-u", "-P", Path(__file__)], timeout=120)

    def test_no_undefined_symbol_inspector_returns_none(self):
        with mock.patch.object(binary, "find_nm", return_value=None), mock.patch.object(
            binary, "find_dumpbin", return_value=None
        ):
            self.assertIsNone(binary.undefined_symbol_names(Path(__file__)))

    def test_failed_installed_inspectors_raise(self):
        failure = mock.Mock(ok=False, stdout=b"")
        with mock.patch.object(binary, "find_nm", return_value="nm"), mock.patch.object(
            binary, "find_dumpbin", return_value="dumpbin"
        ), mock.patch.object(binary.proc, "run", side_effect=(failure, failure)):
            with self.assertRaises(binary.SymbolInspectionError):
                binary.undefined_symbol_names(Path(__file__))


if __name__ == "__main__":
    unittest.main()
