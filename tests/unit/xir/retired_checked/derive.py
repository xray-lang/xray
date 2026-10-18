#!/usr/bin/env python3
"""Recover complete retired Checked packets from their immutable Git sources."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[3]
FIXED_REFS = {
    "P10": "ed5f98a2fa76d980d31daf5ad4e7125e6d22c384",
    "P09": "3fcf11abc70f91814bbbfcb962de721ef59f194d",
    "P08": "26dc3c329790b717fb8f40b67f1ac2f210681dd0",
    "P11": "cefd63eef5436823c6c1138f3be17b60a879501c",
    "P12": "93ea058ccc9c73f2d7b44b0577e486908a249ee3",
    "P13": "55cecf9927cd1d8807adb063cfb0251a2f548525",
}
ARRAY = re.compile(rb"static\s+const\s+uint8_t\s+(\w+)\s*\[[^]]*\]\s*=\s*\{([^}]+)\}\s*;", re.S)
NUMBER = re.compile(rb"(?:0[xX][0-9a-fA-F]+|[0-9]+)[uUlL]*")
HEADER = """/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * Complete immutable packet bytes; regenerate with derive.py.
 */
"""


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def git(*args, data=None):
    result = subprocess.run(["git", *args], cwd=ROOT, input=data,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
    return result.stdout


def tree(ref):
    rows = {}
    for record in git("ls-tree", "-r", "-z", ref, "--", "tests/unit/xir").split(b"\0"):
        if not record:
            continue
        meta, path = record.split(b"\t", 1)
        _, kind, identity = meta.split()
        if kind == b"blob" and path.endswith((b".h", b".c")):
            rows[path.decode()] = identity.decode()
    return rows


def blobs(identities):
    result = {}
    ordered = sorted(identities)
    for start in range(0, len(ordered), 64):
        ids = ordered[start:start + 64]
        raw = git("cat-file", "--batch", data=("\n".join(ids) + "\n").encode())
        position = 0
        for expected in ids:
            end = raw.index(b"\n", position)
            identity, kind, length = raw[position:end].split()
            require(identity.decode() == expected and kind == b"blob", "Wrong Git object")
            length = int(length)
            body = raw[end + 1:end + 1 + length]
            position = end + length + 2
            canonical = b"blob " + str(length).encode() + b"\0" + body
            require(hashlib.sha1(canonical).hexdigest() == expected, "Git blob digest mismatch")
            result[expected] = body
        require(position == len(raw), "Trailing Git batch bytes")
    return result


def arrays(source):
    result = {}
    for match in ARRAY.finditer(source):
        body = re.sub(rb"/\*.*?\*/|//[^\n]*", b"", match[2], flags=re.S)
        tokens = [value.strip() for value in body.split(b",") if value.strip()]
        if not all(NUMBER.fullmatch(token) for token in tokens):
            continue
        values = [int(re.sub(rb"[uUlL]+$", b"", token), 16 if token.lower().startswith(b"0x") else 10)
                  for token in tokens]
        if any(value > 255 for value in values):
            continue
        data = bytes(values)
        if not complete(data):
            continue
        name = match[1].decode()
        require(name not in result, "Repeated array " + name)
        result[name] = data
    return result


def complete(data):
    return (len(data) >= 64 and data[:8] == b"XRCHK\0\0\0" and
            int.from_bytes(data[24:32], "little") == len(data) - 64 and
            hashlib.sha256(data[:32] + data[64:]).digest() == data[32:64])


def verify(data, row):
    require(complete(data), "Not a complete authenticated packet")
    require(len(data) == row["bytes"], "Packet length mismatch")
    require(hashlib.sha256(data).hexdigest() == row["sha256"], "Packet SHA-256 mismatch")
    require(int.from_bytes(data[8:12], "little") == row["wire"], "Wire identity mismatch")
    require(int.from_bytes(data[12:16], "little") == row["semantic"], "Semantic identity mismatch")


def recover(manifest):
    require(manifest["fixed_refs"] == FIXED_REFS, "Immutable inputs changed")
    require(manifest["original_references"] == 3718, "Original reference denominator changed")
    require(manifest["distinct_complete_packets"] == 1213, "Distinct packet denominator changed")
    require(manifest["wire_semantic_pairs"] == 45, "Identity denominator changed")
    trees = {label: tree(ref) for label, ref in FIXED_REFS.items()}
    bodies = blobs({identity for rows in trees.values() for identity in rows.values()})
    decoded = {identity: arrays(body) for identity, body in bodies.items()}
    discovered = {}
    for label, rows in trees.items():
        for path, blob in rows.items():
            for symbol, data in decoded[blob].items():
                if complete(data):
                    discovered[(label, path, symbol, blob)] = data
    selected, recovered, pairs = set(), [], set()
    require(len(manifest["packets"]) == 1213, "Incomplete manifest")
    require([row["sha256"] for row in manifest["packets"]] ==
            sorted({row["sha256"] for row in manifest["packets"]}), "Noncanonical packet ordering")
    for row in manifest["packets"]:
        require((row["wire"], row["semantic"]) != (34, 90), "Current identity is not retired")
        pairs.add((row["wire"], row["semantic"]))
        require(row["references"], "Missing source provenance")
        packet = None
        for source in row["references"]:
            label = source["label"]
            require(source["ref"] == FIXED_REFS[label], "Wrong source commit")
            key = (label, source["path"], source["symbol"], source["blob"])
            require(key not in selected, "Repeated source reference")
            selected.add(key)
            require(key in discovered, "Source packet is absent from its Git tree")
            data = discovered[key]
            verify(data, row)
            require(packet is None or packet == data, "Distinct bytes share one identity")
            packet = data
        recovered.append(packet)
    require(len(selected) == 3718 and len(pairs) == 45, "Incomplete provenance")
    require(selected == set(discovered), "Complete packet discovery differs from the manifest")
    return recovered


def c_array(name, data):
    lines = ["static const uint8_t " + name + "[] = {\n"]
    for position in range(0, len(data), 32):
        lines.append("    " + ",".join("0x%02x" % byte for byte in data[position:position + 32]) + ",\n")
    return "".join(lines) + "};\n"


def render(manifest, packets):
    outputs, includes, part, lines, number = {}, [], HEADER, HEADER.count("\n"), 0
    for index, data in enumerate(packets):
        value = c_array("retired_%04d" % index, data)
        if lines + value.count("\n") > 2700:
            name = "packets_%02d.inc.c" % number
            outputs[name] = part
            includes.append('#include "' + name + '"\n')
            part, lines, number = HEADER, HEADER.count("\n"), number + 1
        part += value
        lines += value.count("\n")
    name = "packets_%02d.inc.c" % number
    outputs[name] = part
    includes.append('#include "' + name + '"\n')
    catalog = HEADER + "".join(includes) + "\nstatic const RetiredPacket retired_packets[] = {\n"
    for index, row in enumerate(manifest["packets"]):
        digest = ",".join("0x" + row["sha256"][i:i + 2] for i in range(0, 64, 2))
        catalog += ('    {retired_%04d,sizeof(retired_%04d),%du,%du,{%s}},\n' %
                    (index, index, row["wire"], row["semantic"], digest))
    outputs["catalog.inc.c"] = catalog + "};\n"
    controls = manifest["current_controls"]
    require(len(controls) == 2, "Two original complete current controls are required")
    current = HEADER
    for index, row in enumerate(controls):
        require(row["expected"] == "XR_XIR_OK" and (row["wire"], row["semantic"]) == (34, 90),
                "Current positive control changed")
        data = arrays((ROOT / row["path"]).read_bytes())[row["symbol"]]
        verify(data, row)
        current += c_array("current_%d" % index, data)
    current += "static const CurrentPacket current_packets[] = {\n"
    for index, row in enumerate(controls):
        current += '    {"%s",current_%d,sizeof(current_%d)},\n' % (row["name"], index, index)
    outputs["current.inc.c"] = current + "};\n"
    return outputs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true", help="write derived C arrays")
    args = parser.parse_args()
    manifest = json.loads((HERE / "original_packets.json").read_text(encoding="utf-8"))
    packets = recover(manifest)
    outputs = render(manifest, packets)
    existing = {path.name for path in HERE.glob("*.inc.c")}
    require(not existing - set(outputs), "Unexpected derived file")
    for name, text in outputs.items():
        target = HERE / name
        require(text.count("\n") <= 3000, "Derived C file exceeds its size bound")
        if args.write:
            target.write_text(text, encoding="utf-8", newline="\n")
        else:
            require(target.is_file() and target.read_text(encoding="utf-8") == text,
                    "Derived bytes differ: " + name)
    print("3718 immutable references, 1213 complete packets, 45 identities, two current controls: PASS")


if __name__ == "__main__":
    main()
