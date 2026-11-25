/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_execution.h - Synchronous termination with preowned failure storage
 *
 * KEY CONCEPT:
 *   The caller prepares its mutable execution, failure slot and location
 *   reference before guest effects. Ending execution allocates nothing.
 */
#ifndef XNATIVE_EXECUTION_H
#define XNATIVE_EXECUTION_H
#include "xnative_locations.h"
#include "xnative_host_temp.h"
#include "os/os_native_output.h"
#include "xnative_value.h"
#include "xnative_managed.h"

typedef enum XrtOutputOperation { XRT_OUTPUT_FLUSH, XRT_OUTPUT_WRITE } XrtOutputOperation;
typedef struct XrtOutputFailure {
    XrtIoResult io;
    XrtOutputOperation operation;
    uint32_t source_id;
} XrtOutputFailure;

typedef struct XrtArithmeticPanic {
    uint32_t code, source_id;
} XrtArithmeticPanic;

typedef enum XrtHostClockReason {
    XRT_CLOCK_READ_FAILED, XRT_CLOCK_OUT_OF_RANGE
} XrtHostClockReason;
typedef struct XrtHostClockFailure {
    uint32_t kind, source_id; // Kinds 1..3 read clocks; kind 4 queries a UTC offset.
    XrtHostClockReason reason;
} XrtHostClockFailure;

typedef enum XrtExecutionFailureKind {
    XRT_FAILURE_OUTPUT,
    XRT_FAILURE_TYPED_PANIC,
    XRT_FAILURE_HOST_CLOCK,
    XRT_FAILURE_VALUE,
    XRT_FAILURE_NULL_UNWRAP,
    XRT_FAILURE_MESSAGE_PANIC,
    XRT_FAILURE_TRY_ASSERT,
    XRT_FAILURE_UNCAUGHT_ERROR,
    XRT_FAILURE_TEMP
} XrtExecutionFailureKind;

typedef struct XrtTempFailure {
    XrtHostTempResult result;
    uint32_t source_id;
    XrtHostTempRetry *owner;
} XrtTempFailure;

typedef struct XrtErrorSnapshot XrtErrorSnapshot;
typedef XrtErrorSnapshot XrtTryAssertFailure;
typedef struct XrtExecutionFailure {
    /* Present for XRT_FAILURE_TEMP: the leaf that could not roll back. */
    uint32_t temp_source;
    XrtExecutionFailureKind kind;
    union {
        XrtOutputFailure output;
        XrtArithmeticPanic panic;
        XrtHostClockFailure clock;
        struct { XrXirValueStatus status; uint32_t source_id; } value;
        struct { uint32_t code, source_id; XrtManagedHeader *message; } message;
        XrtTryAssertFailure *assertion;
        XrtErrorSnapshot *error;
    } detail;
} XrtExecutionFailure;
/* A message failure owns one fixed String and a typed error owns its
 * snapshot independently of execution retirement. Clear the owner before
 * drop callbacks. A receiver must drop or transfer it before reusing failure. */
XR_FUNC void xrt_execution_failure_drop(XrtExecutionFailure *);
XR_FUNC bool xrt_temp_failure_take(XrtTempFailure *received, XrtTempFailure *empty);
/* Consume an independently owned fixed String, publish it to preowned failure
 * storage, then leave the active execution. Duplicate-key code 434 and
 * assertion code 445 are admitted. Transfer allocates and looks up no owner. */
XR_FUNC XR_NORET void xrt_message_panic_take(XrtManagedHeader **owned, uint32_t code, uint32_t source_id);

typedef enum XrtExecutionStatus {
    XRT_EXECUTION_COMPLETED,
    XRT_EXECUTION_FAILURE,
    XRT_EXECUTION_INVALID
} XrtExecutionStatus;

// All inputs are borrowed through this active synchronous C call only. The
// caller holds a location reference and owns failure/context, including the
// sole mutable VM stack. INVALID invokes no guest. COMPLETED and INVALID
// preserve every byte of failure. COMPLETED means the root ran and retired normally;
// it does not reinterpret VM statuses.
// FAILURE writes tagged captured facts only. The previous binding/target is
// restored on either return. No target crosses yield, thread handoff or FFI.
// The local wrapper cannot borrow metadata owners that could outlive the call.
XR_FUNC XrtExecutionStatus xrt_run_embedded(const XrtLocationOwner *locations,
    XrtExecutionFailure *failure, XrtSynchronousRoot root, void *context);
