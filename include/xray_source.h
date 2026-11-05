/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_source.h - Owned source compilation and bounded entry execution
 */
#ifndef XRAY_SOURCE_H
#define XRAY_SOURCE_H
#include "xray_export.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define XRAY_SOURCE_API_VERSION 3u

typedef struct XrSourceArtifact XrSourceArtifact;
typedef struct XrSourceExecution XrSourceExecution;
typedef struct XrSourceDiagnostic XrSourceDiagnostic;
typedef struct XrSourceCall XrSourceCall;
typedef struct XrSourceResult XrSourceResult;
typedef enum XrSourceStatus {
    XR_SOURCE_OK, XR_SOURCE_BAD_ARGUMENT, XR_SOURCE_NOT_FOUND, XR_SOURCE_INVALID,
    XR_SOURCE_LIMIT, XR_SOURCE_MEMORY_LIMIT, XR_SOURCE_OUT_OF_MEMORY, XR_SOURCE_IO,
    XR_SOURCE_UNSUPPORTED, XR_SOURCE_REJECTED, XR_SOURCE_BAD_STATE, XR_SOURCE_BUSY,
    XR_SOURCE_EXECUTION_FAILED, XR_SOURCE_OVERFLOW, XR_SOURCE_CANCELLED,
    XR_SOURCE_DEADLINE, XR_SOURCE_CLOCK_FAILURE, XR_SOURCE_COMPLEXITY
} XrSourceStatus;
typedef enum XrSourceTarget { XR_SOURCE_TARGET_VM, XR_SOURCE_TARGET_C } XrSourceTarget;
typedef enum XrSourceMemoryMode { XR_SOURCE_MEMORY_ORDINARY, XR_SOURCE_MEMORY_FINITE } XrSourceMemoryMode;
typedef enum XrSourceDeadlineMode { XR_SOURCE_DEADLINE_NONE, XR_SOURCE_DEADLINE_AFTER } XrSourceDeadlineMode;
typedef int (*XrSourceCancelPoll)(const void *context);
typedef struct XrSourceCompileOptions {
    XrSourceTarget target;
    XrSourceMemoryMode memory_mode;
    uint64_t memory_limit;
    int profile_allocations;
    const char *stdlib_path;
    XrSourceCancelPoll cancel_poll;
    const void *cancel_context;
    XrSourceDeadlineMode deadline_mode;
    uint64_t timeout_ns;
} XrSourceCompileOptions;
typedef struct XrSourceText {
    const char *absolute_path; /* NULL requires an explicit MEMORY namespace. */
    const char *namespace_id; /* Rooted text requires NULL. */
    const char *bytes;
    size_t length;
} XrSourceText;
typedef enum XrSourceInputKind { XR_SOURCE_INPUT_FILE = 1, XR_SOURCE_INPUT_TEXT = 2 } XrSourceInputKind;
typedef struct XrSourceInput {
    XrSourceInputKind kind;
    union { const char *absolute_path; XrSourceText text; } value;
} XrSourceInput;
typedef enum XrSourceStage {
    XR_SOURCE_STAGE_OPTIONS, XR_SOURCE_STAGE_INPUT, XR_SOURCE_STAGE_COMPILE,
    XR_SOURCE_STAGE_PREPARE, XR_SOURCE_STAGE_PUBLISH
} XrSourceStage;
/* Always available by value, including when diagnostic allocation fails.
 * Primary status wins over cleanup_status; OK cleanup means no cleanup failure
 * was observed, not that the failed request succeeded. */
typedef struct XrSourceFailure {
    XrSourceStatus status, cleanup_status;
    XrSourceStage stage;
    int32_t line, column;
} XrSourceFailure;
typedef struct XrSourceArtifactInfo {
    XrSourceTarget target;
    uint32_t entry, functions, modules, named_entries;
} XrSourceArtifactInfo;
typedef struct XrSourceDiagnosticInfo {
    XrSourceStatus status;
    uint32_t stage;
    int32_t line, column;
    const char *path, *message; /* Borrows end at diagnostic_free. */
} XrSourceDiagnosticInfo;

