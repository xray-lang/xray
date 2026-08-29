/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_http2_huffman.c - HPACK Huffman table cross-language consistency guard
 *
 * KEY CONCEPT:
 *   The HPACK codec (Huffman table included) now lives in Xray, in
 *   stdlib/http2/http2.xr, and no longer has a C entry point this test could
 *   call.  What is left worth guarding from C is the *data*: the six constant
 *   arrays in that file must still spell out the RFC 7541 Appendix B static
 *   Huffman code, in both of the two forms the decoder relies on.
 *
 *   So this file embeds the authoritative Appendix B table (257 entries of
 *   {code, bit length}, symbols 0..255 plus EOS at 256; the literal values are
 *   the ones the deleted C implementation carried verbatim from the RFC), then
 *   parses HUFFMAN_CODE / HUFFMAN_BITS / HUFFMAN_FIRST_CODE /
 *   HUFFMAN_FIRST_INDEX / HUFFMAN_COUNT / HUFFMAN_SYMBOL straight out of
 *   stdlib/http2/http2.xr and compares them entry by entry.  It also proves the
 *   table is a well-formed prefix code and that the four canonical arrays are
 *   the ones the first two imply, so a hand edit to any single array cannot
 *   slip through.
 *
 *   WHERE THE OLD CASES WENT: the behavioural cases that used to live here --
 *   RFC 7541 Appendix C.4/C.6 decode vectors, Huffman padding rejection, HPACK
 *   index 0 rejection, SETTINGS payload validation and inbound frame-header
 *   validation -- exercised http2_hpack_decode(), http2_apply_settings_payload()
 *   and http2_validate_inbound_frame_header(), which no longer exist.  Those
 *   behaviours moved to an Xray regression test alongside the Xray port.
 *
 * TABLE SOURCE:
 *   RFC 7541 (HPACK: Header Compression for HTTP/2), Appendix B --
 *   "Huffman Code", the code used by the HPACK string literal representation.
 */

#include "../test_framework.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========== RFC 7541 Appendix B: the authoritative table ========== */

typedef struct {
    uint32_t code;
    uint8_t bits;
} HuffmanEntry;

#define HUFFMAN_SYMBOL_COUNT 257
#define HUFFMAN_MAX_BITS 30
#define HUFFMAN_EOS_SYMBOL 256
#define HUFFMAN_LENGTH_SLOTS (HUFFMAN_MAX_BITS + 1)

