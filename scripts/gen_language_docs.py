#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

FRONT_RE = re.compile(r"\A---\s*\n(.*?)\n---\s*\n", re.S)
BLOCK_RE = re.compile(r"<!--\s*xr-spec:(?P<name>[a-z_]+)\s*-->\n(?P<body>.*?)\n<!--\s*/xr-spec:(?P=name)\s*-->\n?", re.S)
FENCE_ID_RE = re.compile(r"^```xray\s+@id=(?P<id>[A-Za-z0-9_-]+)\s*$\n(?P<code>.*?)\n```\s*$", re.M | re.S)
FENCE_ID_INFO_RE = re.compile(r"^(```xray)\s+@id=[A-Za-z0-9_-]+\s*$", re.M)

SPEC_OUTPUTS = {
    "cn": "LANGUAGE_SPEC_CN.md",
    "en": "LANGUAGE_SPEC.md",
}

KNOWLEDGE_SUBDIRS = ("topics", "resources", "stdlib")
KNOWLEDGE_ROOT_FILES = ("README.md",)
CARD_SUBDIRS = ("topics", "resources", "stdlib")


def parse_frontmatter(path: Path) -> tuple[dict[str, str], str]:
    text = path.read_text(encoding="utf-8")
    match = FRONT_RE.match(text)
    if not match:
        raise ValueError(f"{path}: missing frontmatter")
    meta: dict[str, str] = {}
    for raw in match.group(1).splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if ":" not in line:
            raise ValueError(f"{path}: invalid frontmatter line {raw!r}")
        key, _, value = line.partition(":")
        meta[key.strip()] = value.strip().strip("\"'")
    return meta, text[match.end():]


def parse_blocks(path: Path, body: str) -> dict[str, str]:
    blocks = {m.group("name"): m.group("body").rstrip() + "\n" for m in BLOCK_RE.finditer(body)}
    if "cn" not in blocks:
        raise ValueError(f"{path}: missing cn block")
    if "en" not in blocks:
        raise ValueError(f"{path}: missing en block")
    return blocks


def extract_fences(unit_path: Path, blocks: dict[str, str], registry: dict[str, dict[str, str]]) -> None:
    """Collect ``​```xray @id=foo`` ... ``​``` blocks from each language block.

    sections carry parallel cn/en blocks whose code samples typically differ
    only in their inline comment language. The registry is keyed first by
    fence id, then by language, so cards can pick the language they need
    (defaulting to en).

    Each (id, lang) is global; the same id reused for a different concept
    elsewhere is an error. Reusing the same id within a single language block
    is also an error.
    """
    for lang, text in blocks.items():
        seen_in_block: set[str] = set()
        for match in FENCE_ID_RE.finditer(text):
            fid = match.group("id")
            code = match.group("code")
            if fid in seen_in_block:
                raise ValueError(
                    f"{unit_path}: duplicate fence id {fid!r} within {lang} block"
                )
            seen_in_block.add(fid)
            slot = registry.setdefault(fid, {})
            if lang in slot:
                raise ValueError(
                    f"{unit_path}: duplicate fence id {fid!r} for lang {lang!r} (already defined elsewhere)"
                )
            slot[lang] = code


def strip_fence_id_markers(text: str) -> str:
    return FENCE_ID_INFO_RE.sub(r"\1", text)


def block_shape(text: str) -> dict[str, int]:
    """Structural fingerprint of a localized block; both languages must match."""
    lines = text.splitlines()
    return {
        "h2": sum(1 for l in lines if l.startswith("## ")),
        "h3": sum(1 for l in lines if l.startswith("### ")),
        "h4": sum(1 for l in lines if l.startswith("#### ")),
        "xray_fences": text.count("```xray"),
        "ebnf_fences": text.count("```ebnf"),
        "tables": sum(1 for l in lines if l.startswith("|") and "--" in l),
    }


