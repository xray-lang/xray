/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhir_boolean.c - Concrete Bool operands for logical syntax and conditions
 */
#include "xhir_boolean.h"
#include "../xir/xxir_compile_memory.h"
#include "../base/xchecks.h"

static XrXirStatus boolean_failure(const char **reason,XrXirStatus status,const char *message) {
    if(reason)*reason=message;return status;
}
XR_FUNC XrXirStatus xr_hir_boolean_check(const XrXirTypes *types,const XrHirBooleanInput *input,
    XrCompileRequestControl *control,XrHirBoolean *output,const char **reason) {
    if(reason)*reason=NULL;
    if(!types || !input || !output)
        return boolean_failure(reason,XR_XIR_BAD_STRUCTURE,"boolean operator input is missing");
    if(control) {
        XrXirStatus status=xir_compile_resource_status(xr_compile_request_control_poll(control));
        if(status!=XR_XIR_OK)return boolean_failure(reason,status,"source request interrupted");
    }
    const char *message;
    switch(input->operation) {
    case XR_HIR_BOOLEAN_NOT:case XR_HIR_BOOLEAN_AND:case XR_HIR_BOOLEAN_OR:message="logical operand must be bool";break;
    case XR_HIR_BOOLEAN_CONDITIONAL:message="conditional requires bool";break;
    case XR_HIR_BOOLEAN_IF:message="if requires bool";break;
    case XR_HIR_BOOLEAN_LOOP:message="loop condition requires bool";break;
    default:return boolean_failure(reason,XR_XIR_BAD_STRUCTURE,"unknown boolean operator");
    }
    if(input->operand!=XR_XIR_BOOL)return boolean_failure(reason,XR_XIR_BAD_TYPE,message);
    *output=(XrHirBoolean){input->origin,input->operation,XR_XIR_BOOL};return XR_XIR_OK;
}
XR_FUNC XrXirType xr_hir_boolean_result_type(XrHirBooleanOperation operation) {
    XR_CHECK(operation==XR_HIR_BOOLEAN_NOT || operation==XR_HIR_BOOLEAN_AND || operation==XR_HIR_BOOLEAN_OR,
        "only logical syntax has a declared Bool expression result");
    return XR_XIR_BOOL;
}
