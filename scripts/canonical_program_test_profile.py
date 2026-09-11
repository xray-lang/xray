"""Shared fail-closed test inventory for the canonical-program edit loop.

This is intentionally a named preflight, not a replacement for any broad test
tier or the full sanitizer lane.  Release and sanitizer runners import the same
inventory so a locally shortened regex cannot silently become the qualification
claim for Task 293.
"""

from __future__ import annotations

import re
from pathlib import Path
import xml.etree.ElementTree as ET

import program_source_fixtures as source_fixtures
import check_xr_program_h2_backend_differential as h2_differential


GENERIC_IDENTITY_CTEST_NAMES = (
    "test_mono",
    "test_xglobal_summary",
    "test_xglobal_cache_payload",
    "test_xr_program_source_build",
)
GENERIC_IDENTITY_BUILD_TARGETS = GENERIC_IDENTITY_CTEST_NAMES

H2_REFERENCE_CTEST_NAMES = (
    "test_core_spec",
    "test_xr_program_verify",
)
H2_REFERENCE_BUILD_TARGETS = H2_REFERENCE_CTEST_NAMES

H2_SOURCE_CTEST_NAMES = (
    "canonical_cutover_manifests",
    "canonical_cutover_manifests_self_test",
    "contract_freeze",
    "contract_freeze_injection",
    "core_spec_registry",
    "core_spec_registry_self_test",
    "meta_ownership_inventory",
    "test_core_spec",
    "test_xr_program",
    "test_xr_program_verify",
    "test_xr_program_vm",
    "test_xr_program_source_build",
    "xr_program_schema",
    "xr_program_schema_self_test",
    "xr_program_semantic_coverage",
    "xr_program_semantic_coverage_self_test",
    "xr_program_source_contracts",
    "xr_program_source_contracts_self_test",
    "xr_program_source_fixtures_self_test",
)
H2_SOURCE_BUILD_TARGETS = (
    "test_core_spec",
    "test_xr_program",
    "test_xr_program_verify",
    "test_xr_program_vm",
    "test_xr_program_source_build",
)

H2_VM_CTEST_NAMES = (
    "test_core_spec",
    "test_xr_program_verify",
    "test_xr_program_vm",
    "test_xr_program_vm_runtime",
    "xr_program_vm_contracts",
    "xr_program_vm_contracts_self_test",
)
H2_VM_BUILD_TARGETS = (
    "test_core_spec",
    "test_xr_program_verify",
    "test_xr_program_vm",
    "test_xr_program_vm_runtime",
)

H2_AOT_CTEST_NAMES = (
    "test_core_spec",
    "test_xr_program_verify",
    "test_xr_program_aot",
    "xr_program_aot_contracts",
    "xr_program_aot_contracts_self_test",
)
H2_AOT_BUILD_TARGETS = (
    "test_core_spec",
    "test_xr_program_verify",
    "test_xr_program_aot",
)

H2_DIFFERENTIAL_CTEST_NAMES = (
    "xr_program_h2_backend_differential",
    "xr_program_h2_backend_differential_self_test",
) + h2_differential.qualification_ctest_names()
H2_DIFFERENTIAL_BUILD_TARGETS = h2_differential.qualification_build_targets()


def _stable_union(*inventories: tuple[str, ...]) -> tuple[str, ...]:
    return tuple(dict.fromkeys(
        item for inventory in inventories for item in inventory
    ))


