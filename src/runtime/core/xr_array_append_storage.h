/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_array_append_storage.h - Transactional executor-neutral array append
 */
#ifndef XR_ARRAY_APPEND_STORAGE_H
#define XR_ARRAY_APPEND_STORAGE_H

#ifndef XR_BUFFER_CAPACITY_CORE_H
#include "../../shared/xr_buffer_capacity_core.h"
#endif
#include <stdint.h>
#include <string.h>

typedef struct XrArrayAppendStorage {
    void *data;
    uint32_t length;
    uint32_t capacity;
} XrArrayAppendStorage;

typedef struct XrArrayAppendAllocator {
    void *context;
    void *(*allocate)(void *context, size_t size);
    void (*release)(void *context, void *pointer);
} XrArrayAppendAllocator;

typedef enum XrArrayAppendResult {
    XR_ARRAY_APPEND_INVALID,
    XR_ARRAY_APPEND_OK,
    XR_ARRAY_APPEND_RESOURCE_LIMIT
} XrArrayAppendResult;

/* Executors prove exclusive mutation and exact element ownership. Byte copies
 * relocate existing owners; they do not duplicate their logical ownership.
 * Callbacks must not reenter. A caller using a temporary storage view must
 * publish the successful view before any further observation or allocation. */
static inline XrArrayAppendResult xr_array_append_storage(
    XrArrayAppendStorage *storage, size_t width, const void *element,
    uint32_t maximum_count, size_t maximum_bytes, const XrArrayAppendAllocator *allocator) {
    if (!storage || !allocator || !allocator->allocate || !allocator->release ||
        storage->length > storage->capacity ||
        (width && (storage->capacity > SIZE_MAX / width || !element ||
                   ((storage->capacity != 0u) != (storage->data != NULL)))) ||
        (!width && storage->data))
        return XR_ARRAY_APPEND_INVALID;
    if (width && storage->data && (uintptr_t) element >= (uintptr_t) storage->data) {
        uintptr_t offset = (uintptr_t) element - (uintptr_t) storage->data;
        size_t bytes = (size_t) storage->capacity * width;
        if (offset <= bytes &&
            (offset % width != 0u || offset / width >= storage->length))
            return XR_ARRAY_APPEND_INVALID;
    }
    size_t maximum = maximum_count;
    if (width && maximum_bytes / width < maximum)
        maximum = maximum_bytes / width;
    size_t capacity = 0u;
    if (!xr_buffer_capacity_plan(storage->length, storage->capacity, 1u, 4u,
                                 maximum, &capacity))
        return XR_ARRAY_APPEND_RESOURCE_LIMIT;
    if (width && capacity != storage->capacity) {
        void *grown = allocator->allocate(allocator->context, capacity * width);
        if (!grown)
            return XR_ARRAY_APPEND_RESOURCE_LIMIT;
        if (storage->length)
            memcpy(grown, storage->data, (size_t) storage->length * width);
        /* Read an aliased input while the old backing is still alive. */
        memcpy((uint8_t *) grown + (size_t) storage->length * width, element, width);
        void *previous = storage->data;
        storage->data = grown;
        storage->capacity = (uint32_t) capacity;
        storage->length++;
        if (previous)
            allocator->release(allocator->context, previous);
        return XR_ARRAY_APPEND_OK;
    }
    if (width)
        memmove((uint8_t *) storage->data + (size_t) storage->length * width, element, width);
    storage->capacity = (uint32_t) capacity;
    storage->length++;
    return XR_ARRAY_APPEND_OK;
}

#endif
