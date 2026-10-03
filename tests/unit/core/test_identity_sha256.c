/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_identity_sha256.c - Real library identity hashes and diagnostic probes
 *
 * KEY CONCEPT:
 *   A bounded binary packet supplies inputs; an independent process owns the oracle.
 */
#include "base/xsha256.h"
#include <stdio.h>
#include <string.h>

#define IDENTITY_MAX_BYTES 1048576u
#define IDENTITY_MAX_CASES 8192u
#define IDENTITY_MAX_CHUNKS 8192u
#define REQUIRE(condition) do { if (!(condition)) { \
    fprintf(stderr, "SHA packet rejected at line %d: %s\n", __LINE__, #condition); return 2; \
} } while (0)

static uint8_t input[IDENTITY_MAX_BYTES + 8u];
static uint32_t chunks[IDENTITY_MAX_CHUNKS];
static uint8_t overread_input[64];
static union {
    XrSHA256Context aligned;
    uint8_t bytes[sizeof(XrSHA256Context) + 1u];
} misaligned_input;

static bool read_u32(FILE *file, uint32_t *value) {
    uint8_t bytes[4];
    if (fread(bytes, 1, sizeof(bytes), file) != sizeof(bytes)) return false;
    *value = (uint32_t) bytes[0] | ((uint32_t) bytes[1] << 8) |
        ((uint32_t) bytes[2] << 16) | ((uint32_t) bytes[3] << 24);
    return true;
}

static int packet_cases(FILE *file) {
    char magic[8];
    uint32_t count = 0;
    REQUIRE(fread(magic, 1, sizeof(magic), file) == sizeof(magic));
    REQUIRE(!memcmp(magic, "XRSHAT01", sizeof(magic)));
    REQUIRE(read_u32(file, &count) && count > 0 && count <= IDENTITY_MAX_CASES);
    for (uint32_t id = 0; id < count; ++id) {
        uint32_t length = 0, alignment = 0, chunk_count = 0;
        REQUIRE(read_u32(file, &length) && length <= IDENTITY_MAX_BYTES);
        REQUIRE(read_u32(file, &alignment) && alignment < 8u);
        REQUIRE(read_u32(file, &chunk_count) && chunk_count <= IDENTITY_MAX_CHUNKS);
        uint32_t total = 0;
        for (uint32_t c = 0; c < chunk_count; ++c) {
            REQUIRE(read_u32(file, &chunks[c]) && chunks[c] <= length - total);
            total += chunks[c];
        }
        REQUIRE(!chunk_count || total == length);
        uint8_t *bytes = input + alignment;
        REQUIRE(fread(bytes, 1, length, file) == length);
        uint8_t whole[32], streamed[32];
        xr_sha256(bytes, length, whole);
        XrSHA256Context context;
        xr_sha256_init(&context);
        xr_sha256_update(&context, NULL, 0);
        if (!chunk_count) xr_sha256_update(&context, bytes, length);
        else {
            uint32_t offset = 0;
            for (uint32_t c = 0; c < chunk_count; ++c) {
                xr_sha256_update(&context, chunks[c] ? bytes + offset : NULL, chunks[c]);
                offset += chunks[c];
            }
        }
        xr_sha256_update(&context, NULL, 0);
        xr_sha256_final(&context, streamed);
        REQUIRE(!memcmp(whole, streamed, sizeof(whole)));
        REQUIRE(printf("%u ", (unsigned) id) > 0);
        for (unsigned b = 0; b < sizeof(whole); ++b) REQUIRE(printf("%02x", (unsigned) whole[b]) > 0);
        REQUIRE(putchar('\n') != EOF);
    }
    REQUIRE(fgetc(file) == EOF && !ferror(file));
    return 0;
}

int main(int argc, char **argv) {
    REQUIRE(argc == 2);
    if (!strcmp(argv[1], "--debug-null")) {
        xr_sha256_init(NULL);
        fputs("Null initialization returned without its Debug assertion\n", stderr);
        return 0;
    }
    if (!strcmp(argv[1], "--asan-overread")) {
        uint8_t digest[32];
        volatile size_t length = sizeof(overread_input) + 1u;
        xr_sha256(overread_input, length, digest);
        fprintf(stderr, "Out-of-bounds library hashing returned: %u\n", (unsigned) digest[0]);
        return 0;
    }
    if (!strcmp(argv[1], "--ubsan-misaligned")) {
        XrSHA256Context *context = (XrSHA256Context *) (void *) (misaligned_input.bytes + 1u);
        xr_sha256_init(context);
        fputs("Misaligned library initialization returned\n", stderr);
        return 0;
    }
    FILE *file = fopen(argv[1], "rb");
    REQUIRE(file);
    int status = packet_cases(file);
    REQUIRE(fclose(file) == 0);
    return status;
}
