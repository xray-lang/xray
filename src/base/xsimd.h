/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsimd.h - Cross-platform SIMD batch scanning utilities
 *
 * KEY CONCEPT:
 *   Multi-platform SIMD: SSE2/AVX2 (x86), NEON (ARM), scalar fallback.
 *   Small data (<16 bytes) uses scalar automatically.
 *   Unified cross-platform API with boundary safety.
 */

#ifndef XSIMD_H
#define XSIMD_H

#include "xdefs.h"

// XR_LIKELY/XR_UNLIKELY now defined in xdefs.h

/* ========== Memory Alignment ========== */

#define XR_ALIGNED(n) XR_ALIGN(n)

/* ========== Character Class Flags ========== */

#define XR_CHAR_DIGIT   0x01
#define XR_CHAR_ALPHA   0x02
#define XR_CHAR_HEX     0x04
#define XR_CHAR_WS      0x08
#define XR_CHAR_NEWLINE 0x10
#define XR_CHAR_IDENT   0x20
#define XR_CHAR_PRINT   0x40

XR_DATA const uint8_t xr_char_class[256];

/* ========== Character Class Macros (O(1) lookup) ========== */

#define XR_IS_DIGIT(c)    (xr_char_class[(unsigned char)(c)] & XR_CHAR_DIGIT)
#define XR_IS_ALPHA(c)    (xr_char_class[(unsigned char)(c)] & XR_CHAR_ALPHA)
#define XR_IS_ALNUM(c)    (xr_char_class[(unsigned char)(c)] & (XR_CHAR_DIGIT | XR_CHAR_ALPHA))
#define XR_IS_HEX(c)      (xr_char_class[(unsigned char)(c)] & XR_CHAR_HEX)
#define XR_IS_WS(c)       (xr_char_class[(unsigned char)(c)] & XR_CHAR_WS)
#define XR_IS_NEWLINE(c)  (xr_char_class[(unsigned char)(c)] & XR_CHAR_NEWLINE)
#define XR_IS_IDENT(c)    (xr_char_class[(unsigned char)(c)] & XR_CHAR_IDENT)
#define XR_IS_PRINT(c)    (xr_char_class[(unsigned char)(c)] & XR_CHAR_PRINT)
#define XR_IS_WHITESPACE(c) (xr_char_class[(unsigned char)(c)] & (XR_CHAR_WS | XR_CHAR_NEWLINE))

/* ========== Hex Conversion Table ========== */

XR_DATA const uint8_t xr_hex_to_val[256];

#define XR_HEX_TO_VAL(c) (xr_hex_to_val[(unsigned char)(c)])

/* ========== Digit Conversion Table ========== */

XR_DATA const uint8_t xr_digit_to_val[256];

#define XR_DIGIT_TO_VAL(c) (xr_digit_to_val[(unsigned char)(c)])

/* ========== Predefined Range Constants ========== */

XR_DATA const char XR_RANGE_PRINTABLE[4];
XR_DATA const char XR_RANGE_WHITESPACE[8];
XR_DATA const char XR_RANGE_DIGIT[2];
XR_DATA const char XR_RANGE_IDENT_START[6];
XR_DATA const char XR_RANGE_IDENT[8];

/* ========== Platform Detection ========== */

typedef enum {
    XR_SIMD_NONE = 0,
    XR_SIMD_SSE2 = 1,
    XR_SIMD_AVX2 = 2,
    XR_SIMD_NEON = 3,
    XR_SIMD_AVX512 = 4
} XrSimdLevel;

XR_FUNC XrSimdLevel xr_simd_detect(void);
XR_FUNC const char* xr_simd_find_char(const char *s, size_t len, char c);
XR_FUNC const char* xr_simd_find_any(const char *s, size_t len, const char *chars, int nchars);
XR_FUNC const char* xr_simd_skip_ws(const char *s, size_t len);
XR_FUNC const char* xr_simd_skip_whitespace(const char *s, size_t len);
XR_FUNC const char* xr_simd_find_newline(const char *s, size_t len);
XR_FUNC const char* xr_simd_find_csv_delim(const char *s, size_t len, char delim, char quote);
XR_FUNC int xr_simd_memcmp(const void *a, const void *b, size_t len);
XR_FUNC const char* xr_simd_find_string_end(const char *s, size_t len);
XR_FUNC const char* xr_simd_find_string_end_quote(const char *s, size_t len, char quote);

/* ========== SSE4.2 Range Matching ========== */

#ifdef __SSE4_2__
#include <nmmintrin.h>

XR_FUNC const char* xr_simd_find_range_sse42(const char *s, size_t len,
                                      const char *ranges, int range_len);
XR_FUNC const char* xr_simd_skip_range_sse42(const char *s, size_t len,
                                      const char *ranges, int range_len);
#endif // ========== ARM NEON Range Matching ==========

#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>

XR_FUNC const char* xr_simd_find_range_neon(const char *s, size_t len,
                                     const char *ranges, int range_len);
XR_FUNC const char* xr_simd_skip_range_neon(const char *s, size_t len,
                                     const char *ranges, int range_len);
#endif // ========== Cross-platform Range Matching Macros ==========

#if defined(__SSE4_2__)
    #define xr_simd_find_range(s, len, ranges, range_len) \
        xr_simd_find_range_sse42(s, len, ranges, range_len)
    #define xr_simd_skip_range(s, len, ranges, range_len) \
        xr_simd_skip_range_sse42(s, len, ranges, range_len)
#elif defined(__ARM_NEON) || defined(__aarch64__)
    #define xr_simd_find_range(s, len, ranges, range_len) \
        xr_simd_find_range_neon(s, len, ranges, range_len)
    #define xr_simd_skip_range(s, len, ranges, range_len) \
        xr_simd_skip_range_neon(s, len, ranges, range_len)
#endif // ========== TypedArray SIMD Memset ==========

// Fill memory with repeated 8-bit value (SIMD accelerated)
XR_FUNC void xr_simd_memset8(void *dst, uint8_t value, size_t count);

// Fill memory with repeated 16-bit value (SIMD accelerated)
XR_FUNC void xr_simd_memset16(void *dst, uint16_t value, size_t count);

// Fill memory with repeated 32-bit value (SIMD accelerated)
XR_FUNC void xr_simd_memset32(void *dst, uint32_t value, size_t count);

// Fill memory with repeated 64-bit value (SIMD accelerated)
XR_FUNC void xr_simd_memset64(void *dst, uint64_t value, size_t count);

// Fill memory with repeated float32 value (SIMD accelerated)
XR_FUNC void xr_simd_memset_f32(void *dst, float value, size_t count);

// Fill memory with repeated float64 value (SIMD accelerated)
XR_FUNC void xr_simd_memset_f64(void *dst, double value, size_t count);

#endif // XSIMD_H
