/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_arith.h - Tagged arithmetic, comparison, and print
 */

#ifndef XRT_ARITH_H
#define XRT_ARITH_H

#include "xrt_value.h"
#include "../runtime/value/xtype_names.h" /* XrTypeId + scalar-rep mapping */
#include "xrt_arc.h"                      // xrt_str_concat used by xrt_add
#include "xrt_range.h"
#include "xrt_coll.h"  // forward-declares xrt_throw_exc (div/mod by zero); the
                       // definition is provided by xrt_exception.h in the full
                       // xrt.h build and by the host TU in standalone unit tests
#include "xrt_class.h"
#include "../shared/xr_int_arith.h"
#include "../shared/xr_bits_core.h"
#include "../shared/xr_type_identity_core.h"

/* =========================================================================
 * Tagged arithmetic — all inline, no extern dependency
 * ========================================================================= */

/* int64 add/sub/mul with two's-complement wrap on overflow.
 * Signed overflow is UB in C, so compute on uint64_t and cast back. This is
 * the single source of truth for AOT integer arithmetic wrap and MUST match
 * the VM (uint64 wrap in xvm_dispatch_arith) and xi_opt constant folding so
 * INT64_MAX + 1, INT64_MIN - 1, etc. produce identical results across tiers. */
static inline int64_t xrt_i64_add(int64_t a, int64_t b) {
    return xr_i64_add_wrap(a, b);
}
static inline int64_t xrt_i64_sub(int64_t a, int64_t b) {
    return xr_i64_sub_wrap(a, b);
}
static inline int64_t xrt_i64_mul(int64_t a, int64_t b) {
    return xr_i64_mul_wrap(a, b);
}

/* =========================================================================
 * BigInt arithmetic — self-contained multi-precision on the AOT value model.
 * Operands are the normalized limb views shared with static literals, never
 * mutated. Results are freshly allocated owned BigInts: header-first so the
 * view and RC read from v.ptr, arena-managed with a zero refcount so ordinary
 * xrt_retain/xrt_release track them and arena teardown reclaims stragglers, and
 * no destructor flag since limbs are inline. The magnitude kernels mirror the
 * VM's xr_bigint.c so both backends compute byte-identical results.
 * ========================================================================= */

/* Allocate an owned, mutable BigInt with room for cap limbs, value zero. */
static inline xrt_bigint_view_t *xrt_bigint_new(uint32_t cap, XrValue *out) {
    if (cap == 0)
        cap = 1;
    size_t total = sizeof(xrt_bigint_view_t) + (size_t) cap * sizeof(uint32_t);
    XrObjHeader *hdr = (XrObjHeader *) xrt_execution_alloc(total, xrt_execution_finalize_generic);
    xrt_bigint_view_t *b = (xrt_bigint_view_t *) hdr;
    b->klass = NULL;
    b->sign = 1;
    b->len = 1;
    b->cap = cap;
    memset(b->limbs, 0, (size_t) cap * sizeof(uint32_t));
    *out = xr_mkptr(hdr, XR_TAG_BIGINT);
    return b;
}

/* Drop leading zero limbs and pin zero to a single positive limb. */
static inline void xrt_bigint_norm(xrt_bigint_view_t *a) {
    while (a->len > 1 && a->limbs[a->len - 1] == 0)
        a->len--;
    if (a->len <= 1 && a->limbs[0] == 0) {
        a->len = 1;
        a->sign = 1;
    }
}

static inline int xrt_bigint_is_zero_v(const xrt_bigint_view_t *a) {
    return a->len == 0 || (a->len == 1 && a->limbs[0] == 0);
}

static inline int xrt_bigint_cmp_abs_v(const xrt_bigint_view_t *a, const xrt_bigint_view_t *b) {
    if (a->len != b->len)
        return a->len > b->len ? 1 : -1;
    for (int i = (int) a->len - 1; i >= 0; i--)
        if (a->limbs[i] != b->limbs[i])
            return a->limbs[i] > b->limbs[i] ? 1 : -1;
    return 0;
}

/* a +/- b as signed magnitudes. subtract flips b's effective sign; equal signs
 * add magnitudes, opposite signs subtract the smaller magnitude from the
 * larger and keep the larger's sign. */
static inline XrValue xrt_bigint_addsub(XrValue av, XrValue bv, int subtract) {
    const xrt_bigint_view_t *a = xrt_bigint_view(av);
    const xrt_bigint_view_t *b = xrt_bigint_view(bv);
    XrValue rv;
    if (!a || !b) {
        xrt_bigint_new(1, &rv);
        return rv;
    }
    int as = xrt_bigint_is_zero_v(a) ? 1 : (a->sign < 0 ? -1 : 1);
    int bs = xrt_bigint_is_zero_v(b) ? 1 : (b->sign < 0 ? -1 : 1);
    if (subtract)
        bs = -bs;
    xrt_bigint_view_t *r;
    if (as == bs) {
        const xrt_bigint_view_t *x = a, *y = b;
        if (x->len < y->len) {
            const xrt_bigint_view_t *t = x;
            x = y;
            y = t;
        }
        r = xrt_bigint_new(x->len + 1, &rv);
        uint64_t carry = 0;
        uint32_t i = 0;
        for (; i < y->len; i++) {
            uint64_t s = (uint64_t) x->limbs[i] + y->limbs[i] + carry;
            r->limbs[i] = (uint32_t) s;
            carry = s >> 32;
        }
        for (; i < x->len; i++) {
            uint64_t s = (uint64_t) x->limbs[i] + carry;
            r->limbs[i] = (uint32_t) s;
            carry = s >> 32;
        }
        if (carry)
            r->limbs[i++] = (uint32_t) carry;
        r->len = i;
        r->sign = (int8_t) as;
    } else {
        int c = xrt_bigint_cmp_abs_v(a, b);
        if (c == 0) {
            xrt_bigint_new(1, &rv);
            return rv;
        }
        const xrt_bigint_view_t *hi = c > 0 ? a : b;
        const xrt_bigint_view_t *lo = c > 0 ? b : a;
        int sign = c > 0 ? as : bs;
        r = xrt_bigint_new(hi->len, &rv);
        int64_t borrow = 0;
        for (uint32_t i = 0; i < hi->len; i++) {
            int64_t d =
                (int64_t) hi->limbs[i] - (i < lo->len ? (int64_t) lo->limbs[i] : 0) - borrow;
            if (d < 0) {
                d += (int64_t) 0x100000000LL;
                borrow = 1;
            } else {
                borrow = 0;
            }
            r->limbs[i] = (uint32_t) d;
        }
        r->len = hi->len;
        r->sign = (int8_t) sign;
    }
    xrt_bigint_norm(r);
    return rv;
}

static inline XrValue xrt_bigint_neg_val(XrValue av) {
    const xrt_bigint_view_t *a = xrt_bigint_view(av);
    XrNumericNegBigIntPlan plan = XR_NUMERIC_NEG_BIGINT_OWNER_PLAN(
        XR_SEM_OWNER_ID_SHARED_NUMERIC_NEG_HI, XR_SEM_OWNER_ID_SHARED_NUMERIC_NEG_LO,
        XR_SEM_CONSUMER_AOT_HOSTED, a ? a->limbs : NULL, a ? a->len : 0, a ? a->sign : 0);
    if (!plan.valid)
        xrt_throw_exc(xr_box_str("E0404: invalid BigInt operand"));
    XrValue rv;
    uint32_t n = plan.length;
    xrt_bigint_view_t *r = xrt_bigint_new(n, &rv);
    memcpy(r->limbs, a->limbs, (size_t) plan.length * sizeof(uint32_t));
    r->len = plan.length;
    r->sign = plan.result_sign;
    xrt_bigint_norm(r);
    return rv;
}

