/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsimd.c - SIMD batch scanning utilities implementation
 *
 * KEY CONCEPT:
 *   Multi-platform SIMD: SSE2 (128-bit), AVX2 (256-bit),
 *   NEON (ARM 128-bit), scalar fallback.
 */

#include "xsimd.h"
#include "xchecks.h"
#include <string.h>

/* ========== Platform Detection ========== */

#if defined(__AVX512F__) && defined(__AVX512BW__)
#define XR_USE_AVX512 1
#include <immintrin.h>
#elif defined(__AVX2__)
#define XR_USE_AVX2 1
#include <immintrin.h>
#elif defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#define XR_USE_SSE2 1
#include <emmintrin.h>
#elif defined(__ARM_NEON) || defined(__aarch64__)
#define XR_USE_NEON 1
#include <arm_neon.h>
#else
#define XR_USE_SCALAR 1
#endif

// SIMD vector width
#if defined(XR_USE_AVX512)
#define XR_SIMD_WIDTH 64
#elif defined(XR_USE_AVX2)
#define XR_SIMD_WIDTH 32
#elif defined(XR_USE_SSE2) || defined(XR_USE_NEON)
#define XR_SIMD_WIDTH 16
#else
#define XR_SIMD_WIDTH 8
#endif  // ========== Scalar Implementation ==========

static const char *find_char_scalar(const char *s, size_t len, char c) {
    for (size_t i = 0; i < len; i++) {
        if (s[i] == c) {
            return s + i;
        }
    }
    return s + len;
}

static const char *find_any_scalar(const char *s, size_t len, const char *chars, int nchars) {
    for (size_t i = 0; i < len; i++) {
        for (int j = 0; j < nchars; j++) {
            if (s[i] == chars[j]) {
                return s + i;
            }
        }
    }
    return s + len;
}

static const char *skip_ws_scalar(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (s[i] != ' ' && s[i] != '\t') {
            return s + i;
        }
    }
    return s + len;
}

static const char *skip_whitespace_scalar(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            return s + i;
        }
    }
    return s + len;
}

static const char *find_newline_scalar(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\n' || s[i] == '\r') {
            return s + i;
        }
    }
    return s + len;
}

static const char *find_string_end_scalar(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '"' || s[i] == '\\') {
            return s + i;
        }
    }
    return s + len;
}

static const char *find_string_end_quote_scalar(const char *s, size_t len, char quote) {
    for (size_t i = 0; i < len; i++) {
        if (s[i] == quote || s[i] == '\\') {
            return s + i;
        }
    }
    return s + len;
}

/* ========== SSE2 Implementation ========== */

#if defined(XR_USE_SSE2)

static const char *find_char_sse2(const char *s, size_t len, char c) {
    size_t i = 0;
    __m128i needle = _mm_set1_epi8(c);

    // Main loop: process 16 bytes at a time
    for (; i + 16 <= len; i += 16) {
        __m128i chunk = _mm_loadu_si128((const __m128i *) (s + i));
        __m128i cmp = _mm_cmpeq_epi8(chunk, needle);
        int mask = _mm_movemask_epi8(cmp);
        if (XR_UNLIKELY(mask)) {
            // Found match, return first position
            return s + i + __builtin_ctz(mask);
        }
    }

    // Tail processing
    for (; i < len; i++) {
        if (s[i] == c) {
            return s + i;
        }
    }

    return s + len;
}

static const char *find_any_sse2(const char *s, size_t len, const char *chars, int nchars) {
    if (nchars <= 0 || nchars > 4) {
        return find_any_scalar(s, len, chars, nchars);
    }

    size_t i = 0;
    __m128i c0 = _mm_set1_epi8(chars[0]);
    __m128i c1 = nchars > 1 ? _mm_set1_epi8(chars[1]) : c0;
    __m128i c2 = nchars > 2 ? _mm_set1_epi8(chars[2]) : c0;
    __m128i c3 = nchars > 3 ? _mm_set1_epi8(chars[3]) : c0;

    for (; i + 16 <= len; i += 16) {
        __m128i chunk = _mm_loadu_si128((const __m128i *) (s + i));
        __m128i r0 = _mm_cmpeq_epi8(chunk, c0);
        __m128i r1 = _mm_cmpeq_epi8(chunk, c1);
        __m128i r2 = _mm_cmpeq_epi8(chunk, c2);
        __m128i r3 = _mm_cmpeq_epi8(chunk, c3);

        __m128i result = _mm_or_si128(_mm_or_si128(r0, r1), _mm_or_si128(r2, r3));
        int mask = _mm_movemask_epi8(result);
        if (XR_UNLIKELY(mask)) {
            return s + i + __builtin_ctz(mask);
        }
    }

    // Tail processing
    return find_any_scalar(s + i, len - i, chars, nchars);
}

static const char *skip_ws_sse2(const char *s, size_t len) {
    size_t i = 0;
    __m128i space = _mm_set1_epi8(' ');
    __m128i tab = _mm_set1_epi8('\t');

    for (; i + 16 <= len; i += 16) {
        __m128i chunk = _mm_loadu_si128((const __m128i *) (s + i));
        __m128i is_space = _mm_cmpeq_epi8(chunk, space);
        __m128i is_tab = _mm_cmpeq_epi8(chunk, tab);
        __m128i is_ws = _mm_or_si128(is_space, is_tab);
        int mask = _mm_movemask_epi8(is_ws);

        if (XR_LIKELY(mask != 0xFFFF)) {
            // Has non-whitespace character
            int first_non_ws = __builtin_ctz(~mask);
            return s + i + first_non_ws;
        }
    }

    // Tail processing
    for (; i < len; i++) {
        if (s[i] != ' ' && s[i] != '\t') {
            return s + i;
        }
    }

    return s + len;
}

static const char *skip_whitespace_sse2(const char *s, size_t len) {
    size_t i = 0;
    __m128i space = _mm_set1_epi8(' ');
    __m128i tab = _mm_set1_epi8('\t');
    __m128i cr = _mm_set1_epi8('\r');
    __m128i lf = _mm_set1_epi8('\n');

    for (; i + 16 <= len; i += 16) {
        __m128i chunk = _mm_loadu_si128((const __m128i *) (s + i));
        __m128i is_space = _mm_cmpeq_epi8(chunk, space);
        __m128i is_tab = _mm_cmpeq_epi8(chunk, tab);
        __m128i is_cr = _mm_cmpeq_epi8(chunk, cr);
        __m128i is_lf = _mm_cmpeq_epi8(chunk, lf);
        __m128i is_ws = _mm_or_si128(_mm_or_si128(is_space, is_tab), _mm_or_si128(is_cr, is_lf));
        int mask = _mm_movemask_epi8(is_ws);

        if (XR_LIKELY(mask != 0xFFFF)) {
            int first_non_ws = __builtin_ctz(~mask);
            return s + i + first_non_ws;
        }
    }

    return skip_whitespace_scalar(s + i, len - i);
}

