/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_bytecode.c - Hand-built SSA execution, ownership and capacity tests.
 */
#include "../lir/lir_test_programs.h"
#include "base/xmalloc.h"
#include "vm/register/xrbc_lir.h"


// The sole guest result is owned; this test helper consumes it after inspection.
static XrRbcStatus execute(XrRbcProgram *program, const uint64_t *arguments,
    size_t count, uint64_t *bits) {
    XrRbcExecution *execution = NULL;
    XrRbcStatus status = xr_rbc_program_prepare(program, arguments, count, &execution);
    if (status != XR_RBC_OK) return status;
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    CHECK(outcome);
    status = xr_rbc_outcome_status(outcome);
    xr_rbc_outcome_bits(outcome, bits);
    xr_rbc_execution_free(&execution);
    xr_rbc_outcome_free(&outcome);
    return status;
}

static XrRbcProgram *compile(XrLirModule *m, XrLirFunction root) {
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic;
    CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_OK);
    CHECK(program && diagnostic.status == XR_RBC_COMPILE_OK);
    return program;
}

static uint64_t run(XrRbcProgram *program, const uint64_t *arguments, size_t count) {
    uint64_t result = UINT64_MAX;
    CHECK(execute(program, arguments, count, &result) == XR_RBC_OK);
    return result;
}

static XrLirValue parameter(XrLirModule *m, XrLirFunction f, uint32_t index) {
    XrLirValue value;
    OK(xr_lir_function_parameter(m, f, index, &value));
    return value;
}

static XrLirValue block_parameter(XrLirModule *m, XrLirBlock b, uint32_t index) {
    XrLirValue value;
    OK(xr_lir_block_parameter(m, b, index, &value));
    return value;
}

static void conditional(XrLirModule *m, XrLirBlock b, XrLirValue condition,
    XrLirEdge yes, XrLirEdge no) {
    XrLirInstruction branch = {.op = XR_LIR_CONDBR, .type = XR_LIR_UNIT,
        .operands = &condition, .operand_count = 1, .edges = {yes, no}, .edge_count = 2};
    OK(xr_lir_set_terminator(m, b, &branch));
}

static XrLirValue call(XrLirModule *m, XrLirBlock b, XrLirFunction callee,
    XrLirType type, const XrLirValue *arguments, uint32_t count) {
    XrLirInstruction i = {.op = XR_LIR_CALL, .type = type, .callee = callee,
        .operands = arguments, .operand_count = count};
    XrLirValue result = {0};
    if (type == XR_LIR_UNIT) OK(xr_lir_emit_effect(m, b, &i));
    else OK(xr_lir_emit_value(m, b, &i, &result));
    return result;
}

#include "lir_bytecode_moves_cases.h"

static void test_i64_identity_cast(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType type = XR_LIR_I64;
    XrLirFunction f = test_function(m, "identity_cast", &type, 1, type);
    XrLirBlock block = test_block(m, f, NULL, 0);
    OK(xr_lir_set_entry(m, f, block));
    XrLirValue input = parameter(m, f, 0), result;
    char path[] = "identity-cast.xr";
    for (uint32_t n = 0; n < 2; ++n) {
        XrLirInstruction instruction = {.op = XR_LIR_INTCAST, .type = type,
            .operands = &input, .operand_count = 1,
            .source = {path, sizeof(path) - 1, 4 + n, 2, 4 + n, 10}};
        OK(xr_lir_emit_value(m, block, &instruction, &result));
        input = result;
    }
    test_return(m, block, input);
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = compile(m, f);
    memset(path, 'x', sizeof(path) - 1);
    xr_lir_module_destroy(m);
    const XrtSourceLocation *location = xrt_locations_get(xr_rbc_program_locations(program), 1);
    CHECK(location && location->line == 4 && location->path_length == sizeof(path) - 1 &&
        !memcmp(location->path, "identity-cast.xr", sizeof(path) - 1));
    const uint64_t cases[] = {0, 1, INT64_MAX, UINT64_C(0x8000000000000000),
        UINT64_MAX, UINT64_C(0xa5a55a5adeadbeef)};
    for (size_t n = 0; n < sizeof(cases) / sizeof(cases[0]); ++n)
        CHECK(run(program, &cases[n], 1) == cases[n]);
    xr_rbc_program_destroy(program);
}

static void test_recursive_and_loop(void) {
    XrLirModule *m = test_fib();
    XrLirFunction root;
    OK(xr_lir_function_at(m, 1, &root));
    XrRbcProgram *program = compile(m, root);
    xr_lir_module_destroy(m);
    uint64_t n = 32;
    CHECK(run(program, &n, 1) == 2178309);
    CHECK(xr_rbc_program_view(program)->function_count == 1);
    xr_rbc_program_destroy(program);
    m = test_sum();
    OK(xr_lir_function_at(m, 1, &root));
    program = compile(m, root);
    xr_lir_module_destroy(m);
    n = 1000;
    CHECK(run(program, &n, 1) == 499500);
    xr_rbc_program_destroy(program);
}

static void test_location_ownership(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirFunction helper = test_function(m, "helper", NULL, 0, XR_LIR_I64);
    XrLirFunction root = test_function(m, "root", NULL, 0, XR_LIR_I64);
    XrLirFunction unused = test_function(m, "unused", NULL, 0, XR_LIR_I64);
    char path[400];
    memset(path, 'p', sizeof(path));
    memcpy(path + sizeof(path) - 7, "\xc3\xa9\n.xr!", 7);
    XrLirSource source = {path, sizeof(path) - 1, 7, 9, 7, 9};
    XrLirBlock finish = test_block(m, root, NULL, 0);
    XrLirBlock entry = test_block(m, root, NULL, 0);
    OK(xr_lir_set_entry(m, root, entry));
    XrLirInstruction instruction = {.op = XR_LIR_CONST, .type = XR_LIR_I64,
        .bits = 1, .source = source};
    XrLirValue ignored;
    OK(xr_lir_emit_value(m, entry, &instruction, &ignored));
    XrLirValue value = call(m, entry, helper, XR_LIR_I64, NULL, 0);
    test_branch(m, entry, finish, NULL, 0);
    instruction = (XrLirInstruction) {.op = XR_LIR_RET, .type = XR_LIR_UNIT,
        .operands = &value, .operand_count = 1, .source = source};
    OK(xr_lir_set_terminator(m, finish, &instruction));
    XrLirBlock block = test_block(m, helper, NULL, 0);
    OK(xr_lir_set_entry(m, helper, block));
    instruction = (XrLirInstruction) {.op = XR_LIR_CONST, .type = XR_LIR_I64,
        .bits = 7, .source = {"helper.xr", 9, 99, 2, 100, 3}};
    OK(xr_lir_emit_value(m, block, &instruction, &value));
    test_return(m, block, value);
    block = test_block(m, unused, NULL, 0);
    OK(xr_lir_set_entry(m, unused, block));
    instruction.source.line = 200;
    instruction.source.end_line = 201;
    OK(xr_lir_emit_value(m, block, &instruction, &value));
    test_return(m, block, value);
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = compile(m, root);
    XrtLocationOwner *owner = xr_rbc_program_locations(program);
    CHECK(owner && xrt_locations_count(owner) == 3);
    CHECK(xrt_locations_retain(owner));
    xr_lir_module_destroy(m);
    CHECK(run(program, NULL, 0) == 7);
    xr_rbc_program_destroy(program);
    const XrtSourceLocation *first = xrt_locations_get(owner, 1);
    const XrtSourceLocation *duplicate = xrt_locations_get(owner, 2);
    const XrtSourceLocation *last = xrt_locations_get(owner, 3);
    CHECK(first && duplicate && last && first->path_length == sizeof(path) - 1);
    CHECK(!memcmp(first->path, path, sizeof(path) - 1));
    memset(path, '?', sizeof(path));
    CHECK(first->line == 7 && duplicate->line == 7 && last->line == 99);
    CHECK(first->column == 9 && first->end_line == 7 && first->end_column == 9);
    CHECK(!memcmp(first->path, duplicate->path, first->path_length));
    CHECK(last->path_length == 9 && !memcmp(last->path, "helper.xr", 9));
    CHECK(!xrt_locations_get(owner, 0) && !xrt_locations_get(owner, 4));
    xrt_locations_release(owner);
    CHECK(!xr_rbc_program_locations(NULL));

    m = test_sum();
    OK(xr_lir_function_at(m, 1, &root));
    program = compile(m, root);
    owner = xr_rbc_program_locations(program);
    CHECK(owner && !xrt_locations_count(owner) && xrt_locations_retain(owner));
    xrt_locations_release(owner);
    CHECK(!xrt_locations_count(xr_rbc_program_locations(program)));
    xr_rbc_program_destroy(program);
    xr_lir_module_destroy(m);
}