static inline XrValue xrt_bigint_mul_val(XrValue av, XrValue bv) {
    const xrt_bigint_view_t *a = xrt_bigint_view(av);
    const xrt_bigint_view_t *b = xrt_bigint_view(bv);
    XrValue rv;
    if (!a || !b) {
        xrt_bigint_new(1, &rv);
        return rv;
    }
    xrt_bigint_view_t *r = xrt_bigint_new(a->len + b->len, &rv);
    for (uint32_t i = 0; i < a->len; i++) {
        uint64_t carry = 0;
        for (uint32_t j = 0; j < b->len; j++) {
            uint64_t p = (uint64_t) a->limbs[i] * b->limbs[j] + r->limbs[i + j] + carry;
            r->limbs[i + j] = (uint32_t) p;
            carry = p >> 32;
        }
        if (carry)
            r->limbs[i + b->len] += (uint32_t) carry;
    }
    r->len = a->len + b->len;
    int za = xrt_bigint_is_zero_v(a), zb = xrt_bigint_is_zero_v(b);
    int sa = a->sign < 0 ? -1 : 1, sb = b->sign < 0 ? -1 : 1;
    r->sign = (za || zb) ? 1 : (int8_t) (sa * sb);
    xrt_bigint_norm(r);
    return rv;
}

/* Copy a limb view into a fresh owned BigInt, preserving sign. */
static inline XrValue xrt_bigint_copy_val(const xrt_bigint_view_t *a) {
    XrValue rv;
    uint32_t n = (a && a->len) ? a->len : 1;
    xrt_bigint_view_t *r = xrt_bigint_new(n, &rv);
    if (a) {
        memcpy(r->limbs, a->limbs, (size_t) a->len * sizeof(uint32_t));
        r->len = a->len ? a->len : 1;
        r->sign = a->sign;
    }
    xrt_bigint_norm(r);
    return rv;
}

/* Small-magnitude BigInt from an int (used for quotient +/-1 and zero). */
static inline XrValue xrt_bigint_from_small(int64_t v) {
    XrValue rv;
    xrt_bigint_view_t *r = xrt_bigint_new(2, &rv);
    uint64_t mag = (v < 0) ? (~(uint64_t) v + 1u) : (uint64_t) v;
    r->limbs[0] = (uint32_t) (mag & 0xFFFFFFFFu);
    r->limbs[1] = (uint32_t) (mag >> 32);
    r->len = r->limbs[1] ? 2 : 1;
    r->sign = (v < 0) ? -1 : 1;
    xrt_bigint_norm(r);
    return rv;
}

