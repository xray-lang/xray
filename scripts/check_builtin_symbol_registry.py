#!/usr/bin/env python3
"""Built-in symbol registry gate.

``stdlib/prelude/builtin_symbols.def`` is the single source of truth for every
symbol the language provides without an import. This script keeps three things
locked to it, fail-closed:

R1  The specification's built-in symbol registry (§2.2.1, both languages) is
    generated from the def. ``--write`` regenerates it; the default ``--check``
    mode fails if the checked-in tables differ.

R2  Every capitalized symbol the specification names in a normative position
    must resolve: to the def, to a stdlib module export, or to a type the
    surrounding example declares itself. This is the rule that would have
    caught `Box<Expr>` — recommended by §5.6 and by a compiler diagnostic while
    being neither a built-in nor a stdlib type.

R3  Every language-surface symbol in the def compiles: ``tests/builtin_probes``
    holds one minimal program per symbol and both ``xray check`` and ``xray
    run`` must succeed. `check` alone is not enough — a generic instance used
    as an enum payload passed `check` and failed in the post-mono pass.

Run R3 only when a built compiler is available (``--xray PATH``); R1 and R2 are
pure text checks and always run.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import builtin_symbols  # noqa: E402

SPEC_SECTION = "spec/source/sections/003-2-type-system.md"
PROBE_DIR = "tests/builtin_probes"

BEGIN = "<!-- xr-builtin-registry:begin -->"
END = "<!-- xr-builtin-registry:end -->"

HEADINGS = {
    "cn": {
        "title": "#### 2.2.1 内置符号登记表",
        "lead": (
            "下表由 `stdlib/prelude/builtin_symbols.def` 生成，是无需 import 即可命名的符号全集。"
            "编译器、LSP 与本表读同一份真值源；表外的大写名字必须来自 import 或用户声明。"
        ),
        "types": "**内置类型**",
        "enums": "**内置 enum**",
        "ifaces": "**内置约束接口**",
        "hints": "**故意不提供的名字**",
        "col_symbol": "符号",
        "col_kind": "构造",
        "col_variants": "变体",
        "col_hint": "诊断提示（编译器原文）",
        "prelude": "prelude",
        "resolver": "解析器内建",
    },
    "en": {
        "title": "#### 2.2.1 Built-in symbol registry",
        "lead": (
            "Generated from `stdlib/prelude/builtin_symbols.def`, this is the complete set of "
            "names available without an import. The compiler, the LSP and this table read the "
            "same source of truth; any capitalized name outside it comes from an import or a "
            "user declaration."
        ),
        "types": "**Built-in types**",
        "enums": "**Built-in enums**",
        "ifaces": "**Built-in constraint interfaces**",
        "hints": "**Deliberately absent names**",
        "col_symbol": "Symbol",
        "col_kind": "Construction",
        "col_variants": "Variants",
        "col_hint": "Diagnostic hint",
        "prelude": "prelude",
        "resolver": "resolver built-in",
    },
}

# Capitalized names that appear in the specification but are neither built-ins
# nor stdlib exports: EBNF metavariables, prose nouns, and the type parameters
# used throughout the generics section.
SPEC_NAME_ALLOWLIST = {
    # single-letter and conventional type parameters
    *{chr(c) for c in range(ord("A"), ord("Z") + 1)},
    "T1", "T2", "T3", "TN", "K1", "K2", "V1", "V2",
    # primitive and syntactic type names owned by the lexer, not the registry
    "Unit", "Self",
}


def spec_path(root: Path) -> Path:
    return root / SPEC_SECTION


def render_registry(registry: builtin_symbols.Registry, lang: str) -> str:
    t = HEADINGS[lang]
    lines: list[str] = [BEGIN, "", t["title"], "", t["lead"], "", t["types"], ""]
    lines.append(f'| {t["col_symbol"]} | {t["col_kind"]} |')
    lines.append("|--|--|")
    for symbol in sorted(
        registry.by_category("prelude_type")
        + registry.by_category("type")
        + registry.by_category("namespace_type"),
        key=lambda s: s.name,
    ):
        kind = t["prelude"] if symbol.category == "prelude_type" else t["resolver"]
        lines.append(f"| `{symbol.spelling}` | {kind} |")

    lines += ["", t["enums"], ""]
    lines.append(f'| {t["col_symbol"]} | {t["col_variants"]} |')
    lines.append("|--|--|")
    for symbol in registry.by_category("enum"):
        variants = " \\| ".join(f"`{name}`" for name, _payload in symbol.variants)
        lines.append(f"| `{symbol.spelling}` | {variants} |")

    lines += ["", t["ifaces"], ""]
    lines.append(f'| {t["col_symbol"]} |')
    lines.append("|--|")
    for symbol in sorted(registry.by_category("interface"), key=lambda s: s.name):
        lines.append(f"| `{symbol.spelling}` |")

    lines += ["", t["hints"], ""]
    lines.append(f'| {t["col_symbol"]} | {t["col_hint"]} |')
    lines.append("|--|--|")
    for symbol in registry.by_category("hint"):
        lines.append(f"| `{symbol.name}` | {symbol.hint} |")

    lines += ["", END]
    return "\n".join(lines)


def splice(text: str, registry: builtin_symbols.Registry) -> str:
    """Replace (or insert) the generated registry inside each language block."""
    out = text
    for lang in ("cn", "en"):
        block = render_registry(registry, lang)
        marker_re = re.compile(re.escape(BEGIN) + r".*?" + re.escape(END), re.S)
        # Each language block owns exactly one region. Find the block bounds
        # first so the cn region is not overwritten with the en table.
        open_tag = f"<!-- xr-spec:{lang} -->"
        close_tag = f"<!-- /xr-spec:{lang} -->"
        start = out.index(open_tag)
        end = out.index(close_tag)
        segment = out[start:end]
        if marker_re.search(segment):
            segment = marker_re.sub(lambda _m: block, segment, count=1)
        else:
            anchor = "\n### 2.3 " if lang == "cn" else "\n### 2.3 "
            insert_at = segment.index(anchor)
            segment = segment[:insert_at] + "\n" + block + "\n" + segment[insert_at:]
        out = out[:start] + segment + out[end:]
    return out


def check_r1(root: Path, registry: builtin_symbols.Registry, write: bool) -> list[str]:
    path = spec_path(root)
    text = path.read_text(encoding="utf-8")
    updated = splice(text, registry)
    if updated == text:
        return []
    if write:
        path.write_text(updated, encoding="utf-8")
        print(f"[R1] regenerated built-in symbol registry in {SPEC_SECTION}")
        return []
    return [
        f"[R1] {SPEC_SECTION}: built-in symbol registry is stale; "
        f"run scripts/check_builtin_symbol_registry.py --write"
    ]


def stdlib_exported_names(root: Path) -> set[str]:
    """Capitalized type names any stdlib module exports."""
    names: set[str] = set()
    declaration = re.compile(
        r"^\s*(?:export\s+)?(?:native_class|handle|class|struct|enum|interface|type)\s+([A-Z]\w*)"
    )
    for path in list((root / "stdlib").rglob("*.def")) + list((root / "stdlib").rglob("*.xr")):
        try:
            for line in path.read_text(encoding="utf-8", errors="strict").splitlines():
                match = declaration.match(line)
                if match:
                    names.add(match.group(1))
        except OSError:
            continue
    return names


CODE_FENCE_RE = re.compile(r"^```.*?^```", re.S | re.M)
INLINE_CODE_RE = re.compile(r"`([^`\n]+)`")
GENERIC_USE_RE = re.compile(r"\b(?:(?P<qualifier>[A-Z]\w*)\.)?(?P<name>[A-Z]\w*)<")
DECLARES_RE = re.compile(r"\b(?:export\s+)?(?:class|struct|enum|interface|type)\s+([A-Z]\w*)")

# Appendix E exists to name other languages' constructs (Rust `Result<T, E>`,
# Swift `Task<Success, Failure>`, ...). Scanning it for Xray symbols would flag
# every comparison it is written to make.
R2_EXEMPT_SECTIONS = {"024-appendix-e-differences-from-other-languages.md"}


def _blank_code_fences(text: str) -> str:
    """Replace fenced code with blank lines, preserving line numbering."""
    return CODE_FENCE_RE.sub(lambda m: "\n" * m.group(0).count("\n"), text)


def check_r2(root: Path, registry: builtin_symbols.Registry) -> list[str]:
    """Every `Name<...>` the spec writes in prose must resolve to something."""
    known = registry.names() | stdlib_exported_names(root) | SPEC_NAME_ALLOWLIST
    errors: list[str] = []
    for path in sorted((root / "spec/source/sections").glob("*.md")):
        if path.name in R2_EXEMPT_SECTIONS:
            continue
        text = path.read_text(encoding="utf-8")
        # Types a fence declares are in scope for the whole section: examples
        # build on each other and a later fence may reuse an earlier class.
        declared = set(DECLARES_RE.findall(text))
        prose = _blank_code_fences(text)
        for lineno, line in enumerate(prose.splitlines(), start=1):
            for snippet in INLINE_CODE_RE.findall(line):
                for match in GENERIC_USE_RE.finditer(snippet):
                    qualifier = match.group("qualifier")
                    name = match.group("name")
                    spelling = f"{qualifier}.{name}" if qualifier else name
                    if spelling in known or (not qualifier and name in declared):
                        continue
                    errors.append(
                        f"[R2] {path.relative_to(root)}:{lineno}: `{spelling}<...>` is not a built-in "
                        f"symbol, a stdlib export, or declared in this section"
                    )
    return errors


def check_r3(root: Path, registry: builtin_symbols.Registry, xray: Path) -> list[str]:
    probe_dir = root / PROBE_DIR
    errors: list[str] = []
    probes = sorted(probe_dir.glob("*.xr"))
    covered = {p.stem for p in probes}
    for symbol in registry.surface:
        stem = symbol.name.split(".", 1)[0].lower()
        if stem not in covered:
            errors.append(f"[R3] {PROBE_DIR}/{stem}.xr missing: no probe covers `{symbol.name}`")
    for probe in probes:
        for mode in ("check", "run"):
            result = subprocess.run(
                [str(xray), mode, str(probe)],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="strict",
                timeout=120,
            )
            if result.returncode != 0:
                head = (result.stderr or result.stdout).strip().splitlines()
                detail = head[0] if head else f"exit {result.returncode}"
                errors.append(f"[R3] xray {mode} {probe.relative_to(root)}: {detail}")
                break
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=Path(__file__).resolve().parents[1], type=Path)
    parser.add_argument(
        "--write", action="store_true", help="regenerate the specification registry in place"
    )
    parser.add_argument("--xray", type=Path, help="compiler binary; enables the R3 probe suite")
    args = parser.parse_args()

    root: Path = args.root
    registry = builtin_symbols.load(root)
    if not registry.surface:
        print("error: builtin_symbols.def produced no symbols", file=sys.stderr)
        return 1

    errors = check_r1(root, registry, args.write)
    errors += check_r2(root, registry)
    if args.xray:
        errors += check_r3(root, registry, args.xray)

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        print(f"\n{len(errors)} built-in symbol registry violation(s)", file=sys.stderr)
        return 1

    surface = len(registry.surface)
    probes = "with probes" if args.xray else "R1+R2 only"
    print(f"builtin symbol registry OK — {surface} language-surface symbols ({probes})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
