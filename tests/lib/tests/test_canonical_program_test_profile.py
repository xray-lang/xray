import importlib.util
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "canonical_program_test_profile",
    ROOT / "scripts" / "canonical_program_test_profile.py",
)
assert SPEC and SPEC.loader
profile = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(profile)


class CanonicalProgramTestProfileTests(unittest.TestCase):
    def test_inventory_is_unique_and_build_targets_are_test_evidence(self) -> None:
        self.assertEqual(len(profile.CTEST_NAMES), len(set(profile.CTEST_NAMES)))
        self.assertEqual(len(profile.BUILD_TARGETS), len(set(profile.BUILD_TARGETS)))
        self.assertLessEqual(set(profile.BUILD_TARGETS), set(profile.CTEST_NAMES))
        self.assertIn("test_xr_program_vm_runtime", profile.BUILD_TARGETS)

    def test_regex_is_exact(self) -> None:
        pattern = re.compile(profile.ctest_regex())
        for name in profile.CTEST_NAMES:
            self.assertIsNotNone(pattern.fullmatch(name))
            self.assertIsNone(pattern.fullmatch(name + "_shadow"))

    def test_ctest_listing_parser_ignores_noise(self) -> None:
        output = """Test project C:/work/build
          Test #12: alpha
        missing executable warning
          Test #104: beta
        Total Tests: 2
        """
        self.assertEqual(profile.listed_ctest_names(output), ("alpha", "beta"))

    def test_preflight_covers_all_canonical_boundaries(self) -> None:
        required = {
            "test_core_spec",
            "test_xr_program",
            "test_xr_program_verify",
            "test_xr_program_source_build",
            "test_xr_program_vm",
            "test_xr_program_vm_runtime",
            "test_xr_program_aot",
            "test_xr_program_provider_trap_cleanup_aot_native",
            "meta_ownership_inventory",
            "contract_freeze",
            "contract_freeze_injection",
        }
        self.assertLessEqual(required, set(profile.CTEST_NAMES))


if __name__ == "__main__":
    unittest.main()
