#!/usr/bin/env python3
"""Unit tests for the fail-closed XTP fuzz evidence runner."""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import run_xtp_fuzz_evidence as evidence


COMMIT = "a" * 40


class XtpFuzzEvidenceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.runtime = root / "runtime"
        self.fuzzer = root / "fuzzer"
        self.resource = root / "resource"
        for path in (self.runtime, self.fuzzer, self.resource):
            path.write_text("fixture", encoding="utf-8")

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def args(self, *extra: str) -> list[str]:
        return [
            "--runtime", str(self.runtime),
            "--fuzzer", str(self.fuzzer),
            "--resource-stress", str(self.resource),
            "--expected-commit", COMMIT,
            "--sanitizer", "release",
            *extra,
        ]

    def completed(self, output: str) -> subprocess.CompletedProcess[str]:
        return subprocess.CompletedProcess([], 0, output, "")

    def test_accepts_complete_evidence(self) -> None:
        outputs = iter((
            self.completed('{"schema":1,"commit":"' + COMMIT + '","dirty":false}'),
            self.completed("typed XTP deterministic mutation matrix passed: executed=26 mutations=26 sanitizer=release\n"),
            self.completed("XTP resource ladder: 1\nXTP resource ladder: 2\nXTP resource ladder: 3\nXTP resource ladder: 4\nXTP resource stress tests passed\n"),
        ))
        with mock.patch.object(evidence, "run_command", side_effect=lambda *_: next(outputs)):
            self.assertEqual(evidence.main(self.args()), 0)

    def test_rejects_identity_mismatch(self) -> None:
        runtime = self.completed('{"schema":1,"commit":"' + "b" * 40 + '","dirty":false}')
        with mock.patch.object(evidence, "run_command", return_value=runtime):
            self.assertEqual(evidence.main(self.args()), 1)

    def test_rejects_zero_executed_mutations(self) -> None:
        outputs = iter((
            self.completed('{"schema":1,"commit":"' + COMMIT + '","dirty":false}'),
            self.completed("typed XTP deterministic mutation matrix passed: executed=0 mutations=26 sanitizer=release\n"),
        ))
        with mock.patch.object(evidence, "run_command", side_effect=lambda *_: next(outputs)):
            self.assertEqual(evidence.main(self.args()), 1)

    def test_rejects_missing_fuzzer(self) -> None:
        self.fuzzer.unlink()
        self.assertEqual(evidence.main(self.args()), 1)

    def test_rejects_windows_tsan_without_skipping(self) -> None:
        args = self.args("--sanitizer", "tsan", "--host-os", "windows")
        with mock.patch.object(evidence, "run_command") as run_command:
            self.assertEqual(evidence.main(args), 1)
        run_command.assert_not_called()


if __name__ == "__main__":
    unittest.main()
