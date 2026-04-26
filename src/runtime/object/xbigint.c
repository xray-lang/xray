/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbigint.c - Arbitrary precision integer implementation
 *
 * KEY CONCEPT:
 *   Core operations using base 2^32 storage.
 *   Includes: creation, arithmetic, comparison, conversion.
 */

#include "xbigint.h"
#include "../../base/xchecks.h"
#include "../gc/xgc.h"
#include "../gc/xalloc_unified.h"
#include "../xisolate_api.h"
#include "../class/xclass_system.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "../../base/xmalloc.h"

/* ========== Internal Constants ========== */

#define LIMB_BITS 32
#define LIMB_BASE 0x100000000ULL
#define LIMB_MAX 0xFFFFFFFFULL
#define LIMB_HIGHBIT 0x80000000U
#define KARATSUBA_THRESHOLD 32  // use Karatsuba when both operands >= 32 limbs

// For toString optimization: 10^9 fits in 32 bits
#define RADIX_BASE 1000000000U  // 10^9
#define RADIX_DIGITS 9          // digits per RADIX_BASE

/* ========== Internal Helper Functions ========== */

// Allocate BigInt on coroutine heap (runtime path)
static XrBigInt *bigint_alloc(struct XrCoroutine *coro, uint32_t cap) {
    if (cap == 0)
        cap = 1;
    size_t size = sizeof(XrBigInt) + cap * sizeof(uint32_t);
    XrBigInt *b = (XrBigInt *) xr_alloc(coro, size, XR_TBIGINT);
    if (b) {
        XrayCoreClasses *core = xr_isolate_get_core_classes(xr_coro_get_isolate(coro));
        (void) core;
        xr_gc_header_init_type(&b->gc, XR_TBIGINT);
        b->sign = 1;
        b->len = 0;
        b->cap = cap;
        memset(b->limbs, 0, cap * sizeof(uint32_t));
    }
    return b;
}

// Allocate BigInt on global GC (compile-time path, no coroutine needed)
static XrBigInt *bigint_alloc_on_gc(struct XrGC *gc, uint32_t cap) {
    if (cap == 0)
        cap = 1;
    size_t size = sizeof(XrBigInt) + cap * sizeof(uint32_t);
    XrBigInt *b = (XrBigInt *) xr_gc_alloc(gc, size, XR_TBIGINT);
    if (b) {
        xr_gc_header_init_type(&b->gc, XR_TBIGINT);
        b->sign = 1;
        b->len = 0;
        b->cap = cap;
        memset(b->limbs, 0, cap * sizeof(uint32_t));
    }
    return b;
}

// Normalize BigInt (remove leading zeros, fix zero's sign)
static void bigint_normalize(XrBigInt *a) {
    while (a->len > 1 && a->limbs[a->len - 1] == 0) {
        a->len--;
    }
    // Zero is always positive
    if (a->len == 1 && a->limbs[0] == 0) {
        a->sign = 1;
    }
}

// Ensure sufficient capacity, reallocate if needed
static XrBigInt *bigint_ensure_cap(struct XrCoroutine *coro, XrBigInt *a, uint32_t cap) {
    if (a->cap >= cap)
        return a;

    XrBigInt *b = bigint_alloc(coro, cap);
    if (b) {
        b->sign = a->sign;
        b->len = a->len;
        memcpy(b->limbs, a->limbs, a->len * sizeof(uint32_t));
    }
    return b;
}

static XrBigInt *bigint_ensure_cap_on_gc(struct XrGC *gc, XrBigInt *a, uint32_t cap) {
    if (a->cap >= cap)
        return a;
    XrBigInt *b = bigint_alloc_on_gc(gc, cap);
    if (b) {
        b->sign = a->sign;
        b->len = a->len;
        memcpy(b->limbs, a->limbs, a->len * sizeof(uint32_t));
    }
    return b;
}

/* ========== Creation Functions ========== */

XrBigInt *xr_bigint_new(struct XrCoroutine *coro, int64_t value) {
    XR_DCHECK(coro != NULL, "bigint_new: NULL coro");
    XrBigInt *b = bigint_alloc(coro, 2);  // 64-bit needs at most 2 limbs
    if (!b)
        return NULL;

    if (value < 0) {
        b->sign = -1;
        // Handle INT64_MIN special case
        if (value == INT64_MIN) {
            b->limbs[0] = 0;
            b->limbs[1] = 0x80000000U;
            b->len = 2;
            return b;
        }
        value = -value;
    }

    if (value == 0) {
        b->limbs[0] = 0;
        b->len = 1;
    } else {
        b->limbs[0] = (uint32_t) (value & 0xFFFFFFFFULL);
        uint32_t high = (uint32_t) (value >> 32);
        if (high == 0) {
            b->len = 1;
        } else {
            b->limbs[1] = high;
            b->len = 2;
        }
    }
    return b;
}

