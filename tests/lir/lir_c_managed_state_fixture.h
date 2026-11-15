/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_c_managed_state_fixture.h - Mixed owned and State-reference edge transport
 */
#include "lir/xlir_state.h"
static XrLirValue managed_state_value(XrLirModule *m, XrLirBlock block, XrLirOp op,
    XrLirType type, const XrLirValue *values, uint32_t count, uint64_t bits) {
    XrLirValue value = {0};
    CHECK(xr_lir_emit_value(m, block, &(XrLirInstruction){.op=op, .type=type,
        .operands=values, .operand_count=count, .bits=bits}, &value) == XR_LIR_OK);
    return value;
}
static void managed_state_effect(XrLirModule *m, XrLirBlock block, XrLirOp op,
    const XrLirValue *values, uint32_t count, uint64_t bits) {
    CHECK(xr_lir_emit_effect(m, block, &(XrLirInstruction){.op=op, .type=XR_LIR_UNIT,
        .operands=values, .operand_count=count, .bits=bits}) == XR_LIR_OK);
}
static XrLirModule *managed_state_fixture(XrLirFunction *root) {
    XrLirModule *m = NULL; CHECK(xr_lir_module_create(NULL, &m) == XR_LIR_OK);
    XrLirStateModule owner = {0};
    XrLirStateSlot slots[] = {
        {.payload=XR_LIR_I64, .mutable=true, .size=8, .alignment=8},
        {.payload=XR_LIR_I64, .mutable=true, .offset=8, .size=8, .alignment=8},
        {.payload=XR_LIR_STRING, .mutable=true, .offset=16, .size=8, .alignment=8}};
    XrLirStateLayout layout = {&owner, 1, NULL, 0, slots, 3, 24, 8};
    CHECK(xr_lir_set_state_layout(m, &layout) == XR_LIR_OK);
    XrLirType state_type = XR_LIR_STATE;
    XrLirFunctionSpec spec = {.name="state", .name_length=5, .parameters=&state_type,
        .parameter_count=1, .result=XR_LIR_I64};
    CHECK(xr_lir_declare_function(m, &spec, root) == XR_LIR_OK);
    CHECK(xr_lir_set_state_entry(m, *root) == XR_LIR_OK && xr_lir_define_function(m, *root) == XR_LIR_OK);
    XrLirBlock entry = {0}, loop = {0}, done = {0};
    CHECK(xr_lir_add_block(m, *root, NULL, 0, &entry) == XR_LIR_OK && xr_lir_set_entry(m, *root, entry) == XR_LIR_OK);
    XrLirValue state; CHECK(xr_lir_function_parameter(m, *root, 0, &state) == XR_LIR_OK);
    managed_state_effect(m, entry, XR_LIR_MODULE_BEGIN, &state, 1, 0);
    XrLirValue refs[3];
    XrLirLiteral literal; CHECK(xr_lir_add_literal(m, (const uint8_t *)"kept", 4, &literal) == XR_LIR_OK);
    for (uint32_t slot = 0; slot < 3; ++slot) {
        XrLirValue value = slot < 2 ? managed_state_value(m, entry, XR_LIR_CONST, XR_LIR_I64, NULL, 0, 11 * (slot + 1)) :
            managed_state_value(m, entry, XR_LIR_STRING_LITERAL, XR_LIR_STRING, NULL, 0, literal.id);
        XrLirValue operands[] = {state, value};
        refs[slot] = managed_state_value(m, entry, XR_LIR_STATE_CONSTRUCT, XR_LIR_STATE_REF, operands, 2, slot);
        if (slot == 2) managed_state_effect(m, entry, XR_LIR_STRING_DROP, &value, 1, 0);
        managed_state_effect(m, entry, XR_LIR_STATE_PUBLISH, &refs[slot], 1, 0);
    }
    managed_state_effect(m, entry, XR_LIR_MODULE_READY, &state, 1, 0);
    XrLirType types[] = {XR_LIR_STATE_REF, XR_LIR_STATE_REF, XR_LIR_STRING, XR_LIR_I64};
    CHECK(xr_lir_add_block(m, *root, types, 4, &loop) == XR_LIR_OK && xr_lir_add_block(m, *root, types, 4, &done) == XR_LIR_OK);
    XrLirValue carried[4], result[4];
    for (uint32_t i = 0; i < 4; ++i) {
        CHECK(xr_lir_block_parameter(m, loop, i, &carried[i]) == XR_LIR_OK);
        CHECK(xr_lir_block_parameter(m, done, i, &result[i]) == XR_LIR_OK);
        if (i < 2) {
            XrLirStateReference shape = {.payload=XR_LIR_I64, .mutable=true};
            CHECK(xr_lir_set_state_reference(m, carried[i], &shape) == XR_LIR_OK);
            CHECK(xr_lir_set_state_reference(m, result[i], &shape) == XR_LIR_OK);
        }
    }
    XrLirValue snapshot = managed_state_value(m, entry, XR_LIR_STATE_READ, XR_LIR_STRING, &refs[2], 1, 0);
    XrLirValue initial[] = {refs[0], refs[1], snapshot, managed_state_value(m, entry, XR_LIR_CONST, XR_LIR_I64, NULL, 0, 5)};
    CHECK(xr_lir_set_terminator(m, entry, &(XrLirInstruction){.op=XR_LIR_BR, .type=XR_LIR_UNIT,
        .edge_count=1, .edges={{loop, initial, 4}}}) == XR_LIR_OK);
    XrLirValue zero = managed_state_value(m, loop, XR_LIR_CONST, XR_LIR_I64, NULL, 0, 0), condition;
    XrLirValue comparison[] = {carried[3], zero};
    CHECK(xr_lir_emit_value(m, loop, &(XrLirInstruction){.op=XR_LIR_ICMP, .type=XR_LIR_I1,
        .operands=comparison, .operand_count=2, .predicate=XR_LIR_GT}, &condition) == XR_LIR_OK);
    XrLirValue one = managed_state_value(m, loop, XR_LIR_CONST, XR_LIR_I64, NULL, 0, 1);
    XrLirValue subtract[] = {carried[3], one};
    XrLirValue next = managed_state_value(m, loop, XR_LIR_SUB, XR_LIR_I64, subtract, 2, 0);
    XrLirValue swapped[] = {carried[1], carried[0], carried[2], next};
    CHECK(xr_lir_set_terminator(m, loop, &(XrLirInstruction){.op=XR_LIR_CONDBR, .type=XR_LIR_UNIT,
        .operands=&condition, .operand_count=1, .edge_count=2,
        .edges={{loop, swapped, 4}, {done, carried, 4}}}) == XR_LIR_OK);
    XrLirValue write[] = {result[0], managed_state_value(m, done, XR_LIR_CONST, XR_LIR_I64, NULL, 0, 33)};
    managed_state_effect(m, done, XR_LIR_STATE_WRITE, write, 2, 0);
    XrLirValue left = managed_state_value(m, done, XR_LIR_STATE_READ, XR_LIR_I64, &result[0], 1, 0);
    XrLirValue right = managed_state_value(m, done, XR_LIR_STATE_READ, XR_LIR_I64, &result[1], 1, 0);
    XrLirValue multiply[] = {left, managed_state_value(m, done, XR_LIR_CONST, XR_LIR_I64, NULL, 0, 100)};
    XrLirValue scaled = managed_state_value(m, done, XR_LIR_MUL, XR_LIR_I64, multiply, 2, 0);
    XrLirValue add[] = {scaled, right}, answer = managed_state_value(m, done, XR_LIR_ADD, XR_LIR_I64, add, 2, 0);
    managed_state_effect(m, done, XR_LIR_STRING_DROP, &result[2], 1, 0);
    CHECK(xr_lir_set_terminator(m, done, &(XrLirInstruction){.op=XR_LIR_RET, .type=XR_LIR_UNIT,
        .operands=&answer, .operand_count=1}) == XR_LIR_OK);
    CHECK(xr_lir_module_finish(m, NULL) == XR_LIR_OK); return m;
}
