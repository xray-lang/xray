"""Generate decimal conversion witnesses using an independent rational oracle."""
from pathlib import Path
from fractions import Fraction
import argparse
import random
from gen_xir_float_vectors import decode, encode, shape, word


def decimal(value):
    negative = value < 0
    value = abs(value)
    power = value.denominator.bit_length() - 1
    assert value.denominator == 1 << power
    digits = str(value.numerator * 5**power).rjust(power + 1, '0')
    return ('-' if negative else '') + (digits[:-power] + '.' + digits[-power:] if power else digits + '.0')


def generate():
    cases = {'0.0', '-0.0', '+.5', '1.', '1_234.5_678e-0_2', '1e999999999999999999', '-1e-999999999999999999'}
    for width in (32, 64):
        fraction, _, infinity, _ = shape(width)
        for lower in (0, 1, 2, (1 << fraction)-1, 1 << fraction, encode(Fraction(1), width), infinity-2):
            middle = (decode(lower, width) + decode(lower+1, width))/2
            for delta in (-1, 0, 1):
                # Fraction denominators containing five are rendered directly at a fixed decimal scale.
                scaled = middle * 10**1600 + delta
                assert scaled.denominator == 1
                digits = str(scaled.numerator).rjust(1601, '0')
                text = digits[:-1600] + '.' + digits[-1600:]
                cases.update((text, '-' + text))
        maximum = decode(infinity-1, width)
        cases.add(decimal(maximum + (maximum-decode(infinity-2, width))/2))
    rng = random.Random(312320)
    for length in (1, 17, 63, 768, 1152, 1153, 1500):
        for _ in range(12):
            digits = str(rng.randrange(1, 10)) + ''.join(str(rng.randrange(10)) for _ in range(length-1))
            cases.add(digits[0] + '.' + (digits[1:] or '0') + 'e' + str(rng.randrange(-400, 401)))
    rows = ['/* Decimal spellings and nearest-even encodings from exact Fraction binary search. */']
    for text in sorted(cases):
        if text in ('1e999999999999999999', '-1e-999999999999999999'):
            value = Fraction(0) if 'e-' in text else 'inf'
        else:
            value = Fraction(text.replace('_', ''))
        for width in (32, 64):
            expected = encode(value, width, text.startswith('-') and not value)
            rows.append(f'XIR_DECIMAL("{text}", {width}, {word(expected)})')
    return '\n'.join(rows) + '\n'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    output = Path(__file__).resolve().parents[1]/'tests/unit/xir/xir_decimal_vectors.def'
    content = generate()
    if args.check:
        if output.read_text(encoding='utf-8') != content:
            raise SystemExit('Decimal witnesses differ from exact rational oracle')
    else:
        output.write_text(content, encoding='utf-8')
    print(f'{content.count("XIR_DECIMAL(")} decimal witnesses')
