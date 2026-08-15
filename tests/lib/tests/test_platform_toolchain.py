"""platform and toolchain tests: name affixes, LF writes, env flags, and the
probe that actually executes a candidate rather than trusting PATH.
"""

import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from _support import bootstrap_xraytest

bootstrap_xraytest()
from xraytest import platform, proc, toolchain  # noqa: E402


def result(*, returncode=0, stdout=b"", stderr=b"", timed_out=False):
    return proc.ProcResult(
        argv=(), returncode=returncode, stdout=stdout, stderr=stderr, timed_out=timed_out
    )


class PlatformTest(unittest.TestCase):
    def test_static_lib_name_matches_host(self):
        name = platform.static_lib_name("xray_core")
        if platform.IS_WINDOWS:
            self.assertEqual(name, "xray_core.lib")
        else:
            self.assertEqual(name, "libxray_core.a")

    def test_cpu_count_at_least_one(self):
        self.assertGreaterEqual(platform.cpu_count(), 1)

    def test_env_flag_accepts_shell_truthy_values(self):
        for val in ("1", "true", "YES", "on"):
            os.environ["XT_PROBE_FLAG"] = val
            self.assertTrue(platform.env_flag("XT_PROBE_FLAG"))
        os.environ["XT_PROBE_FLAG"] = "0"
        self.assertFalse(platform.env_flag("XT_PROBE_FLAG"))
        os.environ.pop("XT_PROBE_FLAG", None)
        self.assertFalse(platform.env_flag("XT_PROBE_FLAG"))
        self.assertTrue(platform.env_flag("XT_PROBE_FLAG", default=True))

    def test_project_root_holds_this_package(self):
        root = platform.project_root()
        self.assertTrue((root / "tests" / "lib" / "xraytest").is_dir())


