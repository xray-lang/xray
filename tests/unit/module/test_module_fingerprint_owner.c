/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_module_fingerprint_owner.c - Source identity and pre-read work admission
 */
#include "module/xmodule_fingerprint.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <string.h>
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
typedef struct Allocation { void *memory; size_t size; } Allocation;
static Allocation blocks[64];
static size_t attempts, fail_at, live, allocated, peak;
static void *probe_malloc(size_t bytes) {
    if (++attempts == fail_at) return NULL;
    void *memory = xr_malloc(bytes); REQUIRE(memory);
    size_t i = 0; while (i < 64 && blocks[i].memory) ++i;
    REQUIRE(i < 64); blocks[i] = (Allocation){memory, bytes};
    live += bytes; allocated += bytes; if (live > peak) peak = live; return memory;
}
static void probe_free(void *memory) {
    if (!memory) return;
    size_t i = 0; while (i < 64 && blocks[i].memory != memory) ++i;
    REQUIRE(i < 64); live -= blocks[i].size; blocks[i] = (Allocation){0}; xr_free(memory);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc probe_malloc
#define xr_free probe_free
#include "base/xcompile_resources.c"

static const XrCompileResourceLimits unlimited = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
static void vector(const char *source, const char *expected) {
    size_t length = strlen(source);
    /* Ledger bootstrap + NUL scan + length encoding + hash init + the 22-byte
     * domain and 8-byte length + source span + finalization + 32-byte publish. */
    uint64_t required = 74 + 2 * length;
    for (uint64_t cap = 1; cap <= required; ++cap) {
        REQUIRE(!live); allocated = peak = 0;
        XrCompileResourceLimits limits = unlimited; limits.work = cap;
        XrCompileResources *owner = NULL;
        REQUIRE(xr_compile_resources_new(&limits, &owner) == XR_COMPILE_RESOURCE_OK);
        XrFingerprint output; memset(&output, 0xa5, sizeof(output));
        XrCompileResourceStatus status = xr_compile_module_source_fingerprint(owner, source, &output);
        if (cap < required) {
            REQUIRE(status == XR_COMPILE_RESOURCE_BUDGET);
            for (size_t i = 0; i < sizeof(output.bytes); ++i) REQUIRE(output.bytes[i] == 0xa5);
        } else {
            REQUIRE(status == XR_COMPILE_RESOURCE_OK);
            static const char hex[] = "0123456789abcdef";
            for (size_t i = 0; i < sizeof(output.bytes); ++i) {
                REQUIRE(hex[output.bytes[i] >> 4] == expected[2*i]);
                REQUIRE(hex[output.bytes[i] & 15] == expected[2*i+1]);
            }
            XrFingerprint pure = {0}; xr_module_source_fingerprint(source, &pure);
            REQUIRE(!memcmp(&pure, &output, sizeof(output)));
            XrCompileResourceStats stats;
            REQUIRE(xr_compile_resources_stats(owner, &stats) == XR_COMPILE_RESOURCE_OK);
            REQUIRE(stats.work == required && stats.allocation_count == 1);
            REQUIRE(stats.live_bytes == live && stats.allocated_bytes == allocated);
            REQUIRE(xr_compile_module_source_fingerprint(owner, (char *)1, &output) == XR_COMPILE_RESOURCE_BUDGET);
            REQUIRE(!memcmp(&pure, &output, sizeof(output)));
        }
        xr_compile_resources_release(owner); REQUIRE(!live);
    }
}
int main(void) {
    vector("", "9e9510f365f323881aab67331161f000320a3a19c5ad5d74f80c7b0b05a5e38a");
    vector("abc", "20a093840eaaf0d6cc3191a5557bdfd186621ad5e98738c8e49acd0b06c12c0e");
    XrFingerprint output; memset(&output, 0xa5, sizeof(output));
    REQUIRE(xr_compile_module_source_fingerprint(NULL, (char *)1, &output) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    for (size_t i = 0; i < sizeof(output.bytes); ++i) REQUIRE(output.bytes[i] == 0xa5);
    puts("Module fingerprint: independent SHA-256 identities and 74/80-unit work boundaries passed");
    return 0;
}
