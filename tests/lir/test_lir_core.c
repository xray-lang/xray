/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_core.c - Scalar SSA construction, ownership and rejection tests
 */
#include "lir_test_programs.h"
#include "lir/xlir_internal.h"
#include "lir/xlir_verify.h"
#include "lir/xlir_rebuild.h"
#include "lir/xlir_inline.h"
#include "lir/xlir_sccp.h"
#include "base/xmalloc.h"

static void queries(void) {
    XrLirModule *fib = test_fib(), *sum = test_sum();
    OK(xr_lir_verify(fib, NULL));
    OK(xr_lir_verify(sum, NULL));
    CHECK(xr_lir_function_count(fib) == 3);
    XrLirFunction function = {0};
    XrLirFunctionView view;
    OK(xr_lir_function_at(fib, 0, &function));
    OK(xr_lir_function_view(fib, function, &view));
    CHECK(view.kind == XR_LIR_FUNCTION_EXTERNAL && view.extern_kind == XR_LIR_EXTERN_PRINT_I64 &&
          view.parameter_count == 1 && view.parameter_types[0] == XR_LIR_I64 &&
          view.result == XR_LIR_UNIT);
    OK(xr_lir_function_at(fib, 1, &function));
    OK(xr_lir_function_view(fib, function, &view));
    CHECK(view.entry == 1 && view.block_count == 3 && !memcmp(view.name, "fib", 3));
    XrLirBlock block = {0};
    XrLirBlockView block_view;
    XrLirInstructionView instruction;
    OK(xr_lir_block_at(fib, function, 0, &block));
    OK(xr_lir_block_view(fib, block, &block_view));
    CHECK(block_view.instruction_count == 3 && block_view.parameter_count == 0);
    OK(xr_lir_instruction_view(fib, block, 1, &instruction));
    CHECK(instruction.op == XR_LIR_ICMP && instruction.predicate == XR_LIR_LT &&
          instruction.type == XR_LIR_I1);
    XrLirType type = XR_LIR_UNIT;
    OK(xr_lir_value_type(fib, (XrLirValue) {fib, function.id, instruction.result}, &type));
    CHECK(type == XR_LIR_I1);
    CHECK(xr_lir_function_at(fib, 3, &function) == XR_LIR_INVALID_ARGUMENT && !function.id);
    CHECK(xr_lir_instruction_view(fib, block, UINT32_MAX, &instruction) ==
              XR_LIR_INVALID_ARGUMENT &&
          !instruction.op);
    CHECK(xr_lir_define_function(fib, (XrLirFunction) {fib, 2}) == XR_LIR_INVALID_STATE);
    OK(xr_lir_verify(fib, NULL));
    xr_lir_module_destroy(fib);
    xr_lir_module_destroy(sum);
}

static XrLirModule *simple(XrLirFunction *function, XrLirBlock *entry, XrLirValue *parameter) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType type = XR_LIR_I64;
    *function = test_function(m, "identity", &type, 1, type);
    *entry = test_block(m, *function, NULL, 0);
    OK(xr_lir_set_entry(m, *function, *entry));
    OK(xr_lir_function_parameter(m, *function, 0, parameter));
    return m;
}

static void builder_rejections(void) {
    for (unsigned scenario = 0; scenario < 7; ++scenario) {
        XrLirFunction f;
        XrLirBlock entry;
        XrLirValue argument, output = {0};
        XrLirModule *m = simple(&f, &entry, &argument);
        XrLirInstruction instruction = {.op = XR_LIR_ADD, .type = XR_LIR_I64, .operand_count = 2};
        XrLirValue operands[] = {argument, argument};
        instruction.operands = operands;
        if (scenario == 0)
            instruction.type = XR_LIR_F64;
        if (scenario == 1)
            operands[1].id = 999;
        if (scenario == 2) {
            instruction.op = XR_LIR_CALL;
            instruction.callee = f;
            instruction.operand_count = 0;
        }
        if (scenario == 3) {
            instruction.op = XR_LIR_CALL_INTRINSIC;
            instruction.intrinsic = 17;
            instruction.operand_count = 0;
        }
        if (scenario == 4) {
            instruction.op = XR_LIR_CONST;
            instruction.type = XR_LIR_I1;
            instruction.bits = 2;
            instruction.operand_count = 0;
        }
        if (scenario == 5) {
            instruction.op = XR_LIR_CONST;
            instruction.type = XR_LIR_I8;
            instruction.bits = 256;
            instruction.operand_count = 0;
        }
        if (scenario == 6) {
            instruction.op = XR_LIR_CONST;
            instruction.type = XR_LIR_F64;
            instruction.bits = UINT64_C(0x7ff0000000000001);
            instruction.operand_count = 0;
        }
        CHECK(xr_lir_emit_value(m, entry, &instruction, &output) != XR_LIR_OK && !output.id);
        CHECK(xr_lir_module_finish(m, NULL) != XR_LIR_OK);
        xr_lir_module_destroy(m);
    }
    XrLirFunction f;
    XrLirBlock entry;
    XrLirValue value;
    XrLirModule *m = simple(&f, &entry, &value);
    CHECK(xr_lir_module_finish(m, NULL) == XR_LIR_VERIFY_ERROR);
    xr_lir_module_destroy(m);
    m = simple(&f, &entry, &value);
    test_return(m, entry, value);
    XrLirInstruction instruction = {.op = XR_LIR_CONST, .type = XR_LIR_I64};
    CHECK(xr_lir_emit_value(m, entry, &instruction, &value) == XR_LIR_INVALID_STATE && !value.id);
    xr_lir_module_destroy(m);
}

