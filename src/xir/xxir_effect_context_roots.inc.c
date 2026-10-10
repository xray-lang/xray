/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_context_roots.inc.c - Context roots from authentic bodies
 *
 * KEY CONCEPT:
 *   Ordinary views derive equations with the existing body solver. This helper
 *   neither admits an executable module nor grants worker authority.
 */
static XrXirStatus effect_ordinary_op_match(EffectOrdinaryContexts *contexts,
    const XrXirModule *source, uint32_t function, uint32_t instruction) {
    EffectOrdinaryNode identity=contexts->nodes[function];
    XrXirInstruction expected=source->functions[identity.declaration].instructions[instruction];
    XrXirGeneric environment={.arguments=identity.arguments,.argument_count=identity.argument_count};
    XrXirStatus status=XR_XIR_OK;
    if (function>=contexts->base_count)
        status=effect_terms_substitute(&contexts->terms,expected.type,&environment,&expected.type);
    if (status!=XR_XIR_OK) return status;
    const XrXirGeneric *generic=source->generics ? &source->generics[identity.declaration] : NULL;
    bool open=function<contexts->base_count && generic && generic->parameter_count;
    XrXirInstruction actual=contexts->functions[function].instructions[instruction];
    if (!open && effect_context_family(expected.op)) {
        EffectContextRequest request={source,&contexts->terms.types,identity.arguments,
            identity.argument_count,identity.declaration,instruction};
        EffectContextResolved resolved={0};
        status=effect_context_resolve_in(&contexts->terms,&request,&resolved);
        if (status!=XR_XIR_OK) return status;
        if (resolved.known) {
            if (actual.immediate<0 || (uint64_t)actual.immediate>=contexts->count) return XR_XIR_BAD_STRUCTURE;
            bool equal=false;
            status=effect_ordinary_environment_same(contexts,&contexts->nodes[actual.immediate],
                resolved.target,resolved.arguments,resolved.argument_count,&equal);
            if (status!=XR_XIR_OK) return status;
            if (!equal) return XR_XIR_BAD_STRUCTURE;
            expected.immediate=actual.immediate;
            if (expected.op==XR_XIR_CALL_REQUIREMENT || expected.op==XR_XIR_CALL_DEFAULT) {
                expected.op=XR_XIR_CALL;expected.targets[0]=expected.targets[1]=0;
            } else if (expected.op==XR_XIR_INVOKE_DEFAULT) {
                expected.op=XR_XIR_INVOKE;expected.args[0]=expected.args[1]=0;
            }
            expected.type_arguments[0]=expected.type_arguments[1]=0;
        }
    }
    if (!xir_compile_work(contexts->terms.remaining,11)) return XR_XIR_BUDGET;
    if (expected.op!=actual.op || expected.immediate!=actual.immediate ||
        expected.args[0]!=actual.args[0] || expected.args[1]!=actual.args[1] ||
        expected.targets[0]!=actual.targets[0] || expected.targets[1]!=actual.targets[1] ||
        expected.type_arguments[0]!=actual.type_arguments[0] ||
        expected.type_arguments[1]!=actual.type_arguments[1]) return XR_XIR_BAD_STRUCTURE;
    XrXirTypeMatchScratch *scratch=effect_terms_type_scratch(&contexts->terms);
    status=xr_xir_compile_type_substitution_matches_between_scratch(contexts->terms.remaining,
        &contexts->terms.types,&contexts->terms.types,NULL,0,expected.type,actual.type,scratch);
    return status==XR_XIR_BAD_TYPE ? XR_XIR_BAD_STRUCTURE : status;
}

