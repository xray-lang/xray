/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_string_elide.c - Shared String references keep verified value flow
 */
#include "lir_test_programs.h"
#include "lir/xlir_string_elide.h"
#include "lir/xlir_verify.h"
#include "vm/register/xrbc_lir.h"

static XrLirValue emit(XrLirModule *m, XrLirBlock block, XrLirOp op, XrLirType type,
    const XrLirValue *operands, uint32_t count) {
    XrLirValue result = {0};
    XrLirInstruction instruction = {.op = op, .type = type, .operands = operands,
        .operand_count = count};
    if (type == XR_LIR_UNIT) OK(xr_lir_emit_effect(m, block, &instruction));
    else OK(xr_lir_emit_value(m, block, &instruction, &result));
    return result;
}
static XrLirValue literal(XrLirModule *m, XrLirBlock block, const char *text) {
    XrLirLiteral bytes = {0};
    OK(xr_lir_add_literal(m, text, strlen(text), &bytes));
    XrLirValue result = {0};
    XrLirInstruction instruction = {.op = XR_LIR_STRING_LITERAL, .type = XR_LIR_STRING,
        .bits = bytes.id};
    OK(xr_lir_emit_value(m, block, &instruction, &result));
    return result;
}
static XrLirValue retain(XrLirModule *m, XrLirBlock block, XrLirValue value) {
    return emit(m, block, XR_LIR_STRING_RETAIN, XR_LIR_STRING, &value, 1);
}
static void drop(XrLirModule *m, XrLirBlock block, XrLirValue value) {
    emit(m, block, XR_LIR_STRING_DROP, XR_LIR_UNIT, &value, 1);
}
static XrLirValue call(XrLirModule *m, XrLirBlock block, XrLirFunction callee, XrLirType type,
    const XrLirValue *arguments, uint32_t count) {
    XrLirValue result = {0};
    XrLirInstruction instruction = {.op = XR_LIR_CALL, .type = type, .callee = callee,
        .operands = arguments, .operand_count = count};
    OK(xr_lir_emit_value(m, block, &instruction, &result));
    return result;
}
static XrLirValue parameter(XrLirModule *m, XrLirFunction f, uint32_t ordinal) {
    XrLirValue value = {0};
    OK(xr_lir_function_parameter(m, f, ordinal, &value));
    return value;
}
static XrLirBlock entry_block(XrLirModule *m, XrLirFunction f) {
    XrLirBlock block = test_block(m, f, NULL, 0);
    OK(xr_lir_set_entry(m, f, block));
    return block;
}

/* label(p) copies its parameter and returns a retained copy of a temporary. */
static void build_label(XrLirModule *m, XrLirFunction f) {
    XrLirBlock b = entry_block(m, f);
    XrLirValue copy = retain(m, b, parameter(m, f, 0)), hash = literal(m, b, "#");
    XrLirValue operands[] = {copy, hash};
    XrLirValue joined = emit(m, b, XR_LIR_STRING_CONCAT, XR_LIR_STRING, operands, 2);
    XrLirValue result = retain(m, b, joined);
    drop(m, b, copy); drop(m, b, hash); drop(m, b, joined);
    test_return(m, b, result);
}
/* A returned copy of a borrowed parameter needs its own reference. */
static void build_keep(XrLirModule *m, XrLirFunction f) {
    XrLirBlock b = entry_block(m, f);
    test_return(m, b, retain(m, b, parameter(m, f, 0)));
}
/* One edge transfers a copy and the copy of that copy: two references. */
static void build_pair(XrLirModule *m, XrLirFunction f) {
    XrLirType strings[] = {XR_LIR_STRING, XR_LIR_STRING};
    XrLirBlock b = entry_block(m, f), join = test_block(m, f, strings, 2);
    XrLirValue first = retain(m, b, parameter(m, f, 0)), second = retain(m, b, first);
    XrLirValue arguments[] = {first, second};
    test_branch(m, b, join, arguments, 2);
    XrLirValue left = {0}, right = {0};
    OK(xr_lir_block_parameter(m, join, 0, &left));
    OK(xr_lir_block_parameter(m, join, 1, &right));
    XrLirValue operands[] = {left, right};
    XrLirValue joined = emit(m, join, XR_LIR_STRING_CONCAT, XR_LIR_STRING, operands, 2);
    drop(m, join, left); drop(m, join, right);
    test_return(m, join, joined);
}
/* Each copy outlives the owner it copies, so all three share one reference. */
static void build_chain(XrLirModule *m, XrLirFunction f) {
    XrLirBlock b = entry_block(m, f);
    XrLirValue text = literal(m, b, "chain"), first = retain(m, b, text);
    XrLirValue second = retain(m, b, first);
    drop(m, b, text); drop(m, b, first);
    test_return(m, b, second);
}
/* A copy dropped before both edges transfer its source borrows that source. */
static void build_branch(XrLirModule *m, XrLirFunction f) {
    XrLirType string = XR_LIR_STRING;
    XrLirBlock b = entry_block(m, f);
    XrLirBlock big = test_block(m, f, &string, 1), small = test_block(m, f, &string, 1);
    XrLirValue number = parameter(m, f, 0);
    XrLirValue text = emit(m, b, XR_LIR_STRING_I64, XR_LIR_STRING, &number, 1);
    XrLirValue copy = retain(m, b, text);
    XrLirValue length = emit(m, b, XR_LIR_STRING_LEN, XR_LIR_I64, &copy, 1);
    drop(m, b, copy);
    XrLirValue limit = test_constant(m, b, XR_LIR_I64, 1);
    XrLirValue condition = test_binary(m, b, XR_LIR_ICMP, length, limit, XR_LIR_GT);
    XrLirInstruction branch = {.op = XR_LIR_CONDBR, .type = XR_LIR_UNIT,
        .operands = &condition, .operand_count = 1,
        .edges = {{big, &text, 1}, {small, &text, 1}}, .edge_count = 2};
    OK(xr_lir_set_terminator(m, b, &branch));
    XrLirValue owned = {0};
    OK(xr_lir_block_parameter(m, big, 0, &owned));
    XrLirValue measured = emit(m, big, XR_LIR_STRING_LEN, XR_LIR_I64, &owned, 1);
    drop(m, big, owned);
    test_return(m, big, measured);
    OK(xr_lir_block_parameter(m, small, 0, &owned));
    drop(m, small, owned);
    test_return(m, small, test_constant(m, small, XR_LIR_I64, 0));
}