// RFC 7541 Appendix B, verbatim: symbols 0..255 followed by EOS at index 256.
// clang-format off
static const HuffmanEntry kAppendixB[HUFFMAN_SYMBOL_COUNT] = {
    /*   0 */ {0x1ff8, 13}, {0x7fffd8, 23}, {0xfffffe2, 28}, {0xfffffe3, 28},
    /*   4 */ {0xfffffe4, 28}, {0xfffffe5, 28}, {0xfffffe6, 28}, {0xfffffe7, 28},
    /*   8 */ {0xfffffe8, 28}, {0xffffea, 24}, {0x3ffffffc, 30}, {0xfffffe9, 28},
    /*  12 */ {0xfffffea, 28}, {0x3ffffffd, 30}, {0xfffffeb, 28}, {0xfffffec, 28},
    /*  16 */ {0xfffffed, 28}, {0xfffffee, 28}, {0xfffffef, 28}, {0xffffff0, 28},
    /*  20 */ {0xffffff1, 28}, {0xffffff2, 28}, {0x3ffffffe, 30}, {0xffffff3, 28},
    /*  24 */ {0xffffff4, 28}, {0xffffff5, 28}, {0xffffff6, 28}, {0xffffff7, 28},
    /*  28 */ {0xffffff8, 28}, {0xffffff9, 28}, {0xffffffa, 28}, {0xffffffb, 28},
    /*  32 */ {0x14, 6}, {0x3f8, 10}, {0x3f9, 10}, {0xffa, 12},
    /*  36 */ {0x1ff9, 13}, {0x15, 6}, {0xf8, 8}, {0x7fa, 11},
    /*  40 */ {0x3fa, 10}, {0x3fb, 10}, {0xf9, 8}, {0x7fb, 11},
    /*  44 */ {0xfa, 8}, {0x16, 6}, {0x17, 6}, {0x18, 6},
    /*  48 */ {0x0, 5}, {0x1, 5}, {0x2, 5}, {0x19, 6},
    /*  52 */ {0x1a, 6}, {0x1b, 6}, {0x1c, 6}, {0x1d, 6},
    /*  56 */ {0x1e, 6}, {0x1f, 6}, {0x5c, 7}, {0xfb, 8},
    /*  60 */ {0x7ffc, 15}, {0x20, 6}, {0xffb, 12}, {0x3fc, 10},
    /*  64 */ {0x1ffa, 13}, {0x21, 6}, {0x5d, 7}, {0x5e, 7},
    /*  68 */ {0x5f, 7}, {0x60, 7}, {0x61, 7}, {0x62, 7},
    /*  72 */ {0x63, 7}, {0x64, 7}, {0x65, 7}, {0x66, 7},
    /*  76 */ {0x67, 7}, {0x68, 7}, {0x69, 7}, {0x6a, 7},
    /*  80 */ {0x6b, 7}, {0x6c, 7}, {0x6d, 7}, {0x6e, 7},
    /*  84 */ {0x6f, 7}, {0x70, 7}, {0x71, 7}, {0x72, 7},
    /*  88 */ {0xfc, 8}, {0x73, 7}, {0xfd, 8}, {0x1ffb, 13},
    /*  92 */ {0x7fff0, 19}, {0x1ffc, 13}, {0x3ffc, 14}, {0x22, 6},
    /*  96 */ {0x7ffd, 15}, {0x3, 5}, {0x23, 6}, {0x4, 5},
    /* 100 */ {0x24, 6}, {0x5, 5}, {0x25, 6}, {0x26, 6},
    /* 104 */ {0x27, 6}, {0x6, 5}, {0x74, 7}, {0x75, 7},
    /* 108 */ {0x28, 6}, {0x29, 6}, {0x2a, 6}, {0x7, 5},
    /* 112 */ {0x2b, 6}, {0x76, 7}, {0x2c, 6}, {0x8, 5},
    /* 116 */ {0x9, 5}, {0x2d, 6}, {0x77, 7}, {0x78, 7},
    /* 120 */ {0x79, 7}, {0x7a, 7}, {0x7b, 7}, {0x7ffe, 15},
    /* 124 */ {0x7fc, 11}, {0x3ffd, 14}, {0x1ffd, 13}, {0xffffffc, 28},
    /* 128 */ {0xfffe6, 20}, {0x3fffd2, 22}, {0xfffe7, 20}, {0xfffe8, 20},
    /* 132 */ {0x3fffd3, 22}, {0x3fffd4, 22}, {0x3fffd5, 22}, {0x7fffd9, 23},
    /* 136 */ {0x3fffd6, 22}, {0x7fffda, 23}, {0x7fffdb, 23}, {0x7fffdc, 23},
    /* 140 */ {0x7fffdd, 23}, {0x7fffde, 23}, {0xffffeb, 24}, {0x7fffdf, 23},
    /* 144 */ {0xffffec, 24}, {0xffffed, 24}, {0x3fffd7, 22}, {0x7fffe0, 23},
    /* 148 */ {0xffffee, 24}, {0x7fffe1, 23}, {0x7fffe2, 23}, {0x7fffe3, 23},
    /* 152 */ {0x7fffe4, 23}, {0x1fffdc, 21}, {0x3fffd8, 22}, {0x7fffe5, 23},
    /* 156 */ {0x3fffd9, 22}, {0x7fffe6, 23}, {0x7fffe7, 23}, {0xffffef, 24},
    /* 160 */ {0x3fffda, 22}, {0x1fffdd, 21}, {0xfffe9, 20}, {0x3fffdb, 22},
    /* 164 */ {0x3fffdc, 22}, {0x7fffe8, 23}, {0x7fffe9, 23}, {0x1fffde, 21},
    /* 168 */ {0x7fffea, 23}, {0x3fffdd, 22}, {0x3fffde, 22}, {0xfffff0, 24},
    /* 172 */ {0x1fffdf, 21}, {0x3fffdf, 22}, {0x7fffeb, 23}, {0x7fffec, 23},
    /* 176 */ {0x1fffe0, 21}, {0x1fffe1, 21}, {0x3fffe0, 22}, {0x1fffe2, 21},
    /* 180 */ {0x7fffed, 23}, {0x3fffe1, 22}, {0x7fffee, 23}, {0x7fffef, 23},
    /* 184 */ {0xfffea, 20}, {0x3fffe2, 22}, {0x3fffe3, 22}, {0x3fffe4, 22},
    /* 188 */ {0x7ffff0, 23}, {0x3fffe5, 22}, {0x3fffe6, 22}, {0x7ffff1, 23},
    /* 192 */ {0x3ffffe0, 26}, {0x3ffffe1, 26}, {0xfffeb, 20}, {0x7fff1, 19},
    /* 196 */ {0x3fffe7, 22}, {0x7ffff2, 23}, {0x3fffe8, 22}, {0x1ffffec, 25},
    /* 200 */ {0x3ffffe2, 26}, {0x3ffffe3, 26}, {0x3ffffe4, 26}, {0x7ffffde, 27},
    /* 204 */ {0x7ffffdf, 27}, {0x3ffffe5, 26}, {0xfffff1, 24}, {0x1ffffed, 25},
    /* 208 */ {0x7fff2, 19}, {0x1fffe3, 21}, {0x3ffffe6, 26}, {0x7ffffe0, 27},
    /* 212 */ {0x7ffffe1, 27}, {0x3ffffe7, 26}, {0x7ffffe2, 27}, {0xfffff2, 24},
    /* 216 */ {0x1fffe4, 21}, {0x1fffe5, 21}, {0x3ffffe8, 26}, {0x3ffffe9, 26},
    /* 220 */ {0xffffffd, 28}, {0x7ffffe3, 27}, {0x7ffffe4, 27}, {0x7ffffe5, 27},
    /* 224 */ {0xfffec, 20}, {0xfffff3, 24}, {0xfffed, 20}, {0x1fffe6, 21},
    /* 228 */ {0x3fffe9, 22}, {0x1fffe7, 21}, {0x1fffe8, 21}, {0x7ffff3, 23},
    /* 232 */ {0x3fffea, 22}, {0x3fffeb, 22}, {0x1ffffee, 25}, {0x1ffffef, 25},
    /* 236 */ {0xfffff4, 24}, {0xfffff5, 24}, {0x3ffffea, 26}, {0x7ffff4, 23},
    /* 240 */ {0x3ffffeb, 26}, {0x7ffffe6, 27}, {0x3ffffec, 26}, {0x3ffffed, 26},
    /* 244 */ {0x7ffffe7, 27}, {0x7ffffe8, 27}, {0x7ffffe9, 27}, {0x7ffffea, 27},
    /* 248 */ {0x7ffffeb, 27}, {0xffffffe, 28}, {0x7ffffec, 27}, {0x7ffffed, 27},
    /* 252 */ {0x7ffffee, 27}, {0x7ffffef, 27}, {0x7fffff0, 27}, {0x3ffffee, 26},
    /* 256 */ {0x3fffffff, 30},
};
// clang-format on

