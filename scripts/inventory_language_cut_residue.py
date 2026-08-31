#!/usr/bin/env python3
"""Inventory Task 287 legacy language surfaces at one exact Git revision."""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import re
import tarfile
from collections import Counter
from dataclasses import asdict, dataclass
from pathlib import Path

from inventory_enum_migration import (
    Token,
    git_output,
    lex,
    load_sources,
    matching_pairs,
)


FROZEN_RESIDUE_SHA256 = "85ecc70b4a580cf0195c00b00902307d25bbed24de94a4f929160d4993b09c8a"
RESIDUE_COLUMNS = (
    "surface", "scope", "count", "unit", "files", "classification",
    "lock_or_boundary", "detail",
)


@dataclass(frozen=True)
class WhereClause:
    path: str
    line: int


@dataclass(frozen=True)
class MatchExpression:
    path: str
    line: int
    category: str
    arm_count: int
    missing_separator_count: int


@dataclass(frozen=True)
class ComputedProperty:
    path: str
    line: int
    owner_kind: str
    name: str
    getter_count: int
    setter_count: int


@dataclass(frozen=True)
class InterfaceProperty:
    path: str
    line: int
    interface: str
    name: str


@dataclass(frozen=True)
class NativeAccessor:
    path: str
    line: int
    kind: str
    name: str
    registration: str


def load_archive_texts(
    root: Path, revision: str, prefixes: tuple[str, ...], suffixes: tuple[str, ...]
) -> dict[str, str]:
    archive = git_output(root, "archive", "--format=tar", revision, *prefixes, binary=True)
    texts: dict[str, str] = {}
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:") as bundle:
        for member in bundle.getmembers():
            if not member.isfile() or not member.name.endswith(suffixes):
                continue
            extracted = bundle.extractfile(member)
            if extracted is not None:
                texts[member.name] = extracted.read().decode("utf-8")
    return dict(sorted(texts.items()))


def top_level_positions(
    tokens: list[Token], begin: int, end: int, *, angles: bool
) -> list[int]:
    closing = {"(": ")", "[": "]", "{": "}"}
    if angles:
        closing["<"] = ">"
    stack: list[str] = []
    positions: list[int] = []
    for index in range(begin, end):
        spelling = tokens[index].spelling
        if stack and spelling == stack[-1]:
            stack.pop()
            continue
        if not stack:
            positions.append(index)
        if spelling in closing:
            stack.append(closing[spelling])
    return positions


def collect_where_clauses(token_sets: dict[str, list[Token]]) -> list[WhereClause]:
    clauses: list[WhereClause] = []
    for path, tokens in token_sets.items():
        for index in range(len(tokens) - 2):
            if (
                tokens[index].spelling == "where"
                and tokens[index + 1].kind == "identifier"
                and tokens[index + 2].spelling == ":"
            ):
                clauses.append(WhereClause(path=path, line=tokens[index].line))
    return clauses


def _is_lambda_return_arrow(
    tokens: list[Token], arrow: int, reverse_parens: dict[int, int], first_body_token: int | None
) -> bool:
    if (
        arrow > 0
        and first_body_token == arrow - 1
        and tokens[arrow - 1].kind == "identifier"
    ):
        return True
    if arrow == 0 or tokens[arrow - 1].spelling != ")":
        return False
    opening = reverse_parens.get(arrow - 1)
    if opening is None:
        return False
    if opening > 0 and tokens[opening - 1].spelling == "fn":
        return True
    return first_body_token == opening


def match_category(path: str) -> str:
    if "regex" in Path(path).name.lower() or "/regex/" in path:
        return "regex-boundary"
    if path.startswith("stdlib/"):
        return "stdlib"
    if path.startswith(("demos/", "bench/")):
        return "examples"
    return "tests"


