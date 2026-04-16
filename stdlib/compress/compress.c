/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * compress.c - Compression standard library implementation
 *
 * KEY CONCEPT:
 *   Implements deflate/inflate algorithm (RFC 1951) with LZ77 + Huffman coding.
 *   Supports gzip (RFC 1952) and zlib (RFC 1950) wrapper formats.
 */

#include "compress.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* ========== External Declarations ========== */

extern XrValue xr_string_value(XrString *str);
extern XrString* xr_string_intern(XrayIsolate *X, const char *str, size_t len, uint32_t hash);

/* ========== CRC32 Implementation ========== */

// Precomputed CRC32 table (polynomial 0xEDB88320, reflected form)
static const uint32_t crc32_table[256] = {
    0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3,
    0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988, 0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91,
    0x1DB71064, 0x6AB020F2, 0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
    0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5,
    0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172, 0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B,
    0x35B5A8FA, 0x42B2986C, 0xDBBBC9D6, 0xACBCF940, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
    0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423, 0xCFBA9599, 0xB8BDA50F,
    0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924, 0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D,
    0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
    0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D, 0x91646C97, 0xE6635C01,
    0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E, 0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457,
    0x65B0D9C6, 0x12B7E950, 0x8BBEB8EA, 0xFCB9887C, 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
    0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2, 0x4ADFA541, 0x3DD895D7, 0xA4D1C46D, 0xD3D6F4FB,
    0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0, 0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9,
    0x5005713C, 0x270241AA, 0xBE0B1010, 0xC90C2086, 0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
    0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81, 0xB7BD5C3B, 0xC0BA6CAD,
    0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A, 0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683,
    0xE3630B12, 0x94643B84, 0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
    0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D, 0x806567CB, 0x196C3671, 0x6E6B06E7,
    0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC, 0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5,
    0xD6D6A3E8, 0xA1D1937E, 0x38D8C2C4, 0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
    0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60, 0xDF60EFC3, 0xA867DF55, 0x316E8EEF, 0x4669BE79,
    0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236, 0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F,
    0xC5BA3BBE, 0xB2BD0B28, 0x2BB45A92, 0x5CB36A04, 0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
    0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F, 0x72076785, 0x05005713,
    0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38, 0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21,
    0x86D3D2D4, 0xF1D4E242, 0x68DDB3F8, 0x1FDA836E, 0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
    0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C, 0x8F659EFF, 0xF862AE69, 0x616BFFD3, 0x166CCF45,
    0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2, 0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB,
    0xAED16A4A, 0xD9D65ADC, 0x40DF0B66, 0x37D83BF0, 0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
    0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6, 0xBAD03605, 0xCDD70693, 0x54DE5729, 0x23D967BF,
    0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94, 0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D,
};

uint32_t xr_crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

uint32_t xr_crc32(const uint8_t *data, size_t len) {
    return xr_crc32_update(0, data, len);
}

/* ========== Adler32 Implementation ========== */

#define ADLER_MOD 65521
// Max bytes before uint32 overflow: 255*5552 + 65520 < 2^32
#define ADLER_NMAX 5552

uint32_t xr_adler32_update(uint32_t adler, const uint8_t *data, size_t len) {
    uint32_t a = adler & 0xFFFF;
    uint32_t b = (adler >> 16) & 0xFFFF;
    
    while (len > 0) {
        size_t chunk = (len >= ADLER_NMAX) ? ADLER_NMAX : len;
        len -= chunk;
        for (size_t i = 0; i < chunk; i++) {
            a += data[i];
            b += a;
        }
        a %= ADLER_MOD;
        b %= ADLER_MOD;
        data += chunk;
    }
    
    return (b << 16) | a;
}

uint32_t xr_adler32(const uint8_t *data, size_t len) {
    return xr_adler32_update(1, data, len);
}

/* ========== Deflate Constants ========== */

#define MAX_BITS        15
#define MAX_SYMBOLS     288
#define MAX_DIST        32
#define MAX_WINDOW      32768
#define MAX_MATCH       258
#define MIN_MATCH       3

// Fixed Huffman literal length codes
static const uint8_t FIXED_LIT_LENGTHS[288] = {
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
    9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
    9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
    9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,8,8,8,8,8,8,8,8
};

// Length code extra bits
static const uint8_t LEN_EXTRA_BITS[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};

// Length code base values
static const uint16_t LEN_BASE[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
};

// Distance code extra bits
static const uint8_t DIST_EXTRA_BITS[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

// Distance code base values
static const uint16_t DIST_BASE[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};

/* ========== Bit Reader ========== */

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t byte_pos;
    int bit_pos;
    uint32_t bit_buf;
    int bits_in_buf;
} BitReader;

static void br_init(BitReader *br, const uint8_t *data, size_t len) {
    br->data = data;
    br->len = len;
    br->byte_pos = 0;
    br->bit_pos = 0;
    br->bit_buf = 0;
    br->bits_in_buf = 0;
}

static void br_fill(BitReader *br) {
    while (br->bits_in_buf <= 24 && br->byte_pos < br->len) {
        br->bit_buf |= (uint32_t)br->data[br->byte_pos++] << br->bits_in_buf;
        br->bits_in_buf += 8;
    }
}

static uint32_t br_peek(BitReader *br, int n) {
    br_fill(br);
    return br->bit_buf & ((1u << n) - 1);
}

static void br_skip(BitReader *br, int n) {
    br->bit_buf >>= n;
    br->bits_in_buf -= n;
}

static uint32_t br_read(BitReader *br, int n) {
    uint32_t val = br_peek(br, n);
    br_skip(br, n);
    return val;
}

static void br_align(BitReader *br) {
    // Discard remaining bits and align to next byte boundary
    // Note: bits_in_buf may contain unconsumed whole bytes
    // We need to "put back" these bytes to byte_pos
    int full_bytes = br->bits_in_buf / 8;
    if (full_bytes > 0) {
        br->byte_pos -= full_bytes;  // Put back unconsumed bytes
    }
    br->bits_in_buf = 0;
    br->bit_buf = 0;
}

/* ========== Bit Writer ========== */

typedef struct {
    uint8_t *data;
    size_t cap;
    size_t byte_pos;
    uint32_t bit_buf;
    int bits_in_buf;
} BitWriter;

static void bw_init(BitWriter *bw, uint8_t *data, size_t cap) {
    bw->data = data;
    bw->cap = cap;
    bw->byte_pos = 0;
    bw->bit_buf = 0;
    bw->bits_in_buf = 0;
}

