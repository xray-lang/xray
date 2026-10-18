/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_retired_checked.c - Complete historical packet admission boundaries
 *
 * KEY CONCEPT:
 *   Retired formats consume only the fixed header work, never an allocation.
 *   Complete current packets prove that valid admission still owns every byte.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_compile_context.h"
#include "base/xmalloc.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)

typedef struct RetiredPacket {
    const uint8_t *bytes;
    size_t length;
    uint32_t wire, semantic;
    uint8_t sha256[32];
} RetiredPacket;
typedef struct CurrentPacket {
    const char *name;
    const uint8_t *bytes;
    size_t length;
} CurrentPacket;
#include "catalog.inc.c"
#include "current.inc.c"

_Static_assert(XR_XIR_CHECKED_SCHEMA == 34u && XR_XIR_CHECKED_CONTRACT == 90u,
    "Current controls describe the complete composed packet fields");
_Static_assert(sizeof(retired_packets) / sizeof(*retired_packets) == 1213,
    "Every distinct authenticated original packet remains covered");
_Static_assert(sizeof(current_packets) / sizeof(*current_packets) == 2,
    "Both complete current acceptance controls remain covered");

typedef struct Allocation { void *pointer; size_t bytes; } Allocation;
static Allocation allocations[4096];
static size_t attempts, fail_at = SIZE_MAX;
static uint64_t physical, allocated, peak, allocation_count;

static void *observed_alloc(size_t bytes) {
    if (attempts++ == fail_at) return NULL;
    void *memory = xr_malloc(bytes);
    if (!memory) return NULL;
    size_t index = 0;
    while (index < 4096 && allocations[index].pointer) ++index;
    CHECK(index < 4096);
    allocations[index] = (Allocation){memory,bytes};
    physical += bytes; allocated += bytes; ++allocation_count;
    if (physical > peak) peak = physical;
    return memory;
}

static void observed_free(void *memory) {
    if (!memory) return;
    size_t index = 0;
    while (index < 4096 && allocations[index].pointer != memory) ++index;
    CHECK(index < 4096 && physical >= allocations[index].bytes);
    physical -= allocations[index].bytes;
    allocations[index] = (Allocation){0};
    xr_free(memory);
}

#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) observed_alloc(bytes)
#define xr_free(memory) observed_free(memory)
#include "base/xcompile_resources.c"
#undef xr_malloc
#undef xr_free

static XrXirCompileContext context_new(uint64_t work) {
    CHECK(!physical);
    attempts = 0; fail_at = SIZE_MAX;
    allocated = peak = allocation_count = 0;
    const XrCompileResourceLimits limits = {UINT64_C(67108864),UINT64_C(8388608),work};
    XrXirCompileContext context = {0};
    CHECK(xr_compile_resources_new(&limits,&context.resources) == XR_COMPILE_RESOURCE_OK);
    context.limits = xr_xir_compile_default_limits();
    CHECK(physical && attempts == 1 && allocation_count == 1);
    return context;
}

static XrCompileResourceStats stats(const XrXirCompileContext *context) {
    XrCompileResourceStats result = {0};
    CHECK(xr_compile_resources_stats(context->resources,&result) == XR_COMPILE_RESOURCE_OK);
    CHECK(result.live_bytes == physical && result.allocated_bytes == allocated);
    CHECK(result.peak_bytes == peak && result.allocation_count == allocation_count);
    return result;
}

static void context_free(XrXirCompileContext *context) {
    CHECK(stats(context).live_bytes == sizeof(XrCompileResources));
    xr_compile_resources_release(context->resources);
    context->resources = NULL;
    CHECK(!physical);
    for (size_t index = 0; index < 4096; ++index) CHECK(!allocations[index].pointer);
}

static uint64_t integer(const uint8_t *bytes, unsigned width) {
    uint64_t result = 0;
    for (unsigned index = 0; index < width; ++index) result |= (uint64_t)bytes[index] << (index * 8);
    return result;
}

static void integrity(const RetiredPacket *packet) {
    CHECK(packet->length >= 64 && !memcmp(packet->bytes,"XRCHK\0\0\0",8));
    CHECK(integer(packet->bytes + 8,4) == packet->wire);
    CHECK(integer(packet->bytes + 12,4) == packet->semantic);
    CHECK(packet->wire != XR_XIR_CHECKED_SCHEMA || packet->semantic != XR_XIR_CHECKED_CONTRACT);
    CHECK(integer(packet->bytes + 24,8) == packet->length - 64);
    uint8_t digest[32];
    XrSHA256Context hash;
    xr_sha256_init(&hash);
    xr_sha256_update(&hash,packet->bytes,32);
    xr_sha256_update(&hash,packet->bytes + 64,packet->length - 64);
    xr_sha256_final(&hash,digest);
    CHECK(!memcmp(digest,packet->bytes + 32,32));
    xr_sha256(packet->bytes,packet->length,digest);
    CHECK(!memcmp(digest,packet->sha256,32));
}

