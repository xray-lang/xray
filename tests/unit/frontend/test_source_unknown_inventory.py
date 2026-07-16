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
    classify_line,
    enforce_category_maxima,
    parse_category_max,
    scan_file,
)


class SourceUnknownInventoryTest(unittest.TestCase):
    def test_task_payload_registration_matches_typed_native_defs(self) -> None:
        analyzer_source = (ROOT / "src/frontend/analyzer/xanalyzer.c").read_text()
        native_defs = (ROOT / "src/frontend/analyzer/xnative_type_defs.inc.c").read_text()

        self.assertIn("Failed(PanicInfo)", native_defs)
        self.assertIn(
            'XrType *task_failed_payload[] = {xr_type_new_named_instance(analyzer->isolate, "PanicInfo")};',
            analyzer_source,
        )
        unknown_ctor = "xr_type_new_" "unknown"
        self.assertNotIn(f"task_failed_payload[] = {{{unknown_ctor}", analyzer_source)

    def test_task_outcome_payload_registration_matches_typed_native_defs(self) -> None:
        analyzer_source = (ROOT / "src/frontend/analyzer/xanalyzer.c").read_text()
        native_defs = (ROOT / "src/frontend/analyzer/xnative_type_defs.inc.c").read_text()

        self.assertIn("Success(PanicInfo)", native_defs)
        self.assertIn("Failed(PanicInfo)", native_defs)
        self.assertIn("task_outcome_success_payload[] = {", analyzer_source)
        self.assertIn("task_outcome_failed_payload[] = {", analyzer_source)
        self.assertIn('xr_type_new_named_instance(analyzer->isolate, "PanicInfo")', analyzer_source)
        unknown_ctor = "xr_type_new_" "unknown"
        self.assertNotIn(f"task_outcome_success_payload[] = {{{unknown_ctor}", analyzer_source)
        self.assertNotIn(f"task_outcome_failed_payload[] = {{{unknown_ctor}", analyzer_source)

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

    def test_task_outcome_payload_surface_is_not_json_unknown_api(self) -> None:
        task_outcome = "Task" "Outcome"
        for line in ("    Success(PanicInfo)", "    Failed(PanicInfo)"):
            categories = classify_line("stdlib/types/coroutine.xr", line)
            self.assertNotIn("STDLIB_DYNAMIC_UNKNOWN_API", categories)

        for line in (f"fn collect() -> Array<{task_outcome}>",):
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
