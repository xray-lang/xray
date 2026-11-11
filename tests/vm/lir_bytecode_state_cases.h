/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_bytecode_state_cases.h - Typed module references through physical copies
 */
#ifndef LIR_BYTECODE_STATE_CASES_H
#define LIR_BYTECODE_STATE_CASES_H
#include "lir/xlir_state.h"

typedef struct StateTransportFixture {
    XrLirModule *module;
    XrLirFunction function;
    XrLirBlock entry;
    XrLirValue state, references[2];
} StateTransportFixture;

static void state_transport_effect(StateTransportFixture *f, XrLirOp op,
    XrLirValue operand, uint64_t bits) {
    XrLirInstruction instruction = {.op=op, .type=XR_LIR_UNIT,
        .operands=&operand, .operand_count=1, .bits=bits};
    OK(xr_lir_emit_effect(f->module, f->entry, &instruction));
}

static XrLirValue state_transport_reference(StateTransportFixture *f, XrLirBlock block,
    uint32_t slot) {
    XrLirInstruction instruction = {.op=XR_LIR_STATE_REFERENCE, .type=XR_LIR_STATE_REF,
        .operands=&f->state, .operand_count=1, .bits=slot};
    XrLirValue reference;
    OK(xr_lir_emit_value(f->module, block, &instruction, &reference));
    return reference;
}

static XrLirValue state_transport_read(XrLirModule *module, XrLirBlock block,
    XrLirValue reference, XrLirType type) {
    XrLirInstruction instruction = {.op=XR_LIR_STATE_READ, .type=type,
        .operands=&reference, .operand_count=1};
    XrLirValue result;
    OK(xr_lir_emit_value(module, block, &instruction, &result));
    return result;
}

static StateTransportFixture state_transport_fixture(void) {
    StateTransportFixture f = {0};
    OK(xr_lir_module_create(NULL, &f.module));
    XrLirStateModule owner = {0};
    XrLirStateSlot slots[] = {
        {.payload=XR_LIR_I64, .mutable=true, .size=8, .alignment=8},
        {.payload=XR_LIR_I64, .mutable=true, .offset=8, .size=8, .alignment=8}
    };
    XrLirStateLayout layout = {.modules=&owner, .module_count=1, .slots=slots,
        .slot_count=2, .payload_size=16, .payload_alignment=8};
    OK(xr_lir_set_state_layout(f.module, &layout));
    XrLirType parameters[] = {XR_LIR_I64, XR_LIR_STATE};
    f.function = test_function(f.module, "state_transport", parameters, 2, XR_LIR_I64);
    OK(xr_lir_set_state_entry(f.module, f.function));
    f.entry = test_block(f.module, f.function, NULL, 0);
    OK(xr_lir_set_entry(f.module, f.function, f.entry));
    f.state = parameter(f.module, f.function, 1);
    state_transport_effect(&f, XR_LIR_MODULE_BEGIN, f.state, 0);
    for (uint32_t slot = 0; slot < 2; ++slot) {
        XrLirValue operands[] = {f.state,
            test_constant(f.module, f.entry, XR_LIR_I64, 11u * (slot + 1))};
        XrLirInstruction construct = {.op=XR_LIR_STATE_CONSTRUCT, .type=XR_LIR_STATE_REF,
            .operands=operands, .operand_count=2, .bits=slot};
        OK(xr_lir_emit_value(f.module, f.entry, &construct, &f.references[slot]));
        state_transport_effect(&f, XR_LIR_STATE_PUBLISH, f.references[slot], 0);
    }
    state_transport_effect(&f, XR_LIR_MODULE_READY, f.state, 0);
    return f;
}