static bool bw_write(BitWriter *bw, uint32_t val, int n) {
    bw->bit_buf |= val << bw->bits_in_buf;
    bw->bits_in_buf += n;
    
    while (bw->bits_in_buf >= 8) {
        if (bw->byte_pos >= bw->cap) return false;
        bw->data[bw->byte_pos++] = bw->bit_buf & 0xFF;
        bw->bit_buf >>= 8;
        bw->bits_in_buf -= 8;
    }
    return true;
}

static bool bw_flush(BitWriter *bw) {
    while (bw->bits_in_buf > 0) {
        if (bw->byte_pos >= bw->cap) return false;
        bw->data[bw->byte_pos++] = bw->bit_buf & 0xFF;
        bw->bit_buf >>= 8;
        bw->bits_in_buf -= 8;
        if (bw->bits_in_buf < 0) bw->bits_in_buf = 0;
    }
    return true;
}

/* ========== Huffman Decoder ========== */

typedef struct {
    uint16_t counts[MAX_BITS + 1];  // Symbol count per length
    uint16_t symbols[MAX_SYMBOLS];  // Symbols in canonical order
} HuffmanTable;

static int build_huffman_table(HuffmanTable *ht, const uint8_t *lengths, int n) {
    // Count symbols per length
    memset(ht->counts, 0, sizeof(ht->counts));
    for (int i = 0; i < n; i++) {
        if (lengths[i] > MAX_BITS) return -1;
        ht->counts[lengths[i]]++;
    }
    ht->counts[0] = 0;
    
    // Compute starting code for each length
    uint16_t offsets[MAX_BITS + 1];
    offsets[0] = 0;
    for (int i = 1; i <= MAX_BITS; i++) {
        offsets[i] = offsets[i - 1] + ht->counts[i - 1];
    }
    
    // Fill symbol table
    for (int i = 0; i < n; i++) {
        if (lengths[i] > 0) {
            ht->symbols[offsets[lengths[i]]++] = i;
        }
    }
    
    return 0;
}