/* ========== Locating and parsing stdlib/http2/http2.xr ========== */

// Paths tried in order.  The first is baked in by CMake (see the
// target_compile_definitions next to this test's registration); the rest keep
// the test runnable when it is launched by hand from a build directory.
static const char *const kSourceCandidates[] = {
#ifdef XRAY_HTTP2_XR_SOURCE
    XRAY_HTTP2_XR_SOURCE,
#endif
    "stdlib/http2/http2.xr",
    "../stdlib/http2/http2.xr",
    "../../stdlib/http2/http2.xr",
    "../../../stdlib/http2/http2.xr",
    "../../../../stdlib/http2/http2.xr",
};

// Parsed contents of the six constant arrays, plus the two scalar constants.
static long long g_code[HUFFMAN_SYMBOL_COUNT];
static long long g_bits[HUFFMAN_SYMBOL_COUNT];
static long long g_symbol[HUFFMAN_SYMBOL_COUNT];
static long long g_first_code[HUFFMAN_LENGTH_SLOTS];
static long long g_first_index[HUFFMAN_LENGTH_SLOTS];
static long long g_count[HUFFMAN_LENGTH_SLOTS];
static long long g_max_bits_const;
static long long g_eos_const;

static int g_n_code, g_n_bits, g_n_symbol;
static int g_n_first_code, g_n_first_index, g_n_count;

