/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call.h - Typed resumable frames and trampoline boundary
 *
 * KEY CONCEPT:
 *   Calls and suspensions leave the native stack before another entry runs.
 */
#ifndef XXIR_CALL_H
#define XXIR_CALL_H
#include "xxir_scalar.h"
#include "xxir_panic.h"
#include "xxir_output_status.h"

#define XR_XIR_CALL_ABI_VERSION 24u
#define XR_XIR_CALL_STATE_ALIGNMENT 16u
typedef struct XrXirCall XrXirCall;
typedef enum XrXirCallStatus {
    XR_XIR_CALL_READY, XR_XIR_CALL_RETURNED, XR_XIR_CALL_THROWN,
    XR_XIR_CALL_SUSPENDED, XR_XIR_CALL_CANCELLED, XR_XIR_CALL_LIMIT,
    XR_XIR_CALL_OOM, XR_XIR_CALL_BAD_ARGUMENT, XR_XIR_CALL_BAD_ABI,
    XR_XIR_CALL_BAD_STATE, XR_XIR_CALL_BUSY, XR_XIR_CALL_DIVIDE_BY_ZERO,
    XR_XIR_CALL_CONSUMED, XR_XIR_CALL_OUTPUT_ERROR, XR_XIR_CALL_NUMERIC_RANGE,
    XR_XIR_CALL_BOUNDS, XR_XIR_CALL_MATCH_FAILURE, XR_XIR_CALL_DEFER_ASYNC,
    XR_XIR_CALL_CANCEL_REQUESTED, XR_XIR_CALL_ASSERTION = 19,
    /* A host service failed. It is never a language panic and no handler observes it. */
    XR_XIR_CALL_HOST_ERROR,
    /* A language panic whose detail code alone names it, such as unwrapping null. */
    XR_XIR_CALL_RUNTIME_PANIC
} XrXirCallStatus;
typedef struct XrXirCallResult {
    XrXirCallStatus status;
    XrXirValue value;
    uint64_t wake;
    XrXirPanicPayload panic;
} XrXirCallResult;
typedef enum XrXirActionKind {
    XR_XIR_ACTION_CALL = 1, XR_XIR_ACTION_RETURN, XR_XIR_ACTION_THROW,
    XR_XIR_ACTION_SUSPEND, XR_XIR_ACTION_CONTINUE, XR_XIR_ACTION_FAULT,
    XR_XIR_ACTION_OUTPUT, XR_XIR_ACTION_WRITE_STREAM, XR_XIR_ACTION_LEAVE,
    XR_XIR_ACTION_EXIT_DONE,
    /* value is the nonzero I64 millisecond duration the activation waits for. */
    XR_XIR_ACTION_TIMER
} XrXirActionKind;
/* A PROTECTED call delivers a panic of its callee subtree back to the caller. */
#define XR_XIR_ACTION_PROTECTED 1u
#define XR_XIR_ACTION_CLEANUP 2u
#define XR_XIR_ACTION_LEAVE_ERROR 4u
#define XR_XIR_ACTION_LEAVE_PANIC 8u
#define XR_XIR_ENTRY_EXIT 1u
typedef enum XrXirCallPhase { XR_XIR_CALL_NORMAL, XR_XIR_CALL_EXIT } XrXirCallPhase;
typedef struct XrXirAction {
    XrXirActionKind kind;
    uint32_t callee;
    const XrXirValue *arguments;
    uint32_t argument_count;
    XrXirValue value;
    XrXirPanicPayload panic;
    uint32_t flags;
} XrXirAction;

