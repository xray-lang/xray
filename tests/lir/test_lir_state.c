/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_state.c - Owned layouts and nonforgeable reference shapes
 */
#include "lir/xlir_internal.h"
#include "lir/xlir_rebuild.h"
#include "lir/xlir_inline.h"
#include "lir/xlir_sccp.h"
#include "lir/xlir_verify.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define OK(c) CHECK((c) == XR_LIR_OK)

typedef struct StateFixture {
    XrLirModule *module;
    XrLirFunction function;
    XrLirBlock entry;
    XrLirValue state, reference;
} StateFixture;

static StateFixture state_fixture(bool mutable) {
    StateFixture f = {0};
    OK(xr_lir_module_create(NULL, &f.module));
    XrLirStateModule modules[] = {{0, 0}, {0, 1}};
    uint32_t dependencies[] = {0};
    XrLirStateSlot slot = {.module=1, .payload=XR_LIR_I64, .mutable=mutable, .size=8, .alignment=8};
    XrLirStateLayout layout = {.modules=modules, .module_count=2, .dependencies=dependencies,
        .dependency_count=1, .slots=&slot, .slot_count=1, .payload_size=8, .payload_alignment=8};
    OK(xr_lir_set_state_layout(f.module, &layout));
    memset(modules, 0xff, sizeof(modules)); dependencies[0] = UINT32_MAX;
    memset(&slot, 0xff, sizeof(slot));
    XrLirType parameter = XR_LIR_STATE;
    XrLirFunctionSpec spec = {.name="root", .name_length=4, .parameters=&parameter,
        .parameter_count=1, .result=XR_LIR_I64};
    OK(xr_lir_declare_function(f.module, &spec, &f.function));
    OK(xr_lir_set_state_entry(f.module, f.function));
    OK(xr_lir_define_function(f.module, f.function));
    OK(xr_lir_function_parameter(f.module, f.function, 0, &f.state));
    OK(xr_lir_add_block(f.module, f.function, NULL, 0, &f.entry));
    OK(xr_lir_set_entry(f.module, f.function, f.entry));
    XrLirInstruction instruction = {.op=XR_LIR_STATE_REFERENCE, .type=XR_LIR_STATE_REF,
        .operands=&f.state, .operand_count=1};
    OK(xr_lir_emit_value(f.module, f.entry, &instruction, &f.reference));
    return f;
}

static void state_finish(StateFixture *f) {
    XrLirValue value;
    XrLirInstruction read = {.op=XR_LIR_STATE_READ, .type=XR_LIR_I64,
        .operands=&f->reference, .operand_count=1};
    OK(xr_lir_emit_value(f->module, f->entry, &read, &value));
    XrLirInstruction ret = {.op=XR_LIR_RET, .type=XR_LIR_UNIT, .operands=&value, .operand_count=1};
    OK(xr_lir_set_terminator(f->module, f->entry, &ret));
    OK(xr_lir_module_finish(f->module, NULL));
}

static void state_ownership(void) {
    StateFixture f = state_fixture(true);
    state_finish(&f);
    XrLirModule *copies[3] = {0};
    OK(xr_lir_rebuild(f.module, &copies[0]));
    OK(xr_lir_inline(f.module, &copies[1]));
    OK(xr_lir_sccp(f.module, &copies[2]));
    xr_lir_module_destroy(f.module);
    for (uint32_t i = 0; i < 3; ++i) {
        XrLirStateLayout layout; XrLirFunction entry; XrLirStateReference shape;
        OK(xr_lir_state_layout_view(copies[i], &layout));
        CHECK(layout.module_count == 2 && layout.dependencies[0] == 0 && layout.slot_count == 1);
        CHECK(layout.slots[0].payload == XR_LIR_I64 && layout.slots[0].mutable);
        OK(xr_lir_state_entry(copies[i], &entry));
        CHECK(entry.module == copies[i] && entry.id == 1);
        OK(xr_lir_state_reference_view(copies[i], (XrLirValue){copies[i], 1, 2}, &shape));
        CHECK(shape.payload == XR_LIR_I64 && shape.mutable);
        OK(xr_lir_verify(copies[i], NULL));
        xr_lir_module_destroy(copies[i]);
    }
}

static void state_rejections(void) {
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        StateFixture f = state_fixture(scenario != 1);
        XrLirInstruction instruction = {.op=XR_LIR_STATE_READ, .type=XR_LIR_STRING,
            .operands=&f.reference, .operand_count=1};
        XrLirValue out = {0}, operands[] = {f.reference, f.state};
        if (scenario == 1) instruction = (XrLirInstruction){.op=XR_LIR_STATE_PUBLISH,
            .type=XR_LIR_UNIT, .operands=&f.reference, .operand_count=1};
        if (scenario == 2) instruction = (XrLirInstruction){.op=XR_LIR_CONST, .type=XR_LIR_STATE};
        if (scenario == 3) instruction = (XrLirInstruction){.op=XR_LIR_STATE_WRITE, .type=XR_LIR_UNIT,
            .operands=operands, .operand_count=2};
        if (scenario == 4) instruction = (XrLirInstruction){.op=XR_LIR_CONST, .type=XR_LIR_STATE_REF};
        if (scenario == 5) instruction.bits = UINT64_MAX;
        XrLirStatus status = instruction.type == XR_LIR_UNIT ?
            xr_lir_emit_effect(f.module, f.entry, &instruction) :
            xr_lir_emit_value(f.module, f.entry, &instruction, &out);
        CHECK(status == XR_LIR_VERIFY_ERROR && !out.id);
        CHECK(xr_lir_module_finish(f.module, NULL) == XR_LIR_VERIFY_ERROR);
        xr_lir_module_destroy(f.module);
    }
    StateFixture f = state_fixture(true);
    state_finish(&f);
    f.module->functions[0].values[1].state_reference.payload = XR_LIR_STRING;
    CHECK(xr_lir_verify(f.module, NULL) == XR_LIR_VERIFY_ERROR);
    xr_lir_module_destroy(f.module);
}

int main(void) {
    CHECK(XR_LIR_STATE == 17 && XR_LIR_STATE_REF == 18);
    state_ownership(); state_rejections();
    puts("State layout ownership and capability rejection passed");
    return 0;
}
