/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_yield.c - Mini Yieldable C function test library
 *
 * KEY CONCEPT:
 *   Isolated testing of Yieldable C functions and coroutine scheduling.
 *
 * TEST SCENARIOS:
 *   1. simple()      - Basic yield test
 *   2. add(a,b)      - Yield with state
 *   3. sync()        - Normal C function comparison
 *   4. multi_yield() - Multiple yields (state machine)
 *   5. chain(n)      - Recursive yield call chain
 *   6. error_test()  - Error handling test
 *   7. cancel_test() - Cancellation handling test
 *   8. counter()     - Concurrent counter
 *   9. nested()      - Simulate nested yield scenario
 *  10. long_task(n)  - Long running task simulation
 */

#include "xray.h"
#include "xchecks.h"
#include "../common.h"
#include "../../src/vm/xvm.h"
#include "../../src/coro/xyieldable.h"
#include "../../src/coro/xcoroutine.h"
#include "../../src/runtime/xexec_frame.h"
#include "../../src/base/xmalloc.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

// Global counter (for concurrency testing). Declared atomic so that the
// concurrent increment path used by test_yield.counter_inc remains race-free
// when driven from multiple workers — TSAN/ASAN would otherwise flag it.
static _Atomic int64_t g_counter = 0;

/* ========================================================================== */
// Scenario 1: Basic yield test
/* ========================================================================== */

static XrCFuncResult test_yield_continue(XrayIsolate *X, int status, void *ctx, XrValue *result) {
    (void)X;
    (void)ctx;
    (void)status;
    *result = xr_int(42);
    return XR_CFUNC_DONE;
}

static XrCFuncResult test_yield_simple(XrayIsolate *X, XrValue *args, int argc, XrValue *result) {
    (void)args;
    (void)argc;
    (void)result;
    return xr_yield(X, test_yield_continue, NULL);
}

/* ========================================================================== */
// Scenario 2: Yield with state
/* ========================================================================== */

typedef struct {
    int64_t a;
    int64_t b;
} AddState;

static XrCFuncResult test_yield_add_continue(XrayIsolate *X, int status, void *ctx, XrValue *result) {
    (void)X;
    (void)status;
    AddState *state = (AddState *)ctx;
    int64_t sum = state->a + state->b;
    xr_free(state);
    *result = xr_int(sum);
    return XR_CFUNC_DONE;
}

static XrCFuncResult test_yield_add(XrayIsolate *X, XrValue *args, int argc, XrValue *result) {
    if (argc < 2) {
        *result = xr_null();
        return XR_CFUNC_ERROR;
    }
    int64_t a = XR_IS_INT(args[0]) ? XR_TO_INT(args[0]) : 0;
    int64_t b = XR_IS_INT(args[1]) ? XR_TO_INT(args[1]) : 0;

    AddState *state = (AddState *)xr_malloc(sizeof(AddState));
    if (!state) { *result = xr_null(); return XR_CFUNC_ERROR; }
    state->a = a;
    state->b = b;
    return xr_yield(X, test_yield_add_continue, state);
}

/* ========================================================================== */
// Scenario 3: Normal C function (non-Yieldable)
/* ========================================================================== */

static XrValue test_yield_sync(XrayIsolate *X, XrValue *args, int argc) {
    (void)X;
    (void)args;
    (void)argc;
    return xr_int(100);
}

/* ========================================================================== */
// Scenario 4: Multiple yields (state machine pattern)
// Simulates operations requiring multiple suspensions: connect->send->recv->close
/* ========================================================================== */

typedef struct {
    int step;           // Current step: 0=start, 1=processing, 2=done
    int64_t value;      // Accumulated value
    int max_steps;      // Maximum number of steps
} MultiYieldState;

static XrCFuncResult multi_yield_continue(XrayIsolate *X, int status, void *ctx, XrValue *result);

static XrCFuncResult multi_yield_step(XrayIsolate *X, MultiYieldState *state, XrValue *result) {
    state->step++;
    state->value += state->step * 10;  // Each step adds step*10

    if (state->step >= state->max_steps) {
        // All steps completed
        int64_t final_value = state->value;
        xr_free(state);
        *result = xr_int(final_value);
        return XR_CFUNC_DONE;
    }

    // Continue to next step, yield again
    return xr_yield(X, multi_yield_continue, state);
}

