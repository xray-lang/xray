#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Any

FRONT_RE = re.compile(r"\A---\s*\n(.*?)\n---\s*\n", re.S)
BLOCK_RE = re.compile(r"<!--\s*xr-spec:(?P<name>[a-z_]+)\s*-->\n(?P<body>.*?)\n<!--\s*/xr-spec:(?P=name)\s*-->\n?", re.S)

SPEC_OUTPUTS = {
    "cn": "LANGUAGE_SPEC_CN.md",
    "en": "LANGUAGE_SPEC.md",
}

KNOWLEDGE_SUBDIRS = ("topics", "resources", "stdlib")
KNOWLEDGE_ROOT_FILES = ("README.md",)


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


def load_spec_units(source: Path) -> list[dict[str, Any]]:
    units: list[dict[str, Any]] = []
    seen_ids: set[str] = set()
    seen_orders: set[str] = set()
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
        units.append({"path": path, "id": meta["id"], "order": meta["order"], "blocks": blocks})
    units.sort(key=lambda item: (item["order"], item["id"]))
    return units


def render_spec(units: list[dict[str, Any]], lang: str) -> str:
    parts = [unit["blocks"][lang].rstrip() for unit in units]
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


def generate_knowledge(root: Path, source: Path, check: bool, errors: list[str]) -> None:
    knowledge_source = source / "knowledge"
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
        src_dir = knowledge_source / subdir
        if not src_dir.is_dir():
            raise ValueError(f"{src_dir}: missing knowledge source directory")
        for path in sorted(src_dir.glob("*.md")):
            out_path = knowledge_out / subdir / path.name
            expected.add(out_path)
            write_or_check(out_path, render_knowledge_file(path), check, errors)
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
    units = load_spec_units(source)
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
    generate_knowledge(root, source, check, errors)
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
