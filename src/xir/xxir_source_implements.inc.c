/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_implements.inc.c - Explicit declaration-owned method witnesses
 */
static bool source_implementation_applications(SourceContext *ctx) {
    uint32_t count = 0;
    for (uint32_t d = 0; d < ctx->nominals.count; ++d) {
        SourceName *owner = ctx->nominal_sources[d];
        if (!source_work(ctx,owner->node)) return false;
        SourceNominalDeclaration declaration;
        if (!source_nominal_declaration(ctx,owner->node,&declaration)) return false;
        if (declaration.interface_count < 0 || (declaration.interface_count && !declaration.interfaces))
            return source_fail(ctx,owner->node,XR_XIR_BAD_STRUCTURE,"implemented interface applications are malformed");
        if ((uint32_t)declaration.interface_count > UINT32_MAX - count)
            return source_fail(ctx,owner->node,XR_XIR_BUDGET,"implementation count exhausted");
        count += (uint32_t)declaration.interface_count;
    }
    XrXirImplementation *records = count ? source_alloc(ctx,count,sizeof(*records)) : NULL;
    if (count && !records) return false;
    ctx->implementations.records = records; ctx->implementations.count = count;
    uint32_t next = 0;
    SourceTypeScope saved = ctx->type_scope;
    uint32_t module = ctx->module;
    bool ok = true;
    for (uint32_t d = 0; d < ctx->nominals.count && ok; ++d) {
        SourceName *owner = ctx->nominal_sources[d];
        SourceNominalDeclaration declaration;
        if (!source_nominal_declaration(ctx,owner->node,&declaration)) { ok = false; break; }
        ctx->module = owner->module;
        ctx->type_scope = (SourceTypeScope){true,owner->node,declaration.parameters,
            (uint32_t)declaration.parameter_count,declaration.parameter_count ? owner->declaration : 0};
        uint32_t first = next;
        for (int i = 0; i < declaration.interface_count && ok; ++i) {
            XrXirImplementation *record = &records[next++]; record->nominal_declaration = d;
            ok = source_interface_application(ctx,owner->node,declaration.interfaces[i],&record->interface);
            for (uint32_t earlier = first; earlier + 1 < next && ok; ++earlier) {
                bool same = false;
                ok = source_constraint_same(ctx,owner->node,&records[earlier].interface,&record->interface,&same);
                if (ok && same) ok = source_fail(ctx,owner->node,XR_XIR_BAD_TYPE,"duplicate explicit implementation");
            }
        }
    }
    ctx->type_scope = saved; ctx->module = module; return ok;
}
static bool source_implementation_promises(SourceContext *ctx, uint32_t function, XrXirType signature) {
    const XrXirTypeNode *found = xr_xir_callable_signature(&ctx->types,signature);
    XrXirTypeNode requirement = *found;
    if (requirement.flags & XR_XIR_CALLABLE_NO_SUSPEND)
        ctx->identities[function].promises |= XR_XIR_FUNCTION_NO_SUSPEND;
    if (ctx->functions[function].parameter_count != requirement.parameter_count + 1) return true;
    for (uint32_t p = 0; p < requirement.parameter_count; ++p) {
        const XrXirTypeNode *expected = xr_xir_callable_signature(&ctx->types,requirement.parameters[p].type);
        if (!expected || !(expected->flags & XR_XIR_CALLABLE_NO_SUSPEND)) continue;
        const XrXirTypeNode *actual = xr_xir_callable_signature(&ctx->types,ctx->functions[function].parameters[p + 1]);
        if (actual && !(actual->flags & XR_XIR_CALLABLE_NO_SUSPEND) &&
            !source_parameter_promise(ctx,function,p + 1)) return false;
    }
    return true;
}
static bool source_implementation_bindings(SourceContext *ctx, XrXirImplementation *implementation) {
    SourceName *owner = ctx->nominal_sources[implementation->nominal_declaration];
    ctx->module = owner->module;
    XrXirTypes input = ctx->types;
    XrXirInterfaceClosure *closure = NULL;
    uint64_t closure_scratch = ctx->budget.scratch_bytes;
    XrXirStatus status = xr_xir_interface_closure_build(&ctx->interfaces,&input,
        &implementation->interface,1,&ctx->budget,&closure);
    if (status != XR_XIR_OK) return source_fail(ctx,owner->node,status,"implementation requirements are invalid");
    closure_scratch -= ctx->budget.scratch_bytes;
    bool ok = false;
    uint32_t count = xr_xir_interface_closure_requirement_count(closure);
    XrXirImplementationBinding *bindings = count ? source_alloc(ctx,count,sizeof(*bindings)) : NULL;
    if (count && !bindings) goto done;
    const XrXirTypes *pool = xr_xir_interface_closure_types(closure);
    for (uint32_t r = 0; r < count; ++r) {
        const XrXirInterfaceRequirement *requirement = xr_xir_interface_closure_requirement(closure,r);
        char *name = source_alloc(ctx,(uint64_t)requirement->name.length + 1,1);
        if (!name || !source_work(ctx,owner->node)) goto done;
        memcpy(name,requirement->name.bytes,requirement->name.length);
        SourceName *method = find_name(ctx,ctx->nominal_methods[owner->index],name);
        if (!method) { source_fail(ctx,owner->node,XR_XIR_BAD_TYPE,"explicit implementation is missing a method"); goto done; }
        if (ctx->identities[method->index].method_kind != XR_XIR_READ_METHOD ||
            ctx->generics[method->index].parameter_count != ctx->nominals.declarations[owner->index].parameter_count) {
            source_fail(ctx,method->node,XR_XIR_BAD_TYPE,"interface implementation requires an ordinary read instance method"); goto done;
        }
        XrXirType signature;
        if (!source_reify_application(ctx,owner->node,pool,
            xr_xir_interface_closure_application(closure,requirement->application),&bindings[r].requirement) ||
            !source_reify_type(ctx,owner->node,pool,requirement->signature,&signature)) goto done;
        bindings[r].member = requirement->member; bindings[r].function = method->index;
        if (!source_implementation_promises(ctx,method->index,signature)) goto done;
    }
    implementation->bindings = bindings; implementation->binding_count = count; ok = true;
done:
    xr_xir_interface_closure_free(closure);
    ctx->budget.scratch_bytes += closure_scratch; return ok;
}
static bool source_implementations_bind(SourceContext *ctx) {
    XrXirDeclarations declarations;
    XrXirModule module = source_module_view(ctx,&declarations);
    XrXirStatus status = xr_xir_types_structure_verify(module.types,&ctx->budget);
    if (status == XR_XIR_OK) status = xr_xir_generics_structure_verify(&module,&ctx->budget);
    if (status != XR_XIR_OK)
        return source_fail(ctx,NULL,status,"source declaration type structure is invalid");
    if (!source_implementation_applications(ctx)) return false;
    XrXirImplementation *records = (XrXirImplementation *)ctx->implementations.records;
    for (uint32_t i = 0; i < ctx->implementations.count; ++i)
        if (!source_implementation_bindings(ctx,&records[i])) return false;
    module = source_module_view(ctx,&declarations);
    status = xr_xir_types_structure_verify(module.types,&ctx->budget);
    if (status == XR_XIR_OK) status = xr_xir_generics_structure_verify(&module,&ctx->budget);
    if (status != XR_XIR_OK) return source_fail(ctx,NULL,status,"bound implementation type structure is invalid");
    status = xr_xir_implementations_verify(&module,&ctx->budget);
    if (status != XR_XIR_OK) return source_fail(ctx,NULL,status,"implementation witness definition obligations failed");
    status = xr_xir_declaration_constraints_verify(&module,&ctx->budget);
    if (status != XR_XIR_OK) return source_fail(ctx,NULL,status,"declaration type obligations failed");
    ctx->implementations_ready = true; return true;
}
