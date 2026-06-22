/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xisolate_api.c - Lightweight Isolate access interface implementation
 */

#include "xisolate_api.h"
#include "xisolate_internal.h"
#include "core/xr_runtime_core.h"
#include "../coro/xcoroutine.h"
#include "../base/xlog.h"
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

/* Monotonic clock helper (returns ns). Returns 0 on failure so
 * deadline checks fail open rather than aborting the embedder. */
static int64_t xr_now_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER freq, counter;
    if (!QueryPerformanceFrequency(&freq) || !QueryPerformanceCounter(&counter))
        return 0;
    return (int64_t) ((double) counter.QuadPart / (double) freq.QuadPart * 1e9);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (int64_t) ts.tv_sec * 1000000000LL + (int64_t) ts.tv_nsec;
#endif
}

struct XrCoroHeap *xr_isolate_get_heap(XrVMRuntime *X) {
    if (!X || !X->main_coro)
        return NULL;
    return ((XrCoroutine *) X->main_coro)->heap;
}

/* ========== Module Subsystem ========== */

XrModuleRegistry *xr_isolate_get_module_registry(XrVMRuntime *X) {
    return X ? X->module_registry : NULL;
}

void xr_isolate_set_module_registry(XrVMRuntime *X, XrModuleRegistry *registry) {
    if (X)
        X->module_registry = registry;
}

XrModule *xr_isolate_get_current_module(XrVMRuntime *X) {
    return X ? X->current_module : NULL;
}

void xr_isolate_set_current_module(XrVMRuntime *X, XrModule *mod) {
    if (X) {
        X->current_module = mod;
    }
}

/* ========== Globals ========== */

XrGlobalsTable *xr_isolate_get_globals(XrVMRuntime *X) {
    return X ? X->globals : NULL;
}

XrGlobalObject *xr_isolate_get_global_object(XrVMRuntime *X) {
    return X ? X->global_object : NULL;
}

struct XrGlobalStringPool *xr_isolate_get_string_pool(XrVMRuntime *X) {
    return (X && X->core_rt) ? X->core_rt->global_string_pool : NULL;
}

struct XrStrBuf **xr_isolate_tmp_strbuf_slot(XrVMRuntime *X) {
    return (X && X->core_rt) ? &X->core_rt->tmp_strbuf : NULL;
}

/* ========== Coroutine ========== */

XrCoroutine *xr_isolate_get_main_coro(XrVMRuntime *X) {
    return X ? X->main_coro : NULL;
}

void xr_isolate_set_main_coro(XrVMRuntime *X, XrCoroutine *coro) {
    if (X)
        X->main_coro = coro;
}

/* ========== VM State ========== */

XrVMState *xr_isolate_get_vm_state(XrVMRuntime *X) {
    return X ? &X->vm : NULL;
}

XrVMContext *xr_isolate_get_vm_ctx(XrVMRuntime *X) {
    return X ? &X->vm_ctx : NULL;
}

/* ========== Storage Mode ========== */

uint8_t xr_isolate_get_storage_mode(XrVMRuntime *X) {
    return X ? atomic_load_explicit(&X->current_storage_mode, memory_order_relaxed) : 0;
}

void xr_isolate_set_storage_mode(XrVMRuntime *X, uint8_t mode) {
    if (X) {
        atomic_store_explicit(&X->current_storage_mode, mode, memory_order_relaxed);
    }
}

/* ========== Config ========== */

void *xr_isolate_get_userdata(XrVMRuntime *X) {
    return (X && X->core_rt) ? X->core_rt->userdata : NULL;
}

struct XrayConfig *xr_isolate_get_config(XrVMRuntime *X) {
    return (X && X->core_rt) ? X->core_rt->config : NULL;
}

const char *xr_isolate_get_script_file(XrVMRuntime *X) {
    return (X && X->core_rt) ? X->core_rt->script_info.file : NULL;
}

int xr_isolate_get_script_argc(XrVMRuntime *X) {
    return (X && X->core_rt) ? X->core_rt->script_info.argc : 0;
}

char **xr_isolate_get_script_argv(XrVMRuntime *X) {
    return (X && X->core_rt) ? X->core_rt->script_info.argv : NULL;
}

/* ========== Debug ========== */

void *xr_isolate_get_debug_state(XrVMRuntime *X) {
    return X ? X->debug_state : NULL;
}

void xr_isolate_set_debug_state(XrVMRuntime *X, void *state) {
    if (X) {
        X->debug_state = state;
    }
}

