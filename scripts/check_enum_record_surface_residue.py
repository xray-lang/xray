#!/usr/bin/env python3
"""Reject the removed positional/call-shaped enum surface.

The executable-source scan is token based. It reuses the migration inventory's
module resolver, but derives enum definitions from the current record syntax so
ordinary methods with the same spelling are not mistaken for constructors.
Negative parser tests live in C string fixtures and remain outside this source
surface gate.
"""

from __future__ import annotations

import argparse
import sys
import tempfile
from pathlib import Path

from inventory_enum_migration import (
    ModuleResolver,
    PayloadDeclaration,
    collect_builtin_enum_names,
    collect_qualified_uses,
    lex,
    matching_pairs,
    split_payload_fields,
)


SCAN_ROOTS = ("bench", "demos", "stdlib", "tests")
REMOVED_COMPILER_SYMBOLS = (
    "allow_payload_enum_ctor_value",
    "parse_enum_variant_payload",
)


def _load_sources(root: Path) -> dict[str, str]:
    sources: dict[str, str] = {}
    for relative in SCAN_ROOTS:
        base = root / relative
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*.xr")):
            sources[path.relative_to(root).as_posix()] = path.read_text(
                encoding="utf-8", errors="strict"
            )
    return sources


def _record_fields(tokens, begin: int, end: int) -> tuple[int, list[str]]:
    fields = split_payload_fields(tokens, begin, end)
    errors: list[str] = []
    for field in fields:
        if not field:
            continue
        if field[0].kind != "identifier":
            errors.append("field has no identifier name")
            continue
        if len(field) < 3 or field[1].spelling != ":":
            errors.append(f"field '{field[0].spelling}' has no ':' type separator")
    return len(fields), errors


def _collect_current_declarations(token_sets):
    rows: list[PayloadDeclaration] = []
    ranges: dict[str, list[tuple[int, int]]] = {}
    errors: list[str] = []
    enum_count = 0

    for path, tokens in token_sets.items():
        braces = matching_pairs(tokens, "{", "}")
        parens = matching_pairs(tokens, "(", ")")
        path_ranges: list[tuple[int, int]] = []
        index = 0
        while index < len(tokens):
            if not (
                tokens[index].spelling == "enum"
                and index + 1 < len(tokens)
                and tokens[index + 1].kind == "identifier"
            ):
                index += 1
                continue
            enum_count += 1
            enum_name = tokens[index + 1].spelling
            body = index + 2
            while body < len(tokens) and tokens[body].spelling != "{":
                body += 1
            body_end = braces.get(body)
            if body_end is None:
                index += 1
                continue
            path_ranges.append((tokens[index].start, tokens[body_end].end))

            cursor = body + 1
            while cursor < body_end:
                token = tokens[cursor]
                if token.spelling == ",":
                    cursor += 1
                    continue
                if token.spelling == "fn":
                    errors.append(
                        f"{path}:{token.line}: enum methods no longer use 'fn'"
                    )
                    break
                if token.spelling in ("@", "static", "ref", "move"):
                    break
                if token.kind != "identifier":
                    cursor += 1
                    continue

                variant = token.spelling
                next_index = cursor + 1
                if next_index < body_end and tokens[next_index].spelling == "{":
                    close = braces.get(next_index)
                    if close is None:
                        break
                    field_count, field_errors = _record_fields(tokens, next_index + 1, close)
                    for message in field_errors:
                        errors.append(f"{path}:{token.line}: {variant}: {message}")
                    if field_count == 0:
                        errors.append(
                            f"{path}:{token.line}: empty record payload declaration for "
                            f"{enum_name}.{variant}"
                        )
                    rows.append(
                        PayloadDeclaration(
                            path=path,
                            line=token.line,
                            enum=enum_name,
                            variant=variant,
                            field_count=field_count,
                            named_count=field_count,
                            unnamed_count=0,
                        )
                    )
                    cursor = close + 1
                    continue

                if next_index < body_end and tokens[next_index].spelling == "(":
                    close = parens.get(next_index)
                    if close is None:
                        break
                    after = tokens[close + 1].spelling if close + 1 < len(tokens) else ""
                    if after in (",", "}"):
                        errors.append(
                            f"{path}:{token.line}: positional enum payload declaration "
                            f"{enum_name}.{variant}(...)"
                        )
                        fields = split_payload_fields(tokens, next_index + 1, close)
                        rows.append(
                            PayloadDeclaration(
                                path=path,
                                line=token.line,
                                enum=enum_name,
                                variant=variant,
                                field_count=len(fields),
                                named_count=0,
                                unnamed_count=len(fields),
                            )
                        )
                        cursor = close + 1
                        continue
                    break

                rows.append(
                    PayloadDeclaration(
                        path=path,
                        line=token.line,
                        enum=enum_name,
                        variant=variant,
                        field_count=0,
                        named_count=0,
                        unnamed_count=0,
                    )
                )
                cursor += 1
            index = body_end + 1
        ranges[path] = path_ranges
    return rows, ranges, enum_count, errors


