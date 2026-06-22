/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcoro_registry.c - Named coroutine registry and lifecycle monitoring
 *
 * KEY CONCEPT:
 *   Open-addressing hash table for name→coroutine mapping.
 *   Monitor list per coroutine for exit notifications via Channel.
 */

#include "xcoro_registry.h"
#include "../base/xhash.h"
#include "../base/xmalloc.h"
#include "../base/xchecks.h"
#include <string.h>

/* ========== Hash Table Internals ========== */

static uint32_t hash_name(const char *name) {
    uint32_t h = xr_hash_bytes(name, strlen(name));
    // Hash must never be 0 (0 = empty slot marker)
    return h ? h : 1;
}

static void registry_grow(XrCoroRegistry *reg) {
    uint32_t old_cap = reg->capacity;
    XrCoroRegEntry *old_entries = reg->entries;

    uint32_t new_cap = old_cap * 2;
    XR_DCHECK((new_cap & (new_cap - 1)) == 0, "registry_grow: capacity not power-of-2");
    XrCoroRegEntry *new_entries = (XrCoroRegEntry *) xr_calloc(new_cap, sizeof(XrCoroRegEntry));
    XR_CHECK(new_entries != NULL, "coro registry grow allocation failed");

    // Rehash all existing entries
    for (uint32_t i = 0; i < old_cap; i++) {
        if (old_entries[i].hash == 0)
            continue;
        uint32_t idx = old_entries[i].hash & (new_cap - 1);
        while (new_entries[idx].hash != 0) {
            idx = (idx + 1) & (new_cap - 1);
        }
        new_entries[idx] = old_entries[i];
    }

    reg->entries = new_entries;
    reg->capacity = new_cap;
    xr_free(old_entries);
}

// Find slot for name. Returns index.
// If found: entries[idx].hash != 0 && strcmp matches
// If not found: entries[idx].hash == 0 (first empty slot)
static uint32_t registry_find_slot(XrCoroRegistry *reg, const char *name, uint32_t h) {
    uint32_t mask = reg->capacity - 1;
    uint32_t idx = h & mask;
    while (reg->entries[idx].hash != 0) {
        if (reg->entries[idx].hash == h && strcmp(reg->entries[idx].name, name) == 0) {
            return idx;  // found
        }
        idx = (idx + 1) & mask;
    }
    return idx;  // empty slot
}

/* ========== Registry API ========== */

void xr_coro_registry_init(XrCoroRegistry *reg) {
    XR_DCHECK(reg != NULL, "coro_registry_init: NULL reg");
    reg->capacity = XR_CORO_REG_INITIAL_CAP;
    reg->count = 0;
    reg->entries = (XrCoroRegEntry *) xr_calloc(reg->capacity, sizeof(XrCoroRegEntry));
    XR_CHECK(reg->entries != NULL, "coro registry init allocation failed");
    xr_amutex_init(&reg->lock);
}

void xr_coro_registry_destroy(XrCoroRegistry *reg) {
    XR_DCHECK(reg != NULL, "coro_registry_destroy: NULL reg");
    if (reg->entries) {
        for (uint32_t i = 0; i < reg->capacity; i++) {
            if (reg->entries[i].hash != 0 && reg->entries[i].name) {
                xr_free((void *) reg->entries[i].name);
            }
        }
        xr_free(reg->entries);
        reg->entries = NULL;
    }
    reg->capacity = 0;
    reg->count = 0;
}

bool xr_coro_registry_register(XrCoroRegistry *reg, const char *name, XrCoroutine *coro) {
    if (!reg || !name || !coro)
        return false;

    xr_amutex_lock(&reg->lock);

    // Grow if load factor exceeded
    if (reg->count * 100 >= reg->capacity * XR_CORO_REG_LOAD_FACTOR) {
        registry_grow(reg);
    }

    uint32_t h = hash_name(name);
    uint32_t idx = registry_find_slot(reg, name, h);

    if (reg->entries[idx].hash != 0) {
        // Name already taken
        xr_amutex_unlock(&reg->lock);
        return false;
    }

    reg->entries[idx].name = xr_strdup(name);
    reg->entries[idx].coro = coro;
    reg->entries[idx].hash = h;
    reg->count++;
    XR_DCHECK(reg->count <= reg->capacity, "registry_register: count > capacity");

    xr_amutex_unlock(&reg->lock);
    return true;
}

void xr_coro_registry_unregister(XrCoroRegistry *reg, const char *name) {
    if (!reg || !name)
        return;

    xr_amutex_lock(&reg->lock);

    uint32_t h = hash_name(name);
    uint32_t idx = registry_find_slot(reg, name, h);

    if (reg->entries[idx].hash != 0) {
        // Delete: mark slot empty and rehash following cluster
        reg->entries[idx].hash = 0;
        xr_free((void *) reg->entries[idx].name);
        reg->entries[idx].name = NULL;
        reg->entries[idx].coro = NULL;
        reg->count--;

        // Robin Hood: rehash entries that may have been displaced
        uint32_t mask = reg->capacity - 1;
        uint32_t j = (idx + 1) & mask;
        while (reg->entries[j].hash != 0) {
            uint32_t ideal = reg->entries[j].hash & mask;
            // If this entry's ideal position is at or before the deleted slot,
            // it needs to be moved back
            if ((j > idx && (ideal <= idx || ideal > j)) ||
                (j < idx && (ideal <= idx && ideal > j))) {
                reg->entries[idx] = reg->entries[j];
                reg->entries[j].hash = 0;
                reg->entries[j].name = NULL;
                reg->entries[j].coro = NULL;
                idx = j;
            }
            j = (j + 1) & mask;
        }
    }

    xr_amutex_unlock(&reg->lock);
}

XrCoroutine *xr_coro_registry_whereis(XrCoroRegistry *reg, const char *name) {
    if (!reg || !name)
        return NULL;

    xr_amutex_lock(&reg->lock);

    uint32_t h = hash_name(name);
    uint32_t idx = registry_find_slot(reg, name, h);
    XrCoroutine *result = NULL;

    if (reg->entries[idx].hash != 0) {
        result = reg->entries[idx].coro;
    }

    xr_amutex_unlock(&reg->lock);
    return result;
}
