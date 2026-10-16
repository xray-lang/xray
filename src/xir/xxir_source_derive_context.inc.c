/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_derive_context.inc.c - Logical requirement and callee descriptions
 */
typedef struct SourceDeriveSchema {
    XrXirType method,methods,callee,callees,names,context;
    SourceInfoRows parameters;
} SourceDeriveSchema;
static bool source_derive_schema(SourceInfoGraph *g,SourceDeriveSchema *s) {
    const XrXirType method[]={XR_XIR_STRING,XR_XIR_I64,XR_XIR_I64,XR_XIR_I64,XR_XIR_I64};
    const XrXirType callee[]={XR_XIR_STRING,XR_XIR_STRING,XR_XIR_I64,XR_XIR_I64,XR_XIR_I64,XR_XIR_I64};
    if (!source_info_tuple_type(g->ctx,method,5,&s->method) ||
        !source_array_element_type(g->ctx,s->method,&s->methods) ||
        !source_info_tuple_type(g->ctx,callee,6,&s->callee) ||
        !source_array_element_type(g->ctx,s->callee,&s->callees) ||
        !source_array_element_type(g->ctx,XR_XIR_STRING,&s->names)) return false;
    const XrXirType context[]={s->methods,s->callees,g->schema.arguments,XR_XIR_I64,s->names};
    return source_info_tuple_type(g->ctx,context,5,&s->context);
}
static bool source_derive_parameter_refs(SourceInfoGraph *g,SourceDeriveSchema *schema,
    const XrXirType *types,uint32_t count,SourceValue output[2]) {
    SourceInfoRows *rows=&schema->parameters;
    if (count>UINT32_MAX-rows->count)
        return source_fail(g->ctx,g->syntax,XR_XIR_BUDGET,"derive parameter table exhausted");
    uint32_t required=rows->count+count;
    if (required>rows->capacity) {
        uint32_t capacity=rows->capacity?rows->capacity:8;
        while (capacity<required) {
            if (!source_work(g->ctx,g->syntax)) return false;
            capacity=capacity>UINT32_MAX/2?UINT32_MAX:capacity*2;
        }
        SourceValue *values=source_recipe_storage(g->ctx,capacity,sizeof(*values));
        if (!values || !source_copy_bytes(g->ctx,g->syntax,values,rows->values,rows->count*sizeof(*values))) return false;
        rows->values=values;rows->capacity=capacity;
    }
    if (!source_info_integer(g,rows->count,&output[0]) || !source_info_integer(g,count,&output[1])) return false;
    for (uint32_t p=0;p<count;++p)
        if (!source_info_ref_value(g,types[p],&rows->values[rows->count++])) return false;
    return true;
}
static bool source_derive_requirements(SourceInfoGraph *g,SourceDeriveJob *job,
    SourceDeriveSchema *schema,SourceValue *output) {
    SourceContext *ctx=g->facts;
    const XrXirDerivationPolicy *policy=&ctx->derivations.policies[job->policy];
    uint32_t ambient=ctx->nominals.declarations[job->owner->index].parameter_count;
    XrXirInterfaceClosureRoots roots={&ctx->interfaces,&ctx->types,&policy->protocol,1,ambient};
    XrXirStatus status=xr_xir_compile_interface_closure_build(&ctx->compile,&roots,&job->closure);
    if (status!=XR_XIR_OK) return source_fail(g->ctx,g->syntax,status,"derive requirements failed expansion");
    job->requirement_count=xr_xir_interface_closure_requirement_count(job->closure);
    job->requirements=source_alloc(ctx,job->requirement_count,sizeof(*job->requirements));
    SourceValue *values=source_recipe_storage(g->ctx,job->requirement_count,sizeof(*values));
    if (!job->requirements || !values) return false;
    for (uint32_t r=0;r<job->requirement_count;++r) {
        const XrXirInterfaceRequirement *requirement=xr_xir_interface_closure_requirement(job->closure,r);
        const XrXirTypes *pool=xr_xir_interface_closure_types(job->closure);
        XrXirImplementationBinding *binding=&job->requirements[r];XrXirType signature;
        if (!source_reify_application(ctx,g->syntax,pool,
            xr_xir_interface_closure_application(job->closure,requirement->application),&binding->requirement) ||
            !source_reify_type(ctx,g->syntax,pool,requirement->signature,&signature)) return false;
        binding->member=requirement->member;
        const XrXirTypeNode copy=*xr_xir_callable_signature(&ctx->types,signature);
        SourceValue fields[5];XrXirType *parameters=source_alloc(ctx,copy.parameter_count,sizeof(*parameters));
        if (!parameters) return false;
        for (uint32_t p=0;p<copy.parameter_count;++p) {
            if (!source_work(g->ctx,g->syntax)) return false;
            parameters[p]=copy.parameters[p].type;
        }
        if (!source_info_text(g,requirement->name,&fields[0]) ||
            !source_info_ref_value(g,copy.result,&fields[1]) ||
            !source_derive_parameter_refs(g,schema,parameters,copy.parameter_count,&fields[2]) ||
            !source_info_integer(g,requirement->own_parameter_count,&fields[4]) ||
            !source_info_group(g,schema->method,fields,5,&values[r])) return false;
    }
    return source_info_array(g,schema->methods,values,job->requirement_count,output);
}
static bool source_derive_callee_visible(SourceContext *ctx,SourceDeriveJob *job,uint32_t f) {
    const XrXirDerivationPolicy *policy=&ctx->derivations.policies[job->policy];
    if (policy->constructor==f+1) return true;
    const XrXirFunctionIdentity *identity=&ctx->identities[f];
    if (identity->nominal_owner || identity->cleanup_owner || identity->derivation || !identity->exported ||
        (ctx->has_generics && (ctx->generics[f].parameter_count || ctx->generics[f].argument_count))) return false;
    XrXirDeclarations declarations;source_module_view(ctx,&declarations);
    return xr_xir_module_imports(&declarations,job->owner->module,identity->module);
}
static bool source_derive_callees(SourceInfoGraph *g,SourceDeriveJob *job,
    SourceDeriveSchema *schema,SourceValue *output,int64_t *constructor) {
    SourceContext *ctx=g->facts;
    job->callees=source_alloc(ctx,ctx->function_count,sizeof(*job->callees));
    SourceValue *values=source_recipe_storage(g->ctx,ctx->function_count,sizeof(*values));
    if (!job->callees || !values) return false;
    *constructor=-1;
    for (uint32_t f=0;f<ctx->function_count;++f) {
        if (!source_work(g->ctx,g->syntax)) return false;
        if (!source_derive_callee_visible(ctx,job,f)) continue;
        const XrXirFunction *function=&ctx->functions[f];
        uint32_t ordinal=job->callee_count++;job->callees[ordinal]=f;
        const XrXirSourceModule *module=&ctx->modules[ctx->identities[f].module];SourceValue fields[6];
        if (!source_info_text(g,(XrXirLiteral){module->name,module->name_length},&fields[0]) ||
            !source_info_text(g,(XrXirLiteral){function->name,function->name_length},&fields[1]) ||
            !source_info_integer(g,ordinal,&fields[2]) ||
            !source_derive_parameter_refs(g,schema,function->parameters,function->parameter_count,&fields[3]) ||
            !source_info_ref_value(g,function->result,&fields[5]) ||
            !source_info_group(g,schema->callee,fields,6,&values[ordinal])) return false;
        if (ctx->derivations.policies[job->policy].constructor==f+1) *constructor=ordinal;
    }
    return source_info_array(g,schema->callees,values,job->callee_count,output);
}
/* Names belong only to the explicitly selected definition. They describe
 * ordered inputs and never authorize an operation on the resulting value. */
