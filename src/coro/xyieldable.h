/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xyieldable.h - C function yieldable protocol definition
 *
 * KEY CONCEPT:
 *   Defines C function Yieldable protocol, allowing C blocking functions
 *   to yield execution to coroutine scheduler for true coroutine-style I/O.
 *
 * CORE IDEAS:
 *   1. C function saves state and returns XR_CFUNC_BLOCKED on block
 *   2. Scheduler suspends coroutine, executes others
 *   3. On I/O ready, scheduler resumes coroutine via continuation
 */

#ifndef XYIELDABLE_H
#define XYIELDABLE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
// POLLIN/POLLOUT come from poll.h on POSIX and winsock2.h on
// Windows; os_net.h pulls in the right header per OS.
#include "../os/os_net.h"

// XrValue type (needed for continuation return value)
#include "../runtime/value/xvalue.h"

// Forward declarations
struct XrayIsolate;
struct XrCoroutine;

// ========== C Function Execution Result ==========

// XrCFuncResult - C function execution result
//
// Yieldable C functions return this enum to tell VM how to proceed:
//   - DONE: function complete, result pushed to stack
//   - YIELD: voluntarily yield, continue from continuation next time
//   - BLOCKED: waiting for I/O, needs netpoll to wake
//   - ERROR: error occurred, exception set
#ifndef XR_CFUNC_RESULT_DEFINED
typedef enum {
    XR_CFUNC_DONE = 0,      // Complete, result pushed
    XR_CFUNC_YIELD,         // Voluntarily yield, continue next time
    XR_CFUNC_BLOCKED,       // Blocked waiting for I/O
    XR_CFUNC_ERROR,         // Error
    XR_CFUNC_CALL_CLOSURE,  // Closure frame pushed, execute it
    XR_CFUNC_WOULD_BLOCK    // JIT try-mode: would block, no side effects
} XrCFuncResult;
#define XR_CFUNC_RESULT_DEFINED
#endif

// ========== Resume Status Enum ==========

// XrResumeStatus - Resume status
//
// Passed to continuation function to indicate why it was resumed
typedef enum XrResumeStatus {
    XR_RESUME_OK = 0,          // Normal resume
    XR_RESUME_IO_READY,        // I/O ready
    XR_RESUME_TIMEOUT,         // Timeout
    XR_RESUME_CANCELLED,       // Cancelled
    XR_RESUME_CHANNEL,         // Channel operation complete (value ready)
    XR_RESUME_CHANNEL_CLOSED,  // Channel closed (need to recheck buffer)
    XR_RESUME_ERROR,           // Error
    XR_RESUME_DEBUG,           // Debug break resume (skip unroll)
    XR_RESUME_CONTINUATION,    // Continuation stealing resume (vm_ctx already set, skip unroll)
    XR_RESUME_CLOSURE_DONE,    // Closure called via xr_yield_call_closure returned normally
    XR_RESUME_CLOSURE_ERROR    // Closure called via xr_yield_call_closure threw exception
} XrResumeStatus;

// ========== Continuation Function Type ==========

// XrContinuation - Continuation function
//
// Called by scheduler when coroutine resumes from blocked state.
// Continuation can return DONE (complete) or BLOCKED (continue waiting).
//
// Parameters:
//   X: Isolate pointer
//   status: resume status (XrResumeStatus)
//   ctx: user context data (points to object in coroutine heap)
//   result: return value output parameter (set when DONE)
//
// Returns:
//   XrCFuncResult indicating next action
//
// Design note:
//   Added result parameter for clearer return value passing, unroll mechanism handles storage.
//   Continuation only needs to focus on business logic, no manual frame manipulation.
typedef XrCFuncResult (*XrContinuation)(struct XrayIsolate *X, int status, void *ctx,
                                        XrValue *result);

// ========== Blocking Context ==========

// XrYieldContext - Blocking context
//
// Saves C function state when blocked, used for resume execution.
typedef struct XrYieldContext {
    // User data
    void *user_data;        // User state data pointer
    size_t user_data_size;  // User data size (for memory management)
    bool user_data_owned;   // Whether memory is managed by coroutine

    // Continuation function
    XrContinuation cont;  // Continuation function

    // I/O wait conditions
    int wait_fd;      // fd to wait on (-1 means none)
    int wait_events;  // POLLIN/POLLOUT

    // Timeout
    int64_t timeout_ms;  // Timeout (milliseconds, -1 means forever)
    int64_t deadline;    // Absolute deadline (microseconds)

    // Result
    int result_events;  // Actually triggered events
    bool timed_out;     // Whether timed out

    // Linked list (for nested blocking)
    struct XrYieldContext *next;
} XrYieldContext;

// ========== Wait Event Constants ==========

#define XR_WAIT_READ POLLIN              // Wait for readable
#define XR_WAIT_WRITE POLLOUT            // Wait for writable
#define XR_WAIT_BOTH (POLLIN | POLLOUT)  // Wait for both

