/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http2.c - HTTP/2 protocol implementation
 *
 * KEY CONCEPT:
 *   Implements RFC 7540 HTTP/2 protocol with HPACK header compression
 */

#include "../../src/base/xmalloc.h"
#include "http2.h"
#include "../net/tls.h"
#include "../../src/base/xhash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../../src/os/os_thread.h"
#include <arpa/inet.h>

/* ========== HPACK Static Table (RFC 7541 Appendix A) ========== */

static const struct {
    const char *name;
    const char *value;
} hpack_static_table[] = {{NULL, NULL},  // Index starts from 1
                          {":authority", ""},
                          {":method", "GET"},
                          {":method", "POST"},
                          {":path", "/"},
                          {":path", "/index.html"},
                          {":scheme", "http"},
                          {":scheme", "https"},
                          {":status", "200"},
                          {":status", "204"},
                          {":status", "206"},
                          {":status", "304"},
                          {":status", "400"},
                          {":status", "404"},
                          {":status", "500"},
                          {"accept-charset", ""},
                          {"accept-encoding", "gzip, deflate"},
                          {"accept-language", ""},
                          {"accept-ranges", ""},
                          {"accept", ""},
                          {"access-control-allow-origin", ""},
                          {"age", ""},
                          {"allow", ""},
                          {"authorization", ""},
                          {"cache-control", ""},
                          {"content-disposition", ""},
                          {"content-encoding", ""},
                          {"content-language", ""},
                          {"content-length", ""},
                          {"content-location", ""},
                          {"content-range", ""},
                          {"content-type", ""},
                          {"cookie", ""},
                          {"date", ""},
                          {"etag", ""},
                          {"expect", ""},
                          {"expires", ""},
                          {"from", ""},
                          {"host", ""},
                          {"if-match", ""},
                          {"if-modified-since", ""},
                          {"if-none-match", ""},
                          {"if-range", ""},
                          {"if-unmodified-since", ""},
                          {"last-modified", ""},
                          {"link", ""},
                          {"location", ""},
                          {"max-forwards", ""},
                          {"proxy-authenticate", ""},
                          {"proxy-authorization", ""},
                          {"range", ""},
                          {"referer", ""},
                          {"refresh", ""},
                          {"retry-after", ""},
                          {"server", ""},
                          {"set-cookie", ""},
                          {"strict-transport-security", ""},
                          {"transfer-encoding", ""},
                          {"user-agent", ""},
                          {"vary", ""},
                          {"via", ""},
                          {"www-authenticate", ""}};

#define HPACK_STATIC_TABLE_SIZE (sizeof(hpack_static_table) / sizeof(hpack_static_table[0]) - 1)

/* ========== HPACK Integer Encoding/Decoding ========== */

// Encode integer (RFC 7541 5.1)
static int hpack_encode_int(uint8_t *buf, size_t buf_len, uint64_t value, int prefix_bits) {
    if (buf_len == 0)
        return -1;

    int max_prefix = (1 << prefix_bits) - 1;

    if (value < (uint64_t) max_prefix) {
        buf[0] |= (uint8_t) value;
        return 1;
    }

    buf[0] |= max_prefix;
    value -= max_prefix;
    int len = 1;

    while (value >= 128 && len < (int) buf_len) {
        buf[len++] = (uint8_t) ((value & 0x7f) | 0x80);
        value >>= 7;
    }

    if (len >= (int) buf_len)
        return -1;
    buf[len++] = (uint8_t) value;

    return len;
}

// Decode integer
static int hpack_decode_int(const uint8_t *buf, size_t buf_len, uint64_t *value, int prefix_bits) {
    if (buf_len == 0)
        return -1;

    int max_prefix = (1 << prefix_bits) - 1;
    *value = buf[0] & max_prefix;

    if (*value < (uint64_t) max_prefix) {
        return 1;
    }

    int shift = 0;
    size_t i = 1;

    while (i < buf_len) {
        *value += ((uint64_t) (buf[i] & 0x7f)) << shift;
        if ((buf[i] & 0x80) == 0) {
            return (int) (i + 1);
        }
        shift += 7;
        i++;
    }

    return -1;  // Incomplete
}

/* ========== HPACK Huffman Decoding (RFC 7541 Section 5.2 / Appendix B) ========== */

