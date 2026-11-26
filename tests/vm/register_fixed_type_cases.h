/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * register_fixed_type_cases.h - Checked physical layouts without guest boxes
 */
#include "vm/register/xrbc_internal.h"

static void test_raw_aggregate_types(void) {
    uint32_t code = 0;
    CHECK(xr_rbc_encode(&code, (XrRbcOperands){.op = XR_RBC_RET0}));
    XrRbcFunction function = {.code = &code, .code_count = 1, .register_count = 1};
    XrPodType type = xr_rbc_fixed_type(1, XR_POD_U8);
    XrRbcModule module = {.functions = &function, .function_count = 1,
        .aggregate_types = &type, .aggregate_type_count = 1};
    const uint32_t counts[] = {1, 11, 16, 17, 256, 65535};
    const uint32_t elements[] = {XR_POD_U8, XR_POD_U32, XR_POD_U64};
    const uint32_t widths[] = {1, 4, 8};
    for (size_t e = 0; e < sizeof(elements) / sizeof(*elements); ++e)
        for (size_t i = 0; i < sizeof(counts) / sizeof(*counts); ++i) {
            type = xr_rbc_fixed_type(counts[i], elements[e]);
            XrRbcAggregateLayout layout = {0};
            CHECK(xr_rbc_aggregate_layout(type, &layout));
            bool lanes = type.count <= 16;
            CHECK(layout.size == (lanes ? type.count * 8u : type.count * widths[e]) &&
                layout.alignment == (lanes ? 8u : widths[e]) &&
                layout.scalar_slots == (lanes ? type.count : 0u));
            uint64_t result = 91;
            CHECK(execute(&module, 0, NULL, 0, &result) == XR_RBC_OK && result == 0);
        }
    XrtLocationOwner *locations = NULL;
    CHECK(xrt_locations_create(NULL, 0, &locations) == XRT_LOCATION_OK);
    for (unsigned scenario = 0; scenario < 9; ++scenario) {
        type = xr_rbc_fixed_type(11, XR_POD_U8);
        module.aggregate_types = &type;
        module.aggregate_type_count = 1;
        if (scenario == 0) type.count = 0;
        if (scenario == 1) type.count = 65536;
        if (scenario == 2) type.kind = (XrPodTypeKind)0;
        if (scenario == 3) type.kind = (XrPodTypeKind)UINT32_MAX;
        if (scenario == 4) module.aggregate_types = NULL;
        /* Only the admitted unsigned widths repeat; a unit, a constructed
         * ordinal or any other scalar width never describes fixed storage. */
        if (scenario == 5) type.element = XR_POD_UNIT;
        if (scenario == 6) type.element = XR_POD_SCALAR_COUNT;
        if (scenario == 7) type = xr_rbc_fixed_type(11, XR_POD_U16);
        if (scenario == 8) type = xr_rbc_fixed_type(11, XR_POD_I64);
        XrRbcExecution *execution = NULL;
        CHECK(xr_rbc_module_prepare(&module, 0, NULL, 0, locations, &execution) == XR_RBC_INVALID && !execution);
    }
    xrt_locations_release(locations);
}
