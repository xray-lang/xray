/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http_parser.c - High-performance zero-copy HTTP parser implementation
 *
 * KEY CONCEPT:
 *   Optimization techniques:
 *   - SSE4.2 SIMD accelerated character scanning
 *   - Zero-copy: returns pointers directly into original buffer
 *   - Lookup table for fast character classification
 *   - Complete Chunked encoding decoder
 */

#include "http_parser.h"
#include "../../src/base/xsimd.h"
#include <string.h>
#include <ctype.h>
#include <assert.h>

/* ========== SIMD Support Detection ========== */

/*
 * Uses unified SIMD support from xray_simd.h:
 * - x86-64: SSE2/SSE4.2/AVX2
 * - ARM64: NEON
 * - Others: scalar fallback
 */
#ifdef __SSE4_2__
#ifdef _MSC_VER
#include <nmmintrin.h>
#else
#include <x86intrin.h>
#endif
#define XR_HTTP_USE_SSE42 1
#endif

#ifdef __ARM_NEON
#include <arm_neon.h>
#define XR_HTTP_USE_NEON 1
#endif

// Unified SIMD enable flag
#if defined(XR_HTTP_USE_SSE42) || defined(XR_HTTP_USE_NEON)
#define XR_HTTP_USE_SIMD 1
#endif

/* ========== Compiler Hints (using macros from xray_simd.h) ========== */

#ifndef likely
#define likely(x)   XR_LIKELY(x)
#endif
#ifndef unlikely
#define unlikely(x) XR_UNLIKELY(x)
#endif

#ifndef ALIGNED
#define ALIGNED(n) XR_ALIGNED(n)
#endif

/* ========== Internal Macro Definitions ========== */

// Printable ASCII character check (0x20-0x7E)
#define IS_PRINTABLE_ASCII(c) ((unsigned char)(c) - 0x20u < 0x5Fu)

// Check buffer end
#define CHECK_EOF()                 \
    if (buf == buf_end) {           \
        *ret = -2;                  \
        return NULL;                \
    }

// Expect specific character (no EOF check)
#define EXPECT_CHAR_NO_CHECK(ch)    \
    if (*buf++ != (ch)) {           \
        *ret = -1;                  \
        return NULL;                \
    }

// Expect specific character (with EOF check)
#define EXPECT_CHAR(ch)             \
    CHECK_EOF();                    \
    EXPECT_CHAR_NO_CHECK(ch);

/* ========== Character Classification Lookup Table ========== */

/* Token character table (RFC 7230)
 * token = 1*tchar
 * tchar = "!" / "#" / "$" / "%" / "&" / "'" / "*" / "+" / "-" / "." /
 *         "^" / "_" / "`" / "|" / "~" / DIGIT / ALPHA
 */
static const char token_char_map[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 0x00-0x0F
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 0x10-0x1F
    0, 1, 0, 1, 1, 1, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0, // 0x20-0x2F: SP,!,",#,$,%,&,',(,),*,+,comma,-,.,/
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, // 0x30-0x3F: 0-9,:,;,<,=,>,?
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, // 0x40-0x4F: @,A-O
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1, // 0x50-0x5F: P-Z,[,\,],^,_
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, // 0x60-0x6F: `,a-o
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 0, // 0x70-0x7F: p-z,{,|,},~,DEL
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 0x80-0x8F
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 0x90-0x9F
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 0xA0-0xAF
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 0xB0-0xBF
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 0xC0-0xCF
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 0xD0-0xDF
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 0xE0-0xEF
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 // 0xF0-0xFF
};

/* ========== SIMD Fast Scanning ========== */

/*
 * Use unified xray_simd.h for range matching
 * Supports SSE4.2 (x86-64) and NEON (ARM64)
 * 
 * ranges: Character range array (pairs of start-end characters)
 * ranges_size: Range array size
 * found: Output, whether found
 * Returns: Pointer to found character, or scan end position
 */
static const char *findchar_fast(const char *buf, const char *buf_end, 
                                  const char *ranges, size_t ranges_size, 
                                  int *found)
{
    *found = 0;
    size_t len = buf_end - buf;
    
#if defined(XR_HTTP_USE_SSE42) || defined(XR_HTTP_USE_NEON)
    // Use unified xray_simd.h API
    if (likely(len >= 16)) {
        const char *result = xr_simd_find_range(buf, len, ranges, (int)ranges_size);
        if (result < buf_end) {
            *found = 1;
            return result;
        }
        return buf_end;
    }
#else
    (void)ranges;
    (void)ranges_size;
    (void)len;
#endif
    
    return buf;
}

