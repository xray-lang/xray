/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_transfer_internal.h - Finite temporary borrowing of actual value DAGs
 *
 * KEY CONCEPT:
 *   Every new leaf remains an authenticated descendant of its exact input.
 */
#ifndef XXIR_VIEW_TRANSFER_INTERNAL_H
#define XXIR_VIEW_TRANSFER_INTERNAL_H
#include "xxir_view_value_internal.h"

typedef struct XrXirViewTransfer {
    const XrXirViewAccess *source,*destination;
    XrXirValueStatus (*borrow)(void *context,const XrXirValue *source,XrXirValue *output);
    void *context;
} XrXirViewTransfer;

/* Rebuilds borrowed Tuple/Optional wrappers without consuming the input.
 * Physical DAG nodes are visited once; unique mutable descendants cannot be
 * copied into multiple output fields. Failure leaves output canonical empty. */
XR_FUNC XrXirValueStatus xr_xir_borrowed_value_reborrow(const XrXirValue *value,
    XrXirType type,const XrXirViewTransfer *transfer,XrXirValue *output);
#endif // XXIR_VIEW_TRANSFER_INTERNAL_H
