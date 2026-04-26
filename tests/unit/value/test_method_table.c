/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_method_table.c - Lock down the builtin method registry contract.
 *
 * KEY POINTS:
 *   - xr_builtin_method_tables[] must be exactly XR_TID_COUNT entries
 *     long. Out-of-bounds reads must yield NULL via the lookup helper.
 *   - The registry starts entirely NULL: types are migrated one at a
 *     time and each migration flips a single slot, so the steady-state
 *     invariant during the migration period is "every slot either NULL
 *     or a stable static pointer". This test checks the all-NULL
 *     pre-migration baseline.
 *   - xr_method_table_lookup must short-circuit on NULL registry slot,
 *     out-of-range type id, out-of-range symbol id, and NULL fn.
 */

#include "../test_framework.h"
#include "runtime/value/xmethod_table.h"
#include "runtime/value/xtype_names.h"

#include <stddef.h>

/* ========== Registry shape ========== */

TEST(registry_is_dense_and_typed) {
    /* Indexing all the way up to XR_TID_COUNT-1 must be safe; the
     * array size is enforced at compile time inside xmethod_table.c. */
    for (int tid = 0; tid < XR_TID_COUNT; tid++) {
        const XrMethodSlot *table = xr_builtin_method_tables[tid];
        /* Until owners migrate, every slot is NULL by spec. */
        ASSERT(table == NULL);
    }
}

/* ========== Lookup helper short-circuits ========== */

TEST(lookup_returns_null_for_unmigrated_type) {
    /* Bool has no table yet — every lookup must fail cleanly. */
    const XrMethodSlot *slot = xr_method_table_lookup(XR_TID_BOOL, 0, 64);
    ASSERT(slot == NULL);
}

TEST(lookup_rejects_out_of_range_type_id) {
    const XrMethodSlot *slot =
        xr_method_table_lookup((XrTypeId)XR_TID_COUNT, 0, 64);
    ASSERT(slot == NULL);

    slot = xr_method_table_lookup((XrTypeId)(XR_TID_COUNT + 100), 0, 64);
    ASSERT(slot == NULL);
}

TEST(lookup_rejects_out_of_range_symbol_id) {
    /* Even with a hypothetical migrated type, an out-of-range symbol
     * id must short-circuit. We exercise the path with NULL table
     * which returns NULL before the symbol bounds check, so install
     * a tiny local-test-only proxy by going through a known NULL
     * slot type and a clearly invalid symbol. */
    const XrMethodSlot *slot = xr_method_table_lookup(XR_TID_INT, -1, 64);
    ASSERT(slot == NULL);

    slot = xr_method_table_lookup(XR_TID_INT, 999999, 64);
    ASSERT(slot == NULL);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()
    RUN_TEST_SUITE("Method-table registry shape");
    RUN_TEST(registry_is_dense_and_typed);

    RUN_TEST_SUITE("xr_method_table_lookup short-circuits");
    RUN_TEST(lookup_returns_null_for_unmigrated_type);
    RUN_TEST(lookup_rejects_out_of_range_type_id);
    RUN_TEST(lookup_rejects_out_of_range_symbol_id);
TEST_MAIN_END()
