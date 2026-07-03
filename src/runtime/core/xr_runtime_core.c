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
#include "../mem/xsystem_heap.h"
#include "../mem/xweak_registry.h"
#include "../object/xstring.h"
#include "../xstrbuf.h"
#include <string.h>

XrRuntimeCore *xr_runtime_core_new(const XrRuntimeCoreConfig *cfg) {
    XrRuntimeCore *core = (XrRuntimeCore *) xr_calloc(1, sizeof(XrRuntimeCore));
    if (!core)
        return NULL;

    core->vm_owner = cfg ? cfg->owner_isolate : NULL;
    core->userdata = cfg ? cfg->userdata : NULL;
    core->ext_type_next = XR_TTHREAD + 1;
    for (int32_t i = 0; i < XR_USER_GLOBALS_START; i++)
        core->builtins[i] = XR_NULL_VAL;
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

    xr_fixed_heap_init(&core->fixed_heap, cfg ? cfg->owner_isolate : NULL);

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

void xr_runtime_core_cleanup_fixed_heap(XrRuntimeCore *core) {
    if (!core)
        return;
    xr_fixed_heap_cleanup(&core->fixed_heap);
    if (core->fixed_heap.isolate)
        xr_weak_registry_destroy(core->fixed_heap.isolate);
}

struct XrVMRuntime *xr_runtime_core_vm_owner(const XrRuntimeCore *core) {
    return core ? core->vm_owner : NULL;
}

XrValue xr_runtime_core_builtin(const XrRuntimeCore *core, int32_t index) {
    if (!core || index < 0 || index >= XR_USER_GLOBALS_START)
        return XR_NULL_VAL;
    return core->builtins[index];
}

void xr_runtime_core_set_builtin(XrRuntimeCore *core, int32_t index, XrValue value) {
    if (!core || index < 0 || index >= XR_USER_GLOBALS_START)
        return;
    core->builtins[index] = value;
}

void xr_runtime_core_set_destroy_op(XrRuntimeCore *core, uint8_t type, XrObjDestroyFn destroy) {
    if (!core || type >= XR_OBJ_TYPE_MAX)
        return;
    core->destroy_ops[type] = destroy;
    if (destroy)
        core->destroy_bitmap |= (1ULL << type);
    else
        core->destroy_bitmap &= ~(1ULL << type);
}

XrObjDestroyFn xr_runtime_core_destroy_op(const XrRuntimeCore *core, uint8_t type) {
    if (!core || type >= XR_OBJ_TYPE_MAX)
        return NULL;
    return core->destroy_ops[type];
}

bool xr_runtime_core_type_needs_destroy(const XrRuntimeCore *core, uint8_t type) {
    return core && type < XR_OBJ_TYPE_MAX && (core->destroy_bitmap & (1ULL << type)) != 0;
}

void xr_runtime_core_set_scope_transfer_ops(XrRuntimeCore *core, const XrScopeTransferOps *ops) {
    if (!core)
        return;
    core->scope_transfer_ops = ops;
}

const XrScopeTransferOps *xr_runtime_core_scope_transfer_ops(const XrRuntimeCore *core) {
    return core ? core->scope_transfer_ops : NULL;
}

void xr_runtime_core_delete(XrRuntimeCore *core) {
    if (!core)
        return;

    xr_runtime_core_free_tmp_strbuf(core);

    xr_runtime_core_destroy_coro_storage(core);
    xr_runtime_core_cleanup_fixed_heap(core);

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
