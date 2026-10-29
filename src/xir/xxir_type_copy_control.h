/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_type_copy_control.h - Synchronous ownership of copied type metadata
 *
 * KEY CONCEPT:
 *   Poll only outside allocator locks. Interrupted storage is freed before it
 *   can enter a destructible child array; requests never enter copied metadata.
 */
#ifndef XXIR_TYPE_COPY_CONTROL_H
#define XXIR_TYPE_COPY_CONTROL_H
#include "xxir_compile_memory.h"
#include "xxir_lower_control.h"

static inline XrXirStatus xir_type_copy_poll(XrCompileRequestControl *control) {
    return control?xir_compile_resource_status(xr_compile_request_control_poll(control)):XR_XIR_OK;
}
static inline XrXirStatus xir_type_copy_rows(XrCompileRequestControl *control,uint32_t row) {
    return row&63u?XR_XIR_OK:xir_type_copy_poll(control);
}
static inline void *xir_type_copy_zero(const XrXirCompileContext *context,size_t count,size_t size,
    XrXirStatus *status,XrCompileRequestControl *control) {
    if(*status!=XR_XIR_OK || !count || !size)return NULL;
    if(!control)return xir_compile_calloc(context,count,size,status);
    if(count>SIZE_MAX/size) {*status=XR_XIR_OVERFLOW;return NULL;}
    size_t bytes=count*size;
    void *memory=xir_compile_alloc(context,bytes,status);if(!memory)return NULL;
    for(size_t offset=0;offset<bytes;) {
        *status=xir_type_copy_poll(control);if(*status!=XR_XIR_OK)break;
        size_t span=bytes-offset;if(span>4096)span=4096;
        memset((unsigned char *)memory+offset,0,span);offset+=span;
    }
    if(*status==XR_XIR_OK)*status=xir_type_copy_poll(control);
    if(*status!=XR_XIR_OK) {xr_compile_resources_free(memory);return NULL;}
    return memory;
}
static inline void *xir_type_copy_bytes(const XrXirCompileContext *context,const void *source,
    size_t count,size_t size,XrXirStatus *status,XrCompileRequestControl *control) {
    if(*status!=XR_XIR_OK || !count || !size)return NULL;
    if(count>SIZE_MAX/size) {*status=XR_XIR_OVERFLOW;return NULL;}
    if(!source) {*status=XR_XIR_BAD_STRUCTURE;return NULL;}
    size_t bytes=count*size;
    void *memory=xir_compile_alloc(context,bytes,status);if(!memory)return NULL;
    *status=xir_compile_resource_status(xir_lower_copy_span(control,memory,source,bytes));
    if(*status!=XR_XIR_OK) {xr_compile_resources_free(memory);return NULL;}
    return memory;
}
#endif // XXIR_TYPE_COPY_CONTROL_H
