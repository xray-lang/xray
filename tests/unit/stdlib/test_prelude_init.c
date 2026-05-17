/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_prelude_init.c - Verifies the prelude module is auto-loaded into
 *                       every full-runtime isolate and exposes a stable
 *                       symbol-table accessor used by the parser.
 */

#include "../test_framework.h"

#include "xray_isolate.h"
#include "../../../src/runtime/xisolate_internal.h"
#include "../../../stdlib/prelude/prelude.h"

/* ========== Helpers ========== */

static XrayIsolate *make_full_isolate(void) {
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    xray_isolate_setup_full(&params);
    return xray_isolate_new(&params);
}

/* ========== Tests ========== */

TEST(prelude_field_populated_after_full_init) {
    XrayIsolate *iso = make_full_isolate();
    ASSERT_NOT_NULL(iso);

    /* Auto-load path in xisolate_full.c::isolate_init_full() must have
     * wired the registry pointer. */
    ASSERT_NOT_NULL(iso->prelude_symbols);

    xray_isolate_delete(iso);
}

TEST(prelude_get_symbols_accessor_returns_same_pointer) {
    XrayIsolate *iso = make_full_isolate();
    ASSERT_NOT_NULL(iso);

    const XrPreludeSymbols *symbols = xr_prelude_get_symbols(iso);
    ASSERT_NOT_NULL(symbols);
    ASSERT_EQ_PTR(symbols, iso->prelude_symbols);

    xray_isolate_delete(iso);
}

TEST(prelude_get_symbols_handles_null_isolate) {
    /* Defensive: accessor must not crash when given NULL. */
    ASSERT_NULL(xr_prelude_get_symbols(NULL));
}

TEST(prelude_table_skeleton_is_consistent) {
    XrayIsolate *iso = make_full_isolate();
    ASSERT_NOT_NULL(iso);

    const XrPreludeSymbols *symbols = xr_prelude_get_symbols(iso);
    ASSERT_NOT_NULL(symbols);

    /* The table is currently empty (entries land in subsequent phases),
     * but the structure must already be self-consistent: a non-zero
     * count must come with a non-NULL types pointer. */
    if (symbols->type_count > 0) {
        ASSERT_NOT_NULL(symbols->types);
    }

    xray_isolate_delete(iso);
}

TEST(prelude_lookup_unknown_returns_null) {
    XrayIsolate *iso = make_full_isolate();
    ASSERT_NOT_NULL(iso);

    const XrPreludeSymbols *symbols = xr_prelude_get_symbols(iso);
    ASSERT_NOT_NULL(symbols);

    /* "Nonexistent" name must miss regardless of how many real entries
     * the table currently holds. */
    const char *needle = "DefinitelyNotAPreludeType";
    ASSERT_NULL(xr_prelude_lookup_type(symbols, needle, strlen(needle)));

    /* Defensive: NULL inputs return NULL without crashing. */
    ASSERT_NULL(xr_prelude_lookup_type(NULL, needle, strlen(needle)));
    ASSERT_NULL(xr_prelude_lookup_type(symbols, NULL, 0));

    xray_isolate_delete(iso);
}

/* ========== Entry point ========== */

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("prelude/init");
RUN_TEST(prelude_field_populated_after_full_init);
RUN_TEST(prelude_get_symbols_accessor_returns_same_pointer);
RUN_TEST(prelude_get_symbols_handles_null_isolate);
RUN_TEST(prelude_table_skeleton_is_consistent);
RUN_TEST(prelude_lookup_unknown_returns_null);
TEST_MAIN_END()
