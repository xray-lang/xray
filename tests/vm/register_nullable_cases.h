/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * register_nullable_cases.h - Hostile type and operation metadata stays closed
 */
#include "register_nullable_fixture.h"

static void test_raw_nullable_boundaries(void) {
    XrtLocationOwner *locations = NULL;
    CHECK(xrt_locations_create(NULL, 0, &locations) == XRT_LOCATION_OK);
    for (uint32_t scenario = 0; scenario < 10; ++scenario) {
        RawNullableFixture f;
        raw_nullable_fixture(&f);
        if (scenario == 1) f.types[0].element = XR_POD_SCALAR_COUNT;
        if (scenario == 2) f.types[1].element = XR_POD_SCALAR_COUNT + 2;
        if (scenario == 3) f.types[1].physical.size++;
        if (scenario == 4) f.types[1].count = 1;
        if (scenario == 5) f.accesses[0].operation = XR_RBC_AGGREGATE_NEW;
        if (scenario == 6) f.accesses[1].source = 1;
        if (scenario == 7) f.accesses[0].result.index = 1;
        if (scenario == 8) { f.function.code = f.code + 1; f.function.code_count = 2; }
        if (scenario == 9) f.types[0].physical.size = UINT32_MAX;
        XrRbcExecution *execution = NULL;
        uint64_t seed = 7;
        XrRbcStatus status = xr_rbc_module_prepare(&f.module, 0, &seed, 1, locations, &execution);
        if (scenario) CHECK(status == XR_RBC_INVALID && !execution);
        else {
            CHECK(status == XR_RBC_OK && execution);
            XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
            xr_rbc_execution_free(&execution);
            memset(&f, 0xa5, sizeof(f));
            XrPodType type;
            const uint8_t *bytes = NULL;
            CHECK(xr_rbc_outcome_aggregate(outcome, &type, &bytes) && type.physical.size == 24);
            CHECK(bytes[0] == 1 && bytes[8] == 1);
            uint64_t result;
            memcpy(&result, bytes + 16, sizeof(result));
            CHECK(result == 7);
            CHECK(xr_rbc_outcome_aggregate_type(outcome, type.element - XR_POD_SCALAR_COUNT, &type) &&
                type.element == XR_POD_I64);
            xr_rbc_outcome_free(&outcome);
        }
    }
    xrt_locations_release(locations);
}
