#!/usr/bin/env python3
"""Inventory task-200 binary stdlib surface convergence residue.

This is a P0 inventory gate, not the final blocker. By default it prints
classified public-surface and consumer hits and exits successfully. Later
task-200 phases can make selected categories fail once string-binary overloads,
null sentinels, legacy aliases, and arbitrary-binary string creators have been
removed from the stdlib surface.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

CATEGORIES = (
    "PUBLIC_BINARY_STRING_SIGNATURE",
    "PUBLIC_FIXED_DIGEST_AS_STRING",
    "PUBLIC_ARBITRARY_STRING_CREATOR",
    "PUBLIC_STRING_BINARY_UNION",
    "PUBLIC_NULL_SENTINEL",
    "PUBLIC_LEGACY_BINARY_ALIAS",
    "PUBLIC_ARRAY_OWNER_INPUT",
    "PUBLIC_DOMAIN_BOOL_SENTINEL",
    "CONSUMER_OLD_BINARY_API_CALL",
    "NATIVE_ARBITRARY_STRING_CREATOR",
    "GENERATED_METADATA_STALE_BINARY_SURFACE",
)

BINARY_STRING_SYMBOLS = {
    ("compress", "adler32"),
    ("compress", "crc32"),
    ("compress", "deflate"),
    ("compress", "gunzip"),
    ("compress", "gzip"),
    ("compress", "inflate"),
    ("compress", "isGzip"),
    ("compress", "isZlib"),
    ("compress", "zlibCompress"),
    ("compress", "zlibDecompress"),
    ("crypto", "decrypt"),
    ("crypto", "encrypt"),
    ("crypto", "hmac"),
    ("crypto", "md5"),
    ("crypto", "randomBytes"),
    ("crypto", "sha1"),
    ("crypto", "sha256"),
    ("crypto", "sha512"),
    ("crypto", "timingSafeEqual"),
    ("encoding", "utf8ByteLength"),
    ("encoding", "utf8Count"),
    ("encoding", "utf8Valid"),
    ("net", "UdpPacket.data"),
    ("net", "read"),
    ("net", "recvFrom"),
    ("net", "sendTo"),
    ("net", "write"),
}

FIXED_DIGEST_STRING_SYMBOLS = {
    ("crypto", "md5"),
    ("crypto", "sha1"),
    ("crypto", "sha256"),
    ("crypto", "sha512"),
}

ARBITRARY_STRING_CREATOR_SYMBOLS = {
    ("compress", "deflate"),
    ("compress", "gunzip"),
    ("compress", "gzip"),
    ("compress", "inflate"),
    ("compress", "zlibCompress"),
    ("compress", "zlibDecompress"),
    ("crypto", "decrypt"),
    ("crypto", "encrypt"),
    ("crypto", "randomBytes"),
    ("net", "UdpPacket.data"),
    ("net", "read"),
}

LEGACY_ALIAS_SYMBOLS = {
    ("base64", "decodeToBytes"),
    ("base64", "decodeUrl"),
    ("base64", "encodeBytes"),
    ("base64", "encodeUrl"),
    ("io", "writeFileBytes"),
    ("net", "writeBytes"),
}

NULL_SENTINEL_SYMBOLS = {
    ("compress", "deflate"),
    ("compress", "gunzip"),
    ("compress", "gzip"),
    ("compress", "inflate"),
    ("compress", "zlibCompress"),
    ("compress", "zlibDecompress"),
    ("crypto", "decrypt"),
    ("crypto", "hmac"),
    ("io", "readFileBytes"),
    ("net", "read"),
    ("net", "recvFrom"),
    ("ws", "WsMessage.data"),
    ("ws", "recv"),
}

BOOL_SENTINEL_SYMBOLS = {
    ("io", "writeFileBytes"),
    ("ws", "send"),
}

ARRAY_OWNER_INPUT_MODULES = {"base64", "encoding", "net", "sys", "ws"}
ARRAY_OWNER_INPUT_RE = re.compile(
    r"\b(?:data|payload|raw|key|buffer|bytes)\s*:\s*(?:Array<u8>|Array<u8>\?)"
)
STRING_BINARY_UNION_RE = re.compile(r"\bstring\s*\|\s*Array<u8>|Array<u8>\s*\|\s*string\b")
RETURN_STRING_RE = re.compile(r"\):\s*string\??\b|->\s*string\??\b")
NULL_SENTINEL_RE = re.compile(r"\?|null\b")
BOOL_RETURN_RE = re.compile(r"\):\s*bool\b|->\s*bool\b")

OLD_API_CALL_RE = re.compile(
    r"\b(?:"
    r"base64\.(?:decodeToBytes|decodeUrl|encodeBytes|encodeUrl)|"
    r"crypto\.(?:decrypt|encrypt|hmac|md5|randomBytes|sha1|sha256|sha512|timingSafeEqual)|"
    r"compress\.(?:adler32|crc32|deflate|gunzip|gzip|inflate|isGzip|isZlib|zlibCompress|zlibDecompress)|"
    r"net\.(?:read|recvFrom|sendTo|write|writeBytes)|"
    r"ws\.send"
    r")\s*\("
)
NATIVE_STRING_CREATOR_RE = re.compile(
    r"\b(?:make_string_n|xr_value_make_string|xr_value_make_string_n|"
    r"xr_string_new|xr_string_from|xrt_compress_finish_string|"
    r"xr_cfunc_ok_string)\b"
)
GENERATED_STALE_RE = re.compile(
    r"encodeBytes|decodeToBytes|writeBytes|writeFileBytes|"
    r"string\s*\|\s*Array<u8>|"
    r"\(data:\s*string(?:,|\)):|"
    r"\):\s*string\?"
)

SCAN_DIRS = ("stdlib", "tests", "spec", "demos", "docs", "src")
TEXT_SUFFIXES = (
    ".c",
    ".h",
    ".inc",
    ".inc.c",
    ".def",
    ".xr",
    ".xrd",
    ".md",
    ".toml",
    ".json",
    ".py",
    ".sh",
    ".expect",
    ".expected",
)
SKIP_DIR_NAMES = {
    ".git",
    ".mypy_cache",
    "__pycache__",
    "build",
    "build-fuzz",
    "cmake-build-debug",
    "cmake-build-release",
    "node_modules",
    "tmp",
}
GENERATED_METADATA_FILES = {
    Path("src/frontend/analyzer/xanalyzer_builtins_generated.h"),
    Path("src/app/lsp/xlsp_stdlib_generated.inc"),
    Path("src/app/mcp/xmcp_knowledge_generated.c"),
}
NATIVE_BINARY_DIR_PREFIXES = (
    "stdlib/compress/",
    "stdlib/crypto/",
    "stdlib/net/",
    "stdlib/ws/",
)
SCRIPT_REL_PATH = Path("scripts/check_binary_stdlib_surface.py")


@dataclass(frozen=True)
class Hit:
    category: str
    path: str
    line: int
    text: str


def rel(root: Path, path: Path) -> Path:
    try:
        return path.relative_to(root)
    except ValueError:
        return path.resolve().relative_to(root)


def symbol_of(item: dict[str, Any]) -> tuple[str, str]:
    return str(item.get("namespace", "")), str(item.get("name", ""))


def load_source_inventory(root: Path) -> list[dict[str, Any]]:
    scripts_dir = root / "scripts"
    sys.path.insert(0, str(scripts_dir))
    try:
        import gen_api_inventory  # type: ignore[import-not-found]
    except ImportError as exc:
        raise SystemExit(f"cannot import scripts/gen_api_inventory.py: {exc}") from exc
    finally:
        try:
            sys.path.remove(str(scripts_dir))
        except ValueError:
            pass

    return [
        *gen_api_inventory.collect_def_stdlib(root),
        *gen_api_inventory.collect_pure_stdlib(root),
    ]


def classify_public_item(item: dict[str, Any]) -> list[Hit]:
    module, name = symbol_of(item)
    symbol = (module, name)
    signature = str(item.get("signature", ""))
    source = str(item.get("source", ""))
    line = int(item.get("line", 1))
    text = f"{module}.{name} {signature}"
    hits: list[Hit] = []

    if STRING_BINARY_UNION_RE.search(signature):
        hits.append(Hit("PUBLIC_STRING_BINARY_UNION", source, line, text))
    if symbol in BINARY_STRING_SYMBOLS and "string" in signature:
        hits.append(Hit("PUBLIC_BINARY_STRING_SIGNATURE", source, line, text))
    if symbol in FIXED_DIGEST_STRING_SYMBOLS and RETURN_STRING_RE.search(signature):
        hits.append(Hit("PUBLIC_FIXED_DIGEST_AS_STRING", source, line, text))
    if symbol in ARBITRARY_STRING_CREATOR_SYMBOLS and RETURN_STRING_RE.search(signature):
        hits.append(Hit("PUBLIC_ARBITRARY_STRING_CREATOR", source, line, text))
    if symbol in LEGACY_ALIAS_SYMBOLS:
        hits.append(Hit("PUBLIC_LEGACY_BINARY_ALIAS", source, line, text))
    if symbol in NULL_SENTINEL_SYMBOLS and NULL_SENTINEL_RE.search(signature):
        hits.append(Hit("PUBLIC_NULL_SENTINEL", source, line, text))
    if module in ARRAY_OWNER_INPUT_MODULES and ARRAY_OWNER_INPUT_RE.search(signature):
        hits.append(Hit("PUBLIC_ARRAY_OWNER_INPUT", source, line, text))
    if symbol in BOOL_SENTINEL_SYMBOLS and BOOL_RETURN_RE.search(signature):
        hits.append(Hit("PUBLIC_DOMAIN_BOOL_SENTINEL", source, line, text))
    return hits


def iter_text_files(root: Path):
    for dirname in SCAN_DIRS:
        base = root / dirname
        if not base.exists():
            continue
        for path in sorted(base.rglob("*")):
            rel_path = rel(root, path)
            if any(part in SKIP_DIR_NAMES for part in rel_path.parts):
                continue
            if len(rel_path.parts) >= 2 and rel_path.parts[:2] == ("docs", "tasks"):
                continue
            if rel_path == SCRIPT_REL_PATH:
                continue
            if path.is_file() and any(str(path).endswith(suffix) for suffix in TEXT_SUFFIXES):
                yield path


def classify_line(rel_path: Path, lineno: int, line: str) -> list[Hit]:
    rel_str = str(rel_path)
    stripped = line.strip()
    hits: list[Hit] = []

    is_comment = stripped.startswith(("//", "/*", "*"))
    is_generated_metadata = rel_path in GENERATED_METADATA_FILES
    if not is_comment and not is_generated_metadata and OLD_API_CALL_RE.search(line):
        hits.append(Hit("CONSUMER_OLD_BINARY_API_CALL", rel_str, lineno, stripped))
    if rel_path in GENERATED_METADATA_FILES and GENERATED_STALE_RE.search(line):
        hits.append(Hit("GENERATED_METADATA_STALE_BINARY_SURFACE", rel_str, lineno, stripped))
    if rel_str.startswith(NATIVE_BINARY_DIR_PREFIXES) and NATIVE_STRING_CREATOR_RE.search(line):
        hits.append(Hit("NATIVE_ARBITRARY_STRING_CREATOR", rel_str, lineno, stripped))
    return hits


def build_inventory(root: Path) -> dict[str, list[Hit]]:
    by_category: dict[str, list[Hit]] = defaultdict(list)
    for category in CATEGORIES:
        by_category[category]

    for item in load_source_inventory(root):
        if item.get("doc_surface") != "stdlib":
            continue
        for hit in classify_public_item(item):
            by_category[hit.category].append(hit)

    for path in iter_text_files(root):
        rel_path = rel(root, path)
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except UnicodeDecodeError:
            continue
        for lineno, line in enumerate(lines, 1):
            for hit in classify_line(rel_path, lineno, line):
                by_category[hit.category].append(hit)

    deduped: dict[str, list[Hit]] = {}
    for category in CATEGORIES:
        seen: set[Hit] = set()
        hits: list[Hit] = []
        for hit in by_category[category]:
            if hit in seen:
                continue
            seen.add(hit)
            hits.append(hit)
        deduped[category] = hits
    return deduped


def print_text_inventory(inventory: dict[str, list[Hit]], max_per_category: int) -> None:
    print("Task 200 binary stdlib surface inventory")
    for category, hits in inventory.items():
        print(f"{category}: {len(hits)}")
        shown = hits if max_per_category <= 0 else hits[:max_per_category]
        for hit in shown:
            print(f"  {hit.path}:{hit.line}: {hit.text}")
        if max_per_category > 0 and len(hits) > max_per_category:
            print(f"  ... {len(hits) - max_per_category} more")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--json", action="store_true", help="print machine-readable JSON")
    parser.add_argument(
        "--max-per-category",
        type=int,
        default=20,
        help="text output limit per category; 0 prints all hits",
    )
    parser.add_argument(
        "--fail-on-public-residue",
        action="store_true",
        help="fail if any task-200 public residue category is non-empty",
    )
    args = parser.parse_args()

    root = Path(args.root).resolve()
    inventory = build_inventory(root)

    if args.json:
        print(
            json.dumps(
                {category: [asdict(hit) for hit in hits] for category, hits in inventory.items()},
                indent=2,
                sort_keys=True,
            )
        )
    else:
        print_text_inventory(inventory, args.max_per_category)

    if args.fail_on_public_residue:
        blocking = {
            category: hits
            for category, hits in inventory.items()
            if category.startswith("PUBLIC_") and hits
        }
        if blocking:
            print("task-200 public binary stdlib surface gate failed:", file=sys.stderr)
            for category, hits in blocking.items():
                print(f"  {category}: {len(hits)}", file=sys.stderr)
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
