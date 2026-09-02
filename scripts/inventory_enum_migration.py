#!/usr/bin/env python3
"""Inventory positional enum payloads and review qualified-use provenance.

The scanner reads an exact Git tree and validates every syntactic use candidate
against a committed, source-span-bound review oracle. It is read-only and does
not rewrite source or claim to replace semantic name resolution.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import posixpath
import re
import subprocess
import tarfile
from collections import Counter
from dataclasses import asdict, dataclass
from pathlib import Path


SCAN_ROOTS = ("bench", "demos", "stdlib", "tests")


@dataclass(frozen=True)
class Token:
    kind: str
    spelling: str
    start: int
    end: int
    line: int


@dataclass(frozen=True)
class PayloadDeclaration:
    path: str
    line: int
    enum: str
    variant: str
    field_count: int
    named_count: int
    unnamed_count: int


@dataclass
class QualifiedUse:
    path: str
    line: int
    enum: str
    variant: str
    start: int
    end: int
    role: str
    declaration_path: str
    declaration_line: int
    resolution: str


@dataclass(frozen=True)
class EnumDefinition:
    path: str
    line: int
    enum: str
    variants: tuple[tuple[str, int], ...]

    def variant_line(self, name: str) -> int | None:
        for variant, line in self.variants:
            if variant == name:
                return line
        return None


@dataclass(frozen=True)
class UseCandidate:
    path: str
    line: int
    receiver: str
    variant: str
    start: int
    end: int
    role: str
    disposition: str
    declaration_path: str
    declaration_line: int
    enum: str
    resolution: str
    review: str
    evidence: str
    source_sha256: str


@dataclass(frozen=True)
class ImportBinding:
    target_path: str
    target_name: str
    resolution: str


PROVENANCE_COLUMNS = (
    "tree",
    "path",
    "start_codepoint",
    "end_codepoint",
    "line",
    "receiver",
    "variant",
    "role",
    "disposition",
    "declaration_path",
    "declaration_line",
    "enum",
    "resolution",
    "review",
    "evidence",
    "source_sha256",
)
ACCEPTED_PROVENANCE_CLASSES = {
    "same-module",
    "builtin-symbol",
    "import->module-export",
    "import->reexport->module-export",
}
REJECTED_PROVENANCE_CLASSES = {
    "reviewed-reject:module-qualified-receiver",
    "reviewed-reject:non-enum-type-shadow",
    "reviewed-reject:resolved-enum-nonpayload-member",
    "reviewed-reject:unresolved-module-name",
    "reviewed-reject:value-shadow",
}
FROZEN_PROVENANCE_SHA256_BY_TREE = {
    "478e72a21e54ae5cc182caaa77cf9e526725a0a6":
        "e409bfaef4e5b13d548631c42c4c65897030db8b865e8b133c2894e2bf006eb9",
}


def git_output(root: Path, *args: str, binary: bool = False):
    result = subprocess.run(
        ["git", *args],
        cwd=root,
        check=True,
        stdout=subprocess.PIPE,
        text=not binary,
    )
    return result.stdout


def load_sources(root: Path, revision: str) -> dict[str, str]:
    archive = git_output(root, "archive", "--format=tar", revision, *SCAN_ROOTS, binary=True)
    sources: dict[str, str] = {}
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:") as bundle:
        for member in bundle.getmembers():
            if not member.isfile() or not member.name.endswith(".xr"):
                continue
            extracted = bundle.extractfile(member)
            if extracted is None:
                continue
            sources[member.name] = extracted.read().decode("utf-8")
    return dict(sorted(sources.items()))


def load_revision_text(root: Path, revision: str, path: str) -> str:
    return git_output(root, "show", f"{revision}:{path}")


def load_reserved_words(root: Path, revision: str) -> set[str]:
    keyword_text = load_revision_text(root, revision, "src/frontend/lexer/xkeywords.def")
    scalar_text = load_revision_text(root, revision, "src/shared/xr_exact_scalar_registry.def")
    keywords = set(re.findall(r'XR_KW\(\s*"([^"]+)"', keyword_text))
    keywords.update(re.findall(r'XR_EXACT_SCALAR\([^,]+,[^,]+,\s*"([^"]+)"', scalar_text))
    return keywords


def _identifier_start(char: str) -> bool:
    return char == "_" or char.isalpha() or ord(char) >= 0x80


def _identifier_continue(char: str) -> bool:
    return char == "_" or char.isalnum() or ord(char) >= 0x80


class XrayLexer:
    """Source-location lexer for inventory, including quoted interpolation."""

    def __init__(self, source: str):
        self.source = source
        self.index = 0
        self.line = 1
        self.tokens: list[Token] = []

    def scan(self) -> list[Token]:
        self._scan_tokens(stop_at_interpolation_end=False)
        return self.tokens

    def _error(self, start: int, line: int, message: str) -> None:
        self.tokens.append(Token("error", message, start, self.index, line))

    def _advance_newline(self) -> None:
        self.line += 1
        self.index += 1

    def _skip_line_comment(self) -> None:
        end = self.source.find("\n", self.index + 2)
        self.index = len(self.source) if end < 0 else end

    def _skip_block_comment(self) -> None:
        start = self.index
        start_line = self.line
        depth = 1
        self.index += 2
        while self.index < len(self.source) and depth:
            if self.source.startswith("/*", self.index):
                depth += 1
                self.index += 2
            elif self.source.startswith("*/", self.index):
                depth -= 1
                self.index += 2
            elif self.source[self.index] == "\n":
                self._advance_newline()
            else:
                self.index += 1
        if depth:
            self._error(start, start_line, "unterminated block comment")

    def _quoted_prefix(self) -> tuple[int, str, bool] | None:
        source = self.source
        index = self.index
        if source.startswith('br"', index):
            return 2, "bytes", True
        if source.startswith('cr"', index):
            return 2, "c-bytes", True
        if source.startswith('r"', index):
            return 1, "string", True
        if source.startswith('b"', index):
            return 1, "bytes", False
        if source.startswith('c"', index):
            return 1, "c-bytes", False
        if source.startswith('"', index):
            return 0, "string", False
        return None

    def _scan_rune(self) -> None:
        start = self.index
        start_line = self.line
        self.index += 1
        while self.index < len(self.source):
            char = self.source[self.index]
            if char == "\\":
                self.index = min(self.index + 2, len(self.source))
            elif char == "'":
                self.index += 1
                return
            elif char in "\r\n":
                self._error(start, start_line, "newline in rune literal")
                return
            else:
                self.index += 1
        self._error(start, start_line, "unterminated rune literal")

    def _scan_backtick(self) -> None:
        start = self.index
        start_line = self.line
        self.index += 1
        while self.index < len(self.source):
            char = self.source[self.index]
            if char == "\\":
                self.index = min(self.index + 2, len(self.source))
            elif char == "`":
                self.index += 1
                return
            elif char == "\n":
                self._advance_newline()
            else:
                self.index += 1
        self._error(start, start_line, "unterminated backtick literal")

    def _scan_quoted(self, prefix_length: int, kind: str, raw: bool) -> None:
        start = self.index
        start_line = self.line
        self.index += prefix_length
        quote_start = self.index
        self.index += 1
        quote_count = 1
        while self.index < len(self.source) and self.source[self.index] == '"':
            quote_count += 1
            self.index += 1
        if quote_count == 2:
            self.tokens.append(Token("string", self.source[start:self.index], start, self.index,
                                     start_line))
            return

        has_interpolation = False
        if quote_count >= 3:
            if self.index >= len(self.source) or self.source[self.index] not in "\r\n":
                self._error(start, start_line,
                            "block quoted literal opener is not followed by newline")
                return
            if self.source.startswith("\r\n", self.index):
                self.index += 2
                self.line += 1
            else:
                self._advance_newline()
            while self.index < len(self.source):
                line_start = self.index == 0 or self.source[self.index - 1] == "\n"
                if line_start:
                    probe = self.index
                    while probe < len(self.source) and self.source[probe] in " \t":
                        probe += 1
                    if self.source.startswith('"' * quote_count, probe):
                        after = probe + quote_count
                        if after == len(self.source) or self.source[after] in "\r\n":
                            self.index = after
                            if not has_interpolation:
                                self.tokens.append(Token("string", self.source[start:self.index],
                                                         start, self.index, start_line))
                            return
                        self.index = after
                        self._error(start, start_line,
                                    "block quoted literal closer has trailing tokens")
                        return
                if kind == "string" and self.source.startswith("${", self.index):
                    has_interpolation = True
                    self.index += 2
                    self._scan_tokens(stop_at_interpolation_end=True)
                    continue
                char = self.source[self.index]
                if char == "\\" and not raw:
                    self.index = min(self.index + 2, len(self.source))
                elif char == "\n":
                    self._advance_newline()
                else:
                    self.index += 1
            self._error(start, start_line, "unterminated block quoted literal")
            return

        while self.index < len(self.source):
            char = self.source[self.index]
            if char == '"':
                self.index += 1
                if not has_interpolation:
                    self.tokens.append(Token("string", self.source[start:self.index], start,
                                             self.index, start_line))
                return
            if char in "\r\n":
                self._error(start, start_line, "newline in inline quoted literal")
                return
            if char == "\\" and not raw:
                self.index = min(self.index + 2, len(self.source))
                continue
            if kind == "string" and self.source.startswith("${", self.index):
                has_interpolation = True
                self.index += 2
                self._scan_tokens(stop_at_interpolation_end=True)
                continue
            self.index += 1
        self._error(start, start_line, f"unterminated quoted literal at offset {quote_start}")

    def _scan_tokens(self, stop_at_interpolation_end: bool) -> None:
        interpolation_braces = 0
        while self.index < len(self.source):
            char = self.source[self.index]
            if stop_at_interpolation_end and char == "}" and interpolation_braces == 0:
                self.index += 1
                return
            if char in " \t\r":
                self.index += 1
                continue
            if char == "\n":
                self._advance_newline()
                continue
            if self.source.startswith("//", self.index):
                self._skip_line_comment()
                continue
            if self.source.startswith("/*", self.index):
                self._skip_block_comment()
                continue
            quoted = self._quoted_prefix()
            if quoted is not None:
                self._scan_quoted(*quoted)
                continue
            if char == "'":
                self._scan_rune()
                continue
            if char == "`":
                self._scan_backtick()
                continue
            if _identifier_start(char):
                start = self.index
                start_line = self.line
                self.index += 1
                while self.index < len(self.source) and _identifier_continue(
                    self.source[self.index]
                ):
                    self.index += 1
                self.tokens.append(Token("identifier", self.source[start:self.index], start,
                                         self.index, start_line))
                continue
            if self.source.startswith("->", self.index):
                self.tokens.append(Token("punctuation", "->", self.index, self.index + 2,
                                         self.line))
                self.index += 2
                continue
            self.tokens.append(Token("punctuation", char, self.index, self.index + 1,
                                     self.line))
            if stop_at_interpolation_end:
                if char == "{":
                    interpolation_braces += 1
                elif char == "}":
                    interpolation_braces -= 1
            self.index += 1
        if stop_at_interpolation_end:
            self._error(self.index, self.line, "unterminated quoted interpolation")


def lex(source: str) -> list[Token]:
    return XrayLexer(source).scan()


def matching_pairs(tokens: list[Token], opening: str, closing: str) -> dict[int, int]:
    pairs: dict[int, int] = {}
    stack: list[int] = []
    for index, token in enumerate(tokens):
        if token.spelling == opening:
            stack.append(index)
        elif token.spelling == closing and stack:
            pairs[stack.pop()] = index
    return pairs


def split_payload_fields(tokens: list[Token], begin: int, end: int) -> list[list[Token]]:
    fields: list[list[Token]] = []
    current: list[Token] = []
    stack: list[str] = []
    closing = {"(": ")", "[": "]", "{": "}", "<": ">"}
    for token in tokens[begin:end]:
        spelling = token.spelling
        if spelling in closing:
            stack.append(closing[spelling])
        elif stack and spelling == stack[-1]:
            stack.pop()
        if spelling == "," and not stack:
            if current:
                fields.append(current)
            current = []
        else:
            current.append(token)
    if current:
        fields.append(current)
    return fields


def field_is_named(field: list[Token]) -> bool:
    stack: list[str] = []
    closing = {"(": ")", "[": "]", "{": "}", "<": ">"}
    for token in field:
        spelling = token.spelling
        if spelling in closing:
            stack.append(closing[spelling])
        elif stack and spelling == stack[-1]:
            stack.pop()
        elif spelling == ":" and not stack:
            return True
    return False


def collect_declarations(
    token_sets: dict[str, list[Token]],
) -> tuple[list[PayloadDeclaration], dict[str, list[tuple[int, int]]], int]:
    declarations: list[PayloadDeclaration] = []
    declaration_ranges: dict[str, list[tuple[int, int]]] = {}
    enum_count = 0
    for path, tokens in token_sets.items():
        parens = matching_pairs(tokens, "(", ")")
        braces = matching_pairs(tokens, "{", "}")
        ranges: list[tuple[int, int]] = []
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
            if body not in braces:
                index += 1
                continue
            body_end = braces[body]
            ranges.append((tokens[index].start, tokens[body_end].end))
            depth = 1
            cursor = body + 1
            while cursor < body_end:
                spelling = tokens[cursor].spelling
                if spelling == "{":
                    depth += 1
                elif spelling == "}":
                    depth -= 1
                elif (
                    depth == 1
                    and tokens[cursor].kind == "identifier"
                    and cursor + 1 < body_end
                    and tokens[cursor + 1].spelling == "("
                    and tokens[cursor - 1].spelling not in (".", "fn")
                    and cursor + 1 in parens
                ):
                    close = parens[cursor + 1]
                    fields = split_payload_fields(tokens, cursor + 2, close)
                    named_count = sum(field_is_named(field) for field in fields)
                    variant = tokens[cursor].spelling
                    declarations.append(
                        PayloadDeclaration(
                            path=path,
                            line=tokens[cursor].line,
                            enum=enum_name,
                            variant=variant,
                            field_count=len(fields),
                            named_count=named_count,
                            unnamed_count=len(fields) - named_count,
                        )
                    )
                    cursor = close
                cursor += 1
            index = body_end + 1
        declaration_ranges[path] = ranges
    return declarations, declaration_ranges, enum_count


def _literal_value(token: Token) -> str | None:
    if token.kind != "string":
        return None
    spelling = token.spelling
    quote = spelling.find('"')
    if quote < 0 or not spelling.endswith('"'):
        return None
    return spelling[quote + 1:-1]


def _resolve_module_path(
    importing_path: str, target: str, sources: dict[str, str]
) -> str | None:
    candidates: list[str] = []
    if target.startswith("."):
        base = posixpath.normpath(posixpath.join(posixpath.dirname(importing_path), target))
        candidates.extend((base, f"{base}.xr", f"{base}/index.xr"))
    else:
        candidates.extend(
            (
                f"stdlib/{target}/{target}.xr",
                f"stdlib/{target}.xr",
                posixpath.normpath(posixpath.join(posixpath.dirname(importing_path), target)),
                posixpath.normpath(
                    posixpath.join(posixpath.dirname(importing_path), f"{target}.xr")
                ),
            )
        )
    for candidate in candidates:
        if candidate in sources:
            return candidate
    return None


def _split_binding_items(tokens: list[Token], begin: int, end: int) -> list[list[Token]]:
    items: list[list[Token]] = []
    current: list[Token] = []
    for token in tokens[begin:end]:
        if token.spelling == ",":
            if current:
                items.append(current)
            current = []
        else:
            current.append(token)
    if current:
        items.append(current)
    return items


def collect_module_bindings(
    sources: dict[str, str], token_sets: dict[str, list[Token]]
) -> tuple[dict[str, dict[str, ImportBinding]], dict[str, dict[str, ImportBinding]]]:
    imports: dict[str, dict[str, ImportBinding]] = {}
    reexports: dict[str, dict[str, ImportBinding]] = {}
    for path, tokens in token_sets.items():
        braces = matching_pairs(tokens, "{", "}")
        path_imports: dict[str, ImportBinding] = {}
        path_reexports: dict[str, ImportBinding] = {}
        index = 0
        while index < len(tokens):
            is_reexport = tokens[index].spelling == "export"
            if is_reexport:
                binding_open = index + 1
            elif tokens[index].spelling == "import":
                binding_open = index + 1
            else:
                index += 1
                continue
            if binding_open >= len(tokens) or tokens[binding_open].spelling != "{":
                index += 1
                continue
            close = braces.get(binding_open)
            if close is None or close + 2 >= len(tokens) or tokens[close + 1].spelling != "from":
                index += 1
                continue
            target_token = tokens[close + 2]
            target_name = _literal_value(target_token)
            if target_name is None and target_token.kind == "identifier":
                target_name = target_token.spelling
            if target_name is None:
                index = close + 1
                continue
            target_path = _resolve_module_path(path, target_name, sources)
            if target_path is None:
                index = close + 1
                continue
            destination = path_reexports if is_reexport else path_imports
            for item in _split_binding_items(tokens, binding_open + 1, close):
                names = [token.spelling for token in item if token.kind == "identifier"]
                if not names:
                    continue
                remote_name = names[0]
                local_name = names[-1] if len(names) >= 3 and names[-2] == "as" else remote_name
                destination[local_name] = ImportBinding(
                    target_path=target_path,
                    target_name=remote_name,
                    resolution="reexport" if is_reexport else "import",
                )
            index = close + 3
        imports[path] = path_imports
        reexports[path] = path_reexports
    return imports, reexports


def collect_non_enum_types(token_sets: dict[str, list[Token]]) -> dict[str, set[str]]:
    result: dict[str, set[str]] = {}
    kinds = {"class", "struct", "interface", "union", "type"}
    for path, tokens in token_sets.items():
        names: set[str] = set()
        for index in range(len(tokens) - 1):
            if tokens[index].spelling in kinds and tokens[index + 1].kind == "identifier":
                names.add(tokens[index + 1].spelling)
        result[path] = names
    return result


def collect_exported_enum_names(token_sets: dict[str, list[Token]]) -> dict[str, set[str]]:
    result: dict[str, set[str]] = {}
    for path, tokens in token_sets.items():
        result[path] = {
            tokens[index + 2].spelling
            for index in range(len(tokens) - 2)
            if tokens[index].spelling == "export"
            and tokens[index + 1].spelling == "enum"
            and tokens[index + 2].kind == "identifier"
        }
    return result


def collect_value_shadows(token_sets: dict[str, list[Token]]) -> dict[str, dict[str, list[int]]]:
    result: dict[str, dict[str, list[int]]] = {}
    for path, tokens in token_sets.items():
        names: dict[str, list[int]] = {}
        for index in range(len(tokens) - 1):
            if tokens[index].spelling in ("var", "const") and tokens[index + 1].kind == "identifier":
                names.setdefault(tokens[index + 1].spelling, []).append(tokens[index + 1].start)
        result[path] = names
    return result


def collect_builtin_enum_names(text: str) -> set[str]:
    return set(re.findall(r'XR_BUILTIN_ENUM\(\s*"([^"]+)"', text))


class ModuleResolver:
    def __init__(
        self,
        sources: dict[str, str],
        token_sets: dict[str, list[Token]],
        declarations: list[PayloadDeclaration],
        builtin_enum_names: set[str],
    ):
        grouped: dict[tuple[str, str], list[PayloadDeclaration]] = {}
        for declaration in declarations:
            grouped.setdefault((declaration.path, declaration.enum), []).append(declaration)
        self.definitions: dict[str, dict[str, EnumDefinition]] = {}
        self.definitions_by_name: dict[str, list[EnumDefinition]] = {}
        for (path, enum_name), rows in grouped.items():
            definition = EnumDefinition(
                path=path,
                line=min(row.line for row in rows),
                enum=enum_name,
                variants=tuple(sorted((row.variant, row.line) for row in rows)),
            )
            self.definitions.setdefault(path, {})[enum_name] = definition
            self.definitions_by_name.setdefault(enum_name, []).append(definition)
        self.imports, self.reexports = collect_module_bindings(sources, token_sets)
        self.exported_enum_names = collect_exported_enum_names(token_sets)
        self.non_enum_types = collect_non_enum_types(token_sets)
        self.value_shadows = collect_value_shadows(token_sets)
        self.builtin_enum_names = builtin_enum_names

    def _resolve_export(
        self, path: str, name: str, seen: set[tuple[str, str]]
    ) -> tuple[EnumDefinition | None, str]:
        key = (path, name)
        if key in seen:
            return None, "reexport-cycle"
        seen.add(key)
        local = self.definitions.get(path, {}).get(name)
        if local is not None and name in self.exported_enum_names.get(path, set()):
            return local, "module-export"
        binding = self.reexports.get(path, {}).get(name)
        if binding is None:
            return None, "missing-export"
        resolved, resolution = self._resolve_export(
            binding.target_path, binding.target_name, seen
        )
        return resolved, f"reexport->{resolution}"

    def resolve(
        self, path: str, name: str, use_start: int
    ) -> tuple[EnumDefinition | None, str]:
        if any(offset < use_start for offset in self.value_shadows.get(path, {}).get(name, ())):
            return None, "value-shadow"
        local = self.definitions.get(path, {}).get(name)
        if local is not None:
            return local, "same-module"
        if name in self.non_enum_types.get(path, set()):
            return None, "non-enum-type-shadow"
        binding = self.imports.get(path, {}).get(name)
        if binding is not None:
            resolved, resolution = self._resolve_export(
                binding.target_path, binding.target_name, set()
            )
            return resolved, f"import->{resolution}"
        if name in self.builtin_enum_names:
            builtin = [
                definition
                for definition in self.definitions_by_name.get(name, ())
                if definition.path.startswith("stdlib/")
            ]
            if len(builtin) == 1:
                return builtin[0], "builtin-symbol"
            return None, "ambiguous-builtin"
        return None, "unresolved-module-name"


def classify_use_role(tokens: list[Token], index: int, close: int) -> str:
    next_index = close + 1
    role = "constructor"
    if next_index < len(tokens) and tokens[next_index].spelling == "->":
        return "pattern-direct"
    if next_index < len(tokens) and tokens[next_index].spelling == "if":
        cursor = next_index + 1
        depth = 0
        while cursor < len(tokens):
            spelling = tokens[cursor].spelling
            if spelling in ("(", "[", "{"):
                depth += 1
            elif spelling in (")", "]", "}"):
                if depth == 0:
                    break
                depth -= 1
            if depth == 0 and spelling == "->":
                return "pattern-guard"
            if depth == 0 and spelling == ";":
                break
            cursor += 1
    if next_index < len(tokens) and tokens[next_index].spelling == "{":
        cursor = index - 1
        while cursor >= 0 and tokens[cursor].spelling not in (
            "catch", ";", "{", "}", "->"
        ):
            cursor -= 1
        if cursor >= 0 and tokens[cursor].spelling == "catch":
            role = "pattern-catch"
    return role


def mark_nested_patterns(uses: list[QualifiedUse]) -> None:
    changed = True
    while changed:
        changed = False
        for inner in uses:
            if inner.role != "constructor":
                continue
            for outer in uses:
                if (
                    inner.path == outer.path
                    and outer.role.startswith("pattern-")
                    and outer.start < inner.start
                    and inner.end < outer.end
                ):
                    inner.role = "pattern-nested"
                    changed = True
                    break


def collect_qualified_uses(
    token_sets: dict[str, list[Token]],
    declaration_ranges: dict[str, list[tuple[int, int]]],
    resolver: ModuleResolver,
) -> tuple[list[QualifiedUse], Counter[str]]:
    uses: list[QualifiedUse] = []
    excluded: Counter[str] = Counter()
    for path, tokens in token_sets.items():
        parens = matching_pairs(tokens, "(", ")")
        for index in range(len(tokens) - 3):
            if not (
                tokens[index].kind == "identifier"
                and tokens[index + 1].spelling == "."
                and tokens[index + 2].kind == "identifier"
                and tokens[index + 3].spelling == "("
                and index + 3 in parens
            ):
                continue
            start = tokens[index].start
            if any(begin <= start < end for begin, end in declaration_ranges[path]):
                continue
            enum_name = tokens[index].spelling
            variant_name = tokens[index + 2].spelling
            definition, resolution = resolver.resolve(path, enum_name, start)
            if definition is None:
                excluded[resolution] += 1
                continue
            variant_line = definition.variant_line(variant_name)
            if variant_line is None:
                excluded["resolved-enum-nonpayload-member"] += 1
                continue
            close = parens[index + 3]
            role = classify_use_role(tokens, index, close)
            uses.append(
                QualifiedUse(
                    path=path,
                    line=tokens[index].line,
                    enum=enum_name,
                    variant=variant_name,
                    start=start,
                    end=tokens[close].end,
                    role=role,
                    declaration_path=definition.path,
                    declaration_line=variant_line,
                    resolution=resolution,
                )
            )

    mark_nested_patterns(uses)
    return uses, excluded


def collect_use_candidate_frontier(
    sources: dict[str, str],
    token_sets: dict[str, list[Token]],
    declarations: list[PayloadDeclaration],
    declaration_ranges: dict[str, list[tuple[int, int]]],
    resolver: ModuleResolver,
    tree: str,
) -> list[UseCandidate]:
    """Return the syntax-only frontier and non-authoritative review suggestions."""
    accepted, _ = collect_qualified_uses(token_sets, declaration_ranges, resolver)
    accepted_by_span = {(row.path, row.start, row.end): row for row in accepted}
    payload_variant_names = {row.variant for row in declarations}
    candidates: list[UseCandidate] = []
    for path, tokens in token_sets.items():
        parens = matching_pairs(tokens, "(", ")")
        for index in range(len(tokens) - 3):
            if not (
                tokens[index].kind == "identifier"
                and tokens[index + 1].spelling == "."
                and tokens[index + 2].kind == "identifier"
                and tokens[index + 2].spelling in payload_variant_names
                and tokens[index + 3].spelling == "("
                and index + 3 in parens
            ):
                continue
            start = tokens[index].start
            if any(begin <= start < end for begin, end in declaration_ranges[path]):
                continue
            close = parens[index + 3]
            end = tokens[close].end
            accepted_use = accepted_by_span.get((path, start, end))
            definition, reason = resolver.resolve(path, tokens[index].spelling, start)
            variant_line = (
                definition.variant_line(tokens[index + 2].spelling)
                if definition is not None
                else None
            )
            if accepted_use is not None:
                disposition = "ACCEPT"
                declaration_path = accepted_use.declaration_path
                declaration_line = accepted_use.declaration_line
                enum_name = definition.enum if definition is not None else accepted_use.enum
                resolution = accepted_use.resolution
                evidence = (
                    f"suggested payload declaration {declaration_path}:{declaration_line}"
                )
                role = accepted_use.role
            else:
                disposition = "REJECT"
                declaration_path = "-"
                declaration_line = 0
                enum_name = "-"
                if definition is not None and variant_line is None:
                    reason = "resolved-enum-nonpayload-member"
                resolution = f"suggested-reject:{reason}"
                if reason == "non-enum-type-shadow":
                    evidence = "suggested class static call; receiver is a non-enum type in this module"
                elif reason == "resolved-enum-nonpayload-member":
                    evidence = "suggested negative fixture uses a nonexistent payload variant"
                elif reason == "unresolved-module-name":
                    evidence = "suggested namespace-imported class constructor; receiver is a module alias"
                else:
                    evidence = "suggested same-spelling call has no payload-enum identity"
                role = classify_use_role(tokens, index, close)
            source_slice = sources[path][start:end]
            candidates.append(
                UseCandidate(
                    path=path,
                    line=tokens[index].line,
                    receiver=tokens[index].spelling,
                    variant=tokens[index + 2].spelling,
                    start=start,
                    end=end,
                    role=role,
                    disposition=disposition,
                    declaration_path=declaration_path,
                    declaration_line=declaration_line,
                    enum=enum_name,
                    resolution=resolution,
                    review="unreviewed",
                    evidence=evidence,
                    source_sha256=hashlib.sha256(source_slice.encode("utf-8")).hexdigest(),
                )
            )
    return sorted(candidates, key=lambda row: (row.path, row.start, row.end))


def provenance_rows(candidates: list[UseCandidate], tree: str) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for candidate in candidates:
        row = {
            column: str(getattr(candidate, column))
            for column in PROVENANCE_COLUMNS
            if column not in ("tree", "start_codepoint", "end_codepoint")
        }
        row["tree"] = tree
        row["start_codepoint"] = str(candidate.start)
        row["end_codepoint"] = str(candidate.end)
        rows.append(row)
    return rows


def emit_provenance(candidates: list[UseCandidate], tree: str) -> str:
    output = io.StringIO()
    writer = csv.DictWriter(output, fieldnames=PROVENANCE_COLUMNS, delimiter="\t",
                            lineterminator="\n")
    writer.writeheader()
    writer.writerows(provenance_rows(candidates, tree))
    return output.getvalue()


def validate_reviewed_decision(
    row_number: int,
    row: dict[str, str],
    candidate: UseCandidate,
    declaration_keys: set[tuple[str, int, str, str]],
) -> tuple[list[str], QualifiedUse | None, str | None]:
    errors: list[str] = []
    accepted: QualifiedUse | None = None
    rejected_reason: str | None = None
    if row.get("review") != "reviewed" or not row.get("evidence"):
        errors.append(f"row {row_number}: missing reviewed disposition evidence")
    disposition = row.get("disposition")
    if disposition == "ACCEPT":
        try:
            declaration_line = int(row["declaration_line"])
        except (KeyError, TypeError, ValueError):
            errors.append(f"row {row_number}: invalid accepted declaration line")
            return errors, None, None
        declaration_key = (
            row.get("declaration_path"), declaration_line, row.get("enum"), row.get("variant")
        )
        if declaration_key not in declaration_keys:
            errors.append(
                f"row {row_number}: accepted use has no exact payload declaration {declaration_key}"
            )
        elif row.get("resolution") not in ACCEPTED_PROVENANCE_CLASSES:
            errors.append(
                f"row {row_number}: invalid accepted provenance class {row.get('resolution')!r}"
            )
        else:
            accepted = QualifiedUse(
                path=candidate.path,
                line=candidate.line,
                enum=row["enum"],
                variant=candidate.variant,
                start=candidate.start,
                end=candidate.end,
                role=candidate.role,
                declaration_path=row["declaration_path"],
                declaration_line=declaration_line,
                resolution=row["resolution"],
            )
    elif disposition == "REJECT":
        if (
            row.get("declaration_path") != "-"
            or row.get("declaration_line") != "0"
            or row.get("enum") != "-"
        ):
            errors.append(f"row {row_number}: rejected use retains declaration identity")
        resolution = row.get("resolution", "")
        if resolution not in REJECTED_PROVENANCE_CLASSES:
            errors.append(
                f"row {row_number}: invalid rejected provenance class {resolution!r}"
            )
        else:
            rejected_reason = resolution.removeprefix("reviewed-reject:")
    else:
        errors.append(f"row {row_number}: invalid disposition {disposition!r}")
    return errors, accepted, rejected_reason


def validate_provenance(
    root: Path, revision: str, provenance_path: Path
) -> dict[str, object]:
    resolved = git_output(root, "rev-parse", revision).strip()
    tree = git_output(root, "rev-parse", f"{resolved}^{{tree}}").strip()
    sources = load_sources(root, resolved)
    token_sets = {path: lex(source) for path, source in sources.items()}
    declarations, declaration_ranges, _ = collect_declarations(token_sets)
    builtin_text = load_revision_text(root, resolved, "stdlib/prelude/builtin_symbols.def")
    resolver = ModuleResolver(
        sources, token_sets, declarations, collect_builtin_enum_names(builtin_text)
    )
    frontier = collect_use_candidate_frontier(
        sources, token_sets, declarations, declaration_ranges, resolver, tree
    )
    expected_by_span = {(row.path, row.start, row.end): row for row in frontier}
    declaration_keys = {
        (row.path, row.line, row.enum, row.variant) for row in declarations
    }
    with provenance_path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream, delimiter="\t")
        actual_rows = list(reader)
        columns = tuple(reader.fieldnames or ())
    errors: list[str] = []
    expected_oracle_digest = FROZEN_PROVENANCE_SHA256_BY_TREE.get(tree)
    actual_oracle_digest = hashlib.sha256(provenance_path.read_bytes()).hexdigest()
    if expected_oracle_digest is not None and actual_oracle_digest != expected_oracle_digest:
        errors.append(
            f"reviewed oracle sha256 {actual_oracle_digest} != frozen {expected_oracle_digest}"
        )
    if columns != PROVENANCE_COLUMNS:
        errors.append(f"unexpected provenance columns: {columns}")
    seen: set[tuple[str, int, int]] = set()
    accepted: list[QualifiedUse] = []
    rejected_reasons: Counter[str] = Counter()
    for row_number, row in enumerate(actual_rows, 2):
        try:
            key = (
                row["path"], int(row["start_codepoint"]), int(row["end_codepoint"])
            )
            line = int(row["line"])
        except (KeyError, TypeError, ValueError):
            errors.append(f"row {row_number}: invalid source-span identity")
            continue
        if key in seen:
            errors.append(f"row {row_number}: duplicate source-span identity {key}")
            continue
        seen.add(key)
        candidate = expected_by_span.get(key)
        if candidate is None:
            errors.append(f"row {row_number}: source span is not in the syntax frontier {key}")
            continue
        exact_fields = {
            "tree": tree,
            "path": candidate.path,
            "start_codepoint": str(candidate.start),
            "end_codepoint": str(candidate.end),
            "line": str(candidate.line),
            "receiver": candidate.receiver,
            "variant": candidate.variant,
            "role": candidate.role,
            "source_sha256": candidate.source_sha256,
        }
        for field, expected in exact_fields.items():
            if row.get(field) != expected:
                errors.append(
                    f"row {row_number}: {field} {row.get(field)!r} != exact source {expected!r}"
                )
        if line != candidate.line:
            continue
        decision_errors, accepted_use, rejected_reason = validate_reviewed_decision(
            row_number, row, candidate, declaration_keys
        )
        errors.extend(decision_errors)
        if accepted_use is not None:
            accepted.append(accepted_use)
        if rejected_reason is not None:
            rejected_reasons[rejected_reason] += 1
    missing = sorted(set(expected_by_span) - seen)
    if missing:
        errors.append(f"provenance is missing {len(missing)} syntax-frontier spans")
    lexical_error_paths = {
        path for path, tokens in token_sets.items()
        if any(token.kind == "error" for token in tokens)
    }
    accepted_error_paths = sorted(lexical_error_paths & {row.path for row in accepted})
    declaration_error_paths = sorted(
        lexical_error_paths & {row.path for row in declarations if row.unnamed_count}
    )
    if accepted_error_paths:
        errors.append(
            "accepted uses overlap lexically invalid files: " + ", ".join(accepted_error_paths)
        )
    if declaration_error_paths:
        errors.append(
            "positional declarations overlap lexically invalid files: "
            + ", ".join(declaration_error_paths)
        )
    if errors:
        raise SystemExit("enum provenance validation failed:\n  " + "\n  ".join(errors))
    accepted_identity = hashlib.sha256()
    for row in sorted(accepted, key=lambda item: (item.path, item.start, item.end)):
        accepted_identity.update(
            f"{row.path}\t{row.start}\t{row.end}\t{row.declaration_path}\t"
            f"{row.declaration_line}\t{row.enum}\t{row.variant}\n".encode("utf-8")
        )
    return {
        "frontier_count": len(frontier),
        "accepted_count": len(accepted),
        "rejected_count": len(frontier) - len(accepted),
        "rejected_reasons": dict(sorted(rejected_reasons.items())),
        "accepted_identity_sha256": accepted_identity.hexdigest(),
        "accepted_uses": accepted,
    }


def category(path: str) -> str:
    return path.split("/", 1)[0]


def make_report_from_sources(
    sources: dict[str, str],
    revision: str,
    tree: str,
    builtin_enum_names: set[str],
) -> dict[str, object]:
    token_sets = {path: lex(source) for path, source in sources.items()}
    declarations, declaration_ranges, enum_count = collect_declarations(token_sets)
    resolver = ModuleResolver(sources, token_sets, declarations, builtin_enum_names)
    uses, excluded = collect_qualified_uses(token_sets, declaration_ranges, resolver)
    positional = [row for row in declarations if row.unnamed_count]
    fully_named = [row for row in declarations if not row.unnamed_count]
    role_counts = Counter(row.role for row in uses)
    resolution_counts = Counter(row.resolution for row in uses)
    root_counts = Counter(category(path) for path in sources)
    positional_roots = Counter(category(row.path) for row in positional)
    constructor_count = role_counts["constructor"]
    return {
        "schema_version": 2,
        "revision": revision,
        "tree": tree,
        "roots": list(SCAN_ROOTS),
        "file_count": len(sources),
        "file_count_by_root": dict(sorted(root_counts.items())),
        "enum_declaration_count": enum_count,
        "payload_variant_count": len(declarations),
        "positional_or_mixed_variant_count": len(positional),
        "positional_or_mixed_by_root": dict(sorted(positional_roots.items())),
        "fully_named_variant_count": len(fully_named),
        "payload_field_count": sum(row.field_count for row in declarations),
        "named_field_count": sum(row.named_count for row in declarations),
        "unnamed_field_count": sum(row.unnamed_count for row in declarations),
        "qualified_use_count": len(uses),
        "constructor_count": constructor_count,
        "pattern_count": len(uses) - constructor_count,
        "use_roles": dict(sorted(role_counts.items())),
        "use_resolutions": dict(sorted(resolution_counts.items())),
        "excluded_qualified_call_candidates": dict(sorted(excluded.items())),
        "lexical_error_count": sum(
            token.kind == "error" for tokens in token_sets.values() for token in tokens
        ),
        "lexical_error_files": sorted(
            path for path, tokens in token_sets.items()
            if any(token.kind == "error" for token in tokens)
        ),
        "payload_declarations": [asdict(row) for row in declarations],
        "positional_or_mixed_declarations": [asdict(row) for row in positional],
        "qualified_uses": [asdict(row) for row in uses],
    }


def make_report(root: Path, revision: str) -> dict[str, object]:
    resolved_revision = git_output(root, "rev-parse", revision).strip()
    tree = git_output(root, "rev-parse", f"{resolved_revision}^{{tree}}").strip()
    sources = load_sources(root, resolved_revision)
    builtin_text = load_revision_text(root, resolved_revision,
                                      "stdlib/prelude/builtin_symbols.def")
    return make_report_from_sources(
        sources,
        resolved_revision,
        tree,
        collect_builtin_enum_names(builtin_text),
    )


def validate_proposed_fields(fields_text: str, reserved_words: set[str]) -> list[str]:
    errors: list[str] = []
    fields = fields_text.split(",")
    if not fields or any(not field for field in fields):
        errors.append("proposed field list contains an empty item")
    for field in fields:
        if field != field.strip():
            errors.append(f"proposed field {field!r} contains surrounding whitespace")
            continue
        field_tokens = lex(field)
        is_exact_identifier = (
            len(field_tokens) == 1
            and field_tokens[0].kind == "identifier"
            and field_tokens[0].start == 0
            and field_tokens[0].end == len(field)
        )
        if field == "_" or not is_exact_identifier:
            errors.append(f"proposed field {field!r} is not a legal identifier")
        elif field in reserved_words:
            errors.append(f"proposed field {field!r} is reserved")
        if re.fullmatch(r"value[0-9]+", field):
            errors.append(f"proposed field {field!r} is a numbered placeholder")
    if len(fields) != len(set(fields)):
        errors.append("proposed field names are not unique")
    return errors


def validate_manifest(
    report: dict[str, object], manifest_path: Path, reserved_words: set[str]
) -> None:
    declaration_rows = report["positional_or_mixed_declarations"]
    expected = {
        (row["path"], int(row["line"]), row["enum"], row["variant"]): int(row["field_count"])
        for row in declaration_rows
    }
    actual: dict[tuple[str, int, str, str], int] = {}
    errors: list[str] = []
    with manifest_path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream, delimiter="\t")
        required = {"path", "line", "enum", "variant", "proposed_fields", "review", "evidence"}
        if set(reader.fieldnames or ()) != required:
            errors.append(f"unexpected manifest columns: {reader.fieldnames}")
        for row_number, row in enumerate(reader, 2):
            try:
                key = (row["path"], int(row["line"]), row["enum"], row["variant"])
            except (KeyError, TypeError, ValueError):
                errors.append(f"row {row_number}: invalid declaration identity")
                continue
            if key in actual:
                errors.append(f"row {row_number}: duplicate declaration identity {key}")
                continue
            fields = row["proposed_fields"].split(",")
            actual[key] = len(fields)
            errors.extend(
                f"row {row_number}: {error}"
                for error in validate_proposed_fields(row["proposed_fields"], reserved_words)
            )
            if row["review"] != "confirmed":
                errors.append(f"row {row_number}: invalid review state {row['review']!r}")
            if not row["evidence"]:
                errors.append(f"row {row_number}: evidence is empty")

    for key, expected_count in expected.items():
        if key not in actual:
            errors.append(f"manifest is missing declaration {key}")
        elif actual[key] != expected_count:
            errors.append(
                f"manifest field count for {key} is {actual[key]}, expected {expected_count}"
            )
    for key in actual.keys() - expected.keys():
        errors.append(f"manifest contains unknown declaration {key}")
    if errors:
        raise SystemExit("manifest validation failed:\n  " + "\n  ".join(errors))


def run_self_tests(reserved_words: set[str]) -> None:
    def fixture_candidates(
        sources: dict[str, str]
    ) -> tuple[list[UseCandidate], list[PayloadDeclaration]]:
        token_sets = {path: lex(source) for path, source in sources.items()}
        declarations, declaration_ranges, _ = collect_declarations(token_sets)
        resolver = ModuleResolver(sources, token_sets, declarations, set())
        return (
            collect_use_candidate_frontier(
                sources, token_sets, declarations, declaration_ranges, resolver, "fixture-tree"
            ),
            declarations,
        )

    cross_collision = {
        "tests/fixture/enum_lib.xr": "enum E { A(i64) }\n",
        "tests/fixture/main.xr": (
            "class E { static A(value: i64) -> i64 { return value } }\n"
            "print(E.A(1))\n"
        ),
    }
    report = make_report_from_sources(cross_collision, "fixture", "fixture-tree", set())
    if report["qualified_use_count"] != 0:
        raise AssertionError("cross-file enum/class spelling collision was accepted")

    alias_fixture = {
        "tests/fixture/dep.xr": "export enum E { A(i64) }\n",
        "tests/fixture/main.xr": (
            'import { E as Alias } from "./dep"\n'
            "print(Alias.A(1))\n"
            "print(E.A(2))\n"
        ),
    }
    report = make_report_from_sources(alias_fixture, "fixture", "fixture-tree", set())
    if report["qualified_use_count"] != 1:
        raise AssertionError("import alias provenance did not select exactly one enum use")
    use = report["qualified_uses"][0]
    if use["declaration_path"] != "tests/fixture/dep.xr" or not use["resolution"].startswith(
        "import->"
    ):
        raise AssertionError("import alias did not retain declaration provenance")

    private_import_fixture = {
        "tests/fixture/dep.xr": "enum E { A(i64) }\n",
        "tests/fixture/main.xr": (
            'import { E as Alias } from "./dep"\n'
            "print(Alias.A(1))\n"
        ),
    }
    report = make_report_from_sources(
        private_import_fixture, "fixture", "fixture-tree", set()
    )
    if report["qualified_use_count"] != 0:
        raise AssertionError("private imported enum was accepted as an exported identity")

    shadow_fixture = {
        "tests/fixture/main.xr": (
            "enum E { A(i64) }\n"
            "fn f() { var E = 1; print(E.A(2)) }\n"
        )
    }
    report = make_report_from_sources(shadow_fixture, "fixture", "fixture-tree", set())
    if report["qualified_use_count"] != 0:
        raise AssertionError("local value shadow did not suppress enum provenance")

    reviewed_scope_fixture = {
        "tests/fixture/main.xr": (
            "enum E { A(i64) }\n"
            "fn parameter(E: i64) { print(E.A(1)) }\n"
            "fn sibling() { var E = 1 }\n"
            "fn afterSibling() { print(E.A(2)) }\n"
            "fn exited() { { var E = 1 } print(E.A(3)) }\n"
            "fn qualified() { print(dep.E.A(4)) }\n"
        )
    }
    scope_candidates, scope_declarations = fixture_candidates(reviewed_scope_fixture)
    scope_by_line = {row.line: row for row in scope_candidates}
    expected_review = {
        2: ("REJECT", None),  # Parameter binding shadows the enum in this function.
        4: ("ACCEPT", ("tests/fixture/main.xr", 1, "E", "A")),
        5: ("ACCEPT", ("tests/fixture/main.xr", 1, "E", "A")),
        6: ("REJECT", None),  # The receiver is module-qualified, not the local enum.
    }
    if set(scope_by_line) != set(expected_review):
        raise AssertionError("scope frontier did not retain every explicit review candidate")
    declaration_keys = {
        (row.path, row.line, row.enum, row.variant) for row in scope_declarations
    }
    for line, (disposition, declaration_key) in expected_review.items():
        candidate = scope_by_line[line]
        row = provenance_rows([candidate], "fixture-tree")[0]
        row["review"] = "reviewed"
        row["evidence"] = "constructed explicit scope review"
        row["disposition"] = disposition
        if disposition == "ACCEPT" and declaration_key is not None:
            row["declaration_path"] = declaration_key[0]
            row["declaration_line"] = str(declaration_key[1])
            row["enum"] = declaration_key[2]
            row["resolution"] = "same-module"
        else:
            row["declaration_path"] = "-"
            row["declaration_line"] = "0"
            row["enum"] = "-"
            row["resolution"] = (
                "reviewed-reject:value-shadow"
                if line == 2
                else "reviewed-reject:module-qualified-receiver"
            )
        decision_errors, accepted_use, rejected_reason = validate_reviewed_decision(
            line, row, candidate, declaration_keys
        )
        if decision_errors:
            raise AssertionError(
                f"explicit scope review failed at line {line}: {decision_errors}"
            )
        if disposition == "ACCEPT" and accepted_use is None:
            raise AssertionError(f"explicit reviewed accept was not materialized at line {line}")
        if disposition == "REJECT" and rejected_reason is None:
            raise AssertionError(f"explicit reviewed reject was not materialized at line {line}")

    distinct_alias_fixture = {
        "tests/fixture/left.xr": "export enum E { V(i64) }\n",
        "tests/fixture/right.xr": "export enum E { V(i64) }\n",
        "tests/fixture/main.xr": (
            'import { E as Left } from "./left"\n'
            'import { E as Right } from "./right"\n'
            "print(Left.V(1))\n"
            "print(Right.V(2))\n"
        ),
    }
    alias_candidates, _ = fixture_candidates(distinct_alias_fixture)
    if (
        len(alias_candidates) != 2
        or {row.declaration_path for row in alias_candidates}
        != {"tests/fixture/left.xr", "tests/fixture/right.xr"}
    ):
        raise AssertionError("distinct import aliases lost exact declaration provenance")

    quoted_fixture = {
        "tests/fixture/main.xr": (
            "enum E { A(i64) }\n"
            'var block = r\"\"\"\nE.A(1)\n\"\"\"\n'
            'var interpolated = "${E.A(2)}"\n'
        )
    }
    report = make_report_from_sources(quoted_fixture, "fixture", "fixture-tree", set())
    if report["qualified_use_count"] != 1:
        raise AssertionError("quoted body or interpolation lexical state is incorrect")

    accepted = validate_proposed_fields("left,right", reserved_words)
    if accepted:
        raise AssertionError(f"valid proposed fields were rejected: {accepted}")
    for mutation in (
        "left,,right", "left-name,right", "left,right name", "9left,right",
        "left.right,next", "match,right", "i64,right", "_,right",
    ):
        if not validate_proposed_fields(mutation, reserved_words):
            raise AssertionError(f"invalid proposed fields were accepted: {mutation!r}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--revision", default="HEAD", help="Git revision to inventory")
    parser.add_argument("--json", action="store_true", help="emit the full machine-readable report")
    parser.add_argument("--manifest", help="validate a field-renaming TSV against the inventory")
    parser.add_argument("--provenance", help="validate the reviewed qualified-use provenance TSV")
    parser.add_argument(
        "--emit-provenance", action="store_true",
        help="emit a non-authoritative candidate TSV for human review",
    )
    parser.add_argument("--self-test", action="store_true", help="run constructed provenance tests")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    resolved_revision = git_output(root, "rev-parse", args.revision).strip()
    reserved_words = load_reserved_words(root, resolved_revision)
    if args.self_test:
        run_self_tests(reserved_words)
        print("enum inventory self-tests: PASS")
    report = make_report(root, resolved_revision)
    if args.emit_provenance:
        sources = load_sources(root, resolved_revision)
        token_sets = {path: lex(source) for path, source in sources.items()}
        declarations, declaration_ranges, _ = collect_declarations(token_sets)
        resolver = ModuleResolver(
            sources,
            token_sets,
            declarations,
            collect_builtin_enum_names(
                load_revision_text(root, resolved_revision, "stdlib/prelude/builtin_symbols.def")
            ),
        )
        candidates = collect_use_candidate_frontier(
            sources, token_sets, declarations, declaration_ranges, resolver, report["tree"]
        )
        print(emit_provenance(candidates, report["tree"]), end="")
        return 0
    if args.manifest:
        validate_manifest(report, Path(args.manifest), reserved_words)
    provenance_report = None
    if args.provenance:
        provenance_report = validate_provenance(
            root, resolved_revision, Path(args.provenance)
        )
        reviewed_uses = provenance_report.pop("accepted_uses")
        role_counts = Counter(row.role for row in reviewed_uses)
        resolution_counts = Counter(row.resolution for row in reviewed_uses)
        report["qualified_uses"] = [asdict(row) for row in reviewed_uses]
        report["qualified_use_count"] = len(reviewed_uses)
        report["constructor_count"] = role_counts["constructor"]
        report["pattern_count"] = len(reviewed_uses) - role_counts["constructor"]
        report["use_roles"] = dict(sorted(role_counts.items()))
        report["use_resolutions"] = dict(sorted(resolution_counts.items()))
        report["reviewed_provenance"] = provenance_report
    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
        return 0
    print(f"revision: {report['revision']}")
    print(f"tree: {report['tree']}")
    print(f".xr files: {report['file_count']}")
    print(f"enum declarations: {report['enum_declaration_count']}")
    print(f"payload variants: {report['payload_variant_count']}")
    print(f"positional or mixed payload variants: {report['positional_or_mixed_variant_count']}")
    print(f"fully named payload variants: {report['fully_named_variant_count']}")
    print(f"payload fields: {report['payload_field_count']}")
    print(f"named payload fields: {report['named_field_count']}")
    print(f"unnamed payload fields: {report['unnamed_field_count']}")
    print(f"old qualified uses: {report['qualified_use_count']}")
    print(f"constructors: {report['constructor_count']}")
    print(f"patterns: {report['pattern_count']}")
    print(f"use roles: {report['use_roles']}")
    print(f"use resolutions: {report['use_resolutions']}")
    print(f"excluded qualified call candidates: {report['excluded_qualified_call_candidates']}")
    if provenance_report is not None:
        print(f"reviewed candidate frontier: {provenance_report['frontier_count']}")
        print(f"reviewed accepts/rejects: {provenance_report['accepted_count']}/"
              f"{provenance_report['rejected_count']}")
        print(f"accepted identity sha256: {provenance_report['accepted_identity_sha256']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