// ========== API Function Declarations ==========

// xr_yield_for_io - Wait for I/O event and yield (core function)
//
// Unified handling for all blocking wait scenarios:
//   - fd >= 0, timeout_ms < 0:  pure I/O wait
//   - fd < 0, timeout_ms >= 0:  pure timeout wait
//   - fd >= 0, timeout_ms >= 0: I/O + timeout
//
// Parameters:
//   X: Isolate pointer
//   fd: file descriptor (-1 means no I/O wait)
//   events: events to wait for (XR_WAIT_READ/XR_WAIT_WRITE)
//   timeout_ms: timeout (milliseconds, -1 means no timeout)
//   cont: continuation function
//   user_data: user data
//
// Returns: XR_CFUNC_BLOCKED
XR_FUNC XrCFuncResult xr_yield_for_io(struct XrayIsolate *X, int fd, int events, int64_t timeout_ms,
                                      XrContinuation cont, void *user_data, XrValue *result);

// xr_yield_for_timeout - Wait for timeout and yield (convenience function)
//
// Equivalent to xr_yield_for_io(X, -1, 0, timeout_ms, cont, user_data)
//
// Parameters:
//   X: Isolate pointer
//   timeout_ms: timeout (milliseconds)
//   cont: continuation function
//   user_data: user data
//
// Returns: XR_CFUNC_BLOCKED
XR_FUNC XrCFuncResult xr_yield_for_timeout(struct XrayIsolate *X, int64_t timeout_ms,
                                           XrContinuation cont, void *user_data, XrValue *result);

// xr_yield - Voluntarily yield (no wait condition)
//
// C function voluntarily yields execution, continuation called on next schedule.
// Used for cooperative multitasking.
//
// Parameters:
//   X: Isolate pointer
//   cont: continuation function
//   user_data: user data
//
// Returns: XR_CFUNC_YIELD
XR_FUNC XrCFuncResult xr_yield(struct XrayIsolate *X, XrContinuation cont, void *user_data);

// ========== Coroutine Helper Functions ==========

// xr_coro_has_continuation - Check if coroutine has pending continuation
XR_FUNC bool xr_coro_has_continuation(struct XrCoroutine *coro);

// ========== State Machine Standard Protocol ==========

// State Machine Protocol
//
// All C functions needing multiple blocks should follow this protocol:
//
// 1. Define state enum (XR_SM_STATE_xxx)
// 2. Define state struct (with XrStateMachine as first field)
// 3. Implement state functions (one function per state)
// 4. Use XR_SM_RUN macro to drive state machine
//
// Example:
//   typedef struct {
//       XrStateMachine sm;      // Must be first field
//       int listen_fd;          // User data
//       XrPollDesc *pd;
//   } HttpListenerState;
//
//   static XrCFuncResult state_accept(XrayIsolate *X, void *ctx);
//   static XrCFuncResult state_process(XrayIsolate *X, void *ctx);
//
//   static XrStateFunc http_listener_states[] = {
//       [0] = state_accept,
//       [1] = state_process,
//   };

// XrStateFunc - State function type
//
// Each state corresponds to a function, returns:
//   - XR_CFUNC_DONE: state machine complete
//   - XR_CFUNC_BLOCKED: current state blocked, retry after wake
//   - XR_CFUNC_YIELD: transition to next state
//   - XR_CFUNC_ERROR: error
typedef XrCFuncResult (*XrStateFunc)(struct XrayIsolate *X, void *state);

// XrStateMachine - State machine base structure
//
// First field of all state machine state structs must be this structure.
// This allows accessing state machine control info via pointer cast.
typedef struct XrStateMachine {
    int current_state;    // Current state index
    int state_count;      // Total state count
    XrStateFunc *states;  // State function array
    bool done;            // Whether state machine is complete
    int error_code;       // Error code (0 means no error)
} XrStateMachine;

// xr_sm_init - Initialize state machine
//
// Parameters:
//   sm: state machine struct pointer
//   states: state function array
//   state_count: state count
static inline void xr_sm_init(XrStateMachine *sm, XrStateFunc *states, int state_count) {
    sm->current_state = 0;
    sm->state_count = state_count;
    sm->states = states;
    sm->done = false;
    sm->error_code = 0;
}

// xr_sm_goto - Jump to specified state
//
// For state transition (not immediate, executed on next run)
static inline void xr_sm_goto(XrStateMachine *sm, int state) {
    if (state >= 0 && state < sm->state_count) {
        sm->current_state = state;
    }
}

// xr_sm_next - Transition to next state
static inline void xr_sm_next(XrStateMachine *sm) {
    if (sm->current_state < sm->state_count - 1) {
        sm->current_state++;
    }
}

