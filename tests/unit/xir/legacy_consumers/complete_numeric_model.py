"""Independent arithmetic and formatting models for nine complete original programs."""
import math

BITS = 64
MIN = -(1 << 63)
MAX = (1 << 63) - 1
FIXED_DECIMALS_MAX = 20

def wrap(x, width=64, signed=True):
    value = x % (1 << width)
    return value - (1 << width) if signed and value >= (1 << (width - 1)) else value

def rotl(x, count, width=64, signed=True):
    value = x % (1 << width)
    count %= width
    bits = ((value << count) | (value >> ((width - count) % width))) % (1 << width)
    return wrap(bits, width, signed)

def rotr(x, count, width=64, signed=True):
    return rotl(x, -count, width, signed)

def swap(x, width=64, signed=True):
    b = (x % (1 << width)).to_bytes(width // 8, 'little')
    return wrap(int.from_bytes(b, 'big'), width, signed)

def pop(x, width=64):
    return (x % (1 << width)).bit_count()

def clz(x, width=64):
    return width - (x % (1 << width)).bit_length()

def ctz(x, width=64):
    value = x % (1 << width)
    return width if not value else (value & -value).bit_length() - 1

def mul_high(x, y, width):
    return ((x % (1 << width)) * (y % (1 << width))) // (1 << width)

def checked(x):
    return x if MIN <= x <= MAX else None

def saturated(x):
    return min(MAX, max(MIN, x))

def overflow(x):
    return not MIN <= x <= MAX

def divrem(a, b):
    quotient = (abs(a) // abs(b)) * (-1 if (a < 0) != (b < 0) else 1)
    return quotient, a - quotient * b

def scalar(x):
    if x is None:
        return 'null'
    if type(x) is bool:
        return str(x).lower()
    if type(x) is list:
        return '[' + ', '.join(scalar(v) for v in x) + ']'
    return str(x)

def observations():
    out = {}
    width_results = [wrap((1 << 31), 32), wrap(256, 8, False), wrap(-129, 8),
                     wrap(((1 << 32) - 1) * 2, 32, False), wrap(1 << 64, 64, False),
                     wrap(MAX + 1), wrap(1 << 15, 16), wrap(1 << 16, 16, False)]
    intermediate = wrap(2147483640 + 10, 32)
    out['int/wrapping_fixed_width'] = width_results + [intermediate,
        intermediate == wrap(2147483640 + 10, 32), checked(MAX + 1), saturated(MAX + 1), overflow(MAX + 1)]
    b = rotl(129, 1, 8, False)
    out['int_wrap/exact_integer_bit_intrinsics'] = [b, swap(b, 8, False), rotl(-128, -1, 8),
        rotr(1, 1, 8, False), rotl(1, 9, 8, False), swap(4660, 16), swap(33023, 16, False),
        clz(0, 16), pop(-1, 16), rotr(rotl(16909060, -37, 32), -37, 32),
        swap(16909060, 32, False), ctz(0, 32), rotr(1, 1), swap(72623859790382856, 64, False),
        pop(-1, 64), mul_high(255, 2, 8), mul_high(65535, 2, 16),
        mul_high((1 << 32) - 1, 2, 32), mul_high((1 << 64) - 1, 2, 64),
        rotr(rotl(1, -1, BITS), -1, BITS), swap(swap(1, BITS, False), BITS, False),
        rotr(rotl(12345, -65), -65), clz(0)]
    integers = [MIN, MAX, -42]
    mixed = ['n', MIN, None, True, MAX]
    out['int_wrap/generic_int_format_shared_core'] = [MIN, MAX, 'left=' + str(MIN), str(MAX) + '=right',
        '|'.join(scalar(x) for x in integers), scalar(integers), ':'.join(scalar(x) for x in mixed)]
    values = [0, 255, 1023, -1]
    out['int_wrap/int_bits_methods'] = [*(pop(x) for x in values),
        *(clz(x) for x in [0, 1, 256, -1]), *(ctz(x) for x in [0, 8, 1024, -1]),
        *(swap(x) for x in [1, 255, -1]), *(rotl(1, n) for n in [4, 0, 64, 68]),
        rotr(16, 4), rotr(1, 1), rotr(1, 64), pop(12345), rotl(12345, 8),
        rotr(rotl(12345, 8), 8), swap(swap(12345))]
    out['int_wrap/int_overflow_predicates'] = [*(overflow(x) for x in [1 + 2, MAX + 1, MAX + MAX,
        5 - 3, MAX - -1, 0 - MAX, 3 * 4, MAX * 2, (1 << 62) * 4, 0 * MAX]),
        overflow(MAX + 1), overflow(MAX + 1) == (checked(MAX + 1) is None),
        overflow(1 + 1) == (checked(1 + 1) is None), overflow(MAX * 2) == (checked(MAX * 2) is None)]
    out['int_wrap/int_safe_methods'] = [checked(MAX), checked(MAX + 1) is None, checked(MIN - 1) is None,
        checked(40 + 2), checked(MIN), checked(MIN - 1) is None, checked(MAX - -1) is None, checked(10 - 3),
        checked(MAX), checked(MAX * 2) is None, checked(MIN * -1) is None, checked(7 * 6),
        *(saturated(x) for x in [MAX + 1, MIN - 1, MAX - -1, MIN - 1, MAX * 2, MIN * 2, MIN * -1]),
        *(wrap(x) for x in [MAX + 1, MIN - 1, MIN - 1, MAX + 1, MAX * 2, MIN * -1]),
        wrap(MAX + 1), wrap(MIN - 1), wrap(MAX * 2), wrap(abs(MIN)), str(MIN), '-0x' + format(abs(MIN), 'x')]
    fixed = lambda digits: format(1.25, '.' + str(min(FIXED_DECIMALS_MAX, max(0, digits))) + 'f')
    out['int_wrap/numeric_method_shared_core'] = [str(MIN), wrap(abs(MIN)), str(wrap(abs(MIN))),
        '-0x' + format(abs(MIN), 'x'), '-0x' + format(15, 'x'), '0x' + format(255, 'x'),
        float(math.isqrt(9)), 'f64', math.floor(3.7), 'i64', math.ceil(3.7), 'i64', math.floor(3.7 + .5), 'i64',
        fixed(2), fixed(-3), fixed(99)]
    out['int_wrap/stringbuilder_numeric_shared_core'] = ['min=' + str(MIN) + '; abs=' + str(wrap(abs(MIN))) + '; next=' + str(-42)]
    x = (0x80000000 << 32) % (1 << 64)
    all_bits = (0xffffffff << 32) | 0xffffffff
    out['int_wrap/uint64_divmod_unsigned'] = [x // 85, x % 85, x // 2, x % 2,
        all_bits // 85, all_bits % 85, all_bits // 3, all_bits % 7, all_bits // 85, all_bits % 85,
        x // 7, x % 7, all_bits // 1000, all_bits % 1000, x // 85, x % 85,
        *divrem(-85, 2), *divrem(-85, 2), *divrem(-8, 3), 0x80000000 // 7, 0x80000000 % 7, 200 // 7, 200 % 7]
    return out

def expected():
    return [{'id':name.split('/')[-1], 'source':'tests/diff/cases/semantics/' + name + '.xr',
             'observations':values, 'stdout_utf8':''.join(scalar(x) + '\n' for x in values),
             'expected_exit_code':0, 'stderr_utf8':''} for name, values in observations().items()]
