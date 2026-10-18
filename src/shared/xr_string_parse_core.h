/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_string_parse_core.h - Strict decimal string->number parsing shared by the
 * VM, hosted AOT and freestanding AOT.
 *
 * Two properties make this a separate header instead of a section of
 * xr_string_core.h:
 *
 *   1. It is libc-free. The freestanding profile links with -nostdlib and may
 *      only import xr_hook_panic / xr_hook_write / mem* intrinsics, so strtoll,
 *      strtod and malloc are all unavailable there. Every profile therefore
 *      runs this same hand-rolled decimal parser, which is what makes the
 *      accept/reject decision identical on all three rather than "whatever the
 *      host libc happened to do".
 *
 *   2. It is strict. The accepted grammar is the whole string:
 *
 *        int:   ws* [+-]? digit+ ws*
 *        float: ws* [+-]? ( digit* ('.' digit*)? ) ( [eE] [+-]? digit+ )? ws*
 *               with at least one integer-or-fraction digit
 *
 *      Any trailing residue rejects the input, so "12abc" fails instead of
 *      yielding 12. This is the exact scalar parse grammar shared by every
 *      backend; it
 *      also rejects the hex-float and inf/nan spellings a libc strtod would
 *      otherwise accept behind the language's back.
 *
 * Integer results are exact and overflow is rejected rather than saturated.
 * Float results use bounded exact integer arithmetic and nearest/ties-to-even
 * rounding, including long decimal midpoints, subnormals and overflow. No host
 * floating arithmetic, locale, heap or approximate fallback selects the bits.
 * A grammatically valid magnitude that overflows binary64 yields an infinity
 * rather than a parse failure, matching the previous strtod behaviour.
 */

#ifndef XRAY_SHARED_XR_STRING_PARSE_CORE_H
#define XRAY_SHARED_XR_STRING_PARSE_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum XrNumberParseFailure {
    XR_NUMBER_PARSE_OK = 0,
    XR_NUMBER_PARSE_INVALID_SYNTAX,
    XR_NUMBER_PARSE_OUT_OF_RANGE,
} XrNumberParseFailure;

typedef struct XrStringParseIntResult {
    XrNumberParseFailure failure;
    int64_t value;
} XrStringParseIntResult;

typedef struct XrStringParseFloatResult {
    XrNumberParseFailure failure;
    double value;
} XrStringParseFloatResult;

static inline uint32_t xr_number_parse_failure_member_index(XrNumberParseFailure failure) {
    switch (failure) {
        case XR_NUMBER_PARSE_INVALID_SYNTAX:
            return 0u;
        case XR_NUMBER_PARSE_OUT_OF_RANGE:
            return 1u;
        case XR_NUMBER_PARSE_OK:
        default:
            return UINT32_MAX;
    }
}

#include "xr_decimal_float.h"

static inline bool xr_string_parse_is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static inline bool xr_string_parse_is_digit(unsigned char c) {
    return c >= '0' && c <= '9';
}

/*
 * Strict decimal integer parse over the whole [data, data+len) span.
 *
 * The accumulator runs negative so INT64_MIN stays representable, and both
 * overflow guards fire before the operation that would overflow -- the same
 * shape used by i64.parse/tryParse, so every backend rejects exactly the same
 * magnitudes.
 */
static inline XrStringParseIntResult xr_string_parse_int64(const char *data, size_t len) {
    XrStringParseIntResult out = {XR_NUMBER_PARSE_INVALID_SYNTAX, 0};
    if (!data)
        return out;

    size_t pos = 0;
    while (pos < len && xr_string_parse_is_space((unsigned char) data[pos]))
        pos++;

    bool negative = false;
    if (pos < len && (data[pos] == '+' || data[pos] == '-')) {
        negative = data[pos] == '-';
        pos++;
    }

    size_t digit_start = pos;
    int64_t limit = negative ? INT64_MIN : -INT64_MAX;
    int64_t multiply_limit = limit / 10;
    int64_t accumulated = 0;
    bool out_of_range = false;
    while (pos < len && xr_string_parse_is_digit((unsigned char) data[pos])) {
        int64_t digit = (int64_t) (data[pos] - '0');
        if (!out_of_range) {
            if (accumulated < multiply_limit) {
                out_of_range = true;
            } else {
                accumulated *= 10;
                if (accumulated < limit + digit)
                    out_of_range = true;
                else
                    accumulated -= digit;
            }
        }
        pos++;
    }
    if (pos == digit_start)
        return out;

    while (pos < len && xr_string_parse_is_space((unsigned char) data[pos]))
        pos++;
    if (pos != len)
        return out;

    if (out_of_range) {
        out.failure = XR_NUMBER_PARSE_OUT_OF_RANGE;
        return out;
    }

    out.failure = XR_NUMBER_PARSE_OK;
    out.value = negative ? accumulated : -accumulated;
    return out;
}

static inline double xr_string_parse_infinity(void) {
    /* Built from bits so no libm/HUGE_VAL dependency and no overflow warning. */
    union {
        uint64_t bits;
        double value;
    } u;
    u.bits = UINT64_C(0x7FF0000000000000);
    return u.value;
}

/* Complete runtime text grammar is received by the canonical decimal owner.
 * Runtime parsing uses no compiler-work callback. */
static inline XrStringParseFloatResult xr_string_parse_float64(const char *data, size_t len) {
    XrStringParseFloatResult out={XR_NUMBER_PARSE_INVALID_SYNTAX,0.0};
    XrDecimalWork work=xr_decimal_runtime_work();uint64_t bits=0;
    if(xr_decimal_float_text_work(&work,data,len,64,&bits)!=XR_DECIMAL_OK)return out;
    memcpy(&out.value,&bits,sizeof(bits));out.failure=XR_NUMBER_PARSE_OK;return out;
}

#endif  // XRAY_SHARED_XR_STRING_PARSE_CORE_H
