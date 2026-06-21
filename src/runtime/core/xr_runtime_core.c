/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_runtime_core.c - VM-neutral runtime core state.
 */

#include "xr_runtime_core.h"
#include "../../base/xmalloc.h"
#include "../gc/xsystem_heap.h"
#include "../gc/xweak_registry.h"
#include "../object/xstring.h"
#include "../xstrbuf.h"
#include <string.h>

XrRuntimeCore *xr_runtime_core_new(const XrRuntimeCoreConfig *cfg) {
    XrRuntimeCore *core = (XrRuntimeCore *) xr_calloc(1, sizeof(XrRuntimeCore));
    if (!core)
        return NULL;

    core->userdata = cfg ? cfg->userdata : NULL;
    core->ext_type_next = XR_TTASK + 1;
    xr_script_info_init(&core->script_info);

    core->config = (XrayConfig *) xr_malloc(sizeof(XrayConfig));
    if (!core->config)
        goto fail;
    xr_config_init(core->config);

    core->global_string_pool = xr_malloc(sizeof(struct XrGlobalStringPool));
    if (!core->global_string_pool)
        goto fail;
    memset(core->global_string_pool, 0, sizeof(struct XrGlobalStringPool));
    xr_global_pool_init(core->global_string_pool);

    xr_gc_init(&core->gc, cfg ? cfg->owner_isolate : NULL);

    core->sys_heap = xr_malloc(sizeof(struct XrSystemHeap));
    if (!core->sys_heap)
        goto fail;
    if (!xr_sysheap_init(core->sys_heap, NULL))
        goto fail;

    return core;

fail:
    xr_runtime_core_delete(core);
    return NULL;
}

void xr_runtime_core_free_tmp_strbuf(XrRuntimeCore *core) {
    if (!core || !core->tmp_strbuf)
        return;
    xr_strbuf_free(core->tmp_strbuf);
    core->tmp_strbuf = NULL;
}

void xr_runtime_core_destroy_coro_storage(XrRuntimeCore *core) {
    if (!core || !core->sys_heap)
        return;
    xr_sysheap_destroy_coro_storage(core->sys_heap);
}

void xr_runtime_core_cleanup_gc(XrRuntimeCore *core) {
    if (!core)
        return;
    xr_gc_cleanup(&core->gc);
    if (core->gc.isolate)
        xr_weak_registry_destroy(core->gc.isolate);
}

void xr_runtime_core_delete(XrRuntimeCore *core) {
    if (!core)
        return;

    xr_runtime_core_free_tmp_strbuf(core);

    xr_runtime_core_destroy_coro_storage(core);
    xr_runtime_core_cleanup_gc(core);

    if (core->global_string_pool) {
        xr_global_pool_free(core->global_string_pool);
        xr_free(core->global_string_pool);
        core->global_string_pool = NULL;
    }

    if (core->sys_heap) {
        xr_sysheap_destroy(core->sys_heap);
        xr_free(core->sys_heap);
        core->sys_heap = NULL;
    }

    if (core->config) {
        xr_free(core->config);
        core->config = NULL;
    }

    xr_free(core);
}
