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
struct XrayIsolate;
/* Types not registered by xr_core_init — they live in stdlib or
 * depend on runtime infrastructure only available after core init. */
extern void xr_iterator_register_class(XrayIsolate *isolate);
extern void xr_register_range_class(XrayIsolate *isolate);
extern void xr_register_logger_class(XrayIsolate *isolate);
extern void xr_register_datetime_class(XrayIsolate *isolate);
extern void xr_regex_register_native_type(XrayIsolate *isolate);
extern void xr_netconn_register_native_type(XrayIsolate *isolate);
extern void xr_netlistener_register_native_type(XrayIsolate *isolate);

#include "../../src/base/xglobal_indices.h"
#include "../../src/runtime/class/xclass_system.h"
#include "../../src/runtime/value/xvalue.h"

/* Bind a unified-class XrClass into the VM builtins slot keyed by a
 * predefined XR_GLOBAL_VAR_* index. The IR lowerer's builtin_classes
 * table maps user-visible names ("Exception", "Range", "DateTime", ...)
 * onto these indices via XI_GET_BUILTIN, so `new Exception(...)`
 * resolves to the actual class value at run time. */
static void bind_class_global(XrayIsolate *X, int global_index, void *cls) {
    if (!X || !cls)
        return;
    if ((size_t) global_index < (size_t) XR_USER_GLOBALS_START) {
        X->vm.builtins[global_index] = xr_value_from_class((struct XrClass *) cls);
        if (X->vm.builtin_count < XR_USER_GLOBALS_START)
            X->vm.builtin_count = XR_USER_GLOBALS_START;
    }
}

void xr_prelude_register_all_native_types(XrayIsolate *isolate) {
    if (!isolate)
        return;
    /* Core types (int/float/bool/string/array/map/set/json/bigint/
     * stringbuilder/arrayslice/Exception) are registered by xr_core_init().
     * Prelude registers only the remaining native types. */
    xr_iterator_register_class(isolate);
    xr_register_range_class(isolate);
    xr_register_datetime_class(isolate);
    xr_register_logger_class(isolate);
    xr_regex_register_native_type(isolate);
    xr_netconn_register_native_type(isolate);
    xr_netlistener_register_native_type(isolate);

    /* Bind unified-class XrClass values into VM builtins so the
     * IR lowerer's builtin_classes table can resolve them. */
    XrayCoreClasses *core = isolate->core;
    if (core) {
        bind_class_global(isolate, XR_GLOBAL_VAR_EXCEPTION, core->exceptionClass);
        bind_class_global(isolate, XR_GLOBAL_VAR_RANGE, core->rangeClass);
        bind_class_global(isolate, XR_GLOBAL_VAR_DATETIME, core->dateTimeClass);
    }
}

/* ========== Module loader ========== */

XrModule *xr_load_module_prelude(XrayIsolate *isolate) {
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

const XrPreludeSymbols *xr_prelude_get_symbols(XrayIsolate *isolate) {
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
