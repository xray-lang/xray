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
#include "../xir/xxir_types.h"
#include "../base/xsha256.h"
#include "../xir/xxir_compile_memory.h"
#include "../xir/xxir_internal.h"
#include <string.h>

struct XrXirSourceProduct {
    XrXirCompileContext context;
    XrXirArtifact *lowered;
    XrXirSourceSnapshot *snapshot;
    XrXirCheckedPacket source_packet, closed_packet;
    XrXirSourceProductFacts facts;
    XrXirSourceTests tests;
    XrXirSourceDebugView debug;
};
static XrXirStatus source_product_tests(XrXirSourceProduct *product, const XrXirModule *module) {
    const XrXirCompileContext *context = &product->context;
    const XrXirDeclarations *d = module->declarations;

    uint32_t root = d->root_module;
    uint32_t count = 0;
    size_t names = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {

        XrXirFunctionIdentity identity = d->functions[f];
        if (!identity.test_role || identity.module != root) continue;

        XrXirFunction function = module->functions[f];
        if ((uint64_t)function.name_length + 1 > SIZE_MAX - names) return XR_XIR_BUDGET;
        names += (size_t)function.name_length + 1;
        ++count;
    }
    if (!count) return XR_XIR_OK;
    uint64_t rows = (uint64_t)count * sizeof(XrXirSourceTestEntry);
    if (rows > SIZE_MAX || names > SIZE_MAX - (size_t)rows) return XR_XIR_BUDGET;
    size_t size = (size_t)rows + names;
    XrXirStatus status = XR_XIR_OK;
    XrXirSourceTestEntry *entries = xir_compile_calloc(context, 1, size, &status);
    if (!entries) return status;
    char *name = (char *)(entries + count);
    uint32_t index = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {

        XrXirFunctionIdentity identity = d->functions[f];
        if (!identity.test_role || identity.module != root) continue;

        XrXirFunction function = module->functions[f];

        memcpy(name, function.name, function.name_length);
        name[function.name_length] = 0;
        entries[index++] = (XrXirSourceTestEntry){f, identity.test_role,
            identity.test_timeout_seconds, name, function.name_length};
        name += (size_t)function.name_length + 1;
    }

    if (status != XR_XIR_OK) { xr_compile_resources_free(entries); return status; }
    product->tests = (XrXirSourceTests){entries, count};
    return XR_XIR_OK;
}
static bool source_diagnostic_empty(const XrXirSourceProductDiagnostic *diagnostic) {
    return !diagnostic || (!diagnostic->snapshot && xr_xir_compile_source_failure_empty(&diagnostic->failure));
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
static void source_layout_word(XrSHA256Context *hash,uint32_t word) {

    uint8_t bytes[4];
    for (unsigned i=0;i<4;++i) bytes[i]=(uint8_t)(word>>(i*8));
    xr_sha256_update(hash,bytes,sizeof(bytes));
}
static XrXirStatus source_product_layout_digest(const XrXirArtifact *lowered,
    const XrXirSourceProductFacts *facts,uint8_t output[32]) {
    static const uint8_t domain[]="xray:xir-lowered-layout:v1";
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    const XrXirTarget *target=xr_xir_compile_artifact_target(lowered);uint32_t entry=0;
    if (!target || !source_product_entry(module,&entry) || entry!=facts->entry ||
        module->function_count!=facts->function_count || module->declarations->module_count!=facts->module_count ||
        target->architecture!=facts->target.architecture || target->abi_version!=facts->target.abi_version)
        return XR_XIR_BAD_STRUCTURE;

    XrSHA256Context hash;xr_sha256_init(&hash);
    xr_sha256_update(&hash,domain,sizeof(domain)-1);
    uint32_t prefix[]={1,facts->target.architecture,facts->target.abi_version,
        XR_XIR_CALL_ABI_VERSION,XR_XIR_PROGRAM_ABI_VERSION,facts->entry,facts->function_count,facts->module_count};
    for (size_t i=0;i<sizeof(prefix)/sizeof(*prefix);++i)
        source_layout_word(&hash,prefix[i]);
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunctionLayout *layout=xr_xir_compile_artifact_layout(lowered,f);
        uint32_t parameters=module->functions[f].parameter_count;
        if (!layout || (layout->slot_count && !layout->offsets) ||
            (parameters && !layout->parameters) || (layout->owned_count && !layout->owned_offsets))
            return XR_XIR_BAD_LAYOUT;
        uint32_t words[]={f,parameters,layout->slot_count,layout->frame_bytes,layout->result.size,
            layout->result.alignment,layout->owned_count,layout->outgoing_count,layout->path_count};
        for (size_t i=0;i<sizeof(words)/sizeof(*words);++i)
            source_layout_word(&hash,words[i]);
        for (uint32_t i=0;i<layout->slot_count;++i)
            source_layout_word(&hash,layout->offsets[i]);
        for (uint32_t i=0;i<parameters;++i)
            {
                source_layout_word(&hash,layout->parameters[i].size);
                source_layout_word(&hash,layout->parameters[i].alignment);
            }
        for (uint32_t i=0;i<layout->owned_count;++i)
            source_layout_word(&hash,layout->owned_offsets[i]);
    }

    xr_sha256_final(&hash,output);return XR_XIR_OK;
}
static XrXirStatus source_product_hash(
    const XrXirCheckedPacket *packet, uint8_t digest[32]) {
    xr_sha256(packet->bytes, packet->length, digest);
    return XR_XIR_OK;
}
static XrXirStatus source_product_compare(
    const void *left, const void *right, size_t size) {
    const unsigned char *a = left, *b = right;
    for (size_t i = 0; i < size; ++i) {

        if (a[i] != b[i]) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
#include "xr_xir_source_debug.inc.c"
typedef enum SourceProductInputKind { SOURCE_PRODUCT_FILE, SOURCE_PRODUCT_TEXT, SOURCE_PRODUCT_AST } SourceProductInputKind;
typedef struct SourceProductInput {
    SourceProductInputKind kind;
    const XrXirSourceText *text;
    const XrXirSourceAst *ast;
} SourceProductInput;
static XrXirStatus source_product_build(const XrXirSourceProductRequest *request,
    SourceProductInput input, XrXirSourceProduct **output,
    XrXirSourceProductDiagnostic *diagnostic) {
    if (!source_diagnostic_empty(diagnostic)) return XR_XIR_BAD_STRUCTURE;
    if (!request || (input.kind == SOURCE_PRODUCT_TEXT && !input.text) ||
        (input.kind == SOURCE_PRODUCT_AST && !input.ast) || !output || *output ||
        !xir_compile_context_valid(request->source.context) ||
        request->source.linkage_kind!=XR_XIR_PROGRAM ||
        request->target.architecture!=XR_XIR_ARCH_X86_64 ||
        request->target.abi_version!=XR_XIR_VALUE_ABI_VERSION)
        return source_product_reject(diagnostic,XR_XIR_SOURCE_PRODUCT_INPUT,XR_XIR_BAD_STRUCTURE);
    const XrXirCompileContext *context = request->source.context;
    if (diagnostic) memset(diagnostic,0,sizeof(*diagnostic));
    XrXirSourceResult source={0};XrXirArtifact *closed=NULL,*lowered=NULL;
    XrXirSourceFailure failure={0};
    XrXirCheckedPacket source_packet={0},closed_packet={0};
    XrXirStatus status = input.kind == SOURCE_PRODUCT_AST ?
        xr_xir_compile_source_check_ast(&request->source, input.ast, &source,
            diagnostic ? &diagnostic->source : NULL, diagnostic ? &failure : NULL) :
        input.kind == SOURCE_PRODUCT_TEXT ?
        xr_xir_compile_source_check_text(&request->source, input.text, &source,
            diagnostic ? &diagnostic->source : NULL, diagnostic ? &failure : NULL) :
        xr_xir_compile_source_check(&request->source, &source,
            diagnostic ? &diagnostic->source : NULL, diagnostic ? &failure : NULL);
    if(diagnostic){diagnostic->failure=failure;failure=(XrXirSourceFailure){0};}
    xr_xir_compile_source_failure_free(&failure);
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
        status=source_product_hash(&source_packet,product->facts.source_digest);
        if (status==XR_XIR_OK) status=source_product_hash(&closed_packet,product->facts.closed_digest);
        if (status==XR_XIR_OK)
            status=source_product_layout_digest(lowered,&product->facts,product->facts.lowered_layout_digest);
        if (status==XR_XIR_OK) status=source_product_tests(product,module);
        if (status==XR_XIR_OK) status=source_product_debug(product,source.snapshot,&source_packet,module);
    }
    if (status==XR_XIR_OK) {
        product->lowered=lowered;product->snapshot=source.snapshot;
        product->source_packet=source_packet;product->closed_packet=closed_packet;
        *output=product;source.snapshot=NULL;source_packet=(XrXirCheckedPacket){0};closed_packet=(XrXirCheckedPacket){0};
    } else {
        if (product) xr_compile_resources_free((void *)product->tests.entries);
        if (product) xr_compile_resources_free((void *)product->debug.functions);
        xr_compile_resources_free(product);
        xr_xir_compile_artifact_free(lowered);
        if (diagnostic) {diagnostic->snapshot=source.snapshot;source.snapshot=NULL;}
    }
    xr_xir_compile_checked_packet_free(&source_packet);xr_xir_compile_checked_packet_free(&closed_packet);
    xr_xir_compile_source_result_free(&source);
    return source_product_reject(diagnostic,stage,status);
}
XR_FUNC XrXirStatus xr_xir_compile_source_product_build(const XrXirSourceProductRequest *request,
    XrXirSourceProduct **output, XrXirSourceProductDiagnostic *diagnostic) {
    return source_product_build(request, (SourceProductInput){0}, output, diagnostic);
}
XR_FUNC XrXirStatus xr_xir_compile_source_product_build_text(const XrXirSourceProductRequest *request,
    const XrXirSourceText *text, XrXirSourceProduct **output, XrXirSourceProductDiagnostic *diagnostic) {
    return source_product_build(request, (SourceProductInput){SOURCE_PRODUCT_TEXT,text,NULL}, output, diagnostic);
}
XR_FUNC XrXirStatus xr_xir_compile_source_product_build_ast(const XrXirSourceProductRequest *request,
    const XrXirSourceAst *ast, XrXirSourceProduct **output, XrXirSourceProductDiagnostic *diagnostic) {
    return source_product_build(request, (SourceProductInput){SOURCE_PRODUCT_AST,NULL,ast}, output, diagnostic);
}
XR_FUNC void xr_xir_compile_source_product_free(XrXirSourceProduct *product) {
    if (!product) return;
    xr_compile_resources_free((void *)product->tests.entries);
    xr_compile_resources_free((void *)product->debug.functions);
    xr_xir_compile_artifact_free(product->lowered);xr_xir_compile_source_snapshot_free(product->snapshot);
    xr_xir_compile_checked_packet_free(&product->source_packet);xr_xir_compile_checked_packet_free(&product->closed_packet);
    xr_compile_resources_free(product);
}
XR_FUNC void xr_xir_compile_source_product_diagnostic_free(XrXirSourceProductDiagnostic *diagnostic) {
    if (!diagnostic) return;
    xr_xir_compile_source_snapshot_free(diagnostic->snapshot);
    xr_xir_compile_source_failure_free(&diagnostic->failure);memset(diagnostic,0,sizeof(*diagnostic));
}
XR_FUNC XrXirStatus xr_xir_compile_source_lowered_public_admit(
    const XrXirArtifact *lowered, XrXirSourceDiagnostic *diagnostic) {
    XrXirStatus status = XR_XIR_BAD_STRUCTURE;
    uint32_t module = 0;
    bool state_rejected = false,birth_rejected=false;
    uint32_t *order=NULL,active_count=0;
    uint8_t *active=NULL;
    const XrXirCompileContext *context=xr_xir_compile_artifact_context(lowered);
    const XrXirModule *body=xr_xir_compile_artifact_module(lowered);
    if (!xir_compile_context_valid(context)) goto done;
    if (!body || body->stage!=XR_XIR_LOWERED) { status = XR_XIR_BAD_STAGE; goto done; }

    const XrXirDeclarations *declarations = body ? body->declarations : NULL;
    if (!body || body->linkage_kind != XR_XIR_PROGRAM || !declarations ||
        !declarations->modules || !declarations->module_count ||
        declarations->root_module >= declarations->module_count ||
        (declarations->slot_count && !declarations->slots)) goto done;
    status = XR_XIR_OK;
    for (module=0;module<declarations->module_count;++module) {

        if (declarations->modules[module].storage==XR_XIR_MODULE_BIRTH_STORAGE) {
            birth_rejected=true;status=XR_XIR_BAD_STRUCTURE;break;
        }
    }
    if (status!=XR_XIR_OK) goto done;
    for (uint32_t s = 0; s < declarations->slot_count; ++s) {

        const XrXirSlot *slot = &declarations->slots[s];
        module = slot->module;
        if (module >= declarations->module_count || slot->mutable > 1) {
            status = XR_XIR_BAD_STRUCTURE; break;
        }
        /* Inactive declarations remain unreachable through the same Program
         * closure. Active foreign mutable storage still needs its birth owner. */
        if (slot->mutable && module != declarations->root_module) {
            if(!active) {
                order=xir_compile_calloc(context,declarations->module_count,sizeof(*order),&status);
                active=xir_compile_calloc(context,declarations->module_count,1,&status);
                if(status==XR_XIR_OK)status=xr_xir_compile_declarations_active(context,declarations,order,active,&active_count);
                if(status!=XR_XIR_OK)break;
            }
            if(active[module]){state_rejected = true; status = XR_XIR_BAD_STRUCTURE; break;}
        }
    }
done:
    xr_compile_resources_free(active);xr_compile_resources_free(order);
    if (diagnostic) {
        *diagnostic = (XrXirSourceDiagnostic){.status = status, .module = module};
        if (state_rejected) {
            static const char message[] = "library mutable or exported state is not admitted";
            memcpy(diagnostic->message, message, sizeof(message));
        } else if (birth_rejected) {
            static const char message[]="historical module requires published birth storage";
            memcpy(diagnostic->message,message,sizeof(message));
        }
    }
    return status;
}
XR_FUNC XrXirStatus xr_xir_compile_source_product_public_admit(
    const XrXirSourceProduct *product, XrXirSourceDiagnostic *diagnostic) {
    if(product && xir_compile_context_valid(&product->context) && product->lowered)
        return xr_xir_compile_source_lowered_public_admit(product->lowered,diagnostic);
    XrXirStatus status=product && xir_compile_context_valid(&product->context)?XR_XIR_BAD_STAGE:XR_XIR_BAD_STRUCTURE;
    if(diagnostic)*diagnostic=(XrXirSourceDiagnostic){.status=status};
    return status;
}
XR_FUNC const XrXirSourceProductFacts *xr_xir_compile_source_product_facts(const XrXirSourceProduct *product) {
    return product ? &product->facts : NULL;
}
XR_FUNC const XrXirModule *xr_xir_compile_source_product_module(const XrXirSourceProduct *product) {
    return product && product->lowered ? xr_xir_compile_artifact_module(product->lowered) : NULL;
}
XR_FUNC const XrXirCompileContext *xr_xir_compile_source_product_context(const XrXirSourceProduct *product) {
    return product ? &product->context : NULL;
}
XR_FUNC const XrXirSourceTests *xr_xir_compile_source_product_tests(const XrXirSourceProduct *product) {
    return product ? &product->tests : NULL;
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
XR_FUNC const XrXirConstruction *xr_xir_compile_source_product_construction(const XrXirSourceProduct *product) {
    return product ? xr_xir_compile_source_snapshot_construction(product->snapshot) : NULL;
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
    const uint8_t expected[32]) {
    if (!packet->bytes || packet->length<64) return XR_XIR_BAD_STRUCTURE;
    uint8_t digest[32];
    XrXirStatus status=source_product_hash(packet,digest);
    return status==XR_XIR_OK ? source_product_compare(digest,expected,32) : status;
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
    XrXirStatus status=source_product_packet_digest(&product->source_packet,product->facts.source_digest);
    if (status==XR_XIR_OK) status=source_product_packet_digest(&product->closed_packet,product->facts.closed_digest);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(context,product->source_packet.bytes,product->source_packet.length,&source,diagnostic);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(context,product->closed_packet.bytes,product->closed_packet.length,&decoded_closed,diagnostic);
    if (status==XR_XIR_OK) status=xr_xir_compile_specialize(source,&closed,diagnostic);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_write(closed,&packet,diagnostic);
    if (status==XR_XIR_OK) status=packet.length!=product->closed_packet.length ? XR_XIR_BAD_STRUCTURE :
        source_product_compare(packet.bytes,product->closed_packet.bytes,packet.length);
    if (status==XR_XIR_OK) status=xr_xir_compile_lower(closed,&product->facts.target,&lowered,diagnostic);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(product->lowered,diagnostic);
    uint8_t layout_digest[32];
    if (status==XR_XIR_OK) status=source_product_layout_digest(lowered,&product->facts,layout_digest);
    if (status==XR_XIR_OK) {
        status=source_product_compare(layout_digest,product->facts.lowered_layout_digest,32);
        if (status==XR_XIR_BAD_STRUCTURE) status=XR_XIR_BAD_LAYOUT;
    }
    if (status==XR_XIR_OK) status=xr_xir_compile_emit_c(lowered,"source_product_proof",code_limit,&expected);
    if (status==XR_XIR_OK) status=xr_xir_compile_emit_c(product->lowered,"source_product_proof",code_limit,&actual);
    if (status==XR_XIR_OK) status=actual.length!=expected.length ? XR_XIR_BAD_STRUCTURE :
        source_product_compare(actual.text,expected.text,actual.length);
    xr_xir_compile_c_source_free(&actual);xr_xir_compile_c_source_free(&expected);xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(decoded_closed);xr_xir_compile_artifact_free(source);
    if (diagnostic) diagnostic->status=status;
    return status;
}
XR_FUNC XrXirStatus xr_xir_compile_source_product_vm_take(XrXirSourceProduct *product,
    XrXirProgram **output) {
    if (!product || !output || *output) return XR_XIR_BAD_STRUCTURE;
    if (!product->lowered) return XR_XIR_BAD_STAGE;
    return xr_xir_compile_vm_program_take(&product->lowered, output);
}
XR_FUNC XrXirStatus xr_xir_compile_source_product_emit(const XrXirSourceProduct *product,
    const char *prefix,size_t byte_limit,XrXirCSource *output) {
    if (!product || !output || output->text || output->length)
        return XR_XIR_BAD_STRUCTURE;
    if (!product->lowered) return XR_XIR_BAD_STAGE;
    return xr_xir_compile_emit_c(product->lowered,prefix,byte_limit,output);
}
XR_FUNC XrXirCtfeStatus xr_xir_compile_source_product_scalar(const XrXirSourceProduct *product,uint32_t function,
    const XrXirValue *arguments,uint32_t count,XrXirCtfeLimits limits,XrCompileRequestControl *control,XrXirValue *output) {
    if (!product || !product->lowered) return XR_XIR_CTFE_BAD_ARGUMENT;

    XrXirCtfeRequest request={.artifact=product->lowered,.function=function,.target=product->facts.target,
        .arguments=arguments,.argument_count=count,.limits=limits,.control=control};
    memcpy(request.checked_identity,product->facts.closed_digest,sizeof(request.checked_identity));
    return xr_xir_compile_ctfe_leaf(&request,output);
}

XR_FUNC const XrXirSourceDependencies *xr_xir_compile_source_product_dependencies(
    const XrXirSourceProduct *product) {
    return product ? xr_xir_compile_source_snapshot_dependencies(product->snapshot) : NULL;
}
