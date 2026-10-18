/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_math.c - Common scalar math without compiler or execution ownership
 *
 * KEY CONCEPT:
 *   Numeric results keep the binary64 canonical boundary. The shared unbiased
 *   random algorithm cannot retry after a metered entropy callback fails.
 */
#include "xxir_math.h"
#include "xxir_float.h"
#include "../shared/xr_math_core.h"
#include <math.h>
#include <string.h>
#include <fenv.h>
#include <errno.h>

static XrXirRunStatus math_numeric_status(XrXirNumericStatus status) {
    return status==XR_XIR_NUMERIC_OK ? XR_XIR_RUN_OK :
        status==XR_XIR_NUMERIC_RANGE ? XR_XIR_RUN_NUMERIC_RANGE : XR_XIR_RUN_BAD_ARGUMENT;
}
static XrXirRunStatus math_float_input(int64_t bits,double *output) {
    uint64_t canonical=0;
    XrXirNumericStatus status=xr_xir_float_convert(64,64,(uint64_t)bits,&canonical);
    if (status!=XR_XIR_NUMERIC_OK) return math_numeric_status(status);
    memcpy(output,&canonical,sizeof(*output));return XR_XIR_RUN_OK;
}
static XrXirRunStatus math_float_result(double value,int64_t *output) {
    uint64_t bits=0,canonical=0;memcpy(&bits,&value,sizeof(bits));
    XrXirNumericStatus status=xr_xir_float_convert(64,64,bits,&canonical);
    if (status!=XR_XIR_NUMERIC_OK) return math_numeric_status(status);
    memcpy(output,&canonical,sizeof(*output));return XR_XIR_RUN_OK;
}
static bool math_unary(uint32_t operation,double input,double *output) {
    switch (operation) {
#define XR_XIR_MATH_UNARY(name,id,source,function) case id:*output=function(input);return true;
#define XR_XIR_MATH_BINARY(name,id,source,function)
#define XR_XIR_MATH_INTEGER(name,id,source,function)
#define XR_XIR_MATH_ENTROPY(name,id,source,result)
#include "xxir_math.def"
#undef XR_XIR_MATH_UNARY
#undef XR_XIR_MATH_BINARY
#undef XR_XIR_MATH_INTEGER
#undef XR_XIR_MATH_ENTROPY
    default:return false;
    }
}
static bool math_binary(uint32_t operation,double left,double right,double *output) {
    switch (operation) {
#define XR_XIR_MATH_UNARY(name,id,source,function)
#define XR_XIR_MATH_BINARY(name,id,source,function) case id:*output=function(left,right);return true;
#define XR_XIR_MATH_INTEGER(name,id,source,function)
#define XR_XIR_MATH_ENTROPY(name,id,source,result)
#include "xxir_math.def"
#undef XR_XIR_MATH_UNARY
#undef XR_XIR_MATH_BINARY
#undef XR_XIR_MATH_INTEGER
#undef XR_XIR_MATH_ENTROPY
    default:return false;
    }
}
static bool math_integral(uint32_t operation,double input,double *output) {
    switch (operation) {
#define XR_XIR_MATH_UNARY(name,id,source,function)
#define XR_XIR_MATH_BINARY(name,id,source,function)
#define XR_XIR_MATH_INTEGER(name,id,source,function) case id:*output=function(input);return true;
#define XR_XIR_MATH_ENTROPY(name,id,source,result)
#include "xxir_math.def"
#undef XR_XIR_MATH_UNARY
#undef XR_XIR_MATH_BINARY
#undef XR_XIR_MATH_INTEGER
#undef XR_XIR_MATH_ENTROPY
    default:return false;
    }
}
static XrXirRunStatus math_host_value(uint32_t operation,const XrXirMathSignature *signature,
    double first,double second,double *output) {
    int saved_errno=errno;fenv_t ambient;
    if (feholdexcept(&ambient)) {errno=saved_errno;return XR_XIR_RUN_HOST_ERROR;}
    XrXirRunStatus status=XR_XIR_RUN_HOST_ERROR;
    if (!fesetenv(FE_DFL_ENV) && !fesetround(FE_TONEAREST)) {
        bool found=signature->result==XR_XIR_I64 ? math_integral(operation,first,output) :
            signature->arity==1 ? math_unary(operation,first,output) : math_binary(operation,first,second,output);
        status=found ? XR_XIR_RUN_OK : XR_XIR_RUN_BAD_ARGUMENT;
    }
    /* Host rounding and exception masks cannot alter a language value or leak
     * through a compiler evaluation. Every exit restores the caller's state. */
    if (fesetenv(&ambient)) status=XR_XIR_RUN_HOST_ERROR;
    errno=saved_errno;return status;
}
XR_FUNC XrXirRunStatus xr_xir_math_evaluate(uint32_t operation,int64_t left,int64_t right,int64_t *output) {
    XrXirMathSignature signature;
    if (!output || !xr_xir_math_signature(operation,&signature) || signature.random ||
        (signature.arity==1 && right)) return XR_XIR_RUN_BAD_ARGUMENT;
    double first=0,second=0,value=0;
    XrXirRunStatus status=math_float_input(left,&first);
    if (status!=XR_XIR_RUN_OK) return status;
    if (signature.arity==2) {
        status=math_float_input(right,&second);
        if (status!=XR_XIR_RUN_OK) return status;
    }
    status=math_host_value(operation,&signature,first,second,&value);
    if (status!=XR_XIR_RUN_OK) return status;
    int64_t result=0;
    if (signature.result==XR_XIR_I64) {
        uint64_t bits=0;memcpy(&bits,&value,sizeof(bits));
        status=math_numeric_status(xr_xir_float_to_integer(64,xr_xir_integer_format(XR_XIR_I64),bits,&result));
    } else {
        status=math_float_result(value,&result);
    }
    if (status==XR_XIR_RUN_OK) *output=result;
    return status;
}