// Parse single hex digit
static int hex_digit_value(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

XrBigInt *xr_bigint_from_string(struct XrCoroutine *coro, const char *str) {
    XR_DCHECK(coro != NULL, "bigint_from_string: NULL coro");
    if (!str || !*str)
        return NULL;

    // Parse sign
    int sign = 1;
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }

    // Detect base prefix
    int base = 10;
    if (str[0] == '0' && str[1] != '\0') {
        if (str[1] == 'x' || str[1] == 'X') {
            base = 16;
            str += 2;
        } else if (str[1] == 'b' || str[1] == 'B') {
            base = 2;
            str += 2;
        } else if (str[1] == 'o' || str[1] == 'O') {
            base = 8;
            str += 2;
        }
    }

    // Skip leading zeros
    while (*str == '0' && *(str + 1) != '\0') {
        str++;
    }

    // Calculate string length
    size_t slen = strlen(str);
    if (slen == 0)
        return xr_bigint_new(coro, 0);

    // Estimate limb count needed
    uint32_t cap;
    if (base == 16) {
        cap = (uint32_t) (slen / 8) + 2;
    } else if (base == 2) {
        cap = (uint32_t) (slen / 32) + 2;
    } else {
        cap = (uint32_t) (slen / 9) + 2;
    }

    XrBigInt *result = bigint_alloc(coro, cap);
    if (!result)
        return NULL;

    result->limbs[0] = 0;
    result->len = 1;
    result->sign = 1;

    // Parse and accumulate character by character
    for (size_t i = 0; i < slen; i++) {
        char c = str[i];
        int digit;

        if (base == 16) {
            digit = hex_digit_value(c);
        } else if (base == 2) {
            if (c == '0' || c == '1') {
                digit = c - '0';
            } else {
                return NULL;  // Invalid character
            }
        } else if (base == 8) {
            if (c >= '0' && c <= '7') {
                digit = c - '0';
            } else {
                return NULL;  // Invalid character
            }
        } else {
            if (isdigit(c)) {
                digit = c - '0';
            } else {
                return NULL;  // Invalid character
            }
        }

        if (digit < 0)
            return NULL;

        // result = result * base + digit
        uint64_t carry = digit;
        for (uint32_t j = 0; j < result->len; j++) {
            uint64_t prod = (uint64_t) result->limbs[j] * base + carry;
            result->limbs[j] = (uint32_t) (prod & 0xFFFFFFFFULL);
            carry = prod >> 32;
        }
        if (carry > 0) {
            if (result->len >= result->cap) {
                result = bigint_ensure_cap(coro, result, result->cap * 2);
                if (!result)
                    return NULL;
            }
            result->limbs[result->len++] = (uint32_t) carry;
        }
    }

    result->sign = sign;
    bigint_normalize(result);
    return result;
}

// Compile-time BigInt creation (global GC, no coroutine needed)
XrBigInt *xr_bigint_new_on_gc(struct XrGC *gc, int64_t value) {
    XrBigInt *b = bigint_alloc_on_gc(gc, 2);
    if (!b)
        return NULL;
    if (value < 0) {
        b->sign = -1;
        if (value == INT64_MIN) {
            b->limbs[0] = 0;
            b->limbs[1] = 0x80000000U;
            b->len = 2;
            return b;
        }
        value = -value;
    }
    if (value == 0) {
        b->limbs[0] = 0;
        b->len = 1;
    } else {
        b->limbs[0] = (uint32_t) (value & 0xFFFFFFFFULL);
        uint32_t high = (uint32_t) (value >> 32);
        if (high == 0) {
            b->len = 1;
        } else {
            b->limbs[1] = high;
            b->len = 2;
        }
    }
    return b;
}

XrBigInt *xr_bigint_from_string_on_gc(struct XrGC *gc, const char *str) {
    if (!str || !*str)
        return NULL;
    int sign = 1;
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }
    int base = 10;
    if (str[0] == '0' && str[1] != '\0') {
        if (str[1] == 'x' || str[1] == 'X') {
            base = 16;
            str += 2;
        } else if (str[1] == 'b' || str[1] == 'B') {
            base = 2;
            str += 2;
        } else if (str[1] == 'o' || str[1] == 'O') {
            base = 8;
            str += 2;
        }
    }
    while (*str == '0' && *(str + 1) != '\0')
        str++;
    size_t slen = strlen(str);
    if (slen == 0)
        return xr_bigint_new_on_gc(gc, 0);
    uint32_t cap = (base == 16)  ? (uint32_t) (slen / 8) + 2
                   : (base == 2) ? (uint32_t) (slen / 32) + 2
                                 : (uint32_t) (slen / 9) + 2;
    XrBigInt *result = bigint_alloc_on_gc(gc, cap);
    if (!result)
        return NULL;
    result->limbs[0] = 0;
    result->len = 1;
    result->sign = 1;
    for (size_t i = 0; i < slen; i++) {
        char c = str[i];
        int digit;
        if (base == 16) {
            digit = hex_digit_value(c);
        } else if (base == 2) {
            digit = (c == '0' || c == '1') ? c - '0' : -1;
        } else if (base == 8) {
            digit = (c >= '0' && c <= '7') ? c - '0' : -1;
        } else {
            digit = isdigit(c) ? c - '0' : -1;
        }
        if (digit < 0)
            return NULL;
        uint64_t carry = digit;
        for (uint32_t j = 0; j < result->len; j++) {
            uint64_t prod = (uint64_t) result->limbs[j] * base + carry;
            result->limbs[j] = (uint32_t) (prod & 0xFFFFFFFFULL);
            carry = prod >> 32;
        }
        if (carry > 0) {
            if (result->len >= result->cap) {
                result = bigint_ensure_cap_on_gc(gc, result, result->cap * 2);
                if (!result)
                    return NULL;
            }
            result->limbs[result->len++] = (uint32_t) carry;
        }
    }
    result->sign = sign;
    bigint_normalize(result);
    return result;
}

XrBigInt *xr_bigint_copy(struct XrCoroutine *coro, XrBigInt *a) {
    XR_DCHECK(coro != NULL, "bigint_copy: NULL coro");
    XR_DCHECK(a != NULL, "bigint_copy: NULL a");
    XrBigInt *b = bigint_alloc(coro, a->len);
    if (!b)
        return NULL;

    b->sign = a->sign;
    b->len = a->len;
    memcpy(b->limbs, a->limbs, a->len * sizeof(uint32_t));
    return b;
}

/* ========== Comparison Operations ========== */

int xr_bigint_cmp_abs(XrBigInt *a, XrBigInt *b) {
    XR_DCHECK(a != NULL, "bigint_cmp_abs: NULL a");
    XR_DCHECK(b != NULL, "bigint_cmp_abs: NULL b");
    if (a->len > b->len)
        return 1;
    if (a->len < b->len)
        return -1;

    // Compare from high to low
    for (int i = (int) a->len - 1; i >= 0; i--) {
        if (a->limbs[i] > b->limbs[i])
            return 1;
        if (a->limbs[i] < b->limbs[i])
            return -1;
    }
    return 0;
}

int xr_bigint_cmp(XrBigInt *a, XrBigInt *b) {
    XR_DCHECK(a != NULL, "bigint_cmp: NULL a");
    XR_DCHECK(b != NULL, "bigint_cmp: NULL b");
    // Different signs
    if (a->sign != b->sign) {
        return a->sign > b->sign ? 1 : -1;
    }

    // Same sign, compare absolute values
    int cmp = xr_bigint_cmp_abs(a, b);
    return a->sign == 1 ? cmp : -cmp;
}