def collect_matches(token_sets: dict[str, list[Token]]) -> list[MatchExpression]:
    matches: list[MatchExpression] = []
    for path, tokens in token_sets.items():
        parens = matching_pairs(tokens, "(", ")")
        reverse_parens = {close: opening for opening, close in parens.items()}
        braces = matching_pairs(tokens, "{", "}")
        for index in range(len(tokens) - 2):
            if tokens[index].spelling != "match" or tokens[index + 1].spelling != "(":
                continue
            close_paren = parens.get(index + 1)
            if close_paren is None or close_paren + 1 >= len(tokens):
                continue
            body = close_paren + 1
            if tokens[body].spelling != "{" or body not in braces:
                continue
            body_end = braces[body]
            positions = top_level_positions(tokens, body + 1, body_end, angles=False)
            arrows: list[int] = []
            first_body_token: int | None = None
            for position in positions:
                if tokens[position].spelling == "->":
                    if _is_lambda_return_arrow(
                        tokens, position, reverse_parens, first_body_token
                    ):
                        continue
                    arrows.append(position)
                    first_body_token = None
                    continue
                if arrows and first_body_token is None:
                    first_body_token = position
            if not arrows:
                continue
            position_set = set(positions)
            missing = 0
            for previous, following in zip(arrows, arrows[1:]):
                has_separator = any(
                    token_index in position_set and tokens[token_index].spelling == ","
                    for token_index in range(previous + 1, following)
                )
                missing += not has_separator
            matches.append(
                MatchExpression(
                    path=path,
                    line=tokens[index].line,
                    category=match_category(path),
                    arm_count=len(arrows),
                    missing_separator_count=missing,
                )
            )
    return matches


def declaration_bodies(
    tokens: list[Token], kinds: set[str]
) -> list[tuple[str, str, int, int]]:
    braces = matching_pairs(tokens, "{", "}")
    bodies: list[tuple[str, str, int, int]] = []
    for index in range(len(tokens) - 1):
        if tokens[index].spelling not in kinds or tokens[index + 1].kind != "identifier":
            continue
        body = index + 2
        while body < len(tokens) and tokens[body].spelling != "{":
            body += 1
        if body in braces:
            bodies.append((tokens[index].spelling, tokens[index + 1].spelling,
                           body, braces[body]))
    return bodies


def type_annotation_end(tokens: list[Token], begin: int, limit: int) -> int:
    """Return the token after one type annotation, mirroring its delimiter shape."""
    parens = matching_pairs(tokens, "(", ")")
    brackets = matching_pairs(tokens, "[", "]")
    braces = matching_pairs(tokens, "{", "}")
    angles = matching_pairs(tokens, "<", ">")

    def base(index: int) -> int:
        while index < limit and tokens[index].spelling == "const":
            index += 1
        if index >= limit:
            return index
        spelling = tokens[index].spelling
        if spelling == "[" and index in brackets:
            return brackets[index] + 1
        if spelling == "{" and index in braces:
            return braces[index] + 1
        if spelling == "fn":
            index += 1
            if index < limit and tokens[index].spelling == "<" and index in angles:
                index = angles[index] + 1
            if index < limit and tokens[index].spelling == "(" and index in parens:
                index = parens[index] + 1
            if index < limit and tokens[index].spelling == "->":
                return annotation(index + 1)
            return index
        if spelling == "(" and index in parens:
            index = parens[index] + 1
            if index < limit and tokens[index].spelling == "->":
                return annotation(index + 1)
            return index
        if tokens[index].kind != "identifier":
            return index
        index += 1
        if index + 1 < limit and tokens[index].spelling == "." \
                and tokens[index + 1].kind == "identifier":
            index += 2
        if index < limit and tokens[index].spelling == "<" and index in angles:
            index = angles[index] + 1
        return index

    def annotation(index: int) -> int:
        index = base(index)
        if index < limit and tokens[index].spelling == "?":
            index += 1
        while index < limit and tokens[index].spelling == "|":
            index = base(index + 1)
            if index < limit and tokens[index].spelling == "?":
                index += 1
        return index

    return annotation(begin)


def _property_accessors(
    tokens: list[Token], body: int, body_end: int, parens: dict[int, int]
) -> tuple[int, int]:
    getters = 0
    setters = 0
    positions = top_level_positions(tokens, body + 1, body_end, angles=True)
    for position in positions:
        if (
            tokens[position].spelling != "fn"
            or position + 1 >= body_end
            or tokens[position + 1].spelling != "("
            or position + 1 not in parens
        ):
            continue
        close = parens[position + 1]
        if close == position + 2:
            getters += 1
        else:
            setters += 1
    return getters, setters