// Static Huffman code table, indexed by symbol (0..255) plus EOS at 256.
// Source: RFC 7541 Appendix B, verbatim.
typedef struct {
    uint32_t code;
    uint8_t bits;
} HpackHuffmanEntry;
static const HpackHuffmanEntry hpack_huffman_table[257] = {
    /*   0 */ {0x1ff8, 13},
    {0x7fffd8, 23},
    {0xfffffe2, 28},
    {0xfffffe3, 28},
    /*   4 */ {0xfffffe4, 28},
    {0xfffffe5, 28},
    {0xfffffe6, 28},
    {0xfffffe7, 28},
    /*   8 */ {0xfffffe8, 28},
    {0xffffea, 24},
    {0x3ffffffc, 30},
    {0xfffffe9, 28},
    /*  12 */ {0xfffffea, 28},
    {0x3ffffffd, 30},
    {0xfffffeb, 28},
    {0xfffffec, 28},
    /*  16 */ {0xfffffed, 28},
    {0xfffffee, 28},
    {0xfffffef, 28},
    {0xffffff0, 28},
    /*  20 */ {0xffffff1, 28},
    {0xffffff2, 28},
    {0x3ffffffe, 30},
    {0xffffff3, 28},
    /*  24 */ {0xffffff4, 28},
    {0xffffff5, 28},
    {0xffffff6, 28},
    {0xffffff7, 28},
    /*  28 */ {0xffffff8, 28},
    {0xffffff9, 28},
    {0xffffffa, 28},
    {0xffffffb, 28},
    /*  32 */ {0x14, 6},
    {0x3f8, 10},
    {0x3f9, 10},
    {0xffa, 12},
    /*  36 */ {0x1ff9, 13},
    {0x15, 6},
    {0xf8, 8},
    {0x7fa, 11},
    /*  40 */ {0x3fa, 10},
    {0x3fb, 10},
    {0xf9, 8},
    {0x7fb, 11},
    /*  44 */ {0xfa, 8},
    {0x16, 6},
    {0x17, 6},
    {0x18, 6},
    /*  48 */ {0x0, 5},
    {0x1, 5},
    {0x2, 5},
    {0x19, 6},
    /*  52 */ {0x1a, 6},
    {0x1b, 6},
    {0x1c, 6},
    {0x1d, 6},
    /*  56 */ {0x1e, 6},
    {0x1f, 6},
    {0x5c, 7},
    {0xfb, 8},
    /*  60 */ {0x7ffc, 15},
    {0x20, 6},
    {0xffb, 12},
    {0x3fc, 10},
    /*  64 */ {0x1ffa, 13},
    {0x21, 6},
    {0x5d, 7},
    {0x5e, 7},
    /*  68 */ {0x5f, 7},
    {0x60, 7},
    {0x61, 7},
    {0x62, 7},
    /*  72 */ {0x63, 7},
    {0x64, 7},
    {0x65, 7},
    {0x66, 7},
    /*  76 */ {0x67, 7},
    {0x68, 7},
    {0x69, 7},
    {0x6a, 7},
    /*  80 */ {0x6b, 7},
    {0x6c, 7},
    {0x6d, 7},
    {0x6e, 7},
    /*  84 */ {0x6f, 7},
    {0x70, 7},
    {0x71, 7},
    {0x72, 7},
    /*  88 */ {0xfc, 8},
    {0x73, 7},
    {0xfd, 8},
    {0x1ffb, 13},
    /*  92 */ {0x7fff0, 19},
    {0x1ffc, 13},
    {0x3ffc, 14},
    {0x22, 6},
    /*  96 */ {0x7ffd, 15},
    {0x3, 5},
    {0x23, 6},
    {0x4, 5},
    /* 100 */ {0x24, 6},
    {0x5, 5},
    {0x25, 6},
    {0x26, 6},
    /* 104 */ {0x27, 6},
    {0x6, 5},
    {0x74, 7},
    {0x75, 7},
    /* 108 */ {0x28, 6},
    {0x29, 6},
    {0x2a, 6},
    {0x7, 5},
    /* 112 */ {0x2b, 6},
    {0x76, 7},
    {0x2c, 6},
    {0x8, 5},
    /* 116 */ {0x9, 5},
    {0x2d, 6},
    {0x77, 7},
    {0x78, 7},
    /* 120 */ {0x79, 7},
    {0x7a, 7},
    {0x7b, 7},
    {0x7ffe, 15},
    /* 124 */ {0x7fc, 11},
    {0x3ffd, 14},
    {0x1ffd, 13},
    {0xffffffc, 28},
    /* 128 */ {0xfffe6, 20},
    {0x3fffd2, 22},
    {0xfffe7, 20},
    {0xfffe8, 20},
    /* 132 */ {0x3fffd3, 22},
    {0x3fffd4, 22},
    {0x3fffd5, 22},
    {0x7fffd9, 23},
    /* 136 */ {0x3fffd6, 22},
    {0x7fffda, 23},
    {0x7fffdb, 23},
    {0x7fffdc, 23},
    /* 140 */ {0x7fffdd, 23},
    {0x7fffde, 23},
    {0xffffeb, 24},
    {0x7fffdf, 23},
    /* 144 */ {0xffffec, 24},
    {0xffffed, 24},
    {0x3fffd7, 22},
    {0x7fffe0, 23},
    /* 148 */ {0xffffee, 24},
    {0x7fffe1, 23},
    {0x7fffe2, 23},
    {0x7fffe3, 23},
    /* 152 */ {0x7fffe4, 23},
    {0x1fffdc, 21},
    {0x3fffd8, 22},
    {0x7fffe5, 23},
    /* 156 */ {0x3fffd9, 22},
    {0x7fffe6, 23},
    {0x7fffe7, 23},
    {0xffffef, 24},
    /* 160 */ {0x3fffda, 22},
    {0x1fffdd, 21},
    {0xfffe9, 20},
    {0x3fffdb, 22},
    /* 164 */ {0x3fffdc, 22},
    {0x7fffe8, 23},
    {0x7fffe9, 23},
    {0x1fffde, 21},
    /* 168 */ {0x7fffea, 23},
    {0x3fffdd, 22},
    {0x3fffde, 22},
    {0xfffff0, 24},
    /* 172 */ {0x1fffdf, 21},
    {0x3fffdf, 22},
    {0x7fffeb, 23},
    {0x7fffec, 23},
    /* 176 */ {0x1fffe0, 21},
    {0x1fffe1, 21},
    {0x3fffe0, 22},
    {0x1fffe2, 21},
    /* 180 */ {0x7fffed, 23},
    {0x3fffe1, 22},
    {0x7fffee, 23},
    {0x7fffef, 23},
    /* 184 */ {0xfffea, 20},
    {0x3fffe2, 22},
    {0x3fffe3, 22},
    {0x3fffe4, 22},
    /* 188 */ {0x7ffff0, 23},
    {0x3fffe5, 22},
    {0x3fffe6, 22},
    {0x7ffff1, 23},
    /* 192 */ {0x3ffffe0, 26},
    {0x3ffffe1, 26},
    {0xfffeb, 20},
    {0x7fff1, 19},
    /* 196 */ {0x3fffe7, 22},
    {0x7ffff2, 23},
    {0x3fffe8, 22},
    {0x1ffffec, 25},
    /* 200 */ {0x3ffffe2, 26},
    {0x3ffffe3, 26},
    {0x3ffffe4, 26},
    {0x7ffffde, 27},
    /* 204 */ {0x7ffffdf, 27},
    {0x3ffffe5, 26},
    {0xfffff1, 24},
    {0x1ffffed, 25},
    /* 208 */ {0x7fff2, 19},
    {0x1fffe3, 21},
    {0x3ffffe6, 26},
    {0x7ffffe0, 27},
    /* 212 */ {0x7ffffe1, 27},
    {0x3ffffe7, 26},
    {0x7ffffe2, 27},
    {0xfffff2, 24},
    /* 216 */ {0x1fffe4, 21},
    {0x1fffe5, 21},
    {0x3ffffe8, 26},
    {0x3ffffe9, 26},
    /* 220 */ {0xffffffd, 28},
    {0x7ffffe3, 27},
    {0x7ffffe4, 27},
    {0x7ffffe5, 27},
    /* 224 */ {0xfffec, 20},
    {0xfffff3, 24},
    {0xfffed, 20},
    {0x1fffe6, 21},
    /* 228 */ {0x3fffe9, 22},
    {0x1fffe7, 21},
    {0x1fffe8, 21},
    {0x7ffff3, 23},
    /* 232 */ {0x3fffea, 22},
    {0x3fffeb, 22},
    {0x1ffffee, 25},
    {0x1ffffef, 25},
    /* 236 */ {0xfffff4, 24},
    {0xfffff5, 24},
    {0x3ffffea, 26},
    {0x7ffff4, 23},
    /* 240 */ {0x3ffffeb, 26},
    {0x7ffffe6, 27},
    {0x3ffffec, 26},
    {0x3ffffed, 26},
    /* 244 */ {0x7ffffe7, 27},
    {0x7ffffe8, 27},
    {0x7ffffe9, 27},
    {0x7ffffea, 27},
    /* 248 */ {0x7ffffeb, 27},
    {0xffffffe, 28},
    {0x7ffffec, 27},
    {0x7ffffed, 27},
    /* 252 */ {0x7ffffee, 27},
    {0x7ffffef, 27},
    {0x7fffff0, 27},
    {0x3ffffee, 26},
    /* 256 EOS */ {0x3fffffff, 30}};

// Huffman decoding trie. Internal nodes have sym == -1; leaves carry the
// symbol value. Root lives at index 0. Built once at first decode via
// pthread_once so we pay the ~513-node construction cost exactly once per
// process.
//
// NOTE: these file-scope variables are write-once (populated by
// hpack_huffman_init, guarded by pthread_once) and read-only thereafter.
// They are effectively immutable after process startup and do not violate
// the "no mutable file-scope globals" rule in spirit. A compile-time
// generated table would eliminate even this edge case but the RFC 7541
// Appendix B encoding makes that impractical without a build-time codegen
// step.
typedef struct {
    int16_t left;
    int16_t right;
    int16_t sym;
} HpackHuffmanNode;
#define HPACK_HUFFMAN_TREE_CAP 1024  // 2 * 257 leaves = 514 nodes upper bound
static HpackHuffmanNode hpack_huffman_tree[HPACK_HUFFMAN_TREE_CAP];
static int hpack_huffman_tree_size = 0;
static xr_once_t hpack_huffman_once = XR_ONCE_INITIALIZER;

static void hpack_huffman_init(void) {
    hpack_huffman_tree[0].left = -1;
    hpack_huffman_tree[0].right = -1;
    hpack_huffman_tree[0].sym = -1;
    hpack_huffman_tree_size = 1;

    for (int sym = 0; sym <= 256; sym++) {
        uint32_t code = hpack_huffman_table[sym].code;
        int bits = hpack_huffman_table[sym].bits;
        int cur = 0;
        for (int b = bits - 1; b >= 0; b--) {
            int bit = (code >> b) & 1;
            int16_t *child = bit ? &hpack_huffman_tree[cur].right : &hpack_huffman_tree[cur].left;
            if (*child < 0) {
                if (hpack_huffman_tree_size >= HPACK_HUFFMAN_TREE_CAP)
                    return;
                int16_t idx = (int16_t) hpack_huffman_tree_size++;
                hpack_huffman_tree[idx].left = -1;
                hpack_huffman_tree[idx].right = -1;
                hpack_huffman_tree[idx].sym = -1;
                *child = idx;
            }
            cur = *child;
        }
        hpack_huffman_tree[cur].sym = (int16_t) sym;
    }
}

