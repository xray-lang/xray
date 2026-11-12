/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * register_state_cases.h - Raw capability spill provenance and shape checks
 */
#ifndef REGISTER_STATE_CASES_H
#define REGISTER_STATE_CASES_H

typedef struct RawStateTransport {
    XrtModuleLayout *layout;
    XrRbcModule module;
    XrRbcFunction function;
    XrRbcRegisterKind kinds[7];
    XrRbcStateAccess accesses[8];
    uint32_t code[28];
} RawStateTransport;

static void test_raw_state_deferred_publication(void) {
    XrtModuleTypeSpec type = {XRT_MODULE_I64, 0};
    XrtModuleSpec owner = {0};
    XrtModuleSlotSpec slot = {0, 0, XRT_MODULE_SLOT_MUTABLE, 0, 8, 8};
    XrtModuleLayoutSpec layout = {.types=&type, .type_count=1, .modules=&owner,
        .module_count=1, .slots=&slot, .slot_count=1, .payload_size=8,
        .payload_alignment=8, .byte_limit=UINT64_MAX};
    XrtModuleLayout *storage = NULL;
    CHECK(xrt_module_layout_new(&layout, &storage) == 0);
    uint32_t code[7], ids[] = {1, 1, 1, 1, 1, 2, 2};
    XrRbcStateAccess accesses[] = {{0}, {0, 0, 1}};
    XrRbcRegisterKind kinds[] = {XR_RBC_STATE, XR_RBC_BITS, XR_RBC_STATE_REF, XR_RBC_STATE_REF};
    XrRbcFunction function = {.code=code, .code_count=7, .register_count=4,
        .parameter_count=1, .register_kinds=kinds, .register_kind_count=4,
        .state_accesses=accesses, .state_access_count=2, .source_ids=ids, .source_id_count=7};
    XrRbcModule module = {.functions=&function, .function_count=1, .state_layout=storage};
    immediate(code, 0, XR_RBC_MODULE_BEGIN, 0, 0);
    immediate(code, 1, XR_RBC_STATE_REFERENCE, 2, 0);
    immediate(code, 2, XR_RBC_LOADI, 1, 37);
    immediate(code, 3, XR_RBC_STATE_CONSTRUCT, 3, 1);
    emit(code, 4, XR_RBC_STATE_PUBLISH, 3, 0, 0);
    emit(code, 5, XR_RBC_STATE_READ, 1, 2, XRT_MODULE_I64);
    emit(code, 6, XR_RBC_RET, 1, 0, 0);
    XrtSourceLocation points[] = {{.path="reference.xr", .path_length=12, .line=3, .column=1},
        {.path="actual-access.xr", .path_length=16, .line=37, .column=9}};
    XrtLocationOwner *locations = NULL;
    CHECK(xrt_locations_create(points, 2, &locations) == XRT_LOCATION_OK);
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_module_prepare(&module, 0, NULL, 0, locations, &execution) == XR_RBC_OK);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    uint64_t result = 0;
    CHECK(xr_rbc_outcome_bits(outcome, &result) && result == 37);
    xr_rbc_outcome_free(&outcome); xr_rbc_execution_free(&execution);
    for (unsigned write = 0; write < 2; ++write) {
        immediate(code, 0, XR_RBC_STATE_REFERENCE, 2, 0);
        emit(code, 1, write ? XR_RBC_STATE_WRITE : XR_RBC_STATE_READ,
            write ? 2u : 1u, write ? 1u : 2u, XRT_MODULE_I64);
        emit(code, 2, XR_RBC_RET, 1, 0, 0);
        ids[1] = ids[2] = 2;
        function.code_count = function.source_id_count = 3;
        CHECK(xr_rbc_module_prepare(&module, 0, NULL, 0, locations, &execution) == XR_RBC_OK);
        outcome = xr_rbc_execution_run(execution);
        XrXirValueStatus status = XR_XIR_VALUE_OK;
        uint32_t source = 0;
        const XrtLocationOwner *borrowed = NULL;
        CHECK(xr_rbc_outcome_value_failure(outcome, &status, &source, &borrowed));
        CHECK(status == XR_XIR_VALUE_BAD_ARGUMENT && source == 2 && borrowed == locations);
        xr_rbc_outcome_free(&outcome); xr_rbc_execution_free(&execution);
    }
    xrt_locations_release(locations); xrt_module_layout_drop(&storage);
}