def collect_properties(
    token_sets: dict[str, list[Token]],
) -> tuple[list[ComputedProperty], list[InterfaceProperty]]:
    computed: list[ComputedProperty] = []
    interfaces: list[InterfaceProperty] = []
    for path, tokens in token_sets.items():
        parens = matching_pairs(tokens, "(", ")")
        braces = matching_pairs(tokens, "{", "}")
        for owner_kind, owner_name, body, body_end in declaration_bodies(
            tokens, {"class", "struct", "interface"}
        ):
            positions = top_level_positions(tokens, body + 1, body_end, angles=True)
            if owner_kind == "interface":
                for offset, position in enumerate(positions):
                    if tokens[position].spelling != ":" or offset == 0:
                        continue
                    name_token = tokens[positions[offset - 1]]
                    if name_token.kind == "identifier":
                        interfaces.append(
                            InterfaceProperty(path, name_token.line, owner_name,
                                              name_token.spelling)
                        )
                continue

            for position in positions:
                if tokens[position].spelling != ":":
                    continue
                name_index = position - 1
                if name_index < 0 or tokens[name_index].kind != "identifier":
                    continue
                property_body = type_annotation_end(tokens, position + 1, body_end)
                if (
                    property_body >= body_end
                    or tokens[property_body].spelling != "{"
                    or property_body not in braces
                ):
                    continue
                getters, setters = _property_accessors(
                    tokens, property_body, braces[property_body], parens
                )
                if getters or setters:
                    computed.append(
                        ComputedProperty(
                            path=path,
                            line=tokens[name_index].line,
                            owner_kind=owner_kind,
                            name=tokens[name_index].spelling,
                            getter_count=getters,
                            setter_count=setters,
                        )
                    )
    return computed, interfaces


def strip_c_comments(source: str) -> str:
    output = list(source)
    index = 0
    while index < len(source):
        if source.startswith("//", index):
            end = source.find("\n", index + 2)
            end = len(source) if end < 0 else end
            for cursor in range(index, end):
                output[cursor] = " "
            index = end
            continue
        if source.startswith("/*", index):
            end = source.find("*/", index + 2)
            end = len(source) if end < 0 else end + 2
            for cursor in range(index, end):
                if output[cursor] != "\n":
                    output[cursor] = " "
            index = end
            continue
        if source[index] in "\"'":
            quote = source[index]
            index += 1
            while index < len(source):
                if source[index] == "\\":
                    index += 2
                elif source[index] == quote:
                    index += 1
                    break
                else:
                    index += 1
            continue
        index += 1
    return "".join(output)


def collect_native_accessors(c_sources: dict[str, str]) -> list[NativeAccessor]:
    accessors: list[NativeAccessor] = []
    array_pattern = re.compile(
        r"static\s+const\s+XrNativeMethod\s+([A-Za-z_][A-Za-z0-9_]*_getters)"
        r"\s*\[\s*\]\s*=\s*\{(.*?)\n\s*\};",
        re.S,
    )
    entry_pattern = re.compile(r'\{\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*,')
    direct_pattern = re.compile(
        r'xr_class_builder_add_method\s*\(\s*[^,]+,\s*"(get|set):([^"\\]+)"',
        re.S,
    )
    for path, source in c_sources.items():
        clean = strip_c_comments(source)
        for array in array_pattern.finditer(clean):
            body = array.group(2)
            body_offset = array.start(2)
            for entry in entry_pattern.finditer(body):
                offset = body_offset + entry.start()
                accessors.append(
                    NativeAccessor(
                        path=path,
                        line=clean.count("\n", 0, offset) + 1,
                        kind="getter",
                        name=entry.group(1),
                        registration=array.group(1),
                    )
                )
        for direct in direct_pattern.finditer(clean):
            accessors.append(
                NativeAccessor(
                    path=path,
                    line=clean.count("\n", 0, direct.start()) + 1,
                    kind="getter" if direct.group(1) == "get" else "setter",
                    name=direct.group(2),
                    registration="xr_class_builder_add_method",
                )
            )
    return accessors


