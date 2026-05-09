/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_mono.c - Unit tests for monomorphization infrastructure
 */

#include "../test_framework.h"
#include "../../../src/frontend/analyzer/xanalyzer_mono.h"
#include "../../../src/frontend/parser/xtype_ref.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"

/* ========== Name Mangling Tests ========== */

TEST(mono_type_tag_basic) {
    XrType int_t = { .kind = XR_KIND_INT };
    XrType float_t = { .kind = XR_KIND_FLOAT };
    XrType str_t = { .kind = XR_KIND_STRING };
    XrType bool_t = { .kind = XR_KIND_BOOL };
    XrType unknown_t = { .kind = XR_KIND_UNKNOWN };

    ASSERT_STR_EQ(xr_mono_type_tag(&int_t), "i64");
    ASSERT_STR_EQ(xr_mono_type_tag(&float_t), "f64");
    ASSERT_STR_EQ(xr_mono_type_tag(&str_t), "str");
    ASSERT_STR_EQ(xr_mono_type_tag(&bool_t), "bool");
    ASSERT_STR_EQ(xr_mono_type_tag(&unknown_t), "unknown");
    ASSERT_STR_EQ(xr_mono_type_tag(NULL), "unknown");
}

TEST(mono_mangle_single) {
    XrType int_t = { .kind = XR_KIND_INT };
    XrType *args[] = { &int_t };
    char *result = xr_mono_mangle("identity", args, 1);
    ASSERT_STR_EQ(result, "identity$i64");
    free(result);
}

TEST(mono_mangle_multi) {
    XrType int_t = { .kind = XR_KIND_INT };
    XrType str_t = { .kind = XR_KIND_STRING };
    XrType *args[] = { &int_t, &str_t };
    char *result = xr_mono_mangle("map", args, 2);
    ASSERT_STR_EQ(result, "map$i64_str");
    free(result);
}

TEST(mono_mangle_null_name) {
    char *result = xr_mono_mangle(NULL, NULL, 0);
    ASSERT(result != NULL);
    ASSERT_STR_EQ(result, "");
    free(result);
}

TEST(mono_mangle_zero_args) {
    char *result = xr_mono_mangle("foo", NULL, 0);
    ASSERT(result != NULL);
    ASSERT_STR_EQ(result, "foo");
    free(result);
}

/* ========== Type Substitution Tests ========== */

TEST(type_substitute_type_param) {
    XrType param_t = { .kind = XR_KIND_TYPE_PARAM };
    param_t.type_param.name = "T";
    param_t.type_param.id = 0;

    XrType int_t = { .kind = XR_KIND_INT };
    XrMonoTypeMap map[] = { { "T", &int_t } };

    XrType *result = xr_mono_type_substitute(&param_t, map, 1);
    ASSERT(result != NULL);
    ASSERT_EQ(result->kind, XR_KIND_INT);
}

TEST(type_substitute_no_match) {
    XrType param_t = { .kind = XR_KIND_TYPE_PARAM };
    param_t.type_param.name = "U";

    XrType int_t = { .kind = XR_KIND_INT };
    XrMonoTypeMap map[] = { { "T", &int_t } };

    XrType *result = xr_mono_type_substitute(&param_t, map, 1);
    // No match, returns original
    ASSERT(result == &param_t);
}

TEST(type_substitute_non_param) {
    XrType int_t = { .kind = XR_KIND_INT };
    XrType concrete = { .kind = XR_KIND_FLOAT };
    XrMonoTypeMap map[] = { { "T", &concrete } };

    XrType *result = xr_mono_type_substitute(&int_t, map, 1);
    // Non-param type is unchanged
    ASSERT(result == &int_t);
}

TEST(type_substitute_array_element) {
