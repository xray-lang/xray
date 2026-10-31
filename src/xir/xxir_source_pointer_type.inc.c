/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_pointer_type.inc.c - Exact byte-pointer type annotations
 *
 * KEY CONCEPT:
 *   A canonical pointer descriptor specifies element width and maximum access.
 *   Its type spelling supplies neither an address nor an origin permission.
 */
static bool source_pointer_type_ref(SourceContext *ctx,XrTypeRef *ref,XrXirType *output) {
    if (ref->nchildren!=1 || !ref->children || source_native_type_shadowed(ctx,ref->name))
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"pointer annotation requires one unshadowed element type");
    if (ctx->depth>=128) return source_fail(ctx,NULL,XR_XIR_BUDGET,"source type depth exhausted");
    ++ctx->depth;XrXirType element=XR_XIR_UNIT;
    bool ok=source_type(ctx,ref->children[0],&element);--ctx->depth;
    if (!ok) return false;
    if (element!=XR_XIR_U8)
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"only byte pointer elements are currently executable");
    uint32_t kind=source_text_same(ctx,NULL,ref->name,"Ptr")?XR_XIR_TYPE_PTR:XR_XIR_TYPE_MUT_PTR;
    return source_intern_type(ctx,(XrXirTypeNode){.kind=kind,.element=element},output);
}