static const char *find_newline_sse2(const char *s, size_t len) {
    size_t i = 0;
    __m128i lf = _mm_set1_epi8('\n');
    __m128i cr = _mm_set1_epi8('\r');

    for (; i + 16 <= len; i += 16) {
        __m128i chunk = _mm_loadu_si128((const __m128i *) (s + i));
        __m128i is_lf = _mm_cmpeq_epi8(chunk, lf);
        __m128i is_cr = _mm_cmpeq_epi8(chunk, cr);
        __m128i result = _mm_or_si128(is_lf, is_cr);
        int mask = _mm_movemask_epi8(result);
        if (mask) {
            return s + i + __builtin_ctz(mask);
        }
    }

    return find_newline_scalar(s + i, len - i);
}

static const char *find_string_end_sse2(const char *s, size_t len) {
    size_t i = 0;
    __m128i quote = _mm_set1_epi8('"');
    __m128i backslash = _mm_set1_epi8('\\');

    for (; i + 16 <= len; i += 16) {
        __m128i chunk = _mm_loadu_si128((const __m128i *) (s + i));
        __m128i is_quote = _mm_cmpeq_epi8(chunk, quote);
        __m128i is_bs = _mm_cmpeq_epi8(chunk, backslash);
        __m128i result = _mm_or_si128(is_quote, is_bs);
        int mask = _mm_movemask_epi8(result);
        if (XR_UNLIKELY(mask)) {
            return s + i + __builtin_ctz(mask);
        }
    }

    return find_string_end_scalar(s + i, len - i);
}

static const char *find_string_end_quote_sse2(const char *s, size_t len, char q) {
    size_t i = 0;
    __m128i quote = _mm_set1_epi8(q);
    __m128i backslash = _mm_set1_epi8('\\');

    for (; i + 16 <= len; i += 16) {
        __m128i chunk = _mm_loadu_si128((const __m128i *) (s + i));
        __m128i is_quote = _mm_cmpeq_epi8(chunk, quote);
        __m128i is_bs = _mm_cmpeq_epi8(chunk, backslash);
        __m128i result = _mm_or_si128(is_quote, is_bs);
        int mask = _mm_movemask_epi8(result);
        if (XR_UNLIKELY(mask)) {
            return s + i + __builtin_ctz(mask);
        }
    }

    return find_string_end_quote_scalar(s + i, len - i, q);
}

#endif  // XR_USE_SSE2

/* ========== AVX-512 Implementation ========== */

#if defined(XR_USE_AVX512)

// AVX-512 find single char, 64 bytes at a time, 4x faster than SSE2
static const char *find_char_avx512(const char *s, size_t len, char c) {
    size_t i = 0;
    __m512i needle = _mm512_set1_epi8(c);

    // Main loop: process 64 bytes at a time
    for (; i + 64 <= len; i += 64) {
        __m512i chunk = _mm512_loadu_si512((const __m512i *) (s + i));
        __mmask64 mask = _mm512_cmpeq_epi8_mask(chunk, needle);
        if (XR_UNLIKELY(mask)) {
            return s + i + __builtin_ctzll(mask);
        }
    }

    // Tail processing with SSE2
    for (; i + 16 <= len; i += 16) {
        __m128i chunk = _mm_loadu_si128((const __m128i *) (s + i));
        __m128i cmp = _mm_cmpeq_epi8(chunk, _mm_set1_epi8(c));
        int mask = _mm_movemask_epi8(cmp);
        if (XR_UNLIKELY(mask)) {
            return s + i + __builtin_ctz(mask);
        }
    }

    // Scalar tail
    for (; i < len; i++) {
        if (s[i] == c)
            return s + i;
    }

    return s + len;
}

// AVX-512 find any of multiple chars
static const char *find_any_avx512(const char *s, size_t len, const char *chars, int nchars) {
    if (nchars <= 0 || nchars > 4) {
        return find_any_scalar(s, len, chars, nchars);
    }

    size_t i = 0;
    __m512i c0 = _mm512_set1_epi8(chars[0]);
    __m512i c1 = nchars > 1 ? _mm512_set1_epi8(chars[1]) : c0;
    __m512i c2 = nchars > 2 ? _mm512_set1_epi8(chars[2]) : c0;
    __m512i c3 = nchars > 3 ? _mm512_set1_epi8(chars[3]) : c0;

    // Main loop: process 64 bytes at a time
    for (; i + 64 <= len; i += 64) {
        __m512i chunk = _mm512_loadu_si512((const __m512i *) (s + i));
        __mmask64 m0 = _mm512_cmpeq_epi8_mask(chunk, c0);
        __mmask64 m1 = _mm512_cmpeq_epi8_mask(chunk, c1);
        __mmask64 m2 = _mm512_cmpeq_epi8_mask(chunk, c2);
        __mmask64 m3 = _mm512_cmpeq_epi8_mask(chunk, c3);
        __mmask64 mask = m0 | m1 | m2 | m3;
        if (XR_UNLIKELY(mask)) {
            return s + i + __builtin_ctzll(mask);
        }
    }

    // Tail processing with SSE2
    return find_any_sse2(s + i, len - i, chars, nchars);
}

// AVX-512 skip whitespace (space and tab)
static const char *skip_ws_avx512(const char *s, size_t len) {
    size_t i = 0;
    __m512i space = _mm512_set1_epi8(' ');
    __m512i tab = _mm512_set1_epi8('\t');

    for (; i + 64 <= len; i += 64) {
        __m512i chunk = _mm512_loadu_si512((const __m512i *) (s + i));
        __mmask64 is_space = _mm512_cmpeq_epi8_mask(chunk, space);
        __mmask64 is_tab = _mm512_cmpeq_epi8_mask(chunk, tab);
        __mmask64 is_ws = is_space | is_tab;

        if (XR_LIKELY(is_ws != 0xFFFFFFFFFFFFFFFFULL)) {
            // Found non-whitespace, find first
            __mmask64 non_ws = ~is_ws;
            return s + i + __builtin_ctzll(non_ws);
        }
    }

    // Tail processing with SSE2
    return skip_ws_sse2(s + i, len - i);
}

