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

static void raw_state_transport(RawStateTransport *f) {
    memset(f, 0, sizeof(*f));
    XrtModuleTypeSpec types[] = {{XRT_MODULE_I64}, {XRT_MODULE_BOOL}, {XRT_MODULE_STRING}};
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

#endif // REGISTER_STATE_CASES_H
