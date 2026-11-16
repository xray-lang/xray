/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Nested declaration names follow owned bindings, never propagated Fn values. */
static XrXirStatus body_declared_function_valid(const XrHirBody *body,uint32_t function,XrXirType type,
    XrCompileRequestControl *control) {
    if(!body->declarations.functions||!body->declarations.identities||
        !*body->declarations.functions||!*body->declarations.identities||
        !body->declarations.function_count||function>=*body->declarations.function_count)return XR_XIR_BAD_STRUCTURE;
    XrXirFunction declaration=(*body->declarations.functions)[function];
    XrXirFunctionIdentity identity=(*body->declarations.identities)[function];
    const XrXirTypeNode *callable=xr_xir_callable_signature(body->types,type);
    if(!callable||identity.capture_count>declaration.parameter_count||
        callable->parameter_count!=declaration.parameter_count-identity.capture_count||
        callable->result!=declaration.result||callable->element!=declaration.error_type)return XR_XIR_BAD_STRUCTURE;
    for(uint32_t p=0;p<callable->parameter_count;++p) {
        if(!(p%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
        uint32_t physical=p+identity.capture_count;
        if(callable->parameters[p].type!=declaration.parameters[physical]||
            !!callable->parameters[p].escaping!=xr_xir_function_parameter_escaping(&declaration,physical)||
            callable->parameters[p].mode!=xr_xir_declared_parameter_mode(body->types,
                declaration.parameters[physical],declaration.consume_parameters,
                declaration.consume_parameter_count,physical))return XR_XIR_BAD_STRUCTURE;
    }
    return body_poll(control);
}
XR_FUNC XrXirStatus xr_hir_body_function_reference(XrHirBody *body,const XrHirFunctionReferenceInput *input,
    XrCompileRequestControl *control,XrHirBodyUse *output) {
    if(!body||body->ended||!input||input->types!=body->types||!output||input->scope.owner!=body||
        input->scope.index>=body->scopes.count||!body->declarations.items)return XR_XIR_BAD_STRUCTURE;
    HirBodyScope scope=((HirBodyScope *)body->scopes.items)[input->scope.index];uint32_t item=0;
    if(scope.origin.function!=input->origin.function||scope.origin.module!=input->origin.module||
        !xr_hir_items_find_scope(*body->declarations.items,input->origin.module,input->origin.node,&item))
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=body_declared_function_valid(body,input->function,input->callable,control);
    if(status!=XR_XIR_OK)return status;
    XrHirBodyInput checked={.origin=input->origin,.rule=XR_HIR_BODY_IDENTITY,
        .source=input->callable,.target=input->callable};
    status=xr_hir_body_check(body,body->types,&checked,control,output,NULL);
    if(status!=XR_XIR_OK)return status;
    HirBodyUse *use=&body->uses[output->index];use->action.operation=XR_XIR_FUNCTION_REF;
    use->required_instruction=true;use->has_immediate=true;use->expected_immediate=input->function;
    use->has_source=true;use->source_value=input->value;use->capture_function=input->function+1;
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_binding_function(XrHirBody *body,XrHirBodyBinding binding,
    XrHirBodyUse initialization,uint32_t function,XrCompileRequestControl *control) {
    if(!body||body->ended||binding.owner!=body||binding.index>=body->binding_count||
        initialization.owner!=body||initialization.index>=body->count)return XR_XIR_BAD_STRUCTURE;
    HirBodyBinding row=body->bindings[binding.index];HirBodyUse use=body->uses[initialization.index];
    if(!row.materialized||row.declared_function||row.declaration.rebindable||
        row.declaration.storage!=XR_HIR_BODY_LOCAL||use.binding.index!=binding.index||
        use.binding.owner!=body||use.binding_kind!=XR_HIR_BINDING_INITIALIZE||
        !use.initializes||!use.consumed||use.projected||
        !body->declarations.items||!xr_hir_items_named_function(*body->declarations.items,row.declaration.key.module,row.declaration.key.node))return XR_XIR_BAD_STRUCTURE;
    XrXirStatus valid=body_declared_function_valid(body,function,row.declaration.logical,control);
    if(valid!=XR_XIR_OK)return valid;
    XrXirFunctionIdentity identity=(*body->declarations.identities)[function];
    bool self=function==row.declaration.origin.function;
    if(!self&&row.declaration.origin.function<*body->declarations.function_count) {
        XrXirFunctionIdentity origin=(*body->declarations.identities)[row.declaration.origin.function];
        self=(origin.promises&XR_XIR_FUNCTION_GENERATOR)&&origin.capture_owner==identity.capture_owner&&
            origin.capture_count==identity.capture_count;
    }
    if(identity.module!=row.declaration.origin.module||
        (identity.capture_owner!=row.declaration.origin.function+1&&!self))return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;
    body->bindings[binding.index].declared_function=function+1;
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_binding_capture_function(XrHirBody *body,XrHirBodyBinding binding,
    XrHirBodyBinding source,XrHirBodyUse capture,XrCompileRequestControl *control) {
    if(!body||body->ended||binding.owner!=body||source.owner!=body||capture.owner!=body||
        binding.index>=body->binding_count||source.index>=body->binding_count||capture.index>=body->count)
        return XR_XIR_BAD_STRUCTURE;
    HirBodyBinding row=body->bindings[binding.index],original=body->bindings[source.index];
    HirBodyUse use=body->uses[capture.index];
    if(!row.materialized||row.capture_source.owner||
        row.declaration.rebindable!=original.declaration.rebindable||!original.materialized||
        use.binding.owner!=body||use.binding.index!=binding.index||use.binding_kind!=XR_HIR_BINDING_CAPTURE||
        !use.initializes||!use.consumed||use.projected||row.declaration.logical!=original.declaration.logical||
        row.declaration.physical!=original.declaration.physical||
        row.declaration.key.declaration!=original.declaration.key.declaration||
        row.declaration.key.module!=original.declaration.key.module||strcmp(row.name,original.name)||
        !body->declarations.function_count||row.declaration.origin.function>=*body->declarations.function_count)
        return XR_XIR_BAD_STRUCTURE;
    XrXirFunctionIdentity receiver=(*body->declarations.identities)[row.declaration.origin.function];
    if(receiver.capture_owner!=original.declaration.origin.function+1||row.location>=receiver.capture_count)
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;
    body->bindings[binding.index].declared_function=original.declared_function;
    body->bindings[binding.index].capture_source=source;
    return XR_XIR_OK;
}
XR_FUNC bool xr_hir_body_binding_function_info(const XrHirBody *body,XrHirBodyBinding binding,
    uint32_t *function,uint32_t *captures) {
    if(!body||body->ended||binding.owner!=body||binding.index>=body->binding_count||!function||!captures)
        return false;
    HirBodyBinding row=body->bindings[binding.index];
    if(!row.materialized||!row.declared_function)return false;
    *function=row.declared_function-1;
    *captures=(*body->declarations.identities)[*function].capture_count;
    return true;
}

static XrXirStatus body_declared_capture_projection(const XrHirBody *body,const HirBodyUse *use,
    const XrHirBodyProjection *projection,XrCompileRequestControl *control) {
    uint32_t function=use->capture_function?use->capture_function-1:body->bindings[use->binding.index].declared_function-1;
    uint32_t node=use->capture_function?use->origin.node:body->bindings[use->binding.index].declaration.key.node;
    uint32_t module=use->origin.module;
    XrXirFunctionIdentity target=(*body->declarations.identities)[function];
    if(projection->source_argument_count!=target.capture_count||
        (target.capture_count&&!projection->source_arguments))return XR_XIR_BAD_STRUCTURE;
    if(!target.capture_count)return body_poll(control);
    for(uint32_t b=0;b<body->binding_count;++b) {
        if(!(b%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
        HirBodyBinding parameter=body->bindings[b];
        if(!parameter.capture_source.owner||parameter.lexical_role!=XIR_LEXICAL_CAPTURE||
            parameter.declaration.key.node!=node||
            parameter.declaration.key.module!=module)continue;
        uint32_t receiver=parameter.declaration.origin.function;
        XrXirFunctionIdentity owner=(*body->declarations.identities)[receiver];
        if(owner.capture_owner!=target.capture_owner||owner.capture_count!=target.capture_count)continue;
        if(parameter.capture_source.owner!=body||parameter.capture_source.index>=body->binding_count||
            parameter.location>=target.capture_count)return XR_XIR_BAD_STRUCTURE;
        HirBodyBinding source=body->bindings[parameter.capture_source.index];
        uint32_t expected=UINT32_MAX;
        if(projection->function==receiver)expected=parameter.location;
        else if(projection->function==source.declaration.origin.function)expected=source.location;
        if(expected==UINT32_MAX||projection->source_arguments[parameter.location]!=expected)return XR_XIR_BAD_STRUCTURE;
    }
    return body_poll(control);
}
