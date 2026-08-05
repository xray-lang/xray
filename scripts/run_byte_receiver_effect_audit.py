#!/usr/bin/env python3
"""Byte receiver effects, audited against the byte storage model.

Two halves, both required:

  - positive cases must run clean, proving the permitted receiver effects work
  - negative cases must be REJECTED, and rejected with the diagnostics their
    `.expected` sibling names -- a case that fails for an unrelated reason
    would otherwise score as a pass and hide the rule going missing

Fails on the first bad step, matching the shell version: this is an audit, and
a broken storage model makes every later result untrustworthy anyway.

Environment: XRAY_BIN must point at the xray executable.

Usage: run_byte_receiver_effect_audit.py
"""

from __future__ import annotations

import os
import sys
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parent.parent / "tests" / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import diagnostics, platform, proc  # noqa: E402

PROJECT_ROOT = Path(__file__).resolve().parent.parent
LABEL = "[byte-receiver-effect]"

POSITIVE_TESTS: tuple[str, ...] = (
    "tests/regression/14_typed_array/1416_byte_receiver_effect_matrix.xr",
    "tests/regression/14_typed_array/1410_shared_provenance_rebind_reset.xr",
    "tests/regression/14_typed_array/1411_shared_provenance_readonly_param.xr",
    "tests/regression/14_typed_array/1412_shared_provenance_readonly_function_value_param.xr",
    "tests/regression/14_typed_array/1413_shared_provenance_imported_readonly_function_value_param.xr",
    "tests/regression/14_typed_array/1414_shared_provenance_reexported_readonly_function_value_param.xr",
    "tests/regression/14_typed_array/1415_shared_provenance_returned_readonly_function_value_param.xr",
    "tests/regression/09_advanced/0916_owned_binding.xr",
)

NEGATIVE_TESTS: tuple[str, ...] = (
    "tests/compile_errors/type/byte_array_append_from_rejects_in.xr",
    "tests/compile_errors/type/byte_slice_repeat_from_rejects_in.xr",
    "tests/compile_errors/type/byte_method_union_matrix.xr",
    "tests/compile_errors/type/byte_method_generic_union_matrix.xr",
    "tests/compile_errors/type/const_byte_slice_mutating_method.xr",
    "tests/compile_errors/type/const_byte_slice_index_store.xr",
    "tests/compile_errors/type/shared_byte_array_mutating_method.xr",
    "tests/compile_errors/type/103_shared_derived_slice_index_store_rejected.xr",
    "tests/compile_errors/type/104_shared_derived_slice_mutating_method_rejected.xr",
    "tests/compile_errors/type/107_shared_derived_slice_mutating_param_rejected.xr",
    "tests/compile_errors/type/108_shared_derived_slice_transitive_mutating_param_rejected.xr",
    "tests/compile_errors/type/110_shared_derived_function_value_mutating_param_rejected.xr",
    "tests/compile_errors/type/111_shared_derived_dynamic_function_value_mutating_param_rejected.xr",
    "tests/compile_errors/type/112_shared_derived_unknown_function_value_param_rejected.xr",
    "tests/compile_errors/type/113_shared_derived_imported_function_value_mutating_param_rejected.xr",
    "tests/compile_errors/type/114_shared_derived_namespace_function_value_mutating_param_rejected.xr",
    "tests/compile_errors/type/115_shared_derived_reexported_function_value_mutating_param_rejected.xr",
    "tests/compile_errors/type/116_shared_derived_star_reexported_function_value_mutating_param_rejected.xr",
    "tests/compile_errors/type/117_shared_derived_returned_function_value_mutating_param_rejected.xr",
    "tests/compile_errors/ownership/078_move_owner_active_span_borrow_rejected.xr",
    "tests/compile_errors/ownership/079_freeze_owner_active_span_borrow_rejected.xr",
)


def typepath_for(case: Path) -> str:
    """The case's own directory, ahead of any inherited XRAY_TYPEPATH."""
    inherited = os.environ.get("XRAY_TYPEPATH", "")
    return f"{case.parent}{os.pathsep}{inherited}" if inherited else str(case.parent)


def run_positive(xray: Path, rel: str, timeout: float | None) -> bool:
    print(f"{LABEL} positive {rel}")
    return proc.run_passthrough([xray, "test", PROJECT_ROOT / rel],
                                timeout=timeout) == 0


def run_negative(xray: Path, rel: str, timeout: float | None) -> bool:
    print(f"{LABEL} negative {rel}")
    case = PROJECT_ROOT / rel
    expected_file = Path(str(case) + ".expected")
    if not case.is_file():
        sys.stderr.write(f"missing compile-error case: {rel}\n")
        return False
    if not expected_file.is_file():
        sys.stderr.write(f"missing expected diagnostics for: {rel}\n")
        return False

    env = dict(os.environ)
    env["XRAY_TYPEPATH"] = typepath_for(case)
    result = proc.run([xray, case], env=env, timeout=timeout)
    if result.ok:
        sys.stderr.write(f"expected compile error but program compiled: {rel}\n")
        return False

    output = result.combined_text()
    missing = diagnostics.missing_lines(
        output, diagnostics.expected_lines(expected_file))
    if missing:
        sys.stderr.write(f"diagnostic mismatch for {rel}\n")
        sys.stderr.write(f"missing: {missing[0]}\n")
        sys.stderr.write("output:\n")
        sys.stderr.write(output.rstrip("\n") + "\n")
        return False
    return True


def main(argv: list[str]) -> int:
    xray = os.environ.get("XRAY_BIN")
    if not xray:
        sys.stderr.write("XRAY_BIN must point to the xray executable\n")
        return 1
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300)

    for rel in POSITIVE_TESTS:
        if not run_positive(Path(xray), rel, timeout):
            return 1
    for rel in NEGATIVE_TESTS:
        if not run_negative(Path(xray), rel, timeout):
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
