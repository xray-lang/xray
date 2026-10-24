/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_bytecode_moves_cases.h - Block copies, live values and early writes.
 */
#ifndef LIR_BYTECODE_MOVES_CASES_H
#define LIR_BYTECODE_MOVES_CASES_H

static size_t move_count(const XrRbcFunction *function) {
    size_t count = 0;
    for (size_t pc = 0; pc < function->code_count; ++pc)
        if ((function->code[pc] & 255) == XR_RBC_MOV) ++count;
    return count;
}

static void test_coalesced_edges(void) {
    for (unsigned live_through = 0; live_through < 2; ++live_through) {
        XrLirModule *m = NULL;
        OK(xr_lir_module_create(NULL, &m));
        XrLirType type = XR_LIR_I64;
        XrLirFunction root = test_function(m, "edge", &type, 1, type);
        XrLirBlock entry = test_block(m, root, NULL, 0), join = test_block(m, root, &type, 1);
        OK(xr_lir_set_entry(m, root, entry));
        XrLirValue original = parameter(m, root, 0), incoming = block_parameter(m, join, 0);
        test_branch(m, entry, join, &original, 1);
        XrLirValue result = live_through ? test_binary(m, join, XR_LIR_ADD,
            original, incoming, XR_LIR_PREDICATE_NONE) : incoming;
        test_return(m, join, result);
        OK(xr_lir_module_finish(m, NULL));
        XrRbcProgram *program = compile(m, root);
        xr_lir_module_destroy(m);
        const XrRbcFunction *function = &xr_rbc_program_view(program)->functions[0];
        // Acyclic copies need no snapshot register; identities need no MOV.
        CHECK(move_count(function) == live_through);
        CHECK(function->register_count == (live_through ? 3u : 1u));
        uint64_t value = 19;
        CHECK(run(program, &value, 1) == (live_through ? 38u : 19u));
        xr_rbc_program_destroy(program);
    }
}

static void test_group_interference(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType boolean = XR_LIR_I1, integer = XR_LIR_I64;
    XrLirFunction root = test_function(m, "group_interference", &boolean, 1, integer);
    XrLirBlock entry = test_block(m, root, NULL, 0), join = test_block(m, root, &integer, 1);
    OK(xr_lir_set_entry(m, root, entry));
    XrLirValue a = test_constant(m, entry, integer, 11), b = test_constant(m, entry, integer, 22);
    conditional(m, entry, parameter(m, root, 0),
        (XrLirEdge) {join, &a, 1}, (XrLirEdge) {join, &b, 1});
    test_return(m, join, block_parameter(m, join, 0));
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = compile(m, root);
    xr_lir_module_destroy(m);
    // Neither constant is precolored. The second merge must see the first
    // constant's interference through the already merged parameter group.
    uint64_t selector = 1;
    CHECK(run(program, &selector, 1) == 11);
    selector = 0;
    CHECK(run(program, &selector, 1) == 22);
    CHECK(move_count(&xr_rbc_program_view(program)->functions[0]) == 1);
    xr_rbc_program_destroy(program);
}

static void test_boolean_early_write(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType types[] = {XR_LIR_I1, XR_LIR_I64};
    XrLirFunction root = test_function(m, "toggle", types, 2, XR_LIR_I1);
    XrLirBlock entry = test_block(m, root, NULL, 0), loop = test_block(m, root, types, 2);
    XrLirBlock body = test_block(m, root, NULL, 0), done = test_block(m, root, NULL, 0);
    OK(xr_lir_set_entry(m, root, entry));
    XrLirValue initial[] = {parameter(m, root, 0), parameter(m, root, 1)};
    XrLirValue current = block_parameter(m, loop, 0), count = block_parameter(m, loop, 1);
    XrLirValue no = test_constant(m, entry, XR_LIR_I1, 0);
    XrLirValue zero = test_constant(m, entry, XR_LIR_I64, 0);
    XrLirValue one = test_constant(m, entry, XR_LIR_I64, 1);
    test_branch(m, entry, loop, initial, 2);
    XrLirValue condition = test_binary(m, loop, XR_LIR_ICMP, count, zero, XR_LIR_GT);
    conditional(m, loop, condition, (XrLirEdge) {.target = body}, (XrLirEdge) {.target = done});
    XrLirValue flipped = test_binary(m, body, XR_LIR_ICMP, current, no, XR_LIR_EQ);
    XrLirValue next = test_binary(m, body, XR_LIR_SUB, count, one, XR_LIR_PREDICATE_NONE);
    XrLirValue actuals[] = {flipped, next};
    test_branch(m, body, loop, actuals, 2);
    test_return(m, done, current);
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = compile(m, root);
    xr_lir_module_destroy(m);
    for (uint64_t initial_bit = 0; initial_bit < 2; ++initial_bit)
        for (uint64_t rounds = 0; rounds < 4; ++rounds) {
            uint64_t arguments[] = {initial_bit, rounds};
            CHECK(run(program, arguments, 2) == (initial_bit ^ (rounds & 1)));
        }
    xr_rbc_program_destroy(program);
}

