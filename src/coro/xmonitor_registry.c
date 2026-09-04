/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmonitor_registry.c - Synchronized channel registry for named monitors
 *
 * The registry owns allocation, locking and wakeup projection only. Callers
 * choose legal targets and channel capacities.
 */

#include "xmonitor_registry.h"

#include "xchannel.h"
#include "../base/xmalloc.h"
#include "../base/xmutex.h"
#include "../runtime/mem/xcoro_heap.h"
#include "../runtime/object/xstring.h"
#include "../runtime/value/xvalue.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/xshared.h"

#include <string.h>

typedef struct XrNodeMonitorEntry {
    char *node_name;
    XrChannel *channel;
    struct XrNodeMonitorEntry *next;
} XrNodeMonitorEntry;

typedef struct XrRemoteMonitorEntry {
    char *node_name;
    char *coroutine_name;
    XrChannel *channel;
    struct XrRemoteMonitorEntry *next;
} XrRemoteMonitorEntry;

struct XrMonitorRegistry {
    XrNodeMonitorEntry *node_monitors;
    XrRemoteMonitorEntry *remote_monitors;
    int64_t node_monitor_count;
    XrAdaptiveMutex lock;
};

static void monitor_release_unsent(XrVMRuntime *isolate, XrValue value) {
    if (!isolate || !XR_IS_PTR(value))
        return;
    if (XR_IS_STRING(value)) {
        xr_rc_release_value(NULL, value);
        return;
    }
    XrObjHeader *object = XR_VALUE_GCPTR(value);
    if (object && XR_OBJ_IS_SHARED(object) && xr_obj_drop_is_last(object))
        xr_shared_destroy_core(xr_isolate_get_runtime_core(isolate), object);
}

static void monitor_notify_string(XrVMRuntime *isolate, XrChannel *channel, const char *text) {
    XrString *string = xr_string_intern(isolate, text, (uint32_t) strlen(text), 0);
    if (!string)
        return;
    XrValue value = xr_string_value(string);
    if (!xr_channel_notify_send(channel, value))
        monitor_release_unsent(isolate, value);
}

static void node_monitor_list_destroy(XrNodeMonitorEntry *entry) {
    while (entry) {
        XrNodeMonitorEntry *next = entry->next;
        if (entry->channel)
            xr_channel_close(entry->channel);
        xr_free(entry->node_name);
        xr_free(entry);
        entry = next;
    }
}

static void remote_monitor_list_destroy(XrRemoteMonitorEntry *entry) {
    while (entry) {
        XrRemoteMonitorEntry *next = entry->next;
        if (entry->channel)
            xr_channel_close(entry->channel);
        xr_free(entry->node_name);
        xr_free(entry->coroutine_name);
        xr_free(entry);
        entry = next;
    }
}

XrMonitorRegistry *xr_monitor_registry_new(void) {
    XrMonitorRegistry *registry = (XrMonitorRegistry *) xr_calloc(1, sizeof(*registry));
    if (!registry)
        return NULL;
    xr_amutex_init(&registry->lock);
    return registry;
}

void xr_monitor_registry_destroy(XrMonitorRegistry *registry) {
    if (!registry)
        return;
    xr_amutex_lock(&registry->lock);
    XrNodeMonitorEntry *node_monitors = registry->node_monitors;
    XrRemoteMonitorEntry *remote_monitors = registry->remote_monitors;
    registry->node_monitors = NULL;
    registry->remote_monitors = NULL;
    registry->node_monitor_count = 0;
    xr_amutex_unlock(&registry->lock);
    node_monitor_list_destroy(node_monitors);
    remote_monitor_list_destroy(remote_monitors);
    xr_free(registry);
}

bool xr_monitor_registry_add_node(XrMonitorRegistry *registry, const char *node_name,
                                  XrChannel *channel) {
    if (!registry || !node_name || !*node_name || !channel)
        return false;
    XrNodeMonitorEntry *entry = (XrNodeMonitorEntry *) xr_calloc(1, sizeof(*entry));
    if (!entry)
        return false;
    entry->node_name = xr_strdup(node_name);
    if (!entry->node_name) {
        xr_free(entry);
        return false;
    }
    entry->channel = channel;
    xr_amutex_lock(&registry->lock);
    entry->next = registry->node_monitors;
    registry->node_monitors = entry;
    registry->node_monitor_count++;
    xr_amutex_unlock(&registry->lock);
    return true;
}