/* Limb-array primitives for the division kernel (Knuth Algorithm D). */
static inline uint32_t xrt_bi_clz32(uint32_t x) {
    if (x == 0)
        return 32;
#if defined(__GNUC__) || defined(__clang__)
    return (uint32_t) __builtin_clz(x);
#else
    uint32_t n = 0;
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

static inline uint32_t xrt_bi_lshift(uint32_t *rp, const uint32_t *ap, uint32_t n, unsigned shift) {
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

static inline void xrt_bi_rshift(uint32_t *rp, const uint32_t *ap, uint32_t n, unsigned shift) {
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

static inline uint32_t xrt_bi_submul_1(uint32_t *np, const uint32_t *dp, uint32_t dn, uint32_t q) {
    uint64_t borrow = 0;
    for (uint32_t i = 0; i < dn; i++) {
        uint64_t prod = (uint64_t) dp[i] * q + borrow;
        uint32_t lo = (uint32_t) (prod & 0xFFFFFFFFULL);
        borrow = prod >> 32;
        if (np[i] < lo)
            borrow++;
        np[i] = np[i] - lo;
    }
    return (uint32_t) borrow;
}

static inline uint32_t xrt_bi_add_n(uint32_t *np, const uint32_t *dp, uint32_t n) {
    uint64_t carry = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t sum = (uint64_t) np[i] + dp[i] + carry;
        np[i] = (uint32_t) (sum & 0xFFFFFFFFULL);
        carry = sum >> 32;
    }
    return (uint32_t) carry;
}

/* q = a / b, r = a % b (truncating toward zero; remainder takes a's sign).
 * *ok is 0 on divide-by-zero. Mirrors the VM's xr_bigint_divmod so quotient and
 * remainder are byte-identical across backends. */
static inline void xrt_bigint_divmod(XrValue av, XrValue bv, XrValue *q_out, XrValue *r_out,
                                     int *ok) {
    const xrt_bigint_view_t *a = xrt_bigint_view(av);
    const xrt_bigint_view_t *b = xrt_bigint_view(bv);
    *ok = 1;
    if (!a || !b || xrt_bigint_is_zero_v(b)) {
        *ok = 0;
        return;
    }
    if (xrt_bigint_is_zero_v(a)) {
        if (q_out)
            xrt_bigint_new(1, q_out);
        if (r_out)
            xrt_bigint_new(1, r_out);
        return;
    }
    int cmp = xrt_bigint_cmp_abs_v(a, b);
    if (cmp < 0) {
        if (q_out)
            xrt_bigint_new(1, q_out);
        if (r_out)
            *r_out = xrt_bigint_copy_val(a);
        return;
    }
    if (cmp == 0) {
        if (q_out)
            *q_out = xrt_bigint_from_small((a->sign < 0 ? -1 : 1) * (b->sign < 0 ? -1 : 1));
        if (r_out)
            xrt_bigint_new(1, r_out);
        return;
    }

    int8_t q_sign = (int8_t) ((a->sign < 0 ? -1 : 1) * (b->sign < 0 ? -1 : 1));
    int8_t r_sign = (int8_t) (a->sign < 0 ? -1 : 1);
    uint32_t n = a->len;
    uint32_t t = b->len;

    uint32_t *x = (uint32_t *) XRT_MALLOC((size_t) (n + 2) * sizeof(uint32_t));
    uint32_t *y = (uint32_t *) XRT_MALLOC((size_t) (t + 1) * sizeof(uint32_t));
    if (!x || !y) {
        XRT_FREE(x);
        XRT_FREE(y);
        *ok = 0;
        return;
    }
    memcpy(x, a->limbs, (size_t) n * sizeof(uint32_t));
    x[n] = 0;
    x[n + 1] = 0;
    memcpy(y, b->limbs, (size_t) t * sizeof(uint32_t));
    y[t] = 0;

    unsigned norm = xrt_bi_clz32(y[t - 1]);
    if (norm > 0) {
        x[n] = xrt_bi_lshift(x, x, n, norm);
        xrt_bi_lshift(y, y, t, norm);
    }

    XrValue qval;
    xrt_bigint_view_t *quotient = xrt_bigint_new(n - t + 1, &qval);
    quotient->len = n - t + 1;

    uint32_t yt = y[t - 1];
    uint32_t yt1 = (t >= 2) ? y[t - 2] : 0;

    for (int i = (int) n; i >= (int) t; i--) {
        uint32_t xi = x[i];
        uint32_t xi1 = (i >= 1) ? x[i - 1] : 0;
        uint32_t xi2 = (i >= 2) ? x[i - 2] : 0;

        uint32_t qhat;
        if (xi == yt) {
            qhat = 0xFFFFFFFFU;
        } else {
            uint64_t tmp = ((uint64_t) xi << 32) | xi1;
            qhat = (uint32_t) (tmp / yt);
        }
        while (1) {
            uint64_t p1 = (uint64_t) qhat * yt;
            uint64_t p2 = (uint64_t) qhat * yt1;
            uint64_t left_hi = p1 + (p2 >> 32);
            uint64_t left_lo = (p2 & 0xFFFFFFFFULL);
            uint64_t right_hi = ((uint64_t) xi << 32) | xi1;
            uint64_t right_lo = xi2;
            if (left_hi > right_hi || (left_hi == right_hi && left_lo > right_lo))
                qhat--;
            else
                break;
        }
        if (qhat > 0) {
            uint32_t borrow = xrt_bi_submul_1(x + (i - (int) t), y, t, qhat);
            if (x[i] < borrow) {
                x[i] -= borrow;
                xrt_bi_add_n(x + (i - (int) t), y, t);
                x[i]++;
                qhat--;
            } else {
                x[i] -= borrow;
            }
        }
        quotient->limbs[i - (int) t] = qhat;
    }

    xrt_bigint_norm(quotient);
    quotient->sign = q_sign;
    if (quotient->len == 1 && quotient->limbs[0] == 0)
        quotient->sign = 1;
    if (q_out)
        *q_out = qval;

    if (r_out) {
        XrValue rval;
        xrt_bigint_view_t *remainder = xrt_bigint_new(t, &rval);
        if (norm > 0)
            xrt_bi_rshift(remainder->limbs, x, t, norm);
        else
            memcpy(remainder->limbs, x, (size_t) t * sizeof(uint32_t));
        remainder->len = t;
        xrt_bigint_norm(remainder);
        remainder->sign = r_sign;
        if (remainder->len == 1 && remainder->limbs[0] == 0)
            remainder->sign = 1;
        *r_out = rval;
    }

    XRT_FREE(x);
    XRT_FREE(y);
}

static inline XrValue xrt_bigint_div_val(XrValue a, XrValue b) {
    XrValue q;
    int ok;
    xrt_bigint_divmod(a, b, &q, NULL, &ok);
    if (!ok)
        xrt_throw_exc(xr_box_str("E0420: division by zero"));
    return q;
}

static inline XrValue xrt_bigint_mod_val(XrValue a, XrValue b) {
    XrValue r;
    int ok;
    xrt_bigint_divmod(a, b, NULL, &r, &ok);
    if (!ok)
        xrt_throw_exc(xr_box_str("E0421: modulo by zero"));
    return r;
}

/* Adapt hosted BigInt representation and allocation to shared.bitwise-binary.
 * Two's-complement conversion, operator choice and result sign remain solely
 * in the runtime-neutral owner. */
static inline XrValue xrt_bigint_bitwise_val(XrValue av, XrValue bv,
                                             XrBitwiseBinaryKind kind) {
    const xrt_bigint_view_t *a = xrt_bigint_view(av);
    const xrt_bigint_view_t *b = xrt_bigint_view(bv);
    XrValue rv;
    if (!a || !b)
        xrt_throw_exc(xr_box_str("E0404: bitwise operation requires integer types"));
    XrBigIntBitwisePlan plan = XR_BITWISE_BINARY_BIGINT_OWNER_PLAN(
        XR_SEM_OWNER_ID_SHARED_BITWISE_BINARY_HI, XR_SEM_OWNER_ID_SHARED_BITWISE_BINARY_LO,
        XR_SEM_CONSUMER_AOT_HOSTED, kind, a->len, a->sign, b->len, b->sign);
    if (plan.status == XR_BITWISE_BINARY_STATUS_INVALID_KIND)
        xrt_throw_exc(xr_box_str("E0404: invalid bitwise operation"));
    if (plan.status == XR_BITWISE_BINARY_STATUS_CAPACITY_OVERFLOW)
        xrt_throw_exc(xr_box_str("E0601: bigint bitwise allocation failed"));
    xrt_bigint_view_t *r = xrt_bigint_new(plan.capacity, &rv);
    r->len = XR_BITWISE_BINARY_BIGINT_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_BITWISE_BINARY_HI, XR_SEM_OWNER_ID_SHARED_BITWISE_BINARY_LO,
        XR_SEM_CONSUMER_AOT_HOSTED, &plan, a->limbs, a->len, a->sign, b->limbs, b->len,
        b->sign, r->limbs, &r->sign);
    return rv;
}

/* Adapt BigInt storage/allocation to the shared shift owner. Negative and
 * oversized counts are rejected; zero copies and right-past-end yields zero. */
static inline XrValue xrt_bigint_shift_val(XrValue av, int64_t count, XrShiftKind kind) {
    const xrt_bigint_view_t *a = xrt_bigint_view(av);
    XrValue rv;
    if (!a)
        xrt_throw_exc(xr_box_str("E0404: shift operation requires integer types"));
    XrBigIntShiftPlan plan = XR_SHIFT_BIGINT_OWNER_PLAN(
        XR_SEM_OWNER_ID_SHARED_SHIFT_HI, XR_SEM_OWNER_ID_SHARED_SHIFT_LO,
        XR_SEM_CONSUMER_AOT_HOSTED, kind, a->len, xrt_bigint_is_zero_v(a), count);
    if (plan.status == XR_SHIFT_STATUS_COUNT_RANGE)
        xrt_throw_exc(xr_box_str("E0404: bigint shift count out of range"));
    if (plan.status == XR_SHIFT_STATUS_CAPACITY_OVERFLOW)
        xrt_throw_exc(xr_box_str("E0601: bigint shift allocation failed"));
    xrt_bigint_view_t *r = xrt_bigint_new(plan.capacity, &rv);
    r->len = XR_SHIFT_BIGINT_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_SHIFT_HI, XR_SEM_OWNER_ID_SHARED_SHIFT_LO,
        XR_SEM_CONSUMER_AOT_HOSTED, &plan, a->limbs, a->len, a->sign, r->limbs, &r->sign);
    return rv;
}

static inline XrValue xrt_add(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return XR_FROM_INT(xrt_i64_add(a.i, b.i));
    if (a.tag == XR_TAG_BIGINT && b.tag == XR_TAG_BIGINT)
        return xrt_bigint_addsub(a, b, 0);
    if (XR_IS_STR(a) && XR_IS_STR(b))
        return xrt_str_concat_value(a, b); /* header lengths, no strlen */
    if (XR_IS_STR(a) || XR_IS_STR(b)) {
        char ba[64], bb[64];
        return xrt_str_concat(xr_to_cstr(a, ba, sizeof(ba)), xr_to_cstr(b, bb, sizeof(bb)));
    }
    double fa = a.tag == XR_TAG_I64 ? (double) a.i : a.f;
    double fb = b.tag == XR_TAG_I64 ? (double) b.i : b.f;
    return XR_FROM_FLOAT(fa + fb);
}

static inline XrValue xrt_sub(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return XR_FROM_INT(xrt_i64_sub(a.i, b.i));
    if (a.tag == XR_TAG_BIGINT && b.tag == XR_TAG_BIGINT)
        return xrt_bigint_addsub(a, b, 1);
    double fa = (a.tag == XR_TAG_I64) ? (double) a.i : a.f;
    double fb = (b.tag == XR_TAG_I64) ? (double) b.i : b.f;
    return XR_FROM_FLOAT(fa - fb);
}

