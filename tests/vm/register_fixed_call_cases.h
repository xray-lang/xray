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
