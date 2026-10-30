/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_receive_control.h - Synchronous control of owned metadata receiving
 *
 * KEY CONCEPT:
 *   Only the calling stack borrows a request. Allocated bytes are initialized
 *   outside allocator locks, and interrupted copies publish no child owner.
 */
#ifndef XXIR_RECEIVE_CONTROL_H
#define XXIR_RECEIVE_CONTROL_H
#include "xxir_type_copy_control.h"

static inline XrXirStatus xir_receive_poll(XrCompileRequestControl *control) {
    return xir_type_copy_poll(control);
}
static inline XrXirStatus xir_receive_step(XrCompileRequestControl *control,uint32_t *rows) {
    if (!control) return XR_XIR_OK;
    if (++*rows<64u) return XR_XIR_OK;
    *rows=0;return xir_receive_poll(control);
}
static inline XrXirStatus xir_receive_copy(XrCompileRequestControl *control,
    void *destination,const void *source,size_t bytes) {
    return xir_compile_resource_status(xir_lower_copy_span(control,destination,source,bytes));
}
static inline XrXirStatus xir_receive_strlen(XrCompileRequestControl *control,
    const char *text,size_t *output) {
    if (!text || !output) return XR_XIR_BAD_STRUCTURE;
    size_t length=0;
    for (;;) {
        if (!(length&4095u)) {
            XrXirStatus status=xir_receive_poll(control);if (status!=XR_XIR_OK) return status;
        }
        if (length==SIZE_MAX) return XR_XIR_OVERFLOW;
        if (!text[length]) break;
        ++length;
    }
    XrXirStatus status=xir_receive_poll(control);
    if (status==XR_XIR_OK) *output=length;
    return status;
}
static inline XrXirStatus xir_receive_nul_free(XrCompileRequestControl *control,
    const char *text,size_t bytes) {
    if (bytes && !text) return XR_XIR_BAD_STRUCTURE;
    for (size_t offset=0;offset<bytes;) {
        XrXirStatus status=xir_receive_poll(control);if (status!=XR_XIR_OK) return status;
        size_t count=bytes-offset;if (count>4096) count=4096;
        if (memchr(text+offset,0,count)) return XR_XIR_BAD_STRUCTURE;
        offset+=count;
    }
    return xir_receive_poll(control);
}
static inline XrXirStatus xir_receive_compare(XrCompileRequestControl *control,
    const void *left,const void *right,size_t bytes,int *output) {
    if (!output || (bytes && (!left || !right))) return XR_XIR_BAD_STRUCTURE;
    int order=0;
    for (size_t offset=0;offset<bytes;) {
        XrXirStatus status=xir_receive_poll(control);if (status!=XR_XIR_OK) return status;
        size_t count=bytes-offset;if (count>4096) count=4096;
        order=memcmp((const unsigned char *)left+offset,(const unsigned char *)right+offset,count);
        if (order) break;
        offset+=count;
    }
    XrXirStatus status=xir_receive_poll(control);
    if (status==XR_XIR_OK) *output=order;
    return status;
}
#endif // XXIR_RECEIVE_CONTROL_H
