/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_scope_cancel_shape.h - Locate the original lexical scope continuation
 */
#ifndef XXIR_SCOPE_CANCEL_SHAPE_H
#define XXIR_SCOPE_CANCEL_SHAPE_H
#include "xxir.h"

static inline uint32_t xr_xir_scope_cancel_join(const XrXirFunction *function,uint32_t frontier) {
    if(!function||frontier>=function->instruction_count||
        function->instructions[frontier].op!=XR_XIR_SCOPE_BEGIN||
        function->parameter_count>UINT32_MAX-frontier)return UINT32_MAX;
    uint32_t group=function->parameter_count+frontier;
    for(uint32_t i=frontier+1;i<function->instruction_count;++i)
        if(function->instructions[i].op==XR_XIR_SCOPE_JOIN&&function->instructions[i].args[0]==group)return i;
    return UINT32_MAX;
}
#endif // XXIR_SCOPE_CANCEL_SHAPE_H