// 0 until the whole file has been located and every array parsed.
static int g_tables_loaded;
static const char *g_source_path;

static char *read_whole_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = (char *) malloc((size_t) size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t) size, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

// Reads the integer tokens of `const <name>: Array<i64> = [ ... ]`.  Accepts
// both 0x-hex and decimal literals and skips `//` line comments.  Answers the
// token count, or -1 if the declaration is missing, unterminated, malformed, or
// longer than `cap`.
static int scan_i64_array(const char *src, const char *name, long long *out, int cap) {
    char needle[128];
    snprintf(needle, sizeof(needle), "const %s: Array<i64> = [", name);
    const char *p = strstr(src, needle);
    if (!p)
        return -1;
    p += strlen(needle);

    int n = 0;
    while (*p && *p != ']') {
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n')
                p++;
            continue;
        }
        if (*p >= '0' && *p <= '9') {
            const char *digits = p;
            int base = 10;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
                base = 16;
                digits = p + 2;
            }
            char *end = NULL;
            unsigned long long v = strtoull(digits, &end, base);
            if (end == digits)
                return -1;
            if (n >= cap)
                return -1;
            out[n++] = (long long) v;
            p = end;
            continue;
        }
        p++;
    }
    if (*p != ']')
        return -1;
    return n;
}

// Reads a scalar `const <name> = <int>` declaration out of the same file.
static int scan_i64_scalar(const char *src, const char *name, long long *out) {
    char needle[128];
    snprintf(needle, sizeof(needle), "const %s = ", name);
    const char *p = strstr(src, needle);
    if (!p)
        return -1;
    p += strlen(needle);
    const char *digits = p;
    int base = 10;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        digits = p + 2;
    }
    char *end = NULL;
    unsigned long long v = strtoull(digits, &end, base);
    if (end == digits)
        return -1;
    *out = (long long) v;
    return 0;
}

// Locates http2.xr, parses every constant this test needs, and frees the file
// buffer.  On failure it prints each path it tried, so a miss is loud instead of
// a silently skipped suite.  Answers 1 on success.
static int load_xray_tables(void) {
    char *src = NULL;
    size_t n_candidates = sizeof(kSourceCandidates) / sizeof(kSourceCandidates[0]);
    for (size_t i = 0; i < n_candidates; i++) {
        src = read_whole_file(kSourceCandidates[i]);
        if (src) {
            g_source_path = kSourceCandidates[i];
            break;
        }
    }
    if (!src) {
        printf("\033[31mFATAL\033[0m: cannot open stdlib/http2/http2.xr; tried:\n");
        for (size_t i = 0; i < n_candidates; i++)
            printf("    %s\n", kSourceCandidates[i]);
        printf("  (cwd-relative paths resolve against the test's working directory)\n");
        return 0;
    }

    g_n_code = scan_i64_array(src, "HUFFMAN_CODE", g_code, HUFFMAN_SYMBOL_COUNT);
    g_n_bits = scan_i64_array(src, "HUFFMAN_BITS", g_bits, HUFFMAN_SYMBOL_COUNT);
    g_n_symbol = scan_i64_array(src, "HUFFMAN_SYMBOL", g_symbol, HUFFMAN_SYMBOL_COUNT);
    g_n_first_code = scan_i64_array(src, "HUFFMAN_FIRST_CODE", g_first_code, HUFFMAN_LENGTH_SLOTS);
    g_n_first_index =
        scan_i64_array(src, "HUFFMAN_FIRST_INDEX", g_first_index, HUFFMAN_LENGTH_SLOTS);
    g_n_count = scan_i64_array(src, "HUFFMAN_COUNT", g_count, HUFFMAN_LENGTH_SLOTS);

    int ok_scalars = scan_i64_scalar(src, "HUFFMAN_MAX_BITS", &g_max_bits_const) == 0 &&
                     scan_i64_scalar(src, "HUFFMAN_EOS", &g_eos_const) == 0;

    free(src);

    if (g_n_code < 0 || g_n_bits < 0 || g_n_symbol < 0 || g_n_first_code < 0 ||
        g_n_first_index < 0 || g_n_count < 0 || !ok_scalars) {
        printf("\033[31mFATAL\033[0m: %s parsed but a HUFFMAN_* declaration is missing or "
               "malformed\n",
               g_source_path);
        printf("    HUFFMAN_CODE=%d HUFFMAN_BITS=%d HUFFMAN_SYMBOL=%d\n", g_n_code, g_n_bits,
               g_n_symbol);
        printf("    HUFFMAN_FIRST_CODE=%d HUFFMAN_FIRST_INDEX=%d HUFFMAN_COUNT=%d scalars=%d\n",
               g_n_first_code, g_n_first_index, g_n_count, ok_scalars);
        return 0;
    }

    g_tables_loaded = 1;
    return 1;
}

