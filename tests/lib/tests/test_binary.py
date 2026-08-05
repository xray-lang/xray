"""Tests for artifact inspection: symbol names and disassembly parsing.

Parsing is the risky part -- symbol-body extraction has to stop at exactly the
next label, or a straight-line assertion would scan a neighbouring function and
either miss a call or invent one. These run on synthetic text, so they need no
toolchain.
"""

import unittest

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


if __name__ == "__main__":
    unittest.main()
