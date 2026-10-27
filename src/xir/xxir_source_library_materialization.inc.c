/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_library_materialization.inc.c - Receiving literal-call bindings
 *
 * KEY CONCEPT:
 *   Complete current Checked verification precedes ordinary ordinal mapping.
 *   The receiver owns every selected edge and repeats complete checking.
 */
static bool source_library_materializations(SourceContext *ctx,const SourceLibraryMap *map) {
    if (!map->unit) return true;
    SourceLibraryUnit *unit=map->unit;
    uint32_t count=xr_xir_compile_construction_materialization_count(unit->construction);
    if (!count) return true;
    if (unit->materializations || unit->materialization_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library literal edges were installed twice");
    XrXirMaterializationBinding *rows=source_alloc(ctx,count,sizeof(*rows));
    if (!rows) return false;
    uint32_t next=0;
    for (uint32_t i=0;i<count;++i) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        const XrXirMaterializationBinding *from=xr_xir_compile_construction_materialization(unit->construction,i);
        if (!from || from->caller>=map->function_count || from->function>=map->function_count ||
            !unit->library->types || !unit->library->types->nominals || from->nominal>=unit->library->types->nominals->count)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library literal edge has no mapped declaration");
        if (map->functions[from->caller]==UINT32_MAX) continue;
        if (map->functions[from->function]==UINT32_MAX)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library literal target is outside its selected closure");
        rows[next++]=(XrXirMaterializationBinding){map->functions[from->caller],from->instruction,
            map->functions[from->function],unit->nominals[from->nominal]};
    }
    unit->materializations=rows;unit->materialization_count=next;return true;
}
