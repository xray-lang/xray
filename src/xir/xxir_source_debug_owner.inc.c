/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_debug_owner.inc.c - Owned source instruction observations
 */
#include "xxir_source_handler_owner.inc.c"
static XrXirStatus source_debug_throw_validate(SourceQueryCopy *copy,
    const XrXirSourceDebugFunction *function,const XrXirModule *module,uint32_t f) {
    if (function->throw_count>function->instruction_count || (!!function->throw_count!=!!function->throw_sites) ||
        (function->throw_count && !function->points)) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *body=&module->functions[f];
    if (function->throw_count && (!body->instructions || !module->types ||
        (body->parameter_count && !body->parameters))) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t t=0;t<function->throw_count;++t) {
        if (!query_work(copy,sizeof(*function->throw_sites))) return copy->status;
        const XrXirSourceDebugThrow *site=&function->throw_sites[t];
        if (site->instruction>=body->instruction_count ||
            (uint64_t)site->operand>=(uint64_t)body->parameter_count+body->instruction_count ||
            (t && site->instruction<=function->throw_sites[t-1].instruction) ||
            !function->points[site->instruction].statement) return XR_XIR_BAD_STRUCTURE;
        const XrXirInstruction *op=&body->instructions[site->instruction];
        if ((op->op!=XR_XIR_THROW && op->op!=XR_XIR_JUMP && op->op!=XR_XIR_CLEANUP_ERROR) ||
            (op->op!=XR_XIR_JUMP && op->args[0]!=site->operand)) return XR_XIR_BAD_STRUCTURE;
        XrXirProofContext proof={module,{XR_XIR_CONTEXT_FUNCTION,f,0}};
        XrXirStatus status=xr_xir_compile_type_markers_prove(&copy->snapshot->context,&proof,
            xr_xir_operand_type(body,site->operand),XR_XIR_CONSTRAINT_ERROR);
        if (status!=XR_XIR_OK)
            return status==XR_XIR_BUDGET || status==XR_XIR_OUT_OF_MEMORY ? status : XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_compile_source_snapshot_debug_copy(XrXirSourceSnapshot *snapshot,
    const XrXirSourceDebugView *debug,const XrXirModule *module) {
    if (!snapshot || snapshot->debug_ready || !debug || !module || !snapshot->view.complete ||
        debug->function_count!=module->function_count ||
        (debug->function_count && (!debug->functions || !module->functions)) ||
        (!!debug->global_count != !!debug->globals) ||
        (debug->global_count && (!module->declarations || !module->declarations->slots || !module->declarations->modules ||
         debug->global_count>module->declarations->slot_count))) return XR_XIR_BAD_STRUCTURE;
    SourceQueryCopy copy={snapshot,XR_XIR_OK};
    for (uint32_t g=0;g<debug->global_count;++g) {
        if (!query_work(&copy,sizeof(*debug->globals))) return copy.status;
        const XrXirSourceDebugGlobal *global=&debug->globals[g];
        if (!global->declaration || global->declaration>snapshot->view.declaration_count ||
            global->slot>=module->declarations->slot_count ||
            (g && (global->slot<=debug->globals[g-1].slot ||
             global->declaration<=debug->globals[g-1].declaration))) return XR_XIR_BAD_STRUCTURE;
        const XrXirSourceDeclaration *declaration=&snapshot->view.declarations[global->declaration-1];
        const XrXirSlot *slot=&module->declarations->slots[global->slot];
        if (declaration->kind!=XR_XIR_SOURCE_BINDING || declaration->parent || !declaration->type.known ||
            declaration->range.module>=snapshot->view.module_count || slot->module>=module->declarations->module_count ||
            slot->mutable>1 || declaration->mutable!=!!slot->mutable ||
            declaration->type.type!=(slot->mutable ? xr_xir_cell_element(module->types,slot->type) : slot->type) ||
            (slot->mutable && !xr_xir_type_is_cell(module->types,slot->type))) return XR_XIR_BAD_STRUCTURE;
        const XrXirSourceModule *owner=&module->declarations->modules[slot->module];
        const char *identity=snapshot->view.modules[declaration->range.module].identity;
        size_t bytes=query_length(&copy,identity);
        if (!query_work(&copy,bytes)) return copy.status;
        if (!bytes || !owner->name || bytes-1!=owner->name_length || memcmp(identity,owner->name,bytes-1)) return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t f=0;f<debug->function_count;++f) {
        if (!query_work(&copy,1)) return copy.status;
        const XrXirSourceDebugFunction *function=&debug->functions[f];
        if (function->function!=f || function->declaration>snapshot->view.declaration_count ||
            function->parameter_count!=module->functions[f].parameter_count ||
            function->instruction_count!=module->functions[f].instruction_count ||
            (function->binding_count && !function->bindings)) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t b=0;b<function->binding_count;++b) {
            if (!query_work(&copy,1)) return copy.status;
            const XrXirSourceDebugBinding *binding=&function->bindings[b];
            if (!binding->declaration || binding->declaration>snapshot->view.declaration_count || binding->parent>b ||
                (binding->flags & ~(XR_XIR_SOURCE_DEBUG_UNIT|XR_XIR_SOURCE_DEBUG_CELL|XR_XIR_SOURCE_DEBUG_UNAVAILABLE)) ||
                (binding->flags && (binding->flags & (binding->flags-1))) ||
                ((binding->flags & XR_XIR_SOURCE_DEBUG_UNAVAILABLE) &&
                 snapshot->view.declarations[binding->declaration-1].kind!=XR_XIR_SOURCE_BINDING) ||
                ((binding->flags & (XR_XIR_SOURCE_DEBUG_UNIT|XR_XIR_SOURCE_DEBUG_UNAVAILABLE)) ? binding->value!=UINT32_MAX :
                 (uint64_t)binding->value>=(uint64_t)function->parameter_count+function->instruction_count)) return XR_XIR_BAD_STRUCTURE;
        }
        XrXirStatus throws=source_debug_throw_validate(&copy,function,module,f);
        if (throws!=XR_XIR_OK) return throws;
        XrXirStatus handlers=source_debug_handler_validate(snapshot->context.resources,function,&module->functions[f]);
        if (handlers!=XR_XIR_OK) return handlers;
        if (!function->points) continue;
        for (uint32_t i=0;i<function->instruction_count;++i) {
            if (!query_work(&copy,1)) return copy.status;
            const XrXirSourceDebugPoint *point=&function->points[i];
            const XrXirSourceRange *range=&point->range;
            if ((point->flags & ~(XR_XIR_SOURCE_DEBUG_ENTRY|XR_XIR_SOURCE_DEBUG_HANDLER_ACCEPT)) || (point->flags && !point->statement) ||
                point->environment>function->binding_count) return XR_XIR_BAD_STRUCTURE;
            if (point->statement && (range->module>=snapshot->view.module_count || range->line<=0 || range->column<=0 ||
                (range->end_line && (range->end_line<range->line || range->end_column<=0 ||
                 (range->end_line==range->line && range->end_column<range->column))))) return XR_XIR_BAD_STRUCTURE;
        }
    }
    SourceQueryMemory *previous=snapshot->memory;
    XrXirSourceDebugGlobal *globals=query_copy(&copy,debug->globals,debug->global_count,sizeof(*globals));
    XrXirSourceDebugFunction *functions=query_copy(&copy,debug->functions,debug->function_count,sizeof(*functions));
    for (uint32_t f=0;functions && f<debug->function_count && copy.status==XR_XIR_OK;++f) {
        if (debug->functions[f].points)
            functions[f].points=query_copy(&copy,debug->functions[f].points,
                debug->functions[f].instruction_count,sizeof(*functions[f].points));
        if (debug->functions[f].binding_count)
            functions[f].bindings=query_copy(&copy,debug->functions[f].bindings,
                debug->functions[f].binding_count,sizeof(*functions[f].bindings));
        if (debug->functions[f].throw_count)
            functions[f].throw_sites=query_copy(&copy,debug->functions[f].throw_sites,
                debug->functions[f].throw_count,sizeof(*functions[f].throw_sites));
        source_debug_handler_copy(&copy,&debug->functions[f],&functions[f]);
    }
    if (!query_work(&copy,sizeof(snapshot->debug)+sizeof(snapshot->debug_ready))) {
        while (snapshot->memory!=previous) {
            SourceQueryMemory *memory=snapshot->memory;snapshot->memory=memory->next;xr_compile_resources_free(memory);
        }
        return copy.status;
    }
    snapshot->debug=(XrXirSourceDebugView){functions,debug->function_count,globals,debug->global_count};snapshot->debug_ready=true;
    return XR_XIR_OK;
}
XR_FUNC const XrXirSourceDebugView *xr_xir_compile_source_snapshot_debug(const XrXirSourceSnapshot *snapshot) {
    return snapshot && snapshot->debug_ready ? &snapshot->debug : NULL;
}
