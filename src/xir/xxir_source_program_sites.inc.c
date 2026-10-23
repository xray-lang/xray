/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_program_sites.inc.c - Complete current symbolic occurrence inventory
 *
 * KEY CONCEPT:
 *   Uninstantiated sites survive alongside actually executed closed keys.
 *   Source coordinates identify original calls, never inferred permissions.
 */
static bool source_program_sites(SourceContext *ctx,const XrXirModule *module,XrXirComptimeTable *table) {
    if (!module->generics) return true;
    uint32_t count=0;
    for (const SourceComptimeSite *s=ctx->comptime_sites;s;s=s->next) {
        if (!source_work_units(ctx,s->syntax,4)) return false;
        if (module->generics[s->helper].parameter_count) ++count;
    }
    XrXirComptimeSite *sites=count?source_alloc(ctx,count,sizeof(*sites)):NULL;
    if (count && !sites) return false;
    uint32_t next=0;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *body=&module->functions[f];
        for (uint32_t i=0;i<body->instruction_count;++i) {
            if (!source_work_units(ctx,NULL,sizeof(XrXirInstruction))) return false;
            const XrXirInstruction *op=&body->instructions[i];
            if (op->op!=XR_XIR_CALL && op->op!=XR_XIR_INVOKE) continue;
            for (const SourceComptimeSite *s=ctx->comptime_sites;s;s=s->next) {
                if (!source_work_units(ctx,s->syntax,4)) return false;
                if (s->owner!=f || op->immediate!=s->helper || !module->generics[s->helper].parameter_count) continue;
                if (next==count) return source_fail(ctx,s->syntax,XR_XIR_BAD_STRUCTURE,"comptime source call inventory grew");
                sites[next++]=(XrXirComptimeSite){f,i,s->helper};
            }
        }
    }
    if (next!=count) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"comptime source call inventory is incomplete");
    table->sites=sites;table->site_count=count;return true;
}
static const SourceComptimeSite *source_program_syntax(SourceContext *ctx,uint32_t helper) {
    for (const SourceComptimeSite *site=ctx->comptime_sites;site;site=site->next) {
        if (!source_work_units(ctx,site->syntax,2)) return NULL;
        if (site->helper==helper) return site;
    }
    return NULL;
}
static bool source_program_failure(SourceContext *ctx,uint32_t helper,XrXirCtfeStatus status) {
    const SourceComptimeSite *site=source_program_syntax(ctx,helper);
    if (ctx->diagnostic.status!=XR_XIR_OK) return false;
    if (site) ctx->module=site->module;
    else if (helper<ctx->function_count) ctx->module=ctx->identities[helper].module;
    return source_comptime_failure(ctx,site?site->syntax:NULL,status);
}
static bool source_program_seal(SourceContext *ctx,const XrXirArtifact *source,XrXirArtifact **output) {
    XrXirComptimeTable tables[2]={source->construction->comptime,{0}},merged={0};
    if (!source_program_sites(ctx,&source->module,&tables[1]) || !source_comptime_table_merge(ctx,tables,2,&merged)) return false;
    const XrXirConstruction *original=source->construction;
    XrXirConstructionSpec input={original->rows,original->count,original->materializations,original->materialization_count,&merged};
    XrXirConstruction *construction=NULL;
    XrXirStatus status=xr_xir_compile_construction_new_v4(&ctx->compile,source->module.types,&input,
        xr_xir_compile_construction_slots(original),&construction);
    if (status==XR_XIR_OK) status=xr_xir_compile_recheck_v2(&ctx->compile,&source->module,construction,output,NULL);
    xr_xir_compile_construction_free(construction);
    return status==XR_XIR_OK || source_fail(ctx,NULL,status,"comptime canonical receiving failed complete checking");
}
