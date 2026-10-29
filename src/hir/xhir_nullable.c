/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhir_nullable.c - Nullable expression qualification before Built emission
 */
#include "xhir_nullable.h"
#include "../xir/xxir_types.h"
#include "../xir/xxir_compile_memory.h"
#include "../base/xchecks.h"

static XrXirStatus nullable_failure(const char **reason,XrXirStatus status,const char *message) {
    if(reason)*reason=message;return status;
}
static XrXirStatus nullable_poll(XrCompileRequestControl *control,const char **reason) {
    XrXirStatus status=control?xir_compile_resource_status(xr_compile_request_control_poll(control)):XR_XIR_OK;
    return status==XR_XIR_OK?status:nullable_failure(reason,status,"source request interrupted");
}
static XrXirStatus nullable_default(const XrHirNullableContext *context,const XrHirNullableInput *input,
    XrHirNullablePlan *plan,const char **reason) {
    XrXirType base=input->role==XR_HIR_NULLABLE_DEAD_DEFAULT?input->default_base:input->source;
    plan->kind=XR_HIR_NULLABLE_IDENTITY;plan->has_default=true;plan->default_type=base;
    if(!input->null_default || xr_xir_type_is_nullable(context->types,base))return XR_XIR_OK;
    if(xr_xir_type_is_cell(context->types,base))
        return nullable_failure(reason,XR_XIR_BAD_TYPE,"nullable element must be an ordinary value type");
    if(!context->intern)return nullable_failure(reason,XR_XIR_BAD_STRUCTURE,"nullable type interner is missing");
    XrXirTypeNode node={.kind=XR_XIR_TYPE_NULLABLE,.element=base};
    XrXirStatus status=context->intern(context->intern_owner,&node,&plan->default_type);
    return status==XR_XIR_OK?status:nullable_failure(reason,status,"nullable default type could not be interned");
}
XR_FUNC XrXirStatus xr_hir_nullable_prepare(const XrHirNullableContext *context,const XrHirNullableInput *input,
    XrHirNullablePlan *output,const char **reason) {
    if(reason)*reason=NULL;
    if(!context || !context->types || !input || !output)
        return nullable_failure(reason,XR_XIR_BAD_STRUCTURE,"nullable expression input is missing");
    XrXirStatus status=nullable_poll(context->control,reason);if(status!=XR_XIR_OK)return status;
    XrHirNullablePlan plan={.origin=input->origin,.role=input->role,.source=input->source,.result=input->source};
    switch(input->role) {
    case XR_HIR_NULLABLE_NULL:
        if(!input->has_expected || !xr_xir_type_is_nullable(context->types,input->expected))
            return nullable_failure(reason,XR_XIR_BAD_TYPE,"null requires an exact nullable context");
        plan.kind=XR_HIR_NULLABLE_NONE;plan.result=input->expected;break;
    case XR_HIR_NULLABLE_EQUAL_NULL:case XR_HIR_NULLABLE_UNEQUAL_NULL:
        if(!xr_xir_type_is_nullable(context->types,input->source))
            return nullable_failure(reason,XR_XIR_BAD_TYPE,"comparison with null requires a nullable operand");
        plan.kind=XR_HIR_NULLABLE_PRESENCE;plan.result=XR_XIR_BOOL;
        plan.immediate=input->role==XR_HIR_NULLABLE_EQUAL_NULL;break;
    case XR_HIR_NULLABLE_FORCE:
        if(!xr_xir_type_is_nullable(context->types,input->source)) {plan.kind=XR_HIR_NULLABLE_IDENTITY;break;}
        /* A nullable force operation has the same outer-layer result as unwrap. */
        plan.kind=XR_HIR_NULLABLE_ELEMENT;plan.result=xr_xir_nullable_element(context->types,input->source);break;
    case XR_HIR_NULLABLE_UNWRAP:
        if(!xr_xir_type_is_nullable(context->types,input->source))
            return nullable_failure(reason,XR_XIR_BAD_TYPE,"unwrap requires a nullable operand");
        plan.kind=XR_HIR_NULLABLE_ELEMENT;plan.result=xr_xir_nullable_element(context->types,input->source);break;
    case XR_HIR_NULLABLE_COALESCE:
        if(xr_xir_type_is_nullable(context->types,input->source)) {
            XrXirType element=xr_xir_nullable_element(context->types,input->source);
            plan.kind=XR_HIR_NULLABLE_BRANCH;plan.has_default=true;plan.result=element;
            if((input->has_expected && input->expected==input->source) ||
                (input->null_default && !xr_xir_type_is_nullable(context->types,element)))plan.result=input->source;
            plan.default_type=plan.result;plan.unwrap_present=plan.result==element;break;
        }
        status=nullable_default(context,input,&plan,reason);break;
    case XR_HIR_NULLABLE_DEAD_DEFAULT:status=nullable_default(context,input,&plan,reason);break;
    default:return nullable_failure(reason,XR_XIR_BAD_STRUCTURE,"unknown nullable expression role");
    }
    if(status!=XR_XIR_OK)return status;
    status=nullable_poll(context->control,reason);if(status==XR_XIR_OK)*output=plan;return status;
}
XR_FUNC XrXirStatus xr_hir_nullable_check(const XrHirNullablePlan *plan,XrXirType actual_default,
    XrCompileRequestControl *control,XrHirNullable *output,const char **reason) {
    if(reason)*reason=NULL;
    if(!plan || !output)return nullable_failure(reason,XR_XIR_BAD_STRUCTURE,"nullable expression plan is missing");
    XrXirStatus status=nullable_poll(control,reason);if(status!=XR_XIR_OK)return status;
    if(plan->has_default && actual_default!=plan->default_type)
        return nullable_failure(reason,XR_XIR_BAD_TYPE,"?? default must match the unwrapped element or its nullable context");
    *output=(XrHirNullable){plan->origin,plan->kind,plan->source,plan->result,plan->default_type,
        plan->unwrap_present,plan->immediate};return XR_XIR_OK;
}
XR_FUNC XrXirInstruction xr_hir_nullable_instruction(const XrHirNullable *node,uint32_t operand) {
    XR_CHECK(node && (node->kind==XR_HIR_NULLABLE_NONE || node->kind==XR_HIR_NULLABLE_PRESENCE ||
        node->kind==XR_HIR_NULLABLE_ELEMENT),"only checked nullable value operations have a Built instruction");
    XrXirOp operation=node->kind==XR_HIR_NULLABLE_NONE?XR_XIR_NULLABLE_NONE:
        node->kind==XR_HIR_NULLABLE_PRESENCE?XR_XIR_NULLABLE_IS_SOME:XR_XIR_NULLABLE_UNWRAP;
    return (XrXirInstruction){operation,node->result,{node->kind==XR_HIR_NULLABLE_NONE?0:operand,0},{0},node->immediate,{0}};
}
XR_FUNC XrXirType xr_hir_nullable_force_hint(const XrXirTypes *types,XrXirType source) {
    return xr_xir_type_is_nullable(types,source)?xr_xir_nullable_element(types,source):source;
}
