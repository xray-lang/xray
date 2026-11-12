/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * register_fixed_call_cases.h - Value returns survive later calls and retirement
 */
#include "register_fixed_call_fixture.h"

static void test_raw_fixed_calls(void) {
    const uint32_t counts[] = {1, 16, 17, 256, 65535};
    for (size_t n = 0; n < sizeof(counts) / sizeof(*counts); ++n) {
        RawFixedCalls f;
        fixed_call_fixture(&f, counts[n]);
        uint64_t result = 0;
        CHECK(execute(&f.module, 0, NULL, 0, &result) == XR_RBC_OK && result == 3737);
        uint32_t saved = f.root_code[1];
        f.root_code[1] = fixed_call_word(XR_RBC_CALL, 2, 0, 0, 1);
        reject_prepare(&f.module, 0, NULL, 0);
        f.root_code[1] = saved;
        f.root_accesses[0].destination = 2;
        reject_prepare(&f.module, 0, NULL, 0);
        f.root_accesses[0].destination = 0;
        f.argument = 2;
        reject_prepare(&f.module, 0, NULL, 0);
        f.argument = 0;
        reject_prepare(&f.module, 2, NULL, 0);
        XrtLocationOwner *locations = NULL;
        CHECK(xrt_locations_create(NULL, 0, &locations) == XRT_LOCATION_OK);
        XrRbcExecution *execution = NULL;
        uint64_t seed = 37;
        CHECK(xr_rbc_module_prepare(&f.module, 1, &seed, 1, locations, &execution) == XR_RBC_OK);
        XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
        xr_rbc_execution_free(&execution); xrt_locations_release(locations);
        xr_free(f.operands);
        memset(&f, 0, sizeof(f));
        XrRbcAggregateType type = {0};
        const uint8_t *bytes = NULL;
        CHECK(!xr_rbc_outcome_bits(outcome, &seed));
        CHECK(xr_rbc_outcome_aggregate(outcome, &type, &bytes) && type.kind == XR_RBC_FIXED_U8 && type.count == counts[n]);
        for (uint32_t p = 0; p < type.count; ++p) CHECK(bytes[p] == 37);
        xr_rbc_outcome_free(&outcome);
    }
}

static void test_raw_fixed_no_value_callee(void) {
    XrRbcAggregateType type = {XR_RBC_FIXED_U8, 17};
    XrRbcAggregateValue home = {0};
    XrRbcAggregateAccess call = {.operation=XR_RBC_AGGREGATE_CALL, .callee=1};
    XrRbcRegisterKind window = XR_RBC_CALL_WINDOW;
    uint32_t root[] = {fixed_call_word(XR_RBC_AGGREGATE_ACCESS, 0, 0, 0, 0),
        fixed_call_word(XR_RBC_RET0, 0, 0, 0, 0)};
    uint32_t callee[] = {fixed_call_word(XR_RBC_LOADI, 0, 0, 0, 1),
        fixed_call_word(XR_RBC_LOADI, 1, 0, 0, 0), fixed_call_word(XR_RBC_DIV, 0, 0, 1, 0),
        fixed_call_word(XR_RBC_JMP, 0, 0, 0, -1)};
    XrRbcFunction functions[] = {
        {.code=root, .code_count=2, .register_count=1, .register_kinds=&window, .register_kind_count=1,
            .aggregate_values=&home, .aggregate_value_count=1, .aggregate_size=17,
            .aggregate_accesses=&call, .aggregate_access_count=1},
        {.code=callee, .code_count=4, .register_count=2, .aggregate_result_type=1}
    };
    XrRbcModule module = {.functions=functions, .function_count=2, .aggregate_types=&type, .aggregate_type_count=1};
    uint64_t bits = 0;
    XrRbcStatus status = execute(&module, 0, NULL, 0, &bits);
    if (status != XR_RBC_ARITHMETIC_PANIC) fprintf(stderr, "fixed empty callee status %u\n", (unsigned)status);
    CHECK(status == XR_RBC_ARITHMETIC_PANIC);
}
