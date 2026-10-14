"""Build independent complete group packets from explicit frozen wire fields."""
import argparse
import hashlib
from pathlib import Path
import struct


def words(*values):
    return struct.pack("<" + "I" * len(values), *values)


def instruction(op, result=0, first=0, count=0, immediate=0):
    return words(op, result, first, count, 0, 0) + struct.pack("<q", immediate) + words(0, 0)


def function(name, result, operations, operands=()):
    label = name.encode("ascii")
    # Name, zero parameters, result, one normal block, complete instructions,
    # then the flattened operand table. Each instruction has exactly 40 bytes.
    return (words(len(label)) + label + words(0, result, 1, 0, len(operations), 0, 0, len(operations))
            + b"".join(operations) + words(len(operands), *operands))


def packet(unit):
    if unit:
        operations = [instruction(145, immediate=3), instruction(33)]
        operands = ()
    else:
        operations = [instruction(2, 2, immediate=1), instruction(2, 2, immediate=2),
            instruction(143, 256, 0, 2), instruction(2, 2, immediate=3),
            instruction(2, 2, immediate=4), instruction(143, 256, 2, 2),
            instruction(145, 0, 4, 2, 3), instruction(33)]
        operands = (0, 1, 3, 4, 2, 5)
    # Program linkage, two functions and the one declaration graph.
    body = words(0, 2, 1) + function("init", 0, operations, operands)
    body += function("main", 2, [instruction(2, 2, immediate=41), instruction(33)])
    # One root module, three const slots, no literals, initializer0 and entry1.
    body += words(1, 3, 0, 0, 1) + words(4) + b"root" + words(0, 0)
    body += words(0, 0, 0, 0, 0, 0, 0, 0, 0)  # initializer identity
    body += words(0, 1, 0, 0, 0, 0, 0, 0, 0)  # exported entry identity
    body += words(0, 0 if unit else 256, 0, 0, 0, 0, 0, 0 if unit else 256, 0)
    body += words(0, 0)  # implementations and generic table absent
    # One Tuple<i64,i64>, no nominal/interface declarations; zero binder span.
    body += words(1, 0, 0, 6, 0, 2, 2, 2)
    # Exact dense nominal construction count, defaults and provenance absent.
    body += words(0, 0, 0)
    prefix = b"XRCHK\0\0\0" + words(28, 73, 2, 0) + struct.pack("<Q", len(body))
    return prefix + hashlib.sha256(prefix + body).digest() + body


def header():
    text = ["/*",
        " * xray - Lightweight typed scripting with native concurrency",
        " * https://www.xray-lang.org",
        " * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>",
        " * Licensed under the MIT License", " *",
        " * slot_group73_golden.h - Independent complete group publication fields",
        " */", "#ifndef SLOT_GROUP73_GOLDEN_H", "#define SLOT_GROUP73_GOLDEN_H", "#include <stdint.h>"]
    for name, unit in [("mixed", False), ("unit", True)]:
        data = packet(unit)
        text.append(f"static const uint8_t slot_group_{name}73_golden[] = {{")
        for offset in range(0, len(data), 12):
            text.append("    " + ",".join(f"0x{byte:02x}" for byte in data[offset:offset + 12]) + ",")
        text.append("};")
    text.append("#endif // SLOT_GROUP73_GOLDEN_H")
    return "\n".join(text) + "\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args()
    path = Path(__file__).with_suffix(".h")
    expected = header()
    if args.write:
        path.write_text(expected, encoding="utf-8")
    else:
        assert path.read_text("utf-8") == expected, path
    print(f"Independent group packets: mixed{len(packet(False))}, unit{len(packet(True))}; wire28/73 fields verified")


if __name__ == "__main__":
    main()