# Shared semantic edits span the producer, verifier/reference oracle, both
# private backends, and the cross-executor differential ratchet. Derive the
# aggregate from every owner inventory so none can grow without the daily H2
# gate growing with it.
H2_CTEST_NAMES = _stable_union(
    H2_REFERENCE_CTEST_NAMES,
    H2_SOURCE_CTEST_NAMES,
    H2_VM_CTEST_NAMES,
    H2_AOT_CTEST_NAMES,
    H2_DIFFERENTIAL_CTEST_NAMES,
)
H2_BUILD_TARGETS = _stable_union(
    H2_REFERENCE_BUILD_TARGETS,
    H2_SOURCE_BUILD_TARGETS,
    H2_VM_BUILD_TARGETS,
    H2_AOT_BUILD_TARGETS,
    H2_DIFFERENTIAL_BUILD_TARGETS,
)


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
) + GENERIC_IDENTITY_CTEST_NAMES + (
    "test_xr_program_verify",
    "test_xr_program_vm",
    "test_xr_program_vm_runtime",
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
    "xr_program_h2_backend_differential",
    "xr_program_h2_backend_differential_self_test",
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
) + GENERIC_IDENTITY_BUILD_TARGETS + (
    "test_xr_program_verify",
    "test_xr_program_vm",
    "test_xr_program_vm_runtime",
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
    pending_tests = source_fixtures.pending_test_names(registry)
    tests = _SCRIPT_AND_EXECUTABLE_TESTS + native_targets + pending_tests
    targets = _EXECUTABLE_TARGETS + tuple(_SUPPORT_BUILD_TARGET_TESTS) + native_targets
    if len(tests) != len(set(tests)) or len(targets) != len(set(targets)):
        raise source_fixtures.FixtureError("canonical profile contains duplicate registration")
    if not set(_EXECUTABLE_TARGETS + native_targets) <= set(tests):
        raise source_fixtures.FixtureError("canonical build target lacks a qualification test")
    if not set(_SUPPORT_BUILD_TARGET_TESTS.values()) <= set(tests):
        raise source_fixtures.FixtureError("canonical support target lacks a qualification test")
    return tests, targets


CTEST_NAMES, BUILD_TARGETS = load_inventory()


def ctest_regex(names: tuple[str, ...] = CTEST_NAMES) -> str:
    # CTest uses its C++/POSIX-style regex engine, which has no Python-style
    # non-capturing group syntax.
    return "^(" + "|".join(re.escape(name) for name in names) + ")$"


def listed_ctest_names(output: str) -> tuple[str, ...]:
    pattern = re.compile(r"^\s*Test\s+#[0-9]+:\s+(\S+)", re.MULTILINE)
    return tuple(pattern.findall(output))


def executed_ctest_names(report: Path) -> tuple[str, ...]:
    """Read the tests CTest says it actually ran from its JUnit report.

    CTest's discovery listing proves registration, not execution.  Exact gates
    need both facts: a disabled or otherwise skipped test can appear in
    ``ctest -N`` while contributing no qualification evidence.  Treat a
    missing, malformed, unnamed, duplicated, or non-executed testcase as an
    invalid report instead of reducing it to a misleading count.
    """
    try:
        root = ET.parse(report).getroot()
    except OSError as error:
        raise source_fixtures.FixtureError(
            f"CTest execution report is missing: {report}"
        ) from error
    except ET.ParseError as error:
        raise source_fixtures.FixtureError(
            f"CTest execution report is malformed: {error}"
        ) from error

    names: list[str] = []
    not_run: list[str] = []
    for testcase in root.iter():
        if testcase.tag.rsplit("}", 1)[-1] != "testcase":
            continue
        name = testcase.get("name", "").strip()
        if not name:
            raise source_fixtures.FixtureError(
                "CTest execution report contains an unnamed testcase"
            )
        skipped = testcase.get("status", "").lower() == "notrun" or any(
            child.tag.rsplit("}", 1)[-1] == "skipped" for child in testcase
        )
        if skipped:
            not_run.append(name)
        else:
            names.append(name)

    if not_run:
        raise source_fixtures.FixtureError(
            "CTest execution report contains non-executed tests: "
            + ", ".join(sorted(not_run))
        )
    duplicates = sorted(name for name in set(names) if names.count(name) > 1)
    if duplicates:
        raise source_fixtures.FixtureError(
            "CTest execution report contains duplicate tests: "
            + ", ".join(duplicates)
        )
    return tuple(names)
