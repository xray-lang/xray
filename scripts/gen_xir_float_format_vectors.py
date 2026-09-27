"""Search independent decimal candidates for shortest original-width formatting."""
from pathlib import Path
from fractions import Fraction
import argparse
import random
from gen_xir_float_vectors import decode, encode, shape, word


def power10(exponent):
    return Fraction(10**exponent) if exponent >= 0 else Fraction(1, 10**-exponent)


def shortest(bits, width):
    value = decode(bits, width)
    if isinstance(value, str):
        return value
    negative = bool(bits >> (width - 1))
    prefix = '-' if negative else ''
    if not value:
        return prefix + '0.0'
    value = abs(value)
    positive_bits = bits & ((1 << (width - 1)) - 1)
    exponent = 0
    while value < power10(exponent):
        exponent -= 1
    while value >= power10(exponent + 1):
        exponent += 1
    for digits in range(1, (9 if width == 32 else 17) + 1):
        unit = power10(exponent - digits + 1)
        scaled = value / unit
        floor = scaled.numerator // scaled.denominator
        candidates = [q for q in (floor, floor + 1) if encode(q * unit, width) == positive_bits]
        if not candidates:
            continue
        coefficient = min(candidates, key=lambda q: (abs(q * unit - value), q % 2))
        scale = exponent - digits + 1
        while coefficient % 10 == 0:
            coefficient //= 10
            scale += 1
        text = str(coefficient)
        k = len(text) - 1 + scale
        if -4 <= k < 16:
            point = k + 1
            if point <= 0:
                result = '0.' + '0' * -point + text
            elif point >= len(text):
                result = text + '0' * (point - len(text)) + '.0'
            else:
                result = text[:point] + '.' + text[point:]
        else:
            result = text[0] + ('.' + text[1:] if len(text) > 1 else '') + f'e{k:+d}'
        return prefix + result
    raise AssertionError((bits, width))


def generate():
    rows = ['/* Shortest candidates independently searched with exact Fraction distances. */']
    rng = random.Random(317319)
    for width in (32, 64):
        fraction, bias, infinity, nan = shape(width)
        one = bias << fraction
        values = {0, 1, 2, (1 << fraction)-1, 1 << fraction, (1 << fraction)+1,
                  one-1, one, one+1, infinity-1, infinity, nan, infinity+1}
        for exponent in (-324, -308, -46, -45, -5, -4, -1, 0, 1, 15, 16, 23, 38, 308):
            middle = encode(power10(exponent), width)
            values.update(v for v in (middle-1, middle, middle+1) if 0 <= v <= infinity)
        values = {v | sign for v in values for sign in (0, 1 << (width-1))}
        values.update(rng.getrandbits(width) for _ in range(192))
        for bits in sorted(values):
            rows.append(f'XIR_FLOAT_FORMAT({width}, {word(bits)}, "{shortest(bits, width)}")')
    return '\n'.join(rows) + '\n', len(rows)-1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    path = Path(__file__).resolve().parents[1] / 'tests/unit/xir/xir_float_format_vectors.def'
    text, count = generate()
    if args.check:
        if not path.exists() or path.read_text(encoding='utf-8') != text:
            raise SystemExit('Floating format vectors differ from the independent candidate search')
    else:
        path.write_text(text, encoding='utf-8')
    print(f'{count} floating format vectors')