int xr_bigint_cmp_int(XrBigInt *a, int64_t n) {
    XR_DCHECK(a != NULL, "bigint_cmp_int: NULL a");
    // Fast path: different signs
    if (a->sign > 0 && n < 0)
        return 1;
    if (a->sign < 0 && n >= 0)
        return -1;

    // Get absolute value of n
    uint64_t abs_n;
    if (n == INT64_MIN) {
        abs_n = (uint64_t) INT64_MAX + 1;
    } else {
        abs_n = (uint64_t) (n < 0 ? -n : n);
    }

    // Extract limbs from abs_n
    uint32_t n_lo = (uint32_t) (abs_n & 0xFFFFFFFFULL);
    uint32_t n_hi = (uint32_t) (abs_n >> 32);
    uint32_t n_len = n_hi ? 2 : 1;

    // Compare absolute values
    int cmp;
    if (a->len > n_len) {
        cmp = 1;
    } else if (a->len < n_len) {
        cmp = -1;
    } else {
        // Same length, compare limb by limb from high to low
        cmp = 0;
        if (a->len == 2) {
            if (a->limbs[1] > n_hi) {
                cmp = 1;
            } else if (a->limbs[1] < n_hi) {
                cmp = -1;
            } else if (a->limbs[0] > n_lo) {
                cmp = 1;
            } else if (a->limbs[0] < n_lo) {
                cmp = -1;
            }
        } else {
            // a->len == 1
            if (a->limbs[0] > n_lo) {
                cmp = 1;
            } else if (a->limbs[0] < n_lo) {
                cmp = -1;
            }
        }
    }

    return a->sign == 1 ? cmp : -cmp;
}

/* ========== Addition Operations ========== */

// Absolute value addition (internal)
// Result sign set by caller
static XrBigInt *bigint_add_abs(struct XrCoroutine *coro, XrBigInt *a, XrBigInt *b) {
    // Ensure a is the longer one
    if (a->len < b->len) {
        XrBigInt *tmp = a;
        a = b;
        b = tmp;
    }

    uint32_t cap = a->len + 1;  // May carry
    XrBigInt *result = bigint_alloc(coro, cap);
    if (!result)
        return NULL;

    uint64_t carry = 0;
    uint32_t i;

    // Process common length
    for (i = 0; i < b->len; i++) {
        uint64_t sum = (uint64_t) a->limbs[i] + b->limbs[i] + carry;
        result->limbs[i] = (uint32_t) (sum & 0xFFFFFFFFULL);
        carry = sum >> 32;
    }

    // Process remaining of a
    for (; i < a->len; i++) {
        uint64_t sum = (uint64_t) a->limbs[i] + carry;
        result->limbs[i] = (uint32_t) (sum & 0xFFFFFFFFULL);
        carry = sum >> 32;
    }

    // Handle final carry
    if (carry) {
        result->limbs[i++] = (uint32_t) carry;
    }

    result->len = i;
    return result;
}

// Absolute value subtraction (internal)
// Assumes |a| >= |b|, result sign set by caller
static XrBigInt *bigint_sub_abs(struct XrCoroutine *coro, XrBigInt *a, XrBigInt *b) {
    XrBigInt *result = bigint_alloc(coro, a->len);
    if (!result)
        return NULL;

    int64_t borrow = 0;
    uint32_t i;

    for (i = 0; i < a->len; i++) {
        int64_t diff = (int64_t) a->limbs[i] - (i < b->len ? b->limbs[i] : 0) - borrow;
        if (diff < 0) {
            diff += LIMB_BASE;
            borrow = 1;
        } else {
            borrow = 0;
        }
        result->limbs[i] = (uint32_t) diff;
    }

    result->len = a->len;
    bigint_normalize(result);
    return result;
}

XrBigInt *xr_bigint_add(struct XrCoroutine *coro, XrBigInt *a, XrBigInt *b) {
    XR_DCHECK(coro != NULL, "bigint_add: NULL coro");
    XR_DCHECK(a != NULL, "bigint_add: NULL a");
    XR_DCHECK(b != NULL, "bigint_add: NULL b");
    // Same sign: add absolute values
    if (a->sign == b->sign) {
        XrBigInt *result = bigint_add_abs(coro, a, b);
        if (result)
            result->sign = a->sign;
        return result;
    }

    // Different signs: subtract absolute values
    int cmp = xr_bigint_cmp_abs(a, b);
    if (cmp == 0) {
        return xr_bigint_new(coro, 0);
    }

    XrBigInt *result;
    if (cmp > 0) {
        result = bigint_sub_abs(coro, a, b);
        if (result)
            result->sign = a->sign;
    } else {
        result = bigint_sub_abs(coro, b, a);
        if (result)
            result->sign = b->sign;
    }
    return result;
}

XrBigInt *xr_bigint_sub(struct XrCoroutine *coro, XrBigInt *a, XrBigInt *b) {
    XR_DCHECK(coro != NULL, "bigint_sub: NULL coro");
    XR_DCHECK(a != NULL, "bigint_sub: NULL a");
    XR_DCHECK(b != NULL, "bigint_sub: NULL b");
    // a - b = a + (-b)
    int8_t orig_sign = b->sign;
    b->sign = -b->sign;
    if (b->len == 1 && b->limbs[0] == 0)
        b->sign = 1;  // -0 = 0

    XrBigInt *result = xr_bigint_add(coro, a, b);

    b->sign = orig_sign;  // Restore original sign
    return result;
}

/* ========== Multiplication Operations ========== */

