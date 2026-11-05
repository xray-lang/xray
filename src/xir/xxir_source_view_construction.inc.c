/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_view_construction.inc.c - Relocate immutable construction coordinates
 *
 * KEY CONCEPT:
 *   Lifetime insertion preserves helper identities but changes CALL ordinals.
 *   A fresh owner must pass complete ordinary admission before publication.
 */
static bool source_view_construction_coordinate(SourceContext *ctx,const XirViewNormalization *normalization,
    uint32_t function,uint32_t *instruction) {
    if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
    if (function>=ctx->function_count || *instruction>=ctx->functions[function].instruction_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"construction lifetime coordinate is invalid");
    const XirViewNormalizedFunction *map=&normalization->functions[function];
    if (!map->instructions) return true;
    uint32_t parameters=ctx->functions[function].parameter_count;
    uint32_t value=map->values[parameters+*instruction];
    if (value<parameters || value-parameters>=normalization->bodies[function].instruction_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"construction lifetime coordinate was not relocated");
    *instruction=value-parameters;return true;
}
static bool source_view_construction(SourceContext *ctx,const XirViewNormalization *normalization,
    XrXirConstruction **output) {
    const XrXirConstruction *old=ctx->construction;
    uint32_t count=xr_xir_compile_construction_count(old);
    uint32_t materializations=xr_xir_compile_construction_materialization_count(old);
    const XrXirComptimeTable *comptime=xr_xir_compile_construction_comptime(old);
    if (!old || !comptime || !output || *output)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"construction lifetime owner is missing");
    XrXirConstructionRow *rows=count?source_alloc(ctx,count,sizeof(*rows)):NULL;
    XrXirMaterializationBinding *bindings=materializations?source_alloc(ctx,materializations,sizeof(*bindings)):NULL;
    XrXirComptimeSite *sites=comptime->site_count?source_alloc(ctx,comptime->site_count,sizeof(*sites)):NULL;
    if ((count&&!rows) || (materializations&&!bindings) || (comptime->site_count&&!sites)) return false;
    for (uint32_t i=0;i<count;++i) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        const XrXirConstructionRow *row=xr_xir_compile_construction_row(old,i);
        if (!row) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"construction lifetime row is absent");
        rows[i]=*row;
    }
    for (uint32_t i=0;i<materializations;++i) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        const XrXirMaterializationBinding *binding=xr_xir_compile_construction_materialization(old,i);
        if (!binding) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"materializer lifetime binding is absent");
        bindings[i]=*binding;
        if (!source_view_construction_coordinate(ctx,normalization,bindings[i].caller,&bindings[i].instruction)) return false;
    }
    for (uint32_t i=0;i<comptime->site_count;++i) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        sites[i]=comptime->sites[i];
        if (!source_view_construction_coordinate(ctx,normalization,sites[i].caller,&sites[i].instruction)) return false;
    }
    XrXirComptimeTable table={sites,comptime->site_count,comptime->bindings,comptime->binding_count};
    XrXirConstructionSpec specification={rows,count,bindings,materializations,&table};
    XrXirStatus status=xr_xir_compile_construction_new_v4(&ctx->compile,&ctx->types,&specification,
        xr_xir_compile_construction_slots(old),output);
    return status==XR_XIR_OK || source_fail(ctx,NULL,status,"relocated construction ownership failed");
}