static XrCFuncResult multi_yield_continue(XrayIsolate *X, int status, void *ctx, XrValue *result) {
    (void)status;
    MultiYieldState *state = (MultiYieldState *)ctx;
    return multi_yield_step(X, state, result);
}

/*
 * test_yield.multi_yield(steps) -> int
 * Executes 'steps' yields, each adding step*10, returns total sum
 * Example: multi_yield(3) = 10 + 20 + 30 = 60
 */
static XrCFuncResult test_yield_multi(XrayIsolate *X, XrValue *args, int argc, XrValue *result) {
    int max_steps = (argc > 0 && XR_IS_INT(args[0])) ? (int)XR_TO_INT(args[0]) : 3;
    if (max_steps < 1) max_steps = 1;
    if (max_steps > 100) max_steps = 100;

    MultiYieldState *state = (MultiYieldState *)xr_malloc(sizeof(MultiYieldState));
    if (!state) { *result = xr_null(); return XR_CFUNC_ERROR; }
    state->step = 0;
    state->value = 0;
    state->max_steps = max_steps;

    return multi_yield_step(X, state, result);
}

/* ========================================================================== */
// Scenario 5: Recursive yield call chain
// Simulates chain(n) = n + chain(n-1), but yields at each level
/* ========================================================================== */

typedef struct {
    int64_t n;
    int64_t accumulated;  // Accumulated value
} ChainState;

static XrCFuncResult chain_continue(XrayIsolate *X, int status, void *ctx, XrValue *result);

static XrCFuncResult chain_step(XrayIsolate *X, ChainState *state, XrValue *result) {
    if (state->n <= 0) {
        int64_t final_value = state->accumulated;
        xr_free(state);
        *result = xr_int(final_value);
        return XR_CFUNC_DONE;
    }

    // Accumulate current n, then yield to continue processing n-1
    state->accumulated += state->n;
    state->n--;

    return xr_yield(X, chain_continue, state);
}

static XrCFuncResult chain_continue(XrayIsolate *X, int status, void *ctx, XrValue *result) {
    (void)status;
    ChainState *state = (ChainState *)ctx;
    return chain_step(X, state, result);
}

/*
 * test_yield.chain(n) -> int
 * Computes 1+2+...+n, yielding once for each number processed
 * Example: chain(5) = 1+2+3+4+5 = 15
 */
static XrCFuncResult test_yield_chain(XrayIsolate *X, XrValue *args, int argc, XrValue *result) {
    int64_t n = (argc > 0 && XR_IS_INT(args[0])) ? XR_TO_INT(args[0]) : 5;
    if (n < 0) n = 0;
    if (n > 1000) n = 1000;

    ChainState *state = (ChainState *)xr_malloc(sizeof(ChainState));
    if (!state) { *result = xr_null(); return XR_CFUNC_ERROR; }
    state->n = n;
    state->accumulated = 0;

    return chain_step(X, state, result);
}

/* ========================================================================== */
// Scenario 6: Error handling test
/* ========================================================================== */

typedef struct {
    int should_error;
    int error_code;
} ErrorState;

static XrCFuncResult error_continue(XrayIsolate *X, int status, void *ctx, XrValue *result) {
    (void)X;
    ErrorState *state = (ErrorState *)ctx;

    if (status == XR_RESUME_CANCELLED) {
        // Cancelled
        xr_free(state);
        *result = xr_int(-1);
        return XR_CFUNC_DONE;
    }

    if (state->should_error) {
        int code = state->error_code;
        xr_free(state);
        *result = xr_int(code);
        return XR_CFUNC_ERROR;
    }

    xr_free(state);
    *result = xr_int(200);  // Success
    return XR_CFUNC_DONE;
}

/*
 * test_yield.error_test(should_error, error_code) -> int
 * If should_error is true, returns error after yield
 */
static XrCFuncResult test_yield_error(XrayIsolate *X, XrValue *args, int argc, XrValue *result) {
    int should_error = (argc > 0 && XR_IS_INT(args[0])) ? (int)XR_TO_INT(args[0]) : 0;
    int error_code = (argc > 1 && XR_IS_INT(args[1])) ? (int)XR_TO_INT(args[1]) : -100;

    ErrorState *state = (ErrorState *)xr_malloc(sizeof(ErrorState));
    if (!state) { *result = xr_null(); return XR_CFUNC_ERROR; }
    state->should_error = should_error;
    state->error_code = error_code;

    return xr_yield(X, error_continue, state);
}