def check_parity(path: Path, blocks: dict[str, str]) -> list[str]:
    """Per-section structural parity check between cn and en blocks.

    Catches the common regression where the English block is a stub of the
    Chinese block. Both blocks must share the same heading hierarchy, the
    same number of code fences (xray / ebnf) and the same number of tables.
    """
    cn = block_shape(blocks["cn"])
    en = block_shape(blocks["en"])
    errors: list[str] = []
    for key in cn:
        if cn[key] != en[key]:
            errors.append(
                f"{path}: cn/en parity mismatch on {key}: cn={cn[key]} en={en[key]}"
            )
    return errors


def load_spec_units(source: Path) -> tuple[list[dict[str, Any]], dict[str, dict[str, str]]]:
    units: list[dict[str, Any]] = []
    seen_ids: set[str] = set()
    seen_orders: set[str] = set()
    fence_registry: dict[str, dict[str, str]] = {}
    sections = source / "sections"
    for path in sorted(sections.rglob("*.md")):
        meta, body = parse_frontmatter(path)
        if "id" not in meta:
            raise ValueError(f"{path}: missing id")
        if "order" not in meta:
            raise ValueError(f"{path}: missing order")
        if meta["id"] in seen_ids:
            raise ValueError(f"{path}: duplicate id {meta['id']}")
        if meta["order"] in seen_orders:
            raise ValueError(f"{path}: duplicate order {meta['order']}")
        seen_ids.add(meta["id"])
        seen_orders.add(meta["order"])
        blocks = parse_blocks(path, body)
        extract_fences(path, blocks, fence_registry)
        units.append({"path": path, "id": meta["id"], "order": meta["order"], "blocks": blocks})
    units.sort(key=lambda item: (item["order"], item["id"]))
    return units, fence_registry


def render_spec(units: list[dict[str, Any]], lang: str) -> str:
    parts = [strip_fence_id_markers(unit["blocks"][lang]).rstrip() for unit in units]
    return "\n\n".join(parts) + "\n"


def normalize_text(text: str) -> str:
    text = text.replace("\r\n", "\n")
    return text if text.endswith("\n") else text + "\n"


def write_or_check(path: Path, text: str, check: bool, errors: list[str]) -> None:
    text = normalize_text(text)
    if check:
        if not path.exists():
            errors.append(f"{path}: missing generated output")
            return
        current = normalize_text(path.read_text(encoding="utf-8"))
        if current != text:
            errors.append(f"{path}: stale generated output")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def render_knowledge_file(path: Path) -> str:
    return normalize_text(path.read_text(encoding="utf-8"))


def lookup_fence(fence_registry: dict[str, dict[str, str]], fid: str, lang: str, card_path: Path) -> str:
    slot = fence_registry.get(fid)
    if slot is None:
        raise ValueError(f"{card_path}: unknown fence id {fid!r}")
    if lang in slot:
        return slot[lang]
    # Fall back to any other language present so a single-language fence
    # remains usable; explicit lang misses are reported clearly.
    for fallback_lang in ("en", "cn"):
        if fallback_lang in slot:
            return slot[fallback_lang]
    available = ", ".join(sorted(slot.keys())) or "none"
    raise ValueError(
        f"{card_path}: fence id {fid!r} has no body for lang {lang!r} (have: {available})"
    )


