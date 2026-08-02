/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * prelude.c - Prelude module loader and process-wide type registry.
 *
 * KEY CONCEPT:
 *   The prelude module owns a single static table built from
 *   builtin_symbols.def. The same const table is shared by every isolate
 *   in the process; per-isolate state is only a pointer back to it
 *   (isolate->prelude_symbols). The loader is therefore idempotent and
 *   has no per-isolate teardown work — the pointer field becomes dangling
 *   only after the isolate is gone, by which point nobody can read it.
 *
 *   The registered XrModule that the loader returns is currently empty
 *   (no exports). Subsequent phases populate it with type markers and
 *   common builtin functions; lexer/parser changes for the unified
 *   IDENT-based type-name path live in those phases too.
 */

#include "prelude.h"

#include "../../src/base/xchecks.h"
#include "../../src/module/xmodule.h"
#include "../../src/runtime/xisolate_api.h"

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
#include "builtin_symbols.def"
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
 * register function. We declare them here so prelude.c does not need
 * to drag in the full stdlib/{log,datetime,regex,net} headers.
 */
struct XrVMRuntime;
/* Types not registered by xr_core_init — they live in stdlib or
 * depend on runtime infrastructure only available after core init. */
extern void xr_iterator_register_class(XrVMRuntime *isolate);
extern void xr_register_range_class(XrVMRuntime *isolate);
extern void xr_regex_register_class(XrVMRuntime *isolate);
extern void xr_sys_mutex_register_class(XrVMRuntime *isolate);
extern void xr_sys_rwlock_register_class(XrVMRuntime *isolate);
extern void xr_sys_condvar_register_class(XrVMRuntime *isolate);
extern void xr_sys_barrier_register_class(XrVMRuntime *isolate);
extern void xr_sys_once_register_class(XrVMRuntime *isolate);
extern void xr_thread_register_native_type(XrVMRuntime *isolate);
extern void xr_netconn_register_class(XrVMRuntime *isolate);
extern void xr_netlistener_register_class(XrVMRuntime *isolate);

#include "../../src/base/xglobal_indices.h"
#include "../../src/runtime/class/xclass_system.h"
#include "../../src/runtime/class/xclass.h"
#include "../../src/runtime/class/xenum.h"
#include "../../src/runtime/core/xr_runtime_core.h"
#include "../../src/runtime/value/xvalue.h"
#include "../../src/base/xmalloc.h"

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

/* Build one canonical prelude enum and bind it into a VM builtin slot so
 * every compilation unit (entry file and imported modules) resolves the
 * same XrEnumType — giving cross-module `Result` / `Ordering` values a
 * single type identity.  Replaces the former per-module AST injection,
 * which created a distinct enum type per module and broke cross-module
 * pattern matching. Members are copied into the symbol table by
 * xr_enum_type_new, so the input array is freed here. */
static XrEnumType *make_prelude_enum(XrVMRuntime *X, const char *name, const char **member_names,
                                     int count, const int *payload_counts, bool is_adt) {
    char **names = (char **) xr_malloc(sizeof(char *) * (size_t) count);
    if (!names) {
        xr_free(names);
        return NULL;
    }
    for (int i = 0; i < count; i++) {
        size_t len = strlen(member_names[i]) + 1;
        names[i] = (char *) xr_malloc(len);
        if (names[i])
            memcpy(names[i], member_names[i], len);
    }

    XrEnumType *et = xr_enum_type_new(X, "prelude", name, names, count);

    if (et && is_adt && payload_counts)
        (void) xr_enum_type_set_adt_payloads(et, payload_counts, count);

    for (int i = 0; i < count; i++)
        xr_free(names[i]);
    xr_free(names);
    return et;
}

/* Canonical prelude enums, built from builtin_symbols.def so the runtime's
 * XrEnumType and the analyzer's XrType describe the same variants. `Ordering`
 * ordinals must match XrAtomicOrdering — the def declares them in that order. */
