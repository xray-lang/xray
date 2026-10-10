/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_declarations.inc.c - Immutable declaration copy storage
 *
 * KEY CONCEPT:
 *   The private certificate owns one packed declaration block. The common
 *   implementation-table owner and every copied value retain their own rules.
 */
#include "xxir_implementation.h"

_Static_assert(sizeof(XrXirDeclarations)%_Alignof(XrXirSourceModule)==0 &&
    sizeof(XrXirSourceModule)%_Alignof(XrXirLiteral)==0 &&
    sizeof(XrXirLiteral)%_Alignof(XrXirFunctionIdentity)==0 &&
    sizeof(XrXirFunctionIdentity)%_Alignof(XrXirSlot)==0 &&
    sizeof(XrXirSlot)%_Alignof(uint32_t)==0,
    "Certificate declaration sections preserve complete child alignment");

typedef struct EffectInvocationDeclarationLayout {
    size_t bytes,modules,literals,functions,slots,dependencies,names;
} EffectInvocationDeclarationLayout;

static XrXirStatus effect_invocation_declaration_add(const XrXirCompileContext *work,
    EffectInvocationDeclarationLayout *layout,uint64_t count,size_t size) {
    if (!xir_compile_work(work,1) || !size || count>SIZE_MAX/size)
        return XR_XIR_BUDGET;
    size_t bytes=(size_t)count*size;
    if (bytes>SIZE_MAX-layout->bytes) return XR_XIR_BUDGET;
    layout->bytes+=bytes;return XR_XIR_OK;
}

/* All counts describe actual source arrays. This extra sizing traversal is
 * charged before access; copies retain their original byte and zeroing fees. */
static XrXirStatus effect_invocation_declaration_layout(const XrXirCompileContext *work,
    const XrXirDeclarations *source,uint32_t functions,EffectInvocationDeclarationLayout *layout) {
    if (!xir_compile_work(work,7)) return XR_XIR_BUDGET;
    if (!source->modules || (source->literal_count && !source->literals)) return XR_XIR_BAD_STRUCTURE;
    layout->bytes=sizeof(*source);layout->modules=layout->bytes;
    XrXirStatus status=effect_invocation_declaration_add(work,layout,source->module_count,sizeof(*source->modules));
    if (status!=XR_XIR_OK) return status;
    layout->literals=layout->bytes;
    status=effect_invocation_declaration_add(work,layout,source->literal_count,sizeof(*source->literals));
    if (status!=XR_XIR_OK) return status;
    layout->functions=layout->bytes;
    status=effect_invocation_declaration_add(work,layout,functions,sizeof(*source->functions));
    if (status!=XR_XIR_OK) return status;
    layout->slots=layout->bytes;
    status=effect_invocation_declaration_add(work,layout,source->slot_count,sizeof(*source->slots));
    if (status!=XR_XIR_OK) return status;
    layout->dependencies=layout->bytes;
    for (uint32_t m=0;m<source->module_count;++m) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        status=effect_invocation_declaration_add(work,layout,source->modules[m].dependency_count,sizeof(uint32_t));
        if (status!=XR_XIR_OK) return status;
    }
    layout->names=layout->bytes;
    for (uint32_t m=0;m<source->module_count;++m) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        status=effect_invocation_declaration_add(work,layout,source->modules[m].name_length,1);
        if (status!=XR_XIR_OK) return status;
    }
    for (uint32_t l=0;l<source->literal_count;++l) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        status=effect_invocation_declaration_add(work,layout,source->literals[l].length,1);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

static void *effect_invocation_declaration_copy(const XrXirCompileContext *work,
    void *destination,const void *source,size_t bytes,XrXirStatus *status) {
    if (*status!=XR_XIR_OK || !bytes) return NULL;
    if (!source) { *status=XR_XIR_BAD_STRUCTURE;return NULL; }
    if (!xir_compile_work(work,bytes)) { *status=XR_XIR_BUDGET;return NULL; }
    memcpy(destination,source,bytes);return destination;
}

static void effect_invocation_declarations_free(XrXirDeclarations *declarations) {
    if (!declarations) return;
    xr_xir_compile_implementations_free((XrXirImplementationTable *)declarations->implementations);
    xr_compile_resources_free(declarations);
}