void *xr_isolate_get_debug_hooks(XrVMRuntime *X) {
    return X ? X->debug_hooks : NULL;
}

void xr_isolate_set_debug_hooks(XrVMRuntime *X, void *hooks) {
    if (X) {
        X->debug_hooks = hooks;
    }
}

/* ========== Exception Print Suppression ========== */

bool xr_isolate_get_suppress_exception_print(XrVMRuntime *X) {
    return X ? X->suppress_exception_print : false;
}

void xr_isolate_set_suppress_exception_print(XrVMRuntime *X, bool suppress) {
    if (X) {
        X->suppress_exception_print = suppress;
    }
}

/* ========== Embedded Execution Policy ========== */

FILE *xr_isolate_stdout(XrVMRuntime *X) {
    if (X && X->user_stdout)
        return (FILE *) X->user_stdout;
    return stdout;
}

void xray_vm_set_stdout(XrVMRuntime *X, FILE *stream) {
    if (X)
        X->user_stdout = stream;
}

void xray_vm_set_deadline_ms(XrVMRuntime *X, int64_t timeout_ms) {
    if (!X)
        return;
    X->deadline_exceeded = false;
    if (timeout_ms <= 0) {
        X->deadline_ns = 0;
        return;
    }
    int64_t now = xr_now_ns();
    /* now == 0 means clock_gettime failed; arm the deadline relative to
     * the requested timeout anyway so the VM still aborts eventually. */
    X->deadline_ns = now + timeout_ms * 1000000LL;
}

bool xr_isolate_check_deadline(XrVMRuntime *X) {
    if (!X || X->deadline_ns == 0)
        return false;
    if (X->deadline_exceeded)
        return true;
    int64_t now = xr_now_ns();
    if (now != 0 && now >= X->deadline_ns) {
        X->deadline_exceeded = true;
        return true;
    }
    return false;
}

bool xr_isolate_timed_out(XrVMRuntime *X) {
    return X ? X->deadline_exceeded : false;
}

bool xray_vm_timed_out(XrVMRuntime *X) {
    return xr_isolate_timed_out(X);
}

void xray_vm_set_module_allowlist(XrVMRuntime *X, const char *const *allowed, size_t count) {
    if (!X)
        return;
    if (!allowed || count == 0) {
        X->module_allowlist = NULL;
        X->module_allowlist_count = 0;
        return;
    }
    X->module_allowlist = allowed;
    X->module_allowlist_count = count;
}

bool xr_isolate_module_allowed(XrVMRuntime *X, const char *module_name) {
    if (!X || !module_name)
        return false;
    if (X->module_allowlist_count == 0)
        return true; /* no allowlist configured — permit everything */
    for (size_t i = 0; i < X->module_allowlist_count; i++) {
        const char *allowed = X->module_allowlist[i];
        if (allowed && strcmp(allowed, module_name) == 0)
            return true;
    }
    return false;
}

/* ========== Extension Type System ========== */

uint8_t xr_alloc_extension_type(XrVMRuntime *isolate, const char *name) {
    XrRuntimeCore *core = xr_isolate_get_runtime_core(isolate);
    if (!core)
        return 0;
    uint8_t id = core->ext_type_next;
    if (id >= XR_OBJ_TYPE_MAX) {
        xr_log_warning("ext_type", "extension type slots exhausted (max %d)", XR_OBJ_TYPE_MAX);
        return 0;
    }
    core->ext_type_next = id + 1;
    core->ext_type_names[id] = name;
    return id;
}

void xr_register_extension_destroy(XrVMRuntime *isolate, uint8_t type_id,
                                   XrExtDestroyFn destroy_fn) {
    XrRuntimeCore *core = xr_isolate_get_runtime_core(isolate);
    if (!core || type_id >= XR_OBJ_TYPE_MAX)
        return;
    core->ext_destroy_funcs[type_id] = (XrObjDestroyFn) destroy_fn;
    core->ext_finalize_bitmap |= (1ULL << type_id);
    xr_runtime_core_set_destroy_op(core, type_id, (XrObjDestroyFn) destroy_fn);
}

void xr_register_extension_traverse(XrVMRuntime *isolate, uint8_t type_id,
                                    XrExtTraverseFn traverse_fn) {
    XrRuntimeCore *core = xr_isolate_get_runtime_core(isolate);
    if (!core || type_id >= XR_OBJ_TYPE_MAX)
        return;
    core->ext_traverse_funcs[type_id] = (void *) traverse_fn;
    core->ext_has_refs_bitmap |= (1ULL << type_id);
}
