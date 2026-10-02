/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xir_source_product.c - Owned typed source to VM and native projections
 *
 * KEY CONCEPT:
 *   Producer destruction cannot invalidate code, queries or diagnostics.
 */
#include "xr_xir_source_product.h"
#include "../xir/xxir_vm.h"
#include "../xir/xxir_generic.h"
#include "../base/xsha256.h"
#include "../base/xmalloc.h"
#include <string.h>

struct XrXirSourceProduct {
    XrXirArtifact *lowered;
    XrXirSourceSnapshot *snapshot;
    XrXirCheckedPacket source_packet, closed_packet;
    XrXirSourceProductFacts facts;
};
static bool source_diagnostic_empty(const XrXirSourceProductDiagnostic *diagnostic) {
    return !diagnostic || !diagnostic->snapshot;
}
static XrXirStatus source_product_reject(XrXirSourceProductDiagnostic *diagnostic,
                                       XrXirSourceProductStage stage,XrXirStatus status) {
    if (diagnostic) {diagnostic->stage=stage;diagnostic->status=status;}
    return status;
}
static bool source_product_entry(const XrXirModule *module,uint32_t *entry) {
    if (!module || !module->declarations ||
        module->linkage_kind!=XR_XIR_PROGRAM) return false;
    uint32_t function=module->declarations->entry_function;
    if (function>=module->function_count || module->functions[function].parameter_count ||
        module->functions[function].result!=XR_XIR_I64) return false;
    *entry=function;return true;
}
static bool source_layout_word(XrSHA256Context *hash,uint64_t *work,uint32_t word) {
    if (*work<4) return false;
    uint8_t bytes[4];
    for (unsigned i=0;i<4;++i) bytes[i]=(uint8_t)(word>>(i*8));
    xr_sha256_update(hash,bytes,sizeof(bytes));*work-=4;return true;
}
static XrXirStatus source_product_layout_digest(const XrXirArtifact *lowered,
    const XrXirSourceProductFacts *facts,uint64_t work,uint8_t output[32]) {
    static const uint8_t domain[]="xray:xir-lowered-layout:v1";
    const XrXirModule *module=xr_xir_artifact_module(lowered);
    const XrXirTarget *target=xr_xir_artifact_target(lowered);uint32_t entry=0;
    if (!target || !source_product_entry(module,&entry) || entry!=facts->entry ||
        module->function_count!=facts->function_count || module->declarations->module_count!=facts->module_count ||
        target->architecture!=facts->target.architecture || target->abi_version!=facts->target.abi_version)
        return XR_XIR_BAD_STRUCTURE;
    if (work<sizeof(domain)-1) return XR_XIR_BUDGET;
    XrSHA256Context hash;xr_sha256_init(&hash);
    xr_sha256_update(&hash,domain,sizeof(domain)-1);work-=sizeof(domain)-1;
    uint32_t prefix[]={1,facts->target.architecture,facts->target.abi_version,
        XR_XIR_CALL_ABI_VERSION,XR_XIR_PROGRAM_ABI_VERSION,facts->entry,facts->function_count,facts->module_count};
    for (size_t i=0;i<sizeof(prefix)/sizeof(*prefix);++i)
        if (!source_layout_word(&hash,&work,prefix[i])) return XR_XIR_BUDGET;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunctionLayout *layout=xr_xir_artifact_layout(lowered,f);
        uint32_t parameters=module->functions[f].parameter_count;
        if (!layout || (layout->slot_count && !layout->offsets) ||
            (parameters && !layout->parameters) || (layout->owned_count && !layout->owned_offsets))
            return XR_XIR_BAD_LAYOUT;
        uint32_t words[]={f,parameters,layout->slot_count,layout->frame_bytes,layout->result.size,
            layout->result.alignment,layout->owned_count,layout->outgoing_count,layout->path_count};
        for (size_t i=0;i<sizeof(words)/sizeof(*words);++i)
            if (!source_layout_word(&hash,&work,words[i])) return XR_XIR_BUDGET;
        for (uint32_t i=0;i<layout->slot_count;++i)
            if (!source_layout_word(&hash,&work,layout->offsets[i])) return XR_XIR_BUDGET;
        for (uint32_t i=0;i<parameters;++i)
            if (!source_layout_word(&hash,&work,layout->parameters[i].size) ||
                !source_layout_word(&hash,&work,layout->parameters[i].alignment)) return XR_XIR_BUDGET;
        for (uint32_t i=0;i<layout->owned_count;++i)
            if (!source_layout_word(&hash,&work,layout->owned_offsets[i])) return XR_XIR_BUDGET;
    }
    xr_sha256_final(&hash,output);return XR_XIR_OK;
}
static XrXirStatus source_product_facts_budget(const XrXirCheckedPacket *source,
    const XrXirCheckedPacket *closed,const XrXirBudget *budget,uint64_t *work) {
    XrXirBudget limits=budget ? *budget : xr_xir_default_budget();
    uint64_t owned=sizeof(XrXirSourceProduct);
    if (owned>limits.metadata_bytes || source->length>limits.metadata_bytes-owned)
        return XR_XIR_BUDGET;
    owned+=source->length;
    if (closed->length>limits.metadata_bytes-owned || source->length>limits.work ||
        closed->length>limits.work-source->length) return XR_XIR_BUDGET;
    *work=limits.work-source->length-closed->length;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_source_product_build(const XrXirSourceProductRequest *request,
    XrXirSourceProduct **output,XrXirSourceProductDiagnostic *diagnostic) {
    if (!request || !output || *output || !source_diagnostic_empty(diagnostic) ||
        request->source.linkage_kind!=XR_XIR_PROGRAM ||
        request->target.architecture!=XR_XIR_ARCH_X86_64 ||
        request->target.abi_version!=XR_XIR_VALUE_ABI_VERSION)
        return source_product_reject(diagnostic,XR_XIR_SOURCE_PRODUCT_INPUT,XR_XIR_BAD_STRUCTURE);
    if (diagnostic) memset(diagnostic,0,sizeof(*diagnostic));
    XrXirSourceResult source={0};XrXirArtifact *closed=NULL,*lowered=NULL;
    XrXirCheckedPacket source_packet={0},closed_packet={0};
    XrXirStatus status=xr_xir_source_check(&request->source,&source,
        diagnostic ? &diagnostic->source : NULL);
    XrXirSourceProductStage stage=XR_XIR_SOURCE_PRODUCT_CHECK;
    if (status==XR_XIR_OK) {
        stage=XR_XIR_SOURCE_PRODUCT_SOURCE_PACKET;
        status=xr_xir_checked_write(source.checked,request->source.budget,&source_packet,
            diagnostic ? &diagnostic->xir : NULL);
    }
    if (status==XR_XIR_OK) {
        stage=XR_XIR_SOURCE_PRODUCT_SPECIALIZE;
        status=xr_xir_specialize(source.checked,request->source.budget,&closed,
            diagnostic ? &diagnostic->xir : NULL);
    }
    if (status==XR_XIR_OK) {
        stage=XR_XIR_SOURCE_PRODUCT_CLOSED_PACKET;
        status=xr_xir_checked_write(closed,request->source.budget,&closed_packet,
            diagnostic ? &diagnostic->xir : NULL);
    }
    if (status==XR_XIR_OK) {
        stage=XR_XIR_SOURCE_PRODUCT_LOWER;
        status=xr_xir_lower(closed,&request->target,request->source.budget,&lowered,
            diagnostic ? &diagnostic->xir : NULL);
    }
    uint32_t entry=0;
    if (status==XR_XIR_OK && !source_product_entry(xr_xir_artifact_module(lowered),&entry))
        status=XR_XIR_BAD_STRUCTURE;
    xr_xir_artifact_free(closed);
    XrXirSourceProduct *product=NULL;uint64_t facts_work=0;
    if (status==XR_XIR_OK) {
        stage=XR_XIR_SOURCE_PRODUCT_FACTS;
        status=source_product_facts_budget(&source_packet,&closed_packet,request->source.budget,&facts_work);
    }
    if (status==XR_XIR_OK) {
        product=xr_calloc(1,sizeof(*product));
        if (!product) status=XR_XIR_OUT_OF_MEMORY;
    }
    if (status==XR_XIR_OK) {
        const XrXirModule *module=xr_xir_artifact_module(lowered);
        product->facts.entry=entry;
        product->facts.target=*xr_xir_artifact_target(lowered);
        product->facts.function_count=module->function_count;product->facts.module_count=module->declarations->module_count;
        xr_sha256(source_packet.bytes,source_packet.length,product->facts.source_digest);
        xr_sha256(closed_packet.bytes,closed_packet.length,product->facts.closed_digest);
        stage=XR_XIR_SOURCE_PRODUCT_FACTS;
        status=source_product_layout_digest(lowered,&product->facts,facts_work,product->facts.lowered_layout_digest);
    }
    if (status==XR_XIR_OK) {
        product->lowered=lowered;product->snapshot=source.snapshot;
        product->source_packet=source_packet;product->closed_packet=closed_packet;
        *output=product;source.snapshot=NULL;source_packet=(XrXirCheckedPacket){0};closed_packet=(XrXirCheckedPacket){0};
    } else {
        xr_free(product);
        xr_xir_artifact_free(lowered);
        if (diagnostic) {diagnostic->snapshot=source.snapshot;source.snapshot=NULL;}
    }
    xr_xir_checked_packet_free(&source_packet);xr_xir_checked_packet_free(&closed_packet);
    xr_xir_source_result_free(&source);
    return source_product_reject(diagnostic,stage,status);
}
XR_FUNC void xr_xir_source_product_free(XrXirSourceProduct *product) {
    if (!product) return;
    xr_xir_artifact_free(product->lowered);xr_xir_source_snapshot_free(product->snapshot);
    xr_xir_checked_packet_free(&product->source_packet);xr_xir_checked_packet_free(&product->closed_packet);
    xr_free(product);
}
XR_FUNC void xr_xir_source_product_diagnostic_free(XrXirSourceProductDiagnostic *diagnostic) {
    if (!diagnostic) return;
    xr_xir_source_snapshot_free(diagnostic->snapshot);memset(diagnostic,0,sizeof(*diagnostic));
}
XR_FUNC const XrXirSourceProductFacts *xr_xir_source_product_facts(const XrXirSourceProduct *product) {
    return product ? &product->facts : NULL;
}
XR_FUNC XrXirStatus xr_xir_source_product_packet(const XrXirSourceProduct *product,
    XrXirSourceProductPacketKind kind,XrXirSourceProductPacketView *output) {
    if (!product || !output || (kind!=XR_XIR_SOURCE_PRODUCT_SOURCE && kind!=XR_XIR_SOURCE_PRODUCT_CLOSED))
        return XR_XIR_BAD_STRUCTURE;
    const XrXirCheckedPacket *packet=kind==XR_XIR_SOURCE_PRODUCT_SOURCE ? &product->source_packet : &product->closed_packet;
    *output=(XrXirSourceProductPacketView){packet->bytes,packet->length};return XR_XIR_OK;
}
XR_FUNC const XrXirSourceView *xr_xir_source_product_view(const XrXirSourceProduct *product) {
    return product ? xr_xir_source_snapshot_view(product->snapshot) : NULL;
}
XR_FUNC XrXirStatus xr_xir_source_product_layout(const XrXirSourceProduct *product,
    uint32_t function,XrXirSourceProductLayoutView *output) {
    if (!product || !output) return XR_XIR_BAD_STRUCTURE;
    if (!product->lowered) return XR_XIR_BAD_STAGE;
    const XrXirModule *module=xr_xir_artifact_module(product->lowered);
    if (function>=module->function_count) return XR_XIR_BAD_STRUCTURE;
    *output=(XrXirSourceProductLayoutView){module->functions[function].parameter_count,
        xr_xir_artifact_layout(product->lowered,function)};return XR_XIR_OK;
}
static XrXirStatus source_product_packet_digest(const XrXirCheckedPacket *packet,
    const uint8_t expected[32],const XrXirBudget *budget) {
    if (!packet->bytes || packet->length<64) return XR_XIR_BAD_STRUCTURE;
    if (packet->length>budget->metadata_bytes || packet->length>budget->work) return XR_XIR_BUDGET;
    uint8_t digest[32];xr_sha256(packet->bytes,packet->length,digest);
    return memcmp(digest,expected,32) ? XR_XIR_BAD_STRUCTURE : XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_source_product_verify(const XrXirSourceProduct *product,
    const XrXirBudget *budget,size_t code_limit,XrXirDiagnostic *diagnostic) {
    if (!product || !code_limit) return XR_XIR_BAD_STRUCTURE;
    if (!product->lowered) return XR_XIR_BAD_STAGE;
    const XrXirModule *module=xr_xir_artifact_module(product->lowered);
    const XrXirTarget *target=xr_xir_artifact_target(product->lowered);uint32_t entry=0;
    if (!target || !source_product_entry(module,&entry) || entry!=product->facts.entry ||
        target->architecture!=product->facts.target.architecture || target->abi_version!=product->facts.target.abi_version ||
        module->function_count!=product->facts.function_count || module->declarations->module_count!=product->facts.module_count)
        return XR_XIR_BAD_STRUCTURE;
    XrXirBudget limits=budget ? *budget : xr_xir_default_budget();
    XrXirArtifact *source=NULL,*decoded_closed=NULL,*closed=NULL,*lowered=NULL;
    XrXirCheckedPacket packet={0};XrXirCSource actual={0},expected={0};
    XrXirStatus status=source_product_packet_digest(&product->source_packet,product->facts.source_digest,&limits);
    if (status==XR_XIR_OK) status=source_product_packet_digest(&product->closed_packet,product->facts.closed_digest,&limits);
    if (status==XR_XIR_OK) status=xr_xir_checked_read(product->source_packet.bytes,product->source_packet.length,&limits,&source,diagnostic);
    if (status==XR_XIR_OK) status=xr_xir_checked_read(product->closed_packet.bytes,product->closed_packet.length,&limits,&decoded_closed,diagnostic);
    if (status==XR_XIR_OK) status=xr_xir_specialize(source,&limits,&closed,diagnostic);
    if (status==XR_XIR_OK) status=xr_xir_checked_write(closed,&limits,&packet,diagnostic);
    if (status==XR_XIR_OK && (packet.length!=product->closed_packet.length ||
        memcmp(packet.bytes,product->closed_packet.bytes,packet.length))) status=XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK) status=xr_xir_lower(closed,&product->facts.target,&limits,&lowered,diagnostic);
    if (status==XR_XIR_OK) status=xr_xir_artifact_verify(product->lowered,&limits,diagnostic);
    uint8_t layout_digest[32];
    if (status==XR_XIR_OK) status=source_product_layout_digest(lowered,&product->facts,limits.work,layout_digest);
    if (status==XR_XIR_OK && memcmp(layout_digest,product->facts.lowered_layout_digest,32)) status=XR_XIR_BAD_LAYOUT;
    if (status==XR_XIR_OK) status=xr_xir_emit_c(lowered,"source_product_proof",code_limit,&expected);
    if (status==XR_XIR_OK) status=xr_xir_emit_c(product->lowered,"source_product_proof",code_limit,&actual);
    if (status==XR_XIR_OK && (actual.length!=expected.length || memcmp(actual.text,expected.text,actual.length))) status=XR_XIR_BAD_STRUCTURE;
    xr_xir_c_source_free(&actual);xr_xir_c_source_free(&expected);xr_xir_checked_packet_free(&packet);
    xr_xir_artifact_free(lowered);xr_xir_artifact_free(closed);xr_xir_artifact_free(decoded_closed);xr_xir_artifact_free(source);
    if (diagnostic) diagnostic->status=status;
    return status;
}
XR_FUNC XrXirStatus xr_xir_source_product_vm_take(XrXirSourceProduct *product,
    XrXirProgramBudget budget,XrXirProgram **output) {
    if (!product || !output || *output) return XR_XIR_BAD_STRUCTURE;
    if (!product->lowered) return XR_XIR_BAD_STAGE;
    return xr_xir_vm_program_take(&product->lowered,budget,output);
}
XR_FUNC XrXirStatus xr_xir_source_product_emit(const XrXirSourceProduct *product,
    const char *prefix,size_t byte_limit,XrXirCSource *output) {
    if (!product || !output || output->text || output->length)
        return XR_XIR_BAD_STRUCTURE;
    if (!product->lowered) return XR_XIR_BAD_STAGE;
    return xr_xir_emit_c(product->lowered,prefix,byte_limit,output);
}
