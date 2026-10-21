/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_test_programs.h - Hand-built scalar SSA fixtures
 */
#ifndef LIR_TEST_PROGRAMS_H
#define LIR_TEST_PROGRAMS_H
#include "lir/xlir_builder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                        \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
#define OK(expression) CHECK((expression) == XR_LIR_OK)

static XrLirFunction test_function(XrLirModule *m, const char *name, const XrLirType *parameters,
                                   uint32_t count, XrLirType result) {
    XrLirFunction f = {0};
    XrLirFunctionSpec spec = {.name = name,
                              .name_length = strlen(name),
                              .parameters = parameters,
                              .parameter_count = count,
                              .result = result};
    OK(xr_lir_declare_function(m, &spec, &f));
    OK(xr_lir_define_function(m, f));
    return f;
}
static XrLirBlock test_block(XrLirModule *m, XrLirFunction f, const XrLirType *types,
                             uint32_t count) {
    XrLirBlock block = {0};
    OK(xr_lir_add_block(m, f, types, count, &block));
    return block;
}
static XrLirValue test_constant(XrLirModule *m, XrLirBlock block, XrLirType type, uint64_t bits) {
    XrLirValue result = {0};
    XrLirInstruction instruction = {.op = XR_LIR_CONST, .type = type, .bits = bits};
    OK(xr_lir_emit_value(m, block, &instruction, &result));
    return result;
}
static XrLirValue test_binary(XrLirModule *m, XrLirBlock block, XrLirOp op, XrLirValue a,
                              XrLirValue b, XrLirPredicate predicate) {
    XrLirValue operands[] = {a, b}, result = {0};
    XrLirInstruction instruction = {.op = op,
                                    .type = op == XR_LIR_ICMP ? XR_LIR_I1 : XR_LIR_I64,
                                    .predicate = predicate,
                                    .operands = operands,
                                    .operand_count = 2};
    OK(xr_lir_emit_value(m, block, &instruction, &result));
    return result;
}
static void test_return(XrLirModule *m, XrLirBlock block, XrLirValue value) {
    XrLirInstruction instruction = {.op = XR_LIR_RET,
                                    .type = XR_LIR_UNIT,
                                    .operands = value.id ? &value : NULL,
                                    .operand_count = value.id ? 1u : 0u};
    OK(xr_lir_set_terminator(m, block, &instruction));
}
static void test_branch(XrLirModule *m, XrLirBlock block, XrLirBlock target,
                        const XrLirValue *arguments, uint32_t count) {
    XrLirInstruction instruction = {.op = XR_LIR_BR,
                                    .type = XR_LIR_UNIT,
                                    .edges = {{target, arguments, count}},
                                    .edge_count = 1};
    OK(xr_lir_set_terminator(m, block, &instruction));
}
static XrLirValue test_call(XrLirModule *m, XrLirBlock block, XrLirFunction callee,
                            XrLirValue argument) {
    XrLirValue result = {0};
    XrLirInstruction instruction = {.op = XR_LIR_CALL,
                                    .type = XR_LIR_I64,
                                    .callee = callee,
                                    .operands = &argument,
                                    .operand_count = 1};
    OK(xr_lir_emit_value(m, block, &instruction, &result));
    return result;
}
static XrLirFunction test_print_declaration(XrLirModule *m) {
    XrLirType type = XR_LIR_I64;
    XrLirFunctionSpec spec = {.name = "xrt_print_i64",
                              .name_length = 13,
                              .parameters = &type,
                              .parameter_count = 1,
                              .result = XR_LIR_UNIT,
                              .kind = XR_LIR_FUNCTION_EXTERNAL,
                              .extern_kind = XR_LIR_EXTERN_PRINT_I64};
    XrLirFunction result = {0};
    OK(xr_lir_declare_function(m, &spec, &result));
    return result;
}
static void test_main(XrLirModule *m, XrLirFunction print, XrLirFunction callee, uint64_t input) {
    XrLirFunction main = test_function(m, "main", NULL, 0, XR_LIR_UNIT);
    XrLirBlock entry = test_block(m, main, NULL, 0);
    OK(xr_lir_set_entry(m, main, entry));
    XrLirValue value = test_constant(m, entry, XR_LIR_I64, input);
    XrLirValue result = test_call(m, entry, callee, value);
    XrLirInstruction instruction = {.op = XR_LIR_CALL,
                                    .type = XR_LIR_UNIT,
                                    .callee = print,
                                    .operands = &result,
                                    .operand_count = 1};
    OK(xr_lir_emit_effect(m, entry, &instruction));
    test_return(m, entry, (XrLirValue) {0});
}

