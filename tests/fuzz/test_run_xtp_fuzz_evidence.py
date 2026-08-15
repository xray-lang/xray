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
        self.corpus = root / "corpus"
        self.corpus.mkdir()
        (self.corpus / "valid.xtpseed").write_bytes(b"V\n")
        for path in (self.runtime, self.fuzzer, self.resource):
            path.write_text("fixture", encoding="utf-8")

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def args(self, *extra: str) -> list[str]:
        return [
            "--runtime", str(self.runtime),
            "--fuzzer", str(self.fuzzer),
            "--resource-stress", str(self.resource),
            "--corpus", str(self.corpus),
            "--expected-commit", COMMIT,
            "--sanitizer", "release",
            *extra,
        ]

    def completed(self, output: str) -> subprocess.CompletedProcess[str]:
        return subprocess.CompletedProcess([], 0, output, "")

    def resource_output(self, wall_ms: float = 4.0,
                        peak_bytes: int = 8192) -> str:
        return (
            "XTP resource ladder: blocks=1 wall-ms=1.0 peak-bytes=1024\n"
            "XTP resource ladder: blocks=2 wall-ms=2.0 peak-bytes=2048\n"
            "XTP resource ladder: blocks=3 wall-ms=3.0 peak-bytes=4096\n"
            f"XTP resource ladder: blocks=4 wall-ms={wall_ms} "
            f"peak-bytes={peak_bytes}\n"
            "XTP resource stress tests passed\n"
        )

    def test_accepts_complete_evidence(self) -> None:
        outputs = iter((
            self.completed('{"schema":1,"commit":"' + COMMIT + '","dirty":false}'),
            self.completed("typed XTP deterministic mutation matrix passed: executed=26 mutations=26 sanitizer=release\n"),
            self.completed(self.resource_output()),
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

    def test_rejects_missing_runtime(self) -> None:
        self.runtime.unlink()
        self.assertEqual(evidence.main(self.args()), 1)

    def test_rejects_missing_resource_stress(self) -> None:
        self.resource.unlink()
        self.assertEqual(evidence.main(self.args()), 1)

    def test_rejects_empty_corpus(self) -> None:
        (self.corpus / "valid.xtpseed").unlink()
        self.assertEqual(evidence.main(self.args()), 1)

    def test_rejects_empty_corpus_input(self) -> None:
        (self.corpus / "valid.xtpseed").write_bytes(b"")
        self.assertEqual(evidence.main(self.args()), 1)

    def test_rejects_sanitizer_identity_mismatch(self) -> None:
        outputs = iter((
            self.completed('{"schema":1,"commit":"' + COMMIT + '","dirty":false}'),
            self.completed("typed XTP deterministic mutation matrix passed: executed=26 mutations=26 sanitizer=asan-ubsan\n"),
        ))
        with mock.patch.object(evidence, "run_command", side_effect=lambda *_: next(outputs)):
            self.assertEqual(evidence.main(self.args()), 1)

    def test_rejects_resource_without_metrics(self) -> None:
        outputs = iter((
            self.completed('{"schema":1,"commit":"' + COMMIT + '","dirty":false}'),
            self.completed("typed XTP deterministic mutation matrix passed: executed=26 mutations=26 sanitizer=release\n"),
            self.completed("XTP resource ladder: 1\n" * 4 + "XTP resource stress tests passed\n"),
        ))
        with mock.patch.object(evidence, "run_command", side_effect=lambda *_: next(outputs)):
            self.assertEqual(evidence.main(self.args()), 1)

    def test_rejects_resource_wall_over_budget(self) -> None:
        outputs = iter((
            self.completed('{"schema":1,"commit":"' + COMMIT + '","dirty":false}'),
            self.completed("typed XTP deterministic mutation matrix passed: executed=26 mutations=26 sanitizer=release\n"),
            self.completed(self.resource_output(wall_ms=5.0)),
        ))
        with mock.patch.object(evidence, "run_command", side_effect=lambda *_: next(outputs)):
            self.assertEqual(
                evidence.main(self.args("--max-resource-wall-ms", "4.5")), 1
            )

    def test_rejects_resource_rss_over_budget(self) -> None:
        outputs = iter((
            self.completed('{"schema":1,"commit":"' + COMMIT + '","dirty":false}'),
            self.completed("typed XTP deterministic mutation matrix passed: executed=26 mutations=26 sanitizer=release\n"),
            self.completed(self.resource_output(peak_bytes=9000)),
        ))
        with mock.patch.object(evidence, "run_command", side_effect=lambda *_: next(outputs)):
            self.assertEqual(
                evidence.main(self.args("--max-resource-rss-bytes", "8192")), 1
            )

    def test_rejects_windows_tsan_without_skipping(self) -> None:
        args = self.args("--sanitizer", "tsan", "--host-os", "windows")
        with mock.patch.object(evidence, "run_command") as run_command:
            self.assertEqual(evidence.main(args), 1)
        run_command.assert_not_called()


if __name__ == "__main__":
    unittest.main()