/* ========== Line Parsing Helper Functions ========== */

/*
 * Get one line of content (up to CRLF or LF)
 * Returns next line start position, or NULL (incomplete or error)
 */
static const char *get_token_to_eol(const char *buf, const char *buf_end,
                                     const char **token, size_t *token_len,
                                     int *ret)
{
    const char *token_start = buf;

#ifdef XR_HTTP_USE_SIMD
    // Use SIMD to find control characters
    static const char ALIGNED(16) ranges1[16] = "\x00\x08"    // Allow HT (0x09)
                                                "\x0a\x1f"    // Allow SP and above, but not DEL
                                                "\x7f\x7f";   // Allow high-bit characters
    int found;
    buf = findchar_fast(buf, buf_end, ranges1, 6, &found);
    if (found) {
        goto FOUND_CTL;
    }
#else
    // Non-SIMD: 8-byte batch check
    while (likely(buf_end - buf >= 8)) {
#define DOIT()                                                  \
        do {                                                    \
            if (unlikely(!IS_PRINTABLE_ASCII(*buf)))            \
                goto NonPrintable;                              \
            ++buf;                                              \
        } while (0)
        DOIT(); DOIT(); DOIT(); DOIT();
        DOIT(); DOIT(); DOIT(); DOIT();
#undef DOIT
        continue;
    NonPrintable:
        if ((likely((unsigned char)*buf < 0x20) && likely(*buf != '\t')) ||
            unlikely(*buf == 0x7f)) {
            goto FOUND_CTL;
        }
        ++buf;
    }
#endif

    // Check remaining bytes one by one
    for (;; ++buf) {
        CHECK_EOF();
        if (unlikely(!IS_PRINTABLE_ASCII(*buf))) {
            if ((likely((unsigned char)*buf < 0x20) && likely(*buf != '\t')) ||
                unlikely(*buf == 0x7f)) {
                goto FOUND_CTL;
            }
        }
    }

FOUND_CTL:
    if (likely(*buf == '\r')) {
        ++buf;
        EXPECT_CHAR('\n');
        *token_len = buf - 2 - token_start;
    } else if (*buf == '\n') {
        *token_len = buf - token_start;
        ++buf;
    } else {
        *ret = -1;
        return NULL;
    }
    *token = token_start;
    return buf;
}

/*
 * Check if request/response is complete (fast detect \r\n\r\n or \n\n)
 * Used to prevent slowloris attacks
 */
static const char *is_complete(const char *buf, const char *buf_end, 
                                size_t last_len, int *ret)
{
    int ret_cnt = 0;
    buf = last_len < 3 ? buf : buf + last_len - 3;

    while (1) {
        CHECK_EOF();
        if (*buf == '\r') {
            ++buf;
            CHECK_EOF();
            EXPECT_CHAR('\n');
            ++ret_cnt;
        } else if (*buf == '\n') {
            ++buf;
            ++ret_cnt;
        } else {
            ++buf;
            ret_cnt = 0;
        }
        if (ret_cnt == 2) {
            return buf;
        }
    }

    *ret = -2;
    return NULL;
}

/* ========== Token Parsing ========== */

/*
 * Parse token (method name, header name, etc.)
 * next_char: Expected terminating character
 */
static const char *parse_token(const char *buf, const char *buf_end,
                                const char **token, size_t *token_len,
                                char next_char, int *ret)
{
#ifdef XR_HTTP_USE_SIMD
    // Use SIMD to detect non-token characters
    static const char ALIGNED(16) ranges[] = "\x00 "   // Control chars and space
                                             "\"\""    // 0x22
                                             "()"      // 0x28,0x29
                                             ",,"      // 0x2c
                                             "//"      // 0x2f
                                             ":@"      // 0x3a-0x40
                                             "[]"      // 0x5b-0x5d
                                             "{\xff";  // 0x7b-0xff
#endif
    const char *buf_start = buf;
    
#ifdef XR_HTTP_USE_SIMD
    int found;
    buf = findchar_fast(buf, buf_end, ranges, sizeof(ranges) - 1, &found);
    if (!found) {
        CHECK_EOF();
    }
#endif

    while (1) {
        if (*buf == next_char) {
            break;
        } else if (!token_char_map[(unsigned char)*buf]) {
            *ret = -1;
            return NULL;
        }
        ++buf;
        CHECK_EOF();
    }
    
    *token = buf_start;
    *token_len = buf - buf_start;
    return buf;
}

