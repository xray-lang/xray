"""Generate exact decimal inputs using rational binary64 midpoint arithmetic."""
from fractions import Fraction
from pathlib import Path
import argparse
import json


def binary_value(bits):
    exponent, significand = bits >> 52, bits & ((1 << 52) - 1)
    return Fraction(significand if not exponent else (1 << 52) + significand) * Fraction(2) ** (
        -1074 if not exponent else exponent - 1075
    )


def decimal_pair(value):
    power = value.denominator.bit_length() - 1
    assert value.denominator == 1 << power
    coefficient = value.numerator * 5**power
    while coefficient and coefficient % 10 == 0:
        coefficient //= 10
        power -= 1
    return coefficient, -power


def rounded(value):
    if value >= Fraction(2) ** 1024 - Fraction(2) ** 970:
        return 0x7FF0000000000000
    low, high = 0, 0x7FEFFFFFFFFFFFFF
    while low < high:
        middle = (low + high + 1) // 2
        if binary_value(middle) <= value:
            low = middle
        else:
            high = middle - 1
    if low < 0x7FEFFFFFFFFFFFFF:
        left, right = value - binary_value(low), binary_value(low + 1) - value
        if right < left or (right == left and low & 1):
            low += 1
    return low


def vectors():
    rows = []

    def append(text, expected):
        rows.append((text, expected))

    def rational(coefficient, exponent):
        text = str(coefficient) + "e" + str(exponent)
        expected = rounded(Fraction(coefficient) * Fraction(10) ** exponent)
        append(text, expected)

    for exponent in range(1, 2047):
        upper = exponent << 52
        coefficient, scale = decimal_pair((binary_value(upper - 1) + binary_value(upper)) / 2)
        for delta in (-1, 0, 1):
            rational(coefficient + delta, scale)
    for lower in range(512):
        coefficient, scale = decimal_pair((binary_value(lower) + binary_value(lower + 1)) / 2)
        for delta in (-1, 0, 1):
            rational(coefficient + delta, scale)
    state = 0x29C8E733
    for _ in range(384):
        state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
        coefficient = (state << 32) | (state ^ 0xA536712B)
        scale = state % 652 - 343
        rational(coefficient, scale)
    for lower in (0, 1, 2, (1 << 52) - 1, 0x3FF0000000000000, 0x7FEFFFFFFFFFFFFE):
        coefficient, scale = decimal_pair((binary_value(lower) + binary_value(lower + 1)) / 2)
        for zeros in (799, 800, 801, 1200):
            rational(coefficient * 10 ** (zeros + 1) + 1, scale - zeros - 1)
    for count in (799, 800, 801, 3000):
        append("1" + "0" * count + "e-" + str(count), 0x3FF0000000000000)
        append("0." + "0" * count + "1e" + str(count + 1), 0x3FF0000000000000)
    for scale in ("400", "4294967296", "18446744073709551615", "18446744073709551616", "18446744073709551617", "9" * 128):
        append("1e" + scale, 0x7FF0000000000000)
        append("1e-" + scale, 0)
        append("0e" + scale, 0)
        append("0e-" + scale, 0)
    for text, expected in list(rows[:32]) + list(rows[-32:]):
        append("-" + text, expected | (1 << 63))
    for text in ("0", "0.1", "1e23", "2.2250738585072014e-308", "4.9406564584124654e-324", "2.4703282292062327e-324", "1.7976931348623157e308", "1.7976931348623158e308", "1.00000000000000011102230246251565404236316680908203125", "1.00000000000000011102230246251565404236316680908203126", "9007199254740993", "99999999999999999999999999999999999999999999999999999999999999999999e-65"):
        append(text, rounded(Fraction(text)))
    append("-0", 1 << 63)
    append(" \n+1.25\t\r", 0x3FF4000000000000)
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    options = parser.parse_args()
    rows = vectors()
    text = "/* Generated exact rational binary64 boundary vectors; regenerate with the adjacent Python generator. */\n"
    text += "".join(f"DECIMAL_CASE({json.dumps(s)},UINT64_C(0x{b:016x}))\n" for s, b in rows)
    path = Path(__file__).with_name("decimal_rounding_vectors.def")
    if options.check:
        assert path.read_text(encoding="utf-8") == text, "decimal rounding vectors are stale"
    else:
        path.write_text(text, encoding="utf-8")
    print(f"Exact rational rounding vectors: {len(rows)}, all 2046 normal exponent transitions, 512 subnormal midpoints, long sticky tails and exponent cancellation")


if __name__ == "__main__":
    main()