/* ========== Canonical-form derivation ========== */

typedef struct {
    int bits;
    uint32_t code;
    int symbol;
} CanonicalEntry;

static int canonical_cmp(const void *lhs, const void *rhs) {
    const CanonicalEntry *a = (const CanonicalEntry *) lhs;
    const CanonicalEntry *b = (const CanonicalEntry *) rhs;
    if (a->bits != b->bits)
        return a->bits < b->bits ? -1 : 1;
    if (a->code != b->code)
        return a->code < b->code ? -1 : 1;
    return 0;
}

/* ========== Tests ========== */

// Guards the guard: without a readable, fully parsed http2.xr every comparison
// below would be vacuous, so the missing-file case must fail here.
TEST(xray_source_tables_are_readable) {
    ASSERT_MSG(g_tables_loaded, "stdlib/http2/http2.xr could not be read or parsed (see above)");
    ASSERT_EQ_INT(g_n_code, HUFFMAN_SYMBOL_COUNT);
    ASSERT_EQ_INT(g_n_bits, HUFFMAN_SYMBOL_COUNT);
    ASSERT_EQ_INT(g_n_symbol, HUFFMAN_SYMBOL_COUNT);
    ASSERT_EQ_INT(g_n_first_code, HUFFMAN_LENGTH_SLOTS);
    ASSERT_EQ_INT(g_n_first_index, HUFFMAN_LENGTH_SLOTS);
    ASSERT_EQ_INT(g_n_count, HUFFMAN_LENGTH_SLOTS);
    ASSERT_EQ_INT(g_max_bits_const, HUFFMAN_MAX_BITS);
    ASSERT_EQ_INT(g_eos_const, HUFFMAN_EOS_SYMBOL);
}

// HUFFMAN_CODE / HUFFMAN_BITS must be RFC 7541 Appendix B entry for entry.
TEST(xray_code_and_bits_match_rfc7541_appendix_b) {
    ASSERT(g_tables_loaded);
    char msg[160];
    for (int i = 0; i < HUFFMAN_SYMBOL_COUNT; i++) {
        if ((unsigned long long) g_code[i] != (unsigned long long) kAppendixB[i].code) {
            snprintf(msg, sizeof(msg), "HUFFMAN_CODE[%d] = 0x%llx, RFC 7541 Appendix B says 0x%lx",
                     i, (unsigned long long) g_code[i], (unsigned long) kAppendixB[i].code);
            ASSERT_MSG(0, msg);
        }
        if (g_bits[i] != (long long) kAppendixB[i].bits) {
            snprintf(msg, sizeof(msg), "HUFFMAN_BITS[%d] = %lld, RFC 7541 Appendix B says %u", i,
                     g_bits[i], (unsigned) kAppendixB[i].bits);
            ASSERT_MSG(0, msg);
        }
    }
}

