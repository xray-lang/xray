/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_fixed_bytecode_cases.h - Exact dynamic and spilled construction execution
 */
#include "lir_fixed_bytecode_fixture.h"

static void fixed_bytecode_case(uint32_t count, bool dynamic, bool ignored, uint64_t index) {
    XrLirFunction root = {0};
    XrLirModule *module = fixed_bytecode_fixture(count, dynamic, ignored, &root);
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic = {0};
    XrRbcCompileStatus status = xr_rbc_compile(module, root, &program, &diagnostic);
    if (status != XR_RBC_COMPILE_OK) fprintf(stderr, "Fixed compile %u: %s\n", (unsigned)status, diagnostic.message);
    CHECK(status == XR_RBC_COMPILE_OK && program);
    const XrRbcModule *owned = xr_rbc_program_view(program);
    CHECK(owned->aggregate_type_count == 1 && owned->aggregate_types[0].count == count &&
        owned->functions[0].aggregate_value_count == 1 && owned->functions[0].aggregate_access_count == 2);
    CHECK(owned->functions[0].aggregate_size == (count <= 16 ? (size_t)count * 8 : count));
    if (dynamic) {
        CHECK(owned->functions[0].spill_count);
        bool spilled = false;
        for (uint32_t n = 0; n < count; ++n) spilled |= owned->functions[0].aggregate_operands[n].spill;
        CHECK(spilled);
    }
    xr_lir_module_destroy(module);
    uint64_t arguments[] = {37, index};
    XrRbcExecution *execution = NULL;
    if (count == 17 && !ignored) {
        const uint64_t invalid[] = {256, 257, UINT64_MAX};
        for (size_t n = 0; n < sizeof(invalid) / sizeof(*invalid); ++n) {
            arguments[0] = invalid[n];
            CHECK(xr_rbc_program_prepare(program, arguments, 2, &execution) == XR_RBC_INVALID && !execution);
        }
        arguments[0] = 37;
    }
    CHECK(xr_rbc_program_prepare(program, arguments, 2, &execution) == XR_RBC_OK);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution); xr_rbc_program_destroy(program);
    if (index >= count) {
        XrXirValueStatus failure = XR_XIR_VALUE_OK;
        uint32_t source = 0;
        const XrtLocationOwner *locations = NULL;
        CHECK(xr_rbc_outcome_value_failure(outcome, &failure, &source, &locations) && failure == XR_XIR_VALUE_BOUNDS);
        const XrtSourceLocation *point = xrt_locations_get(locations, source);
        CHECK(point && point->line == 37 && point->column == 4 && point->path_length == 17 &&
            !memcmp(point->path, "fixed-bytecode.xr", 17));
    } else {
        uint64_t value = 0;
        CHECK(xr_rbc_outcome_bits(outcome, &value));
        CHECK(value == (ignored ? count : dynamic ? (37u + index) & 255u : 37u));
    }
    xr_rbc_outcome_free(&outcome);
}

static void fixed_bytecode_execution(void) {
    const uint32_t counts[] = {1, 11, 16, 17, 256, 65535};
    for (size_t n = 0; n < sizeof(counts) / sizeof(*counts); ++n)
        fixed_bytecode_case(counts[n], false, false, counts[n] - 1);
    fixed_bytecode_case(300, true, false, 299);
    fixed_bytecode_case(11, false, true, UINT64_MAX);
    fixed_bytecode_case(17, false, true, 17);
}

static void fixed_bytecode_selected_edges(void) {
    const uint32_t counts[] = {1, 16, 17, 256};
    for (size_t n = 0; n < sizeof(counts) / sizeof(*counts); ++n) {
        XrLirFunction root = {0};
        XrLirModule *module = fixed_bytecode_loop_fixture(counts[n], &root);
        XrRbcProgram *program = NULL;
        XrRbcCompileDiagnostic diagnostic = {0};
        CHECK(xr_rbc_compile(module, root, &program, &diagnostic) == XR_RBC_COMPILE_OK);
        xr_lir_module_destroy(module);
        for (uint64_t iterations = 0; iterations < 4; ++iterations) {
            XrRbcExecution *execution = NULL;
            CHECK(xr_rbc_program_prepare(program, &iterations, 1, &execution) == XR_RBC_OK);
            XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
            xr_rbc_execution_free(&execution);
            uint64_t bits = 0;
            CHECK(xr_rbc_outcome_bits(outcome, &bits) && bits == (iterations & 1 ? 1107 : 711));
            xr_rbc_outcome_free(&outcome);
        }
        xr_rbc_program_destroy(program);
    }
}