static void test_comparisons(void) {
    const uint64_t inputs[][2] = {{0, 0}, {UINT64_MAX, 2}, {INT64_MAX, UINT64_C(1) << 63}};
    const bool expected[][6] = {{true, false, false, true, false, true},
        {false, true, true, true, false, false}, {false, true, false, false, true, true}};
    for (uint32_t predicate = XR_LIR_EQ; predicate <= XR_LIR_GE; ++predicate) {
        XrLirModule *m = NULL;
        OK(xr_lir_module_create(NULL, &m));
        XrLirType types[] = {XR_LIR_I64, XR_LIR_I64}, boolean = XR_LIR_I1;
        XrLirFunction root = test_function(m, "predicate", types, 2, boolean);
        XrLirBlock entry = test_block(m, root, NULL, 0);
        XrLirBlock join = test_block(m, root, &boolean, 1);
        OK(xr_lir_set_entry(m, root, entry));
        XrLirValue result = test_binary(m, entry, XR_LIR_ICMP, parameter(m, root, 0),
            parameter(m, root, 1), (XrLirPredicate) predicate);
        test_branch(m, entry, join, &result, 1);
        test_return(m, join, block_parameter(m, join, 0));
        OK(xr_lir_module_finish(m, NULL));
        XrRbcProgram *program = compile(m, root);
        xr_lir_module_destroy(m);
        for (size_t n = 0; n < 3; ++n)
            CHECK(run(program, inputs[n], 2) == expected[n][predicate - XR_LIR_EQ]);
        xr_rbc_program_destroy(program);
    }
}

static void test_arithmetic(void) {
    const XrLirOp operations[] = {XR_LIR_ADD, XR_LIR_SUB, XR_LIR_MUL,
        XR_LIR_DIV, XR_LIR_REM, XR_LIR_NEG};
    const uint64_t expected[] = {UINT64_C(9223372036854775807),
        UINT64_C(9223372036854775809), UINT64_C(9223372036854775808),
        UINT64_C(9223372036854775808), 0, UINT64_C(9223372036854775808)};
    for (size_t n = 0; n < sizeof(operations) / sizeof(operations[0]); ++n) {
        XrLirModule *m = NULL;
        OK(xr_lir_module_create(NULL, &m));
        XrLirType types[] = {XR_LIR_I64, XR_LIR_I64};
        XrLirFunction root = test_function(m, "arithmetic", types, 2, XR_LIR_I64);
        XrLirBlock entry = test_block(m, root, NULL, 0);
        OK(xr_lir_set_entry(m, root, entry));
        XrLirValue args[] = {parameter(m, root, 0), parameter(m, root, 1)}, result;
        XrLirInstruction i = {.op = operations[n], .type = XR_LIR_I64, .operands = args,
            .operand_count = operations[n] == XR_LIR_NEG ? 1 : 2};
        OK(xr_lir_emit_value(m, entry, &i, &result));
        test_return(m, entry, result);
        OK(xr_lir_module_finish(m, NULL));
        XrRbcProgram *program = compile(m, root);
        xr_lir_module_destroy(m);
        uint64_t values[] = {UINT64_C(9223372036854775808), UINT64_MAX};
        CHECK(run(program, values, 2) == expected[n]);
        if (operations[n] == XR_LIR_DIV || operations[n] == XR_LIR_REM) {
            values[0] = (uint64_t) -7; values[1] = 3;
            CHECK(run(program, values, 2) == (uint64_t) (operations[n] == XR_LIR_DIV ? -2 : -1));
            values[1] = 0;
            uint64_t sentinel = 123;
            CHECK(execute(program, values, 2, &sentinel) == XR_RBC_ARITHMETIC_PANIC);
            CHECK(sentinel == 123);
        }
        xr_rbc_program_destroy(program);
    }
}

static void test_constants_and_bool_calls(void) {
    const uint64_t constants[] = {0, 1, INT16_MAX, (uint64_t) INT16_MIN,
        32768, (uint64_t) -32769, INT64_MAX, UINT64_C(1) << 63, UINT64_MAX};
    for (size_t n = 0; n < sizeof(constants) / sizeof(constants[0]); ++n) {
        XrLirModule *m = NULL;
        OK(xr_lir_module_create(NULL, &m));
        XrLirFunction root = test_function(m, "constant", NULL, 0, XR_LIR_I64);
        XrLirBlock entry = test_block(m, root, NULL, 0);
        OK(xr_lir_set_entry(m, root, entry));
        test_return(m, entry, test_constant(m, entry, XR_LIR_I64, constants[n]));
        OK(xr_lir_module_finish(m, NULL));
        XrRbcProgram *program = compile(m, root);
        xr_lir_module_destroy(m);
        CHECK(run(program, NULL, 0) == constants[n]);
        xr_rbc_program_destroy(program);
    }
    for (XrLirPredicate predicate = XR_LIR_EQ; predicate <= XR_LIR_NE; ++predicate) {
        XrLirModule *m = NULL;
        XrLirType boolean = XR_LIR_I1, pair[] = {XR_LIR_I1, XR_LIR_I1};
        OK(xr_lir_module_create(NULL, &m));
        XrLirFunction identity = test_function(m, "identity", &boolean, 1, boolean);
        XrLirBlock body = test_block(m, identity, NULL, 0);
        OK(xr_lir_set_entry(m, identity, body));
        test_return(m, body, parameter(m, identity, 0));
        XrLirFunction root = test_function(m, "bool_call", pair, 2, boolean);
        XrLirBlock entry = test_block(m, root, NULL, 0);
        OK(xr_lir_set_entry(m, root, entry));
        XrLirValue result = test_binary(m, entry, XR_LIR_ICMP, parameter(m, root, 0),
            parameter(m, root, 1), predicate);
        test_return(m, entry, call(m, entry, identity, boolean, &result, 1));
        OK(xr_lir_module_finish(m, NULL));
        XrRbcProgram *program = compile(m, root);
        xr_lir_module_destroy(m);
        for (uint64_t a = 0; a < 2; ++a) for (uint64_t b = 0; b < 2; ++b) {
            uint64_t arguments[] = {a, b};
            CHECK(run(program, arguments, 2) == (uint64_t) (predicate == XR_LIR_EQ ? a == b : a != b));
        }
        xr_rbc_program_destroy(program);
    }
}

