/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_source_value.h - Independently owned typed data on its execution memory owner
 */
#ifndef XRAY_SOURCE_VALUE_H
#define XRAY_SOURCE_VALUE_H
#include "xray_source.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct XrSourceValue XrSourceValue;
typedef enum XrSourceValueKind {
    XR_SOURCE_VALUE_UNIT, XR_SOURCE_VALUE_BOOL,
    XR_SOURCE_VALUE_I8, XR_SOURCE_VALUE_I16, XR_SOURCE_VALUE_I32, XR_SOURCE_VALUE_I64,
    XR_SOURCE_VALUE_U8, XR_SOURCE_VALUE_U16, XR_SOURCE_VALUE_U32, XR_SOURCE_VALUE_U64,
    XR_SOURCE_VALUE_F32, XR_SOURCE_VALUE_F64, XR_SOURCE_VALUE_RUNE, XR_SOURCE_VALUE_STRING,
    XR_SOURCE_VALUE_ARRAY, XR_SOURCE_VALUE_TUPLE, XR_SOURCE_VALUE_STRUCT, XR_SOURCE_VALUE_ENUM,
    XR_SOURCE_VALUE_NULLABLE, XR_SOURCE_VALUE_CLASS, XR_SOURCE_VALUE_ATOMIC, XR_SOURCE_VALUE_TASK,
    XR_SOURCE_VALUE_CALLABLE, XR_SOURCE_VALUE_ERROR, XR_SOURCE_VALUE_PANIC_INFO
} XrSourceValueKind;
typedef struct XrSourceScalar {
    XrSourceValueKind kind;
    union { int64_t signed_value; uint64_t unsigned_value; float f32; double f64;
        uint32_t rune; int boolean; } value;
} XrSourceScalar;
/* Outputs must be NULL and remain unchanged on failure. Scalars and String
 * storage belong to the explicit execution memory owner. String accepts an
 * exact UTF-8 span, including NUL. F32 requires its runtime's canonical NaN;
 * F64 preserves every binary64 bit. Owned values may outlive the execution
 * handle. Calls require caller exclusion. Strings above PTRDIFF_MAX bytes
 * are unrepresentable and return LIMIT. */
XRAY_API XrSourceStatus xray_source_value_new_scalar(XrSourceExecution *, const XrSourceScalar *, XrSourceValue **);
XRAY_API XrSourceStatus xray_source_value_new_string(XrSourceExecution *, const char *, size_t, XrSourceValue **);
/* Copy actual RETURNED or THROWN data, without adopting its execution state.
 * Owning data grants no Domain, loan, callable or Task execution permission. */
XRAY_API XrSourceStatus xray_source_result_value(const XrSourceResult *, XrSourceValue **);
XRAY_API XrSourceStatus xray_source_value_copy(const XrSourceValue *, XrSourceValue **);
XRAY_API void xray_source_value_free(XrSourceValue *);
/* Refusals preserve outputs. Byte spans borrow until value_free. */
XRAY_API XrSourceStatus xray_source_value_kind(const XrSourceValue *, XrSourceValueKind *);
XRAY_API XrSourceStatus xray_source_value_scalar(const XrSourceValue *, XrSourceScalar *);
XRAY_API XrSourceStatus xray_source_value_bytes(const XrSourceValue *, const char **, size_t *);
/* Synchronous shared formatting; keep the value alive and unchanged until it
 * returns. A sink may receive a prefix before failure. Unsupported display
 * families return UNSUPPORTED rather than inventing text. */
XRAY_API XrSourceStatus xray_source_value_format(const XrSourceValue *, XrSourceWrite, void *);
/* Immutable data projections retain the original value/arena ownership and
 * grant no execution or member-access authority. Tuple requires a non-Unit
 * tuple; an out-of-range field returns BAD_ARGUMENT, or LIMIT above UINT32_MAX.
 * Nullable writes zero/one
 * and publishes an owned payload only for Some. All failures preserve outputs;
 * owned outputs must be NULL. PanicInfo messages own exact assertion bytes or
 * the common kernel's canonical intrinsic message. Copies and allocations use
 * the retained execution memory owner and preserve outputs on failure. */
XRAY_API XrSourceStatus xray_source_value_tuple_count(const XrSourceValue *, size_t *);
XRAY_API XrSourceStatus xray_source_value_tuple_get(const XrSourceValue *, size_t, XrSourceValue **);
XRAY_API XrSourceStatus xray_source_value_nullable(const XrSourceValue *, int *, XrSourceValue **);
XRAY_API XrSourceStatus xray_source_value_panic_code(const XrSourceValue *, int64_t *);
XRAY_API XrSourceStatus xray_source_value_panic_message(const XrSourceValue *, XrSourceValue **);
/* Copy an already terminal Task's actual outcome into an independently owned
 * Result. OK means the copy succeeded; inspect result_event for RETURNED,
 * THROWN, PANICKED, CANCELLED or another real terminal state. Pending returns
 * BAD_STATE without driving, waiting, cancelling or acquiring an executor
 * lease. Copies retain actual Task data and use the execution memory owner; failure
 * preserves *output, which must be NULL. Producers and the Task may die after
 * success. Data ownership grants no Task execution or cross-Instance access. */
XRAY_API XrSourceStatus xray_source_value_task_result(const XrSourceValue *, XrSourceResult **);
#ifdef __cplusplus
}
#endif
#endif // XRAY_SOURCE_VALUE_H