// xr_sm_done - Mark state machine as complete
static inline void xr_sm_done(XrStateMachine *sm) {
    sm->done = true;
}

// xr_sm_error - Mark state machine as error
static inline void xr_sm_error(XrStateMachine *sm, int error_code) {
    sm->done = true;
    sm->error_code = error_code;
}

// xr_sm_run - Run state machine
//
// Execute current state function, decide next step based on return:
//   - DONE: state machine complete
//   - BLOCKED: wait for I/O, keep current state
//   - YIELD: execute next state (continue loop immediately)
//   - ERROR: state machine error
//
// Parameters:
//   X: Isolate pointer
//   state: state struct pointer (first field is XrStateMachine)
//
// Returns:
//   Final execution result
static inline XrCFuncResult xr_sm_run(struct XrayIsolate *X, void *state) {
    XrStateMachine *sm = (XrStateMachine *) state;

    while (!sm->done && sm->current_state < sm->state_count) {
        XrStateFunc func = sm->states[sm->current_state];
        if (!func) {
            sm->error_code = -1;
            return XR_CFUNC_ERROR;
        }

        XrCFuncResult result = func(X, state);

        switch (result) {
            case XR_CFUNC_DONE:
                sm->done = true;
                return XR_CFUNC_DONE;

            case XR_CFUNC_BLOCKED:
                // Keep current state, wait for wake
                return XR_CFUNC_BLOCKED;

            case XR_CFUNC_YIELD:
                // Continue to next state (loop)
                break;

            case XR_CFUNC_CALL_CLOSURE:
                // State function called xr_yield_call_closure
                return XR_CFUNC_CALL_CLOSURE;

            case XR_CFUNC_ERROR:
                sm->done = true;
                return XR_CFUNC_ERROR;
        }
    }

    return sm->error_code ? XR_CFUNC_ERROR : XR_CFUNC_DONE;
}

// xr_sm_continuation - State machine continuation (new signature: added result param)
//
// Generic continuation function, for resuming state machine execution from blocked state.
// Can be passed directly to xr_yield_for_io etc.
static inline XrCFuncResult xr_sm_continuation(struct XrayIsolate *X, int status, void *state,
                                               XrValue *result) {
    // State machine can get status via xr_get_resume_status
    (void) status;  // Ignore status for now, state machine gets from X
    (void) result;  // State machine doesn't use return value
    return xr_sm_run(X, state);
}

// ========== State Machine Helper Macros ==========

// XR_SM_DEFINE_STATES - Define state function array
//
// Usage:
//   XR_SM_DEFINE_STATES(http_listener,
//       state_accept,
//       state_process,
//       state_cleanup
//   );
#define XR_SM_DEFINE_STATES(name, ...)                                                             \
    static XrStateFunc name##_states[] = {__VA_ARGS__};                                            \
    static const int name##_state_count = sizeof(name##_states) / sizeof(XrStateFunc)

// XR_SM_INIT - Initialize state machine (using macro-defined state array)
//
// Usage:
//   XR_SM_INIT(&state->sm, http_listener);
#define XR_SM_INIT(sm, name) xr_sm_init(sm, name##_states, name##_state_count)

// XR_SM_YIELD_FOR_IO - Block waiting for I/O in state machine
//
// Usage:
//   return XR_SM_YIELD_FOR_IO(X, fd, XR_WAIT_READ, state);
#define XR_SM_YIELD_FOR_IO(X, fd, events, state, result)                                           \
    xr_yield_for_io(X, fd, events, -1, xr_sm_continuation, state, result)

// XR_SM_YIELD_FOR_TIMEOUT - Block waiting for timeout in state machine
//
// Usage:
//   return XR_SM_YIELD_FOR_TIMEOUT(X, 1000, state);
#define XR_SM_YIELD_FOR_TIMEOUT(X, timeout_ms, state, result)                                      \
    xr_yield_for_timeout(X, timeout_ms, xr_sm_continuation, state, result)

// ========== Closure Call from C Layer ==========

// xr_yield_call_closure - Call a user closure from C yieldable function
//
// The closure may yield (channel/await/sleep). When the closure finally
// returns, on_complete is called with XR_RESUME_CLOSURE_DONE.
// The closure return value is accessible via xr_get_closure_result(X).
//
// Must be called from a yieldable C function or continuation.
// Always returns XR_CFUNC_CALL_CLOSURE.
XR_FUNC XrCFuncResult xr_yield_call_closure(struct XrayIsolate *X, struct XrClosure *closure,
                                            XrValue *args, int nargs, XrContinuation on_complete,
                                            void *user_ctx, XrValue *result);

// xr_get_closure_result - Retrieve closure return value inside on_complete
//
// Call this in the on_complete continuation to get the value returned
// by the closure that was called via xr_yield_call_closure.
XR_FUNC XrValue xr_get_closure_result(struct XrayIsolate *X);

#endif  // XYIELDABLE_H