static void test_parallel_cycle(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType types[] = {XR_LIR_I64, XR_LIR_I64, XR_LIR_I64, XR_LIR_I64};
    XrLirFunction root = test_function(m, "rotate", types, 4, XR_LIR_I64);
    // Entry is deliberately the third block in construction order.
    XrLirBlock done = test_block(m, root, NULL, 0), loop = test_block(m, root, types, 4);
    XrLirBlock entry = test_block(m, root, NULL, 0), body = test_block(m, root, NULL, 0);
    OK(xr_lir_set_entry(m, root, entry));
    XrLirValue initial[4], carried[4];
    for (uint32_t i = 0; i < 4; ++i) {
        initial[i] = parameter(m, root, i);
        carried[i] = block_parameter(m, loop, i);
    }
    XrLirValue zero = test_constant(m, entry, XR_LIR_I64, 0);
    XrLirValue one = test_constant(m, entry, XR_LIR_I64, 1);
    test_branch(m, entry, loop, initial, 4);
    XrLirValue condition = test_binary(m, loop, XR_LIR_ICMP, carried[3], zero, XR_LIR_GT);
    conditional(m, loop, condition, (XrLirEdge) {.target = body}, (XrLirEdge) {.target = done});
    XrLirValue next = test_binary(m, body, XR_LIR_SUB, carried[3], one, XR_LIR_PREDICATE_NONE);
    XrLirValue rotated[] = {carried[1], carried[2], carried[0], next};
    test_branch(m, body, loop, rotated, 4);
    XrLirValue ten = test_constant(m, done, XR_LIR_I64, 10);
    XrLirValue a = test_binary(m, done, XR_LIR_MUL, carried[0], ten, XR_LIR_PREDICATE_NONE);
    a = test_binary(m, done, XR_LIR_ADD, a, carried[1], XR_LIR_PREDICATE_NONE);
    a = test_binary(m, done, XR_LIR_MUL, a, ten, XR_LIR_PREDICATE_NONE);
    a = test_binary(m, done, XR_LIR_ADD, a, carried[2], XR_LIR_PREDICATE_NONE);
    test_return(m, done, a);
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = compile(m, root);
    xr_lir_module_destroy(m);
    const uint64_t expected[] = {123, 231, 312, 123};
    for (uint64_t n = 0; n < 4; ++n) {
        uint64_t values[] = {1, 2, 3, n};
        CHECK(run(program, values, 4) == expected[n]);
    }
    xr_rbc_program_destroy(program);
}

static void test_same_target_edges(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType types[] = {XR_LIR_I1, XR_LIR_I64, XR_LIR_I64}, pair[] = {XR_LIR_I64, XR_LIR_I64};
    XrLirFunction root = test_function(m, "select_pair", types, 3, XR_LIR_I64);
    XrLirBlock entry = test_block(m, root, NULL, 0), done = test_block(m, root, pair, 2);
    OK(xr_lir_set_entry(m, root, entry));
    XrLirValue a = parameter(m, root, 1), b = parameter(m, root, 2);
    XrLirValue yes[] = {a, a}, no[] = {b, a};
    conditional(m, entry, parameter(m, root, 0),
        (XrLirEdge) {done, yes, 2}, (XrLirEdge) {done, no, 2});
    XrLirValue ten = test_constant(m, done, XR_LIR_I64, 10);
    XrLirValue value = test_binary(m, done, XR_LIR_MUL, block_parameter(m, done, 0), ten,
        XR_LIR_PREDICATE_NONE);
    value = test_binary(m, done, XR_LIR_ADD, value, block_parameter(m, done, 1),
        XR_LIR_PREDICATE_NONE);
    test_return(m, done, value);
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = compile(m, root);
    xr_lir_module_destroy(m);
    uint64_t values[] = {1, 1, 2};
    CHECK(run(program, values, 3) == 11);
    values[0] = 0;
    CHECK(run(program, values, 3) == 21);
    values[0] = 2;
    uint64_t sentinel = 123;
    CHECK(execute(program, values, 3, &sentinel) == XR_RBC_INVALID && sentinel == 123);
    xr_rbc_program_destroy(program);
}

static void test_call_windows(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType type = XR_LIR_I64, pair[] = {XR_LIR_I64, XR_LIR_I64};
    XrLirFunction zero = test_function(m, "zero", NULL, 0, type);
    XrLirFunction one = test_function(m, "one", &type, 1, type);
    XrLirFunction two = test_function(m, "two", pair, 2, type);
    XrLirFunction unit = test_function(m, "unit", NULL, 0, XR_LIR_UNIT);
    XrLirFunction root = test_function(m, "root", &type, 1, type);
    XrLirBlock z = test_block(m, zero, NULL, 0), o = test_block(m, one, NULL, 0);
    XrLirBlock t = test_block(m, two, NULL, 0), u = test_block(m, unit, NULL, 0);
    XrLirBlock r = test_block(m, root, NULL, 0);
    OK(xr_lir_set_entry(m, zero, z)); OK(xr_lir_set_entry(m, one, o));
    OK(xr_lir_set_entry(m, two, t)); OK(xr_lir_set_entry(m, unit, u));
    OK(xr_lir_set_entry(m, root, r));
    test_return(m, z, test_constant(m, z, type, 7));
    XrLirValue three = test_constant(m, o, type, 3);
    test_return(m, o, test_binary(m, o, XR_LIR_MUL, parameter(m, one, 0), three,
        XR_LIR_PREDICATE_NONE));
    XrLirValue ten = test_constant(m, t, type, 10);
    XrLirValue combined = test_binary(m, t, XR_LIR_MUL, parameter(m, two, 0), ten,
        XR_LIR_PREDICATE_NONE);
    test_return(m, t, test_binary(m, t, XR_LIR_ADD, combined, parameter(m, two, 1),
        XR_LIR_PREDICATE_NONE));
    test_return(m, u, (XrLirValue) {0});
    XrLirValue sentinel = test_constant(m, r, type, 1000), n = parameter(m, root, 0);
    XrLirValue a = call(m, r, zero, type, NULL, 0);
    XrLirValue b = call(m, r, one, type, &n, 1);
    XrLirValue reverse[] = {b, a}, duplicate[] = {n, n};
    XrLirValue c = call(m, r, two, type, reverse, 2);
    XrLirValue d = call(m, r, two, type, duplicate, 2);
    call(m, r, unit, XR_LIR_UNIT, NULL, 0);
    XrLirValue answer = test_binary(m, r, XR_LIR_ADD, c, d, XR_LIR_PREDICATE_NONE);
    answer = test_binary(m, r, XR_LIR_ADD, answer, sentinel, XR_LIR_PREDICATE_NONE);
    answer = test_binary(m, r, XR_LIR_ADD, answer, a, XR_LIR_PREDICATE_NONE);
    test_return(m, r, answer);
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = compile(m, root);
    XrRbcProgram *empty = compile(m, unit);
    xr_lir_module_destroy(m);
    uint64_t input = 2;
    CHECK(run(program, &input, 1) == 1096);
    CHECK(run(empty, NULL, 0) == 0);
    xr_rbc_program_destroy(empty);
    xr_rbc_program_destroy(program);
}