/* ========== HTTP Version Parsing ========== */

/*
 * Parse HTTP/1.x version string
 */
static const char *parse_http_version(const char *buf, const char *buf_end,
                                       int *minor_version, int *ret)
{
    // Need at least "HTTP/1.x" (8 bytes) + 1 byte following char
    if (buf_end - buf < 9) {
        *ret = -2;
        return NULL;
    }
    
    EXPECT_CHAR_NO_CHECK('H');
    EXPECT_CHAR_NO_CHECK('T');
    EXPECT_CHAR_NO_CHECK('T');
    EXPECT_CHAR_NO_CHECK('P');
    EXPECT_CHAR_NO_CHECK('/');
    EXPECT_CHAR_NO_CHECK('1');
    EXPECT_CHAR_NO_CHECK('.');
    
    if (*buf < '0' || *buf > '9') {
        *ret = -1;
        return NULL;
    }
    *minor_version = *buf++ - '0';
    
    return buf;
}

/* ========== Header Parsing ========== */

/*
 * Parse all Headers
 */
static const char *parse_headers(const char *buf, const char *buf_end,
                                  XrHttpHeader *headers, size_t *num_headers,
                                  size_t max_headers, int *ret)
{
    for (;; ++*num_headers) {
        CHECK_EOF();
        
        // Check for empty line (headers end)
        if (*buf == '\r') {
            ++buf;
            EXPECT_CHAR('\n');
            break;
        } else if (*buf == '\n') {
            ++buf;
            break;
        }
        
        // Check header count limit
        if (*num_headers == max_headers) {
            *ret = -1;
            return NULL;
        }
        
        // Check if continuation line (starts with space or tab)
        if (!(*num_headers != 0 && (*buf == ' ' || *buf == '\t'))) {
            // Parse header name
            if ((buf = parse_token(buf, buf_end, 
                                   &headers[*num_headers].name,
                                   &headers[*num_headers].name_len,
                                   ':', ret)) == NULL) {
                return NULL;
            }
            if (headers[*num_headers].name_len == 0) {
                *ret = -1;
                return NULL;
            }
            ++buf; // Skip colon
            
            // Skip optional whitespace (OWS)
            for (;; ++buf) {
                CHECK_EOF();
                if (!(*buf == ' ' || *buf == '\t')) {
                    break;
                }
            }
        } else {
            // Continuation line: no header name
            headers[*num_headers].name = NULL;
            headers[*num_headers].name_len = 0;
        }
        
        // Parse header value
        const char *value;
        size_t value_len;
        if ((buf = get_token_to_eol(buf, buf_end, &value, &value_len, ret)) == NULL) {
            return NULL;
        }
        
        // Trim trailing whitespace
        const char *value_end = value + value_len;
        for (; value_end != value; --value_end) {
            char c = *(value_end - 1);
            if (!(c == ' ' || c == '\t')) {
                break;
            }
        }
        headers[*num_headers].value = value;
        headers[*num_headers].value_len = value_end - value;
    }
    
    return buf;
}

/* ========== Request Parsing ========== */

/*
 * Parse complete request line
 */
static const char *parse_request(const char *buf, const char *buf_end,
                                  const char **method, size_t *method_len,
                                  const char **path, size_t *path_len,
                                  int *minor_version,
                                  XrHttpHeader *headers, size_t *num_headers,
                                  size_t max_headers, int *ret)
{
    // Skip first empty line (some clients add CRLF after POST content)
    CHECK_EOF();
    if (*buf == '\r') {
        ++buf;
        EXPECT_CHAR('\n');
    } else if (*buf == '\n') {
        ++buf;
    }
    
    // Parse method
    if ((buf = parse_token(buf, buf_end, method, method_len, ' ', ret)) == NULL) {
        return NULL;
    }
    
    // Skip spaces
    do {
        ++buf;
        CHECK_EOF();
    } while (*buf == ' ');
    
    // Parse path (until space)
    const char *path_start = buf;
    
#ifdef XR_HTTP_USE_SIMD
    // Use SIMD to find space or control characters
    static const char ALIGNED(16) ranges2[16] = "\x00\x20\x7f\x7f";
    int found2;
    buf = findchar_fast(buf, buf_end, ranges2, 4, &found2);
    if (!found2) {
        CHECK_EOF();
    }
#endif

    while (1) {
        if (*buf == ' ') {
            break;
        } else if (unlikely(!IS_PRINTABLE_ASCII(*buf))) {
            if ((unsigned char)*buf < 0x20 || *buf == 0x7f) {
                *ret = -1;
                return NULL;
            }
        }
        ++buf;
        CHECK_EOF();
    }
    *path = path_start;
    *path_len = buf - path_start;
    
    // Skip spaces
    do {
        ++buf;
        CHECK_EOF();
    } while (*buf == ' ');
    
    // Validate
    if (*method_len == 0 || *path_len == 0) {
        *ret = -1;
        return NULL;
    }
    
    // Parse HTTP version
    if ((buf = parse_http_version(buf, buf_end, minor_version, ret)) == NULL) {
        return NULL;
    }
    
    // Expect CRLF
    if (*buf == '\r') {
        ++buf;
        EXPECT_CHAR('\n');
    } else if (*buf == '\n') {
        ++buf;
    } else {
        *ret = -1;
        return NULL;
    }
    
    return parse_headers(buf, buf_end, headers, num_headers, max_headers, ret);
}