/* Normalize failures inside an admitted entry; admission errors are separate. */
XR_FUNC XrXirAction xr_xir_call_fault(XrXirRunStatus status);
/* Integer arithmetic faults; remainder by zero keeps its own panic code. */
XR_FUNC XrXirAction xr_xir_call_numeric_fault(XrXirRunStatus status, bool remainder);
XR_FUNC XrXirAction xr_xir_call_bounds(int64_t index, int64_t length);
XR_FUNC XrXirAction xr_xir_call_match_failure(void);
/* The returned action borrows message until the current resume returns. */
XR_FUNC XrXirAction xr_xir_call_assertion(const XrXirValue *message);
/* Panic-channel statuses are the only faults a protected region may observe. */
XR_FUNC bool xr_xir_call_panic_status(XrXirCallStatus status);
XR_FUNC bool xr_xir_call_panic_payload(XrXirCallStatus status, const XrXirPanicPayload *panic);
XR_FUNC bool xr_xir_call_panic_action(const XrXirAction *action);
/* Poll and view results borrow. Explicit copies own both outcome domains. */
XR_FUNC bool xr_xir_call_result_empty(const XrXirCallResult *result);
XR_FUNC bool xr_xir_call_result_valid(const XrXirCallResult *result);
XR_FUNC XrXirValueStatus xr_xir_call_result_copy(const XrXirCallResult *source, XrXirCallResult *output);
XR_FUNC void xr_xir_call_result_move(XrXirCallResult *source, XrXirCallResult *output);
XR_FUNC void xr_xir_call_result_drop(XrXirCallResult *result);
typedef struct XrXirCallView {
    XrXirCall *activation;
    void *instance;
    const void *environment;
    void *state;
    const XrXirValue *arguments;
    uint32_t argument_count;
    XrXirCallResult inbox;
    const XrXirTypeArena *arena;
    XrXirCallPhase phase;
    /* Borrowed pending exit; scope completion restores it to this frame inbox. */
    XrXirCallResult exit;
    bool scope_exit;
} XrXirCallView;
/* Borrowed only by the driver's current resume view, until that callback returns. */
XR_FUNC XrXirValueAdmission *xr_xir_call_admission(const XrXirCallView *view);
/* Consume the driver's admitted returned owner, never the borrowed view copy. */
XR_FUNC XrXirCallStatus xr_xir_call_discard_inbox(XrXirCallView *view, XrXirType expected);
/* Internal authority exists only within an active cleanup callback or value admission. */
XR_FUNC bool xr_xir_call_cleanup_active(const XrXirCall *activation);
typedef XrXirAction (*XrXirResumeEntry)(XrXirCallView *view);
typedef void (*XrXirReleaseEntry)(XrXirCallView *view, XrXirCallStatus reason);
typedef struct XrXirCallEntry {
    uint32_t abi_version;
    const XrXirType *parameters;
    uint32_t parameter_count;
    XrXirType result;
    uint32_t state_bytes;
    XrXirResumeEntry resume;
    XrXirReleaseEntry release;
    const void *environment;
    uint32_t flags, cleanup_owner;
} XrXirCallEntry;
typedef struct XrXirCallAccounting {
    uint64_t live_bytes, peak_bytes, allocations, frees;
    uint64_t polls;
    uint32_t depth, peak_depth;
} XrXirCallAccounting;

typedef enum XrXirOutputStream { XR_XIR_STDOUT = 1, XR_XIR_STDERR = 2 } XrXirOutputStream;
/* OUTPUT action callee: stream 1/2 for raw values, 3 for one stdout line. */
#define XR_XIR_OUTPUT_LINE 3u
typedef struct XrXirOutputGroup {
    XrXirOutputStream stream;
    const XrXirValue *values;
    uint32_t count;
    bool line;
} XrXirOutputGroup;
typedef XrXirOutputStatus (*XrXirOutputEntry)(void *context, const XrXirOutputGroup *group);
typedef struct XrXirOutputProvider {
    uint32_t abi_version, reserved;
    XrXirOutputEntry write;
    void *context;
} XrXirOutputProvider;
/* Clock reads and UTC offsets are synchronous services of the instance; they
 * never suspend. An absent provider is all zero and every service then fails
 * as a host error. The provider context stays host-owned. */