static XrXirStatus effect_ordinary_signature_match(EffectOrdinaryContexts *contexts,
    const XrXirModule *source, uint32_t function) {
    EffectOrdinaryNode identity=contexts->nodes[function];
    const XrXirFunction *original=&source->functions[identity.declaration];
    const XrXirFunction *actual=&contexts->functions[function];
    XrXirGeneric environment={.arguments=identity.arguments,.argument_count=identity.argument_count};
    XrXirTypeMatchScratch *scratch=effect_terms_type_scratch(&contexts->terms);
    XrXirStatus status=XR_XIR_OK;
    for (uint32_t p=0;p<=actual->parameter_count && status==XR_XIR_OK;++p) {
        XrXirType expected=p==actual->parameter_count ? original->result : original->parameters[p];
        XrXirType type=p==actual->parameter_count ? actual->result :
            contexts->nodes[function].source_parameters[p];
        if (function>=contexts->base_count)
            status=effect_terms_substitute(&contexts->terms,expected,&environment,&expected);
        if (status==XR_XIR_OK)
            status=xr_xir_compile_type_substitution_matches_between_scratch(contexts->terms.remaining,
                &contexts->terms.types,&contexts->terms.types,NULL,0,expected,type,scratch);
    }
    return status==XR_XIR_BAD_TYPE ? XR_XIR_BAD_STRUCTURE : status;
}

static XrXirStatus effect_ordinary_application_match(EffectOrdinaryContexts *contexts,
    const XrXirTypes *types, XrXirInterfaceApplication a, XrXirInterfaceApplication b,
    uint32_t scope) {
    if (a.declaration!=b.declaration || a.argument_count!=b.argument_count ||
        !!a.arguments!=!!a.argument_count || !!b.arguments!=!!b.argument_count) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t p=0;p<a.argument_count;++p) {
        XrXirStatus status=effect_terms_domain_type(&contexts->terms,types,a.arguments[p],b.arguments[p],scope);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_ordinary_implementations_match(EffectOrdinaryContexts *contexts,
    const XrXirModule *source) {
    const XrXirImplementationTable *a=contexts->source_declarations ? contexts->source_declarations->implementations : NULL;
    const XrXirImplementationTable *b=source->declarations ? source->declarations->implementations : NULL;
    if (!!a!=!!b || (a && a->count!=b->count)) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t i=0;a && i<a->count;++i) {
        const XrXirImplementation *left=&a->records[i],*right=&b->records[i];
        if (!xir_compile_work(contexts->terms.remaining,2)) return XR_XIR_BUDGET;
        if (left->nominal_declaration!=right->nominal_declaration || left->binding_count!=right->binding_count ||
            !contexts->terms.types.nominals || left->nominal_declaration>=contexts->terms.types.nominals->count)
            return XR_XIR_BAD_STRUCTURE;
        uint32_t scope=contexts->terms.types.nominals->declarations[left->nominal_declaration].parameter_count;
        XrXirStatus status=effect_ordinary_application_match(contexts,source->types,left->interface,right->interface,scope);
        for (uint32_t bnd=0;bnd<left->binding_count && status==XR_XIR_OK;++bnd) {
            if (!left->bindings || !right->bindings || left->bindings[bnd].member!=right->bindings[bnd].member ||
                left->bindings[bnd].function!=right->bindings[bnd].function) return XR_XIR_BAD_STRUCTURE;
            status=effect_ordinary_application_match(contexts,source->types,left->bindings[bnd].requirement,
                right->bindings[bnd].requirement,scope);
        }
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_ordinary_declarations_match(EffectOrdinaryContexts *contexts,
    const XrXirModule *source) {
    const XrXirCompileContext *context=contexts->terms.remaining;
    const XrXirDeclarations *a=contexts->source_declarations,*b=source->declarations;
    if (!!a!=!!b || (a && (a->module_count!=b->module_count || a->slot_count!=b->slot_count ||
        a->literal_count!=b->literal_count || a->root_module!=b->root_module ||
        a->entry_function!=b->entry_function))) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t m=0;a && m<a->module_count;++m) {
        const XrXirSourceModule *left=&a->modules[m],*right=&b->modules[m];
        if (left->initializer!=right->initializer || left->dependency_count!=right->dependency_count)
            return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status=effect_terms_literal_same(&contexts->terms,
            (XrXirLiteral){left->name,left->name_length},(XrXirLiteral){right->name,right->name_length});
        if (status!=XR_XIR_OK) return status;
        if (!xir_compile_work(context,(uint64_t)left->dependency_count*sizeof(uint32_t))) return XR_XIR_BUDGET;
        if (left->dependency_count && (!right->dependencies || memcmp(left->dependencies,right->dependencies,
            (size_t)left->dependency_count*sizeof(uint32_t)))) return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t f=0;a && f<contexts->base_count;++f) {
        if (!xir_compile_work(context,sizeof(XrXirFunctionIdentity))) return XR_XIR_BUDGET;
        if (!b->functions || memcmp(&a->functions[f],&b->functions[f],sizeof(XrXirFunctionIdentity)))
            return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t l=0;a && l<a->literal_count;++l) {
        XrXirStatus status=effect_terms_literal_same(&contexts->terms,a->literals[l],b->literals[l]);
        if (status!=XR_XIR_OK) return status;
    }
    for (uint32_t s=0;a && s<a->slot_count;++s) {
        if (!xir_compile_work(context,2)) return XR_XIR_BUDGET;
        if (!b->slots || a->slots[s].module!=b->slots[s].module || a->slots[s].mutable!=b->slots[s].mutable)
            return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status=effect_terms_domain_type(&contexts->terms,source->types,a->slots[s].type,b->slots[s].type,0);
        if (status!=XR_XIR_OK) return status;
    }
    if (!!contexts->source_generics!=!!source->generics) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t f=0;source->generics && f<contexts->base_count;++f) {
        const XrXirGeneric *left=&contexts->source_generics[f],*right=&source->generics[f];
        if (left->parameter_count!=right->parameter_count || left->argument_count!=right->argument_count)
            return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status=effect_terms_domain_constraints(&contexts->terms,source->types,
            left->constraints,right->constraints,left->parameter_count);
        for (uint32_t p=0;p<left->parameter_count && status==XR_XIR_OK;++p) {
            if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
            if (xr_xir_binder_kind(left,p)!=xr_xir_binder_kind(right,p)) return XR_XIR_BAD_STRUCTURE;
        }
        for (uint32_t v=0;v<left->argument_count && status==XR_XIR_OK;++v)
            status=effect_terms_domain_type(&contexts->terms,source->types,left->arguments[v],right->arguments[v],left->parameter_count);
        if (status!=XR_XIR_OK) return status;
    }
    uint32_t count=source->defaults ? source->defaults->count : 0;
    if (count!=contexts->source_defaults.count) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(context,(uint64_t)count*sizeof(XrXirDefaultBinding))) return XR_XIR_BUDGET;
    if (count && (!source->defaults->records || memcmp(contexts->source_defaults.records,
        source->defaults->records,(size_t)count*sizeof(XrXirDefaultBinding)))) return XR_XIR_BAD_STRUCTURE;
    return effect_ordinary_implementations_match(contexts,source);
}