/* ========== Response Parsing ========== */

/*
 * Parse complete response line
 */
static const char *parse_response(const char *buf, const char *buf_end,
                                   int *minor_version, int *status,
                                   const char **msg, size_t *msg_len,
                                   XrHttpHeader *headers, size_t *num_headers,
                                   size_t max_headers, int *ret)
{
    // Parse HTTP version
    if ((buf = parse_http_version(buf, buf_end, minor_version, ret)) == NULL) {
        return NULL;
    }
    
    // Skip spaces
    if (*buf != ' ') {
        *ret = -1;
        return NULL;
    }
    do {
        ++buf;
        CHECK_EOF();
    } while (*buf == ' ');
    
    // Parse status code (need at least 3 digits + 1 char)
    if (buf_end - buf < 4) {
        *ret = -2;
        return NULL;
    }
    
    // Parse 3-digit status code
    if (*buf < '0' || *buf > '9') { *ret = -1; return NULL; }
    *status = (*buf++ - '0') * 100;
    if (*buf < '0' || *buf > '9') { *ret = -1; return NULL; }
    *status += (*buf++ - '0') * 10;
    if (*buf < '0' || *buf > '9') { *ret = -1; return NULL; }
    *status += (*buf++ - '0');
    
    // Parse status message (including leading space)
    if ((buf = get_token_to_eol(buf, buf_end, msg, msg_len, ret)) == NULL) {
        return NULL;
    }
    
    // Trim leading spaces
    if (*msg_len == 0) {
        // OK, no status message
    } else if (**msg == ' ') {
        do {
            ++*msg;
            --*msg_len;
        } while (*msg_len > 0 && **msg == ' ');
    } else {
        // Garbage after status code
        *ret = -1;
        return NULL;
    }
    
    return parse_headers(buf, buf_end, headers, num_headers, max_headers, ret);
}

/* ========== Public API ========== */

int xr_http_parse_request_ex(const char *buf_start, size_t len,
                              const char **method, size_t *method_len,
                              const char **path, size_t *path_len,
                              int *minor_ver,
                              XrHttpHeader *headers, size_t *num_headers,
                              size_t last_len)
{
    const char *buf = buf_start;
    const char *buf_end = buf_start + len;
    size_t max_headers = *num_headers;
    int r;
    
    *method = NULL;
    *method_len = 0;
    *path = NULL;
    *path_len = 0;
    *minor_ver = -1;
    *num_headers = 0;
    
    // If has history position, first check if request is complete (prevent slowloris)
    if (last_len != 0 && is_complete(buf, buf_end, last_len, &r) == NULL) {
        return r;
    }
    
    if ((buf = parse_request(buf, buf_end, method, method_len, path, path_len,
                              minor_ver, headers, num_headers, max_headers, &r)) == NULL) {
        return r;
    }
    
    return (int)(buf - buf_start);
}

int xr_http_parse_response_ex(const char *buf_start, size_t len,
                               int *minor_ver, int *status,
                               const char **msg, size_t *msg_len,
                               XrHttpHeader *headers, size_t *num_headers,
                               size_t last_len)
{
    const char *buf = buf_start;
    const char *buf_end = buf + len;
    size_t max_headers = *num_headers;
    int r;
    
    *minor_ver = -1;
    *status = 0;
    *msg = NULL;
    *msg_len = 0;
    *num_headers = 0;
    
    // If has history position, first check if response is complete
    if (last_len != 0 && is_complete(buf, buf_end, last_len, &r) == NULL) {
        return r;
    }
    
    if ((buf = parse_response(buf, buf_end, minor_ver, status, msg, msg_len,
                               headers, num_headers, max_headers, &r)) == NULL) {
        return r;
    }
    
    return (int)(buf - buf_start);
}

