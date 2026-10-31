/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_native_callable.inc.c - Authenticated compiler-native callee execution
 *
 * KEY CONCEPT:
 *   A real compiler child borrows its received view grant. Fresh outcomes belong
 *   to that frame until ordinary return or error delivery copies them onward.
 */
#include "xxir_from_utf8_internal.h"
#include "xxir_unicode_internal.h"

static XrXirCtfeStatus ctfe_native_callable_step(CtfeCallExecution *execution,
    XirScalarCoreAction *action) {
    CtfeCallFrame *frame=execution->top;
    const XrXirModule *module=&execution->artifact->module;
    if (xir_native_callable_verify(module,frame->function)!=XR_XIR_OK ||
        !module->declarations->functions[frame->function].native_callable ||
        frame->cleanup || frame->state.leaving || frame->returned.type ||
        frame->returned.reserved || frame->returned.payload) return XR_XIR_CTFE_BAD_ARTIFACT;
    uint32_t native_id=module->declarations->functions[frame->function].native_callable;
    if (native_id>=XR_NATIVE_CALLABLE_BUFFER_ALLOCATE && native_id<=XR_NATIVE_CALLABLE_BUFFER_LENGTH)
        return XR_XIR_CTFE_NOT_SUPPORTED;
    CtfeOwnedState *state=&execution->owned;
    XrXirCtfeStatus status=ctfe_control_poll(state->control);
    if (status!=XR_XIR_CTFE_OK) return status;
    XrXirValueAdmission admission=ctfe_value_admission(state);
    CtfeViewAccessContext context={execution,frame,&admission,XR_XIR_CTFE_OK};
    if (!ctfe_view_current(&context)) return XR_XIR_CTFE_BAD_ARTIFACT;
    XrXirValue argument=ctfe_nominal_operand(&frame->run,0),result={0};
    XrXirViewAccess access={&admission,ctfe_view_authorize,&context};
    uint32_t id=module->declarations->functions[frame->function].native_callable;
    XrXirValue second=id==XR_NATIVE_CALLABLE_UNICODE_HAS_PROPERTY ? ctfe_nominal_operand(&frame->run,1) : (XrXirValue){0};
    XrXirValueStatus converted=id==XR_NATIVE_CALLABLE_UNICODE_PROPERTY_ID ?
        xir_unicode_property_id(&argument,&result) : id==XR_NATIVE_CALLABLE_UNICODE_HAS_PROPERTY ?
        xir_unicode_has_property(&argument,&second,&result) : xir_from_utf8(&argument,&access,&result);
    bool thrown=converted==XR_XIR_VALUE_BAD_UTF8;
    if (context.failure!=XR_XIR_CTFE_OK) status=context.failure;
    else {
        if (thrown) converted=xr_xir_enum_new(frame->run.function->error_type,0,NULL,0,&admission,&result);
        status=ctfe_value_status(converted);
    }
    if (status==XR_XIR_CTFE_OK) status=ctfe_control_poll(state->control);
    if (status==XR_XIR_CTFE_OK && thrown) {
        status=ctfe_error_begin(execution,&result,CTFE_EXIT_ERROR);
        action->kind=XIR_SCALAR_CONTINUE;
    } else if (status==XR_XIR_CTFE_OK) {
        frame->returned=result;result=(XrXirValue){0};
        action->kind=XIR_SCALAR_RETURN;action->value=frame->returned;
    }
    xr_xir_value_drop(&result);return status;
}