typedef struct MathEntropyRun { XrXirMathEntropy source;XrXirRunStatus status; } MathEntropyRun;
static void math_entropy_fill(void *context,unsigned char *bytes,size_t length) {
    MathEntropyRun *run=context;
    if (run->status==XR_XIR_RUN_OK) {
        XrXirRunStatus status=run->source.fill(run->source.context,bytes,length);
        run->status=(unsigned)status<=XR_XIR_RUN_UNSUPPORTED ? status : XR_XIR_RUN_BAD_ARGUMENT;
    }
    /* UINT64_MAX always passes the shared rejection threshold. Preserve the
     * failure status and discard this temporary value at the owned boundary. */
    if (run->status!=XR_XIR_RUN_OK) memset(bytes,0xff,length);
}
static XrXirRunStatus math_random_value(uint32_t operation,MathEntropyRun *run,
    int64_t minimum,int64_t maximum,int64_t *output) {
    if (operation==XR_XIR_MATH_RANDOM) {
        double value=xr_math_core_random_f64(math_entropy_fill,run);
        if (run->status!=XR_XIR_RUN_OK) return run->status;
        return math_float_result(value,output);
    }
    int64_t value=xr_math_core_random_i64(math_entropy_fill,run,minimum,maximum);
    if (run->status!=XR_XIR_RUN_OK) return run->status;
    *output=value;return XR_XIR_RUN_OK;
}
XR_FUNC XrXirRunStatus xr_xir_math_random(uint32_t operation,XrXirMathEntropy entropy,
    int64_t minimum,int64_t maximum,int64_t *output) {
    XrXirMathSignature signature;
    if (!output || !entropy.fill || !xr_xir_math_signature(operation,&signature) || !signature.random ||
        (!signature.arity && (minimum || maximum))) return XR_XIR_RUN_BAD_ARGUMENT;
    MathEntropyRun run={entropy,XR_XIR_RUN_OK};int64_t result=0;
    XrXirRunStatus status=math_random_value(operation,&run,minimum,maximum,&result);
    if (status==XR_XIR_RUN_OK) *output=result;
    return status;
}
