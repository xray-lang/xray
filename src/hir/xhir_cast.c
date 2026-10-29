/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhir_cast.c - Concrete numeric and scalar text conversion qualification
 */
#include "xhir_cast.h"
#include "../xir/xxir_types.h"
#include "../xir/xxir_compile_memory.h"
#include "../base/xchecks.h"
static XrXirStatus cast_failure(const char **reason,XrXirStatus status,const char *message) {
    if(reason)*reason=message;return status;
}
static XrXirStatus cast_poll(XrCompileRequestControl *control,const char **reason) {
    XrXirStatus status=control?xir_compile_resource_status(xr_compile_request_control_poll(control)):XR_XIR_OK;
    return status==XR_XIR_OK?status:cast_failure(reason,status,"source request interrupted");
}
static bool cast_target_valid(const XrHirCastTarget *target) {
    return target->kind==XR_HIR_CAST_NUMBER ? target->role==XR_HIR_CAST_EXPLICIT && xr_xir_type_is_number(target->type) :
        target->kind==XR_HIR_CAST_TO_TEXT && target->type==XR_XIR_STRING &&
        target->role>=XR_HIR_CAST_EXPLICIT && target->role<=XR_HIR_CAST_INTERPOLATION;
}
XR_FUNC XrXirStatus xr_hir_cast_target_check(const XrXirTypes *types,const XrHirCastTargetInput *input,
    XrCompileRequestControl *control,XrHirCastTarget *output,const char **reason) {
    if(reason)*reason=NULL;
    if(!types || !input || !output)return cast_failure(reason,XR_XIR_BAD_STRUCTURE,"cast target input is missing");
    XrXirStatus status=cast_poll(control,reason);if(status!=XR_XIR_OK)return status;
    if(input->role<XR_HIR_CAST_EXPLICIT || input->role>XR_HIR_CAST_INTERPOLATION)
        return cast_failure(reason,XR_XIR_BAD_STRUCTURE,"unknown scalar conversion role");
    if(input->role==XR_HIR_CAST_EXPLICIT) {
        if(input->safe || (input->target!=XR_XIR_STRING && !xr_xir_type_is_number(input->target)))
            return cast_failure(reason,XR_XIR_BAD_TYPE,"numeric cast requires a concrete nonnullable numeric target");
    } else if(input->safe || input->target!=XR_XIR_STRING)
        return cast_failure(reason,XR_XIR_BAD_STRUCTURE,"text conversion target must be string");
    *output=(XrHirCastTarget){input->origin,input->role,input->target==XR_XIR_STRING?XR_HIR_CAST_TO_TEXT:XR_HIR_CAST_NUMBER,input->target};
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_cast_check(const XrXirTypes *types,const XrHirCastTarget *target,XrXirType operand,
    XrCompileRequestControl *control,XrHirCast *output,const char **reason) {
    if(reason)*reason=NULL;
    if(!types || !target || !output)return cast_failure(reason,XR_XIR_BAD_STRUCTURE,"cast operand input is missing");
    XrXirStatus status=cast_poll(control,reason);if(status!=XR_XIR_OK)return status;
    if(!cast_target_valid(target))return cast_failure(reason,XR_XIR_BAD_STRUCTURE,"cast target qualification is invalid");
    XrHirCast node={target->origin,target->kind,operand,target->type};
    if(target->kind==XR_HIR_CAST_NUMBER) {
        if(!xr_xir_type_is_number(operand))
            return cast_failure(reason,XR_XIR_BAD_TYPE,"numeric cast requires a concrete numeric operand");
    } else if(operand==XR_XIR_STRING)node.kind=XR_HIR_CAST_IDENTITY;
    else if(operand!=XR_XIR_BOOL && operand!=XR_XIR_RUNE && !xr_xir_type_is_number(operand))
        return cast_failure(reason,XR_XIR_BAD_TYPE,target->role==XR_HIR_CAST_INTERPOLATION?
            "interpolated value has no admitted string conversion":"value has no admitted string conversion");
    *output=node;return XR_XIR_OK;
}
XR_FUNC XrHirCastLiteralContext xr_hir_cast_literal_context(const XrHirCastTarget *target,bool present,bool negative,uint64_t magnitude) {
    XR_CHECK(target && target->kind==XR_HIR_CAST_NUMBER && cast_target_valid(target),"numeric cast target must be qualified before literal context");
    return (XrHirCastLiteralContext){present && !negative && magnitude>INT64_MAX,XR_XIR_U64};
}
XR_FUNC XrXirInstruction xr_hir_cast_instruction(const XrHirCast *node,uint32_t operand) {
    XR_CHECK(node && (node->kind==XR_HIR_CAST_NUMBER || node->kind==XR_HIR_CAST_TO_TEXT),"only nonidentity checked casts produce an instruction");
    return (XrXirInstruction){node->kind==XR_HIR_CAST_NUMBER?XR_XIR_CONVERT_NUMBER:XR_XIR_TO_STRING,node->target,{operand,0},{0},0,{0}};
}