typedef struct { XrLirFunction label, keep, pair, chain, branch, root; } Fixture;

static void build_root(XrLirModule *m, const Fixture *f) {
    XrLirBlock b = entry_block(m, f->root);
    XrLirValue row = literal(m, b, "row");
    XrLirValue labelled = call(m, b, f->label, XR_LIR_STRING, &row, 1);
    XrLirValue copy = retain(m, b, labelled);
    XrLirValue kept = call(m, b, f->keep, XR_LIR_STRING, &copy, 1);
    XrLirValue paired = call(m, b, f->pair, XR_LIR_STRING, &kept, 1);
    XrLirValue chained = call(m, b, f->chain, XR_LIR_STRING, NULL, 0);
    XrLirValue number = test_constant(m, b, XR_LIR_I64, 12345);
    XrLirValue branched = call(m, b, f->branch, XR_LIR_I64, &number, 1);
    XrLirValue pair_length = emit(m, b, XR_LIR_STRING_LEN, XR_LIR_I64, &paired, 1);
    XrLirValue chain_length = emit(m, b, XR_LIR_STRING_LEN, XR_LIR_I64, &chained, 1);
    drop(m, b, row); drop(m, b, labelled); drop(m, b, copy);
    drop(m, b, kept); drop(m, b, paired); drop(m, b, chained);
    XrLirValue hundred = test_constant(m, b, XR_LIR_I64, 100);
    XrLirValue ten = test_constant(m, b, XR_LIR_I64, 10);
    XrLirValue total = test_binary(m, b, XR_LIR_ADD,
        test_binary(m, b, XR_LIR_MUL, pair_length, hundred, XR_LIR_PREDICATE_NONE),
        test_binary(m, b, XR_LIR_MUL, chain_length, ten, XR_LIR_PREDICATE_NONE),
        XR_LIR_PREDICATE_NONE);
    test_return(m, b, test_binary(m, b, XR_LIR_ADD, total, branched, XR_LIR_PREDICATE_NONE));
}

static XrLirModule *fixture(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType string = XR_LIR_STRING, integer = XR_LIR_I64;
    Fixture f = {
        test_function(m, "label", &string, 1, XR_LIR_STRING),
        test_function(m, "keep", &string, 1, XR_LIR_STRING),
        test_function(m, "pair", &string, 1, XR_LIR_STRING),
        test_function(m, "chain", NULL, 0, XR_LIR_STRING),
        test_function(m, "branch", &integer, 1, XR_LIR_I64),
        test_function(m, "root", NULL, 0, XR_LIR_I64)};
    build_label(m, f.label); build_keep(m, f.keep); build_pair(m, f.pair);
    build_chain(m, f.chain); build_branch(m, f.branch); build_root(m, &f);
    OK(xr_lir_module_finish(m, NULL));
    return m;
}

