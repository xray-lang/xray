/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xenum_descriptor.h - erased enum-domain descriptor box
 */

#ifndef XENUM_DESCRIPTOR_H
#define XENUM_DESCRIPTOR_H

#include "xvalue.h"

struct XrCoroutine;

typedef struct XrEnumDescriptorBox {
    XrObjHeader hdr;
    uint32_t layout_id;
    uint8_t metadata_kind;
    uint8_t _reserved[3];
    int64_t scalar;
} XrEnumDescriptorBox;

XR_FUNC XrValue xr_enum_descriptor_box_new(struct XrCoroutine *coro, uint32_t layout_id,
                                           uint8_t metadata_kind, int64_t scalar);
XR_FUNC bool xr_value_is_enum_descriptor(XrValue value);
XR_FUNC bool xr_enum_descriptor_matches(XrValue value, uint32_t layout_id, uint8_t metadata_kind);
XR_FUNC bool xr_enum_descriptor_scalar(XrValue value, int64_t *out_scalar);

#endif  // XENUM_DESCRIPTOR_H
