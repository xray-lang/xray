#!/usr/bin/env python3
"""Verify the checked-in Unicode grapheme inputs and generated tables."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    command = [
        sys.executable,
        str(ROOT / "scripts" / "gen_unicode_grapheme_tables.py"),
        "--unicode-dir",
        str(ROOT / "third_party" / "unicode" / "17.0.0"),
        "--output-dir",
        str(ROOT / "src" / "shared"),
        "--check",
    ]
    return subprocess.run(command, cwd=ROOT, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
