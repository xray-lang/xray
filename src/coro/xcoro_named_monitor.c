/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcoro_named_monitor.c - Named coroutine monitor notifications.
 */

#include "xcoro_registry.h"
#include "xchannel.h"
#include "xcoroutine.h"
#include "../base/xchecks.h"
#include "../base/xhash.h"
#include "../base/xmalloc.h"
#include "../runtime/object/xstring.h"
#include "../runtime/value/xvalue.h"
#include <string.h>

static uint32_t monitor_hash_name(const char *name) {
    uint32_t h = xr_hash_bytes(name, strlen(name));
    return h ? h : 1;
}

static uint32_t monitor_find_slot(XrCoroRegistry *reg, const char *name, uint32_t h) {
    uint32_t mask = reg->capacity - 1;
    uint32_t idx = h & mask;
    while (reg->entries[idx].hash != 0) {
        if (reg->entries[idx].hash == h && strcmp(reg->entries[idx].name, name) == 0)
            return idx;
        idx = (idx + 1) & mask;
    }
    return idx;
}

static void monitor_send_noproc(XrVMRuntime *X, XrChannel *ch) {
    XrString *s = xr_string_new(X, "noproc", 6);
    xr_channel_try_send(ch, s ? xr_string_value(s) : xr_null());
}

XrChannel *xr_coro_monitor(XrVMRuntime *X, XrCoroRegistry *reg, const char *name) {
    if (!X || !reg || !name)
        return NULL;

    XrChannel *ch = xr_channel_new_vm(X, 1);
    if (!ch)
        return NULL;

    xr_amutex_lock(&reg->lock);

    uint32_t h = monitor_hash_name(name);
    uint32_t idx = monitor_find_slot(reg, name, h);

    if (reg->entries[idx].hash == 0) {
        xr_amutex_unlock(&reg->lock);
        monitor_send_noproc(X, ch);
        return ch;
    }

    XrCoroutine *coro = reg->entries[idx].coro;
    if (xr_coro_flags_has(coro, XR_CORO_FLG_DONE)) {
        xr_amutex_unlock(&reg->lock);
        monitor_send_noproc(X, ch);
        return ch;
    }

    XrCoroExt *ext = xr_coro_ensure_ext(coro);
    XR_CHECK(ext != NULL, "coro ext allocation failed");
    XrCoroMonitor *mon = (XrCoroMonitor *) xr_malloc(sizeof(XrCoroMonitor));
    XR_CHECK(mon != NULL, "coro monitor allocation failed");
    mon->channel = ch;
    mon->next = ext->watched_by;
    ext->watched_by = mon;

    xr_amutex_unlock(&reg->lock);
    return ch;
}

void xr_coro_demonitor(XrCoroRegistry *reg, XrCoroutine *coro, XrChannel *ch) {
    if (!reg || !coro || !ch)
        return;

    xr_amutex_lock(&reg->lock);

    if (!coro->ext) {
        xr_amutex_unlock(&reg->lock);
        return;
    }
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

    xr_amutex_unlock(&reg->lock);
}

void xr_coro_notify_monitors(XrVMRuntime *X, XrCoroRegistry *reg, XrCoroutine *coro,
                             const char *reason) {
    if (!coro)
        return;
    if (!coro->ext || !coro->ext->watched_by)
        return;

    XrCoroMonitor *mon = NULL;
    if (reg) {
        xr_amutex_lock(&reg->lock);
        mon = coro->ext->watched_by;
        coro->ext->watched_by = NULL;
        xr_amutex_unlock(&reg->lock);
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