/* ========================================================================== */
// Scenario 7: Cancellation handling test
/* ========================================================================== */

typedef struct {
    int cleanup_called;
    int64_t resource_id;
} CancelState;

static XrCFuncResult cancel_continue(XrayIsolate *X, int status, void *ctx, XrValue *result) {
    (void)X;
    CancelState *state = (CancelState *)ctx;

    if (status == XR_RESUME_CANCELLED) {
        // Coroutine cancelled, perform cleanup
        state->cleanup_called = 1;
        int64_t resource = state->resource_id;
        xr_free(state);
        *result = xr_int(-resource);  // Return negative value to indicate cancelled
        return XR_CFUNC_DONE;
    }

    int64_t resource = state->resource_id;
    xr_free(state);
    *result = xr_int(resource);  // Normal completion
    return XR_CFUNC_DONE;
}

/*
 * test_yield.cancel_test(resource_id) -> int
 * Simulates a cancellable operation, returns resource_id (negative if cancelled)
 */
static XrCFuncResult test_yield_cancel(XrayIsolate *X, XrValue *args, int argc, XrValue *result) {
    int64_t resource_id = (argc > 0 && XR_IS_INT(args[0])) ? XR_TO_INT(args[0]) : 42;

    CancelState *state = (CancelState *)xr_malloc(sizeof(CancelState));
    if (!state) { *result = xr_null(); return XR_CFUNC_ERROR; }
    state->cleanup_called = 0;
    state->resource_id = resource_id;

    return xr_yield(X, cancel_continue, state);
}

/* ========================================================================== */
// Scenario 8: Concurrent counter
/* ========================================================================== */

static XrCFuncResult counter_continue(XrayIsolate *X, int status, void *ctx, XrValue *result) {
    (void)X;
    (void)status;
    (void)ctx;
    // Read global counter after yield using an acquire load so the value
    // observed by the test reflects all prior atomic increments.
    *result = xr_int(atomic_load_explicit(&g_counter, memory_order_acquire));
    return XR_CFUNC_DONE;
}

/*
 * test_yield.counter_inc() -> int
 * Increments global counter, yields, then returns current value.
 */
static XrCFuncResult test_yield_counter_inc(XrayIsolate *X, XrValue *args, int argc, XrValue *result) {
    (void)args;
    (void)argc;
    (void)result;
    atomic_fetch_add_explicit(&g_counter, 1, memory_order_relaxed);
    return xr_yield(X, counter_continue, NULL);
}

/*
 * test_yield.counter_get() -> int
 * Gets global counter value (synchronous function).
 */
static XrValue test_yield_counter_get(XrayIsolate *X, XrValue *args, int argc) {
    (void)X;
    (void)args;
    (void)argc;
    return xr_int(atomic_load_explicit(&g_counter, memory_order_acquire));
}

/*
 * test_yield.counter_reset() -> int
 * Resets global counter (synchronous function).
 */
static XrValue test_yield_counter_reset(XrayIsolate *X, XrValue *args, int argc) {
    (void)X;
    (void)args;
    (void)argc;
    int64_t old = atomic_exchange_explicit(&g_counter, 0, memory_order_acq_rel);
    return xr_int(old);
}

/* ========================================================================== */
// Scenario 9: Nested yield simulation
// Simulates outer -> yield -> inner -> yield -> result
/* ========================================================================== */

typedef struct {
    int phase;          // 0=outer_yield, 1=inner_yield, 2=done
    int64_t outer_val;
    int64_t inner_val;
} NestedState;

static XrCFuncResult nested_continue(XrayIsolate *X, int status, void *ctx, XrValue *result);

static XrCFuncResult nested_step(XrayIsolate *X, NestedState *state, XrValue *result) {
    switch (state->phase) {
        case 0:
            // Outer yield done, start inner
            state->phase = 1;
            state->outer_val = 100;
            return xr_yield(X, nested_continue, state);

        case 1:
            // Inner yield done, compute result
            state->phase = 2;
            state->inner_val = 50;
            // Yield once more to simulate deeper level
            return xr_yield(X, nested_continue, state);

        case 2:
        default: {
            // All levels completed
            int64_t final_val = state->outer_val + state->inner_val;
            xr_free(state);
            *result = xr_int(final_val);
            return XR_CFUNC_DONE;
        }
    }
}