static void state_transport_copies(const XrRbcFunction *function, bool spills) {
    size_t moves = 0, stores = 0, loads = 0;
    for (size_t pc = 0; pc < function->code_count; ++pc) {
        uint32_t word = function->code[pc], a = (word >> 8) & 255u;
        XrRbcOp op = (XrRbcOp)(word & 255u);
        if (op != XR_RBC_MOV && op != XR_RBC_SPILL && op != XR_RBC_RELOAD) continue;
        if (function->register_kinds[a] != XR_RBC_STATE_REF) continue;
        if (op == XR_RBC_MOV) {
            CHECK(function->register_kinds[(word >> 16) & 255u] == XR_RBC_STATE_REF);
            ++moves;
        }
        stores += op == XR_RBC_SPILL;
        loads += op == XR_RBC_RELOAD;
    }
    if (spills) CHECK(function->spill_count && stores && loads);
    else CHECK(moves >= 3);
}

static void test_state_reference_cycle(bool spills) {
    StateTransportFixture f = state_transport_fixture();
    XrLirModule *m = f.module;
    XrLirType types[] = {XR_LIR_STATE_REF, XR_LIR_STATE_REF, XR_LIR_I64};
    XrLirBlock loop = test_block(m, f.function, types, 3);
    XrLirBlock body = test_block(m, f.function, NULL, 0);
    XrLirBlock done = test_block(m, f.function, NULL, 0);
    XrLirValue carried[] = {block_parameter(m, loop, 0), block_parameter(m, loop, 1),
        block_parameter(m, loop, 2)};
    XrLirStateReference shape = {XR_LIR_I64, true};
    OK(xr_lir_set_state_reference(m, carried[0], &shape));
    OK(xr_lir_set_state_reference(m, carried[1], &shape));
    if (spills)
        for (uint32_t n = 0; n < 280; ++n) (void)test_constant(m, f.entry, XR_LIR_I64, n);
    XrLirValue initial[] = {f.references[0], f.references[1], parameter(m, f.function, 0)};
    test_branch(m, f.entry, loop, initial, 3);
    XrLirValue zero = test_constant(m, loop, XR_LIR_I64, 0);
    XrLirValue condition = test_binary(m, loop, XR_LIR_ICMP, carried[2], zero, XR_LIR_GT);
    conditional(m, loop, condition, (XrLirEdge){.target=body}, (XrLirEdge){.target=done});
    XrLirValue next = test_binary(m, body, XR_LIR_SUB, carried[2],
        test_constant(m, body, XR_LIR_I64, 1), XR_LIR_PREDICATE_NONE);
    XrLirValue swapped[] = {carried[1], carried[0], next};
    test_branch(m, body, loop, swapped, 3);
    XrLirValue a = state_transport_read(m, done, carried[0], XR_LIR_I64);
    XrLirValue b = state_transport_read(m, done, carried[1], XR_LIR_I64);
    XrLirValue scaled = test_binary(m, done, XR_LIR_MUL, a,
        test_constant(m, done, XR_LIR_I64, 100), XR_LIR_PREDICATE_NONE);
    test_return(m, done, test_binary(m, done, XR_LIR_ADD, scaled, b, XR_LIR_PREDICATE_NONE));
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = compile(m, f.function);
    const XrRbcFunction *physical = &xr_rbc_program_view(program)->functions[0];
    state_transport_copies(physical, false);
    CHECK((physical->spill_count != 0) == spills);
    xr_lir_module_destroy(m);
    for (uint64_t rounds = 0; rounds < 6; ++rounds)
        CHECK(run(program, &rounds, 1) == (rounds & 1u ? 2211u : 1122u));
    xr_rbc_program_destroy(program);
}

