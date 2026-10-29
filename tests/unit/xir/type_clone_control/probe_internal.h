/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * probe_internal.h - Compile each canonical copy TU with observations around its real helpers.
 */
#ifndef TYPE_CLONE_PROBE_INTERNAL_H
#define TYPE_CLONE_PROBE_INTERNAL_H
#include "type_clone_probe.h"
#include "xir/xxir_compile_memory.h"
static inline XrCompileResourceStatus tc_poll(XrCompileRequestControl *control) {
    tc_probe_observe(control);return xr_compile_request_control_poll(control);
}
static inline void *tc_allocate(const XrXirCompileContext *context,size_t bytes,XrXirStatus *status,const char *caller) {
    return tc_probe_alloc(context,bytes,status,caller);
}
#define xr_compile_request_control_poll tc_poll
#define xir_compile_alloc(c,b,s) tc_allocate(c,b,s,__func__)
#include "xir/xxir_type_copy_control.h"
#undef xir_compile_alloc
#undef xr_compile_request_control_poll
static inline void *tc_zero(const XrXirCompileContext *context,size_t count,size_t size,XrXirStatus *status,XrCompileRequestControl *control) {
    bool watch=tc_probe_zero_begin(count,size,control);void *memory=xir_type_copy_zero(context,count,size,status,control);
    tc_probe_span_end(watch,*status);return memory;
}
static inline void *tc_bytes(const XrXirCompileContext *context,const void *source,size_t count,size_t size,XrXirStatus *status,XrCompileRequestControl *control) {
    bool watch=tc_probe_copy_begin(source,count,size,control);void *memory=xir_type_copy_bytes(context,source,count,size,status,control);
    tc_probe_span_end(watch,*status);return memory;
}
static inline XrCompileResourceStatus tc_span(XrCompileRequestControl *control,void *destination,const void *source,size_t bytes) {
    bool watch=tc_probe_copy_begin(source,bytes,1,control);TcProbe *p=tc_probe_current();
    if(watch) {p->destination=destination;memset(destination,0xa5,bytes);}
    XrCompileResourceStatus status=xir_lower_copy_span(control,destination,source,bytes);
    tc_probe_span_end(watch,xir_compile_resource_status(status));return status;
}
#endif // TYPE_CLONE_PROBE_INTERNAL_H
