#!/usr/bin/env python3
"""Verify the clean exact-scalar source surface and deleted legacy owners."""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from pathlib import Path


EXACT_NAMES = (
    "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64",
    "f32", "f64", "isize", "usize",
)
REMOVED_NAMES = ("int", "byte", "float")
REGISTRY = Path("src/shared/xr_exact_scalar_registry.def")
REGISTRY_HEADER = Path("src/shared/xr_exact_scalar_registry.h")
POSITIVE_FIXTURE = Path("tests/regression/05_functions/0583_retired_scalar_names_reusable.xr")
TYPE_NEGATIVE = Path("tests/compile_errors/type/retired_scalar_type_spellings_removed.xr")
TYPE_EXPECTED = Path(str(TYPE_NEGATIVE) + ".expected")
CALL_NEGATIVE = Path("tests/compile_errors/type/retired_scalar_builtins_removed.xr")
CALL_EXPECTED = Path(str(CALL_NEGATIVE) + ".expected")
MODULE_NEGATIVE = Path("tests/compile_errors/stdlib/strconv_module_removed.xr")
MODULE_EXPECTED = Path(str(MODULE_NEGATIVE) + ".expected")

REMOVED_PATHS = (
    Path("src/shared/xr_scalar_type.h"),
    Path("src/shared/xr_scalar_type.def"),
    Path("stdlib/strconv/strconv.xr"),
    Path("stdlib/strconv/strconv.c"),
    Path("spec/source/cards/stdlib/strconv.json"),
    Path("tests/stdlib/contracts/strconv"),
)
REMOVED_SEMANTIC_TOKENS = (
    "TK_INT", "TK_BYTE", "TK_FLOAT",
    "XR_TREF_INT", "XR_TREF_FLOAT", "XR_TREF_BYTE",
    "XR_TREF_INT_WIDTH", "XR_TREF_FLOAT_WIDTH",
    "XR_TID_INT", "XR_TID_FLOAT", "XR_TID_BYTE",
    "XR_SOURCE_TYPE_INT", "XR_SOURCE_TYPE_FLOAT", "XR_SOURCE_TYPE_BYTE",
    "builtin_spelling", "xr_tref_int", "xr_tref_float", "xr_tref_byte",
    "xr_tref_int_width", "xr_tref_float_width",
)
SEMANTIC_ROOTS = ("src", "scripts", "tools", "xisa")
SEMANTIC_SUFFIXES = (".c", ".h", ".inc.c", ".py", ".def")

PUBLIC_BINDING_FILES = (
    Path("src/frontend/analyzer/xanalyzer.c"),
    Path("src/analysis/xglobal_producer.c"),
    Path("src/ir/xi_lower_expr.c"),
    Path("src/app/cli/xcmd_repl.c"),
    Path("scripts/gen_api_inventory.py"),
)
PUBLIC_BINDING_RE = re.compile(
    r"(?:register_builtin_func|strcmp)\s*\([^\n;]*[\"'](?:int|byte|float)[\"']"
    r"|[\"'](?:int|byte|float)[\"']\s*:"
)
SOURCE_SIGNATURE_RE = re.compile(
    r"(?:->|:|<|\bas\b|\bis\b)\s*(?:int|byte|float)\b"
    r"|\b(?:int|byte|float)\s*(?:\?|>)"
)
SOURCE_EXACT_BARE_IDENTIFIER_RE = re.compile(
    r"^\s*(?P<name>" + "|".join(EXACT_NAMES) + r")\s*,?\s*$"
)
PUBLIC_SIGNATURE_RE = re.compile(
    r"(?:->|:|<)\s*(?:int|byte|float)\b"
    r"|\b(?:int|byte|float)\s*(?:\?|>)"
)
PUBLIC_TEXT_FILES = (
    Path("LANGUAGE_SPEC.md"), Path("LANGUAGE_SPEC_CN.md"),
    Path("src/app/mcp/xmcp_knowledge_generated.c"),
    Path("src/app/lsp/xlsp_stdlib_generated.inc"),
    Path("src/frontend/analyzer/xanalyzer_builtins_generated.h"),
    Path("stdlib/defs/core.def"),
)
SCRIPT_SOURCE_OWNERS = (
    Path("scripts/add_type_annotations.py"),
    Path("scripts/check_binary_stdlib_kat_baseline.py"),
    Path("scripts/check_source_unknown_aot_baseline.py"),
)

