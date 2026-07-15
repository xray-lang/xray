#!/usr/bin/env python3
"""Keep migrated stdlib modules from growing a second AOT helper surface."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

from stdlib_manifest import load_manifest

STATIC_FORBIDDEN_TOKENS = ((re.compile(r"\bXR_TAG_DATETIME\b"), "DateTime light-object tag"),)


def check_resurrected_files(root: Path, migrated_modules: tuple[str, ...]) -> list[str]:
    aot_dir = root / "src" / "aot"
    errors: list[str] = []
    for module in migrated_modules:
        for path in sorted(aot_dir.glob(f"xrt_{module}*")):
            errors.append(f"{path.relative_to(root)}: migrated module AOT helper file must not exist")
    return errors


def check_forbidden_tokens(
    root: Path,
    migrated_modules: tuple[str, ...],
    allowed_helpers: dict[str, set[str]],
) -> list[str]:
    aot_dir = root / "src" / "aot"
    errors: list[str] = []
    seen_allowed: set[str] = set()
    module_patterns = {
        module: re.compile(rf"\b(xrt_{re.escape(module)}_[A-Za-z0-9_]*)\b")
        for module in migrated_modules
    }
    for path in sorted(aot_dir.rglob("*")):
        if not path.is_file() or path.suffix not in {".c", ".h"}:
            continue
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except UnicodeDecodeError:
            continue
        for lineno, line in enumerate(lines, 1):
            for pattern, label in STATIC_FORBIDDEN_TOKENS:
                if pattern.search(line):
                    errors.append(f"{path.relative_to(root)}:{lineno}: {label} is forbidden")
            for module, pattern in module_patterns.items():
                unexpected: set[str] = set()
                for match in pattern.finditer(line):
                    symbol = match.group(1)
                    if symbol in allowed_helpers.get(module, set()):
                        seen_allowed.add(symbol)
                    else:
                        unexpected.add(symbol)
                for symbol in sorted(unexpected):
                    errors.append(
                        f"{path.relative_to(root)}:{lineno}: {module} AOT helper {symbol} is forbidden"
                    )
    declared_allowed = set().union(*allowed_helpers.values()) if allowed_helpers else set()
    for symbol in sorted(declared_allowed - seen_allowed):
        errors.append(f"stdlib boundary declares stale AOT runtime adapter: {symbol}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    manifest = load_manifest(root)
    migrated_modules = manifest.aot_helper_forbidden_modules
    allowed_helpers = {
        name: set(str(symbol) for symbol in module.get("aot_runtime_adapters", ()))
        for name, module in manifest.by_name.items()
        if module.get("aot_runtime_adapters")
    }
    errors = check_resurrected_files(root, migrated_modules) + check_forbidden_tokens(
        root, migrated_modules, allowed_helpers
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
