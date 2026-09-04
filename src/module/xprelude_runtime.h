/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xprelude_runtime.h - Compiler/runtime prelude registry
 *
 * KEY CONCEPT:
 *   Prelude is the single source of truth for which "built-in" type names
 *   (Array, Map, Json, BigInt, ...) the parser recognises implicitly. Prelude
 *   is language-core state and is deliberately not importable. The lexer treats every such name
 *   as a plain identifier; the parser's type-context branch consults the
 *   per-isolate prelude symbol table (populated by xr_prelude_install
 *   during isolate init) to decide whether a name maps to a generic
 *   container, a singleton type, or a simple
 *   named-instance type.
 *
 * WHY THIS DESIGN:
 *   - Single registration point: adding a new built-in type name only
 *     requires one line in builtin_symbols.def.
 *   - User class can shadow prelude entries (Rust prelude semantics):
 *     `class Array { ... }` is consulted first, prelude is the fallback.
 *   - Registry is a process-wide constant (read-only), shared across all
 *     isolates with zero per-isolate cost.
 */

#ifndef XR_MODULE_XPRELUDE_RUNTIME_H
#define XR_MODULE_XPRELUDE_RUNTIME_H

#include "../runtime/xisolate_internal.h"

/*
 * Prelude marker kind. Selects the syntactic and constructor path the
 * parser must follow once the resolver has confirmed a name belongs to
 * the prelude.
 */
typedef enum {
    XR_PRELUDE_KIND_SIMPLE,     // XR_KIND_INSTANCE with class_name == name
    XR_PRELUDE_KIND_GENERIC_1,  // requires <T>, e.g. Array<int>, Set<T>, Channel<T>
    XR_PRELUDE_KIND_GENERIC_2,  // requires <K, V>, e.g. Map<string, int>
    XR_PRELUDE_KIND_SINGLETON,  // process-wide singleton XrType
} XrPreludeKind;

/*
 * Single entry in the prelude type table. Populated by builtin_symbols.def.
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
 * Install the prelude into an isolate. Idempotent: calling it twice on the
 * same isolate is harmless because the registry is process-wide constant and
 * only the isolate->prelude_symbols pointer is rewired (to the same value).
 *
 * Called from xisolate_full.c::isolate_init_full() before the module system
 * starts, because what it installs is isolate state that every module load
 * already assumes is in place.
 */
XR_FUNC void xr_prelude_install(XrVMRuntime *isolate);

/*
 * Accessor used by the frontend (parser type-context branch) to retrieve
 * the table without depending on stdlib internals. Returns NULL when the
 * isolate has not installed the prelude (e.g. minimal-runtime isolates that
 * skipped full VM setup).
 */
XR_FUNC const XrPreludeSymbols *xr_prelude_get_symbols(XrVMRuntime *isolate);

/*
 * Lookup a name (length-bounded, not NUL-terminated) in the prelude
 * table. Returns NULL when not present. Linear scan is intentional:
 * the table currently has < 16 entries and is searched only on
 * type-annotation parsing, never on a hot bytecode dispatch path.
 */
XR_FUNC const XrPreludeTypeEntry *xr_prelude_lookup_type(const XrPreludeSymbols *symbols,
                                                         const char *name, size_t len);

/*
 * Eagerly register runtime-owned native XrClasses. Private provider storage
 * classes are registered here so stdlib source modules can call their leaves;
 * they are not prelude types and never publish a user-visible class identity.
 *
 * Registration is centralized so class construction order is deterministic
 * and no stdlib module needs a second user-visible initialization path.
 */
XR_FUNC void xr_prelude_register_all_native_types(XrVMRuntime *isolate);

#endif  // XR_MODULE_XPRELUDE_RUNTIME_H