static void raw_state_transport(RawStateTransport *f) {
    memset(f, 0, sizeof(*f));
    XrtModuleTypeSpec types[] = {{XRT_MODULE_I64, 0}, {XRT_MODULE_BOOL, 0}, {XRT_MODULE_STRING, 0}};
    XrtModuleSpec owner = {0};
    XrtModuleSlotSpec slots[] = {
        {0, 0, XRT_MODULE_SLOT_MUTABLE, 0, 8, 8},
        {0, 1, XRT_MODULE_SLOT_MUTABLE, 8, 1, 1},
        {0, 0, 0, 16, 8, 8},
        {0, 2, XRT_MODULE_SLOT_MUTABLE, 24, sizeof(void *), _Alignof(void *)}
    };
    XrtModuleLayoutSpec layout = {.types=types, .type_count=3, .modules=&owner,
        .module_count=1, .slots=slots, .slot_count=4, .payload_size=32,
        .payload_alignment=8, .byte_limit=UINT64_MAX};
    CHECK(xrt_module_layout_new(&layout, &f->layout) == 0);
    const XrRbcRegisterKind kinds[] = {XR_RBC_BITS, XR_RBC_STATE, XR_RBC_BITS,
        XR_RBC_STATE_REF, XR_RBC_STATE_REF, XR_RBC_BITS, XR_RBC_STRING};
    memcpy(f->kinds, kinds, sizeof(kinds));
    const XrRbcStateAccess accesses[] = {
        {0, 1, 2}, {1, 1, 2}, {2, 0, 2},
        {0, 1, 0}, {1, 1, 0}, {2, 1, 0}, {3, 1, 0}, {3, 1, 6}
    };
    memcpy(f->accesses, accesses, sizeof(accesses));
    f->function = (XrRbcFunction){.code=f->code, .code_count=28, .register_count=7,
        .parameter_count=2, .register_kinds=f->kinds, .register_kind_count=7,
        .state_accesses=f->accesses, .state_access_count=8, .spill_count=1};
    f->module = (XrRbcModule){.functions=&f->function, .function_count=1,
        .state_layout=f->layout};
    static const XrRbcLiteral literal = {"owned", 5};
    f->module.literals = &literal;
    f->module.literal_count = 1;
    immediate(f->code, 0, XR_RBC_MODULE_BEGIN, 1, 0);
    immediate(f->code, 1, XR_RBC_LOADI, 2, 11);
    immediate(f->code, 2, XR_RBC_STATE_CONSTRUCT, 3, 0);
    emit(f->code, 3, XR_RBC_STATE_PUBLISH, 3, 0, 0);
    immediate(f->code, 4, XR_RBC_LOADI, 2, 1);
    immediate(f->code, 5, XR_RBC_STATE_CONSTRUCT, 3, 1);
    emit(f->code, 6, XR_RBC_STATE_PUBLISH, 3, 0, 0);
    immediate(f->code, 7, XR_RBC_LOADI, 2, 33);
    immediate(f->code, 8, XR_RBC_STATE_INITIALIZE, 1, 2);
    immediate(f->code, 9, XR_RBC_STRING_LITERAL, 6, 0);
    immediate(f->code, 10, XR_RBC_STATE_CONSTRUCT, 3, 7);
    emit(f->code, 11, XR_RBC_STATE_PUBLISH, 3, 0, 0);
    emit(f->code, 12, XR_RBC_STRING_DROP, 6, 0, 0);
    immediate(f->code, 13, XR_RBC_MODULE_READY, 1, 0);
    immediate(f->code, 14, XR_RBC_LOADI, 2, 0);
    emit(f->code, 15, XR_RBC_EQ, 0, 2, 0);
    CHECK(xr_rbc_jump(f->code + 16, 16, 21, 28));
    immediate(f->code, 17, XR_RBC_STATE_REFERENCE, 3, 3);
    immediate(f->code, 18, XR_RBC_SPILL, 3, 0);
    immediate(f->code, 19, XR_RBC_STATE_REFERENCE, 3, 4);
    CHECK(xr_rbc_jump(f->code + 20, 20, 25, 28));
    immediate(f->code, 21, XR_RBC_STATE_REFERENCE, 3, 3);
    immediate(f->code, 22, XR_RBC_SPILL, 3, 0);
    immediate(f->code, 23, XR_RBC_STATE_REFERENCE, 3, 4);
    CHECK(xr_rbc_jump(f->code + 24, 24, 25, 28));
    immediate(f->code, 25, XR_RBC_RELOAD, 4, 0);
    emit(f->code, 26, XR_RBC_STATE_READ, 5, 4, XRT_MODULE_I64);
    emit(f->code, 27, XR_RBC_RET, 5, 0, 0);
}