// Basic O(n*m) multiplication
static XrBigInt *bigint_mul_basecase(struct XrCoroutine *coro, XrBigInt *a, XrBigInt *b) {
    uint32_t cap = a->len + b->len;
    XrBigInt *result = bigint_alloc(coro, cap);
    if (!result)
        return NULL;

    for (uint32_t i = 0; i < a->len; i++) {
        uint64_t carry = 0;
        for (uint32_t j = 0; j < b->len; j++) {
            uint64_t prod = (uint64_t) a->limbs[i] * b->limbs[j] + result->limbs[i + j] + carry;
            result->limbs[i + j] = (uint32_t) (prod & 0xFFFFFFFFULL);
            carry = prod >> 32;
        }
        if (carry) {
            result->limbs[i + b->len] += (uint32_t) carry;
        }
    }

    result->len = cap;
    result->sign = 1;  // caller sets sign
    bigint_normalize(result);
    return result;
}

// Extract low B limbs as a new BigInt
static XrBigInt *bigint_low(struct XrCoroutine *coro, XrBigInt *a, uint32_t B) {
    uint32_t len = (a->len < B) ? a->len : B;
    XrBigInt *result = bigint_alloc(coro, len);
    if (!result)
        return NULL;
    memcpy(result->limbs, a->limbs, len * sizeof(uint32_t));
    result->len = len;
    result->sign = 1;
    bigint_normalize(result);
    return result;
}

// Extract high limbs (starting from B) as a new BigInt
static XrBigInt *bigint_high(struct XrCoroutine *coro, XrBigInt *a, uint32_t B) {
    if (a->len <= B) {
        return xr_bigint_new(coro, 0);
    }
    uint32_t len = a->len - B;
    XrBigInt *result = bigint_alloc(coro, len);
    if (!result)
        return NULL;
    memcpy(result->limbs, a->limbs + B, len * sizeof(uint32_t));
    result->len = len;
    result->sign = 1;
    bigint_normalize(result);
    return result;
}

// Left shift by B limbs (multiply by 2^(32*B))
static XrBigInt *bigint_lshift_limbs(struct XrCoroutine *coro, XrBigInt *a, uint32_t B) {
    if (xr_bigint_is_zero(a))
        return xr_bigint_new(coro, 0);
    if (B == 0)
        return xr_bigint_copy(coro, a);

    XrBigInt *result = bigint_alloc(coro, a->len + B);
    if (!result)
        return NULL;

    memset(result->limbs, 0, B * sizeof(uint32_t));
    memcpy(result->limbs + B, a->limbs, a->len * sizeof(uint32_t));
    result->len = a->len + B;
    result->sign = a->sign;
    return result;
}

// Karatsuba multiplication: O(n^1.585)
// a = a1*B^n + a0, b = b1*B^n + b0
// a*b = a1*b1*B^(2n) + ((a1+a0)*(b1+b0) - a1*b1 - a0*b0)*B^n + a0*b0
static XrBigInt *bigint_mul_karatsuba(struct XrCoroutine *coro, XrBigInt *a, XrBigInt *b) {
    uint32_t min_len = (a->len < b->len) ? a->len : b->len;

    // Base case: use standard multiplication for small numbers
    if (min_len < KARATSUBA_THRESHOLD) {
        return bigint_mul_basecase(coro, a, b);
    }

    // Split point
    uint32_t B = min_len / 2;

    // Split: a = a1*B^n + a0, b = b1*B^n + b0
    XrBigInt *a0 = bigint_low(coro, a, B);
    XrBigInt *a1 = bigint_high(coro, a, B);
    XrBigInt *b0 = bigint_low(coro, b, B);
    XrBigInt *b1 = bigint_high(coro, b, B);

    if (!a0 || !a1 || !b0 || !b1)
        return NULL;

    // z0 = a0 * b0
    XrBigInt *z0 = bigint_mul_karatsuba(coro, a0, b0);
    if (!z0)
        return NULL;

    // z2 = a1 * b1
    XrBigInt *z2 = bigint_mul_karatsuba(coro, a1, b1);
    if (!z2)
        return NULL;

    // z1 = (a0 + a1) * (b0 + b1) - z0 - z2
    XrBigInt *a0_plus_a1 = bigint_add_abs(coro, a0, a1);
    XrBigInt *b0_plus_b1 = bigint_add_abs(coro, b0, b1);
    if (!a0_plus_a1 || !b0_plus_b1)
        return NULL;

    XrBigInt *z1_tmp = bigint_mul_karatsuba(coro, a0_plus_a1, b0_plus_b1);
    if (!z1_tmp)
        return NULL;

    // z1 = z1_tmp - z0 - z2
    XrBigInt *z1_sub1 = bigint_sub_abs(coro, z1_tmp, z0);
    if (!z1_sub1)
        return NULL;
    XrBigInt *z1 = bigint_sub_abs(coro, z1_sub1, z2);
    if (!z1)
        return NULL;

    // result = z2 * B^(2n) + z1 * B^n + z0
    XrBigInt *z2_shifted = bigint_lshift_limbs(coro, z2, 2 * B);
    XrBigInt *z1_shifted = bigint_lshift_limbs(coro, z1, B);
    if (!z2_shifted || !z1_shifted)
        return NULL;

    XrBigInt *tmp = bigint_add_abs(coro, z2_shifted, z1_shifted);
    if (!tmp)
        return NULL;
    XrBigInt *result = bigint_add_abs(coro, tmp, z0);

    return result;
}

XrBigInt *xr_bigint_mul(struct XrCoroutine *coro, XrBigInt *a, XrBigInt *b) {
    XR_DCHECK(coro != NULL, "bigint_mul: NULL coro");
    XR_DCHECK(a != NULL, "bigint_mul: NULL a");
    XR_DCHECK(b != NULL, "bigint_mul: NULL b");
    // Fast path: multiply by zero
    if ((a->len == 1 && a->limbs[0] == 0) || (b->len == 1 && b->limbs[0] == 0)) {
        return xr_bigint_new(coro, 0);
    }

    XrBigInt *result;
    uint32_t min_len = (a->len < b->len) ? a->len : b->len;

    if (min_len >= KARATSUBA_THRESHOLD) {
        result = bigint_mul_karatsuba(coro, a, b);
    } else {
        result = bigint_mul_basecase(coro, a, b);
    }

    if (result) {
        result->sign = a->sign * b->sign;
        if (result->len == 1 && result->limbs[0] == 0)
            result->sign = 1;
    }
    return result;
}

/* ========== Division Operations ========== */