static void test_copy_cycle(uint32_t cycle, XrLirType type) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType types[4];
    for (uint32_t p = 0; p < cycle; ++p) types[p] = type;
    types[cycle] = XR_LIR_I64;
    XrLirFunction root = test_function(m, "cycle", types, cycle + 1, type);
    // Reverse construction order forces liveness to cross a real backedge.
    XrLirBlock done = test_block(m, root, NULL, 0), loop = test_block(m, root, types, cycle + 1);
    XrLirBlock body = test_block(m, root, NULL, 0), entry = test_block(m, root, NULL, 0);
    OK(xr_lir_set_entry(m, root, entry));
    XrLirValue initial[4], carried[4], actuals[4];
    for (uint32_t p = 0; p <= cycle; ++p) {
        initial[p] = parameter(m, root, p);
        carried[p] = block_parameter(m, loop, p);
    }
    XrLirValue zero = test_constant(m, entry, XR_LIR_I64, 0);
    XrLirValue one = test_constant(m, entry, XR_LIR_I64, 1);
    test_branch(m, entry, loop, initial, cycle + 1);
    XrLirValue condition = test_binary(m, loop, XR_LIR_ICMP, carried[cycle], zero, XR_LIR_GT);
    conditional(m, loop, condition, (XrLirEdge) {.target = body}, (XrLirEdge) {.target = done});
    for (uint32_t p = 0; p < cycle; ++p) actuals[p] = carried[(p + 1) % cycle];
    actuals[cycle] = test_binary(m, body, XR_LIR_SUB, carried[cycle], one, XR_LIR_PREDICATE_NONE);
    test_branch(m, body, loop, actuals, cycle + 1);
    test_return(m, done, carried[0]);
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = compile(m, root);
    xr_lir_module_destroy(m);
    const XrRbcFunction *function = &xr_rbc_program_view(program)->functions[0];
    CHECK(move_count(function) == cycle + 1);
    // One temporary is shared with the preceding condition's zero slot.
    uint32_t temporary = function->register_count - 1;
    size_t saves = 0, restores = 0;
    for (size_t pc = 0; pc < function->code_count; ++pc) {
        uint32_t word = function->code[pc];
        if ((word & 255) != XR_RBC_MOV) continue;
        if (((word >> 8) & 255) == temporary) ++saves;
        if (((word >> 16) & 255) == temporary) ++restores;
    }
    CHECK(saves == 1 && restores == 1);
    uint64_t values[] = {UINT64_C(0x8000000000000000), UINT64_C(0x7ff0000000000001),
        UINT64_C(0xfff8000000004321), 0};
    for (uint64_t rounds = 0; rounds <= cycle + 1; ++rounds) {
        uint64_t arguments[4];
        memcpy(arguments, values, sizeof(arguments));
        arguments[cycle] = rounds;
        CHECK(run(program, arguments, cycle + 1) == values[rounds % cycle]);
    }
    xr_rbc_program_destroy(program);
}

static void test_copy_fanout(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType types[] = {XR_LIR_I64, XR_LIR_I64, XR_LIR_I64, XR_LIR_I64};
    XrLirFunction root = test_function(m, "fanout", types, 4, XR_LIR_I64);
    XrLirBlock entry = test_block(m, root, NULL, 0), loop = test_block(m, root, types, 4);
    XrLirBlock body = test_block(m, root, NULL, 0), done = test_block(m, root, NULL, 0);
    OK(xr_lir_set_entry(m, root, entry));
    XrLirValue initial[4], carried[4];
    for (uint32_t p = 0; p < 4; ++p) {
        initial[p] = parameter(m, root, p);
        carried[p] = block_parameter(m, loop, p);
    }
    XrLirValue zero = test_constant(m, entry, XR_LIR_I64, 0);
    XrLirValue one = test_constant(m, entry, XR_LIR_I64, 1);
    test_branch(m, entry, loop, initial, 4);
    XrLirValue condition = test_binary(m, loop, XR_LIR_ICMP, carried[3], zero, XR_LIR_GT);
    conditional(m, loop, condition, (XrLirEdge) {.target = body}, (XrLirEdge) {.target = done});
    XrLirValue next = test_binary(m, body, XR_LIR_SUB, carried[3], one, XR_LIR_PREDICATE_NONE);
    XrLirValue actuals[] = {carried[1], carried[0], carried[1], next};
    test_branch(m, body, loop, actuals, 4);
    XrLirValue ten = test_constant(m, done, XR_LIR_I64, 10);
    XrLirValue value = test_binary(m, done, XR_LIR_MUL, carried[0], ten, XR_LIR_PREDICATE_NONE);
    value = test_binary(m, done, XR_LIR_ADD, value, carried[1], XR_LIR_PREDICATE_NONE);
    value = test_binary(m, done, XR_LIR_MUL, value, ten, XR_LIR_PREDICATE_NONE);
    value = test_binary(m, done, XR_LIR_ADD, value, carried[2], XR_LIR_PREDICATE_NONE);
    test_return(m, done, value);
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = compile(m, root);
    xr_lir_module_destroy(m);
    const uint64_t expected[] = {123, 212, 121, 212};
    for (uint64_t rounds = 0; rounds < 4; ++rounds) {
        uint64_t arguments[] = {1, 2, 3, rounds};
        CHECK(run(program, arguments, 4) == expected[rounds]);
    }
    xr_rbc_program_destroy(program);
}

static void test_edge_moves(void) {
    test_coalesced_edges();
    test_group_interference();
    test_boolean_early_write();
    test_copy_cycle(2, XR_LIR_I64);
    test_copy_cycle(3, XR_LIR_I64);
    test_copy_cycle(2, XR_LIR_F64);
    test_copy_cycle(3, XR_LIR_F64);
    test_copy_fanout();
}
#endif
