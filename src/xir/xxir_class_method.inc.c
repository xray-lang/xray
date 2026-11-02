/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_class_method.inc.c - Module-owned virtual slot target observations
 *
 * KEY CONCEPT:
 *   The actual receiver chooses a target only inside a verified declaration chain.
 */
static uint32_t class_method_parent(const XrXirNominalTable *table,uint32_t owner) {
    return table->declarations?table->declarations[owner].parent:table->identities[owner].parent;
}
static bool class_method_name(const XrXirFunction *a,const XrXirFunction *b) {
    return a->name && b->name && a->name_length==b->name_length &&
        !memcmp(a->name,b->name,a->name_length);
}
static XrXirStatus class_method_entry(const XrXirModule *module,uint32_t entry) {
    if(!module || !module->functions || !module->declarations || !module->declarations->functions ||
        !module->types || !module->types->nominals || entry>=module->function_count)return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *function=&module->functions[entry];
    const XrXirFunctionIdentity *identity=&module->declarations->functions[entry];
    const XrXirNominalTable *table=module->types->nominals;
    if(!identity->nominal_owner || identity->nominal_owner>table->count ||
        identity->method_kind!=XR_XIR_READ_METHOD || identity->member_access!=XR_XIR_MEMBER_PUBLIC ||
        identity->cleanup_owner || identity->native_callable || identity->test_role ||
        (identity->promises&(XR_XIR_FUNCTION_GENERATOR|XR_XIR_FUNCTION_REST)) ||
        !function->parameter_count || !function->parameters ||
        (function->consume_parameter_count && (!function->consume_parameters || !function->consume_parameters[0])))
        return XR_XIR_BAD_TYPE;
    const XrXirTypeNode *receiver=xr_xir_type_node(module->types,function->parameters[0]);
    uint32_t d=identity->nominal_owner-1;
    uint32_t kind=table->declarations?table->declarations[d].kind:table->identities[d].kind;
    uint32_t arity=table->declarations?table->declarations[d].parameter_count:table->identities[d].arity;
    uint32_t native=table->declarations?table->declarations[d].native.native_id:table->identities[d].native.native_id;
    if(kind!=XR_XIR_NOMINAL_CLASS || arity || native || !receiver || receiver->kind!=XR_XIR_TYPE_NOMINAL ||
        receiver->nominal.declaration!=d || receiver->nominal.argument_count || receiver->parameter_span ||
        (module->generics && (module->generics[entry].parameter_count || module->generics[entry].argument_count)))
        return XR_XIR_BAD_TYPE;
    return XR_XIR_OK;
}
static XrXirStatus class_method_override(const XrXirModule *module,uint32_t derived,uint32_t base) {
    XrXirStatus status=class_method_entry(module,derived);
    if(status!=XR_XIR_OK)return status;
    status=class_method_entry(module,base);if(status!=XR_XIR_OK)return status;
    const XrXirFunction *a=&module->functions[derived],*b=&module->functions[base];
    const XrXirFunctionIdentity *identity=module->declarations->functions;
    if(!class_method_name(a,b) || a->parameter_count!=b->parameter_count || a->result!=b->result ||
        a->consume_parameter_count!=b->consume_parameter_count ||
        !xr_xir_class_is_subtype(module->types,a->parameters[0],b->parameters[0]) ||
        !xr_xir_error_bound_accepts(module->types,a->error_type,b->error_type) ||
        ((identity[base].promises&XR_XIR_FUNCTION_NO_SUSPEND) &&
            !(identity[derived].promises&XR_XIR_FUNCTION_NO_SUSPEND)))return XR_XIR_BAD_TYPE;
    for(uint32_t p=1;p<a->parameter_count;++p)
        if(a->parameters[p]!=b->parameters[p])return XR_XIR_BAD_TYPE;
    for(uint32_t p=0;p<a->consume_parameter_count;++p)
        if(a->consume_parameters[p]!=b->consume_parameters[p])return XR_XIR_BAD_TYPE;
    return XR_XIR_OK;
}
static XrXirStatus class_method_selected(const XrXirModule *module,XrXirType actual,
    uint32_t slot,XirClassMethodTarget *output,XrCompileRequestControl *control) {
    XrXirStatus status=class_method_entry(module,slot);if(status!=XR_XIR_OK)return status;
    const XrXirFunction *anchor=&module->functions[slot];
    if(!xr_xir_class_is_subtype(module->types,actual,anchor->parameters[0]))return XR_XIR_BAD_TYPE;
    uint32_t owner=xr_xir_type_node(module->types,actual)->nominal.declaration;
    const XrXirNominalTable *table=module->types->nominals;
    const XrXirFunctionIdentity *identities=module->declarations->functions;
    uint32_t selected=UINT32_MAX,steps=0;
    for(uint32_t depth=0;depth<table->count;++depth) {
        uint32_t found=UINT32_MAX;
        for(uint32_t f=0;f<module->function_count;++f) {
            status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
            if(identities[f].nominal_owner!=owner+1 ||
                !class_method_name(&module->functions[f],anchor))continue;
            if(found!=UINT32_MAX)return XR_XIR_BAD_STRUCTURE;
            status=class_method_override(module,f,slot);if(status!=XR_XIR_OK)return status;
            if(f!=slot && !(identities[f].promises&XR_XIR_FUNCTION_OVERRIDE))return XR_XIR_BAD_TYPE;
            found=f;
        }
        if(found!=UINT32_MAX && selected==UINT32_MAX)selected=found;
        if(owner+1==identities[slot].nominal_owner) {
            if(found!=slot || selected==UINT32_MAX)return XR_XIR_BAD_STRUCTURE;
            *output=(XirClassMethodTarget){selected,module->functions[selected].parameters[0]};return XR_XIR_OK;
        }
        uint32_t parent=class_method_parent(table,owner);
        if(!parent || parent>table->count)return XR_XIR_BAD_TYPE;
        owner=parent-1;
    }
    return XR_XIR_BAD_STRUCTURE;
}
XR_FUNC XrXirStatus xir_compile_class_method_candidates(const XrXirModule *module,
    XrXirType static_receiver,uint32_t slot,XirClassMethodVisitor visit,
    void *owner,XrCompileRequestControl *control) {
    if(!visit)return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=xir_receive_poll(control);if(status!=XR_XIR_OK)return status;
    status=class_method_entry(module,slot);if(status!=XR_XIR_OK)return status;
    if(!xr_xir_class_is_subtype(module->types,static_receiver,module->functions[slot].parameters[0]))return XR_XIR_BAD_TYPE;
    uint32_t steps=0;bool found=false;
    for(uint32_t t=0;t<module->types->count;++t) {
        status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
        XrXirType type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+t);
        if(!xr_xir_class_is_subtype(module->types,type,static_receiver))continue;
        XirClassMethodTarget target={0};
        status=class_method_selected(module,type,slot,&target,control);
        if(status==XR_XIR_OK)status=visit(owner,&target);
        if(status!=XR_XIR_OK)return status;
        found=true;
    }
    return found?XR_XIR_OK:XR_XIR_BAD_TYPE;
}
XR_FUNC XrXirStatus xir_compile_class_method_site(const XrXirModule *module,
    uint32_t caller,uint32_t instruction,XrXirType actual_receiver,
    XirClassMethodTarget *output,XrCompileRequestControl *control) {
    if(!output)return XR_XIR_BAD_STRUCTURE;
    *output=(XirClassMethodTarget){0};
    XrXirStatus status=xir_receive_poll(control);if(status!=XR_XIR_OK)return status;
    if(!module || !module->functions || caller>=module->function_count)return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *function=&module->functions[caller];
    if(!function->instructions || instruction>=function->instruction_count)return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *op=&function->instructions[instruction];
    if(op->op!=XR_XIR_CLASS_METHOD_REF || op->immediate<0 || (uint64_t)op->immediate>=module->function_count ||
        op->args[1] || op->targets[0] || op->targets[1] || op->type_arguments[0] || op->type_arguments[1] ||
        op->args[0]>=(uint64_t)function->parameter_count+instruction)return XR_XIR_BAD_STRUCTURE;
    uint32_t slot=(uint32_t)op->immediate;
    status=class_method_entry(module,slot);if(status!=XR_XIR_OK)return status;
    const XrXirFunction *anchor=&module->functions[slot];
    XrXirType receiver=xr_xir_operand_type(function,op->args[0]);
    const XrXirTypeNode *signature=xr_xir_callable_signature(module->types,op->type);
    if(!xr_xir_class_is_subtype(module->types,receiver,anchor->parameters[0]) ||
        !xr_xir_class_is_subtype(module->types,actual_receiver,receiver) || !signature ||
        signature->parameter_count!=anchor->parameter_count-1 || signature->result!=anchor->result ||
        signature->element!=anchor->error_type || (signature->flags&XR_XIR_CALLABLE_REST) ||
        ((signature->flags&XR_XIR_CALLABLE_NO_SUSPEND) &&
            !(module->declarations->functions[slot].promises&XR_XIR_FUNCTION_NO_SUSPEND)))return XR_XIR_BAD_TYPE;
    for(uint32_t p=0;p<signature->parameter_count;++p)
        if(!xr_xir_callable_parameter_storage_valid(module->types,&signature->parameters[p]) ||
            signature->parameters[p].type!=anchor->parameters[p+1] ||
            signature->parameters[p].mode!=xr_xir_declared_parameter_mode(module->types,anchor->parameters[p+1],
                anchor->consume_parameters,anchor->consume_parameter_count,p+1))return XR_XIR_BAD_TYPE;
    const XrXirDeclarations *scope=module->declarations;
    const XrXirFunctionIdentity *target=&scope->functions[slot],*from=&scope->functions[caller];
    if(from->module>=scope->module_count || target->module>=scope->module_count ||
        (!xr_xir_module_same_scope(scope,from->module,target->module) && !target->exported) ||
        !xr_xir_module_imports(scope,from->module,target->module))return XR_XIR_BAD_TYPE;
    return class_method_selected(module,actual_receiver,slot,output,control);
}