// Count leading zeros in a 32-bit value
static inline int clz32(uint32_t x) {
    if (x == 0)
        return 32;
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_clz(x);
#else
    int n = 0;
    if ((x & 0xFFFF0000U) == 0) {
        n += 16;
        x <<= 16;
    }
    if ((x & 0xFF000000U) == 0) {
        n += 8;
        x <<= 8;
    }
    if ((x & 0xF0000000U) == 0) {
        n += 4;
        x <<= 4;
    }
    if ((x & 0xC0000000U) == 0) {
        n += 2;
        x <<= 2;
    }
    if ((x & 0x80000000U) == 0) {
        n += 1;
    }
    return n;
#endif
}

// Left shift limbs array by 'shift' bits (in-place, returns carry out)
static uint32_t limbs_lshift(uint32_t *rp, const uint32_t *ap, uint32_t n, unsigned shift) {
    if (shift == 0) {
        if (rp != ap)
            memcpy(rp, ap, n * sizeof(uint32_t));
        return 0;
    }
    uint32_t carry = 0;
    unsigned rshift = 32 - shift;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t tmp = ap[i];
        rp[i] = (tmp << shift) | carry;
        carry = tmp >> rshift;
    }
    return carry;
}

// Right shift limbs array by 'shift' bits (in-place)
static void limbs_rshift(uint32_t *rp, const uint32_t *ap, uint32_t n, unsigned shift) {
    if (shift == 0) {
        if (rp != ap)
            memcpy(rp, ap, n * sizeof(uint32_t));
        return;
    }
    uint32_t carry = 0;
    unsigned lshift = 32 - shift;
    for (int i = (int) n - 1; i >= 0; i--) {
        uint32_t tmp = ap[i];
        rp[i] = (tmp >> shift) | carry;
        carry = tmp << lshift;
    }
}

// Multiply limbs by single digit, subtract from np, return borrow
// np -= dp * q, returns borrow
static uint32_t limbs_submul_1(uint32_t *np, const uint32_t *dp, uint32_t dn, uint32_t q) {
    uint64_t borrow = 0;
    for (uint32_t i = 0; i < dn; i++) {
        uint64_t prod = (uint64_t) dp[i] * q + borrow;
        uint32_t lo = (uint32_t) (prod & 0xFFFFFFFFULL);
        borrow = prod >> 32;
        if (np[i] < lo) {
            np[i] = np[i] - lo;  // wraps around
            borrow++;
        } else {
            np[i] = np[i] - lo;
        }
    }
    return (uint32_t) borrow;
}

// Add limbs: np += dp, return carry
static uint32_t limbs_add_n(uint32_t *np, const uint32_t *dp, uint32_t n) {
    uint64_t carry = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t sum = (uint64_t) np[i] + dp[i] + carry;
        np[i] = (uint32_t) (sum & 0xFFFFFFFFULL);
        carry = sum >> 32;
    }
    return (uint32_t) carry;
}

// School division algorithm (HAC 14.20)
// Computes quotient and remainder: a = q*b + r
// Returns false if b == 0
bool xr_bigint_divmod(struct XrCoroutine *coro, XrBigInt *a, XrBigInt *b, XrBigInt **q_out,
                      XrBigInt **r_out) {
    // Division by zero
    if (xr_bigint_is_zero(b)) {
        return false;
    }

    // Dividend is zero
    if (xr_bigint_is_zero(a)) {
        if (q_out)
            *q_out = xr_bigint_new(coro, 0);
        if (r_out)
            *r_out = xr_bigint_new(coro, 0);
        return true;
    }

    int cmp = xr_bigint_cmp_abs(a, b);

    // |a| < |b| => q=0, r=a
    if (cmp < 0) {
        if (q_out)
            *q_out = xr_bigint_new(coro, 0);
        if (r_out)
            *r_out = xr_bigint_copy(coro, a);
        return true;
    }

    // |a| == |b| => q=±1, r=0
    if (cmp == 0) {
        if (q_out)
            *q_out = xr_bigint_new(coro, a->sign * b->sign);
        if (r_out)
            *r_out = xr_bigint_new(coro, 0);
        return true;
    }

    int8_t q_sign = a->sign * b->sign;
    int8_t r_sign = a->sign;

    uint32_t n = a->len;
    uint32_t t = b->len;

    // Allocate working space for normalized dividend (n+1 limbs)
    uint32_t *x = (uint32_t *) xr_malloc((n + 2) * sizeof(uint32_t));
    uint32_t *y = (uint32_t *) xr_malloc((t + 1) * sizeof(uint32_t));
    if (!x || !y) {
        xr_free(x);
        xr_free(y);
        return false;
    }
    memcpy(x, a->limbs, n * sizeof(uint32_t));
    x[n] = 0;
    x[n + 1] = 0;
    memcpy(y, b->limbs, t * sizeof(uint32_t));
    y[t] = 0;

    // Normalize: shift so that high bit of divisor is set
    int norm = clz32(y[t - 1]);
    if (norm > 0) {
        x[n] = limbs_lshift(x, x, n, norm);
        limbs_lshift(y, y, t, norm);
    }

    // Allocate quotient
    uint32_t q_len = n - t + 1;
    XrBigInt *quotient = bigint_alloc(coro, q_len);
    if (!quotient) {
        xr_free(x);
        xr_free(y);
        return false;
    }
    quotient->len = q_len;

    // Get top two digits of divisor for estimation
    uint32_t yt = y[t - 1];
    uint32_t yt1 = (t >= 2) ? y[t - 2] : 0;

    // Main division loop: for i from n down to t
    for (int i = (int) n; i >= (int) t; i--) {
        // Get top digits of current remainder
        uint32_t xi = (i <= (int) n) ? x[i] : 0;
        uint32_t xi1 = (i >= 1) ? x[i - 1] : 0;
        uint32_t xi2 = (i >= 2) ? x[i - 2] : 0;

        // Estimate quotient digit q = floor((xi*B + xi1) / yt)
        uint64_t tmp;
        uint32_t qhat;
        if (xi == yt) {
            qhat = 0xFFFFFFFFU;
        } else {
            tmp = ((uint64_t) xi << 32) | xi1;
            qhat = (uint32_t) (tmp / yt);
        }

        // Refine estimate: while q*(yt*B + yt1) > xi*B^2 + xi1*B + xi2
        // This loop runs at most 2 times
        while (1) {
            uint64_t left_hi, left_lo;
            // left = qhat * (yt*B + yt1) = qhat*yt*B + qhat*yt1
            uint64_t p1 = (uint64_t) qhat * yt;
            uint64_t p2 = (uint64_t) qhat * yt1;
            left_hi = p1 + (p2 >> 32);
            left_lo = (p2 & 0xFFFFFFFFULL);

            // right = xi*B^2 + xi1*B + xi2
            // Compare (left_hi, left_lo) > (xi*B + xi1, xi2)
            uint64_t right_hi = ((uint64_t) xi << 32) | xi1;
            uint64_t right_lo = xi2;

            if (left_hi > right_hi || (left_hi == right_hi && left_lo > right_lo)) {
                qhat--;
            } else {
                break;
            }
        }

        // x = x - qhat * y * B^(i-t)
        if (qhat > 0) {
            uint32_t borrow = limbs_submul_1(x + (i - t), y, t, qhat);
            // Handle borrow from x[i]
            if (x[i] < borrow) {
                x[i] -= borrow;
                // Negative: add back y and decrement q
                limbs_add_n(x + (i - t), y, t);
                x[i]++;
                qhat--;
            } else {
                x[i] -= borrow;
            }
        }

        quotient->limbs[i - t] = qhat;
    }

    bigint_normalize(quotient);
    quotient->sign = q_sign;
    if (quotient->len == 1 && quotient->limbs[0] == 0)
        quotient->sign = 1;

    if (q_out)
        *q_out = quotient;

    // Remainder: denormalize x[0..t-1]
    if (r_out) {
        XrBigInt *remainder = bigint_alloc(coro, t);
        if (remainder) {
            if (norm > 0) {
                limbs_rshift(remainder->limbs, x, t, norm);
            } else {
                memcpy(remainder->limbs, x, t * sizeof(uint32_t));
            }
            remainder->len = t;
            bigint_normalize(remainder);
            remainder->sign = r_sign;
            if (remainder->len == 1 && remainder->limbs[0] == 0)
                remainder->sign = 1;
        }
        *r_out = remainder;
    }

    xr_free(x);
    xr_free(y);
    return true;
}

