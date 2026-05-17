/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_http2_huffman.c - HPACK Huffman decoder tests (RFC 7541 Appendix C)
 *
 * KEY CONCEPT:
 *   Drives the public xr_hpack_decode() API with verbatim HPACK header
 *   blocks from RFC 7541 Appendix C.4 / C.6 to catch regressions in the
 *   Huffman decoder (RFC 7541 Section 5.2 / Appendix B).
 */

#include "../test_framework.h"
#include <string.h>
#include <stdlib.h>

#include "../../../stdlib/http/http2.h"

/* ========== Callback Harness ========== */

// Collects at most 8 decoded headers from one HPACK block for assertions.
typedef struct {
    char name[8][64];
    char value[8][256];
    size_t name_len[8];
    size_t value_len[8];
    int count;
} CollectedHeaders;

static void collect_header(const char *name, size_t name_len, const char *value, size_t value_len,
                           void *user_data) {
    CollectedHeaders *c = (CollectedHeaders *) user_data;
    if (c->count >= 8)
        return;
    if (name_len >= sizeof(c->name[0]))
        name_len = sizeof(c->name[0]) - 1;
    if (value_len >= sizeof(c->value[0]))
        value_len = sizeof(c->value[0]) - 1;
    memcpy(c->name[c->count], name, name_len);
    c->name[c->count][name_len] = '\0';
    c->name_len[c->count] = name_len;
    memcpy(c->value[c->count], value, value_len);
    c->value[c->count][value_len] = '\0';
    c->value_len[c->count] = value_len;
    c->count++;
}

/* ========== RFC 7541 C.4.1: First Request with Huffman ========== */

// :method = GET            indexed 2:      0x82
// :scheme = http           indexed 6:      0x86
// :path = /                indexed 4:      0x84
// :authority = www.example.com (Huffman):  0x41 0x8c f1e3 c2e5 f23a 6ba0 ab90 f4ff
TEST(huffman_rfc7541_c_4_1) {
    static const uint8_t block[] = {0x82, 0x86, 0x84, 0x41, 0x8c, 0xf1, 0xe3, 0xc2, 0xe5,
                                    0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xff};
    XrHpackTable table;
    xr_hpack_init(&table, 4096);
    CollectedHeaders c = {0};
    int rc = xr_hpack_decode(&table, block, sizeof(block), collect_header, &c);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(c.count, 4);
    ASSERT_STR_EQ(c.name[0], ":method");
    ASSERT_STR_EQ(c.value[0], "GET");
    ASSERT_STR_EQ(c.name[1], ":scheme");
    ASSERT_STR_EQ(c.value[1], "http");
    ASSERT_STR_EQ(c.name[2], ":path");
    ASSERT_STR_EQ(c.value[2], "/");
    ASSERT_STR_EQ(c.name[3], ":authority");
    ASSERT_STR_EQ(c.value[3], "www.example.com");
    xr_hpack_free(&table);
}

/* ========== RFC 7541 C.4.2: Second Request with Huffman ========== */

// Adds cache-control = no-cache (Huffman):  0x58 0x86 a8eb 1064 9cbf
TEST(huffman_rfc7541_c_4_2_no_cache) {
    static const uint8_t block[] = {0x82, 0x86, 0x84, 0xbe, 0x58, 0x86,
                                    0xa8, 0xeb, 0x10, 0x64, 0x9c, 0xbf};
    XrHpackTable table;
    xr_hpack_init(&table, 4096);
    // Pre-populate :authority in dynamic table via C.4.1 first so index 0xbe
    // (indexed header field 62 = first dynamic entry) resolves.
    static const uint8_t prereq[] = {0x82, 0x86, 0x84, 0x41, 0x8c, 0xf1, 0xe3, 0xc2, 0xe5,
                                     0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xff};
    CollectedHeaders c1 = {0};
    xr_hpack_decode(&table, prereq, sizeof(prereq), collect_header, &c1);
    CollectedHeaders c = {0};
    int rc = xr_hpack_decode(&table, block, sizeof(block), collect_header, &c);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(c.count, 5);  // 4 from prior-indexed + cache-control
    // Last header is the newly-added Huffman literal.
    ASSERT_STR_EQ(c.name[4], "cache-control");
    ASSERT_STR_EQ(c.value[4], "no-cache");
    xr_hpack_free(&table);
}

/* ========== RFC 7541 C.4.3: Third Request (custom-key/value Huffman) ========== */

// custom-key / custom-value (both Huffman, literal with indexing, new name):
//   0x40 0x88 25a8 49e9 5ba9 7d7f  0x89 25a8 49e9 5bb8 e8b4 bf
TEST(huffman_rfc7541_c_4_3_custom) {
    static const uint8_t block[] = {0x40, 0x88, 0x25, 0xa8, 0x49, 0xe9, 0x5b, 0xa9, 0x7d, 0x7f,
                                    0x89, 0x25, 0xa8, 0x49, 0xe9, 0x5b, 0xb8, 0xe8, 0xb4, 0xbf};
    XrHpackTable table;
    xr_hpack_init(&table, 4096);
    CollectedHeaders c = {0};
    int rc = xr_hpack_decode(&table, block, sizeof(block), collect_header, &c);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(c.count, 1);
    ASSERT_STR_EQ(c.name[0], "custom-key");
    ASSERT_STR_EQ(c.value[0], "custom-value");
    xr_hpack_free(&table);
}