static bool source_derive_constructor_names(SourceInfoGraph *g,SourceDeriveJob *job,
    const SourceDeriveSchema *schema,SourceValue *output) {
    SourceContext *ctx=g->facts;
    const XrXirDerivationPolicy *policy=&ctx->derivations.policies[job->policy];
    if (!policy->constructor) return source_info_array(g,schema->names,NULL,0,output);
    uint32_t function=policy->constructor-1;
    if (function>=ctx->function_count || ctx->identities[function].method_kind!=XR_XIR_CONSTRUCTOR ||
        ctx->identities[function].nominal_owner!=job->owner->index+1 ||
        ctx->identities[function].module!=job->owner->module || ctx->bodies[function].checked_library)
        return source_fail(g->ctx,g->syntax,XR_XIR_BAD_STRUCTURE,"derive constructor names have no current definition");
    AstNode *node=ctx->bodies[function].node;
    uint32_t count=ctx->functions[function].parameter_count;
    if (!node || node->type!=AST_METHOD_DECL || !node->as.method_decl.is_constructor ||
        node->as.method_decl.param_count<0 || (uint32_t)node->as.method_decl.param_count!=count ||
        (count && !node->as.method_decl.params))
        return source_fail(g->ctx,g->syntax,XR_XIR_BAD_STRUCTURE,"derive constructor parameter names disagree with its signature");
    SourceValue *names=count?source_recipe_storage(g->ctx,count,sizeof(*names)):NULL;
    if (count && !names) return false;
    for (uint32_t p=0;p<count;++p) {
        if (!source_work(g->ctx,g->syntax)) return false;
        const XrParamNode *parameter=node->as.method_decl.params[p];
        if (!parameter || !parameter->name || !*parameter->name)
            return source_fail(g->ctx,g->syntax,XR_XIR_BAD_STRUCTURE,"derive constructor parameter name is missing");
        size_t length=source_text_size(g->ctx,parameter->name);
        if (length>UINT32_MAX)
            return source_fail(g->ctx,g->syntax,XR_XIR_BUDGET,"derive constructor parameter name is too large");
        if (!source_info_text(g,(XrXirLiteral){parameter->name,(uint32_t)length},&names[p])) return false;
    }
    return source_info_array(g,schema->names,names,count,output);
}
static bool source_derive_context(SourceInfoGraph *g,SourceDeriveJob *job,SourceValue *output) {
    SourceDeriveSchema schema={0};SourceValue fields[5];int64_t constructor;
    return source_derive_schema(g,&schema) && source_derive_requirements(g,job,&schema,&fields[0]) &&
        source_derive_callees(g,job,&schema,&fields[1],&constructor) &&
        source_info_array(g,g->schema.arguments,schema.parameters.values,schema.parameters.count,&fields[2]) &&
        source_info_integer(g,constructor,&fields[3]) &&
        source_derive_constructor_names(g,job,&schema,&fields[4]) &&
        source_info_group(g,schema.context,fields,5,output);
}