static int decode_huffman(BitReader *br, HuffmanTable *ht) {
    br_fill(br);
    
    uint32_t code = 0;
    int first = 0;
    int index = 0;
    
    for (int len = 1; len <= MAX_BITS; len++) {
        code |= br_peek(br, 1);
        br_skip(br, 1);
        
        int count = ht->counts[len];
        if ((int)(code - first) < count) {
            return ht->symbols[index + (code - first)];
        }
        
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    
    return -1;  // Invalid code
}

/* ========== Inflate Implementation ========== */

XrCompressError xr_inflate(const uint8_t *input, size_t in_len,
                           uint8_t *output, size_t out_cap, size_t *out_len) {
    if (!input || !output || !out_len) return XR_COMPRESS_ERR_DATA;
    
    BitReader br;
    br_init(&br, input, in_len);
    
    size_t out_pos = 0;
    bool final_block = false;
    
    while (!final_block) {
        // Read block header
        final_block = br_read(&br, 1) != 0;
        int type = br_read(&br, 2);
        
        if (type == 0) {
            // Uncompressed block
            br_align(&br);
            
            if (br.byte_pos + 4 > br.len) return XR_COMPRESS_ERR_DATA;
            
            uint16_t len = br.data[br.byte_pos] | (br.data[br.byte_pos + 1] << 8);
            uint16_t nlen = br.data[br.byte_pos + 2] | (br.data[br.byte_pos + 3] << 8);
            br.byte_pos += 4;
            br.bits_in_buf = 0;
            br.bit_buf = 0;
            
            if ((uint16_t)(~nlen) != len) return XR_COMPRESS_ERR_DATA;
            if (br.byte_pos + len > br.len) return XR_COMPRESS_ERR_DATA;
            if (out_pos + len > out_cap) return XR_COMPRESS_ERR_BUFFER;
            
            memcpy(output + out_pos, br.data + br.byte_pos, len);
            br.byte_pos += len;
            out_pos += len;
            
        } else if (type == 1 || type == 2) {
            // Compressed block
            HuffmanTable lit_table, dist_table;
            
            if (type == 1) {
                // Fixed Huffman codes
                build_huffman_table(&lit_table, FIXED_LIT_LENGTHS, 288);
                
                // Fixed distance codes: all 5 bits
                uint8_t dist_lengths[32];
                memset(dist_lengths, 5, 32);
                build_huffman_table(&dist_table, dist_lengths, 32);
                
            } else {
                // Dynamic Huffman codes
                int hlit = br_read(&br, 5) + 257;
                int hdist = br_read(&br, 5) + 1;
                int hclen = br_read(&br, 4) + 4;
                
                // Read code length code lengths
                static const uint8_t CLEN_ORDER[19] = {
                    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
                };
                
                uint8_t clen_lengths[19] = {0};
                for (int i = 0; i < hclen; i++) {
                    clen_lengths[CLEN_ORDER[i]] = br_read(&br, 3);
                }
                
                HuffmanTable clen_table;
                build_huffman_table(&clen_table, clen_lengths, 19);
                
                // Decode literal/length and distance code lengths
                uint8_t lengths[288 + 32];
                int i = 0;
                while (i < hlit + hdist) {
                    int sym = decode_huffman(&br, &clen_table);
                    if (sym < 0) return XR_COMPRESS_ERR_DATA;
                    
                    if (sym < 16) {
                        lengths[i++] = sym;
                    } else if (sym == 16) {
                        if (i == 0) return XR_COMPRESS_ERR_DATA;
                        int rep = br_read(&br, 2) + 3;
                        uint8_t prev = lengths[i - 1];
                        while (rep-- > 0 && i < hlit + hdist) lengths[i++] = prev;
                    } else if (sym == 17) {
                        int rep = br_read(&br, 3) + 3;
                        while (rep-- > 0 && i < hlit + hdist) lengths[i++] = 0;
                    } else {
                        int rep = br_read(&br, 7) + 11;
                        while (rep-- > 0 && i < hlit + hdist) lengths[i++] = 0;
                    }
                }
                
                build_huffman_table(&lit_table, lengths, hlit);
                build_huffman_table(&dist_table, lengths + hlit, hdist);
            }
            
            // Decode data
            while (1) {
                int sym = decode_huffman(&br, &lit_table);
                if (sym < 0) return XR_COMPRESS_ERR_DATA;
                
                if (sym < 256) {
                    // Literal
                    if (out_pos >= out_cap) return XR_COMPRESS_ERR_BUFFER;
                    output[out_pos++] = sym;
                    
                } else if (sym == 256) {
                    // End of block
                    break;
                    
                } else {
                    // Length-distance pair
                    int len_idx = sym - 257;
                    if (len_idx >= 29) return XR_COMPRESS_ERR_DATA;
                    
                    int length = LEN_BASE[len_idx] + br_read(&br, LEN_EXTRA_BITS[len_idx]);
                    
                    int dist_sym = decode_huffman(&br, &dist_table);
                    if (dist_sym < 0 || dist_sym >= 30) return XR_COMPRESS_ERR_DATA;
                    
                    int distance = DIST_BASE[dist_sym] + br_read(&br, DIST_EXTRA_BITS[dist_sym]);
                    
                    if ((size_t)distance > out_pos) return XR_COMPRESS_ERR_DATA;
                    if (out_pos + length > out_cap) return XR_COMPRESS_ERR_BUFFER;
                    
                    // Copy match
                    size_t src = out_pos - distance;
                    for (int j = 0; j < length; j++) {
                        output[out_pos++] = output[src++];
                    }
                }
            }
            
        } else {
            return XR_COMPRESS_ERR_DATA;
        }
    }
    
    *out_len = out_pos;
    return XR_COMPRESS_OK;
}

/* ========== LZ77 + Fixed Huffman Deflate Implementation ========== */

// LZ77 parameters
#define LZ77_WINDOW_SIZE  32768   // Sliding window size
#define LZ77_MIN_MATCH    3       // Minimum match length
#define LZ77_MAX_MATCH    258     // Maximum match length
#define LZ77_HASH_BITS    15
#define LZ77_HASH_SIZE    (1 << LZ77_HASH_BITS)
#define LZ77_HASH_MASK    (LZ77_HASH_SIZE - 1)

// Compute 3-byte hash (multiplicative hash for better distribution)
static inline uint32_t lz77_hash(const uint8_t *p) {
    uint32_t h = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    return (h * 0x1E35A7BD) >> (32 - LZ77_HASH_BITS);
}

// Precomputed Fixed Huffman codes (reversed, ready for bitstream output)
static const struct { uint16_t code; uint8_t bits; } FIXED_HUFF_CODES[288] = {
    {0x00C,8}, {0x08C,8}, {0x04C,8}, {0x0CC,8}, {0x02C,8}, {0x0AC,8}, {0x06C,8}, {0x0EC,8},
    {0x01C,8}, {0x09C,8}, {0x05C,8}, {0x0DC,8}, {0x03C,8}, {0x0BC,8}, {0x07C,8}, {0x0FC,8},
    {0x002,8}, {0x082,8}, {0x042,8}, {0x0C2,8}, {0x022,8}, {0x0A2,8}, {0x062,8}, {0x0E2,8},
    {0x012,8}, {0x092,8}, {0x052,8}, {0x0D2,8}, {0x032,8}, {0x0B2,8}, {0x072,8}, {0x0F2,8},
    {0x00A,8}, {0x08A,8}, {0x04A,8}, {0x0CA,8}, {0x02A,8}, {0x0AA,8}, {0x06A,8}, {0x0EA,8},
    {0x01A,8}, {0x09A,8}, {0x05A,8}, {0x0DA,8}, {0x03A,8}, {0x0BA,8}, {0x07A,8}, {0x0FA,8},
    {0x006,8}, {0x086,8}, {0x046,8}, {0x0C6,8}, {0x026,8}, {0x0A6,8}, {0x066,8}, {0x0E6,8},
    {0x016,8}, {0x096,8}, {0x056,8}, {0x0D6,8}, {0x036,8}, {0x0B6,8}, {0x076,8}, {0x0F6,8},
    {0x00E,8}, {0x08E,8}, {0x04E,8}, {0x0CE,8}, {0x02E,8}, {0x0AE,8}, {0x06E,8}, {0x0EE,8},
    {0x01E,8}, {0x09E,8}, {0x05E,8}, {0x0DE,8}, {0x03E,8}, {0x0BE,8}, {0x07E,8}, {0x0FE,8},
    {0x001,8}, {0x081,8}, {0x041,8}, {0x0C1,8}, {0x021,8}, {0x0A1,8}, {0x061,8}, {0x0E1,8},
    {0x011,8}, {0x091,8}, {0x051,8}, {0x0D1,8}, {0x031,8}, {0x0B1,8}, {0x071,8}, {0x0F1,8},
    {0x009,8}, {0x089,8}, {0x049,8}, {0x0C9,8}, {0x029,8}, {0x0A9,8}, {0x069,8}, {0x0E9,8},
    {0x019,8}, {0x099,8}, {0x059,8}, {0x0D9,8}, {0x039,8}, {0x0B9,8}, {0x079,8}, {0x0F9,8},
    {0x005,8}, {0x085,8}, {0x045,8}, {0x0C5,8}, {0x025,8}, {0x0A5,8}, {0x065,8}, {0x0E5,8},
    {0x015,8}, {0x095,8}, {0x055,8}, {0x0D5,8}, {0x035,8}, {0x0B5,8}, {0x075,8}, {0x0F5,8},
    {0x00D,8}, {0x08D,8}, {0x04D,8}, {0x0CD,8}, {0x02D,8}, {0x0AD,8}, {0x06D,8}, {0x0ED,8},
    {0x01D,8}, {0x09D,8}, {0x05D,8}, {0x0DD,8}, {0x03D,8}, {0x0BD,8}, {0x07D,8}, {0x0FD,8},
    {0x013,9}, {0x113,9}, {0x093,9}, {0x193,9}, {0x053,9}, {0x153,9}, {0x0D3,9}, {0x1D3,9},
    {0x033,9}, {0x133,9}, {0x0B3,9}, {0x1B3,9}, {0x073,9}, {0x173,9}, {0x0F3,9}, {0x1F3,9},
    {0x00B,9}, {0x10B,9}, {0x08B,9}, {0x18B,9}, {0x04B,9}, {0x14B,9}, {0x0CB,9}, {0x1CB,9},
    {0x02B,9}, {0x12B,9}, {0x0AB,9}, {0x1AB,9}, {0x06B,9}, {0x16B,9}, {0x0EB,9}, {0x1EB,9},
    {0x01B,9}, {0x11B,9}, {0x09B,9}, {0x19B,9}, {0x05B,9}, {0x15B,9}, {0x0DB,9}, {0x1DB,9},
    {0x03B,9}, {0x13B,9}, {0x0BB,9}, {0x1BB,9}, {0x07B,9}, {0x17B,9}, {0x0FB,9}, {0x1FB,9},
    {0x007,9}, {0x107,9}, {0x087,9}, {0x187,9}, {0x047,9}, {0x147,9}, {0x0C7,9}, {0x1C7,9},
    {0x027,9}, {0x127,9}, {0x0A7,9}, {0x1A7,9}, {0x067,9}, {0x167,9}, {0x0E7,9}, {0x1E7,9},
    {0x017,9}, {0x117,9}, {0x097,9}, {0x197,9}, {0x057,9}, {0x157,9}, {0x0D7,9}, {0x1D7,9},
    {0x037,9}, {0x137,9}, {0x0B7,9}, {0x1B7,9}, {0x077,9}, {0x177,9}, {0x0F7,9}, {0x1F7,9},
    {0x00F,9}, {0x10F,9}, {0x08F,9}, {0x18F,9}, {0x04F,9}, {0x14F,9}, {0x0CF,9}, {0x1CF,9},
    {0x02F,9}, {0x12F,9}, {0x0AF,9}, {0x1AF,9}, {0x06F,9}, {0x16F,9}, {0x0EF,9}, {0x1EF,9},
    {0x01F,9}, {0x11F,9}, {0x09F,9}, {0x19F,9}, {0x05F,9}, {0x15F,9}, {0x0DF,9}, {0x1DF,9},
    {0x03F,9}, {0x13F,9}, {0x0BF,9}, {0x1BF,9}, {0x07F,9}, {0x17F,9}, {0x0FF,9}, {0x1FF,9},
    {0x000,7}, {0x040,7}, {0x020,7}, {0x060,7}, {0x010,7}, {0x050,7}, {0x030,7}, {0x070,7},
    {0x008,7}, {0x048,7}, {0x028,7}, {0x068,7}, {0x018,7}, {0x058,7}, {0x038,7}, {0x078,7},
    {0x004,7}, {0x044,7}, {0x024,7}, {0x064,7}, {0x014,7}, {0x054,7}, {0x034,7}, {0x074,7},
    {0x003,8}, {0x083,8}, {0x043,8}, {0x0C3,8}, {0x023,8}, {0x0A3,8}, {0x063,8}, {0x0E3,8},
};

// Precomputed 5-bit reversed distance codes
static const uint8_t DIST_REV_CODES[32] = {
    0x00, 0x10, 0x08, 0x18, 0x04, 0x14, 0x0C, 0x1C,
    0x02, 0x12, 0x0A, 0x1A, 0x06, 0x16, 0x0E, 0x1E,
    0x01, 0x11, 0x09, 0x19, 0x05, 0x15, 0x0D, 0x1D,
    0x03, 0x13, 0x0B, 0x1B, 0x07, 0x17, 0x0F, 0x1F,
};

// Emit fixed Huffman code using precomputed table
static inline void emit_fixed_literal(BitWriter *bw, int lit) {
    bw_write(bw, FIXED_HUFF_CODES[lit].code, FIXED_HUFF_CODES[lit].bits);
}

// Find length code via binary search: O(5) vs O(29) linear scan
static inline int find_len_code(int len) {
    int lo = 0, hi = 28;
    while (lo < hi) {
        int mid = (lo + hi + 1) >> 1;
        if (len >= LEN_BASE[mid]) lo = mid;
        else hi = mid - 1;
    }
    return 257 + lo;
}

// Find distance code via binary search: O(5) vs O(30) linear scan
static inline int find_dist_code(int dist) {
    int lo = 0, hi = 29;
    while (lo < hi) {
        int mid = (lo + hi + 1) >> 1;
        if (dist >= DIST_BASE[mid]) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

// Emit length+distance pair
static void emit_match(BitWriter *bw, int len, int dist) {
    // Output length code
    int len_code = find_len_code(len);
    emit_fixed_literal(bw, len_code);
    
    // Output length extra bits
    int len_idx = len_code - 257;
    if (LEN_EXTRA_BITS[len_idx] > 0) {
        bw_write(bw, len - LEN_BASE[len_idx], LEN_EXTRA_BITS[len_idx]);
    }
    
    // Output distance code (fixed 5 bits, precomputed reverse)
    int dist_code = find_dist_code(dist);
    bw_write(bw, DIST_REV_CODES[dist_code], 5);
    
    // Output distance extra bits
    if (DIST_EXTRA_BITS[dist_code] > 0) {
        bw_write(bw, dist - DIST_BASE[dist_code], DIST_EXTRA_BITS[dist_code]);
    }
}

// LZ77 find longest match
static int lz77_find_match(const uint8_t *input, size_t in_len, size_t pos,
                           int *hash_table, int *prev_chain, int *match_dist) {
    if (pos + LZ77_MIN_MATCH > in_len) return 0;
    
    uint32_t h = lz77_hash(input + pos);
    int best_len = 0;
    int best_dist = 0;
    
    int chain_len = 0;
    int max_chain = 128;  // Limit chain search length
    
    int match_pos = hash_table[h];
    while (match_pos >= 0 && chain_len < max_chain) {
        int dist = (int)pos - match_pos;
        if (dist > LZ77_WINDOW_SIZE) break;
        
        // Compare match length
        int max_len = (int)(in_len - pos);
        if (max_len > LZ77_MAX_MATCH) max_len = LZ77_MAX_MATCH;
        
        int len = 0;
        while (len < max_len && input[match_pos + len] == input[pos + len]) {
            len++;
        }
        
        if (len > best_len) {
            best_len = len;
            best_dist = dist;
            if (len >= LZ77_MAX_MATCH) break;  // Maximum match
        }
        
        // Follow chain
        match_pos = prev_chain[match_pos & (LZ77_WINDOW_SIZE - 1)];
        chain_len++;
    }
    
    if (best_len >= LZ77_MIN_MATCH) {
        *match_dist = best_dist;
        return best_len;
    }
    
    return 0;
}

// Update hash chain
static void lz77_update_hash(const uint8_t *input, size_t pos,
                             int *hash_table, int *prev_chain) {
    uint32_t h = lz77_hash(input + pos);
    prev_chain[pos & (LZ77_WINDOW_SIZE - 1)] = hash_table[h];
    hash_table[h] = (int)pos;
}

size_t xr_deflate_bound(size_t in_len) {
    // Worst case: uncompressed storage
    size_t blocks = (in_len + 65534) / 65535;
    return in_len + blocks * 5 + 10;
}

XrCompressError xr_deflate(const uint8_t *input, size_t in_len,
                           uint8_t *output, size_t out_cap, size_t *out_len,
                           int level) {
    if (!input || !output || !out_len) return XR_COMPRESS_ERR_DATA;
    
    // Level 0 or small data uses uncompressed storage
    if (level == 0 || in_len < 10) {
        BitWriter bw;
        bw_init(&bw, output, out_cap);
        
        if (in_len == 0) {
            // Empty input: emit a single empty final stored block
            if (out_cap < 5) return XR_COMPRESS_ERR_BUFFER;
            if (!bw_write(&bw, 1, 1)) return XR_COMPRESS_ERR_BUFFER;  // BFINAL=1
            if (!bw_write(&bw, 0, 2)) return XR_COMPRESS_ERR_BUFFER;  // BTYPE=0 (stored)
            if (!bw_flush(&bw)) return XR_COMPRESS_ERR_BUFFER;
            output[bw.byte_pos++] = 0x00;  // LEN low
            output[bw.byte_pos++] = 0x00;  // LEN high
            output[bw.byte_pos++] = 0xFF;  // NLEN low
            output[bw.byte_pos++] = 0xFF;  // NLEN high
            *out_len = bw.byte_pos;
            return XR_COMPRESS_OK;
        }
        
        size_t pos = 0;
        while (pos < in_len) {
            size_t chunk = in_len - pos;
            if (chunk > 65535) chunk = 65535;
            
            bool is_final = (pos + chunk >= in_len);
            
            if (!bw_write(&bw, is_final ? 1 : 0, 1)) return XR_COMPRESS_ERR_BUFFER;
            if (!bw_write(&bw, 0, 2)) return XR_COMPRESS_ERR_BUFFER;
            if (!bw_flush(&bw)) return XR_COMPRESS_ERR_BUFFER;
            
            uint16_t len = (uint16_t)chunk;
            uint16_t nlen = ~len;
            
            if (bw.byte_pos + 4 + chunk > out_cap) return XR_COMPRESS_ERR_BUFFER;
            
            output[bw.byte_pos++] = len & 0xFF;
            output[bw.byte_pos++] = (len >> 8) & 0xFF;
            output[bw.byte_pos++] = nlen & 0xFF;
            output[bw.byte_pos++] = (nlen >> 8) & 0xFF;
            
            memcpy(output + bw.byte_pos, input + pos, chunk);
            bw.byte_pos += chunk;
            pos += chunk;
        }
        
        *out_len = bw.byte_pos;
        return XR_COMPRESS_OK;
    }
    
    // LZ77 + Fixed Huffman compression
    
    // Allocate hash table and chain
    int *hash_table = (int*)malloc(LZ77_HASH_SIZE * sizeof(int));
    int *prev_chain = (int*)malloc(LZ77_WINDOW_SIZE * sizeof(int));
    
    if (!hash_table || !prev_chain) {
        free(hash_table);
        free(prev_chain);
        return XR_COMPRESS_ERR_MEMORY;
    }
    
    // Initialize hash table
    for (int i = 0; i < LZ77_HASH_SIZE; i++) hash_table[i] = -1;
    
    BitWriter bw;
    bw_init(&bw, output, out_cap);
    
    // Block header: BFINAL=1, BTYPE=01 (fixed Huffman)
    bw_write(&bw, 1, 1);  // final block
    bw_write(&bw, 1, 2);  // fixed huffman
    
    size_t pos = 0;
    while (pos < in_len) {
        int match_dist;
        int match_len = 0;
        
        if (pos + LZ77_MIN_MATCH <= in_len) {
            match_len = lz77_find_match(input, in_len, pos, hash_table, prev_chain, &match_dist);
        }
        
        if (match_len >= LZ77_MIN_MATCH) {
            // Output match
            emit_match(&bw, match_len, match_dist);
            
            // Update hash chain (lazy update)
            for (int i = 0; i < match_len && pos + i + LZ77_MIN_MATCH <= in_len; i++) {
                lz77_update_hash(input, pos + i, hash_table, prev_chain);
            }
            pos += match_len;
        } else {
            // Output literal
            emit_fixed_literal(&bw, input[pos]);
            
            if (pos + LZ77_MIN_MATCH <= in_len) {
                lz77_update_hash(input, pos, hash_table, prev_chain);
            }
            pos++;
        }
    }
    
    // Output end-of-block symbol 256
    emit_fixed_literal(&bw, 256);
    
    // Flush bit buffer
    bw_flush(&bw);
    
    free(hash_table);
    free(prev_chain);
    
    *out_len = bw.byte_pos;
    return XR_COMPRESS_OK;
}

/* ========== Gzip Implementation ========== */

bool xr_is_gzip(const uint8_t *data, size_t len) {
    if (len < 10) return false;
    return data[0] == 0x1F && data[1] == 0x8B;
}

uint32_t xr_gzip_original_size(const uint8_t *data, size_t len) {
    if (len < 18) return 0;  // valid gzip = 10-byte header + 8-byte trailer minimum
    return data[len-4] | (data[len-3] << 8) | (data[len-2] << 16) | (data[len-1] << 24);
}

XrCompressError xr_gzip(const uint8_t *input, size_t in_len,
                        uint8_t *output, size_t out_cap, size_t *out_len,
                        int level) {
    if (!input || !output || !out_len) return XR_COMPRESS_ERR_DATA;
    
    // Gzip header (10 bytes)
    if (out_cap < 18) return XR_COMPRESS_ERR_BUFFER;
    
    size_t pos = 0;
    output[pos++] = 0x1F;  // Magic number
    output[pos++] = 0x8B;
    output[pos++] = 0x08;  // Compression method: deflate
    output[pos++] = 0x00;  // Flags
    output[pos++] = 0x00;  // Modification time (4 bytes)
    output[pos++] = 0x00;
    output[pos++] = 0x00;
    output[pos++] = 0x00;
    output[pos++] = 0x00;  // Extra flags
    output[pos++] = 0xFF;  // OS: unknown
    
    // Deflate compression
    size_t deflate_len;
    XrCompressError err = xr_deflate(input, in_len, 
                                      output + pos, out_cap - pos - 8, 
                                      &deflate_len, level);
    if (err != XR_COMPRESS_OK) return err;
    
    pos += deflate_len;
    
    // CRC32
    uint32_t crc = xr_crc32(input, in_len);
    output[pos++] = crc & 0xFF;
    output[pos++] = (crc >> 8) & 0xFF;
    output[pos++] = (crc >> 16) & 0xFF;
    output[pos++] = (crc >> 24) & 0xFF;
    
    // Original size (mod 2^32)
    output[pos++] = in_len & 0xFF;
    output[pos++] = (in_len >> 8) & 0xFF;
    output[pos++] = (in_len >> 16) & 0xFF;
    output[pos++] = (in_len >> 24) & 0xFF;
    
    *out_len = pos;
    return XR_COMPRESS_OK;
}

XrCompressError xr_gunzip(const uint8_t *input, size_t in_len,
                          uint8_t *output, size_t out_cap, size_t *out_len) {
    if (!input || !output || !out_len) return XR_COMPRESS_ERR_DATA;
    if (!xr_is_gzip(input, in_len)) return XR_COMPRESS_ERR_HEADER;
    
    // Parse gzip header
    if (in_len < 18) return XR_COMPRESS_ERR_DATA;
    
    if (input[2] != 0x08) return XR_COMPRESS_ERR_DATA;  // Must be deflate
    
    uint8_t flags = input[3];
    size_t pos = 10;
    
    // Skip optional fields
    if (flags & 0x04) {  // FEXTRA
        if (pos + 2 > in_len) return XR_COMPRESS_ERR_DATA;
        uint16_t xlen = input[pos] | (input[pos + 1] << 8);
        pos += 2 + xlen;
    }
    if (flags & 0x08) {  // FNAME
        while (pos < in_len && input[pos] != 0) pos++;
        pos++;
    }
    if (flags & 0x10) {  // FCOMMENT
        while (pos < in_len && input[pos] != 0) pos++;
        pos++;
    }
    if (flags & 0x02) {  // FHCRC
        pos += 2;
    }
    
    if (pos + 8 > in_len) return XR_COMPRESS_ERR_DATA;
    
    // Decompress deflate data
    size_t deflate_len = in_len - pos - 8;
    XrCompressError err = xr_inflate(input + pos, deflate_len, output, out_cap, out_len);
    if (err != XR_COMPRESS_OK) return err;
    
    // Verify CRC32
    pos += deflate_len;
    uint32_t stored_crc = (uint32_t)input[pos] | ((uint32_t)input[pos+1] << 8) | 
                          ((uint32_t)input[pos+2] << 16) | ((uint32_t)input[pos+3] << 24);
    uint32_t actual_crc = xr_crc32(output, *out_len);
    if (stored_crc != actual_crc) return XR_COMPRESS_ERR_CHECKSUM;
    
    // Verify original size
    pos += 4;
    uint32_t stored_size = (uint32_t)input[pos] | ((uint32_t)input[pos+1] << 8) | 
                           ((uint32_t)input[pos+2] << 16) | ((uint32_t)input[pos+3] << 24);
    if (stored_size != (*out_len & 0xFFFFFFFF)) return XR_COMPRESS_ERR_DATA;
    
    return XR_COMPRESS_OK;
}

/* ========== Zlib Implementation ========== */

bool xr_is_zlib(const uint8_t *data, size_t len) {
    if (len < 2) return false;
    // zlib header: CMF + FLG, ((CMF * 256 + FLG) % 31 == 0)
    uint16_t check = (data[0] << 8) | data[1];
    return (check % 31 == 0) && ((data[0] & 0x0F) == 8);
}

XrCompressError xr_zlib_compress(const uint8_t *input, size_t in_len,
                                  uint8_t *output, size_t out_cap, size_t *out_len,
                                  int level) {
    if (!input || !output || !out_len) return XR_COMPRESS_ERR_DATA;
    if (out_cap < 6) return XR_COMPRESS_ERR_BUFFER;
    
    // zlib header (2 bytes): CMF + FLG
    // CMF = 0x78 (deflate method, 32K window)
    // FLG FLEVEL: 0=fastest, 1=fast, 2=default, 3=best
    uint8_t cmf = 0x78;
    uint8_t flevel;
    if (level <= 1) flevel = 0;       // fastest
    else if (level <= 5) flevel = 1;  // fast
    else if (level <= 7) flevel = 2;  // default
    else flevel = 3;                  // best
    uint8_t flg = (flevel << 6);
    // Adjust FLG so that (CMF*256 + FLG) % 31 == 0
    uint16_t check = (uint16_t)cmf * 256 + flg;
    uint8_t rem = check % 31;
    if (rem != 0) flg += (31 - rem);
    output[0] = cmf;
    output[1] = flg;
    
    // Deflate compression
    size_t deflate_len;
    XrCompressError err = xr_deflate(input, in_len, 
                                      output + 2, out_cap - 6, 
                                      &deflate_len, level);
    if (err != XR_COMPRESS_OK) return err;
    
    // Adler32 checksum
    size_t pos = 2 + deflate_len;
    uint32_t adler = xr_adler32(input, in_len);
    output[pos++] = (adler >> 24) & 0xFF;
    output[pos++] = (adler >> 16) & 0xFF;
    output[pos++] = (adler >> 8) & 0xFF;
    output[pos++] = adler & 0xFF;
    
    *out_len = pos;
    return XR_COMPRESS_OK;
}

XrCompressError xr_zlib_decompress(const uint8_t *input, size_t in_len,
                                    uint8_t *output, size_t out_cap, size_t *out_len) {
    if (!input || !output || !out_len) return XR_COMPRESS_ERR_DATA;
    if (!xr_is_zlib(input, in_len)) return XR_COMPRESS_ERR_HEADER;
    if (in_len < 6) return XR_COMPRESS_ERR_DATA;
    
    // Skip zlib header: 2 bytes CMF+FLG, plus 4 bytes DICTID if FDICT is set
    size_t hdr_len = 2;
    if (input[1] & 0x20) {
        hdr_len += 4;  // FDICT flag set, skip DICTID
        if (in_len < hdr_len + 4) return XR_COMPRESS_ERR_DATA;
    }
    
    // Decompress deflate data (trailer is 4 bytes Adler32)
    XrCompressError err = xr_inflate(input + hdr_len, in_len - hdr_len - 4,
                                      output, out_cap, out_len);
    if (err != XR_COMPRESS_OK) return err;
    
    // Verify Adler32
    size_t pos = in_len - 4;
    uint32_t stored_adler = ((uint32_t)input[pos] << 24) | ((uint32_t)input[pos+1] << 16) | 
                            ((uint32_t)input[pos+2] << 8) | (uint32_t)input[pos+3];
    uint32_t actual_adler = xr_adler32(output, *out_len);
    if (stored_adler != actual_adler) return XR_COMPRESS_ERR_CHECKSUM;
    
    return XR_COMPRESS_OK;
}

/* ========== Heap-Allocated Versions ========== */

uint8_t* xr_gzip_alloc(const uint8_t *input, size_t in_len, 
                        size_t *out_len, int level) {
    size_t bound = xr_deflate_bound(in_len) + 18;
    uint8_t *output = (uint8_t*)malloc(bound);
    if (!output) return NULL;
    
    XrCompressError err = xr_gzip(input, in_len, output, bound, out_len, level);
    if (err != XR_COMPRESS_OK) {
        free(output);
        return NULL;
    }
    
    return output;
}

uint8_t* xr_gunzip_alloc(const uint8_t *input, size_t in_len, size_t *out_len) {
    // Try to get original size from gzip trailer
    uint32_t orig_size = xr_gzip_original_size(input, in_len);
    if (orig_size == 0) orig_size = in_len * 4;  // Estimate
    
    // Try to decompress, expand buffer if needed
    size_t cap = orig_size + 256;
    for (int tries = 0; tries < 4; tries++) {
        uint8_t *output = (uint8_t*)malloc(cap);
        if (!output) return NULL;
        
        XrCompressError err = xr_gunzip(input, in_len, output, cap, out_len);
        if (err == XR_COMPRESS_OK) {
            return output;
        } else if (err == XR_COMPRESS_ERR_BUFFER) {
            free(output);
            cap *= 2;
        } else {
            free(output);
            return NULL;
        }
    }
    
    return NULL;
}

/* ========== Error Messages ========== */

const char* xr_compress_error_str(XrCompressError err) {
    switch (err) {
        case XR_COMPRESS_OK: return "OK";
        case XR_COMPRESS_ERR_MEMORY: return "Memory allocation failed";
        case XR_COMPRESS_ERR_DATA: return "Invalid data";
        case XR_COMPRESS_ERR_BUFFER: return "Buffer too small";
        case XR_COMPRESS_ERR_STREAM: return "Stream error";
        case XR_COMPRESS_ERR_HEADER: return "Invalid header";
        case XR_COMPRESS_ERR_CHECKSUM: return "Checksum mismatch";
        default: return "Unknown error";
    }
}

/* ========== Helper Functions ========== */

static const char* get_string_arg(XrValue v, size_t *len) {
    if (!XR_IS_STRING(v)) return NULL;
    XrString *str = XR_TO_STRING(v);
    if (len) *len = str->length;
    return str->data;
}

static XrValue make_string(XrayIsolate *X, const char *s, size_t len) {
    if (!s) return xr_null();
    XrString *str = xr_string_new(X, s, len);
    return xr_string_value(str);
}

/* ========== xray Binding Functions ========== */

// compress.gzip(data, level?) -> string
static XrValue compress_gzip(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 1) return xr_null();
    
    size_t len;
    const char *data = get_string_arg(args[0], &len);
    if (!data) return xr_null();
    
    int level = XR_COMPRESS_DEFAULT_COMPRESSION;
    if (nargs >= 2 && XR_IS_INT(args[1])) {
        level = (int)XR_TO_INT(args[1]);
        if (level < 0) level = 0;
        if (level > 9) level = 9;
    }
    
    size_t out_len;
    uint8_t *output = xr_gzip_alloc((const uint8_t*)data, len, &out_len, level);
    if (!output) return xr_null();
    
    XrValue result = make_string(X, (char*)output, out_len);
    free(output);
    return result;
}

// compress.gunzip(data) -> string
static XrValue compress_gunzip(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 1) return xr_null();
    
    size_t len;
    const char *data = get_string_arg(args[0], &len);
    if (!data) return xr_null();
    
    size_t out_len;
    uint8_t *output = xr_gunzip_alloc((const uint8_t*)data, len, &out_len);
    if (!output) return xr_null();
    
    XrValue result = make_string(X, (char*)output, out_len);
    free(output);
    return result;
}

// compress.deflate(data, level?) -> string
static XrValue compress_deflate(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 1) return xr_null();
    
    size_t len;
    const char *data = get_string_arg(args[0], &len);
    if (!data) return xr_null();
    
    int level = XR_COMPRESS_DEFAULT_COMPRESSION;
    if (nargs >= 2 && XR_IS_INT(args[1])) {
        level = (int)XR_TO_INT(args[1]);
        if (level < 0) level = 0;
        if (level > 9) level = 9;
    }
    
    size_t bound = xr_deflate_bound(len);
    uint8_t *output = (uint8_t*)malloc(bound);
    if (!output) return xr_null();
    
    size_t out_len;
    XrCompressError err = xr_deflate((const uint8_t*)data, len, output, bound, &out_len, level);
    if (err != XR_COMPRESS_OK) {
        free(output);
        return xr_null();
    }
    
    XrValue result = make_string(X, (char*)output, out_len);
    free(output);
    return result;
}

