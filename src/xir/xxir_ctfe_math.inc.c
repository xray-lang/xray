/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_math.inc.c - Pure math in the current owned compiler child
 *
 * KEY CONCEPT:
 *   Scalar results use the runtime evaluator; system entropy never enters CTFE.
 */
#include "xxir_math_eval_internal.h"
static XrXirCtfeStatus ctfe_math_native_step(CtfeCallExecution *execution,
    XirScalarCoreAction *action,uint32_t id) {
    CtfeCallFrame *frame=execution->top;
    if(xir_math_native_random(id))return XR_XIR_CTFE_NOT_SUPPORTED;
    XrXirCtfeStatus status=ctfe_control_poll(execution->owned.control);
    if(status!=XR_XIR_CTFE_OK)return status;
    uint32_t count=frame->run.function->parameter_count;
    if(count>2)return XR_XIR_CTFE_BAD_ARTIFACT;
    XrXirValue arguments[2]={{0}},result={0};
    for(uint32_t p=0;p<count;++p)arguments[p]=ctfe_nominal_operand(&frame->run,p);
    XrXirRunStatus evaluated=xir_math_native_evaluate(id,count?arguments:NULL,count,&result);
    if(evaluated!=XR_XIR_RUN_OK) {
        if(evaluated!=XR_XIR_RUN_NUMERIC_RANGE || frame->exit_kind || frame->state.frontier ||
            !xr_xir_panic_empty(&frame->panic))return XR_XIR_CTFE_BAD_ARTIFACT;
        XrXirPanicPayload panic={.detail={XR_XIR_PANIC_RANGE,0,0,0}};
        status=ctfe_value_status(xr_xir_panic_copy(&panic,&frame->panic));
        if(status!=XR_XIR_CTFE_OK)return status;
        frame->exit_kind=CTFE_EXIT_PANIC;frame->state.leaving=true;frame->state.exit_target=0;
        action->kind=XIR_SCALAR_CONTINUE;return XR_XIR_CTFE_OK;
    }
    status=ctfe_control_poll(execution->owned.control);
    if(status!=XR_XIR_CTFE_OK)return status;
    frame->returned=result;action->kind=XIR_SCALAR_RETURN;action->value=frame->returned;
    return XR_XIR_CTFE_OK;
}
