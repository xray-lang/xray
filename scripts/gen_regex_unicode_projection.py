"""Project canonical Unicode property tables into private ordinary Regex functions."""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re
import sys

BEGIN = "// BEGIN GENERATED UNICODE PROPERTY QUERIES"
END = "// END GENERATED UNICODE PROPERTY QUERIES"


def without_comments(text: str) -> str:
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)


def canonical(root: Path):
    header = root / "src/base/xunicode.h"
    source = root / "src/base/xunicode.c"
    declaration = re.search(r"typedef enum\s*\{(.*?)\}\s*XrUnicodeProperty;",
                            without_comments(header.read_text(encoding="utf-8")), re.S)
    if not declaration:
        raise ValueError("Missing canonical Unicode property enum")
    entries = [x.strip() for x in declaration[1].split(",") if x.strip()]
    if entries[0] != "XR_UP_INVALID = 0" or entries[-1] != "XR_UP_COUNT":
        raise ValueError("Canonical Unicode property ID bounds changed")
    properties = {}
    for ordinal, entry in enumerate(entries[1:-1], 1):
        match = re.fullmatch(r"XR_UP_(\w+)", entry)
        if not match or match[1] in properties:
            raise ValueError("Unicode property IDs must be unique and sequential")
        properties[match[1]] = ordinal
    text = without_comments(source.read_text(encoding="utf-8"))
    tables = {}
    number = r"(?:0[xX][0-9a-fA-F]+|[0-9]+)"
    item = r"\{\s*(" + number + r")\s*,\s*(" + number + r")\s*\}"
    for match in re.finditer(r"static const XrUnicodeRange ranges_(\w+)\[\]\s*=\s*\{(.*?)\};", text, re.S):
        name, body = match[1], match[2]
        if name in tables or re.sub(item + r"\s*,?", "", body).strip():
            raise ValueError("Ambiguous or unsupported canonical Unicode range syntax")
        ranges = [(int(lo, 0), int(hi, 0)) for lo, hi in re.findall(item, body)]
        if not ranges or any(not (0 <= lo <= hi <= 0x10ffff) for lo, hi in ranges):
            raise ValueError("Invalid canonical Unicode range")
        if any(ranges[i - 1][1] >= ranges[i][0] for i in range(1, len(ranges))):
            raise ValueError("Canonical Unicode ranges must be sorted without overlap")
        tables[name] = ranges
    if set(tables) != set(properties):
        raise ValueError("Every canonical Unicode property needs its exact range table")
    names = re.search(r"static const PropertyEntry property_table\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if not names:
        raise ValueError("Missing canonical Unicode property names")
    aliases = re.findall(r'\{\s*"([A-Za-z_]+)"\s*,\s*XR_UP_(\w+)\s*\}', names[1])
    remainder = re.sub(r'\{\s*"[A-Za-z_]+"\s*,\s*XR_UP_\w+\s*\}\s*,?', "", names[1])
    if not re.fullmatch(r"\s*\{\s*NULL\s*,\s*XR_UP_INVALID\s*\}\s*", remainder):
        raise ValueError("Unsupported property-name syntax or missing sole terminator")
    if (not aliases or len({name.lower() for name, _ in aliases}) != len(aliases) or
            {prop for _, prop in aliases} != set(properties)):
        raise ValueError("Property aliases must be unique and cover every canonical property")
    digest = hashlib.sha256(header.read_text(encoding="utf-8").encode("utf-8") + b"\0" +
                            source.read_text(encoding="utf-8").encode("utf-8")).hexdigest()
    return properties, tables, aliases, digest


def projection(root: Path) -> str:
    properties, tables, aliases, digest = canonical(root)
    lines = [BEGIN, "// Generated from the canonical Unicode property enum, ranges and aliases.",
             "// The Source module carries these ordinary bodies and their exact profile.",
             "// Canonical input SHA-256: " + digest,
             "fn rxUnicodePropertyId(name: string) -> i64 {",
             f"    if (len(name) == 0 || len(name) > {max(len(name) for name, _ in aliases)}) {{ return -1 }}",
             "    var bytes = name.copyBytes()", '    var normalized = ""',
             "    var i = 0", "    while (i < len(bytes)) {",
             "        var value = bytes[i] as i64", "        if (value > 127) { return -1 }",
             "        if (value >= 65 && value <= 90) { value = value + 32 }",
             "        normalized = normalized + string(rune(value))",
             "        i = i + 1", "    }"]
    for name, prop in aliases:
        lines.append(f'    if (normalized == "{name.lower()}") {{ return {properties[prop]} }}')
    offsets, endpoints = [len(properties) + 1], []
    for name in sorted(properties, key=properties.get):
        endpoints.extend(value for pair in tables[name] for value in pair)
        offsets.append(len(properties) + 1 + len(endpoints))
    values = ",".join(map(str, offsets + endpoints))
    lines += ["    return -1", "}", "", "fn rxUnicodeRangeData() -> Array<i64> {",
              f"    return [{values}]", "}", "", "fn rxUnicodeRanges(property: i64) -> Array<i64> {",
              f"    if (property < 1 || property > {len(properties)}) {{ return [] }}",
              "    var data = rxUnicodeRangeData()", "    var result: Array<i64> = []",
              "    var i = data[property - 1]", "    var end = data[property]",
              "    while (i < end) { result.push(data[i]); i = i + 1 }", "    return result",
              "}", "", "fn rxUnicodeHasProperty(cp: i64, property: i64) -> bool {",
              "    if (cp < 0 || cp > 1114111 || (cp >= 55296 && cp <= 57343)) { return false }",
              "    var ranges = rxUnicodeRanges(property)", "    var lo = 0", "    var hi = len(ranges) / 2",
              "    while (lo < hi) {", "        var mid = lo + (hi - lo) / 2",
              "        if (cp < ranges[mid * 2]) { hi = mid }",
              "        else if (cp > ranges[mid * 2 + 1]) { lo = mid + 1 }",
              "        else { return true }", "    }", "    return false", "}", END]
    return "\n".join(lines) + "\n"


def render(root: Path) -> str:
    path = root / "stdlib/regex/regex.xr"
    text = path.read_text(encoding="utf-8")
    if not text.count(BEGIN) and not text.count(END):
        return text.rstrip() + "\n\n" + projection(root)
    if text.count(BEGIN) != 1 or text.count(END) != 1 or text.index(BEGIN) > text.index(END):
        raise ValueError("Regex must have one complete private Unicode generation region")
    first, last = text.index(BEGIN), text.index(END) + len(END)
    return text[:first] + projection(root).rstrip("\n") + text[last:]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        output = render(root)
        path = root / "stdlib/regex/regex.xr"
        if args.check:
            if path.read_text(encoding="utf-8") != output:
                raise ValueError("Private ordinary Regex Unicode projection is stale; regenerate it")
        elif path.read_text(encoding="utf-8") != output:
            path.write_text(output, encoding="utf-8", newline="\n")
        properties, tables, aliases, digest = canonical(root)
        print(f"Canonical Unicode projection: {len(properties)} properties, {len(aliases)} aliases, "
              f"{sum(map(len, tables.values()))} ranges, input SHA-256 {digest}")
    except (OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
