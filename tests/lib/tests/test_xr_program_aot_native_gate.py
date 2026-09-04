"""Pure-Python tests for the canonical-program native-AOT residue gate."""

import unittest
from pathlib import Path
from unittest import mock

from _support import bootstrap_xraytest, load_module

bootstrap_xraytest()

ROOT = Path(__file__).resolve().parents[3]
gate = load_module(
    "check_xr_program_aot_native_under_test",
    ROOT / "scripts" / "check_xr_program_aot_native.py",
)


class NativeAotGateTest(unittest.TestCase):
    def test_exit_status_matches_portable_generated_main_on_every_host(self):
        self.assertEqual(gate.expected_process_exit(477, windows=True), 221)
        self.assertEqual(gate.expected_process_exit(477, windows=False), 221)

    def test_inventory_uses_verified_normalized_dumper(self):
        dumper = mock.Mock()
        dumper.dump_defined_symbols.return_value = (True, "main\nxr_clean")
        executable = Path("artifact.exe")
        with mock.patch.object(gate.os, "name", "posix"), mock.patch.object(
            gate.toolchain, "find_symbol_dumper", return_value=dumper
        ):
            symbols, error = gate.load_symbol_inventory(executable)

        self.assertEqual(symbols, "main\nxr_clean")
        self.assertEqual(error, "")
        dumper.dump_defined_symbols.assert_called_once_with(executable)

    def test_inventory_fails_closed_without_symbols(self):
        mutations = (
            (None, "no verified defined-symbol dumper"),
            (mock.Mock(), "empty inventory"),
            (mock.Mock(), "malformed symbol rows"),
        )
        mutations[1][0].dump_defined_symbols.return_value = (True, "\n")
        mutations[2][0].dump_defined_symbols.return_value = (False, "malformed symbol rows")
        for dumper, expected in mutations:
            with self.subTest(expected=expected), mock.patch.object(
                gate.os, "name", "posix"
            ), mock.patch.object(gate.toolchain, "find_symbol_dumper", return_value=dumper):
                symbols, error = gate.load_symbol_inventory(Path("artifact.exe"))
            self.assertIsNone(symbols)
            self.assertIn(expected, error)

    def test_windows_inventory_uses_exact_sibling_linker_map(self):
        executable = Path("artifact.exe")
        with mock.patch.object(gate.os, "name", "nt"), mock.patch.object(
            gate.toolchain,
            "load_msvc_link_map_symbols",
            return_value=(True, "main\nxr_clean"),
        ) as load:
            symbols, error = gate.load_symbol_inventory(executable)

        self.assertEqual(symbols, "main\nxr_clean")
        self.assertEqual(error, "")
        load.assert_called_once_with(executable, Path("artifact.map"))


if __name__ == "__main__":
    unittest.main()