XrBigInt *xr_bigint_div(struct XrCoroutine *coro, XrBigInt *a, XrBigInt *b) {
    XR_DCHECK(coro != NULL, "bigint_div: NULL coro");
    XR_DCHECK(a != NULL, "bigint_div: NULL a");
    XR_DCHECK(b != NULL, "bigint_div: NULL b");
    XrBigInt *q = NULL;
    if (!xr_bigint_divmod(coro, a, b, &q, NULL)) {
        return NULL;
    }
    return q;
}

// Modulo operation: a % b
// Remainder has same sign as dividend a
XrBigInt *xr_bigint_mod(struct XrCoroutine *coro, XrBigInt *a, XrBigInt *b) {
    XR_DCHECK(coro != NULL, "bigint_mod: NULL coro");
    XR_DCHECK(a != NULL, "bigint_mod: NULL a");
    XR_DCHECK(b != NULL, "bigint_mod: NULL b");
    XrBigInt *r = NULL;
    if (!xr_bigint_divmod(coro, a, b, NULL, &r)) {
        return NULL;
    }
    return r;
}

/* ========== Negation and Absolute Value ========== */

XrBigInt *xr_bigint_neg(struct XrCoroutine *coro, XrBigInt *a) {
    XR_DCHECK(coro != NULL, "bigint_neg: NULL coro");
    XR_DCHECK(a != NULL, "bigint_neg: NULL a");
    XrBigInt *result = xr_bigint_copy(coro, a);
    if (result && !(result->len == 1 && result->limbs[0] == 0)) {
        result->sign = -result->sign;
    }
    return result;
}

XrBigInt *xr_bigint_abs(struct XrCoroutine *coro, XrBigInt *a) {
    XR_DCHECK(coro != NULL, "bigint_abs: NULL coro");
    XR_DCHECK(a != NULL, "bigint_abs: NULL a");
    XrBigInt *result = xr_bigint_copy(coro, a);
    if (result) {
        result->sign = 1;
    }
    return result;
}

/* ========== Type Conversion ========== */

char *xr_bigint_to_string(XrBigInt *a) {
    XR_DCHECK(a != NULL, "bigint_to_string: NULL a");
    // Estimate string length (each limb ~10 decimal digits)
    size_t max_len = a->len * 10 + 2;  // +2 for sign and null
    char *result = (char *) xr_malloc(max_len);
    if (!result)
        return NULL;

    // Special case: zero
    if (a->len == 1 && a->limbs[0] == 0) {
        strcpy(result, "0");
        return result;
    }

    // Copy for division
    uint32_t *tmp = (uint32_t *) xr_malloc(a->len * sizeof(uint32_t));
    if (!tmp) {
        xr_free(result);
        return NULL;
    }
    memcpy(tmp, a->limbs, a->len * sizeof(uint32_t));
    uint32_t tmp_len = a->len;

    // Generate digits in reverse order
    char *p = result + max_len - 1;
    *p-- = '\0';

    // Optimized: divide by 10^9 to get 9 digits at a time
    while (tmp_len > 0 && !(tmp_len == 1 && tmp[0] == 0)) {
        // Divide by RADIX_BASE (10^9)
        uint64_t carry = 0;
        for (int i = (int) tmp_len - 1; i >= 0; i--) {
            uint64_t val = (carry << 32) | tmp[i];
            tmp[i] = (uint32_t) (val / RADIX_BASE);
            carry = val % RADIX_BASE;
        }

        // Remove leading zeros first
        while (tmp_len > 1 && tmp[tmp_len - 1] == 0) {
            tmp_len--;
        }

        // Output up to 9 digits from carry
        // If there are more digits to come, output exactly 9 digits (with leading zeros)
        // Otherwise, output only significant digits
        bool more_digits = !(tmp_len == 1 && tmp[0] == 0);
        uint32_t rem = (uint32_t) carry;

        if (more_digits) {
            // Output exactly RADIX_DIGITS digits with leading zeros
            for (int d = 0; d < RADIX_DIGITS; d++) {
                *p-- = '0' + (rem % 10);
                rem /= 10;
            }
        } else {
            // Output only significant digits (no leading zeros)
            do {
                *p-- = '0' + (rem % 10);
                rem /= 10;
            } while (rem > 0);
        }
    }

    // Add sign
    if (a->sign < 0) {
        *p-- = '-';
    }

    // Move to beginning
    memmove(result, p + 1, strlen(p + 1) + 1);

    xr_free(tmp);
    return result;
}