static void test_capacity(uint32_t count, bool scratch) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType types[256];
    uint64_t arguments[256];
    for (uint32_t n = 0; n < count; ++n) { types[n] = XR_LIR_I64; arguments[n] = n; }
    if (scratch) types[0] = XR_LIR_I1;
    XrLirFunction root = test_function(m, "capacity", types, count, XR_LIR_I64);
    XrLirBlock entry = test_block(m, root, NULL, 0);
    OK(xr_lir_set_entry(m, root, entry));
    XrLirValue value = parameter(m, root, count - 1);
    if (scratch) {
        XrLirType type = XR_LIR_I64;
        XrLirBlock join = test_block(m, root, &type, 1);
        conditional(m, entry, parameter(m, root, 0),
            (XrLirEdge) {join, &value, 1}, (XrLirEdge) {join, &value, 1});
        test_return(m, join, block_parameter(m, join, 0));
    } else test_return(m, entry, value);
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic;
    XrRbcCompileStatus status = xr_rbc_compile(m, root, &program, &diagnostic);
    if ((count == 255 && !scratch) || (count == 254 && scratch)) {
        CHECK(status == XR_RBC_COMPILE_OK);
        CHECK(xr_rbc_program_view(program)->functions[0].register_count == 255);
        CHECK(run(program, arguments, count) == count - 1);
        if (scratch) {
            arguments[0] = 1;
            CHECK(run(program, arguments, count) == count - 1);
        }
    } else CHECK(status == XR_RBC_COMPILE_LIMIT && !program);
    xr_lir_module_destroy(m);
    xr_rbc_program_destroy(program);
}

static void test_call_capacity(uint32_t count) {
    XrLirModule *m = NULL;
    XrLirType types[254];
    for (uint32_t n = 0; n < count; ++n) types[n] = XR_LIR_I64;
    OK(xr_lir_module_create(NULL, &m));
    XrLirFunction callee = test_function(m, "callee", NULL, 0, XR_LIR_I64);
    XrLirBlock body = test_block(m, callee, NULL, 0);
    OK(xr_lir_set_entry(m, callee, body));
    test_return(m, body, test_constant(m, body, XR_LIR_I64, 71));
    XrLirFunction root = test_function(m, "window_capacity", types, count, XR_LIR_I64);
    XrLirBlock entry = test_block(m, root, NULL, 0);
    OK(xr_lir_set_entry(m, root, entry));
    test_return(m, entry, call(m, entry, callee, XR_LIR_I64, NULL, 0));
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic;
    XrRbcCompileStatus status = xr_rbc_compile(m, root, &program, &diagnostic);
    if (count == 253) {
        CHECK(status == XR_RBC_COMPILE_OK);
        uint64_t arguments[254] = {0};
        CHECK(run(program, arguments, count) == 71);
    } else CHECK(status == XR_RBC_COMPILE_LIMIT && !program);
    xr_lir_module_destroy(m);
    xr_rbc_program_destroy(program);
}

static void test_unused_and_invalid(void) {
    XrLirType type = XR_LIR_I64;
    XrLirIntrinsic intrinsic = {123, &type, 1, type, true};
    XrLirRegistry registry = {&intrinsic, 1};
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(&registry, &m));
    XrLirFunction unused = test_function(m, "unused_float", NULL, 0, XR_LIR_F32);
    XrLirBlock float_body = test_block(m, unused, NULL, 0);
    OK(xr_lir_set_entry(m, unused, float_body));
    test_return(m, float_body, test_constant(m, float_body, XR_LIR_F32, 0));
    XrLirFunction root = test_function(m, "root", NULL, 0, XR_LIR_I64);
    XrLirBlock body = test_block(m, root, NULL, 0);
    OK(xr_lir_set_entry(m, root, body));
    test_return(m, body, test_constant(m, body, XR_LIR_I64, 17));
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic;
    CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_INVALID);
    OK(xr_lir_module_finish(m, NULL));
    program = compile(m, root);
    XrRbcProgram *preserved = program;
    CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_INVALID);
    CHECK(program == preserved);
    XrRbcProgram *unsupported = NULL;
    CHECK(xr_rbc_compile(m, unused, &unsupported, &diagnostic) == XR_RBC_COMPILE_UNSUPPORTED);
    CHECK(!unsupported);
    xr_lir_module_destroy(m);
    CHECK(run(program, NULL, 0) == 17);
    uint64_t result = 44;
    CHECK(execute(program, NULL, 1, &result) == XR_RBC_INVALID && result == 44);
    xr_rbc_program_destroy(program);

    // Release construction can publish before the explicit SSA trust check.
    // A value defined only on one sibling must never execute on the other.
    m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    root = test_function(m, "bad_dominance", NULL, 0, XR_LIR_I64);
    XrLirBlock entry = test_block(m, root, NULL, 0);
    XrLirBlock left = test_block(m, root, NULL, 0), right = test_block(m, root, NULL, 0);
    OK(xr_lir_set_entry(m, root, entry));
    XrLirValue condition = test_constant(m, entry, XR_LIR_I1, 1);
    XrLirValue sibling = test_constant(m, left, XR_LIR_I64, 1);
    conditional(m, entry, condition, (XrLirEdge) {.target = left}, (XrLirEdge) {.target = right});
    test_return(m, left, sibling);
    test_return(m, right, sibling);
    XrLirStatus finished = xr_lir_module_finish(m, NULL);
    CHECK(finished == XR_LIR_OK || finished == XR_LIR_VERIFY_ERROR);
    program = NULL;
    CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_INVALID);
    CHECK(!program);
    xr_lir_module_destroy(m);
}