def summarize_matches(matches: list[MatchExpression]) -> dict[str, dict[str, int]]:
    categories = ("stdlib", "examples", "tests", "regex-boundary")
    summary: dict[str, dict[str, int]] = {}
    for category in categories:
        rows = [row for row in matches if row.category == category]
        summary[category] = {
            "match_count": len(rows),
            "multi_arm_count": sum(row.arm_count >= 2 for row in rows),
            "comma_less_multi_arm_count": sum(
                row.arm_count >= 2 and row.missing_separator_count > 0 for row in rows
            ),
            "missing_separator_count": sum(row.missing_separator_count for row in rows),
        }
    return summary


def make_report(root: Path, revision: str) -> dict[str, object]:
    resolved = git_output(root, "rev-parse", revision).strip()
    tree = git_output(root, "rev-parse", f"{resolved}^{{tree}}").strip()
    sources = load_sources(root, resolved)
    token_sets = {path: lex(source) for path, source in sources.items()}
    clauses = collect_where_clauses(token_sets)
    matches = collect_matches(token_sets)
    computed, interfaces = collect_properties(token_sets)
    c_sources = load_archive_texts(root, resolved, ("src",), (".c", ".h"))
    native = collect_native_accessors(c_sources)
    match_summary = summarize_matches(matches)
    all_match = {
        "match_count": len(matches),
        "multi_arm_count": sum(row.arm_count >= 2 for row in matches),
        "comma_less_multi_arm_count": sum(
            row.arm_count >= 2 and row.missing_separator_count > 0 for row in matches
        ),
        "missing_separator_count": sum(row.missing_separator_count for row in matches),
    }
    return {
        "schema_version": 1,
        "revision": resolved,
        "tree": tree,
        "where_clause_count": len(clauses),
        "where_file_count": len({row.path for row in clauses}),
        "where_clauses": [asdict(row) for row in clauses],
        "match": all_match,
        "match_by_category": match_summary,
        "match_file_count_with_missing_separator": len(
            {row.path for row in matches if row.missing_separator_count > 0}
        ),
        "matches": [asdict(row) for row in matches],
        "computed_property_count": len(computed),
        "computed_property_file_count": len({row.path for row in computed}),
        "computed_getter_count": sum(row.getter_count for row in computed),
        "computed_setter_count": sum(row.setter_count for row in computed),
        "computed_property_by_root": dict(
            sorted(Counter(row.path.split("/", 1)[0] for row in computed).items())
        ),
        "computed_properties": [asdict(row) for row in computed],
        "interface_property_count": len(interfaces),
        "interface_property_file_count": len({row.path for row in interfaces}),
        "interface_properties": [asdict(row) for row in interfaces],
        "native_getter_count": sum(row.kind == "getter" for row in native),
        "native_setter_count": sum(row.kind == "setter" for row in native),
        "native_accessor_file_count": len({row.path for row in native}),
        "native_accessors": [asdict(row) for row in native],
        "lexical_error_count": sum(
            token.kind == "error" for tokens in token_sets.values() for token in tokens
        ),
    }


def _row(rows: list[dict[str, str]], surface: str, scope: str, unit: str) -> dict[str, str]:
    found = [
        row for row in rows
        if row["surface"] == surface and row["scope"] == scope and row["unit"] == unit
    ]
    if len(found) != 1:
        raise ValueError(f"expected one residue row for {(surface, scope, unit)}, found {len(found)}")
    return found[0]