// AVX-512 skip whitespace (including newlines)
static const char *skip_whitespace_avx512(const char *s, size_t len) {
    size_t i = 0;
    __m512i space = _mm512_set1_epi8(' ');
    __m512i tab = _mm512_set1_epi8('\t');
    __m512i cr = _mm512_set1_epi8('\r');
    __m512i lf = _mm512_set1_epi8('\n');

    for (; i + 64 <= len; i += 64) {
        __m512i chunk = _mm512_loadu_si512((const __m512i *) (s + i));
        __mmask64 m_space = _mm512_cmpeq_epi8_mask(chunk, space);
        __mmask64 m_tab = _mm512_cmpeq_epi8_mask(chunk, tab);
        __mmask64 m_cr = _mm512_cmpeq_epi8_mask(chunk, cr);
        __mmask64 m_lf = _mm512_cmpeq_epi8_mask(chunk, lf);
        __mmask64 is_ws = m_space | m_tab | m_cr | m_lf;

        if (XR_LIKELY(is_ws != 0xFFFFFFFFFFFFFFFFULL)) {
            __mmask64 non_ws = ~is_ws;
            return s + i + __builtin_ctzll(non_ws);
        }
    }

    return skip_whitespace_sse2(s + i, len - i);
}

// AVX-512 find newline
static const char *find_newline_avx512(const char *s, size_t len) {
    size_t i = 0;
    __m512i lf = _mm512_set1_epi8('\n');
    __m512i cr = _mm512_set1_epi8('\r');

    for (; i + 64 <= len; i += 64) {
        __m512i chunk = _mm512_loadu_si512((const __m512i *) (s + i));
        __mmask64 m_lf = _mm512_cmpeq_epi8_mask(chunk, lf);
        __mmask64 m_cr = _mm512_cmpeq_epi8_mask(chunk, cr);
        __mmask64 mask = m_lf | m_cr;
        if (XR_UNLIKELY(mask)) {
            return s + i + __builtin_ctzll(mask);
        }
    }

    return find_newline_sse2(s + i, len - i);
}

// AVX-512 find string terminator (double quote or backslash)
static const char *find_string_end_avx512(const char *s, size_t len) {
    size_t i = 0;
    __m512i quote = _mm512_set1_epi8('"');
    __m512i backslash = _mm512_set1_epi8('\\');

    for (; i + 64 <= len; i += 64) {
        __m512i chunk = _mm512_loadu_si512((const __m512i *) (s + i));
        __mmask64 m_quote = _mm512_cmpeq_epi8_mask(chunk, quote);
        __mmask64 m_bs = _mm512_cmpeq_epi8_mask(chunk, backslash);
        __mmask64 mask = m_quote | m_bs;
        if (XR_UNLIKELY(mask)) {
            return s + i + __builtin_ctzll(mask);
        }
    }

    return find_string_end_sse2(s + i, len - i);
}

#endif  // XR_USE_AVX512

/* ========== NEON Implementation ========== */

#if defined(XR_USE_NEON)

static const char *find_char_neon(const char *s, size_t len, char c) {
    size_t i = 0;
    uint8x16_t needle = vdupq_n_u8((uint8_t) c);

    for (; i + 16 <= len; i += 16) {
        uint8x16_t chunk = vld1q_u8((const uint8_t *) (s + i));
        uint8x16_t cmp = vceqq_u8(chunk, needle);

        // Check for match
        uint64_t mask_lo = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 0);
        uint64_t mask_hi = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 1);

        if (XR_UNLIKELY(mask_lo)) {
            int pos = __builtin_ctzll(mask_lo) / 8;
            return s + i + pos;
        }
        if (XR_UNLIKELY(mask_hi)) {
            int pos = __builtin_ctzll(mask_hi) / 8 + 8;
            return s + i + pos;
        }
    }

    // Tail processing
    for (; i < len; i++) {
        if (s[i] == c) {
            return s + i;
        }
    }

    return s + len;
}

static const char *skip_ws_neon(const char *s, size_t len) {
    size_t i = 0;
    uint8x16_t space = vdupq_n_u8(' ');
    uint8x16_t tab = vdupq_n_u8('\t');

    for (; i + 16 <= len; i += 16) {
        uint8x16_t chunk = vld1q_u8((const uint8_t *) (s + i));
        uint8x16_t is_space = vceqq_u8(chunk, space);
        uint8x16_t is_tab = vceqq_u8(chunk, tab);
        uint8x16_t is_ws = vorrq_u8(is_space, is_tab);

        // Check for non-whitespace
        uint64_t mask_lo = vgetq_lane_u64(vreinterpretq_u64_u8(is_ws), 0);
        uint64_t mask_hi = vgetq_lane_u64(vreinterpretq_u64_u8(is_ws), 1);

        if (XR_LIKELY(mask_lo != 0xFFFFFFFFFFFFFFFFULL)) {
            // Low 8 bytes have non-whitespace
            for (int j = 0; j < 8; j++) {
                if (s[i + j] != ' ' && s[i + j] != '\t') {
                    return s + i + j;
                }
            }
        }
        if (XR_LIKELY(mask_hi != 0xFFFFFFFFFFFFFFFFULL)) {
            // High 8 bytes have non-whitespace
            for (int j = 8; j < 16; j++) {
                if (s[i + j] != ' ' && s[i + j] != '\t') {
                    return s + i + j;
                }
            }
        }
    }

    return skip_ws_scalar(s + i, len - i);
}

static const char *find_newline_neon(const char *s, size_t len) {
    size_t i = 0;
    uint8x16_t lf = vdupq_n_u8('\n');
    uint8x16_t cr = vdupq_n_u8('\r');

    for (; i + 16 <= len; i += 16) {
        uint8x16_t chunk = vld1q_u8((const uint8_t *) (s + i));
        uint8x16_t is_lf = vceqq_u8(chunk, lf);
        uint8x16_t is_cr = vceqq_u8(chunk, cr);
        uint8x16_t result = vorrq_u8(is_lf, is_cr);

        uint64_t mask_lo = vgetq_lane_u64(vreinterpretq_u64_u8(result), 0);
        uint64_t mask_hi = vgetq_lane_u64(vreinterpretq_u64_u8(result), 1);

        if (XR_UNLIKELY(mask_lo)) {
            int pos = __builtin_ctzll(mask_lo) / 8;
            return s + i + pos;
        }
        if (XR_UNLIKELY(mask_hi)) {
            int pos = __builtin_ctzll(mask_hi) / 8 + 8;
            return s + i + pos;
        }
    }

    return find_newline_scalar(s + i, len - i);
}