/* An outer owner is either empty or contains constructor-produced cold billing
 * setup only. An active ancestor's owner cannot be reused by a nested execution.
 * Both successful and failed execution drain the supplied outer owner. If a
 * close fails, the first execution failure is preserved and the supplied
 * owner remains live for close-only retry via xrt_value_root_drain. */
XR_FUNC XrtExecutionStatus xrt_run_embedded_owned(const XrtLocationOwner *locations,
    XrtExecutionFailure *failure, XrtSynchronousRoot root, void *context, XrtValueRoot *strings);
/* The host owns a stable descriptor and live context for the synchronous
 * call. The callback observes the request through that context. Neither the
 * Root nor a pending close retains the descriptor or the context. */
typedef struct XrtHostExecutionControl {
    bool (*current)(void *context);
    void *context;
} XrtHostExecutionControl;
XR_FUNC XrtExecutionStatus xrt_run_embedded_owned_controlled(const XrtLocationOwner *, XrtExecutionFailure *,
    XrtSynchronousRoot, void *context, XrtValueRoot *, const XrtHostExecutionControl *);
/* A leaf borrows only the current execution's control, when one was supplied. */
XR_FUNC const XrtHostExecutionControl *xrt_execution_host_control(void);
/* The closer is borrowed through the one drain at execution exit. */
XR_FUNC XrtExecutionStatus xrt_run_embedded_owned_with_metadata_close(const XrtLocationOwner *, XrtExecutionFailure *,
    XrtSynchronousRoot, void *context, XrtValueRoot *, XrtMetadataCloser);
/* The initialized receiver is separate from ordinary failure storage. It must
 * be empty and may not be shared with an active ancestor. A temporary effect
 * can never attach an owner to the ordinary failure, which is reusable after
 * observing the error. No installed startup entry enables this effect. */
XR_FUNC XrtExecutionStatus xrt_run_embedded_receiving_temp(const XrtLocationOwner *,
    XrtExecutionFailure *, XrtSynchronousRoot, void *context, XrtValueRoot *, XrtTempFailure *receiver);
/* The receiving binding is required before any host effect. If rollback stays
 * pending, transfer the actual owner to the outer failure before leaving. */
XR_FUNC XrtHostTempResult xrt_execution_temp_directory(const XrtHostNullableSlots *,
    const XrtManagedHeader *root, bool (*current)(void *), void *context, uint32_t source_id);
XR_FUNC XrtHostTempResult xrt_execution_temp_admission(uint32_t source_id);
XR_FUNC XR_NORET void xrt_execution_temp_failure_take(XrtTempFailure *owned);

// I/O helpers have already restored their OS state before this transfer.
XR_FUNC XR_NORET void xrt_end_execution(XrtIoResult failure,
    XrtOutputOperation operation, uint32_t source_id);
XR_FUNC XR_NORET void xrt_output_exit(const XrtSourceMap *map, XrtIoResult failure,
    XrtOutputOperation operation, uint32_t source_id);
XR_FUNC XR_NORET void xrt_clock_exit(const XrtSourceMap *map, XrtHostClockFailure failure);
XR_FUNC void xrt_value_failure_report(XrXirValueStatus, const XrtSourceLocation *);
XR_FUNC void xrt_null_unwrap_report(const XrtSourceLocation *);
/* Both hosted backends render the same exact message bytes, with the source
 * presented separately for assertions rather than embedded in the message. */
XR_FUNC bool xrt_message_panic_report(uint32_t code, const char *, size_t, const XrtSourceLocation *);
XR_FUNC const XrtSourceLocation *xrt_try_assert_location(const XrtTryAssertFailure *);
typedef struct XrtTryAssertValue {
    const uint8_t *metadata;
    size_t metadata_size;
    uint32_t variant, field_count;
} XrtTryAssertValue;
/* Both the field data and the borrowed name/key bytes remain valid until the
 * failure is dropped, even after the original execution and code retire. */
XR_FUNC bool xrt_try_assert_value(const XrtTryAssertFailure *,XrtTryAssertValue *);
XR_FUNC bool xrt_try_assert_field(const XrtTryAssertFailure *,uint32_t field,uint32_t *scalar,uint64_t *bits);
XR_FUNC bool xrt_try_assert_report(const XrtTryAssertFailure *);
XR_FUNC bool xrt_uncaught_error_report(const XrtErrorSnapshot *);
#endif // XNATIVE_EXECUTION_H
