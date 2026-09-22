/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_text_kernel.h - Shared typed text kernel for canonical string and rune
 * values
 *
 * One implementation of the library-style value semantics that CoreSpec
 * assigns to `string` and `rune`: strict UTF-8 admission, Unicode scalar
 * admission, canonical display rendering, byte-lexicographic comparison,
 * concatenation sizing and typed output-group assembly.  The reference
 * evaluator, the program VM and generated pure-AOT C all consume this same
 * text, so no executor carries a second copy of these rules.
 *
 * The file is portable C11, header-only, allocation-free and self-contained:
 * generated C embeds it with the shared float formatter; guarded project includes
 * are resolved before embedding. It must not define non-static symbols outside the xr_text_
 * prefix.  Every function is a `static inline` that tolerates being unused,
 * so an executor that needs only part of the surface compiles warning-free
 * under -Wall -Wextra -Werror and /W4 /WX.
 */

#ifndef XR_TEXT_KERNEL_H
#define XR_TEXT_KERNEL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#ifndef XR_FLOAT_FMT_H
#include "../../shared/xr_float_fmt.h"
#endif

/* Generated C compiles this text in its main translation unit with every
 * warning fatal; clang reports an unused `static inline` there, so the
 * GNU-compatible spelling carries the unused attribute and MSVC keeps plain
 * C. */
#if defined(__GNUC__) || defined(__clang__)
#define XR_TEXT_KERNEL_FUNCTION static inline __attribute__((unused))
#else
#define XR_TEXT_KERNEL_FUNCTION static inline
#endif

/* Largest Unicode scalar value. */
#define XR_TEXT_RUNE_MAX UINT32_C(0x10FFFF)
/* Bytes needed by the widest canonical i64 rendering ("-9223372036854775808"). */
#define XR_TEXT_I64_DISPLAY_MAX 20u
#define XR_TEXT_U64_DISPLAY_MAX 20u
/* Bytes needed by the widest UTF-8 encoding of one scalar value. */
#define XR_TEXT_RUNE_UTF8_MAX 4u
/* Bytes needed by the widest bool rendering ("false"). */
#define XR_TEXT_BOOL_DISPLAY_MAX 5u

/* Comparison predicates in registry immediate order. */
enum {
    XR_TEXT_PREDICATE_EQ = 0,
    XR_TEXT_PREDICATE_NE = 1,
    XR_TEXT_PREDICATE_LT = 2,
    XR_TEXT_PREDICATE_LE = 3,
    XR_TEXT_PREDICATE_GT = 4,
    XR_TEXT_PREDICATE_GE = 5
};

/* Display-typed operand kinds of core.output.group. */
enum {
    XR_TEXT_DISPLAY_I64 = 1,
    XR_TEXT_DISPLAY_BOOL = 2,
    XR_TEXT_DISPLAY_STRING = 3,
    XR_TEXT_DISPLAY_RUNE = 4,
    XR_TEXT_DISPLAY_U64 = 5,
    XR_TEXT_DISPLAY_F64 = 6
};

typedef struct XrTextDisplayOperand {
    uint32_t kind;
    uint32_t rune;
    int64_t i64;
    uint64_t u64;
    double f64;
    int boolean;
    const uint8_t *bytes;
    size_t size;
} XrTextDisplayOperand;

/* A scalar value is any code point outside the UTF-16 surrogate range. */
XR_TEXT_KERNEL_FUNCTION int xr_text_rune_is_scalar(uint32_t value) {
    return value <= XR_TEXT_RUNE_MAX && (value < UINT32_C(0xD800) || value > UINT32_C(0xDFFF));
}

/* Strict UTF-8: shortest form only, no surrogates, nothing above U+10FFFF. */
XR_TEXT_KERNEL_FUNCTION int xr_text_utf8_is_valid(const uint8_t *bytes, size_t size) {
    size_t index = 0u;
    if (!bytes && size != 0u)
        return 0;
    while (index < size) {
        uint8_t lead = bytes[index];
        size_t need;
        uint32_t minimum;
        uint32_t value;
        size_t tail;
        if (lead < 0x80u) {
            ++index;
            continue;
        }
        if (lead >= 0xC2u && lead <= 0xDFu) {
            need = 1u;
            minimum = 0x80u;
            value = (uint32_t) (lead & 0x1Fu);
        } else if (lead >= 0xE0u && lead <= 0xEFu) {
            need = 2u;
            minimum = 0x800u;
            value = (uint32_t) (lead & 0x0Fu);
        } else if (lead >= 0xF0u && lead <= 0xF4u) {
            need = 3u;
            minimum = 0x10000u;
            value = (uint32_t) (lead & 0x07u);
        } else {
            return 0;
        }
        if (size - index <= need)
            return 0;
        for (tail = 1u; tail <= need; ++tail) {
            uint8_t byte = bytes[index + tail];
            if ((byte & 0xC0u) != 0x80u)
                return 0;
            value = (value << 6) | (uint32_t) (byte & 0x3Fu);
        }
        if (value < minimum || !xr_text_rune_is_scalar(value))
            return 0;
        index += need + 1u;
    }
    return 1;
}

