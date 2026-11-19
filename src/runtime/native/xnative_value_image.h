/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_value_image.h - Independent receiving plans from a raw value image
 *
 * KEY CONCEPT:
 *   Binding copies the physical descriptions once at the image boundary.
 *   Values subsequently borrow sealed plans without compiler-pool queries.
 */
#ifndef XNATIVE_VALUE_IMAGE_H
#define XNATIVE_VALUE_IMAGE_H
#include "xnative_value_plan.h"

/* Call after verifying the row's vector spans and its earlier dependencies. */
static inline bool xrt_value_image_inline_display_shape(const XrtValueImage *image, uint32_t index) {
    const XrtValueImageType *row = &image->types[index];
    if (row->kind == XRT_VALUE_NULLABLE) {
        if (!row->nullable_element || row->nullable_element > index) return false;
        const XrtValueImageType *child = &image->types[row->nullable_element - 1];
        return xrt_value_kind_is_display_leaf(child->kind) || (child->flags & XRT_VALUE_INLINE_DISPLAY);
    }
    if (row->kind != XRT_VALUE_TUPLE) return false;
    for (uint32_t field = 0; field < row->field_count; ++field) {
        uint32_t type = image->slots[row->first_field + field].type;
        if (!type || type > index) return false;
        const XrtValueImageType *child = &image->types[type - 1];
        if (!xrt_value_kind_is_display_leaf(child->kind) && !(child->flags & XRT_VALUE_INLINE_DISPLAY)) return false;
    }
    return true;
}

typedef struct XrtValueImagePlans XrtValueImagePlans;
typedef struct XrtBoundInlineArray {
    const XrtValuePlan *element;
    XrtManagedOps *ops;
} XrtBoundInlineArray;
/* Descriptors remain immutable through binding. Code is an independent real
 * lease for the image's executable code and raw descriptions, never Layout,
 * State, Ops or runtime plans. Binding takes a temporary lease before provider
 * calls; only Function/Environment plans retain it after complete receiving
 * copies. A static C image needs no synthetic lease. No entry is invoked here.
 * All metadata, including temporary vectors, uses the supplied billing owner.
 * Failure leaves *empty unchanged. An entirely empty image succeeds without
 * allocation and leaves *empty NULL. A live set is move-only. */
XR_FUNC XrtManagedStatus xrt_value_image_bind(XrtBillingOwner *, const XrtValueImage *,
    XrtManagedLease code, XrtValueImagePlans **empty);
XR_FUNC void xrt_value_image_drop(XrtValueImagePlans **owned);
XR_FUNC uint32_t xrt_value_image_count(const XrtValueImagePlans *);
/* A returned plan borrows the live set. Retain it before retiring that set.
 * The one-based id belongs to this exact image, not another module's pool. */
XR_FUNC XrtManagedStatus xrt_value_image_get(const XrtValueImagePlans *, uint32_t id,
    const XrtValuePlan **empty);
/* Both references borrow the live image. Non-Array rows and the closed scalar
 * families do not have an inline backing descriptor. */
XR_FUNC XrtManagedStatus xrt_value_image_inline_array(const XrtValueImagePlans *, uint32_t id,
    XrtBoundInlineArray *empty);
#endif // XNATIVE_VALUE_IMAGE_H
