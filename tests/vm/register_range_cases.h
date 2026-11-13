/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static void test_raw_range_checks(void) {
    uint32_t code[] = {XR_RBC_AGGREGATE_ACCESS, XR_RBC_RET0}, sources[] = {1, 0};
    XrRbcArrayLocation operands[] = {{0, false}, {1, false}, {2, false}};
    XrRbcAggregateAccess access = {.operation=XR_RBC_RANGE_CHECK, .count=3};
    XrRbcFunction function = {.code=code, .code_count=2, .register_count=3, .parameter_count=3,
        .source_ids=sources, .source_id_count=2, .aggregate_accesses=&access, .aggregate_access_count=1,
        .aggregate_operands=operands, .aggregate_operand_count=3};
    XrRbcModule module = {.functions=&function, .function_count=1};
    XrtSourceLocation point = {"range.xr", 8, 27, 3, 27, 15};
    XrtLocationOwner *locations = NULL;
    CHECK(xrt_locations_create(&point, 1, &locations) == XRT_LOCATION_OK);
    const uint64_t arguments[][3] = {{0,0,0}, {0,3,3}, {3,3,3}, {1,2,3},
        {UINT64_MAX,2,3}, {2,1,3}, {0,4,3}, {0,0,UINT64_MAX}};
    for (unsigned n = 0; n < sizeof(arguments) / sizeof(*arguments); ++n) {
        XrRbcExecution *execution = NULL;
        CHECK(xr_rbc_module_prepare(&module, 0, arguments[n], 3, locations, &execution) == XR_RBC_OK);
        XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
        CHECK(outcome);
        if (n < 4) CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_OK);
        else {
            XrXirValueStatus status = XR_XIR_VALUE_OK;
            uint32_t source = 0;
            const XrtLocationOwner *owned = NULL;
            CHECK(xr_rbc_outcome_value_failure(outcome, &status, &source, &owned));
            CHECK(status == XR_XIR_VALUE_BOUNDS && source == 1 && xrt_locations_get(owned, source)->line == 27);
        }
        xr_rbc_execution_free(&execution); xr_rbc_outcome_free(&outcome);
    }
    for (unsigned n = 0; n < 6; ++n) {
        XrRbcAggregateAccess saved = access;
        if (n == 0) access.count = 2;
        if (n == 1) access.first = UINT32_MAX;
        if (n == 2) access.result.index = 1;
        if (n == 3) access.slot = 1;
        if (n == 4) operands[2].index = 3;
        if (n == 5) function.parameter_count = 0;
        XrRbcExecution *execution = NULL;
        CHECK(xr_rbc_module_prepare(&module, 0, arguments[0], function.parameter_count, locations, &execution) == XR_RBC_INVALID);
        CHECK(!execution);
        access = saved; operands[2].index = 2; function.parameter_count = 3;
    }
    xrt_locations_release(locations);
}
