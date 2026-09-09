"""Shared fail-closed test inventory for the canonical-program edit loop.

This is intentionally a named preflight, not a replacement for any broad test
tier or the full sanitizer lane.  Release and sanitizer runners import the same
inventory so a locally shortened regex cannot silently become the qualification
claim for Task 293.
"""

from __future__ import annotations

import re
from pathlib import Path

import program_source_fixtures as source_fixtures


_SCRIPT_AND_EXECUTABLE_TESTS = (
    "canonical_source_run_cli",
    "canonical_cutover_manifests",
    "canonical_cutover_manifests_self_test",
    "contract_freeze",
    "contract_freeze_injection",
    "core_spec_registry",
    "core_spec_registry_self_test",
    "meta_ownership_inventory",
    "test_core_spec",
    "test_xr_program",
    "test_xr_program_aot",
    "test_xr_program_aot_condition_assert",
    "test_xr_program_source_build",
    "test_xr_program_verify",
    "test_xr_program_vm",
    "test_xr_program_vm_runtime",
    "test_xglobal_summary",
    "test_xi_pipeline_canonical",
    "test_xi_verify_ext",
    "xr_execution_contracts",
    "xr_execution_contracts_self_test",
    "xr_program_aot_contracts",
    "xr_program_aot_contracts_self_test",
    "xr_program_schema",
    "xr_program_schema_self_test",
    "xr_program_semantic_coverage",
    "xr_program_semantic_coverage_self_test",
    "xr_program_source_contracts",
    "xr_program_source_contracts_self_test",
    "xr_program_source_fixtures_self_test",
    "xr_program_vm_contracts",
    "xr_program_vm_contracts_self_test",
    "xr_program_wave3_closure",
    "xr_program_wave3_closure_self_test",
    "xr_program_wave4_contract",
    "xr_program_wave4_contract_self_test",
)

# Script-only gates have no Ninja target. Native source fixtures are projected
# from the same registry as CMake; the CLI is not a blanket build proxy.
_EXECUTABLE_TARGETS = (
    "test_core_spec",
    "test_xr_program",
    "test_xr_program_aot",
    "test_xr_program_aot_condition_assert",
    "test_xr_program_source_build",
    "test_xr_program_verify",
    "test_xr_program_vm",
    "test_xr_program_vm_runtime",
    "test_xglobal_summary",
    "test_xi_verify_ext",
)

# Support executables whose CTest registration has a deliberately different
# name. Keep the evidence mapping explicit so adding a broad build proxy cannot
# silently enter the exact profile.
_SUPPORT_BUILD_TARGET_TESTS = {
    "test_xi_pipeline": "test_xi_pipeline_canonical",
    "xray": "canonical_source_run_cli",
}


def load_inventory(manifest: Path = source_fixtures.MANIFEST,
                   source: Path = source_fixtures.SOURCE) -> tuple[tuple[str, ...], tuple[str, ...]]:
    registry = source_fixtures.load_registry(manifest, source)
    native_targets = source_fixtures.native_target_names(registry)
    tests = _SCRIPT_AND_EXECUTABLE_TESTS + native_targets
    targets = _EXECUTABLE_TARGETS + tuple(_SUPPORT_BUILD_TARGET_TESTS) + native_targets
    if len(tests) != len(set(tests)) or len(targets) != len(set(targets)):
        raise source_fixtures.FixtureError("canonical profile contains duplicate registration")
    if not set(_EXECUTABLE_TARGETS + native_targets) <= set(tests):
        raise source_fixtures.FixtureError("canonical build target lacks a qualification test")
    if not set(_SUPPORT_BUILD_TARGET_TESTS.values()) <= set(tests):
        raise source_fixtures.FixtureError("canonical support target lacks a qualification test")
    return tests, targets


CTEST_NAMES, BUILD_TARGETS = load_inventory()


def ctest_regex() -> str:
    # CTest uses its C++/POSIX-style regex engine, which has no Python-style
    # non-capturing group syntax.
    return "^(" + "|".join(re.escape(name) for name in CTEST_NAMES) + ")$"


def listed_ctest_names(output: str) -> tuple[str, ...]:
    pattern = re.compile(r"^\s*Test\s+#[0-9]+:\s+(\S+)", re.MULTILINE)
    return tuple(pattern.findall(output))