HOST_SOURCE_ROOTS = ("src", "stdlib", "tests")
HOST_SOURCE_SUFFIXES = (".c", ".h", ".cc", ".cpp", ".cxx", ".hpp")
# Exact scalar spellings are source-language names, not host C/C++ typedefs.  Match
# declaration-shaped uses only so fields, variables, and macro suffixes named i64,
# u8, or f64 remain ordinary implementation identifiers.
HOST_EXACT_TYPE_RE = re.compile(
    r"(?:^|[;{}(,])\s*(?:(?:static|extern|inline|const|volatile|register|_Atomic)\s+)*"
    r"(?P<name>i64|u8|f64)\b\s*(?:\*\s*)?[A-Za-z_]"
)
HOST_ENTRY_ABI_EXPECTATION = Path("tests/unit/ir/test_xi_cgen.c")
HOST_ENTRY_ABI_REQUIRED = 'int main(int argc, char **argv)'
HOST_ENTRY_ABI_FORBIDDEN = re.compile(r"\b(?:i64|u8|f64)\s+main\s*\(")
HOST_WARNING_REQUIRED = "-Wimplicit-int-conversion"
HOST_WARNING_FORBIDDEN = "-Wimplicit-i64-conversion"

TOMBSTONE_ROWS = (
    "legacy-scalar-type-spellings\tint|byte|float\tTK_INT|TK_BYTE|TK_FLOAT|XR_TREF_INT|XR_TREF_BYTE|XR_TREF_FLOAT\t"
    "tests/compile_errors/type/retired_scalar_type_spellings_removed.xr\ti64|u8|f64",
    "legacy-scalar-global-conversions\tint(value)|byte(value)|float(value)\tbuiltin_spelling|global conversion lowering\t"
    "tests/compile_errors/type/retired_scalar_builtins_removed.xr\texpr as exact-type|i64.parse|f64.parse",
    "legacy-strconv-module\timport strconv|strconv.*\tstdlib module factory|manifest|generated API owner\t"
    "tests/compile_errors/stdlib/strconv_module_removed.xr\ti64.parse|i64.tryParse|f64.parse|f64.tryParse",
)


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="strict")


