#!/usr/bin/env python3
"""Generate Xray's Unicode grapheme property tables from vendored UCD data."""

from __future__ import annotations

import argparse
import hashlib
import sys
from dataclasses import dataclass
from pathlib import Path


UNICODE_VERSION = "17.0.0"
UAX29_REVISION = 47
GENERATOR_REVISION = 1
CODE_POINT_COUNT = 0x110000
PAGE_SIZE = 256
PAGE_COUNT = CODE_POINT_COUNT // PAGE_SIZE

REQUIRED_INPUTS = (
    "DerivedCoreProperties.txt",
    "GraphemeBreakProperty.txt",
    "GraphemeBreakTest.txt",
    "LICENSE.txt",
    "emoji-data.txt",
)

GCB_VALUES = {
    "Other": 0,
    "CR": 1,
    "LF": 2,
    "Control": 3,
    "Extend": 4,
    "ZWJ": 5,
    "Regional_Indicator": 6,
    "Prepend": 7,
    "SpacingMark": 8,
    "L": 9,
    "V": 10,
    "T": 11,
    "LV": 12,
    "LVT": 13,
}

INCB_VALUES = {
    "None": 0,
    "Consonant": 1,
    "Extend": 2,
    "Linker": 3,
}


class GenerationError(RuntimeError):
    pass


@dataclass(frozen=True)
class GeneratedFile:
    path: Path
    content: str


def sha256(path: Path) -> str:
    content = path.read_bytes().replace(b"\r\n", b"\n")
    return hashlib.sha256(content).hexdigest()


def read_manifest(unicode_dir: Path) -> dict[str, str]:
    manifest_path = unicode_dir / "SHA256SUMS"
    if not manifest_path.is_file():
        raise GenerationError(f"missing checksum manifest: {manifest_path}")
    entries: dict[str, str] = {}
    for line_no, raw in enumerate(manifest_path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) != 2 or len(parts[0]) != 64:
            raise GenerationError(f"{manifest_path}:{line_no}: malformed SHA256 entry")
        digest, name = parts
        if name.startswith("*"):
            name = name[1:]
        if name in entries:
            raise GenerationError(f"{manifest_path}:{line_no}: duplicate entry for {name}")
        entries[name] = digest.lower()
    return entries


def verify_inputs(unicode_dir: Path) -> dict[str, str]:
    manifest = read_manifest(unicode_dir)
    expected = set(REQUIRED_INPUTS)
    missing = expected - manifest.keys()
    extra = manifest.keys() - expected
    if missing or extra:
        raise GenerationError(
            f"SHA256SUMS input set mismatch: missing={sorted(missing)}, extra={sorted(extra)}"
        )
    actual: dict[str, str] = {}
    for name in REQUIRED_INPUTS:
        path = unicode_dir / name
        if not path.is_file():
            raise GenerationError(f"missing Unicode input: {path}")
        digest = sha256(path)
        if digest != manifest[name]:
            raise GenerationError(
                f"checksum mismatch for {name}: expected {manifest[name]}, got {digest}"
            )
        actual[name] = digest

    version_markers = {
        "DerivedCoreProperties.txt": f"DerivedCoreProperties-{UNICODE_VERSION}.txt",
        "GraphemeBreakProperty.txt": f"GraphemeBreakProperty-{UNICODE_VERSION}.txt",
        "GraphemeBreakTest.txt": f"GraphemeBreakTest-{UNICODE_VERSION}.txt",
        "emoji-data.txt": "Version: 17.0",
    }
    for name, marker in version_markers.items():
        prefix = (unicode_dir / name).read_text(encoding="utf-8")[:4096]
        if marker not in prefix:
            raise GenerationError(f"{name}: expected version marker {marker!r}")
    return actual


def parse_code_point_range(text: str, path: Path, line_no: int) -> tuple[int, int]:
    fields = text.split("..", 1)
    try:
        start = int(fields[0], 16)
        end = int(fields[1], 16) if len(fields) == 2 else start
    except ValueError as error:
        raise GenerationError(f"{path}:{line_no}: invalid code point range {text!r}") from error
    if start > end or end >= CODE_POINT_COUNT:
        raise GenerationError(f"{path}:{line_no}: out-of-range code point range {text!r}")
    return start, end


def data_lines(path: Path):
    for line_no, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if line:
            yield line_no, line


def assign_range(
    values: bytearray,
    assigned: bytearray,
    start: int,
    end: int,
    value: int,
    path: Path,
    line_no: int,
    domain: str,
) -> None:
    for code_point in range(start, end + 1):
        if assigned[code_point]:
            raise GenerationError(
                f"{path}:{line_no}: overlapping {domain} assignment at U+{code_point:04X}"
            )
        assigned[code_point] = 1
        values[code_point] = value


