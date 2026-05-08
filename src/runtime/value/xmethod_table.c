/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmethod_table.c - Builtin method registry: XrTypeId -> method table.
 *
 * KEY POINTS:
 *   - Compile-time-constant array indexed by XrTypeId. Adding a new
 *     builtin type means appending one extern reference here next to
 *     the matching XR_TID_* slot.
 *   - The C language guarantees missing initializers are zero, so any
 *     XrTypeId without a method table reads back as NULL automatically.
 *     OP_INVOKE_BUILTIN treats NULL as "no migrated table available;
 *     fall through to the legacy *_method_call_by_symbol dispatch".
 *
 * INVARIANTS:
 *   - The array is exactly XR_TID_COUNT entries long. A static_assert
 *     checks this at compile time so a stale entry never silently
 *     points to the wrong type.
 */

#include "xmethod_table.h"
#include "xbool_methods.h"
#include "xint_methods.h"
#include "xfloat_methods.h"
#include "../object/xbigint_methods.h"
#include "../object/xset_methods.h"
#include "../object/xmap_methods.h"
#include "../object/xjson_methods.h"
#include "../object/xarray_methods.h"
#include "../object/xstring_methods.h"
#include "../../../stdlib/datetime/datetime_methods.h"
#include "../../../stdlib/regex/regex_methods.h"

/*
 * Per-type method tables are declared as `extern const XrMethodSlot []`
 * by their owning modules and published here.
 *
 * Each owner module exposes its slot here; types whose dispatch is
 * still routed through native_type_classes (Iterator, StringBuilder,
 * ...) keep their slot NULL until they migrate to an
 * XrMethodSlot table.
 */

const XrMethodSlot *const xr_builtin_method_tables[XR_TID_COUNT] = {
    [XR_TID_BOOL] = xr_bool_method_table,     [XR_TID_INT] = xr_int_method_table,
    [XR_TID_FLOAT] = xr_float_method_table,   [XR_TID_BIGINT] = xr_bigint_method_table,
    [XR_TID_SET] = xr_set_method_table,       [XR_TID_MAP] = xr_map_method_table,
    [XR_TID_JSON] = xr_json_method_table,     [XR_TID_ARRAY] = xr_array_method_table,
    [XR_TID_STRING] = xr_string_method_table, [XR_TID_DATETIME] = xr_datetime_method_table,
    [XR_TID_REGEX] = xr_regex_method_table,
};

/*
 * Compile-time guards: catch the day someone reorders XrTypeId or
 * adds a new entry without thinking about builtin methods.
 *
 *   - Length must equal XR_TID_COUNT (one slot per type, plus the
 *     implicit zero initializer for the rest).
 *   - XR_TID_NULL must remain 0 so that the default-initialized
 *     entry corresponds to NULL.
 */
XR_STATIC_ASSERT(XR_TID_NULL == 0, "XR_TID_NULL must be 0 so default-init slots map to it");

/* ========== Protocol Completeness Verification ========== */

#include "../../base/xchecks.h"
#include "../symbol/xsymbol_table.h"

/*
 * Verify that every type with a registered method table implements the
 * required protocols.  Catches missing method registrations at boot
 * rather than at first call.
 *
 *   - toString:  every type with a table must provide it (print,
 *                string interpolation, and explicit conversion all
 *                dispatch through the method table).
 *   - iterator:  collection types that use XrIterator (Map, Set, Json)
 *                must provide it for for-in loops.
 *   - keys:      key-enumerable types (Map, Json) must provide it.
 */

/* Helper: check that table[sym] has a non-NULL fn. */
static bool has_method(XrTypeId tid, int sym) {
    return xr_method_table_lookup(tid, sym, SYMBOL_BUILTIN_COUNT) != NULL;
}

XR_FUNC void xr_method_table_verify_protocols(void) {
#if XR_DEBUG
    /* Every type with a registered method table must support toString. */
    for (int tid = 0; tid < XR_TID_COUNT; tid++) {
        if (xr_builtin_method_tables[tid] != NULL) {
            XR_CHECK_FMT(has_method((XrTypeId)tid, SYMBOL_TOSTRING),
                         "builtin type %d has method table but missing toString", tid);
        }
    }

    /* Collections whose plain for-in (for v in coll) dispatches through
     * the method table's iterator() slot.  Array/String use index-based
     * loops and don't need this. */
    static const XrTypeId iterator_types[] = {
        XR_TID_MAP, XR_TID_SET
    };
    for (int i = 0; i < (int)(sizeof(iterator_types) / sizeof(iterator_types[0])); i++) {
        XR_CHECK_FMT(has_method(iterator_types[i], SYMBOL_ITERATOR),
                     "type %d missing iterator method", iterator_types[i]);
    }

    /* Types that support key-value for-in (for k, v in coll) must
     * provide entriesIterator(). */
    static const XrTypeId kv_iterable_types[] = {
        XR_TID_MAP, XR_TID_JSON
    };
    for (int i = 0; i < (int)(sizeof(kv_iterable_types) / sizeof(kv_iterable_types[0])); i++) {
        XR_CHECK_FMT(has_method(kv_iterable_types[i], SYMBOL_ENTRIES_ITERATOR),
                     "type %d missing entriesIterator method", kv_iterable_types[i]);
    }

    /* Key-enumerable types must provide keys(). */
    static const XrTypeId keyable_types[] = {
        XR_TID_MAP, XR_TID_JSON
    };
    for (int i = 0; i < (int)(sizeof(keyable_types) / sizeof(keyable_types[0])); i++) {
        XR_CHECK_FMT(has_method(keyable_types[i], SYMBOL_KEYS),
                     "type %d missing keys method", keyable_types[i]);
    }
#endif  /* XR_DEBUG */
}
