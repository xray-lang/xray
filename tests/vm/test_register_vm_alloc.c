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

static struct { void *pointer; size_t size; } blocks[3];
static enum { NO_FAILURE, OUTCOME_FAILURE, INITIAL_STACK_FAILURE, GROWTH_FAILURE,
    CELL_INITIAL_FAILURE, CELL_GROWTH_FAILURE } fault;

static void *real_calloc(size_t count, size_t size) { return xr_calloc(count, size); }
static void *execution_calloc(size_t count, size_t size);

static void *moving_realloc(void *old, size_t size) {
    size_t index = 0;
    while (index < 3 && blocks[index].pointer != old) ++index;
    XR_CHECK(index < 3, "unknown stack allocation");
    if ((fault == INITIAL_STACK_FAILURE && !old) || (fault == GROWTH_FAILURE && old) ||
        (fault == CELL_INITIAL_FAILURE && !old && index == 2) ||
        (fault == CELL_GROWTH_FAILURE && old && index == 2)) {
        fault = NO_FAILURE;
        return NULL;
    }
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
    for (size_t i = 0; i < 3; ++i) {
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
    XrRbcModule module = {&function, 1, NULL, 0, NULL, 0};
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


static void cell_growth(bool panic) {
    uint32_t root[7], body[17], ids[17] = {0};
    immediate(root, 0, XR_RBC_LOADI, 1, 7);
    immediate(root, 1, XR_RBC_CELL_NEW, 1, 0);
    emit(root, 2, XR_RBC_MOV, 4, 0, 0);
    emit(root, 3, XR_RBC_MOV, 5, 1, 0);
    immediate(root, 4, XR_RBC_CALL, 3, 1);
    emit(root, 5, XR_RBC_CELL_READ, 0, 1, 0);
    emit(root, 6, XR_RBC_RET, 0, 0, 0);
    immediate(body, 0, XR_RBC_LOADI, 2, 1);
    immediate(body, 1, XR_RBC_CELL_NEW, 2, 0);
    emit(body, 2, XR_RBC_CELL_READ, 3, 1, 0);
    emit(body, 3, XR_RBC_CELL_READ, 2, 2, 0);
    emit(body, 4, XR_RBC_ADD, 3, 3, 2);
    emit(body, 5, XR_RBC_CELL_WRITE, 1, 3, 0);
    immediate(body, 6, XR_RBC_LOADI, 2, 0);
    emit(body, 7, XR_RBC_EQ, 0, 2, 0);
    XR_CHECK(xr_rbc_jump(body + 8, 8, 15, 17), "deepest cell activation");
    immediate(body, 9, XR_RBC_LOADI, 2, 1);
    emit(body, 10, XR_RBC_SUB, 4, 0, 2);
    emit(body, 11, XR_RBC_MOV, 5, 1, 0);
    immediate(body, 12, XR_RBC_CALL, 3, 1);
    emit(body, 13, XR_RBC_RET, 3, 0, 0);
    emit(body, 14, XR_RBC_RET0, 0, 0, 0);
    emit(body, 15, panic ? XR_RBC_DIV : XR_RBC_CELL_READ, 0, panic ? 0 : 1, panic ? 2 : 0);
    emit(body, 16, XR_RBC_RET, 0, 0, 0);
    ids[15] = 1;
    XrRbcFunction functions[] = {{root, 7, NULL, 0, 6, 1, NULL, 0, 1},
        {body, 17, NULL, 0, 6, 2, ids, 17, 1}};
    XrRbcModule module = {functions, 2, NULL, 0, NULL, 0};
    XrtSourceLocation location = {"cells.xr", 8, 19, 2, 19, 7};
    XrtLocationOwner *owner = NULL;
    XR_CHECK(xrt_locations_create(&location, 1, &owner) == XRT_LOCATION_OK, "cell fault site");
    uint64_t argument = 128, bits = 999;
    XrRbcExecution *execution = NULL;
    bool prepare_fails = fault == CELL_INITIAL_FAILURE;
    bool growth_fails = fault == CELL_GROWTH_FAILURE;
    XrRbcStatus status = xr_rbc_module_prepare(&module, 0, &argument, 1, owner, &execution);
    if (prepare_fails) {
        XR_CHECK(status == XR_RBC_OUT_OF_MEMORY && !execution && fault == NO_FAILURE,
            "real initial cell allocation failed atomically");
    } else {
        XR_CHECK(status == XR_RBC_OK && execution, "cell execution is ready");
        XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
        XR_CHECK(outcome && !xr_rbc_execution_run(execution), "one owned cell outcome");
        XR_CHECK(xr_rbc_outcome_status(outcome) == (growth_fails ? XR_RBC_OUT_OF_MEMORY :
            panic ? XR_RBC_ARITHMETIC_PANIC : XR_RBC_OK), "cell outcome classification");
        xr_rbc_execution_free(&execution);
        if (!panic && !growth_fails)
            XR_CHECK(xr_rbc_outcome_bits(outcome, &bits) && bits == 136,
                "ancestor cell survives every relocated recursive bank");
        else XR_CHECK(!xr_rbc_outcome_bits(outcome, &bits) && bits == 999, "failure preserves bits");
        if (panic && !growth_fails) {
            XrtArithmeticPanic facts;
            const XrtLocationOwner *borrowed = NULL;
            XR_CHECK(xr_rbc_outcome_arithmetic_panic(outcome, &facts, &borrowed), "owned cell panic");
            XR_CHECK(facts.code == 420 && xrt_locations_get(borrowed, facts.source_id)->line == 19,
                "panic locations survive cell stack retirement");
        }
        xr_rbc_outcome_free(&outcome);
    }
    xrt_locations_release(owner);
    XR_CHECK(fault == NO_FAILURE, "requested cell fault reached");
    for (size_t i = 0; i < 3; ++i)
        XR_CHECK(!blocks[i].pointer && !blocks[i].size, "all three moving buffers reclaimed");
}

int main(void) {
    uint32_t code[11];
    XrRbcFunction function = make_countdown(code);
    XrRbcModule module = {&function, 1, NULL, 0, NULL, 0};
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
    cell_growth(false);
    fault = CELL_INITIAL_FAILURE;
    cell_growth(false);
    fault = CELL_GROWTH_FAILURE;
    cell_growth(false);
    cell_growth(false);
    cell_growth(true);
    return 0;
}
