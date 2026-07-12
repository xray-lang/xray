#!/usr/bin/env python3
"""Keep migrated stdlib modules from growing a second AOT helper surface."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

from stdlib_manifest import load_manifest

STATIC_FORBIDDEN_TOKENS = (
    (re.compile(r"\bXR_TAG_DATETIME\b"), "DateTime light-object tag"),
    (re.compile(r"\bxrt_datetime_"), "DateTime AOT helper prefix"),
    (re.compile(r"\bxrt_path_"), "path AOT helper prefix"),
    (re.compile(r"\bxrt_url_"), "url AOT helper prefix"),
    (re.compile(r"\bxrt_base64_"), "base64 AOT helper prefix"),
    (re.compile(r"\bxrt_encoding_"), "encoding AOT helper prefix"),
    (re.compile(r"\bxrt_csv_"), "csv AOT helper prefix"),
    (re.compile(r"\bxrt_toml_"), "toml AOT helper prefix"),
    (re.compile(r"\bxrt_xml_"), "xml AOT helper prefix"),
    (re.compile(r"\bxrt_yaml_"), "yaml AOT helper prefix"),
    (re.compile(r"\bxrt_log_"), "log AOT helper prefix"),
)


def check_resurrected_files(root: Path, migrated_modules: tuple[str, ...]) -> list[str]:
    aot_dir = root / "src" / "aot"
    errors: list[str] = []
    for module in migrated_modules:
        for path in sorted(aot_dir.glob(f"xrt_{module}*")):
            errors.append(f"{path.relative_to(root)}: migrated module AOT helper file must not exist")
    return errors


def check_forbidden_tokens(root: Path, migrated_modules: tuple[str, ...]) -> list[str]:
    aot_dir = root / "src" / "aot"
    errors: list[str] = []
    for path in sorted(aot_dir.rglob("*")):
        if not path.is_file() or path.suffix not in {".c", ".h"}:
            continue
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except UnicodeDecodeError:
            continue
        for lineno, line in enumerate(lines, 1):
            tokens = list(STATIC_FORBIDDEN_TOKENS)
            tokens.extend(
                (re.compile(rf"\bxrt_{re.escape(module)}_"), f"{module} AOT helper prefix")
                for module in migrated_modules
            )
            for pattern, label in tokens:
                if pattern.search(line):
                    errors.append(f"{path.relative_to(root)}:{lineno}: {label} is forbidden")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    migrated_modules = load_manifest(root).aot_helper_forbidden_modules
    errors = check_resurrected_files(root, migrated_modules) + check_forbidden_tokens(
        root, migrated_modules
    )
    if errors:
        print("stdlib AOT helper residue gate failed:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        print(
            "Migrated L4/L5 modules must use their .xr source as the semantic source; "
            "do not restore module-specific xrt_* helpers.",
            file=sys.stderr,
        )
        return 1

    print("OK: migrated stdlib modules have no module-specific AOT helper residue")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