// compress.inflate(data) -> string
static XrValue compress_inflate(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 1) return xr_null();
    
    size_t len;
    const char *data = get_string_arg(args[0], &len);
    if (!data) return xr_null();
    
    // Estimate output size (generous to handle high compression ratios)
    size_t cap = len * 8 + 1024;
    for (int tries = 0; tries < 8; tries++) {
        uint8_t *output = (uint8_t*)malloc(cap);
        if (!output) return xr_null();
        
        size_t out_len;
        XrCompressError err = xr_inflate((const uint8_t*)data, len, output, cap, &out_len);
        if (err == XR_COMPRESS_OK) {
            XrValue result = make_string(X, (char*)output, out_len);
            free(output);
            return result;
        } else if (err == XR_COMPRESS_ERR_BUFFER) {
            free(output);
            cap *= 2;
        } else {
            free(output);
            return xr_null();
        }
    }
    
    return xr_null();
}

// compress.zlibCompress(data, level?) -> string
static XrValue compress_zlib_compress(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 1) return xr_null();
    
    size_t len;
    const char *data = get_string_arg(args[0], &len);
    if (!data) return xr_null();
    
    int level = XR_COMPRESS_DEFAULT_COMPRESSION;
    if (nargs >= 2 && XR_IS_INT(args[1])) {
        level = (int)XR_TO_INT(args[1]);
        if (level < 0) level = 0;
        if (level > 9) level = 9;
    }
    
    size_t bound = xr_deflate_bound(len) + 6;
    uint8_t *output = (uint8_t*)malloc(bound);
    if (!output) return xr_null();
    
    size_t out_len;
    XrCompressError err = xr_zlib_compress((const uint8_t*)data, len, output, bound, &out_len, level);
    if (err != XR_COMPRESS_OK) {
        free(output);
        return xr_null();
    }
    
    XrValue result = make_string(X, (char*)output, out_len);
    free(output);
    return result;
}