static const char *find_string_end_neon(const char *s, size_t len) {
    size_t i = 0;
    uint8x16_t quote = vdupq_n_u8('"');
    uint8x16_t backslash = vdupq_n_u8('\\');

    for (; i + 16 <= len; i += 16) {
        uint8x16_t chunk = vld1q_u8((const uint8_t *) (s + i));
        uint8x16_t is_quote = vceqq_u8(chunk, quote);
        uint8x16_t is_bs = vceqq_u8(chunk, backslash);
        uint8x16_t result = vorrq_u8(is_quote, is_bs);

        uint64_t mask_lo = vgetq_lane_u64(vreinterpretq_u64_u8(result), 0);
        uint64_t mask_hi = vgetq_lane_u64(vreinterpretq_u64_u8(result), 1);

        if (XR_UNLIKELY(mask_lo)) {
            int pos = __builtin_ctzll(mask_lo) / 8;
            return s + i + pos;
        }
        if (XR_UNLIKELY(mask_hi)) {
            int pos = __builtin_ctzll(mask_hi) / 8 + 8;
            return s + i + pos;
        }
    }

    return find_string_end_scalar(s + i, len - i);
}

static const char *find_string_end_quote_neon(const char *s, size_t len, char q) {
    size_t i = 0;
    uint8x16_t quote = vdupq_n_u8(q);
    uint8x16_t backslash = vdupq_n_u8('\\');

    for (; i + 16 <= len; i += 16) {
        uint8x16_t chunk = vld1q_u8((const uint8_t *) (s + i));
        uint8x16_t is_quote = vceqq_u8(chunk, quote);
        uint8x16_t is_bs = vceqq_u8(chunk, backslash);
        uint8x16_t result = vorrq_u8(is_quote, is_bs);

        uint64_t mask_lo = vgetq_lane_u64(vreinterpretq_u64_u8(result), 0);
        uint64_t mask_hi = vgetq_lane_u64(vreinterpretq_u64_u8(result), 1);

        if (XR_UNLIKELY(mask_lo)) {
            int pos = __builtin_ctzll(mask_lo) / 8;
            return s + i + pos;
        }
        if (XR_UNLIKELY(mask_hi)) {
            int pos = __builtin_ctzll(mask_hi) / 8 + 8;
            return s + i + pos;
        }
    }

    return find_string_end_quote_scalar(s + i, len - i, q);
}

// NEON find any of multiple chars
static const char *find_any_neon(const char *s, size_t len, const char *chars, int nchars) {
    if (nchars <= 0 || nchars > 4) {
        return find_any_scalar(s, len, chars, nchars);
    }

    size_t i = 0;
    uint8x16_t c0 = vdupq_n_u8((uint8_t) chars[0]);
    uint8x16_t c1 = nchars > 1 ? vdupq_n_u8((uint8_t) chars[1]) : c0;
    uint8x16_t c2 = nchars > 2 ? vdupq_n_u8((uint8_t) chars[2]) : c0;
    uint8x16_t c3 = nchars > 3 ? vdupq_n_u8((uint8_t) chars[3]) : c0;

    for (; i + 16 <= len; i += 16) {
        uint8x16_t chunk = vld1q_u8((const uint8_t *) (s + i));
        uint8x16_t r0 = vceqq_u8(chunk, c0);
        uint8x16_t r1 = vceqq_u8(chunk, c1);
        uint8x16_t r2 = vceqq_u8(chunk, c2);
        uint8x16_t r3 = vceqq_u8(chunk, c3);

        uint8x16_t result = vorrq_u8(vorrq_u8(r0, r1), vorrq_u8(r2, r3));

        uint64_t mask_lo = vgetq_lane_u64(vreinterpretq_u64_u8(result), 0);
        uint64_t mask_hi = vgetq_lane_u64(vreinterpretq_u64_u8(result), 1);

        if (XR_UNLIKELY(mask_lo)) {
            int pos = __builtin_ctzll(mask_lo) / 8;
            return s + i + pos;
        }
        if (XR_UNLIKELY(mask_hi)) {
            int pos = __builtin_ctzll(mask_hi) / 8 + 8;
            return s + i + pos;
        }
    }

    return find_any_scalar(s + i, len - i, chars, nchars);
}

// NEON skip whitespace (including newlines)
static const char *skip_whitespace_neon(const char *s, size_t len) {
    size_t i = 0;
    uint8x16_t space = vdupq_n_u8(' ');
    uint8x16_t tab = vdupq_n_u8('\t');
    uint8x16_t cr = vdupq_n_u8('\r');
    uint8x16_t lf = vdupq_n_u8('\n');

    for (; i + 16 <= len; i += 16) {
        uint8x16_t chunk = vld1q_u8((const uint8_t *) (s + i));
        uint8x16_t is_space = vceqq_u8(chunk, space);
        uint8x16_t is_tab = vceqq_u8(chunk, tab);
        uint8x16_t is_cr = vceqq_u8(chunk, cr);
        uint8x16_t is_lf = vceqq_u8(chunk, lf);
        uint8x16_t is_ws = vorrq_u8(vorrq_u8(is_space, is_tab), vorrq_u8(is_cr, is_lf));

        uint64_t mask_lo = vgetq_lane_u64(vreinterpretq_u64_u8(is_ws), 0);
        uint64_t mask_hi = vgetq_lane_u64(vreinterpretq_u64_u8(is_ws), 1);

        if (XR_LIKELY(mask_lo != 0xFFFFFFFFFFFFFFFFULL)) {
            for (int j = 0; j < 8; j++) {
                char c = s[i + j];
                if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                    return s + i + j;
                }
            }
        }
        if (XR_LIKELY(mask_hi != 0xFFFFFFFFFFFFFFFFULL)) {
            for (int j = 8; j < 16; j++) {
                char c = s[i + j];
                if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                    return s + i + j;
                }
            }
        }
    }

    return skip_whitespace_scalar(s + i, len - i);
}