static void foreign_handles(void) {
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        XrLirFunction f, other;
        XrLirBlock entry, foreign;
        XrLirValue argument, other_argument;
        XrLirModule *m = simple(&f, &entry, &argument),
                    *different = simple(&other, &foreign, &other_argument);
        XrLirType type = XR_LIR_I64;
        XrLirFunction second = test_function(m, "second", &type, 1, type);
        XrLirValue second_argument = {0};
        OK(xr_lir_function_parameter(m, second, 0, &second_argument));
        CHECK(argument.id == second_argument.id && argument.id == other_argument.id);
        XrLirValue operands[] = {argument, scenario == 0 ? second_argument : other_argument},
                   output = {0};
        XrLirInstruction instruction = {
            .op = XR_LIR_ADD, .type = type, .operands = operands, .operand_count = 2};
        XrLirStatus status;
        if (scenario < 2)
            status = xr_lir_emit_value(m, entry, &instruction, &output);
        else if (scenario == 2) {
            instruction = (XrLirInstruction) {.op = XR_LIR_CALL,
                                              .type = type,
                                              .callee = other,
                                              .operands = &argument,
                                              .operand_count = 1};
            status = xr_lir_emit_value(m, entry, &instruction, &output);
        } else if (scenario == 3)
            status = xr_lir_set_entry(m, second, foreign);
        else {
            instruction = (XrLirInstruction) {.op = XR_LIR_BR,
                                              .type = XR_LIR_UNIT,
                                              .edges = {{.target = foreign}},
                                              .edge_count = 1};
            status = xr_lir_set_terminator(m, entry, &instruction);
        }
        CHECK(status != XR_LIR_OK && !output.id && xr_lir_module_finish(m, NULL) != XR_LIR_OK);
        xr_lir_module_destroy(m);
        xr_lir_module_destroy(different);
    }
}

static void verifier_rejections(void) {
    XrLirModule *m = test_fib();
    XrLirDiagnostic d;
    XrLirInstructionView *constant = &m->functions[1].blocks[0].instructions[0].view;
    constant->type = XR_LIR_F64;
    CHECK(xr_lir_verify(m, &d) == XR_LIR_VERIFY_ERROR && d.function == 2);
    constant->type = XR_LIR_I64;
    XrLirInstructionView *comparison = &m->functions[1].blocks[0].instructions[1].view;
    uint32_t *arguments = (uint32_t *) comparison->operands, saved = arguments[0];
    arguments[0] = UINT32_MAX;
    CHECK(xr_lir_verify(m, &d) == XR_LIR_VERIFY_ERROR);
    arguments[0] = saved;
    XrLirInstructionView *call = &m->functions[1].blocks[2].instructions[2].view;
    CHECK(call->op == XR_LIR_CALL);
    call->type = XR_LIR_F64;
    CHECK(xr_lir_verify(m, &d) == XR_LIR_VERIFY_ERROR);
    call->type = XR_LIR_I64;
    --m->functions[1].blocks[0].instruction_count;
    CHECK(xr_lir_verify(m, &d) == XR_LIR_VERIFY_ERROR);
    ++m->functions[1].blocks[0].instruction_count;
    OK(xr_lir_verify(m, &d));
    xr_lir_module_destroy(m);
    m = test_sum();
    XrLirEdgeView *edge = &m->functions[1].blocks[0].instructions[2].view.edges[0];
    --edge->argument_count;
    CHECK(xr_lir_verify(m, &d) == XR_LIR_VERIFY_ERROR);
    ++edge->argument_count;
    XrLirInstructionView *add = &m->functions[1].blocks[2].instructions[0].view;
    arguments = (uint32_t *) add->operands;
    saved = arguments[0];
    arguments[0] = add->result;
    CHECK(xr_lir_verify(m, &d) == XR_LIR_VERIFY_ERROR && strstr(d.message, "dominate"));
    arguments[0] = saved;
    OK(xr_lir_verify(m, &d));
    xr_lir_module_destroy(m);
}

static void definition_prevalidation_rejections(void) {
    XrLirModule *m = test_fib();
    XrLirFunctionData *f = &m->functions[1];
    XrLirBlockData *later = &f->blocks[2];
    XrLirInstructionView *comparison = &f->blocks[0].instructions[1].view;
    uint32_t *arguments = (uint32_t *)comparison->operands;
    uint32_t original = arguments[0], target = later->instructions[2].view.result;
    XrLirValueData *value = &f->values[target - 1];
    CHECK(value->block == 3 && value->instruction == 2);
    CHECK(xr_lir_internal_value_definition(f, target) == &later->instructions[2].view);
    CHECK(!xr_lir_internal_value_definition(NULL, target));
    arguments[0] = target;
    XrLirDiagnostic diagnostic = {0};
    XrLirInstructionData *storage = later->instructions;
    later->instructions = NULL;
    CHECK(!xr_lir_internal_value_definition(f, target));
    CHECK(xr_lir_verify(m, &diagnostic) == XR_LIR_VERIFY_ERROR &&
          diagnostic.status == XR_LIR_VERIFY_ERROR && diagnostic.function == 2 &&
          diagnostic.block == 3 && diagnostic.instruction == 0);
    later->instructions = storage;
    uint32_t count = later->instruction_count, coordinate = value->instruction;
    CHECK(later->instruction_capacity < UINT32_MAX);
    later->instruction_count = later->instruction_capacity + 1;
    value->instruction = later->instruction_capacity;
    CHECK(!xr_lir_internal_value_definition(f, target));
    CHECK(xr_lir_verify(m, &diagnostic) == XR_LIR_VERIFY_ERROR &&
          diagnostic.status == XR_LIR_VERIFY_ERROR && diagnostic.function == 2 &&
          diagnostic.block == 3 && diagnostic.instruction == 0);
    later->instruction_count = count;
    value->instruction = count;
    CHECK(!xr_lir_internal_value_definition(f, target));
    CHECK(xr_lir_verify(m, &diagnostic) == XR_LIR_VERIFY_ERROR && diagnostic.function == 2);
    value->instruction = coordinate;
    CHECK(xr_lir_verify(m, &diagnostic) == XR_LIR_VERIFY_ERROR &&
          strstr(diagnostic.message, "dominate"));
    arguments[0] = original;
    OK(xr_lir_verify(m, &diagnostic));
    xr_lir_module_destroy(m);
    puts("cross-block definition storage rejects before lookup; restored dominance remains enforced");
}

