/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_input_control.h - Synchronous source-byte transfer and validation
 */
#ifndef XSOURCE_INPUT_CONTROL_H
#define XSOURCE_INPUT_CONTROL_H
#include "../base/xcompile_state.h"
#include "../base/xcompile_request.h"
#include <limits.h>
#include <string.h>

enum { XR_SOURCE_INPUT_CHUNK_BYTES = 4096 };
typedef struct XrCompileInputNul { bool found; size_t byte; int line,column; } XrCompileInputNul;

/* These helpers borrow the control only on the calling stack. A failed copy
 * never publishes its partially initialized destination to a diagnostic. */
static inline XrCompileResourceStatus xr_source_input_poll(XrCompileState *state,
    XrCompileRequestControl *control) {
    XrCompileResourceStatus status=xr_compile_state_status(state);
    if(status==XR_COMPILE_RESOURCE_OK&&control)status=xr_compile_request_control_poll(control);
    return xr_compile_state_fail(state,status);
}
static inline XrCompileResourceStatus xr_source_input_copy(XrCompileState *state,
    XrCompileRequestControl *control,char *destination,const char *source,size_t length,size_t chunk) {
    if(!chunk||(length&&(!destination||!source)))return xr_compile_state_fail(state,XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    if(!control)return xr_compile_state_copy(state,destination,source,length);
    size_t at=0;
    do {
        XrCompileResourceStatus status=xr_source_input_poll(state,control);
        if(status!=XR_COMPILE_RESOURCE_OK)return status;
        size_t count=length-at;if(count>chunk)count=chunk;
        if(count)memcpy(destination+at,source+at,count);
        at+=count;
    } while(at<length);
    return XR_COMPILE_RESOURCE_OK;
}
static inline XrCompileResourceStatus xr_source_input_length(XrCompileState *state,
    XrCompileRequestControl *control,const char *source,size_t chunk,size_t *output) {
    if(!source||!chunk||!output)return xr_compile_state_fail(state,XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    size_t at=0;
    for(;;) {
        XrCompileResourceStatus status=xr_source_input_poll(state,control);
        if(status!=XR_COMPILE_RESOURCE_OK)return status;
        for(size_t count=0;count<chunk;++count) {
            if(!source[at]){*output=at;return XR_COMPILE_RESOURCE_OK;}
            if(at==SIZE_MAX-1)return xr_compile_state_fail(state,XR_COMPILE_RESOURCE_OVERFLOW);
            ++at;
        }
    }
}
static inline XrCompileResourceStatus xr_source_input_nul(XrCompileState *state,
    XrCompileRequestControl *control,const char *source,size_t length,size_t chunk,XrCompileInputNul *output) {
    if((length&&!source)||!chunk||!output)return xr_compile_state_fail(state,XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    XrCompileInputNul result={0};size_t at=0,line=1,line_start=0;
    do {
        XrCompileResourceStatus status=xr_source_input_poll(state,control);
        if(status!=XR_COMPILE_RESOURCE_OK)return status;
        size_t count=length-at;if(count>chunk)count=chunk;
        size_t end=at+count;
        for(;at<end;++at) {
            if(!source[at]) {
                if(line>INT_MAX||at-line_start>=INT_MAX)
                    return xr_compile_state_fail(state,XR_COMPILE_RESOURCE_OVERFLOW);
                result=(XrCompileInputNul){true,at,(int)line,(int)(at-line_start)+1};
                *output=result;return XR_COMPILE_RESOURCE_OK;
            }
            if(source[at]=='\n'){++line;line_start=at+1;}
        }
    } while(at<length);
    *output=result;return XR_COMPILE_RESOURCE_OK;
}
#endif // XSOURCE_INPUT_CONTROL_H
