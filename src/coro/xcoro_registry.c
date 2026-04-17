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
#include "xcoroutine.h"
#include "xchannel.h"
#include "../base/xhash.h"
#include "../base/xmalloc.h"
#include "../base/xchecks.h"
#include "../runtime/value/xvalue.h"
#include "../runtime/object/xstring.h"
#include "../runtime/xisolate_api.h"
#include "xexec_state.h"
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
    XrCoroRegEntry *new_entries = (XrCoroRegEntry *)xr_calloc(new_cap, sizeof(XrCoroRegEntry));
    XR_CHECK(new_entries != NULL, "coro registry grow allocation failed");

    // Rehash all existing entries
    for (uint32_t i = 0; i < old_cap; i++) {
        if (old_entries[i].hash == 0) continue;
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
        if (reg->entries[idx].hash == h &&
            strcmp(reg->entries[idx].name, name) == 0) {
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
    reg->entries = (XrCoroRegEntry *)xr_calloc(reg->capacity, sizeof(XrCoroRegEntry));
    XR_CHECK(reg->entries != NULL, "coro registry init allocation failed");
    xr_spinlock_init(&reg->lock);
}

void xr_coro_registry_destroy(XrCoroRegistry *reg) {
    XR_DCHECK(reg != NULL, "coro_registry_destroy: NULL reg");
    if (reg->entries) {
        for (uint32_t i = 0; i < reg->capacity; i++) {
            if (reg->entries[i].hash != 0 && reg->entries[i].name) {
                xr_free((void *)reg->entries[i].name);
            }
        }
        xr_free(reg->entries);
        reg->entries = NULL;
    }
    reg->capacity = 0;
    reg->count = 0;
}

bool xr_coro_registry_register(XrCoroRegistry *reg, const char *name, XrCoroutine *coro) {
    if (!reg || !name || !coro) return false;

    xr_spinlock_lock(&reg->lock);

    // Grow if load factor exceeded
    if (reg->count * 100 >= reg->capacity * XR_CORO_REG_LOAD_FACTOR) {
        registry_grow(reg);
    }

    uint32_t h = hash_name(name);
    uint32_t idx = registry_find_slot(reg, name, h);

    if (reg->entries[idx].hash != 0) {
        // Name already taken
        xr_spinlock_unlock(&reg->lock);
        return false;
    }

    reg->entries[idx].name = xr_strdup(name);
    reg->entries[idx].coro = coro;
    reg->entries[idx].hash = h;
    reg->count++;
    XR_DCHECK(reg->count <= reg->capacity, "registry_register: count > capacity");

    xr_spinlock_unlock(&reg->lock);
    return true;
}

void xr_coro_registry_unregister(XrCoroRegistry *reg, const char *name) {
    if (!reg || !name) return;

    xr_spinlock_lock(&reg->lock);

    uint32_t h = hash_name(name);
    uint32_t idx = registry_find_slot(reg, name, h);

    if (reg->entries[idx].hash != 0) {
        // Delete: mark slot empty and rehash following cluster
        reg->entries[idx].hash = 0;
        xr_free((void *)reg->entries[idx].name);
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

    xr_spinlock_unlock(&reg->lock);
}

XrCoroutine *xr_coro_registry_whereis(XrCoroRegistry *reg, const char *name) {
    if (!reg || !name) return NULL;

    xr_spinlock_lock(&reg->lock);

    uint32_t h = hash_name(name);
    uint32_t idx = registry_find_slot(reg, name, h);
    XrCoroutine *result = NULL;

    if (reg->entries[idx].hash != 0) {
        result = reg->entries[idx].coro;
    }

    xr_spinlock_unlock(&reg->lock);
    return result;
}

/* ========== Monitor API ========== */

XrChannel *xr_coro_monitor(XrayIsolate *X, XrCoroRegistry *reg, const char *name) {
    if (!X || !reg || !name) return NULL;

    // Create notification channel (buffered=1 so send never blocks)
    XrChannel *ch = xr_channel_new(X, 1);
    if (!ch) return NULL;

    xr_spinlock_lock(&reg->lock);

    uint32_t h = hash_name(name);
    uint32_t idx = registry_find_slot(reg, name, h);

    if (reg->entries[idx].hash == 0) {
        // Coroutine not found — send immediate "noproc" notification
        xr_spinlock_unlock(&reg->lock);
        XrString *s = xr_string_new(X, "noproc", 6);
        xr_channel_try_send(ch, s ? xr_string_value(s) : xr_null());
        return ch;
    }

    XrCoroutine *coro = reg->entries[idx].coro;

    // Check if already done
    if (xr_coro_flags_has(coro, XR_CORO_FLG_DONE)) {
        xr_spinlock_unlock(&reg->lock);
        XrString *s = xr_string_new(X, "noproc", 6);
        xr_channel_try_send(ch, s ? xr_string_value(s) : xr_null());
        return ch;
    }

    // Add monitor to coroutine's watched_by list (lazy-alloc ext)
    XrCoroExt *ext = xr_coro_ensure_ext(coro);
    XR_CHECK(ext != NULL, "coro ext allocation failed");
    XrCoroMonitor *mon = (XrCoroMonitor *)xr_malloc(sizeof(XrCoroMonitor));
    XR_CHECK(mon != NULL, "coro monitor allocation failed");
    mon->channel = ch;
    mon->next = ext->watched_by;
    ext->watched_by = mon;

    xr_spinlock_unlock(&reg->lock);
    return ch;
}

void xr_coro_demonitor(XrCoroRegistry *reg, XrCoroutine *coro, XrChannel *ch) {
    if (!reg || !coro || !ch) return;

    xr_spinlock_lock(&reg->lock);

    // Walk watched_by list and remove matching entry
    if (!coro->ext) { xr_spinlock_unlock(&reg->lock); return; }
    XrCoroMonitor **pp = &coro->ext->watched_by;
    while (*pp) {
        if ((*pp)->channel == ch) {
            XrCoroMonitor *victim = *pp;
            *pp = victim->next;
            xr_free(victim);
            break;
        }
        pp = &(*pp)->next;
    }

    xr_spinlock_unlock(&reg->lock);
}

void xr_coro_notify_monitors(XrayIsolate *X, XrCoroRegistry *reg, XrCoroutine *coro, const char *reason) {
    if (!coro) return;
    if (!coro->ext || !coro->ext->watched_by) return;  // fast path: no monitors

    // Detach the list under lock, then send notifications outside lock
    XrCoroMonitor *mon = NULL;
    if (reg) {
        xr_spinlock_lock(&reg->lock);
        mon = coro->ext->watched_by;
        coro->ext->watched_by = NULL;
        xr_spinlock_unlock(&reg->lock);
    } else {
        mon = coro->ext->watched_by;
        coro->ext->watched_by = NULL;
    }

    const char *r = reason ? reason : "unknown";
    XrString *reason_str = xr_string_new(X, r, strlen(r));
    XrValue reason_val = reason_str ? xr_string_value(reason_str) : xr_null();

    while (mon) {
        XrCoroMonitor *next = mon->next;
        xr_channel_notify_send(mon->channel, reason_val);
        xr_free(mon);
        mon = next;
    }
}

void xr_coro_on_exit(XrayIsolate *X, XrCoroutine *coro) {
    if (!X || !coro) return;
    if (!coro->name) return;  // fast path: anonymous coroutines

    XrScheduler *sched = (XrScheduler *)xr_isolate_get_vm_state(X)->scheduler;
    if (sched && sched->coro_registry) {
        xr_coro_registry_unregister(sched->coro_registry, coro->name);
    }
}
