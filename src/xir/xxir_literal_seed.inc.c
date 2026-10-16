/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_literal_seed.inc.c - Receiving inventory from actual Checked declarations
 *
 * KEY CONCEPT:
 *   The original definition stays immutable throughout synchronous receiving.
 *   Mutable descriptor heads and appended bodies belong to the receiving arena.
 */
static bool literal_seed(SourceContext *ctx,const XrXirArtifact *definition,uint32_t count) {
    const XrXirModule *module=&definition->module;const XrXirDeclarations *d=module->declarations;
    if (module->stage!=XR_XIR_CHECKED || !module->generics || !d || xir_effect_evidence_is_instance(module))
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"literal receiving requires a genuine generic definition inventory");
    if (module->function_count>ctx->compile.limits.functions || count>ctx->compile.limits.functions-module->function_count)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"literal receiving function inventory exhausted");
    ctx->literal_definition=module;ctx->linkage_kind=module->linkage_kind;
    ctx->function_count=module->function_count;ctx->function_capacity=module->function_count+count;
    ctx->functions=source_alloc(ctx,ctx->function_capacity,sizeof(*ctx->functions));
    ctx->identities=source_alloc(ctx,ctx->function_capacity,sizeof(*ctx->identities));
    ctx->generics=source_alloc(ctx,ctx->function_capacity,sizeof(*ctx->generics));
    ctx->bodies=source_alloc(ctx,ctx->function_capacity,sizeof(*ctx->bodies));
    if (!ctx->functions || !ctx->identities || !ctx->generics || !ctx->bodies ||
        !source_copy_bytes(ctx,NULL,ctx->functions,module->functions,(size_t)module->function_count*sizeof(*ctx->functions)) ||
        !source_copy_bytes(ctx,NULL,ctx->identities,d->functions,(size_t)module->function_count*sizeof(*ctx->identities)) ||
        !source_copy_bytes(ctx,NULL,ctx->generics,module->generics,(size_t)module->function_count*sizeof(*ctx->generics))) return false;
    ctx->has_generics=true;ctx->modules=(XrXirSourceModule *)d->modules;ctx->module_count=d->module_count;
    ctx->slots=(XrXirSlot *)d->slots;ctx->slot_count=d->slot_count;
    ctx->literals=(XrXirLiteral *)d->literals;ctx->literal_count=ctx->literal_capacity=d->literal_count;
    ctx->entry_function=d->entry_function;
    if (module->types) {
        ctx->types=*module->types;ctx->type_capacity=ctx->types.count;
        if (ctx->types.count) {
            XrXirTypeNode *nodes=source_alloc(ctx,ctx->types.count,sizeof(*nodes));
            if (!nodes || !source_copy_bytes(ctx,NULL,nodes,ctx->types.nodes,(size_t)ctx->types.count*sizeof(*nodes))) return false;
            ctx->types.nodes=nodes;
        }
        if (module->types->nominals) { ctx->nominals=*module->types->nominals;ctx->types.nominals=&ctx->nominals; }
        if (module->types->interfaces) { ctx->interfaces=*module->types->interfaces;ctx->types.interfaces=&ctx->interfaces; }
    }
    ctx->remaining_instructions=ctx->compile.limits.instructions;ctx->remaining_blocks=ctx->compile.limits.blocks;
    for (uint32_t f=0;f<module->function_count;++f) {
        if (!source_work_units(ctx,NULL,sizeof(XrXirFunction)+4)) return false;
        const XrXirFunction *body=&module->functions[f];
        if (body->instruction_count>ctx->remaining_instructions || body->block_count>ctx->remaining_blocks)
            return source_fail(ctx,NULL,XR_XIR_BUDGET,"literal receiving body inventory exhausted");
        ctx->remaining_instructions-=body->instruction_count;ctx->remaining_blocks-=body->block_count;
        ctx->bodies[f].module=d->functions[f].module;
        const XrXirProvenance *proof=module->provenance;
        if (proof) {
            if (proof->kind!=XR_XIR_EVIDENCE_TEMPLATE || proof->contract_count!=module->function_count || !proof->contracts)
                return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"literal definition lost its current effect template");
            ctx->bodies[f].library_effect=&proof->contracts[f];ctx->bodies[f].checked_library=definition;
        }
    }
    ctx->materialization_begin=ctx->function_capacity;return true;
}
static bool literal_binding(SourceContext *ctx,const XirCtfeInstanceView *view,XirLiteralBinding *output) {
    const XrXirModule *closed=&view->checked->module;
    const XrXirOrigin *origin=&closed->provenance->origins[view->instance_function];
    if (!source_work_units(ctx,NULL,sizeof(*output)+sizeof(*origin))) return false;
    if (view->definition_function>=ctx->function_count || origin->function!=view->definition_function ||
        !origin->argument_count || origin->argument_count!=ctx->generics[origin->function].parameter_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"literal receiving lost its actual definition or ordered substitution");
    output->definition=origin->function;output->instance=view->instance_function;output->argument_count=origin->argument_count;
    XrXirStatus status=XR_XIR_OK;
    XrXirType *arguments=xir_compile_calloc(&ctx->compile,origin->argument_count,sizeof(*arguments),&status);
    if (!arguments) return source_fail(ctx,NULL,status,"literal substitution receiving failed");
    output->arguments=arguments;
    const XrXirTypes *from=closed->types;
    SourceInstanceTypes importer={ctx,from,from && from->count?source_alloc(ctx,from->count,sizeof(XrXirType)):NULL};
    if (from && from->count && !importer.mapped) return false;
    for (uint32_t a=0;a<origin->argument_count;++a)
        if (!source_instance_argument(&importer,origin->arguments[a],0,&arguments[a])) return false;
    return true;
}