#define XR_PRELUDE_ENUM_MAX_VARIANTS 8
/* -1 marks an enum the runtime does not bind into a VM builtin slot. */
#define XR_GLOBAL_VAR_NONE (-1)

typedef struct {
    const char *name;
    bool has_payload;
} XrPreludeEnumVariantRow;

typedef struct {
    const char *name;
    int slot;
    int variant_count;
} XrPreludeEnumRow;

/* All variants of all prelude enums, flattened in declaration order; each
 * enum's slice starts where the previous one ended. */
static const XrPreludeEnumVariantRow g_prelude_enum_variants[] = {
#define XR_BUILTIN_ENUM(ename, earity, evm_slot, evariants) evariants
#define XR_BUILTIN_ENUM_VARIANT(vname, payload) {(vname), XR_PRELUDE_PAYLOAD_IS_SET_##payload},
#define XR_PRELUDE_PAYLOAD_IS_SET_NONE false
#define XR_PRELUDE_PAYLOAD_IS_SET_TYPE_PARAM_0 true
#define XR_PRELUDE_PAYLOAD_IS_SET_UNKNOWN true
#include "builtin_symbols.def"
};

/* Registration exists only to bind canonical enum types into VM builtin slots,
 * so slotless enums (the stdlib error enums, whose values never cross a module
 * boundary) are skipped rather than built and dropped. */

static const XrPreludeEnumRow g_prelude_enum_rows[] = {
#define XR_BUILTIN_ENUM(ename, earity, evm_slot, evariants)                                        \
    {(ename), XR_GLOBAL_VAR_##evm_slot,                                                            \
     (int) (sizeof((const XrPreludeEnumVariantRow[]) {evariants}) /                                \
            sizeof(XrPreludeEnumVariantRow))},
#define XR_BUILTIN_ENUM_VARIANT(vname, payload) {(vname), XR_PRELUDE_PAYLOAD_IS_SET_##payload},
#include "builtin_symbols.def"
};

#undef XR_PRELUDE_PAYLOAD_IS_SET_NONE
#undef XR_PRELUDE_PAYLOAD_IS_SET_TYPE_PARAM_0
#undef XR_PRELUDE_PAYLOAD_IS_SET_UNKNOWN

static void xr_prelude_register_builtin_enums(XrVMRuntime *X) {
    if (!X)
        return;

    int variant_base = 0;
    for (size_t e = 0; e < sizeof(g_prelude_enum_rows) / sizeof(g_prelude_enum_rows[0]); e++) {
        const XrPreludeEnumRow *row = &g_prelude_enum_rows[e];
        const XrPreludeEnumVariantRow *variants = &g_prelude_enum_variants[variant_base];
        variant_base += row->variant_count;

        if (row->slot < 0)
            continue;

        XR_DCHECK(row->variant_count <= XR_PRELUDE_ENUM_MAX_VARIANTS,
                  "prelude enum exceeds XR_PRELUDE_ENUM_MAX_VARIANTS");
        if (row->variant_count > XR_PRELUDE_ENUM_MAX_VARIANTS)
            continue;

        const char *members[XR_PRELUDE_ENUM_MAX_VARIANTS];
        int payload_counts[XR_PRELUDE_ENUM_MAX_VARIANTS];
        bool is_adt = false;
        for (int v = 0; v < row->variant_count; v++) {
            members[v] = variants[v].name;
            payload_counts[v] = variants[v].has_payload ? 1 : 0;
            is_adt = is_adt || variants[v].has_payload;
        }

        XrEnumType *et = make_prelude_enum(X, row->name, members, row->variant_count,
                                           is_adt ? payload_counts : NULL, is_adt);
        if (et)
            bind_builtin_value(X, row->slot, XR_FROM_PTR(et));
    }

    if (X->vm.builtin_count < XR_USER_GLOBALS_START)
        X->vm.builtin_count = XR_USER_GLOBALS_START;
}