// Decode a Huffman-encoded byte string (RFC 7541 5.2).
// Returns 0 on success, -1 on any decoding error:
//   - invalid code (no matching symbol)
//   - explicit EOS symbol in data (RFC 7541 5.2 forbids)
//   - padding longer than 7 bits
//   - padding not a strict prefix of the EOS code (i.e. not all 1-bits)
//   - output buffer too small
static int hpack_decode_huffman(const uint8_t *src, size_t src_len, char *dst, size_t dst_cap,
                                size_t *dst_len) {
    xr_once_call(&hpack_huffman_once, hpack_huffman_init);

    int cur = 0;               // current trie node
    size_t out = 0;            // bytes written to dst
    bool pad_all_ones = true;  // true while we've seen only 1-bits since last symbol
    int pad_bits = 0;          // bits consumed since last emitted symbol

    for (size_t i = 0; i < src_len; i++) {
        uint8_t byte = src[i];
        for (int b = 7; b >= 0; b--) {
            int bit = (byte >> b) & 1;
            int16_t next = bit ? hpack_huffman_tree[cur].right : hpack_huffman_tree[cur].left;
            if (next < 0)
                return -1;  // Invalid code path
            cur = next;
            pad_bits++;
            if (!bit)
                pad_all_ones = false;

            int16_t sym = hpack_huffman_tree[cur].sym;
            if (sym < 0)
                continue;  // Internal node, keep walking
            if (sym == 256)
                return -1;  // Explicit EOS in data is forbidden
            if (out >= dst_cap)
                return -1;
            dst[out++] = (char) (uint8_t) sym;

            cur = 0;
            pad_bits = 0;
            pad_all_ones = true;
        }
    }
    // End-of-input: any residual walked bits are padding. RFC 7541 5.2 requires
    // padding to be <= 7 bits AND equal to the MSBs of the EOS code (all ones).
    if (cur != 0) {
        if (pad_bits > 7)
            return -1;
        if (!pad_all_ones)
            return -1;
    }
    if (dst_len)
        *dst_len = out;
    return 0;
}

/* ========== HPACK String Encoding/Decoding ========== */

// Encode string (without Huffman)
static int hpack_encode_string(uint8_t *buf, size_t buf_len, const char *str, size_t str_len) {
    buf[0] = 0;  // Not using Huffman
    int int_len = hpack_encode_int(buf, buf_len, str_len, 7);
    if (int_len < 0)
        return -1;

    if ((size_t) int_len + str_len > buf_len)
        return -1;

    memcpy(buf + int_len, str, str_len);
    return int_len + (int) str_len;
}

// Decode string (RFC 7541 5.2)
static int hpack_decode_string(const uint8_t *buf, size_t buf_len, char **str, size_t *str_len) {
    if (buf_len == 0)
        return -1;

    bool huffman = (buf[0] & 0x80) != 0;
    uint64_t len;
    int int_len = hpack_decode_int(buf, buf_len, &len, 7);
    if (int_len < 0)
        return -1;

    if ((size_t) int_len + len > buf_len)
        return -1;

    if (huffman) {
        // Huffman-encoded. Shortest code is 5 bits, so each input byte expands
        // to at most 8/5 = 1.6 output bytes; allocate 2x for safety headroom.
        size_t max_out = (size_t) len * 2 + 1;
        char *out = (char *) xr_malloc(max_out);
        if (!out)
            return -1;
        size_t decoded_len = 0;
        if (hpack_decode_huffman(buf + int_len, (size_t) len, out, max_out - 1, &decoded_len) !=
            0) {
            xr_free(out);
            return -1;
        }
        out[decoded_len] = '\0';
        *str = out;
        *str_len = decoded_len;
    } else {
        *str_len = (size_t) len;
        *str = (char *) xr_malloc(len + 1);
        if (!*str)
            return -1;
        memcpy(*str, buf + int_len, len);
        (*str)[len] = '\0';
    }

    return int_len + (int) len;
}

/* ========== HPACK Dynamic Table ========== */

void xr_hpack_init(XrHpackTable *table, size_t max_size) {
    memset(table, 0, sizeof(XrHpackTable));
    table->max_size = max_size;
}

void xr_hpack_free(XrHpackTable *table) {
    XrHpackEntry *entry = table->entries;
    while (entry) {
        XrHpackEntry *next = entry->next;
        xr_free(entry->name);
        xr_free(entry->value);
        xr_free(entry);
        entry = next;
    }
    table->entries = NULL;
    table->tail = NULL;
    table->size = 0;
    table->count = 0;
}

// Unlink and free the tail (oldest) entry. O(1) via prev pointer.
static void hpack_table_evict_one(XrHpackTable *table) {
    XrHpackEntry *victim = table->tail;
    if (!victim)
        return;

    // Detach from doubly-linked list
    if (victim->prev) {
        victim->prev->next = NULL;
        table->tail = victim->prev;
    } else {
        // Single-element list
        table->entries = NULL;
        table->tail = NULL;
    }

    table->size -= victim->name_len + victim->value_len + 32;
    table->count--;
    xr_free(victim->name);
    xr_free(victim->value);
    xr_free(victim);
}

// Add entry to dynamic table (RFC 7541 §4.4).
// New entries go to head; eviction removes from tail — O(1) in both
// directions thanks to the doubly-linked list.
static void hpack_table_add(XrHpackTable *table, const char *name, size_t name_len,
                            const char *value, size_t value_len) {
    size_t entry_size = name_len + value_len + 32;  // RFC 7541: 32 bytes overhead

    // Evict oldest entries until enough space (O(1) per eviction)
    while (table->size + entry_size > table->max_size && table->tail) {
        hpack_table_evict_one(table);
    }

    if (entry_size > table->max_size) {
        // Entry too large, clear table (RFC 7541 §4.4)
        xr_hpack_free(table);
        return;
    }

    // Create new entry
    XrHpackEntry *entry = (XrHpackEntry *) xr_calloc(1, sizeof(XrHpackEntry));
    if (!entry)
        return;

    entry->name = (char *) xr_malloc(name_len + 1);
    entry->value = (char *) xr_malloc(value_len + 1);
    if (!entry->name || !entry->value) {
        xr_free(entry->name);
        xr_free(entry->value);
        xr_free(entry);
        return;
    }

    memcpy(entry->name, name, name_len);
    entry->name[name_len] = '\0';
    entry->name_len = name_len;

    memcpy(entry->value, value, value_len);
    entry->value[value_len] = '\0';
    entry->value_len = value_len;

    // Insert at head of doubly-linked list
    entry->prev = NULL;
    entry->next = table->entries;
    if (table->entries) {
        table->entries->prev = entry;
    } else {
        table->tail = entry;  // First entry is also the tail
    }
    table->entries = entry;
    table->size += entry_size;
    table->count++;
}

// Find dynamic table entry
static XrHpackEntry *hpack_table_get(XrHpackTable *table, int index) {
    index -= HPACK_STATIC_TABLE_SIZE + 1;
    if (index < 0)
        return NULL;

    XrHpackEntry *entry = table->entries;
    while (entry && index > 0) {
        entry = entry->next;
        index--;
    }
    return entry;
}

/* ========== HPACK Encoding ========== */