int64_t xr_bigint_to_int64(XrBigInt *a, bool *overflow) {
    XR_DCHECK(a != NULL, "bigint_to_int64: NULL a");
    *overflow = false;

    // Check if exceeds 64-bit range
    if (a->len > 2) {
        *overflow = true;
        return a->sign > 0 ? INT64_MAX : INT64_MIN;
    }

    uint64_t val = a->limbs[0];
    if (a->len == 2) {
        val |= ((uint64_t) a->limbs[1]) << 32;
    }

    // Check signed range
    if (a->sign > 0) {
        if (val > (uint64_t) INT64_MAX) {
            *overflow = true;
            return INT64_MAX;
        }
        return (int64_t) val;
    } else {
        if (val > (uint64_t) INT64_MAX + 1) {
            *overflow = true;
            return INT64_MIN;
        }
        return -(int64_t) val;
    }
}

double xr_bigint_to_double(XrBigInt *a) {
    XR_DCHECK(a != NULL, "bigint_to_double: NULL a");
    double result = 0.0;
    double base = 1.0;

    for (uint32_t i = 0; i < a->len; i++) {
        result += a->limbs[i] * base;
        base *= LIMB_BASE;
    }

    return a->sign > 0 ? result : -result;
}

/* ========== Utility Functions ========== */

bool xr_bigint_is_zero(XrBigInt *a) {
    XR_DCHECK(a != NULL, "bigint_is_zero: NULL a");
    return a->len == 1 && a->limbs[0] == 0;
}

bool xr_bigint_is_positive(XrBigInt *a) {
    return a->sign > 0 && !xr_bigint_is_zero(a);
}

bool xr_bigint_is_negative(XrBigInt *a) {
    return a->sign < 0;
}

size_t xr_bigint_memsize(XrBigInt *a) {
    XR_DCHECK(a != NULL, "bigint_memsize: NULL a");
    return sizeof(XrBigInt) + a->cap * sizeof(uint32_t);
}

uint32_t xr_bigint_bit_length(XrBigInt *a) {
    XR_DCHECK(a != NULL, "bigint_bit_length: NULL a");
    if (xr_bigint_is_zero(a))
        return 0;
    uint32_t top = a->limbs[a->len - 1];
    return (a->len - 1) * 32 + (32 - clz32(top));
}

int xr_bigint_get_bit(XrBigInt *a, uint32_t n) {
    uint32_t limb_idx = n / 32;
    uint32_t bit_idx = n % 32;
    if (limb_idx >= a->len)
        return 0;
    return (a->limbs[limb_idx] >> bit_idx) & 1;
}

/* ========== Power Operation ========== */

XrBigInt *xr_bigint_pow(struct XrCoroutine *coro, XrBigInt *a, uint32_t n) {
    XR_DCHECK(coro != NULL, "bigint_pow: NULL coro");
    XR_DCHECK(a != NULL, "bigint_pow: NULL a");
    // a^0 = 1
    if (n == 0) {
        return xr_bigint_new(coro, 1);
    }

    // a^1 = a
    if (n == 1) {
        return xr_bigint_copy(coro, a);
    }

    // 0^n = 0 for n > 0
    if (xr_bigint_is_zero(a)) {
        return xr_bigint_new(coro, 0);
    }

    // Binary exponentiation: O(log n) multiplications
    XrBigInt *result = xr_bigint_new(coro, 1);
    XrBigInt *base = xr_bigint_copy(coro, a);
    if (!result || !base)
        return NULL;

    while (n > 0) {
        if (n & 1) {
            XrBigInt *tmp = xr_bigint_mul(coro, result, base);
            if (!tmp)
                return NULL;
            result = tmp;
        }
        n >>= 1;
        if (n > 0) {
            XrBigInt *tmp = xr_bigint_mul(coro, base, base);
            if (!tmp)
                return NULL;
            base = tmp;
        }
    }

    return result;
}

/* ========== Bitwise Operations (two's complement semantics) ========== */

// Get two's complement limbs for a BigInt value
// Positive: limbs as-is, high extension = 0x00000000
// Negative: ~(|x|-1), high extension = 0xFFFFFFFF
static void bigint_to_twos_comp(XrBigInt *a, uint32_t *tc, uint32_t len) {
    if (a->sign >= 0) {
        for (uint32_t i = 0; i < len; i++)
            tc[i] = (i < a->len) ? a->limbs[i] : 0;
        return;
    }
    // Negative: tc = ~(|a| - 1)
    uint32_t borrow = 1;
    for (uint32_t i = 0; i < len; i++) {
        uint64_t limb = (i < a->len) ? (uint64_t) a->limbs[i] : 0;
        uint64_t sub = limb - borrow;
        tc[i] = ~(uint32_t) sub;
        borrow = (limb < borrow) ? 1 : 0;
    }
}

// Convert two's complement back to sign-magnitude BigInt
static XrBigInt *bigint_from_twos_comp(struct XrCoroutine *coro, uint32_t *tc, uint32_t len,
                                       bool negative) {
    if (!negative) {
        XrBigInt *r = bigint_alloc(coro, len);
        if (!r)
            return NULL;
        for (uint32_t i = 0; i < len; i++)
            r->limbs[i] = tc[i];
        r->len = len;
        r->sign = 1;
        bigint_normalize(r);
        return r;
    }
    // Negative: |result| = ~tc + 1
    XrBigInt *r = bigint_alloc(coro, len);
    if (!r)
        return NULL;
    uint32_t carry = 1;
    for (uint32_t i = 0; i < len; i++) {
        uint64_t val = (uint64_t) (~tc[i]) + carry;
        r->limbs[i] = (uint32_t) val;
        carry = (uint32_t) (val >> 32);
    }
    r->len = len;
    r->sign = -1;
    bigint_normalize(r);
    return r;
}

