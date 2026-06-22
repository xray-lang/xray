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
#include "../../base/xforward_decl.h"
#include "../mem/xheap.h"
#include "../object/xnative_type.h"
#include "xr_script_info.h"
#include <stdbool.h>

struct XrCoroutine;
struct XrVMRuntime;
struct XrGlobalStringPool;
struct XrStrBuf;
struct XrSystemHeap;
struct XrScopeContext;

typedef struct XrScopeTransferOps {
    bool (*record_child_error_locked)(struct XrCoroutine *coro, struct XrScopeContext *scope);
} XrScopeTransferOps;

typedef struct XrRuntimeCoreConfig {
    struct XrVMRuntime *owner_isolate;
    void *userdata;
} XrRuntimeCoreConfig;

typedef struct XrRuntimeCore {
    XrFixedHeap fixed_heap;
    struct XrSystemHeap *sys_heap;
    struct XrGlobalStringPool *global_string_pool;
    struct XrStrBuf *tmp_strbuf;
    void *weak_registry;
    struct XrVMRuntime *vm_owner;

    XrTypeRegistry *type_registry;
    XrSymbolTable *symbol_table;
    XrClass *native_type_classes[XR_NATIVE_TYPE_MAX];

    void *userdata;
    XrayConfig *config;
    XrScriptInfo script_info;

    uint8_t ext_type_next;
    const char *ext_type_names[XR_OBJ_TYPE_MAX];
    uint64_t destroy_bitmap;
    XrObjDestroyFn destroy_ops[XR_OBJ_TYPE_MAX];
    uint64_t ext_finalize_bitmap;
    uint64_t ext_has_refs_bitmap;
    XrObjDestroyFn ext_destroy_funcs[XR_OBJ_TYPE_MAX];
    void *ext_traverse_funcs[XR_OBJ_TYPE_MAX];
    const XrScopeTransferOps *scope_transfer_ops;
} XrRuntimeCore;

XR_FUNC XrRuntimeCore *xr_runtime_core_new(const XrRuntimeCoreConfig *cfg);
XR_FUNC void xr_runtime_core_delete(XrRuntimeCore *core);
XR_FUNC void xr_runtime_core_free_tmp_strbuf(XrRuntimeCore *core);
XR_FUNC void xr_runtime_core_destroy_coro_storage(XrRuntimeCore *core);
XR_FUNC void xr_runtime_core_cleanup_fixed_heap(XrRuntimeCore *core);
XR_FUNC struct XrVMRuntime *xr_runtime_core_vm_owner(const XrRuntimeCore *core);
XR_FUNC void xr_runtime_core_set_destroy_op(XrRuntimeCore *core, uint8_t type,
                                            XrObjDestroyFn destroy);
XR_FUNC XrObjDestroyFn xr_runtime_core_destroy_op(const XrRuntimeCore *core, uint8_t type);
XR_FUNC bool xr_runtime_core_type_needs_destroy(const XrRuntimeCore *core, uint8_t type);
XR_FUNC void xr_runtime_core_set_scope_transfer_ops(XrRuntimeCore *core,
                                                    const XrScopeTransferOps *ops);
XR_FUNC const XrScopeTransferOps *xr_runtime_core_scope_transfer_ops(const XrRuntimeCore *core);

#endif  // XR_RUNTIME_CORE_H
