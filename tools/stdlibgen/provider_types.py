"""Bounded logical provider types, independent of host C representations."""

from __future__ import annotations

import hashlib
import re
import struct


SCALARS = {"()": b"\x01", "bool": b"\x02", "i64": b"\x03", "string": b"\x04"}


def native_resource_id(module: str, name: str) -> bytes:
    digest = hashlib.sha256(b"xray-stdlib-resource-v1\0")
    for text in (module, name):
        raw = text.encode("utf-8")
        digest.update(struct.pack("<Q", len(raw)))
        digest.update(raw)
    return digest.digest()[:16]


def tuple_parts(text: str) -> tuple[str, ...]:
    depth = 0
    start = 0
    parts = []
    for index, char in enumerate(text):
        if char in "(<":
            depth += 1
        elif char in ")>":
            depth -= 1
        elif char == "," and depth == 0:
            parts.append(text[start:index].strip())
            start = index + 1
        if depth < 0:
            raise ValueError("unbalanced provider type")
    parts.append(text[start:].strip())
    if depth or not all(parts):
        raise ValueError("incomplete provider type")
    return tuple(parts)


def logical_type(text: str, depth: int = 0) -> tuple[bytes, bool]:
    text = text.strip()
    if depth > 32 or len(text) > 4096:
        raise ValueError("provider type exceeds its bound")
    if text == "Array<u8>":
        return b"\x08", True
    if text in SCALARS:
        return SCALARS[text], text == "string"
    resource = re.fullmatch(r"resource<([A-Za-z_][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)>", text)
    if resource:
        return b"\x07" + native_resource_id(*resource.groups()), True
    if text.endswith("?"):
        encoded, owned = logical_type(text[:-1], depth + 1)
        return b"\x06" + encoded, owned
    if text.startswith("(") and text.endswith(")"):
        children = [logical_type(part, depth + 1) for part in tuple_parts(text[1:-1])]
        if not 1 <= len(children) <= 62:
            raise ValueError("provider tuple exceeds its bound")
        return b"\x05" + bytes([len(children)]) + b"".join(row[0] for row in children), any(row[1] for row in children)
    raise ValueError("unsupported provider type: " + text)


def resolve_native_types(text: str, module: str, native_names: set[str]) -> str:
    """Only same-module registered storage declarations acquire resource identity."""
    return re.sub(r"[A-Za-z_][A-Za-z0-9_]*", lambda match:
                  f"resource<{module}.{match[0]}>" if match[0] in native_names else match[0], text)
