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

/*
 * Per-type method tables are declared as `extern const XrMethodSlot []`
 * by their owning modules and published here.
 *
 * Migration is incremental: each owner module flips its slot from NULL
 * to its own table. Until a type is migrated, its slot stays NULL and
 * the VM keeps using the legacy hand-rolled dispatcher in
 * src/vm/xvm_builtins.c.
 */

const XrMethodSlot *const xr_builtin_method_tables[XR_TID_COUNT] = {
    [XR_TID_BOOL]   = xr_bool_method_table,
    [XR_TID_INT]    = xr_int_method_table,
    [XR_TID_FLOAT]  = xr_float_method_table,
    [XR_TID_BIGINT] = xr_bigint_method_table,
    [XR_TID_SET]    = xr_set_method_table,
    [XR_TID_MAP]    = xr_map_method_table,
    [XR_TID_JSON]   = xr_json_method_table,
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
XR_STATIC_ASSERT(XR_TID_NULL == 0,
                 "XR_TID_NULL must be 0 so default-init slots map to it");
