/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_unicode_internal.h - Read-only canonical Unicode value adapters
 *
 * KEY CONCEPT:
 *   Checked value handles stay borrowed for one synchronous query.
 *   Scalar results retain no source value or compiler lifetime.
 */
#ifndef XXIR_UNICODE_INTERNAL_H
#define XXIR_UNICODE_INTERNAL_H
#include "xxir_value.h"
#include "../base/xunicode.h"

static inline XrXirValueStatus xir_unicode_property_id(const XrXirValue *input,XrXirValue *output) {
    if (!output || output->type || output->reserved || output->payload) return XR_XIR_VALUE_BAD_ARGUMENT;
    const char *bytes=NULL;size_t length=0;
    if (!xr_xir_string_view(input,&bytes,&length)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrUnicodeProperty property=xr_unicode_property_lookup(bytes,length);
    *output=(XrXirValue){XR_XIR_I64,0,property==XR_UP_INVALID ? -1 : (int64_t)property};
    return XR_XIR_VALUE_OK;
}
static inline XrXirValueStatus xir_unicode_has_property(const XrXirValue *codepoint,
    const XrXirValue *property,XrXirValue *output) {
    if (!codepoint || !property || codepoint->type!=XR_XIR_I64 || property->type!=XR_XIR_I64 ||
        codepoint->reserved || property->reserved || !output || output->type || output->reserved || output->payload)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    int64_t cp=codepoint->payload,prop=property->payload;
    bool matches=cp>=0 && (uint64_t)cp<=UINT32_MAX && prop>=0 && prop<XR_UP_COUNT &&
        xr_unicode_is_property((uint32_t)cp,(XrUnicodeProperty)prop);
    *output=(XrXirValue){XR_XIR_BOOL,0,matches};return XR_XIR_VALUE_OK;
}
#endif // XXIR_UNICODE_INTERNAL_H