def parse_gcb(path: Path) -> bytearray:
    values = bytearray(CODE_POINT_COUNT)
    assigned = bytearray(CODE_POINT_COUNT)
    for line_no, line in data_lines(path):
        parts = [part.strip() for part in line.split(";")]
        if len(parts) != 2:
            raise GenerationError(f"{path}:{line_no}: malformed GCB row")
        range_text, property_name = parts
        if property_name not in GCB_VALUES or property_name == "Other":
            raise GenerationError(f"{path}:{line_no}: unknown/explicit GCB value {property_name!r}")
        start, end = parse_code_point_range(range_text, path, line_no)
        assign_range(
            values,
            assigned,
            start,
            end,
            GCB_VALUES[property_name],
            path,
            line_no,
            "GCB",
        )
    return values


def parse_incb(path: Path) -> bytearray:
    values = bytearray(CODE_POINT_COUNT)
    assigned = bytearray(CODE_POINT_COUNT)
    for line_no, line in data_lines(path):
        parts = [part.strip() for part in line.split(";")]
        if len(parts) < 2 or parts[1] != "InCB":
            continue
        if len(parts) != 3:
            raise GenerationError(f"{path}:{line_no}: malformed InCB row")
        range_text, _, property_name = parts
        if property_name not in INCB_VALUES or property_name == "None":
            raise GenerationError(f"{path}:{line_no}: unknown/explicit InCB value {property_name!r}")
        start, end = parse_code_point_range(range_text, path, line_no)
        assign_range(
            values,
            assigned,
            start,
            end,
            INCB_VALUES[property_name],
            path,
            line_no,
            "InCB",
        )
    return values


def parse_extended_pictographic(path: Path) -> bytearray:
    values = bytearray(CODE_POINT_COUNT)
    assigned = bytearray(CODE_POINT_COUNT)
    known_properties = {
        "Emoji",
        "Emoji_Component",
        "Emoji_Modifier",
        "Emoji_Modifier_Base",
        "Emoji_Presentation",
        "Extended_Pictographic",
    }
    for line_no, line in data_lines(path):
        parts = [part.strip() for part in line.split(";")]
        if len(parts) != 2:
            raise GenerationError(f"{path}:{line_no}: malformed emoji property row")
        range_text, property_name = parts
        if property_name not in known_properties:
            raise GenerationError(f"{path}:{line_no}: unknown emoji property {property_name!r}")
        if property_name != "Extended_Pictographic":
            continue
        start, end = parse_code_point_range(range_text, path, line_no)
        assign_range(values, assigned, start, end, 1, path, line_no, property_name)
    return values


def combine_properties(gcb: bytearray, incb: bytearray, ep: bytearray) -> bytes:
    result = bytearray(CODE_POINT_COUNT)
    for code_point in range(CODE_POINT_COUNT):
        word = gcb[code_point] | (ep[code_point] << 4) | (incb[code_point] << 5)
        if word & 0x80:
            raise GenerationError(f"reserved property bit set for U+{code_point:04X}")
        result[code_point] = word
    return bytes(result)


def deduplicate_pages(properties: bytes) -> tuple[list[int], list[bytes]]:
    page_to_block: list[int] = []
    blocks: list[bytes] = []
    block_ids: dict[bytes, int] = {}
    for page_index in range(PAGE_COUNT):
        start = page_index * PAGE_SIZE
        page = properties[start : start + PAGE_SIZE]
        block_id = block_ids.get(page)
        if block_id is None:
            block_id = len(blocks)
            if block_id > 0xFFFF:
                raise GenerationError("deduplicated block count exceeds uint16_t")
            block_ids[page] = block_id
            blocks.append(page)
        page_to_block.append(block_id)
    if page_to_block[0] != 0:
        raise GenerationError("ASCII page must be emitted as property block zero")
    return page_to_block, blocks


def format_values(values: list[int] | bytes, columns: int, formatter) -> list[str]:
    lines: list[str] = []
    for start in range(0, len(values), columns):
        chunk = values[start : start + columns]
        lines.append("    " + ", ".join(formatter(value) for value in chunk) + ",")
    return lines