/* The caller has already admitted strict UTF-8. Count leading bytes once
 * at construction; execution-private immutable carriers cache this value. */
XR_TEXT_KERNEL_FUNCTION size_t xr_text_scalar_count(const uint8_t *bytes, size_t size) {
    size_t count = 0u;
    for (size_t index = 0u; index < size; ++index)
        count += (bytes[index] & UINT8_C(0xC0)) != UINT8_C(0x80) ? 1u : 0u;
    return count;
}

/* Both signed and unsigned display use one magnitude conversion. A null
 * output only measures; the largest unsigned value never crosses int64_t. */
XR_TEXT_KERNEL_FUNCTION size_t xr_text_display_u64(uint64_t value, uint8_t *out) {
    uint8_t reverse[XR_TEXT_U64_DISPLAY_MAX];
    size_t count = 0u;
    size_t cursor = 0u;
    do {
        reverse[count++] = (uint8_t) ('0' + (unsigned) (value % UINT64_C(10)));
        value /= UINT64_C(10);
    } while (value != 0u);
    while (count != 0u) {
        --count;
        if (out)
            out[cursor] = reverse[count];
        ++cursor;
    }
    return cursor;
}

XR_TEXT_KERNEL_FUNCTION size_t xr_text_display_i64(int64_t value, uint8_t *out) {
    uint64_t magnitude = value < 0 ? UINT64_C(0) - (uint64_t) value : (uint64_t) value;
    size_t prefix = value < 0 ? 1u : 0u;
    if (prefix && out)
        out[0] = (uint8_t) '-';
    return prefix + xr_text_display_u64(magnitude, out ? out + prefix : NULL);
}

/* Renders `true` or `false`; returns the byte count.  A null output only
 * measures. */
XR_TEXT_KERNEL_FUNCTION size_t xr_text_display_bool(int value, uint8_t *out) {
    static const uint8_t true_text[4] = {'t', 'r', 'u', 'e'};
    static const uint8_t false_text[5] = {'f', 'a', 'l', 's', 'e'};
    size_t size = value ? sizeof(true_text) : sizeof(false_text);
    if (out)
        memcpy(out, value ? true_text : false_text, size);
    return size;
}

/* Encodes one scalar value as UTF-8; returns the byte count, or zero for a
 * value that is not a scalar value.  A null output only measures. */
XR_TEXT_KERNEL_FUNCTION size_t xr_text_encode_rune(uint32_t rune, uint8_t *out) {
    if (!xr_text_rune_is_scalar(rune))
        return 0u;
    if (rune < 0x80u) {
        if (out)
            out[0] = (uint8_t) rune;
        return 1u;
    }
    if (rune < 0x800u) {
        if (out) {
            out[0] = (uint8_t) (0xC0u | (rune >> 6));
            out[1] = (uint8_t) (0x80u | (rune & 0x3Fu));
        }
        return 2u;
    }
    if (rune < 0x10000u) {
        if (out) {
            out[0] = (uint8_t) (0xE0u | (rune >> 12));
            out[1] = (uint8_t) (0x80u | ((rune >> 6) & 0x3Fu));
            out[2] = (uint8_t) (0x80u | (rune & 0x3Fu));
        }
        return 3u;
    }
    if (out) {
        out[0] = (uint8_t) (0xF0u | (rune >> 18));
        out[1] = (uint8_t) (0x80u | ((rune >> 12) & 0x3Fu));
        out[2] = (uint8_t) (0x80u | ((rune >> 6) & 0x3Fu));
        out[3] = (uint8_t) (0x80u | (rune & 0x3Fu));
    }
    return 4u;
}

/* Unsigned byte lexicographic order, which equals Unicode scalar order for
 * valid UTF-8; a proper prefix orders first.  Returns -1, 0 or 1. */
