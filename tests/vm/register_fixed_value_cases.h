/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * register_fixed_value_cases.h - Real lane and compact byte construction
 */
#include "base/xmalloc.h"

static uint32_t fixed_raw_word(XrRbcOp op, uint32_t a, uint32_t b, int64_t immediate) {
    uint32_t word = 0;
    CHECK(xr_rbc_encode(&word, (XrRbcOperands){.op = op, .a = a, .b = b, .immediate = immediate}));
    return word;
}

static void test_raw_fixed_values(uint32_t count, bool spills) {
    XrRbcAggregateType type = xr_rbc_fixed_type(count);
    XrRbcAggregateLayout layout = {0};
    CHECK(xr_rbc_aggregate_layout(type, &layout));
    XrRbcAggregateValue homes[] = {{0, 0}, {0, layout.size}};
    XrRbcArrayLocation *operands = xr_calloc((size_t)count + 1, sizeof(*operands));
    CHECK(operands);
    for (uint32_t n = 0; n < count; ++n) operands[n] = (XrRbcArrayLocation){0, spills};
    operands[count] = (XrRbcArrayLocation){1, spills};
    XrRbcAggregateAccess accesses[] = {
        {.operation = XR_RBC_AGGREGATE_NEW, .destination = 0, .count = count},
        {.operation = XR_RBC_AGGREGATE_COPY, .destination = 1, .source = 0},
        {.operation = XR_RBC_AGGREGATE_GET, .source = 1, .result = {spills ? 0u : 2u, spills},
            .first = count, .count = 1}
    };
    uint32_t code[12];
    size_t size = 0;
    code[size++] = fixed_raw_word(XR_RBC_LOADI, 0, 0, 37);
    if (spills) code[size++] = fixed_raw_word(XR_RBC_SPILL, 0, 0, 0);
    code[size++] = fixed_raw_word(XR_RBC_AGGREGATE_ACCESS, 0, 0, 0);
    code[size++] = fixed_raw_word(XR_RBC_AGGREGATE_ACCESS, 0, 0, 1);
    code[size++] = fixed_raw_word(XR_RBC_LOADI, 0, 0, 91);
    if (spills) code[size++] = fixed_raw_word(XR_RBC_SPILL, 0, 0, 0);
    code[size++] = fixed_raw_word(XR_RBC_AGGREGATE_ACCESS, 0, 0, 0);
    code[size++] = count <= INT16_MAX ? fixed_raw_word(XR_RBC_LOADI, 1, 0, count - 1) :
        fixed_raw_word(XR_RBC_LOADK, 1, 0, 0);
    if (spills) code[size++] = fixed_raw_word(XR_RBC_SPILL, 1, 0, 1);
    code[size++] = fixed_raw_word(XR_RBC_AGGREGATE_ACCESS, 0, 0, 2);
    if (spills) code[size++] = fixed_raw_word(XR_RBC_RELOAD, 2, 0, 0);
    code[size++] = fixed_raw_word(XR_RBC_RET, 2, 0, 0);
    CHECK(size <= sizeof(code) / sizeof(*code));
    uint64_t index = count - 1;
    XrRbcFunction function = {.code = code, .code_count = size, .register_count = 3,
        .constants = &index, .constant_count = 1, .spill_count = spills ? 2u : 0u,
        .aggregate_values = homes, .aggregate_value_count = 2, .aggregate_size = (size_t)layout.size * 2,
        .aggregate_accesses = accesses, .aggregate_access_count = 3,
        .aggregate_operands = operands, .aggregate_operand_count = (size_t)count + 1};
    XrRbcModule module = {.functions = &function, .function_count = 1,
        .aggregate_types = &type, .aggregate_type_count = 1};
    uint64_t bits = 0;
    CHECK(execute(&module, 0, NULL, 0, &bits) == XR_RBC_OK && bits == 37);
    xr_free(operands);
}