int xr_hpack_encode(XrHpackTable *table, const char *name, size_t name_len, const char *value,
                    size_t value_len, uint8_t *buf, size_t buf_len) {
    (void) table;
    // Simplified: use literal without indexing
    if (buf_len < 1)
        return -1;

    buf[0] = 0x00;  // Literal without indexing, new name
    int total = 1;

    // Encode name
    int len = hpack_encode_string(buf + total, buf_len - total, name, name_len);
    if (len < 0)
        return -1;
    total += len;

    // Encode value
    len = hpack_encode_string(buf + total, buf_len - total, value, value_len);
    if (len < 0)
        return -1;
    total += len;

    return total;
}

/* ========== HPACK Decoding ========== */

int xr_hpack_decode(XrHpackTable *table, const uint8_t *buf, size_t buf_len,
                    void (*callback)(const char *name, size_t name_len, const char *value,
                                     size_t value_len, void *user_data),
                    void *user_data) {
    size_t pos = 0;

    while (pos < buf_len) {
        uint8_t b = buf[pos];
        char *name = NULL, *value = NULL;
        size_t name_len = 0, value_len = 0;
        bool add_to_table = false;

        if (b & 0x80) {
            // Indexed header field (RFC 7541 6.1)
            uint64_t index;
            int len = hpack_decode_int(buf + pos, buf_len - pos, &index, 7);
            if (len < 0)
                return -1;
            pos += len;

            if (index <= HPACK_STATIC_TABLE_SIZE) {
                name = (char *) hpack_static_table[index].name;
                name_len = strlen(name);
                value = (char *) hpack_static_table[index].value;
                value_len = strlen(value);
            } else {
                XrHpackEntry *entry = hpack_table_get(table, (int) index);
                if (!entry)
                    return -1;
                name = entry->name;
                name_len = entry->name_len;
                value = entry->value;
                value_len = entry->value_len;
            }

            if (callback)
                callback(name, name_len, value, value_len, user_data);

        } else if (b & 0x40) {
            // Literal with incremental indexing (RFC 7541 6.2.1)
            uint64_t index;
            int len = hpack_decode_int(buf + pos, buf_len - pos, &index, 6);
            if (len < 0)
                return -1;
            pos += len;

            if (index == 0) {
                // New name
                len = hpack_decode_string(buf + pos, buf_len - pos, &name, &name_len);
                if (len < 0)
                    return -1;
                pos += len;
            } else if (index <= HPACK_STATIC_TABLE_SIZE) {
                name = xr_strdup(hpack_static_table[index].name);
                name_len = strlen(name);
            } else {
                XrHpackEntry *entry = hpack_table_get(table, (int) index);
                if (!entry)
                    return -1;
                name = xr_strdup(entry->name);
                name_len = entry->name_len;
            }

            len = hpack_decode_string(buf + pos, buf_len - pos, &value, &value_len);
            if (len < 0) {
                xr_free(name);
                return -1;
            }
            pos += len;

            add_to_table = true;
            if (callback)
                callback(name, name_len, value, value_len, user_data);

            if (add_to_table) {
                hpack_table_add(table, name, name_len, value, value_len);
            }
            xr_free(name);
            xr_free(value);

        } else if (b & 0x20) {
            // Dynamic table size update (RFC 7541 6.3)
            uint64_t new_size;
            int len = hpack_decode_int(buf + pos, buf_len - pos, &new_size, 5);
            if (len < 0)
                return -1;
            pos += len;
            table->max_size = new_size;

        } else {
            // Literal without indexing (RFC 7541 6.2.2/6.2.3)
            uint64_t index;
            int prefix = (b & 0x10) ? 4 : 4;
            int len = hpack_decode_int(buf + pos, buf_len - pos, &index, prefix);
            if (len < 0)
                return -1;
            pos += len;

            if (index == 0) {
                len = hpack_decode_string(buf + pos, buf_len - pos, &name, &name_len);
                if (len < 0)
                    return -1;
                pos += len;
            } else if (index <= HPACK_STATIC_TABLE_SIZE) {
                name = xr_strdup(hpack_static_table[index].name);
                name_len = strlen(name);
            } else {
                XrHpackEntry *entry = hpack_table_get(table, (int) index);
                if (!entry)
                    return -1;
                name = xr_strdup(entry->name);
                name_len = entry->name_len;
            }

            len = hpack_decode_string(buf + pos, buf_len - pos, &value, &value_len);
            if (len < 0) {
                xr_free(name);
                return -1;
            }
            pos += len;

            if (callback)
                callback(name, name_len, value, value_len, user_data);
            xr_free(name);
            xr_free(value);
        }
    }

    return 0;
}

/* ========== Frame Header Parsing/Generation ========== */

int xr_h2_parse_frame_header(const uint8_t *buf, XrH2FrameHeader *header) {
    header->length = ((uint32_t) buf[0] << 16) | ((uint32_t) buf[1] << 8) | buf[2];
    header->type = buf[3];
    header->flags = buf[4];
    header->stream_id =
        ((uint32_t) buf[5] << 24) | ((uint32_t) buf[6] << 16) | ((uint32_t) buf[7] << 8) | buf[8];
    header->stream_id &= 0x7FFFFFFF;  // Clear reserved bit
    return 0;
}

void xr_h2_write_frame_header(uint8_t *buf, const XrH2FrameHeader *header) {
    buf[0] = (header->length >> 16) & 0xFF;
    buf[1] = (header->length >> 8) & 0xFF;
    buf[2] = header->length & 0xFF;
    buf[3] = header->type;
    buf[4] = header->flags;
    buf[5] = (header->stream_id >> 24) & 0x7F;
    buf[6] = (header->stream_id >> 16) & 0xFF;
    buf[7] = (header->stream_id >> 8) & 0xFF;
    buf[8] = header->stream_id & 0xFF;
}

/* ========== HTTP/2 Connection ========== */

XrH2Conn *xr_h2_conn_new_client(int fd, void *tls_conn) {
    XrH2Conn *conn = (XrH2Conn *) xr_calloc(1, sizeof(XrH2Conn));
    if (!conn)
        return NULL;

    conn->fd = fd;
    conn->tls_conn = tls_conn;
    conn->is_client = true;
    conn->next_stream_id = 1;  // Client uses odd stream IDs
    conn->connection_window = XR_H2_DEFAULT_INITIAL_WINDOW_SIZE;

    // Default settings
    conn->local_settings[XR_H2_SETTINGS_HEADER_TABLE_SIZE] = XR_H2_DEFAULT_HEADER_TABLE_SIZE;
    conn->local_settings[XR_H2_SETTINGS_ENABLE_PUSH] = 0;  // Client disables push
    conn->local_settings[XR_H2_SETTINGS_MAX_CONCURRENT_STREAMS] =
        XR_H2_DEFAULT_MAX_CONCURRENT_STREAMS;
    conn->local_settings[XR_H2_SETTINGS_INITIAL_WINDOW_SIZE] = XR_H2_DEFAULT_INITIAL_WINDOW_SIZE;
    conn->local_settings[XR_H2_SETTINGS_MAX_FRAME_SIZE] = XR_H2_DEFAULT_MAX_FRAME_SIZE;

    memcpy(conn->remote_settings, conn->local_settings, sizeof(conn->local_settings));

    // Initialize HPACK tables
    xr_hpack_init(&conn->encoder_table, XR_H2_DEFAULT_HEADER_TABLE_SIZE);
    xr_hpack_init(&conn->decoder_table, XR_H2_DEFAULT_HEADER_TABLE_SIZE);

    // Allocate receive buffer
    conn->recv_cap = 16384;
    conn->recv_buf = (char *) xr_malloc(conn->recv_cap);
    if (!conn->recv_buf) {
        xr_free(conn);
        return NULL;
    }

    return conn;
}

