/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * common.h - Shared inline helpers for stdlib C modules
 *
 * KEY CONCEPT:
 *   Eliminates per-module duplication of argument parsing, interned-string
 *   creation and module-export boilerplate. All helpers are header-only and
 *   add no new translation unit to the link graph.
 *
 * WHY THIS DESIGN:
 *   Before this header, each stdlib .c file redefined `get_string_arg`,
 *   `make_string` and the `EXPORT_CFUNC` macro, producing ~150 lines of
 *   boilerplate per module and drifting subtly over time (different NULL
 *   handling, different naming). A single inline header keeps the bindings
 *   uniform and lets the compiler inline everything away.
 *
 * USAGE:
 *     #include "../common.h"
 *
 *     static XrValue my_fn(XrayIsolate *X, XrValue *args, int argc) {
 *         const char *s = xrs_string_arg(args[0], NULL);
 *         if (!s) return xr_null();
 *         return xrs_string_value_c(X, s);
 *     }
 *
 *     XrModule* xr_load_module_example(XrayIsolate *X) {
 *         XrModule *m = xr_module_create_native(X, "example");
 *         XRS_EXPORT(m, X, "myFn", my_fn);
 *         m->loaded = true;
 *         return m;
 *     }
 */

#ifndef XR_STDLIB_COMMON_H
#define XR_STDLIB_COMMON_H

#include <string.h>

#include "../src/runtime/value/xvalue.h"
#include "../src/runtime/object/xstring.h"
#include "../src/module/xmodule.h"
#include "../src/module/xbuiltin_decl.h"
#include "../src/runtime/xexec_frame.h"     // XrCFunction full definition, XR_CFUNC_SLOW

/* ========== String Argument Accessor ========== */

/*
 * Extract a C-string pointer from an XrValue argument.
 * Returns NULL if the value is not a string. The returned pointer remains
 * valid for the lifetime of the underlying XrString (GC-managed); callers
 * must not free it.
 *
 * @param v       Argument value.
 * @param out_len If non-NULL, receives the string length in bytes.
 */
static inline const char* xrs_string_arg(XrValue v, size_t *out_len) {
    if (!XR_IS_STRING(v)) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    XrString *s = XR_TO_STRING(v);
    if (out_len) *out_len = s->length;
    return s->data;
}

/* ========== Interned String Value Constructors ========== */

/*
 * Intern an (ptr, len) pair and wrap the resulting XrString into an XrValue.
 * Returns xr_null() if ptr is NULL.
 */
static inline XrValue xrs_string_value_n(XrayIsolate *X, const char *s, size_t len) {
    if (!s) return xr_null();
    XrString *str = xr_string_intern(X, s, len, 0);
    return xr_string_value(str);
}

/*
 * Intern a NUL-terminated C string and wrap the result into an XrValue.
 * Returns xr_null() if s is NULL.
 */
static inline XrValue xrs_string_value_c(XrayIsolate *X, const char *s) {
    if (!s) return xr_null();
    return xrs_string_value_n(X, s, strlen(s));
}

/* ========== Module Export Helper ========== */

/*
 * Register a non-yieldable C function as a module export.
 *
 * Thin alias over XR_MODULE_EXPORT (declared in xbuiltin_decl.h) that keeps
 * call sites short and visually consistent with the xrs_* helper family.
 */
#define XRS_EXPORT(mod, isolate, name_str, func_ptr) \
    XR_MODULE_EXPORT((mod), (isolate), (name_str), (func_ptr))

/*
 * Register a yieldable C function (may call xr_yield / xr_await) as a
 * module export. Yieldable bindings use a different cfunction constructor
 * because their entry point carries an out-parameter signature.
 *
 * The callee declaration xr_vm_yieldable_cfunction_new() lives in
 * src/vm/xvm.h. Modules using XRS_EXPORT_YIELDABLE therefore need to have
 * that header on the include path; see stdlib/test_yield/test_yield.c for
 * a representative example.
 */
/*
 * Register a C function marked SLOW (blocking I/O). The scheduler will
 * hand the coroutine to a dedicated M-thread immediately instead of running
 * it on a P worker queue.
 */
#define XRS_EXPORT_SLOW(mod, isolate, name_str, func_ptr)                                   \
    do {                                                                                    \
        struct XrCFunction *_cf =                                                           \
            xr_vm_cfunction_new((isolate), (func_ptr), (name_str));                         \
        _cf->cfunc_class = XR_CFUNC_SLOW;                                                   \
        xr_module_add_export((isolate), (mod), (name_str), xr_value_from_cfunction(_cf));   \
    } while (0)

#define XRS_EXPORT_YIELDABLE(mod, isolate, name_str, func_ptr)                              \
    do {                                                                                    \
        struct XrCFunction *_cf =                                                           \
            xr_vm_yieldable_cfunction_new((isolate), (func_ptr), (name_str));               \
        xr_module_add_export((isolate), (mod), (name_str), xr_value_from_cfunction(_cf));   \
    } while (0)

#endif // XR_STDLIB_COMMON_H