static void graph_edges(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType boolean = XR_LIR_I1, integer = XR_LIR_I64;
    XrLirFunction f = test_function(m, "choose", &boolean, 1, integer);
    XrLirBlock join = test_block(m, f, &integer, 1), entry = test_block(m, f, NULL, 0);
    OK(xr_lir_set_entry(m, f, entry));
    XrLirValue condition = {0}, answer = {0};
    OK(xr_lir_function_parameter(m, f, 0, &condition));
    OK(xr_lir_block_parameter(m, join, 0, &answer));
    XrLirValue a = test_constant(m, entry, integer, 11), b = test_constant(m, entry, integer, 29);
    XrLirInstruction branch = {.op = XR_LIR_CONDBR,
                               .type = XR_LIR_UNIT,
                               .operands = &condition,
                               .operand_count = 1,
                               .edges = {{join, &a, 1}, {join, &b, 1}},
                               .edge_count = 2};
    OK(xr_lir_set_terminator(m, entry, &branch));
    test_return(m, join, answer);
    OK(xr_lir_module_finish(m, NULL));
    OK(xr_lir_verify(m, NULL));
    XrLirInstructionView view;
    OK(xr_lir_instruction_view(m, entry, 2, &view));
    CHECK(view.edges[0].target == view.edges[1].target &&
          view.edges[0].arguments[0] != view.edges[1].arguments[0]);
    xr_lir_module_destroy(m);
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        XrLirValue parameter;
        m = simple(&f, &entry, &parameter);
        if (scenario == 0)
            test_branch(m, entry, entry, NULL, 0);
        else if (scenario == 1) {
            test_return(m, entry, parameter);
            XrLirBlock unreachable = test_block(m, f, NULL, 0);
            test_return(m, unreachable, parameter);
        } else {
            XrLirBlock left = test_block(m, f, NULL, 0), right = test_block(m, f, NULL, 0),
                       merge = test_block(m, f, NULL, 0);
            condition = test_constant(m, entry, XR_LIR_I1, 1);
            branch = (XrLirInstruction) {.op = XR_LIR_CONDBR,
                                         .type = XR_LIR_UNIT,
                                         .operands = &condition,
                                         .operand_count = 1,
                                         .edges = {{.target = left}, {.target = right}},
                                         .edge_count = 2};
            OK(xr_lir_set_terminator(m, entry, &branch));
            a = test_constant(m, left, integer, 7);
            test_branch(m, left, merge, NULL, 0);
            test_branch(m, right, merge, NULL, 0);
            test_return(m, merge, a);
        }
        XrLirStatus status = xr_lir_module_finish(m, NULL);
        CHECK(status == XR_LIR_OK || status == XR_LIR_VERIFY_ERROR);
        if (status == XR_LIR_OK)
            CHECK(xr_lir_verify(m, NULL) == XR_LIR_VERIFY_ERROR);
        else
            CHECK(!m->finished);
        xr_lir_module_destroy(m);
    }
}

static void owned_inputs_and_growth(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    char *name = xr_malloc(6), *path = xr_malloc(10);
    XrLirType *type = xr_malloc(sizeof(*type));
    CHECK(name && path && type);
    memcpy(name, "owned", 6);
    memcpy(path, "source.xr", 10);
    *type = XR_LIR_I64;
    XrLirFunctionSpec spec = {
        .name = name, .name_length = 5, .parameters = type, .parameter_count = 1, .result = *type};
    XrLirFunction f = {0};
    OK(xr_lir_declare_function(m, &spec, &f));
    memset(name, 'x', 5);
    *type = XR_LIR_F64;
    xr_free(name);
    xr_free(type);
    OK(xr_lir_define_function(m, f));
    XrLirBlock entry = test_block(m, f, NULL, 0);
    OK(xr_lir_set_entry(m, f, entry));
    XrLirValue parameter = {0};
    OK(xr_lir_function_parameter(m, f, 0, &parameter));
    XrLirInstruction constant = {
        .op = XR_LIR_CONST, .type = XR_LIR_I64, .source = {path, 9, 2, 3, 2, 8}};
    XrLirValue result = {0};
    OK(xr_lir_emit_value(m, entry, &constant, &result));
    memset(path, 'z', 9);
    xr_free(path);
    for (uint32_t i = 0; i < 300; ++i)
        result = test_constant(m, entry, XR_LIR_I64, i);
    test_return(m, entry, parameter);
    for (uint32_t i = 0; i < 40; ++i) {
        char other_name[32];
        snprintf(other_name, sizeof(other_name), "extra_%u", i);
        XrLirFunction other = test_function(m, other_name, NULL, 0, XR_LIR_UNIT);
        XrLirBlock block = test_block(m, other, NULL, 0);
        OK(xr_lir_set_entry(m, other, block));
        test_return(m, block, (XrLirValue) {0});
    }
    OK(xr_lir_module_finish(m, NULL));
    OK(xr_lir_verify(m, NULL));
    XrLirFunctionView view;
    OK(xr_lir_function_view(m, f, &view));
    CHECK(!strcmp(view.name, "owned") && view.parameter_types[0] == XR_LIR_I64 &&
          view.value_count > 255);
    XrLirInstructionView instruction;
    OK(xr_lir_instruction_view(m, entry, 0, &instruction));
    CHECK(!strcmp(instruction.source.path, "source.xr") && instruction.source.line == 2 &&
          instruction.source.column == 3);
    XrLirType actual;
    OK(xr_lir_value_type(m, result, &actual));
    CHECK(actual == XR_LIR_I64);
    xr_lir_module_destroy(m);
}

