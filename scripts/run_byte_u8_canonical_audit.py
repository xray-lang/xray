#!/usr/bin/env python3
"""`byte` and `u8` are one canonical type, checked at all three layers.

The identity has to hold in the language, in what the LSP shows the user, and
in the global evidence cache's type keys. Checking only the language would let
the other two drift into presenting `byte` and `u8` as distinct types, which is
exactly the confusion the canonicalisation exists to prevent.

Environment:
    XRAY_BIN                  the xray executable
    XRAY_TEST_LSP_DOCUMENT    the test_lsp_document binary
    XRAY_TEST_XGLOBAL_SUMMARY the test_xglobal_summary binary

Usage: run_byte_u8_canonical_audit.py
"""

from __future__ import annotations

import os
import sys
from pathlib import Path
from typing import Sequence


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parent.parent / "tests" / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc  # noqa: E402

PROJECT_ROOT = Path(__file__).resolve().parent.parent
LABEL = "[byte-u8-canonical]"

REGRESSION_CASE = (PROJECT_ROOT / "tests" / "regression" / "14_typed_array"
                   / "1409_byte_u8_canonical_identity.xr")


def require_env(name: str) -> str | None:
    value = os.environ.get(name)
    if not value:
        sys.stderr.write(f"{name} must point to the executable\n")
        return None
    return value


def run_step(label: str, argv: Sequence, timeout: float | None) -> bool:
    print(f"{LABEL} {label}")
    return proc.run_passthrough(argv, timeout=timeout) == 0


def main(argv: list[str]) -> int:
    xray = require_env("XRAY_BIN")
    lsp_document = require_env("XRAY_TEST_LSP_DOCUMENT")
    xglobal_summary = require_env("XRAY_TEST_XGLOBAL_SUMMARY")
    if not (xray and lsp_document and xglobal_summary):
        return 1
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300)

    steps = (
        ("language identity regression", [xray, "test", REGRESSION_CASE]),
        ("LSP canonical byte display/docs", [lsp_document]),
        ("global evidence/cache canonical U8 type keys", [xglobal_summary]),
    )
    for label, command in steps:
        if not run_step(label, command, timeout):
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