static void test_owned_diagnostic_and_rejection(void) {
    XrLirType type = XR_LIR_I64;
    XrLirIntrinsic intrinsic = {123, &type, 1, type, true};
    XrLirRegistry registry = {&intrinsic, 1};
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(&registry, &m));
    XrLirFunction root = test_function(m, "intrinsic", &type, 1, type);
    XrLirBlock entry = test_block(m, root, NULL, 0);
    OK(xr_lir_set_entry(m, root, entry));
    XrLirValue arg = parameter(m, root, 0), result;
    char path[] = "owned-source.xr";
    XrLirInstruction i = {.op = XR_LIR_CALL_INTRINSIC, .type = type,
        .operands = &arg, .operand_count = 1, .intrinsic = 123,
        .source = {path, sizeof(path) - 1, 17, 9, 17, 15}};
    OK(xr_lir_emit_value(m, entry, &i, &result));
    memset(path, 'x', sizeof(path) - 1);
    test_return(m, entry, result);
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic;
    CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_UNSUPPORTED);
    CHECK(!program);
    xr_lir_module_destroy(m);
    CHECK(strcmp(diagnostic.path, "owned-source.xr") == 0);
    CHECK(diagnostic.line == 17 && diagnostic.column == 9 && diagnostic.instruction == 1);

    m = test_sum();
    OK(xr_lir_function_at(m, 2, &root));
    CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_OK);
    CHECK(xr_rbc_program_view(program)->native_count == XR_RBC_NATIVE_COUNT &&
        xr_rbc_program_view(program)->natives[0] == XR_RBC_NATIVE_PRINT_I64);
    xr_lir_module_destroy(m);
    xr_rbc_program_destroy(program);
    program = NULL;
    CHECK(xr_rbc_compile(NULL, (XrLirFunction) {0}, &program, &diagnostic) == XR_RBC_COMPILE_INVALID);
}

static void test_f64_constants_and_refusals(void) {
    const uint64_t bits[] = {0, UINT64_C(0x8000000000000000),
        UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
        UINT64_C(0x7ff8000000000000)};
    for (size_t n = 0; n < sizeof(bits) / sizeof(bits[0]); ++n) {
        XrLirModule *m = NULL;
        OK(xr_lir_module_create(NULL, &m));
        XrLirFunction f = test_function(m, "constant", NULL, 0, XR_LIR_F64);
        XrLirBlock b = test_block(m, f, NULL, 0);
        OK(xr_lir_set_entry(m, f, b));
        test_return(m, b, test_constant(m, b, XR_LIR_F64, bits[n]));
        OK(xr_lir_module_finish(m, NULL));
        XrRbcProgram *program = compile(m, f);
        xr_lir_module_destroy(m);
        CHECK(run(program, NULL, 0) == bits[n]);
        xr_rbc_program_destroy(program);
    }
    const struct { XrLirOp op; XrLirType input, output; uint32_t arity; } cases[] = {
        {XR_LIR_FADD, XR_LIR_F64, XR_LIR_F64, 2},
        {XR_LIR_FSUB, XR_LIR_F64, XR_LIR_F64, 2},
        {XR_LIR_FMUL, XR_LIR_F64, XR_LIR_F64, 2},
        {XR_LIR_FDIV, XR_LIR_F64, XR_LIR_F64, 2},
        {XR_LIR_FNEG, XR_LIR_F64, XR_LIR_F64, 1},
        {XR_LIR_FCMP, XR_LIR_F64, XR_LIR_I1, 2},
        {XR_LIR_ITOF, XR_LIR_I64, XR_LIR_F64, 1},
        {XR_LIR_FCAST, XR_LIR_F64, XR_LIR_F64, 1},
        {XR_LIR_INTCAST, XR_LIR_I8, XR_LIR_I64, 1},
        {XR_LIR_INTCAST, XR_LIR_U64, XR_LIR_I64, 1},
        {XR_LIR_INTCAST, XR_LIR_I64, XR_LIR_I8, 1},
        {XR_LIR_INTCAST, XR_LIR_I64, XR_LIR_U64, 1}
    };
    for (size_t n = 0; n < sizeof(cases) / sizeof(cases[0]); ++n) {
        XrLirModule *m = NULL;
        OK(xr_lir_module_create(NULL, &m));
        XrLirFunction f = test_function(m, "refused", &cases[n].input, 1, cases[n].output);
        XrLirBlock b = test_block(m, f, NULL, 0);
        OK(xr_lir_set_entry(m, f, b));
        XrLirValue a = parameter(m, f, 0), arguments[] = {a, a}, result;
        XrLirInstruction i = {.op = cases[n].op, .type = cases[n].output,
            .operands = arguments, .operand_count = cases[n].arity,
            .predicate = cases[n].op == XR_LIR_FCMP ? XR_LIR_EQ : XR_LIR_PREDICATE_NONE,
            .source = {"refused.xr", 10, 3, 5, 3, 9}};
        OK(xr_lir_emit_value(m, b, &i, &result));
        test_return(m, b, result);
        OK(xr_lir_module_finish(m, NULL));
        XrRbcProgram *program = NULL;
        XrRbcCompileDiagnostic diagnostic;
        CHECK(xr_rbc_compile(m, f, &program, &diagnostic) == XR_RBC_COMPILE_UNSUPPORTED);
        CHECK(!program);
        xr_lir_module_destroy(m);
        CHECK(!strcmp(diagnostic.path, "refused.xr") && diagnostic.line == 3);
    }
    // Unsupported value admission still owns the available defining location.
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirFunction f = test_function(m, "f32_value", NULL, 0, XR_LIR_I64);
    XrLirBlock b = test_block(m, f, NULL, 0);
    OK(xr_lir_set_entry(m, f, b));
    XrLirValue unused;
    XrLirInstruction i = {.op = XR_LIR_CONST, .type = XR_LIR_F32, .bits = 0,
        .source = {"unsupported-value.xr", 20, 7, 11, 7, 14}};
    OK(xr_lir_emit_value(m, b, &i, &unused));
    test_return(m, b, test_constant(m, b, XR_LIR_I64, 0));
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic;
    CHECK(xr_rbc_compile(m, f, &program, &diagnostic) == XR_RBC_COMPILE_UNSUPPORTED);
    CHECK(!program);
    xr_lir_module_destroy(m);
    CHECK(!strcmp(diagnostic.path, "unsupported-value.xr") && diagnostic.line == 7 &&
        diagnostic.column == 11 && diagnostic.instruction == 1);
}