static void test_raw_fixed_failures(void) {
    XrRbcAggregateType type = xr_rbc_fixed_type(1);
    XrRbcAggregateValue home = {0, 0};
    XrRbcArrayLocation operands[] = {{0, false}, {1, false}};
    XrRbcAggregateAccess accesses[] = {
        {.operation = XR_RBC_AGGREGATE_NEW, .count = 1},
        {.operation = XR_RBC_AGGREGATE_GET, .result = {2, false}, .first = 1, .count = 1}
    };
    uint64_t constants[] = {1, 0};
    uint32_t code[] = {fixed_raw_word(XR_RBC_LOADK, 0, 0, 0),
        fixed_raw_word(XR_RBC_AGGREGATE_ACCESS, 0, 0, 0),
        fixed_raw_word(XR_RBC_LOADK, 1, 0, 1), fixed_raw_word(XR_RBC_AGGREGATE_ACCESS, 0, 0, 1), XR_RBC_RET0};
    uint32_t sources[] = {0, 1, 0, 1, 0};
    XrRbcFunction function = {.code = code, .code_count = 5, .register_count = 3,
        .constants = constants, .constant_count = 2, .source_ids = sources, .source_id_count = 5,
        .aggregate_values = &home, .aggregate_value_count = 1, .aggregate_size = 8,
        .aggregate_accesses = accesses, .aggregate_access_count = 2,
        .aggregate_operands = operands, .aggregate_operand_count = 2};
    XrRbcModule module = {.functions = &function, .function_count = 1,
        .aggregate_types = &type, .aggregate_type_count = 1};
    XrtSourceLocation point = {"fixed-get.xr", 12, 37, 4, 37, 12};
    XrtLocationOwner *locations = NULL;
    CHECK(xrt_locations_create(&point, 1, &locations) == XRT_LOCATION_OK);
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        constants[0] = scenario < 3 ? (scenario == 0 ? 256 : scenario == 1 ? 257 : UINT64_MAX) : 37;
        constants[1] = scenario == 3 ? UINT64_MAX : 1;
        XrRbcExecution *execution = NULL;
        CHECK(xr_rbc_module_prepare(&module, 0, NULL, 0, locations, &execution) == XR_RBC_OK);
        XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
        XrXirValueStatus status = XR_XIR_VALUE_OK;
        uint32_t source = 0;
        const XrtLocationOwner *owned = NULL;
        CHECK(xr_rbc_outcome_value_failure(outcome, &status, &source, &owned));
        CHECK(status == (scenario < 3 ? XR_XIR_VALUE_BAD_ARGUMENT : XR_XIR_VALUE_BOUNDS) && source == 1);
        CHECK(xrt_locations_get(owned, source)->line == 37 && xrt_locations_get(owned, source)->column == 4);
        xr_rbc_execution_free(&execution); xr_rbc_outcome_free(&outcome);
    }
    constants[0] = 37; constants[1] = 0;
    for (unsigned scenario = 0; scenario < 7; ++scenario) {
        XrRbcFunction changed = function;
        XrRbcAggregateAccess altered[2]; memcpy(altered, accesses, sizeof(altered));
        changed.aggregate_accesses = altered;
        XrRbcAggregateValue wrong_home = home;
        changed.aggregate_values = &wrong_home;
        uint32_t wrong_code[5]; memcpy(wrong_code, code, sizeof(wrong_code));
        changed.code = wrong_code;
        if (scenario == 0) changed.aggregate_size = 7;
        if (scenario == 1) wrong_home.type = 1;
        if (scenario == 2) wrong_home.offset = 1;
        if (scenario == 3) altered[0].count = 2;
        if (scenario == 4) altered[1].result.index = 3;
        if (scenario == 5) wrong_code[1] = fixed_raw_word(XR_RBC_LOADI, 0, 0, 37);
        if (scenario == 6) wrong_code[0] = fixed_raw_word(XR_RBC_JMP, 0, 0, 0);
        module.functions = &changed;
        XrRbcExecution *execution = NULL;
        CHECK(xr_rbc_module_prepare(&module, 0, NULL, 0, locations, &execution) == XR_RBC_INVALID && !execution);
    }
    xrt_locations_release(locations);
}

static void test_raw_fixed_selected_edges(void) {
    XrRbcAggregateType type = xr_rbc_fixed_type(17);
    XrRbcAggregateValue homes[] = {{0, 0}, {0, 17}, {0, 34}};
    XrRbcArrayLocation operands[18];
    for (uint32_t n = 0; n < 18; ++n) operands[n] = (XrRbcArrayLocation){1, false};
    XrRbcAggregateAccess accesses[] = {
        {.operation = XR_RBC_AGGREGATE_NEW, .destination = 0, .count = 17},
        {.operation = XR_RBC_AGGREGATE_NEW, .destination = 2, .count = 17},
        {.operation = XR_RBC_AGGREGATE_COPY, .destination = 1, .source = 0},
        {.operation = XR_RBC_AGGREGATE_COPY, .destination = 1, .source = 2},
        {.operation = XR_RBC_AGGREGATE_GET, .source = 1, .result = {2, false}, .first = 17, .count = 1}
    };
    uint32_t code[] = {fixed_raw_word(XR_RBC_LOADI, 1, 0, 13),
        fixed_raw_word(XR_RBC_AGGREGATE_ACCESS, 0, 0, 0), fixed_raw_word(XR_RBC_LOADI, 1, 0, 91),
        fixed_raw_word(XR_RBC_AGGREGATE_ACCESS, 0, 0, 1), fixed_raw_word(XR_RBC_EQI, 0, 0, 0), 0,
        fixed_raw_word(XR_RBC_AGGREGATE_ACCESS, 0, 0, 2), 0,
        fixed_raw_word(XR_RBC_AGGREGATE_ACCESS, 0, 0, 3), fixed_raw_word(XR_RBC_LOADI, 1, 0, 16),
        fixed_raw_word(XR_RBC_AGGREGATE_ACCESS, 0, 0, 4), fixed_raw_word(XR_RBC_RET, 2, 0, 0)};
    CHECK(xr_rbc_jump(&code[5], 5, 8, 12) && xr_rbc_jump(&code[7], 7, 9, 12));
    XrRbcFunction function = {.code = code, .code_count = 12, .register_count = 3, .parameter_count = 1,
        .aggregate_values = homes, .aggregate_value_count = 3, .aggregate_size = 51,
        .aggregate_accesses = accesses, .aggregate_access_count = 5,
        .aggregate_operands = operands, .aggregate_operand_count = 18};
    XrRbcModule module = {.functions = &function, .function_count = 1,
        .aggregate_types = &type, .aggregate_type_count = 1};
    for (uint64_t argument = 0; argument < 2; ++argument) {
        uint64_t bits = 0;
        CHECK(execute(&module, 0, &argument, 1, &bits) == XR_RBC_OK && bits == (argument ? 13u : 91u));
    }
    code[8] = fixed_raw_word(XR_RBC_LOADI, 1, 0, 91);
    XrtLocationOwner *locations = NULL;
    CHECK(xrt_locations_create(NULL, 0, &locations) == XRT_LOCATION_OK);
    XrRbcExecution *execution = NULL;
    uint64_t argument = 0;
    CHECK(xr_rbc_module_prepare(&module, 0, &argument, 1, locations, &execution) == XR_RBC_INVALID && !execution);
    xrt_locations_release(locations);
}