static void constants_and_swap(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirFunction f = test_function(m, "constants", NULL, 0, XR_LIR_UNIT);
    XrLirBlock entry = test_block(m, f, NULL, 0);
    OK(xr_lir_set_entry(m, f, entry));
    const uint64_t bits[] = {1,
                             128,
                             32768,
                             UINT64_C(0x80000000),
                             UINT64_C(0x8000000000000000),
                             255,
                             65535,
                             UINT64_C(0xffffffff),
                             UINT64_MAX,
                             UINT64_C(0x80000000),
                             UINT64_C(0x7ff8000000000000),
                             0};
    for (unsigned i = 0; i < sizeof(bits) / sizeof(bits[0]); ++i)
        test_constant(m, entry, (XrLirType) (XR_LIR_I1 + i), bits[i]);
    test_return(m, entry, (XrLirValue) {0});
    OK(xr_lir_module_finish(m, NULL));
    OK(xr_lir_verify(m, NULL));
    for (unsigned i = 0; i < sizeof(bits) / sizeof(bits[0]); ++i) {
        XrLirInstructionView instruction;
        OK(xr_lir_instruction_view(m, entry, i, &instruction));
        CHECK(instruction.op == XR_LIR_CONST && instruction.type == (XrLirType) (XR_LIR_I1 + i) &&
              instruction.bits == bits[i]);
    }
    xr_lir_module_destroy(m);
    m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType scalar = XR_LIR_I64, types[] = {scalar, scalar, scalar};
    f = test_function(m, "swap", &scalar, 1, scalar);
    entry = test_block(m, f, NULL, 0);
    XrLirBlock loop = test_block(m, f, types, 3), body = test_block(m, f, NULL, 0),
               done = test_block(m, f, &scalar, 1);
    OK(xr_lir_set_entry(m, f, entry));
    XrLirValue n = {0}, a = {0}, b = {0}, count = {0}, answer = {0};
    OK(xr_lir_function_parameter(m, f, 0, &n));
    OK(xr_lir_block_parameter(m, loop, 0, &a));
    OK(xr_lir_block_parameter(m, loop, 1, &b));
    OK(xr_lir_block_parameter(m, loop, 2, &count));
    OK(xr_lir_block_parameter(m, done, 0, &answer));
    XrLirValue zero = test_constant(m, entry, scalar, 0), one = test_constant(m, entry, scalar, 1);
    XrLirValue initial[] = {zero, one, n};
    test_branch(m, entry, loop, initial, 3);
    XrLirValue condition = test_binary(m, loop, XR_LIR_ICMP, count, zero, XR_LIR_GT);
    XrLirInstruction branch = {.op = XR_LIR_CONDBR,
                               .type = XR_LIR_UNIT,
                               .operands = &condition,
                               .operand_count = 1,
                               .edges = {{.target = body}, {done, &a, 1}},
                               .edge_count = 2};
    OK(xr_lir_set_terminator(m, loop, &branch));
    XrLirValue remaining = test_binary(m, body, XR_LIR_SUB, count, one, XR_LIR_PREDICATE_NONE);
    XrLirValue carried[] = {b, a, remaining};
    test_branch(m, body, loop, carried, 3);
    memset(carried, 0, sizeof(carried));
    test_return(m, done, answer);
    OK(xr_lir_module_finish(m, NULL));
    OK(xr_lir_verify(m, NULL));
    XrLirInstructionView backedge;
    OK(xr_lir_instruction_view(m, body, 1, &backedge));
    CHECK(backedge.edges[0].arguments[0] == b.id && backedge.edges[0].arguments[1] == a.id);
    xr_lir_module_destroy(m);
}