static uint32_t count(const XrLirModule *m, uint32_t ordinal, XrLirOp op) {
    XrLirFunction f;
    XrLirFunctionView view;
    OK(xr_lir_function_at(m, ordinal, &f));
    OK(xr_lir_function_view(m, f, &view));
    uint32_t total = 0;
    for (uint32_t b = 0; b < view.block_count; ++b) {
        XrLirBlock block;
        XrLirBlockView blocks;
        OK(xr_lir_block_at(m, f, b, &block));
        OK(xr_lir_block_view(m, block, &blocks));
        for (uint32_t n = 0; n < blocks.instruction_count; ++n) {
            XrLirInstructionView instruction;
            OK(xr_lir_instruction_view(m, block, n, &instruction));
            total += instruction.op == op;
        }
    }
    return total;
}
static uint64_t run_root(const XrLirModule *m) {
    XrLirFunction root;
    OK(xr_lir_function_at(m, 5, &root));
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(m, root, &program, NULL) == XR_RBC_COMPILE_OK);
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution);
    xr_rbc_program_destroy(program);
    uint64_t bits = UINT64_MAX;
    CHECK(outcome && xr_rbc_outcome_status(outcome) == XR_RBC_OK);
    CHECK(xr_rbc_outcome_bits(outcome, &bits));
    xr_rbc_outcome_free(&outcome);
    return bits;
}

static void elided_shapes(void) {
    XrLirModule *input = fixture(), *output = NULL;
    // retains and drops per function before and after: label, keep, pair, chain, branch, root
    static const uint32_t retains[2][6] = {{2, 1, 2, 2, 1, 1}, {0, 1, 2, 0, 0, 0}};
    static const uint32_t drops[2][6] = {{3, 0, 2, 2, 3, 6}, {1, 0, 2, 0, 2, 5}};
    OK(xr_lir_string_elide(input, &output));
    CHECK(output && output != input && xr_lir_module_is_readable(output));
    OK(xr_lir_verify(output, NULL));
    for (uint32_t f = 0; f < 6; ++f) {
        CHECK(count(input, f, XR_LIR_STRING_RETAIN) == retains[0][f]);
        CHECK(count(input, f, XR_LIR_STRING_DROP) == drops[0][f]);
        CHECK(count(output, f, XR_LIR_STRING_RETAIN) == retains[1][f]);
        CHECK(count(output, f, XR_LIR_STRING_DROP) == drops[1][f]);
    }
    CHECK(xr_lir_function_count(output) == xr_lir_function_count(input));
    CHECK(xr_lir_literal_count(output) == xr_lir_literal_count(input));
    // "row#row#" * 100 + "chain" * 10 + digits of 12345.
    CHECK(run_root(input) == 855 && run_root(output) == 855);
    XrLirModule *again = NULL;
    OK(xr_lir_string_elide(output, &again));
    for (uint32_t f = 0; f < 6; ++f) {
        CHECK(count(again, f, XR_LIR_STRING_RETAIN) == retains[1][f]);
        CHECK(count(again, f, XR_LIR_STRING_DROP) == drops[1][f]);
    }
    xr_lir_module_destroy(again);
    xr_lir_module_destroy(input);
    CHECK(run_root(output) == 855);
    xr_lir_module_destroy(output);
    puts("String copies borrow parameters and move into their outliving owner");
}

static uint32_t instructions(const XrLirModule *m, uint32_t ordinal) {
    uint32_t total = 0;
    for (XrLirOp op = XR_LIR_OP_INVALID + 1; op < XR_LIR_OP_COUNT; ++op)
        total += count(m, ordinal, op);
    return total;
}
static void scalar_identity(void) {
    XrLirModule *inputs[] = {test_fib(), test_sum()};
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        XrLirModule *output = NULL;
        OK(xr_lir_string_elide(inputs[i], &output));
        OK(xr_lir_verify(output, NULL));
        CHECK(xr_lir_function_count(output) == xr_lir_function_count(inputs[i]));
        for (uint32_t f = 0; f < xr_lir_function_count(output); ++f)
            CHECK(instructions(output, f) == instructions(inputs[i], f));
        xr_lir_module_destroy(output);
        xr_lir_module_destroy(inputs[i]);
    }
    puts("Modules without String owners copy every instruction");
}

static void rejected_arguments(void) {
    XrLirModule *input = fixture(), *occupied = input, *unfinished = NULL, *output = NULL;
    CHECK(xr_lir_string_elide(NULL, &output) == XR_LIR_INVALID_ARGUMENT && !output);
    CHECK(xr_lir_string_elide(input, NULL) == XR_LIR_INVALID_ARGUMENT);
    CHECK(xr_lir_string_elide(input, &occupied) == XR_LIR_INVALID_ARGUMENT && occupied == input);
    OK(xr_lir_module_create(NULL, &unfinished));
    CHECK(xr_lir_string_elide(unfinished, &output) == XR_LIR_INVALID_ARGUMENT && !output);
    xr_lir_module_destroy(unfinished);
    CHECK(xr_lir_module_is_readable(input) && run_root(input) == 855);
    xr_lir_module_destroy(input);
    puts("String elision preserves rejected inputs and occupied outputs");
}

int main(void) {
    elided_shapes();
    scalar_identity();
    rejected_arguments();
    return 0;
}