void xr_h2_conn_free(XrH2Conn *conn) {
    if (!conn)
        return;

    xr_hpack_free(&conn->encoder_table);
    xr_hpack_free(&conn->decoder_table);

    // Free stream hash table
    xr_h2_stream_hash_free(&conn->stream_hash);

    xr_free(conn->recv_buf);
    xr_free(conn);
}

// Send data
static int h2_send(XrH2Conn *conn, const void *buf, size_t len) {
    // Simplified: use write directly
    return (int) write(conn->fd, buf, len);
}

int xr_h2_conn_init(XrH2Conn *conn) {
    if (!conn)
        return -1;

    // Send connection preface
    if (conn->is_client) {
        if (h2_send(conn, XR_HTTP2_PREFACE, XR_HTTP2_PREFACE_LEN) < 0) {
            return -1;
        }
    }

    // Send SETTINGS
    return xr_h2_send_settings(conn);
}

int xr_h2_send_settings(XrH2Conn *conn) {
    uint8_t frame[XR_H2_FRAME_HEADER_SIZE + 36];
    uint8_t *payload = frame + XR_H2_FRAME_HEADER_SIZE;
    int payload_len = 0;

// Encode settings parameters
#define ADD_SETTING(id, val)                                                                       \
    do {                                                                                           \
        payload[payload_len++] = ((id) >> 8) & 0xFF;                                               \
        payload[payload_len++] = (id) & 0xFF;                                                      \
        payload[payload_len++] = ((val) >> 24) & 0xFF;                                             \
        payload[payload_len++] = ((val) >> 16) & 0xFF;                                             \
        payload[payload_len++] = ((val) >> 8) & 0xFF;                                              \
        payload[payload_len++] = (val) & 0xFF;                                                     \
    } while (0)

    ADD_SETTING(XR_H2_SETTINGS_MAX_CONCURRENT_STREAMS,
                conn->local_settings[XR_H2_SETTINGS_MAX_CONCURRENT_STREAMS]);
    ADD_SETTING(XR_H2_SETTINGS_INITIAL_WINDOW_SIZE,
                conn->local_settings[XR_H2_SETTINGS_INITIAL_WINDOW_SIZE]);
    ADD_SETTING(XR_H2_SETTINGS_MAX_FRAME_SIZE, conn->local_settings[XR_H2_SETTINGS_MAX_FRAME_SIZE]);

    if (conn->is_client) {
        ADD_SETTING(XR_H2_SETTINGS_ENABLE_PUSH, 0);
    }

#undef ADD_SETTING

    // Write frame header
    XrH2FrameHeader header = {
        .length = payload_len, .type = XR_H2_FRAME_SETTINGS, .flags = 0, .stream_id = 0};
    xr_h2_write_frame_header(frame, &header);

    conn->settings_sent = true;
    return h2_send(conn, frame, XR_H2_FRAME_HEADER_SIZE + payload_len);
}

int xr_h2_send_settings_ack(XrH2Conn *conn) {
    uint8_t frame[XR_H2_FRAME_HEADER_SIZE];
    XrH2FrameHeader header = {
        .length = 0, .type = XR_H2_FRAME_SETTINGS, .flags = XR_H2_FLAG_ACK, .stream_id = 0};
    xr_h2_write_frame_header(frame, &header);
    return h2_send(conn, frame, XR_H2_FRAME_HEADER_SIZE);
}

/* ========== Stream Hash Table Implementation ========== */

// Hash a stream ID into a bucket index. Uses xr_hash_bytes from
// xhash.h (FNV-1a) and masks to nbuckets (always a power of 2).
// Plain modulo of sequential odd IDs (1,3,5,...) against a power-of-2
// size wastes half the buckets; FNV-1a scatters them properly.
static inline uint32_t stream_hash_func(uint32_t stream_id, uint32_t nbuckets) {
    uint32_t h = xr_hash_bytes(&stream_id, sizeof(stream_id));
    return h & (nbuckets - 1);
}

void xr_h2_stream_hash_init(XrH2StreamHash *hash) {
    if (!hash)
        return;
    hash->nbuckets = XR_H2_STREAM_HASH_INIT_CAP;
    hash->buckets = (XrH2Stream **) xr_calloc(hash->nbuckets, sizeof(XrH2Stream *));
    hash->count = 0;
}

// Grow the bucket array by 2x and rehash all streams.
static void stream_hash_resize(XrH2StreamHash *hash) {
    uint32_t new_cap = hash->nbuckets * 2;
    XrH2Stream **new_buckets = (XrH2Stream **) xr_calloc(new_cap, sizeof(XrH2Stream *));
    if (!new_buckets)
        return;  // OOM: keep old table, accept degradation

    // Rehash every stream into the new bucket array
    for (uint32_t i = 0; i < hash->nbuckets; i++) {
        XrH2Stream *s = hash->buckets[i];
        while (s) {
            XrH2Stream *next = s->next;
            uint32_t idx = stream_hash_func(s->id, new_cap);
            s->next = new_buckets[idx];
            new_buckets[idx] = s;
            s = next;
        }
    }

    xr_free(hash->buckets);
    hash->buckets = new_buckets;
    hash->nbuckets = new_cap;
}

void xr_h2_stream_hash_add(XrH2StreamHash *hash, XrH2Stream *stream) {
    if (!hash || !stream)
        return;

    // Resize when load factor exceeds 75%
    if (hash->buckets &&
        hash->count * XR_H2_STREAM_HASH_LOAD_DEN >= hash->nbuckets * XR_H2_STREAM_HASH_LOAD_NUM) {
        stream_hash_resize(hash);
    }

    if (!hash->buckets)
        return;  // init failed or OOM
    uint32_t idx = stream_hash_func(stream->id, hash->nbuckets);
    stream->next = hash->buckets[idx];
    hash->buckets[idx] = stream;
    hash->count++;
}

XrH2Stream *xr_h2_stream_hash_find(XrH2StreamHash *hash, uint32_t stream_id) {
    if (!hash || !hash->buckets)
        return NULL;
    uint32_t idx = stream_hash_func(stream_id, hash->nbuckets);
    XrH2Stream *stream = hash->buckets[idx];
    while (stream) {
        if (stream->id == stream_id)
            return stream;
        stream = stream->next;
    }
    return NULL;
}

void xr_h2_stream_hash_remove(XrH2StreamHash *hash, uint32_t stream_id) {
    if (!hash || !hash->buckets)
        return;
    uint32_t idx = stream_hash_func(stream_id, hash->nbuckets);
    XrH2Stream **pp = &hash->buckets[idx];
    while (*pp) {
        if ((*pp)->id == stream_id) {
            XrH2Stream *to_remove = *pp;
            *pp = to_remove->next;
            hash->count--;
            return;
        }
        pp = &(*pp)->next;
    }
}

void xr_h2_stream_hash_free(XrH2StreamHash *hash) {
    if (!hash)
        return;
    if (hash->buckets) {
        for (uint32_t i = 0; i < hash->nbuckets; i++) {
            XrH2Stream *stream = hash->buckets[i];
            while (stream) {
                XrH2Stream *next = stream->next;
                // Free stream resources
                xr_free(stream->headers_buf);
                xr_free(stream->data_buf);
                xr_free(stream->trailers_buf);
                xr_free(stream);
                stream = next;
            }
        }
        xr_free(hash->buckets);
        hash->buckets = NULL;
    }
    hash->nbuckets = 0;
    hash->count = 0;
}

/* ========== Stream Management ========== */

