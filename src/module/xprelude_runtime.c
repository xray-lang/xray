/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xprelude_runtime.c - Compiler/runtime prelude registry installation
 *
 * KEY CONCEPT:
 *   The compiler/runtime boundary owns a single static table built from
 *   builtin_symbols.def. The same const table is shared by every isolate
 *   in the process; per-isolate state is only a pointer back to it
 *   (isolate->prelude_symbols). The loader is therefore idempotent and
 *   has no per-isolate teardown work — the pointer field becomes dangling
 *   only after the isolate is gone, by which point nobody can read it.
 *
 *   Prelude is language-core state, not a module. This file installs the
 *   implicit registry required before any source module can be analyzed;
 *   an explicit `import prelude` is rejected like any unknown module.
 */

#include "xprelude_runtime.h"

#include "../base/xchecks.h"
#include "../runtime/xisolate_api.h"

#include <stddef.h>
#include <string.h>

/* ========== Static type registry (process-wide) ========== */

/*
 * Build the type table from the prelude-type rows of builtin_symbols.def. The
 * sentinel entry guarantees the array is non-empty in standard C even
 * before any real entries land in subsequent phases; readers stop at
 * type_count, so the sentinel is never visited.
 */
static const XrPreludeTypeEntry g_prelude_types[] = {
#define XR_BUILTIN_PRELUDE_TYPE(name, arity, native_type, prelude_kind)                            \
    {(name), XR_PRELUDE_KIND_##prelude_kind, (native_type)},
#include "../../stdlib/prelude/builtin_symbols.def"
    /* Sentinel to keep the array non-empty under strict C rules. Not
     * counted in type_count and therefore never visited by lookups. */
    {NULL, 0, 0},
};

#define XR_PRELUDE_TYPE_COUNT                                                                      \
    ((uint16_t) ((sizeof(g_prelude_types) / sizeof(g_prelude_types[0])) - 1u))

static const XrPreludeSymbols g_prelude_symbols = {
    .types = g_prelude_types,
    .type_count = XR_PRELUDE_TYPE_COUNT,
};

/* ========== Native-type registration forwards ==========
 *
 * Every stdlib module that owns a native XrClass exports a small
 * register function. We declare them here so the registry does not need
 * to drag in the full stdlib/{log,datetime,regex,net} headers.
 */
struct XrVMRuntime;
/* Types not registered by xr_core_init — they live in stdlib or
 * depend on runtime infrastructure only available after core init. */
extern void xr_iterator_register_class(XrVMRuntime *isolate);
extern void xr_register_range_class(XrVMRuntime *isolate);
extern void xr_thread_register_native_type(XrVMRuntime *isolate);
#if defined(XR_HAS_NETWORK)
extern void xr_net_conn_storage_register_class(XrVMRuntime *isolate);
extern void xr_net_listener_storage_register_class(XrVMRuntime *isolate);
#endif

#include "../base/xglobal_indices.h"
#include "../base/xmalloc.h"
#include "../runtime/class/xclass.h"
#include "../runtime/class/xclass_system.h"
#include "../runtime/class/xenum.h"
#include "../runtime/core/xr_runtime_core.h"
#include "../runtime/value/xvalue.h"

static void bind_builtin_value(XrVMRuntime *X, int global_index, XrValue value) {
    if (!X || (size_t) global_index >= (size_t) XR_USER_GLOBALS_START)
        return;
    X->vm.builtins[global_index] = value;
    xr_runtime_core_set_builtin(xr_isolate_get_runtime_core(X), global_index, value);
    if (X->vm.builtin_count < XR_USER_GLOBALS_START)
        X->vm.builtin_count = XR_USER_GLOBALS_START;
}

/* Bind a unified-class XrClass into the VM builtins slot keyed by a
 * predefined XR_GLOBAL_VAR_* index. The IR lowerer's builtin_classes
 * table maps user-visible names ("PanicInfo", "Range", ...)
 * onto these indices via XI_GET_BUILTIN, so `new Exception(...)`
 * resolves to the actual class value at run time. */
static void bind_class_global(XrVMRuntime *X, int global_index, void *cls) {
    if (!X || !cls)
        return;
    bind_builtin_value(X, global_index, xr_value_from_class((struct XrClass *) cls));
}

void xr_prelude_register_all_native_types(XrVMRuntime *isolate) {
    if (!isolate)
        return;
    /* Core types (int/float/bool/string/array/map/set/json/bigint/
     * stringbuilder/arrayslice/Exception) are registered by xr_core_init().
     * Prelude registers only the remaining native types. */
    xr_iterator_register_class(isolate);
    xr_register_range_class(isolate);
    xr_thread_register_native_type(isolate);
#if defined(XR_HAS_NETWORK)
    xr_net_conn_storage_register_class(isolate);
    xr_net_listener_storage_register_class(isolate);
#endif

    /* Bind unified-class XrClass values into VM builtins so the
     * IR lowerer's builtin_classes table can resolve them. */
    XrayCoreClasses *core = isolate->core;
    if (core) {
        bind_class_global(isolate, XR_GLOBAL_VAR_PANIC_INFO, core->panicInfoClass);
        bind_class_global(isolate, XR_GLOBAL_VAR_RANGE, core->rangeClass);
    }
    /* Atomic native type class (registered by xr_core_init). */
    XrClass *atomic_cls = xr_isolate_get_native_type_class(isolate, XR_TATOMIC);
    if (atomic_cls)
        bind_class_global(isolate, XR_GLOBAL_VAR_ATOMIC, atomic_cls);
}

/* ========== Isolate installation ========== */

/*
 * Everything the prelude stands for is isolate state, not module state: the
 * symbol table pointer, the runtime-owned native XrClasses and the canonical
 * builtin enums. Installing it is therefore a step of isolate setup rather
 * than something a module load has to trigger. There is no loadable `prelude`
 * module or compatibility import.
 */
bool xr_prelude_install(XrVMRuntime *isolate) {
    if (!isolate)
        return false;

    if (isolate->prelude_symbols == &g_prelude_symbols)
        return true;

    /* Eagerly register every native XrClass that prelude entries refer
     * to. Pure-Xray stdlib modules provide their own exported classes; native
     * classes here are only for remaining runtime-owned prelude types. */
    xr_prelude_register_all_native_types(isolate);

    /* Publish one enum identity per isolate before loading any module. */
    if (!xr_isolate_register_runtime_prelude_enums(isolate))
        return false;
    isolate->prelude_symbols = (void *) &g_prelude_symbols;
    return true;
}

/* ========== Public accessors (consumed by frontend / tests) ========== */

const XrPreludeSymbols *xr_prelude_get_symbols(XrVMRuntime *isolate) {
    if (!isolate)
        return NULL;
    return (const XrPreludeSymbols *) isolate->prelude_symbols;
}

const XrPreludeTypeEntry *xr_prelude_lookup_type(const XrPreludeSymbols *symbols, const char *name,
                                                 size_t len) {
    if (!symbols || !name || symbols->type_count == 0)
        return NULL;
    for (uint16_t i = 0; i < symbols->type_count; i++) {
        const XrPreludeTypeEntry *entry = &symbols->types[i];
        if (!entry->name)
            continue;
        size_t entry_len = strlen(entry->name);
        if (entry_len == len && memcmp(entry->name, name, len) == 0)
            return entry;
    }
    return NULL;
}
