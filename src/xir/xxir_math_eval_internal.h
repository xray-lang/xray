/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_math_eval_internal.h - Shared pure math results with checked integer conversion
 *
 * KEY CONCEPT:
 *   Pure native frames use identical binary64 operations and canonical output in VM, C and CTFE.
 */
#ifndef XXIR_MATH_EVAL_INTERNAL_H
#define XXIR_MATH_EVAL_INTERNAL_H
#include "xxir_math_native.h"
#include "xxir_value.h"
#include "xxir_float.h"
#include <math.h>
#include <string.h>

static inline bool xir_math_native_arguments(uint32_t id,const XrXirValue *arguments,
    uint32_t count,const XrXirValue *output) {
    const XirMathNativeShape *shape=xir_math_native_shape(id);
    if(!shape || count!=shape->count || (count&&!arguments) || !output ||
        output->type || output->reserved || output->payload) return false;
    for(uint32_t p=0;p<count;++p)
        if(arguments[p].reserved || arguments[p].type!=(uint32_t)(shape->integer_input?XR_XIR_I64:XR_XIR_F64))return false;
    return true;
}
static inline XrXirRunStatus xir_math_native_evaluate(uint32_t id,const XrXirValue *arguments,
    uint32_t count,XrXirValue *output) {
    if(!xir_math_native_arguments(id,arguments,count,output) || xir_math_native_random(id))
        return XR_XIR_RUN_BAD_ARTIFACT;
    const XirMathNativeShape *shape=xir_math_native_shape(id);
    double x=0.0,y=0.0,result=0.0;
    if(count)memcpy(&x,&arguments[0].payload,sizeof(x));
    if(count>1)memcpy(&y,&arguments[1].payload,sizeof(y));
    switch(id) {
    case XR_NATIVE_CALLABLE_MATH_ABS:result=fabs(x);break;
    case XR_NATIVE_CALLABLE_MATH_FLOOR:result=floor(x);break;
    case XR_NATIVE_CALLABLE_MATH_CEIL:result=ceil(x);break;
    case XR_NATIVE_CALLABLE_MATH_ROUND:result=round(x);break;
    case XR_NATIVE_CALLABLE_MATH_SQRT:result=sqrt(x);break;
    case XR_NATIVE_CALLABLE_MATH_POW:result=pow(x,y);break;
    case XR_NATIVE_CALLABLE_MATH_SIN:result=sin(x);break;
    case XR_NATIVE_CALLABLE_MATH_COS:result=cos(x);break;
    case XR_NATIVE_CALLABLE_MATH_TAN:result=tan(x);break;
    case XR_NATIVE_CALLABLE_MATH_ASIN:result=asin(x);break;
    case XR_NATIVE_CALLABLE_MATH_ACOS:result=acos(x);break;
    case XR_NATIVE_CALLABLE_MATH_ATAN:result=atan(x);break;
    case XR_NATIVE_CALLABLE_MATH_ATAN2:result=atan2(x,y);break;
    case XR_NATIVE_CALLABLE_MATH_LOG:result=log(x);break;
    case XR_NATIVE_CALLABLE_MATH_LOG10:result=log10(x);break;
    case XR_NATIVE_CALLABLE_MATH_LOG2:result=log2(x);break;
    case XR_NATIVE_CALLABLE_MATH_EXP:result=exp(x);break;
    case XR_NATIVE_CALLABLE_MATH_SINH:result=sinh(x);break;
    case XR_NATIVE_CALLABLE_MATH_COSH:result=cosh(x);break;
    case XR_NATIVE_CALLABLE_MATH_TANH:result=tanh(x);break;
    case XR_NATIVE_CALLABLE_MATH_HYPOT:result=hypot(x,y);break;
    case XR_NATIVE_CALLABLE_MATH_CBRT:result=cbrt(x);break;
    case XR_NATIVE_CALLABLE_MATH_TRUNC:result=trunc(x);break;
    case XR_NATIVE_CALLABLE_MATH_FMOD:result=fmod(x,y);break;
    case XR_NATIVE_CALLABLE_MATH_LOG1P:result=log1p(x);break;
    case XR_NATIVE_CALLABLE_MATH_EXPM1:result=expm1(x);break;
    default:return XR_XIR_RUN_BAD_ARTIFACT;
    }
    uint64_t bits;memcpy(&bits,&result,sizeof(bits));
    if(shape->integer_result) {
        int64_t input,integer=0;memcpy(&input,&bits,sizeof(input));
        XrXirRunStatus status=xr_xir_number_convert(XR_XIR_F64,XR_XIR_I64,input,&integer);
        if(status!=XR_XIR_RUN_OK)return status;
        *output=(XrXirValue){XR_XIR_I64,0,(uint64_t)integer};
    } else {
        uint64_t canonical=0;
        if(xr_xir_float_convert(64,64,bits,&canonical)!=XR_XIR_NUMERIC_OK)return XR_XIR_RUN_BAD_ARTIFACT;
        *output=(XrXirValue){XR_XIR_F64,0,canonical};
    }
    return XR_XIR_RUN_OK;
}
#endif // XXIR_MATH_EVAL_INTERNAL_H
