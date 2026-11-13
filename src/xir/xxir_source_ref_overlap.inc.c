/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_ref_overlap.inc.c - Exclusive ref places of one call
 *
 * KEY CONCEPT:
 *   Every ref argument of one call, and a ref receiver, borrows its place
 *   exclusively, so no two of them may overlap. Places are compared by their
 *   paths from the root binding: only a split at different field names proves
 *   two places disjoint; an index never does, even between two constants, and
 *   a path that ends first contains the other. Fields of class objects are
 *   compared the same way from every pair of class receivers, because two
 *   different paths can reach one object.
 */
typedef enum SourceRefOverlap {
    SOURCE_REF_DISJOINT,
    SOURCE_REF_CONTAINED,
    SOURCE_REF_INDEXED,
} SourceRefOverlap;
/* Selectors run root-first; each records the declared type it selects from,
 * UNIT when that type is not known. */
typedef struct SourceRefPath {
    AstNode *steps[128];
    XrXirType receivers[128];
    uint32_t count;
    SourceName *root;
} SourceRefPath;
static bool source_ref_path(SourceContext *ctx, AstNode *node, SourceRefPath *path) {
    AstNode *steps[128]; uint32_t count;
    if (!source_path_nodes(ctx,node,steps,&count,&path->root)) return false;
    if (!path->root)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"ref argument requires an authentic named place root");
    XR_CHECK(count <= 128,"source place depth stays within its selector table");
    XrXirType type = path->root->kind == SOURCE_LOCAL || path->root->kind == SOURCE_SLOT ?
        source_symbol_type(ctx,path->root) : XR_XIR_UNIT;
    if (xr_xir_type_is_cell(&ctx->types,type)) type = xr_xir_cell_element(&ctx->types,type);
    path->count = count;
    for (uint32_t level = 0; level < count; ++level) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        path->steps[level] = steps[count - 1 - level];
        path->receivers[level] = type;
        if (type != XR_XIR_UNIT && !source_path_step_type(ctx,path->steps[level],&type)) return false;
    }
    return ctx->diagnostic.status == XR_XIR_OK;
}
/* Compares two paths that start at one storage owner. An index pair may select
 * one element, so the walk continues as if it did: different elements are
 * disjoint anyway, and on one element a later split at different field names
 * still proves the places disjoint. A path that ends first contains the other. */
static SourceRefOverlap source_ref_paths_from(SourceContext *ctx, const SourceRefPath *first, uint32_t a,
    const SourceRefPath *second, uint32_t b) {
    SourceRefOverlap overlap = SOURCE_REF_CONTAINED;
    for (; a < first->count && b < second->count; ++a, ++b) {
        AstNode *left = first->steps[a], *right = second->steps[b];
        if (left->type != AST_MEMBER_ACCESS || right->type != AST_MEMBER_ACCESS) {
            overlap = SOURCE_REF_INDEXED;
            continue;
        }
        if (!source_text_same(ctx,left,left->as.member_access.name,right->as.member_access.name))
            return SOURCE_REF_DISJOINT;
    }
    return overlap;
}
/* A field step selects storage inside a class object when its receiver is a
 * class; an unknown receiver type is treated as one. */
static bool source_ref_object_step(SourceContext *ctx, const SourceRefPath *path, uint32_t level) {
    if (path->steps[level]->type != AST_MEMBER_ACCESS) return false;
    XrXirType receiver = path->receivers[level];
    return receiver == XR_XIR_UNIT || xr_xir_type_is_class(&ctx->types,receiver);
}
static bool source_ref_class_descends(SourceContext *ctx, uint32_t declaration, uint32_t ancestor) {
    for (uint32_t depth = 0; depth <= ctx->nominals.count; ++depth) {
        if (declaration >= ctx->nominals.count) return true;
        if (declaration == ancestor) return true;
        uint32_t parent = ctx->nominals.declarations[declaration].parent;
        if (!parent) return false;
        declaration = parent - 1;
    }
    return true;
}
/* Two receivers can be one object only when one class is the other or
 * inherits from it. Unknown types stay conservative. */
static bool source_ref_classes_related(SourceContext *ctx, XrXirType first, XrXirType second) {
    if (first == XR_XIR_UNIT || second == XR_XIR_UNIT || first == second) return true;
    const XrXirTypeNode *a = xr_xir_type_node(&ctx->types,first), *b = xr_xir_type_node(&ctx->types,second);
    if (!a || !b) return true;
    return source_ref_class_descends(ctx,a->nominal.declaration,b->nominal.declaration) ||
        source_ref_class_descends(ctx,b->nominal.declaration,a->nominal.declaration);
}
/* Fails when the place of `later` may overlap `earlier`, another ref place of
 * the same call; the diagnostic names the root that starts the later place.
 * Places in class objects reached from different receivers conflict only when
 * both receivers are one object; that needs a run-time identity check, which
 * compiled code cannot perform yet, so such a call is rejected rather than
 * admitted unchecked. */
static bool source_reference_disjoint(SourceContext *ctx, AstNode *earlier, AstNode *later) {
    SourceRefPath first, second;
    if (!source_ref_path(ctx,earlier,&first) || !source_ref_path(ctx,later,&second)) return false;
    AstNode *site = source_ref_root_syntax(ctx,later);
    if (!site) return source_fail(ctx,later,XR_XIR_BAD_TYPE,"ref argument requires an authentic named place root");
    if (first.root == second.root) {
        SourceRefOverlap overlap = source_ref_paths_from(ctx,&first,0,&second,0);
        if (ctx->diagnostic.status != XR_XIR_OK) return false;
        if (overlap == SOURCE_REF_CONTAINED)
            return source_fail(ctx,site,XR_XIR_BAD_TYPE,"ref arguments of one call must not alias");
        if (overlap == SOURCE_REF_INDEXED)
            return source_fail(ctx,site,XR_XIR_BAD_TYPE,
                "ref arguments of one call must not alias; an index never proves two places disjoint");
    }
    for (uint32_t a = 0; a < first.count; ++a) {
        if (!source_request_poll(ctx,site)) return false;
        if (!source_ref_object_step(ctx,&first,a)) continue;
        for (uint32_t b = 0; b < second.count; ++b) {
            if (!source_ref_object_step(ctx,&second,b) ||
                !source_ref_classes_related(ctx,first.receivers[a],second.receivers[b])) continue;
            SourceRefOverlap overlap = source_ref_paths_from(ctx,&first,a,&second,b);
            if (ctx->diagnostic.status != XR_XIR_OK) return false;
            if (overlap != SOURCE_REF_DISJOINT)
                return source_fail(ctx,site,XR_XIR_UNSUPPORTED,
                    "ref arguments of one call may reach one class field through different objects; "
                    "the run-time identity check this needs is not supported yet");
        }
    }
    return true;
}
