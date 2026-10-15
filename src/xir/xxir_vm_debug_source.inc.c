/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm_debug_source.inc.c - Immutable source boundaries at actual VM hooks
 */
static void debug_source_clear(XrXirVmDebug *debug) {
    xr_compile_resources_free(debug->source_memory);
    debug->source_memory=NULL;debug->source_offsets=NULL;debug->source_points=NULL;debug->source_functions=0;
    debug->step_source=(DebugSourcePoint){0};
}
static DebugSourcePoint debug_source_point(const XrXirVmDebug *debug,uint32_t function,uint32_t instruction) {
    if (!debug->source_memory || function>=debug->source_functions ||
        instruction>=debug->source_offsets[function+1]-debug->source_offsets[function]) return (DebugSourcePoint){0};
    return debug->source_points[debug->source_offsets[function]+instruction];
}
static bool debug_source_boundary(const XrXirVmDebug *debug,const DebugFrame *frame,const XirDebugCallFrame *call,unsigned mode) {
    bool line=mode>=XR_XIR_VM_DEBUG_LINE_IN;
    unsigned base=line ? mode-(XR_XIR_VM_DEBUG_LINE_IN-XR_XIR_VM_DEBUG_STATEMENT_IN) : mode;
    if ((base==XR_XIR_VM_DEBUG_STATEMENT_OVER && call->event.depth>debug->step_depth) ||
        (base==XR_XIR_VM_DEBUG_STATEMENT_OUT && call->event.depth>=debug->step_depth)) return false;
    const XirVmDebugPoint *point=&frame->point;
    DebugSourcePoint current=debug_source_point(debug,point->binding->function,point->instruction);
    if (!current.statement) return false;
    if (debug->step_source.statement && frame->id==debug->skip_id && !(current.flags & XR_XIR_SOURCE_DEBUG_ENTRY)) return false;
    return !debug->step_source.statement || frame->id!=debug->skip_id ||
        current.module!=debug->step_source.module ||
        (line ? current.line!=debug->step_source.line : current.statement!=debug->step_source.statement) ||
        point->instruction<=debug->skip_instruction;
}
XR_FUNC XrXirStatus xir_vm_debug_source_map(XrXirVmDebug *debug,const XrXirCompileContext *context,
    const XrXirSourceDebugView *source) {
    if (!debug_valid(debug) || !debug->artifact || debug->target || debug->source_memory || !context ||
        context->resources!=xr_xir_compile_artifact_context(debug->artifact)->resources || !source || !source->functions)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirModule *module=xr_xir_compile_artifact_module(debug->artifact);
    if (source->function_count!=module->function_count || source->function_count==UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    uint64_t instructions=0;
    for (uint32_t f=0;f<source->function_count;++f) {
        if (xr_compile_resources_work(context->resources,1)!=XR_COMPILE_RESOURCE_OK) return XR_XIR_BUDGET;
        const XrXirSourceDebugFunction *function=&source->functions[f];
        if (function->function!=f || function->parameter_count!=module->functions[f].parameter_count ||
            function->instruction_count!=module->functions[f].instruction_count) return XR_XIR_BAD_STRUCTURE;
        instructions+=function->instruction_count;
        if (instructions>UINT32_MAX) return XR_XIR_BUDGET;
    }
    uint64_t offsets=((uint64_t)source->function_count+1)*sizeof(uint32_t);
    uint64_t bytes=offsets+instructions*sizeof(DebugSourcePoint);
    uint64_t work=bytes+instructions*(sizeof(XrXirSourceDebugPoint)+1);
    if (bytes>SIZE_MAX || xr_compile_resources_work(context->resources,work)!=XR_COMPILE_RESOURCE_OK) return XR_XIR_BUDGET;
    void *memory=NULL;
    XrCompileResourceStatus allocated=xr_compile_resources_alloc(context->resources,(size_t)bytes,&memory);
    if (allocated!=XR_COMPILE_RESOURCE_OK) return allocated==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    uint32_t *starts=memory;DebugSourcePoint *points=(DebugSourcePoint *)((unsigned char *)memory+(size_t)offsets);
    uint32_t at=0;
    for (uint32_t f=0;f<source->function_count;++f) {
        starts[f]=at;
        const XrXirSourceDebugFunction *function=&source->functions[f];
        for (uint32_t i=0;i<function->instruction_count;++i) {
            XrXirSourceDebugPoint point=function->points ? function->points[i] : (XrXirSourceDebugPoint){0};
            if ((point.flags & ~XR_XIR_SOURCE_DEBUG_ENTRY) || (point.flags && !point.statement) ||
                (point.statement && (point.range.line<=0 || point.range.column<=0))) {
                xr_compile_resources_free(memory);return XR_XIR_BAD_STRUCTURE;
            }
            points[at++]=(DebugSourcePoint){point.range.module,point.statement,(uint32_t)point.range.line,point.flags};
        }
    }
    starts[source->function_count]=at;
    debug->source_memory=memory;debug->source_offsets=starts;debug->source_points=points;
    debug->source_functions=source->function_count;return XR_XIR_OK;
}
