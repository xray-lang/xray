/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * prelude.h - Prelude module: process-wide type marker registry
 *
 * KEY CONCEPT:
 *   Prelude is the single source of truth for which "built-in" type names
 *   (Array, Map, Json, BigInt, ...) the parser should recognise without
 *   the user writing `import prelude`. The lexer treats every such name
 *   as a plain identifier; the parser's type-context branch consults the
 *   per-isolate prelude symbol table (populated by xr_load_module_prelude
 *   during isolate init) to decide whether a name maps to a generic
 *   container, a singleton type, or a simple
 *   named-instance type.
 *
 * WHY THIS DESIGN:
 *   - Single registration point: adding a new built-in type name only
 *     requires one line in prelude_types.def.
 *   - User class can shadow prelude entries (Rust prelude semantics):
 *     `class Array { ... }` is consulted first, prelude is the fallback.
 *   - Registry is a process-wide constant (read-only), shared across all
 *     isolates with zero per-isolate cost.
 */

#ifndef XR_STDLIB_PRELUDE_H
#define XR_STDLIB_PRELUDE_H

#include "../../src/runtime/xisolate_internal.h"

/*
 * Prelude marker kind. Selects the syntactic and constructor path the
 * parser must follow once the resolver has confirmed a name belongs to
 * the prelude.
 */
typedef enum {
    XR_PRELUDE_KIND_SIMPLE,     // XR_KIND_INSTANCE with class_name == name
    XR_PRELUDE_KIND_GENERIC_1,  // requires <T>, e.g. Array<int>, Set<T>, Channel<T>
    XR_PRELUDE_KIND_GENERIC_2,  // requires <K, V>, e.g. Map<string, int>
    XR_PRELUDE_KIND_SINGLETON,  // process-wide singleton XrType (e.g. Json)
} XrPreludeKind;

/*
 * Single entry in the prelude type table. Populated by prelude_types.def.
 *
 * native_type carries the GC type id (XR_TARRAY etc.) when applicable so
 * downstream consumers (analyzer method tables, runtime registration)
 * can index into per-type tables without a second name lookup. A value
 * of 0 means "no associated GC type id".
 */
typedef struct XrPreludeTypeEntry {
    const char *name;
    int kind;             // XrPreludeKind value
    uint8_t native_type;  // XrObjType, 0 if not applicable
} XrPreludeTypeEntry;

/*
 * Per-isolate handle to the (process-wide) prelude registry. Stored on
 * isolate->prelude_symbols as an opaque void*; cast back via
 * xr_prelude_get_symbols.
 */
typedef struct XrPreludeSymbols {
    const XrPreludeTypeEntry *types;
    uint16_t type_count;
} XrPreludeSymbols;

/*
 * Module loader. Idempotent: calling it twice on the same isolate is
 * harmless because the registry is process-wide constant and only the
 * isolate->prelude_symbols pointer is rewired (to the same value).
 *
 * Registered in src/module/xmodule.c::stdlib_core[] so that an explicit
 * `import prelude` works as a no-op alias; auto-invoked from
 * xisolate_full.c::isolate_init_full() so users do not need it.
 */
XR_FUNC struct XrModule *xr_load_module_prelude(XrayIsolate *isolate);

/*
 * Accessor used by the frontend (parser type-context branch) to retrieve
 * the table without depending on stdlib internals. Returns NULL when the
 * isolate has not loaded the prelude (e.g. minimal-runtime isolates that
 * skipped xray_isolate_setup_full).
 */
XR_FUNC const XrPreludeSymbols *xr_prelude_get_symbols(XrayIsolate *isolate);

/*
 * Lookup a name (length-bounded, not NUL-terminated) in the prelude
 * table. Returns NULL when not present. Linear scan is intentional:
 * the table currently has < 16 entries and is searched only on
 * type-annotation parsing, never on a hot bytecode dispatch path.
 */
XR_FUNC const XrPreludeTypeEntry *xr_prelude_lookup_type(const XrPreludeSymbols *symbols,
                                                         const char *name, size_t len);

/*
 * Eagerly register every native XrClass that prelude entries refer to:
 * Logger (log), DateTime (datetime), Regex (regex), NetConn / NetListener
 * (net). Called from inside xr_load_module_prelude during isolate init,
 * so that user code can write `let dt: DateTime = ...` and have method
 * dispatch work even when the user has not separately `import datetime`.
 *
 * The cost of this design is that the four stdlib modules above are
 * always linked into the binary (their method handlers and class
 * builders are reachable from the prelude). xray's "batteries included"
 * stance accepts this in exchange for a single uniform registration
 * point — there is no longer any per-module xr_register_native_type
 * call. Each stdlib module exports a `xr_<type>_register_native_type`
 * function that this routine forwards to.
 */
XR_FUNC void xr_prelude_register_all_native_types(XrayIsolate *isolate);

#endif  // XR_STDLIB_PRELUDE_H
