/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_runtime_core.h - VM-neutral runtime core state.
 */

#ifndef XR_RUNTIME_CORE_H
#define XR_RUNTIME_CORE_H

#include "../../base/xconfig.h"
#include "../gc/xgc.h"
#include "xr_script_info.h"

struct XrayIsolate;
struct XrGlobalStringPool;
struct XrStrBuf;
struct XrSystemHeap;

typedef struct XrRuntimeCoreConfig {
    struct XrayIsolate *owner_isolate;
    void *userdata;
} XrRuntimeCoreConfig;

typedef struct XrRuntimeCore {
    XrGC gc;
    struct XrSystemHeap *sys_heap;
    struct XrGlobalStringPool *global_string_pool;
    struct XrStrBuf *tmp_strbuf;
    void *weak_registry;

    void *userdata;
    XrayConfig *config;
    XrScriptInfo script_info;

    uint8_t ext_type_next;
    const char *ext_type_names[XGC_MAX_TYPES];
    uint64_t ext_finalize_bitmap;
    uint64_t ext_has_refs_bitmap;
    XrGCDestroyFn ext_destroy_funcs[XGC_MAX_TYPES];
    void *ext_traverse_funcs[XGC_MAX_TYPES];
} XrRuntimeCore;

XR_FUNC XrRuntimeCore *xr_runtime_core_new(const XrRuntimeCoreConfig *cfg);
XR_FUNC void xr_runtime_core_delete(XrRuntimeCore *core);
XR_FUNC void xr_runtime_core_free_tmp_strbuf(XrRuntimeCore *core);
XR_FUNC void xr_runtime_core_destroy_coro_storage(XrRuntimeCore *core);
XR_FUNC void xr_runtime_core_cleanup_gc(XrRuntimeCore *core);

#endif  // XR_RUNTIME_CORE_H
