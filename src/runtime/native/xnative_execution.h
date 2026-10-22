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
#include "os/os_native_output.h"

typedef enum XrtOutputOperation { XRT_OUTPUT_FLUSH, XRT_OUTPUT_WRITE } XrtOutputOperation;
typedef struct XrtOutputFailure {
    XrtIoResult io;
    XrtOutputOperation operation;
    uint32_t source_id;
} XrtOutputFailure;

typedef struct XrtArithmeticPanic {
    uint32_t code, source_id;
} XrtArithmeticPanic;

typedef enum XrtExecutionFailureKind {
    XRT_FAILURE_OUTPUT,
    XRT_FAILURE_ARITHMETIC_PANIC
} XrtExecutionFailureKind;

typedef struct XrtExecutionFailure {
    XrtExecutionFailureKind kind;
    union {
        XrtOutputFailure output;
        XrtArithmeticPanic panic;
    } detail;
} XrtExecutionFailure;

typedef enum XrtExecutionStatus {
    XRT_EXECUTION_COMPLETED,
    XRT_EXECUTION_FAILURE,
    XRT_EXECUTION_INVALID
} XrtExecutionStatus;

// All inputs are borrowed through this active synchronous C call only. The
// caller holds a location reference and owns failure/context, including the
// sole mutable VM stack. INVALID invokes no guest. COMPLETED and INVALID
// preserve every byte of failure. COMPLETED means root returned normally;
// it does not reinterpret VM statuses.
// FAILURE writes tagged captured facts only. The previous binding/target is
// restored on either return. No target crosses yield, thread handoff or FFI.
XR_FUNC XrtExecutionStatus xrt_run_embedded(const XrtLocationOwner *locations,
    XrtExecutionFailure *failure, XrtSynchronousRoot root, void *context);

// I/O helpers have already restored their OS state before this transfer.
XR_FUNC XR_NORET void xrt_end_execution(XrtIoResult failure,
    XrtOutputOperation operation, uint32_t source_id);
XR_FUNC XR_NORET void xrt_output_exit(const XrtSourceMap *map, XrtIoResult failure,
    XrtOutputOperation operation, uint32_t source_id);
#endif // XNATIVE_EXECUTION_H