// NEON range matching (simplified, supports up to 4 ranges)
const char *xr_simd_find_range_neon(const char *s, size_t len, const char *ranges, int range_len) {
    size_t i = 0;

    // Condition: data long enough, ranges valid, range count within SIMD support
    if (XR_LIKELY(len >= 16 && range_len >= 2 && range_len <= 8)) {
        // Build range masks
        uint8x16_t lo1 = vdupq_n_u8((uint8_t) ranges[0]);
        uint8x16_t hi1 = vdupq_n_u8((uint8_t) ranges[1]);
        uint8x16_t lo2 = (range_len >= 4) ? vdupq_n_u8((uint8_t) ranges[2]) : lo1;
        uint8x16_t hi2 = (range_len >= 4) ? vdupq_n_u8((uint8_t) ranges[3]) : hi1;
        uint8x16_t lo3 = (range_len >= 6) ? vdupq_n_u8((uint8_t) ranges[4]) : lo1;
        uint8x16_t hi3 = (range_len >= 6) ? vdupq_n_u8((uint8_t) ranges[5]) : hi1;
        uint8x16_t lo4 = (range_len >= 8) ? vdupq_n_u8((uint8_t) ranges[6]) : lo1;
        uint8x16_t hi4 = (range_len >= 8) ? vdupq_n_u8((uint8_t) ranges[7]) : hi1;

        for (; i + 16 <= len; i += 16) {
            uint8x16_t chunk = vld1q_u8((const uint8_t *) (s + i));

            // Check if in range 1
            uint8x16_t ge_lo1 = vcgeq_u8(chunk, lo1);
            uint8x16_t le_hi1 = vcleq_u8(chunk, hi1);
            uint8x16_t in_range1 = vandq_u8(ge_lo1, le_hi1);

            // Check if in range 2
            uint8x16_t ge_lo2 = vcgeq_u8(chunk, lo2);
            uint8x16_t le_hi2 = vcleq_u8(chunk, hi2);
            uint8x16_t in_range2 = vandq_u8(ge_lo2, le_hi2);

            // Check if in range 3
            uint8x16_t ge_lo3 = vcgeq_u8(chunk, lo3);
            uint8x16_t le_hi3 = vcleq_u8(chunk, hi3);
            uint8x16_t in_range3 = vandq_u8(ge_lo3, le_hi3);

            // Check if in range 4
            uint8x16_t ge_lo4 = vcgeq_u8(chunk, lo4);
            uint8x16_t le_hi4 = vcleq_u8(chunk, hi4);
            uint8x16_t in_range4 = vandq_u8(ge_lo4, le_hi4);

            // Merge results
            uint8x16_t in_any =
                vorrq_u8(vorrq_u8(in_range1, in_range2), vorrq_u8(in_range3, in_range4));

            uint64_t mask_lo = vgetq_lane_u64(vreinterpretq_u64_u8(in_any), 0);
            uint64_t mask_hi = vgetq_lane_u64(vreinterpretq_u64_u8(in_any), 1);

            if (XR_UNLIKELY(mask_lo)) {
                int pos = __builtin_ctzll(mask_lo) / 8;
                return s + i + pos;
            }
            if (XR_UNLIKELY(mask_hi)) {
                int pos = __builtin_ctzll(mask_hi) / 8 + 8;
                return s + i + pos;
            }
        }
    }

    // Scalar process remaining bytes (or all, if SIMD condition not met)
    for (; i < len; i++) {
        unsigned char c = (unsigned char) s[i];
        for (int k = 0; k < range_len; k += 2) {
            if (c >= (unsigned char) ranges[k] && c <= (unsigned char) ranges[k + 1]) {
                return s + i;
            }
        }
    }

    return s + len;
}

// NEON skip chars in range
const char *xr_simd_skip_range_neon(const char *s, size_t len, const char *ranges, int range_len) {
    size_t i = 0;

    if (XR_LIKELY(len >= 16 && range_len >= 2 && range_len <= 8)) {
        uint8x16_t lo1 = vdupq_n_u8((uint8_t) ranges[0]);
        uint8x16_t hi1 = vdupq_n_u8((uint8_t) ranges[1]);
        uint8x16_t lo2 = (range_len >= 4) ? vdupq_n_u8((uint8_t) ranges[2]) : lo1;
        uint8x16_t hi2 = (range_len >= 4) ? vdupq_n_u8((uint8_t) ranges[3]) : hi1;

        for (; i + 16 <= len; i += 16) {
            uint8x16_t chunk = vld1q_u8((const uint8_t *) (s + i));

            uint8x16_t ge_lo1 = vcgeq_u8(chunk, lo1);
            uint8x16_t le_hi1 = vcleq_u8(chunk, hi1);
            uint8x16_t in_range1 = vandq_u8(ge_lo1, le_hi1);

            uint8x16_t ge_lo2 = vcgeq_u8(chunk, lo2);
            uint8x16_t le_hi2 = vcleq_u8(chunk, hi2);
            uint8x16_t in_range2 = vandq_u8(ge_lo2, le_hi2);

            uint8x16_t in_any = vorrq_u8(in_range1, in_range2);

            // Invert: find chars not in range
            uint8x16_t not_in = vmvnq_u8(in_any);

            uint64_t mask_lo = vgetq_lane_u64(vreinterpretq_u64_u8(not_in), 0);
            uint64_t mask_hi = vgetq_lane_u64(vreinterpretq_u64_u8(not_in), 1);

            if (XR_UNLIKELY(mask_lo)) {
                int pos = __builtin_ctzll(mask_lo) / 8;
                return s + i + pos;
            }
            if (XR_UNLIKELY(mask_hi)) {
                int pos = __builtin_ctzll(mask_hi) / 8 + 8;
                return s + i + pos;
            }
        }
    }

    // Scalar process remaining bytes
    for (; i < len; i++) {
        unsigned char c = (unsigned char) s[i];
        int in_range = 0;
        for (int k = 0; k < range_len; k += 2) {
            if (c >= (unsigned char) ranges[k] && c <= (unsigned char) ranges[k + 1]) {
                in_range = 1;
                break;
            }
        }
        if (!in_range) {
            return s + i;
        }
    }

    return s + len;
}

#endif  // XR_USE_NEON

/* ========== Public API Implementation ========== */

XrSimdLevel xr_simd_detect(void) {
#if defined(XR_USE_AVX512)
    return XR_SIMD_AVX512;
#elif defined(XR_USE_AVX2)
    return XR_SIMD_AVX2;
#elif defined(XR_USE_SSE2)
    return XR_SIMD_SSE2;
#elif defined(XR_USE_NEON)
    return XR_SIMD_NEON;
#else
    return XR_SIMD_NONE;
#endif
}

const char *xr_simd_find_char(const char *s, size_t len, char c) {
    // Small data uses scalar directly
    if (len < 16) {
        return find_char_scalar(s, len, c);
    }

#if defined(XR_USE_AVX512)
    return find_char_avx512(s, len, c);
#elif defined(XR_USE_SSE2) || defined(XR_USE_AVX2)
    return find_char_sse2(s, len, c);
#elif defined(XR_USE_NEON)
    return find_char_neon(s, len, c);
#else
    return find_char_scalar(s, len, c);
#endif
}

const char *xr_simd_find_any(const char *s, size_t len, const char *chars, int nchars) {
    if (len < 16 || nchars <= 0) {
        return find_any_scalar(s, len, chars, nchars);
    }

#if defined(XR_USE_AVX512)
    return find_any_avx512(s, len, chars, nchars);
#elif defined(XR_USE_SSE2) || defined(XR_USE_AVX2)
    return find_any_sse2(s, len, chars, nchars);
#elif defined(XR_USE_NEON)
    return find_any_neon(s, len, chars, nchars);
#else
    return find_any_scalar(s, len, chars, nchars);
#endif
}