class ToolchainProbeTest(unittest.TestCase):
    def setUp(self):
        toolchain.reset_probe_cache()

    def tearDown(self):
        toolchain.reset_probe_cache()
        os.environ.pop("XRAY_TEST_PYTHON", None)

    def test_find_python_returns_a_runnable_interpreter(self):
        os.environ["XRAY_TEST_PYTHON"] = sys.executable
        toolchain.reset_probe_cache()
        found = toolchain.find_python()
        self.assertIsNotNone(found)

    def test_find_python_rejects_a_stub_that_does_nothing(self):
        # Simulate the App Execution Alias: a shim on PATH that exits 1 without
        # running the code. The probe must reject it, not trust its presence.
        import tempfile
        import stat

        tmp = tempfile.mkdtemp(prefix="xt_stub.")
        try:
            stub = Path(tmp) / "python3"
            stub.write_text("#!/bin/sh\nexit 1\n", encoding="utf-8")
            stub.chmod(stub.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
            os.environ["XRAY_TEST_PYTHON"] = str(stub)
            toolchain.reset_probe_cache()
            self.assertIsNone(toolchain.find_python())
        finally:
            import shutil

            shutil.rmtree(tmp, ignore_errors=True)

    @unittest.skipIf(platform.IS_WINDOWS, "POSIX shell stub")
    def test_zig_compiler_uses_object_compile_for_syntax(self):
        cc = toolchain.CCompiler(path="/usr/bin/zig", driver=toolchain.CC_DRIVER_ZIG)
        argv = cc.syntax_check_argv(Path("a.c"), ["/inc"], Path("/tmp/a.o"))
        self.assertIn("cc", argv)
        self.assertIn("-std=c11", argv)
        self.assertIn("-c", argv)
        self.assertNotIn("-fsyntax-only", argv)

    def test_normal_compiler_uses_fsyntax_only(self):
        cc = toolchain.CCompiler(path="/usr/bin/cc", driver=toolchain.CC_DRIVER_GNU)
        argv = cc.syntax_check_argv(Path("a.c"), ["/inc"], Path("/tmp/a.o"))
        self.assertIn("-std=c11", argv)
        self.assertIn("-fsyntax-only", argv)
        self.assertIn("-I/inc", argv)

    def test_msvc_compiler_uses_c11_syntax_only_contract(self):
        cc = toolchain.CCompiler(path=r"C:\VC\bin\cl.exe", driver=toolchain.CC_DRIVER_MSVC)
        argv = cc.syntax_check_argv(Path("a.c"), [r"C:\xray\include"], Path("unused.obj"))
        self.assertIn("/TC", argv)
        self.assertIn("/std:c11", argv)
        self.assertIn("/experimental:c11atomics", argv)
        self.assertIn("/utf-8", argv)
        self.assertIn("/Zs", argv)
        self.assertIn(r"/IC:\xray\include", argv)
        self.assertNotIn("unused.obj", argv)

    def test_find_c_syntax_compiler_prefers_and_probes_msvc_on_windows(self):
        cl = r"C:\VC\bin\cl.exe"

        def resolve(name):
            return cl if name == "cl" else None

        with mock.patch.dict(os.environ, {"CC": ""}), mock.patch.object(
            toolchain.platform, "IS_WINDOWS", True
        ), mock.patch.object(toolchain.shutil, "which", side_effect=resolve), mock.patch.object(
            toolchain.proc, "run", return_value=mock.Mock(ok=True)
        ) as run:
            self.assertIsNone(toolchain.find_c_compiler())
            found = toolchain.find_c_syntax_compiler()

        self.assertEqual(found, toolchain.CCompiler(path=cl, driver=toolchain.CC_DRIVER_MSVC))
        run.assert_called_once_with([cl, "/nologo", "/?"], timeout=30)

    def test_find_symbol_dumper_prefers_and_probes_dumpbin_on_windows(self):
        dumpbin = r"C:\VC\bin\dumpbin.exe"

        def resolve(name):
            return dumpbin if name == "dumpbin" else None

        identity = result(
            stdout=b"Dump of file dumpbin.exe\nPE signature found\nFile Type: EXECUTABLE IMAGE\n"
        )
        with mock.patch.object(
            toolchain.platform, "IS_WINDOWS", True
        ), mock.patch.object(
            toolchain.shutil, "which", side_effect=resolve
        ), mock.patch.object(toolchain.proc, "run", return_value=identity) as run:
            found = toolchain.find_symbol_dumper()

        self.assertEqual(
            found,
            toolchain.SymbolDumper(path=dumpbin, driver=toolchain.SYMBOL_DUMPER_DUMPBIN),
        )
        run.assert_called_once_with(
            [dumpbin, "/nologo", "/headers", dumpbin], timeout=30
        )

    def test_symbol_dumper_rejects_wrong_identity_even_on_zero_exit(self):
        dumpbin = r"C:\wrong\dumpbin.exe"
        with mock.patch.object(
            toolchain.platform, "IS_WINDOWS", True
        ), mock.patch.object(
            toolchain.shutil, "which",
            side_effect=lambda name: dumpbin if name == "dumpbin" else None,
        ), mock.patch.object(
            toolchain.proc, "run", return_value=result(stdout=b"not Microsoft dumpbin\n")
        ):
            self.assertIsNone(toolchain.find_symbol_dumper())

    def test_symbol_dumper_rejects_nonzero_identity_probe(self):
        dumpbin = r"C:\VC\bin\dumpbin.exe"
        with mock.patch.object(
            toolchain.platform, "IS_WINDOWS", True
        ), mock.patch.object(
            toolchain.shutil, "which",
            side_effect=lambda name: dumpbin if name == "dumpbin" else None,
        ), mock.patch.object(
            toolchain.proc,
            "run",
            return_value=result(
                returncode=1,
                stdout=b"PE signature found\nFile Type: EXECUTABLE IMAGE\n",
            ),
        ):
            self.assertIsNone(toolchain.find_symbol_dumper())

    def test_dumpbin_normalizes_defined_symbols_and_drops_undefined_rows(self):
        dumper = toolchain.SymbolDumper(
            path=r"C:\VC\bin\dumpbin.exe", driver=toolchain.SYMBOL_DUMPER_DUMPBIN
        )
        output = (
            b"001 00000000 UNDEF notype () External | xr_vm_unresolved\n"
            b"002 00000010 SECT3 notype () External | xr_parse_defined\n"
            b"003 00000020 ABS notype Static | xr_absolute\n"
        )
        with mock.patch.object(toolchain.proc, "run", return_value=result(stdout=output)) as run:
            ok, symbols = dumper.dump_defined_symbols(Path("artifact.lib"))

        self.assertTrue(ok)
        self.assertEqual(symbols.splitlines(), ["xr_parse_defined", "xr_absolute"])
        run.assert_called_once_with(
            [r"C:\VC\bin\dumpbin.exe", "/nologo", "/symbols", "artifact.lib"],
            timeout=120,
        )

    def test_dumpbin_rejects_nonzero_and_malformed_or_empty_output(self):
        dumper = toolchain.SymbolDumper(
            path=r"C:\VC\bin\dumpbin.exe", driver=toolchain.SYMBOL_DUMPER_DUMPBIN
        )
        mutations = [
            result(returncode=2, stderr=b"bad image\n"),
            result(stdout=b"001 malformed | xr_parse_hidden\n"),
            result(stdout=b"Dump of file stripped.exe\nFile Type: EXECUTABLE IMAGE\n"),
        ]
        for mutation in mutations:
            with self.subTest(mutation=mutation), mock.patch.object(
                toolchain.proc, "run", return_value=mutation
            ):
                ok, detail = dumper.dump_defined_symbols(Path("artifact.exe"))
            self.assertFalse(ok)
            self.assertTrue("exit 2" in detail or "unrecognized" in detail or
                            "no defined symbols" in detail)

    def test_llvm_nm_normalizes_posix_defined_symbol_rows(self):
        dumper = toolchain.SymbolDumper(
            path="llvm-nm", driver=toolchain.SYMBOL_DUMPER_LLVM_NM
        )
        output = b"member.obj:\nxr_clean T 0 10\nxr_data D 10 8\n"
        with mock.patch.object(toolchain.proc, "run", return_value=result(stdout=output)):
            ok, symbols = dumper.dump_defined_symbols(Path("artifact.lib"))
        self.assertTrue(ok)
        self.assertEqual(symbols.splitlines(), ["xr_clean", "xr_data"])

    def test_llvm_nm_rejects_undefined_rows_in_defined_only_output(self):
        dumper = toolchain.SymbolDumper(
            path="llvm-nm", driver=toolchain.SYMBOL_DUMPER_LLVM_NM
        )
        with mock.patch.object(
            toolchain.proc, "run", return_value=result(stdout=b"xr_bad U 0 0\n")
        ):
            ok, detail = dumper.dump_defined_symbols(Path("artifact.lib"))
        self.assertFalse(ok)
        self.assertIn("undefined symbol", detail)


class MsvcLinkMapTest(unittest.TestCase):
    TIMESTAMP = 0x6A801B14

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="xray_msvc_map_test.")
        self.root = Path(self.temp.name)
        self.driver = self.root / "cl.exe"
        self.driver.write_bytes(b"driver")
        self.binary = self.root / "runtime.exe.tmp"
        self.object = self.root / "generated.obj"
        self.archive = self.root / "xray_rt_coro.lib"
        self.map_path = self.root / "runtime.map"
        self.object.write_bytes(b"object")
        self.archive.write_bytes(b"archive")
        self._write_pe(self.binary, self.TIMESTAMP)
        self.compiler = toolchain.CCompiler(
            path=str(self.driver), driver=toolchain.CC_DRIVER_MSVC
        )

    def tearDown(self):
        self.temp.cleanup()

    @staticmethod
    def _write_pe(path: Path, timestamp: int):
        data = bytearray(256)
        data[:2] = b"MZ"
        struct.pack_into("<I", data, 0x3C, 0x80)
        data[0x80:0x84] = b"PE\0\0"
        struct.pack_into("<I", data, 0x88, timestamp)
        path.write_bytes(data)

    def _log(self, *, driver: "Path | None" = None, output: "Path | None" = None) -> str:
        driver = driver or self.driver
        output = output or self.binary
        return (
            f"Link command: {driver} /nologo /Fe{output} {self.object} "
            f"{self.archive} ws2_32.lib /link /OPT:REF\n"
        )

    def _map_text(
        self, *, target: str = "runtime.exe", timestamp: "int | None" = None,
        public_row: str | None = None, static_row: str | None = None
    ) -> str:
        timestamp = self.TIMESTAMP if timestamp is None else timestamp
        public_row = public_row or (
            " 0001:00000000       main                       "
            "0000000140001000 f   generated.obj"
        )
        static_row = static_row or (
            " 0001:00000020       xr_clean                   "
            "0000000140001020 f   xray_rt_coro:runtime.obj"
        )
        return "\n".join([
            f" {target}",
            "",
            f" Timestamp is {timestamp:08x} (test)",
            "",
            "  Address         Publics by Value              Rva+Base               Lib:Object",
            public_row,
            "",
            " entry point at        0001:00000000",
            "",
            " Static symbols",
            static_row,
            "",
        ])

    def _capture(self, map_text: "str | None", run_result=None):
        run_result = run_result or result()

        def run(argv, timeout):
            if map_text is not None:
                self.map_path.write_text(map_text, encoding="utf-8")
            return run_result

        with mock.patch.object(toolchain.proc, "run", side_effect=run) as invoked:
            evidence, error = toolchain.capture_msvc_link_map(
                self._log(), self.compiler, self.binary, self.map_path, timeout=17
            )
        return evidence, error, invoked

    def test_link_map_binds_command_inputs_and_pe_identity(self):
        evidence, error, invoked = self._capture(self._map_text())
        self.assertEqual(error, "")
        self.assertIsNotNone(evidence)
        self.assertEqual(evidence.symbols.splitlines(), ["main", "xr_clean"])
        self.assertEqual(evidence.symbol_count, 2)
        self.assertEqual(evidence.coff_timestamp, self.TIMESTAMP)
        self.assertEqual(len(evidence.command_sha256), 64)
        self.assertEqual(len(evidence.binary_sha256), 64)
        argv = invoked.call_args.args[0]
        self.assertEqual(argv[0], str(self.driver))
        self.assertEqual(argv[-1], f"/MAP:{self.map_path}")
        self.assertEqual(invoked.call_args.kwargs["timeout"], 17)

    def test_link_map_rejects_wrong_driver_before_execution(self):
        wrong = self.root / "gcc.exe"
        with mock.patch.object(toolchain.proc, "run") as invoked:
            evidence, error = toolchain.capture_msvc_link_map(
                self._log(driver=wrong), self.compiler, self.binary, self.map_path
            )
        self.assertIsNone(evidence)
        self.assertIn("supported MSVC driver", error)
        invoked.assert_not_called()

    def test_link_map_rejects_nonzero_relink(self):
        evidence, error, _ = self._capture(None, result(returncode=2, stderr=b"link failed"))
        self.assertIsNone(evidence)
        self.assertIn("exit 2", error)

    def test_link_map_rejects_missing_or_empty_output(self):
        for text in (None, ""):
            with self.subTest(text=text):
                evidence, error, _ = self._capture(text)
                self.assertIsNone(evidence)
                self.assertIn("non-empty linker map", error)

    def test_link_map_rejects_malformed_symbol_row(self):
        evidence, error, _ = self._capture(
            self._map_text(public_row=" 0001 malformed defined symbol")
        )
        self.assertIsNone(evidence)
        self.assertIn("malformed defined-symbol row", error)

    def test_link_map_rejects_wrong_binary_name(self):
        evidence, error, _ = self._capture(self._map_text(target="other.exe"))
        self.assertIsNone(evidence)
        self.assertIn("target does not match", error)

    def test_link_map_rejects_wrong_binary_timestamp(self):
        evidence, error, _ = self._capture(self._map_text(timestamp=self.TIMESTAMP + 1))
        self.assertIsNone(evidence)
        self.assertIn("timestamp does not match", error)

    def test_link_map_rejects_missing_binary_after_relink(self):
        map_text = self._map_text()

        def run(argv, timeout):
            self.binary.unlink()
            self.map_path.write_text(map_text, encoding="utf-8")
            return result()

        with mock.patch.object(toolchain.proc, "run", side_effect=run):
            evidence, error = toolchain.capture_msvc_link_map(
                self._log(), self.compiler, self.binary, self.map_path
            )
        self.assertIsNone(evidence)
        self.assertIn("no target PE image", error)

    def test_link_map_rejects_missing_final_input(self):
        self.object.unlink()
        with mock.patch.object(toolchain.proc, "run") as invoked:
            evidence, error = toolchain.capture_msvc_link_map(
                self._log(), self.compiler, self.binary, self.map_path
            )
        self.assertIsNone(evidence)
        self.assertIn("final object input is missing", error)
        invoked.assert_not_called()

    def test_link_map_rejects_uncovered_archive_input(self):
        text = self._map_text(
            static_row=(
                " 0001:00000020       xr_clean                   "
                "0000000140001020 f   other_runtime:runtime.obj"
            )
        )
        evidence, error, _ = self._capture(text)
        self.assertIsNone(evidence)
        self.assertIn("does not cover final archive input", error)


if __name__ == "__main__":
    unittest.main()
