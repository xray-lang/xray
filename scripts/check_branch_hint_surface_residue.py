#!/usr/bin/env python3
"""Reject removed compiler-owned branch hints without reserving their names."""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from pathlib import Path


TOMBSTONE_HEADER = (
    "capability_id\tsource_forms\tsemantic_forms\tnegative_fixture\treplacement"
)
TOMBSTONE_ROW = (
    "source-branch-probability-hints\tlikely(bool)|unlikely(bool)\t"
    "XI_COPY_KIND_LIKELY|XI_COPY_KIND_UNLIKELY\t"
    "tests/compile_errors/type/branch_hint_builtins_removed.xr\t"
    "ordinary bool control expressions"
)
NEGATIVE_FIXTURE = Path("tests/compile_errors/type/branch_hint_builtins_removed.xr")
NEGATIVE_EXPECTED = Path(str(NEGATIVE_FIXTURE) + ".expected")
# The oracle is written in the compile-error suite's expected-file grammar
# (tests/compile_errors/expected_format.py): each `-->` line pins where the
# diagnostic points, so a rename that also moved the caret would be caught here
# rather than silently accepted.
BRANCH_HINT_ORACLE = (
    "--> 1:11 E0351\n"
    "Undeclared variable 'likely'\n"
    "\n"
    "--> 2:12 E0351\n"
    "Undeclared variable 'unlikely'\n"
)

SEMANTIC_RESIDUE = (
    "XI_COPY_KIND_LIKELY",
    "XI_COPY_KIND_UNLIKELY",
    "XR_COPY_SEMANTIC_BRANCH_LIKELY",
    "XR_COPY_SEMANTIC_BRANCH_UNLIKELY",
    "xi_copy_is_branch_hint",
    "carries_branch_hint",
    "emit_likely_condition_expr",
)
SEMANTIC_SCAN_ROOTS = ("src", "stdlib", "tests", "scripts")
SEMANTIC_SUFFIXES = (".c", ".h", ".inc.c", ".py", ".def", ".xr", ".expect")

PUBLIC_BINDING_PATTERNS = (
    (Path("src/frontend/analyzer/xanalyzer.c"),
     re.compile(r'register_builtin_func\([^;\n]*["\'](?:likely|unlikely)["\']')),
    (Path("src/ir/xi_lower_expr.c"),
     re.compile(r'strcmp\(\s*fname\s*,\s*["\'](?:likely|unlikely)["\']')),
    (Path("src/app/lsp/xlsp_keywords.c"),
     re.compile(r'["\'](?:likely|unlikely)["\']')),
    (Path("scripts/gen_api_inventory.py"),
     re.compile(r'["\'](?:likely|unlikely)["\']\s*:')),
)
RETIRED_BOUND_FIXTURES = (
    Path("tests/aot/filetests/cgen/branch_likely_hints.xr"),
    Path("tests/aot/filetests/cgen/branch_likely_hints.expect"),
)

INTERNAL_HEADERS = (
    Path("src/base/xdefs.h"),
    Path("src/aot/xrt_value.h"),
)
INTERNAL_MACROS = ("XR_LIKELY", "XR_UNLIKELY")


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="strict")