const char *xr_simd_skip_ws(const char *s, size_t len) {
    if (len < 16) {
        return skip_ws_scalar(s, len);
    }

#if defined(XR_USE_AVX512)
    return skip_ws_avx512(s, len);
#elif defined(XR_USE_SSE2) || defined(XR_USE_AVX2)
    return skip_ws_sse2(s, len);
#elif defined(XR_USE_NEON)
    return skip_ws_neon(s, len);
#else
    return skip_ws_scalar(s, len);
#endif
}

const char *xr_simd_skip_whitespace(const char *s, size_t len) {
    if (len < 16) {
        return skip_whitespace_scalar(s, len);
    }

#if defined(XR_USE_AVX512)
    return skip_whitespace_avx512(s, len);
#elif defined(XR_USE_SSE2) || defined(XR_USE_AVX2)
    return skip_whitespace_sse2(s, len);
#elif defined(XR_USE_NEON)
    return skip_whitespace_neon(s, len);
#else
    return skip_whitespace_scalar(s, len);
#endif
}

const char *xr_simd_find_newline(const char *s, size_t len) {
    if (len < 16) {
        return find_newline_scalar(s, len);
    }

#if defined(XR_USE_AVX512)
    return find_newline_avx512(s, len);
#elif defined(XR_USE_SSE2) || defined(XR_USE_AVX2)
    return find_newline_sse2(s, len);
#elif defined(XR_USE_NEON)
    return find_newline_neon(s, len);
#else
    return find_newline_scalar(s, len);
#endif
}

const char *xr_simd_find_csv_delim(const char *s, size_t len, char delim, char quote) {
    // CSV delimiters: delimiter, quote, \n, \r
    char chars[4] = {delim, quote, '\n', '\r'};
    return xr_simd_find_any(s, len, chars, 4);
}

int xr_simd_memcmp(const void *a, const void *b, size_t len) {
    // Use stdlib directly, usually well optimized
    return memcmp(a, b, len);
}

const char *xr_simd_find_string_end(const char *s, size_t len) {
    if (len < 16) {
        return find_string_end_scalar(s, len);
    }

#if defined(XR_USE_AVX512)
    return find_string_end_avx512(s, len);
#elif defined(XR_USE_SSE2) || defined(XR_USE_AVX2)
    return find_string_end_sse2(s, len);
#elif defined(XR_USE_NEON)
    return find_string_end_neon(s, len);
#else
    return find_string_end_scalar(s, len);
#endif
}

const char *xr_simd_find_string_end_quote(const char *s, size_t len, char quote) {
    if (len < 16) {
        return find_string_end_quote_scalar(s, len, quote);
    }

#if defined(XR_USE_SSE2) || defined(XR_USE_AVX2)
    return find_string_end_quote_sse2(s, len, quote);
#elif defined(XR_USE_NEON)
    return find_string_end_quote_neon(s, len, quote);
#else
    return find_string_end_quote_scalar(s, len, quote);
#endif
}

/* ========== Character Class Lookup Table ========== */

/*
 * 256-byte character class table.
 * Each byte contains flag bits for that character.
 *
 * Flag definitions:
 *   XR_CHAR_DIGIT   0x01  Digits 0-9
 *   XR_CHAR_ALPHA   0x02  Letters a-z, A-Z
 *   XR_CHAR_HEX     0x04  Hex digits 0-9, a-f, A-F
 *   XR_CHAR_WS      0x08  Whitespace: space, tab
 *   XR_CHAR_NEWLINE 0x10  Newline: \r, \n
 *   XR_CHAR_IDENT   0x20  Identifier: a-z, A-Z, 0-9, _
 *   XR_CHAR_PRINT   0x40  Printable: 0x20-0x7E
 */
const uint8_t xr_char_class[256] = {
    // 0x00-0x0F: Control chars
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x10, 0x00, 0x00, 0x10, 0x00,
    0x00,  // \t=0x09, \n=0x0A, \r=0x0D

    // 0x10-0x1F: Control chars
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    // 0x20-0x2F: Space and punctuation
    0x48, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  // space=0x20 has WS+PRINT
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,

    // 0x30-0x3F: Digits and punctuation
    0x65, 0x65, 0x65, 0x65, 0x65, 0x65, 0x65, 0x65,  // 0-7: DIGIT+HEX+IDENT+PRINT
    0x65, 0x65, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  // 8-9: same, :;<=>? only PRINT

    // 0x40-0x4F: @ and uppercase A-O
    0x40, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x62,  // @, A-F have HEX, A-O have ALPHA+IDENT+PRINT
    0x62, 0x62, 0x62, 0x62, 0x62, 0x62, 0x62, 0x62,

    // 0x50-0x5F: Uppercase P-Z and punctuation
    0x62, 0x62, 0x62, 0x62, 0x62, 0x62, 0x62, 0x62,  // P-W: ALPHA+IDENT+PRINT
    0x62, 0x62, 0x62, 0x40, 0x40, 0x40, 0x40, 0x60,  // X-Z, [\]^, _ has IDENT+PRINT

    // 0x60-0x6F: ` and lowercase a-o
    0x40, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x62,  // `, a-f have HEX, a-o have ALPHA+IDENT+PRINT
    0x62, 0x62, 0x62, 0x62, 0x62, 0x62, 0x62, 0x62,

    // 0x70-0x7F: Lowercase p-z and punctuation
    0x62, 0x62, 0x62, 0x62, 0x62, 0x62, 0x62, 0x62,  // p-w: ALPHA+IDENT+PRINT
    0x62, 0x62, 0x62, 0x40, 0x40, 0x40, 0x40, 0x00,  // x-z, {|}~, DEL=0x7F

    // 0x80-0xFF: UTF-8 lead/continuation bytes — valid in identifiers
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20};

/* ========== Hex Conversion Table ========== */

/*
 * Fast hex char to value.
 * Valid chars return 0-15, invalid return 255.
 */
const uint8_t xr_hex_to_val[256] = {
    // 0x00-0x0F
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    // 0x10-0x1F
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    // 0x20-0x2F
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    // 0x30-0x3F: 0-9
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 255, 255, 255, 255, 255, 255,
    // 0x40-0x4F: A-F
    255, 10, 11, 12, 13, 14, 15, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    // 0x50-0x5F
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    // 0x60-0x6F: a-f
    255, 10, 11, 12, 13, 14, 15, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    // 0x70-0x7F
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    // 0x80-0xFF
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};

/* ========== Digit Conversion Table ========== */

/*
 * Fast digit char to value.
 * Valid chars return 0-9, invalid return 255.
 */
const uint8_t xr_digit_to_val[256] = {
    // 0x00-0x0F
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    // 0x10-0x1F
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    // 0x20-0x2F
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    // 0x30-0x3F: 0-9
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 255, 255, 255, 255, 255, 255,
    // 0x40-0xFF: All invalid
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255};