int xr_http_parse_headers(const char *buf_start, size_t len,
                           XrHttpHeader *headers, size_t *num_headers,
                           size_t last_len)
{
    const char *buf = buf_start;
    const char *buf_end = buf + len;
    size_t max_headers = *num_headers;
    int r;
    
    *num_headers = 0;
    
    // If has history position, first check if complete
    if (last_len != 0 && is_complete(buf, buf_end, last_len, &r) == NULL) {
        return r;
    }
    
    if ((buf = parse_headers(buf, buf_end, headers, num_headers, max_headers, &r)) == NULL) {
        return r;
    }
    
    return (int)(buf - buf_start);
}

/* ========== Chunked Decoder ========== */

// Chunked decoder states
enum {
    CHUNKED_IN_CHUNK_SIZE,
    CHUNKED_IN_CHUNK_EXT,
    CHUNKED_IN_CHUNK_HEADER_EXPECT_LF,
    CHUNKED_IN_CHUNK_DATA,
    CHUNKED_IN_CHUNK_DATA_EXPECT_CR,
    CHUNKED_IN_CHUNK_DATA_EXPECT_LF,
    CHUNKED_IN_TRAILERS_LINE_HEAD,
    CHUNKED_IN_TRAILERS_LINE_MIDDLE
};

// Parse hexadecimal character
static int decode_hex(int ch)
{
    if ('0' <= ch && ch <= '9') {
        return ch - '0';
    } else if ('A' <= ch && ch <= 'F') {
        return ch - 'A' + 0xa;
    } else if ('a' <= ch && ch <= 'f') {
        return ch - 'a' + 0xa;
    } else {
        return -1;
    }
}

ssize_t xr_http_decode_chunked(XrChunkedDecoder *decoder, char *buf, size_t *_bufsz)
{
    size_t dst = 0, src = 0, bufsz = *_bufsz;
    ssize_t ret = -2; // incomplete
    
    decoder->_total_read += bufsz;
    
    while (1) {
        switch (decoder->_state) {
        case CHUNKED_IN_CHUNK_SIZE:
            // Parse chunk size (hexadecimal)
            for (;; ++src) {
                int v;
                if (src == bufsz)
                    goto Exit;
                if ((v = decode_hex(buf[src])) == -1) {
                    if (decoder->_hex_count == 0) {
                        ret = -1;
                        goto Exit;
                    }
                    // Only BWS, semicolon, or CRLF allowed after chunk size
                    switch (buf[src]) {
                    case ' ':
                    case '\t':
                    case ';':
                    case '\n':
                    case '\r':
                        break;
                    default:
                        ret = -1;
                        goto Exit;
                    }
                    break;
                }
                if (decoder->_hex_count == (char)(sizeof(size_t) * 2)) {
                    ret = -1;
                    goto Exit;
                }
                decoder->bytes_left_in_chunk = decoder->bytes_left_in_chunk * 16 + v;
                ++decoder->_hex_count;
            }
            decoder->_hex_count = 0;
            decoder->_state = CHUNKED_IN_CHUNK_EXT;
            // fall through
            
        case CHUNKED_IN_CHUNK_EXT:
            // Skip chunk extension
            for (;; ++src) {
                if (src == bufsz)
                    goto Exit;
                if (buf[src] == '\r') {
                    break;
                } else if (buf[src] == '\n') {
                    ret = -1;
                    goto Exit;
                }
            }
            ++src;
            decoder->_state = CHUNKED_IN_CHUNK_HEADER_EXPECT_LF;
            // fall through
            
        case CHUNKED_IN_CHUNK_HEADER_EXPECT_LF:
            if (src == bufsz)
                goto Exit;
            if (buf[src] != '\n') {
                ret = -1;
                goto Exit;
            }
            ++src;
            if (decoder->bytes_left_in_chunk == 0) {
                // Last chunk (size 0)
                if (decoder->consume_trailer) {
                    decoder->_state = CHUNKED_IN_TRAILERS_LINE_HEAD;
                    break;
                } else {
                    goto Complete;
                }
            }
            decoder->_state = CHUNKED_IN_CHUNK_DATA;
            // fall through
            
        case CHUNKED_IN_CHUNK_DATA: {
            // Copy chunk data
            size_t avail = bufsz - src;
            if (avail < decoder->bytes_left_in_chunk) {
                if (dst != src)
                    memmove(buf + dst, buf + src, avail);
                src += avail;
                dst += avail;
                decoder->bytes_left_in_chunk -= avail;
                goto Exit;
            }
            if (dst != src)
                memmove(buf + dst, buf + src, decoder->bytes_left_in_chunk);
            src += decoder->bytes_left_in_chunk;
            dst += decoder->bytes_left_in_chunk;
            decoder->bytes_left_in_chunk = 0;
            decoder->_state = CHUNKED_IN_CHUNK_DATA_EXPECT_CR;
        }
            // fall through
            
        case CHUNKED_IN_CHUNK_DATA_EXPECT_CR:
            if (src == bufsz)
                goto Exit;
            if (buf[src] != '\r') {
                ret = -1;
                goto Exit;
            }
            ++src;
            decoder->_state = CHUNKED_IN_CHUNK_DATA_EXPECT_LF;
            // fall through
            
        case CHUNKED_IN_CHUNK_DATA_EXPECT_LF:
            if (src == bufsz)
                goto Exit;
            if (buf[src] != '\n') {
                ret = -1;
                goto Exit;
            }
            ++src;
            decoder->_state = CHUNKED_IN_CHUNK_SIZE;
            break;
            
        case CHUNKED_IN_TRAILERS_LINE_HEAD:
            // Parse trailer headers
            for (;; ++src) {
                if (src == bufsz)
                    goto Exit;
                if (buf[src] != '\r')
                    break;
            }
            if (buf[src++] == '\n')
                goto Complete;
            decoder->_state = CHUNKED_IN_TRAILERS_LINE_MIDDLE;
            // fall through
            
        case CHUNKED_IN_TRAILERS_LINE_MIDDLE:
            for (;; ++src) {
                if (src == bufsz)
                    goto Exit;
                if (buf[src] == '\n')
                    break;
            }
            ++src;
            decoder->_state = CHUNKED_IN_TRAILERS_LINE_HEAD;
            break;
            
        default:
            assert(!"decoder is corrupt");
        }
    }
    
Complete:
    ret = bufsz - src;
    
Exit:
    if (dst != src)
        memmove(buf + dst, buf + src, bufsz - src);
    *_bufsz = dst;
    
    // If incomplete but chunked overhead exceeds 100KB and accounts for over 80%, error
    if (ret == -2) {
        decoder->_total_overhead += bufsz - dst;
        if (decoder->_total_overhead >= 100 * 1024 &&
            decoder->_total_read - decoder->_total_overhead < decoder->_total_read / 4) {
            ret = -1;
        }
    }
    
    return ret;
}