static inline XrValue xrt_mul(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return XR_FROM_INT(xrt_i64_mul(a.i, b.i));
    if (a.tag == XR_TAG_BIGINT && b.tag == XR_TAG_BIGINT)
        return xrt_bigint_mul_val(a, b);
    double fa = (a.tag == XR_TAG_I64) ? (double) a.i : a.f;
    double fb = (b.tag == XR_TAG_I64) ? (double) b.i : b.f;
    return XR_FROM_FLOAT(fa * fb);
}

/* Integer div/mod with zero-check + wrap.
 * Single source of truth for every AOT integer divide path (typed scalar
 * codegen in xi_cgen and the tagged xrt_div / xrt_mod below).
 *   b == 0          → throw (matches VM E0420 / E0421)
 *   INT64_MIN / -1  → INT64_MIN (unsigned negate; matches xi_opt fold)
 *   INT64_MIN % -1  → 0 */
static inline int64_t xrt_int_div(int64_t a, int64_t b) {
    if (XR_UNLIKELY(b == 0))
        xrt_throw_exc(xr_box_str("E0420: division by zero"));
    return xr_i64_div_wrap(a, b);
}
static inline int64_t xrt_int_mod(int64_t a, int64_t b) {
    if (XR_UNLIKELY(b == 0))
        xrt_throw_exc(xr_box_str("E0421: modulo by zero"));
    return xr_i64_mod_wrap(a, b);
}

/* Unsigned division / modulo for statically-unsigned operands (mirrors VM
 * OP_DIV_U / OP_MOD_U and xr_i64_div_u_wrap). uint64_t covers every unsigned
 * width: narrower payloads are zero-extended in the i64 value model. */
static inline int64_t xrt_uint_div(int64_t a, int64_t b) {
    if (XR_UNLIKELY(b == 0))
        xrt_throw_exc(xr_box_str("E0420: division by zero"));
    return xr_i64_div_u_wrap(a, b);
}
static inline int64_t xrt_uint_mod(int64_t a, int64_t b) {
    if (XR_UNLIKELY(b == 0))
        xrt_throw_exc(xr_box_str("E0421: modulo by zero"));
    return xr_i64_mod_u_wrap(a, b);
}

/* Shifts: the language defines the count as taken mod 64 (spec: "shift count
 * is taken modulo 64 — unlike C, xray shifts are always defined"). Matches
 * xi_opt constant folding and AOT hardware behavior (x64 SHL/SAR with CL,
 * ARM64 LSL/ASR, RISC-V SLL/SRA all mask to 6 bits). Left shift goes through
 * uint64_t because shifting into/past the sign bit is UB on signed in C. */
static inline XrValue xrt_div(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return XR_FROM_INT(xrt_int_div(a.i, b.i));
    if (a.tag == XR_TAG_BIGINT && b.tag == XR_TAG_BIGINT)
        return xrt_bigint_div_val(a, b);
    double fa = (a.tag == XR_TAG_I64) ? (double) a.i : a.f;
    double fb = (b.tag == XR_TAG_I64) ? (double) b.i : b.f;
    return XR_FROM_FLOAT(fa / fb);
}

static inline XrValue xrt_mod(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return XR_FROM_INT(xrt_int_mod(a.i, b.i));
    if (a.tag == XR_TAG_BIGINT && b.tag == XR_TAG_BIGINT)
        return xrt_bigint_mod_val(a, b);
    xrt_throw_exc(xr_box_str("E0404: modulo requires integer types"));
}

static inline XrValue xrt_neg(XrValue a) {
    if (a.tag == XR_TAG_I64) {
        XrNumericNegResult result = XR_NUMERIC_NEG_OWNER_APPLY(
            XR_SEM_OWNER_ID_SHARED_NUMERIC_NEG_HI, XR_SEM_OWNER_ID_SHARED_NUMERIC_NEG_LO,
            XR_SEM_CONSUMER_AOT_HOSTED, XR_NUMERIC_NEG_I64, a.i, 0.0);
        return XR_FROM_INT(result.i64);
    }
    if (a.tag == XR_TAG_BIGINT)
        return xrt_bigint_neg_val(a);
    if (a.tag == XR_TAG_F64) {
        XrNumericNegResult result = XR_NUMERIC_NEG_OWNER_APPLY(
            XR_SEM_OWNER_ID_SHARED_NUMERIC_NEG_HI, XR_SEM_OWNER_ID_SHARED_NUMERIC_NEG_LO,
            XR_SEM_CONSUMER_AOT_HOSTED, XR_NUMERIC_NEG_F64, 0, a.f);
        return XR_FROM_FLOAT(result.f64);
    }
    xrt_throw_exc(xr_box_str("E0404: operand must be numeric"));
}

/* =========================================================================
 * Inline tagged comparisons
 * ========================================================================= */

static inline int64_t xrt_lt(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return a.i < b.i;
    if (a.tag == XR_TAG_BIGINT && b.tag == XR_TAG_BIGINT)
        return xrt_bigint_cmp_value(a, b) < 0;
    double fa = (a.tag == XR_TAG_I64) ? (double) a.i : a.f;
    double fb = (b.tag == XR_TAG_I64) ? (double) b.i : b.f;
    return fa < fb;
}

static inline int64_t xrt_le(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return a.i <= b.i;
    if (a.tag == XR_TAG_BIGINT && b.tag == XR_TAG_BIGINT)
        return xrt_bigint_cmp_value(a, b) <= 0;
    double fa = (a.tag == XR_TAG_I64) ? (double) a.i : a.f;
    double fb = (b.tag == XR_TAG_I64) ? (double) b.i : b.f;
    return fa <= fb;
}

/* =========================================================================
 * Inline print
 * ========================================================================= */

/* Recursion / element caps must match the VM formatter (xvalue_format.c:
 * XR_FORMAT_MAX_DEPTH / XR_FORMAT_MAX_ELEMENTS) so container printing is
 * byte-identical across backends. */
#define XRT_FORMAT_MAX_DEPTH 3
#define XRT_FORMAT_MAX_ELEMENTS 32

/* Format sink: a NULL strbuf writes to stdout, otherwise text is appended to the
 * builder. The print path and value.toString() share xrt_format_value so they
 * render scalars and containers identically (single source of truth). */
static inline void xrt_fmt_puts(xrt_strbuf_t *sb, const char *s, size_t n) {
    if (sb) {
        xrt_strbuf_grow(sb, (int64_t) n);
        memcpy(sb->buf + sb->len, s, n);
        sb->len += (int64_t) n;
        sb->buf[sb->len] = 0;
    } else if (n > 0) {
        fwrite(s, 1, n, stdout);
    }
}
static inline void xrt_fmt_cstr(xrt_strbuf_t *sb, const char *s) {
    xrt_fmt_puts(sb, s, strlen(s));
}
static inline void xrt_fmt_char(xrt_strbuf_t *sb, char c) {
    xrt_fmt_puts(sb, &c, 1);
}

/* Append the full decimal rendering of BigInt |a| to sb. Mirrors the VM's
 * xr_bigint_to_string (repeatedly divide the magnitude by 10^9, emitting nine
 * digits per step) so both backends print byte-identical decimals. A growable
 * strbuf lets arbitrarily large values render without the fixed-buffer cap that
 * forced xr_to_cstr to fall back to a marker. */