static XrLirModule *test_fib(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirFunction print = test_print_declaration(m);
    XrLirType type = XR_LIR_I64;
    XrLirFunction fib = test_function(m, "fib", &type, 1, type);
    XrLirValue n = {0};
    OK(xr_lir_function_parameter(m, fib, 0, &n));
    XrLirBlock entry = test_block(m, fib, NULL, 0), base = test_block(m, fib, NULL, 0),
               recur = test_block(m, fib, NULL, 0);
    OK(xr_lir_set_entry(m, fib, entry));
    XrLirValue two = test_constant(m, entry, type, 2);
    XrLirValue condition = test_binary(m, entry, XR_LIR_ICMP, n, two, XR_LIR_LT);
    XrLirInstruction branch = {.op = XR_LIR_CONDBR,
                               .type = XR_LIR_UNIT,
                               .operands = &condition,
                               .operand_count = 1,
                               .edges = {{.target = base}, {.target = recur}},
                               .edge_count = 2};
    OK(xr_lir_set_terminator(m, entry, &branch));
    test_return(m, base, n);
    XrLirValue one = test_constant(m, recur, type, 1);
    XrLirValue previous = test_binary(m, recur, XR_LIR_SUB, n, one, XR_LIR_PREDICATE_NONE);
    XrLirValue a = test_call(m, recur, fib, previous);
    XrLirValue earlier = test_binary(m, recur, XR_LIR_SUB, n, two, XR_LIR_PREDICATE_NONE);
    XrLirValue b = test_call(m, recur, fib, earlier);
    XrLirValue result = test_binary(m, recur, XR_LIR_ADD, a, b, XR_LIR_PREDICATE_NONE);
    test_return(m, recur, result);
    test_main(m, print, fib, 32);
    OK(xr_lir_module_finish(m, NULL));
    return m;
}

static XrLirModule *test_sum(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirFunction print = test_print_declaration(m);
    XrLirType type = XR_LIR_I64, pair[] = {XR_LIR_I64, XR_LIR_I64};
    XrLirFunction sum = test_function(m, "sum", &type, 1, type);
    XrLirValue n = {0}, i = {0}, total = {0}, answer = {0};
    OK(xr_lir_function_parameter(m, sum, 0, &n));
    XrLirBlock entry = test_block(m, sum, NULL, 0), loop = test_block(m, sum, pair, 2);
    XrLirBlock body = test_block(m, sum, NULL, 0), done = test_block(m, sum, &type, 1);
    OK(xr_lir_set_entry(m, sum, entry));
    OK(xr_lir_block_parameter(m, loop, 0, &i));
    OK(xr_lir_block_parameter(m, loop, 1, &total));
    OK(xr_lir_block_parameter(m, done, 0, &answer));
    XrLirValue zero = test_constant(m, entry, type, 0), one = test_constant(m, entry, type, 1);
    XrLirValue initial[] = {zero, zero};
    test_branch(m, entry, loop, initial, 2);
    XrLirValue condition = test_binary(m, loop, XR_LIR_ICMP, i, n, XR_LIR_LT);
    XrLirInstruction branch = {.op = XR_LIR_CONDBR,
                               .type = XR_LIR_UNIT,
                               .operands = &condition,
                               .operand_count = 1,
                               .edges = {{.target = body}, {done, &total, 1}},
                               .edge_count = 2};
    OK(xr_lir_set_terminator(m, loop, &branch));
    XrLirValue next_total = test_binary(m, body, XR_LIR_ADD, total, i, XR_LIR_PREDICATE_NONE);
    XrLirValue next_i = test_binary(m, body, XR_LIR_ADD, i, one, XR_LIR_PREDICATE_NONE);
    XrLirValue carried[] = {next_i, next_total};
    test_branch(m, body, loop, carried, 2);
    test_return(m, done, answer);
    test_main(m, print, sum, UINT64_C(100000000));
    OK(xr_lir_module_finish(m, NULL));
    return m;
}

#endif  // LIR_TEST_PROGRAMS_H