/* ========== Predefined Range Constants ========== */

// Printable ASCII range (0x20-0x7E)
const char XR_RANGE_PRINTABLE[4] = {0x20, 0x7E, 0x00, 0x00};

// Whitespace range: space, Tab, \r, \n
const char XR_RANGE_WHITESPACE[8] = {0x09, 0x0A, 0x0D, 0x0D, 0x20, 0x20, 0x00, 0x00};

// Digit range (0-9)
const char XR_RANGE_DIGIT[2] = {'0', '9'};

// Identifier start chars range (a-z, A-Z, _)
const char XR_RANGE_IDENT_START[6] = {'A', 'Z', 'a', 'z', '_', '_'};

// Identifier chars range (a-z, A-Z, 0-9, _)
const char XR_RANGE_IDENT[8] = {'0', '9', 'A', 'Z', 'a', 'z', '_', '_'};

/* ========== SSE4.2 Range Matching ========== */

#ifdef __SSE4_2__

/*
 * Range matching using SSE4.2 pcmpestri.
 *
 * pcmpestri can check 8 ranges simultaneously (16 bytes / 2 = 8 ranges).
 * 2-3x faster than per-char comparison.
 */
const char *xr_simd_find_range_sse42(const char *s, size_t len, const char *ranges, int range_len) {
    if (XR_UNLIKELY(len < 16)) {
        goto scalar;
    }

    __m128i ranges_vec = _mm_loadu_si128((const __m128i *) ranges);
    size_t i = 0;

    // Main loop: process 16 bytes at a time
    for (; i + 16 <= len; i += 16) {
        __m128i chunk = _mm_loadu_si128((const __m128i *) (s + i));
        int idx = _mm_cmpestri(ranges_vec, range_len, chunk, 16,
                               _SIDD_UBYTE_OPS | _SIDD_CMP_RANGES | _SIDD_LEAST_SIGNIFICANT);
        if (XR_UNLIKELY(idx != 16)) {
            return s + i + idx;
        }
    }

scalar:
    // Process remaining bytes
    for (size_t j = (len >= 16) ? i : 0; j < len; j++) {
        unsigned char c = (unsigned char) s[j];
        // Check if in any range
        for (int k = 0; k < range_len; k += 2) {
            if (c >= (unsigned char) ranges[k] && c <= (unsigned char) ranges[k + 1]) {
                return s + j;
            }
        }
    }

    return s + len;
}

/*
 * Skip chars in range using SSE4.2.
 * Returns position of first char not in range.
 */
const char *xr_simd_skip_range_sse42(const char *s, size_t len, const char *ranges, int range_len) {
    if (XR_UNLIKELY(len < 16)) {
        goto scalar;
    }

    __m128i ranges_vec = _mm_loadu_si128((const __m128i *) ranges);
    size_t i = 0;

    // Main loop: process 16 bytes at a time
    for (; i + 16 <= len; i += 16) {
        __m128i chunk = _mm_loadu_si128((const __m128i *) (s + i));
        // Use NEGATIVE_POLARITY to find chars not in range
        int idx = _mm_cmpestri(ranges_vec, range_len, chunk, 16,
                               _SIDD_UBYTE_OPS | _SIDD_CMP_RANGES | _SIDD_LEAST_SIGNIFICANT |
                                   _SIDD_NEGATIVE_POLARITY);
        if (XR_UNLIKELY(idx != 16)) {
            return s + i + idx;
        }
    }

scalar:
    // Process remaining bytes
    for (size_t j = (len >= 16) ? i : 0; j < len; j++) {
        unsigned char c = (unsigned char) s[j];
        int in_range = 0;
        // Check if in any range
        for (int k = 0; k < range_len; k += 2) {
            if (c >= (unsigned char) ranges[k] && c <= (unsigned char) ranges[k + 1]) {
                in_range = 1;
                break;
            }
        }
        if (!in_range) {
            return s + j;
        }
    }

    return s + len;
}

#endif  // __SSE4_2__

/* ========== TypedArray SIMD Memset ========== */

// Scalar fallback for memset16
static void memset16_scalar(void *dst, uint16_t value, size_t count) {
    uint16_t *p = (uint16_t *) dst;
    while (count--) {
        *p++ = value;
    }
}

// Scalar fallback for memset32
static void memset32_scalar(void *dst, uint32_t value, size_t count) {
    uint32_t *p = (uint32_t *) dst;
    while (count--) {
        *p++ = value;
    }
}

// Scalar fallback for memset64
static void memset64_scalar(void *dst, uint64_t value, size_t count) {
    uint64_t *p = (uint64_t *) dst;
    while (count--) {
        *p++ = value;
    }
}

void xr_simd_memset8(void *dst, uint8_t value, size_t count) {
    // Use standard memset for 8-bit values (already highly optimized)
    memset(dst, value, count);
}

#if defined(XR_USE_AVX2)

void xr_simd_memset16(void *dst, uint16_t value, size_t count) {
    if (count < 16) {
        memset16_scalar(dst, value, count);
        return;
    }

    __m256i v = _mm256_set1_epi16(value);
    uint16_t *p = (uint16_t *) dst;

    // AVX2: 16 x 16-bit values per iteration
    while (count >= 16) {
        _mm256_storeu_si256((__m256i *) p, v);
        p += 16;
        count -= 16;
    }

    // Handle remaining
    while (count--) {
        *p++ = value;
    }
}

void xr_simd_memset32(void *dst, uint32_t value, size_t count) {
    if (count < 8) {
        memset32_scalar(dst, value, count);
        return;
    }

    __m256i v = _mm256_set1_epi32(value);
    uint32_t *p = (uint32_t *) dst;

    // AVX2: 8 x 32-bit values per iteration
    while (count >= 8) {
        _mm256_storeu_si256((__m256i *) p, v);
        p += 8;
        count -= 8;
    }

    // Handle remaining
    while (count--) {
        *p++ = value;
    }
}

void xr_simd_memset64(void *dst, uint64_t value, size_t count) {
    if (count < 4) {
        memset64_scalar(dst, value, count);
        return;
    }

    __m256i v = _mm256_set1_epi64x(value);
    uint64_t *p = (uint64_t *) dst;

    // AVX2: 4 x 64-bit values per iteration
    while (count >= 4) {
        _mm256_storeu_si256((__m256i *) p, v);
        p += 4;
        count -= 4;
    }

    // Handle remaining
    while (count--) {
        *p++ = value;
    }
}

