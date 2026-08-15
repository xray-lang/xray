"""platform and toolchain tests: name affixes, LF writes, env flags, and the
probe that actually executes a candidate rather than trusting PATH.
"""

import os
import sys
import unittest
from pathlib import Path
from unittest import mock

from _support import bootstrap_xraytest

bootstrap_xraytest()
from xraytest import platform, toolchain  # noqa: E402


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


if __name__ == "__main__":
    unittest.main()
