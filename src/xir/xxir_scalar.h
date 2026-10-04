/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_scalar.h - Shared scalar execution boundary and checked arithmetic
 *
 * KEY CONCEPT:
 *   Interpreter and native code share scalar rules and frame accounting.
 */

#ifndef XXIR_SCALAR_H
#define XXIR_SCALAR_H

#include "../base/xdefs.h"
#include <string.h>

#include "xxir_value.h"

typedef enum XrXirRunStatus {
    XR_XIR_RUN_OK,
    XR_XIR_RUN_BAD_ARGUMENT,
    XR_XIR_RUN_BAD_ARTIFACT,
    XR_XIR_RUN_BAD_ABI,
    XR_XIR_RUN_DIVIDE_BY_ZERO,
    XR_XIR_RUN_STEP_LIMIT,
    XR_XIR_RUN_FRAME_LIMIT,
    XR_XIR_RUN_OUT_OF_MEMORY, XR_XIR_RUN_NUMERIC_RANGE,
    XR_XIR_RUN_HOST_ERROR,
    XR_XIR_RUN_NULL_UNWRAP
} XrXirRunStatus;

/* Host clocks readable by the time primitives; the values are fixed operand encodings. */
typedef enum XrXirClockKind {
    XR_XIR_CLOCK_REALTIME = 1, XR_XIR_CLOCK_CPU, XR_XIR_CLOCK_MONOTONIC
} XrXirClockKind;

typedef struct XrXirRunContext {
    uint64_t steps;
    uint64_t frame_limit;
    uint64_t live_bytes;
    uint64_t peak_bytes;
    uint64_t allocations;
    uint64_t frees;
} XrXirRunContext;

typedef XrXirRunStatus (*XrXirLeafEntry)(XrXirRunContext *context,
    const XrXirValue *arguments, uint32_t argument_count, XrXirValue *result);

XR_FUNC XrXirRunStatus xr_xir_scalar_frame_begin(XrXirRunContext *context,
                                               uint32_t bytes, void **frame);
XR_FUNC void xr_xir_scalar_frame_end(XrXirRunContext *context, uint32_t bytes, void *frame);
typedef enum XrXirArithmetic {
    XR_XIR_ARITH_ADD, XR_XIR_ARITH_SUB, XR_XIR_ARITH_MUL, XR_XIR_ARITH_DIV, XR_XIR_ARITH_REM,
    XR_XIR_ARITH_AND, XR_XIR_ARITH_OR, XR_XIR_ARITH_XOR, XR_XIR_ARITH_SHL, XR_XIR_ARITH_SHR
} XrXirArithmetic;
typedef struct XrXirIntegerFormat {
    uint32_t bits;
    bool is_signed;
} XrXirIntegerFormat;
static inline XrXirIntegerFormat xr_xir_integer_format(XrXirType type) {
    return (XrXirIntegerFormat) {xr_xir_integer_bits(type), xr_xir_integer_signed(type)};
}
/* Shift counts use all payload bits independently of the left operand format. */
/* Failed construction preserves the caller output. Rune never becomes numeric. */
XR_FUNC XrXirRunStatus xr_xir_rune_convert(XrXirType source, XrXirType target,
    int64_t payload, int64_t *output);
XR_FUNC XrXirRunStatus xr_xir_integer_arithmetic(XrXirIntegerFormat format,
    XrXirArithmetic operation, int64_t left, int64_t right, int64_t *result);
XR_FUNC XrXirRunStatus xr_xir_integer_compare(XrXirIntegerFormat format,
    int64_t left, int64_t right, int *result);
XR_FUNC XrXirRunStatus xr_xir_integer_convert(XrXirIntegerFormat source,
    XrXirIntegerFormat target, int64_t value, int64_t *result);

static inline bool xr_xir_scalar_step(XrXirRunContext *context) {
    if (!context->steps)
        return false;
    --context->steps;
    return true;
}

static inline int64_t xr_xir_scalar_load(const void *frame, uint32_t offset) {
    int64_t value;
    memcpy(&value, (const unsigned char *) frame + offset, sizeof(value));
    return value;
}

static inline void xr_xir_scalar_store(void *frame, uint32_t offset, int64_t value) {
    memcpy((unsigned char *) frame + offset, &value, sizeof(value));
}

#endif // XXIR_SCALAR_H
