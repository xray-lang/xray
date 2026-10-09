#!/usr/bin/env python3
"""Encode strict-range fields independently and preserve complete prior packets."""
import argparse
from hashlib import sha256
from pathlib import Path
import re
import struct


def words(*values):
    return struct.pack("<" + "I" * len(values), *values)


def instruction(opcode, result, first, count):
    return words(opcode, result, first, count, 0, 0) + struct.pack("<q", 0) + words(0, 0)


def packet(contract):
    body = words(0, 1, 0)
    body += words(5) + b"range"
    body += words(3, 2, 2, 2, 2)
    body += words(1, 0, 2, 0, 0, 2)
    body += instruction(148, 0, 0, 3) + instruction(33, 0, 0, 0)
    body += words(3, 0, 1, 2)
    body += words(0, 0, 0, 0, 0, 0)
    prefix = b"XRCHK\0\0\0" + struct.pack("<IIIIQ", 25, contract, 2, 0, len(body))
    return prefix + sha256(prefix + body).digest() + body


def historical(path, name):
    source = path.read_text(encoding="utf-8")
    match = re.search(r"static const uint8_t " + name + r"\[\] = \{(.*?)\};", source, re.S)
    assert match, name
    return bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", match.group(1)))


def render(data):
    lines = ["/* Complete named strict-range fields; no product writer is used. */",
             "#ifndef XIR_CHECKED_RANGE70_GOLDEN_H", "#define XIR_CHECKED_RANGE70_GOLDEN_H",
             "static const uint8_t checked_range70_golden[] = {"]
    lines += ["    " + ",".join("0x%02x" % b for b in data[i:i + 12]) + ","
              for i in range(0, len(data), 12)]
    return "\n".join(lines + ["};", "#endif // XIR_CHECKED_RANGE70_GOLDEN_H", ""])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    test = args.root / "tests/unit/xir"
    for contract, header, name in [(67, "xir_checked_range68_golden.h", "checked_range67_rejected"),
                                   (68, "xir_checked_range68_golden.h", "checked_range68_golden"),
                                   (68, "xir_checked_range69_golden.h", "checked_range68_rejected"),
                                   (69, "xir_checked_range69_golden.h", "checked_range69_golden")]:
        assert historical(test / header, name) == packet(contract), name
    data = packet(70)
    assert all(packet(contract)[64:] == data[64:] for contract in [67, 68, 69])
    if args.output:
        args.output.write_text(render(data), encoding="utf-8", newline="\n")
    else:
        assert (test / "xir_checked_range70_golden.h").read_text(encoding="utf-8") == render(data)
    print("range named70 full KAT bytes=%d sha256=%s complete67/68/69 model=exact body=same no-writer"
          % (len(data), sha256(data).hexdigest()))


if __name__ == "__main__":
    main()