static XrRbcProgram *panic_program(XrLirOp op, bool unknown, const char *path) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType flag_type = XR_LIR_I1, types[] = {XR_LIR_I64, XR_LIR_I64};
    XrLirFunction root = test_function(m, "choose", &flag_type, 1, XR_LIR_I64);
    XrLirFunction helpers[] = {test_function(m, "left", types, 2, XR_LIR_I64),
        test_function(m, "right", types, 2, XR_LIR_I64)};
    XrLirBlock entry = test_block(m, root, NULL, 0);
    XrLirBlock yes = test_block(m, root, NULL, 0), no = test_block(m, root, NULL, 0);
    OK(xr_lir_set_entry(m, root, entry));
    XrLirValue arguments[] = {test_constant(m, entry, XR_LIR_I64, 17),
        test_constant(m, entry, XR_LIR_I64, 0)};
    conditional(m, entry, parameter(m, root, 0), (XrLirEdge){yes, NULL, 0},
        (XrLirEdge){no, NULL, 0});
    test_return(m, yes, call(m, yes, helpers[0], XR_LIR_I64, arguments, 2));
    test_return(m, no, call(m, no, helpers[1], XR_LIR_I64, arguments, 2));
    for (unsigned h = 0; h < 2; ++h) {
        XrLirBlock b = test_block(m, helpers[h], NULL, 0);
        OK(xr_lir_set_entry(m, helpers[h], b));
        XrLirValue one, result;
        XrLirInstruction instruction = {.op = XR_LIR_CONST, .type = XR_LIR_I64, .bits = 1,
            .source = {path, strlen(path), 3, 2, 3, 4}};
        OK(xr_lir_emit_value(m, b, &instruction, &one));
        XrLirValue lhs = test_binary(m, b, XR_LIR_ADD, parameter(m, helpers[h], 0), one,
            XR_LIR_PREDICATE_NONE);
        XrLirValue operands[] = {lhs, parameter(m, helpers[h], 1)};
        XrLirSource source = unknown && h ? (XrLirSource){0} :
            (XrLirSource){path, strlen(path), h ? 29u : 17u, 5, h ? 30u : 18u, 9};
        instruction = (XrLirInstruction){.op = op, .type = XR_LIR_I64,
            .operands = operands, .operand_count = 2, .source = source};
        OK(xr_lir_emit_value(m, b, &instruction, &result));
        test_return(m, b, result);
    }
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = compile(m, root);
    xr_lir_module_destroy(m);
    return program;
}

static void test_owned_arithmetic_panics(void) {
    for (unsigned op = 0; op < 2; ++op) {
        for (unsigned unknown = 0; unknown < 2; ++unknown) {
            for (unsigned program_first = 0; program_first < 2; ++program_first) {
                char *path = xr_malloc(401), expected[401];
                CHECK(path);
                memset(path, 'p', 400);
                memcpy(path + 380, "\xc3\xa9\n\"\\.xr", 8);
                path[400] = 0;
                memcpy(expected, path, 401);
                XrRbcProgram *program = panic_program(op ? XR_LIR_REM : XR_LIR_DIV,
                    unknown != 0, path);
                memset(path, 'x', 400);
                xr_free(path);
                XrRbcOutcome *outcomes[2];
                for (unsigned h = 0; h < 2; ++h) {
                    uint64_t argument = h ? 0 : 1;
                    XrRbcExecution *execution = NULL;
                    CHECK(xr_rbc_program_prepare(program, &argument, 1, &execution) == XR_RBC_OK);
                    outcomes[h] = xr_rbc_execution_run(execution);
                    CHECK(outcomes[h] && !xr_rbc_execution_run(execution));
                    xr_rbc_execution_free(&execution);
                }
                if (program_first) xr_rbc_program_destroy(program);
                for (unsigned h = 0; h < 2; ++h) {
                    XrRbcOutcome *outcome = outcomes[h];
                    CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_ARITHMETIC_PANIC);
                    uint64_t bits = 919;
                    CHECK(!xr_rbc_outcome_bits(outcome, &bits) && bits == 919);
                    XrtArithmeticPanic facts = {77, 88};
                    const XrtLocationOwner *borrowed = NULL;
                    CHECK(!xr_rbc_outcome_arithmetic_panic(outcome, NULL, &borrowed) && !borrowed);
                    CHECK(!xr_rbc_outcome_arithmetic_panic(outcome, &facts, NULL));
                    CHECK(facts.code == 77 && facts.source_id == 88);
                    CHECK(xr_rbc_outcome_arithmetic_panic(outcome, &facts, &borrowed));
                    CHECK(facts.code == (op ? 421u : 420u));
                    bool known = !unknown || !h;
                    CHECK(facts.source_id == (known ? h ? 4u : 2u : 0u));
                    XrtOutputFailure output = {.source_id = 99};
                    unsigned char before[sizeof(output)];
                    memcpy(before, &output, sizeof(output));
                    const XrtLocationOwner *same = borrowed;
                    CHECK(!xr_rbc_outcome_output_failure(outcome, &output, &borrowed));
                    CHECK(!memcmp(before, &output, sizeof(output)) && borrowed == same);
                    XrtLocationOwner *retained = (XrtLocationOwner *)borrowed;
                    CHECK(xrt_locations_retain(retained));
                    xr_rbc_outcome_free(&outcome);
                    CHECK(xrt_locations_count(retained) == (unknown ? 3u : 4u));
                    const XrtSourceLocation *location = xrt_locations_get(retained, facts.source_id);
                    if (known) {
                        CHECK(location && location->path_length == 400);
                        CHECK(!memcmp(location->path, expected, 400));
                        CHECK(location->line == (h ? 29u : 17u) && location->column == 5);
                        CHECK(location->end_line == (h ? 30u : 18u) && location->end_column == 9);
                    } else CHECK(!location);
                    xrt_locations_release(retained);
                }
                if (!program_first) xr_rbc_program_destroy(program);
            }
        }
    }
}


static XrLirValue cell_value(XrLirModule *m, XrLirBlock block, XrLirOp op,
    XrLirValue operand) {
    XrLirInstruction instruction = {.op = op,
        .type = op == XR_LIR_CELL_NEW ? XR_LIR_I64_CELL : XR_LIR_I64,
        .operands = &operand, .operand_count = 1};
    XrLirValue result;
    OK(xr_lir_emit_value(m, block, &instruction, &result));
    return result;
}

