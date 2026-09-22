/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_byte_storage_provider_checks.inc.c - Physical storage and failure checks
 */
#include "base/xmalloc.h"

static size_t storage_attempt, storage_fail_at, storage_live;

static void *storage_test_allocate(size_t size, size_t alignment, bool zeroed) {
    if (++storage_attempt == storage_fail_at)
        return NULL;
    void *pointer = alignment ? xr_malloc_aligned(size, alignment) :
                               (zeroed ? xr_calloc(1u, size) : xr_malloc(size));
    if (pointer)
        ++storage_live;
    return pointer;
}

static void storage_test_free(void *pointer, size_t alignment) {
    if (!pointer)
        return;
    CHECK(storage_live > 0u, "physical storage is freed only once");
    --storage_live;
    if (alignment)
        xr_free_aligned(pointer, alignment);
    else
        xr_free(pointer);
}

#undef xr_malloc
#undef xr_calloc
#undef xr_free
#undef xr_malloc_aligned
#undef xr_free_aligned
#define xr_malloc(size) storage_test_allocate(size, 0u, false)
#define xr_calloc(count, size) storage_test_allocate((count) * (size), 0u, true)
#define xr_free(pointer) storage_test_free(pointer, 0u)
#define xr_malloc_aligned(size, alignment) storage_test_allocate(size, alignment, false)
#define xr_free_aligned(pointer, alignment) storage_test_free(pointer, alignment)
#include "execution/xr_byte_storage_provider.h"
#undef xr_malloc
#undef xr_calloc
#undef xr_free
#undef xr_malloc_aligned
#undef xr_free_aligned

static void byte_storage_physical_checks(void) {
    typedef XrProviderCallStatus (*Allocate)(void *, const XrProviderValuePack *,
                                            XrProviderValuePack *);
    const Allocate allocate[] = {xr_byte_storage_allocate, xr_byte_storage_allocate_zeroed,
                                 xr_byte_storage_allocate_aligned};
    for (size_t variant = 0u; variant < 3u; ++variant) {
        for (size_t failure = 0u; failure <= 2u; ++failure) {
            XrProviderValuePack arguments = {0}, result = {0}, length = {0};
            arguments.count = variant == 2u ? 2u : 1u;
            arguments.nodes[0].token = 3u;
            arguments.nodes[0].as.i64 = 17;
            arguments.nodes[1].token = 3u;
            arguments.nodes[1].as.i64 = 64;
            storage_attempt = 0u;
            storage_fail_at = failure;
            XrProviderCallStatus status = allocate[variant](NULL, &arguments, &result);
            if (failure) {
                CHECK(status == XR_PROVIDER_CALL_OUT_OF_MEMORY && result.count == 0u,
                      "every allocation refusal stays OOM and publishes no owner");
                CHECK(storage_attempt == failure && storage_live == 0u,
                      "allocation refusal releases its complete partial prefix");
                continue;
            }
            CHECK(status == XR_PROVIDER_CALL_OK && result.count == 1u && storage_live == 2u,
                  "successful allocation owns exactly its wrapper and bytes");
            if (status != XR_PROVIDER_CALL_OK)
                continue;
            XrProviderByteStorage *storage = result.nodes[0].as.resource.payload;
            CHECK(result.nodes[0].as.resource.owner == NULL &&
                      result.nodes[0].as.resource.destroy != NULL,
                  "host transfers a raw owner for boundary adoption");
            CHECK(xr_byte_storage_length(NULL, &result, &length) == XR_PROVIDER_CALL_OK &&
                      length.count == 1u && length.nodes[0].as.i64 == 17,
                  "borrowed length reports requested bytes without consuming storage");
            if (variant == 1u) {
                const unsigned char *bytes = storage->data;
                for (size_t index = 0u; index < 17u; ++index)
                    CHECK(bytes[index] == 0u, "zeroed allocation clears every byte");
            }
            if (variant == 2u)
                CHECK((uintptr_t)storage->data % 64u == 0u, "aligned allocation honors alignment");
            length.count = 0u;
            result.nodes[0].as.resource.id.bytes[0] ^= 1u;
            CHECK(xr_byte_storage_length(NULL, &result, &length) == XR_PROVIDER_CALL_FAILED &&
                      length.count == 0u && storage_live == 2u,
                  "foreign resource identity is refused without consuming the input");
            result.nodes[0].as.resource.destroy(storage);
            CHECK(storage_live == 0u, "physical storage reaches zero after destruction");
        }
    }
    storage_fail_at = 0u;
    XrProviderValuePack arguments = {0}, result = {0};
    arguments.count = 1u;
    arguments.nodes[0].token = 3u;
    CHECK(xr_byte_storage_allocate(NULL, &arguments, &result) == XR_PROVIDER_CALL_OK,
          "empty storage is a valid owned value");
    CHECK(storage_live == 1u && result.nodes[0].as.resource.payload != NULL,
          "empty storage retains an owner without allocating bytes");
    if (result.count)
        result.nodes[0].as.resource.destroy(result.nodes[0].as.resource.payload);
    result.count = 0u;
    arguments.nodes[0].as.i64 = -1;
    storage_attempt = 0u;
    CHECK(xr_byte_storage_allocate(NULL, &arguments, &result) == XR_PROVIDER_CALL_FAILED &&
              storage_attempt == 0u && result.count == 0u,
          "negative sizes fail before allocation");
    arguments.nodes[0].as.i64 = 17;
    arguments.count = 2u;
    arguments.nodes[1].token = 3u;
    arguments.nodes[1].as.i64 = 24;
    CHECK(xr_byte_storage_allocate_aligned(NULL, &arguments, &result) == XR_PROVIDER_CALL_FAILED &&
              storage_attempt == 0u && storage_live == 0u,
          "non-power-of-two alignment fails before allocation");
}
