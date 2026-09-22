/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_byte_storage_provider.h - Typed host boundary for owned raw byte storage
 */
#ifndef XR_BYTE_STORAGE_PROVIDER_H
#define XR_BYTE_STORAGE_PROVIDER_H

#include "xr_provider_value.h"
#include "../base/xmalloc.h"
#include "../runtime/abi/xr_stdlib_provider_keys_gen.h"
#include <string.h>

typedef struct XrProviderByteStorage {
    void *data;
    size_t size;
    size_t alignment;
} XrProviderByteStorage;

static inline void xr_byte_storage_destroy(void *payload) {
    XrProviderByteStorage *storage = payload;
    if (!storage)
        return;
    if (storage->alignment)
        xr_free_aligned(storage->data, storage->alignment);
    else
        xr_free(storage->data);
    xr_free(storage);
}

static inline XrProviderCallStatus xr_byte_storage_create(
    const XrProviderValuePack *arguments, XrProviderValuePack *result, bool zeroed, bool aligned) {
    if (!arguments || !result || result->count || arguments->count != (aligned ? 2u : 1u) ||
        arguments->nodes[0].token != 3u || arguments->nodes[0].as.i64 < 0 ||
        (uint64_t)arguments->nodes[0].as.i64 > SIZE_MAX)
        return XR_PROVIDER_CALL_FAILED;
    size_t size = (size_t)arguments->nodes[0].as.i64;
    size_t alignment = 0u;
    if (aligned) {
        int64_t requested = arguments->nodes[1].as.i64;
        if (arguments->nodes[1].token != 3u || requested < (int64_t)sizeof(void *) ||
            (uint64_t)requested > SIZE_MAX || (requested & (requested - 1)) != 0)
            return XR_PROVIDER_CALL_FAILED;
        alignment = (size_t)requested;
    }
    XrProviderByteStorage *storage = xr_calloc(1u, sizeof(*storage));
    if (!storage)
        return XR_PROVIDER_CALL_OUT_OF_MEMORY;
    storage->size = size;
    storage->alignment = alignment;
    if (size) {
        storage->data = alignment ? xr_malloc_aligned(size, alignment) :
                                   (zeroed ? xr_calloc(1u, size) : xr_malloc(size));
        if (!storage->data) {
            xr_byte_storage_destroy(storage);
            return XR_PROVIDER_CALL_OUT_OF_MEMORY;
        }
    }
    result->count = 1u;
    result->nodes[0] = (XrProviderValueNode){.token = 7u};
    result->nodes[0].as.resource.id = (XrStableId)XR_PROVIDER_RESOURCE_MEM___BUFFERSTORAGE_ID;
    result->nodes[0].as.resource.payload = storage;
    result->nodes[0].as.resource.destroy = xr_byte_storage_destroy;
    return XR_PROVIDER_CALL_OK;
}

static inline XrProviderCallStatus xr_byte_storage_allocate(
    void *context, const XrProviderValuePack *arguments, XrProviderValuePack *result) {
    (void)context;
    return xr_byte_storage_create(arguments, result, false, false);
}

static inline XrProviderCallStatus xr_byte_storage_allocate_zeroed(
    void *context, const XrProviderValuePack *arguments, XrProviderValuePack *result) {
    (void)context;
    return xr_byte_storage_create(arguments, result, true, false);
}

static inline XrProviderCallStatus xr_byte_storage_allocate_aligned(
    void *context, const XrProviderValuePack *arguments, XrProviderValuePack *result) {
    (void)context;
    return xr_byte_storage_create(arguments, result, false, true);
}

static inline XrProviderCallStatus xr_byte_storage_length(
    void *context, const XrProviderValuePack *arguments, XrProviderValuePack *result) {
    (void)context;
    const XrStableId identity = XR_PROVIDER_RESOURCE_MEM___BUFFERSTORAGE_ID;
    if (!arguments || !result || result->count || arguments->count != 1u ||
        arguments->nodes[0].token != 7u || !arguments->nodes[0].as.resource.payload ||
        memcmp(identity.bytes, arguments->nodes[0].as.resource.id.bytes, sizeof(identity.bytes)) != 0)
        return XR_PROVIDER_CALL_FAILED;
    const XrProviderByteStorage *storage = arguments->nodes[0].as.resource.payload;
    result->count = 1u;
    result->nodes[0] = (XrProviderValueNode){.token = 3u};
    result->nodes[0].as.i64 = (int64_t)storage->size;
    return XR_PROVIDER_CALL_OK;
}

#endif  // XR_BYTE_STORAGE_PROVIDER_H
