/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_rebuild.c - Retired-input ownership and executable graph copies
 */
#include "lir_test_programs.h"
#include "lir/xlir_rebuild.h"
#include "lir/xlir_verify.h"
#include "lir/xlir_print.h"
#include "lir/xlir_c.h"
#include "vm/register/xrbc_lir.h"
#include "base/xmalloc.h"
#include <inttypes.h>

static bool print_count(void *context, const char *bytes, size_t length) {
    CHECK(bytes && length <= SIZE_MAX - *(size_t *)context);
    *(size_t *)context += length;
    return true;
}

static XrLirModule *copy_and_retire(XrLirModule *source) {
    XrLirModule *copy = NULL;
    OK(xr_lir_rebuild(source, &copy));
    CHECK(copy && copy != source && xr_lir_module_is_readable(copy));
    xr_lir_module_destroy(source);
    OK(xr_lir_verify(copy, NULL));
    size_t bytes = 0;
    OK(xr_lir_print(copy, print_count, &bytes));
    CHECK(bytes);
    return copy;
}

static uint64_t consume_vm(XrLirModule *module, uint32_t ordinal,
    const uint64_t *arguments, size_t count) {
    XrLirFunction root;
    OK(xr_lir_function_at(module, ordinal, &root));
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

static XrLirModule *make_dominator(bool condition) {
    XrLirModule *module = NULL;
    OK(xr_lir_module_create(NULL, &module));
    XrLirFunction root = test_function(module, "root", NULL, 0, XR_LIR_I64);
    XrLirType type = XR_LIR_I64;
    XrLirBlock entry = test_block(module, root, NULL, 0);
    XrLirBlock join = test_block(module, root, &type, 1);
    XrLirBlock later = test_block(module, root, NULL, 0);
    OK(xr_lir_set_entry(module, root, entry));
    test_branch(module, entry, later, NULL, 0);
    XrLirValue base = test_constant(module, later, type, 40);
    XrLirValue a = test_constant(module, later, type, 2);
    XrLirValue b = test_constant(module, later, type, 3);
    XrLirValue predicate = test_constant(module, later, XR_LIR_I1, condition);
    XrLirInstruction branch = {.op = XR_LIR_CONDBR, .type = XR_LIR_UNIT,
        .operands = &predicate, .operand_count = 1,
        .edges = {{join, &a, 1}, {join, &b, 1}}, .edge_count = 2};
    OK(xr_lir_set_terminator(module, later, &branch));
    XrLirValue parameter;
    OK(xr_lir_block_parameter(module, join, 0, &parameter));
    test_return(module, join, test_binary(module, join, XR_LIR_ADD,
        base, parameter, XR_LIR_PREDICATE_NONE));
    OK(xr_lir_module_finish(module, NULL));
    return module;
}

static XrLirModule *make_loop(void) {
    XrLirModule *module = NULL;
    OK(xr_lir_module_create(NULL, &module));
    XrLirFunction root = test_function(module, "loop", NULL, 0, XR_LIR_I64);
    XrLirType pair[] = {XR_LIR_I64, XR_LIR_I64};
    XrLirBlock entry = test_block(module, root, NULL, 0);
    XrLirBlock done = test_block(module, root, pair, 1);
    XrLirBlock body = test_block(module, root, NULL, 0);
    XrLirBlock loop = test_block(module, root, pair, 2);
    OK(xr_lir_set_entry(module, root, entry));
    XrLirValue zero = test_constant(module, entry, XR_LIR_I64, 0);
    XrLirValue one = test_constant(module, entry, XR_LIR_I64, 1);
    XrLirValue limit = test_constant(module, entry, XR_LIR_I64, 20);
    XrLirValue initial[] = {zero, zero};
    test_branch(module, entry, loop, initial, 2);
    XrLirValue i, total, answer;
    OK(xr_lir_block_parameter(module, loop, 0, &i));
    OK(xr_lir_block_parameter(module, loop, 1, &total));
    OK(xr_lir_block_parameter(module, done, 0, &answer));
    XrLirValue condition = test_binary(module, loop, XR_LIR_ICMP, i, limit, XR_LIR_LT);
    XrLirInstruction branch = {.op = XR_LIR_CONDBR, .type = XR_LIR_UNIT,
        .operands = &condition, .operand_count = 1,
        .edges = {{body, NULL, 0}, {done, &total, 1}}, .edge_count = 2};
    OK(xr_lir_set_terminator(module, loop, &branch));
    XrLirValue next[] = {
        test_binary(module, body, XR_LIR_ADD, i, one, XR_LIR_PREDICATE_NONE),
        test_binary(module, body, XR_LIR_ADD, total, i, XR_LIR_PREDICATE_NONE)};
    test_branch(module, body, loop, next, 2);
    test_return(module, done, answer);
    OK(xr_lir_module_finish(module, NULL));
    return module;
}

static XrLirModule *make_unit(void) {
    XrLirModule *module = NULL;
    OK(xr_lir_module_create(NULL, &module));
    XrLirFunction root = test_function(module, "root", NULL, 0, XR_LIR_UNIT);
    XrLirFunction helper = test_function(module, "helper", NULL, 0, XR_LIR_UNIT);
    XrLirBlock caller = test_block(module, root, NULL, 0);
    XrLirBlock callee = test_block(module, helper, NULL, 0);
    OK(xr_lir_set_entry(module, root, caller));
    OK(xr_lir_set_entry(module, helper, callee));
    XrLirInstruction call = {.op = XR_LIR_CALL, .type = XR_LIR_UNIT, .callee = helper,
        .source = {"unit-call.xr", 12, 2, 1, 2, 9}};
    OK(xr_lir_emit_effect(module, caller, &call));
    test_return(module, caller, (XrLirValue){0});
    test_return(module, callee, (XrLirValue){0});
    OK(xr_lir_module_finish(module, NULL));
    return module;
}

static void test_readability(void) {
    XrLirModule *input = NULL, *copy = NULL;
    CHECK(!xr_lir_module_is_readable(NULL));
    CHECK(xr_lir_rebuild(NULL, &copy) == XR_LIR_INVALID_ARGUMENT && !copy);
    OK(xr_lir_module_create(NULL, &input));
    CHECK(!xr_lir_module_is_readable(input));
    CHECK(xr_lir_rebuild(input, &copy) == XR_LIR_INVALID_ARGUMENT && !copy);
    OK(xr_lir_module_finish(input, NULL));
    CHECK(xr_lir_module_is_readable(input));
    CHECK(xr_lir_rebuild(input, NULL) == XR_LIR_INVALID_ARGUMENT);
    XrLirModule *occupied = input;
    CHECK(xr_lir_rebuild(input, &occupied) == XR_LIR_INVALID_ARGUMENT && occupied == input);
    OK(xr_lir_rebuild(input, &copy));
    xr_lir_module_destroy(input);
    CHECK(xr_lir_module_is_readable(copy) && !xr_lir_function_count(copy));
    OK(xr_lir_verify(copy, NULL));
    xr_lir_module_destroy(copy);
    input = copy = NULL;
    OK(xr_lir_module_create(NULL, &input));
    test_print_declaration(input);
    OK(xr_lir_module_finish(input, NULL));
    copy = copy_and_retire(input);
    CHECK(xr_lir_function_count(copy) == 1);
    xr_lir_module_destroy(copy);
    input = copy = NULL;
    OK(xr_lir_module_create(NULL, &input));
    CHECK(xr_lir_set_entry(input, (XrLirFunction){0}, (XrLirBlock){0}) == XR_LIR_INVALID_ARGUMENT);
    CHECK(!xr_lir_module_is_readable(input));
    CHECK(xr_lir_rebuild(input, &copy) == XR_LIR_INVALID_ARGUMENT && !copy);
    xr_lir_module_destroy(input);
}

static void test_invalid_graph(void) {
    XrLirModule *module = NULL, *copy = NULL;
    OK(xr_lir_module_create(NULL, &module));
    XrLirFunction root = test_function(module, "unreachable", NULL, 0, XR_LIR_UNIT);
    XrLirBlock entry = test_block(module, root, NULL, 0);
    XrLirBlock dead = test_block(module, root, NULL, 0);
    OK(xr_lir_set_entry(module, root, entry));
    test_return(module, entry, (XrLirValue){0});
    test_return(module, dead, (XrLirValue){0});
    XrLirStatus finish = xr_lir_module_finish(module, NULL);
    if (finish == XR_LIR_OK) {
        CHECK(xr_lir_module_is_readable(module));
        CHECK(xr_lir_rebuild(module, &copy) == XR_LIR_VERIFY_ERROR);
    } else {
        CHECK(finish == XR_LIR_VERIFY_ERROR && !xr_lir_module_is_readable(module));
        CHECK(xr_lir_rebuild(module, &copy) == XR_LIR_INVALID_ARGUMENT);
    }
    CHECK(!copy);
    xr_lir_module_destroy(module);
}

static void test_registry_and_bits(void) {
    XrLirType *types = xr_malloc(3 * sizeof(*types));
    CHECK(types);
    types[0] = XR_LIR_I64; types[1] = XR_LIR_PTR; types[2] = XR_LIR_I1;
    XrLirIntrinsic entries[] = {{7, types, 1, XR_LIR_I64, true},
        {91, types + 1, 2, XR_LIR_UNIT, true}};
    XrLirRegistry registry = {entries, 2};
    XrLirModule *module = NULL;
    OK(xr_lir_module_create(&registry, &module));
    xr_free(types);
    XrLirType parameter_type = XR_LIR_I64;
    XrLirFunction root = test_function(module, "intrinsic", &parameter_type, 1, XR_LIR_I64);
    XrLirBlock block = test_block(module, root, NULL, 0);
    OK(xr_lir_set_entry(module, root, block));
    XrLirValue argument, result;
    OK(xr_lir_function_parameter(module, root, 0, &argument));
    char expected[400], *path = xr_malloc(sizeof(expected) + 1);
    CHECK(path);
    memset(path, 'p', sizeof(expected));
    memcpy(path + 390, "\xc3\xa9", 2); path[392] = '\n';
    path[393] = '\\'; path[394] = '"'; path[400] = 0;
    memcpy(expected, path, sizeof(expected));
    XrLirInstruction call = {.op = XR_LIR_CALL_INTRINSIC, .type = XR_LIR_I64,
        .intrinsic = 7, .operands = &argument, .operand_count = 1,
        .source = {path, sizeof(expected), 3, 2, 3, 7}};
    OK(xr_lir_emit_value(module, block, &call, &result));
    xr_free(path);
    test_return(module, block, result);
    XrLirFunction floating = test_function(module, "payload", NULL, 0, XR_LIR_F64);
    XrLirBlock payload = test_block(module, floating, NULL, 0);
    OK(xr_lir_set_entry(module, floating, payload));
    test_constant(module, payload, XR_LIR_F64, UINT64_C(0x8000000000000000));
    test_return(module, payload, test_constant(module, payload, XR_LIR_F64, UINT64_C(0x7ff8000000000000)));
    OK(xr_lir_module_finish(module, NULL));
    module = copy_and_retire(module);
    CHECK(xr_lir_intrinsic_count(module) == 2);
    XrLirIntrinsic unused;
    OK(xr_lir_intrinsic_view(module, 91, &unused));
    CHECK(unused.parameter_count == 2 && unused.parameters[0] == XR_LIR_PTR &&
        unused.parameters[1] == XR_LIR_I1 && unused.result == XR_LIR_UNIT && unused.scalar_sync);
    OK(xr_lir_function_at(module, 0, &root));
    OK(xr_lir_block_at(module, root, 0, &block));
    XrLirInstructionView view;
    OK(xr_lir_instruction_view(module, block, 0, &view));
    CHECK(view.intrinsic == 7 && view.source.path_length == sizeof(expected));
    CHECK(!memcmp(view.source.path, expected, sizeof(expected)) && view.source.end_column == 7);
    OK(xr_lir_function_at(module, 1, &floating));
    OK(xr_lir_block_at(module, floating, 0, &payload));
    OK(xr_lir_instruction_view(module, payload, 0, &view));
    CHECK(view.type == XR_LIR_F64 && view.bits == UINT64_C(0x8000000000000000));
    OK(xr_lir_instruction_view(module, payload, 1, &view));
    CHECK(view.type == XR_LIR_F64 && view.bits == UINT64_C(0x7ff8000000000000));
    xr_lir_module_destroy(module);
}

int main(int argc, char **argv) {
    if (argc == 1) {
        test_readability();
        test_invalid_graph();
        test_registry_and_bits();
        uint64_t n = 12;
        CHECK(consume_vm(copy_and_retire(test_fib()), 1, &n, 1) == 144);
        n = 100;
        CHECK(consume_vm(copy_and_retire(test_sum()), 1, &n, 1) == 4950);
        CHECK(consume_vm(copy_and_retire(make_dominator(true)), 0, NULL, 0) == 42);
        CHECK(consume_vm(copy_and_retire(make_dominator(false)), 0, NULL, 0) == 43);
        CHECK(consume_vm(copy_and_retire(make_loop()), 0, NULL, 0) == 190);
        CHECK(consume_vm(copy_and_retire(make_unit()), 0, NULL, 0) == 0);
        puts("Owned LIR reconstruction passed");
        return 0;
    }
    CHECK(argc == 2 || argc == 3);
    bool fib = !strcmp(argv[1], "fib"), unit = !strcmp(argv[1], "unit");
    XrLirModule *module = fib ? test_fib() : unit ? make_unit() :
        !strcmp(argv[1], "loop") ? make_loop() : make_dominator(strcmp(argv[1], "false") != 0);
    module = copy_and_retire(module);
    uint32_t ordinal = fib ? 2u : 0u;
    if (argc == 2) {
        uint64_t bits = consume_vm(module, ordinal, NULL, 0);
        if (!fib) printf("%" PRIu64 "\n", bits);
    } else {
        XrLirFunction root;
        OK(xr_lir_function_at(module, ordinal, &root));
        XrCompileResourceLimits limits = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
        XrCompileResources *resources = NULL;
        CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
        XrLirCRequest request = {.module = module, .entry = root,
            .resources = resources, .translation_unit = argv[2]};
        XrLirCOutput output = {0};
        CHECK(xr_lir_emit_c(&request, &output, NULL) == XR_LIR_C_OK);
        xr_compile_resources_release(resources);
        xr_lir_module_destroy(module);
        FILE *file = fopen(argv[2], "wb");
        CHECK(file && fwrite(output.bytes, 1, output.length, file) == output.length);
        CHECK(fclose(file) == 0);
        xr_lir_c_output_free(&output);
    }
    return 0;
}
