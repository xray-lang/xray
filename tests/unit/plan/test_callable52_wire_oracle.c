/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_callable52_wire_oracle.c - Complete independently framed XSM cutover
 */
#include "../test_framework.h"
#include "callable52_independent_wire.h"
#include "base/xmalloc.h"
#include "ir/xi.h"
#include "ir/xi_module.h"
#include "plan/format/xr_xsm_schema.h"
#include "plan/semantic/xr_semantic_builder.h"
#include "runtime/value/xtype.h"

static XrType integer_type = {.kind = XR_KIND_INT, .id = 1, .frozen = true,
                               .scalar_rep = XR_NATIVE_I64};
static XrType boolean_type = {.kind = XR_KIND_BOOL, .id = 2, .frozen = true,
                               .scalar_rep = XR_SCALAR_REP_NONE};
static XrType string_type = {.kind = XR_KIND_STRING, .id = 3, .frozen = true,
                              .scalar_rep = XR_SCALAR_REP_NONE};

TEST(complete_current_wire_matches_independent_fields_and_old_payload_is_rejected) {
    XiFunc *function = xi_func_new("artifact_probe", &integer_type);
    ASSERT_NOT_NULL(function);
    XiBlock *entry = xi_block_new(function);
    ASSERT_NOT_NULL(entry);
    ASSERT_NOT_NULL(xi_const_bool(function, entry, true, &boolean_type));
    ASSERT_NOT_NULL(xi_const_str(function, entry, "owned-by-plan", &string_type));
    XiValue *result = xi_const_int(function, entry, 42, &integer_type);
    ASSERT_NOT_NULL(result);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    XiModule module = {
        .identity = "memory-module-v1:id=24:semantic-plan-fixture-v1",
        .path = "semantic-plan-fixture.xr", .name = "semantic_plan_fixture", .init = function};
    function->module = &module;
    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    ASSERT_TRUE(xr_semantic_plan_build(function, &plan, error, sizeof(error)));
    function->module = NULL;
    xi_func_free(function);
    ASSERT_NOT_NULL(plan);
    char hex[65];
    xr_fingerprint_hex(xr_semantic_plan_fingerprint(plan), hex);
    ASSERT_TRUE(strcmp(hex, "6f039e4401545acf6ec2b986417fd0936220d49c6a4466728f84514e8947b275") == 0);
    uint8_t *wire = NULL;
    size_t size = 0;
    ASSERT_TRUE(xr_xsm_encode(plan, &wire, &size, error, sizeof(error)));
    fprintf(stderr, "wire literal=%zu encoder=%zu\n", sizeof(k_callable52_wire), size);
    ASSERT_TRUE(size == sizeof(k_callable52_wire));
    ASSERT_TRUE(memcmp(wire, k_callable52_wire, size) == 0);
    XrSemanticPlan *decoded = NULL;
    ASSERT_TRUE(xr_xsm_decode(k_callable52_wire, sizeof(k_callable52_wire),
                               &decoded, error, sizeof(error)));
    xr_semantic_plan_free(decoded);
    decoded = NULL;
    ASSERT_TRUE(!xr_xsm_decode(k_callable51_wire, sizeof(k_callable51_wire),
                                &decoded, error, sizeof(error)));
    ASSERT_TRUE(decoded == NULL && strstr(error, "XR_ARTIFACT_2000") != NULL);
    xr_free(wire);
    xr_semantic_plan_free(plan);
}
TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Independent complete callable52 wire");
RUN_TEST(complete_current_wire_matches_independent_fields_and_old_payload_is_rejected);
TEST_MAIN_END()
