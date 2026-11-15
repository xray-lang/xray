/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * register_set_cases.h - Independent Set row admission and result retirement
 *
 * KEY CONCEPT:
 *   The shared managed directory carries an exact Set kind. Other family
 *   tags and raw scalar bits cannot establish a Set owner or binding.
 */
#ifndef REGISTER_SET_CASES_H
#define REGISTER_SET_CASES_H
static XrRbcArrayAccess raw_set_op(XrRbcArrayOperation operation, uint32_t result, uint32_t count) {
    return (XrRbcArrayAccess){.operation=operation, .kind=XR_RBC_SET_I64, .result={result, false}, .count=count};
}
static void raw_set_cow(RawArray *f) {
    raw_array_init(f);
    const XrRbcRegisterKind kinds[] = {XR_RBC_BITS, XR_RBC_SET_I64, XR_RBC_SET_I64_CELL,
        XR_RBC_SET_I64, XR_RBC_SET_I64, XR_RBC_BITS, XR_RBC_BITS};
    memcpy(f->kinds, kinds, sizeof(kinds));
    f->cells[0] = XR_RBC_SET_I64;
    immediate(f->code, f->function.code_count++, XR_RBC_LOADI, 0, 7);
    XrRbcArrayLocation args[] = {{1, false}, {0, false}};
    raw_array_access(f, raw_set_op(XR_RBC_ARRAY_NEW, 1, 0), NULL);
    raw_array_access(f, raw_set_op(XR_RBC_ARRAY_CELL_NEW, 2, 1), args);
    raw_array_access(f, raw_set_op(XR_RBC_ARRAY_RETAIN, 3, 1), args);
    raw_array_access(f, raw_set_op(XR_RBC_ARRAY_DROP, 0, 1), args);
    args[0].index = 2;
    raw_array_access(f, raw_set_op(XR_RBC_SET_ADD, 0, 2), args);
    raw_array_access(f, raw_set_op(XR_RBC_SET_ADD, 0, 2), args);
    raw_array_access(f, raw_set_op(XR_RBC_ARRAY_CELL_READ, 4, 1), args);
    args[0].index = 4;
    raw_array_access(f, raw_set_op(XR_RBC_ARRAY_LEN, 5, 1), args);
    args[0].index = 3;
    raw_array_access(f, raw_set_op(XR_RBC_SET_CONTAINS, 6, 2), args);
    raw_array_access(f, raw_set_op(XR_RBC_ARRAY_DROP, 0, 1), args);
    args[0].index = 4;
    raw_array_access(f, raw_set_op(XR_RBC_ARRAY_DROP, 0, 1), args);
    args[0].index = 2;
    raw_array_access(f, raw_set_op(XR_RBC_ARRAY_CELL_DROP, 0, 1), args);
    emit(f->code, f->function.code_count++, XR_RBC_ADD, 5, 5, 6);
    emit(f->code, f->function.code_count++, XR_RBC_RET, 5, 0, 0);
}
static void test_raw_sets(void) {
    RawArray f;
    raw_set_cow(&f);
    uint64_t result = 99;
    CHECK(execute(&f.module, 0, NULL, 0, &result) == XR_RBC_OK && result == 1);
    f.accesses[0].kind = XR_RBC_MAP_STRING_I64;
    reject_prepare(&f.module, 0, NULL, 0);
    f.accesses[0].kind = XR_RBC_SET_I64;
    f.accesses[4].operation = XR_RBC_MAP_REMOVE;
    reject_prepare(&f.module, 0, NULL, 0);
    f.accesses[4].operation = XR_RBC_SET_ADD;
    f.kinds[2] = XR_RBC_MAP_STRING_I64_CELL;
    reject_prepare(&f.module, 0, NULL, 0);
    f.kinds[2] = XR_RBC_SET_I64_CELL;
    f.kinds[1] = XR_RBC_BITS;
    reject_prepare(&f.module, 0, NULL, 0);
    f.kinds[1] = XR_RBC_SET_I64;
    f.accesses[11].operation = XR_RBC_MAP_CLEAR;
    reject_prepare(&f.module, 0, NULL, 0);
}
static void test_raw_set_result(void) {
    RawArray f;
    raw_array_init(&f);
    f.function.register_count = f.function.register_kind_count = 2;
    f.kinds[0] = f.kinds[1] = XR_RBC_SET_I64;
    f.function.cell_count = f.function.cell_kind_count = 0;
    f.function.cell_kinds = NULL;
    f.function.result_kind = XR_RBC_SET_I64;
    raw_array_access(&f, raw_set_op(XR_RBC_ARRAY_NEW, 0, 0), NULL);
    XrRbcArrayLocation argument = {0, false};
    raw_array_access(&f, raw_set_op(XR_RBC_ARRAY_RETAIN, 1, 1), &argument);
    raw_array_access(&f, raw_set_op(XR_RBC_ARRAY_DROP, 0, 1), &argument);
    emit(f.code, f.function.code_count++, XR_RBC_RET, 1, 0, 0);
    for (unsigned n = 0; n < 16; ++n) {
        XrtLocationOwner *owner = NULL;
        CHECK(xrt_locations_create(NULL, 0, &owner) == XRT_LOCATION_OK);
        XrRbcExecution *execution = NULL;
        CHECK(xr_rbc_module_prepare(&f.module, 0, NULL, 0, owner, &execution) == XR_RBC_OK);
        XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
        CHECK(outcome && xr_rbc_outcome_status(outcome) == XR_RBC_OK);
        uint64_t bits = 91;
        CHECK(!xr_rbc_outcome_bits(outcome, &bits) && bits == 91);
        xr_rbc_execution_free(&execution);
        xr_rbc_outcome_free(&outcome);
        xrt_locations_release(owner);
    }
    f.function.parameter_count = 1;
    f.function.result_kind = XR_RBC_BITS;
    f.function.array_accesses = NULL;
    f.function.array_access_count = 0;
    f.function.array_operands = NULL;
    f.function.array_operand_count = 0;
    f.function.code_count = 1;
    emit(f.code, 0, XR_RBC_RET0, 0, 0, 0);
    uint64_t fabricated = 1;
    reject_prepare(&f.module, 0, &fabricated, 1);
}
#endif // REGISTER_SET_CASES_H
