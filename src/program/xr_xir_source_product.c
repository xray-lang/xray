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
#include "../xir/xxir_compile_memory.h"
#include <string.h>

struct XrXirSourceProduct {
    XrXirCompileContext context;
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
static bool source_layout_word(XrSHA256Context *hash,const XrXirCompileContext *context,uint32_t word) {
    if (!xir_compile_work(context, 8)) return false;
    uint8_t bytes[4];
    for (unsigned i=0;i<4;++i) bytes[i]=(uint8_t)(word>>(i*8));
    xr_sha256_update(hash,bytes,sizeof(bytes));return true;
}
static XrXirStatus source_product_layout_digest(const XrXirArtifact *lowered,
    const XrXirSourceProductFacts *facts,uint8_t output[32]) {
    const XrXirCompileContext *context = xr_xir_compile_artifact_context(lowered);
    static const uint8_t domain[]="xray:xir-lowered-layout:v1";
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    const XrXirTarget *target=xr_xir_compile_artifact_target(lowered);uint32_t entry=0;
    if (!target || !source_product_entry(module,&entry) || entry!=facts->entry ||
        module->function_count!=facts->function_count || module->declarations->module_count!=facts->module_count ||
        target->architecture!=facts->target.architecture || target->abi_version!=facts->target.abi_version)
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(context, sizeof(domain))) return XR_XIR_BUDGET;
    XrSHA256Context hash;xr_sha256_init(&hash);
    xr_sha256_update(&hash,domain,sizeof(domain)-1);
    uint32_t prefix[]={1,facts->target.architecture,facts->target.abi_version,
        XR_XIR_CALL_ABI_VERSION,XR_XIR_PROGRAM_ABI_VERSION,facts->entry,facts->function_count,facts->module_count};
    for (size_t i=0;i<sizeof(prefix)/sizeof(*prefix);++i)
        if (!source_layout_word(&hash,context,prefix[i])) return XR_XIR_BUDGET;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunctionLayout *layout=xr_xir_compile_artifact_layout(lowered,f);
        uint32_t parameters=module->functions[f].parameter_count;
        if (!layout || (layout->slot_count && !layout->offsets) ||
            (parameters && !layout->parameters) || (layout->owned_count && !layout->owned_offsets))
            return XR_XIR_BAD_LAYOUT;
        uint32_t words[]={f,parameters,layout->slot_count,layout->frame_bytes,layout->result.size,
            layout->result.alignment,layout->owned_count,layout->outgoing_count,layout->path_count};
        for (size_t i=0;i<sizeof(words)/sizeof(*words);++i)
            if (!source_layout_word(&hash,context,words[i])) return XR_XIR_BUDGET;
        for (uint32_t i=0;i<layout->slot_count;++i)
            if (!source_layout_word(&hash,context,layout->offsets[i])) return XR_XIR_BUDGET;
        for (uint32_t i=0;i<parameters;++i)
            if (!source_layout_word(&hash,context,layout->parameters[i].size) ||
                !source_layout_word(&hash,context,layout->parameters[i].alignment)) return XR_XIR_BUDGET;
        for (uint32_t i=0;i<layout->owned_count;++i)
            if (!source_layout_word(&hash,context,layout->owned_offsets[i])) return XR_XIR_BUDGET;
    }
    if (!xir_compile_work(context, 1 + 32)) return XR_XIR_BUDGET;
    xr_sha256_final(&hash,output);return XR_XIR_OK;
}
static XrXirStatus source_product_hash(const XrXirCompileContext *context,
    const XrXirCheckedPacket *packet, uint8_t digest[32]) {
    if (packet->length > UINT64_MAX - 34 ||
        !xir_compile_work(context, (uint64_t)packet->length + 34)) return XR_XIR_BUDGET;
    xr_sha256(packet->bytes, packet->length, digest);
    return XR_XIR_OK;
}
static XrXirStatus source_product_compare(const XrXirCompileContext *context,
    const void *left, const void *right, size_t size) {
    const unsigned char *a = left, *b = right;
    for (size_t i = 0; i < size; ++i) {
        if (!xir_compile_work(context, 2)) return XR_XIR_BUDGET;
        if (a[i] != b[i]) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_compile_source_product_build(const XrXirSourceProductRequest *request,
    XrXirSourceProduct **output,XrXirSourceProductDiagnostic *diagnostic) {
    if (!request || !output || *output || !source_diagnostic_empty(diagnostic) ||
        !xir_compile_context_valid(request->source.context) ||
        request->source.linkage_kind!=XR_XIR_PROGRAM ||
        request->target.architecture!=XR_XIR_ARCH_X86_64 ||
        request->target.abi_version!=XR_XIR_VALUE_ABI_VERSION)
        return source_product_reject(diagnostic,XR_XIR_SOURCE_PRODUCT_INPUT,XR_XIR_BAD_STRUCTURE);
    const XrXirCompileContext *context = request->source.context;
    if (diagnostic) memset(diagnostic,0,sizeof(*diagnostic));
    XrXirSourceResult source={0};XrXirArtifact *closed=NULL,*lowered=NULL;
    XrXirCheckedPacket source_packet={0},closed_packet={0};
    XrXirStatus status=xr_xir_compile_source_check(&request->source,&source,
        diagnostic ? &diagnostic->source : NULL);
    XrXirSourceProductStage stage=XR_XIR_SOURCE_PRODUCT_CHECK;
    if (status==XR_XIR_OK) {
        stage=XR_XIR_SOURCE_PRODUCT_SOURCE_PACKET;
        status=xr_xir_compile_checked_write(source.checked,&source_packet,
            diagnostic ? &diagnostic->xir : NULL);
    }
    if (status==XR_XIR_OK) {
        stage=XR_XIR_SOURCE_PRODUCT_SPECIALIZE;
        status=xr_xir_compile_specialize(source.checked,&closed,
            diagnostic ? &diagnostic->xir : NULL);
    }
    if (status==XR_XIR_OK) {
        stage=XR_XIR_SOURCE_PRODUCT_CLOSED_PACKET;
        status=xr_xir_compile_checked_write(closed,&closed_packet,
            diagnostic ? &diagnostic->xir : NULL);
    }
    if (status==XR_XIR_OK) {
        stage=XR_XIR_SOURCE_PRODUCT_LOWER;
        status=xr_xir_compile_lower(closed,&request->target,&lowered,
            diagnostic ? &diagnostic->xir : NULL);
    }
    uint32_t entry=0;
    if (status==XR_XIR_OK && !source_product_entry(xr_xir_compile_artifact_module(lowered),&entry))
        status=XR_XIR_BAD_STRUCTURE;
    xr_xir_compile_artifact_free(closed);
    XrXirSourceProduct *product=NULL;
    if (status==XR_XIR_OK) {
        stage=XR_XIR_SOURCE_PRODUCT_FACTS;
        product=xir_compile_calloc(context,1,sizeof(*product),&status);
    }
    if (status==XR_XIR_OK) {
        const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
        product->context = *context;
        product->facts.entry=entry;
        product->facts.target=*xr_xir_compile_artifact_target(lowered);
        product->facts.function_count=module->function_count;product->facts.module_count=module->declarations->module_count;
        status=source_product_hash(context,&source_packet,product->facts.source_digest);
        if (status==XR_XIR_OK) status=source_product_hash(context,&closed_packet,product->facts.closed_digest);
        if (status==XR_XIR_OK)
            status=source_product_layout_digest(lowered,&product->facts,product->facts.lowered_layout_digest);
    }
    if (status==XR_XIR_OK) {
        product->lowered=lowered;product->snapshot=source.snapshot;
        product->source_packet=source_packet;product->closed_packet=closed_packet;
        *output=product;source.snapshot=NULL;source_packet=(XrXirCheckedPacket){0};closed_packet=(XrXirCheckedPacket){0};
    } else {
        xr_compile_resources_free(product);
        xr_xir_compile_artifact_free(lowered);
        if (diagnostic) {diagnostic->snapshot=source.snapshot;source.snapshot=NULL;}
    }
    xr_xir_compile_checked_packet_free(&source_packet);xr_xir_compile_checked_packet_free(&closed_packet);
    xr_xir_compile_source_result_free(&source);
    return source_product_reject(diagnostic,stage,status);
}
XR_FUNC void xr_xir_compile_source_product_free(XrXirSourceProduct *product) {
    if (!product) return;
    xr_xir_compile_artifact_free(product->lowered);xr_xir_compile_source_snapshot_free(product->snapshot);
    xr_xir_compile_checked_packet_free(&product->source_packet);xr_xir_compile_checked_packet_free(&product->closed_packet);
    xr_compile_resources_free(product);
}
XR_FUNC void xr_xir_compile_source_product_diagnostic_free(XrXirSourceProductDiagnostic *diagnostic) {
    if (!diagnostic) return;
    xr_xir_compile_source_snapshot_free(diagnostic->snapshot);memset(diagnostic,0,sizeof(*diagnostic));
}
XR_FUNC const XrXirSourceProductFacts *xr_xir_compile_source_product_facts(const XrXirSourceProduct *product) {
    return product ? &product->facts : NULL;
}
XR_FUNC const XrXirCompileContext *xr_xir_compile_source_product_context(const XrXirSourceProduct *product) {
    return product ? &product->context : NULL;
}
XR_FUNC XrXirStatus xr_xir_compile_source_product_packet(const XrXirSourceProduct *product,
    XrXirSourceProductPacketKind kind,XrXirSourceProductPacketView *output) {
    if (!product || !output || (kind!=XR_XIR_SOURCE_PRODUCT_SOURCE && kind!=XR_XIR_SOURCE_PRODUCT_CLOSED))
        return XR_XIR_BAD_STRUCTURE;
    const XrXirCheckedPacket *packet=kind==XR_XIR_SOURCE_PRODUCT_SOURCE ? &product->source_packet : &product->closed_packet;
    *output=(XrXirSourceProductPacketView){packet->bytes,packet->length};return XR_XIR_OK;
}
XR_FUNC const XrXirSourceView *xr_xir_compile_source_product_view(const XrXirSourceProduct *product) {
    return product ? xr_xir_compile_source_snapshot_view(product->snapshot) : NULL;
}
XR_FUNC XrXirStatus xr_xir_compile_source_product_layout(const XrXirSourceProduct *product,
    uint32_t function,XrXirSourceProductLayoutView *output) {
    if (!product || !output) return XR_XIR_BAD_STRUCTURE;
    if (!product->lowered) return XR_XIR_BAD_STAGE;
    const XrXirModule *module=xr_xir_compile_artifact_module(product->lowered);
    if (function>=module->function_count) return XR_XIR_BAD_STRUCTURE;
    *output=(XrXirSourceProductLayoutView){module->functions[function].parameter_count,
        xr_xir_compile_artifact_layout(product->lowered,function)};return XR_XIR_OK;
}
static XrXirStatus source_product_packet_digest(const XrXirCheckedPacket *packet,
    const uint8_t expected[32],const XrXirCompileContext *context) {
    if (!packet->bytes || packet->length<64) return XR_XIR_BAD_STRUCTURE;
    uint8_t digest[32];
    XrXirStatus status=source_product_hash(context,packet,digest);
    return status==XR_XIR_OK ? source_product_compare(context,digest,expected,32) : status;
}
XR_FUNC XrXirStatus xr_xir_compile_source_product_verify(const XrXirSourceProduct *product,
    size_t code_limit,XrXirDiagnostic *diagnostic) {
    if (!product || !code_limit) return XR_XIR_BAD_STRUCTURE;
    if (!product->lowered) return XR_XIR_BAD_STAGE;
    const XrXirModule *module=xr_xir_compile_artifact_module(product->lowered);
    const XrXirTarget *target=xr_xir_compile_artifact_target(product->lowered);uint32_t entry=0;
    if (!target || !source_product_entry(module,&entry) || entry!=product->facts.entry ||
        target->architecture!=product->facts.target.architecture || target->abi_version!=product->facts.target.abi_version ||
        module->function_count!=product->facts.function_count || module->declarations->module_count!=product->facts.module_count)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirCompileContext *context = &product->context;
    XrXirArtifact *source=NULL,*decoded_closed=NULL,*closed=NULL,*lowered=NULL;
    XrXirCheckedPacket packet={0};XrXirCSource actual={0},expected={0};
    XrXirStatus status=source_product_packet_digest(&product->source_packet,product->facts.source_digest,context);
    if (status==XR_XIR_OK) status=source_product_packet_digest(&product->closed_packet,product->facts.closed_digest,context);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(context,product->source_packet.bytes,product->source_packet.length,&source,diagnostic);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(context,product->closed_packet.bytes,product->closed_packet.length,&decoded_closed,diagnostic);
    if (status==XR_XIR_OK) status=xr_xir_compile_specialize(source,&closed,diagnostic);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_write(closed,&packet,diagnostic);
    if (status==XR_XIR_OK) status=packet.length!=product->closed_packet.length ? XR_XIR_BAD_STRUCTURE :
        source_product_compare(context,packet.bytes,product->closed_packet.bytes,packet.length);
    if (status==XR_XIR_OK) status=xr_xir_compile_lower(closed,&product->facts.target,&lowered,diagnostic);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(product->lowered,diagnostic);
    uint8_t layout_digest[32];
    if (status==XR_XIR_OK) status=source_product_layout_digest(lowered,&product->facts,layout_digest);
    if (status==XR_XIR_OK) {
        status=source_product_compare(context,layout_digest,product->facts.lowered_layout_digest,32);
        if (status==XR_XIR_BAD_STRUCTURE) status=XR_XIR_BAD_LAYOUT;
    }
    if (status==XR_XIR_OK) status=xr_xir_compile_emit_c(lowered,"source_product_proof",code_limit,&expected);
    if (status==XR_XIR_OK) status=xr_xir_compile_emit_c(product->lowered,"source_product_proof",code_limit,&actual);
    if (status==XR_XIR_OK) status=actual.length!=expected.length ? XR_XIR_BAD_STRUCTURE :
        source_product_compare(context,actual.text,expected.text,actual.length);
    xr_xir_compile_c_source_free(&actual);xr_xir_compile_c_source_free(&expected);xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(decoded_closed);xr_xir_compile_artifact_free(source);
    if (diagnostic) diagnostic->status=status;
    return status;
}
XR_FUNC XrXirStatus xr_xir_compile_source_product_vm_take(XrXirSourceProduct *product,
    XrXirProgram **output) {
    if (!product || !output || *output) return XR_XIR_BAD_STRUCTURE;
    if (!product->lowered) return XR_XIR_BAD_STAGE;
    return xr_xir_compile_vm_program_take(&product->lowered,output);
}
XR_FUNC XrXirStatus xr_xir_compile_source_product_emit(const XrXirSourceProduct *product,
    const char *prefix,size_t byte_limit,XrXirCSource *output) {
    if (!product || !output || output->text || output->length)
        return XR_XIR_BAD_STRUCTURE;
    if (!product->lowered) return XR_XIR_BAD_STAGE;
    return xr_xir_compile_emit_c(product->lowered,prefix,byte_limit,output);
}