XR_TEXT_KERNEL_FUNCTION int xr_text_compare(const uint8_t *left, size_t left_size,
                                            const uint8_t *right, size_t right_size) {
    size_t common = left_size < right_size ? left_size : right_size;
    int order = common == 0u ? 0 : memcmp(left, right, common);
    if (order != 0)
        return order < 0 ? -1 : 1;
    if (left_size == right_size)
        return 0;
    return left_size < right_size ? -1 : 1;
}

/* Maps a three-way order onto one registry comparison predicate. */
XR_TEXT_KERNEL_FUNCTION int xr_text_predicate(int order, uint32_t predicate) {
    switch (predicate) {
        case XR_TEXT_PREDICATE_EQ:
            return order == 0;
        case XR_TEXT_PREDICATE_NE:
            return order != 0;
        case XR_TEXT_PREDICATE_LT:
            return order < 0;
        case XR_TEXT_PREDICATE_LE:
            return order <= 0;
        case XR_TEXT_PREDICATE_GT:
            return order > 0;
        case XR_TEXT_PREDICATE_GE:
            return order >= 0;
        default:
            return 0;
    }
}

/* Concatenation size with overflow rejection; returns zero and sets *ok to
 * zero when the result would not fit a size_t. */
XR_TEXT_KERNEL_FUNCTION size_t xr_text_concat_size(size_t left_size, size_t right_size, int *ok) {
    if (left_size > SIZE_MAX - right_size) {
        *ok = 0;
        return 0u;
    }
    *ok = 1;
    return left_size + right_size;
}

/* Writes left followed by right into out, which must hold left_size +
 * right_size bytes. */
XR_TEXT_KERNEL_FUNCTION void xr_text_concat(const uint8_t *left, size_t left_size,
                                            const uint8_t *right, size_t right_size, uint8_t *out) {
    if (left_size != 0u)
        memcpy(out, left, left_size);
    if (right_size != 0u)
        memcpy(out + left_size, right, right_size);
}

/* Renders one display operand; returns the byte count, or zero for a kind or
 * rune that has no canonical display.  A null output only measures. */
XR_TEXT_KERNEL_FUNCTION size_t xr_text_display_operand(const XrTextDisplayOperand *operand,
                                                       uint8_t *out) {
    switch (operand->kind) {
        case XR_TEXT_DISPLAY_I64:
            return xr_text_display_i64(operand->i64, out);
        case XR_TEXT_DISPLAY_U64:
            return xr_text_display_u64(operand->u64, out);
        case XR_TEXT_DISPLAY_F64: {
            char buffer[32];
            int length = xr_format_float(buffer, sizeof(buffer), operand->f64);
            if (length <= 0 || (size_t)length >= sizeof(buffer)) return 0u;
            if (out) memcpy(out, buffer, (size_t)length);
            return (size_t)length;
        }
        case XR_TEXT_DISPLAY_BOOL:
            return xr_text_display_bool(operand->boolean, out);
        case XR_TEXT_DISPLAY_STRING:
            if (out && operand->size != 0u)
                memcpy(out, operand->bytes, operand->size);
            return operand->size;
        case XR_TEXT_DISPLAY_RUNE:
            return xr_text_encode_rune(operand->rune, out);
        default:
            return 0u;
    }
}

/* Measures one atomic output group: operands joined by one space byte and
 * terminated by one line feed.  Returns zero and sets *ok to zero for an
 * undisplayable operand or a size overflow. */
XR_TEXT_KERNEL_FUNCTION size_t xr_text_group_size(const XrTextDisplayOperand *operands,
                                                  size_t count, int *ok) {
    size_t total = 1u;
    size_t index;
    for (index = 0u; index < count; ++index) {
        size_t piece = xr_text_display_operand(&operands[index], NULL);
        int displayable = operands[index].kind == XR_TEXT_DISPLAY_STRING || piece != 0u;
        if (!displayable || piece > SIZE_MAX - total - 1u) {
            *ok = 0;
            return 0u;
        }
        total += piece + (index != 0u ? 1u : 0u);
    }
    *ok = 1;
    return total;
}

/* Renders one atomic output group into out, which must hold
 * xr_text_group_size bytes; returns the byte count written. */
XR_TEXT_KERNEL_FUNCTION size_t xr_text_group_render(const XrTextDisplayOperand *operands,
                                                    size_t count, uint8_t *out) {
    size_t cursor = 0u;
    size_t index;
    for (index = 0u; index < count; ++index) {
        if (index != 0u)
            out[cursor++] = (uint8_t) ' ';
        cursor += xr_text_display_operand(&operands[index], out + cursor);
    }
    out[cursor++] = (uint8_t) '\n';
    return cursor;
}

#endif /* XR_TEXT_KERNEL_H */