// compress.zlibDecompress(data) -> string
static XrValue compress_zlib_decompress(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 1) return xr_null();
    
    size_t len;
    const char *data = get_string_arg(args[0], &len);
    if (!data) return xr_null();
    
    size_t cap = len * 8 + 1024;
    for (int tries = 0; tries < 8; tries++) {
        uint8_t *output = (uint8_t*)malloc(cap);
        if (!output) return xr_null();
        
        size_t out_len;
        XrCompressError err = xr_zlib_decompress((const uint8_t*)data, len, output, cap, &out_len);
        if (err == XR_COMPRESS_OK) {
            XrValue result = make_string(X, (char*)output, out_len);
            free(output);
            return result;
        } else if (err == XR_COMPRESS_ERR_BUFFER) {
            free(output);
            cap *= 2;
        } else {
            free(output);
            return xr_null();
        }
    }
    
    return xr_null();
}

// compress.isGzip(data) -> bool
static XrValue compress_is_gzip(XrayIsolate *X, XrValue *args, int nargs) {
    (void)X;
    if (nargs < 1) return xr_bool(false);
    
    size_t len;
    const char *data = get_string_arg(args[0], &len);
    if (!data) return xr_bool(false);
    
    return xr_bool(xr_is_gzip((const uint8_t*)data, len));
}

