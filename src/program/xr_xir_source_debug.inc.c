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
    product->debug=(XrXirSourceDebugView){functions,module->function_count};return XR_XIR_OK;
}
XR_FUNC const XrXirSourceDebugView *xr_xir_compile_source_product_debug(const XrXirSourceProduct *product) {
    return product ? &product->debug : NULL;
}
