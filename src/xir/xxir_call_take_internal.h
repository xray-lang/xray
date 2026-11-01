/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_take_internal.h - Synchronous transfer from real compiler frames
 *
 * KEY CONCEPT:
 *   The trusted VM and generated-C adapters lend their exact compact frame.
 *   Mode facts remain in the actual declaration or callable signature.
 */
#ifndef XXIR_CALL_TAKE_INTERNAL_H
#define XXIR_CALL_TAKE_INTERNAL_H
#include "xxir.h"
#include "xxir_call.h"
#include "xxir_types.h"
typedef struct XirCallTakeArguments {
    const XrXirFunction *caller,*callee;
    const XrXirTypeNode *signature;
    const XrXirFunctionLayout *layout;
    const XrXirValue *arguments;
    void *frame;
    uint32_t instruction,target,count;
} XirCallTakeArguments;
/* All borrowed coordinates expire with this current callback's child prepare. */
XR_FUNC XrXirCallStatus xir_call_arguments_stage(XrXirCallView *view,const XirCallTakeArguments *take);
/* The driver already owns this logical argument independently of its caller. */
XR_FUNC XrXirValueStatus xir_call_owned_argument_take(XrXirCallView *view,uint32_t parameter,XrXirValue *output);
#endif /* XXIR_CALL_TAKE_INTERNAL_H */