static void rejected(const XrXirCompileContext *context,const RetiredPacket *packet,
    uint64_t available,XrXirStatus expected) {
    const XrCompileResourceStats before = stats(context);
    const size_t prior_attempts = attempts;
    XrXirArtifact *artifact = NULL;
    XrXirDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_checked_read(context,packet->bytes,packet->length,&artifact,&diagnostic);
    if (status != expected) fprintf(stderr,"retired %u/%u length=%zu status=%u expected=%u work=%llu\n",
        packet->wire,packet->semantic,packet->length,status,expected,(unsigned long long)available);
    CHECK(status == expected && diagnostic.status == expected && !artifact);
    const XrCompileResourceStats after = stats(context);
    CHECK(attempts == prior_attempts && after.allocation_count == before.allocation_count);
    CHECK(after.allocated_bytes == before.allocated_bytes && after.live_bytes == before.live_bytes);
    CHECK(after.peak_bytes == before.peak_bytes && after.work >= before.work);
    CHECK(after.work - before.work <= available);
    if (expected == XR_XIR_BAD_STRUCTURE) CHECK(after.work - before.work == 32);
}

static void occupied(const XrXirCompileContext *context,const RetiredPacket *packet) {
    const XrCompileResourceStats before = stats(context);
    const size_t prior_attempts = attempts;
    XrXirArtifact *artifact = (XrXirArtifact *)(uintptr_t)1;
    XrXirDiagnostic diagnostic = {0};
    CHECK(xr_xir_compile_checked_read(context,packet->bytes,packet->length,&artifact,&diagnostic)
        == XR_XIR_BAD_STRUCTURE);
    CHECK(diagnostic.status == XR_XIR_BAD_STRUCTURE && artifact == (XrXirArtifact *)(uintptr_t)1);
    const XrCompileResourceStats after = stats(context);
    CHECK(attempts == prior_attempts && after.allocation_count == before.allocation_count);
    CHECK(after.allocated_bytes == before.allocated_bytes && after.live_bytes == before.live_bytes);
    CHECK(after.peak_bytes == before.peak_bytes && after.work == before.work);
}

static void current_control(const CurrentPacket *packet) {
    XrXirCompileContext context = context_new(UINT64_C(128000000));
    const XrCompileResourceStats before = stats(&context);
    void *input = NULL;
    CHECK(xr_compile_resources_alloc(context.resources,packet->length,&input) == XR_COMPILE_RESOURCE_OK);
    memcpy(input,packet->bytes,packet->length);
    XrXirArtifact *artifact = NULL;
    XrXirDiagnostic diagnostic = {0};
    const XrXirStatus status = xr_xir_compile_checked_read(&context,input,packet->length,&artifact,&diagnostic);
    memset(input,0xa5,packet->length);
    xr_compile_resources_free(input);
    if (status != XR_XIR_OK) fprintf(stderr,"%s current status=%u f%u b%u i%u\n",
        packet->name,status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status == XR_XIR_OK && artifact);
    CHECK(xr_xir_compile_artifact_verify(artifact,NULL) == XR_XIR_OK);
    XrXirCheckedPacket encoded = {0};
    CHECK(xr_xir_compile_checked_write(artifact,&encoded,NULL) == XR_XIR_OK);
    CHECK(encoded.length == packet->length && !memcmp(encoded.bytes,packet->bytes,encoded.length));
    xr_xir_compile_checked_packet_free(&encoded);
    xr_xir_compile_artifact_free(artifact);
    CHECK(stats(&context).live_bytes == before.live_bytes);
    context_free(&context);
}

static void originals(void) {
    XrXirCompileContext context = context_new(UINT64_C(128000000));
    for (size_t index = 0; index < 1213; ++index) {
        const RetiredPacket *packet = &retired_packets[index];
        integrity(packet);
        rejected(&context,packet,32,XR_XIR_BAD_STRUCTURE);
        fail_at = attempts;
        rejected(&context,packet,32,XR_XIR_BAD_STRUCTURE);
        occupied(&context,packet);
        CHECK(fail_at == attempts);
        fail_at = SIZE_MAX;
    }
    CHECK(stats(&context).work == 1 + UINT64_C(1213) * 64);
    context_free(&context);
}

static void prefixes(void) {
    for (size_t index = 0; index < 1213; ++index) {
        integrity(&retired_packets[index]);
        for (uint64_t available = 0; available <= 32; ++available) {
            XrXirCompileContext context = context_new(1 + available);
            fail_at = attempts;
            rejected(&context,&retired_packets[index],available,
                available < 32 ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE);
            occupied(&context,&retired_packets[index]);
            CHECK(fail_at == attempts);
            fail_at = SIZE_MAX;
            context_free(&context);
        }
    }
}

int main(int argc,char **argv) {
    const bool budget = argc == 2 && !strcmp(argv[1],"--budget-prefixes");
    CHECK(argc == 1 || budget);
    if (budget) prefixes();
    else originals();
    for (size_t index = 0; index < 2; ++index) current_control(&current_packets[index]);
    printf("1213 complete originals, %s, two detached current roundtrips, physical0 PASS\n",
        budget ? "40029 header work prefixes" : "normal, armed-allocation and occupied-output rejection");
    return 0;
}
