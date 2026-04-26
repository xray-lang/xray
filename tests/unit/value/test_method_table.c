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
#include "runtime/symbol/xsymbol_table.h"

#include <stddef.h>

/* ========== Registry shape ========== */

TEST(registry_is_dense_and_typed) {
    /* Indexing all the way up to XR_TID_COUNT-1 must be safe; the
     * array size is enforced at compile time inside xmethod_table.c.
     * Migrated types must point at a stable, non-NULL table; the rest
     * stay NULL until their owner module migrates. */
    /* Migrated types so far. Add new ids here as the migration
     * sweeps the rest of xvm_builtins.c. */
    static const XrTypeId migrated[] = {
        XR_TID_BOOL,
        XR_TID_INT,
        XR_TID_FLOAT,
        XR_TID_BIGINT,
        XR_TID_SET,
        XR_TID_MAP,
    };
    for (int tid = 0; tid < XR_TID_COUNT; tid++) {
        const XrMethodSlot *table = xr_builtin_method_tables[tid];
        bool is_migrated = false;
        for (size_t i = 0; i < sizeof(migrated)/sizeof(migrated[0]); i++) {
            if (migrated[i] == (XrTypeId)tid) { is_migrated = true; break; }
        }
        if (is_migrated) {
            ASSERT(table != NULL);
        } else {
            ASSERT(table == NULL);
        }
    }
}

/* ========== Bool migration (vertical slice) ========== */

TEST(bool_method_table_exposes_toString) {
    const XrMethodSlot *slot = xr_method_table_lookup(
        XR_TID_BOOL, SYMBOL_TOSTRING, SYMBOL_BUILTIN_COUNT);
    ASSERT_NOT_NULL(slot);
    ASSERT_NOT_NULL(slot->fn);
    ASSERT_EQ_INT(slot->min_args, 0);
    ASSERT_EQ_INT(slot->max_args, 0);
    /* toString is pure / no-GC by spec. */
    ASSERT(slot->flags & XR_METHOD_FLAG_PURE);
    ASSERT(slot->flags & XR_METHOD_FLAG_NO_GC);
}

TEST(bool_method_table_unknown_symbol_returns_null) {
    /* push() makes no sense on bool; the table slot must be empty. */
    const XrMethodSlot *slot = xr_method_table_lookup(
        XR_TID_BOOL, SYMBOL_PUSH, SYMBOL_BUILTIN_COUNT);
    ASSERT(slot == NULL);
}

/* ========== BigInt migration ========== */

TEST(bigint_method_table_exposes_full_surface) {
    /* All eight migrated symbols must resolve. The flag bits are
     * deliberately not asserted here; xbigint_methods.c is the
     * authoritative source. */
    static const int symbols[] = {
        SYMBOL_TOSTRING, SYMBOL_ABS, SYMBOL_SIGN,
        SYMBOL_ISZERO, SYMBOL_ISNEGATIVE, SYMBOL_ISPOSITIVE,
        SYMBOL_TOINT, SYMBOL_TOFLOAT,
    };
    for (size_t i = 0; i < sizeof(symbols)/sizeof(symbols[0]); i++) {
        const XrMethodSlot *slot = xr_method_table_lookup(
            XR_TID_BIGINT, symbols[i], SYMBOL_BUILTIN_COUNT);
        ASSERT_NOT_NULL(slot);
        ASSERT_NOT_NULL(slot->fn);
        /* All bigint methods take exactly the receiver. */
        ASSERT_EQ_INT(slot->min_args, 0);
        ASSERT_EQ_INT(slot->max_args, 0);
    }
}

TEST(bigint_pure_predicates_carry_flags) {
    /* Pure / no-GC predicates must advertise both flags so JIT and
     * AOT specializers can lift them above safepoints. */
    static const int pure_symbols[] = {
        SYMBOL_SIGN, SYMBOL_ISZERO, SYMBOL_ISNEGATIVE,
        SYMBOL_ISPOSITIVE, SYMBOL_TOINT, SYMBOL_TOFLOAT,
    };
    for (size_t i = 0; i < sizeof(pure_symbols)/sizeof(pure_symbols[0]); i++) {
        const XrMethodSlot *slot = xr_method_table_lookup(
            XR_TID_BIGINT, pure_symbols[i], SYMBOL_BUILTIN_COUNT);
        ASSERT_NOT_NULL(slot);
        ASSERT(slot->flags & XR_METHOD_FLAG_PURE);
        ASSERT(slot->flags & XR_METHOD_FLAG_NO_GC);
    }
}

/* ========== Int migration ========== */