static void first_failure_diagnostic(void) {
    XrLirFunction f;
    XrLirBlock entry;
    XrLirValue parameter, output = {0};
    XrLirModule *m = simple(&f, &entry, &parameter);
    char *path = xr_malloc(12);
    CHECK(path != NULL);
    memcpy(path, "bad-call.xr", 12);
    XrLirInstruction invalid = {
        .op = XR_LIR_CALL, .type = XR_LIR_I64, .callee = f, .source = {path, 11, 7, 5, 7, 17}};
    CHECK(xr_lir_emit_value(m, entry, &invalid, &output) == XR_LIR_VERIFY_ERROR && !output.id);
    memset(path, 'x', 11);
    xr_free(path);
    XrLirDiagnostic diagnostic;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        memset(&diagnostic, 0xa5, sizeof(diagnostic));
        CHECK(xr_lir_module_finish(m, &diagnostic) == XR_LIR_VERIFY_ERROR);
        CHECK(diagnostic.status == XR_LIR_VERIFY_ERROR && diagnostic.function == f.id &&
              diagnostic.block == entry.id && diagnostic.instruction == 1);
        CHECK(diagnostic.source.path && !strcmp(diagnostic.source.path, "bad-call.xr") &&
              diagnostic.source.line == 7 && diagnostic.source.column == 5 &&
              diagnostic.source.end_line == 7 && diagnostic.source.end_column == 17);
        CHECK(strstr(diagnostic.message, "signature"));
    }
    invalid = (XrLirInstruction) {.op = XR_LIR_CONST, .type = XR_LIR_I64};
    CHECK(xr_lir_emit_value(m, entry, &invalid, &output) == XR_LIR_VERIFY_ERROR);
    CHECK(xr_lir_module_finish(m, &diagnostic) == XR_LIR_VERIFY_ERROR &&
          !strcmp(diagnostic.source.path, "bad-call.xr"));
    xr_lir_module_destroy(m);
    memset(&diagnostic, 0xa5, sizeof(diagnostic));
    CHECK(xr_lir_module_finish(NULL, &diagnostic) == XR_LIR_INVALID_ARGUMENT &&
          diagnostic.status == XR_LIR_INVALID_ARGUMENT && !diagnostic.source.path &&
          !diagnostic.function);
    memset(&diagnostic, 0xa5, sizeof(diagnostic));
    CHECK(xr_lir_verify(NULL, &diagnostic) == XR_LIR_INVALID_ARGUMENT &&
          diagnostic.status == XR_LIR_INVALID_ARGUMENT && !diagnostic.source.path);
    m = simple(&f, &entry, &parameter);
    CHECK(xr_lir_module_finish(m, NULL) == XR_LIR_VERIFY_ERROR);
    CHECK(xr_lir_module_finish(m, &diagnostic) == XR_LIR_VERIFY_ERROR &&
          diagnostic.status == XR_LIR_VERIFY_ERROR && diagnostic.function == f.id &&
          diagnostic.block == entry.id && strstr(diagnostic.message, "terminator"));
    xr_lir_module_destroy(m);
    m = test_fib();
    memset(&diagnostic, 0xa5, sizeof(diagnostic));
    CHECK(xr_lir_module_finish(m, &diagnostic) == XR_LIR_INVALID_STATE &&
          diagnostic.status == XR_LIR_INVALID_STATE && !diagnostic.source.path);
    OK(xr_lir_verify(m, NULL));
    xr_lir_module_destroy(m);
}

static void intrinsic_and_float(void) {
    XrLirType input = XR_LIR_F64;
    XrLirIntrinsic intrinsic = {71, &input, 1, XR_LIR_F64, true};
    XrLirRegistry registry = {&intrinsic, 1};
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(&registry, &m));
    input = XR_LIR_I1;
    intrinsic.id = 0;
    XrLirType type = XR_LIR_F64;
    XrLirFunction f = test_function(m, "float_identity", &type, 1, type);
    XrLirBlock entry = test_block(m, f, NULL, 0);
    OK(xr_lir_set_entry(m, f, entry));
    XrLirValue parameter = {0}, converted = {0}, result = {0};
    OK(xr_lir_function_parameter(m, f, 0, &parameter));
    XrLirInstruction conversion = {
        .op = XR_LIR_FCAST, .type = XR_LIR_F64, .operands = &parameter, .operand_count = 1};
    OK(xr_lir_emit_value(m, entry, &conversion, &converted));
    XrLirInstruction call = {.op = XR_LIR_CALL_INTRINSIC,
                             .type = XR_LIR_F64,
                             .intrinsic = 71,
                             .operands = &converted,
                             .operand_count = 1};
    OK(xr_lir_emit_value(m, entry, &call, &result));
    test_return(m, entry, result);
    OK(xr_lir_module_finish(m, NULL));
    OK(xr_lir_verify(m, NULL));
    XrLirInstructionView instruction;
    OK(xr_lir_instruction_view(m, entry, 0, &instruction));
    CHECK(instruction.op == XR_LIR_FCAST && instruction.type == XR_LIR_F64 &&
          instruction.operands[0] == parameter.id);
    OK(xr_lir_instruction_view(m, entry, 1, &instruction));
    CHECK(instruction.op == XR_LIR_CALL_INTRINSIC && instruction.intrinsic == 71 &&
          instruction.operands[0] == converted.id);
    xr_lir_module_destroy(m);
    intrinsic = (XrLirIntrinsic) {71, &type, 1, XR_LIR_F64, false};
    m = NULL;
    CHECK(xr_lir_module_create(&registry, &m) == XR_LIR_UNSUPPORTED && !m);
}

