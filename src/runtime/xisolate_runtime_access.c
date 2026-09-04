/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xisolate_runtime_access.c - runtime-only isolate field accessors
 */

#include "xisolate_api.h"
#include "xisolate_internal.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "core/xr_runtime_core.h"

typedef struct XrProviderLifecycleEntry {
    void **slot;
    XrCFunctionPtr shutdown;
    struct XrProviderLifecycleEntry *next;
} XrProviderLifecycleEntry;

static XrProviderLifecycleEntry **provider_entry_link(XrVMRuntime *isolate, void **slot,
                                                      XrCFunctionPtr shutdown) {
    XrProviderLifecycleEntry **link = &isolate->provider_lifecycle_entries;
    while (*link) {
        if ((*link)->slot == slot && (!shutdown || (*link)->shutdown == shutdown))
            return link;
        link = &(*link)->next;
    }
    return NULL;
}

void *xr_isolate_provider_acquire(XrVMRuntime *isolate, void **slot,
                                  XrProviderRetainFn retain) {
    if (!isolate || !slot || !retain)
        return NULL;

    void *provider = NULL;
    xr_amutex_lock(&isolate->provider_lifecycle_lock);
    if (!isolate->provider_lifecycle_closing && provider_entry_link(isolate, slot, NULL)) {
        provider = *slot;
        if (provider)
            retain(provider);
    }
    xr_amutex_unlock(&isolate->provider_lifecycle_lock);
    return provider;
}

bool xr_isolate_provider_publish(XrVMRuntime *isolate, void **slot, void *provider,
                                 XrCFunctionPtr shutdown) {
    if (!isolate || !slot || !provider || !shutdown)
        return false;

    XrProviderLifecycleEntry *entry = (XrProviderLifecycleEntry *) xr_malloc(sizeof(*entry));
    if (!entry)
        return false;
    entry->slot = slot;
    entry->shutdown = shutdown;

    xr_amutex_lock(&isolate->provider_lifecycle_lock);
    bool published = !isolate->provider_lifecycle_closing && *slot == NULL &&
                     !provider_entry_link(isolate, slot, NULL);
    if (published) {
        entry->next = isolate->provider_lifecycle_entries;
        isolate->provider_lifecycle_entries = entry;
        *slot = provider;
    }
    xr_amutex_unlock(&isolate->provider_lifecycle_lock);
    if (!published)
        xr_free(entry);
    return published;
}

void *xr_isolate_provider_detach(XrVMRuntime *isolate, void **slot, XrCFunctionPtr shutdown) {
    if (!isolate || !slot || !shutdown)
        return NULL;

    XrProviderLifecycleEntry *entry = NULL;
    void *provider = NULL;
    xr_amutex_lock(&isolate->provider_lifecycle_lock);
    XrProviderLifecycleEntry **link = provider_entry_link(isolate, slot, shutdown);
    if (link) {
        entry = *link;
        *link = entry->next;
        provider = *slot;
        *slot = NULL;
    }
    xr_amutex_unlock(&isolate->provider_lifecycle_lock);
    xr_free(entry);
    return provider;
}

void xr_isolate_shutdown_providers(XrVMRuntime *isolate) {
    if (!isolate)
        return;

    xr_amutex_lock(&isolate->provider_lifecycle_lock);
    isolate->provider_lifecycle_closing = true;
    XrProviderLifecycleEntry *entry = isolate->provider_lifecycle_entries;
    xr_amutex_unlock(&isolate->provider_lifecycle_lock);

    while (entry) {
        void **slot = entry->slot;
        XrCFunctionPtr shutdown = entry->shutdown;
        (void) shutdown(isolate, NULL, 0);

        xr_amutex_lock(&isolate->provider_lifecycle_lock);
        entry = isolate->provider_lifecycle_entries;
        XR_CHECK(!provider_entry_link(isolate, slot, shutdown),
                 "provider shutdown leaf must detach its lifecycle registration");
        xr_amutex_unlock(&isolate->provider_lifecycle_lock);
    }
}

XrRuntimeCore *xr_isolate_get_runtime_core(XrVMRuntime *X) {
    return X ? X->core_rt : NULL;
}

XrRuntime *xr_isolate_get_scheduler_runtime(XrVMRuntime *X) {
    return X ? X->vm.scheduler : NULL;
}

XrFixedHeap *xr_isolate_get_fixed_heap(XrVMRuntime *X) {
    return (X && X->core_rt) ? &X->core_rt->fixed_heap : NULL;
}

struct XrSystemHeap *xr_isolate_get_sys_heap(XrVMRuntime *X) {
    return (X && X->core_rt) ? X->core_rt->sys_heap : NULL;
}

/* Live sys.Thread entries still executing against this isolate. The deadlock
 * detector reads it as one of its external-waker sources. */
size_t xr_isolate_sys_thread_count(XrVMRuntime *X) {
    return X ? atomic_load_explicit(&X->sys_thread_count, memory_order_acquire) : 0;
}

struct XrayCoreClasses *xr_isolate_get_core_classes(XrVMRuntime *X) {
    return X ? X->core : NULL;
}

XrTypeRegistry *xr_isolate_get_type_registry(XrVMRuntime *X) {
    return (X && X->core_rt) ? X->core_rt->type_registry : NULL;
}

void xr_isolate_set_type_registry(XrVMRuntime *X, XrTypeRegistry *registry) {
    if (X && X->core_rt)
        X->core_rt->type_registry = registry;
}

void *xr_isolate_get_symbol_table(XrVMRuntime *isolate) {
    return (isolate && isolate->core_rt) ? isolate->core_rt->symbol_table : NULL;
}

struct XrGlobalStringPool *xr_isolate_get_string_pool(XrVMRuntime *X) {
    return (X && X->core_rt) ? X->core_rt->global_string_pool : NULL;
}

XrClass *xr_isolate_get_native_type_class(XrVMRuntime *X, uint8_t type_id) {
    if (!X || !X->core_rt || type_id >= XR_NATIVE_TYPE_MAX)
        return NULL;
    return X->core_rt->native_type_classes[type_id];
}

void xr_isolate_set_native_type_class(XrVMRuntime *X, uint8_t type_id, XrClass *cls) {
    if (X && X->core_rt && type_id < XR_NATIVE_TYPE_MAX)
        X->core_rt->native_type_classes[type_id] = cls;
}

uint64_t xr_isolate_get_ext_finalize_bitmap(XrVMRuntime *isolate) {
    XrRuntimeCore *core = xr_isolate_get_runtime_core(isolate);
    return core ? core->ext_finalize_bitmap : 0;
}

uint64_t xr_isolate_get_ext_has_refs_bitmap(XrVMRuntime *isolate) {
    XrRuntimeCore *core = xr_isolate_get_runtime_core(isolate);
    return core ? core->ext_has_refs_bitmap : 0;
}

XrExtDestroyFn xr_isolate_get_ext_destroy(XrVMRuntime *isolate, uint8_t type_id) {
    XrRuntimeCore *core = xr_isolate_get_runtime_core(isolate);
    if (!core || type_id >= XR_OBJ_TYPE_MAX)
        return NULL;
    return (XrExtDestroyFn) core->ext_destroy_funcs[type_id];
}

XrExtTraverseFn xr_isolate_get_ext_traverse(XrVMRuntime *isolate, uint8_t type_id) {
    XrRuntimeCore *core = xr_isolate_get_runtime_core(isolate);
    if (!core || type_id >= XR_OBJ_TYPE_MAX)
        return NULL;
    return (XrExtTraverseFn) core->ext_traverse_funcs[type_id];
}