def validate_residue_tsv(report: dict[str, object], path: Path) -> None:
    actual_digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if actual_digest != FROZEN_RESIDUE_SHA256:
        raise SystemExit(
            "residue validation failed:\n  "
            f"reviewed TSV sha256 {actual_digest} != frozen {FROZEN_RESIDUE_SHA256}"
        )
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream, delimiter="\t")
        rows = list(reader)
    errors: list[str] = []
    if tuple(reader.fieldnames or ()) != RESIDUE_COLUMNS:
        errors.append(f"unexpected residue columns: {reader.fieldnames}")
    keys = [(row["surface"], row["scope"], row["unit"]) for row in rows]
    if len(rows) != 40:
        errors.append(f"expected 40 residue rows, found {len(rows)}")
    if len(keys) != len(set(keys)):
        errors.append("residue TSV contains duplicate surface/scope/unit keys")

    def check(row: dict[str, str], expected_count: int, expected_files: int | None = None) -> None:
        if int(row["count"]) != expected_count:
            errors.append(
                f"{row['surface']}/{row['scope']}/{row['unit']}: TSV count {row['count']} "
                f"!= source count {expected_count}"
            )
        if expected_files is not None:
            try:
                actual_files = int(row["files"])
            except ValueError:
                actual_files = None
            if actual_files != expected_files:
                errors.append(
                    f"{row['surface']}/{row['scope']}/{row['unit']}: TSV files "
                    f"{row['files']} != source files {expected_files}"
                )

    def check_detail(row: dict[str, str], label: str, expected: int) -> None:
        found = re.search(rf"\b([0-9]+)\s+{re.escape(label)}\b", row["detail"])
        if found is None or int(found.group(1)) != expected:
            errors.append(
                f"{row['surface']}/{row['scope']}/{row['unit']}: detail must record "
                f"{expected} {label}"
            )

    check(_row(rows, "where", "tests", "trailing where clauses"),
          int(report["where_clause_count"]), int(report["where_file_count"]))
    summary = report["match_by_category"]
    for scope, category in (("stdlib excluding regex", "stdlib"), ("demos", "examples"),
                            ("tests", "tests")):
        match_row = _row(rows, "match-comma", scope, "match expressions")
        missing_row = _row(rows, "match-comma", scope, "comma-less multi-arm matches")
        check(match_row, int(summary[category]["match_count"]))
        check_detail(match_row, "multi-arm", int(summary[category]["multi_arm_count"]))
        check(missing_row, int(summary[category]["comma_less_multi_arm_count"]))
        check_detail(missing_row, "missing separators",
                     int(summary[category]["missing_separator_count"]))
    check(_row(rows, "match-comma", "regex boundary", "match expression"),
          int(summary["regex-boundary"]["match_count"]))
    all_match_row = _row(rows, "match-comma", "all scoped Xray source", "match expressions")
    all_missing_row = _row(rows, "match-comma", "all scoped Xray source",
                           "comma-less multi-arm matches")
    check(all_match_row, int(report["match"]["match_count"]))
    check_detail(all_match_row, "multi-arm", int(report["match"]["multi_arm_count"]))
    check(all_missing_row, int(report["match"]["comma_less_multi_arm_count"]),
          int(report["match_file_count_with_missing_separator"]))
    check_detail(all_missing_row, "missing separators",
                 int(report["match"]["missing_separator_count"]))
    all_property_row = _row(rows, "property", "all scoped Xray source",
                            "computed accessor blocks")
    check(all_property_row, int(report["computed_property_count"]),
          int(report["computed_property_file_count"]))
    check_detail(all_property_row, "getters", int(report["computed_getter_count"]))
    check_detail(all_property_row, "setters", int(report["computed_setter_count"]))
    check(_row(rows, "property", "stdlib", "computed accessor blocks"),
          int(report["computed_property_by_root"].get("stdlib", 0)))
    check(_row(rows, "property", "tests", "computed accessor blocks"),
          int(report["computed_property_by_root"].get("tests", 0)))
    check(_row(rows, "property", "tests", "interface properties"),
          int(report["interface_property_count"]), int(report["interface_property_file_count"]))
    check(_row(rows, "property", "native registration", "native getter entries"),
          int(report["native_getter_count"]), int(report["native_accessor_file_count"]))
    if int(report["native_setter_count"]) != 0:
        errors.append(
            f"native registration inventory found {report['native_setter_count']} setters; "
            "the reviewed TSV has no setter row"
        )
    if errors:
        raise SystemExit("residue validation failed:\n  " + "\n  ".join(errors))


