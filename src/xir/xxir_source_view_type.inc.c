/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_view_type.inc.c - Exact borrowed annotations in the source owner
 *
 * KEY CONCEPT:
 *   A type annotation creates a descriptor, never a root or lifetime proof.
 */
static bool source_view_shape(SourceContext *ctx,XrXirType type,uint32_t *kind,XrXirType *element) {
    const XrXirTypeNode *node=xr_xir_type_node(&ctx->types,type);
    if (!node || (node->kind!=XR_XIR_TYPE_SLICE && node->kind!=XR_XIR_TYPE_MUT_SLICE)) return false;
    *kind=node->kind;*element=node->element;return true;
}
static bool source_view_type_ref(SourceContext *ctx,XrTypeRef *ref,XrXirType *type) {
    if (ref->nchildren!=1 || !ref->children || source_native_type_shadowed(ctx,ref->name))
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"borrowed view annotation requires one unshadowed element type");
    if (ctx->depth>=128) return source_fail(ctx,NULL,XR_XIR_BUDGET,"source type depth exhausted");
    ++ctx->depth;XrXirType element;
    bool valid=source_type(ctx,ref->children[0],&element);--ctx->depth;
    if (!valid) return false;
    if (element==XR_XIR_UNIT || xr_xir_type_is_cell(&ctx->types,element))
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"borrowed view element requires ordinary storable values");
    uint32_t borrowed=0;
    XrXirStatus status=xr_xir_compile_view_type_flags(&ctx->compile,&ctx->types,element,&borrowed);
    if (status!=XR_XIR_OK || borrowed)
        return source_fail(ctx,NULL,status==XR_XIR_OK ? XR_XIR_BAD_TYPE : status,
            "borrowed view elements cannot contain another borrowed view");
    XrXirTypeKind kind=source_text_same(ctx,NULL,ref->name,"Slice") ? XR_XIR_TYPE_SLICE : XR_XIR_TYPE_MUT_SLICE;
    return source_intern_type(ctx,(XrXirTypeNode){.kind=kind,.element=element},type);
}
