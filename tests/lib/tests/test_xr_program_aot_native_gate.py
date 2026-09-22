"""Pure-Python tests for the canonical-program native-AOT residue gate."""

import unittest
import subprocess
import tempfile
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
    def test_exact_error_diagnostic_rejects_missing_changed_and_truncated_payload(self):
        expected = b'[Uncaught Error] Failure.Failed("detail")\n'
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "native.exe"
            executable.write_bytes(b"fixture")
            for actual in (expected, b"", expected[:-1], expected.replace(b"detail", b"other")):
                with self.subTest(actual=actual), mock.patch.object(gate.sys, "argv", [
                    "gate", "--executable", str(executable), "--expected-exit", "1",
                    "--expected-stderr-hex", expected.hex(),
                ]), mock.patch.object(gate.subprocess, "run", return_value=
                    subprocess.CompletedProcess([], 1, b"", actual)), mock.patch.object(
                    gate, "load_symbol_inventory", return_value=("main", "")
                ), mock.patch.object(gate.sys, "stderr", mock.Mock()):
                    self.assertEqual(gate.main(), 0 if actual == expected else 1)

    def test_compiler_families_reject_decorated_defined_symbols(self):
        forbidden = (
            "xr_program_validate", "xr_program_write", "xr_program_decode",
            "xr_backend_ir_build", "xr_vm_execute", "xvm_run", "xaot_build",
            "xr_target_plan_build", "xr_core_ir_new", "xi_lower_program",
            "xi_pipeline_run", "xi_cgen_emit",
        )
        decorations = ("", "_", "__imp_", "__imp__", "$unwind$", "?")
        for symbol in forbidden:
            for decoration in decorations:
                with self.subTest(symbol=symbol, decoration=decoration):
                    self.assertIsNotNone(gate.forbidden_symbol_family(
                        "main\n" + decoration + symbol + "\nxr_clean\n"))

    def test_source_symbol_substrings_do_not_grant_compiler_identity(self):
        symbols = ("xray_xaot_net_tls_8432_modinit",
                   "xrt_shared_xray_xaot_net_tls_8432",
                   "$unwind$xray_xaot_net_tls_8432_modinit",
                   "fixture_xr_program_validate_user_function",
                   "_user_xi_cgen_emit_wrapper")
        self.assertIsNone(gate.forbidden_symbol_family("\n".join(symbols)))
        self.assertIsNotNone(gate.forbidden_symbol_family(
            "\n".join(symbols) + "\n_XAOT_BUILD\n"))

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
