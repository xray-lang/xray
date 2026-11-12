/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_nullable_owner_cases.h - Result catalogs survive both producer owners
 */
#include "../lir/lir_nullable_fixture.h"

static void test_nullable_outcome_owner(void) {
    for (uint32_t leaf = 0; leaf < 2; ++leaf) {
        XrLirFunction root;
        XrLirModule *module = nullable_none_module(128, leaf ? XR_LIR_I64 : XR_LIR_UNIT, &root);
        XrRbcProgram *program = compile(module, root);
        xr_lir_module_destroy(module);
        XrRbcExecution *execution = NULL;
        CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
        XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
        xr_rbc_execution_free(&execution);
        xr_rbc_program_destroy(program);
        XrRbcAggregateType type;
        const uint8_t *bytes = NULL;
        CHECK(xr_rbc_outcome_aggregate(outcome, &type, &bytes));
        CHECK(type.kind == XR_RBC_NULLABLE && type.physical.size == (leaf ? 1032u : 128u));
        for (uint32_t n = 0; n < type.physical.size; ++n) CHECK(!bytes[n]);
        uint32_t depth = 1;
        while (type.element >= XR_POD_SCALAR_COUNT) {
            CHECK(xr_rbc_outcome_aggregate_type(outcome, type.element - XR_POD_SCALAR_COUNT, &type));
            ++depth;
        }
        CHECK(depth == 128 && type.element == (leaf ? XR_POD_I64 : XR_POD_UNIT));
        type.kind = XR_RBC_FIXED_U8;
        CHECK(!xr_rbc_outcome_aggregate_type(outcome, 128, &type) && type.kind == XR_RBC_FIXED_U8);
        xr_rbc_outcome_free(&outcome);
    }
}
