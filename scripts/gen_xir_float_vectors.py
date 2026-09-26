"""Generate exact rational floating conversion witnesses without a host float oracle."""
from pathlib import Path
from fractions import Fraction
import argparse


def shape(width):
    return (23, 127, 0x7f800000, 0x7fc00000) if width == 32 else (52, 1023, 0x7ff0000000000000, 0x7ff8000000000000)


def power(exponent):
    return Fraction(2**exponent) if exponent >= 0 else Fraction(1, 2**-exponent)


def decode(bits, width):
    fraction, bias, infinity, _ = shape(width)
    negative = bool(bits >> (width-1))
    exponent = (bits & infinity) >> fraction
    tail = bits & ((1 << fraction)-1)
    if bits & infinity == infinity:
        return 'nan' if tail else '-inf' if negative else 'inf'
    value = (tail + ((1 << fraction) if exponent else 0)) * power((exponent if exponent else 1)-bias-fraction)
    return -value if negative else value


def encode(value, width, negative_zero=False):
    """Locate neighboring encodings by rational binary search, then choose the nearer."""
    _, _, infinity, nan = shape(width)
    if value == 'nan':
        return nan
    if value in ('inf', '-inf'):
        return infinity | ((1 << (width-1)) if value == '-inf' else 0)
    sign = (1 << (width-1)) if value < 0 or (not value and negative_zero) else 0
    value = abs(value)
    maximum = decode(infinity-1, width)
    if value > maximum:
        half_step = (maximum-decode(infinity-2, width))/2
        return sign | (infinity if value >= maximum+half_step else infinity-1)
    low, high = 0, infinity-1
    while low < high:
        middle = (low+high+1)//2
        if decode(middle, width) <= value:
            low = middle
        else:
            high = middle-1
    if decode(low, width) == value:
        return sign | low
    a, b = value-decode(low, width), decode(low+1, width)-value
    return sign | (low if a < b or (a == b and not low & 1) else low+1)


def word(value):
    return f'UINT64_C(0x{value & ((1<<64)-1):016x})'


def generate():
    rows = ['/* Exact Fraction oracle: nearest encodings found by binary search, not runtime packing. */']
    f2f, i2f, f2i = set(), set(), set()
    for width in (32, 64):
        fraction, _, infinity, nan = shape(width)
        magnitudes = [0, 1, 2, (1 << fraction)-1, 1 << fraction, (1 << fraction)+1,
                      encode(Fraction(1), width), infinity-1, infinity, nan, nan+1, infinity+1]
        inputs = {x | sign for x in magnitudes for sign in (0, 1 << (width-1))}
        if width == 64:
            for lower in [0, 1, 2, 0x007ffffe, 0x007fffff, 0x00800000, 0x3f7fffff,
                          0x3f800000, 0x3f800001, 0x4b7fffff, 0x4b800000, 0x7f7ffffe]:
                middle = encode((decode(lower, 32)+decode(lower+1, 32))/2, 64)
                inputs.update((middle+delta) | sign for delta in (-1, 0, 1) for sign in (0, 1 << 63))
            threshold = encode(decode(0x7f7fffff, 32)+(decode(0x7f7fffff, 32)-decode(0x7f7ffffe, 32))/2, 64)
            inputs.update((threshold+delta) | sign for delta in (-1, 0, 1) for sign in (0, 1 << 63))
        for value in inputs:
            for target in (32, 64):
                f2f.add((width, target, value, encode(decode(value, width), target, bool(value >> (width-1)))))
    for width in (8, 16, 32, 64):
        for signed in (False, True):
            low = -(1 << (width-1)) if signed else 0
            high = (1 << (width-(1 if signed else 0)))-1
            integers = {low, low+1, high-1, high, -1, 0, 1}
            for exponent in (24, 25, 53, 54, 63, 64):
                integers.update(sign*((1 << exponent)+delta) for sign in (-1, 1) for delta in (-3, -1, 0, 1, 3))
            for value in sorted(integers):
                if low <= value <= high:
                    for target in (32, 64):
                        i2f.add((width, signed, target, value, encode(Fraction(value), target)))
            values = {Fraction(-1), Fraction(0), Fraction(1), Fraction(-1, 2), Fraction(1, 2)}
            for boundary in (low, high, high+1):
                values.update(Fraction(boundary)+delta for delta in (Fraction(-1), Fraction(-1,2), Fraction(0), Fraction(1,2), Fraction(1)))
            for source in (32, 64):
                _, _, infinity, nan = shape(source)
                inputs = {encode(value, source) for value in values} | {infinity, infinity | (1 << (source-1)), nan, infinity+1}
                for bits in sorted(inputs):
                    value = decode(bits, source)
                    truncated = int(value) if isinstance(value, Fraction) else None
                    valid = truncated is not None and low <= truncated <= high
                    f2i.add((source, width, signed, bits, 'XR_XIR_NUMERIC_OK' if valid else 'XR_XIR_NUMERIC_RANGE', truncated if valid else 0))
    for source, target, bits, result in sorted(f2f):
        rows.append(f'XIR_F2F({source}, {target}, {word(bits)}, {word(result)})')
    for width, signed, target, value, result in sorted(i2f):
        rows.append(f'XIR_I2F({width}, {str(signed).lower()}, {target}, {word(value)}, {word(result)})')
    for source, width, signed, bits, status, result in sorted(f2i):
        rows.append(f'XIR_F2I({source}, {width}, {str(signed).lower()}, {word(bits)}, {status}, {word(result)})')
    return '\n'.join(rows)+'\n', {'float_to_float': len(f2f), 'integer_to_float': len(i2f), 'float_to_integer': len(f2i)}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    output = Path(__file__).resolve().parents[1]/'tests/unit/xir/xir_float_vectors.def'
    text, counts = generate()
    if args.check:
        if not output.exists() or output.read_text(encoding='utf-8') != text:
            raise SystemExit('Floating vector file differs from the exact rational oracle')
    else:
        output.write_text(text, encoding='utf-8')
    print(counts)