def fnv1a64(data: bytes) -> int:
    value = 0xCBF29CE484222325
    for byte in data:
        value ^= byte
        value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def render_data(properties: bytes, page_to_block: list[int], blocks: list[bytes]) -> str:
    property_digest = hashlib.sha256(properties).hexdigest()
    lines = [
        "/* Generated by scripts/gen_unicode_grapheme_tables.py; do not edit. */",
        f"/* Unicode {UNICODE_VERSION}; UAX #29 revision {UAX29_REVISION}; UAX29-C1-1. */",
        f"/* Full property-word SHA256: {property_digest}. */",
        "",
        f"#define XR_GRAPHEME_PAGE_COUNT {PAGE_COUNT}u",
        f"#define XR_GRAPHEME_BLOCK_COUNT {len(blocks)}u",
        "",
        "static const uint16_t xr_grapheme_page_to_block[XR_GRAPHEME_PAGE_COUNT] = {",
    ]
    lines.extend(format_values(page_to_block, 16, lambda value: str(value)))
    lines.extend(
        [
            "};",
            "",
            "static const uint8_t",
            "    xr_grapheme_property_blocks[XR_GRAPHEME_BLOCK_COUNT][256] = {",
        ]
    )
    for block_id, block in enumerate(blocks):
        lines.append(f"    /* block {block_id} */ {{")
        lines.extend(format_values(block, 16, lambda value: f"0x{value:02x}"))
        lines.append("    },")
    lines.extend(["};", ""])
    return "\n".join(lines)


def render_version(properties: bytes, hashes: dict[str, str], block_count: int) -> str:
    property_digest = hashlib.sha256(properties).hexdigest()
    def string_macro(name: str, value: str) -> str:
        prefix = f"#define {name}"
        return f'{prefix}{" " * (99 - len(prefix))}\\\n    "{value}"'

    return f"""/* Generated by scripts/gen_unicode_grapheme_tables.py; do not edit. */
#ifndef XR_UNICODE_GRAPHEME_VERSION_H
#define XR_UNICODE_GRAPHEME_VERSION_H

#include <stdint.h>

#define XR_UNICODE_GRAPHEME_VERSION \"{UNICODE_VERSION}\"
#define XR_UNICODE_GRAPHEME_UAX29_REVISION {UAX29_REVISION}
#define XR_UNICODE_GRAPHEME_PROFILE \"UAX29-C1-1\"
#define XR_UNICODE_GRAPHEME_GENERATOR_REVISION {GENERATOR_REVISION}
#define XR_UNICODE_GRAPHEME_BLOCK_COUNT {block_count}
{string_macro('XR_UNICODE_GRAPHEME_PROPERTY_SHA256', property_digest)}
#define XR_UNICODE_GRAPHEME_PROPERTY_FNV1A64 UINT64_C(0x{fnv1a64(properties):016x})
{string_macro('XR_UNICODE_GRAPHEME_GCB_SHA256', hashes['GraphemeBreakProperty.txt'])}
{string_macro('XR_UNICODE_GRAPHEME_INCB_SHA256', hashes['DerivedCoreProperties.txt'])}
{string_macro('XR_UNICODE_GRAPHEME_EMOJI_SHA256', hashes['emoji-data.txt'])}
{string_macro('XR_UNICODE_GRAPHEME_TEST_SHA256', hashes['GraphemeBreakTest.txt'])}

#endif /* XR_UNICODE_GRAPHEME_VERSION_H */
"""


def generated_files(unicode_dir: Path, output_dir: Path) -> list[GeneratedFile]:
    hashes = verify_inputs(unicode_dir)
    gcb = parse_gcb(unicode_dir / "GraphemeBreakProperty.txt")
    incb = parse_incb(unicode_dir / "DerivedCoreProperties.txt")
    ep = parse_extended_pictographic(unicode_dir / "emoji-data.txt")
    properties = combine_properties(gcb, incb, ep)
    page_to_block, blocks = deduplicate_pages(properties)
    return [
        GeneratedFile(
            output_dir / "xr_unicode_grapheme_data.inc",
            render_data(properties, page_to_block, blocks),
        ),
        GeneratedFile(
            output_dir / "xr_unicode_grapheme_version.h",
            render_version(properties, hashes, len(blocks)),
        ),
    ]


def write_or_check(files: list[GeneratedFile], check: bool) -> None:
    stale: list[Path] = []
    for generated in files:
        if check:
            try:
                existing = generated.path.read_text(encoding="utf-8")
            except FileNotFoundError:
                stale.append(generated.path)
                continue
            if existing != generated.content:
                stale.append(generated.path)
        else:
            generated.path.parent.mkdir(parents=True, exist_ok=True)
            with generated.path.open("w", encoding="utf-8", newline="\n") as stream:
                stream.write(generated.content)
    if stale:
        names = ", ".join(str(path) for path in stale)
        raise GenerationError(f"generated grapheme tables are stale: {names}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--unicode-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, default=Path("src/shared"))
    parser.add_argument("--check", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        files = generated_files(args.unicode_dir, args.output_dir)
        write_or_check(files, args.check)
    except (GenerationError, OSError, UnicodeError) as error:
        print(f"unicode grapheme generation failed: {error}", file=sys.stderr)
        return 1
    action = "verified" if args.check else "generated"
    print(f"{action} Unicode {UNICODE_VERSION} grapheme tables")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
