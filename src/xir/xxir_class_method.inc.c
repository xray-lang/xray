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
static bool class_method_name(const XirClassMethodSignature *a,const XirClassMethodSignature *b) {
    return a->name && b->name && a->name_length==b->name_length && !memcmp(a->name,b->name,a->name_length);
}
static XrXirStatus class_method_header(const XirClassMethodRead *input,uint32_t entry,XirClassMethodSignature *out) {
    if(!input || !input->domain || !input->owner || !input->signature || !out ||
        !input->domain->declarations || !input->domain->declarations->functions ||
        !input->domain->types || !input->domain->types->nominals || entry>=input->domain->function_count)
        return XR_XIR_BAD_STRUCTURE;
    return input->signature(input->owner,entry,out);
}
static XrXirStatus class_method_entry(const XirClassMethodRead *input,uint32_t entry,XirClassMethodSignature *out) {
    XrXirStatus status=class_method_header(input,entry,out);if(status!=XR_XIR_OK)return status;
    const XirMetadataDomain *domain=input->domain;const XrXirTypes *types=domain->types;
    const XrXirFunctionIdentity *identity=&domain->declarations->functions[entry];
    const XrXirNominalTable *table=types->nominals;
    if(!identity->nominal_owner || identity->nominal_owner>table->count ||
        identity->method_kind!=XR_XIR_READ_METHOD || identity->member_access!=XR_XIR_MEMBER_PUBLIC ||
        identity->cleanup_owner || identity->native_callable || identity->test_role ||
        (identity->promises&(XR_XIR_FUNCTION_GENERATOR|XR_XIR_FUNCTION_REST)) ||
        !out->parameter_count || !out->parameters ||
        (out->consume_parameter_count && (!out->consume_parameters || !out->consume_parameters[0])))return XR_XIR_BAD_TYPE;
    const XrXirTypeNode *receiver=xr_xir_type_node(types,out->parameters[0]);uint32_t d=identity->nominal_owner-1;
    uint32_t kind=table->declarations?table->declarations[d].kind:table->identities[d].kind;
    uint32_t arity=table->declarations?table->declarations[d].parameter_count:table->identities[d].arity;
    uint32_t native=table->declarations?table->declarations[d].native.native_id:table->identities[d].native.native_id;
    if(kind!=XR_XIR_NOMINAL_CLASS || arity || native || !receiver || receiver->kind!=XR_XIR_TYPE_NOMINAL ||
        receiver->nominal.declaration!=d || receiver->nominal.argument_count || receiver->parameter_span ||
        (domain->generics && (domain->generics[entry].parameter_count || domain->generics[entry].argument_count)))return XR_XIR_BAD_TYPE;
    return XR_XIR_OK;
}
static XrXirStatus class_method_override(const XirClassMethodRead *input,uint32_t derived,uint32_t base) {
    XirClassMethodSignature a={0},b={0};XrXirStatus status=class_method_entry(input,derived,&a);
    if(status==XR_XIR_OK)status=class_method_entry(input,base,&b);
    if(status!=XR_XIR_OK)return status;
    const XirMetadataDomain *domain=input->domain;const XrXirFunctionIdentity *identity=domain->declarations->functions;
    if(!class_method_name(&a,&b) || a.parameter_count!=b.parameter_count || a.result!=b.result ||
        a.consume_parameter_count!=b.consume_parameter_count ||
        !xr_xir_class_is_subtype(domain->types,a.parameters[0],b.parameters[0]) ||
        !xr_xir_error_bound_accepts(domain->types,a.error_type,b.error_type) ||
        ((identity[base].promises&XR_XIR_FUNCTION_NO_SUSPEND) &&
            !(identity[derived].promises&XR_XIR_FUNCTION_NO_SUSPEND)))return XR_XIR_BAD_TYPE;
    for(uint32_t p=1;p<a.parameter_count;++p)if(a.parameters[p]!=b.parameters[p])return XR_XIR_BAD_TYPE;
    for(uint32_t p=0;p<a.consume_parameter_count;++p)if(a.consume_parameters[p]!=b.consume_parameters[p])return XR_XIR_BAD_TYPE;
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_compile_class_method_select_read(const XirClassMethodRead *input,
    uint32_t slot,XrXirType actual,XirClassMethodTarget *output,XrCompileRequestControl *control) {
    if(!output)return XR_XIR_BAD_STRUCTURE;
    *output=(XirClassMethodTarget){0};
    XrXirStatus status=xir_receive_poll(control);if(status!=XR_XIR_OK)return status;
    XirClassMethodSignature anchor={0};status=class_method_entry(input,slot,&anchor);if(status!=XR_XIR_OK)return status;
    const XirMetadataDomain *domain=input->domain;
    if(!xr_xir_class_is_subtype(domain->types,actual,anchor.parameters[0]))return XR_XIR_BAD_TYPE;
    uint32_t owner=xr_xir_type_node(domain->types,actual)->nominal.declaration;
    const XrXirNominalTable *table=domain->types->nominals;
    const XrXirFunctionIdentity *identities=domain->declarations->functions;
    uint32_t selected=UINT32_MAX,previous=UINT32_MAX,steps=0;XrXirType receiver=XR_XIR_UNIT;
    for(uint32_t depth=0;depth<table->count;++depth) {
        uint32_t found=UINT32_MAX;
        for(uint32_t f=0;f<domain->function_count;++f) {
            status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
            if(identities[f].nominal_owner!=owner+1)continue;
            XirClassMethodSignature method={0};status=class_method_header(input,f,&method);if(status!=XR_XIR_OK)return status;
            if(!class_method_name(&method,&anchor))continue;
            if(found!=UINT32_MAX)return XR_XIR_BAD_STRUCTURE;
            status=class_method_override(input,f,slot);if(status!=XR_XIR_OK)return status;
            if(f!=slot && !(identities[f].promises&XR_XIR_FUNCTION_OVERRIDE))return XR_XIR_BAD_TYPE;
            found=f;
            if(selected==UINT32_MAX)receiver=method.parameters[0];
        }
        if(found!=UINT32_MAX) {
            if(previous!=UINT32_MAX) {status=class_method_override(input,previous,found);if(status!=XR_XIR_OK)return status;}
            previous=found;if(selected==UINT32_MAX)selected=found;
        }
        if(owner+1==identities[slot].nominal_owner) {
            if(found!=slot || selected==UINT32_MAX)return XR_XIR_BAD_STRUCTURE;
            *output=(XirClassMethodTarget){selected,receiver};return XR_XIR_OK;
        }
        uint32_t parent=class_method_parent(table,owner);if(!parent || parent>table->count)return XR_XIR_BAD_TYPE;
        owner=parent-1;
    }
    return XR_XIR_BAD_STRUCTURE;
}
XR_FUNC XrXirStatus xir_compile_class_method_candidates_read(const XirClassMethodRead *input,
    XrXirType static_receiver,uint32_t slot,XirClassMethodVisitor visit,
    void *owner,XrCompileRequestControl *control) {
    if(!visit)return XR_XIR_BAD_STRUCTURE;
    XirClassMethodTarget initial={0};
    XrXirStatus status=xir_compile_class_method_select_read(input,slot,static_receiver,&initial,control);
    if(status==XR_XIR_OK)status=visit(owner,&initial);
    if(status!=XR_XIR_OK)return status;
    const XirMetadataDomain *domain=input->domain;XirClassMethodSignature anchor={0};
    status=class_method_entry(input,slot,&anchor);if(status!=XR_XIR_OK)return status;
    uint32_t steps=0;
    for(uint32_t f=0;f<domain->function_count;++f) {
        status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
        const XrXirFunctionIdentity *identity=&domain->declarations->functions[f];
        if(f==initial.function || identity->method_kind!=XR_XIR_READ_METHOD)continue;
        XirClassMethodSignature function={0};status=class_method_header(input,f,&function);if(status!=XR_XIR_OK)return status;
        if(!class_method_name(&function,&anchor) || !function.parameter_count || !function.parameters ||
            !xr_xir_class_is_subtype(domain->types,function.parameters[0],static_receiver))continue;
        XirClassMethodTarget target={0};
        status=xir_compile_class_method_select_read(input,slot,function.parameters[0],&target,control);
        if(status!=XR_XIR_OK)return status;
        if(target.function!=f)return XR_XIR_BAD_STRUCTURE;
        status=visit(owner,&target);if(status!=XR_XIR_OK)return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus class_method_module_header(const void *owner,uint32_t entry,XirClassMethodSignature *output) {
    const XrXirModule *module=owner;
    if(!module || !module->functions || entry>=module->function_count || !output)return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *function=&module->functions[entry];
    *output=(XirClassMethodSignature){function->name,function->name_length,function->parameters,function->parameter_count,
        function->result,function->error_type,function->consume_parameters,function->consume_parameter_count};return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_compile_class_method_candidates(const XrXirModule *module,
    XrXirType static_receiver,uint32_t slot,XirClassMethodVisitor visit,void *owner,XrCompileRequestControl *control) {
    XirMetadataDomain domain=xir_metadata_domain_from_module(module);
    XirClassMethodRead input={&domain,module,class_method_module_header};
    return xir_compile_class_method_candidates_read(&input,static_receiver,slot,visit,owner,control);
}
XR_FUNC XrXirStatus xir_compile_class_method_site(const XrXirModule *module,
    uint32_t caller,uint32_t instruction,XrXirType actual_receiver,XirClassMethodTarget *output,XrCompileRequestControl *control) {
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
    uint32_t slot=(uint32_t)op->immediate;XirMetadataDomain domain=xir_metadata_domain_from_module(module);
    XirClassMethodRead input={&domain,module,class_method_module_header};XirClassMethodSignature anchor={0};
    status=class_method_entry(&input,slot,&anchor);if(status!=XR_XIR_OK)return status;
    XrXirType receiver=xr_xir_operand_type(function,op->args[0]);
    const XrXirTypeNode *signature=xr_xir_callable_signature(module->types,op->type);
    if(!xr_xir_class_is_subtype(module->types,receiver,anchor.parameters[0]) ||
        !xr_xir_class_is_subtype(module->types,actual_receiver,receiver) || !signature ||
        signature->parameter_count!=anchor.parameter_count-1 || signature->result!=anchor.result ||
        signature->element!=anchor.error_type || (signature->flags&XR_XIR_CALLABLE_REST) ||
        ((signature->flags&XR_XIR_CALLABLE_NO_SUSPEND) && !(domain.declarations->functions[slot].promises&XR_XIR_FUNCTION_NO_SUSPEND)))return XR_XIR_BAD_TYPE;
    for(uint32_t p=0;p<signature->parameter_count;++p)
        if(!xr_xir_callable_parameter_storage_valid(module->types,&signature->parameters[p]) ||
            signature->parameters[p].type!=anchor.parameters[p+1] ||
            signature->parameters[p].mode!=xr_xir_declared_parameter_mode(module->types,anchor.parameters[p+1],
                anchor.consume_parameters,anchor.consume_parameter_count,p+1))return XR_XIR_BAD_TYPE;
    const XrXirDeclarations *scope=domain.declarations;
    const XrXirFunctionIdentity *target=&scope->functions[slot],*from=&scope->functions[caller];
    if(from->module>=scope->module_count || target->module>=scope->module_count ||
        (!xr_xir_module_same_scope(scope,from->module,target->module) && !target->exported) ||
        !xr_xir_module_imports(scope,from->module,target->module))return XR_XIR_BAD_TYPE;
    return xir_compile_class_method_select_read(&input,slot,actual_receiver,output,control);
}

static uint32_t class_declaration_module(const XrXirModule *module,uint32_t declaration) {
    const XrXirNominalTable *table=module->types->nominals;
    XrXirLiteral name=table->declarations?table->declarations[declaration].module:table->identities[declaration].module;
    for(uint32_t m=0;m<module->declarations->module_count;++m)
        if(name.length==module->declarations->modules[m].name_length &&
            !memcmp(name.bytes,module->declarations->modules[m].name,name.length))return m;
    return UINT32_MAX;
}
static XrXirStatus class_declaration_parent(const XrXirModule *module,uint32_t d) {
    const XrXirNominalTable *table=module->types->nominals;uint32_t parent=class_method_parent(table,d);
    if(!parent)return XR_XIR_OK;
    if(parent>table->count)return XR_XIR_BAD_STRUCTURE;
    uint32_t from=class_declaration_module(module,d),to=class_declaration_module(module,parent-1);
    if(from==UINT32_MAX || to==UINT32_MAX)return XR_XIR_BAD_STRUCTURE;
    uint32_t exported=table->declarations?table->declarations[parent-1].exported:table->identities[parent-1].exported;
    return xr_xir_module_imports(module->declarations,from,to) &&
        (exported || xr_xir_module_same_scope(module->declarations,from,to))?XR_XIR_OK:XR_XIR_BAD_TYPE;
}
static XrXirStatus class_declaration_method(const XirClassMethodRead *input,uint32_t entry,
    XrCompileRequestControl *control) {
    XirClassMethodSignature method={0};XrXirStatus status=class_method_header(input,entry,&method);
    if(status!=XR_XIR_OK)return status;
    const XirMetadataDomain *domain=input->domain;const XrXirNominalTable *table=domain->types->nominals;
    const XrXirFunctionIdentity *identity=domain->declarations->functions;uint32_t child=identity[entry].nominal_owner-1;
    uint32_t parent=class_method_parent(table,child),steps=0;
    for(uint32_t depth=0;parent && depth<table->count;++depth) {
        uint32_t found=UINT32_MAX;
        for(uint32_t f=0;f<domain->function_count;++f) {
            status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
            if(identity[f].nominal_owner!=parent ||
                (identity[f].method_kind!=XR_XIR_READ_METHOD && identity[f].method_kind!=XR_XIR_STATIC_METHOD))continue;
            XirClassMethodSignature candidate={0};status=class_method_header(input,f,&candidate);if(status!=XR_XIR_OK)return status;
            if(!class_method_name(&method,&candidate))continue;
            if(found!=UINT32_MAX)return XR_XIR_BAD_STRUCTURE;
            found=f;
        }
        if(found!=UINT32_MAX) {
            if(!(identity[entry].promises&XR_XIR_FUNCTION_OVERRIDE))return XR_XIR_BAD_TYPE;
            return class_method_override(input,entry,found);
        }
        parent=class_method_parent(table,parent-1);
    }
    return identity[entry].promises&XR_XIR_FUNCTION_OVERRIDE?XR_XIR_BAD_TYPE:XR_XIR_OK;
}
static XrXirStatus class_declaration_fields(const XirClassMethodRead *input,uint32_t d,
    XrCompileRequestControl *control) {
    const XirMetadataDomain *domain=input->domain;const XrXirNominalTable *table=domain->types->nominals;
    uint32_t parent=class_method_parent(table,d),steps=0;
    if(!parent)return XR_XIR_OK;
    uint32_t first=table->declarations?table->declarations[parent-1].field_count:table->identities[parent-1].field_count;
    uint32_t count=table->declarations?table->declarations[d].field_count:table->identities[d].field_count;
    if(first==count)return XR_XIR_OK;
    for(uint32_t depth=0;parent && depth<table->count;++depth) {
        for(uint32_t f=0;f<domain->function_count;++f) {
            XrXirStatus status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
            const XrXirFunctionIdentity *identity=&domain->declarations->functions[f];
            if(identity->nominal_owner!=parent ||
                (identity->method_kind!=XR_XIR_READ_METHOD && identity->method_kind!=XR_XIR_STATIC_METHOD))continue;
            XirClassMethodSignature method={0};status=class_method_header(input,f,&method);if(status!=XR_XIR_OK)return status;
            for(uint32_t field=first;field<count;++field) {
                status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
                XrXirLiteral name=table->declarations?table->declarations[d].fields[field].name:table->identities[d].fields[field].name;
                if(name.length==method.name_length && !memcmp(name.bytes,method.name,name.length))return XR_XIR_BAD_TYPE;
            }
        }
        parent=class_method_parent(table,parent-1);
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_compile_class_declarations_verify(const XrXirModule *module,
    XrXirDiagnostic *location,XrCompileRequestControl *control) {
    if(location)*location=(XrXirDiagnostic){.function=UINT32_MAX,.block=UINT32_MAX,.instruction=UINT32_MAX};
    XrXirStatus status=xir_receive_poll(control);if(status!=XR_XIR_OK)return status;
    if(!module || !module->functions)return XR_XIR_BAD_STRUCTURE;
    const XrXirNominalTable *table=module->types?module->types->nominals:NULL;
    if(!table)return XR_XIR_OK;
    if(!module->declarations) {
        for(uint32_t d=0;d<table->count;++d)if(class_method_parent(table,d))return XR_XIR_BAD_STRUCTURE;
        return XR_XIR_OK;
    }
    XirMetadataDomain domain=xir_metadata_domain_from_module(module);
    XirClassMethodRead input={&domain,module,class_method_module_header};uint32_t steps=0;
    for(uint32_t d=0;d<table->count;++d) {
        status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
        status=class_declaration_parent(module,d);
        if(status==XR_XIR_OK)status=class_declaration_fields(&input,d,control);
        if(status!=XR_XIR_OK)return status;
    }
    for(uint32_t f=0;f<module->function_count;++f) {
        status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
        const XrXirFunctionIdentity *identity=&module->declarations->functions[f];
        if(!identity->nominal_owner || identity->nominal_owner>table->count) {
            if(identity->promises&XR_XIR_FUNCTION_OVERRIDE)return XR_XIR_BAD_TYPE;
            continue;
        }
        uint32_t d=identity->nominal_owner-1;
        if(!(identity->promises&XR_XIR_FUNCTION_OVERRIDE) &&
            (!class_method_parent(table,d) || (identity->method_kind!=XR_XIR_READ_METHOD && identity->method_kind!=XR_XIR_STATIC_METHOD)))continue;
        if(location)location->function=f;
        uint32_t kind=table->declarations?table->declarations[d].kind:table->identities[d].kind;
        if(kind!=XR_XIR_NOMINAL_CLASS || (identity->method_kind!=XR_XIR_READ_METHOD && identity->method_kind!=XR_XIR_STATIC_METHOD))
            return XR_XIR_BAD_TYPE;
        XirClassMethodSignature method={0};status=class_method_header(&input,f,&method);if(status!=XR_XIR_OK)return status;
        uint32_t fields=table->declarations?table->declarations[d].field_count:table->identities[d].field_count;
        for(uint32_t field=0;field<fields;++field) {
            XrXirLiteral name=table->declarations?table->declarations[d].fields[field].name:table->identities[d].fields[field].name;
            if(name.length==method.name_length && !memcmp(name.bytes,method.name,name.length))return XR_XIR_BAD_TYPE;
        }
        status=class_declaration_method(&input,f,control);if(status!=XR_XIR_OK)return status;
    }
    if(location)*location=(XrXirDiagnostic){.function=UINT32_MAX,.block=UINT32_MAX,.instruction=UINT32_MAX};
    return XR_XIR_OK;
}
