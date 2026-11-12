/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * register_pod_display_cases.h - Exact catalog identities precede value display
 */
static void test_raw_pod_display(void) {
    for (uint32_t scenario = 0; scenario < 9; ++scenario) {
        XrPodLayout scalar, nullable;
        CHECK(xr_pod_scalar_layout(XR_POD_I64, &scalar) && xr_pod_nullable_layout(scalar, &nullable));
        XrPodType types[] = {{XR_POD_NULLABLE, 0, XR_POD_I64, nullable},
            {XR_POD_NULLABLE, 0, XR_POD_U64, nullable}};
        XrRbcAggregateValue home = {0, 0};
        XrRbcAggregateAccess accesses[] = {{.operation=XR_RBC_NULLABLE_NONE},
            {.operation=XR_RBC_AGGREGATE_DISPLAY, .slot=XR_POD_SCALAR_COUNT, .result={0, false}}};
        XrRbcRegisterKind kinds[] = {XR_RBC_STRING, XR_RBC_BITS};
        uint32_t code[5];
        immediate(code, 0, XR_RBC_AGGREGATE_ACCESS, 0, 0);
        immediate(code, 1, XR_RBC_AGGREGATE_ACCESS, 0, 1);
        emit(code, 2, XR_RBC_STRING_LEN, 1, 0, 0);
        emit(code, 3, XR_RBC_STRING_DROP, 0, 0, 0);
        emit(code, 4, XR_RBC_RET, 1, 0, 0);
        XrRbcFunction function = {.code=code, .code_count=5, .register_count=2,
            .register_kinds=kinds, .register_kind_count=2, .aggregate_values=&home,
            .aggregate_value_count=1, .aggregate_size=nullable.size,
            .aggregate_accesses=accesses, .aggregate_access_count=2};
        XrRbcModule module = {.functions=&function, .function_count=1,
            .aggregate_types=types, .aggregate_type_count=2};
        if (scenario == 1) accesses[1].slot++;
        if (scenario == 2) accesses[1].source = 1;
        if (scenario == 3) accesses[1].count = 1;
        if (scenario == 4) kinds[0] = XR_RBC_BITS;
        if (scenario == 5) { function.code = code + 1; function.code_count = 4; }
        if (scenario == 6) accesses[1].destination = 1;
        if (scenario == 7) accesses[1].slot = UINT32_MAX;
        if (scenario == 8) {
            types[0] = xr_rbc_fixed_type(17);
            CHECK(xr_pod_nullable_layout(types[0].physical, &nullable));
            types[1] = (XrPodType){XR_POD_NULLABLE, 0, XR_POD_SCALAR_COUNT, nullable};
            home.type = 1;
            function.aggregate_size = nullable.size;
            accesses[1].slot++;
        }
        if (scenario) reject_prepare(&module, 0, NULL, 0);
        else {
            uint64_t result = 0;
            CHECK(execute(&module, 0, NULL, 0, &result) == XR_RBC_OK && result == 4);
        }
    }
}
