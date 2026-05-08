/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmethod_table.h - Builtin method dispatch table protocol.
 *
 * KEY CONCEPT:
 *   Every builtin type owns a `static const XrMethodSlot[]` table living in
 *   its own owning module (e.g. xint_methods.c, xstring_methods.c). The
 *   global `xr_builtin_method_tables[]` array maps XrTypeId -> per-type
 *   table pointer. VM dispatch (OP_INVOKE_BUILTIN) and AOT codegen
 *   both resolve through this single source of truth, so builtin
 *   implementations are never duplicated.
 *
 * INVARIANTS:
 *   - Every entry in `xr_builtin_method_tables` is either NULL (type has
 *     no builtin methods) or a stable pointer to a `static const`
 *     XrMethodSlot array of length XR_SYMBOL_COUNT.
 *   - Each XrMethodSlot is keyed by symbol_id (= analyzer-assigned
 *     dense int). Empty slots have fn == NULL.
 *   - Tables are compile-time constants. Never mutated at runtime.
 *
 * INLINING POLICY:
 *   - Hot small methods (length / charAt / has / push fast-path / ...)
 *     live as `static inline` in the corresponding *_methods.h header.
 *     AOT-generated C #includes the header and inlines them at the call
 *     site for zero call overhead.
 *   - The method table takes the address of the inline function, which
 *     forces the compiler to emit a single out-of-line copy used by VM
 *     interpreter dispatch (one indirect call).
 *   - Cold or large methods stay extern (`XR_FUNC`) in *_methods.c.
 */

#ifndef XMETHOD_TABLE_H
#define XMETHOD_TABLE_H

#include "xvalue.h"
#include "xtype_names.h"
#include "../../base/xchecks.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct XrayIsolate XrayIsolate;

/*
 * Unified method dispatch signature.
 *
 *   iso     - owning isolate (always non-NULL on entry).
 *   self    - receiver value (boxed; method body unboxes via XR_TO_*).
 *   args    - pointer to first argument (positional, no this slot).
 *   argc    - argument count (excluding self).
 *
 * RETURN VALUE:
 *   Successful dispatch returns the method's result. Contract violations
 *   (wrong argc, wrong arg type, missing receiver state, ...) MUST throw
 *   a catchable exception via xr_vm_unwind_with_trace() and return
 *   xr_null(). The VM dispatcher checks for a pending exception after
 *   the call and propagates accordingly.
 */
typedef XrValue (*XrMethodFn)(XrayIsolate *iso, XrValue self, XrValue *args, int argc);

/*
 * One entry in a per-type method table.
 *
 *   fn        - implementation pointer; NULL = symbol unsupported on this
 *               type (dispatcher reports "type has no method 'foo'").
 *   min_args  - minimum positional argument count (0 if no required args).
 *   max_args  - maximum positional argument count, or -1 for vararg.
 *   flags     - declarative attributes consumed by JIT/AOT. See
 *               XR_METHOD_FLAG_* below. Always 0 in 3a; populated as
 *               types migrate.
 */
typedef struct XrMethodSlot {
    XrMethodFn fn;
    int8_t min_args;
    int8_t max_args; /* -1 == vararg */
    uint8_t flags;
    uint8_t _reserved; /* keep struct 16-byte aligned */
} XrMethodSlot;

/* Method-slot flags (declarative; consumed by JIT/AOT specializers). */
#define XR_METHOD_FLAG_PURE (1u << 0)      /* no observable side effects */
#define XR_METHOD_FLAG_NO_GC (1u << 1)     /* never triggers GC */
#define XR_METHOD_FLAG_MAY_THROW (1u << 2) /* may raise on contract violation */
#define XR_METHOD_FLAG_MAY_YIELD (1u << 3) /* may yield the coroutine */

/*
 * Global registry: XrTypeId -> per-type method table.
 *
 * Each per-type table is a `static const XrMethodSlot []` indexed by
 * symbol_id. The registry itself is also `const`: a NULL entry means
 * "this type has no builtin methods (yet)".
 *
 * Defined in xmethod_table.c. Owning modules (xint_methods.c etc.)
 * publish their tables by populating the matching entry through a
 * dedicated initializer or, more commonly in xray, by extern-declaring
 * their table and letting xmethod_table.c reference it.
 */
extern const XrMethodSlot *const xr_builtin_method_tables[XR_TID_COUNT];

/*
 * Look up the method slot for (type_id, symbol_id) in O(1).
 *
 *   Returns NULL when:
 *     - type_id has no registered method table (registry slot is NULL),
 *     - or symbol_id is out of the table's symbol range,
 *     - or the slot's fn pointer is NULL (symbol unsupported on type).
 *
 *   The caller is responsible for argument-count and argument-type
 *   validation against slot->min_args / max_args / flags.
 */
static inline const XrMethodSlot *xr_method_table_lookup(XrTypeId type_id, int symbol_id,
                                                         int symbol_count) {
    if ((unsigned) type_id >= (unsigned) XR_TID_COUNT)
        return NULL;
    const XrMethodSlot *table = xr_builtin_method_tables[type_id];
    if (!table)
        return NULL;
    if ((unsigned) symbol_id >= (unsigned) symbol_count)
        return NULL;
    const XrMethodSlot *slot = &table[symbol_id];
    return slot->fn ? slot : NULL;
}

/*
 * Boot-time verification: every type with a method table must implement
 * required protocols (toString, iterator for collections, etc.).
 * Only checks in debug builds; compiles to no-op in release.
 * Call once during isolate initialization.
 */
XR_FUNC void xr_method_table_verify_protocols(void);

#ifdef __cplusplus
}
#endif

#endif /* XMETHOD_TABLE_H */
