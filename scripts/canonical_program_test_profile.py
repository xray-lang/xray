"""Shared fail-closed test inventory for the canonical-program edit loop.

This is intentionally a named preflight, not a replacement for any broad test
tier or the full sanitizer lane.  Release and sanitizer runners import the same
inventory so a locally shortened regex cannot silently become the qualification
claim for Task 293.
"""

from __future__ import annotations

import re


CTEST_NAMES = (
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
    "test_xr_program_provider_trap_cleanup_aot_native",
    "test_xr_program_pipe_cancel_cleanup_aot_native",
    "test_xr_program_source_build",
    "test_xr_program_verify",
    "test_xr_program_vm",
    "test_xr_program_vm_runtime",
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
    "xr_program_vm_contracts",
    "xr_program_vm_contracts_self_test",
    "xr_program_wave3_closure",
    "xr_program_wave3_closure_self_test",
    "xr_program_wave4_contract",
    "xr_program_wave4_contract_self_test",
)

# Script-only gates have no Ninja target.  The ten targets below are the
# executable/native evidence required before the matching CTest inventory can
# run.  In particular, this does not build the CLI merely as a blanket proxy.
BUILD_TARGETS = (
    "test_core_spec",
    "test_xr_program",
    "test_xr_program_aot",
    "test_xr_program_aot_condition_assert",
    "test_xr_program_provider_trap_cleanup_aot_native",
    "test_xr_program_pipe_cancel_cleanup_aot_native",
    "test_xr_program_source_build",
    "test_xr_program_verify",
    "test_xr_program_vm",
    "test_xr_program_vm_runtime",
)


def ctest_regex() -> str:
    # CTest uses its C++/POSIX-style regex engine, which has no Python-style
    # non-capturing group syntax.
    return "^(" + "|".join(re.escape(name) for name in CTEST_NAMES) + ")$"


def listed_ctest_names(output: str) -> tuple[str, ...]:
    pattern = re.compile(r"^\s*Test\s+#[0-9]+:\s+(\S+)", re.MULTILINE)
    return tuple(pattern.findall(output))
