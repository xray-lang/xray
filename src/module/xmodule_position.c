/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmodule_position.c - Preserve exact source positions as owned scalar facts
 */
#include "xmodule_position.h"
#include "xmodule_compile_internal.h"
#include "../shared/xr_utf8_core.h"
#include <limits.h>
static int position_read(void *context,const uint8_t *address,uint8_t *out) {
    if(!module_work(context,1))return 0;
    *out=*address;return 1;
}
XR_FUNC XrModuleStatus xr_compile_module_source_position(XrCompileResources *resources,
    const char *text,size_t length,int line,int column,XrModuleSourcePosition *out) {
    if(!resources||!text||!out||line<=0||column<=0||length==SIZE_MAX)return XR_MODULE_INVALID;
    ModuleWork work={resources,XR_MODULE_OK};
    if(!module_work(&work,1))return work.status;
    uint32_t row=0,source_row=0,units=0;size_t bytes=0,offset=0;
    for(;;) {
        if(source_row==(uint32_t)(line-1)&&bytes==(size_t)(column-1)) {
            if(row>INT32_MAX||units>INT32_MAX)return XR_MODULE_INVALID;
            *out=(XrModuleSourcePosition){row,units};return XR_MODULE_OK;
        }
        if(offset==length)return XR_MODULE_INVALID;
        XrUtf8Step step;
        if(!xr_utf8_core_decode_step_read((const uint8_t *)text+offset,length-offset,position_read,&work,&step))return work.status;
        if(step.error!=XR_UTF8_OK||!step.scalar)return XR_MODULE_INVALID;
        if(step.scalar=='\r') {
            if(offset+1<length) {
                if(!module_work(&work,1))return work.status;
                if(text[offset+1]=='\n')++step.consumed;
            }
            if(row==UINT32_MAX)return XR_MODULE_INVALID;
            ++row;units=0;
            if(step.consumed==2){++source_row;bytes=0;}else ++bytes;
        }else if(step.scalar=='\n') {
            if(row==UINT32_MAX||source_row==UINT32_MAX)return XR_MODULE_INVALID;
            ++row;++source_row;units=0;bytes=0;
        }else {
            uint32_t width=step.scalar>UINT32_C(0xffff)?2u:1u;
            if(units>UINT32_MAX-width||bytes>SIZE_MAX-step.consumed)return XR_MODULE_INVALID;
            units+=width;bytes+=step.consumed;
        }
        offset+=step.consumed;
        if(source_row>(uint32_t)(line-1)||(source_row==(uint32_t)(line-1)&&bytes>(size_t)(column-1)))return XR_MODULE_INVALID;
    }
}
