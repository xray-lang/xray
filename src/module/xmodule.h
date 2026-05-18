/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmodule.h - Module system core
 *
 * KEY CONCEPT:
 *   - Module loading and caching
 *   - Export table management
 *   - Native and Script module support
 *   - Path resolution (supports index.xr directory entry)
 */

#ifndef XMODULE_H
#define XMODULE_H

#include "../runtime/value/xvalue.h"
#include "../base/xhashmap.h"
#include "../runtime/symbol/xsymbol_table.h"
#include <stdbool.h>
#include "../base/xdefs.h"

// Forward declaration (full definition in pkg/xproject.h, only needed in xmodule.c)
typedef struct XrProject XrProject;

/* ========== Module Types ========== */

typedef enum {
    MODULE_TYPE_NATIVE,  // Native module implemented in C (stdlib)
    MODULE_TYPE_SCRIPT,  // Xray script module
    MODULE_TYPE_MIXED,   // Mixed module (C + Xray)
} ModuleType;

/* ========== Module Object ========== */

/*
** XrModule - Dedicated Module type
**
** Design:
** - SymbolId-indexed flat export table for O(1) property access
** - Sparse array maps SymbolId range to dense index
** - No XrMap/XrHashMap overhead, direct array indexing in VM hot path
*/

#define XR_EXPORT_CONST 0x01

typedef struct XrModule {
    XrGCHeader gc;

    char *name;  // Module name (e.g. "time")
    char *path;  // Module path (e.g. "std/time/time.xr")
    ModuleType module_type;

    // SymbolId-indexed export table
    XrValue *export_values;    // Dense array of export values [export_count]
    SymbolId *export_symbols;  // Dense array of SymbolIds [export_count] (for iteration)
    uint8_t *export_flags;     // Dense array of flags [export_count] (bit 0 = const)

    int16_t *symbol_to_index;  // Sparse lookup: [max_symbol - min_symbol + 1], -1 = not found
    SymbolId min_symbol;
    SymbolId max_symbol;
    uint16_t export_count;
    uint16_t export_capacity;

    bool loaded;
    bool loading;

    void *native_handle;
    struct XrClosure *init_fn;
    void *compiled_code;
} XrModule;

/* ========== Inline O(1) Export Access ========== */

static inline XrValue xr_module_get_sym(XrModule *m, SymbolId sym) {
    if (m->symbol_to_index) {
        if (sym < m->min_symbol || sym > m->max_symbol)
            return xr_null();
        int16_t idx = m->symbol_to_index[sym - m->min_symbol];
        return (idx >= 0) ? m->export_values[idx] : xr_null();
    }
    // Fallback: linear scan (during loading or before index built)
    for (uint16_t i = 0; i < m->export_count; i++) {
        if (m->export_symbols[i] == sym)
            return m->export_values[i];
    }
    return xr_null();
}

static inline void xr_module_set_sym(XrModule *m, SymbolId sym, XrValue val) {
    if (m->symbol_to_index) {
        if (sym < m->min_symbol || sym > m->max_symbol)
            return;
        int16_t idx = m->symbol_to_index[sym - m->min_symbol];
        if (idx >= 0)
            m->export_values[idx] = val;
        return;
    }
    for (uint16_t i = 0; i < m->export_count; i++) {
        if (m->export_symbols[i] == sym) {
            m->export_values[i] = val;
            return;
        }
    }
}

static inline bool xr_module_is_const_sym(XrModule *m, SymbolId sym) {
    if (m->symbol_to_index) {
        if (sym < m->min_symbol || sym > m->max_symbol)
            return false;
        int16_t idx = m->symbol_to_index[sym - m->min_symbol];
        return (idx >= 0) && (m->export_flags[idx] & XR_EXPORT_CONST);
    }
    for (uint16_t i = 0; i < m->export_count; i++) {
        if (m->export_symbols[i] == sym)
            return (m->export_flags[i] & XR_EXPORT_CONST) != 0;
    }
    return false;
}

static inline bool xr_module_has_sym(XrModule *m, SymbolId sym) {
    if (m->symbol_to_index) {
        if (sym < m->min_symbol || sym > m->max_symbol)
            return false;
        return m->symbol_to_index[sym - m->min_symbol] >= 0;
    }
    for (uint16_t i = 0; i < m->export_count; i++) {
        if (m->export_symbols[i] == sym)
            return true;
    }
    return false;
}

/* ========== Native Module Loader Type ========== */

typedef XrModule *(*NativeModuleLoader)(struct XrayIsolate *isolate);

