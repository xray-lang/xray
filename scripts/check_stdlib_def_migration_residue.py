#!/usr/bin/env python3
"""Keep migrated pure-Xray stdlib modules out of stdlib/defs/*.def."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

from stdlib_manifest import load_manifest

MODULE_RE = re.compile(r"\s*module\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{\s*")


def strip_comment(line: str) -> str:
    return line.split("//", 1)[0].strip()


def check_def_files(root: Path, migrated_modules: tuple[str, ...] | None = None) -> list[str]:
    defs_dir = root / "stdlib" / "defs"
    errors: list[str] = []
    migrated = set(migrated_modules or load_manifest(root).def_migrated_modules)
    for path in sorted(defs_dir.glob("*.def")):
        for lineno, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            line = strip_comment(raw_line)
            if not line:
                continue
            match = MODULE_RE.fullmatch(line)
            if match and match.group(1) in migrated:
                module = match.group(1)
                errors.append(
                    f"{path.relative_to(root)}:{lineno}: migrated module {module!r} "
                    "must not be declared in stdlib/defs/*.def"
                )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    migrated_modules = load_manifest(root).def_migrated_modules
    errors = check_def_files(root, migrated_modules)
    if errors:
        print("stdlib .def migration residue gate failed:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        modules = ", ".join(migrated_modules)
        print(
            "These modules use their stdlib/<module>/<module>.xr exports as the "
            f"declaration source: {modules}.",
            file=sys.stderr,
        )
        return 1

    print("OK: migrated pure-Xray stdlib modules are absent from stdlib/defs/*.def")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
