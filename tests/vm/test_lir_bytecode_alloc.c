/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_bytecode_alloc.c - Relocating compilation and representative OOMs.
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

static size_t attempts, fail_at = SIZE_MAX;
static struct { void *pointer; size_t size; } buffers[32];

static void *allocate(size_t size) {
    if (++attempts == fail_at) return NULL;
    return xr_malloc(size);
}

static void *allocate_zero(size_t count, size_t size) {
    if (++attempts == fail_at) return NULL;
    return xr_calloc(count, size);
}

static void release(void *pointer) {
    for (size_t i = 0; i < 32; ++i)
        if (buffers[i].pointer == pointer) {
            buffers[i].pointer = NULL;
            buffers[i].size = 0;
        }
    xr_free(pointer);
}

static void *relocate(void *old, size_t size) {
    if (++attempts == fail_at) return NULL;
    size_t i = 0;
    while (i < 32 && buffers[i].pointer != old) ++i;
    CHECK(i < 32);
    void *fresh = xr_malloc(size);
    CHECK(fresh);
    if (old) {
        memcpy(fresh, old, buffers[i].size < size ? buffers[i].size : size);
        xr_free(old);
    }
    buffers[i].pointer = fresh;
    buffers[i].size = size;
    return fresh;
}

#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_malloc(size) allocate(size)
#define xr_calloc(count, size) allocate_zero(count, size)
#define xr_realloc(old, size) relocate(old, size)
#define xr_free(pointer) release(pointer)
static bool fail_location_allocation;
static void *allocate_location(size_t size) {
    if (fail_location_allocation) {
        fail_location_allocation = false;
        return NULL;
    }
    return allocate(size);
}
#undef xr_malloc
#define xr_malloc(size) allocate_location(size)
#include "runtime/native/xnative_locations.c"
#undef xr_malloc
#define xr_malloc(size) allocate(size)
#include "vm/register/xrbc_lir.c"

static void source_map_failures(void) {
    for (unsigned grown = 0; grown < 2; ++grown) {
        XrRbcCompileDiagnostic diagnostic = {0};
        RbcLirCompiler compiler = {.diagnostic = &diagnostic};
        size_t words = 0;
        if (grown) {
            compiler.current_source_id = 1;
            CHECK(rbc_lir_emit(&compiler, XR_RBC_LOADI, 0, 0, 0, 0));
            words = compiler.code_capacity;
        }
        for (size_t i = compiler.code_count; i < words; ++i) {
            compiler.current_source_id = (uint32_t)i + 1;
            CHECK(rbc_lir_emit(&compiler, XR_RBC_LOADI, 0, 0, 0, (int64_t)i));
        }
        // Code growth succeeds and relocates; the following map allocation fails.
        attempts = 0;
        fail_at = 2;
        CHECK(!rbc_lir_emit(&compiler, XR_RBC_LOADI, 0, 0, 0, 99));
        CHECK(compiler.status == XR_RBC_COMPILE_OUT_OF_MEMORY && compiler.code_count == words);
        CHECK(compiler.code && compiler.code_capacity > words && compiler.source_id_capacity == words);
        for (size_t i = 0; i < words; ++i)
            CHECK(compiler.source_ids[i] == i + 1);
        xr_free(compiler.code);
        xr_free(compiler.source_ids);
        fail_at = SIZE_MAX;
        attempts = 0;
    }
    XrRbcCompileDiagnostic diagnostic = {0};
    RbcLirCompiler compiler = {.diagnostic = &diagnostic};
    for (uint32_t i = 0; i < 40; ++i) {
        compiler.current_source_id = i + 1;
        CHECK(rbc_lir_emit(&compiler, XR_RBC_LOADI, 0, 0, 0, i));
    }
    CHECK(compiler.code_count == 40);
    for (size_t i = 0; i < 40; ++i) CHECK(compiler.source_ids[i] == i + 1);
    xr_free(compiler.code);
    xr_free(compiler.source_ids);
    attempts = 0;
}

static void compiled_source_maps(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType type = XR_LIR_I64;
    XrLirFunction root = test_function(m, "root", NULL, 0, type);
    XrLirFunction helper = test_function(m, "helper", &type, 1, type);
    XrLirBlock entry = test_block(m, root, NULL, 0), body = test_block(m, helper, NULL, 0);
    OK(xr_lir_set_entry(m, root, entry));
    OK(xr_lir_set_entry(m, helper, body));
    XrLirInstruction instruction = {.op = XR_LIR_CONST, .type = type, .bits = 42,
        .source = {"mapped.xr", 9, 2, 3, 2, 5}};
    XrLirValue value, argument;
    OK(xr_lir_emit_value(m, entry, &instruction, &value));
    test_return(m, entry, test_call(m, entry, helper, value));
    OK(xr_lir_function_parameter(m, helper, 0, &argument));
    test_return(m, body, argument);
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(m, root, &program, NULL) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(m);
    CHECK(program->module.function_count == 2 && xrt_locations_count(program->locations) == 1);
    bool located = false;
    for (size_t f = 0; f < 2; ++f) {
        const XrRbcFunction *function = &program->module.functions[f];
        CHECK(function->source_ids && function->source_id_count == function->code_count);
        CHECK(function->source_ids[0] == 0);
        for (size_t pc = 0; pc < function->code_count; ++pc) {
            CHECK(function->source_ids[pc] <= 1);
            if (f) CHECK(function->source_ids[pc] == 0);
            located |= function->source_ids[pc] != 0;
        }
    }
    uint64_t bits = 0;
    CHECK(located && execute(program, NULL, 0, &bits) == XR_RBC_OK && bits == 42);
    xr_rbc_program_destroy(program);
}

int main(void) {
    source_map_failures();
    XrLirModule *m = test_fib();
    XrLirFunction root;
    OK(xr_lir_function_at(m, 1, &root));
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic;
    CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_OK);
    for (size_t f = 0; f < program->module.function_count; ++f) {
        const XrRbcFunction *function = &program->module.functions[f];
        CHECK(function->source_ids && function->source_id_count == function->code_count);
        CHECK(function->source_ids[0] == 0);
    }
    size_t total = attempts;
    xr_rbc_program_destroy(program);
    const size_t failures[] = {1, total / 2, total};
    for (size_t i = 0; i < 3; ++i) {
        attempts = 0; fail_at = failures[i]; program = NULL;
        CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_OUT_OF_MEMORY);
        CHECK(!program && diagnostic.status == XR_RBC_COMPILE_OUT_OF_MEMORY);
        fail_at = SIZE_MAX;
        CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_OK);
        uint64_t n = 12, result = 0;
        CHECK(execute(program, &n, 1, &result) == XR_RBC_OK && result == 144);
        xr_rbc_program_destroy(program);
    }
    program = NULL;
    fail_location_allocation = true;
    CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_OUT_OF_MEMORY);
    CHECK(!program && !fail_location_allocation && diagnostic.status == XR_RBC_COMPILE_OUT_OF_MEMORY);
    CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_OK);
    CHECK(xr_rbc_program_locations(program));
    xr_rbc_program_destroy(program);
    xr_lir_module_destroy(m);
    compiled_source_maps();
    puts("lir_bytecode_alloc: passed (representative compiler/owner failures, relocating buffers)");
    return 0;
}