// compress.isZlib(data) -> bool
static XrValue compress_is_zlib(XrayIsolate *X, XrValue *args, int nargs) {
    (void)X;
    if (nargs < 1) return xr_bool(false);
    
    size_t len;
    const char *data = get_string_arg(args[0], &len);
    if (!data) return xr_bool(false);
    
    return xr_bool(xr_is_zlib((const uint8_t*)data, len));
}

// compress.crc32(data) -> int
static XrValue compress_crc32(XrayIsolate *X, XrValue *args, int nargs) {
    (void)X;
    if (nargs < 1) return xr_int(0);
    
    size_t len;
    const char *data = get_string_arg(args[0], &len);
    if (!data) return xr_int(0);
    
    return xr_int((int64_t)xr_crc32((const uint8_t*)data, len));
}

// compress.adler32(data) -> int
static XrValue compress_adler32(XrayIsolate *X, XrValue *args, int nargs) {
    (void)X;
    if (nargs < 1) return xr_int(1);
    
    size_t len;
    const char *data = get_string_arg(args[0], &len);
    if (!data) return xr_int(1);
    
    return xr_int((int64_t)xr_adler32((const uint8_t*)data, len));
}

/* ========== Module Loading ========== */

// ========== Type Declarations (parsed by gen_stdlib_types.py) ==========