static inline void xrt_bigint_format(const xrt_bigint_view_t *a, xrt_strbuf_t *sb) {
    if (!a || a->len == 0 || (a->len == 1 && a->limbs[0] == 0)) {
        xrt_fmt_char(sb, '0');
        return;
    }
    uint32_t tmp_len = a->len;
    uint32_t *tmp = (uint32_t *) XRT_MALLOC((size_t) tmp_len * sizeof(uint32_t));
    size_t cap = (size_t) a->len * 10 + 2;
    char *out = (char *) XRT_MALLOC(cap);
    if (!tmp || !out) {
        XRT_FREE(tmp);
        XRT_FREE(out);
        xrt_fmt_cstr(sb, "<BigInt>");
        return;
    }
    memcpy(tmp, a->limbs, (size_t) tmp_len * sizeof(uint32_t));

    char *p = out + cap - 1;
    *p-- = '\0';
    while (tmp_len > 0 && !(tmp_len == 1 && tmp[0] == 0)) {
        uint64_t carry = 0;
        for (int i = (int) tmp_len - 1; i >= 0; i--) {
            uint64_t val = (carry << 32) | tmp[i];
            tmp[i] = (uint32_t) (val / 1000000000U);
            carry = val % 1000000000U;
        }
        while (tmp_len > 1 && tmp[tmp_len - 1] == 0)
            tmp_len--;
        int more = !(tmp_len == 1 && tmp[0] == 0);
        uint32_t rem = (uint32_t) carry;
        if (more) {
            for (int d = 0; d < 9; d++) {
                *p-- = (char) ('0' + (rem % 10));
                rem /= 10;
            }
        } else {
            do {
                *p-- = (char) ('0' + (rem % 10));
                rem /= 10;
            } while (rem > 0);
        }
    }
    if (a->sign < 0)
        *p-- = '-';
    xrt_fmt_cstr(sb, p + 1);
    XRT_FREE(out);
    XRT_FREE(tmp);
}

static void xrt_format_value(XrValue v, xrt_strbuf_t *sb, int depth) {
    switch (v.tag) {
        case XR_TAG_STR:
        case XR_TAG_STR_ARC:
            /* Nested strings are quoted, matching the VM formatter. */
            if (depth > 0)
                xrt_fmt_char(sb, '"');
            xrt_fmt_puts(sb, xr_str_data(v), (size_t) xr_str_len(v));
            if (depth > 0)
                xrt_fmt_char(sb, '"');
            return;
        case XR_TAG_I64: {
            char buf[24];
            int n = snprintf(buf, sizeof(buf), "%lld", (long long) v.i);
            xrt_fmt_puts(sb, buf, (size_t) n);
            return;
        }
        case XR_TAG_F64: {
            char buf[64];
            xrt_format_float(buf, sizeof(buf), v.f);
            xrt_fmt_cstr(sb, buf);
            return;
        }
        case XR_TAG_BOOL:
            xrt_fmt_cstr(sb, v.i ? "true" : "false");
            return;
        case XR_TAG_RUNE: {
            char buf[4];
            int n = xrt_rune_utf8_encode(XR_TO_RUNE(v), buf);
            if (n > 0)
                xrt_fmt_puts(sb, buf, (size_t) n);
            return;
        }
        case XR_TAG_NULL:
            xrt_fmt_cstr(sb, "null");
            return;
        case XR_TAG_BIGINT:
            xrt_bigint_format(xrt_bigint_view(v), sb);
            return;
        case XR_TAG_ENUM: {
            const char *enum_name = NULL;
            const char *member_name = NULL;
            if (!xrt_enum_key_parts(v, &enum_name, &member_name, NULL, NULL)) {
                xrt_fmt_cstr(sb, "<enum>");
                return;
            }
            xrt_fmt_cstr(sb, enum_name ? enum_name : "<enum>");
            xrt_fmt_char(sb, '.');
            xrt_fmt_cstr(sb, member_name ? member_name : "?");
            const XrAotEnumBox *ev = xrt_enum_box_view(v);
            if (ev && ev->payload_count > 0) {
                xrt_fmt_char(sb, '(');
                for (uint32_t i = 0; i < ev->payload_count; i++) {
                    if (i > 0)
                        xrt_fmt_cstr(sb, ", ");
                    xrt_format_value(ev->payloads[i], sb, depth + 1);
                }
                xrt_fmt_char(sb, ')');
            }
            return;
        }
        case XR_TAG_RANGE: {
            char buf[96];
            xrt_range_format_buf((const xrt_range_t *) v.ptr, buf, sizeof(buf));
            xrt_fmt_cstr(sb, buf);
            return;
        }
        /* Name-free by design, matching the VM: the callable descriptor carries
         * no source name, and taking one from the type-name profile would make
         * a program's output depend on how it was built. */
        case XR_TAG_CLOSURE:
            xrt_fmt_cstr(sb, "<fn>");
            return;
        default:
            break;
    }

    if (depth > XRT_FORMAT_MAX_DEPTH) {
        xrt_fmt_cstr(sb, "...");
        return;
    }

    switch (xrt_value_kind(v)) {
        case XR_TAG_ARRAY: {
            xrt_array_t *a = (xrt_array_t *) v.ptr;
            int64_t len = a ? a->length : 0;
            int64_t limit = len > XRT_FORMAT_MAX_ELEMENTS ? XRT_FORMAT_MAX_ELEMENTS : len;
            xrt_fmt_char(sb, '[');
            for (int64_t i = 0; i < limit; i++) {
                if (i > 0)
                    xrt_fmt_cstr(sb, ", ");
                xrt_format_value(xr_typed_get(a->data, (int32_t) i, a->elem_type), sb, depth + 1);
            }
            if (len > limit) {
                char more[48];
                int n = snprintf(more, sizeof(more), ", ...(%lld more)", (long long) (len - limit));
                xrt_fmt_puts(sb, more, (size_t) n);
            }
            xrt_fmt_char(sb, ']');
            return;
        }
        case XR_TAG_TUPLE: {
            xrt_tuple_t *t = (xrt_tuple_t *) v.ptr;
            int64_t len = t ? t->len : 0;
            int64_t limit = len > XRT_FORMAT_MAX_ELEMENTS ? XRT_FORMAT_MAX_ELEMENTS : len;
            xrt_fmt_char(sb, '(');
            for (int64_t i = 0; i < limit; i++) {
                if (i > 0)
                    xrt_fmt_cstr(sb, ", ");
                xrt_format_value(t->items[i], sb, depth + 1);
            }
            if (len > limit) {
                char more[48];
                int n = snprintf(more, sizeof(more), ", ...(%lld more)", (long long) (len - limit));
                xrt_fmt_puts(sb, more, (size_t) n);
            }
            if (len == 1)
                xrt_fmt_char(sb, ',');
            xrt_fmt_char(sb, ')');
            return;
        }
        case XR_TAG_MAP: {
            xrt_map_t *m = (xrt_map_t *) v.ptr;
            int64_t total = m ? xrt_map_len(m) : 0;
            int64_t n_slots = !m ? 0 : (xrt_map_is_typed(m) ? m->order_len : (int64_t) m->nentries);
            int64_t count = 0;
            xrt_fmt_cstr(sb, "#{");
            for (int64_t i = 0; i < n_slots && count < XRT_FORMAT_MAX_ELEMENTS; i++) {
                int64_t slot = xrt_map_is_typed(m) ? m->order[i] : i;
                if (!xrt_map_slot_is_full(m, slot))
                    continue;
                if (count > 0)
                    xrt_fmt_cstr(sb, ", ");
                xrt_format_value(xrt_map_slot_key(m, slot), sb, depth + 1);
                xrt_fmt_cstr(sb, ": ");
                xrt_format_value(xrt_map_slot_value(m, slot), sb, depth + 1);
                count++;
            }
            if (total > count) {
                char more[48];
                int n =
                    snprintf(more, sizeof(more), ", ...(%lld more)", (long long) (total - count));
                xrt_fmt_puts(sb, more, (size_t) n);
            }
            xrt_fmt_char(sb, '}');
            return;
        }
        case XR_TAG_SET: {
            xrt_set_t *s = (xrt_set_t *) v.ptr;
            int64_t total = s ? xrt_set_len(s) : 0;
            int64_t n_slots = !s ? 0 : (xrt_set_is_typed(s) ? s->order_len : (int64_t) s->nentries);
            int64_t count = 0;
            xrt_fmt_cstr(sb, "#[");
            for (int64_t i = 0; i < n_slots && count < XRT_FORMAT_MAX_ELEMENTS; i++) {
                int64_t slot = xrt_set_is_typed(s) ? s->order[i] : i;
                if (!xrt_set_slot_is_full(s, slot))
                    continue;
                if (count > 0)
                    xrt_fmt_cstr(sb, ", ");
                xrt_format_value(xrt_set_slot_item(s, slot), sb, depth + 1);
                count++;
            }
            if (total > count) {
                char more[48];
                int n =
                    snprintf(more, sizeof(more), ", ...(%lld more)", (long long) (total - count));
                xrt_fmt_puts(sb, more, (size_t) n);
            }
            xrt_fmt_char(sb, ']');
            return;
        }
        case XR_TAG_PTR: {
            if (xrt_is_struct_object_value(v)) {
                xrt_object_t *j = (xrt_object_t *) v.ptr;
                int64_t emitted = 0;
                int64_t total = 0;
                xrt_fmt_char(sb, '{');
                for (int64_t i = 0; i < xrt_object_field_count(j); i++) {
                    const char *name = xrt_object_field_name(j, i);
                    if (!name)
                        continue;
                    total++;
                    if (emitted >= XRT_FORMAT_MAX_ELEMENTS)
                        continue;
                    if (emitted > 0)
                        xrt_fmt_cstr(sb, ", ");
                    xrt_fmt_cstr(sb, name);
                    xrt_fmt_cstr(sb, ": ");
                    xrt_format_value(j->fields[i], sb, depth + 1);
                    emitted++;
                }
                if (total > emitted) {
                    char more[48];
                    int n = snprintf(more, sizeof(more), ", ...(%lld more)",
                                     (long long) (total - emitted));
                    xrt_fmt_puts(sb, more, (size_t) n);
                }
                xrt_fmt_char(sb, '}');
                return;
            }
            if (v.heap_type != XR_TINSTANCE || !v.ptr)
                break;
            XrObjHeader *hdr = (XrObjHeader *) v.ptr;
            const XrtTypeInfo *ti = xrt_type_info(xrt_aot_class_type_id(hdr));
            const char *type_name = ti ? xrt_type_display_name(ti->type_id) : "<object>";
            const XrtTypeDeriveInfo *di = ti ? xrt_type_derive_info(ti->type_id) : NULL;
            xrt_fmt_cstr(sb, type_name ? type_name : "<object>");
            if (!di || (di->derive_flags & XR_DERIVE_INSPECT) == 0 ||
                (di->inspect_field_count > 0 && !di->inspect_fields)) {
                xrt_fmt_cstr(sb, "{...}");
                return;
            }
            xrt_fmt_char(sb, '{');
            for (uint16_t i = 0; i < di->inspect_field_count; i++) {
                const XrtInspectField *field = &di->inspect_fields[i];
                if (i > 0)
                    xrt_fmt_cstr(sb, ", ");
                xrt_fmt_cstr(sb, field->name ? field->name : "?");
                xrt_fmt_cstr(sb, ": ");
                xrt_format_value(xrt_inspect_field_value(v.ptr, field), sb, depth + 1);
            }
            xrt_fmt_char(sb, '}');
            return;
        }
        default: {
            char buf[32];
            int n = snprintf(buf, sizeof(buf), "<object@%p>", v.ptr);
            xrt_fmt_puts(sb, buf, (size_t) n);
            return;
        }
    }
}

