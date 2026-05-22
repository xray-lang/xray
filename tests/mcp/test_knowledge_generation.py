#!/usr/bin/env python3
"""Regression checks for generated MCP knowledge sources."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

FENCE_RE = re.compile(r"```xray\n(.*?)\n```", re.S)
FRONT_RE = re.compile(r"\A---\s*\n(.*?)\n---\s*\n", re.S)
SYMBOL_RE = re.compile(
    r"\.module = \"(?P<module>[^\"]+)\".*?\.symbols = (?P<symbols>_symbols_[A-Za-z0-9_]+|NULL),"
    r".*?\.symbol_count = (?P<count>[^,]+),",
    re.S,
)
SYMBOL_ARRAY_RE = re.compile(
    r"static const XmcpGeneratedStdlibSymbol (?P<name>_symbols_[A-Za-z0-9_]+)\[\] = \{(?P<body>.*?)\n\};",
    re.S,
)
SYMBOL_NAME_RE = re.compile(r"\.name = \"([^\"]+)\"")


def heading_anchor(heading: str) -> str:
    text = heading.strip().lower()
    text = re.sub(r"^#+\s*", "", text)
    text = text.replace("`", "")
    text = re.sub(r"[^\w\u4e00-\u9fff\s-]", "", text)
    text = re.sub(r"[\s]+", "-", text)
    text = re.sub(r"-+", "-", text)
    return "#" + text.strip("-")


def load_spec_anchors(root: Path) -> set[str]:
    anchors: set[str] = set()
    for line in (root / "LANGUAGE_SPEC_CN.md").read_text(encoding="utf-8").splitlines():
        if line.startswith("#"):
            anchors.add(heading_anchor(line))
    return anchors


def parse_frontmatter(path: Path) -> dict[str, str]:
    match = FRONT_RE.match(path.read_text(encoding="utf-8"))
    if not match:
        raise ValueError(f"{path}: missing frontmatter")
    meta: dict[str, str] = {}
    for raw in match.group(1).splitlines():
        if ":" not in raw:
            continue
        key, _, value = raw.partition(":")
        meta[key.strip()] = value.strip().strip("\"'")
    return meta


def run(cmd: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, cwd=str(cwd), text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)


def check_xray_fences(root: Path, xray: Path) -> list[str]:
    errors: list[str] = []
    for path in sorted((root / "docs/knowledge/topics").glob("*.md")):
        text = path.read_text(encoding="utf-8")
        for idx, match in enumerate(FENCE_RE.finditer(text), 1):
            code = match.group(1).strip() + "\n"
            with tempfile.NamedTemporaryFile("w", suffix=".xr", delete=False, encoding="utf-8") as tmp:
                tmp.write(code)
                tmp_path = Path(tmp.name)
            try:
                proc = run([str(xray), "check", str(tmp_path)], root)
                if proc.returncode != 0:
                    errors.append(
                        f"{path.relative_to(root)} fence #{idx} failed\n{proc.stdout}{proc.stderr}"
                    )
            finally:
                tmp_path.unlink(missing_ok=True)
    return errors


def check_topic_spec_anchors(root: Path) -> list[str]:
    anchors = load_spec_anchors(root)
    errors: list[str] = []
    for path in sorted((root / "docs/knowledge/topics").glob("*.md")):
        try:
            meta = parse_frontmatter(path)
        except ValueError as err:
            errors.append(str(err))
            continue
        spec = meta.get("spec", "")
        if not spec:
            errors.append(f"{path.relative_to(root)}: missing spec anchor")
        elif spec not in anchors:
            errors.append(f"{path.relative_to(root)}: unknown spec anchor {spec}")
    return errors


def generated_symbol_index(generated: Path) -> dict[str, set[str]]:
    text = generated.read_text(encoding="utf-8")
    arrays: dict[str, set[str]] = {}
    for match in SYMBOL_ARRAY_RE.finditer(text):
        arrays[match.group("name")] = set(SYMBOL_NAME_RE.findall(match.group("body")))

    result: dict[str, set[str]] = {}
    for match in SYMBOL_RE.finditer(text):
        module = match.group("module")
        symbols = match.group("symbols")
        if symbols == "NULL":
            result[module] = set()
        else:
            result[module] = arrays.get(symbols, set())
    return result


def builtin_symbol_index(root: Path, xray: Path) -> dict[str, set[str]]:
    proc = run([str(xray), "builtin-dump"], root)
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr or proc.stdout)
    data = json.loads(proc.stdout)
    return {m["name"]: {s["name"] for s in m.get("symbols", [])} for m in data.get("modules", [])}


def check_symbol_subset(root: Path, xray: Path) -> list[str]:
    generated = generated_symbol_index(root / "src/app/mcp/xmcp_knowledge_generated.c")
    builtins = builtin_symbol_index(root, xray)
    errors: list[str] = []
    for module, symbols in sorted(generated.items()):
        missing = symbols - builtins.get(module, set())
        if missing:
            preview = ", ".join(sorted(missing)[:8])
            errors.append(f"{module}: generated symbols missing from analyzer builtin dump: {preview}")
    return errors


def check_generated_stdlib_api_tables(root: Path) -> list[str]:
    text = (root / "src/app/mcp/xmcp_knowledge_generated.c").read_text(encoding="utf-8")
    required = ['"## API\\n"', '`csv.parse`', '`http.get`']
    return [f"generated stdlib API table missing {needle}" for needle in required if needle not in text]


def check_generated_is_current(root: Path, xray: Path) -> list[str]:
    proc = run([str(xray), "builtin-dump"], root)
    if proc.returncode != 0:
        return [proc.stderr or proc.stdout]
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, encoding="utf-8") as tmp:
        tmp.write(proc.stdout)
        tmp_path = Path(tmp.name)
    try:
        check = run(
            [
                sys.executable,
                str(root / "scripts/gen_mcp_knowledge.py"),
                "--docs",
                str(root / "docs/knowledge"),
                "--spec",
                str(root / "LANGUAGE_SPEC_CN.md"),
                "--builtins",
                str(tmp_path),
                "--out",
                str(root / "src/app/mcp/xmcp_knowledge_generated.c"),
                "--check",
            ],
            root,
        )
    finally:
        tmp_path.unlink(missing_ok=True)
    if check.returncode != 0:
        return [check.stderr or check.stdout]
    return []


def check_key_syntax(root: Path) -> list[str]:
    required = {
        "channel": ["shared const ch = new Channel<int>(10)", "let (next, ok) = ch.tryRecv()"],
        "functions": ["let (q, r) = divmod(17, 5)"],
        "class": ["override speak() -> string"],
        "control_flow": ["match (x)", "try { throw"],
        "modules": ["export fn helper()", "export class MyClass"],
    }
    errors: list[str] = []
    for topic, needles in required.items():
        text = (root / f"docs/knowledge/topics/{topic}.md").read_text(encoding="utf-8")
        for needle in needles:
            if needle not in text:
                errors.append(f"{topic}: missing required syntax example: {needle}")
    return errors


def check_prompt_smoke_examples(root: Path, xray: Path) -> list[str]:
    examples = {
        "code-review": "fn add(a: int, b: int) -> int {\n    return a + b\n}\nprint(add(1, 2))\n",
        "explain-error": (
            "shared const ch = new Channel<int>(1)\n"
            "ch.send(42)\n"
            "let value = ch.recv()\n"
        ),
        "convert-to-xray": (
            "let status = 200\n"
            "let label = match (status) {\n"
            "    200 -> \"ok\",\n"
            "    _ -> \"other\"\n"
            "}\n"
        ),
        "concurrency-pattern": (
            "fn worker(input: int) -> int {\n"
            "    return input * 2\n"
            "}\n"
            "let task = go worker(21)\n"
            "let result = await task\n"
        ),
        "write-test": (
            "@test\n"
            "fn test_add() {\n"
            "    assert_eq(1 + 1, 2)\n"
            "}\n"
        ),
    }
    errors: list[str] = []
    for name, code in examples.items():
        with tempfile.NamedTemporaryFile("w", suffix=".xr", delete=False, encoding="utf-8") as tmp:
            tmp.write(code)
            tmp_path = Path(tmp.name)
        try:
            proc = run([str(xray), "check", str(tmp_path)], root)
            if proc.returncode != 0:
                errors.append(f"prompt {name} smoke example failed\n{proc.stdout}{proc.stderr}")
        finally:
            tmp_path.unlink(missing_ok=True)
    return errors


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--xray", required=True, type=Path)
    args = parser.parse_args(argv)

    root = args.root.resolve()
    xray = args.xray.resolve()
    errors: list[str] = []
    errors.extend(check_topic_spec_anchors(root))
    errors.extend(check_xray_fences(root, xray))
    errors.extend(check_generated_is_current(root, xray))
    errors.extend(check_symbol_subset(root, xray))
    errors.extend(check_generated_stdlib_api_tables(root))
    errors.extend(check_key_syntax(root))
    errors.extend(check_prompt_smoke_examples(root, xray))

    if errors:
        for err in errors:
            print(err, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