#include "../../src/module/xbuiltin_decl.h"

// @module compress

XR_DEFINE_BUILTIN(compress_gzip, "gzip", "(data: string, level?: int): string?", "Gzip compress")
XR_DEFINE_BUILTIN(compress_gunzip, "gunzip", "(data: string): string?", "Gzip decompress")
XR_DEFINE_BUILTIN(compress_is_gzip, "isGzip", "(data: string): bool", "Check if gzip data")
XR_DEFINE_BUILTIN(compress_deflate, "deflate", "(data: string, level?: int): string?", "Deflate compress")
XR_DEFINE_BUILTIN(compress_inflate, "inflate", "(data: string): string?", "Inflate decompress")
XR_DEFINE_BUILTIN(compress_zlib_compress, "zlibCompress", "(data: string, level?: int): string?", "Zlib compress")
XR_DEFINE_BUILTIN(compress_zlib_decompress, "zlibDecompress", "(data: string): string?", "Zlib decompress")
XR_DEFINE_BUILTIN(compress_is_zlib, "isZlib", "(data: string): bool", "Check if zlib data")
XR_DEFINE_BUILTIN(compress_crc32, "crc32", "(data: string): int", "Compute CRC-32 checksum")
XR_DEFINE_BUILTIN(compress_adler32, "adler32", "(data: string): int", "Compute Adler-32 checksum")