static void xrt_print_value(XrValue v, int depth) {
    xrt_format_value(v, NULL, depth);
}

/* True when xrt_format_value renders v structurally rather than falling through
 * to its "<object@%p>" placeholder. Callers that must not emit a raw address
 * (string(x), x.toString()) gate on this. */
static inline int xrt_value_kind_is_formattable_aggregate(XrValue v) {
    switch (xrt_value_kind(v)) {
        case XR_TAG_ARRAY:
        case XR_TAG_MAP:
        case XR_TAG_SET:
        case XR_TAG_TUPLE:
        case XR_TAG_CLOSURE:
            return 1;
        case XR_TAG_PTR:
            return v.ptr && (xrt_is_struct_object_value(v) || v.heap_type == XR_TINSTANCE);
        default:
            return 0;
    }
}

/* value.toString() for containers and other non-scalars: render via the shared
 * formatter into a string (matches the VM's xr_value_to_string output). */
static inline XrValue xrt_value_to_string(XrValue v) {
    if (XR_IS_STR(v))
        return v;
    XrValue sbv = xrt_strbuf_new();
    xrt_format_value(v, (xrt_strbuf_t *) sbv.ptr, 0);
    return xrt_strbuf_finish(sbv);
}

/* =========================================================================
 * String concatenation parts
 *
 * One stack-local part per operand of a lowered concatenation; the common
 * shapes (strings, scalars, zero-payload enum names) borrow existing memory
 * or format into the inline scratch, so the whole concat performs a single
 * allocation. Payload-bearing enum parts are the exception: their text runs
 * through the shared value formatter and is owned by the part until
 * xrt_str_concat_parts copies it out.
 * ========================================================================= */

typedef struct {
    const char *a;
    const char *b;
    size_t alen;
    size_t blen;
    /* Owned rendered text for XRT_STRPART_OWNED_STR parts (payload-bearing
     * enums). Written only when kind says so; xrt_str_concat_parts releases
     * it after copying, so parts must always reach that call. */
    XrValue owned;
    char scratch[64];
    uint8_t kind;
} xrt_strpart_t;

#define XRT_STRPART_SINGLE 0u
#define XRT_STRPART_ENUM 1u
#define XRT_STRPART_OWNED_STR 2u

static inline size_t xrt_format_uint64(char *buf, size_t cap, uint64_t value) {
    int n = snprintf(buf, cap, "%llu", (unsigned long long) value);
    if (n < 0)
        return 0;
    if ((size_t) n >= cap)
        return cap ? cap - 1u : 0u;
    return (size_t) n;
}

static inline XrValue xrt_uint64_to_string(uint64_t value) {
    char scratch[32];
    size_t len = xrt_format_uint64(scratch, sizeof(scratch), value);
    XrValue out = xrt_str_alloc(len);
    memcpy(xr_str_buf(out), scratch, len);
    return out;
}

static inline void xrt_strpart_init_u64(xrt_strpart_t *part, uint64_t value) {
    part->a = "";
    part->b = NULL;
    part->alen = 0;
    part->blen = 0;
    part->kind = XRT_STRPART_SINGLE;
    part->alen = xrt_format_uint64(part->scratch, sizeof(part->scratch), value);
    part->a = part->scratch;
}