XrH2Stream *xr_h2_stream_new(XrH2Conn *conn) {
    if (!conn)
        return NULL;

    XrH2Stream *stream = (XrH2Stream *) xr_calloc(1, sizeof(XrH2Stream));
    if (!stream)
        return NULL;

    stream->id = conn->next_stream_id;
    conn->next_stream_id += 2;  // Skip even numbers (server push)
    stream->state = XR_H2_STREAM_IDLE;
    stream->window_size = conn->remote_settings[XR_H2_SETTINGS_INITIAL_WINDOW_SIZE];

    // Add to stream hash table
    xr_h2_stream_hash_add(&conn->stream_hash, stream);

    return stream;
}

XrH2Stream *xr_h2_get_stream(XrH2Conn *conn, uint32_t stream_id) {
    if (!conn)
        return NULL;
    return xr_h2_stream_hash_find(&conn->stream_hash, stream_id);
}

// Receive data
static int h2_recv(XrH2Conn *conn, void *buf, size_t len) {
    if (conn->tls_conn) {
        return xr_tls_conn_read(conn->tls_conn, buf, len);
    }
    return (int) read(conn->fd, buf, len);
}

// HPACK header decode callback: extract :status pseudo-header
static void h2_header_callback(const char *name, size_t name_len, const char *value,
                               size_t value_len, void *user_data) {
    XrH2Stream *stream = (XrH2Stream *) user_data;
    if (name_len == 7 && memcmp(name, ":status", 7) == 0 && value_len > 0) {
        int status = 0;
        for (size_t i = 0; i < value_len && value[i] >= '0' && value[i] <= '9'; i++) {
            status = status * 10 + (value[i] - '0');
        }
        stream->status = status;
    }
}

// Receive and process frame
int xr_h2_recv(XrH2Conn *conn) {
    if (!conn)
        return -1;

    // Receive frame header
    uint8_t frame_header[XR_H2_FRAME_HEADER_SIZE];
    int n = h2_recv(conn, frame_header, XR_H2_FRAME_HEADER_SIZE);
    if (n != XR_H2_FRAME_HEADER_SIZE)
        return -1;

    XrH2FrameHeader header;
    xr_h2_parse_frame_header(frame_header, &header);

    // Receive frame payload
    uint8_t *payload = NULL;
    if (header.length > 0) {
        payload = (uint8_t *) xr_malloc(header.length);
        if (!payload)
            return -1;

        n = h2_recv(conn, payload, header.length);
        if (n != (int) header.length) {
            xr_free(payload);
            return -1;
        }
    }

    // Process frame
    int result = 0;
    switch (header.type) {
        case XR_H2_FRAME_SETTINGS:
            if (header.flags & XR_H2_FLAG_ACK) {
                conn->settings_acked = true;
            } else {
                // Parse SETTINGS
                for (uint32_t i = 0; i + 6 <= header.length; i += 6) {
                    uint16_t id = (payload[i] << 8) | payload[i + 1];
                    uint32_t val = (payload[i + 2] << 24) | (payload[i + 3] << 16) |
                                   (payload[i + 4] << 8) | payload[i + 5];
                    if (id < 7)
                        conn->remote_settings[id] = val;
                }
                xr_h2_send_settings_ack(conn);
            }
            break;

        case XR_H2_FRAME_HEADERS: {
            XrH2Stream *stream = xr_h2_get_stream(conn, header.stream_id);
            if (!stream) {
                stream = (XrH2Stream *) xr_calloc(1, sizeof(XrH2Stream));
                if (stream) {
                    stream->id = header.stream_id;
                    xr_h2_stream_hash_add(&conn->stream_hash, stream);
                }
            }
            // RFC 7540 6.2: strip optional Pad Length + Exclusive/Stream-Dep/
            // Weight fields before the Header Block Fragment. Without this the
            // HPACK decoder sees the priority/padding bytes as literal header
            // bytes and produces garbage (or returns -1, silently dropping all
            // headers including :status).
            const uint8_t *hdr_ptr = payload;
            uint32_t hdr_len = header.length;
            if (header.flags & XR_H2_FLAG_PADDED) {
                if (hdr_len < 1) {
                    result = -1;
                    break;
                }
                uint32_t pad_len = hdr_ptr[0];
                if (pad_len + 1 > hdr_len) {
                    result = -1;
                    break;
                }
                hdr_ptr += 1;
                hdr_len -= 1 + pad_len;
            }
            if (header.flags & XR_H2_FLAG_PRIORITY) {
                if (hdr_len < 5) {
                    result = -1;
                    break;
                }
                hdr_ptr += 5;
                hdr_len -= 5;
            }
            // Decode HPACK headers to extract :status pseudo-header
            if (stream && hdr_ptr && hdr_len > 0) {
                xr_hpack_decode(&conn->decoder_table, hdr_ptr, hdr_len, h2_header_callback, stream);
            }
            if (stream && header.flags & XR_H2_FLAG_END_STREAM) {
                stream->state = XR_H2_STREAM_HALF_CLOSED_REMOTE;
            }
            break;
        }

        case XR_H2_FRAME_DATA: {
            XrH2Stream *stream = xr_h2_get_stream(conn, header.stream_id);
            // RFC 7540 6.1: if PADDED flag set, first octet is Pad Length and
            // the last `pad_len` octets are padding that must NOT be delivered
            // to the application. Prior code pass-through made HTTP body carry
            // trailing garbage whenever the peer padded (e.g. HTTP/2 clients
            // that anti-fingerprint via random padding).
            const uint8_t *data_ptr = payload;
            uint32_t data_len = header.length;
            if (header.flags & XR_H2_FLAG_PADDED) {
                if (data_len < 1) {
                    result = -1;
                    break;
                }
                uint32_t pad_len = data_ptr[0];
                if (pad_len + 1 > data_len) {
                    result = -1;
                    break;
                }
                data_ptr += 1;
                data_len -= 1 + pad_len;
            }
            if (stream && data_ptr && data_len > 0) {
                // Append data to stream buffer. Use a temporary pointer so the
                // original stream->data_buf is preserved on realloc failure.
                size_t new_len = stream->data_len + data_len;
                char *new_buf = (char *) xr_realloc(stream->data_buf, new_len + 1);
                if (!new_buf) {
                    // Propagate OOM instead of silently dropping body bytes.
                    result = -1;
                    break;
                }
                memcpy(new_buf + stream->data_len, data_ptr, data_len);
                new_buf[new_len] = '\0';
                stream->data_buf = new_buf;
                stream->data_len = new_len;
            }
            if (stream && header.flags & XR_H2_FLAG_END_STREAM) {
                stream->state = XR_H2_STREAM_HALF_CLOSED_REMOTE;
            }
            // RFC 7540 6.9.1: flow-control accounts for the *entire* DATA
            // payload including Pad Length and padding octets, not just the
            // useful body bytes. So credit back header.length, not data_len.
            if (header.length > 0) {
                xr_h2_send_window_update(conn, 0, header.length);
                xr_h2_send_window_update(conn, header.stream_id, header.length);
            }
            break;
        }

        case XR_H2_FRAME_GOAWAY:
            conn->goaway_received = true;
            break;

        case XR_H2_FRAME_PING:
            if (!(header.flags & XR_H2_FLAG_ACK)) {
                xr_h2_send_ping(conn, payload, true);
            }
            break;

        case XR_H2_FRAME_RST_STREAM: {
            XrH2Stream *stream = xr_h2_get_stream(conn, header.stream_id);
            if (stream) {
                stream->state = XR_H2_STREAM_STATE_CLOSED;
                stream->cancelled = true;
            }
            break;
        }

        case XR_H2_FRAME_WINDOW_UPDATE: {
            // RFC 7540 §6.9: WINDOW_UPDATE payload is exactly 4 bytes.
            if (header.length != 4 || !payload) {
                result = -1;
                break;
            }
            uint32_t inc = ((uint32_t) payload[0] << 24) | ((uint32_t) payload[1] << 16) |
                           ((uint32_t) payload[2] << 8) | (uint32_t) payload[3];
            // RFC 7540 §6.9: increment of 0 is a PROTOCOL_ERROR.
            inc &= 0x7FFFFFFFU;  // Clear reserved bit (RFC 7540 §6.9)
            if (inc == 0) {
                if (header.stream_id == 0) {
                    xr_h2_send_goaway(conn, 0, XR_H2_PROTOCOL_ERROR);
                } else {
                    xr_h2_send_rst_stream(conn, header.stream_id, XR_H2_PROTOCOL_ERROR);
                }
                result = -1;
                break;
            }
            if (header.stream_id == 0) {
                // RFC 7540 §6.9.1: connection window must not exceed
                // 2^31-1. Overflow → FLOW_CONTROL_ERROR on connection.
                if ((int64_t) conn->connection_window + inc > 0x7FFFFFFF) {
                    xr_h2_send_goaway(conn, 0, XR_H2_FLOW_CONTROL_ERROR);
                    result = -1;
                    break;
                }
                conn->connection_window += (int32_t) inc;
            } else {
                XrH2Stream *stream = xr_h2_get_stream(conn, header.stream_id);
                if (stream) {
                    // RFC 7540 §6.9.1: stream window must not exceed
                    // 2^31-1. Overflow → RST_STREAM FLOW_CONTROL_ERROR.
                    if ((int64_t) stream->window_size + inc > 0x7FFFFFFF) {
                        xr_h2_send_rst_stream(conn, header.stream_id, XR_H2_FLOW_CONTROL_ERROR);
                        result = -1;
                        break;
                    }
                    stream->window_size += (int32_t) inc;
                }
            }
            break;
        }

        default:
            break;
    }

    xr_free(payload);
    return result;
}