static void test_state_reference_spills(void) {
    StateTransportFixture f = state_transport_fixture();
    for (uint32_t n = 0; n < 280; ++n)
        (void)test_constant(f.module, f.entry, XR_LIR_I64, n);
    XrLirValue result = test_constant(f.module, f.entry, XR_LIR_I64, 0);
    for (uint32_t n = 0; n < 24; ++n) {
        XrLirValue reference = state_transport_reference(&f, f.entry, n & 1u);
        XrLirValue value = state_transport_read(f.module, f.entry, reference, XR_LIR_I64);
        result = test_binary(f.module, f.entry, XR_LIR_ADD, result, value, XR_LIR_PREDICATE_NONE);
    }
    test_return(f.module, f.entry, result);
    OK(xr_lir_module_finish(f.module, NULL));
    XrRbcProgram *program = compile(f.module, f.function);
    state_transport_copies(&xr_rbc_program_view(program)->functions[0], true);
    xr_lir_module_destroy(f.module);
    for (uint64_t argument = 0; argument < 3; ++argument)
        CHECK(run(program, &argument, 1) == 396);
    xr_rbc_program_destroy(program);
}

static void test_state_reference_edges(void) {
    StateTransportFixture f = state_transport_fixture();
    XrLirModule *m = f.module;
    XrLirType type = XR_LIR_STATE_REF;
    XrLirBlock yes = test_block(m, f.function, NULL, 0);
    XrLirBlock no = test_block(m, f.function, NULL, 0);
    XrLirBlock join = test_block(m, f.function, &type, 1);
    XrLirValue incoming = block_parameter(m, join, 0);
    XrLirStateReference shape = {XR_LIR_I64, true};
    OK(xr_lir_set_state_reference(m, incoming, &shape));
    XrLirValue zero = test_constant(m, f.entry, XR_LIR_I64, 0);
    XrLirValue condition = test_binary(m, f.entry, XR_LIR_ICMP,
        parameter(m, f.function, 0), zero, XR_LIR_NE);
    conditional(m, f.entry, condition, (XrLirEdge){.target=yes}, (XrLirEdge){.target=no});
    XrLirValue left = state_transport_reference(&f, yes, 0);
    XrLirValue right = state_transport_reference(&f, no, 1);
    test_branch(m, yes, join, &left, 1);
    test_branch(m, no, join, &right, 1);
    test_return(m, join, state_transport_read(m, join, incoming, XR_LIR_I64));
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = compile(m, f.function);
    const XrRbcFunction *physical = &xr_rbc_program_view(program)->functions[0];
    uint32_t destination = UINT32_MAX;
    unsigned producers = 0;
    for (size_t pc = 0; pc < physical->code_count; ++pc) {
        uint32_t word = physical->code[pc];
        if ((word & 255u) != XR_RBC_STATE_REFERENCE) continue;
        uint32_t current = (word >> 8) & 255u;
        if (producers) CHECK(current == destination);
        destination = current;
        ++producers;
    }
    CHECK(producers == 2);
    xr_lir_module_destroy(m);
    uint64_t choice = 0;
    CHECK(run(program, &choice, 1) == 22);
    choice = 1;
    CHECK(run(program, &choice, 1) == 11);
    xr_rbc_program_destroy(program);
}

