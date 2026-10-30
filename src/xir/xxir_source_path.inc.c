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
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
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
            *type = xr_xir_type_is_array(&ctx->types,*type) ? xr_xir_array_element(&ctx->types,*type) :
                xr_xir_type_is_map(&ctx->types,*type) ? xr_xir_map_element(&ctx->types,*type) : XR_XIR_UNIT;
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
                if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
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
    XrHirBodyBinding binding={0};
    if(root&&!source_hir_binding_receive(ctx,root,&binding))return false;
    XrHirPlaceAction root_action;
    if(!source_hir_place_check(ctx,node,(XrHirPlaceInput){.kind=XR_HIR_PLACE_ROOT,
        .root=binding,.type=root_type,.reference=reference},&root_action))return false;
    bool object_root=root_action.object_root;
    SourceName saved = *root;
    AstNode *base = count ? steps[count - 1] : node;
    if (count) base = base->type == AST_MEMBER_ACCESS ? base->as.member_access.object : base->as.index_get.array;
    if (!source_query_reference(ctx,base,root,root,object_root ? XR_XIR_SOURCE_READ : XR_XIR_SOURCE_READ_WRITE)) return false;
    uint32_t root_before=ctx->bodies[ctx->function].count;
    if (object_root) {
        SourceValue handle;
        if (!source_binding_read(ctx, &saved, &handle)) return false;
        /* A binding narrowed to hold a value roots the path at that value. */
        if (narrowed) {
            uint32_t prefix=source_fact_depth(ctx,root);
            for (uint32_t layer=0;layer<prefix;++layer)
                if (!((ctx)->diagnostic.status == XR_XIR_OK) || !source_unwrap_value(ctx,node,handle,&handle)) return false;
        }
        if (!source_recipe_record(ctx,(XrXirInstruction){root_action.operation,root_action.type,{handle.id,0},{0},0,{0}},place)) return false;
    } else {
        SourceValue cell = {0};
        if (!source_binding_cell(ctx, &saved, &cell) ||
            !source_recipe_record(ctx,(XrXirInstruction){root_action.operation,
                root_action.type,{cell.id,0},{0},0,{0}},place)) return false;
    }
    if(!source_hir_place_emitted(ctx,&root_action,place,root_before))return false;
    place->place=root_action.place;
    while (count) {
        AstNode *step = steps[--count];
        if (step->type == AST_MEMBER_ACCESS) {
            XrHirPlaceAction receiver_action;
            if(!source_hir_place_check(ctx,step,(XrHirPlaceInput){.kind=XR_HIR_PLACE_FIELD_BASE,.parent=place->place},&receiver_action)||
                !source_hir_place_emitted(ctx,&receiver_action,NULL,ctx->bodies[ctx->function].count))return false;
            uint32_t field; XrXirType type;
            if(!source_struct_field(ctx,step,receiver_action.type,step->as.member_access.name,3,&field,&type))return false;
            XrHirPlaceAction action;
            if(!source_hir_place_check(ctx,step,(XrHirPlaceInput){.kind=XR_HIR_PLACE_FIELD,
                .parent=place->place,.type=type,.field=field,.access=3},&action))return false;
            uint32_t before=ctx->bodies[ctx->function].count;
            if(!source_recipe_record(ctx,(XrXirInstruction){action.operation,action.type,{place->id},{0},action.immediate,{0}},place)||
                !source_hir_place_emitted(ctx,&action,place,before))return false;
            place->place=action.place;
        } else {
            XrHirPlaceAction shape;
            if(!source_hir_place_check(ctx,step,(XrHirPlaceInput){.kind=XR_HIR_PLACE_SHAPE,.parent=place->place},&shape)||
                !source_hir_place_emitted(ctx,&shape,NULL,ctx->bodies[ctx->function].count))return false;
            XrXirType key=shape.key;SourceValue index;
            if (plan) {
                if (selector >= plan->count)
                    return source_fail(ctx,step,XR_XIR_BAD_STRUCTURE,"reference selector plan is incomplete");
                SourceExpressionPlan *index_plan = plan->selectors[selector++];
                if (!index_plan || index_plan->owner != ctx->function ||
                    index_plan->syntax != step->as.index_get.index || index_plan->state != SOURCE_TERM_UNRESOLVED ||
                    !index_plan->expected.present || index_plan->expected.type != key)
                    return source_fail(ctx,step,XR_XIR_BAD_STRUCTURE,"reference selector plan does not match its path");
                if (!source_plan_complete(ctx,index_plan,&index)) return false;
            } else if (!source_plan_expression(ctx,step->as.index_get.index,
                (SourceExpectedType){true,key,false,false},&index)) return false;
            XrHirPlaceAction action;
            if(!source_hir_place_check(ctx,step,(XrHirPlaceInput){.kind=XR_HIR_PLACE_INDEX,.parent=place->place,
                .key=index.type,.selector=index.id},&action))return false;
            uint32_t before=ctx->bodies[ctx->function].count;
            if(!source_recipe_record(ctx,(XrXirInstruction){action.operation,action.type,{place->id,index.id},{0},0,{0}},place)||
                !source_hir_place_emitted(ctx,&action,place,before))return false;
            place->place=action.place;
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
    if(!source_hir_reference(ctx,node,root,root_node!=node,"ref requires a mutable stored binding"))return false;
    if (root_node==node) {
        if (plan) return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"binding ref has a projected selector plan");
        return source_query_reference(ctx,node,root,root,XR_XIR_SOURCE_READ_WRITE) &&
            source_binding_cell(ctx,root,cell);
    }
    SourceValue place={0}; XrXirType physical;
    if (!source_value_place_mode(ctx,node,&place,true,plan)) return false;
    XrHirPlaceAction leaf;
    if(!source_hir_place_check(ctx,node,(XrHirPlaceInput){.kind=XR_HIR_PLACE_REF_LEAF,.parent=place.place},&leaf)||
        !source_hir_place_emitted(ctx,&leaf,NULL,ctx->bodies[ctx->function].count)||!source_cell_type(ctx,leaf.type,&physical))return false;
    XrHirPlaceAction action;
    if(!source_hir_place_check(ctx,node,(XrHirPlaceInput){.kind=XR_HIR_PLACE_REFERENCE,
        .parent=place.place,.type=physical},&action))return false;
    uint32_t before=ctx->bodies[ctx->function].count;
    return source_recipe_record(ctx,(XrXirInstruction){action.operation,action.type,{place.id,0},{0},0,{0}},cell)&&
        source_hir_place_emitted(ctx,&action,cell,before);
}
static bool source_reference_place(SourceContext *ctx, AstNode *node, SourceValue *cell) {
    return source_reference_place_planned(ctx,node,NULL,cell);
}
static bool source_reference_effects(SourceContext *ctx, AstNode *node, SourceReferencePlan **output) {
    AstNode *steps[128]; uint32_t count, selectors = 0; SourceName *root;
    *output = NULL;
    if (!source_path_nodes(ctx,node,steps,&count,&root)) return false;
    for (uint32_t step = 0; step < count; ++step) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
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
        XrXirType container = XR_XIR_UNIT;
        if (!source_path_type(ctx,step->as.index_get.array,&container)) return false;
        XrHirPlaceAction hint;
        if(!source_hir_place_check(ctx,step,(XrHirPlaceInput){.kind=XR_HIR_PLACE_HINT_SHAPE,.type=container,
            .failure="indexed ref requires an Array or Map"},&hint))return false;
        XrXirType key=hint.key;
        SourceExpressionPlan *index=source_plan_collect(ctx,step->as.index_get.index,
            (SourceExpectedType){true,key,false,false});
        if (!index || !source_planned_effects(ctx,index,0)) return false;
        plan->selectors[selector++] = index;
    }
    *output = plan; return true;
}