/* ========== Send Frames ========== */

int xr_h2_send_headers(XrH2Conn *conn, XrH2Stream *stream, const char **names,
                       const size_t *name_lens, const char **values, const size_t *value_lens,
                       int count, bool end_stream) {
    if (!conn || !stream)
        return -1;

    // Encode headers. Zero-initialise so that analyzers can prove the
    // subsequent memcpy only reads bytes that were written by
    // xr_hpack_encode; the cost is negligible compared to the network I/O.
    uint8_t headers_buf[16384] = {0};
    int headers_len = 0;

    for (int i = 0; i < count; i++) {
        int len =
            xr_hpack_encode(&conn->encoder_table, names[i], name_lens[i], values[i], value_lens[i],
                            headers_buf + headers_len, sizeof(headers_buf) - headers_len);
        if (len < 0)
            return -1;
        headers_len += len;
    }

    // Send HEADERS frame
    uint8_t frame[XR_H2_FRAME_HEADER_SIZE + 16384];
    XrH2FrameHeader header = {.length = headers_len,
                              .type = XR_H2_FRAME_HEADERS,
                              .flags =
                                  XR_H2_FLAG_END_HEADERS | (end_stream ? XR_H2_FLAG_END_STREAM : 0),
                              .stream_id = stream->id};
    xr_h2_write_frame_header(frame, &header);
    if (headers_len > 0) {
        memcpy(frame + XR_H2_FRAME_HEADER_SIZE, headers_buf, (size_t) headers_len);
    }

    stream->state = XR_H2_STREAM_OPEN;
    return h2_send(conn, frame, XR_H2_FRAME_HEADER_SIZE + headers_len);
}

// Receive until stream closes, return response data
int xr_h2_recv_stream_data(XrH2Conn *conn, XrH2Stream *stream, char **out_data, size_t *out_len) {
    if (!conn || !stream)
        return -1;

    // Receive until stream closes
    while (stream->state != XR_H2_STREAM_STATE_CLOSED &&
           stream->state != XR_H2_STREAM_HALF_CLOSED_REMOTE) {
        if (xr_h2_recv(conn) < 0)
            return -1;
    }

    // Copy response body
    if (out_data && stream->data_buf && stream->data_len > 0) {
        *out_data = (char *) xr_malloc(stream->data_len + 1);
        if (*out_data) {
            memcpy(*out_data, stream->data_buf, stream->data_len);
            (*out_data)[stream->data_len] = '\0';
            if (out_len)
                *out_len = stream->data_len;
        }
    }

    return 0;
}

int xr_h2_send_data(XrH2Conn *conn, XrH2Stream *stream, const void *data, size_t len,
                    bool end_stream) {
    if (!conn || !stream)
        return -1;

    // Send in fragments (respecting frame size limit)
    size_t max_frame = conn->remote_settings[XR_H2_SETTINGS_MAX_FRAME_SIZE];
    size_t sent = 0;

    while (sent < len) {
        size_t chunk = len - sent;
        if (chunk > max_frame)
            chunk = max_frame;

        uint8_t frame[XR_H2_FRAME_HEADER_SIZE];
        XrH2FrameHeader header = {
            .length = (uint32_t) chunk,
            .type = XR_H2_FRAME_DATA,
            .flags = (sent + chunk >= len && end_stream) ? XR_H2_FLAG_END_STREAM : 0,
            .stream_id = stream->id};
        xr_h2_write_frame_header(frame, &header);

        if (h2_send(conn, frame, XR_H2_FRAME_HEADER_SIZE) < 0)
            return -1;
        if (h2_send(conn, (const char *) data + sent, chunk) < 0)
            return -1;

        sent += chunk;
    }

    if (end_stream) {
        stream->state = XR_H2_STREAM_HALF_CLOSED_LOCAL;
    }

    return (int) sent;
}

int xr_h2_send_goaway(XrH2Conn *conn, uint32_t last_stream_id, XrH2ErrorCode error) {
    uint8_t frame[XR_H2_FRAME_HEADER_SIZE + 8];
    XrH2FrameHeader header = {.length = 8, .type = XR_H2_FRAME_GOAWAY, .flags = 0, .stream_id = 0};
    xr_h2_write_frame_header(frame, &header);

    uint8_t *payload = frame + XR_H2_FRAME_HEADER_SIZE;
    payload[0] = (last_stream_id >> 24) & 0x7F;
    payload[1] = (last_stream_id >> 16) & 0xFF;
    payload[2] = (last_stream_id >> 8) & 0xFF;
    payload[3] = last_stream_id & 0xFF;
    payload[4] = (error >> 24) & 0xFF;
    payload[5] = (error >> 16) & 0xFF;
    payload[6] = (error >> 8) & 0xFF;
    payload[7] = error & 0xFF;

    conn->goaway_sent = true;
    return h2_send(conn, frame, XR_H2_FRAME_HEADER_SIZE + 8);
}

int xr_h2_send_window_update(XrH2Conn *conn, uint32_t stream_id, uint32_t increment) {
    uint8_t frame[XR_H2_FRAME_HEADER_SIZE + 4];
    XrH2FrameHeader header = {
        .length = 4, .type = XR_H2_FRAME_WINDOW_UPDATE, .flags = 0, .stream_id = stream_id};
    xr_h2_write_frame_header(frame, &header);

    uint8_t *payload = frame + XR_H2_FRAME_HEADER_SIZE;
    payload[0] = (increment >> 24) & 0x7F;
    payload[1] = (increment >> 16) & 0xFF;
    payload[2] = (increment >> 8) & 0xFF;
    payload[3] = increment & 0xFF;

    return h2_send(conn, frame, XR_H2_FRAME_HEADER_SIZE + 4);
}

/* ========== Additional API ========== */

