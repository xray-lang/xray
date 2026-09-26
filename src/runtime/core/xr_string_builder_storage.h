/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_string_builder_storage.h - Transactional owned mutable text storage
 *
 * Executors enforce exclusive mutation and supply allocation and release.
 * Growth commits only after copying; published snapshots never alias storage.
 * A zero-initialized carrier is an empty builder. Carriers must not be copied.
 */
#ifndef XR_STRING_BUILDER_STORAGE_H
#define XR_STRING_BUILDER_STORAGE_H

#ifndef XR_TEXT_KERNEL_H
#include "xr_text_kernel.h"
#endif
#ifndef XR_BUFFER_CAPACITY_CORE_H
#include "../../shared/xr_buffer_capacity_core.h"
#endif

typedef struct XrStringBuilderStorage {
    uint8_t *bytes;
    size_t size;
    size_t capacity;
    size_t scalar_count;
} XrStringBuilderStorage;

/* Both callbacks share one executor allocation domain and must not reenter. */
typedef struct XrStringBuilderAllocator {
    void *context;
    void *(*allocate)(void *context, size_t size);
    void (*release)(void *context, void *pointer);
} XrStringBuilderAllocator;

typedef enum XrStringBuilderResult {
    XR_STRING_BUILDER_INVALID = 0,
    XR_STRING_BUILDER_OK,
    XR_STRING_BUILDER_RESOURCE_LIMIT
} XrStringBuilderResult;

XR_TEXT_KERNEL_FUNCTION bool xr_string_builder_valid(const XrStringBuilderStorage *storage) {
    return storage && storage->size <= storage->capacity &&
           storage->scalar_count <= storage->size &&
           ((storage->capacity != 0u) == (storage->bytes != NULL));
}

/* maximum is the executor's permitted total buffer capacity, in bytes. */
XR_TEXT_KERNEL_FUNCTION XrStringBuilderResult xr_string_builder_append(
    XrStringBuilderStorage *storage, const uint8_t *bytes, size_t size,
    size_t maximum, const XrStringBuilderAllocator *allocator) {
    if (!xr_string_builder_valid(storage) || !allocator || !allocator->allocate ||
        !allocator->release || (!bytes && size))
        return XR_STRING_BUILDER_INVALID;
    /* Validate internal views before reading UTF-8. Keep the original buffer
     * alive during growth, so even self-append requires no pointer rebasing. */
    if (storage->bytes && bytes && (uintptr_t) bytes >= (uintptr_t) storage->bytes) {
        uintptr_t offset = (uintptr_t) bytes - (uintptr_t) storage->bytes;
        if (offset <= storage->capacity &&
            (offset > storage->size || size > storage->size - offset))
            return XR_STRING_BUILDER_INVALID;
    }
    size_t capacity = 0u;
    if (!xr_buffer_capacity_plan(storage->size, storage->capacity, size,
                                      64u, maximum, &capacity))
        return XR_STRING_BUILDER_RESOURCE_LIMIT;
    if (!xr_text_utf8_is_valid(bytes, size))
        return XR_STRING_BUILDER_INVALID;
    size_t scalars = xr_text_scalar_count(bytes, size);
    if (capacity != storage->capacity) {
        uint8_t *grown = allocator->allocate(allocator->context, capacity);
        if (!grown)
            return XR_STRING_BUILDER_RESOURCE_LIMIT;
        if (storage->size)
            memcpy(grown, storage->bytes, storage->size);
        if (size)
            memcpy(grown + storage->size, bytes, size);
        if (storage->bytes)
            allocator->release(allocator->context, storage->bytes);
        storage->bytes = grown;
        storage->capacity = capacity;
    } else if (size) {
        memmove(storage->bytes + storage->size, bytes, size);
    }
    storage->size += size;
    storage->scalar_count += scalars;
    return XR_STRING_BUILDER_OK;
}

/* NULL denotes the exact null arm of the declared append union. Other
 * operands use the canonical text formatter; u64 and unknown kinds reject. */
XR_TEXT_KERNEL_FUNCTION XrStringBuilderResult xr_string_builder_append_value(
    XrStringBuilderStorage *storage, const XrTextDisplayOperand *value,
    size_t maximum, const XrStringBuilderAllocator *allocator) {
    if (!value)
        return xr_string_builder_append(storage, (const uint8_t *) "null", 4u,
                                         maximum, allocator);
    if (value->kind == XR_TEXT_DISPLAY_STRING)
        return xr_string_builder_append(storage, value->bytes, value->size, maximum, allocator);
    if (value->kind != XR_TEXT_DISPLAY_I64 && value->kind != XR_TEXT_DISPLAY_F64 &&
        value->kind != XR_TEXT_DISPLAY_BOOL && value->kind != XR_TEXT_DISPLAY_RUNE)
        return XR_STRING_BUILDER_INVALID;
    uint8_t rendered[32];
    size_t size = xr_text_display_operand(value, NULL);
    if (!size || size > sizeof(rendered))
        return XR_STRING_BUILDER_INVALID;
    if (xr_text_display_operand(value, rendered) != size)
        return XR_STRING_BUILDER_INVALID;
    return xr_string_builder_append(storage, rendered, size, maximum, allocator);
}

XR_TEXT_KERNEL_FUNCTION bool xr_string_builder_clear(XrStringBuilderStorage *storage) {
    if (!xr_string_builder_valid(storage))
        return false;
    storage->size = 0u;
    storage->scalar_count = 0u;
    return true;
}

/* The executor wraps the fresh allocation in its immutable string carrier.
 * On failure, no pointer is published and the builder remains unchanged. */
XR_TEXT_KERNEL_FUNCTION XrStringBuilderResult xr_string_builder_snapshot(
    const XrStringBuilderStorage *storage, size_t maximum,
    const XrStringBuilderAllocator *allocator, uint8_t **out) {
    if (!out)
        return XR_STRING_BUILDER_INVALID;
    *out = NULL;
    if (!xr_string_builder_valid(storage) || !allocator || !allocator->allocate ||
        !allocator->release)
        return XR_STRING_BUILDER_INVALID;
    size_t allocation_size = storage->size ? storage->size : 1u;
    if (allocation_size > maximum)
        return XR_STRING_BUILDER_RESOURCE_LIMIT;
    uint8_t *copy = allocator->allocate(allocator->context, allocation_size);
    if (!copy)
        return XR_STRING_BUILDER_RESOURCE_LIMIT;
    if (storage->size)
        memcpy(copy, storage->bytes, storage->size);
    *out = copy;
    return XR_STRING_BUILDER_OK;
}

XR_TEXT_KERNEL_FUNCTION void xr_string_builder_dispose(
    XrStringBuilderStorage *storage, const XrStringBuilderAllocator *allocator) {
    if (!storage || !allocator || !allocator->release)
        return;
    if (storage->bytes)
        allocator->release(allocator->context, storage->bytes);
    *storage = (XrStringBuilderStorage) {0};
}

#endif // XR_STRING_BUILDER_STORAGE_H