/* ========== RFC 7541 C.6.1: Response with Huffman ========== */

// :status=302   literal w/indexing name idx 8 (=:status), Huffman value "302":
//   0x48 0x82 6402
// cache-control=private  literal w/indexing name idx 24, Huffman value:
//   0x58 0x85 aec3 771a 4b
// date=Mon, 21 Oct 2013 20:13:21 GMT  literal w/indexing name idx 33, Huffman:
//   0x61 0x96 d07a be94 1054 d444 a820 0595 040b 8166 e082 a62d 1bff
// location=https://www.example.com  literal w/indexing name idx 46, Huffman:
//   0x6e 0x91 9d29 ad17 1863 c78f 0b97 c8e9 ae82 ae43 d3
TEST(huffman_rfc7541_c_6_1_response) {
    static const uint8_t block[] = {
        0x48, 0x82, 0x64, 0x02, 0x58, 0x85, 0xae, 0xc3, 0x77, 0x1a, 0x4b, 0x61, 0x96, 0xd0,
        0x7a, 0xbe, 0x94, 0x10, 0x54, 0xd4, 0x44, 0xa8, 0x20, 0x05, 0x95, 0x04, 0x0b, 0x81,
        0x66, 0xe0, 0x82, 0xa6, 0x2d, 0x1b, 0xff, 0x6e, 0x91, 0x9d, 0x29, 0xad, 0x17, 0x18,
        0x63, 0xc7, 0x8f, 0x0b, 0x97, 0xc8, 0xe9, 0xae, 0x82, 0xae, 0x43, 0xd3};
    XrHpackTable table;
    xr_hpack_init(&table, 4096);
    CollectedHeaders c = {0};
    int rc = xr_hpack_decode(&table, block, sizeof(block), collect_header, &c);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(c.count, 4);
    ASSERT_STR_EQ(c.name[0], ":status");
    ASSERT_STR_EQ(c.value[0], "302");
    ASSERT_STR_EQ(c.name[1], "cache-control");
    ASSERT_STR_EQ(c.value[1], "private");
    ASSERT_STR_EQ(c.name[2], "date");
    ASSERT_STR_EQ(c.value[2], "Mon, 21 Oct 2013 20:13:21 GMT");
    ASSERT_STR_EQ(c.name[3], "location");
    ASSERT_STR_EQ(c.value[3], "https://www.example.com");
    xr_hpack_free(&table);
}

/* ========== RFC 7541 C.6.3: Third Response content-encoding=gzip ========== */

// Direct test of Huffman-encoded "gzip" literal (length 3 Huffman block).
TEST(huffman_rfc7541_c_6_3_gzip) {
    // Literal with indexing, name idx 26 (content-encoding), Huffman value "gzip":
    //   0x5a 0x83 9bd9 ab
    static const uint8_t block[] = {0x5a, 0x83, 0x9b, 0xd9, 0xab};
    XrHpackTable table;
    xr_hpack_init(&table, 4096);
    CollectedHeaders c = {0};
    int rc = xr_hpack_decode(&table, block, sizeof(block), collect_header, &c);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(c.count, 1);
    ASSERT_STR_EQ(c.name[0], "content-encoding");
    ASSERT_STR_EQ(c.value[0], "gzip");
    xr_hpack_free(&table);
}

/* ========== Huffman padding correctness ========== */

// All-zero byte cannot be a valid Huffman string: "00000" decodes to '0', and
// the remaining 3 bits "000" are not MSBs of the EOS code (must be all 1s),
// so decode MUST fail.
TEST(huffman_invalid_padding_rejected) {
    // Literal-new-name with Huffman length 1, data 0x00.
    static const uint8_t block[] = {0x40, 0x81, 0x00, 0x81, 0x00};
    XrHpackTable table;
    xr_hpack_init(&table, 4096);
    CollectedHeaders c = {0};
    int rc = xr_hpack_decode(&table, block, sizeof(block), collect_header, &c);
    ASSERT_EQ_INT(rc, -1);
    xr_hpack_free(&table);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("HPACK Huffman - RFC 7541 Appendix C.4 Request");
RUN_TEST(huffman_rfc7541_c_4_1);
RUN_TEST(huffman_rfc7541_c_4_2_no_cache);
RUN_TEST(huffman_rfc7541_c_4_3_custom);

RUN_TEST_SUITE("HPACK Huffman - RFC 7541 Appendix C.6 Response");
RUN_TEST(huffman_rfc7541_c_6_1_response);
RUN_TEST(huffman_rfc7541_c_6_3_gzip);

RUN_TEST_SUITE("HPACK Huffman - Padding / Error Cases");
RUN_TEST(huffman_invalid_padding_rejected);

TEST_MAIN_END()