XrModule* xr_load_module_compress(XrayIsolate *isolate) {
    XrModule *module = xr_module_create_native(isolate, "compress");
    if (!module) return NULL;
    
    // Export function helper macro
    extern XrCFunction* xr_vm_cfunction_new(XrayIsolate *isolate, XrCFunctionPtr func, const char *name);
    extern XrValue xr_value_from_cfunction(XrCFunction *cfunc);
    
    #define EXPORT_CFUNC(name_str, func_ptr) \
        do { \
            XrCFunction *cfunc = xr_vm_cfunction_new(isolate, func_ptr, name_str); \
            XrValue fn_val = xr_value_from_cfunction(cfunc); \
            xr_module_add_export(isolate, module, name_str, fn_val); \
        } while(0)
    
    // Gzip
    EXPORT_CFUNC("gzip", compress_gzip);
    EXPORT_CFUNC("gunzip", compress_gunzip);
    EXPORT_CFUNC("isGzip", compress_is_gzip);
    
    // Deflate/Inflate
    EXPORT_CFUNC("deflate", compress_deflate);
    EXPORT_CFUNC("inflate", compress_inflate);
    
    // Zlib
    EXPORT_CFUNC("zlibCompress", compress_zlib_compress);
    EXPORT_CFUNC("zlibDecompress", compress_zlib_decompress);
    EXPORT_CFUNC("isZlib", compress_is_zlib);
    
    // Checksums
    EXPORT_CFUNC("crc32", compress_crc32);
    EXPORT_CFUNC("adler32", compress_adler32);
    
    #undef EXPORT_CFUNC
    
    module->loaded = true;
    return module;
}
