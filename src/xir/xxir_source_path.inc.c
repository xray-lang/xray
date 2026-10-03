/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_path.inc.c - Logical source places with evaluated-once selectors
 */
static bool source_path_nodes(SourceContext *ctx, AstNode *node, AstNode **steps,
    uint32_t *count, SourceName **root) {
    *count = 0; *root = NULL;
    while (node && (node->type == AST_MEMBER_ACCESS || node->type == AST_INDEX_GET)) {
        if (!source_work(ctx,node)) return false;
        if (*count >= 128 - ctx->depth)
            return source_fail(ctx,node,XR_XIR_BUDGET,"source place depth exhausted");
        steps[(*count)++] = node;
        node = node->type == AST_MEMBER_ACCESS ? node->as.member_access.object : node->as.index_get.array;
    }
    if (node && (node->type == AST_VARIABLE || node->type == AST_THIS_EXPR))
        *root = visible_name(ctx,node->type == AST_THIS_EXPR ? "this" : node->as.variable.name);
    return true;
}
/* Method selection inspects declarations without evaluating a receiver or index.
 * Actual place construction independently checks access and records references. */
static bool source_path_type(SourceContext *ctx, AstNode *node, XrXirType *type) {
    AstNode *steps[128]; uint32_t count; SourceName *root;
    *type = XR_XIR_UNIT;
    if (!source_path_nodes(ctx,node,steps,&count,&root)) return false;
    if (!root || (root->kind != SOURCE_LOCAL && root->kind != SOURCE_SLOT)) return true;
    *type = root->type;
    while (count) {
        AstNode *step = steps[--count];
        if (step->type == AST_INDEX_GET) {
            *type = xr_xir_type_is_array(&ctx->types,*type) ? xr_xir_array_element(&ctx->types,*type) : XR_XIR_UNIT;
        } else {
            const XrXirTypeNode *container = xr_xir_type_node(&ctx->types,*type);
            if (!container || (!xr_xir_type_is_struct(&ctx->types,*type) && !xr_xir_type_is_class(&ctx->types,*type))) {
                *type = XR_XIR_UNIT; return true;
            }
            const XrXirNominalDeclaration *decl = &ctx->nominals.declarations[container->nominal.declaration];
            SourceSubstitution substitution = {container->nominal.arguments,container->nominal.argument_count};
            *type = XR_XIR_UNIT;
            const char *name = step->as.member_access.name;
            for (uint32_t f = 0; f < decl->field_count; ++f) {
                if (!source_work(ctx,step)) return false;
                const XrXirNominalField *field = &decl->fields[f];
                if (source_text_size(ctx, name) == field->name.length && source_span_same(ctx, NULL, name, field->name.bytes, field->name.length)) {
                    if (!source_substitute(ctx,&substitution,field->type,0,type)) return false;
                    break;
                }
            }
        }
        if (*type == XR_XIR_UNIT) return true;
    }
    return true;
}
static bool source_value_place(SourceContext *ctx, AstNode *node, SourceValue *place) {
    AstNode *steps[128]; uint32_t count; SourceName *root;
    if (!source_path_nodes(ctx,node,steps,&count,&root)) return false;
    /* An unrebindable class binding still lets its fields change: the identity roots the path. */
    bool object_root = root && (root->kind == SOURCE_LOCAL || root->kind == SOURCE_SLOT) &&
        xr_xir_type_is_class(&ctx->types,root->type) && !root->mutable;
    if (!root || (!root->mutable && !object_root) || (root->kind != SOURCE_LOCAL && root->kind != SOURCE_SLOT))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"value mutation requires a mutable named root");
    if (object_root && root->construction)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"class constructor field mutation is not implemented in XIR");
    SourceName saved = *root;
    AstNode *base = count ? steps[count - 1] : node;
    if (count) base = base->type == AST_MEMBER_ACCESS ? base->as.member_access.object : base->as.index_get.array;
    if (!source_query_reference(ctx,base,root,root,object_root ? XR_XIR_SOURCE_READ : XR_XIR_SOURCE_READ_WRITE)) return false;
    if (object_root) {
        SourceValue handle = {saved.index,saved.type};
        if (saved.kind == SOURCE_SLOT &&
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_SLOT_LOAD,saved.type,{0,0},{0,0},saved.index,{0}},&handle)) return false;
        if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_OBJECT_PLACE,saved.type,{handle.id,0},{0},0,{0}},place)) return false;
    } else if (!source_recipe_record(ctx,(XrXirInstruction){saved.kind == SOURCE_SLOT ? XR_XIR_SLOT_PLACE : XR_XIR_CELL_PLACE,
            saved.type,{saved.kind == SOURCE_SLOT ? 0 : saved.index,0},{0},
            saved.kind == SOURCE_SLOT ? saved.index : 0,{0}},place)) return false;
    while (count) {
        AstNode *step = steps[--count];
        if (step->type == AST_MEMBER_ACCESS) {
            uint32_t field; XrXirType type;
            if (!source_struct_field(ctx,step,place->type,step->as.member_access.name,3,&field,&type) ||
                !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_FIELD_PLACE,type,{place->id},{0},field,{0}},place)) return false;
        } else {
            if (!xr_xir_type_is_array(&ctx->types,place->type))
                return source_fail(ctx,step,XR_XIR_BAD_TYPE,"indexed place is not an Array");
            XrXirType element = xr_xir_array_element(&ctx->types,place->type); SourceValue index;
            if (!source_plan_expression(ctx, step->as.index_get.index, (SourceExpectedType){XR_XIR_I64 != XR_XIR_UNIT,XR_XIR_I64}, &index) ||
                !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_INDEX_PLACE,element,{place->id,index.id},{0},0,{0}},place)) return false;
        }
        if (!source_query_expression(ctx,step,place->type)) return false;
    }
    return true;
}
