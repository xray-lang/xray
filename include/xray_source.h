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
#define XRAY_SOURCE_API_VERSION 2u
typedef struct XrSourceCompiler XrSourceCompiler;
typedef struct XrSourceProgram XrSourceProgram;
typedef struct XrSourceDiagnostic XrSourceDiagnostic;
typedef struct XrSourceCall XrSourceCall;
typedef struct XrSourceResult XrSourceResult;
typedef enum XrSourceStatus {
    XR_SOURCE_OK, XR_SOURCE_BAD_ARGUMENT, XR_SOURCE_NOT_FOUND, XR_SOURCE_INVALID,
    XR_SOURCE_LIMIT, XR_SOURCE_BUDGET, XR_SOURCE_OUT_OF_MEMORY, XR_SOURCE_IO,
    XR_SOURCE_UNSUPPORTED, XR_SOURCE_REJECTED, XR_SOURCE_BAD_STATE, XR_SOURCE_BUSY, XR_SOURCE_EXECUTION_FAILED
} XrSourceStatus;
typedef struct XrSourceCompilerOptions {
    uint32_t api_version, struct_size;
    uint32_t wait_size; /* Exact sizeof(XrSourceWait) from this header. */
    uint64_t allocated_bytes, live_bytes, work;
    const char *stdlib_path; /* NULL selects environment/executable/cwd precedence. */
} XrSourceCompilerOptions;
typedef struct XrSourceText {
    const char *absolute_path; /* NULL requires an explicit MEMORY namespace. */
    const char *namespace_id; /* Rooted text requires NULL. */
    const char *bytes;
    size_t length;
} XrSourceText;
typedef struct XrSourceProgramInfo { uint32_t entry, functions, modules; } XrSourceProgramInfo;
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
typedef struct XrSourceCallOptions {
    uint32_t api_version, struct_size;
    /* Runtime limits include the live Call/HostCall/Instance envelope. Opaque
     * Program/diagnostic/result blocks use the compiler's original ledger. */
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
/* Initialize options before construction. One compiler owns one cumulative
 * ledger; all compiler/Program operations require caller exclusion. Products,
 * diagnostics and execution results survive their producer's destruction.
 * Output owner pointers must be NULL and are preserved on failure. */
XRAY_API XrSourceStatus xray_source_compiler_options_init(XrSourceCompilerOptions *, size_t);
XRAY_API XrSourceStatus xray_source_compiler_new(const XrSourceCompilerOptions *, XrSourceCompiler **);
XRAY_API void xray_source_compiler_free(XrSourceCompiler *);
XRAY_API XrSourceStatus xray_source_compile_file(XrSourceCompiler *, const char *absolute_path, XrSourceProgram **, XrSourceDiagnostic **);
XRAY_API XrSourceStatus xray_source_compile_text(XrSourceCompiler *, const XrSourceText *, XrSourceProgram **, XrSourceDiagnostic **);
XRAY_API XrSourceStatus xray_source_program_info(const XrSourceProgram *, XrSourceProgramInfo *);
XRAY_API void xray_source_program_free(XrSourceProgram *);
XRAY_API XrSourceStatus xray_source_diagnostic_info(const XrSourceDiagnostic *, XrSourceDiagnosticInfo *);
XRAY_API void xray_source_diagnostic_free(XrSourceDiagnostic *);
XRAY_API XrSourceStatus xray_source_call_options_init(XrSourceCallOptions *, size_t);
/* Begin executes the ordinary admitted root entry. Polls borrow no result
 * payload; take publishes an owned terminal result exactly once. Reentrant
 * state-changing calls return BUSY. Cancellation only queues cleanup; honor
 * any valid waits while continuing bounded polls to the terminal outcome.
 * Program may die after begin succeeds. */
XRAY_API XrSourceStatus xray_source_call_begin(XrSourceProgram *, const XrSourceCallOptions *, XrSourceCall **);
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
XRAY_API XrSourceStatus xray_source_execute(XrSourceProgram *, const XrSourceCallOptions *, XrSourceResult **, XrSourceEvent *failure);
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