static void fixed_bytecode_state_rejections(XrRbcProgram *program, bool immutable) {
    const XrRbcModule *owned = xr_rbc_program_view(program);
    CHECK(owned->function_count == 1 && owned->aggregate_type_count == 1);
    XrRbcModule module = *owned;
    XrRbcFunction function = owned->functions[0];
    size_t count = function.aggregate_access_count;
    XrRbcAggregateAccess *accesses = xr_malloc(count * sizeof(*accesses));
    XrRbcAggregateValue *values = xr_malloc((function.aggregate_value_count + 1) * sizeof(*values));
    CHECK(accesses && values);
    memcpy(accesses, function.aggregate_accesses, count * sizeof(*accesses));
    memcpy(values, function.aggregate_values, function.aggregate_value_count * sizeof(*values));
    function.aggregate_accesses = accesses;
    module.functions = &function;
    XrRbcExecution *execution = NULL;
    if (immutable) {
        uint32_t snapshot = UINT32_MAX;
        for (size_t n = 0; n < count; ++n) {
            if (accesses[n].operation != XR_RBC_AGGREGATE_STATE_READ) continue;
            if (snapshot == UINT32_MAX) snapshot = accesses[n].destination;
            else {
                accesses[n].operation = XR_RBC_AGGREGATE_STATE_WRITE;
                accesses[n].source = snapshot;
                accesses[n].destination = 0;
                break;
            }
        }
        CHECK(snapshot != UINT32_MAX);
    } else {
        XrRbcAggregateType types[] = {owned->aggregate_types[0], {XR_RBC_FIXED_U8, 18}};
        module.aggregate_types = types;
        module.aggregate_type_count = 2;
        uint32_t home = (uint32_t)function.aggregate_value_count++;
        values[home] = (XrRbcAggregateValue){1, (uint32_t)function.aggregate_size};
        function.aggregate_size += 18;
        function.aggregate_values = values;
        uint32_t previous = UINT32_MAX;
        for (size_t n = 0; n < count; ++n)
            if (accesses[n].operation == XR_RBC_AGGREGATE_STATE_READ) {
                previous = accesses[n].destination;
                accesses[n].destination = home;
                break;
            }
        CHECK(previous != UINT32_MAX);
        for (size_t n = 0; n < count; ++n)
            if (accesses[n].operation == XR_RBC_AGGREGATE_GET && accesses[n].source == previous)
                accesses[n].source = home;
        CHECK(xr_rbc_module_prepare(&module, 0, NULL, 0,
            xr_rbc_program_locations(program), &execution) == XR_RBC_INVALID && !execution);
    }
    if (immutable) CHECK(xr_rbc_module_prepare(&module, 0, NULL, 0,
        xr_rbc_program_locations(program), &execution) == XR_RBC_INVALID && !execution);
    xr_free(accesses); xr_free(values);
}

static void fixed_bytecode_state(void) {
    const uint32_t counts[] = {1, 16, 17, 256, 65535};
    for (size_t n = 0; n < sizeof(counts) / sizeof(*counts); ++n)
        for (unsigned mode = 0; mode < 4; ++mode) {
            XrLirFunction root = {0};
            XrLirModule *module = fixed_bytecode_state_fixture(counts[n], mode, &root);
            XrRbcProgram *program = NULL;
            XrRbcCompileDiagnostic diagnostic = {0};
            XrRbcCompileStatus status = xr_rbc_compile(module, root, &program, &diagnostic);
            if (status) fprintf(stderr, "Fixed State compile %u: %s\n", (unsigned)status, diagnostic.message);
            CHECK(status == XR_RBC_COMPILE_OK && program);
            if (mode == 3) {
                const XrRbcFunction *function = &xr_rbc_program_view(program)->functions[0];
                CHECK(function->spill_count);
                bool spilled = false;
                for (size_t p = 0; p < function->aggregate_access_count; ++p) {
                    XrRbcAggregateAccess access = function->aggregate_accesses[p];
                    if (access.operation == XR_RBC_AGGREGATE_STATE_READ)
                        spilled |= function->aggregate_operands[access.first].spill;
                }
                CHECK(spilled);
            }
            if (counts[n] == 17 && mode < 2) fixed_bytecode_state_rejections(program, mode == 1);
            xr_lir_module_destroy(module);
            XrRbcExecution *execution = NULL;
            CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
            XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
            xr_rbc_execution_free(&execution); xr_rbc_program_destroy(program);
            uint64_t bits = 0;
            if (mode != 2) CHECK(xr_rbc_outcome_bits(outcome, &bits) && bits == (mode == 1 ? 74 : 128));
            else {
                XrXirValueStatus failure = XR_XIR_VALUE_OK;
                uint32_t source = 0;
                const XrtLocationOwner *locations = NULL;
                CHECK(xr_rbc_outcome_value_failure(outcome, &failure, &source, &locations) && failure == XR_XIR_VALUE_BAD_ARGUMENT);
                const XrtSourceLocation *point = xrt_locations_get(locations, source);
                CHECK(point && point->line == 45 && point->column == 6 && point->path_length == 14 &&
                    !memcmp(point->path, "fixed-state.xr", 14));
            }
            xr_rbc_outcome_free(&outcome);
        }
}

static void fixed_bytecode_calls(void) {
    const uint32_t counts[] = {1, 16, 17, 256, 65535};
    for (size_t n = 0; n < sizeof(counts) / sizeof(*counts); ++n)
        for (unsigned aggregate = 0; aggregate < 2; ++aggregate) {
            XrLirFunction root = {0};
            XrLirModule *module = fixed_bytecode_calls_fixture(counts[n], aggregate != 0, counts[n] == 256, &root);
            XrRbcProgram *program = NULL;
            XrRbcCompileDiagnostic diagnostic = {0};
            XrRbcCompileStatus status = xr_rbc_compile(module, root, &program, &diagnostic);
            if (status) fprintf(stderr, "Fixed call compile %u: %s\n", (unsigned)status, diagnostic.message);
            CHECK(status == XR_RBC_COMPILE_OK && program);
            xr_lir_module_destroy(module);
            XrRbcExecution *execution = NULL;
            CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
            XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
            xr_rbc_execution_free(&execution); xr_rbc_program_destroy(program);
            uint64_t bits = 0;
            if (!aggregate) CHECK(xr_rbc_outcome_bits(outcome, &bits) && bits == 3737);
            else {
                const uint8_t *bytes = NULL;
                XrRbcAggregateType type = {0};
                CHECK(xr_rbc_outcome_aggregate(outcome, &type, &bytes) && type.kind == XR_RBC_FIXED_U8 && type.count == counts[n]);
                CHECK(!xr_rbc_outcome_bits(outcome, &bits));
                for (uint32_t p = 0; p < type.count; ++p) CHECK(bytes[p] == 37);
            }
            xr_rbc_outcome_free(&outcome);
        }
}