static void test_raw_fixed_callee_growth(void) {
    XrRbcAggregateType type = xr_rbc_fixed_type(256);
    XrRbcAggregateValue home = {0, 0};
    XrRbcArrayLocation operands[257] = {{0}};
    operands[256].index = 1;
    XrRbcAggregateAccess accesses[] = {
        {.operation = XR_RBC_AGGREGATE_NEW, .count = 256},
        {.operation = XR_RBC_AGGREGATE_GET, .result = {2, false}, .first = 256, .count = 1}
    };
    uint32_t caller[] = {fixed_raw_word(XR_RBC_LOADI, 0, 0, 37),
        fixed_raw_word(XR_RBC_AGGREGATE_ACCESS, 0, 0, 0), fixed_raw_word(XR_RBC_CALL, 2, 0, 1),
        fixed_raw_word(XR_RBC_LOADI, 1, 0, 255), fixed_raw_word(XR_RBC_AGGREGATE_ACCESS, 0, 0, 1),
        fixed_raw_word(XR_RBC_RET, 0, 0, 0)};
    uint32_t callee[] = {fixed_raw_word(XR_RBC_LOADI, 0, 0, 91),
        fixed_raw_word(XR_RBC_AGGREGATE_ACCESS, 0, 0, 0), fixed_raw_word(XR_RBC_LOADI, 1, 0, 255),
        fixed_raw_word(XR_RBC_AGGREGATE_ACCESS, 0, 0, 1), fixed_raw_word(XR_RBC_RET, 2, 0, 0)};
    XrRbcFunction functions[2] = {{.code = caller, .code_count = 6, .register_count = 3},
        {.code = callee, .code_count = 5, .register_count = 255}};
    for (uint32_t f = 0; f < 2; ++f) {
        functions[f].aggregate_values = &home; functions[f].aggregate_value_count = 1;
        functions[f].aggregate_size = 256; functions[f].aggregate_accesses = accesses;
        functions[f].aggregate_access_count = 2; functions[f].aggregate_operands = operands;
        functions[f].aggregate_operand_count = 257;
    }
    XrRbcAggregateAccess caller_accesses[2]; memcpy(caller_accesses, accesses, sizeof(accesses));
    caller_accesses[1].result.index = 0;
    XrRbcRegisterKind caller_kinds[] = {XR_RBC_BITS, XR_RBC_BITS, XR_RBC_CALL_WINDOW};
    functions[0].aggregate_accesses = caller_accesses;
    functions[0].register_kinds = caller_kinds; functions[0].register_kind_count = 3;
    XrRbcModule module = {.functions = functions, .function_count = 2,
        .aggregate_types = &type, .aggregate_type_count = 1};
    uint64_t bits = 0;
    CHECK(execute(&module, 0, NULL, 0, &bits) == XR_RBC_OK && bits == 37);
}

static void test_raw_fixed_aggregate_execution(void) {
    const uint32_t counts[] = {1, 11, 16, 17, 256, 65535};
    for (size_t n = 0; n < sizeof(counts) / sizeof(*counts); ++n) {
        test_raw_fixed_values(counts[n], false);
        test_raw_fixed_values(counts[n], true);
    }
    test_raw_fixed_failures();
    test_raw_fixed_selected_edges();
    test_raw_fixed_callee_growth();
}