typedef enum XrXirTimeStatus {
    XR_XIR_TIME_OK, XR_XIR_TIME_RANGE, XR_XIR_TIME_FAILED
} XrXirTimeStatus;
typedef XrXirTimeStatus (*XrXirClockEntry)(void *context, XrXirClockKind clock, int64_t *nanoseconds);
typedef XrXirTimeStatus (*XrXirUtcOffsetEntry)(void *context, int64_t seconds, int64_t *minutes);
typedef struct XrXirTimeProvider {
    uint32_t abi_version, reserved;
    XrXirClockEntry clock;
    XrXirUtcOffsetEntry utc_offset;
    void *context;
} XrXirTimeProvider;
static inline bool xr_xir_time_provider_valid(const XrXirTimeProvider *provider) {
    if (!provider) return false;
    if (!provider->abi_version)
        return !provider->reserved && !provider->clock && !provider->utc_offset && !provider->context;
    return provider->abi_version == XR_XIR_CALL_ABI_VERSION && !provider->reserved &&
        provider->clock && provider->utc_offset;
}
/* A suspension asks its host for nothing (YIELD) or for a duration (TIMER). The
 * request is published only for the exact pending wake token. */
typedef enum XrXirWaitKind { XR_XIR_WAIT_NONE, XR_XIR_WAIT_YIELD, XR_XIR_WAIT_TIMER_MS } XrXirWaitKind;
#define XR_XIR_TIMER_MAX_MS UINT64_C(86400000)
typedef struct XrXirWaitRequest {
    uint32_t kind, reserved;
    uint64_t after_ms;
} XrXirWaitRequest;

typedef struct XrXirCallConfig {
    uint32_t abi_version, struct_size;
    const XrXirCallEntry *entries;
    uint32_t entry_count;
    void *instance;
    uint64_t byte_limit, poll_limit;
    uint32_t depth_limit;
    XrXirCallAccounting *accounting;
    XrXirOutputProvider output;
    XrXirValueAdmission admission;
} XrXirCallConfig;

/* Failed initialization leaves the complete caller buffer unchanged. */
XR_FUNC XrXirCallStatus xr_xir_call_config_init(XrXirCallConfig *config, size_t size);
/* An absent provider has no callback, context or versioned state. */
static inline bool xr_xir_output_provider_valid(const XrXirOutputProvider *provider) {
    if (!provider) return false;
    if (!provider->abi_version)
        return !provider->reserved && !provider->write && !provider->context;
    return provider->abi_version == XR_XIR_CALL_ABI_VERSION && !provider->reserved && provider->write;
}
XR_FUNC XrXirCallStatus xr_xir_call_new(const XrXirCallConfig *config, uint32_t entry,
    const XrXirValue *arguments, uint32_t count, XrXirCall **output);
/* Each resume, completed exit or aborted frame consumes one quantum unit.
 * READY means more work remains; cumulative resume limits never reset. */
XR_FUNC XrXirCallResult xr_xir_call_poll_bounded(XrXirCall *activation, uint64_t quantum);
XR_FUNC XrXirCallStatus xr_xir_call_take_result(XrXirCall *activation, XrXirValue *output);
XR_FUNC XrXirCallStatus xr_xir_call_resume(XrXirCall *activation, uint64_t wake);
/* Fills the request of the pending suspension. Failure leaves output unchanged;
 * no clock is read and nothing is resumed. Resuming a timer is the host's decision
 * that the duration has elapsed; the driver never interprets wall time. */
XR_FUNC XrXirCallStatus xr_xir_call_wait_request(const XrXirCall *activation, uint64_t wake,
    XrXirWaitRequest *output);
/* Queues cancellation without callbacks or reclamation; poll drains cleanup. */
XR_FUNC XrXirCallStatus xr_xir_call_request_cancel(XrXirCall *activation);
/* Consumes the activation except on BUSY; reports a failed language cleanup. */
XR_FUNC XrXirCallStatus xr_xir_call_free(XrXirCall *activation);
XR_FUNC uint32_t xr_xir_call_current_entry(const XrXirCall *activation);
XR_FUNC XrXirCallStatus xr_xir_call_state(const XrXirCall *activation);
#endif // XXIR_CALL_H