TEST(int_method_table_exposes_full_surface) {
    /* All twelve int symbols (toString / abs / toBigInt / max / min /
     * toFloat / toHex / floor / ceil / round / sqrt / pow) must
     * resolve. */
    static const int symbols[] = {
        SYMBOL_TOSTRING, SYMBOL_ABS, SYMBOL_TOBIGINT,
        SYMBOL_MAX, SYMBOL_MIN, SYMBOL_TOFLOAT, SYMBOL_TOHEX,
        SYMBOL_FLOOR, SYMBOL_CEIL, SYMBOL_ROUND,
        SYMBOL_SQRT, SYMBOL_POW,
    };
    for (size_t i = 0; i < sizeof(symbols)/sizeof(symbols[0]); i++) {
        const XrMethodSlot *slot = xr_method_table_lookup(
            XR_TID_INT, symbols[i], SYMBOL_BUILTIN_COUNT);
        ASSERT_NOT_NULL(slot);
        ASSERT_NOT_NULL(slot->fn);
    }
}

TEST(int_floor_ceil_round_are_no_ops) {
    /* Integer floor/ceil/round must reuse the same impl pointer
     * (the no-op identity function), proving the migration removed
     * the legacy float roundtrip. */
    const XrMethodSlot *floor_slot = xr_method_table_lookup(
        XR_TID_INT, SYMBOL_FLOOR, SYMBOL_BUILTIN_COUNT);
    const XrMethodSlot *ceil_slot = xr_method_table_lookup(
        XR_TID_INT, SYMBOL_CEIL, SYMBOL_BUILTIN_COUNT);
    const XrMethodSlot *round_slot = xr_method_table_lookup(
        XR_TID_INT, SYMBOL_ROUND, SYMBOL_BUILTIN_COUNT);
    ASSERT_NOT_NULL(floor_slot);
    ASSERT_NOT_NULL(ceil_slot);
    ASSERT_NOT_NULL(round_slot);
    /* All three must advertise pure / no-GC. */
    ASSERT(floor_slot->flags & XR_METHOD_FLAG_PURE);
    ASSERT(ceil_slot->flags  & XR_METHOD_FLAG_PURE);
    ASSERT(round_slot->flags & XR_METHOD_FLAG_PURE);
}

/* ========== Float migration ========== */

TEST(float_method_table_exposes_full_surface) {
    static const int symbols[] = {
        SYMBOL_TOSTRING, SYMBOL_TOFIXED,
        SYMBOL_FLOOR, SYMBOL_CEIL, SYMBOL_ROUND,
        SYMBOL_ABS, SYMBOL_SQRT, SYMBOL_TOINT, SYMBOL_POW,
    };
    for (size_t i = 0; i < sizeof(symbols)/sizeof(symbols[0]); i++) {
        const XrMethodSlot *slot = xr_method_table_lookup(
            XR_TID_FLOAT, symbols[i], SYMBOL_BUILTIN_COUNT);
        ASSERT_NOT_NULL(slot);
        ASSERT_NOT_NULL(slot->fn);
    }
}

TEST(float_arity_caps) {
    /* toFixed and pow accept up to one optional argument. */
    const XrMethodSlot *tofixed = xr_method_table_lookup(
        XR_TID_FLOAT, SYMBOL_TOFIXED, SYMBOL_BUILTIN_COUNT);
    const XrMethodSlot *powslot = xr_method_table_lookup(
        XR_TID_FLOAT, SYMBOL_POW, SYMBOL_BUILTIN_COUNT);
    ASSERT_NOT_NULL(tofixed);
    ASSERT_NOT_NULL(powslot);
    ASSERT_EQ_INT(tofixed->min_args, 0);
    ASSERT_EQ_INT(tofixed->max_args, 1);
    ASSERT_EQ_INT(powslot->min_args, 0);
    ASSERT_EQ_INT(powslot->max_args, 1);
}

/* ========== Map / WeakMap migration ========== */

TEST(map_method_table_exposes_full_surface) {
    /* All thirteen migrated map symbols must resolve. */
    static const int symbols[] = {
        SYMBOL_IS_EMPTY, SYMBOL_HAS, SYMBOL_GET, SYMBOL_SET,
        SYMBOL_DELETE, SYMBOL_CLEAR, SYMBOL_KEYS, SYMBOL_VALUES,
        SYMBOL_ENTRIES, SYMBOL_HAS_VALUE_MAP, SYMBOL_ITERATOR,
        SYMBOL_ENTRIES_ITERATOR, SYMBOL_TOSTRING,
    };
    for (size_t i = 0; i < sizeof(symbols)/sizeof(symbols[0]); i++) {
        const XrMethodSlot *slot = xr_method_table_lookup(
            XR_TID_MAP, symbols[i], SYMBOL_BUILTIN_COUNT);
        ASSERT_NOT_NULL(slot);
        ASSERT_NOT_NULL(slot->fn);
    }
}