static inline void xrt_strpart_init(xrt_strpart_t *part, XrValue val) {
    part->a = "";
    part->b = NULL;
    part->alen = 0;
    part->blen = 0;
    part->kind = XRT_STRPART_SINGLE;

    if (val.tag == XR_TAG_STR || val.tag == XR_TAG_STR_ARC) {
        part->a = xr_str_data(val);
        part->alen = (size_t) xr_str_len(val);
    } else if (val.tag == XR_TAG_I64) {
        int n = snprintf(part->scratch, sizeof(part->scratch), "%lld", (long long) val.i);
        part->a = part->scratch;
        part->alen = (size_t) (n < 0 ? 0 : n);
    } else if (val.tag == XR_TAG_F64) {
        int n = xr_format_float(part->scratch, sizeof(part->scratch), val.f);
        part->a = part->scratch;
        part->alen = (size_t) (n < 0 ? 0 : n);
    } else if (val.tag == XR_TAG_BOOL) {
        part->a = val.i ? "true" : "false";
        part->alen = val.i ? 4u : 5u;
    } else if (val.tag == XR_TAG_RUNE) {
        uint32_t cp = XR_TO_RUNE(val);
        int n = 0;
        if (cp <= 0x7Fu) {
            part->scratch[n++] = (char) cp;
        } else if (cp <= 0x7FFu) {
            part->scratch[n++] = (char) (0xC0u | (cp >> 6));
            part->scratch[n++] = (char) (0x80u | (cp & 0x3Fu));
        } else if (cp <= 0xFFFFu && !(cp >= 0xD800u && cp <= 0xDFFFu)) {
            part->scratch[n++] = (char) (0xE0u | (cp >> 12));
            part->scratch[n++] = (char) (0x80u | ((cp >> 6) & 0x3Fu));
            part->scratch[n++] = (char) (0x80u | (cp & 0x3Fu));
        } else if (cp <= 0x10FFFFu) {
            part->scratch[n++] = (char) (0xF0u | (cp >> 18));
            part->scratch[n++] = (char) (0x80u | ((cp >> 12) & 0x3Fu));
            part->scratch[n++] = (char) (0x80u | ((cp >> 6) & 0x3Fu));
            part->scratch[n++] = (char) (0x80u | (cp & 0x3Fu));
        }
        part->a = part->scratch;
        part->alen = (size_t) n;
    } else if (val.tag == XR_TAG_NULL) {
        part->a = "null";
        part->alen = 4u;
    } else if (val.tag == XR_TAG_ENUM) {
        const XrAotEnumBox *box = xrt_enum_box_view(val);
        const char *enum_name = NULL;
        const char *member_name = NULL;
        if (box && box->payload_count > 0) {
            /* Payload variants render "Enum.Member(p1, ...)" through the
             * shared formatter so concatenation matches print and the VM.
             * Payloads may nest strings or further enums, so this text has
             * no scratch-size bound and must be heap-rendered. */
            part->owned = xrt_value_to_string(val);
            part->a = xr_str_data(part->owned);
            part->alen = (size_t) xr_str_len(part->owned);
            part->kind = XRT_STRPART_OWNED_STR;
        } else if (xrt_enum_key_parts(val, &enum_name, &member_name, NULL, NULL) && enum_name &&
                   member_name) {
            part->a = enum_name;
            part->b = member_name;
            part->alen = strlen(enum_name);
            part->blen = strlen(member_name);
            part->kind = XRT_STRPART_ENUM;
        } else {
            int n = snprintf(part->scratch, sizeof(part->scratch), "<enum@%p>", val.ptr);
            part->a = part->scratch;
            part->alen = (size_t) (n < 0 ? 0 : n);
        }
    } else {
        const char *s = xr_to_cstr(val, part->scratch, sizeof(part->scratch));
        part->a = s;
        part->alen = strlen(s);
    }
}

static inline size_t xrt_strpart_len(const xrt_strpart_t *part) {
    return part->kind == XRT_STRPART_ENUM ? part->alen + 1u + part->blen : part->alen;
}

static inline char *xrt_strpart_copy(char *dst, const xrt_strpart_t *part) {
    memcpy(dst, part->a, part->alen);
    dst += part->alen;
    if (part->kind == XRT_STRPART_ENUM) {
        *dst++ = '.';
        memcpy(dst, part->b, part->blen);
        dst += part->blen;
    }
    return dst;
}

static inline XrValue xrt_str_concat_parts(size_t count, xrt_strpart_t *parts) {
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        size_t len = xrt_strpart_len(&parts[i]);
        if (XR_UNLIKELY(len > SIZE_MAX - total)) {
            fprintf(stderr, "xrt_str_concat_parts: string length overflow\n");
            abort();
        }
        total += len;
    }
    XrValue out = xrt_str_alloc(total);
    char *dst = xr_str_buf(out);
    for (size_t i = 0; i < count; i++) {
        dst = xrt_strpart_copy(dst, &parts[i]);
        if (parts[i].kind == XRT_STRPART_OWNED_STR)
            xrt_release(parts[i].owned);
    }
    *dst = 0;
    return out;
}

static inline void xrt_print(XrValue v) {
    xrt_print_value(v, 0);
}

static inline void xrt_println(XrValue v) {
    xrt_print(v);
    printf("\n");
}

/* typeof(x) — return integer type ID matching VM XrTypeId.
 * XR_TID_INT=8, XR_TID_FLOAT=11, XR_TID_BOOL=1, XR_TID_NULL=0,
 * XR_TID_STRING=12, XR_TID_FUNCTION=13, XR_TID_ARRAY=14, XR_TID_SET=15,
 * XR_TID_MAP=16. */
#define XRT_TYPE_IDENTITY_ASSERT_PUBLIC_ID(core_id, public_id, numeric_id)         \
    _Static_assert((unsigned) (core_id) == (numeric_id),                           \
                   "AOT type identity core id drifted");                          \
    _Static_assert((unsigned) (public_id) == (numeric_id),                         \
                   "AOT public type id drifted")

XRT_TYPE_IDENTITY_ASSERT_PUBLIC_ID(XR_TYPE_IDENTITY_CORE_NULL, XR_TID_NULL, 0u);
XRT_TYPE_IDENTITY_ASSERT_PUBLIC_ID(XR_TYPE_IDENTITY_CORE_INT, XR_TID_INT, 8u);
XRT_TYPE_IDENTITY_ASSERT_PUBLIC_ID(XR_TYPE_IDENTITY_CORE_FLOAT, XR_TID_FLOAT, 11u);
XRT_TYPE_IDENTITY_ASSERT_PUBLIC_ID(XR_TYPE_IDENTITY_CORE_BUFFER, XR_TID_BUFFER, 42u);
XRT_TYPE_IDENTITY_ASSERT_PUBLIC_ID(XR_TYPE_IDENTITY_CORE_RUNE, XR_TID_RUNE, 43u);

#undef XRT_TYPE_IDENTITY_ASSERT_PUBLIC_ID

