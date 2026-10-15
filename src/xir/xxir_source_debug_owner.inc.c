/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_debug_owner.inc.c - Owned source instruction observations
 */
XR_FUNC XrXirStatus xr_xir_compile_source_snapshot_debug_copy(XrXirSourceSnapshot *snapshot,
    const XrXirSourceDebugView *debug,const XrXirModule *module) {
    if (!snapshot || snapshot->debug_ready || !debug || !module || !snapshot->view.complete ||
        debug->function_count!=module->function_count ||
        (debug->function_count && (!debug->functions || !module->functions))) return XR_XIR_BAD_STRUCTURE;
    SourceQueryCopy copy={snapshot,XR_XIR_OK};
    for (uint32_t f=0;f<debug->function_count;++f) {
        if (!query_work(&copy,1)) return copy.status;
        const XrXirSourceDebugFunction *function=&debug->functions[f];
        if (function->function!=f || function->declaration>snapshot->view.declaration_count ||
            function->parameter_count!=module->functions[f].parameter_count ||
            function->instruction_count!=module->functions[f].instruction_count) return XR_XIR_BAD_STRUCTURE;
        if (!function->points) continue;
        for (uint32_t i=0;i<function->instruction_count;++i) {
            if (!query_work(&copy,1)) return copy.status;
            const XrXirSourceDebugPoint *point=&function->points[i];
            const XrXirSourceRange *range=&point->range;
            if (point->statement && (range->module>=snapshot->view.module_count || range->line<=0 || range->column<=0 ||
                (range->end_line && (range->end_line<range->line || range->end_column<=0 ||
                 (range->end_line==range->line && range->end_column<range->column))))) return XR_XIR_BAD_STRUCTURE;
        }
    }
    SourceQueryMemory *previous=snapshot->memory;
    XrXirSourceDebugFunction *functions=query_copy(&copy,debug->functions,debug->function_count,sizeof(*functions));
    for (uint32_t f=0;functions && f<debug->function_count && copy.status==XR_XIR_OK;++f) {
        if (debug->functions[f].points)
            functions[f].points=query_copy(&copy,debug->functions[f].points,
                debug->functions[f].instruction_count,sizeof(*functions[f].points));
    }
    if (!query_work(&copy,sizeof(snapshot->debug)+sizeof(snapshot->debug_ready))) {
        while (snapshot->memory!=previous) {
            SourceQueryMemory *memory=snapshot->memory;snapshot->memory=memory->next;xr_compile_resources_free(memory);
        }
        return copy.status;
    }
    snapshot->debug=(XrXirSourceDebugView){functions,debug->function_count};snapshot->debug_ready=true;
    return XR_XIR_OK;
}
XR_FUNC const XrXirSourceDebugView *xr_xir_compile_source_snapshot_debug(const XrXirSourceSnapshot *snapshot) {
    return snapshot && snapshot->debug_ready ? &snapshot->debug : NULL;
}
