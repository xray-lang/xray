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
#include "../../base/xglobal_indices.h"
#include "../../base/xmutex.h"
#include "../mem/xcoro_heap.h"
#include "../mem/xheap.h"
#include "../object/xnative_type.h"
#include "../value/xvalue.h"
#include "xr_script_info.h"
#include "xr_exec_context.h"
#include <stdbool.h>
#include <stdint.h>

struct XrCoroutine;
struct XrVMRuntime;
struct XrGlobalStringPool;
struct XrStrBuf;
struct XrSystemHeap;
struct XrScopeContext;

typedef struct XrScopeTransferOps {
    bool (*record_child_completion_locked)(struct XrCoroutine *coro, struct XrScopeContext *scope);
} XrScopeTransferOps;

typedef void (*XrAotNativeValueReleaseFn)(XrValue value);

typedef struct XrRuntimeCoreConfig {
    struct XrVMRuntime *owner_isolate;
    void *userdata;
} XrRuntimeCoreConfig;

typedef struct XrRuntimeCore {
    XrFixedHeap fixed_heap;
    struct XrSystemHeap *sys_heap;
    struct XrGlobalStringPool *global_string_pool;
    struct XrStrBuf *tmp_strbuf;
    struct XrVMRuntime *vm_owner;

    /* The root execution's EXEC_LOCAL heap.
     *
     * Embedded rather than pooled: it lives exactly as long as the core, and it
     * must never be recycled into the coroutine-heap struct pool. Its presence
     * is what makes XR_STORAGE_EXEC_LOCAL mean the same thing at top level as
     * it does inside a coroutine — before this, root allocations fell through
     * to the fixed heap and were pinned immortal. */
    XrCoroHeap root_heap;
    /* Set once xr_runtime_core_teardown_root_heap has run, so the staged
     * isolate teardown and xr_runtime_core_delete can both call it without
     * tearing the heap down twice. */
    bool root_heap_torn_down;

    /* Storage lifetime and task identity are deliberately orthogonal.  These
     * contexts exist even when no scheduler or coroutine object is present. */
    XrAllocationContext root_alloc;
    XrAllocationContext module_alloc;
    XrAllocationContext shared_alloc;
    XrExecutionContext root_exec;
    XrExecutionContext module_exec;

    XrTypeRegistry *type_registry;
    XrSymbolTable *symbol_table;
    XrClass *native_type_classes[XR_NATIVE_TYPE_MAX];
    XrValue builtins[XR_USER_GLOBALS_START];

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
    /* Standalone AOT containers share the canonical object header but keep
     * backend-native bodies.  The core owns the one release capability that
     * lets cross-execution holders destroy an abandoned native value without
     * re-shelling or copying its graph. */
    XrAotNativeValueReleaseFn aot_native_value_release;

    /* Guards isolate-level shared metadata writes that happen off the object
     * fast path: the hidden-class transition chains (xinstance.c). These are
     * cold paths (first time a given field is added to a shape), so a single
     * lock adds no measurable cost to steady-state execution while making
     * concurrent shape evolution across worker threads race-free (P1-3). */
    XrAdaptiveMutex metadata_lock;
} XrRuntimeCore;

XR_FUNC XrRuntimeCore *xr_runtime_core_new(const XrRuntimeCoreConfig *cfg);
XR_FUNC void xr_runtime_core_delete(XrRuntimeCore *core);
XR_FUNC void xr_runtime_core_free_tmp_strbuf(XrRuntimeCore *core);
XR_FUNC void xr_runtime_core_destroy_coro_storage(XrRuntimeCore *core);
XR_FUNC void xr_runtime_core_teardown_root_heap(XrRuntimeCore *core);
XR_FUNC void xr_runtime_core_cleanup_fixed_heap(XrRuntimeCore *core);
XR_FUNC struct XrVMRuntime *xr_runtime_core_vm_owner(const XrRuntimeCore *core);
XR_FUNC XrValue xr_runtime_core_builtin(const XrRuntimeCore *core, int32_t index);
XR_FUNC void xr_runtime_core_set_builtin(XrRuntimeCore *core, int32_t index, XrValue value);
XR_FUNC void xr_runtime_core_set_destroy_op(XrRuntimeCore *core, uint8_t type,
                                            XrObjDestroyFn destroy);
XR_FUNC XrObjDestroyFn xr_runtime_core_destroy_op(const XrRuntimeCore *core, uint8_t type);
XR_FUNC bool xr_runtime_core_type_needs_destroy(const XrRuntimeCore *core, uint8_t type);
XR_FUNC void xr_runtime_core_set_scope_transfer_ops(XrRuntimeCore *core,
                                                    const XrScopeTransferOps *ops);
XR_FUNC const XrScopeTransferOps *xr_runtime_core_scope_transfer_ops(const XrRuntimeCore *core);
XR_FUNC void xr_runtime_core_set_aot_native_value_release(XrRuntimeCore *core,
                                                          XrAotNativeValueReleaseFn release);
XR_FUNC bool xr_runtime_core_release_aot_native_value(XrRuntimeCore *core, XrObjHeader *obj);
XR_FUNC XrExecutionContext *xr_runtime_core_root_exec(XrRuntimeCore *core);
XR_FUNC XrExecutionContext *xr_runtime_core_module_exec(XrRuntimeCore *core);

#endif  // XR_RUNTIME_CORE_H