/* There is no independently published owner with an empty descriptor table.
 * The canonical clone preserves its NULL result and original failure ledger;
 * none of its storage can escape into the private packed owner. */
static XrXirStatus effect_invocation_declarations_empty(const XrXirCompileContext *work,
    const XrXirDeclarations *source,uint32_t functions) {
    XrXirDeclarations *copy=NULL;
    XrXirStatus status=xr_xir_compile_declarations_clone(work,source,functions,&copy);
    if (copy) { xr_xir_compile_declarations_free(copy);return XR_XIR_BAD_STRUCTURE; }
    return status;
}

static XrXirStatus effect_invocation_declarations_clone(const XrXirCompileContext *work,
    const XrXirDeclarations *source,uint32_t functions,XrXirDeclarations **output) {
    if (!xir_compile_context_valid(work) || !output || *output) return XR_XIR_BAD_STRUCTURE;
    if (!source) return XR_XIR_OK;
    if (!source->module_count || !functions) return effect_invocation_declarations_empty(work,source,functions);
    /* This is the original declaration-copy traversal fee. */
    if (!xir_compile_work(work,(uint64_t)source->module_count+source->literal_count+1)) return XR_XIR_BUDGET;
    EffectInvocationDeclarationLayout layout={0};
    XrXirStatus status=effect_invocation_declaration_layout(work,source,functions,&layout);
    if (status!=XR_XIR_OK) return status;
    XrXirDeclarations *copy=xir_compile_alloc(work,layout.bytes,&status);
    if (!copy) return status;
    if (!xir_compile_work(work,sizeof(*copy))) { xr_compile_resources_free(copy);return XR_XIR_BUDGET; }
    memset(copy,0,sizeof(*copy));
    *copy=*source;copy->implementations=NULL;
    uint8_t *storage=(uint8_t *)copy;
    XrXirSourceModule *modules=(XrXirSourceModule *)(storage+layout.modules);
    XrXirLiteral *literals=source->literal_count ? (XrXirLiteral *)(storage+layout.literals):NULL;
    uint32_t *dependencies=(uint32_t *)(storage+layout.dependencies);
    char *bytes=(char *)(storage+layout.names);
    size_t module_bytes=(size_t)source->module_count*sizeof(*modules);
    size_t literal_bytes=(size_t)source->literal_count*sizeof(*literals);
    if (!xir_compile_work(work,module_bytes+literal_bytes)) { status=XR_XIR_BUDGET;goto failed; }
    memset(modules,0,module_bytes);if (literal_bytes)memset(literals,0,literal_bytes);
    copy->modules=modules;copy->literals=literals;
    copy->functions=effect_invocation_declaration_copy(work,storage+layout.functions,source->functions,
        (size_t)functions*sizeof(*source->functions),&status);
    copy->slots=effect_invocation_declaration_copy(work,storage+layout.slots,source->slots,
        (size_t)source->slot_count*sizeof(*source->slots),&status);
    if (status!=XR_XIR_OK) goto failed;
    for (uint32_t m=0;m<source->module_count;++m) {
        const XrXirSourceModule *from=&source->modules[m];modules[m]=*from;
        modules[m].name=effect_invocation_declaration_copy(work,bytes,from->name,from->name_length,&status);
        bytes+=from->name_length;
        modules[m].dependencies=effect_invocation_declaration_copy(work,dependencies,from->dependencies,
            (size_t)from->dependency_count*sizeof(*dependencies),&status);
        dependencies+=from->dependency_count;
        if (status!=XR_XIR_OK || !modules[m].name) goto failed;
    }
    for (uint32_t l=0;l<source->literal_count;++l) {
        const XrXirLiteral *from=&source->literals[l];literals[l].length=from->length;
        literals[l].bytes=effect_invocation_declaration_copy(work,bytes,from->bytes,from->length,&status);
        bytes+=from->length;if (status!=XR_XIR_OK) goto failed;
    }
    XrXirImplementationTable *implementations=NULL;
    status=xr_xir_compile_implementations_copy_verified(work,source->implementations,&implementations);
    if (status!=XR_XIR_OK) goto failed;
    copy->implementations=implementations;*output=copy;return XR_XIR_OK;
failed:
    effect_invocation_declarations_free(copy);return status;
}
