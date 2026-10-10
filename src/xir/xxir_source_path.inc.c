/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_path.inc.c - Logical source places with evaluated-once selectors
 */
/* The enclosing region owns these plans and their selectors until sealing. */
struct SourceReferencePlan {
    AstNode *syntax;
    uint32_t owner, count;
    SourceExpressionPlan **selectors;
};
_Static_assert(sizeof(SourceReferencePlan) % _Alignof(SourceExpressionPlan *) == 0 &&
    _Alignof(SourceReferencePlan) >= _Alignof(SourceExpressionPlan *),"selector plan view alignment");
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
    *type = source_symbol_type(ctx, root);
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
static bool source_value_place_mode(SourceContext *ctx, AstNode *node, SourceValue *place,
    bool reference, const SourceReferencePlan *plan) {
    AstNode *steps[128]; uint32_t count; SourceName *root;
    if (!source_path_nodes(ctx,node,steps,&count,&root)) return false;
    uint32_t selector = 0;
    if (plan && (!reference || plan->owner != ctx->function || plan->syntax != node || !plan->count ||
        plan->count > count || ctx->bodies[ctx->function].region_sealed))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"reference selector plan has no active owner");
    /* An unrebindable class binding still lets its fields change: the identity roots the path. */
    XrXirType root_type = root ? source_symbol_type(ctx,root) : XR_XIR_UNIT;
    bool narrowed = root && root_type != root->type;
    if (reference && (narrowed || !root || root->construction ||
        (!xr_xir_type_is_struct(&ctx->types,root_type) && !xr_xir_type_is_array(&ctx->types,root_type) &&
            !xr_xir_type_is_class(&ctx->types,root_type))))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"projected ref requires an actual stored root");
    bool object_root = root && (root->kind == SOURCE_LOCAL || root->kind == SOURCE_SLOT) &&
        xr_xir_type_is_class(&ctx->types,root_type);
    if (!root || (!root->mutable && !object_root) || (root->kind != SOURCE_LOCAL && root->kind != SOURCE_SLOT))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"value mutation requires a mutable named root");
    if (object_root && root->construction)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"class constructor field mutation is not implemented in XIR");
    SourceName saved = *root;
    AstNode *base = count ? steps[count - 1] : node;
    if (count) base = base->type == AST_MEMBER_ACCESS ? base->as.member_access.object : base->as.index_get.array;
    if (!source_query_reference(ctx,base,root,root,object_root ? XR_XIR_SOURCE_READ : XR_XIR_SOURCE_READ_WRITE)) return false;
    if (object_root) {
        SourceValue handle;
        if (!source_binding_read(ctx, &saved, &handle)) return false;
        /* A binding narrowed to hold a value roots the path at that value. */
        if (narrowed) {
            uint32_t prefix=source_fact_depth(ctx,root);
            for (uint32_t layer=0;layer<prefix;++layer)
                if (!source_work(ctx,node) || !source_unwrap_value(ctx,node,handle,&handle)) return false;
        }
        if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_OBJECT_PLACE,root_type,{handle.id,0},{0},0,{0}},place)) return false;
    } else {
        SourceValue cell = {0};
        if (!source_binding_cell(ctx, &saved, &cell) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_PLACE,
                saved.type,{cell.id,0},{0},0,{0}},place)) return false;
    }
    uint32_t initial_count = count;
    while (count) {
        AstNode *step = steps[--count];
        if (step->type == AST_MEMBER_ACCESS) {
            if (reference && !xr_xir_type_is_struct(&ctx->types,place->type) &&
                !(object_root && count + 1 == initial_count))
                return source_fail(ctx,step,XR_XIR_BAD_TYPE,"projected ref cannot cross a Class identity");
            uint32_t field; XrXirType type;
            if (!source_struct_field(ctx,step,place->type,step->as.member_access.name,3,&field,&type) ||
                !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_FIELD_PLACE,type,{place->id},{0},field,{0}},place)) return false;
        } else {
            if (!xr_xir_type_is_array(&ctx->types,place->type))
                return source_fail(ctx,step,XR_XIR_BAD_TYPE,"indexed place is not an Array");
            XrXirType element = xr_xir_array_element(&ctx->types,place->type); SourceValue index;
            if (plan) {
                if (selector >= plan->count)
                    return source_fail(ctx,step,XR_XIR_BAD_STRUCTURE,"reference selector plan is incomplete");
                SourceExpressionPlan *index_plan = plan->selectors[selector++];
                if (!index_plan || index_plan->owner != ctx->function ||
                    index_plan->syntax != step->as.index_get.index || index_plan->state != SOURCE_TERM_UNRESOLVED ||
                    !index_plan->expected.present || index_plan->expected.type != XR_XIR_I64)
                    return source_fail(ctx,step,XR_XIR_BAD_STRUCTURE,"reference selector plan does not match its path");
                if (!source_plan_complete(ctx,index_plan,&index)) return false;
            } else if (!source_plan_expression(ctx,step->as.index_get.index,
                (SourceExpectedType){true,XR_XIR_I64,false,false},&index)) return false;
            if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_INDEX_PLACE,element,{place->id,index.id},{0},0,{0}},place)) return false;
        }
        if (!source_query_expression(ctx,step,place->type)) return false;
    }
    if (plan && selector != plan->count)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"reference selector plan has unused selectors");
    return true;
}

