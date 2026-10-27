/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_sccp.c - Changed SSA, live effects and retired-input consumers
 */
#include "lir_test_programs.h"
#include "lir/xlir_sccp.h"
#include "lir/xlir_verify.h"
#include "lir/xlir_print.h"
#include "lir/xlir_c.h"
#include "vm/register/xrbc_lir.h"
#include "base/xmalloc.h"
#include <inttypes.h>

static XrLirInstructionView instruction_at(XrLirModule *m, uint32_t f, uint32_t b, uint32_t n) {
    XrLirFunction function; XrLirBlock block; XrLirInstructionView view;
    OK(xr_lir_function_at(m, f, &function));
    OK(xr_lir_block_at(m, function, b, &block));
    OK(xr_lir_instruction_view(m, block, n, &view));
    return view;
}
static uint32_t blocks_in(XrLirModule *m, uint32_t ordinal) {
    XrLirFunction function; XrLirFunctionView view;
    OK(xr_lir_function_at(m, ordinal, &function));
    OK(xr_lir_function_view(m, function, &view));
    return view.block_count;
}
static size_t count_op(XrLirModule *m, uint32_t ordinal, XrLirOp op) {
    XrLirFunction function; XrLirFunctionView f;
    OK(xr_lir_function_at(m, ordinal, &function));
    OK(xr_lir_function_view(m, function, &f));
    size_t count = 0;
    for (uint32_t b = 0; b < f.block_count; ++b) {
        XrLirBlock block; XrLirBlockView view;
        OK(xr_lir_block_at(m, function, b, &block));
        OK(xr_lir_block_view(m, block, &view));
        for (uint32_t n = 0; n < view.instruction_count; ++n)
            count += instruction_at(m, ordinal, b, n).op == op;
    }
    return count;
}
static bool printed(void *context, const char *bytes, size_t count) {
    CHECK(bytes && count <= SIZE_MAX - *(size_t *)context);
    *(size_t *)context += count;
    return true;
}
static XrLirModule *optimize_and_retire(XrLirModule *input) {
    XrLirModule *output = NULL;
    OK(xr_lir_sccp(input, &output));
    CHECK(output && output != input);
    xr_lir_module_destroy(input);
    OK(xr_lir_verify(output, NULL));
    size_t bytes = 0;
    OK(xr_lir_print(output, printed, &bytes));
    CHECK(bytes);
    return output;
}
static uint64_t run_vm_function(XrLirModule *module, uint32_t ordinal,
    const uint64_t *arguments, size_t count) {
    XrLirFunction root; OK(xr_lir_function_at(module, ordinal, &root));
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(module);
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_program_prepare(program, arguments, count, &execution) == XR_RBC_OK);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution);
    xr_rbc_program_destroy(program);
    uint64_t bits = UINT64_MAX;
    CHECK(outcome && xr_rbc_outcome_status(outcome) == XR_RBC_OK);
    CHECK(xr_rbc_outcome_bits(outcome, &bits));
    xr_rbc_outcome_free(&outcome);
    return bits;
}
static uint64_t run_vm(XrLirModule *module) { return run_vm_function(module, 0, NULL, 0); }
static void conditional(XrLirModule *m, XrLirBlock from, XrLirValue condition,
    XrLirEdge yes, XrLirEdge no) {
    XrLirInstruction v = {.op = XR_LIR_CONDBR, .type = XR_LIR_UNIT,
        .operands = &condition, .operand_count = 1, .edges = {yes, no}, .edge_count = 2};
    OK(xr_lir_set_terminator(m, from, &v));
}
static XrLirModule *make_parallel(bool condition, bool unknown) {
    XrLirModule *m = NULL; OK(xr_lir_module_create(NULL, &m));
    XrLirFunction root = test_function(m, "root", NULL, 0, XR_LIR_I64);
    XrLirType type = XR_LIR_I1;
    XrLirFunction f = unknown ? test_function(m, "choose", &type, 1, XR_LIR_I64) : root;
    XrLirBlock entry = test_block(m, f, NULL, 0);
    type = XR_LIR_I64;
    XrLirBlock join = test_block(m, f, &type, 1);
    OK(xr_lir_set_entry(m, f, entry));
    XrLirValue cond, result;
    if (unknown) OK(xr_lir_function_parameter(m, f, 0, &cond));
    else cond = test_constant(m, entry, XR_LIR_I1, condition);
    XrLirValue a = test_constant(m, entry, type, 42), b = test_constant(m, entry, type, 43);
    conditional(m, entry, cond, (XrLirEdge){join, &a, 1}, (XrLirEdge){join, &b, 1});
    OK(xr_lir_block_parameter(m, join, 0, &result));
    test_return(m, join, result);
    if (unknown) {
        XrLirBlock caller = test_block(m, root, NULL, 0);
        OK(xr_lir_set_entry(m, root, caller));
        cond = test_constant(m, caller, XR_LIR_I1, condition);
        XrLirInstruction v = {.op = XR_LIR_CALL, .type = type, .callee = f,
            .operands = &cond, .operand_count = 1};
        OK(xr_lir_emit_value(m, caller, &v, &result));
        test_return(m, caller, result);
    }
    OK(xr_lir_module_finish(m, NULL));
    return m;
}
static XrLirModule *make_fold(void) {
    XrLirModule *m = NULL; OK(xr_lir_module_create(NULL, &m));
    XrLirFunction f = test_function(m, "fold", NULL, 0, XR_LIR_I64);
    XrLirBlock entry = test_block(m, f, NULL, 0), dead = test_block(m, f, NULL, 0);
    XrLirBlock live = test_block(m, f, NULL, 0);
    OK(xr_lir_set_entry(m, f, entry));
    XrLirValue a = test_constant(m, entry, XR_LIR_I64, 40);
    XrLirValue b = test_constant(m, entry, XR_LIR_I64, 2);
    XrLirValue sum = test_binary(m, entry, XR_LIR_ADD, a, b, XR_LIR_PREDICATE_NONE);
    XrLirValue expected = test_constant(m, entry, XR_LIR_I64, 42);
    XrLirValue cmp = test_binary(m, entry, XR_LIR_ICMP, sum, expected, XR_LIR_EQ);
    conditional(m, entry, cmp, (XrLirEdge){live, NULL, 0}, (XrLirEdge){dead, NULL, 0});
    test_return(m, live, sum);
    test_return(m, dead, test_constant(m, dead, XR_LIR_I64, 99));
    OK(xr_lir_module_finish(m, NULL));
    return m;
}
static XrLirModule *make_loop(void) {
    XrLirModule *m = NULL; OK(xr_lir_module_create(NULL, &m));
    XrLirFunction f = test_function(m, "loop", NULL, 0, XR_LIR_I64);
    XrLirType pair[] = {XR_LIR_I64, XR_LIR_I64};
    XrLirBlock entry = test_block(m, f, NULL, 0), done = test_block(m, f, pair, 1);
    XrLirBlock body = test_block(m, f, NULL, 0), loop = test_block(m, f, pair, 2);
    OK(xr_lir_set_entry(m, f, entry));
    XrLirValue zero = test_constant(m, entry, XR_LIR_I64, 0);
    XrLirValue one = test_constant(m, entry, XR_LIR_I64, 1);
    XrLirValue limit = test_constant(m, entry, XR_LIR_I64, 20), initial[] = {zero, zero};
    test_branch(m, entry, loop, initial, 2);
    XrLirValue i, total, result;
    OK(xr_lir_block_parameter(m, loop, 0, &i));
    OK(xr_lir_block_parameter(m, loop, 1, &total));
    OK(xr_lir_block_parameter(m, done, 0, &result));
    XrLirValue cmp = test_binary(m, loop, XR_LIR_ICMP, i, limit, XR_LIR_LT);
    conditional(m, loop, cmp, (XrLirEdge){body, NULL, 0}, (XrLirEdge){done, &total, 1});
    XrLirValue next[] = {test_binary(m, body, XR_LIR_ADD, i, one, XR_LIR_PREDICATE_NONE),
        test_binary(m, body, XR_LIR_ADD, total, i, XR_LIR_PREDICATE_NONE)};
    test_branch(m, body, loop, next, 2);
    test_return(m, done, result);
    OK(xr_lir_module_finish(m, NULL));
    return m;
}
static void print_value(XrLirModule *m, XrLirBlock b, XrLirFunction print, uint64_t bits) {
    XrLirValue value = test_constant(m, b, XR_LIR_I64, bits);
    XrLirInstruction call = {.op = XR_LIR_CALL, .type = XR_LIR_UNIT, .callee = print,
        .operands = &value, .operand_count = 1, .source = {"effects.xr", 10, (uint32_t)bits, 1, (uint32_t)bits, 2}};
    OK(xr_lir_emit_effect(m, b, &call));
}
static XrLirModule *make_effects(void) {
    XrLirModule *m = NULL; OK(xr_lir_module_create(NULL, &m));
    XrLirFunction root = test_function(m, "root", NULL, 0, XR_LIR_I64);
    XrLirFunction helper = test_function(m, "helper", NULL, 0, XR_LIR_I64);
    XrLirFunction print = test_print_declaration(m);
    XrLirBlock entry = test_block(m, root, NULL, 0), done = test_block(m, root, NULL, 0);
    XrLirBlock dead = test_block(m, root, NULL, 0), body = test_block(m, helper, NULL, 0);
    OK(xr_lir_set_entry(m, root, entry)); OK(xr_lir_set_entry(m, helper, body));
    print_value(m, entry, print, 1);
    XrLirInstruction call = {.op = XR_LIR_CALL, .type = XR_LIR_I64, .callee = helper,
        .source = {"effects.xr", 10, 2, 1, 2, 2}};
    XrLirValue unused; OK(xr_lir_emit_value(m, entry, &call, &unused));
    XrLirValue yes = test_constant(m, entry, XR_LIR_I1, 1);
    conditional(m, entry, yes, (XrLirEdge){done, NULL, 0}, (XrLirEdge){dead, NULL, 0});
    print_value(m, dead, print, 99); test_branch(m, dead, done, NULL, 0);
    print_value(m, done, print, 3);
    test_return(m, done, test_constant(m, done, XR_LIR_I64, 0));
    print_value(m, body, print, 2);
    test_return(m, body, test_constant(m, body, XR_LIR_I64, 7));
    OK(xr_lir_module_finish(m, NULL));
    return m;
}
static XrLirModule *make_wrap(void) {
    XrLirModule *m = NULL; OK(xr_lir_module_create(NULL, &m));
    XrLirFunction f = test_function(m, "wrap", NULL, 0, XR_LIR_I64);
    XrLirBlock b = test_block(m, f, NULL, 0); OK(xr_lir_set_entry(m, f, b));
    XrLirValue max = test_constant(m, b, XR_LIR_I64, INT64_MAX);
    XrLirValue one = test_constant(m, b, XR_LIR_I64, 1);
    XrLirValue min = test_binary(m, b, XR_LIR_ADD, max, one, XR_LIR_PREDICATE_NONE), result;
    XrLirInstruction neg = {.op = XR_LIR_NEG, .type = XR_LIR_I64, .operands = &min, .operand_count = 1};
    OK(xr_lir_emit_value(m, b, &neg, &result)); test_return(m, b, result);
    OK(xr_lir_module_finish(m, NULL));
    return m;
}
static XrLirModule *make_case(const char *name) {
    if (!strcmp(name, "fold")) return make_fold();
    if (!strcmp(name, "loop")) return make_loop();
    if (!strcmp(name, "effects")) return make_effects();
    if (!strcmp(name, "wrap")) return make_wrap();
    CHECK(!strcmp(name, "true") || !strcmp(name, "false") ||
        !strcmp(name, "unknown-true") || !strcmp(name, "unknown-false"));
    return make_parallel(strstr(name, "false") == NULL, !strncmp(name, "unknown-", 8));
}

