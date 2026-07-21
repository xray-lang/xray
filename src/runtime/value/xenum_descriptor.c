/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xenum_descriptor.c - erased enum-domain descriptor box
 */

#include "xenum_descriptor.h"
#include "../mem/xheap.h"

XrValue xr_enum_descriptor_box_new(struct XrCoroutine *coro, uint32_t layout_id,
                                   uint8_t metadata_kind, int64_t scalar) {
    XrEnumDescriptorBox *box =
        (XrEnumDescriptorBox *) xr_alloc(coro, sizeof(*box), XR_TENUM_DESCRIPTOR);
    if (!box)
        return XR_NULL_VAL;
    xr_obj_header_init_type(&box->hdr, XR_TENUM_DESCRIPTOR);
    box->layout_id = layout_id;
    box->metadata_kind = metadata_kind;
    box->_reserved[0] = box->_reserved[1] = box->_reserved[2] = 0;
    box->scalar = scalar;
    return XR_FROM_PTR(box);
}

bool xr_value_is_enum_descriptor(XrValue value) {
    return XR_IS_PTR(value) && XR_HEAP_TYPE(value) == XR_TENUM_DESCRIPTOR && value.ptr != NULL;
}

bool xr_enum_descriptor_matches(XrValue value, uint32_t layout_id, uint8_t metadata_kind) {
    if (!xr_value_is_enum_descriptor(value))
        return false;
    const XrEnumDescriptorBox *box = (const XrEnumDescriptorBox *) XR_TO_PTR(value);
    return box->layout_id == layout_id && box->metadata_kind == metadata_kind;
}

bool xr_enum_descriptor_scalar(XrValue value, int64_t *out_scalar) {
    if (!xr_value_is_enum_descriptor(value) || !out_scalar)
        return false;
    *out_scalar = ((const XrEnumDescriptorBox *) XR_TO_PTR(value))->scalar;
    return true;
}