XrH2Conn *xr_h2_conn_new(int fd, void *tls_conn, bool is_client) {
    XrH2Conn *conn = (XrH2Conn *) xr_calloc(1, sizeof(XrH2Conn));
    if (!conn)
        return NULL;

    conn->fd = fd;
    conn->tls_conn = tls_conn;
    conn->is_client = is_client;

    // Initialize settings to default values
    conn->local_settings[XR_H2_SETTINGS_HEADER_TABLE_SIZE] = XR_H2_DEFAULT_HEADER_TABLE_SIZE;
    conn->local_settings[XR_H2_SETTINGS_ENABLE_PUSH] = 0;
    conn->local_settings[XR_H2_SETTINGS_MAX_CONCURRENT_STREAMS] =
        XR_H2_DEFAULT_MAX_CONCURRENT_STREAMS;
    conn->local_settings[XR_H2_SETTINGS_INITIAL_WINDOW_SIZE] = XR_H2_DEFAULT_INITIAL_WINDOW_SIZE;
    conn->local_settings[XR_H2_SETTINGS_MAX_FRAME_SIZE] = XR_H2_DEFAULT_MAX_FRAME_SIZE;
    conn->local_settings[XR_H2_SETTINGS_MAX_HEADER_LIST_SIZE] = XR_H2_DEFAULT_MAX_HEADER_LIST_SIZE;

    memcpy(conn->remote_settings, conn->local_settings, sizeof(conn->remote_settings));

    xr_hpack_init(&conn->encoder_table, XR_H2_DEFAULT_HEADER_TABLE_SIZE);
    xr_hpack_init(&conn->decoder_table, XR_H2_DEFAULT_HEADER_TABLE_SIZE);

    conn->next_stream_id = is_client ? 1 : 2;
    conn->connection_window = XR_H2_DEFAULT_INITIAL_WINDOW_SIZE;

    conn->recv_cap = 65536;
    conn->recv_buf = (char *) xr_malloc(conn->recv_cap);
    if (!conn->recv_buf) {
        xr_hpack_free(&conn->encoder_table);
        xr_hpack_free(&conn->decoder_table);
        xr_free(conn);
        return NULL;
    }

    return conn;
}

/* ========== Stream Priority ========== */

int xr_h2_set_priority(XrH2Conn *conn, XrH2Stream *stream, const XrH2Priority *priority) {
    if (!conn || !stream || !priority)
        return -1;

    stream->priority = *priority;
    return xr_h2_send_priority(conn, stream->id, priority);
}

int xr_h2_send_priority(XrH2Conn *conn, uint32_t stream_id, const XrH2Priority *priority) {
    if (!conn || !priority)
        return -1;

    uint8_t frame[XR_H2_FRAME_HEADER_SIZE + 5];
    XrH2FrameHeader header = {
        .length = 5, .type = XR_H2_FRAME_PRIORITY, .flags = 0, .stream_id = stream_id};
    xr_h2_write_frame_header(frame, &header);

    uint8_t *payload = frame + XR_H2_FRAME_HEADER_SIZE;
    uint32_t dep = priority->dependency;
    if (priority->exclusive)
        dep |= 0x80000000;

    payload[0] = (dep >> 24) & 0xFF;
    payload[1] = (dep >> 16) & 0xFF;
    payload[2] = (dep >> 8) & 0xFF;
    payload[3] = dep & 0xFF;
    payload[4] = priority->weight - 1;  // weight 1-256 -> 0-255

    return h2_send(conn, frame, XR_H2_FRAME_HEADER_SIZE + 5);
}

/* ========== Stream Cancellation ========== */

int xr_h2_cancel_stream(XrH2Conn *conn, XrH2Stream *stream, XrH2ErrorCode error) {
    if (!conn || !stream)
        return -1;

    stream->cancelled = true;
    stream->state = XR_H2_STREAM_STATE_CLOSED;

    return xr_h2_send_rst_stream(conn, stream->id, error);
}

int xr_h2_send_rst_stream(XrH2Conn *conn, uint32_t stream_id, XrH2ErrorCode error) {
    if (!conn)
        return -1;

    uint8_t frame[XR_H2_FRAME_HEADER_SIZE + 4];
    XrH2FrameHeader header = {
        .length = 4, .type = XR_H2_FRAME_RST_STREAM, .flags = 0, .stream_id = stream_id};
    xr_h2_write_frame_header(frame, &header);

    uint8_t *payload = frame + XR_H2_FRAME_HEADER_SIZE;
    payload[0] = (error >> 24) & 0xFF;
    payload[1] = (error >> 16) & 0xFF;
    payload[2] = (error >> 8) & 0xFF;
    payload[3] = error & 0xFF;

    return h2_send(conn, frame, XR_H2_FRAME_HEADER_SIZE + 4);
}

/* ========== Trailers ========== */

int xr_h2_send_trailers(XrH2Conn *conn, XrH2Stream *stream, const char **names,
                        const size_t *name_lens, const char **values, const size_t *value_lens,
                        int count) {
    if (!conn || !stream)
        return -1;

    // Encode trailer headers (zero-init for analyzer friendliness; see
    // xr_h2_send_headers for rationale).
    uint8_t headers_buf[16384] = {0};
    int headers_len = 0;

    for (int i = 0; i < count; i++) {
        int len =
            xr_hpack_encode(&conn->encoder_table, names[i], name_lens[i], values[i], value_lens[i],
                            headers_buf + headers_len, sizeof(headers_buf) - headers_len);
        if (len < 0)
            return -1;
        headers_len += len;
    }

    // Send HEADERS frame with END_STREAM
    uint8_t frame[XR_H2_FRAME_HEADER_SIZE + 16384];
    XrH2FrameHeader header = {.length = headers_len,
                              .type = XR_H2_FRAME_HEADERS,
                              .flags = XR_H2_FLAG_END_HEADERS | XR_H2_FLAG_END_STREAM,
                              .stream_id = stream->id};
    xr_h2_write_frame_header(frame, &header);
    if (headers_len > 0) {
        memcpy(frame + XR_H2_FRAME_HEADER_SIZE, headers_buf, (size_t) headers_len);
    }

    stream->state = XR_H2_STREAM_HALF_CLOSED_LOCAL;
    return h2_send(conn, frame, XR_H2_FRAME_HEADER_SIZE + headers_len);
}

/* ========== PING ========== */

int xr_h2_send_ping(XrH2Conn *conn, const uint8_t data[8], bool ack) {
    if (!conn)
        return -1;

    uint8_t frame[XR_H2_FRAME_HEADER_SIZE + 8];
    XrH2FrameHeader header = {
        .length = 8, .type = XR_H2_FRAME_PING, .flags = ack ? XR_H2_FLAG_ACK : 0, .stream_id = 0};
    xr_h2_write_frame_header(frame, &header);

    if (data) {
        memcpy(frame + XR_H2_FRAME_HEADER_SIZE, data, 8);
    } else {
        memset(frame + XR_H2_FRAME_HEADER_SIZE, 0, 8);
    }

    return h2_send(conn, frame, XR_H2_FRAME_HEADER_SIZE + 8);
}

/* ========== h2c (HTTP/2 Cleartext) ========== */

int xr_h2_upgrade_from_http1(XrH2Conn *conn, const char *settings_payload, size_t len) {
    if (!conn)
        return -1;

    // Parse Base64 encoded SETTINGS
    (void) settings_payload;
    (void) len;

    // Send server connection preface
    if (!conn->is_client) {
        if (xr_h2_send_settings(conn) < 0)
            return -1;
    }

    conn->settings_sent = true;
    return 0;
}

int xr_h2_start_h2c(XrH2Conn *conn) {
    if (!conn)
        return -1;

    // h2c Prior Knowledge: send connection preface directly
    if (conn->is_client) {
        if (h2_send(conn, XR_HTTP2_PREFACE, XR_HTTP2_PREFACE_LEN) < 0) {
            return -1;
        }
    }

    // Send SETTINGS frame
    if (xr_h2_send_settings(conn) < 0) {
        return -1;
    }

    conn->settings_sent = true;
    return 0;
}
