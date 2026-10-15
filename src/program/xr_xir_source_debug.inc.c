/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xir_source_debug.inc.c - Source coordinates through verified specialization
 */
static XrXirStatus source_product_debug(XrXirSourceProduct *product,const XrXirSourceSnapshot *snapshot,
    const XrXirCheckedPacket *source_packet,const XrXirModule *module) {
    const XrXirCompileContext *context=&product->context;
    const XrXirSourceDebugView *source=xr_xir_compile_source_snapshot_debug(snapshot);
    if (!source || (source->function_count && !source->functions)) return XR_XIR_BAD_STRUCTURE;
    const XrXirProvenance *provenance=module->provenance;
    bool instances=provenance && provenance->kind==XR_XIR_EVIDENCE_INSTANCE;
    XrXirCheckedPacket verified={0};XrXirStatus status=XR_XIR_OK;
    if (instances) {
        if (!provenance->source || provenance->count!=module->function_count || !provenance->origins)
            return XR_XIR_BAD_STRUCTURE;
        status=xr_xir_compile_checked_write(provenance->source,&verified,NULL);
        if (status==XR_XIR_OK && verified.length!=source_packet->length) status=XR_XIR_BAD_STRUCTURE;
        if (status==XR_XIR_OK)
            status=source_product_compare(context,verified.bytes,source_packet->bytes,verified.length);
        xr_xir_compile_checked_packet_free(&verified);
        if (status!=XR_XIR_OK) return status;
    } else if (source->function_count!=module->function_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirSourceView *query=xr_xir_compile_source_snapshot_view(snapshot);
    if ((source->global_count && (!source->globals || !module->declarations || !module->declarations->slots || !module->declarations->modules)) ||
        !query || !module->declarations) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t g=0;g<source->global_count;++g) {
        if (!xir_compile_work(context,sizeof(*source->globals))) return XR_XIR_BUDGET;
        const XrXirSourceDebugGlobal *global=&source->globals[g];
        if (global->slot>=module->declarations->slot_count || !global->declaration ||
            global->declaration>query->declaration_count) return XR_XIR_BAD_STRUCTURE;
        const XrXirSourceDeclaration *declaration=&query->declarations[global->declaration-1];
        const XrXirSlot *slot=&module->declarations->slots[global->slot];
        if (declaration->range.module>=query->module_count || slot->module>=module->declarations->module_count ||
            slot->mutable>1 || !!slot->mutable!=declaration->mutable) return XR_XIR_BAD_STRUCTURE;
        const XrXirSourceModule *owner=&module->declarations->modules[slot->module];
        const char *identity=query->modules[declaration->range.module].identity;
        if (!identity || !owner->name) return XR_XIR_BAD_STRUCTURE;
        size_t length=0;
        while (identity[length]) { if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;++length; }
        if (!xir_compile_work(context,length+1)) return XR_XIR_BUDGET;
        if (length!=owner->name_length || memcmp(identity,owner->name,length)) return XR_XIR_BAD_STRUCTURE;
    }
    XrXirSourceDebugFunction *functions=xir_compile_calloc(context,module->function_count,sizeof(*functions),&status);
    if (module->function_count && !functions) return status;
    for (uint32_t f=0;f<module->function_count;++f) {
        if (!xir_compile_work(context,sizeof(*functions))) { status=XR_XIR_BUDGET;break; }
        uint32_t origin=instances ? provenance->origins[f].function : f;
        if (origin>=source->function_count) { status=XR_XIR_BAD_STRUCTURE;break; }
        const XrXirSourceDebugFunction *original=&source->functions[origin];
        if (original->parameter_count!=module->functions[f].parameter_count ||
            original->instruction_count!=module->functions[f].instruction_count) { status=XR_XIR_BAD_STRUCTURE;break; }
        functions[f]=*original;functions[f].function=f;
    }
    if (status!=XR_XIR_OK) { xr_compile_resources_free(functions);return status; }
    product->debug=(XrXirSourceDebugView){functions,module->function_count,source->globals,source->global_count};return XR_XIR_OK;
}
XR_FUNC const XrXirSourceDebugView *xr_xir_compile_source_product_debug(const XrXirSourceProduct *product) {
    return product ? &product->debug : NULL;
}
