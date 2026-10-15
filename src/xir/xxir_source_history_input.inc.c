/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_history_input.inc.c - Exact published Checked units in the Source graph
 *
 * KEY CONCEPT:
 *   History views borrow a real record from this owner. They are not source
 *   reparses or Catalog libraries, and their identity is not a storage proof.
 */
static const XrXirModule *source_history_module(SourceContext *ctx,const SourceHistoryUnit *unit) {
    if (!unit || !ctx->self_unit || !unit->published || !unit->owner ||
        unit->owner!=ctx->self_unit->owner || !unit->serial || unit->serial>=ctx->self_unit->serial ||
        !unit->checked || !unit->semantic || !unit->semantic->graph) {
        source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"history requires this owner's exact published Source record");return NULL;
    }
    const XrXirCompileContext *owner=xr_xir_compile_artifact_context(unit->checked);
    const XrXirModule *module=xr_xir_compile_artifact_module(unit->checked);
    if (!owner || owner->resources!=ctx->compile.resources ||
        unit->semantic->graph->resources!=ctx->compile.resources || !module ||
        module->stage!=XR_XIR_CHECKED || module->linkage_kind!=XR_XIR_PROGRAM || !module->declarations ||
        unit->entry_module>=unit->semantic->module_count ||
        unit->entry_module!=module->declarations->root_module) {
        source_fail(ctx,NULL,XR_XIR_BAD_STAGE,"history Checked owner, stage or module closure disagrees");return NULL;
    }
    XrXirStatus status=xr_xir_compile_artifact_verify(unit->checked,NULL);
    if (status!=XR_XIR_OK) {source_fail(ctx,NULL,status,"history Checked verification failed");return NULL;}
    return module;
}
static void source_history_spec_free(XrModuleSpec *spec) {
    xr_compile_resources_free(spec->canonical);
    xr_compile_resources_free(spec->dep_indices);
}
static bool source_history_input(SourceContext *ctx) {
    if (!ctx->history_input) return true;
    const SourceHistoryUnit *unit=ctx->history_input;
    const XrXirModule *module=source_history_module(ctx,unit);
    if (!module) return false;
    uint32_t count=module->declarations->module_count,first=(uint32_t)ctx->graph->spec_count;
    if (count>XR_MODULE_GRAPH_MAX_MODULES || first>XR_MODULE_GRAPH_MAX_MODULES-count)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"history module closure exhausted");
    XrModuleSpec *specs=source_semantic_alloc(ctx,first+count,sizeof(*specs));
    if (!specs) return false;
    if (!source_copy_bytes(ctx,NULL,specs,ctx->graph->specs,first*sizeof(*specs))) goto failed;
    for (uint32_t m=0;m<count;++m) {
        const XrXirSourceModule *original=&module->declarations->modules[m];
        XrModuleSpec *spec=&specs[first+m];
        if (!source_work(ctx,NULL)) goto failed;
        char *name=source_semantic_alloc(ctx,(size_t)original->name_length+1,1);
        if (!name) goto failed;
        spec->canonical=name;
        if (!source_copy_bytes(ctx,NULL,name,original->name,original->name_length)) goto failed;
        spec->kind=XR_MOD_HISTORY;spec->representation=XR_MODULE_CHECKED_HISTORY;
        spec->history=unit;spec->history_module=m;spec->status=XR_MODSPEC_RESOLVED;
        if (m<unit->semantic->module_count)
            spec->source_content_fingerprint=unit->semantic->graph->specs[m].source_content_fingerprint;
        if (original->dependency_count>INT_MAX) {
            source_fail(ctx,NULL,XR_XIR_BUDGET,"history dependency arity exhausted");goto failed;
        }
        spec->dep_count=spec->dep_capacity=(int)original->dependency_count;
        spec->dep_indices=spec->dep_count ? source_semantic_alloc(ctx,original->dependency_count,sizeof(*spec->dep_indices)) : NULL;
        if (spec->dep_count && !spec->dep_indices) goto failed;
        for (uint32_t d=0;d<original->dependency_count;++d) {
            if (!source_work(ctx,NULL)) goto failed;
            if (original->dependencies[d]>=count) {
                source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"history dependency has a foreign module ordinal");goto failed;
            }
            spec->dep_indices[d]=(int)(first+original->dependencies[d]);
        }
    }
    SourceHistoryReceive *received=source_alloc(ctx,1,sizeof(*received));
    if (!received) goto failed;
    received->unit=unit;
    /* Current source lookup remains in the resolver's canonical index. History
     * is selected by exact unit/module pairs and never by a filename alias. */
    xr_compile_resources_free(ctx->graph->specs);ctx->graph->specs=specs;
    ctx->graph->spec_count=ctx->graph->spec_capacity=(int)(first+count);ctx->history=received;
    return true;
failed:
    for (uint32_t m=0;m<count;++m) source_history_spec_free(&specs[first+m]);
    xr_compile_resources_free(specs);return false;
}
