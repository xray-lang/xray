/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_view_prepare.inc.c - Real ordinary Source input before lifetime work
 */
#include "xxir_view_prepare_internal.h"
#include "xxir_view_liveness_internal.h"
#include "xxir_view_roots_internal.h"
#include "xxir_view_access_internal.h"
#include "xxir_source_view_normalize.inc.c"

static bool source_view_preparation_needed(SourceContext *ctx,bool *needed) {
    *needed=false;
    for (uint32_t i=0;i<ctx->types.count;++i) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        uint32_t kind=ctx->types.nodes[i].kind;
        if (kind==XR_XIR_TYPE_SLICE || kind==XR_XIR_TYPE_MUT_SLICE || kind==XR_XIR_TYPE_PTR || kind==XR_XIR_TYPE_MUT_PTR) {*needed=true;break;}
    }
    return true;
}
/* Construction precedes numeric collection and is replaced after relocation. */
static bool source_construction_ready(SourceContext *ctx) {
    return ctx->construction || source_construction_prepare(ctx);
}
static bool source_view_prepare(SourceContext *ctx) {
    bool needed=false;
    if (!source_view_preparation_needed(ctx,&needed)) return false;
    if (!needed) return true;
    if (!ctx->semantic_prepare || !ctx->construction)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"view lifetime preparation requires a private semantic owner");
    XrXirDeclarations declarations;XrXirModule module=source_module_view(ctx,&declarations);
    XrXirDiagnostic diagnostic={XR_XIR_OK,UINT32_MAX,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE};
    XrXirStatus status=xir_compile_view_prepare_ordinary(&ctx->compile,(XirEffectInput){&module,ctx->construction,ctx->request},&diagnostic);
    for (uint32_t f=0;status==XR_XIR_OK && f<ctx->function_count;++f) {
        if (!ctx->bodies[f].initialization_regions) continue;
        status=xr_xir_compile_initialization_check(&ctx->compile,&module,f,ctx->bodies[f].initialization_regions,&diagnostic);
    }
    if (status==XR_XIR_OK) status=xir_compile_view_return_origins_verified(&ctx->compile,(XirEffectInput){&module,ctx->construction,ctx->request},&diagnostic);
    if (status==XR_XIR_OK) status=xir_compile_view_affine_verified(&ctx->compile,&module,&diagnostic);
    if (status==XR_XIR_OK) status=xir_compile_view_liveness_verified(&ctx->compile,&module,NULL,NULL,&diagnostic);
    if (status==XR_XIR_OK) status=xir_compile_view_roots_verified(&ctx->compile,(XirEffectInput){&module,ctx->construction,ctx->request},NULL,NULL,&diagnostic);
    bool normalizing=status==XR_XIR_OK;
    if (normalizing) status=source_view_normalize(ctx,&module,&diagnostic);
    if (status==XR_XIR_OK) return true;
    AstNode *site=NULL;
    if (diagnostic.function<ctx->function_count) {
        site=ctx->bodies[diagnostic.function].node;ctx->module=ctx->bodies[diagnostic.function].module;
    }
    char message[256]={0};const char *operation="unknown";
    if (diagnostic.function<module.function_count &&
        diagnostic.instruction<module.functions[diagnostic.function].instruction_count)
        operation=xr_xir_op_name(module.functions[diagnostic.function].instructions[diagnostic.instruction].op);
    if (!operation) operation="unknown";
    if (status!=XR_XIR_BUDGET && status!=XR_XIR_OUT_OF_MEMORY && source_format(ctx,message,sizeof(message),
        "constructed XIR failed checking %s view lifetime normalization at function %u block %u instruction %u (%s)",
        normalizing?"during":"before",diagnostic.function,diagnostic.block,diagnostic.instruction,operation)!=XR_DIAG_OK)
        status=XR_XIR_BUDGET;
    return source_fail(ctx,site,status,message);
}