static void test_state_reference_string_retirement(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirStateModule owner = {0};
    XrLirStateSlot slot = {.payload=XR_LIR_STRING, .mutable=true,
        .size=sizeof(void *), .alignment=_Alignof(void *)};
    XrLirStateLayout layout = {.modules=&owner, .module_count=1, .slots=&slot,
        .slot_count=1, .payload_size=sizeof(void *), .payload_alignment=_Alignof(void *)};
    OK(xr_lir_set_state_layout(m, &layout));
    XrLirType parameters[] = {XR_LIR_I64, XR_LIR_STATE};
    XrLirFunction root = test_function(m, "state_string_retirement", parameters, 2, XR_LIR_STRING);
    OK(xr_lir_set_state_entry(m, root));
    XrLirBlock entry = test_block(m, root, NULL, 0);
    OK(xr_lir_set_entry(m, root, entry));
    StateTransportFixture f = {.module=m, .function=root, .entry=entry,
        .state=parameter(m, root, 1)};
    state_transport_effect(&f, XR_LIR_MODULE_BEGIN, f.state, 0);
    const char bytes[] = "execution-owned String remains valid after every producer retires"
        "\0and preserves its full payload across typed reference spills: ";
    XrLirValue text = frame_text(m, entry, bytes, sizeof(bytes) - 1);
    XrLirValue operands[] = {f.state, text}, reference;
    XrLirInstruction op = {.op=XR_LIR_STATE_CONSTRUCT, .type=XR_LIR_STATE_REF,
        .operands=operands, .operand_count=2};
    OK(xr_lir_emit_value(m, entry, &op, &reference));
    state_transport_effect(&f, XR_LIR_STATE_PUBLISH, reference, 0);
    frame_drop(m, entry, text, false);
    state_transport_effect(&f, XR_LIR_MODULE_READY, f.state, 0);
    for (uint32_t n = 0; n < 280; ++n) (void)test_constant(m, entry, XR_LIR_I64, n);
    reference = state_transport_reference(&f, entry, 0);
    XrLirValue original = state_transport_read(m, entry, reference, XR_LIR_STRING);
    frame_drop(m, entry, original, false);
    XrLirValue argument = parameter(m, root, 0), digits, replacement;
    op = (XrLirInstruction){.op=XR_LIR_STRING_I64, .type=XR_LIR_STRING,
        .operands=&argument, .operand_count=1};
    OK(xr_lir_emit_value(m, entry, &op, &digits));
    XrLirValue prefix = frame_text(m, entry, bytes, sizeof(bytes) - 1);
    XrLirValue pieces[] = {prefix, digits};
    op = (XrLirInstruction){.op=XR_LIR_STRING_CONCAT, .type=XR_LIR_STRING,
        .operands=pieces, .operand_count=2};
    OK(xr_lir_emit_value(m, entry, &op, &replacement));
    frame_drop(m, entry, prefix, false);
    frame_drop(m, entry, digits, false);
    XrLirValue write[] = {reference, replacement};
    op = (XrLirInstruction){.op=XR_LIR_STATE_WRITE, .type=XR_LIR_UNIT,
        .operands=write, .operand_count=2};
    OK(xr_lir_emit_effect(m, entry, &op));
    frame_drop(m, entry, replacement, false);
    test_return(m, entry, state_transport_read(m, entry, reference, XR_LIR_STRING));
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = compile(m, root);
    state_transport_copies(&xr_rbc_program_view(program)->functions[0], true);
    xr_lir_module_destroy(m);
    XrRbcExecution *executions[3] = {0};
    XrRbcOutcome *outcomes[3] = {0};
    for (uint64_t n = 0; n < 3; ++n)
        CHECK(xr_rbc_program_prepare(program, &n, 1, &executions[n]) == XR_RBC_OK);
    XrRbcExecution *unrun = NULL;
    uint64_t unused = 99;
    CHECK(xr_rbc_program_prepare(program, &unused, 1, &unrun) == XR_RBC_OK);
    xr_rbc_execution_free(&unrun);
    CHECK(!unrun);
    for (unsigned n = 0; n < 3; ++n) {
        outcomes[n] = xr_rbc_execution_run(executions[n]);
        CHECK(outcomes[n] && xr_rbc_outcome_status(outcomes[n]) == XR_RBC_OK);
        xr_rbc_execution_free(&executions[n]);
    }
    xr_rbc_program_destroy(program);
    for (unsigned n = 0; n < 3; ++n) {
        const XrtString *result = NULL;
        CHECK(xr_rbc_outcome_string(outcomes[n], &result));
        size_t length = 0;
        const char *actual = xrt_string_bytes(result, &length);
        CHECK(length == sizeof(bytes) && !memcmp(actual, bytes, sizeof(bytes) - 1));
        CHECK(actual[sizeof(bytes) - 1] == (char)('0' + n));
        xr_rbc_outcome_free(&outcomes[n]);
    }
}

static void test_state_reference_transport(void) {
    test_state_reference_cycle(false);
    test_state_reference_cycle(true);
    test_state_reference_spills();
    test_state_reference_edges();
    test_state_reference_string_retirement();
}

#endif // LIR_BYTECODE_STATE_CASES_H