void xr_prelude_register_all_native_types(XrVMRuntime *isolate) {
    if (!isolate)
        return;
    /* Core types (int/float/bool/string/array/map/set/json/bigint/
     * stringbuilder/arrayslice/Exception) are registered by xr_core_init().
     * Prelude registers only the remaining native types. */
    xr_iterator_register_class(isolate);
    xr_register_range_class(isolate);
    xr_regex_register_class(isolate);
    xr_sys_mutex_register_class(isolate);
    xr_sys_rwlock_register_class(isolate);
    xr_sys_condvar_register_class(isolate);
    xr_sys_barrier_register_class(isolate);
    xr_sys_once_register_class(isolate);
    xr_thread_register_native_type(isolate);
    xr_netconn_register_class(isolate);
    xr_netlistener_register_class(isolate);

    /* Bind unified-class XrClass values into VM builtins so the
     * IR lowerer's builtin_classes table can resolve them. */
    XrayCoreClasses *core = isolate->core;
    if (core) {
        bind_class_global(isolate, XR_GLOBAL_VAR_PANIC_INFO, core->panicInfoClass);
        bind_class_global(isolate, XR_GLOBAL_VAR_RANGE, core->rangeClass);
    }
    /* Atomic native type class (registered by xr_core_init). */
    XrClass *atomic_cls = xr_isolate_get_native_type_class(isolate, XR_TATOMIC);
    XrClass *work_queue_cls = xr_isolate_get_native_type_class(isolate, XR_TWORKQUEUE);
    XrClass *result_group_cls = xr_isolate_get_native_type_class(isolate, XR_TRESULTGROUP);
    XrClass *countdown_latch_cls = xr_isolate_get_native_type_class(isolate, XR_TCOUNTDOWNLATCH);
    XrClass *semaphore_cls = xr_isolate_get_native_type_class(isolate, XR_TSEMAPHORE);
    XrClass *event_count_cls = xr_isolate_get_native_type_class(isolate, XR_TEVENTCOUNT);
    if (atomic_cls)
        bind_class_global(isolate, XR_GLOBAL_VAR_ATOMIC, atomic_cls);
    if (work_queue_cls) {
        bind_class_global(isolate, XR_GLOBAL_VAR_WORKQUEUE, work_queue_cls);
    }
    if (result_group_cls) {
        bind_class_global(isolate, XR_GLOBAL_VAR_RESULTGROUP, result_group_cls);
    }
    if (countdown_latch_cls) {
        bind_class_global(isolate, XR_GLOBAL_VAR_COUNTDOWNLATCH, countdown_latch_cls);
    }
    if (semaphore_cls) {
        bind_class_global(isolate, XR_GLOBAL_VAR_SEMAPHORE, semaphore_cls);
    }
    if (event_count_cls) {
        bind_class_global(isolate, XR_GLOBAL_VAR_EVENTCOUNT, event_count_cls);
    }
}

/* ========== Module loader ========== */

XrModule *xr_load_module_prelude(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_prelude: NULL isolate");

    /* Wire isolate to the (process-wide const) symbol table. Idempotent
     * because the right-hand side is constant and the field is just a
     * pointer cache for downstream consumers. */
    isolate->prelude_symbols = (void *) &g_prelude_symbols;

    /* Eagerly register every native XrClass that prelude entries refer
     * to. Pure-Xray stdlib modules provide their own exported classes; native
     * classes here are only for remaining runtime-owned prelude types. */
    xr_prelude_register_all_native_types(isolate);

    /* Bind canonical Ordering enum type into VM builtin slot so
     * every module shares one identity (replaces per-module AST injection). */
    xr_prelude_register_builtin_enums(isolate);

    XrModule *module = xr_module_create_native(isolate, "prelude");
    if (!module)
        return NULL;

    /* No exports yet. Marking loaded prevents the module subsystem from
     * re-entering the loader if user code does an explicit
     * `import prelude`. */
    return module;
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
