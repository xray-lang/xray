/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbigint.h - Arbitrary precision integer
 *
 * KEY CONCEPT:
 *   BigInt uses base 2^32 storage, each limb is uint32_t.
 *   Supports positive/negative, zero is always positive.
 *
 * MEMORY LAYOUT:
 *   ┌─────────────────────┐
 *   │ XrGCHeader (16B)    │ GC header
 *   ├─────────────────────┤
 *   │ sign (1B)           │ sign: 1 or -1
 *   │ padding (3B)        │ alignment padding
 *   │ len (4B)            │ current limb count
 *   │ cap (4B)            │ allocated limb capacity
 *   │ padding (4B)        │ align to 32 bytes
 *   ├─────────────────────┤
 *   │ limbs[]             │ flexible array, little-endian
 *   └─────────────────────┘
 */

#ifndef XBIGINT_H
#define XBIGINT_H

#include "../gc/xgc_header.h"
#include "../../base/xdefs.h"
#include <stdint.h>
#include <stdbool.h>

struct XrayIsolate;
struct XrCoroutine;

/* ========== BigInt Structure ========== */

// XrBigInt - Arbitrary precision integer
// Base 2^32 storage, ~10x more efficient than base 10
// Sign: 1 = positive/zero, -1 = negative
typedef struct XrBigInt {
    XrGCHeader gc;          // GC header (must be first)
    int8_t sign;            // sign: 1 or -1
    uint8_t _pad1[3];       // alignment padding
    uint32_t len;           // current limb count
    uint32_t cap;           // allocated limb capacity
    uint32_t _pad2;         // align to 32 bytes
    uint32_t limbs[];       // flexible array, little-endian
} XrBigInt;

struct XrGC;

/* ========== Create ========== */

// Create from int64
XR_FUNC XrBigInt* xr_bigint_new(struct XrCoroutine *coro, int64_t value);

// Create from decimal string ("-123", "+456", "789")
XR_FUNC XrBigInt* xr_bigint_from_string(struct XrCoroutine *coro, const char *str);

// Compile-time variants: allocate on global GC (no coroutine needed)
XR_FUNC XrBigInt* xr_bigint_new_on_gc(struct XrGC *gc, int64_t value);
XR_FUNC XrBigInt* xr_bigint_from_string_on_gc(struct XrGC *gc, const char *str);

// Deep copy
XR_FUNC XrBigInt* xr_bigint_copy(struct XrCoroutine *coro, XrBigInt *a);

/* ========== Arithmetic ========== */

XR_FUNC XrBigInt* xr_bigint_add(struct XrCoroutine *coro, XrBigInt *a, XrBigInt *b);  // a + b
XR_FUNC XrBigInt* xr_bigint_sub(struct XrCoroutine *coro, XrBigInt *a, XrBigInt *b);  // a - b
XR_FUNC XrBigInt* xr_bigint_mul(struct XrCoroutine *coro, XrBigInt *a, XrBigInt *b);  // a * b (Karatsuba for large)
XR_FUNC XrBigInt* xr_bigint_div(struct XrCoroutine *coro, XrBigInt *a, XrBigInt *b);  // a / b (truncate toward zero)
XR_FUNC XrBigInt* xr_bigint_mod(struct XrCoroutine *coro, XrBigInt *a, XrBigInt *b);  // a % b

// Divmod: returns quotient and remainder, false if b==0
XR_FUNC bool xr_bigint_divmod(struct XrCoroutine *coro, XrBigInt *a, XrBigInt *b,
                      XrBigInt **q, XrBigInt **r);

XR_FUNC XrBigInt* xr_bigint_neg(struct XrCoroutine *coro, XrBigInt *a);               // -a
XR_FUNC XrBigInt* xr_bigint_abs(struct XrCoroutine *coro, XrBigInt *a);               // |a|
XR_FUNC XrBigInt* xr_bigint_pow(struct XrCoroutine *coro, XrBigInt *a, uint32_t n);   // a^n

/* ========== Bitwise ========== */

XR_FUNC XrBigInt* xr_bigint_and(struct XrCoroutine *coro, XrBigInt *a, XrBigInt *b);  // a & b
XR_FUNC XrBigInt* xr_bigint_or(struct XrCoroutine *coro, XrBigInt *a, XrBigInt *b);   // a | b
XR_FUNC XrBigInt* xr_bigint_xor(struct XrCoroutine *coro, XrBigInt *a, XrBigInt *b);  // a ^ b
XR_FUNC XrBigInt* xr_bigint_shl(struct XrCoroutine *coro, XrBigInt *a, uint32_t n);   // a << n
XR_FUNC XrBigInt* xr_bigint_shr(struct XrCoroutine *coro, XrBigInt *a, uint32_t n);   // a >> n (arithmetic)

/* ========== Comparison ========== */

XR_FUNC int xr_bigint_cmp(XrBigInt *a, XrBigInt *b);      // -1/0/1 for a <=> b
XR_FUNC int xr_bigint_cmp_abs(XrBigInt *a, XrBigInt *b);  // compare |a| vs |b|
XR_FUNC int xr_bigint_cmp_int(XrBigInt *a, int64_t n);    // compare with int64

/* ========== Conversion ========== */

XR_FUNC char* xr_bigint_to_string(XrBigInt *a);                    // decimal string (caller frees)
XR_FUNC int64_t xr_bigint_to_int64(XrBigInt *a, bool *overflow);  // may truncate if overflow
XR_FUNC double xr_bigint_to_double(XrBigInt *a);                  // may lose precision

/* ========== Utility ========== */

XR_FUNC bool xr_bigint_is_zero(XrBigInt *a);
XR_FUNC bool xr_bigint_is_positive(XrBigInt *a);     // >0 (not including zero)
XR_FUNC bool xr_bigint_is_negative(XrBigInt *a);     // <0
XR_FUNC uint32_t xr_bigint_bit_length(XrBigInt *a);  // binary bit count
XR_FUNC int xr_bigint_get_bit(XrBigInt *a, uint32_t n);  // get bit at index n

/* ========== GC ========== */

XR_FUNC size_t xr_bigint_memsize(XrBigInt *a);  // memory size of BigInt object

#endif // XBIGINT_H