def verify(root: Path) -> tuple[list[str], dict[str, int]]:
    errors: list[str] = []
    sources = _load_sources(root)
    token_sets = {path: lex(source) for path, source in sources.items()}
    declarations, declaration_ranges, enum_count, declaration_errors = (
        _collect_current_declarations(token_sets)
    )
    errors.extend(declaration_errors)

    builtin_path = root / "stdlib/prelude/builtin_symbols.def"
    builtin_names = (
        collect_builtin_enum_names(builtin_path.read_text(encoding="utf-8"))
        if builtin_path.is_file()
        else set()
    )
    resolver = ModuleResolver(sources, token_sets, declarations, builtin_names)
    old_uses, _excluded = collect_qualified_uses(
        token_sets, declaration_ranges, resolver
    )
    for use in old_uses:
        errors.append(
            f"{use.path}:{use.line}: call-shaped enum use "
            f"{use.enum}.{use.variant}(...) [{use.role}]"
        )

    checker = Path(__file__).resolve()
    for path in sorted((root / "src").rglob("*")):
        if not path.is_file() or path.suffix not in {".c", ".h"}:
            continue
        if path.resolve() == checker:
            continue
        text = path.read_text(encoding="utf-8", errors="strict")
        for symbol in REMOVED_COMPILER_SYMBOLS:
            if symbol in text:
                errors.append(
                    f"{path.relative_to(root).as_posix()}: removed enum compatibility symbol "
                    f"{symbol}"
                )

    return errors, {
        "files": len(sources),
        "enums": enum_count,
        "variants": len(declarations),
        "old_uses": len(old_uses),
    }


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def self_test() -> int:
    with tempfile.TemporaryDirectory(prefix="xray-enum-record-residue-") as raw:
        root = Path(raw)
        _write(root / "stdlib/prelude/builtin_symbols.def", "")
        _write(
            root / "tests/clean.xr",
            "enum Result { Ok { value: i64 }, Done }\n"
            "var value = Result.Ok { value: 1 }\n"
            "var done = Result.Done\n",
        )
        errors, _ = verify(root)
        if errors:
            print("enum record residue clean fixture failed", file=sys.stderr)
            for error in errors:
                print(f"  {error}", file=sys.stderr)
            return 1

        _write(
            root / "tests/legacy.xr",
            "enum Old { Item(i64) }\nvar value = Old.Item(1)\n",
        )
        errors, _ = verify(root)
        if not any("payload declaration" in error for error in errors):
            print("enum record residue self-test missed old declaration", file=sys.stderr)
            return 1
        if not any("call-shaped enum use" in error for error in errors):
            print("enum record residue self-test missed old construction", file=sys.stderr)
            return 1

        _write(root / "tests/legacy.xr", "enum Old { Item { value: i64 } }\n")
        _write(root / "src/legacy.c", "bool allow_payload_enum_ctor_value;\n")
        errors, _ = verify(root)
        if not any("compatibility symbol" in error for error in errors):
            print("enum record residue self-test missed old compiler symbol", file=sys.stderr)
            return 1

    print("enum record surface residue self-test: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()

    root = Path(args.root).resolve()
    try:
        errors, counts = verify(root)
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"enum record surface residue: FAIL: {exc}", file=sys.stderr)
        return 1
    if errors:
        print("enum record surface residue gate failed:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1
    print(
        "enum record surface residue: PASS "
        f"({counts['files']} files, {counts['enums']} enums, "
        f"{counts['variants']} variants, 0 old qualified uses)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
