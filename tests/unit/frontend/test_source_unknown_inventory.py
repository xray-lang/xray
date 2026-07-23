#!/usr/bin/env python3
"""Focused coverage for task-202 source-unknown inventory classification."""

from __future__ import annotations

import argparse
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))

from check_source_unknown_convergence import (  # noqa: E402
    CATEGORIES,
    build_inventory,
    classify_line,
    enforce_category_maxima,
    parse_category_max,
    scan_file,
)


class SourceUnknownInventoryTest(unittest.TestCase):
    def test_removed_source_unknown_negative_fixtures_are_not_live_surface(self) -> None:
        for path, line in (
            (
                "tests/compile_errors/type/source_unknown_cast_removed.xr",
                "var value = 1 as unknown",
            ),
            (
                "tests/compile_errors/type/source_unknown_member_removed.xr",
                "var erased: unknown = null",
            ),
            (
                "tests/compile_errors/type/source_unknown_param_removed.xr",
                "fn accept(value: unknown) {",
            ),
            (
                "tests/compile_errors/type/span_as_unknown_escape.xr",
                "var erased: unknown = null",
            ),
            (
                "tests/compile_errors/type/span_unknown_return_escape.xr",
                "var values = Array<unknown>()",
            ),
        ):
            categories = classify_line(path, line)
            self.assertIn("ALLOWED_REMOVED_SOURCE_UNKNOWN_NEGATIVE_TEST", categories)
            self.assertNotIn("SOURCE_UNKNOWN_TYPE_SURFACE", categories)

    def test_removed_source_unknown_diagnostic_is_not_live_surface(self) -> None:
        diagnostic = "'unk" "nown' type has been removed"
        categories = classify_line(
            "tests/compile_errors/type/source_unknown_param_removed.xr.expected",
            diagnostic,
        )
        self.assertIn("REMOVED_SOURCE_UNKNOWN_DIAGNOSTIC", categories)
        self.assertNotIn("SOURCE_UNKNOWN_TYPE_SURFACE", categories)

    def test_unknown_identifier_guard_is_not_live_source_type_surface(self) -> None:
        categories = classify_line(
            "tests/regression/02_variables/0210_unknown_identifier.xr",
            "assert_eq(unk" "nown, 7)",
        )
        self.assertIn("UNKNOWN_IDENTIFIER_ALLOWED_GUARD", categories)
        self.assertNotIn("SOURCE_UNKNOWN_TYPE_SURFACE", categories)

    def test_runtime_unknown_output_fixture_is_not_live_source_type_surface(self) -> None:
        categories = classify_line(
            "tests/regression/11_coroutine/1123_channel_timeout.xr.expected",
            "result: 42 ok: <unk" "nown>",
        )
        self.assertIn("ALLOWED_RUNTIME_UNKNOWN_OUTPUT_FIXTURE", categories)
        self.assertNotIn("SOURCE_UNKNOWN_TYPE_SURFACE", categories)

    def test_unknown_attribute_diagnostic_is_not_a_source_type(self) -> None:
        categories = classify_line(
            "tests/compile_errors/ffi/016_extern_attribute_removed.xr.expected",
            "error: unknown attribute name",
        )
        self.assertNotIn("SOURCE_UNKNOWN_TYPE_SURFACE", categories)

    def test_current_inventory_has_only_task_failed_unknown_boundary(self) -> None:
        inventory = build_inventory(ROOT)
        hits = inventory["SOURCE_UNKNOWN_TYPE_SURFACE"]
        self.assertEqual(1, len(hits))
        self.assertEqual("stdlib/types/coroutine.xr", hits[0].path)
        self.assertEqual("Failed(" "unknown)", hits[0].text)

    def test_task_payload_registration_matches_typed_native_defs(self) -> None:
        analyzer_source = (ROOT / "src/frontend/analyzer/xanalyzer.c").read_text()
        native_defs = (ROOT / "src/frontend/analyzer/xnative_type_defs.inc.c").read_text()

        self.assertIn("Failed(" "unknown)", native_defs)
        self.assertIn(
            "XrType *task_failed_payload[] = {xr_type_new_unknown(analyzer->isolate)};",
            analyzer_source,
        )
        self.assertNotIn(
            'task_failed_payload[] = {xr_type_new_named_instance(analyzer->isolate, "PanicInfo")}',
            analyzer_source,
        )

    def test_task_outcome_public_surface_is_removed(self) -> None:
        task_outcome = "Task" "Outcome"
        task_outcome_global = "XR_GLOBAL_VAR_TASK_" "OUTCOME"
        analyzer_source = (ROOT / "src/frontend/analyzer/xanalyzer.c").read_text()
        native_defs = (ROOT / "src/frontend/analyzer/xnative_type_defs.inc.c").read_text()
        prelude_source = (ROOT / "stdlib/prelude/prelude.c").read_text()
        coroutine_defs = (ROOT / "stdlib/types/coroutine.xr").read_text()
        api_inventory = (ROOT / "scripts/gen_api_inventory.py").read_text()

        self.assertNotIn(task_outcome, analyzer_source)
        self.assertNotIn(task_outcome, native_defs)
        self.assertNotIn(task_outcome, prelude_source)
        self.assertNotIn(task_outcome, coroutine_defs)
        self.assertNotIn(task_outcome, api_inventory)
        self.assertNotIn(task_outcome_global, analyzer_source)

    def test_typed_task_result_is_not_erased_result_residue(self) -> None:
        task_result = "Task" "Result"
        for line in (
            f"enum {task_result}<T> {{",
            f"    poll() -> {task_result}<T>",
            f"    {task_result}.Success(value) -> value",
            f"    {task_result}.Pending -> false",
            f'XrEnumType *task_result_et = make_prelude_enum(X, "{task_result}", members, 5,',
        ):
            categories = classify_line("stdlib/types/coroutine.xr", line)
            self.assertNotIn("TASK_ERASED_RESULT_RESIDUE", categories)
            self.assertNotIn("STDLIB_DYNAMIC_UNKNOWN_API", categories)

    def test_removed_task_outcome_surface_still_counts_as_erasure_residue(self) -> None:
        task_outcome = "Task" "Outcome"
        for line in (f"enum {task_outcome} {{", f"fn collect() -> Array<{task_outcome}>"):
            categories = classify_line("stdlib/types/coroutine.xr", line)
            self.assertIn("TASK_ERASED_RESULT_RESIDUE", categories)
            self.assertNotIn("STDLIB_DYNAMIC_UNKNOWN_API", categories)

    def test_legacy_failed_unknown_still_counts_as_stdlib_dynamic_unknown_api(self) -> None:
        failed_unknown = "Failed(" "unknown)"
        categories = classify_line("stdlib/types/coroutine.xr", f"    {failed_unknown}")
        self.assertIn("TASK_ERASED_RESULT_RESIDUE", categories)
        self.assertIn("STDLIB_DYNAMIC_UNKNOWN_API", categories)

    def test_task_result_unknown_payload_doc_still_counts_as_erasure_residue(self) -> None:
        task_result = "Task" "Result"
        categories = classify_line(
            "LANGUAGE_SPEC.md",
            f"{task_result}.Failed(error) preserves the original failure value as unknown.",
        )
        self.assertIn("TASK_ERASED_RESULT_RESIDUE", categories)

    def test_sibling_inventory_regexes_are_not_task_erasure_residue(self) -> None:
        hits = scan_file(ROOT, ROOT / "scripts/check_error_effect_convergence.py")
        self.assertEqual([], hits)

    def test_inventory_readme_is_not_counted_as_source_residue(self) -> None:
        hits = scan_file(ROOT, ROOT / "scripts/README.md")
        self.assertEqual([], hits)

    def test_category_maxima_accepts_current_or_lower_residue(self) -> None:
        inventory = {category: [] for category in CATEGORIES}
        inventory["TASK_ERASED_RESULT_RESIDUE"] = [object(), object()]

        self.assertEqual(
            [],
            enforce_category_maxima(inventory, [("TASK_ERASED_RESULT_RESIDUE", 2)]),
        )

        errors = enforce_category_maxima(inventory, [("TASK_ERASED_RESULT_RESIDUE", 1)])
        self.assertEqual(1, len(errors))
        self.assertIn("TASK_ERASED_RESULT_RESIDUE: 2 exceeds max 1", errors[0])

    def test_parse_category_max_rejects_unknown_categories(self) -> None:
        self.assertEqual(
            ("TASK_ERASED_RESULT_RESIDUE", 52),
            parse_category_max("TASK_ERASED_RESULT_RESIDUE=52"),
        )
        with self.assertRaises(argparse.ArgumentTypeError):
            parse_category_max("TASK_ERASED_RESULT_RESIDUE=-1")
        with self.assertRaises(argparse.ArgumentTypeError):
            parse_category_max("NOT_A_CATEGORY=1")


if __name__ == "__main__":
    unittest.main()
