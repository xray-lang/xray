/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_inline.c - Independent instances, loop prefixes and real consumers
 */
#include "lir_test_programs.h"
#include "lir/xlir_inline.h"
#include "lir/xlir_sccp.h"
#include "lir/xlir_verify.h"
#include "lir/xlir_print.h"
#include "lir/xlir_c.h"
#include "vm/register/xrbc_lir.h"
#include "base/xmalloc.h"
#include <inttypes.h>

static size_t calls_to(XrLirModule *m, uint32_t ordinal, uint32_t callee) {
    XrLirFunction f; XrLirFunctionView fv; size_t count = 0;
    OK(xr_lir_function_at(m, ordinal, &f)); OK(xr_lir_function_view(m, f, &fv));
    for (uint32_t b = 0; b < fv.block_count; ++b) {
        XrLirBlock block; XrLirBlockView bv;
        OK(xr_lir_block_at(m, f, b, &block)); OK(xr_lir_block_view(m, block, &bv));
        for (uint32_t n = 0; n < bv.instruction_count; ++n) {
            XrLirInstructionView v; OK(xr_lir_instruction_view(m, block, n, &v));
            count += v.op == XR_LIR_CALL && v.callee == callee;
        }
    }
    return count;
}
static bool print_count(void *context, const char *bytes, size_t count) {
    CHECK(bytes && count <= SIZE_MAX - *(size_t *)context); *(size_t *)context += count; return true;
}
static XrLirModule *inline_and_retire(XrLirModule *input) {
    XrLirModule *output = NULL; OK(xr_lir_inline(input, &output));
    CHECK(output && output != input); xr_lir_module_destroy(input);
    OK(xr_lir_verify(output, NULL)); size_t bytes = 0;
    OK(xr_lir_print(output, print_count, &bytes)); CHECK(bytes); return output;
}
static uint64_t run_function(XrLirModule *m, uint32_t ordinal, const uint64_t *args, size_t count) {
    XrLirFunction root; OK(xr_lir_function_at(m, ordinal, &root));
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(m, root, &program, NULL) == XR_RBC_COMPILE_OK); xr_lir_module_destroy(m);
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_program_prepare(program, args, count, &execution) == XR_RBC_OK);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution); xr_rbc_program_destroy(program);
    uint64_t bits = UINT64_MAX;
    CHECK(outcome && xr_rbc_outcome_status(outcome) == XR_RBC_OK && xr_rbc_outcome_bits(outcome, &bits));
    xr_rbc_outcome_free(&outcome); return bits;
}
static void conditional(XrLirModule *m, XrLirBlock b, XrLirValue cond, XrLirEdge a, XrLirEdge c) {
    XrLirInstruction v = {.op = XR_LIR_CONDBR, .type = XR_LIR_UNIT, .operands = &cond,
        .operand_count = 1, .edges = {a, c}, .edge_count = 2};
    OK(xr_lir_set_terminator(m, b, &v));
}
static XrLirValue call_value(XrLirModule *m, XrLirBlock b, XrLirFunction callee,
    XrLirValue argument) {
    XrLirValue result;
    XrLirInstruction v = {.op = XR_LIR_CALL, .type = XR_LIR_I64, .callee = callee,
        .operands = argument.id ? &argument : NULL, .operand_count = argument.id ? 1u : 0u,
        .source = {"caller.xr", 9, 4, 1, 4, 7}};
    OK(xr_lir_emit_value(m, b, &v, &result)); return result;
}
static void output_value(XrLirModule *m, XrLirBlock b, XrLirFunction print, XrLirValue value, uint32_t line) {
    XrLirInstruction v = {.op = XR_LIR_CALL, .type = XR_LIR_UNIT, .callee = print,
        .operands = &value, .operand_count = 1, .source = {"callee.xr", 9, line, 1, line, 5}};
    OK(xr_lir_emit_effect(m, b, &v));
}
static XrLirFunction chooser(XrLirModule *m) {
    XrLirType type = XR_LIR_I64; XrLirFunction f = test_function(m, "choose", &type, 1, type);
    XrLirBlock entry = test_block(m, f, NULL, 0), yes = test_block(m, f, NULL, 0), no = test_block(m, f, NULL, 0);
    OK(xr_lir_set_entry(m, f, entry)); XrLirValue n;
    OK(xr_lir_function_parameter(m, f, 0, &n));
    XrLirValue zero = test_constant(m, entry, type, 0);
    XrLirValue cond = test_binary(m, entry, XR_LIR_ICMP, n, zero, XR_LIR_LT);
    conditional(m, entry, cond, (XrLirEdge){yes, NULL, 0}, (XrLirEdge){no, NULL, 0});
    XrLirValue ten = test_constant(m, yes, type, 10), twenty = test_constant(m, no, type, 20);
    test_return(m, yes, test_binary(m, yes, XR_LIR_ADD, n, ten, XR_LIR_PREDICATE_NONE));
    test_return(m, no, test_binary(m, no, XR_LIR_ADD, n, twenty, XR_LIR_PREDICATE_NONE));
    return f;
}
static XrLirModule *make_twice(unsigned count) {
    XrLirModule *m = NULL; OK(xr_lir_module_create(NULL, &m));
    XrLirFunction root = test_function(m, "root", NULL, 0, XR_LIR_I64), helper = chooser(m);
    XrLirBlock b = test_block(m, root, NULL, 0); OK(xr_lir_set_entry(m, root, b));
    XrLirValue total = test_constant(m, b, XR_LIR_I64, 0);
    for (unsigned n = 0; n < count; ++n) {
        uint64_t bits = count == 2 ? (n ? 4 : UINT64_MAX - 2) : 2;
        XrLirValue value = test_constant(m, b, XR_LIR_I64, bits);
        total = test_binary(m, b, XR_LIR_ADD, total, call_value(m, b, helper, value), XR_LIR_PREDICATE_NONE);
    }
    test_return(m, b, total); OK(xr_lir_module_finish(m, NULL)); return m;
}
static XrLirModule *make_later(void) {
    XrLirModule *m = NULL; OK(xr_lir_module_create(NULL, &m));
    XrLirFunction root = test_function(m, "root", NULL, 0, XR_LIR_I64), helper = chooser(m);
    XrLirType type = XR_LIR_I64;
    XrLirBlock entry = test_block(m, root, NULL, 0), done = test_block(m, root, &type, 1);
    XrLirBlock later = test_block(m, root, NULL, 0); OK(xr_lir_set_entry(m, root, entry));
    test_branch(m, entry, later, NULL, 0);
    XrLirValue base = test_constant(m, later, type, 40), n = test_constant(m, later, type, UINT64_MAX - 2);
    XrLirValue value = call_value(m, later, helper, n), result;
    test_branch(m, later, done, &value, 1); OK(xr_lir_block_parameter(m, done, 0, &result));
    test_return(m, done, test_binary(m, done, XR_LIR_ADD, base, result, XR_LIR_PREDICATE_NONE));
    OK(xr_lir_module_finish(m, NULL)); return m;
}
static XrLirModule *make_nested(void) {
    XrLirModule *m = NULL; OK(xr_lir_module_create(NULL, &m)); XrLirType type = XR_LIR_I64;
    XrLirFunction root = test_function(m, "root", NULL, 0, type);
    XrLirFunction middle = test_function(m, "middle", &type, 1, type), leaf = test_function(m, "leaf", &type, 1, type);
    XrLirBlock a = test_block(m, root, NULL, 0), b = test_block(m, middle, NULL, 0), c = test_block(m, leaf, NULL, 0);
    OK(xr_lir_set_entry(m, root, a)); OK(xr_lir_set_entry(m, middle, b)); OK(xr_lir_set_entry(m, leaf, c));
    test_return(m, a, call_value(m, a, middle, test_constant(m, a, type, 5)));
    XrLirValue n, k; OK(xr_lir_function_parameter(m, middle, 0, &n)); OK(xr_lir_function_parameter(m, leaf, 0, &k));
    XrLirValue result = call_value(m, b, leaf, n), two = test_constant(m, b, type, 2);
    test_return(m, b, test_binary(m, b, XR_LIR_ADD, result, two, XR_LIR_PREDICATE_NONE));
    two = test_constant(m, c, type, 2);
    test_return(m, c, test_binary(m, c, XR_LIR_MUL, k, two, XR_LIR_PREDICATE_NONE));
    OK(xr_lir_module_finish(m, NULL)); return m;
}
static XrLirModule *make_effects(void) {
    XrLirModule *m = NULL; OK(xr_lir_module_create(NULL, &m));
    XrLirFunction root = test_function(m, "root", NULL, 0, XR_LIR_I64);
    XrLirFunction helper = test_function(m, "unit", NULL, 0, XR_LIR_UNIT), print = test_print_declaration(m);
    XrLirBlock a = test_block(m, root, NULL, 0), b = test_block(m, helper, NULL, 0);
    OK(xr_lir_set_entry(m, root, a)); OK(xr_lir_set_entry(m, helper, b));
    output_value(m, a, print, test_constant(m, a, XR_LIR_I64, 1), 1);
    XrLirInstruction call = {.op = XR_LIR_CALL, .type = XR_LIR_UNIT, .callee = helper,
        .source = {"caller.xr", 9, 4, 1, 4, 7}}; OK(xr_lir_emit_effect(m, a, &call));
    output_value(m, a, print, test_constant(m, a, XR_LIR_I64, 3), 3);
    test_return(m, a, test_constant(m, a, XR_LIR_I64, 0));
    output_value(m, b, print, test_constant(m, b, XR_LIR_I64, 2), 2);
    test_return(m, b, (XrLirValue){0}); OK(xr_lir_module_finish(m, NULL)); return m;
}
static XrLirModule *make_loop(void) {
    XrLirModule *m = NULL; OK(xr_lir_module_create(NULL, &m)); XrLirType type = XR_LIR_I64;
    XrLirFunction root = test_function(m, "root", NULL, 0, type), helper = test_function(m, "step", &type, 1, type);
    XrLirFunction print = test_print_declaration(m);
    XrLirBlock entry = test_block(m, root, NULL, 0), loop = test_block(m, root, &type, 1);
    XrLirBlock body = test_block(m, root, NULL, 0), done = test_block(m, root, NULL, 0);
    XrLirBlock callee = test_block(m, helper, NULL, 0);
    OK(xr_lir_set_entry(m, root, entry)); OK(xr_lir_set_entry(m, helper, callee));
    XrLirValue zero = test_constant(m, entry, type, 0), limit = test_constant(m, entry, type, 3), i, n;
    test_branch(m, entry, loop, &zero, 1); OK(xr_lir_block_parameter(m, loop, 0, &i));
    XrLirValue cond = test_binary(m, loop, XR_LIR_ICMP, i, limit, XR_LIR_LT);
    conditional(m, loop, cond, (XrLirEdge){body, NULL, 0}, (XrLirEdge){done, NULL, 0});
    output_value(m, body, print, i, 1); XrLirValue next = call_value(m, body, helper, i);
    test_branch(m, body, loop, &next, 1);
    output_value(m, done, print, i, 3); test_return(m, done, zero);
    OK(xr_lir_function_parameter(m, helper, 0, &n));
    XrLirValue ten = test_constant(m, callee, type, 10), one = test_constant(m, callee, type, 1);
    output_value(m, callee, print, test_binary(m, callee, XR_LIR_ADD, n, ten, XR_LIR_PREDICATE_NONE), 2);
    test_return(m, callee, test_binary(m, callee, XR_LIR_ADD, n, one, XR_LIR_PREDICATE_NONE));
    OK(xr_lir_module_finish(m, NULL)); return m;
}
static void mutual_body(XrLirModule *m, XrLirFunction f, XrLirFunction other) {
    XrLirBlock entry = test_block(m, f, NULL, 0), base = test_block(m, f, NULL, 0), body = test_block(m, f, NULL, 0);
    OK(xr_lir_set_entry(m, f, entry)); XrLirValue n; OK(xr_lir_function_parameter(m, f, 0, &n));
    XrLirValue zero = test_constant(m, entry, XR_LIR_I64, 0), one = test_constant(m, entry, XR_LIR_I64, 1);
    XrLirValue cond = test_binary(m, entry, XR_LIR_ICMP, n, zero, XR_LIR_LE);
    conditional(m, entry, cond, (XrLirEdge){base, NULL, 0}, (XrLirEdge){body, NULL, 0});
    test_return(m, base, zero);
    XrLirValue next = test_binary(m, body, XR_LIR_SUB, n, one, XR_LIR_PREDICATE_NONE);
    test_return(m, body, test_binary(m, body, XR_LIR_ADD, call_value(m, body, other, next), one, XR_LIR_PREDICATE_NONE));
}
static XrLirModule *make_mutual(void) {
    XrLirModule *m = NULL; OK(xr_lir_module_create(NULL, &m)); XrLirType type = XR_LIR_I64;
    XrLirFunction root = test_function(m, "root", NULL, 0, type);
    XrLirFunction a = test_function(m, "a", &type, 1, type), b = test_function(m, "b", &type, 1, type);
    XrLirBlock entry = test_block(m, root, NULL, 0); OK(xr_lir_set_entry(m, root, entry));
    test_return(m, entry, call_value(m, entry, a, test_constant(m, entry, type, 6)));
    mutual_body(m, a, b); mutual_body(m, b, a); OK(xr_lir_module_finish(m, NULL)); return m;
}
static XrLirModule *make_oversized(void) {
    XrLirModule *m = NULL; OK(xr_lir_module_create(NULL, &m)); XrLirType type = XR_LIR_I64;
    XrLirFunction root = test_function(m, "root", NULL, 0, type), helper = test_function(m, "large", &type, 1, type);
    XrLirBlock entry = test_block(m, root, NULL, 0), chain[40];
    OK(xr_lir_set_entry(m, root, entry));
    test_return(m, entry, call_value(m, entry, helper, test_constant(m, entry, type, 41)));
    for (size_t b = 0; b < 40; ++b) chain[b] = test_block(m, helper, NULL, 0);
    OK(xr_lir_set_entry(m, helper, chain[0]));
    for (size_t b = 0; b + 1 < 40; ++b) test_branch(m, chain[b], chain[b + 1], NULL, 0);
    XrLirValue n; OK(xr_lir_function_parameter(m, helper, 0, &n)); test_return(m, chain[39], n);
    OK(xr_lir_module_finish(m, NULL)); return m;
}
static XrLirModule *make_case(const char *name) {
    if (!strcmp(name, "twice")) return make_twice(2);
    if (!strcmp(name, "growth")) return make_twice(20);
    if (!strcmp(name, "later")) return make_later();
    if (!strcmp(name, "nested")) return make_nested();
    if (!strcmp(name, "effects")) return make_effects();
    if (!strcmp(name, "loop")) return make_loop();
    if (!strcmp(name, "mutual")) return make_mutual();
    CHECK(!strcmp(name, "oversized")); return make_oversized();
}
static void test_structure(void) {
    XrLirModule *m = inline_and_retire(make_twice(2)); CHECK(calls_to(m, 0, 2) == 0); xr_lir_module_destroy(m);
    m = inline_and_retire(make_twice(20)); CHECK(calls_to(m, 0, 2) > 0 && calls_to(m, 0, 2) < 20); xr_lir_module_destroy(m);
    m = inline_and_retire(make_nested()); CHECK(calls_to(m, 0, 2) == 0 && calls_to(m, 0, 3) == 1);
    CHECK(calls_to(m, 1, 3) == 0); xr_lir_module_destroy(m);
    m = inline_and_retire(make_effects()); CHECK(calls_to(m, 0, 2) == 0 && calls_to(m, 0, 3) == 3);
    XrLirFunction root; XrLirFunctionView fv; OK(xr_lir_function_at(m, 0, &root)); OK(xr_lir_function_view(m, root, &fv));
    for (uint32_t b = 0; b < fv.block_count; ++b) {
        XrLirBlock block; XrLirBlockView view; OK(xr_lir_block_at(m, root, b, &block)); OK(xr_lir_block_view(m, block, &view));
        CHECK(!view.parameter_count);
    }
    xr_lir_module_destroy(m);
    m = inline_and_retire(make_mutual()); CHECK(calls_to(m, 0, 2) == 1 && calls_to(m, 1, 3) == 1 && calls_to(m, 2, 2) == 1);
    xr_lir_module_destroy(m);
    m = inline_and_retire(make_oversized()); CHECK(calls_to(m, 0, 2) == 1); xr_lir_module_destroy(m);
    m = inline_and_retire(make_twice(2)); XrLirModule *folded = NULL; OK(xr_lir_sccp(m, &folded)); xr_lir_module_destroy(m);
    CHECK(run_function(folded, 0, NULL, 0) == 31);
}
static void test_noreturn_and_api(void) {
    XrLirModule *m = NULL, *out = NULL; CHECK(xr_lir_inline(NULL, &out) == XR_LIR_INVALID_ARGUMENT && !out);
    OK(xr_lir_module_create(NULL, &m)); CHECK(xr_lir_inline(m, &out) == XR_LIR_INVALID_ARGUMENT && !out);
    OK(xr_lir_module_finish(m, NULL)); OK(xr_lir_inline(m, &out)); XrLirModule *occupied = out;
    CHECK(xr_lir_inline(m, &out) == XR_LIR_INVALID_ARGUMENT && out == occupied);
    xr_lir_module_destroy(out); xr_lir_module_destroy(m); m = NULL; OK(xr_lir_module_create(NULL, &m));
    test_print_declaration(m); OK(xr_lir_module_finish(m, NULL)); out = NULL; OK(xr_lir_inline(m, &out));
    CHECK(xr_lir_function_count(out) == 1); xr_lir_module_destroy(out); xr_lir_module_destroy(m);
    m = NULL; OK(xr_lir_module_create(NULL, &m));
    XrLirFunction root = test_function(m, "root", NULL, 0, XR_LIR_UNIT), spin = test_function(m, "spin", NULL, 0, XR_LIR_UNIT);
    XrLirBlock a = test_block(m, root, NULL, 0), b = test_block(m, spin, NULL, 0), c = test_block(m, spin, NULL, 0);
    OK(xr_lir_set_entry(m, root, a)); OK(xr_lir_set_entry(m, spin, b));
    XrLirInstruction call = {.op = XR_LIR_CALL, .type = XR_LIR_UNIT, .callee = spin}; OK(xr_lir_emit_effect(m, a, &call));
    test_return(m, a, (XrLirValue){0}); test_branch(m, b, c, NULL, 0); test_branch(m, c, c, NULL, 0);
    OK(xr_lir_module_finish(m, NULL)); m = inline_and_retire(m); CHECK(calls_to(m, 0, 2) == 1); xr_lir_module_destroy(m);
}
static void test_owned_metadata(void) {
    XrLirType *types = xr_malloc(2 * sizeof(*types)); CHECK(types); types[0] = XR_LIR_I64; types[1] = XR_LIR_PTR;
    XrLirIntrinsic entries[] = {{7, types, 1, XR_LIR_I64, true}, {91, types, 2, XR_LIR_UNIT, true}};
    XrLirRegistry registry = {entries, 2}; XrLirModule *m = NULL; OK(xr_lir_module_create(&registry, &m)); xr_free(types);
    XrLirType type = XR_LIR_I64; XrLirFunction root = test_function(m, "root", NULL, 0, type);
    char *name = xr_malloc(13), *path = xr_malloc(401); CHECK(name && path);
    memcpy(name, "owned-helper", 13); memset(path, 'p', 400); memcpy(path + 390, "\xc3\xa9", 2); path[400] = 0;
    XrLirFunction helper = test_function(m, name, &type, 1, type); xr_free(name);
    XrLirBlock a = test_block(m, root, NULL, 0), b = test_block(m, helper, NULL, 0);
    OK(xr_lir_set_entry(m, root, a)); OK(xr_lir_set_entry(m, helper, b));
    XrLirValue value = test_constant(m, a, type, 41), unused, result, n;
    XrLirInstruction v = {.op = XR_LIR_CALL_INTRINSIC, .type = type, .intrinsic = 7,
        .operands = &value, .operand_count = 1, .source = {path, 400, 3, 1, 3, 7}};
    OK(xr_lir_emit_value(m, a, &v, &unused));
    v.op = XR_LIR_CALL; v.callee = helper; v.intrinsic = 0; v.source.line = 4; v.source.end_line = 4;
    OK(xr_lir_emit_value(m, a, &v, &result)); test_return(m, a, result);
    OK(xr_lir_function_parameter(m, helper, 0, &n)); XrLirValue one = test_constant(m, b, type, 1), operands[] = {n, one};
    v = (XrLirInstruction){.op = XR_LIR_ADD, .type = type, .operands = operands, .operand_count = 2,
        .source = {path, 400, 8, 1, 8, 7}};
    OK(xr_lir_emit_value(m, b, &v, &result));
    v = (XrLirInstruction){.op = XR_LIR_RET, .type = XR_LIR_UNIT, .operands = &result,
        .operand_count = 1, .source = {path, 400, 9, 1, 9, 7}};
    OK(xr_lir_set_terminator(m, b, &v)); xr_free(path);
    XrLirFunction floating = test_function(m, "payload", NULL, 0, XR_LIR_F64);
    XrLirBlock payload = test_block(m, floating, NULL, 0); OK(xr_lir_set_entry(m, floating, payload));
    test_constant(m, payload, XR_LIR_F64, UINT64_C(0x8000000000000000));
    test_return(m, payload, test_constant(m, payload, XR_LIR_F64, UINT64_C(0x7ff8000000000000)));
    OK(xr_lir_module_finish(m, NULL)); m = inline_and_retire(m);
    XrLirFunctionView fv; OK(xr_lir_function_at(m, 1, &helper)); OK(xr_lir_function_view(m, helper, &fv));
    CHECK(fv.name_length == 12 && !memcmp(fv.name, "owned-helper", 12));
    XrLirIntrinsic entry; OK(xr_lir_intrinsic_view(m, 91, &entry)); CHECK(entry.parameters[1] == XR_LIR_PTR && entry.scalar_sync);
    CHECK(xr_lir_intrinsic_count(m) == 2 && calls_to(m, 0, 2) == 0);
    OK(xr_lir_function_at(m, 0, &root)); OK(xr_lir_function_view(m, root, &fv));
    bool prefix = false, cloned = false, intrinsic = false, returned = false;
    for (uint32_t block = 0; block < fv.block_count; ++block) {
        XrLirBlock handle; XrLirBlockView bv; OK(xr_lir_block_at(m, root, block, &handle)); OK(xr_lir_block_view(m, handle, &bv));
        for (uint32_t i = 0; i < bv.instruction_count; ++i) {
            XrLirInstructionView in; OK(xr_lir_instruction_view(m, handle, i, &in));
            if (in.source.path_length != 400) continue;
            CHECK(in.source.path[0] == 'p' && !memcmp(in.source.path + 390, "\xc3\xa9", 2));
            prefix |= in.op == XR_LIR_BR && in.source.line == 4;
            cloned |= in.op == XR_LIR_ADD && in.source.line == 8;
            intrinsic |= in.op == XR_LIR_CALL_INTRINSIC && in.intrinsic == 7;
            returned |= in.op == XR_LIR_BR && in.source.line == 9;
        }
    }
    CHECK(prefix && cloned && intrinsic && returned);
    OK(xr_lir_function_at(m, 2, &floating)); OK(xr_lir_block_at(m, floating, 0, &payload));
    XrLirInstructionView bits;
    OK(xr_lir_instruction_view(m, payload, 0, &bits)); CHECK(bits.bits == UINT64_C(0x8000000000000000));
    OK(xr_lir_instruction_view(m, payload, 1, &bits)); CHECK(bits.bits == UINT64_C(0x7ff8000000000000));
    xr_lir_module_destroy(m);
}
static void emit_c(XrLirModule *m, const char *path) {
    XrLirFunction root; OK(xr_lir_function_at(m, 0, &root));
    XrCompileResourceLimits limits = {UINT64_MAX, UINT64_MAX, UINT64_MAX}; XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrLirCRequest request = {.module = m, .entry = root, .resources = resources, .translation_unit = path};
    XrLirCOutput out = {0}; CHECK(xr_lir_emit_c(&request, &out, NULL) == XR_LIR_C_OK);
    xr_compile_resources_release(resources); xr_lir_module_destroy(m);
    FILE *file = fopen(path, "wb"); CHECK(file && fwrite(out.bytes, 1, out.length, file) == out.length);
    CHECK(fclose(file) == 0); xr_lir_c_output_free(&out);
}
int main(int argc, char **argv) {
    if (argc == 1) {
        test_structure(); test_noreturn_and_api(); test_owned_metadata();
        const char *names[] = {"twice", "growth", "later", "nested", "mutual", "oversized"};
        const uint64_t answers[] = {31, 440, 47, 12, 6, 41};
        for (size_t i = 0; i < 6; ++i) {
            CHECK(run_function(make_case(names[i]), 0, NULL, 0) == answers[i]);
            CHECK(run_function(inline_and_retire(make_case(names[i])), 0, NULL, 0) == answers[i]);
        }
        uint64_t n = 12; CHECK(run_function(inline_and_retire(test_fib()), 1, &n, 1) == 144);
        n = 100; CHECK(run_function(inline_and_retire(test_sum()), 1, &n, 1) == 4950);
        puts("Bounded call expansion, stable prefixes and ownership passed"); return 0;
    }
    CHECK(argc == 3 || argc == 4); XrLirModule *m = make_case(argv[1]);
    CHECK(!strcmp(argv[2], "before") || !strcmp(argv[2], "after"));
    if (!strcmp(argv[2], "after")) m = inline_and_retire(m);
    if (argc == 4) emit_c(m, argv[3]);
    else { uint64_t result = run_function(m, 0, NULL, 0);
        if (strcmp(argv[1], "effects") && strcmp(argv[1], "loop")) printf("%" PRIu64 "\n", result); }
    return 0;
}