def render_card_section(section: dict[str, Any], fence_registry: dict[str, dict[str, str]], card_path: Path, lang: str) -> list[str]:
    """Render one section block of a card to markdown lines.

    Each section may contribute, in order: an optional ``heading``, an optional
    ``before_text`` paragraph, one or more ``fences`` referencing sections, an
    optional list of inline ``code`` examples, an optional ``after_text``
    paragraph, and an optional ``bullets`` list. Empty sections are an error.
    """
    out: list[str] = []
    heading = section.get("heading")
    if heading is not None:
        level = section.get("heading_level", 3)
        if not isinstance(level, int) or level < 1 or level > 6:
            raise ValueError(f"{card_path}: invalid heading_level {level!r}")
        out.append(f"{'#' * level} {heading}")
    before = section.get("before_text")
    if before:
        out.append(before.rstrip())
    markdown = section.get("markdown")
    if markdown:
        out.append(markdown.rstrip())
    for fid in section.get("fences", []):
        out.append("```xray")
        out.append(lookup_fence(fence_registry, fid, lang, card_path).rstrip())
        out.append("```")
    for code in section.get("code", []):
        out.append("```xray")
        out.append(code.rstrip("\n"))
        out.append("```")
    after = section.get("after_text")
    if after:
        out.append(after.rstrip())
    for bullet in section.get("bullets", []):
        out.append(f"- {bullet}")
    if heading is None and not (before or markdown or after) and not section.get("fences") and not section.get("code") and not section.get("bullets"):
        raise ValueError(f"{card_path}: section has no content")
    return out


def render_topic_card(card_path: Path, fence_registry: dict[str, dict[str, str]]) -> str:
    card = json.loads(card_path.read_text(encoding="utf-8"))
    expected_id = card_path.stem
    if card.get("id") != expected_id:
        raise ValueError(f"{card_path}: id field {card.get('id')!r} must equal filename stem {expected_id!r}")
    title = card.get("title")
    if not title:
        raise ValueError(f"{card_path}: missing title")
    spec_anchor = card.get("spec_anchor")
    if not spec_anchor or not spec_anchor.startswith("#"):
        raise ValueError(f"{card_path}: spec_anchor must be a heading anchor like '#foo'")
    aliases = card.get("aliases", [])
    if not isinstance(aliases, list):
        raise ValueError(f"{card_path}: aliases must be a list")
    lang = card.get("lang", "en")
    out: list[str] = []
    out.append("---")
    out.append(f"id: {expected_id}")
    out.append(f"title: {title}")
    out.append(f"spec: {spec_anchor}")
    out.append(f"aliases: [{', '.join(aliases)}]")
    out.append("---")
    out.append(f"## {title}")
    lead = card.get("lead")
    if lead:
        out.append("")
        out.append(lead.rstrip())
    sections = card.get("sections", [])
    if not sections:
        raise ValueError(f"{card_path}: card must have at least one section")
    for section in sections:
        out.append("")
        out.extend(render_card_section(section, fence_registry, card_path, lang))
    return "\n".join(out) + "\n"


def render_resource_card(card_path: Path, fence_registry: dict[str, dict[str, str]]) -> str:
    card = json.loads(card_path.read_text(encoding="utf-8"))
    expected_id = card_path.stem
    if card.get("id") != expected_id:
        raise ValueError(f"{card_path}: id field {card.get('id')!r} must equal filename stem {expected_id!r}")
    lang = card.get("lang", "en")
    out: list[str] = []
    out.append("---")
    out.append(f"id: {expected_id}")
    out.append("---")
    title = card.get("title")
    if title:
        out.append(f"# {title}")
    lead = card.get("lead")
    if lead:
        out.append("")
        out.append(lead.rstrip())
    sections = card.get("sections", [])
    if not sections:
        raise ValueError(f"{card_path}: resource card must have at least one section")
    for section in sections:
        out.append("")
        out.extend(render_card_section(section, fence_registry, card_path, lang))
    return "\n".join(out) + "\n"