static void test_cell_transport(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType flag = XR_LIR_I1, cells[] = {XR_LIR_I64_CELL, XR_LIR_I64_CELL};
    XrLirFunction root = test_function(m, "root", &flag, 1, XR_LIR_I64);
    XrLirFunction helper = test_function(m, "alias", cells, 2, XR_LIR_I64);
    XrLirBlock entry = test_block(m, root, NULL, 0), join = test_block(m, root, cells, 2);
    XrLirBlock body = test_block(m, helper, NULL, 0);
    OK(xr_lir_set_entry(m, root, entry));
    OK(xr_lir_set_entry(m, helper, body));
    XrLirValue a = cell_value(m, entry, XR_LIR_CELL_NEW, test_constant(m, entry, XR_LIR_I64, 7));
    XrLirValue b = cell_value(m, entry, XR_LIR_CELL_NEW, test_constant(m, entry, XR_LIR_I64, 20));
    XrLirValue yes[] = {a, b}, no[] = {b, a};
    conditional(m, entry, parameter(m, root, 0), (XrLirEdge){join, yes, 2}, (XrLirEdge){join, no, 2});
    XrLirValue selected = block_parameter(m, join, 0), other = block_parameter(m, join, 1);
    XrLirValue arguments[] = {selected, selected};
    XrLirValue first = call(m, join, helper, XR_LIR_I64, arguments, 2);
    arguments[0] = arguments[1] = other;
    XrLirValue second = call(m, join, helper, XR_LIR_I64, arguments, 2);
    first = test_binary(m, join, XR_LIR_MUL, first, test_constant(m, join, XR_LIR_I64, 100),
        XR_LIR_PREDICATE_NONE);
    test_return(m, join, test_binary(m, join, XR_LIR_ADD, first, second, XR_LIR_PREDICATE_NONE));
    XrLirValue value = cell_value(m, body, XR_LIR_CELL_READ, parameter(m, helper, 0));
    value = test_binary(m, body, XR_LIR_ADD, value, test_constant(m, body, XR_LIR_I64, 1),
        XR_LIR_PREDICATE_NONE);
    XrLirValue write[] = {parameter(m, helper, 1), value};
    XrLirInstruction instruction = {.op = XR_LIR_CELL_WRITE, .type = XR_LIR_UNIT,
        .operands = write, .operand_count = 2};
    OK(xr_lir_emit_effect(m, body, &instruction));
    test_return(m, body, cell_value(m, body, XR_LIR_CELL_READ, parameter(m, helper, 0)));
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = compile(m, root);
    CHECK(xr_rbc_program_view(program)->functions[0].cell_count == 2);
    XrRbcProgram *rejected = NULL;
    XrRbcCompileDiagnostic diagnostic;
    CHECK(xr_rbc_compile(m, helper, &rejected, &diagnostic) == XR_RBC_COMPILE_UNSUPPORTED && !rejected);
    CHECK(strstr(diagnostic.message, "cell argument"));
    xr_lir_module_destroy(m);
    uint64_t choose = 1;
    XrRbcExecution *first_execution = NULL, *second_execution = NULL;
    CHECK(xr_rbc_program_prepare(program, &choose, 1, &first_execution) == XR_RBC_OK);
    CHECK(xr_rbc_program_prepare(program, &choose, 1, &second_execution) == XR_RBC_OK);
    XrRbcOutcome *first_outcome = xr_rbc_execution_run(first_execution);
    CHECK(!xr_rbc_execution_run(first_execution));
    XrRbcOutcome *second_outcome = xr_rbc_execution_run(second_execution);
    xr_rbc_execution_free(&first_execution);
    xr_rbc_execution_free(&second_execution);
    choose = 0;
    CHECK(run(program, &choose, 1) == 2108);
    xr_rbc_program_destroy(program);
    uint64_t bits = 0;
    CHECK(xr_rbc_outcome_bits(first_outcome, &bits) && bits == 821);
    CHECK(xr_rbc_outcome_bits(second_outcome, &bits) && bits == 821);
    xr_rbc_outcome_free(&first_outcome);
    xr_rbc_outcome_free(&second_outcome);
}


static void test_cell_escape_rejections(void) {
    for (unsigned kind = 0; kind < 3; ++kind) {
        XrLirModule *m = NULL;
        OK(xr_lir_module_create(NULL, &m));
        XrLirFunction root = {0};
        if (kind == 1) {
            XrLirFunctionSpec spec = {.name = "invalid_cell", .name_length = 12,
                .result = XR_LIR_I64_CELL};
            CHECK(xr_lir_declare_function(m, &spec, &root) == XR_LIR_INVALID_ARGUMENT && !root.id);
        } else {
            root = test_function(m, "invalid_cell", NULL, 0, XR_LIR_I64);
            XrLirBlock body = test_block(m, root, NULL, 0);
            OK(xr_lir_set_entry(m, root, body));
            XrLirInstruction instruction = {.op = XR_LIR_CONST, .type = XR_LIR_I64_CELL, .bits = 1};
            XrLirValue rejected = {0};
            if (kind == 0) {
                CHECK(xr_lir_emit_value(m, body, &instruction, &rejected) == XR_LIR_VERIFY_ERROR);
                CHECK(!rejected.id);
            } else {
                XrLirValue value = test_constant(m, body, XR_LIR_I64, 7);
                XrLirValue cell = cell_value(m, body, XR_LIR_CELL_NEW, value);
                instruction = (XrLirInstruction) {.op = XR_LIR_CALL, .type = XR_LIR_UNIT,
                    .callee = test_print_declaration(m), .operands = &cell, .operand_count = 1};
                CHECK(xr_lir_emit_effect(m, body, &instruction) == XR_LIR_VERIFY_ERROR);
            }
        }
        XrRbcProgram *program = NULL;
        CHECK(xr_rbc_compile(m, root, &program, NULL) == XR_RBC_COMPILE_INVALID && !program);
        xr_lir_module_destroy(m);
    }
}


static XrLirModule *literal_module(XrLirFunction *root, unsigned unsigned_use) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    unsigned char bytes[] = {'A', 0, 0xc3, 0xa9};
    XrLirLiteral literal = {0}, unused = {0};
    OK(xr_lir_add_literal(m, bytes, sizeof(bytes), &literal));
    OK(xr_lir_add_literal(m, "unused", 6, &unused));
    memset(bytes, 0xa5, sizeof(bytes));
    XrLirType flag = XR_LIR_I1, types[] = {XR_LIR_PTR, XR_LIR_U64};
    *root = test_function(m, "literal", &flag, 1, XR_LIR_UNIT);
    XrLirFunction print;
    XrLirFunctionSpec spec = {.name="xrt_print_string", .name_length=16,
        .parameters=types, .parameter_count=2, .result=XR_LIR_UNIT,
        .kind=XR_LIR_FUNCTION_EXTERNAL, .extern_kind=XR_LIR_EXTERN_PRINT_STRING};
    OK(xr_lir_declare_function(m, &spec, &print));
    XrLirBlock entry = test_block(m, *root, NULL, 0), output = test_block(m, *root, NULL, 0);
    XrLirBlock finish = test_block(m, *root, NULL, 0);
    OK(xr_lir_set_entry(m, *root, entry));
    conditional(m, entry, parameter(m, *root, 0), (XrLirEdge){output, NULL, 0},
        (XrLirEdge){finish, NULL, 0});
    XrLirValue length = test_constant(m, output, XR_LIR_U64, sizeof(bytes));
    for (unsigned i = 0; i < 2; ++i) {
        XrLirValue args[2] = {{0}, length};
        OK(xr_lir_literal_address(m, output, literal, &args[0]));
        call(m, output, print, XR_LIR_UNIT, args, 2);
    }
    if (unsigned_use == 1 || unsigned_use == 2) {
        XrLirValue args[] = {length, length}, result;
        XrLirInstruction i = {.op=unsigned_use == 1 ? XR_LIR_ICMP : XR_LIR_ADD,
            .type=unsigned_use == 1 ? XR_LIR_I1 : XR_LIR_U64,
            .predicate=unsigned_use == 1 ? XR_LIR_LT : XR_LIR_PREDICATE_NONE,
            .operands=args, .operand_count=2};
        OK(xr_lir_emit_value(m, output, &i, &result));
    }
    if (unsigned_use == 3) {
        XrLirType type = XR_LIR_U64;
        XrLirBlock transport = test_block(m, *root, &type, 1);
        test_branch(m, output, transport, &length, 1);
        test_branch(m, transport, finish, NULL, 0);
    } else test_branch(m, output, finish, NULL, 0);
    test_return(m, finish, (XrLirValue){0});
    OK(xr_lir_module_finish(m, NULL));
    return m;
}

