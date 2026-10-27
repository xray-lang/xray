/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm_debug_source.inc.c - Immutable source boundaries at actual VM hooks
 */
#include "xxir_source_handler_validate.inc.c"
_Static_assert(_Alignof(DebugSourceHandler)<=_Alignof(uint32_t) &&
    _Alignof(DebugSourcePoint)<=_Alignof(uint32_t) &&
    _Alignof(XrXirSourceDebugThrow)<=_Alignof(uint32_t) &&
    _Alignof(XrXirSourceDebugSelector)<=_Alignof(uint32_t) &&
    _Alignof(XrXirSourceDebugRoute)<=_Alignof(uint32_t),"source table rows require four-byte alignment");
static void debug_source_clear(XrXirVmDebug *debug) {
    xr_compile_resources_free(debug->source_memory);
    debug->source_memory=NULL;debug->source_offsets=NULL;debug->source_points=NULL;debug->source_functions=0;
    debug->source_throw_offsets=NULL;debug->source_throws=NULL;
    debug->source_handler_offsets=NULL;debug->source_route_offsets=NULL;
    debug->source_handlers=NULL;debug->source_selectors=NULL;debug->source_routes=NULL;
    debug->step_source=(DebugSourcePoint){0};
}
static void debug_source_throw(const XrXirVmDebug *debug,uint32_t function,uint32_t instruction,uint32_t *operand) {
    if (!debug->source_memory || function>=debug->source_functions) return;
    uint32_t first=debug->source_throw_offsets[function],end=debug->source_throw_offsets[function+1];
    while (first<end) {
        uint32_t middle=first+(end-first)/2;
        const XrXirSourceDebugThrow *site=&debug->source_throws[middle];
        if (site->instruction==instruction) { *operand=site->operand;return; }
        if (site->instruction<instruction) first=middle+1;else end=middle;
    }
}
static const XrXirSourceDebugRoute *debug_source_route(const XrXirVmDebug *debug,uint32_t function,uint32_t instruction) {
    if (!debug->source_memory || function>=debug->source_functions) return NULL;
    uint32_t first=debug->source_route_offsets[function],end=debug->source_route_offsets[function+1];
    while (first<end) {
        uint32_t middle=first+(end-first)/2;const XrXirSourceDebugRoute *route=&debug->source_routes[middle];
        if (route->instruction==instruction) return route;
        if (route->instruction<instruction) first=middle+1;else end=middle;
    }
    return NULL;
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
    uint64_t instructions=0,throws=0,handlers=0,routes=0,selectors=0;
    for (uint32_t f=0;f<source->function_count;++f) {
        const XrXirSourceDebugFunction *function=&source->functions[f];
        if (function->function!=f || function->parameter_count!=module->functions[f].parameter_count ||
            function->instruction_count!=module->functions[f].instruction_count ||
            function->throw_count>function->instruction_count || (!!function->throw_count!=!!function->throw_sites) ||
            (function->throw_count && !function->points)) return XR_XIR_BAD_STRUCTURE;
        XrXirStatus checked=source_debug_handler_validate(function,&module->functions[f]);
        if (checked!=XR_XIR_OK) return checked;
        instructions+=function->instruction_count;throws+=function->throw_count;
        handlers+=function->handler_count;routes+=function->route_count;
        for (uint32_t h=0;h<function->handler_count;++h) selectors+=function->handlers[h].selector_count;
        if (instructions>UINT32_MAX || throws>UINT32_MAX || handlers>UINT32_MAX || routes>UINT32_MAX || selectors>UINT32_MAX)
            return XR_XIR_BUDGET;
    }
    uint64_t offsets=((uint64_t)source->function_count+1)*sizeof(uint32_t);
    uint64_t bytes=offsets*4+instructions*sizeof(DebugSourcePoint)+throws*sizeof(XrXirSourceDebugThrow)+
        handlers*sizeof(DebugSourceHandler)+routes*sizeof(XrXirSourceDebugRoute)+selectors*sizeof(XrXirSourceDebugSelector);
    if (bytes>SIZE_MAX) return XR_XIR_OVERFLOW;
    void *memory=NULL;
    XrCompileResourceStatus allocated=xr_compile_resources_alloc(context->resources,(size_t)bytes,&memory);
    if (allocated!=XR_COMPILE_RESOURCE_OK) return xir_compile_resource_status(allocated);
    uint32_t *starts=memory,*throw_starts=(uint32_t *)((unsigned char *)memory+(size_t)offsets);
    uint32_t *handler_starts=(uint32_t *)((unsigned char *)memory+(size_t)offsets*2);
    uint32_t *route_starts=(uint32_t *)((unsigned char *)memory+(size_t)offsets*3);
    DebugSourcePoint *points=(DebugSourcePoint *)((unsigned char *)memory+(size_t)offsets*4);
    XrXirSourceDebugThrow *sites=(XrXirSourceDebugThrow *)(points+instructions);
    DebugSourceHandler *handler_rows=(DebugSourceHandler *)(sites+throws);
    XrXirSourceDebugRoute *route_rows=(XrXirSourceDebugRoute *)(handler_rows+handlers);
    XrXirSourceDebugSelector *selector_rows=(XrXirSourceDebugSelector *)(route_rows+routes);
    uint32_t at=0,throw_at=0,handler_at=0,route_at=0,selector_at=0;
    for (uint32_t f=0;f<source->function_count;++f) {
        starts[f]=at;
        const XrXirSourceDebugFunction *function=&source->functions[f];
        throw_starts[f]=throw_at;
        handler_starts[f]=handler_at;route_starts[f]=route_at;
        for (uint32_t h=0;h<function->handler_count;++h) {
            const XrXirSourceDebugHandler *handler=&function->handlers[h];
            handler_rows[handler_at++]=(DebugSourceHandler){handler->error,handler->parent,handler->propagation,selector_at,handler->selector_count};
            memcpy(selector_rows+selector_at,handler->selectors,(size_t)handler->selector_count*sizeof(*selector_rows));
            selector_at+=handler->selector_count;
        }
        if (function->route_count) memcpy(route_rows+route_at,function->routes,(size_t)function->route_count*sizeof(*route_rows));
        route_at+=function->route_count;
        for (uint32_t t=0;t<function->throw_count;++t) {
            const XrXirSourceDebugThrow *site=&function->throw_sites[t];const XrXirFunction *body=&module->functions[f];
            if (site->instruction>=body->instruction_count ||
                (uint64_t)site->operand>=(uint64_t)body->parameter_count+body->instruction_count ||
                (t && site->instruction<=function->throw_sites[t-1].instruction) ||
                !function->points[site->instruction].statement) { xr_compile_resources_free(memory);return XR_XIR_BAD_STRUCTURE; }
            const XrXirInstruction *op=&body->instructions[site->instruction];
            XrXirType type=xr_xir_operand_type(body,site->operand);
            if ((op->op!=XR_XIR_THROW && op->op!=XR_XIR_JUMP && op->op!=XR_XIR_CLEANUP_ERROR) ||
                (op->op!=XR_XIR_JUMP && op->args[0]!=site->operand) ||
                (type!=XR_XIR_ERROR && !xr_xir_type_is_enum(module->types,type))) {
                xr_compile_resources_free(memory);return XR_XIR_BAD_STRUCTURE;
            }
            sites[throw_at++]=*site;
        }
        for (uint32_t i=0;i<function->instruction_count;++i) {
            XrXirSourceDebugPoint point=function->points ? function->points[i] : (XrXirSourceDebugPoint){0};
            if ((point.flags & ~(XR_XIR_SOURCE_DEBUG_ENTRY|XR_XIR_SOURCE_DEBUG_HANDLER_ACCEPT)) || (point.flags && !point.statement) ||
                (point.statement && (point.range.line<=0 || point.range.column<=0))) {
                xr_compile_resources_free(memory);return XR_XIR_BAD_STRUCTURE;
            }
            points[at++]=(DebugSourcePoint){point.range.module,point.statement,(uint32_t)point.range.line,
                point.flags|(function->points ? DEBUG_SOURCE_AUTHORED : 0)};
        }
    }
    starts[source->function_count]=at;
    throw_starts[source->function_count]=throw_at;
    handler_starts[source->function_count]=handler_at;route_starts[source->function_count]=route_at;
    debug->source_memory=memory;debug->source_offsets=starts;debug->source_points=points;
    debug->source_throw_offsets=throw_starts;debug->source_throws=sites;
    debug->source_handler_offsets=handler_starts;debug->source_route_offsets=route_starts;
    debug->source_handlers=handler_rows;debug->source_routes=route_rows;debug->source_selectors=selector_rows;
    debug->source_functions=source->function_count;return XR_XIR_OK;
}
