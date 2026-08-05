"""Unit tests for the unified freestanding QEMU runner's configuration and helpers.

The load-bearing assertions: the include set must reach include/ (the omission
that kept four lanes red), and every target's fixture files must actually exist
under the names its xray.toml references -- a rename there fails the build with
an error that looks nothing like the cause.
"""

import unittest
from pathlib import Path

from _support import bootstrap_xraytest, load_module

bootstrap_xraytest()

_AOT_DIR = Path(__file__).resolve().parents[2] / "aot"
runner = load_module("freestanding_qemu_under_test", _AOT_DIR / "run_freestanding_qemu_smoke.py")


class TargetConfigTest(unittest.TestCase):
    def test_all_three_targets_present(self):
        self.assertEqual(sorted(runner.TARGETS), ["riscv32", "thumb", "x86_64"])

    def test_include_dirs_reach_public_abi_headers(self):
        # xrt_core_freestanding.h includes "xray_value_abi.h" by bare name; that
        # header lives in include/. Four lanes were red for want of this entry.
        self.assertIn("include", runner.INCLUDE_DIRS)

    def test_every_target_names_its_fixture_files(self):
        for name, target in runner.TARGETS.items():
            self.assertTrue(target.main_source, f"{name} has no main_source")
            self.assertTrue(target.linker_script, f"{name} has no linker_script")

    def test_fixture_files_exist_under_manifest_names(self):
        # The manifest references `main` and (RISC-V) the asm unit's `sources`
        # by name, so the on-disk names must match the config exactly.
        for name, target in runner.TARGETS.items():
            fixture_dir = runner.FIXTURE_ROOT / name
            self.assertTrue((fixture_dir / "xray.toml").is_file(), f"{name}: xray.toml")
            self.assertTrue((fixture_dir / target.main_source).is_file(),
                            f"{name}: {target.main_source}")
            self.assertTrue((fixture_dir / target.linker_script).is_file(),
                            f"{name}: {target.linker_script}")
            if target.boot_source:
                self.assertTrue((fixture_dir / target.boot_source).is_file(),
                                f"{name}: {target.boot_source}")

    def test_manifest_main_matches_configured_main_source(self):
        # If xray.toml says main = "foo.xr", the runner must stage foo.xr.
        for name, target in runner.TARGETS.items():
            toml = (runner.FIXTURE_ROOT / name / "xray.toml").read_text(encoding="utf-8")
            for line in toml.splitlines():
                if line.strip().startswith("main ="):
                    declared = line.split("=", 1)[1].strip().strip('"')
                    self.assertEqual(declared, target.main_source,
                                     f"{name}: manifest main != main_source")
                    break

    def test_riscv_manifest_carries_hash_placeholder(self):
        # The RISC-V manifest pins its boot asm by content hash; the runner must
        # substitute it, so both the placeholder and the config must agree.
        target = runner.TARGETS["riscv32"]
        self.assertEqual(target.hash_placeholder, "$START_HASH")
        toml = (runner.FIXTURE_ROOT / "riscv32" / "xray.toml").read_text(encoding="utf-8")
        self.assertIn("$START_HASH", toml)

    def test_verification_strategy_is_known(self):
        for name, target in runner.TARGETS.items():
            self.assertIn(target.verify, ("serial", "qmp_vga"), name)
            if target.verify == "serial":
                self.assertTrue(target.serial_marker, f"{name} has no serial marker")
            else:
                self.assertTrue(target.vga_expect, f"{name} has no VGA expectation")

    def test_each_target_has_a_pass_message(self):
        for name, target in runner.TARGETS.items():
            self.assertTrue(target.pass_message.startswith("PASS:"), name)


class SkipSemanticsTest(unittest.TestCase):
    def test_skip_exit_code_matches_ctest_property(self):
        # CMake declares SKIP_RETURN_CODE 77 for these lanes.
        self.assertEqual(runner.SKIP_EXIT, 77)

    def test_resolve_tool_prefers_env_override(self):
        import os

        os.environ["XT_FAKE_TOOL"] = "/some/pinned/path"
        try:
            self.assertEqual(runner.resolve_tool("XT_FAKE_TOOL", "nonexistent-binary"),
                             "/some/pinned/path")
        finally:
            os.environ.pop("XT_FAKE_TOOL", None)

    def test_resolve_tool_raises_skip_when_absent(self):
        with self.assertRaises(runner.Skip):
            runner.resolve_tool("XT_UNSET_VAR", "definitely-not-a-real-binary-xyz")


class CheckContainsTest(unittest.TestCase):
    def test_passes_when_all_present(self):
        runner.check_contains("alpha beta gamma", ("alpha", "gamma"), "test")

    def test_fails_naming_the_missing_needle(self):
        with self.assertRaises(runner.Fail) as ctx:
            runner.check_contains("alpha", ("beta",), "ELF header")
        self.assertIn("beta", str(ctx.exception))
        self.assertIn("ELF header", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