// Every code fits its declared length and the lengths satisfy Kraft equality,
// computed in integers: sum of 2^(30 - bits) over all 257 codes must be 2^30.
TEST(xray_table_lengths_are_sane_and_kraft_sum_is_one) {
    ASSERT(g_tables_loaded);
    char msg[160];
    unsigned long long kraft = 0;
    for (int i = 0; i < HUFFMAN_SYMBOL_COUNT; i++) {
        if (g_bits[i] < 1 || g_bits[i] > HUFFMAN_MAX_BITS) {
            snprintf(msg, sizeof(msg), "HUFFMAN_BITS[%d] = %lld is outside 1..30", i, g_bits[i]);
            ASSERT_MSG(0, msg);
        }
        if ((unsigned long long) g_code[i] >= (1ULL << g_bits[i])) {
            snprintf(msg, sizeof(msg), "HUFFMAN_CODE[%d] = 0x%llx does not fit in %lld bits", i,
                     (unsigned long long) g_code[i], g_bits[i]);
            ASSERT_MSG(0, msg);
        }
        kraft += 1ULL << (HUFFMAN_MAX_BITS - g_bits[i]);
    }
    ASSERT_EQ_UINT(kraft, 1ULL << HUFFMAN_MAX_BITS);
}

// No code may be a prefix of another, or the decoder could never be
// unambiguous.  257 codes, so the quadratic sweep is ~33k comparisons.
TEST(xray_table_is_prefix_free) {
    ASSERT(g_tables_loaded);
    for (int i = 0; i < HUFFMAN_SYMBOL_COUNT; i++) {
        for (int j = i + 1; j < HUFFMAN_SYMBOL_COUNT; j++) {
            int shorter = i, longer = j;
            if (g_bits[i] > g_bits[j]) {
                shorter = j;
                longer = i;
            }
            int drop = (int) (g_bits[longer] - g_bits[shorter]);
            unsigned long long head = (unsigned long long) g_code[longer] >> drop;
            if (head == (unsigned long long) g_code[shorter]) {
                char msg[160];
                snprintf(msg, sizeof(msg),
                         "symbol %d (0x%llx/%lld bits) is a prefix of symbol %d (0x%llx/%lld bits)",
                         shorter, (unsigned long long) g_code[shorter], g_bits[shorter], longer,
                         (unsigned long long) g_code[longer], g_bits[longer]);
                ASSERT_MSG(0, msg);
            }
        }
    }
}