static void test_preserved_operations(void) {
    XrLirType *parameters = xr_malloc(2 * sizeof(*parameters)); CHECK(parameters);
    parameters[0] = XR_LIR_I64; parameters[1] = XR_LIR_PTR;
    XrLirIntrinsic entries[] = {{7, parameters, 1, XR_LIR_I64, true},
        {91, parameters, 2, XR_LIR_UNIT, true}};
    XrLirRegistry registry = {entries, 2};
    XrLirModule *m = NULL; OK(xr_lir_module_create(&registry, &m));
    xr_free(parameters);
    XrLirFunction f = test_function(m, "preserve", NULL, 0, XR_LIR_UNIT);
    XrLirBlock b = test_block(m, f, NULL, 0); OK(xr_lir_set_entry(m, f, b));
    XrLirValue zero = test_constant(m, b, XR_LIR_I64, 0);
    XrLirValue min = test_constant(m, b, XR_LIR_I64, UINT64_C(0x8000000000000000));
    XrLirValue minus = test_constant(m, b, XR_LIR_I64, UINT64_MAX);
    test_binary(m, b, XR_LIR_DIV, min, minus, XR_LIR_PREDICATE_NONE);
    test_binary(m, b, XR_LIR_REM, min, zero, XR_LIR_PREDICATE_NONE);
    XrLirValue narrow = test_constant(m, b, XR_LIR_I8, 128), value;
    XrLirInstruction v = {.op = XR_LIR_INTCAST, .type = XR_LIR_I64, .operands = &narrow, .operand_count = 1};
    OK(xr_lir_emit_value(m, b, &v, &value));
    XrLirValue u = test_constant(m, b, XR_LIR_U64, UINT64_MAX), operands[] = {u, u};
    v = (XrLirInstruction){.op = XR_LIR_ADD, .type = XR_LIR_U64, .operands = operands, .operand_count = 2};
    OK(xr_lir_emit_value(m, b, &v, &value));
    XrLirValue fp = test_constant(m, b, XR_LIR_F64, UINT64_C(0x8000000000000000));
    test_constant(m, b, XR_LIR_F64, UINT64_C(0x7ff8000000000000));
    v = (XrLirInstruction){.op = XR_LIR_FNEG, .type = XR_LIR_F64, .operands = &fp, .operand_count = 1};
    OK(xr_lir_emit_value(m, b, &v, &value));
    XrLirValue ptr = test_constant(m, b, XR_LIR_PTR, 0); operands[0] = operands[1] = ptr;
    v = (XrLirInstruction){.op = XR_LIR_ICMP, .type = XR_LIR_I1, .predicate = XR_LIR_EQ,
        .operands = operands, .operand_count = 2};
    OK(xr_lir_emit_value(m, b, &v, &value));
    char *path = xr_malloc(401); CHECK(path); memset(path, 'p', 400); path[400] = 0;
    v = (XrLirInstruction){.op = XR_LIR_CALL_INTRINSIC, .type = XR_LIR_I64, .intrinsic = 7,
        .operands = &zero, .operand_count = 1, .source = {path, 400, 4, 1, 4, 2}};
    OK(xr_lir_emit_value(m, b, &v, &value)); xr_free(path);
    test_return(m, b, (XrLirValue){0}); OK(xr_lir_module_finish(m, NULL));
    m = optimize_and_retire(m);
    const XrLirOp preserved[] = {XR_LIR_DIV, XR_LIR_REM, XR_LIR_INTCAST, XR_LIR_ADD,
        XR_LIR_FNEG, XR_LIR_ICMP, XR_LIR_CALL_INTRINSIC};
    for (size_t i = 0; i < sizeof(preserved) / sizeof(preserved[0]); ++i)
        CHECK(count_op(m, 0, preserved[i]) == 1);
    CHECK(instruction_at(m, 0, 0, 9).bits == UINT64_C(0x8000000000000000));
    CHECK(instruction_at(m, 0, 0, 10).bits == UINT64_C(0x7ff8000000000000));
    XrLirInstructionView call = instruction_at(m, 0, 0, 14);
    CHECK(call.op == XR_LIR_CALL_INTRINSIC && call.intrinsic == 7 && call.source.path_length == 400);
    for (size_t p = 0; p < call.source.path_length; ++p) CHECK(call.source.path[p] == 'p');
    XrLirIntrinsic unused; OK(xr_lir_intrinsic_view(m, 91, &unused));
    CHECK(xr_lir_intrinsic_count(m) == 2 && unused.parameter_count == 2 &&
        unused.parameters[0] == XR_LIR_I64 && unused.parameters[1] == XR_LIR_PTR && unused.scalar_sync);
    xr_lir_module_destroy(m);
}
static void test_integer_bits(void) {
    XrLirModule *m = NULL; OK(xr_lir_module_create(NULL, &m));
    XrLirFunction f = test_function(m, "bits", NULL, 0, XR_LIR_UNIT);
    XrLirBlock b = test_block(m, f, NULL, 0); OK(xr_lir_set_entry(m, f, b));
    XrLirValue min = test_constant(m, b, XR_LIR_I64, UINT64_C(0x8000000000000000));
    XrLirValue max = test_constant(m, b, XR_LIR_I64, INT64_MAX);
    XrLirValue minus = test_constant(m, b, XR_LIR_I64, UINT64_MAX);
    test_binary(m, b, XR_LIR_SUB, min, minus, XR_LIR_PREDICATE_NONE);
    test_binary(m, b, XR_LIR_MUL, min, minus, XR_LIR_PREDICATE_NONE);
    const XrLirPredicate predicates[] = {XR_LIR_EQ, XR_LIR_NE, XR_LIR_LT, XR_LIR_LE, XR_LIR_GT, XR_LIR_GE};
    for (size_t i = 0; i < 6; ++i) test_binary(m, b, XR_LIR_ICMP, min, max, predicates[i]);
    XrLirValue yes = test_constant(m, b, XR_LIR_I1, 1), no = test_constant(m, b, XR_LIR_I1, 0);
    test_binary(m, b, XR_LIR_ICMP, yes, no, XR_LIR_EQ);
    test_binary(m, b, XR_LIR_ICMP, yes, no, XR_LIR_NE);
    test_return(m, b, (XrLirValue){0}); OK(xr_lir_module_finish(m, NULL));
    m = optimize_and_retire(m);
    CHECK(instruction_at(m, 0, 0, 3).bits == UINT64_C(0x8000000000000001));
    CHECK(instruction_at(m, 0, 0, 4).bits == UINT64_C(0x8000000000000000));
    const uint64_t expected[] = {0, 1, 1, 1, 0, 0};
    for (uint32_t i = 0; i < 6; ++i) {
        XrLirInstructionView v = instruction_at(m, 0, 0, 5 + i);
        CHECK(v.op == XR_LIR_CONST && v.bits == expected[i] && !v.operand_count &&
            !v.edge_count && !v.callee && !v.intrinsic && v.predicate == XR_LIR_PREDICATE_NONE);
    }
    CHECK(instruction_at(m, 0, 0, 13).bits == 0 && instruction_at(m, 0, 0, 14).bits == 1);
    xr_lir_module_destroy(m);
}
static void test_api_and_graph(void) {
    XrLirModule *m = NULL, *out = NULL;
    CHECK(xr_lir_sccp(NULL, &out) == XR_LIR_INVALID_ARGUMENT && !out);
    OK(xr_lir_module_create(NULL, &m));
    CHECK(xr_lir_sccp(m, &out) == XR_LIR_INVALID_ARGUMENT && !out);
    OK(xr_lir_module_finish(m, NULL)); OK(xr_lir_sccp(m, &out));
    XrLirModule *occupied = out;
    CHECK(xr_lir_sccp(m, &out) == XR_LIR_INVALID_ARGUMENT && out == occupied);
    CHECK(xr_lir_module_is_readable(m));
    xr_lir_module_destroy(out); xr_lir_module_destroy(m);
    m = NULL; OK(xr_lir_module_create(NULL, &m)); test_print_declaration(m);
    OK(xr_lir_module_finish(m, NULL)); out = NULL; OK(xr_lir_sccp(m, &out));
    CHECK(xr_lir_function_count(out) == 1); xr_lir_module_destroy(out); xr_lir_module_destroy(m);
    m = optimize_and_retire(make_fold());
    CHECK(blocks_in(m, 0) == 2 && count_op(m, 0, XR_LIR_ADD) == 0 && count_op(m, 0, XR_LIR_CONDBR) == 0);
    CHECK(instruction_at(m, 0, 0, 2).op == XR_LIR_CONST && instruction_at(m, 0, 0, 2).bits == 42);
    CHECK(instruction_at(m, 0, 0, 5).op == XR_LIR_BR && !instruction_at(m, 0, 0, 5).operand_count);
    xr_lir_module_destroy(m);
    m = optimize_and_retire(make_parallel(true, true));
    CHECK(count_op(m, 1, XR_LIR_CONDBR) == 1 && blocks_in(m, 1) == 2);
    XrLirInstructionView parallel = instruction_at(m, 1, 0, 2);
    CHECK(parallel.edges[0].target == parallel.edges[1].target);
    xr_lir_module_destroy(m);
    m = optimize_and_retire(make_loop()); CHECK(blocks_in(m, 0) == 4 && count_op(m, 0, XR_LIR_CONDBR) == 1);
    xr_lir_module_destroy(m);
    m = optimize_and_retire(make_effects());
    CHECK(blocks_in(m, 0) == 2 && count_op(m, 0, XR_LIR_CALL) == 3 && count_op(m, 1, XR_LIR_CALL) == 1);
    XrLirInstructionView first = instruction_at(m, 0, 0, 1), unused = instruction_at(m, 0, 0, 2);
    CHECK(first.source.line == 1 && unused.source.line == 2 && unused.type == XR_LIR_I64 && unused.result);
    CHECK(first.source.path_length == 10 && !memcmp(first.source.path, "effects.xr", 10));
    xr_lir_module_destroy(m);
}
static void emit_c(XrLirModule *m, const char *path) {
    XrLirFunction root; OK(xr_lir_function_at(m, 0, &root));
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrLirCRequest request = {.module = m, .entry = root, .resources = resources, .translation_unit = path, .control = NULL};
    XrLirCOutput output = {0};
    CHECK(xr_lir_emit_c(&request, &output, NULL) == XR_LIR_C_OK);
    xr_compile_resources_release(resources); xr_lir_module_destroy(m);
    FILE *file = fopen(path, "wb");
    CHECK(file && fwrite(output.bytes, 1, output.length, file) == output.length);
    CHECK(fclose(file) == 0); xr_lir_c_output_free(&output);
}
int main(int argc, char **argv) {
    if (argc == 1) {
        test_api_and_graph(); test_preserved_operations(); test_integer_bits();
        const char *names[] = {"fold", "true", "false", "unknown-true", "unknown-false", "loop", "wrap"};
        const uint64_t answers[] = {42, 42, 43, 42, 43, 190, UINT64_C(0x8000000000000000)};
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
            CHECK(run_vm(make_case(names[i])) == answers[i]);
            CHECK(run_vm(optimize_and_retire(make_case(names[i]))) == answers[i]);
        }
        uint64_t n = 12;
        CHECK(run_vm_function(optimize_and_retire(test_fib()), 1, &n, 1) == 144);
        n = 100;
        CHECK(run_vm_function(optimize_and_retire(test_sum()), 1, &n, 1) == 4950);
        puts("Sparse integer propagation and owned reconstruction passed");
        return 0;
    }
    CHECK(argc == 3 || argc == 4);
    XrLirModule *m = make_case(argv[1]);
    CHECK(!strcmp(argv[2], "before") || !strcmp(argv[2], "after"));
    if (!strcmp(argv[2], "after")) m = optimize_and_retire(m);
    if (argc == 4) emit_c(m, argv[3]);
    else { uint64_t bits = run_vm(m); if (strcmp(argv[1], "effects")) printf("%" PRIu64 "\n", bits); }
    return 0;
}