static bool source_value_place(SourceContext *ctx, AstNode *node, SourceValue *place) {
    return source_value_place_mode(ctx,node,place,false,NULL);
}
static bool source_reference_place_planned(SourceContext *ctx, AstNode *node,
    const SourceReferencePlan *plan, SourceValue *cell) {
    AstNode *root_node=source_ref_root_syntax(ctx,node);
    SourceName *root=source_ref_binding(ctx,node);
    bool object_field = root && root_node != node && xr_xir_type_is_class(&ctx->types,source_symbol_type(ctx,root));
    if (!root || (!root->mutable && !object_field) || root->construction ||
        (root->kind!=SOURCE_LOCAL && root->kind!=SOURCE_SLOT && root->kind!=SOURCE_UNIT_SLOT))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"ref requires a mutable stored binding");
    if (root_node==node) {
        if (plan) return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"binding ref has a projected selector plan");
        return source_query_reference(ctx,node,root,root,XR_XIR_SOURCE_READ_WRITE) &&
            source_binding_cell(ctx,root,cell);
    }
    SourceValue place={0}; XrXirType physical;
    if (!source_value_place_mode(ctx,node,&place,true,plan)) return false;
    if (place.type==XR_XIR_UNIT)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"projected ref requires an admitted stored leaf type");
    if (xr_xir_type_is_class(&ctx->types,place.type) || xr_xir_type_is_cell(&ctx->types,place.type))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"projected Class ref requires its own object borrow owner");
    return source_cell_type(ctx,place.type,&physical) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_PROJECT,physical,{place.id,0},{0},0,{0}},cell);
}
static bool source_reference_place(SourceContext *ctx, AstNode *node, SourceValue *cell) {
    return source_reference_place_planned(ctx,node,NULL,cell);
}
static bool source_reference_effects(SourceContext *ctx, AstNode *node, SourceReferencePlan **output) {
    AstNode *steps[128]; uint32_t count, selectors = 0; SourceName *root;
    *output = NULL;
    if (!source_path_nodes(ctx,node,steps,&count,&root)) return false;
    for (uint32_t step = 0; step < count; ++step) {
        if (!source_work(ctx,steps[step])) return false;
        if (steps[step]->type == AST_INDEX_GET) ++selectors;
    }
    if (!selectors) return true;
    /* The bounded path fixes one pointer per real selector; the call's union
     * stores this descriptor without allocating a second argument table. */
    SourceReferencePlan *plan = source_recipe_storage(ctx,1,
        sizeof(*plan) + (size_t)selectors * sizeof(*plan->selectors));
    if (!plan) return false;
    plan->syntax = node; plan->owner = ctx->function; plan->count = selectors;
    plan->selectors = (SourceExpressionPlan **)(plan + 1);
    uint32_t selector = 0;
    while (count) {
        AstNode *step=steps[--count];
        if (step->type!=AST_INDEX_GET) continue;
        SourceExpressionPlan *index=source_plan_collect(ctx,step->as.index_get.index,
            (SourceExpectedType){true,XR_XIR_I64,false,false});
        if (!index || !source_planned_effects(ctx,index,0)) return false;
        plan->selectors[selector++] = index;
    }
    *output = plan; return true;
}
