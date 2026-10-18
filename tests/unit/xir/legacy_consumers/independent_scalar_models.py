"""Compute preserved integer observations with arbitrary-precision host arithmetic."""
from __future__ import annotations

from frozen_derivation_inputs import resolve as resolve_derivation
import hashlib
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]


def wrap(value: int, width: int, signed: bool) -> int:
    value %= 1 << width
    return value - (1 << width) if signed and value >= (1 << (width - 1)) else value


def values() -> dict[str, list[int]]:
    s8 = lambda n: wrap(n, 8, True)
    u8 = lambda n: wrap(n, 8, False)
    s16 = lambda n: wrap(n, 16, True)
    u16 = lambda n: wrap(n, 16, False)
    s32 = lambda n: wrap(n, 32, True)
    u32 = lambda n: wrap(n, 32, False)
    u64 = lambda n: wrap(n, 64, False)
    high = 1 << 63
    mask = (1 << 64) - 1
    h = 0x27D4EB2F165667C5
    h ^= h >> 33
    h = u64(h * 0xC2B2AE3D27D4EB4F)
    h ^= h >> 29
    return {
        'add_overflow': [wrap(high, 64, True), wrap(-high - 1, 64, True), wrap(2 * (high - 1), 64, True)],
        'int_as_truncation': [u32(4294967297), u32(4294967297), u8(-1), u8(-1), s32(2147483648), u32(4294967297)],
        'narrow_arith_wrap': [
            s8(127 + 1), s8(-128 - 1), s8(100 * 50), s8(-100 * 50),
            u8(255 + 1), u8(0 - 1), u8(200 * 100),
            s16(32767 + 1), s16(-32768 - 1), s16(200 * 250), s16(-200 * 250),
            u16(65535 + 1), u16(0 - 1), u16(60000 * 60000), u16(300 * 300),
            # Same-sign mixed operands use their wider operand domain before return widening.
            s16(127 + 32767), s16(-128 - 32768),
            u32(4000000000 + 1000000000), u32(0 - 1), u32(100000 * 100000),
            s32(2147483647 + 1), s32(-2147483648 - 1), s32(100000 * 100000),
            u8(128 << 1), u8(128 >> 1),
        ],
        'uint64_large_literals': [0x9E3779B185EBCA87, mask, high, mask, high, u64(-1), -high],
        'uint64_shift_logical': [high, high >> 1, high >> 63, high >> 0, high >> 1, high >> 63,
                                 mask >> 33, mask >> 63, h, -8 >> 1, -8 >> 1, 0x80000000 >> 31, 128 >> 7, high >> 62],
    }


def expected_rows() -> list[dict]:
    bindings = json.loads((HERE / 'independent_scalar_bindings.json').read_text(encoding='utf-8'))
    for entry in bindings['inputs']:
        data = resolve_derivation(ROOT, entry['path'], entry['lf_sha256']).read_bytes().replace(b'\r\n', b'\n')
        if hashlib.sha256(data).hexdigest() != entry['lf_sha256']:
            raise ValueError('independent scalar source or arithmetic definition changed: ' + entry['path'])
    result = []
    for name, expected in values().items():
        result.append({'id': 'independent_scalar_' + name,
                       'expected': {'exit_code': 0, 'stdout_utf8': ''.join(str(v) + '\n' for v in expected), 'stderr_utf8': ''},
                       'source': 'tests/diff/cases/semantics/int_wrap/' + name + '.xr',
                       'model': 'Arbitrary-precision modulo and signed interpretation; no Xray execution output is read.'})
    return result