def run_self_tests() -> None:
    sources = {
        "tests/fixture/main.xr": (
            "fn f<T>(value: T) -> T where T: Comparable { return value }\n"
            "fn where() -> i64 { return 1 }\n"
            "class Box { value: i64 { fn() { return 1 } fn(next: i64) {} } }\n"
            "class CallbackBox {\n"
            "  callback: fn() -> i64\n"
            "  fn run() -> i64 { return callback() }\n"
            "}\n"
            "class MultiLine {\n"
            "  amount:\n"
            "    i64\n"
            "  { fn() { return 1 } }\n"
            "  callback:\n"
            "    fn() -> i64\n"
            "  { fn() { return fn() -> i64 { return 1 } } }\n"
            "}\n"
            "interface Sized { const length: i64; get(index: i64) -> i64 }\n"
            "interface MultiLineSized {\n"
            "  amount\n"
            "    : i64\n"
            "}\n"
            "var x = match (1) { 1 -> fn(v: i64) -> i64 { return v } 2 -> 2 }\n"
            "var y = match (2) { 1 -> match (1) { 1 -> 1 2 -> 2 }, 2 -> 2 }\n"
            "var z = match (3) { 1 -> () -> { return 1 }, 2 -> (x) -> x, }\n"
            "var bare = match (4) { 1 -> v -> v, 2 -> next -> next, }\n"
        )
    }
    token_sets = {path: lex(source) for path, source in sources.items()}
    if len(collect_where_clauses(token_sets)) != 1:
        raise AssertionError("where-clause shape classifier rejected or overcounted fixture")
    matches = collect_matches(token_sets)
    if len(matches) != 5 or sum(row.missing_separator_count for row in matches) != 2:
        raise AssertionError("match separator or nested-match fixture count is incorrect")
    if any(
        row.arm_count != 2 or row.missing_separator_count != 0
        for row in matches[-2:]
    ):
        raise AssertionError("arrow-lambda arm bodies were counted as match arms")
    computed, interfaces = collect_properties(token_sets)
    if (
        len(computed) != 3
        or sum(row.getter_count for row in computed) != 3
        or sum(row.setter_count for row in computed) != 1
    ):
        raise AssertionError("computed-property fixture count is incorrect")
    if len(interfaces) != 2 or {row.name for row in interfaces} != {"length", "amount"}:
        raise AssertionError("interface-property fixture count is incorrect")

    c_source = {
        "src/fixture.c": (
            "static const XrNativeMethod sample_getters[] = {\n"
            "  {\"value\", get_value, 0}, {NULL, NULL, 0},\n};\n"
            "xr_class_builder_add_method(builder, \"get:size\", get_size, 0, 0);\n"
        )
    }
    native = collect_native_accessors(c_source)
    if len(native) != 2 or any(row.kind != "getter" for row in native):
        raise AssertionError("native getter fixture count is incorrect")

    malformed = lex('var text = "unterminated\nwhere T: Trait\n')
    if sum(token.kind == "error" for token in malformed) != 1:
        raise AssertionError("malformed quoted input did not produce deterministic recovery")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--revision", default="HEAD", help="Git revision to inventory")
    parser.add_argument("--json", action="store_true", help="emit full machine-readable report")
    parser.add_argument("--residue", help="validate the preparation residue TSV")
    parser.add_argument("--self-test", action="store_true", help="run constructed scanner tests")
    args = parser.parse_args()

    if args.self_test:
        run_self_tests()
        print("language-cut residue self-tests: PASS")
    report = make_report(Path(args.root).resolve(), args.revision)
    if args.residue:
        validate_residue_tsv(report, Path(args.residue))
    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
        return 0
    print(f"revision: {report['revision']}")
    print(f"tree: {report['tree']}")
    print(f"where clauses: {report['where_clause_count']} in {report['where_file_count']} files")
    print(f"match expressions: {report['match']['match_count']}")
    print(f"multi-arm matches: {report['match']['multi_arm_count']}")
    print(f"comma-less multi-arm matches: {report['match']['comma_less_multi_arm_count']}")
    print(f"missing match separators: {report['match']['missing_separator_count']}")
    print(f"computed properties: {report['computed_property_count']}")
    print(f"computed getters/setters: {report['computed_getter_count']}/{report['computed_setter_count']}")
    print(f"interface properties: {report['interface_property_count']}")
    print(f"native getters: {report['native_getter_count']}")
    print(f"lexically invalid tracked fixtures: {report['lexical_error_count']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
