/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_map_alloc_cases.h - Map compiler and runtime allocation failure accounting
 *
 * KEY CONCEPT:
 *   Every observed failure leaves the receiving owner unpublished or drains
 *   its already owned outcome before the next attempt.
 */
#include "../lir/lir_map_fixture.h"
static void map_compiler_failures(void) {
    XrLirModule *module = map_fixture(1);
    XrLirFunction root = {module, 1};
    XrRbcProgram *program = NULL;
    attempts = 0; fail_at = SIZE_MAX;
    CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OK);
    size_t total = attempts;
    xr_rbc_program_destroy(program); program = NULL;
    CHECK(total && !live_allocations);
    for (size_t n = 1; n <= total; ++n) {
        attempts = 0; fail_at = n;
        CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OUT_OF_MEMORY);
        CHECK(!program && !live_allocations);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(module);
    size_t retained = live_allocations;
    uint64_t bits = 0;
    attempts = 0;
    CHECK(execute(program, NULL, 0, &bits) == XR_RBC_OK && bits == 42);
    size_t runtime = attempts;
    CHECK(live_allocations == retained);
    for (size_t n = 1; n <= runtime; ++n) {
        attempts = 0; fail_at = n;
        CHECK(execute(program, NULL, 0, &bits) == XR_RBC_OUT_OF_MEMORY);
        CHECK(live_allocations == retained);
    }
    fail_at = SIZE_MAX;
    xr_rbc_program_destroy(program);
    CHECK(!live_allocations);
    printf("Map compiler/runtime allocation failures %zu/%zu drained\n", total, runtime);
    attempts = 0;
}

static void map_borrowed_literal_fault_and_skip(void) {
    CArrayFixture c = {0};
    OK(xr_lir_module_create(NULL, &c.module));
    OK(xr_lir_map_catalog_install(c.module, false, false, false, false, false));
    XrLirType type = XR_LIR_I64;
    XrLirFunction root = test_function(c.module, "lazy_key", &type, 1, type);
    XrLirBlock entry = test_block(c.module, root, NULL, 0);
    XrLirBlock read = test_block(c.module, root, NULL, 0);
    XrLirBlock skip = test_block(c.module, root, NULL, 0);
    OK(xr_lir_set_entry(c.module, root, entry));
    XrLirValue take = {0};
    OK(xr_lir_function_parameter(c.module, root, 0, &take));
    XrLirValue zero = ca_constant(&c, entry, 0);
    XrLirValue condition = test_binary(c.module, entry, XR_LIR_ICMP, zero, take, XR_LIR_LT);
    OK(xr_lir_set_terminator(c.module, entry, &(XrLirInstruction){.op=XR_LIR_CONDBR, .type=XR_LIR_UNIT,
        .operands=&condition, .operand_count=1, .edges={{read,NULL,0},{skip,NULL,0}}, .edge_count=2}));
    ca_return(&c, skip, ca_constant(&c, skip, 17));

    XrLirValue initial[] = {ca_text(&c, read, "answer", 6), ca_constant(&c, read, 42)};
    XrLirValue map = map_fixture_call(&c, read, XR_LIR_MAP_OP_NEW, XR_LIR_MAP_STRING_I64, initial, 2);
    ca_effect(&c, read, XR_LIR_STRING_DROP, initial[0]);
    XrLirLiteral bytes = {0};
    XrLirValue key = {0};
    OK(xr_lir_add_literal(c.module, "answer", 6, &bytes));
    OK(xr_lir_emit_value(c.module, read, &(XrLirInstruction){.op=XR_LIR_STRING_LITERAL,
        .type=XR_LIR_STRING, .bits=bytes.id, .source={"lazy-key.xr",11,7,5,7,13}}, &key));
    XrLirValue args[] = {map, key};
    XrLirValue result = map_fixture_call(&c, read, XR_LIR_MAP_OP_INDEX, type, args, 2);
    ca_effect(&c, read, XR_LIR_STRING_DROP, key);
    ca_effect(&c, read, XR_LIR_MAP_DROP, map);
    ca_return(&c, read, result);
    OK(xr_lir_module_finish(c.module, NULL));

    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(c.module, root, &program, NULL) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(c.module);
    const XrRbcFunction *function = &program->module.functions[0];
    uint32_t literal_source = 0;
    uint32_t *words = (uint32_t *)(void *)function->code;
    for (size_t pc = 0; pc < function->code_count; ++pc) {
        if ((words[pc] & 255u) != XR_RBC_STRING_LITERAL_BORROW) continue;
        CHECK(!literal_source);
        literal_source = function->source_ids[pc];
        uint32_t original = words[pc];
        words[pc] = (original & ~255u) | XR_RBC_VALUE_ACCESS;
        CHECK(!xr_rbc_value_instruction_valid(&program->module, function, pc));
        words[pc] = original;
    }
    CHECK(literal_source);

    size_t literal_budget = SIZE_MAX;
    for (size_t budget = 0;; ++budget) {
        XrRbcExecution *execution = NULL;
        uint64_t argument = 1;
        CHECK(xr_rbc_program_prepare(program, &argument, 1, &execution) == XR_RBC_OK);
        XrtBillingOwner *billing = NULL;
        CHECK(xrt_native_values_prepare(&execution->values, &billing) == XRT_MANAGED_OK &&
            xrt_billing_owner_retain(billing));
        XrtBillingStats control = xrt_billing_owner_stats(billing);
        xrt_billing_owner_fail_after(billing, budget);
        XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
        CHECK(outcome && !execution->stack.values && xrt_value_root_empty(&execution->values));
        XrRbcStatus status = xr_rbc_outcome_status(outcome);
        uint64_t bits = 0;
        if (status == XR_RBC_OK) CHECK(xr_rbc_outcome_bits(outcome, &bits) && bits == 42);
        else {
            XrXirValueStatus failure;
            uint32_t source;
            const XrtLocationOwner *locations;
            CHECK(status == XR_RBC_VALUE_FAILURE &&
                xr_rbc_outcome_value_failure(outcome, &failure, &source, &locations) && failure == XR_XIR_VALUE_OOM);
            if (source == literal_source) literal_budget = budget;
        }
        xrt_billing_owner_fail_after(billing, SIZE_MAX);
        xr_rbc_execution_free(&execution);
        xr_rbc_outcome_free(&outcome);
        XrtBillingStats retired = xrt_billing_owner_stats(billing);
        CHECK(retired.blocks == control.blocks && retired.bytes == control.bytes);
        xrt_billing_owner_drop(billing);
        if (status == XR_RBC_OK) break;
        CHECK(budget < 64);
    }
    CHECK(literal_budget != SIZE_MAX);

    /* The exact fault that prevents first use has no effect on the edge that skips it. */
    XrRbcExecution *execution = NULL;
    uint64_t argument = 0, bits = 0;
    CHECK(xr_rbc_program_prepare(program, &argument, 1, &execution) == XR_RBC_OK);
    XrtBillingOwner *billing = NULL;
    CHECK(xrt_native_values_prepare(&execution->values, &billing) == XRT_MANAGED_OK);
    xrt_billing_owner_fail_after(billing, literal_budget);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    CHECK(outcome && xr_rbc_outcome_status(outcome) == XR_RBC_OK &&
        xr_rbc_outcome_bits(outcome, &bits) && bits == 17);
    xr_rbc_execution_free(&execution);
    xr_rbc_outcome_free(&outcome);
    xr_rbc_program_destroy(program);
    CHECK(!live_allocations);
}
