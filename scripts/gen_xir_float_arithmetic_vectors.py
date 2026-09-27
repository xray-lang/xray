"""Generate independent exact-rational witnesses for binary floating arithmetic."""
from pathlib import Path
from fractions import Fraction
import argparse
import random
import gen_xir_float_vectors as oracle

def calculate(width, operation, left, right):
    sign_mask = 1 << (width - 1)
    if operation == 'sub':
        right ^= sign_mask
        operation = 'add'
    a, b = oracle.decode(left, width), oracle.decode(right, width)
    negative_a, negative_b = bool(left & sign_mask), bool(right & sign_mask)
    negative = negative_a != negative_b
    if a == 'nan' or b == 'nan':
        result = 'nan'
    elif operation == 'add':
        if isinstance(a, str) and isinstance(b, str):
            result = a if a == b else 'nan'
        else:
            result = a if isinstance(a, str) else b if isinstance(b, str) else a + b
        negative = negative_a and negative_b
    elif operation == 'mul':
        if isinstance(a, str) or isinstance(b, str):
            result = 'nan' if a == 0 or b == 0 else '-inf' if negative else 'inf'
        else:
            result = a * b
    else:
        assert operation == 'div'
        if isinstance(a, str) and isinstance(b, str):
            result = 'nan'
        elif isinstance(a, str):
            result = '-inf' if negative else 'inf'
        elif isinstance(b, str):
            result = Fraction(0)
        elif b == 0:
            result = 'nan' if a == 0 else '-inf' if negative else 'inf'
        else:
            result = a / b
    return oracle.encode(result, width, negative)


def generate():
    rows = ['/* Exact rational results, rounded by independent encoding binary search. */']
    names = {'add': 'ADD', 'sub': 'SUBTRACT', 'mul': 'MULTIPLY', 'div': 'DIVIDE'}
    random_bits = random.Random(317)
    for width in (32, 64):
        fraction, bias, infinity, nan = oracle.shape(width)
        one = bias << fraction
        magnitudes = (0, 1, 2, (1 << fraction)-1, 1 << fraction, (1 << fraction)+1,
                      one-1, one, one+1, infinity-1, infinity, nan, infinity+1)
        values = [m | sign for m in magnitudes for sign in (0, 1 << (width-1))]
        for operation, name in names.items():
            pairs = [(a, b) for a in values for b in values]
            pairs += [(random_bits.getrandbits(width), random_bits.getrandbits(width)) for _ in range(128)]
            for a, b in pairs:
                expected = calculate(width, operation, a, b)
                rows.append(f'XIR_FLOAT_ARITHMETIC({width}, XR_XIR_FLOAT_{name}, {oracle.word(a)}, {oracle.word(b)}, {oracle.word(expected)})')
    return '\n'.join(rows)+'\n', len(rows)-1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    output = Path(__file__).resolve().parents[1] / 'tests/unit/xir/xir_float_arithmetic_vectors.def'
    text, count = generate()
    if args.check:
        if not output.exists() or output.read_text(encoding='utf-8') != text:
            raise SystemExit('Floating arithmetic vectors differ from the exact rational oracle')
    else:
        output.write_text(text, encoding='utf-8')
    print(f'{count} floating arithmetic vectors')
