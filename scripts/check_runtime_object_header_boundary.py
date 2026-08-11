#!/usr/bin/env python3
"""Reject legacy object-header dependencies in the canonical ABI leaf."""

from __future__ import annotations

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
FILES = (
    "src/runtime/abi/xr_runtime_object_header.h",
    "src/runtime/abi/xr_runtime_object_header.c",
    "tests/unit/runtime/test_runtime_object_header.c",
)
FORBIDDEN = (
    "xr_obj_header.h",
    "XrObjHeader",
    "XrObjType",
    "XR_OBJ_",
)


def main() -> int:
    errors: list[str] = []
    for relative in FILES:
        path = ROOT / relative
        text = path.read_text(encoding="utf-8")
        for token in FORBIDDEN:
            if token in text:
                errors.append(f"{relative}: forbidden token {token!r}")
    if errors:
        print("canonical object-header boundary failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    print("canonical object-header boundary passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
