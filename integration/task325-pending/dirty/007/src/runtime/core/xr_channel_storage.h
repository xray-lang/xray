/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_channel_storage.h - Executor-independent owned channel message storage
 *
 * The executor allocates the header and slots and supplies independently live
 * message owners. Queue operations and close require its channel lock. Retain
 * requires a live owner; every operation keeps that owner until it completes.
 * Only the last release drains messages and permits freeing header and slots.
 * Waiter registration and wake publication belong to the scheduler.
 */

#ifndef XR_CHANNEL_STORAGE_H
#define XR_CHANNEL_STORAGE_H

#ifndef XR_CHANNEL_BUFFER_H
#include "xr_channel_buffer.h"
#endif
#ifndef XR_ATOMIC_COMPAT_H
#include "../../shared/xr_atomic_compat.h"
#endif
#include <stddef.h>

typedef struct XrChannelMessageOwner {
    void *value;
    void (*drop)(void *value);
} XrChannelMessageOwner;

typedef struct XrChannelStorage {
    _Atomic(uint32_t) owners;
    _Atomic(bool) closed;
    XrChannelBufferState buffer;
    XrChannelMessageOwner *slots;
} XrChannelStorage;

typedef enum XrChannelStorageResult {
    XR_CHANNEL_STORAGE_INVALID = 0,
    XR_CHANNEL_STORAGE_OK,
    XR_CHANNEL_STORAGE_WOULD_BLOCK,
    XR_CHANNEL_STORAGE_CLOSED,
    XR_CHANNEL_STORAGE_LAST_OWNER
} XrChannelStorageResult;

XR_CHANNEL_CORE_FUNCTION bool xr_channel_storage_init(XrChannelStorage *storage,
                                           XrChannelMessageOwner *slots, uint32_t capacity) {
    if (!storage || (capacity != 0u && !slots) ||
        (capacity != 0u && sizeof(*slots) > SIZE_MAX / capacity))
        return false;
    atomic_init(&storage->owners, 1u);
    atomic_init(&storage->closed, false);
    storage->buffer = (XrChannelBufferState) {.capacity = capacity};
    storage->slots = slots;
    for (uint32_t index = 0u; index < capacity; ++index)
        slots[index] = (XrChannelMessageOwner) {0};
    return true;
}

XR_CHANNEL_CORE_FUNCTION bool xr_channel_storage_retain(XrChannelStorage *storage) {
    if (!storage)
        return false;
    uint32_t count = atomic_load_explicit(&storage->owners, memory_order_relaxed);
    while (count != 0u && count != UINT32_MAX) {
        if (atomic_compare_exchange_weak_explicit(&storage->owners, &count, count + 1u,
                                                  memory_order_relaxed, memory_order_relaxed))
            return true;
    }
    return false;
}

XR_CHANNEL_CORE_FUNCTION XrChannelStorageResult xr_channel_storage_send(
    XrChannelStorage *storage, XrChannelMessageOwner *message) {
    if (!storage || !message || !message->value || !message->drop ||
        !atomic_load_explicit(&storage->owners, memory_order_relaxed))
        return XR_CHANNEL_STORAGE_INVALID;
    if (atomic_load_explicit(&storage->closed, memory_order_relaxed))
        return XR_CHANNEL_STORAGE_CLOSED;
    if (!xr_channel_buffer_valid(&storage->buffer))
        return XR_CHANNEL_STORAGE_INVALID;
    uint32_t slot;
    if (!xr_channel_buffer_push(&storage->buffer, &slot))
        return XR_CHANNEL_STORAGE_WOULD_BLOCK;
    storage->slots[slot] = *message;
    *message = (XrChannelMessageOwner) {0};
    return XR_CHANNEL_STORAGE_OK;
}

XR_CHANNEL_CORE_FUNCTION XrChannelStorageResult xr_channel_storage_receive(
    XrChannelStorage *storage, XrChannelMessageOwner *message) {
    if (!storage || !message || message->value || message->drop ||
        !atomic_load_explicit(&storage->owners, memory_order_relaxed) ||
        !xr_channel_buffer_valid(&storage->buffer))
        return XR_CHANNEL_STORAGE_INVALID;
    uint32_t slot;
    if (!xr_channel_buffer_pop(&storage->buffer, &slot))
        return atomic_load_explicit(&storage->closed, memory_order_relaxed)
                   ? XR_CHANNEL_STORAGE_CLOSED : XR_CHANNEL_STORAGE_WOULD_BLOCK;
    *message = storage->slots[slot];
    storage->slots[slot] = (XrChannelMessageOwner) {0};
    return XR_CHANNEL_STORAGE_OK;
}

/* Close preserves queued owners so receivers can drain them. */
XR_CHANNEL_CORE_FUNCTION bool xr_channel_storage_close(XrChannelStorage *storage) {
    if (!storage || !atomic_load_explicit(&storage->owners, memory_order_relaxed))
        return false;
    return !atomic_exchange_explicit(&storage->closed, true, memory_order_release);
}

/* Final callbacks run with no remaining owner and no channel lock held. An
 * executor must remove pending registrations before surrendering their owners. */
XR_CHANNEL_CORE_FUNCTION XrChannelStorageResult xr_channel_storage_release(XrChannelStorage *storage) {
    if (!storage)
        return XR_CHANNEL_STORAGE_INVALID;
    uint32_t count = atomic_load_explicit(&storage->owners, memory_order_relaxed);
    while (count != 0u) {
        if (!atomic_compare_exchange_weak_explicit(&storage->owners, &count, count - 1u,
                                                   memory_order_acq_rel, memory_order_relaxed))
            continue;
        if (count != 1u)
            return XR_CHANNEL_STORAGE_OK;
        atomic_store_explicit(&storage->closed, true, memory_order_release);
        uint32_t slot;
        while (xr_channel_buffer_pop(&storage->buffer, &slot)) {
            XrChannelMessageOwner message = storage->slots[slot];
            storage->slots[slot] = (XrChannelMessageOwner) {0};
            message.drop(message.value);
        }
        return XR_CHANNEL_STORAGE_LAST_OWNER;
    }
    return XR_CHANNEL_STORAGE_INVALID;
}

#endif // XR_CHANNEL_STORAGE_H