def _relative(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def _iter_files(root: Path, roots: tuple[str, ...], suffixes: tuple[str, ...]):
    for rel_root in roots:
        base = root / rel_root
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if not path.is_file() or not str(path).endswith(suffixes):
                continue
            if any(part in {"build", ".git", "__pycache__"} for part in path.parts):
                continue
            yield path


def _check_tombstone(root: Path, errors: list[str]) -> None:
    path = root / "contracts/capability-deletions.tsv"
    if not path.is_file():
        errors.append("missing capability-deletion tombstone inventory")
        return
    lines = [line.rstrip("\r\n") for line in _read(path).splitlines() if line.strip()]
    if not lines or lines[0] != TOMBSTONE_HEADER:
        errors.append("capability-deletion tombstone header drifted")
    if lines.count(TOMBSTONE_ROW) != 1:
        errors.append("source branch-hint deletion must have exactly one tombstone row")

    fixture = root / NEGATIVE_FIXTURE
    expected = root / NEGATIVE_EXPECTED
    if not fixture.is_file() or _read(fixture) != "var hot = likely(true)\nvar cold = unlikely(false)\n":
        errors.append("removed source branch-hint fixture is missing or drifted")
    if not expected.is_file():
        errors.append("removed source branch-hint diagnostic oracle is missing")
    elif _read(expected) != BRANCH_HINT_ORACLE:
        errors.append("removed source branch-hint diagnostic oracle drifted")


def _check_semantic_residue(root: Path, errors: list[str]) -> None:
    checker = Path(__file__).resolve()
    for path in _iter_files(root, SEMANTIC_SCAN_ROOTS, SEMANTIC_SUFFIXES):
        if path.resolve() == checker:
            continue
        text = _read(path)
        for token in SEMANTIC_RESIDUE:
            if token in text:
                errors.append(f"{_relative(path, root)}: removed semantic residue {token}")


def _check_public_bindings(root: Path, errors: list[str]) -> None:
    for rel, pattern in PUBLIC_BINDING_PATTERNS:
        path = root / rel
        if not path.is_file():
            errors.append(f"missing public-surface owner {rel.as_posix()}")
            continue
        for line_no, line in enumerate(_read(path).splitlines(), 1):
            if pattern.search(line):
                errors.append(
                    f"{rel.as_posix()}:{line_no}: removed compiler-owned branch-hint binding"
                )

    for rel in RETIRED_BOUND_FIXTURES:
        if (root / rel).exists():
            errors.append(f"{rel.as_posix()}: retired builtin-bound fixture still exists")


def _check_internal_macros(root: Path, errors: list[str]) -> int:
    count = 0
    for rel in INTERNAL_HEADERS:
        path = root / rel
        if not path.is_file():
            errors.append(f"missing internal branch-hint header {rel.as_posix()}")
            continue
        text = _read(path)
        for macro in INTERNAL_MACROS:
            if re.search(rf"^#define\s+{macro}\s*\(", text, re.MULTILINE) is None:
                errors.append(f"{rel.as_posix()}: internal {macro} definition is missing")
    for path in _iter_files(root, ("src", "stdlib"), (".c", ".h", ".inc.c")):
        text = _read(path)
        count += sum(text.count(macro) for macro in INTERNAL_MACROS)
    return count


def verify(root: Path) -> tuple[list[str], int]:
    errors: list[str] = []
    _check_tombstone(root, errors)
    _check_semantic_residue(root, errors)
    _check_public_bindings(root, errors)
    internal_count = _check_internal_macros(root, errors)
    return errors, internal_count


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def self_test() -> int:
    with tempfile.TemporaryDirectory(prefix="xray-branch-hint-residue-") as raw:
        root = Path(raw)
        _write(root / "contracts/capability-deletions.tsv", TOMBSTONE_HEADER + "\n" + TOMBSTONE_ROW + "\n")
        _write(root / NEGATIVE_FIXTURE, "var hot = likely(true)\nvar cold = unlikely(false)\n")
        _write(root / NEGATIVE_EXPECTED,
               BRANCH_HINT_ORACLE)
        for rel in INTERNAL_HEADERS:
            _write(root / rel, "#define XR_LIKELY(x) (x)\n#define XR_UNLIKELY(x) (x)\n")
        for rel, _ in PUBLIC_BINDING_PATTERNS:
            _write(root / rel, "canonical public surface\n")
        _write(root / "src/runtime.c", "if (XR_LIKELY(ready)) return;\n")
        _write(root / "tests/user_names.xr",
               "fn likely(x: bool) -> bool { return x }\n"
               "fn unlikely(x: bool) -> bool { return x }\n"
               "print(likely(true), unlikely(false))\n")

        errors, _ = verify(root)
        if errors:
            print("branch-hint residue self-test clean fixture failed", file=sys.stderr)
            for error in errors:
                print(f"  {error}", file=sys.stderr)
            return 1

        _write(root / "src/ir/residue.c", "int x = XI_COPY_KIND_LIKELY;\n")
        errors, _ = verify(root)
        if not any("XI_COPY_KIND_LIKELY" in error for error in errors):
            print("branch-hint residue self-test did not fail closed", file=sys.stderr)
            return 1
        _write(root / "src/ir/residue.c", "int x = 0;\n")
        _write(root / "src/frontend/analyzer/xanalyzer.c",
               'register_builtin_func(analyzer, "likely", type);\n')
        errors, _ = verify(root)
        if not any("compiler-owned branch-hint binding" in error for error in errors):
            print("branch-hint binding self-test did not fail closed", file=sys.stderr)
            return 1
    print("branch-hint source residue self-test: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--self-test", action="store_true", help="prove injected residue is rejected")
    args = parser.parse_args()
    if args.self_test:
        return self_test()

    root = Path(args.root).resolve()
    errors, internal_count = verify(root)
    if errors:
        print("branch-hint source residue gate failed:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1
    print(
        "branch-hint source residue: PASS "
        f"(0 public builtin bindings, 0 Xi variants, "
        f"{internal_count} internal XR_* occurrences excluded)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