static XrXirStatus effect_ordinary_bodies_match(EffectOrdinaryContexts *contexts,
    const XrXirModule *source) {
    XrXirStatus declarations=effect_ordinary_declarations_match(contexts,source);
    if (declarations!=XR_XIR_OK) return declarations;
    for (uint32_t f=0;f<contexts->count;++f) {
        if (!xir_compile_work(contexts->terms.remaining,3)) return XR_XIR_BUDGET;
        uint32_t declaration=contexts->nodes[f].declaration;
        if (declaration>=source->function_count) return XR_XIR_BAD_STRUCTURE;
        const XrXirFunction *original=&source->functions[declaration], *actual=&contexts->functions[f];
        if (actual->parameter_count!=original->parameter_count ||
            actual->instruction_count!=original->instruction_count ||
            actual->operand_count!=original->operand_count) return XR_XIR_BAD_STRUCTURE;
        XrXirStatus name=effect_terms_literal_same(&contexts->terms,
            (XrXirLiteral){actual->name,actual->name_length},(XrXirLiteral){original->name,original->name_length});
        if (name!=XR_XIR_OK) return name;
        XrXirStatus signature=effect_ordinary_signature_match(contexts,source,f);
        if (signature!=XR_XIR_OK) return signature;
        for (uint32_t v=0;v<actual->operand_count;++v) {
            if (!xir_compile_work(contexts->terms.remaining,2)) return XR_XIR_BUDGET;
            if (actual->operands[v]!=original->operands[v]) return XR_XIR_BAD_STRUCTURE;
        }
        for (uint32_t i=0;i<actual->instruction_count;++i) {
            XrXirStatus status=effect_ordinary_op_match(contexts,source,f,i);
            if (status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_ordinary_root_storage(EffectOrdinaryContexts *contexts,
    const XrXirCompileContext *context) {
    XrXirEffects *effects=&contexts->uses;
    XrXirStatus status=XR_XIR_OK;
    effects->functions=xir_compile_calloc(context,contexts->count,sizeof(*effects->functions),&status);
    if (!effects->functions) return status;
    effects->task_creation=xir_compile_calloc(context,contexts->count,sizeof(*effects->task_creation),&status);
    if (!effects->task_creation) return status;
    effects->root=xir_compile_calloc(context,contexts->count,sizeof(*effects->root),&status);
    if (!effects->root) return status;
    effects->root_witnesses=xir_compile_calloc(context,contexts->count,sizeof(*effects->root_witnesses),&status);
    if (!effects->root_witnesses) return status;
    effects->unresolved_witnesses=xir_compile_calloc(context,contexts->count,
        sizeof(*effects->unresolved_witnesses),&status);
    return effects->unresolved_witnesses ? XR_XIR_OK : status;
}

/* Descriptor declarations, structural children, equations, facts and
 * forests are owned. Source bodies are consulted only during this proof. */
static XrXirStatus effect_invocation_context_bounds(const XrXirCompileContext *work,
    const XrXirModule *source,EffectOrdinaryContexts *contexts,EffectInvocationDeclaredBounds **output);

static XrXirStatus effect_ordinary_roots_with_bounds(const XrXirCompileContext *context,
    const XrXirModule *source,EffectOrdinaryContexts *contexts,
    const EffectInvocationDeclaredBounds *declared) {
    if (!xir_compile_context_valid(context) || !source || !source->declarations ||
        !source->declarations->functions || !source->declarations->modules || !contexts ||
        contexts->uses.resources!=context->resources || contexts->uses.root ||
        contexts->base_count!=source->function_count) return XR_XIR_BAD_STRUCTURE;
    contexts->terms.remaining=context;
    XrXirStatus status=effect_terms_snapshot_match(&contexts->terms,source->types,contexts->source_type_count);
    if (status==XR_XIR_OK && !contexts->dense) status=effect_ordinary_bodies_match(contexts,source);
    EffectInvocationDeclaredBounds *temporary=NULL;
    if (status==XR_XIR_OK && !declared)
        status=effect_invocation_context_bounds(context,source,contexts,&temporary);
    EffectOrdinaryView view={0};
    if (status==XR_XIR_OK) status=effect_ordinary_view(contexts,source,&view);
    if (status==XR_XIR_OK) status=effect_ordinary_root_storage(contexts,context);
    EffectGraph graph={0};XrXirCompileContext remaining=*context;
    if (status==XR_XIR_OK) status=effect_graph_build(&view.module,&contexts->uses,&graph,&remaining);
    if (status==XR_XIR_OK) status=effect_invocation_publish(context,&view.module,&contexts->uses,
        &graph,declared?declared:temporary,contexts);
    effect_graph_free(&graph);
    effect_invocation_bounds_free(temporary);
    contexts->terms.remaining=NULL;
    return status;
}

static XrXirStatus effect_ordinary_roots(const XrXirCompileContext *context,
    const XrXirModule *source,EffectOrdinaryContexts *contexts) {
    return effect_ordinary_roots_with_bounds(context,source,contexts,NULL);
}