static void test_raw_state_spill_transport(void) {
    RawStateTransport f;
    raw_state_transport(&f);
    for (uint64_t choice = 0; choice < 2; ++choice) {
        uint64_t result = 0;
        CHECK(execute(&f.module, 0, &choice, 1, &result) == XR_RBC_OK && result == 11);
    }
    uint64_t choice = 0;
    uint32_t saved = f.code[22];
    immediate(f.code, 22, XR_RBC_LOADI, 2, 0);
    reject_prepare(&f.module, 0, &choice, 1);
    /* A fresh producer restores the guarantee after the missing spill. */
    immediate(f.code, 25, XR_RBC_STATE_REFERENCE, 4, 3);
    uint64_t result = 0;
    CHECK(execute(&f.module, 0, &choice, 1, &result) == XR_RBC_OK && result == 11);
    f.code[22] = saved;
    immediate(f.code, 25, XR_RBC_RELOAD, 4, 0);
    immediate(f.code, 21, XR_RBC_STATE_REFERENCE, 3, 4);
    reject_prepare(&f.module, 0, &choice, 1);
    immediate(f.code, 21, XR_RBC_STATE_REFERENCE, 3, 3);
    emit(f.code, 26, XR_RBC_STATE_READ, 5, 4, XRT_MODULE_BOOL);
    reject_prepare(&f.module, 0, &choice, 1);
    emit(f.code, 26, XR_RBC_STATE_READ, 5, 4, XRT_MODULE_I64);
    immediate(f.code, 25, XR_RBC_LOADI, 4, 1);
    reject_prepare(&f.module, 0, &choice, 1);
    immediate(f.code, 25, XR_RBC_RELOAD, 4, 0);
    f.kinds[4] = XR_RBC_CALL_WINDOW;
    reject_prepare(&f.module, 0, &choice, 1);
    f.kinds[4] = XR_RBC_STATE_REF;
    f.function.reference_spill_count = 1;
    reject_prepare(&f.module, 0, &choice, 1);
    xrt_module_layout_drop(&f.layout);
}

static void test_raw_state_spill_shapes(void) {
    RawStateTransport f;
    raw_state_transport(&f);
    uint64_t choice = 0, result = 0;
    immediate(f.code, 17, XR_RBC_STATE_REFERENCE, 3, 5);
    immediate(f.code, 21, XR_RBC_STATE_REFERENCE, 3, 5);
    CHECK(execute(&f.module, 0, &choice, 1, &result) == XR_RBC_OK && result == 33);
    emit(f.code, 26, XR_RBC_STATE_WRITE, 4, 2, XRT_MODULE_I64);
    reject_prepare(&f.module, 0, &choice, 1);
    emit(f.code, 26, XR_RBC_STATE_PUBLISH, 4, 0, 0);
    reject_prepare(&f.module, 0, &choice, 1);
    emit(f.code, 26, XR_RBC_STATE_READ, 5, 4, XRT_MODULE_I64);
    immediate(f.code, 21, XR_RBC_STATE_REFERENCE, 3, 3);
    reject_prepare(&f.module, 0, &choice, 1);
    immediate(f.code, 17, XR_RBC_STATE_REFERENCE, 3, 6);
    immediate(f.code, 21, XR_RBC_STATE_REFERENCE, 3, 6);
    reject_prepare(&f.module, 0, &choice, 1);
    xrt_module_layout_drop(&f.layout);
}

