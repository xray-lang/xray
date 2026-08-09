#!/usr/bin/env python3
"""Parser for ``stdlib/prelude/builtin_symbols.def``.

That file is the single source of truth for every symbol the language provides
without an import. Three tools read it — the API inventory, the specification
generator, and the registry gate — so the parsing lives here once instead of in
three regexes that can disagree about what the registry contains.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

DEF_RELPATH = "stdlib/prelude/builtin_symbols.def"

_PRELUDE_TYPE_RE = re.compile(
    r'XR_BUILTIN_PRELUDE_TYPE\(\s*"(?P<name>[^"]+)"\s*,\s*(?P<arity>\d+)\s*,'
    r'\s*(?P<native>[^,]+?)\s*,\s*(?P<kind>[A-Z0-9_]+)\s*\)'
)
_TYPE_RE = re.compile(r'XR_BUILTIN_TYPE\(\s*"(?P<name>[^"]+)"\s*,\s*(?P<arity>\d+)\s*\)')
_NAMESPACE_TYPE_RE = re.compile(
    r'XR_BUILTIN_NAMESPACE_TYPE\(\s*"(?P<namespace>[^"]+)"\s*,\s*'
    r'"(?P<name>[^"]+)"\s*,\s*(?P<arity>\d+)\s*\)'
)
_IFACE_RE = re.compile(r'XR_BUILTIN_IFACE\(\s*"(?P<name>[^"]+)"\s*,\s*(?P<arity>\d+)\s*\)')
_ENUM_RE = re.compile(
    r'XR_BUILTIN_ENUM\(\s*"(?P<name>[^"]+)"\s*,\s*(?P<arity>\d+)\s*,'
    r'\s*(?P<slot>\w+)\s*,(?P<variants>.*?)\)\s*\n\s*\n',
    re.S,
)
_VARIANT_RE = re.compile(
    r'XR_BUILTIN_ENUM_VARIANT\(\s*"(?P<name>[^"]+)"\s*,\s*(?P<payload>[A-Z0-9_]+)\s*\)'
)
_HINT_RE = re.compile(
    r'XR_BUILTIN_UNDEFINED_HINT\(\s*"(?P<name>[^"]+)"\s*,(?P<text>.*?)\)\s*\n(?=XR_BUILTIN|\n|#undef)',
    re.S,
)
# Definition lines inside the usage header; they declare the macros rather than
# register a symbol.
_MACRO_DEFINITION_LINE = re.compile(r'^\s*#\s*(define|ifndef|undef)\b')


@dataclass(frozen=True)
class Symbol:
    name: str
    category: str  # prelude_type | type | namespace_type | enum | interface | hint
    arity: int = 0
    native_type: str | None = None
    prelude_kind: str | None = None
    variants: tuple[tuple[str, str], ...] = ()
    hint: str | None = None
    line: int = 1

    @property
    def is_language_surface(self) -> bool:
        """Nameable by user code and therefore owed a specification entry."""
        return self.category in {
            "prelude_type",
            "type",
            "namespace_type",
            "enum",
            "interface",
        }

    @property
    def spelling(self) -> str:
        """How the symbol is written in the specification registry table."""
        if self.arity == 1:
            return f"{self.name}<T>"
        if self.arity == 2:
            return f"{self.name}<K, V>"
        return self.name


@dataclass
class Registry:
    path: Path
    symbols: list[Symbol] = field(default_factory=list)

    def by_category(self, category: str) -> list[Symbol]:
        return [s for s in self.symbols if s.category == category]

    @property
    def surface(self) -> list[Symbol]:
        return [s for s in self.symbols if s.is_language_surface]

    def names(self) -> set[str]:
        return {s.name for s in self.symbols}


def _strip_macro_header(text: str) -> str:
    """Drop the preprocessor scaffolding so the fallback ``#define`` bodies do
    not register as symbols."""
    return "\n".join(
        "" if _MACRO_DEFINITION_LINE.match(line) else line for line in text.splitlines()
    )


def _line_of(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def _c_string_literal_join(raw: str) -> str:
    """Concatenate adjacent C string literals into one Python string."""
    return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', raw)).replace('\\"', '"')


def load(root: Path | str = ".") -> Registry:
    root = Path(root)
    path = root / DEF_RELPATH
    raw = path.read_text(encoding="utf-8")
    body = _strip_macro_header(raw)
    registry = Registry(path=path)

    for match in _PRELUDE_TYPE_RE.finditer(body):
        registry.symbols.append(
            Symbol(
                name=match.group("name"),
                category="prelude_type",
                arity=int(match.group("arity")),
                native_type=match.group("native").strip(),
                prelude_kind=match.group("kind"),
                line=_line_of(body, match.start()),
            )
        )
    for match in _TYPE_RE.finditer(body):
        registry.symbols.append(
            Symbol(
                name=match.group("name"),
                category="type",
                arity=int(match.group("arity")),
                line=_line_of(body, match.start()),
            )
        )
    for match in _NAMESPACE_TYPE_RE.finditer(body):
        registry.symbols.append(
            Symbol(
                name=f'{match.group("namespace")}.{match.group("name")}',
                category="namespace_type",
                arity=int(match.group("arity")),
                line=_line_of(body, match.start()),
            )
        )
    for match in _ENUM_RE.finditer(body + "\n\n"):
        variants = tuple(
            (v.group("name"), v.group("payload"))
            for v in _VARIANT_RE.finditer(match.group("variants"))
        )
        registry.symbols.append(
            Symbol(
                name=match.group("name"),
                category="enum",
                arity=int(match.group("arity")),
                variants=variants,
                line=_line_of(body, match.start()),
            )
        )
    for match in _IFACE_RE.finditer(body):
        registry.symbols.append(
            Symbol(
                name=match.group("name"),
                category="interface",
                arity=int(match.group("arity")),
                line=_line_of(body, match.start()),
            )
        )
    for match in _HINT_RE.finditer(body):
        registry.symbols.append(
            Symbol(
                name=match.group("name"),
                category="hint",
                hint=_c_string_literal_join(match.group("text")),
                line=_line_of(body, match.start()),
            )
        )
    return registry


if __name__ == "__main__":
    import json
    import sys

    reg = load(sys.argv[1] if len(sys.argv) > 1 else ".")
    print(
        json.dumps(
            [
                {
                    "name": s.name,
                    "category": s.category,
                    "arity": s.arity,
                    "variants": [v[0] for v in s.variants],
                }
                for s in reg.symbols
            ],
            indent=2,
        )
    )
