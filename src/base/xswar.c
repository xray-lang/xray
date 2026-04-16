/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xswar.c - SWAR fast parsing utilities implementation
 *
 * KEY CONCEPT:
 *   SWAR (SIMD Within A Register) technique.
 *   Uses 64-bit registers to process 8 bytes in parallel.
 */

#include "xswar.h"
#include "xchecks.h"
#include <string.h>

// SWAR magic constants for parallel detection
#define SWAR_ZEROS    0x3030303030303030ULL  // '0' = 0x30
#define SWAR_NINES    0x3939393939393939ULL  // '9' = 0x39
#define SWAR_LOW_MASK 0x0F0F0F0F0F0F0F0FULL  // Low 4 bits mask
#define SWAR_HIGH_BIT 0x8080808080808080ULL  // High bit mask

/*
 * Multiplication constants for SWAR parallel multiplication.
 * Combines 8 bytes by weight into integer.
 *
 * Byte positions: [0]  [1]  [2]  [3]  [4]  [5]  [6]  [7]
 * Weights:        10^7 10^6 10^5 10^4 10^3 10^2 10^1 10^0
 *
 * Two steps:
 * 1. Merge adjacent byte pairs: d0*10+d1, d2*10+d3, ...
 * 2. Merge into final result
 */
#define SWAR_MUL1  0x000000FF00FF00FFULL
#define SWAR_MUL2  0x000000010000FFFFULL
#define SWAR_MUL3  0x0000000100000001ULL

/* ========== Internal Helper Functions ========== */

// Scalar: check if single char is digit
static inline bool is_digit(char c) {
    return (unsigned char)(c - '0') <= 9;
}

// Scalar: parse single digit
static inline int digit_value(char c) {
    return c - '0';
}

// Scalar: parse small digit string (< 8 digits)
static bool parse_uint_scalar(const char *s, size_t len, uint64_t *result) {
    uint64_t val = 0;
    for (size_t i = 0; i < len; i++) {
        if (!is_digit(s[i])) {
            return false;
        }
        // Overflow check
        if (val > UINT64_MAX / 10) {
            return false;
        }
        val = val * 10 + digit_value(s[i]);
    }
    *result = val;
    return true;
}

// Scalar: check if string is all digits
static bool is_digits_scalar(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (!is_digit(s[i])) {
            return false;
        }
    }
    return true;
}

// Scalar: parse hex character
static inline int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ========== SWAR Core Implementation ========== */

/*
 * Check if 8 bytes are all digits.
 * Method: (chunk - '0') and ('9' - chunk) should not produce high-bit borrow.
 */
bool xr_swar_is_8_digits(const char *s) {
    uint64_t chunk;
    memcpy(&chunk, s, 8);
    
    // If any non-digit, a or b will have high bit set in some byte
    uint64_t a = chunk - SWAR_ZEROS;
    uint64_t b = SWAR_NINES - chunk;
    
    return ((a | b) & SWAR_HIGH_BIT) == 0;
}

/*
 * Parse 8 digit characters to integer.
 * Assumes input is validated as 8 digit chars.
 * Uses simple reliable scalar implementation.
 */
uint64_t xr_swar_parse_8_digits(const char *s) {
    // Simple reliable implementation, compiler will optimize
    uint64_t val = 0;
    val = val * 10 + (s[0] - '0');
    val = val * 10 + (s[1] - '0');
    val = val * 10 + (s[2] - '0');
    val = val * 10 + (s[3] - '0');
    val = val * 10 + (s[4] - '0');
    val = val * 10 + (s[5] - '0');
    val = val * 10 + (s[6] - '0');
    val = val * 10 + (s[7] - '0');
    return val;
}

/* ========== Public API Implementation ========== */

bool xr_swar_is_digits(const char *s, size_t len) {
    if (len == 0) {
        return false;
    }
    
    size_t i = 0;
    
    // Use SWAR to batch check 8 bytes
    while (i + 8 <= len) {
        if (!xr_swar_is_8_digits(s + i)) {
            return false;
        }
        i += 8;
    }
    
    // Process remaining bytes
    return is_digits_scalar(s + i, len - i);
}

bool xr_swar_parse_uint(const char *s, size_t len, uint64_t *result) {
    if (len == 0 || len > 19) {
        return false;
    }
    
    // Small numbers: use scalar
    if (len < 8) {
        return parse_uint_scalar(s, len, result);
    }
    
    // First validate all digits
    if (!xr_swar_is_digits(s, len)) {
        return false;
    }
    
    /*
     * Large number strategy:
     * - 8 digits: direct SWAR
     * - 9-16 digits: split into 8 + rest
     * - 17-19 digits: split into 8 + 8 + rest
     */
    
    uint64_t val = 0;
    size_t pos = 0;
    
    if (len >= 16) {
        // Process first 8 digits
        val = xr_swar_parse_8_digits(s);
        pos = 8;
        
        // Process middle 8 digits
        uint64_t mid = xr_swar_parse_8_digits(s + 8);
        
        // Overflow check
        if (val > UINT64_MAX / 100000000ULL) {
            return false;
        }
        val = val * 100000000ULL + mid;
        pos = 16;
    } else if (len >= 8) {
        // Process first 8 digits
        val = xr_swar_parse_8_digits(s);
        pos = 8;
    }
    
    // Process remaining digits
    while (pos < len) {
        if (val > UINT64_MAX / 10) {
            return false;
        }
        val = val * 10 + digit_value(s[pos]);
        pos++;
    }
    
    *result = val;
    return true;
}

bool xr_swar_parse_int(const char *s, size_t len, int64_t *result) {
    if (len == 0) {
        return false;
    }
    
    bool negative = false;
    size_t start = 0;
    
    // Handle sign
    if (s[0] == '-') {
        negative = true;
        start = 1;
    } else if (s[0] == '+') {
        start = 1;
    }
    
    if (start >= len) {
        return false;
    }
    
    // Parse unsigned part
    uint64_t uval;
    if (!xr_swar_parse_uint(s + start, len - start, &uval)) {
        return false;
    }
    
    // Range check
    if (negative) {
        if (uval > (uint64_t)INT64_MAX + 1) {
            return false;
        }
        *result = -(int64_t)uval;
    } else {
        if (uval > (uint64_t)INT64_MAX) {
            return false;
        }
        *result = (int64_t)uval;
    }
    
    return true;
}

bool xr_swar_parse_hex(const char *s, size_t len, uint64_t *result) {
    if (len == 0 || len > 16) {
        return false;
    }
    
    uint64_t val = 0;
    
    for (size_t i = 0; i < len; i++) {
        int hv = hex_value(s[i]);
        if (hv < 0) {
            return false;
        }
        val = (val << 4) | hv;
    }
    
    *result = val;
    return true;
}