/* ========== Module Registry ========== */

typedef struct XrModuleRegistry {
    XrHashMap *native_loaders;  // Module name → NativeModuleLoader
    XrHashMap *loaded_modules;  // Module path → XrModule*

    char *stdlib_path;  // Stdlib path (default: stdlib/)

    // Project config (optional, for package management)
    XrProject *project;

    // Compiler hooks — per-Isolate, NULL in lite/bytecode-only mode.
    // Using void* function pointers avoids pulling in parser/compiler headers.
    void *(*fn_parse)(void *, const char *, const char *);
    void *(*fn_compile_ast)(void *, void *, const char *);
    void *(*fn_compile_src)(void *, const char *, const char *);
    void (*fn_ast_free)(void *);
} XrModuleRegistry;

/* ========== Module System API ========== */

XR_FUNC void xr_module_system_init(struct XrayIsolate *isolate);

/*
** Initialize module system (with script path)
** Loads project config (if xray.toml exists)
**
** @param isolate     Isolate instance
** @param script_path Entry script path
*/
XR_FUNC void xr_module_system_init_with_script(struct XrayIsolate *isolate,
                                               const char *script_path);
XR_FUNC void xr_module_system_free(struct XrayIsolate *isolate);

/*
** Register Native module loader
**
** @param isolate Isolate instance
** @param name    Module name (e.g. "time")
** @param loader  Loader function
**
** Example:
**   xr_module_register_native(isolate, "time", xr_load_module_time);
*/
XR_FUNC void xr_module_register_native(struct XrayIsolate *isolate, const char *name,
                                       NativeModuleLoader loader);

/*
** Import module (called by VM instruction)
**
** @param isolate     Isolate instance
** @param module_name Module name (e.g. "time")
** @return            Module object (contains export table) or XR_NULL
**
** Flow:
** 1. Check cache
** 2. Resolve path
** 3. Load module (native or script)
** 4. Execute initialization
** 5. Cache and return
*/
XR_FUNC XrValue xr_module_import(struct XrayIsolate *isolate, const char *module_name);
XR_FUNC XrValue xr_module_import_member(struct XrayIsolate *isolate, const char *module_name,
                                        const char *member_name);
XR_FUNC void xr_module_add_current_export(struct XrayIsolate *isolate, const char *name,
                                          XrValue value, bool is_const);
XR_FUNC bool xr_module_is_export_const(struct XrayIsolate *isolate, struct XrModule *module,
                                       const char *name);

/* ========== Module Object Operations ========== */

XR_FUNC XrModule *xr_module_create_native(struct XrayIsolate *isolate, const char *name);
XR_FUNC XrModule *xr_module_create_script(struct XrayIsolate *isolate, const char *name,
                                          const char *path);
XR_FUNC void xr_module_add_export(struct XrayIsolate *isolate, XrModule *module, const char *name,
                                  XrValue value);
XR_FUNC void xr_module_add_export_sym(struct XrayIsolate *isolate, XrModule *module, SymbolId sym,
                                      XrValue value, bool is_const);
XR_FUNC XrValue xr_module_get_export(struct XrayIsolate *isolate, XrModule *module,
                                     const char *name);
XR_FUNC void xr_module_build_export_index(XrModule *module);
XR_FUNC void xr_module_free(XrModule *module);

/* ========== Module Path Resolution ========== */

/*
** Resolve module path
**
** @param isolate     Isolate instance
** @param module_name Module name
** @return            Full path or NULL
**
** Resolution rules:
** - "time"           → "stdlib/time/time.c" (native)
** - "datetime"       → "stdlib/datetime/datetime.c" (native)
** - "./mylib.xr"     → Relative path
** - "/path/to/lib.xr" → Absolute path
*/
XR_FUNC char *xr_module_resolve_path(struct XrayIsolate *isolate, const char *module_name);
XR_FUNC ModuleType xr_module_detect_type(const char *path);
XR_FUNC void xr_module_register_stdlib(struct XrayIsolate *isolate);

/* ========== Compiler Hook Registration ========== */

// Set compiler hooks for module loading (per-Isolate, called during isolate init).
// Uses void* function pointers to avoid pulling in parser/compiler headers.
XR_FUNC void xr_module_set_compiler_hooks(
    struct XrayIsolate *isolate, void *(*parse_fn)(void *, const char *, const char *),
    void *(*compile_ast_fn)(void *, void *, const char *),
    void *(*compile_src_fn)(void *, const char *, const char *), void (*ast_free_fn)(void *));

#endif  // XMODULE_H