typedef enum XrSourceRunState {
    XR_SOURCE_RUN_READY, XR_SOURCE_RUN_SUSPENDED, XR_SOURCE_RUN_RETURNED,
    XR_SOURCE_RUN_THROWN, XR_SOURCE_RUN_PANICKED, XR_SOURCE_RUN_CANCELLED,
    XR_SOURCE_RUN_LIMIT, XR_SOURCE_RUN_OUT_OF_MEMORY, XR_SOURCE_RUN_HOST_ERROR,
    XR_SOURCE_RUN_OUTPUT_ERROR, XR_SOURCE_RUN_UNSUPPORTED, XR_SOURCE_RUN_API_ERROR, XR_SOURCE_RUN_CLEANUP_FAILED
} XrSourceRunState;
typedef struct XrSourceEvent {
    XrSourceRunState state;
    uint32_t engine_status; /* Exact diagnostic status, not a language panic code. */
    uint64_t epoch, wake;
} XrSourceEvent;
typedef enum XrSourceOutputStream { XR_SOURCE_STDOUT=1, XR_SOURCE_STDERR=2 } XrSourceOutputStream;
typedef enum XrSourceWriteStatus { XR_SOURCE_WRITE_OK, XR_SOURCE_WRITE_OOM, XR_SOURCE_WRITE_LIMIT, XR_SOURCE_WRITE_IO } XrSourceWriteStatus;
typedef XrSourceWriteStatus (*XrSourceWrite)(void *, XrSourceOutputStream, const char *, size_t);
/* This identity belongs only to the unchanged runtime-call option shape. */
#define XRAY_SOURCE_EXECUTION_ABI_VERSION 1u
typedef struct XrSourceExecutionOptions {
    XrSourceMemoryMode memory_mode;
    uint64_t memory_limit;
} XrSourceExecutionOptions;
typedef struct XrSourceCallOptions {
    uint32_t api_version, struct_size;
    /* Runtime limits include the live Call/HostCall/Instance envelope. Call and result allocations belong to the execution owner. */
    /* work_limit is the current interpreter execution count. It is unrelated
     * to removed compile work; ordinary/finite fuel replacement belongs to the
     * execution backend, not the compilation request. */
    uint64_t metadata_limit, value_limit, call_limit, poll_limit, work_limit;
    uint32_t depth_limit;
    size_t output_byte_limit;
    XrSourceWrite write; /* NULL uses the host's stdio mode; callbacks receive exact bytes. */
    void *context; /* Host-owned until call_drop or execute returns. */
} XrSourceCallOptions;
typedef enum XrSourceWaitKind { XR_SOURCE_WAIT_YIELD, XR_SOURCE_WAIT_TIMER, XR_SOURCE_WAIT_TASK } XrSourceWaitKind;
typedef struct XrSourceWait {
    XrSourceWaitKind kind;
    uint64_t after_ms;
    /* Borrowed observation, not a Task handle or execution authority. Never
     * dereference or adopt subject. It expires on the next successful change
     * to this Call, including a poll, resume, cancellation or drop. */
    const void *subject;
    uint64_t generation, ticket;
} XrSourceWait;
/* Configuration is reusable; build owns one short-lived request. Input bytes,
 * options and cancellation context are borrowed only until build returns.
 * Ordinary requires memory_limit == 0; finite zero grants no memory. AFTER
 * creates one monotonic deadline; zero is already expired at the first poll.
 * Cancel callbacks run synchronously at safe points, outside allocator locks,
 * and must not reenter this request. The caller excludes concurrent mutation.
 *
 * Success publishes only a ready VM executable or owned generated C text.
 * No request, parser, Catalog or cancellation state is kept by the artifact.
 * Output must be NULL; failure preserves it and fills failure without allocating.
 * An optional diagnostic owns a bounded detail snapshot independently. */
