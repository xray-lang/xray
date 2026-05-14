/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xglobal_dict.h - Name-keyed top-level binding store
 *
 * KEY CONCEPT:
 *   Single source of truth for top-level user bindings.  Every
 *   `let / const / fn / class` at the top level reads and writes
 *   through this dict; the bytecode encodes the binding's name as
 *   an interned string in the constant pool, never an integer slot.
 *
 *   Replaces the old XrSharedArray + per-proto shared_offset model,
 *   which required several disjoint tables (compile-time slot map,
 *   REPL symbol table, runtime shared array) to be kept in sync.
 *
 * VS XrGlobalsTable:
 *   XrGlobalsTable is an integer-indexed array used by the compiler
 *   for classes / enums / types — it is a registry, not a runtime
 *   binding store.  The two coexist for now; XrGlobalsTable will be
 *   merged in once class/enum top-level declarations migrate to the
 *   dict path.
 *
 * VS XrMap:
 *   XrGlobalDict is implemented over XrMap with XrString* keys.  The
 *   wrapper exists so the dispatch code does not box raw pointers
 *   into XrValue at every access site, and so we can swap the
 *   underlying representation later without churning every call site.
 *
 * THREAD SAFETY:
 *   Not thread-safe.  Each isolate has its own dict; cross-coroutine
 *   access goes through the same isolate VM serialization that the
 *   rest of the runtime relies on.
 */

#ifndef XGLOBAL_DICT_H
#define XGLOBAL_DICT_H

#include "value/xvalue.h"
#include "object/xstring.h"
#include "object/xmap.h"
#include "../base/xdefs.h"
#include <stdbool.h>

/* Wrapper around a string-keyed XrMap.  All keys are interned
 * XrString*; values are arbitrary XrValue.  The map node memory is
 * owned by the GC heap, so the dict participates in mark-sweep via
 * the isolate's GC root list. */
typedef struct XrGlobalDict {
    XrMap *map;
} XrGlobalDict;

/* Construct an empty dict.  Allocates the underlying XrMap on the
 * isolate's GC heap, so a coroutine context is required (use the
 * main coroutine for isolate-wide globals). */
XR_FUNC void xr_global_dict_init(XrGlobalDict *gd, struct XrCoroutine *coro);

/* Tear down the dict.  The XrMap itself is GC-owned; this clears the
 * pointer so subsequent accesses fault loudly during shutdown. */
XR_FUNC void xr_global_dict_destroy(XrGlobalDict *gd);

/* Look up a binding.  Returns null if absent.  Never throws. */
XR_FUNC XrValue xr_global_dict_get(XrGlobalDict *gd, XrString *name);

/* Insert or overwrite a binding.  Triggers a GC barrier on the
 * underlying map, mirroring the contract of xr_map_set. */
XR_FUNC void xr_global_dict_set(XrGlobalDict *gd, XrString *name, XrValue value);

/* Existence check used by `.vars` / completion / analyzer-side
 * cross-checks.  O(1) average. */
XR_FUNC bool xr_global_dict_has(XrGlobalDict *gd, XrString *name);

/* Number of bindings.  Used by introspection and tests. */
XR_FUNC uint32_t xr_global_dict_count(XrGlobalDict *gd);

/* Iterate every (name, value) pair.  Iteration order is insertion-
 * stable for the chained-hash XrMap implementation, which is what
 * `.vars` and tab completion rely on for predictable output. */
typedef void (*XrGlobalDictVisitor)(XrString *name, XrValue *value, void *ud);
XR_FUNC void xr_global_dict_iter(XrGlobalDict *gd, XrGlobalDictVisitor visit, void *ud);

#endif  // XGLOBAL_DICT_H
