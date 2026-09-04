/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtombstone_registry.c - Expiring synchronized name tombstones
 *
 * Retention is selected by the source-language caller. This file only owns
 * bounded storage, expiration projection and synchronization.
 */

#include "xtombstone_registry.h"

#include "../base/xmalloc.h"
#include "../base/xmutex.h"

#include <string.h>

typedef struct XrTombstoneEntry {
    char *name;
    int64_t marked_at_ms;
} XrTombstoneEntry;

struct XrTombstoneRegistry {
    XrTombstoneEntry *entries;
    uint32_t count;
    uint32_t capacity;
    int64_t retention_ms;
    XrAdaptiveMutex lock;
};

static void tombstone_prune_locked(XrTombstoneRegistry *registry, int64_t now_ms) {
    uint32_t write = 0;
    for (uint32_t read = 0; read < registry->count; read++) {
        XrTombstoneEntry entry = registry->entries[read];
        bool expired =
            now_ms >= entry.marked_at_ms && now_ms - entry.marked_at_ms >= registry->retention_ms;
        if (expired) {
            xr_free(entry.name);
            continue;
        }
        if (write != read)
            registry->entries[write] = entry;
        write++;
    }
    registry->count = write;
}

XrTombstoneRegistry *xr_tombstone_registry_new(uint32_t initial_capacity, int64_t retention_ms) {
    if (initial_capacity == 0 || retention_ms <= 0 ||
        (size_t) initial_capacity > SIZE_MAX / sizeof(XrTombstoneEntry))
        return NULL;
    XrTombstoneRegistry *registry = (XrTombstoneRegistry *) xr_calloc(1, sizeof(*registry));
    if (!registry)
        return NULL;
    registry->entries = (XrTombstoneEntry *) xr_calloc(initial_capacity, sizeof(XrTombstoneEntry));
    if (!registry->entries) {
        xr_free(registry);
        return NULL;
    }
    registry->capacity = initial_capacity;
    registry->retention_ms = retention_ms;
    xr_amutex_init(&registry->lock);
    return registry;
}

void xr_tombstone_registry_destroy(XrTombstoneRegistry *registry) {
    if (!registry)
        return;
    for (uint32_t i = 0; i < registry->count; i++)
        xr_free(registry->entries[i].name);
    xr_free(registry->entries);
    xr_free(registry);
}

bool xr_tombstone_registry_add(XrTombstoneRegistry *registry, const char *name, int64_t now_ms) {
    if (!registry || !name || !*name)
        return false;
    char *owned_name = xr_strdup(name);
    if (!owned_name)
        return false;

    xr_amutex_lock(&registry->lock);
    tombstone_prune_locked(registry, now_ms);
    for (uint32_t i = 0; i < registry->count; i++) {
        if (strcmp(registry->entries[i].name, name) == 0) {
            registry->entries[i].marked_at_ms = now_ms;
            xr_amutex_unlock(&registry->lock);
            xr_free(owned_name);
            return true;
        }
    }

    if (registry->count == registry->capacity) {
        uint32_t new_capacity =
            registry->capacity <= UINT32_MAX / 2 ? registry->capacity * 2 : UINT32_MAX;
        XrTombstoneEntry *grown = NULL;
        if (new_capacity > registry->capacity &&
            (size_t) new_capacity <= SIZE_MAX / sizeof(*grown)) {
            grown = (XrTombstoneEntry *) xr_realloc(registry->entries,
                                                    (size_t) new_capacity * sizeof(*grown));
        }
        if (grown) {
            registry->entries = grown;
            registry->capacity = new_capacity;
        } else {
            xr_free(registry->entries[0].name);
            memmove(registry->entries, registry->entries + 1,
                    (size_t) (registry->count - 1) * sizeof(*registry->entries));
            registry->count--;
        }
    }

    registry->entries[registry->count++] =
        (XrTombstoneEntry) {.name = owned_name, .marked_at_ms = now_ms};
    xr_amutex_unlock(&registry->lock);
    return true;
}

bool xr_tombstone_registry_contains(XrTombstoneRegistry *registry, const char *name,
                                    int64_t now_ms) {
    if (!registry || !name)
        return false;
    bool found = false;
    xr_amutex_lock(&registry->lock);
    tombstone_prune_locked(registry, now_ms);
    for (uint32_t i = 0; i < registry->count; i++) {
        if (strcmp(registry->entries[i].name, name) == 0) {
            found = true;
            break;
        }
    }
    xr_amutex_unlock(&registry->lock);
    return found;
}

int64_t xr_tombstone_registry_count(XrTombstoneRegistry *registry, int64_t now_ms) {
    if (!registry)
        return 0;
    xr_amutex_lock(&registry->lock);
    tombstone_prune_locked(registry, now_ms);
    int64_t count = registry->count;
    xr_amutex_unlock(&registry->lock);
    return count;
}