int xr_http_chunked_is_in_data(XrChunkedDecoder *decoder)
{
    return decoder->_state == CHUNKED_IN_CHUNK_DATA;
}

/* ========== HTTP Method Parsing ========== */

XrHttpMethod xr_http_method_from_string(const char *str, size_t len)
{
    if (len < 3) return XR_HTTP_METHOD_UNKNOWN;
    
    // Fast path: common methods
    switch (str[0]) {
    case 'G':
        if (len == 3 && str[1] == 'E' && str[2] == 'T')
            return XR_HTTP_METHOD_GET;
        break;
    case 'P':
        if (len == 4 && str[1] == 'O' && str[2] == 'S' && str[3] == 'T')
            return XR_HTTP_METHOD_POST;
        if (len == 3 && str[1] == 'U' && str[2] == 'T')
            return XR_HTTP_METHOD_PUT;
        if (len == 5 && memcmp(str, "PATCH", 5) == 0)
            return XR_HTTP_METHOD_PATCH;
        break;
    case 'D':
        if (len == 6 && memcmp(str, "DELETE", 6) == 0)
            return XR_HTTP_METHOD_DELETE;
        break;
    case 'H':
        if (len == 4 && str[1] == 'E' && str[2] == 'A' && str[3] == 'D')
            return XR_HTTP_METHOD_HEAD;
        break;
    case 'O':
        if (len == 7 && memcmp(str, "OPTIONS", 7) == 0)
            return XR_HTTP_METHOD_OPTIONS;
        break;
    case 'C':
        if (len == 7 && memcmp(str, "CONNECT", 7) == 0)
            return XR_HTTP_METHOD_CONNECT;
        break;
    case 'T':
        if (len == 5 && memcmp(str, "TRACE", 5) == 0)
            return XR_HTTP_METHOD_TRACE;
        break;
    }
    
    return XR_HTTP_METHOD_UNKNOWN;
}