static void test_literal_ownership(void) {
    XrLirFunction root;
    XrLirModule *m = literal_module(&root, false);
    XrRbcProgram *program = compile(m, root);
    const XrRbcModule *module = xr_rbc_program_view(program);
    CHECK(module->literal_count == 1 && module->literals[0].length == 4);
    const unsigned char expected[] = {'A', 0, 0xc3, 0xa9};
    CHECK(!memcmp(module->literals[0].bytes, expected, sizeof(expected)));
    uint32_t prints = 0;
    for (size_t pc = 0; pc < module->functions[0].code_count; ++pc)
        if ((module->functions[0].code[pc] & 255) == XR_RBC_PRINT_LITERAL) {
            CHECK((module->functions[0].code[pc] >> 8) == 0);++prints;
        }
    CHECK(prints == 2);
    XrLirLiteral input;
    const uint8_t *bytes = NULL;size_t length = 0;
    OK(xr_lir_literal_at(m, 0, &input));OK(xr_lir_literal_view(m, input, &bytes, &length));
    CHECK((const void *)bytes != (const void *)module->literals[0].bytes);
    xr_lir_module_destroy(m);
    CHECK(!memcmp(module->literals[0].bytes, expected, sizeof(expected)));
    XrRbcOutcome *outcomes[2];
    for (unsigned i = 0; i < 2; ++i) {
        uint64_t flag = 0;
        XrRbcExecution *execution = NULL;
        CHECK(xr_rbc_program_prepare(program, &flag, 1, &execution) == XR_RBC_OK);
        outcomes[i] = xr_rbc_execution_run(execution);
        CHECK(outcomes[i] && !xr_rbc_execution_run(execution));
        xr_rbc_execution_free(&execution);
    }
    xr_rbc_program_destroy(program);
    for (unsigned i = 0; i < 2; ++i) {
        uint64_t bits = 99;
        CHECK(xr_rbc_outcome_bits(outcomes[i], &bits) && !bits);
        xr_rbc_outcome_free(&outcomes[i]);
    }
    program = NULL;
    for (unsigned i = 1; i <= 3; ++i) {
        m = literal_module(&root, i);
        CHECK(xr_rbc_compile(m, root, &program, NULL) == XR_RBC_COMPILE_UNSUPPORTED && !program);
        xr_lir_module_destroy(m);
    }
    XrLirType types[] = {XR_LIR_PTR, XR_LIR_U64};
    for (unsigned i = 0; i < 2; ++i) {
        m = NULL;OK(xr_lir_module_create(NULL, &m));
        root = test_function(m, "opaque_argument", &types[i], 1, XR_LIR_UNIT);
        XrLirBlock b = test_block(m, root, NULL, 0);OK(xr_lir_set_entry(m, root, b));
        test_return(m, b, (XrLirValue){0});OK(xr_lir_module_finish(m, NULL));
        CHECK(xr_rbc_compile(m, root, &program, NULL) == XR_RBC_COMPILE_UNSUPPORTED && !program);
        xr_lir_module_destroy(m);
    }
}

static void test_string_edge_swap(void) {
    XrLirModule *m = NULL; OK(xr_lir_module_create(NULL,&m));
    XrLirType flag = XR_LIR_I1, types[] = {XR_LIR_STRING,XR_LIR_STRING};
    XrLirFunction f = test_function(m,"string_swap",&flag,1,XR_LIR_STRING);
    XrLirBlock entry=test_block(m,f,NULL,0), left=test_block(m,f,types,2),
        right=test_block(m,f,types,2), join=test_block(m,f,types,2);
    OK(xr_lir_set_entry(m,f,entry));
    XrLirValue values[2];
    for (unsigned i=0;i<2;++i) {
        XrLirLiteral literal = {0}; const char *text=i?"B":"A"; OK(xr_lir_add_literal(m,text,1,&literal));
        XrLirInstruction v={.op=XR_LIR_STRING_LITERAL,.type=XR_LIR_STRING,.bits=literal.id};
        OK(xr_lir_emit_value(m,entry,&v,&values[i]));
    }
    conditional(m,entry,parameter(m,f,0),(XrLirEdge){left,values,2},(XrLirEdge){right,values,2});
    XrLirValue order[]={block_parameter(m,left,0),block_parameter(m,left,1)};
    test_branch(m,left,join,order,2);
    order[0]=block_parameter(m,right,1);order[1]=block_parameter(m,right,0);test_branch(m,right,join,order,2);
    order[0]=block_parameter(m,join,0);order[1]=block_parameter(m,join,1);
    XrLirInstruction concat={.op=XR_LIR_STRING_CONCAT,.type=XR_LIR_STRING,.operands=order,.operand_count=2};
    XrLirValue result; OK(xr_lir_emit_value(m,join,&concat,&result));
    for (unsigned i=0;i<2;++i) {
        XrLirInstruction drop={.op=XR_LIR_STRING_DROP,.type=XR_LIR_UNIT,.operands=&order[i],.operand_count=1};
        OK(xr_lir_emit_effect(m,join,&drop));
    }
    test_return(m,join,result);OK(xr_lir_module_finish(m,NULL));
    XrRbcProgram *program=compile(m,f);xr_lir_module_destroy(m);
    for (uint64_t flag_value=0;flag_value<2;++flag_value) {
        XrRbcExecution *execution=NULL;CHECK(xr_rbc_program_prepare(program,&flag_value,1,&execution)==XR_RBC_OK);
        XrRbcOutcome *outcome=xr_rbc_execution_run(execution);CHECK(outcome&&xr_rbc_outcome_status(outcome)==XR_RBC_OK);
        xr_rbc_execution_free(&execution);
        if(flag_value){xr_rbc_program_destroy(program);program=NULL;}
        const XrtString *text=NULL;CHECK(xr_rbc_outcome_string(outcome,&text));
        size_t size=0;const char *bytes=xrt_string_bytes(text,&size);
        CHECK(size==2&&!memcmp(bytes,flag_value?"AB":"BA",2));xr_rbc_outcome_free(&outcome);
    }
}
int main(void) {
    test_string_edge_swap();
    test_literal_ownership();
    test_cell_escape_rejections();
    test_cell_transport();
    test_edge_moves();
    test_owned_arithmetic_panics();
    test_i64_identity_cast();
    test_recursive_and_loop();
    test_location_ownership();
    test_comparisons();
    test_arithmetic();
    test_constants_and_bool_calls();
    test_parallel_cycle();
    test_same_target_edges();
    test_call_windows();
    test_capacity(255, false);
    test_capacity(256, false);
    test_capacity(254, true);
    test_capacity(255, true);
    test_call_capacity(253);
    test_call_capacity(254);
    test_unused_and_invalid();
    test_owned_diagnostic_and_rejection();
    test_f64_constants_and_refusals();
    puts("lir_bytecode: passed");
    return 0;
}