def render_stdlib_card(card_path: Path, fence_registry: dict[str, dict[str, str]]) -> str:
    card = json.loads(card_path.read_text(encoding="utf-8"))
    expected_module = card_path.stem
    module = card.get("module")
    if module != expected_module:
        raise ValueError(f"{card_path}: module field {module!r} must equal filename stem {expected_module!r}")
    summary = card.get("summary")
    if not summary:
        raise ValueError(f"{card_path}: missing summary")
    lang = card.get("lang", "en")
    out: list[str] = []
    out.append("---")
    out.append(f"module: {module}")
    out.append(f"summary: {summary}")
    out.append("---")
    out.append(f"# {module} module")
    lead = card.get("lead") or summary
    out.append("")
    out.append(lead.rstrip())
    out.append("")
    out.append(f"Usage: `import {module}` then call `{module}.function()`.")
    for section in card.get("sections", []):
        out.append("")
        out.extend(render_card_section(section, fence_registry, card_path, lang))
    return "\n".join(out) + "\n"


CARD_RENDERERS = {
    "topics": render_topic_card,
    "resources": render_resource_card,
    "stdlib": render_stdlib_card,
}


def list_card_paths(cards_root: Path, subdir: str) -> list[Path]:
    src_dir = cards_root / subdir
    if not src_dir.is_dir():
        raise ValueError(f"{src_dir}: missing card source directory")
    return sorted(src_dir.glob("*.json"))


def generate_knowledge(
    root: Path,
    source: Path,
    fence_registry: dict[str, dict[str, str]],
    check: bool,
    errors: list[str],
) -> None:
    knowledge_source = source / "knowledge"
    cards_root = source / "cards"
    knowledge_out = root / "docs" / "knowledge"
    expected: set[Path] = set()
    for name in KNOWLEDGE_ROOT_FILES:
        path = knowledge_source / name
        if not path.is_file():
            raise ValueError(f"{path}: missing knowledge source file")
        out_path = knowledge_out / name
        expected.add(out_path)
        write_or_check(out_path, render_knowledge_file(path), check, errors)
    for subdir in KNOWLEDGE_SUBDIRS:
        renderer = CARD_RENDERERS.get(subdir)
        if renderer is None:
            raise ValueError(f"{subdir}: no card renderer registered")
        for path in list_card_paths(cards_root, subdir):
            stem = path.stem
            out_path = knowledge_out / subdir / f"{stem}.md"
            expected.add(out_path)
            write_or_check(out_path, renderer(path, fence_registry), check, errors)
    for name in KNOWLEDGE_ROOT_FILES:
        path = knowledge_out / name
        if path.exists() and path not in expected:
            if check:
                errors.append(f"{path}: stale generated output")
            else:
                path.unlink()
    for subdir in KNOWLEDGE_SUBDIRS:
        out_dir = knowledge_out / subdir
        if not out_dir.is_dir():
            continue
        for path in sorted(out_dir.glob("*.md")):
            if path in expected:
                continue
            if check:
                errors.append(f"{path}: stale generated output")
            else:
                path.unlink()


def generate(root: Path, check: bool, parity: str = "warn") -> list[str]:
    source = root / "docs" / "spec" / "source"
    if not source.is_dir():
        raise ValueError(f"{source}: missing spec source directory")
    units, fence_registry = load_spec_units(source)
    errors: list[str] = []
    parity_errors: list[str] = []
    for unit in units:
        parity_errors.extend(check_parity(unit["path"], unit["blocks"]))
    if parity_errors:
        if parity == "strict":
            errors.extend(parity_errors)
        else:
            for err in parity_errors:
                print(f"warning: {err}", file=sys.stderr)
    for lang, rel in SPEC_OUTPUTS.items():
        write_or_check(root / rel, render_spec(units, lang), check, errors)
    generate_knowledge(root, source, fence_registry, check, errors)
    return errors


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--check", action="store_true")
    parser.add_argument(
        "--parity",
        choices=("warn", "strict"),
        default="warn",
        help="cn/en structural parity policy (default: warn). Use strict to fail on mismatch.",
    )
    args = parser.parse_args(argv)

    try:
        errors = generate(args.root.resolve(), args.check, args.parity)
    except ValueError as err:
        print(f"error: {err}", file=sys.stderr)
        return 2
    if errors:
        for err in errors:
            print(f"error: {err}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
