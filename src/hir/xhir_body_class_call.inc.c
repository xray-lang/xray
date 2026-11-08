/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhir_body_class_call.inc.c - Declaration-owned arguments for captured dispatch
 *
 * KEY CONCEPT:
 *   Defaults and access modes belong to the static method declaration. The
 *   physical indirect call omits only the receiver held by its real producer.
 */
static bool body_class_call_bound(const HirBodySignature *call) {
    return call->declaration.family==XR_HIR_CALL_FUNCTION&&call->declaration.receiver_count==1&&
        call->declaration.parameter_offset==1&&call->declaration.callable!=XR_XIR_UNIT;
}
XR_FUNC XrXirStatus xr_hir_body_call_method(XrHirBody *body,XrHirBodySignature signature,
    XrXirType callable,uint32_t callee,XrCompileRequestControl *control) {
    if(!body||body->ended||signature.owner!=body||signature.index>=body->signatures.count||
        callee==UINT32_MAX)return XR_XIR_BAD_STRUCTURE;
    HirBodySignature call=((HirBodySignature *)body->signatures.items)[signature.index];
    if(call.finished||call.declaration.family!=XR_HIR_CALL_FUNCTION||call.declaration.callable!=XR_XIR_UNIT||
        call.declaration.receiver_count!=1||call.declaration.parameter_offset!=1||
        !call.argument_count||!call.parameters||call.argument_count!=call.declaration.count||call.first_argument>=body->count||
        !body->declarations.functions||!*body->declarations.functions||!body->declarations.identities||
        !*body->declarations.identities||!body->declarations.function_count||
        call.declaration.function>=*body->declarations.function_count)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *function=&(*body->declarations.functions)[call.declaration.function];
    const XrXirFunctionIdentity *identity=&(*body->declarations.identities)[call.declaration.function];
    const XrXirTypeNode *type=xr_xir_callable_signature(body->types,callable);
    const XrXirTypeNode *receiver=xr_xir_type_node(body->types,call.parameters[0].type);
    if(identity->method_kind!=XR_XIR_READ_METHOD||identity->member_access||!identity->nominal_owner||
        !receiver||receiver->kind!=XR_XIR_TYPE_NOMINAL||receiver->nominal.declaration!=identity->nominal_owner-1||
        !xr_xir_type_is_class(body->types,call.parameters[0].type)||call.parameters[0].mode!=XR_PARAM_READ||
        !type||(type->flags&XR_XIR_CALLABLE_REST)||type->parameter_count!=call.argument_count-1||
        (type->parameter_count&&!type->parameters)||type->result!=function->result||type->element!=function->error_type||
        !!(type->flags&XR_XIR_CALLABLE_NO_SUSPEND)!=!!(identity->promises&XR_XIR_FUNCTION_NO_SUSPEND))
        return XR_XIR_BAD_TYPE;
    for(uint32_t p=0;p<type->parameter_count;++p) {
        if(!(p%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
        if(type->parameters[p].type!=call.parameters[p+1].type||
            type->parameters[p].mode!=call.parameters[p+1].mode || type->parameters[p].escaping != call.parameters[p+1].escaping)return XR_XIR_BAD_TYPE;
    }
    HirBodyUse first=body->uses[call.first_argument];
    if(first.signature.owner!=body||first.signature.index!=signature.index||first.action.formal||
        first.action.argument!=UINT32_MAX)return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;
    HirBodySignature *owned=&((HirBodySignature *)body->signatures.items)[signature.index];
    owned->declaration.callable=callable;owned->declaration.callee_value=callee;return XR_XIR_OK;
}
static XrXirStatus body_class_call_project(const XrHirBody *body,const HirBodySignature *call,
    const XrHirBodyProjection *projection,XrCompileRequestControl *control) {
    if(!projection->source_value_map||call->declaration.callee_value>=projection->source_value_count||
        call->first_argument>=body->count||!body->declarations.functions||!*body->declarations.functions||
        projection->function>=*body->declarations.function_count)return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *function=&(*body->declarations.functions)[projection->function];
    uint32_t value=projection->source_value_map[call->declaration.callee_value];
    if(value<function->parameter_count||value-function->parameter_count>=function->instruction_count||
        !function->instructions)return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *producer=&function->instructions[value-function->parameter_count];
    uint32_t receiver=body->uses[call->first_argument].source_value;
    if(receiver>=projection->source_value_count||producer->op!=XR_XIR_CLASS_METHOD_REF||
        producer->immediate!=(int64_t)call->declaration.function||producer->type!=call->declaration.callable||
        producer->args[0]!=projection->source_value_map[receiver]||producer->args[1]||
        producer->targets[0]||producer->targets[1]||producer->type_arguments[0]||producer->type_arguments[1])
        return XR_XIR_BAD_STRUCTURE;
    return body_poll(control);
}
