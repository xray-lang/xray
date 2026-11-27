/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_ctfe.h - Bounded deterministic evaluation of integer register code.
 *
 * KEY CONCEPT:
 *   The compiler borrows the code for one evaluation. A located failure
 *   retains its source owner independently of that code and its producer.
 */
#ifndef XRBC_CTFE_H
#define XRBC_CTFE_H

#include "xrbc.h"

typedef enum XrRbcCtfeStatus {
    XR_RBC_CTFE_OK,
    XR_RBC_CTFE_INVALID,
    XR_RBC_CTFE_UNSUPPORTED,
    XR_RBC_CTFE_CODE_LIMIT,
    XR_RBC_CTFE_FUEL_LIMIT,
    XR_RBC_CTFE_MEMORY_LIMIT,
    XR_RBC_CTFE_OUT_OF_MEMORY,
    XR_RBC_CTFE_CANCELLED,
    XR_RBC_CTFE_PANIC
} XrRbcCtfeStatus;

typedef struct XrRbcCtfeRequest {
    const XrRbcFunction *functions;
    size_t function_count;
    uint32_t entry;
    const uint64_t *arguments;
    size_t argument_count;
    XrtLocationOwner *locations;
    const XrtHostExecutionControl *control;
    size_t code_words;
    size_t memory_bytes;
    uint64_t fuel;
} XrRbcCtfeRequest;

typedef struct XrRbcCtfeOutcome {
    uint64_t bits;
    uint32_t source_id;
    uint32_t panic_code;
    XrtLocationOwner *locations;
} XrRbcCtfeOutcome;

/* The descriptors are immutable and readable during the synchronous call.
 * Calls may only name this closed table. The root entry, each direct call and
 * each taken backward branch consume one unit. The static word limit bounds
 * straight line work. The byte limit covers the capacities of both arrays.
 * The result must be empty. Failures return no scalar bits. The caller may
 * release the producer before reading a retained diagnostic location. */
XR_FUNC XrRbcCtfeStatus xr_rbc_ctfe_evaluate(const XrRbcCtfeRequest *, XrRbcCtfeOutcome *);
XR_FUNC void xr_rbc_ctfe_outcome_free(XrRbcCtfeOutcome *);

#endif // XRBC_CTFE_H