static inline XrTypeIdentityCoreKind xrt_type_identity_kind(XrValue v) {
    switch (xrt_value_kind(v)) {
        case XR_TAG_I64:
            return XR_TYPE_IDENTITY_CORE_INT;
        case XR_TAG_F64:
            return XR_TYPE_IDENTITY_CORE_FLOAT;
        case XR_TAG_BOOL:
            return XR_TYPE_IDENTITY_CORE_BOOL;
        case XR_TAG_RUNE:
            return XR_TYPE_IDENTITY_CORE_RUNE;
        case XR_TAG_NULL:
            return XR_TYPE_IDENTITY_CORE_NULL;
        case XR_TAG_STR:
        case XR_TAG_STR_ARC:
            return XR_TYPE_IDENTITY_CORE_STRING;
        case XR_TAG_ARRAY:
            return XR_TYPE_IDENTITY_CORE_ARRAY;
        case XR_TAG_SET:
            return XR_TYPE_IDENTITY_CORE_SET;
        case XR_TAG_MAP:
            return XR_TYPE_IDENTITY_CORE_MAP;
        case XR_TAG_PTR:
            if (v.ptr && v.heap_type == 0)
                return XR_TYPE_IDENTITY_CORE_OBJECT;
            return XR_TYPE_IDENTITY_CORE_INSTANCE;
        case XR_TAG_CLOSURE:
            return XR_TYPE_IDENTITY_CORE_FUNCTION;
        case XR_TAG_STRBUF:
            return XR_TYPE_IDENTITY_CORE_STRINGBUILDER;
        case XR_TAG_RANGE:
            return XR_TYPE_IDENTITY_CORE_RANGE;
        case XR_TAG_ENUM:
            return XR_TYPE_IDENTITY_CORE_ENUM_VALUE;
        case XR_TAG_BIGINT:
            return XR_TYPE_IDENTITY_CORE_BIGINT;
        case XR_TAG_NET_CONN:
            return XR_TYPE_IDENTITY_CORE_NETCONN;
        case XR_TAG_NET_LISTENER:
            return XR_TYPE_IDENTITY_CORE_NETLISTENER;
        default:
            return XR_TYPE_IDENTITY_CORE_INSTANCE;
    }
}

static inline int64_t xrt_typeof_id(XrValue v) {
    return (int64_t) xr_type_identity_core_eval(
        XR_SEM_OWNER_ID_PRIMITIVE_TYPE_IDENTITY_HI,
        XR_SEM_OWNER_ID_PRIMITIVE_TYPE_IDENTITY_LO, XR_SEM_CONSUMER_AOT_HOSTED,
        xrt_type_identity_kind(v));
}

/* The `is T` / checked-cast predicate, matching the VM's xr_value_is_type_id.
 * A dynamically erased value carries no width, so a fixed-width numeric id is
 * answered by exact representability rather than a type-id compare — the id
 * side alone would report `int` for every integer and `float` for every
 * floating value. */
/* Membership in the JSON.Value domain -- the AOT mirror of the VM's
 * xr_value_in_json_domain. The scalar forms are self-describing; the two
 * composite forms use the canonical Map and JSON-element Array storage. */
static inline int xrt_value_in_json_domain(XrValue v) {
    int64_t tid = xrt_typeof_id(v);
    switch (tid) {
        case XR_TID_NULL:
        case XR_TID_BOOL:
        case XR_TID_STRING:
            return 1;
        case XR_TID_MAP:
            return v.ptr != NULL;
        case XR_TID_ARRAY:
            return v.ptr && ((const xrt_array_t *) v.ptr)->elem_tid == XR_TID_JSON;
        default:
            return XR_TID_IS_NUMBER(tid);
    }
}

static inline int xrt_value_is_type_id(XrValue v, int64_t tid) {
    /* Json names a domain, not a tag, so membership cannot be a tag compare. */
    if (tid == XR_TID_JSON)
        return xrt_value_in_json_domain(v);
    uint8_t rep = xr_typeid_scalar_rep((XrTypeId) tid);
    if (rep != XR_SCALAR_REP_NONE) {
        if (xr_scalar_rep_is_integer(rep))
            return v.tag == XR_TAG_I64 && xr_scalar_rep_holds_i64(rep, v.i);
        return v.tag == XR_TAG_F64 && xr_scalar_rep_holds_f64(rep, v.f);
    }
    return xrt_typeof_id(v) == tid;
}

/* typename(x) — return the debug/logging type name as a string value. */
static inline XrValue xrt_typename(XrValue v) {
    XRT_STR_LIT_DEF(xs_int, "int");
    XRT_STR_LIT_DEF(xs_float, "float");
    XRT_STR_LIT_DEF(xs_bool, "bool");
    XRT_STR_LIT_DEF(xs_rune, "char");
    XRT_STR_LIT_DEF(xs_null, "null");
    XRT_STR_LIT_DEF(xs_string, "string");
    XRT_STR_LIT_DEF(xs_array, "Array");
    XRT_STR_LIT_DEF(xs_set, "Set");
    XRT_STR_LIT_DEF(xs_map, "Map");
    XRT_STR_LIT_DEF(xs_function, "function");
    XRT_STR_LIT_DEF(xs_strbuf, "StringBuilder");
    XRT_STR_LIT_DEF(xs_tuple, "tuple");
    XRT_STR_LIT_DEF(xs_range, "Range");
    XRT_STR_LIT_DEF(xs_bigint, "BigInt");
    XRT_STR_LIT_DEF(xs_net_conn, "NetConn");
    XRT_STR_LIT_DEF(xs_net_listener, "NetListener");
    XRT_STR_LIT_DEF(xs_object, "object");
    switch (xrt_value_kind(v)) {
        case XR_TAG_I64:
            return xr_str_lit(&xs_int);
        case XR_TAG_F64:
            return xr_str_lit(&xs_float);
        case XR_TAG_BOOL:
            return xr_str_lit(&xs_bool);
        case XR_TAG_RUNE:
            return xr_str_lit(&xs_rune);
        case XR_TAG_NULL:
            return xr_str_lit(&xs_null);
        case XR_TAG_STR:
        case XR_TAG_STR_ARC:
            return xr_str_lit(&xs_string);
        case XR_TAG_ARRAY:
            return xr_str_lit(&xs_array);
        case XR_TAG_SET:
            return xr_str_lit(&xs_set);
        case XR_TAG_MAP: {
            const xrt_map_t *m = (const xrt_map_t *) v.ptr;
            if (m && m->class_name)
                return xr_box_str(m->class_name);
            return xr_str_lit(&xs_map);
        }
        case XR_TAG_CLOSURE:
            return xr_str_lit(&xs_function);
        case XR_TAG_STRBUF:
            return xr_str_lit(&xs_strbuf);
        case XR_TAG_TUPLE:
            return xr_str_lit(&xs_tuple);
        case XR_TAG_RANGE:
            return xr_str_lit(&xs_range);
        case XR_TAG_ENUM: {
            const char *enum_name = NULL;
            return xrt_enum_key_parts(v, &enum_name, NULL, NULL, NULL) && enum_name
                       ? xr_box_str(enum_name)
                       : xr_str_lit(&xs_object);
        }
        case XR_TAG_BIGINT:
            return xr_str_lit(&xs_bigint);
        case XR_TAG_NET_CONN:
            return xr_str_lit(&xs_net_conn);
        case XR_TAG_NET_LISTENER:
            return xr_str_lit(&xs_net_listener);
        case XR_TAG_PTR:
            if (v.ptr && v.heap_type == 0) {
                return xr_str_lit(&xs_object);
            }
            if (v.ptr && v.heap_type == XR_TINSTANCE) {
                XrObjHeader *hdr = (XrObjHeader *) v.ptr;
                const char *name = hdr ? xrt_type_display_name(xrt_aot_class_type_id(hdr)) : NULL;
                return name ? xr_box_str(name) : xr_str_lit(&xs_object);
            }
            return xr_str_lit(&xs_object);
        default:
            return xr_str_lit(&xs_object);
    }
}

#endif  // XRT_ARITH_H
