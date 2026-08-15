"""Unit tests for fail-closed symbol capability use in the AOT isolate gate."""

import unittest
from pathlib import Path
from unittest import mock

from _support import bootstrap_xraytest, load_module

bootstrap_xraytest()

_AOT_DIR = Path(__file__).resolve().parents[2] / "aot"
runner = load_module(
    "run_aot_isolate_symbol_tests_under_test",
    _AOT_DIR / "run_aot_isolate_symbol_tests.py",
)


class SymbolCapabilityTest(unittest.TestCase):
    def tearDown(self):
        runner.toolchain.reset_probe_cache()

    def test_runner_consumes_normalized_defined_symbols(self):
        dumper = mock.Mock()
        dumper.dump_defined_symbols.return_value = (True, "xr_clean\nxr_parse_forbidden")
        artifact = Path("artifact.lib")
        with mock.patch.object(
            runner.toolchain, "find_symbol_dumper", return_value=dumper
        ):
            ok, symbols = runner.dump_symbols(artifact)

        self.assertTrue(ok)
        self.assertRegex(symbols, runner.FORBIDDEN_SYMBOL_RE)
        dumper.dump_defined_symbols.assert_called_once_with(artifact)

    def test_runner_fails_when_no_verified_symbol_capability_exists(self):
        with mock.patch.object(
            runner.toolchain, "find_symbol_dumper", return_value=None
        ):
            ok, detail = runner.dump_symbols(Path("artifact.exe"))
        self.assertFalse(ok)
        self.assertIn("no verified defined-symbol dumper", detail)


if __name__ == "__main__":
    unittest.main()