static void intrinsic_query_and_transfer(void) {
    XrLirType *input = xr_malloc(sizeof(*input));
    CHECK(input);
    *input = XR_LIR_I64;
    XrLirIntrinsic entries[] = {{19, input, 1, XR_LIR_I64, true}, {71, NULL, 0, XR_LIR_UNIT, true}};
    XrLirRegistry registry = {entries, 2};
    XrLirModule *source = NULL, *copy = NULL;
    OK(xr_lir_module_create(&registry, &source));
    *input = XR_LIR_I1;
    xr_free(input);
    XrLirIntrinsic view = {0};
    CHECK(!xr_lir_intrinsic_count(source));
    CHECK(xr_lir_intrinsic_at(source, 0, &view) == XR_LIR_INVALID_ARGUMENT && !view.id &&
          !view.parameters);
    XrLirType type = XR_LIR_I64;
    XrLirFunction function = test_function(source, "registered", &type, 1, type);
    XrLirBlock block = test_block(source, function, NULL, 0);
    OK(xr_lir_set_entry(source, function, block));
    XrLirValue parameter = {0}, result = {0};
    OK(xr_lir_function_parameter(source, function, 0, &parameter));
    XrLirInstruction call = {.op = XR_LIR_CALL_INTRINSIC,
                             .type = type,
                             .intrinsic = 19,
                             .operands = &parameter,
                             .operand_count = 1,
                             .source = {"intrinsic-owner.xr", 18, 3, 2, 3, 6}};
    OK(xr_lir_emit_value(source, block, &call, &result));
    test_return(source, block, result);
    OK(xr_lir_module_finish(source, NULL));
    CHECK(xr_lir_intrinsic_count(source) == 2 && !xr_lir_intrinsic_count(NULL));
    OK(xr_lir_intrinsic_at(source, 0, &entries[0]));
    OK(xr_lir_intrinsic_at(source, 1, &entries[1]));
    CHECK(entries[0].id == 19 && entries[0].parameter_count == 1 &&
          entries[0].parameters[0] == type);
    CHECK(entries[1].id == 71 && !entries[1].parameters && entries[1].result == XR_LIR_UNIT &&
          entries[1].scalar_sync);
    OK(xr_lir_intrinsic_view(source, 19, &view));
    CHECK(view.parameters == entries[0].parameters && view.result == type && view.scalar_sync);
    CHECK(xr_lir_intrinsic_at(source, 2, &view) == XR_LIR_INVALID_ARGUMENT && !view.id &&
          !view.parameters);
    CHECK(xr_lir_intrinsic_view(source, 0, &view) == XR_LIR_INVALID_ARGUMENT && !view.id);
    CHECK(xr_lir_intrinsic_view(source, 1, &view) == XR_LIR_INVALID_ARGUMENT && !view.id);
    CHECK(xr_lir_intrinsic_view(NULL, 19, &view) == XR_LIR_INVALID_ARGUMENT && !view.id);
    CHECK(xr_lir_intrinsic_at(source, 0, NULL) == XR_LIR_INVALID_ARGUMENT);
    CHECK(xr_lir_intrinsic_view(source, 19, NULL) == XR_LIR_INVALID_ARGUMENT);
    OK(xr_lir_module_create(&registry, &copy));
    XrLirFunctionView function_view;
    XrLirInstructionView instruction_view;
    OK(xr_lir_function_view(source, function, &function_view));
    OK(xr_lir_instruction_view(source, block, 0, &instruction_view));
    XrLirFunction cloned = test_function(copy, function_view.name, function_view.parameter_types,
                                         function_view.parameter_count, function_view.result);
    XrLirBlock cloned_block = test_block(copy, cloned, NULL, 0);
    OK(xr_lir_set_entry(copy, cloned, cloned_block));
    OK(xr_lir_function_parameter(copy, cloned, 0, &parameter));
    call = (XrLirInstruction) {.op = instruction_view.op,
                               .type = instruction_view.type,
                               .intrinsic = instruction_view.intrinsic,
                               .operands = &parameter,
                               .operand_count = 1,
                               .source = instruction_view.source};
    OK(xr_lir_emit_value(copy, cloned_block, &call, &result));
    test_return(copy, cloned_block, result);
    OK(xr_lir_module_finish(copy, NULL));
    xr_lir_module_destroy(source);
    memset(entries, 0, sizeof(entries));
    OK(xr_lir_verify(copy, NULL));
    CHECK(xr_lir_intrinsic_count(copy) == 2);
    OK(xr_lir_intrinsic_view(copy, 19, &view));
    CHECK(view.parameter_count == 1 && view.parameters[0] == type && view.result == type);
    OK(xr_lir_intrinsic_view(copy, 71, &view));
    CHECK(!view.parameter_count && view.result == XR_LIR_UNIT && view.scalar_sync);
    OK(xr_lir_instruction_view(copy, cloned_block, 0, &instruction_view));
    CHECK(instruction_view.intrinsic == 19 &&
          !strcmp(instruction_view.source.path, "intrinsic-owner.xr"));
    xr_lir_module_destroy(copy);
    XrLirIntrinsic duplicate[] = {{19, &type, 1, type, true}, {19, NULL, 0, XR_LIR_UNIT, true}};
    registry = (XrLirRegistry) {duplicate, 2};
    copy = NULL;
    CHECK(xr_lir_module_create(&registry, &copy) == XR_LIR_INVALID_ARGUMENT && !copy);
}

