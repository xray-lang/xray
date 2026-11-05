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
        if (kind==XR_XIR_TYPE_SLICE || kind==XR_XIR_TYPE_MUT_SLICE) {*needed=true;break;}
    }
    return true;
}
/* Lifetime work publishes the relocated immutable construction owner before
 * effect records capture the final SSA numbering. */
static bool source_construction_ready(SourceContext *ctx) {
    return ctx->construction || source_construction_prepare(ctx);
}
static bool source_view_prepare(SourceContext *ctx) {
    bool needed=false;
    if (!source_view_preparation_needed(ctx,&needed)) return false;
    if (!needed) return true;
    if (ctx->effect_evidence)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"view lifetime preparation requires unfrozen Source effects");
    if (!source_construction_prepare(ctx)) return false;
    XrXirDeclarations declarations;XrXirModule module=source_module_view(ctx,&declarations);
    XrXirDiagnostic diagnostic={XR_XIR_OK,UINT32_MAX,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE};
    XrXirStatus status=xir_compile_view_prepare_ordinary(&ctx->compile,&module,ctx->construction,&diagnostic);
    for (uint32_t f=0;status==XR_XIR_OK && f<ctx->function_count;++f) {
        if (!ctx->bodies[f].initialization_regions) continue;
        status=xr_xir_compile_initialization_check(&ctx->compile,&module,f,ctx->bodies[f].initialization_regions,&diagnostic);
    }
    if (status==XR_XIR_OK) status=xir_compile_view_return_origins_verified(&ctx->compile,&module,&diagnostic);
    if (status==XR_XIR_OK) status=xir_compile_view_affine_verified(&ctx->compile,&module,&diagnostic);
    if (status==XR_XIR_OK) status=xir_compile_view_liveness_verified(&ctx->compile,&module,NULL,NULL,&diagnostic);
    if (status==XR_XIR_OK) status=xir_compile_view_roots_verified(&ctx->compile,&module,NULL,NULL,&diagnostic);
    bool normalizing=status==XR_XIR_OK;
    if (normalizing) status=source_view_normalize(ctx,&module,&diagnostic);
    if (status==XR_XIR_OK) return true;
    AstNode *site=NULL;
    if (diagnostic.function<ctx->function_count) {
        site=ctx->bodies[diagnostic.function].node;ctx->module=ctx->bodies[diagnostic.function].module;
    }
    return source_fail(ctx,site,status,normalizing?
        "constructed XIR failed checking during view lifetime normalization":
        "constructed XIR failed checking before view lifetime normalization");
}