def _relative(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def _iter_files(root: Path, rel_roots: tuple[str, ...], suffixes: tuple[str, ...]):
    for rel_root in rel_roots:
        base = root / rel_root
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if not path.is_file() or not str(path).endswith(suffixes):
                continue
            if any(part in {"build", ".git", "__pycache__"} for part in path.parts):
                continue
            yield path


def _strip_non_code(source: str) -> str:
    """Replace comments and literals with spaces while preserving line structure."""
    out = list(source)
    i = 0
    depth = 0
    quote = ""
    while i < len(source):
        if depth:
            if source.startswith("/*", i):
                out[i:i + 2] = "  "
                depth += 1
                i += 2
            elif source.startswith("*/", i):
                out[i:i + 2] = "  "
                depth -= 1
                i += 2
            else:
                if source[i] not in "\r\n":
                    out[i] = " "
                i += 1
            continue
        if quote:
            if source[i] == "\\":
                out[i] = " "
                if i + 1 < len(source) and source[i + 1] not in "\r\n":
                    out[i + 1] = " "
                i += 2
            elif source[i] == quote:
                out[i] = " "
                quote = ""
                i += 1
            else:
                if source[i] not in "\r\n":
                    out[i] = " "
                i += 1
            continue
        if source.startswith("//", i):
            while i < len(source) and source[i] not in "\r\n":
                out[i] = " "
                i += 1
            continue
        if source.startswith("/*", i):
            out[i:i + 2] = "  "
            depth = 1
            i += 2
            continue
        if source[i] in {'"', "'", "`"}:
            quote = source[i]
            out[i] = " "
        i += 1
    return "".join(out)


def _check_registry(root: Path, errors: list[str]) -> None:
    path = root / REGISTRY
    if not path.is_file():
        errors.append("missing exact scalar registry")
        return
    rows = re.findall(
        r'^XR_EXACT_SCALAR\([^,]+,\s*(\d+),\s*"([^"]+)",\s*([^,]+),',
        _read(path), re.MULTILINE,
    )
    names = tuple(row[1] for row in rows)
    ids = tuple(int(row[0]) for row in rows)
    native_types = tuple(row[2].strip() for row in rows)
    if names != EXACT_NAMES:
        errors.append(f"exact scalar registry rows drifted: {names!r}")
    if ids != tuple(range(1, 13)) or len(set(native_types)) != 12:
        errors.append("exact scalar registry stable ids/native representations are not exhaustive and unique")
    if set(names) & set(REMOVED_NAMES):
        errors.append("exact scalar registry revived a removed spelling")
    header = root / REGISTRY_HEADER
    if not header.is_file() or "xr_exact_scalar_registry_validate" not in _read(header):
        errors.append("exact scalar registry validator is missing")


def _check_fixtures(root: Path, errors: list[str]) -> None:
    expected = {
        POSITIVE_FIXTURE: (
            "fn int(value: i64) -> i64", "fn byte(value: u8) -> u8",
            "fn float(value: f64) -> f64", "print(int(41))", "print(byte(7))",
            "print(float(3.5))",
        ),
        TYPE_NEGATIVE: ("value: int", "value: byte", "value: float"),
        TYPE_EXPECTED: ("undefined type 'int'", "undefined type 'byte'", "undefined type 'float'"),
        CALL_NEGATIVE: ('int("42")', "byte(7)", 'float("3.5")'),
        CALL_EXPECTED: ("Undeclared variable 'int'", "Undeclared variable 'byte'", "Undeclared variable 'float'"),
        MODULE_NEGATIVE: ("import strconv", "strconv.parseInt"),
        MODULE_EXPECTED: ("Module 'strconv' not found",),
    }
    for rel, needles in expected.items():
        path = root / rel
        if not path.is_file():
            errors.append(f"missing exact-scalar surface fixture {rel.as_posix()}")
            continue
        text = _read(path)
        for needle in needles:
            if needle not in text:
                errors.append(f"{rel.as_posix()}: fixture lost required evidence {needle!r}")


def _check_removed_paths_and_tokens(root: Path, errors: list[str]) -> None:
    for rel in REMOVED_PATHS:
        path = root / rel
        if path.is_dir():
            exists = any(child.is_file() for child in path.rglob("*"))
        else:
            exists = path.exists()
        if exists:
            errors.append(f"{rel.as_posix()}: removed owner still exists")
    checker = Path(__file__).resolve()
    token_re = re.compile(r"\b(?:" + "|".join(map(re.escape, REMOVED_SEMANTIC_TOKENS)) + r")\b")
    for path in _iter_files(root, SEMANTIC_ROOTS, SEMANTIC_SUFFIXES):
        if path.resolve() in {checker, (root / REGISTRY_HEADER).resolve()}:
            continue
        for line_no, line in enumerate(_read(path).splitlines(), 1):
            match = token_re.search(line)
            if match:
                errors.append(f"{_relative(path, root)}:{line_no}: removed semantic token {match.group(0)}")


def _check_public_owners(root: Path, errors: list[str]) -> None:
    for rel in PUBLIC_BINDING_FILES:
        path = root / rel
        if not path.is_file():
            errors.append(f"missing public scalar owner {rel.as_posix()}")
            continue
        for line_no, line in enumerate(_read(path).splitlines(), 1):
            if PUBLIC_BINDING_RE.search(line):
                errors.append(f"{rel.as_posix()}:{line_no}: legacy scalar public binding revived")
    abi_source = root / "xisa/aot/abi.def"
    if not abi_source.is_file() or re.search(r"\(define-aot-abi\s+(?:int|float)\b", _read(abi_source)):
        errors.append("xisa/aot/abi.def: legacy scalar ABI identity remains")
    abi_generated = root / "src/aot/xaot_abi_gen.h"
    if not abi_generated.is_file() or re.search(r'X\((?:INT|FLOAT),\s*"(?:int|float)"', _read(abi_generated)):
        errors.append("src/aot/xaot_abi_gen.h: generated legacy scalar ABI identity remains")
    for rel in PUBLIC_TEXT_FILES:
        path = root / rel
        if not path.is_file():
            errors.append(f"missing generated/public scalar surface {rel.as_posix()}")
            continue
        for line_no, line in enumerate(_read(path).splitlines(), 1):
            if PUBLIC_SIGNATURE_RE.search(line):
                errors.append(f"{rel.as_posix()}:{line_no}: legacy scalar signature residue")
    for rel in SCRIPT_SOURCE_OWNERS:
        path = root / rel
        if not path.is_file():
            errors.append(f"missing source-emitting scalar owner {rel.as_posix()}")
            continue
        for line_no, line in enumerate(_read(path).splitlines(), 1):
            for literal in re.findall(r"(?P<quote>['\"])(.*?)(?P=quote)", line):
                if PUBLIC_SIGNATURE_RE.search(literal[1]):
                    errors.append(f"{rel.as_posix()}:{line_no}: legacy scalar signature in emitted source")


def _check_source_corpus(root: Path, errors: list[str]) -> int:
    count = 0
    excluded = {TYPE_NEGATIVE, CALL_NEGATIVE, MODULE_NEGATIVE}
    for rel_root in ("stdlib", "tests", "demos", "bench"):
        base = root / rel_root
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if not path.is_file() or path.suffix not in {".xr", ".xrd"}:
                continue
            rel = path.relative_to(root)
            if rel in excluded:
                continue
            count += 1
            source = _read(path)
            code = _strip_non_code(source)
            for line_no, line in enumerate(code.splitlines(), 1):
                match = SOURCE_SIGNATURE_RE.search(line)
                if match:
                    errors.append(f"{rel.as_posix()}:{line_no}: unmigrated bound scalar spelling")
                identifier_match = SOURCE_EXACT_BARE_IDENTIFIER_RE.search(line)
                if identifier_match:
                    name = identifier_match.group("name")
                    errors.append(
                        f"{rel.as_posix()}:{line_no}: exact scalar keyword {name} used as an identifier"
                    )
            for interpolation in re.findall(r"\$\{([^}\r\n]*)\}", source):
                if SOURCE_SIGNATURE_RE.search(interpolation):
                    errors.append(f"{rel.as_posix()}: unmigrated scalar spelling in interpolation")
    return count


def _check_host_source_types(root: Path, errors: list[str]) -> None:
    for path in _iter_files(root, HOST_SOURCE_ROOTS, HOST_SOURCE_SUFFIXES):
        code = _strip_non_code(_read(path))
        for line_no, line in enumerate(code.splitlines(), 1):
            match = HOST_EXACT_TYPE_RE.search(line)
            if match:
                errors.append(
                    f"{_relative(path, root)}:{line_no}: source scalar "
                    f"{match.group('name')} used as a host C/C++ type"
                )
    entry_expectation = root / HOST_ENTRY_ABI_EXPECTATION
    if not entry_expectation.is_file():
        errors.append(f"missing generated-C entry ABI expectation {HOST_ENTRY_ABI_EXPECTATION.as_posix()}")
        return
    entry_text = _read(entry_expectation)
    for line_no, line in enumerate(entry_text.splitlines(), 1):
        if HOST_ENTRY_ABI_FORBIDDEN.search(line):
            errors.append(
                f"{HOST_ENTRY_ABI_EXPECTATION.as_posix()}:{line_no}: "
                "source scalar leaked into the generated-C entry ABI expectation"
            )
        if HOST_WARNING_FORBIDDEN in line:
            errors.append(
                f"{HOST_ENTRY_ABI_EXPECTATION.as_posix()}:{line_no}: "
                "source scalar leaked into a host compiler warning name"
            )
    if HOST_ENTRY_ABI_REQUIRED not in entry_text:
        errors.append(
            f"{HOST_ENTRY_ABI_EXPECTATION.as_posix()}: "
            "generated-C entry ABI expectation is missing"
        )
    if HOST_WARNING_REQUIRED not in entry_text:
        errors.append(
            f"{HOST_ENTRY_ABI_EXPECTATION.as_posix()}: "
            "host compiler warning expectation is missing"
        )


def _check_strconv(root: Path, errors: list[str]) -> None:
    checker = Path(__file__).resolve()
    allowed = {
        (root / "contracts/capability-deletions.tsv").resolve(),
        (root / MODULE_NEGATIVE).resolve(),
        (root / MODULE_EXPECTED).resolve(),
    }
    for path in _iter_files(
        root, ("src", "stdlib", "scripts", "spec", "docs"),
        (".c", ".h", ".inc.c", ".py", ".def", ".toml", ".md", ".json"),
    ):
        if path.resolve() == checker or path.resolve() in allowed:
            continue
        if re.search(r"\bstrconv\b", _read(path)):
            errors.append(f"{_relative(path, root)}: removed strconv owner/reference remains")


def _check_tombstones(root: Path, errors: list[str]) -> None:
    path = root / "contracts/capability-deletions.tsv"
    if not path.is_file():
        errors.append("missing capability deletion inventory")
        return
    lines = _read(path).splitlines()
    for row in TOMBSTONE_ROWS:
        if lines.count(row) != 1:
            errors.append(f"capability deletion row missing or duplicated: {row.split(chr(9), 1)[0]}")


def verify(root: Path) -> tuple[list[str], int]:
    errors: list[str] = []
    _check_registry(root, errors)
    _check_fixtures(root, errors)
    _check_removed_paths_and_tokens(root, errors)
    _check_public_owners(root, errors)
    source_count = _check_source_corpus(root, errors)
    _check_host_source_types(root, errors)
    _check_strconv(root, errors)
    _check_tombstones(root, errors)
    return errors, source_count


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def self_test() -> int:
    with tempfile.TemporaryDirectory(prefix="xray-exact-scalar-surface-") as raw:
        root = Path(raw)
        rows = "\n".join(
            f'XR_EXACT_SCALAR({name.upper()}, {index}, "{name}", XR_NATIVE_{name.upper()}, INTEGER, SIGNED, NONE)'
            for index, name in enumerate(EXACT_NAMES, 1)
        ) + "\n"
        _write(root / REGISTRY, rows)
        _write(root / REGISTRY_HEADER, "xr_exact_scalar_registry_validate\n")
        _write(root / POSITIVE_FIXTURE,
               "fn int(value: i64) -> i64 { return value }\n"
               "fn byte(value: u8) -> u8 { return value }\n"
               "fn float(value: f64) -> f64 { return value }\n"
               "print(int(41))\nprint(byte(7))\nprint(float(3.5))\n")
        _write(root / TYPE_NEGATIVE,
               "fn a(value: int) -> int { return value }\n"
               "fn b(value: byte) -> byte { return value }\n"
               "fn c(value: float) -> float { return value }\n")
        _write(root / TYPE_EXPECTED, "undefined type 'int'\nundefined type 'byte'\nundefined type 'float'\n")
        _write(root / CALL_NEGATIVE, 'int("42")\nbyte(7)\nfloat("3.5")\n')
        _write(root / CALL_EXPECTED,
               "Undeclared variable 'int'\nUndeclared variable 'byte'\nUndeclared variable 'float'\n")
        _write(root / MODULE_NEGATIVE, 'import strconv\nstrconv.parseInt("42")\n')
        _write(root / MODULE_EXPECTED, "Module 'strconv' not found\n")
        _write(root / "contracts/capability-deletions.tsv", "header\n" + "\n".join(TOMBSTONE_ROWS) + "\n")
        for rel in PUBLIC_BINDING_FILES + PUBLIC_TEXT_FILES + SCRIPT_SOURCE_OWNERS:
            _write(root / rel, "exact surface\n")
        _write(root / HOST_ENTRY_ABI_EXPECTATION,
               'assert(contains(code, "int main(int argc, char **argv)"));\n'
               'assert(contains(code, "-Wimplicit-int-conversion"));\n')
        _write(root / "xisa/aot/abi.def", "(define-aot-abi i64)\n(define-aot-abi f64)\n")
        _write(root / "src/aot/xaot_abi_gen.h", 'X(I64, "i64") X(F64, "f64")\n')

        errors, _ = verify(root)
        if errors:
            print("exact scalar surface self-test clean fixture failed:", file=sys.stderr)
            for error in errors:
                print(f"  {error}", file=sys.stderr)
            return 1
        _write(root / "src/frontend/parser/residue.c", "int token = TK_INT;\n")
        errors, _ = verify(root)
        if not any("TK_INT" in error for error in errors):
            print("exact scalar surface self-test did not fail closed", file=sys.stderr)
            return 1
        _write(root / "src/frontend/parser/residue.c", "int token = 0;\n")
        _write(root / "src/frontend/parser/host_type_residue.c", "static i64 parse_count(void);\n")
        errors, _ = verify(root)
        if not any("i64 used as a host C/C++ type" in error for error in errors):
            print("exact scalar host-type mutation did not fail closed", file=sys.stderr)
            return 1
        _write(root / "src/frontend/parser/host_type_residue.c", "static int parse_count(void);\n")
        _write(root / HOST_ENTRY_ABI_EXPECTATION,
               'assert(contains(code, "i64 main(i64 argc, char **argv)"));\n'
               'assert(contains(code, "-Wimplicit-int-conversion"));\n')
        errors, _ = verify(root)
        if not any("source scalar leaked into the generated-C entry ABI expectation" in error
                   for error in errors):
            print("exact scalar generated-C entry ABI mutation did not fail closed", file=sys.stderr)
            return 1
        _write(root / HOST_ENTRY_ABI_EXPECTATION,
               'assert(contains(code, "int main(int argc, char **argv)"));\n'
               'assert(contains(code, "-Wimplicit-i64-conversion"));\n')
        errors, _ = verify(root)
        if not any("source scalar leaked into a host compiler warning name" in error
                   for error in errors):
            print("exact scalar host warning mutation did not fail closed", file=sys.stderr)
            return 1
        _write(root / HOST_ENTRY_ABI_EXPECTATION,
               'assert(contains(code, "int main(int argc, char **argv)"));\n'
               'assert(contains(code, "-Wimplicit-int-conversion"));\n')
        _write(root / "tests/exact_keyword_identifier.xr", "enum Signal {\n    TERM,\n    i64\n}\n")
        errors, _ = verify(root)
        if not any("exact scalar keyword i64 used as an identifier" in error for error in errors):
            print("exact scalar source-identifier mutation did not fail closed", file=sys.stderr)
            return 1
        _write(root / "tests/exact_keyword_identifier.xr", "enum Signal { TERM, INTERRUPT }\n")
        _write(root / "tests/ordinary.xr", "fn int(x: i64) -> i64 { return x }\nprint(int(1))\n")
        errors, _ = verify(root)
        if errors:
            print("ordinary retired identifier spelling was incorrectly reserved", file=sys.stderr)
            return 1
    print("exact scalar surface residue self-test: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    root = Path(args.root).resolve()
    errors, source_count = verify(root)
    if errors:
        print("exact scalar surface residue gate failed:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1
    print(
        "exact scalar surface residue: PASS "
        f"(12 exact rows, {source_count} source/fixture files, 0 legacy owners)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