const char* xr_http_method_to_string(XrHttpMethod method)
{
    static const char *methods[] = {
        "GET", "POST", "PUT", "DELETE", "HEAD",
        "OPTIONS", "PATCH", "CONNECT", "TRACE", "UNKNOWN"
    };
    if (method < 0 || method > XR_HTTP_METHOD_UNKNOWN) {
        return "UNKNOWN";
    }
    return methods[method];
}

/* ========== Initialization Functions ========== */

void xr_http_parser_init(XrHttpParser *parser)
{
    parser->state = HTTP_STATE_REQUEST_LINE;
    parser->consumed = 0;
    parser->line_start = 0;
}

void xr_http_parser_reset(XrHttpParser *parser)
{
    xr_http_parser_init(parser);
}

void xr_http_request_init(XrHttpRequest *req)
{
    memset(req, 0, sizeof(XrHttpRequest));
    req->content_length = -1;
    req->keep_alive = true;  // HTTP/1.1 defaults to keep-alive
}

void xr_http_response_init(XrHttpResponse *resp)
{
    memset(resp, 0, sizeof(XrHttpResponse));
    resp->content_length = -1;
    resp->keep_alive = true;
}

/* ========== Header Lookup ========== */

// Case-insensitive comparison
static inline bool strcasecmp_n(const char *a, const char *b, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
            return false;
        }
    }
    return true;
}

const char* xr_http_get_header(XrHttpHeader *headers, size_t count,
                                const char *name, size_t *out_len)
{
    size_t name_len = strlen(name);
    
    for (size_t i = 0; i < count; i++) {
        if (headers[i].name_len == name_len &&
            strcasecmp_n(headers[i].name, name, name_len)) {
            if (out_len) *out_len = headers[i].value_len;
            return headers[i].value;
        }
    }
    
    return NULL;
}

/* ========== Parse Decimal Number ========== */

static inline int64_t parse_int(const char *p, const char *end)
{
    int64_t val = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        p++;
    }
    return val;
}

/* ========== Process Special Headers ========== */

static void process_special_header_request(XrHttpRequest *req, XrHttpHeader *h)
{
    // Length-based dispatch: compiler emits jump table, O(1) vs O(n) if-else
    switch (h->name_len) {
    case 4: // Host
        if (strcasecmp_n(h->name, "Host", 4)) {
            req->host = h->value;
            req->host_len = h->value_len;
        }
        break;
    case 10: // Connection
        if (strcasecmp_n(h->name, "Connection", 10)) {
            if (h->value_len == 10 && strcasecmp_n(h->value, "keep-alive", 10)) {
                req->keep_alive = true;
            } else if (h->value_len == 5 && strcasecmp_n(h->value, "close", 5)) {
                req->keep_alive = false;
            }
        }
        break;
    case 12: // Content-Type
        if (strcasecmp_n(h->name, "Content-Type", 12)) {
            req->content_type = h->value;
            req->content_type_len = h->value_len;
        }
        break;
    case 14: // Content-Length
        if (strcasecmp_n(h->name, "Content-Length", 14)) {
            req->content_length = parse_int(h->value, h->value + h->value_len);
        }
        break;
    case 17: // Transfer-Encoding
        if (strcasecmp_n(h->name, "Transfer-Encoding", 17)) {
            if (h->value_len >= 7) {
                const char *p = h->value;
                const char *end = h->value + h->value_len;
                while (p + 7 <= end) {
                    if (strcasecmp_n(p, "chunked", 7)) {
                        req->chunked = true;
                        break;
                    }
                    p++;
                }
            }
        }
        break;
    default:
        break;
    }
}

static void process_special_header_response(XrHttpResponse *resp, XrHttpHeader *h)
{
    switch (h->name_len) {
    case 10: // Connection
        if (strcasecmp_n(h->name, "Connection", 10)) {
            if (h->value_len == 10 && strcasecmp_n(h->value, "keep-alive", 10)) {
                resp->keep_alive = true;
            } else if (h->value_len == 5 && strcasecmp_n(h->value, "close", 5)) {
                resp->keep_alive = false;
            }
        }
        break;
    case 12: // Content-Type
        if (strcasecmp_n(h->name, "Content-Type", 12)) {
            resp->content_type = h->value;
            resp->content_type_len = h->value_len;
        }
        break;
    case 14: // Content-Length
        if (strcasecmp_n(h->name, "Content-Length", 14)) {
            resp->content_length = parse_int(h->value, h->value + h->value_len);
        }
        break;
    case 17: // Transfer-Encoding
        if (strcasecmp_n(h->name, "Transfer-Encoding", 17)) {
            if (h->value_len >= 7) {
                const char *p = h->value;
                const char *end = h->value + h->value_len;
                while (p + 7 <= end) {
                    if (strcasecmp_n(p, "chunked", 7)) {
                        resp->chunked = true;
                        break;
                    }
                    p++;
                }
            }
        }
        break;
    default:
        break;
    }
}