bool xr_monitor_registry_add_remote(XrMonitorRegistry *registry, const char *node_name,
                                    const char *coroutine_name, XrChannel *channel) {
    if (!registry || !node_name || !*node_name || !coroutine_name || !*coroutine_name || !channel)
        return false;
    XrRemoteMonitorEntry *entry = (XrRemoteMonitorEntry *) xr_calloc(1, sizeof(*entry));
    if (!entry)
        return false;
    entry->node_name = xr_strdup(node_name);
    entry->coroutine_name = xr_strdup(coroutine_name);
    if (!entry->node_name || !entry->coroutine_name) {
        xr_free(entry->node_name);
        xr_free(entry->coroutine_name);
        xr_free(entry);
        return false;
    }
    entry->channel = channel;
    xr_amutex_lock(&registry->lock);
    entry->next = registry->remote_monitors;
    registry->remote_monitors = entry;
    xr_amutex_unlock(&registry->lock);
    return true;
}

bool xr_monitor_registry_remove_remote(XrMonitorRegistry *registry, const char *node_name,
                                       const char *coroutine_name, XrChannel *channel) {
    if (!registry || !node_name || !coroutine_name || !channel)
        return false;
    XrRemoteMonitorEntry *removed = NULL;
    xr_amutex_lock(&registry->lock);
    XrRemoteMonitorEntry **cursor = &registry->remote_monitors;
    while (*cursor) {
        XrRemoteMonitorEntry *entry = *cursor;
        if (entry->channel == channel && strcmp(entry->node_name, node_name) == 0 &&
            strcmp(entry->coroutine_name, coroutine_name) == 0) {
            *cursor = entry->next;
            entry->next = NULL;
            removed = entry;
            break;
        }
        cursor = &entry->next;
    }
    xr_amutex_unlock(&registry->lock);
    if (!removed)
        return false;
    xr_free(removed->node_name);
    xr_free(removed->coroutine_name);
    xr_free(removed);
    return true;
}

void xr_monitor_registry_notify_node(XrMonitorRegistry *registry, XrVMRuntime *isolate,
                                     const char *node_name) {
    if (!registry || !isolate || !node_name)
        return;
    enum {
        INLINE_TARGETS = 16
    };
    XrChannel *inline_targets[INLINE_TARGETS];
    XrChannel **targets = inline_targets;
    uint32_t count = 0;
    uint32_t capacity = INLINE_TARGETS;

    xr_amutex_lock(&registry->lock);
    for (XrNodeMonitorEntry *entry = registry->node_monitors; entry; entry = entry->next) {
        if (xr_channel_is_closed(entry->channel) ||
            (strcmp(entry->node_name, "*") != 0 && strcmp(entry->node_name, node_name) != 0))
            continue;
        if (count == capacity) {
            XrChannel **grown = NULL;
            if (capacity <= UINT32_MAX / 2) {
                uint32_t new_capacity = capacity * 2;
                if ((size_t) new_capacity <= SIZE_MAX / sizeof(*grown)) {
                    if (targets == inline_targets) {
                        grown = (XrChannel **) xr_malloc((size_t) new_capacity * sizeof(*grown));
                        if (grown)
                            memcpy(grown, targets, (size_t) count * sizeof(*grown));
                    } else {
                        grown = (XrChannel **) xr_realloc(targets,
                                                          (size_t) new_capacity * sizeof(*grown));
                    }
                }
            }
            if (!grown)
                break;
            targets = grown;
            capacity *= 2;
        }
        targets[count++] = entry->channel;
    }
    xr_amutex_unlock(&registry->lock);

    for (uint32_t i = 0; i < count; i++)
        monitor_notify_string(isolate, targets[i], node_name);
    if (targets != inline_targets)
        xr_free(targets);
}

void xr_monitor_registry_notify_remote(XrMonitorRegistry *registry, XrVMRuntime *isolate,
                                       const char *node_name, const char *coroutine_name,
                                       const char *reason) {
    if (!registry || !isolate || !node_name || !coroutine_name || !reason)
        return;
    XrRemoteMonitorEntry *matches = NULL;
    xr_amutex_lock(&registry->lock);
    XrRemoteMonitorEntry **cursor = &registry->remote_monitors;
    while (*cursor) {
        XrRemoteMonitorEntry *entry = *cursor;
        if (strcmp(entry->node_name, node_name) == 0 &&
            strcmp(entry->coroutine_name, coroutine_name) == 0) {
            *cursor = entry->next;
            entry->next = matches;
            matches = entry;
            continue;
        }
        cursor = &entry->next;
    }
    xr_amutex_unlock(&registry->lock);

    while (matches) {
        XrRemoteMonitorEntry *next = matches->next;
        if (!xr_channel_is_closed(matches->channel))
            monitor_notify_string(isolate, matches->channel, reason);
        xr_channel_close(matches->channel);
        xr_free(matches->node_name);
        xr_free(matches->coroutine_name);
        xr_free(matches);
        matches = next;
    }
}

int64_t xr_monitor_registry_node_count(XrMonitorRegistry *registry) {
    if (!registry)
        return 0;
    xr_amutex_lock(&registry->lock);
    int64_t count = registry->node_monitor_count;
    xr_amutex_unlock(&registry->lock);
    return count;
}