static XrLirModule *literal_program(uint32_t attack) {
    XrLirModule *m = NULL;OK(xr_lir_module_create(NULL, &m));
    unsigned char bytes[] = {'A', 0, 0xc3, 0xa9};
    XrLirLiteral literal = {0};OK(xr_lir_add_literal(m, bytes, sizeof(bytes), &literal));
    memset(bytes, 0xa5, sizeof(bytes));
    XrLirType parameters[] = {XR_LIR_PTR, XR_LIR_U64};
    XrLirFunction print = {0};
    XrLirFunctionSpec external = {.name="xrt_print_string", .name_length=16, .parameters=parameters,
        .parameter_count=2, .result=XR_LIR_UNIT, .kind=XR_LIR_FUNCTION_EXTERNAL,
        .extern_kind=XR_LIR_EXTERN_PRINT_STRING};
    OK(xr_lir_declare_function(m, &external, &print));
    XrLirFunction f = test_function(m, "literal", NULL, 0, attack == 4 ? XR_LIR_PTR : XR_LIR_UNIT);
    XrLirBlock block = test_block(m, f, NULL, 0);OK(xr_lir_set_entry(m, f, block));
    XrLirValue args[2] = {{0}};
    if (attack == 1) {
        literal.id = 2;
        CHECK(xr_lir_literal_address(m, block, literal, &args[0]) == XR_LIR_INVALID_ARGUMENT && !args[0].id);
        xr_lir_module_destroy(m);return NULL;
    }
    if (attack == 2) {
        XrLirModule *other = NULL;OK(xr_lir_module_create(NULL, &other));
        XrLirLiteral foreign = {0};OK(xr_lir_add_literal(other, "x", 1, &foreign));
        CHECK(xr_lir_literal_address(m, block, foreign, &args[0]) == XR_LIR_INVALID_ARGUMENT && !args[0].id);
        xr_lir_module_destroy(other);xr_lir_module_destroy(m);return NULL;
    }
    if (attack == 3) args[0] = test_constant(m, block, XR_LIR_PTR, 0);
    else OK(xr_lir_literal_address(m, block, literal, &args[0]));
    if (attack == 4) {
        XrLirInstruction ret = {.op=XR_LIR_RET, .type=XR_LIR_UNIT, .operands=args, .operand_count=1};
        CHECK(xr_lir_set_terminator(m, block, &ret) == XR_LIR_VERIFY_ERROR);
        xr_lir_module_destroy(m);return NULL;
    }
    args[1] = test_constant(m, block, XR_LIR_U64, attack == 5 ? 3 : sizeof(bytes));
    XrLirInstruction call = {.op=XR_LIR_CALL, .type=XR_LIR_UNIT, .callee=print,
        .operands=args, .operand_count=2};
    if (attack == 3 || attack == 5) {
        CHECK(xr_lir_emit_effect(m, block, &call) == XR_LIR_VERIFY_ERROR);
        xr_lir_module_destroy(m);return NULL;
    }
    if (attack != 7) OK(xr_lir_emit_effect(m, block, &call));
    if (attack == 6) OK(xr_lir_emit_effect(m, block, &call));
    XrLirInstruction ret = {.op=XR_LIR_RET, .type=XR_LIR_UNIT};OK(xr_lir_set_terminator(m, block, &ret));
    if (attack) {
        CHECK(xr_lir_verify(m, NULL) == XR_LIR_VERIFY_ERROR);
        xr_lir_module_destroy(m);return NULL;
    }
    OK(xr_lir_module_finish(m, NULL));return m;
}
static void literal_ownership(void) {
    XrLirModule *m = literal_program(0), *copies[3] = {0};
    OK(xr_lir_rebuild(m, &copies[0]));OK(xr_lir_inline(m, &copies[1]));OK(xr_lir_sccp(m, &copies[2]));
    XrLirLiteral original = {0};const uint8_t *bytes = NULL;size_t length = 0;
    OK(xr_lir_literal_at(m, 0, &original));OK(xr_lir_literal_view(m, original, &bytes, &length));
    CHECK(length == 4 && xr_lir_literal_count(m) == 1);
    for (uint32_t i = 0; i < 3; ++i) {
        XrLirLiteral owned = {0};const uint8_t *copy = NULL;size_t size = 0;
        OK(xr_lir_literal_at(copies[i], 0, &owned));OK(xr_lir_literal_view(copies[i], owned, &copy, &size));
        CHECK(copy != bytes && size == length && !memcmp(copy, bytes, size));
        CHECK(xr_lir_literal_view(copies[i], original, &copy, &size) == XR_LIR_INVALID_ARGUMENT);
    }
    xr_lir_module_destroy(m);
    const uint8_t expected[] = {'A', 0, 0xc3, 0xa9};
    for (uint32_t i = 0; i < 3; ++i) {
        XrLirLiteral owned = {0};OK(xr_lir_literal_at(copies[i], 0, &owned));
        OK(xr_lir_literal_view(copies[i], owned, &bytes, &length));
        CHECK(length == sizeof(expected) && !memcmp(bytes, expected, sizeof(expected)));
        OK(xr_lir_verify(copies[i], NULL));xr_lir_module_destroy(copies[i]);
    }
    for (uint32_t attack = 1; attack <= 7; ++attack) CHECK(!literal_program(attack));
}

