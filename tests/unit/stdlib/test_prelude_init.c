/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_prelude_init.c - Verifies the implicit language-core prelude is
 *                       installed into every full-runtime isolate and exposes
 *                       the stable symbol-table accessor used by the parser.
 */

#include "../test_framework.h"

#include "xray_vm.h"
#include "../../../src/runtime/xisolate_internal.h"
#include "../../../src/module/xprelude_runtime.h"
#include "../../../src/runtime/class/xenum.h"
#include "../../../src/runtime/core/xr_runtime_core.h"
#include "../../../src/runtime/value/xvalue.h"

/* ========== Helpers ========== */

static XrVMRuntime *make_full_isolate(void) {
    XrVMConfig params = {0};
    return xray_vm_new_full(&params);
}

/* ========== Tests ========== */

TEST(prelude_field_populated_after_full_init) {
    XrVMRuntime *iso = make_full_isolate();
    ASSERT_NOT_NULL(iso);

    /* Isolate initialization in xisolate_full.c::isolate_init_full() must have
     * wired the registry pointer. */
    ASSERT_NOT_NULL(iso->prelude_symbols);

    xray_vm_delete(iso);
}

TEST(prelude_get_symbols_accessor_returns_same_pointer) {
    XrVMRuntime *iso = make_full_isolate();
    ASSERT_NOT_NULL(iso);

    const XrPreludeSymbols *symbols = xr_prelude_get_symbols(iso);
    ASSERT_NOT_NULL(symbols);
    ASSERT_EQ_PTR(symbols, iso->prelude_symbols);

    xray_vm_delete(iso);
}

TEST(prelude_get_symbols_handles_null_isolate) {
    /* Defensive: accessor must not crash when given NULL. */
    ASSERT_NULL(xr_prelude_get_symbols(NULL));
}

TEST(prelude_table_skeleton_is_consistent) {
    XrVMRuntime *iso = make_full_isolate();
    ASSERT_NOT_NULL(iso);

    const XrPreludeSymbols *symbols = xr_prelude_get_symbols(iso);
    ASSERT_NOT_NULL(symbols);

    /* The table is currently empty (entries land in subsequent phases),
     * but the structure must already be self-consistent: a non-zero
     * count must come with a non-NULL types pointer. */
    if (symbols->type_count > 0) {
        ASSERT_NOT_NULL(symbols->types);
    }

    xray_vm_delete(iso);
}

TEST(prelude_lookup_unknown_returns_null) {
    XrVMRuntime *iso = make_full_isolate();
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

    xray_vm_delete(iso);
}

TEST(prelude_enum_slots_members_and_identity_are_stable) {
    static const struct {
        int slot;
        const char *name;
        uint32_t count;
        uint32_t payload_mask;
        const char *members[5];
    } expected[] = {
        {22, "Ordering", 5, 0, {"Relaxed", "Acquire", "Release", "AcquireRelease", "SeqCst"}},
        {23, "Endian", 3, 0, {"Native", "LE", "BE"}},
        {24, "Recv", 4, 1, {"Value", "Empty", "Timeout", "Closed"}},
        {25, "SendResult", 4, 0, {"Sent", "Full", "Timeout", "Closed"}},
        {26, "TaskResult", 5, 3, {"Success", "Failed", "Cancelled", "Timeout", "Pending"}},
        {27, "TaskStatus", 5, 0, {"Pending", "Running", "Success", "Failed", "Cancelled"}},
        {30, "NumberParseError", 2, 0, {"InvalidSyntax", "OutOfRange"}},
        {34, "Utf8Error", 1, 0, {"InvalidUtf8"}},
        {35, "StringSliceError", 1, 0, {"InvalidByteRange"}},
        {36, "CompressionError", 1, 0, {"InvalidData"}},
        {37, "CryptoError", 1, 0, {"InvalidLength"}},
    };
    XrVMRuntime *first = make_full_isolate();
    XrVMRuntime *second = make_full_isolate();
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        XrValue left = first->vm.builtins[expected[i].slot];
        XrValue right = second->vm.builtins[expected[i].slot];
        ASSERT(XR_IS_ENUM_TYPE(left) && XR_IS_ENUM_TYPE(right));
        XrEnumType *a = XR_TO_ENUM_TYPE(left);
        XrEnumType *b = XR_TO_ENUM_TYPE(right);
        ASSERT(a != b && xr_enum_type_same_nominal(a, b));
        ASSERT(strcmp(a->name, expected[i].name) == 0);
        ASSERT_EQ(a->member_count, expected[i].count);
        for (uint32_t j = 0; j < expected[i].count; j++) {
            ASSERT(strcmp(a->members[j].name, expected[i].members[j]) == 0);
            ASSERT_EQ(xr_enum_type_payload_count(a, j), (expected[i].payload_mask >> j) & 1u);
        }
        ASSERT_EQ_PTR(XR_TO_ENUM_TYPE(xr_runtime_core_builtin(first->core_rt, expected[i].slot)), a);
        if (expected[i].slot == 30)
            ASSERT_EQ(a->layout->layout_id, UINT32_C(3802613823));
        ASSERT(xr_prelude_install(first));
        ASSERT_EQ_PTR(XR_TO_ENUM_TYPE(first->vm.builtins[expected[i].slot]), a);
    }
    xray_vm_delete(second);
    xray_vm_delete(first);
}

/* ========== Entry point ========== */

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("prelude/init");
RUN_TEST(prelude_field_populated_after_full_init);
RUN_TEST(prelude_get_symbols_accessor_returns_same_pointer);
RUN_TEST(prelude_get_symbols_handles_null_isolate);
RUN_TEST(prelude_table_skeleton_is_consistent);
RUN_TEST(prelude_lookup_unknown_returns_null);
RUN_TEST(prelude_enum_slots_members_and_identity_are_stable);
TEST_MAIN_END()