static XrCFuncResult nested_continue(XrayIsolate *X, int status, void *ctx, XrValue *result) {
    (void)status;
    NestedState *state = (NestedState *)ctx;
    return nested_step(X, state, result);
}

/*
 * test_yield.nested() -> int
 * Simulates nested yield: outer yield -> inner yield -> returns 150
 */
static XrCFuncResult test_yield_nested(XrayIsolate *X, XrValue *args, int argc, XrValue *result) {
    (void)args;
    (void)argc;

    NestedState *state = (NestedState *)xr_malloc(sizeof(NestedState));
    if (!state) { *result = xr_null(); return XR_CFUNC_ERROR; }
    state->phase = 0;
    state->outer_val = 0;
    state->inner_val = 0;

    // Start first yield
    return xr_yield(X, nested_continue, state);
}

/* ========================================================================== */
// Scenario 10: Long running task simulation
/* ========================================================================== */

typedef struct {
    int iterations;
    int current;
    int64_t result;
} LongTaskState;

static XrCFuncResult long_task_continue(XrayIsolate *X, int status, void *ctx, XrValue *result);

static XrCFuncResult long_task_step(XrayIsolate *X, LongTaskState *state, XrValue *result) {
    if (state->current >= state->iterations) {
        int64_t final_result = state->result;
        xr_free(state);
        *result = xr_int(final_result);
        return XR_CFUNC_DONE;
    }

    // Simulate computation
    state->result += state->current * state->current;
    state->current++;

    // Yield at each iteration, simulating cooperative scheduling for long tasks
    return xr_yield(X, long_task_continue, state);
}

static XrCFuncResult long_task_continue(XrayIsolate *X, int status, void *ctx, XrValue *result) {
    (void)status;
    LongTaskState *state = (LongTaskState *)ctx;
    return long_task_step(X, state, result);
}

/*
 * test_yield.long_task(n) -> int
 * Simulates long running task: computes 0^2 + 1^2 + ... + (n-1)^2, yielding each iteration
 */
static XrCFuncResult test_yield_long_task(XrayIsolate *X, XrValue *args, int argc, XrValue *result) {
    int iterations = (argc > 0 && XR_IS_INT(args[0])) ? (int)XR_TO_INT(args[0]) : 10;
    if (iterations < 0) iterations = 0;
    if (iterations > 10000) iterations = 10000;

    if (iterations == 0) {
        *result = xr_int(0);
        return XR_CFUNC_DONE;
    }

    LongTaskState *state = (LongTaskState *)xr_malloc(sizeof(LongTaskState));
    if (!state) { *result = xr_null(); return XR_CFUNC_ERROR; }
    state->iterations = iterations;
    state->current = 0;
    state->result = 0;

    return long_task_step(X, state, result);
}

/* ========================================================================== */
// Module registration
/* ========================================================================== */

XrModule* xr_load_module_test_yield(XrayIsolate *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_test_yield: NULL isolate");

    XrModule *mod = xr_module_create_native(isolate, "test_yield");
    if (!mod) return NULL;

    // Basic tests
    XRS_EXPORT_YIELDABLE(mod, isolate, "simple", test_yield_simple);
    XRS_EXPORT_YIELDABLE(mod, isolate, "add", test_yield_add);
    XRS_EXPORT(mod, isolate, "sync", test_yield_sync);

    // Complex scenarios
    XRS_EXPORT_YIELDABLE(mod, isolate, "multi_yield", test_yield_multi);
    XRS_EXPORT_YIELDABLE(mod, isolate, "chain",       test_yield_chain);
    XRS_EXPORT_YIELDABLE(mod, isolate, "error_test",  test_yield_error);
    XRS_EXPORT_YIELDABLE(mod, isolate, "cancel_test", test_yield_cancel);
    XRS_EXPORT_YIELDABLE(mod, isolate, "nested",      test_yield_nested);
    XRS_EXPORT_YIELDABLE(mod, isolate, "long_task",   test_yield_long_task);

    // Concurrent counter
    XRS_EXPORT_YIELDABLE(mod, isolate, "counter_inc", test_yield_counter_inc);
    XRS_EXPORT(mod, isolate, "counter_get",   test_yield_counter_get);
    XRS_EXPORT(mod, isolate, "counter_reset", test_yield_counter_reset);

    mod->loaded = true;
    return mod;
}