/* ========== Simplified API ========== */

XrHttpParseResult xr_http_parse_request(XrHttpParser *parser,
                                         XrHttpRequest *req,
                                         const char *data,
                                         size_t len)
{
    // Use new API to parse
    size_t num_headers = XR_HTTP_MAX_HEADERS;
    int r = xr_http_parse_request_ex(data, len,
                                      &req->method_str, &req->method_len,
                                      &req->path, &req->path_len,
                                      &req->version_minor,
                                      req->headers, &num_headers,
                                      0);
    
    if (r == -2) {
        return XR_HTTP_PARSE_INCOMPLETE;
    }
    if (r == -1) {
        parser->state = HTTP_STATE_ERROR;
        return XR_HTTP_PARSE_ERROR;
    }
    
    // Fill request structure
    req->version_major = 1;
    req->header_count = num_headers;
    req->header_bytes = (size_t)r;
    req->method = xr_http_method_from_string(req->method_str, req->method_len);
    
    // Separate path and query string
    const char *q = memchr(req->path, '?', req->path_len);
    if (q) {
        req->query = q + 1;
        req->query_len = req->path_len - (q - req->path) - 1;
        req->path_len = q - req->path;
    }
    
    // Set keep-alive default value
    req->keep_alive = (req->version_minor >= 1);
    
    // Process special headers
    for (size_t i = 0; i < req->header_count; i++) {
        process_special_header_request(req, &req->headers[i]);
    }
    
    // Set body pointer
    req->body = data + r;
    size_t remaining = len - r;
    
    if (req->chunked) {
        req->body_len = remaining;
        parser->state = HTTP_STATE_CHUNK_SIZE;
        return XR_HTTP_PARSE_OK;
    }
    
    if (req->content_length >= 0) {
        if (remaining >= (size_t)req->content_length) {
            req->body_len = (size_t)req->content_length;
            parser->state = HTTP_STATE_DONE;
            return XR_HTTP_PARSE_OK;
        }
        req->body_len = remaining;
        return XR_HTTP_PARSE_INCOMPLETE;
    }
    
    // No Content-Length and not chunked, assume no body
    req->body_len = 0;
    parser->state = HTTP_STATE_DONE;
    return XR_HTTP_PARSE_OK;
}

XrHttpParseResult xr_http_parse_response(XrHttpParser *parser,
                                          XrHttpResponse *resp,
                                          const char *data,
                                          size_t len)
{
    // Use new API to parse
    size_t num_headers = XR_HTTP_MAX_HEADERS;
    int r = xr_http_parse_response_ex(data, len,
                                       &resp->version_minor, &resp->status_code,
                                       &resp->status_text, &resp->status_text_len,
                                       resp->headers, &num_headers,
                                       0);
    
    if (r == -2) {
        return XR_HTTP_PARSE_INCOMPLETE;
    }
    if (r == -1) {
        parser->state = HTTP_STATE_ERROR;
        return XR_HTTP_PARSE_ERROR;
    }
    
    // Fill response structure
    resp->version_major = 1;
    resp->header_count = num_headers;
    resp->header_bytes = (size_t)r;
    
    // Set keep-alive default value
    resp->keep_alive = (resp->version_minor >= 1);
    
    // Process special headers
    for (size_t i = 0; i < resp->header_count; i++) {
        process_special_header_response(resp, &resp->headers[i]);
    }
    
    // Set body pointer
    resp->body = data + r;
    size_t remaining = len - r;
    
    if (resp->chunked) {
        resp->body_len = remaining;
        parser->state = HTTP_STATE_CHUNK_SIZE;
        return XR_HTTP_PARSE_OK;
    }
    
    if (resp->content_length >= 0) {
        if (remaining >= (size_t)resp->content_length) {
            resp->body_len = (size_t)resp->content_length;
            parser->state = HTTP_STATE_DONE;
            return XR_HTTP_PARSE_OK;
        }
        resp->body_len = remaining;
        return XR_HTTP_PARSE_INCOMPLETE;
    }
    
    // No Content-Length: read until connection close
    resp->body_len = remaining;
    parser->state = HTTP_STATE_BODY;
    return XR_HTTP_PARSE_OK;
}
