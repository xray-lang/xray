/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_register_vm_alloc.c - Moving stacks and representative execution OOMs
 *
 * KEY CONCEPT:
 *   Real stack growth always relocates. Preparation failures publish nothing;
 *   a failure after invocation still transfers the already owned outcome.
 */
#include "register_vm_programs.h"
#include "base/xmalloc.h"

static struct { void *pointer; size_t size; } blocks[2];
static enum { NO_FAILURE, OUTCOME_FAILURE, INITIAL_STACK_FAILURE, GROWTH_FAILURE } fault;

static void *real_calloc(size_t count, size_t size) { return xr_calloc(count, size); }
static void *execution_calloc(size_t count, size_t size);

static void *moving_realloc(void *old, size_t size) {
    if ((fault == INITIAL_STACK_FAILURE && !old) || (fault == GROWTH_FAILURE && old)) {
        fault = NO_FAILURE;
        return NULL;
    }
    size_t index = 0;
    while (index < 2 && blocks[index].pointer != old) ++index;
    XR_CHECK(index < 2, "unknown stack allocation");
    void *fresh = xr_malloc(size);
    if (!fresh) return NULL;
    if (old) {
        XR_CHECK(size >= blocks[index].size, "interpreter only grows buffers");
        memcpy(fresh, old, blocks[index].size);
        xr_free(old);
    }
    blocks[index].pointer = fresh;
    blocks[index].size = size;
    return fresh;
}

static void moving_free(void *pointer) {
    if (!pointer) return;
    for (size_t i = 0; i < 2; ++i) {
        if (blocks[i].pointer != pointer) continue;
        blocks[i].pointer = NULL;
        blocks[i].size = 0;
    }
    xr_free(pointer);
}

// Only this VM instance's allocator is interposed. The common execution
// boundary and location owner remain the actual linked production runtime.
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_calloc(count, size) execution_calloc(count, size)
#define xr_realloc(pointer, size) moving_realloc(pointer, size)
#define xr_free(pointer) moving_free(pointer)
#include "vm/register/xrbc.c"

static void *execution_calloc(size_t count, size_t size) {
    if (fault == OUTCOME_FAILURE && size == sizeof(XrRbcOutcome)) {
        fault = NO_FAILURE;
        return NULL;
    }
    return real_calloc(count, size);
}

static void invoke(const XrRbcModule *module, XrtLocationOwner *owner, bool prepare_fails,
    XrRbcStatus expected) {
    uint64_t argument = 128, result = 999;
    XrRbcExecution *execution = NULL;
    XrRbcStatus status = xr_rbc_module_prepare(module, 0, &argument, 1, owner, &execution);
    if (prepare_fails) {
        XR_CHECK(fault == NO_FAILURE, "preparation fault was reached");
        XR_CHECK(status == XR_RBC_OUT_OF_MEMORY && !execution, "failed prepare publishes nothing");
        return;
    }
    XR_CHECK(status == XR_RBC_OK && execution, "prepared execution is owned");
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    XR_CHECK(fault == NO_FAILURE, "growth fault was reached during invocation");
    XR_CHECK(outcome && xr_rbc_outcome_status(outcome) == expected, "one owned guest result");
    bool value = xr_rbc_outcome_bits(outcome, &result);
    XR_CHECK(value == (expected == XR_RBC_OK), "bits require success");
    XR_CHECK(result == (value ? argument : 999), "success or preserved failure output");
    xr_rbc_execution_free(&execution);
    xr_rbc_outcome_free(&outcome);
}

static void panic_after_growth(void) {
    uint32_t code[12], ids[12] = {0};
    XrRbcFunction function = make_countdown(code);
    // The base case faults only after both indexed arrays have repeatedly relocated.
    emit(code, 10, XR_RBC_DIV, 0, 0, 1);
    emit(code, 11, XR_RBC_RET0, 0, 0, 0);
    function.code_count = 12;
    ids[10] = 1;
    function.source_ids = ids;
    function.source_id_count = 12;
    XrRbcModule module = {&function, 1, NULL, 0};
    XrtSourceLocation location = {"grown.xr", 8, 19, 2, 19, 7};
    XrtLocationOwner *owner = NULL;
    XR_CHECK(xrt_locations_create(&location, 1, &owner) == XRT_LOCATION_OK, "owned fault site");
    for (unsigned kind = 0; kind < 2; ++kind) {
        emit(code, 10, kind ? XR_RBC_REM : XR_RBC_DIV, 0, 0, 1);
        uint64_t argument = 128;
        XrRbcExecution *execution = NULL;
        XR_CHECK(xr_rbc_module_prepare(&module, 0, &argument, 1, owner, &execution) == XR_RBC_OK,
            "prepared growing execution");
        XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
        XR_CHECK(outcome, "owned outcome after transfer");
        xr_rbc_execution_free(&execution);
        XrtArithmeticPanic facts;
        const XrtLocationOwner *borrowed = NULL;
        XR_CHECK(xr_rbc_outcome_arithmetic_panic(outcome, &facts, &borrowed), "owned panic");
        XR_CHECK(facts.code == (kind ? 421u : 420u) && facts.source_id == 1,
            "actual deepest instruction site");
        XR_CHECK(xrt_locations_get(borrowed, facts.source_id)->line == 19, "retained source");
        xr_rbc_outcome_free(&outcome);
    }
    xrt_locations_release(owner);
}

int main(void) {
    uint32_t code[11];
    XrRbcFunction function = make_countdown(code);
    XrRbcModule module = {&function, 1, NULL, 0};
    XrtLocationOwner *owner = NULL;
    XR_CHECK(xrt_locations_create(NULL, 0, &owner) == XRT_LOCATION_OK, "real empty image");
    invoke(&module, owner, false, XR_RBC_OK);
    fault = OUTCOME_FAILURE;
    invoke(&module, owner, true, XR_RBC_OUT_OF_MEMORY);
    fault = INITIAL_STACK_FAILURE;
    invoke(&module, owner, true, XR_RBC_OUT_OF_MEMORY);
    fault = GROWTH_FAILURE;
    invoke(&module, owner, false, XR_RBC_OUT_OF_MEMORY);
    invoke(&module, owner, false, XR_RBC_OK);
    xrt_locations_release(owner);
    puts("register_vm_alloc: passed");
    panic_after_growth();
    return 0;
}