void xr_simd_memset_f32(void *dst, float value, size_t count) {
    if (count < 8) {
        float *p = (float *) dst;
        while (count--)
            *p++ = value;
        return;
    }

    __m256 v = _mm256_set1_ps(value);
    float *p = (float *) dst;

    // AVX2: 8 x float32 per iteration
    while (count >= 8) {
        _mm256_storeu_ps(p, v);
        p += 8;
        count -= 8;
    }

    // Handle remaining
    while (count--) {
        *p++ = value;
    }
}

void xr_simd_memset_f64(void *dst, double value, size_t count) {
    if (count < 4) {
        double *p = (double *) dst;
        while (count--)
            *p++ = value;
        return;
    }

    __m256d v = _mm256_set1_pd(value);
    double *p = (double *) dst;

    // AVX2: 4 x float64 per iteration
    while (count >= 4) {
        _mm256_storeu_pd(p, v);
        p += 4;
        count -= 4;
    }

    // Handle remaining
    while (count--) {
        *p++ = value;
    }
}

#elif defined(XR_USE_SSE2)

void xr_simd_memset16(void *dst, uint16_t value, size_t count) {
    if (count < 8) {
        memset16_scalar(dst, value, count);
        return;
    }

    __m128i v = _mm_set1_epi16(value);
    uint16_t *p = (uint16_t *) dst;

    // SSE2: 8 x 16-bit values per iteration
    while (count >= 8) {
        _mm_storeu_si128((__m128i *) p, v);
        p += 8;
        count -= 8;
    }

    // Handle remaining
    while (count--) {
        *p++ = value;
    }
}

void xr_simd_memset32(void *dst, uint32_t value, size_t count) {
    if (count < 4) {
        memset32_scalar(dst, value, count);
        return;
    }

    __m128i v = _mm_set1_epi32(value);
    uint32_t *p = (uint32_t *) dst;

    // SSE2: 4 x 32-bit values per iteration
    while (count >= 4) {
        _mm_storeu_si128((__m128i *) p, v);
        p += 4;
        count -= 4;
    }

    // Handle remaining
    while (count--) {
        *p++ = value;
    }
}

void xr_simd_memset64(void *dst, uint64_t value, size_t count) {
    if (count < 2) {
        memset64_scalar(dst, value, count);
        return;
    }

    __m128i v = _mm_set1_epi64x(value);
    uint64_t *p = (uint64_t *) dst;

    // SSE2: 2 x 64-bit values per iteration
    while (count >= 2) {
        _mm_storeu_si128((__m128i *) p, v);
        p += 2;
        count -= 2;
    }

    // Handle remaining
    if (count) {
        *p = value;
    }
}

void xr_simd_memset_f32(void *dst, float value, size_t count) {
    if (count < 4) {
        float *p = (float *) dst;
        while (count--)
            *p++ = value;
        return;
    }

    __m128 v = _mm_set1_ps(value);
    float *p = (float *) dst;

    // SSE2: 4 x float32 per iteration
    while (count >= 4) {
        _mm_storeu_ps(p, v);
        p += 4;
        count -= 4;
    }

    // Handle remaining
    while (count--) {
        *p++ = value;
    }
}

void xr_simd_memset_f64(void *dst, double value, size_t count) {
    if (count < 2) {
        double *p = (double *) dst;
        while (count--)
            *p++ = value;
        return;
    }

    __m128d v = _mm_set1_pd(value);
    double *p = (double *) dst;

    // SSE2: 2 x float64 per iteration
    while (count >= 2) {
        _mm_storeu_pd(p, v);
        p += 2;
        count -= 2;
    }

    // Handle remaining
    if (count) {
        *p = value;
    }
}

#elif defined(XR_USE_NEON)

void xr_simd_memset16(void *dst, uint16_t value, size_t count) {
    if (count < 8) {
        memset16_scalar(dst, value, count);
        return;
    }

    uint16x8_t v = vdupq_n_u16(value);
    uint16_t *p = (uint16_t *) dst;

    // NEON: 8 x 16-bit values per iteration
    while (count >= 8) {
        vst1q_u16(p, v);
        p += 8;
        count -= 8;
    }

    // Handle remaining
    while (count--) {
        *p++ = value;
    }
}

void xr_simd_memset32(void *dst, uint32_t value, size_t count) {
    if (count < 4) {
        memset32_scalar(dst, value, count);
        return;
    }

    uint32x4_t v = vdupq_n_u32(value);
    uint32_t *p = (uint32_t *) dst;

    // NEON: 4 x 32-bit values per iteration
    while (count >= 4) {
        vst1q_u32(p, v);
        p += 4;
        count -= 4;
    }

    // Handle remaining
    while (count--) {
        *p++ = value;
    }
}

void xr_simd_memset64(void *dst, uint64_t value, size_t count) {
    if (count < 2) {
        memset64_scalar(dst, value, count);
        return;
    }

    uint64x2_t v = vdupq_n_u64(value);
    uint64_t *p = (uint64_t *) dst;

    // NEON: 2 x 64-bit values per iteration
    while (count >= 2) {
        vst1q_u64(p, v);
        p += 2;
        count -= 2;
    }

    // Handle remaining
    if (count) {
        *p = value;
    }
}

void xr_simd_memset_f32(void *dst, float value, size_t count) {
    if (count < 4) {
        float *p = (float *) dst;
        while (count--)
            *p++ = value;
        return;
    }

    float32x4_t v = vdupq_n_f32(value);
    float *p = (float *) dst;

    // NEON: 4 x float32 per iteration
    while (count >= 4) {
        vst1q_f32(p, v);
        p += 4;
        count -= 4;
    }

    // Handle remaining
    while (count--) {
        *p++ = value;
    }
}

void xr_simd_memset_f64(void *dst, double value, size_t count) {
    if (count < 2) {
        double *p = (double *) dst;
        while (count--)
            *p++ = value;
        return;
    }

    float64x2_t v = vdupq_n_f64(value);
    double *p = (double *) dst;

    // NEON: 2 x float64 per iteration
    while (count >= 2) {
        vst1q_f64(p, v);
        p += 2;
        count -= 2;
    }

    // Handle remaining
    if (count) {
        *p = value;
    }
}

#else  // Scalar fallback

void xr_simd_memset16(void *dst, uint16_t value, size_t count) {
    memset16_scalar(dst, value, count);
}

void xr_simd_memset32(void *dst, uint32_t value, size_t count) {
    memset32_scalar(dst, value, count);
}

void xr_simd_memset64(void *dst, uint64_t value, size_t count) {
    memset64_scalar(dst, value, count);
}

void xr_simd_memset_f32(void *dst, float value, size_t count) {
    float *p = (float *) dst;
    while (count--)
        *p++ = value;
}

void xr_simd_memset_f64(void *dst, double value, size_t count) {
    double *p = (double *) dst;
    while (count--)
        *p++ = value;
}

#endif