// Generic bitwise operation with two's complement semantics
typedef uint32_t (*BitwiseOp)(uint32_t, uint32_t);
static uint32_t op_and(uint32_t a, uint32_t b) {
    return a & b;
}
static uint32_t op_or(uint32_t a, uint32_t b) {
    return a | b;
}
static uint32_t op_xor(uint32_t a, uint32_t b) {
    return a ^ b;
}

static XrBigInt *bigint_bitwise(struct XrCoroutine *coro, XrBigInt *a, XrBigInt *b, BitwiseOp op,
                                bool result_negative) {
    // +1 for potential carry in two's complement conversion
    uint32_t len = ((a->len > b->len) ? a->len : b->len) + 1;

    uint32_t *ta = (uint32_t *) xr_malloc(len * sizeof(uint32_t));
    uint32_t *tb = (uint32_t *) xr_malloc(len * sizeof(uint32_t));
    if (!ta || !tb) {
        xr_free(ta);
        xr_free(tb);
        return NULL;
    }

    bigint_to_twos_comp(a, ta, len);
    bigint_to_twos_comp(b, tb, len);

    for (uint32_t i = 0; i < len; i++) {
        ta[i] = op(ta[i], tb[i]);
    }

    XrBigInt *result = bigint_from_twos_comp(coro, ta, len, result_negative);
    xr_free(ta);
    xr_free(tb);
    return result;
}

XrBigInt *xr_bigint_and(struct XrCoroutine *coro, XrBigInt *a, XrBigInt *b) {
    XR_DCHECK(coro != NULL, "bigint_and: NULL coro");
    XR_DCHECK(a != NULL, "bigint_and: NULL a");
    XR_DCHECK(b != NULL, "bigint_and: NULL b");
    // AND is negative only if both operands are negative
    bool neg = (a->sign < 0) && (b->sign < 0);
    return bigint_bitwise(coro, a, b, op_and, neg);
}

XrBigInt *xr_bigint_or(struct XrCoroutine *coro, XrBigInt *a, XrBigInt *b) {
    XR_DCHECK(coro != NULL, "bigint_or: NULL coro");
    XR_DCHECK(a != NULL, "bigint_or: NULL a");
    XR_DCHECK(b != NULL, "bigint_or: NULL b");
    // OR is negative if either operand is negative
    bool neg = (a->sign < 0) || (b->sign < 0);
    return bigint_bitwise(coro, a, b, op_or, neg);
}

XrBigInt *xr_bigint_xor(struct XrCoroutine *coro, XrBigInt *a, XrBigInt *b) {
    XR_DCHECK(coro != NULL, "bigint_xor: NULL coro");
    XR_DCHECK(a != NULL, "bigint_xor: NULL a");
    XR_DCHECK(b != NULL, "bigint_xor: NULL b");
    // XOR is negative if exactly one operand is negative
    bool neg = (a->sign < 0) != (b->sign < 0);
    return bigint_bitwise(coro, a, b, op_xor, neg);
}

XrBigInt *xr_bigint_shl(struct XrCoroutine *coro, XrBigInt *a, uint32_t n) {
    XR_DCHECK(coro != NULL, "bigint_shl: NULL coro");
    XR_DCHECK(a != NULL, "bigint_shl: NULL a");
    if (xr_bigint_is_zero(a) || n == 0) {
        return xr_bigint_copy(coro, a);
    }

    uint32_t limb_shift = n / 32;
    uint32_t bit_shift = n % 32;

    uint32_t new_len = a->len + limb_shift + 1;
    XrBigInt *result = bigint_alloc(coro, new_len);
    if (!result)
        return NULL;

    // Zero out low limbs
    for (uint32_t i = 0; i < limb_shift; i++) {
        result->limbs[i] = 0;
    }

    // Shift bits
    uint32_t carry = 0;
    for (uint32_t i = 0; i < a->len; i++) {
        uint64_t val = ((uint64_t) a->limbs[i] << bit_shift) | carry;
        result->limbs[i + limb_shift] = (uint32_t) (val & 0xFFFFFFFFULL);
        carry = (uint32_t) (val >> 32);
    }
    if (carry) {
        result->limbs[a->len + limb_shift] = carry;
        result->len = a->len + limb_shift + 1;
    } else {
        result->len = a->len + limb_shift;
    }

    result->sign = a->sign;
    bigint_normalize(result);
    return result;
}

XrBigInt *xr_bigint_shr(struct XrCoroutine *coro, XrBigInt *a, uint32_t n) {
    XR_DCHECK(coro != NULL, "bigint_shr: NULL coro");
    XR_DCHECK(a != NULL, "bigint_shr: NULL a");
    if (xr_bigint_is_zero(a) || n == 0) {
        return xr_bigint_copy(coro, a);
    }

    uint32_t limb_shift = n / 32;
    uint32_t bit_shift = n % 32;

    // Shift too far: result is 0 (or -1 for negative, but we simplify to 0)
    if (limb_shift >= a->len) {
        return xr_bigint_new(coro, 0);
    }

    uint32_t new_len = a->len - limb_shift;
    XrBigInt *result = bigint_alloc(coro, new_len);
    if (!result)
        return NULL;

    // Shift bits
    uint32_t carry = 0;
    for (int i = (int) new_len - 1; i >= 0; i--) {
        uint64_t val = ((uint64_t) carry << 32) | a->limbs[i + limb_shift];
        result->limbs[i] = (uint32_t) (val >> bit_shift);
        carry = a->limbs[i + limb_shift] & ((1U << bit_shift) - 1);
    }

    result->len = new_len;
    result->sign = a->sign;
    bigint_normalize(result);
    return result;
}

// BigInt has no GC references — simple type turns black directly, no traversal or destructor needed
