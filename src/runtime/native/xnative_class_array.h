/*
 * xray - Lightweight typed scripting with native concurrency
 * https://xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_class_array.h - Synchronous guarded Class field access
 */
#ifndef XNATIVE_CLASS_ARRAY_H
#define XNATIVE_CLASS_ARRAY_H
#include "xnative_backing_types.h"
#include "xnative_value_plan.h"
#include "xray_native_place.h"

/* The caller keeps both owners alive until return. No descriptor stores an
 * interior address across evaluations or accesses. */
typedef struct XrtClassFieldAccess {
    XrtManagedHeader *instance;
    const XrtValuePlan *plan;
    size_t index;
} XrtClassFieldAccess;
/* Called only by the synchronous transactions below. No owner contents may
 * be accessed before locking the actual instance field guard. */
XR_FUNC XrtManagedStatus xrt_class_array_binding(const XrtClassFieldAccess *, XrtExternalOwner *, XrtBackingArrayKind *);
XR_FUNC XrtManagedStatus xrt_class_array_field_copy(const XrtClassFieldAccess *, XrtManagedFrame *, size_t destination);
XR_FUNC XrtManagedStatus xrt_class_array_field_replace(const XrtClassFieldAccess *, const XrtManagedHeader *incoming);
XR_FUNC XrtManagedStatus xrt_class_array_field_push(const XrtClassFieldAccess *,
    XrtManagedFrame *, size_t workspace, const XrtBackingTypes *, int64_t item);
XR_FUNC XrtManagedStatus xrt_class_array_field_push_string(const XrtClassFieldAccess *,
    XrtManagedFrame *, size_t workspace, const XrtBackingTypes *, const XrtStringBacking *item);
#endif // XNATIVE_CLASS_ARRAY_H
