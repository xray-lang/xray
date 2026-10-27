/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_library_comptime.inc.c - Owned relocation of canonical calls
 *
 * KEY CONCEPT:
 *   Selected module views retain all their functions, including providers
 *   referenced only by metadata. Original instruction coordinates stay exact.
 */
static bool source_library_comptime(SourceContext *ctx,const SourceLibraryMap *map) {
    SourceLibraryUnit *unit=map->unit;
    const XrXirComptimeTable *input=xr_xir_compile_construction_comptime(unit->construction);
    if (!input || unit->comptime.site_count || unit->comptime.binding_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library comptime inventory is missing or repeated");
    uint32_t *ordinals=input->site_count?source_alloc(ctx,input->site_count,sizeof(*ordinals)):NULL;
    XrXirComptimeSite *sites=input->site_count?source_alloc(ctx,input->site_count,sizeof(*sites)):NULL;
    XrXirComptimeBinding *rows=input->binding_count?source_alloc(ctx,input->binding_count,sizeof(*rows)):NULL;
    if ((input->site_count && (!ordinals || !sites)) || (input->binding_count && !rows)) return false;
    uint32_t site_count=0,binding_count=0;
    for (uint32_t i=0;i<input->site_count;++i) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        const XrXirComptimeSite *from=&input->sites[i];ordinals[i]=UINT32_MAX;
        if (from->caller>=map->function_count || from->helper>=map->function_count)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library comptime function is outside its inventory");
        if (map->functions[from->caller]==UINT32_MAX) continue;
        if (map->functions[from->helper]==UINT32_MAX)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library comptime helper is outside selected module views");
        ordinals[i]=site_count;
        sites[site_count++]=(XrXirComptimeSite){map->functions[from->caller],from->instruction,map->functions[from->helper]};
    }
    for (uint32_t i=0;i<input->binding_count;++i) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        const XrXirComptimeBinding *from=&input->bindings[i];
        if (from->site>=input->site_count || from->provider>=map->function_count)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library comptime binding is outside its inventory");
        if (ordinals[from->site]==UINT32_MAX) continue;
        if (map->functions[from->provider]==UINT32_MAX)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library comptime provider is outside selected module views");
        const XrXirType *arguments=NULL;
        if (!source_library_type_arguments(ctx,unit,from->arguments,from->argument_count,&arguments)) return false;
        rows[binding_count++]=(XrXirComptimeBinding){ordinals[from->site],arguments,from->argument_count,map->functions[from->provider]};
    }
    unit->comptime=(XrXirComptimeTable){site_count?sites:NULL,site_count,binding_count?rows:NULL,binding_count};
    return true;
}
