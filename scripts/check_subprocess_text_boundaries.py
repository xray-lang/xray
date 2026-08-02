#!/usr/bin/env python3
"""Enforce explicit, fail-closed subprocess byte/text boundaries.

Subprocess pipes are bytes unless a caller declares a stable ASCII or UTF-8
protocol.  Declared text protocols must use strict decoding.  Provider and
program output that has no text contract therefore stays in binary mode.
"""

from __future__ import annotations

import argparse
import ast
from dataclasses import dataclass
from pathlib import Path
import sys


TEXT_ENCODINGS = {"ascii", "utf-8"}
SUBPROCESS_CALLS = {"run", "Popen", "check_output", "check_call"}
PATH_TEXT_CALLS = {"read_text", "write_text"}


@dataclass(frozen=True)
class Violation:
    path: Path
    line: int
    message: str


def literal(node: ast.expr | None) -> object:
    if isinstance(node, ast.Constant):
        return node.value
    return None


def subprocess_name(call: ast.Call) -> str | None:
    func = call.func
    if (
        isinstance(func, ast.Attribute)
        and isinstance(func.value, ast.Name)
        and func.value.id == "subprocess"
        and func.attr in SUBPROCESS_CALLS
    ):
        return func.attr
    return None


def path_text_name(call: ast.Call) -> str | None:
    if isinstance(call.func, ast.Attribute) and call.func.attr in PATH_TEXT_CALLS:
        return call.func.attr
    return None


def validate_path_text(call: ast.Call, name: str) -> list[str]:
    keywords = {item.arg: item.value for item in call.keywords if item.arg is not None}
    encoding = literal(keywords.get("encoding"))
    errors = literal(keywords.get("errors"))
    messages: list[str] = []
    if not isinstance(encoding, str) or encoding.lower() not in TEXT_ENCODINGS:
        messages.append(f"Path.{name} requires literal encoding='ascii' or encoding='utf-8'")
    if errors is not None and errors != "strict":
        messages.append(f"Path.{name} does not permit lossy errors={errors!r}")
    return messages


def classify(call: ast.Call) -> tuple[str, list[str]]:
    keywords = {item.arg: item.value for item in call.keywords if item.arg is not None}
    text = literal(keywords.get("text"))
    universal = literal(keywords.get("universal_newlines"))
    encoding = literal(keywords.get("encoding"))
    errors = literal(keywords.get("errors"))
    text_mode = text is True or universal is True or encoding is not None or errors is not None
    messages: list[str] = []
    if not text_mode:
        return "bytes", messages
    if not isinstance(encoding, str) or encoding.lower() not in TEXT_ENCODINGS:
        messages.append("text mode requires literal encoding='ascii' or encoding='utf-8'")
    if errors != "strict":
        messages.append("text mode requires literal errors='strict'")
    if text is False or universal is False:
        messages.append("text decoding options conflict with an explicit binary-mode flag")
    return f"text:{encoding}" if isinstance(encoding, str) else "text:implicit", messages


def scan_source(path: Path, source: str) -> tuple[list[Violation], dict[str, int]]:
    tree = ast.parse(source, filename=str(path))
    violations: list[Violation] = []
    inventory: dict[str, int] = {"bytes": 0, "text:ascii": 0, "text:utf-8": 0}
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        subprocess_call = subprocess_name(node)
        if subprocess_call is not None:
            category, messages = classify(node)
            inventory[category] = inventory.get(category, 0) + 1
            violations.extend(Violation(path, node.lineno, message) for message in messages)
        path_text_call = path_text_name(node)
        if path_text_call is not None:
            violations.extend(
                Violation(path, node.lineno, message)
                for message in validate_path_text(node, path_text_call)
            )
    return violations, inventory


def python_files(root: Path) -> list[Path]:
    files: list[Path] = []
    for directory in (root / "scripts", root / "tests"):
        if directory.is_dir():
            files.extend(directory.rglob("*.py"))
    return sorted(files)


def scan_repository(root: Path) -> tuple[list[Violation], dict[str, int]]:
    violations: list[Violation] = []
    inventory: dict[str, int] = {}
    for path in python_files(root):
        try:
            source = path.read_text(encoding="utf-8")
            found, counts = scan_source(path.relative_to(root), source)
        except (OSError, SyntaxError, UnicodeDecodeError) as exc:
            violations.append(Violation(path.relative_to(root), 1, f"cannot scan: {exc}"))
            continue
        violations.extend(found)
        for category, count in counts.items():
            inventory[category] = inventory.get(category, 0) + count
    return violations, inventory


def self_test() -> int:
    accepted = [
        "subprocess.run(cmd)",
        "subprocess.run(cmd, text=True, encoding='utf-8', errors='strict')",
        "subprocess.Popen(cmd, encoding='ascii', errors='strict')",
        "p.read_text(encoding='utf-8')",
        "p.write_text('value', encoding='ascii', errors='strict')",
    ]
    rejected = [
        "subprocess.run(cmd, text=True)",
        "subprocess.run(cmd, encoding='utf-8')",
        "subprocess.run(cmd, text=True, encoding='utf-8', errors='replace')",
        "p.read_text()",
        "p.write_text('value', encoding='utf-8', errors='ignore')",
    ]
    prefix = "import subprocess\ncmd = ['tool']\np = None\n"
    assert all(not scan_source(Path("accepted.py"), prefix + item)[0] for item in accepted)
    assert all(scan_source(Path("rejected.py"), prefix + item)[0] for item in rejected)
    print("subprocess text-boundary self-test: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    root = args.root.resolve()
    violations, inventory = scan_repository(root)
    for item in violations:
        print(f"{item.path}:{item.line}: {item.message}", file=sys.stderr)
    rendered = ", ".join(f"{key}={inventory[key]}" for key in sorted(inventory))
    print(f"subprocess boundary inventory: {rendered}")
    if violations:
        print(f"subprocess text-boundary gate: FAIL ({len(violations)} violations)", file=sys.stderr)
        return 1
    print("subprocess text-boundary gate: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