static void clock_signatures(void) {
    static const char *names[] = {"xrt_clock_realtime_nanos", "xrt_clock_process_cpu_nanos",
        "xrt_clock_monotonic_nanos"};
    for (uint32_t kind=0; kind<3; ++kind) {
        XrLirModule *m = NULL; XrLirFunction f = {0};
        OK(xr_lir_module_create(NULL, &m));
        XrLirFunctionSpec spec = {.name=names[kind], .name_length=strlen(names[kind]),
            .result=XR_LIR_I64, .kind=XR_LIR_FUNCTION_EXTERNAL,
            .extern_kind=(XrLirExternKind)(XR_LIR_EXTERN_CLOCK_REALTIME + kind)};
        OK(xr_lir_declare_function(m, &spec, &f));
        OK(xr_lir_module_finish(m, NULL));
        OK(xr_lir_verify(m, NULL)); xr_lir_module_destroy(m); m = NULL;
        OK(xr_lir_module_create(NULL, &m));
        spec.result = XR_LIR_UNIT;
        CHECK(xr_lir_declare_function(m, &spec, &f) == XR_LIR_INVALID_ARGUMENT);
        xr_lir_module_destroy(m);
    }
}
static void offset_signature(void) {
    const XrLirType argument = XR_LIR_I64, wrong = XR_LIR_U64;
    XrLirModule *m = NULL; XrLirFunction f = {0};
    XrLirFunctionSpec spec = {.name="xrt_clock_utc_offset_at", .name_length=23,
        .result=XR_LIR_I64, .kind=XR_LIR_FUNCTION_EXTERNAL, .extern_kind=XR_LIR_EXTERN_UTC_OFFSET_AT,
        .parameters=&argument, .parameter_count=1};
    OK(xr_lir_module_create(NULL, &m)); OK(xr_lir_declare_function(m, &spec, &f));
    OK(xr_lir_module_finish(m, NULL)); OK(xr_lir_verify(m, NULL)); xr_lir_module_destroy(m); m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    spec.parameter_count = 0;
    CHECK(xr_lir_declare_function(m, &spec, &f) == XR_LIR_INVALID_ARGUMENT);
    spec.parameter_count = 1; spec.parameters = &wrong;
    CHECK(xr_lir_declare_function(m, &spec, &f) == XR_LIR_INVALID_ARGUMENT);
    spec.parameters = &argument; spec.result = XR_LIR_UNIT;
    CHECK(xr_lir_declare_function(m, &spec, &f) == XR_LIR_INVALID_ARGUMENT);
    xr_lir_module_destroy(m);
}
static void string_phi_owners(void) {
    for (unsigned attack = 0; attack < 7; ++attack) {
        XrLirModule *m = NULL; OK(xr_lir_module_create(NULL, &m));
        XrLirType types[] = {XR_LIR_I1, XR_LIR_STRING};
        XrLirFunction f = test_function(m, "choose", types, 2, XR_LIR_STRING);
        XrLirBlock entry = test_block(m,f,NULL,0), left = test_block(m,f,NULL,0), right = test_block(m,f,NULL,0);
        XrLirType outputs[] = {XR_LIR_STRING,XR_LIR_STRING};
        XrLirBlock join = test_block(m,f,outputs,attack == 2 ? 2u : 1u);
        OK(xr_lir_set_entry(m,f,entry));
        XrLirValue condition,borrowed; OK(xr_lir_function_parameter(m,f,0,&condition));
        OK(xr_lir_function_parameter(m,f,1,&borrowed));
        XrLirInstruction branch = {.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,.operands=&condition,.operand_count=1,
            .edges={{left,NULL,0},{right,NULL,0}},.edge_count=2};
        OK(xr_lir_set_terminator(m,entry,&branch));
        XrLirLiteral literal = {0}; OK(xr_lir_add_literal(m,"x",1,&literal));
        XrLirValue values[2]; XrLirBlock arms[] = {left,right};
        for (unsigned arm = 0; arm < 2; ++arm) {
            XrLirInstruction string = {.op=XR_LIR_STRING_LITERAL,.type=XR_LIR_STRING,.bits=literal.id};
            OK(xr_lir_emit_value(m,arms[arm],&string,&values[arm]));
            if (attack == 4 && !arm) { XrLirValue leaked; OK(xr_lir_emit_value(m,left,&string,&leaked)); }
            if (attack == 1 && !arm) {
                XrLirInstruction drop = {.op=XR_LIR_STRING_DROP,.type=XR_LIR_UNIT,.operands=&values[arm],.operand_count=1};
                OK(xr_lir_emit_effect(m,left,&drop)); values[arm]=borrowed;
            }
            XrLirValue arguments[] = {values[arm],values[arm]};
            if (attack == 6 && arm) test_branch(m,right,right,NULL,0);
            else test_branch(m,arms[arm],join,arguments,attack == 2 ? 2u : 1u);
        }
        XrLirValue result; OK(xr_lir_block_parameter(m,join,0,&result));
        if (attack == 2) {
            XrLirValue second; OK(xr_lir_block_parameter(m,join,1,&second));
            XrLirInstruction drop = {.op=XR_LIR_STRING_DROP,.type=XR_LIR_UNIT,.operands=&second,.operand_count=1};
            OK(xr_lir_emit_effect(m,join,&drop));
        }
        if (attack == 3 || attack == 5) {
            XrLirInstruction drop = {.op=XR_LIR_STRING_DROP,.type=XR_LIR_UNIT,.operands=&result,.operand_count=1};
            OK(xr_lir_emit_effect(m,join,&drop));
            if (attack == 3) {
                XrLirValue retained; XrLirInstruction retain = {.op=XR_LIR_STRING_RETAIN,.type=XR_LIR_STRING,.operands=&result,.operand_count=1};
                OK(xr_lir_emit_value(m,join,&retain,&retained)); result=retained;
            } else OK(xr_lir_emit_effect(m,join,&drop));
        }
        test_return(m,join,result);
        CHECK(xr_lir_verify(m,NULL) == (attack ? XR_LIR_VERIFY_ERROR : XR_LIR_OK));
        if (!attack) OK(xr_lir_module_finish(m,NULL));
        xr_lir_module_destroy(m);
    }
}
int main(void) {
    string_phi_owners();
    offset_signature();
    clock_signatures();
    queries();
    literal_ownership();
    builder_rejections();
    foreign_handles();
    verifier_rejections();
    definition_prevalidation_rejections();
    graph_edges();
    owned_inputs_and_growth();
    constants_and_swap();
    intrinsic_and_float();
    intrinsic_query_and_transfer();
    first_failure_diagnostic();
    puts("scalar LIR construction, queries, ownership and rejection tests passed");
    return 0;
}
