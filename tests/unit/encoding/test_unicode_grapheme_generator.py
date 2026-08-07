#!/usr/bin/env python3
"""Failure-mode tests for the deterministic Unicode grapheme generator."""

from __future__ import annotations

import importlib.util
import shutil
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
GENERATOR_PATH = ROOT / "scripts" / "gen_unicode_grapheme_tables.py"
SPEC = importlib.util.spec_from_file_location("gen_unicode_grapheme_tables", GENERATOR_PATH)
assert SPEC and SPEC.loader
GENERATOR = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = GENERATOR
SPEC.loader.exec_module(GENERATOR)


class UnicodeGraphemeGeneratorTests(unittest.TestCase):
    def test_checked_in_outputs_are_current(self) -> None:
        files = GENERATOR.generated_files(
            ROOT / "third_party" / "unicode" / "17.0.0", ROOT / "src" / "shared"
        )
        GENERATOR.write_or_check(files, check=True)

    def test_unknown_gcb_value_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "GraphemeBreakProperty.txt"
            path.write_text("0041 ; Not_A_GCB\n", encoding="utf-8")
            with self.assertRaises(GENERATOR.GenerationError):
                GENERATOR.parse_gcb(path)

    def test_overlapping_gcb_range_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "GraphemeBreakProperty.txt"
            path.write_text("0041..0042 ; Extend\n0042 ; ZWJ\n", encoding="utf-8")
            with self.assertRaises(GENERATOR.GenerationError):
                GENERATOR.parse_gcb(path)

    def test_unknown_emoji_property_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "emoji-data.txt"
            path.write_text("1F600 ; Approximate_Emoji\n", encoding="utf-8")
            with self.assertRaises(GENERATOR.GenerationError):
                GENERATOR.parse_extended_pictographic(path)

    def test_version_mismatch_is_rejected_after_valid_checksum(self) -> None:
        source = ROOT / "third_party" / "unicode" / "17.0.0"
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp)
            for name in GENERATOR.REQUIRED_INPUTS:
                shutil.copyfile(source / name, target / name)
            gcb_path = target / "GraphemeBreakProperty.txt"
            gcb_path.write_text(
                gcb_path.read_text(encoding="utf-8").replace(
                    "GraphemeBreakProperty-17.0.0.txt",
                    "GraphemeBreakProperty-16.0.0.txt",
                    1,
                ),
                encoding="utf-8",
            )
            entries = []
            for name in GENERATOR.REQUIRED_INPUTS:
                digest = GENERATOR.sha256(target / name)
                entries.append(f"{digest}  {name}\n")
            (target / "SHA256SUMS").write_text("".join(entries), encoding="utf-8")
            with self.assertRaises(GENERATOR.GenerationError):
                GENERATOR.verify_inputs(target)


if __name__ == "__main__":
    unittest.main()
