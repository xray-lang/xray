#!/usr/bin/env python3
"""Independently check the three fixed scalar Program wire known-answer vectors.

The historical 24/63 packets remain in checked_sizing_cases.h. Current 25/64
packets use the same explicit program and instruction roles, including the
RETURN opcode transition. No production writer or executable output is read.
"""
from pathlib import Path
import hashlib
import re
import struct


def fixed_program(name: bytes, schema: int, semantic: int, return_op: int) -> bytes:
    def u32(*values: int) -> bytes:
        return struct.pack("<" + "I" * len(values), *values)

    def instruction(op: int, value_type: int, immediate: int) -> bytes:
        return (u32(op, value_type, 0, 0, 0, 0)
                + struct.pack("<Q", immediate) + u32(0, 0))

    # Program linkage, one function, no declaration table, exact counted name.
    body = u32(0, 1, 0, len(name)) + name
    # No parameters, I64 result, one block with two instructions and no handler.
    body += u32(0, 2, 1, 0, 2, 0, 0, 2)
    body += instruction(2, 2, 42) + instruction(return_op, 0, 0)
    # No operands, generics, type nodes, nominal/interface tables, defaults,
    # or provenance. These are distinct counted fields, never omitted.
    body += u32(0, 0, 0, 0, 0, 0, 0)
    prefix = (b"XRCHK\0\0\0" + u32(schema, semantic, 2, 0)
              + struct.pack("<Q", len(body)))
    return prefix + hashlib.sha256(prefix + body).digest() + body


def array(source: str, name: str) -> bytes:
    match = re.search(r"static const uint8_t " + re.escape(name)
                      + r"\[(\d+)\] = \{(.*?)\};", source, re.S)
    if not match:
        raise AssertionError("missing fixed vector: " + name)
    result = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-f]{2})", match[2]))
    if len(result) != int(match[1]):
        raise AssertionError("counted array length mismatch: " + name)
    return result


def main() -> None:
    directory = Path(__file__).resolve().parent
    historical = (directory / "checked_sizing_cases.h").read_text()
    current = (directory / "checked_sizing64_golden.h").read_text()
    rows = [("sizing_golden", "sizing64_golden", b"main"),
            ("sizing_embedded_golden", "sizing64_embedded_golden", b"m\0in"),
            ("sizing_empty_golden", "sizing64_empty_golden", b"")]
    for old_name, new_name, name in rows:
        if array(historical, old_name) != fixed_program(name, 24, 63, 25):
            raise AssertionError("historical known-answer vector changed: " + old_name)
        if array(current, new_name) != fixed_program(name, 25, 64, 33):
            raise AssertionError("current independent vector mismatch: " + new_name)
    current65=(directory / 'checked_sizing65_golden.h').read_text()
    for _,old64_name,name in rows:
        new65_name=old64_name.replace('64','65')
        match=re.search(r'static const uint8_t '+new65_name+r'\[\] = \{(.*?)\};',current65,re.S)
        assert match and bytes(int(value,16) for value in re.findall(r'0x[0-9a-f]{2}',match[1]))==fixed_program(name,25,65,33)
    print('3 original63 + 3 complete64 + 3 current65 independent sizing packet vectors passed')


if __name__ == "__main__":
    main()