XRAY_API XrSourceCompileOptions xray_source_compile_options_default(void);
XRAY_API XrSourceStatus xray_source_build(const XrSourceInput *, const XrSourceCompileOptions *,
    XrSourceArtifact **, XrSourceFailure *, XrSourceDiagnostic **);
XRAY_API XrSourceStatus xray_source_artifact_info(const XrSourceArtifact *, XrSourceArtifactInfo *);
XRAY_API XrSourceStatus xray_source_artifact_c_text(const XrSourceArtifact *, const char **, size_t *);
XRAY_API void xray_source_artifact_free(XrSourceArtifact *);
XRAY_API XrSourceStatus xray_source_diagnostic_info(const XrSourceDiagnostic *, XrSourceDiagnosticInfo *);
XRAY_API void xray_source_diagnostic_free(XrSourceDiagnostic *);
/* The execution owner is independent of compilation. Calls and result/value
 * blocks retain their required owner and may outlive this handle. FINITE is
 * currently UNSUPPORTED until the backend can cover all execution allocations. */
XRAY_API XrSourceExecutionOptions xray_source_execution_options_default(void);
XRAY_API XrSourceStatus xray_source_execution_new(const XrSourceExecutionOptions *, XrSourceExecution **);
XRAY_API void xray_source_execution_free(XrSourceExecution *);
XRAY_API XrSourceStatus xray_source_call_options_init(XrSourceCallOptions *, size_t);
/* Begin executes the ordinary admitted root entry. Polls borrow no result
 * payload; take publishes an owned terminal result exactly once. Reentrant
 * state-changing calls return BUSY. Cancellation only queues cleanup; honor
 * any valid waits while continuing bounded polls to the terminal outcome.
 * Artifact and Execution handles may die after begin succeeds. */
XRAY_API XrSourceStatus xray_source_artifact_begin(XrSourceExecution *, XrSourceArtifact *, const XrSourceCallOptions *, XrSourceCall **);
XRAY_API XrSourceStatus xray_source_call_poll(XrSourceCall *, uint64_t quantum, XrSourceEvent *);
XRAY_API XrSourceStatus xray_source_call_resume(XrSourceCall *, uint64_t epoch, uint64_t wake);
/* Size must match this header; any refusal preserves every output field. */
XRAY_API XrSourceStatus xray_source_call_wait_request(const XrSourceCall *, uint64_t epoch, uint64_t wake, XrSourceWait *, size_t);
XRAY_API XrSourceStatus xray_source_call_cancel(XrSourceCall *);
XRAY_API XrSourceStatus xray_source_call_take(XrSourceCall *, XrSourceResult **);
/* Consumes and clears call except on BUSY. Cleanup failure is reported in the
 * event and does not revoke a result that the caller already owns. */
XRAY_API XrSourceStatus xray_source_call_drop(XrSourceCall **, XrSourceEvent *cleanup);
/* Uses the real host clock for timers and drains host failures before return. */
XRAY_API XrSourceStatus xray_source_artifact_execute(XrSourceExecution *, XrSourceArtifact *, const XrSourceCallOptions *, XrSourceResult **, XrSourceEvent *failure);
XRAY_API XrSourceStatus xray_source_result_event(const XrSourceResult *, XrSourceEvent *);
XRAY_API XrSourceStatus xray_source_result_i64(const XrSourceResult *, int64_t *);
/* Reads owned STRING outcome or assertion message bytes, including NUL. */
XRAY_API XrSourceStatus xray_source_result_bytes(const XrSourceResult *, const char **, size_t *);
/* Uses canonical Error/Panic and returned-value display. Policy outcomes are
 * inspected through result_event and return UNSUPPORTED here. The callback is
 * synchronous; the result must remain alive until formatting returns. */
XRAY_API XrSourceStatus xray_source_result_format(const XrSourceResult *, XrSourceWrite, void *context);
XRAY_API void xray_source_result_free(XrSourceResult *);
#ifdef __cplusplus
}
#endif
#endif // XRAY_SOURCE_H