static void test_raw_state_bool_bits(void) {
    RawStateTransport f;
    raw_state_transport(&f);
    const int32_t inputs[] = {0, 1, 2, 256, 257, -1};
    for (size_t i = 0; i < sizeof(inputs) / sizeof(*inputs); ++i) {
        immediate(f.code, 4, XR_RBC_LOADI, 2, inputs[i]);
        uint64_t choice = 0, result = 0;
        XrRbcStatus status = execute(&f.module, 0, &choice, 1, &result);
        CHECK(status == (inputs[i] <= 1 && inputs[i] >= 0 ? XR_RBC_OK : XR_RBC_INVALID));
    }
    xrt_module_layout_drop(&f.layout);
}

static void test_raw_state_fused_read_entries(void) {
    RawStateTransport f;
    raw_state_transport(&f);
    f.function.code_count = 25;
    immediate(f.code, 14, XR_RBC_STATE_REFERENCE, 4, 3);
    immediate(f.code, 15, XR_RBC_EQI, 0, 0);
    CHECK(xr_rbc_jump(f.code + 16, 16, 20, 25));
    immediate(f.code, 17, XR_RBC_LOADI, 5, 33);
    emit(f.code, 18, XR_RBC_STATE_WRITE, 4, 5, XRT_MODULE_I64);
    immediate(f.code, 19, XR_RBC_STATE_REFERENCE, 4, 3);
    emit(f.code, 20, XR_RBC_STATE_READ, 5, 4, XRT_MODULE_I64);
    emit(f.code, 21, XR_RBC_STATE_READ, 2, 4, XRT_MODULE_I64);
    CHECK(xr_rbc_encode(f.code + 22, (XrRbcOperands){XR_RBC_MULI, 5, 5, 0, 100}));
    emit(f.code, 23, XR_RBC_ADD, 5, 5, 2);
    emit(f.code, 24, XR_RBC_RET, 5, 0, 0);
    /* One path enters READ directly with the original contents. The other
     * writes first and must keep the fused producer's Ref for the next READ. */
    for (uint64_t choice = 0; choice < 2; ++choice) {
        uint64_t result = 0;
        CHECK(execute(&f.module, 0, &choice, 1, &result) == XR_RBC_OK);
        CHECK(result == (choice ? 3333u : 1111u));
    }
    uint64_t choice = 0;
    f.function.code_count = 20;
    reject_prepare(&f.module, 0, &choice, 1);
    f.function.code_count = 21;
    reject_prepare(&f.module, 0, &choice, 1);
    xrt_module_layout_drop(&f.layout);
}

static void test_raw_fixed_state_frontier(void) {
    XrtModuleTypeSpec type = {XRT_MODULE_FIXED_U8, 17};
    XrtModuleSpec owner = {0};
    XrtModuleSlotSpec slot = {.size=17, .alignment=1};
    XrtModuleLayoutSpec spec = {.types=&type, .type_count=1, .modules=&owner,
        .module_count=1, .slots=&slot, .slot_count=1, .payload_size=17,
        .payload_alignment=1, .byte_limit=UINT64_MAX};
    XrtModuleLayout *layout = NULL;
    CHECK(xrt_module_layout_new(&spec, &layout) == 0);
    uint32_t code[2];
    immediate(code, 0, XR_RBC_LOADI, 1, 0);
    emit(code, 1, XR_RBC_RET, 1, 0, 0);
    XrRbcRegisterKind kinds[] = {XR_RBC_STATE, XR_RBC_BITS};
    XrRbcFunction function = {.code=code, .code_count=2, .register_count=2,
        .parameter_count=1, .register_kinds=kinds, .register_kind_count=2};
    XrRbcModule module = {.functions=&function, .function_count=1, .state_layout=layout};
    reject_prepare(&module, 0, NULL, 0);
    xrt_module_layout_drop(&layout);
}

#endif // REGISTER_STATE_CASES_H
