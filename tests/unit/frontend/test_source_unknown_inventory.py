#!/usr/bin/env python3
"""Focused coverage for task-202 source-unknown inventory classification."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))

from check_source_unknown_convergence import classify_line  # noqa: E402


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

    def test_typed_task_result_is_not_stdlib_dynamic_unknown_api(self) -> None:
        task_result = "Task" "Result"
        for line in (
            f"enum {task_result}<T> {{",
            f"    poll() -> {task_result}<T>",
            f'XrEnumType *task_result_et = make_prelude_enum(X, "{task_result}", members, 5,',
        ):
            self.assertNotIn(
                "STDLIB_DYNAMIC_UNKNOWN_API",
                classify_line("stdlib/types/coroutine.xr", line),
            )
            self.assertIn(
                "TASK_ERASED_RESULT_RESIDUE",
                classify_line("stdlib/types/coroutine.xr", line),
            )

    def test_task_outcome_payload_surface_is_not_json_unknown_api(self) -> None:
        task_outcome = "Task" "Outcome"
        for line in ("    Success(PanicInfo)", "    Failed(PanicInfo)"):
            categories = classify_line("stdlib/types/coroutine.xr", line)
            self.assertNotIn("STDLIB_DYNAMIC_UNKNOWN_API", categories)

        for line in (f"fn collect() -> Array<{task_outcome}>",):
            categories = classify_line("stdlib/types/coroutine.xr", line)
            self.assertIn("STDLIB_DYNAMIC_UNKNOWN_API", categories)


if __name__ == "__main__":
    unittest.main()