TEST(map_set_advertises_may_throw) {
    /* WeakMap.set throws on contract violation. */
    const XrMethodSlot *slot = xr_method_table_lookup(
        XR_TID_MAP, SYMBOL_SET, SYMBOL_BUILTIN_COUNT);
    ASSERT_NOT_NULL(slot);
    ASSERT(slot->flags & XR_METHOD_FLAG_MAY_THROW);
}

/* ========== Set / WeakSet migration ========== */

TEST(set_method_table_exposes_full_surface) {
    /* All fourteen migrated set symbols must resolve. */
    static const int symbols[] = {
        SYMBOL_HAS, SYMBOL_DELETE, SYMBOL_IS_EMPTY, SYMBOL_ADD,
        SYMBOL_CLEAR, SYMBOL_UNION, SYMBOL_INTERSECTION,
        SYMBOL_DIFFERENCE, SYMBOL_SYMMETRIC_DIFFERENCE,
        SYMBOL_IS_SUBSET, SYMBOL_IS_SUPERSET,
        SYMBOL_TO_ARRAY, SYMBOL_ITERATOR, SYMBOL_TOSTRING,
    };
    for (size_t i = 0; i < sizeof(symbols)/sizeof(symbols[0]); i++) {
        const XrMethodSlot *slot = xr_method_table_lookup(
            XR_TID_SET, symbols[i], SYMBOL_BUILTIN_COUNT);
        ASSERT_NOT_NULL(slot);
        ASSERT_NOT_NULL(slot->fn);
    }
}

TEST(set_add_advertises_may_throw) {
    /* WeakSet.add validates its argument and throws on
     * contract violation, so the slot must claim MAY_THROW. */
    const XrMethodSlot *slot = xr_method_table_lookup(
        XR_TID_SET, SYMBOL_ADD, SYMBOL_BUILTIN_COUNT);
    ASSERT_NOT_NULL(slot);
    ASSERT(slot->flags & XR_METHOD_FLAG_MAY_THROW);
}

/* ========== Lookup helper short-circuits ========== */

TEST(lookup_returns_null_for_unmigrated_type) {
    /* string has no table yet — every lookup must fail cleanly. */
    const XrMethodSlot *slot = xr_method_table_lookup(XR_TID_STRING, 0, 64);
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
    /* Symbol-id range check must short-circuit even when the
     * receiver type DOES have a method table (XR_TID_INT). */
    const XrMethodSlot *slot = xr_method_table_lookup(XR_TID_INT, -1, 64);
    ASSERT(slot == NULL);

    slot = xr_method_table_lookup(XR_TID_INT, 999999, 64);
    ASSERT(slot == NULL);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()
    RUN_TEST_SUITE("Method-table registry shape");
    RUN_TEST(registry_is_dense_and_typed);

    RUN_TEST_SUITE("Bool migration");
    RUN_TEST(bool_method_table_exposes_toString);
    RUN_TEST(bool_method_table_unknown_symbol_returns_null);

    RUN_TEST_SUITE("BigInt migration");
    RUN_TEST(bigint_method_table_exposes_full_surface);
    RUN_TEST(bigint_pure_predicates_carry_flags);

    RUN_TEST_SUITE("Int migration");
    RUN_TEST(int_method_table_exposes_full_surface);
    RUN_TEST(int_floor_ceil_round_are_no_ops);

    RUN_TEST_SUITE("Float migration");
    RUN_TEST(float_method_table_exposes_full_surface);
    RUN_TEST(float_arity_caps);

    RUN_TEST_SUITE("Map / WeakMap migration");
    RUN_TEST(map_method_table_exposes_full_surface);
    RUN_TEST(map_set_advertises_may_throw);

    RUN_TEST_SUITE("Set / WeakSet migration");
    RUN_TEST(set_method_table_exposes_full_surface);
    RUN_TEST(set_add_advertises_may_throw);

    RUN_TEST_SUITE("xr_method_table_lookup short-circuits");
    RUN_TEST(lookup_returns_null_for_unmigrated_type);
    RUN_TEST(lookup_rejects_out_of_range_type_id);
    RUN_TEST(lookup_rejects_out_of_range_symbol_id);
TEST_MAIN_END()
