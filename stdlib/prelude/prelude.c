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
 *   prelude_types.def. The same const table is shared by every isolate
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
 * Build the type table from the X-macro list in prelude_types.def. The
 * sentinel entry guarantees the array is non-empty in standard C even
 * before any real entries land in subsequent phases; readers stop at
 * type_count, so the sentinel is never visited.
 */
static const XrPreludeTypeEntry g_prelude_types[] = {
#define XR_PRELUDE_TYPE(name, native_type, kind) {(name), XR_PRELUDE_KIND_##kind, (native_type)},
#include "prelude_types.def"
#undef XR_PRELUDE_TYPE
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
extern void xr_register_logger_class(XrVMRuntime *isolate);
extern void xr_register_datetime_class(XrVMRuntime *isolate);
extern void xr_regex_register_class(XrVMRuntime *isolate);
extern void xr_sys_mutex_register_class(XrVMRuntime *isolate);
extern void xr_sys_rwlock_register_class(XrVMRuntime *isolate);
extern void xr_sys_condvar_register_class(XrVMRuntime *isolate);
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
 * table maps user-visible names ("PanicInfo", "Range", "DateTime", ...)
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
 * pattern matching.  Members/values are interned/copied by
 * xr_enum_type_new, so the input arrays are freed here. */
static XrEnumType *make_prelude_enum(XrVMRuntime *X, const char *name, const char **member_names,
                                     const int *member_values, int count, const int *payload_counts,
                                     bool is_adt) {
    char **names = (char **) xr_malloc(sizeof(char *) * (size_t) count);
    XrValue *values = (XrValue *) xr_malloc(sizeof(XrValue) * (size_t) count);
    if (!names || !values) {
        xr_free(names);
        xr_free(values);
        return NULL;
    }
    for (int i = 0; i < count; i++) {
        size_t len = strlen(member_names[i]) + 1;
        names[i] = (char *) xr_malloc(len);
        if (names[i])
            memcpy(names[i], member_names[i], len);
        values[i] = xr_int(member_values[i]);
    }

    XrEnumType *et = xr_enum_type_new(X, name, XR_TINT, names, values, count);

    if (et && is_adt) {
        et->is_adt = true;
        et->payload_counts = (int *) xr_calloc((size_t) count, sizeof(int));
        int max_pc = 0;
        if (et->payload_counts) {
            for (int i = 0; i < count; i++) {
                int pc = payload_counts ? payload_counts[i] : 0;
                et->payload_counts[i] = pc;
                if (pc > max_pc)
                    max_pc = pc;
            }
        }
        et->max_payload = max_pc;
        if (et->enum_class && max_pc > 0) {
            et->enum_class->field_count = (uint16_t) (1 + max_pc);
            et->enum_class->own_field_count = (uint16_t) (1 + max_pc);
            et->enum_class->builtin_kind = XR_BK_ADT_ENUM;
        }
    }

    for (int i = 0; i < count; i++)
        xr_free(names[i]);
    xr_free(names);
    xr_free(values);
    return et;
}

static void xr_prelude_register_builtin_enums(XrVMRuntime *X) {
    if (!X)
        return;

    /* Ordering { Relaxed, Acquire, Release, AcquireRelease, SeqCst } — values
     * must match XrAtomicOrdering. */
    static const char *ordering_members[] = {"Relaxed", "Acquire", "Release", "AcquireRelease",
                                             "SeqCst"};
    static const int ordering_values[] = {0, 1, 2, 3, 4};
    XrEnumType *ordering_et =
        make_prelude_enum(X, "Ordering", ordering_members, ordering_values, 5, NULL, false);
    if (ordering_et)
        bind_builtin_value(X, XR_GLOBAL_VAR_ORDERING, XR_FROM_PTR(ordering_et));

    static const char *recv_members[] = {"Value", "Empty", "Timeout", "Closed"};
    static const int recv_values[] = {0, 1, 2, 3};
    static const int recv_payload_counts[] = {1, 0, 0, 0};
    XrEnumType *recv_et =
        make_prelude_enum(X, "Recv", recv_members, recv_values, 4, recv_payload_counts, true);
    if (recv_et)
        bind_builtin_value(X, XR_GLOBAL_VAR_RECV, XR_FROM_PTR(recv_et));

    static const char *send_result_members[] = {"Sent", "Full", "Timeout", "Closed"};
    static const int send_result_values[] = {0, 1, 2, 3};
    XrEnumType *send_result_et =
        make_prelude_enum(X, "SendResult", send_result_members, send_result_values, 4, NULL, false);
    if (send_result_et)
        bind_builtin_value(X, XR_GLOBAL_VAR_SEND_RESULT, XR_FROM_PTR(send_result_et));

    static const char *task_result_members[] = {"Success", "Failed", "Cancelled", "Timeout",
                                                "Pending"};
    static const int task_result_values[] = {0, 1, 2, 3, 4};
    static const int task_result_payload_counts[] = {1, 1, 0, 0, 0};
    XrEnumType *task_result_et =
        make_prelude_enum(X, "TaskResult", task_result_members, task_result_values, 5,
                          task_result_payload_counts, true);
    if (task_result_et)
        bind_builtin_value(X, XR_GLOBAL_VAR_TASK_RESULT, XR_FROM_PTR(task_result_et));

    static const char *task_outcome_members[] = {"Success", "Failed", "Cancelled"};
    static const int task_outcome_values[] = {0, 1, 2};
    static const int task_outcome_payload_counts[] = {1, 1, 0};
    XrEnumType *task_outcome_et =
        make_prelude_enum(X, "TaskOutcome", task_outcome_members, task_outcome_values, 3,
                          task_outcome_payload_counts, true);
    if (task_outcome_et)
        bind_builtin_value(X, XR_GLOBAL_VAR_TASK_OUTCOME, XR_FROM_PTR(task_outcome_et));

    static const char *task_status_members[] = {"Pending", "Running", "Success", "Failed",
                                                "Cancelled"};
    static const int task_status_values[] = {0, 1, 2, 3, 4};
    XrEnumType *task_status_et =
        make_prelude_enum(X, "TaskStatus", task_status_members, task_status_values, 5, NULL, false);
    if (task_status_et)
        bind_builtin_value(X, XR_GLOBAL_VAR_TASK_STATUS, XR_FROM_PTR(task_status_et));

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
    xr_register_datetime_class(isolate);
    xr_register_logger_class(isolate);
    xr_regex_register_class(isolate);
    xr_sys_mutex_register_class(isolate);
    xr_sys_rwlock_register_class(isolate);
    xr_sys_condvar_register_class(isolate);
    xr_netconn_register_class(isolate);
    xr_netlistener_register_class(isolate);

    /* Bind unified-class XrClass values into VM builtins so the
     * IR lowerer's builtin_classes table can resolve them. */
    XrayCoreClasses *core = isolate->core;
    if (core) {
        bind_class_global(isolate, XR_GLOBAL_VAR_PANIC_INFO, core->panicInfoClass);
        bind_class_global(isolate, XR_GLOBAL_VAR_RANGE, core->rangeClass);
        bind_class_global(isolate, XR_GLOBAL_VAR_DATETIME, core->dateTimeClass);
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
     * to. This makes user-side annotations like `let dt: DateTime = ...`
     * usable without a separate `import datetime`, at the cost of always
     * linking those four stdlib modules into the binary. */
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
    module->loaded = true;
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
