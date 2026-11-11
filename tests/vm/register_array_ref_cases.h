/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * register_array_ref_cases.h - Real reference provenance and indexed frame growth
 */
#ifndef REGISTER_ARRAY_REF_CASES_H
#define REGISTER_ARRAY_REF_CASES_H
static void raw_array_reference(RawArray *f, bool rebuild, bool refresh) {
    raw_array_init(f);
    f->kinds[7] = XR_RBC_ARRAY_I64_REF;
    f->function.register_count = f->function.register_kind_count = 8;
    immediate(f->code, f->function.code_count++, XR_RBC_LOADI, 0, 7);
    XrRbcArrayLocation args[] = {{0, false}, {0, false}};
    raw_array_access(f, raw_array_op(XR_RBC_ARRAY_NEW, 2, 1), args);
    args[0].index = 2;
    raw_array_access(f, raw_array_op(XR_RBC_ARRAY_CELL_NEW, 4, 1), args);
    args[0].index = 4;
    raw_array_access(f, raw_array_op(XR_RBC_ARRAY_BORROW, 7, 1), args);
    if (rebuild) {
        raw_array_access(f, raw_array_op(XR_RBC_ARRAY_CELL_DROP, 0, 1), args);
        args[0].index = 2;
        raw_array_access(f, raw_array_op(XR_RBC_ARRAY_CELL_NEW, 4, 1), args);
        if (refresh) {
            args[0].index = 4;
            raw_array_access(f, raw_array_op(XR_RBC_ARRAY_BORROW, 7, 1), args);
        }
    }
    args[0].index = 7;
    raw_array_access(f, raw_array_op(XR_RBC_ARRAY_REF_READ, 5, 1), args);
    args[0].index = 5;
    raw_array_access(f, raw_array_op(XR_RBC_ARRAY_LEN, 6, 1), args);
    raw_array_access(f, raw_array_op(XR_RBC_ARRAY_DROP, 0, 1), args);
    args[0].index = 4;
    raw_array_access(f, raw_array_op(XR_RBC_ARRAY_CELL_DROP, 0, 1), args);
    args[0].index = 2;
    raw_array_access(f, raw_array_op(XR_RBC_ARRAY_DROP, 0, 1), args);
    emit(f->code, f->function.code_count++, XR_RBC_RET, 6, 0, 0);
}
static void test_raw_array_reference_lifetime(void) {
    RawArray f;
    raw_array_reference(&f, false, false);
    uint64_t result = 0;
    CHECK(execute(&f.module, 0, NULL, 0, &result) == XR_RBC_OK && result == 1);
    uint32_t borrow = f.code[3];
    immediate(f.code, 3, XR_RBC_LOADI, 7, 1);
    reject_prepare(&f.module, 0, NULL, 0);
    f.code[3] = borrow;
    f.kinds[7] = XR_RBC_ARRAY_U64_REF;
    reject_prepare(&f.module, 0, NULL, 0);
    raw_array_reference(&f, true, false);
    reject_prepare(&f.module, 0, NULL, 0);
    raw_array_reference(&f, true, true);
    CHECK(execute(&f.module, 0, NULL, 0, &result) == XR_RBC_OK && result == 1);
    f.function.result_kind = XR_RBC_ARRAY_I64_REF;
    reject_prepare(&f.module, 0, NULL, 0);
    uint32_t code;
    emit(&code, 0, XR_RBC_RET0, 0, 0, 0);
    XrRbcRegisterKind kind = XR_RBC_ARRAY_I64_REF;
    XrRbcFunction entry = {.code=&code, .code_count=1, .register_count=1, .parameter_count=1,
        .register_kinds=&kind, .register_kind_count=1};
    XrRbcModule module = {.functions=&entry, .function_count=1};
    uint64_t forged = 3;
    reject_prepare(&module, 0, &forged, 1);
}
static void test_raw_array_reference_growth(void) {
    RawArray f;
    raw_array_init(&f);
    f.kinds[3] = XR_RBC_ARRAY_I64_REF;
    f.kinds[6] = f.kinds[7] = XR_RBC_CALL_WINDOW;
    f.function.register_count = f.function.register_kind_count = 8;
    immediate(f.code, f.function.code_count++, XR_RBC_LOADI, 0, 7);
    XrRbcArrayLocation args[] = {{0, false}};
    raw_array_access(&f, raw_array_op(XR_RBC_ARRAY_NEW, 2, 1), args);
    args[0].index = 2;
    raw_array_access(&f, raw_array_op(XR_RBC_ARRAY_CELL_NEW, 4, 1), args);
    args[0].index = 4;
    raw_array_access(&f, raw_array_op(XR_RBC_ARRAY_BORROW, 3, 1), args);
    emit(f.code, f.function.code_count++, XR_RBC_MOV, 7, 3, 0);
    immediate(f.code, f.function.code_count++, XR_RBC_CALL, 6, 1);
    emit(f.code, f.function.code_count++, XR_RBC_MOV, 1, 6, 0);
    raw_array_access(&f, raw_array_op(XR_RBC_ARRAY_CELL_DROP, 0, 1), args);
    args[0].index = 2;
    raw_array_access(&f, raw_array_op(XR_RBC_ARRAY_DROP, 0, 1), args);
    emit(f.code, f.function.code_count++, XR_RBC_RET, 1, 0, 0);
    XrRbcRegisterKind kinds[255] = {0}, cells[64] = {0};
    kinds[0] = XR_RBC_ARRAY_I64_REF; kinds[1] = XR_RBC_ARRAY_I64;
    XrRbcArrayAccess accesses[] = {
        {.operation=XR_RBC_ARRAY_REF_READ, .kind=XR_RBC_ARRAY_I64, .result={1, false}, .count=1},
        {.operation=XR_RBC_ARRAY_LEN, .kind=XR_RBC_ARRAY_I64, .result={2, false}, .first=1, .count=1},
        {.operation=XR_RBC_ARRAY_DROP, .kind=XR_RBC_ARRAY_I64, .first=2, .count=1}};
    XrRbcArrayLocation operands[] = {{0, false}, {1, false}, {1, false}};
    uint32_t code[4];
    for (uint32_t i = 0; i < 3; ++i) immediate(code, i, XR_RBC_ARRAY_ACCESS, 0, i);
    emit(code, 3, XR_RBC_RET, 2, 0, 0);
    XrRbcFunction functions[] = {f.function, {.code=code, .code_count=4, .register_count=255,
        .parameter_count=1, .register_kinds=kinds, .register_kind_count=255, .cell_count=64,
        .cell_kinds=cells, .cell_kind_count=64, .array_accesses=accesses, .array_access_count=3,
        .array_operands=operands, .array_operand_count=3}};
    f.module.functions = functions; f.module.function_count = 2;
    uint64_t result = 0;
    CHECK(execute(&f.module, 0, NULL, 0, &result) == XR_RBC_OK && result == 1);
    kinds[0] = XR_RBC_ARRAY_STRING_REF;
    reject_prepare(&f.module, 0, NULL, 0);
}
static void test_raw_array_reference_publication(void) {
    for (unsigned write = 0; write < 2; ++write) {
        RawArray f;
        raw_array_init(&f);
        f.kinds[0] = XR_RBC_STATE; f.kinds[4] = XR_RBC_STATE_REF; f.kinds[7] = XR_RBC_ARRAY_I64_REF;
        f.function.parameter_count = 1;
        f.function.register_count = f.function.register_kind_count = 8;
        f.function.cell_count = f.function.cell_kind_count = 0; f.function.cell_kinds = NULL;
        XrtModuleTypeSpec type = {XRT_MODULE_ARRAY_I64};
        XrtModuleSpec owner = {0};
        XrtModuleSlotSpec slot = {.flags=XRT_MODULE_SLOT_MUTABLE, .size=8, .alignment=8};
        XrtModuleLayoutSpec layout = {.types=&type, .type_count=1, .modules=&owner, .module_count=1,
            .slots=&slot, .slot_count=1, .payload_size=8, .payload_alignment=8, .byte_limit=UINT64_MAX};
        XrtModuleLayout *storage = NULL;
        CHECK(xrt_module_layout_new(&layout, &storage) == 0);
        f.module.state_layout = storage;
        XrRbcStateAccess state = {0};
        f.function.state_accesses = &state; f.function.state_access_count = 1;
        immediate(f.code, f.function.code_count++, XR_RBC_STATE_REFERENCE, 4, 0);
        XrRbcArrayLocation args[] = {{4, false}, {2, false}};
        raw_array_access(&f, raw_array_op(XR_RBC_ARRAY_BORROW, 7, 1), args);
        if (write) {
            args[0].index = 1;
            raw_array_access(&f, raw_array_op(XR_RBC_ARRAY_NEW, 2, 1), args);
        }
        uint32_t actual = (uint32_t)f.function.code_count;
        args[0].index = 7;
        raw_array_access(&f, raw_array_op(write ? XR_RBC_ARRAY_REF_WRITE : XR_RBC_ARRAY_REF_READ,
            write ? 0 : 2, write ? 2 : 1), args);
        args[0].index = 2;
        raw_array_access(&f, raw_array_op(XR_RBC_ARRAY_DROP, 0, 1), args);
        emit(f.code, f.function.code_count++, XR_RBC_RET0, 0, 0, 0);
        uint32_t ids[64] = {0}; ids[1] = 1; ids[actual] = 2;
        f.function.source_ids = ids; f.function.source_id_count = f.function.code_count;
        XrtSourceLocation points[] = {{.path="borrow.xr", .path_length=9, .line=3, .column=1},
            {.path="access.xr", .path_length=9, .line=7, .column=2}};
        XrtLocationOwner *locations = NULL;
        CHECK(xrt_locations_create(points, 2, &locations) == XRT_LOCATION_OK);
        XrRbcExecution *execution = NULL;
        CHECK(xr_rbc_module_prepare(&f.module, 0, NULL, 0, locations, &execution) == XR_RBC_OK);
        XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
        XrXirValueStatus status = XR_XIR_VALUE_OK;
        uint32_t source = 0;
        const XrtLocationOwner *borrowed = NULL;
        CHECK(xr_rbc_outcome_value_failure(outcome, &status, &source, &borrowed));
        CHECK(status == XR_XIR_VALUE_BAD_ARGUMENT && source == 2 && borrowed == locations);
        xr_rbc_outcome_free(&outcome); xr_rbc_execution_free(&execution); xrt_locations_release(locations);
        xrt_module_layout_drop(&storage);
        slot.flags = 0;
        CHECK(xrt_module_layout_new(&layout, &storage) == 0);
        f.module.state_layout = storage;
        f.function.source_ids = NULL; f.function.source_id_count = 0;
        reject_prepare(&f.module, 0, NULL, 0);
        xrt_module_layout_drop(&storage);
    }
}
static void test_raw_array_reference_byte_bits(void) {
    for (unsigned set = 0; set < 2; ++set) {
        RawArray f;
        raw_array_init(&f);
        raw_unsigned_kinds(&f, false);
        f.kinds[7] = XR_RBC_ARRAY_U8_REF;
        f.function.register_count = f.function.register_kind_count = 8;
        f.function.parameter_count = 1;
        immediate(f.code, f.function.code_count++, XR_RBC_LOADI, 1, 0);
        XrRbcArrayLocation args[] = {{1, false}, {0, false}, {0, false}};
        raw_unsigned_access(&f, raw_array_op(XR_RBC_ARRAY_NEW, 2, 1), args);
        args[0].index = 2;
        raw_unsigned_access(&f, raw_array_op(XR_RBC_ARRAY_CELL_NEW, 4, 1), args);
        raw_unsigned_access(&f, raw_array_op(XR_RBC_ARRAY_DROP, 0, 1), args);
        args[0].index = 4;
        raw_unsigned_access(&f, raw_array_op(XR_RBC_ARRAY_BORROW, 7, 1), args);
        args[0].index = 7; args[1].index = set ? 1 : 0;
        raw_unsigned_access(&f, raw_array_op(set ? XR_RBC_ARRAY_REF_SET : XR_RBC_ARRAY_REF_PUSH,
            0, set ? 3 : 2), args);
        raw_unsigned_access(&f, raw_array_op(XR_RBC_ARRAY_REF_READ, 5, 1), args);
        args[0].index = 5;
        raw_unsigned_access(&f, raw_array_op(XR_RBC_ARRAY_LEN, 6, 1), args);
        raw_unsigned_access(&f, raw_array_op(XR_RBC_ARRAY_DROP, 0, 1), args);
        args[0].index = 4;
        raw_unsigned_access(&f, raw_array_op(XR_RBC_ARRAY_CELL_DROP, 0, 1), args);
        emit(f.code, f.function.code_count++, XR_RBC_RET, 6, 0, 0);
        uint64_t value = 255, result = 0;
        CHECK(execute(&f.module, 0, &value, 1, &result) == XR_RBC_OK && result == (set ? 1u : 2u));
        const uint64_t invalid[] = {256, 257, UINT64_MAX};
        for (uint32_t i = 0; i < 3; ++i) {
            result = 91;
            CHECK(execute(&f.module, 0, &invalid[i], 1, &result) == XR_RBC_INVALID && result == 91);
        }
    }
}
#endif // REGISTER_ARRAY_REF_CASES_H