// The four canonical arrays must be exactly what HUFFMAN_CODE/HUFFMAN_BITS
// imply: sort the 257 codes by (bit length, code value) and each length owns one
// contiguous ascending run.
TEST(xray_canonical_arrays_match_code_and_bits) {
    ASSERT(g_tables_loaded);
    char msg[160];

    CanonicalEntry sorted[HUFFMAN_SYMBOL_COUNT];
    for (int i = 0; i < HUFFMAN_SYMBOL_COUNT; i++) {
        sorted[i].bits = (int) g_bits[i];
        sorted[i].code = (uint32_t) g_code[i];
        sorted[i].symbol = i;
    }
    qsort(sorted, HUFFMAN_SYMBOL_COUNT, sizeof(sorted[0]), canonical_cmp);

    for (int i = 0; i < HUFFMAN_SYMBOL_COUNT; i++) {
        if (g_symbol[i] != sorted[i].symbol) {
            snprintf(msg, sizeof(msg), "HUFFMAN_SYMBOL[%d] = %lld, canonical order wants %d", i,
                     g_symbol[i], sorted[i].symbol);
            ASSERT_MSG(0, msg);
        }
    }

    // Unused lengths are spelled as 0/0/0 in http2.xr, so derive them the same way.
    long long want_first_code[HUFFMAN_LENGTH_SLOTS] = {0};
    long long want_first_index[HUFFMAN_LENGTH_SLOTS] = {0};
    long long want_count[HUFFMAN_LENGTH_SLOTS] = {0};

    for (int n = 1; n <= HUFFMAN_MAX_BITS; n++) {
        int first = -1, count = 0;
        for (int i = 0; i < HUFFMAN_SYMBOL_COUNT; i++) {
            if (sorted[i].bits != n)
                continue;
            if (first < 0)
                first = i;
            count++;
        }
        want_count[n] = count;
        if (count == 0)
            continue;
        want_first_code[n] = sorted[first].code;
        want_first_index[n] = first;
        for (int k = 0; k < count; k++) {
            if (sorted[first + k].code != sorted[first].code + (uint32_t) k) {
                snprintf(msg, sizeof(msg),
                         "length %d is not a contiguous run: entry %d is 0x%lx, expected 0x%lx", n,
                         k, (unsigned long) sorted[first + k].code,
                         (unsigned long) (sorted[first].code + (uint32_t) k));
                ASSERT_MSG(0, msg);
            }
        }
    }

    for (int n = 0; n < HUFFMAN_LENGTH_SLOTS; n++) {
        if (g_count[n] != want_count[n]) {
            snprintf(msg, sizeof(msg), "HUFFMAN_COUNT[%d] = %lld, derived %lld", n, g_count[n],
                     want_count[n]);
            ASSERT_MSG(0, msg);
        }
        if (g_first_code[n] != want_first_code[n]) {
            snprintf(msg, sizeof(msg), "HUFFMAN_FIRST_CODE[%d] = 0x%llx, derived 0x%llx", n,
                     (unsigned long long) g_first_code[n], (unsigned long long) want_first_code[n]);
            ASSERT_MSG(0, msg);
        }
        if (g_first_index[n] != want_first_index[n]) {
            snprintf(msg, sizeof(msg), "HUFFMAN_FIRST_INDEX[%d] = %lld, derived %lld", n,
                     g_first_index[n], want_first_index[n]);
            ASSERT_MSG(0, msg);
        }
    }
}

// RFC 7541 Appendix B: EOS is symbol 256, code 0x3fffffff, 30 bits -- the
// longest code, so it also sorts last in the canonical order.
TEST(xray_eos_is_symbol_256_thirty_bits) {
    ASSERT(g_tables_loaded);
    ASSERT_EQ_INT(g_eos_const, 256);
    ASSERT_EQ_INT(g_max_bits_const, 30);

    ASSERT_EQ_UINT(kAppendixB[HUFFMAN_EOS_SYMBOL].code, 0x3fffffffULL);
    ASSERT_EQ_INT(kAppendixB[HUFFMAN_EOS_SYMBOL].bits, 30);

    ASSERT_EQ_UINT(g_code[HUFFMAN_EOS_SYMBOL], 0x3fffffffULL);
    ASSERT_EQ_INT(g_bits[HUFFMAN_EOS_SYMBOL], 30);
    ASSERT_EQ_INT(g_symbol[HUFFMAN_SYMBOL_COUNT - 1], HUFFMAN_EOS_SYMBOL);

    // 0x3fffffff is the last code of length 30, i.e. first_code[30] + count - 1.
    ASSERT_EQ_UINT(g_first_code[HUFFMAN_MAX_BITS] + g_count[HUFFMAN_MAX_BITS] - 1, 0x3fffffffULL);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

// Diagnostics go here, before the first suite header; the tests below turn a
// failed load into a real failure rather than a skip.
(void) load_xray_tables();
if (g_source_path)
    printf("  table source: %s\n", g_source_path);

RUN_TEST_SUITE("stdlib/http2/http2.xr - table extraction");
RUN_TEST(xray_source_tables_are_readable);

RUN_TEST_SUITE("HPACK Huffman - RFC 7541 Appendix B agreement");
RUN_TEST(xray_code_and_bits_match_rfc7541_appendix_b);
RUN_TEST(xray_eos_is_symbol_256_thirty_bits);

RUN_TEST_SUITE("HPACK Huffman - table is a valid prefix code");
RUN_TEST(xray_table_lengths_are_sane_and_kraft_sum_is_one);
RUN_TEST(xray_table_is_prefix_free);

RUN_TEST_SUITE("HPACK Huffman - canonical form self-consistency");
RUN_TEST(xray_canonical_arrays_match_code_and_bits);

TEST_MAIN_END()
