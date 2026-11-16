/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_destructure.inc.c - Owned product bindings prepared before publication
 */
typedef struct SourceDestructureField {
    const XrDestructurePattern *pattern;
    SourceValue value;
    SourceName *symbol;
    XrXirType type;
} SourceDestructureField;
typedef struct SourceDestructurePlan {
    SourceDestructureField *fields;
    uint32_t count,capacity,next;
} SourceDestructurePlan;
static bool source_destructure_collect(SourceContext *ctx,AstNode *node,
    const XrDestructurePattern *pattern,SourceDestructurePlan *plan,uint32_t depth) {
    if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
    if (!pattern)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"product binding pattern is missing");
    if (depth>=128)
        return source_fail(ctx,node,XR_XIR_BUDGET,"product binding nesting exhausted");
    if (pattern->type==PATTERN_SKIP) return true;
    if (pattern->type==PATTERN_IDENTIFIER) {
        size_t length=0;
        if (pattern->as.identifier.type || !pattern->as.identifier.name ||
            !source_text_length(ctx,node,pattern->as.identifier.name,&length) || !length ||
            source_text_same(ctx,node,pattern->as.identifier.name,"_"))
            return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"product binding needs an identifier or discard");
        for (uint32_t i=0;i<plan->count;++i) {
            if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
            if (source_text_same(ctx,node,plan->fields[i].pattern->as.identifier.name,pattern->as.identifier.name))
                return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"duplicate product binding name");
        }
        SourceDestructureField *fields=source_query_append(ctx,plan->fields,&plan->count,
            &plan->capacity,sizeof(*fields));
        if (!fields) return false;
        plan->fields=fields;fields[plan->count-1]=(SourceDestructureField){.pattern=pattern};
        return true;
    }
    bool tuple=pattern->type==PATTERN_TUPLE,array=pattern->type==PATTERN_ARRAY;
    int count=tuple || array ? pattern->as.array.element_count : -1;
    if (array && depth)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Array binding requires a flat declaration pattern");
    if (count<0 || (count && !pattern->as.array.elements))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"product binding fields are missing");
    for (int i=0;i<count;++i) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        const XrDestructurePattern *child=pattern->as.array.elements[i];
        if (array && (!child || (child->type!=PATTERN_IDENTIFIER && child->type!=PATTERN_SKIP)))
            return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Array binding requires names or discards");
        if (!source_destructure_collect(ctx,node,child,plan,depth+1)) return false;
    }
    return true;
}
static bool source_destructure_pattern(SourceContext *ctx,AstNode *node,SourceDestructurePlan *plan) {
    const XrDestructurePattern *pattern=node->as.destructure_decl.pattern;
    if (!pattern || (pattern->type!=PATTERN_TUPLE && pattern->type!=PATTERN_ARRAY) ||
        !node->as.destructure_decl.initializer)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"product binding and initializer required");
    return source_destructure_collect(ctx,node,pattern,plan,0);
}
static XrXirSourceRange source_destructure_range(SourceContext *ctx,const XrDestructurePattern *field) {
    return (XrXirSourceRange){ctx->module,field->line,field->column,field->end_line,field->end_column};
}
static bool source_destructure_array_length(SourceContext *ctx,AstNode *node,
    SourceValue initial,uint32_t count) {
    if (!xr_xir_type_is_array(&ctx->types,initial.type))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Array binding requires an exact Array type");
    SourceValue length,expected,condition,message;
    static const char text[]="Array binding length must match its pattern";
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ARRAY_LEN,XR_XIR_I64,
            {initial.id},{0},0,{0}},&length) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,
            {0},{0},count,{0}},&expected) &&
        source_binary_apply(ctx,node,AST_BINARY_EQ,length,expected,&condition) &&
        source_string_literal(ctx,node,text,sizeof(text)-1,&message) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ASSERT_CONDITION,XR_XIR_UNIT,
            {condition.id,message.id},{0},0,{0}},NULL);
}
static bool source_destructure_array_field(SourceContext *ctx,AstNode *node,
    SourceValue initial,uint32_t index,SourceValue *value) {
    SourceValue selector;XrHirPlaceAction shape,action;
    if (!source_hir_place_check(ctx,node,(XrHirPlaceInput){.kind=XR_HIR_PLACE_ARRAY_SHAPE,
            .parent=initial.place,.type=initial.type,.base=initial.id},&shape) ||
        !source_hir_place_emitted(ctx,&shape,NULL,ctx->bodies[ctx->function].count) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,
            {0},{0},index,{0}},&selector) ||
        !source_hir_place_check(ctx,node,(XrHirPlaceInput){.kind=XR_HIR_PLACE_INDEX_READ,
            .parent=initial.place,.type=initial.type,.base=initial.id,.key=selector.type,
            .selector=selector.id},&action)) return false;
    uint32_t before=ctx->bodies[ctx->function].count;
    return source_recipe_record(ctx,(XrXirInstruction){action.operation,action.type,
        {initial.id,selector.id},{0},0,{0}},value) && source_hir_place_emitted(ctx,&action,value,before);
}
static bool source_destructure_project(SourceContext *ctx,AstNode *node,
    const XrDestructurePattern *pattern,SourceValue initial,SourceDestructurePlan *plan,uint32_t depth) {
    if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
    if (!pattern)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"product binding pattern is missing");
    if (depth>=128)
        return source_fail(ctx,node,XR_XIR_BUDGET,"product binding nesting exhausted");
    if (pattern->type==PATTERN_SKIP) return true;
    if (pattern->type==PATTERN_IDENTIFIER) {
        if (plan->next>=plan->count || plan->fields[plan->next].pattern!=pattern)
            return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"product binding inventory mismatch");
        plan->fields[plan->next].value=initial;plan->fields[plan->next++].type=initial.type;return true;
    }
    bool tuple=pattern->type==PATTERN_TUPLE,array=pattern->type==PATTERN_ARRAY;
    if (!tuple && !array)return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"product binding requires a Tuple or Array pattern");
    uint32_t count=(uint32_t)pattern->as.array.element_count;
    if (tuple) {
        XrXirStatus status=xr_hir_tuple_arity(&ctx->types,initial.type,count,ctx->request,NULL);
        if(status!=XR_XIR_OK)return source_fail(ctx,node,status,count?
            "Tuple binding requires its exact ordered arity":"empty binding requires Unit");
    } else if (array) {
        /* The exact count check dominates every read and binding initialization. */
        if (!source_destructure_array_length(ctx,node,initial,count)) return false;
    }
    for (uint32_t i=0;i<count;++i) {
        SourceValue value;const XrDestructurePattern *child=pattern->as.array.elements[i];
        if (array && child->type==PATTERN_SKIP) continue;
        if (tuple) {
            if (!source_tuple_projection(ctx,node,initial,i,&value)) return false;
        } else {
            if (!source_destructure_array_field(ctx,node,initial,i,&value)) return false;
        }
        if (!source_destructure_project(ctx,node,child,value,plan,depth+1)) return false;
    }
    return true;
}
static bool source_destructure_binding(SourceContext *ctx,AstNode *node,bool top) {
    SourceDestructurePlan plan={0};
    if (!source_destructure_pattern(ctx,node,&plan)) return false;
    for (uint32_t i=0;i<plan.count;++i) {
        const char *name=plan.fields[i].pattern->as.identifier.name;
        for (SourceName *prior=top ? ctx->scope : ctx->locals;prior!=ctx->scope;prior=prior->next) {
            if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
            if (source_text_same(ctx,node,prior->name,name))
                return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"duplicate local product binding name");
        }
    }
    SourceValue initial={0};
    if (!expression(ctx,node->as.destructure_decl.initializer,&initial)) return false;
    if (source_value_taken(ctx,initial))
        return source_fail(ctx,node,XR_XIR_UNSUPPORTED,"product binding has no owned transfer recipe");
    if (!source_destructure_project(ctx,node,node->as.destructure_decl.pattern,initial,&plan,0)) return false;
    if (plan.next!=plan.count)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"product binding inventory is incomplete");
    uint32_t first_slot=0,payloads=0;
    SourceValue *compact=top && plan.count ? source_recipe_storage(ctx,plan.count,sizeof(*compact)) : NULL;
    if (top && plan.count && !compact) return false;
    for (uint32_t i=0;i<plan.count;++i) {
        SourceDestructureField *field=&plan.fields[i];
        if (top) {
            field->symbol=find_name(ctx,ctx->names[ctx->module],field->pattern->as.identifier.name);
            if (!field->symbol || field->symbol->kind!=SOURCE_SLOT || field->symbol->node!=node ||
                (i && field->symbol->index!=first_slot+i))
                return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"module product binding slot inventory mismatch");
            if (!i) first_slot=field->symbol->index;
            field->symbol->inferred=true;
            if (!source_root_join_record(ctx,&field->value) ||
                !source_binding_prepare(ctx,field->symbol,&field->value)) return false;
            if (field->value.type!=XR_XIR_UNIT) compact[payloads++]=field->value;
            continue;
        }
        field->symbol=source_alloc(ctx,1,sizeof(*field->symbol));
        if (!field->symbol) return false;
        bool unit=field->type==XR_XIR_UNIT&&node->as.destructure_decl.is_const;
        *field->symbol=(SourceName){.name=field->pattern->as.identifier.name,.node=node,
            .kind=unit?SOURCE_UNIT_LOCAL:SOURCE_LOCAL,.index=field->value.id,.module=ctx->module,
            .type=field->type,.mutable=!node->as.destructure_decl.is_const,.inferred=true};
        if(!source_query_declare(ctx,field->symbol,XR_XIR_SOURCE_BINDING,ctx->bodies[ctx->function].declaration,
            source_destructure_range(ctx,field->pattern)))return false;
        XrHirBodyBinding binding={0};if(!source_hir_binding_register(ctx,field->symbol,&binding))return false;
        XrHirBindingAction initialization;
        if(!node->as.destructure_decl.is_const&&!source_root_join_record(ctx,&field->value))return false;
        if(!source_hir_binding_use(ctx,node,field->symbol,XR_HIR_BINDING_INITIALIZE,field->value,&initialization))return false;
        uint32_t before=ctx->bodies[ctx->function].count;
        if(initialization.operation!=XR_XIR_INVALID&&
            !source_recipe_record(ctx,(XrXirInstruction){initialization.operation,initialization.physical,
                {initialization.logical==XR_XIR_UNIT?0:field->value.id,0},{0},0,{0}},&field->value))return false;
        if(!source_hir_binding_emitted(ctx,&initialization,&field->value,before))return false;
        field->symbol->index=initialization.route==XR_HIR_BINDING_UNIT?UINT32_MAX:field->value.id;
    }
    if (top) {
        if (!plan.count) return true;
        uint64_t packed=((uint64_t)first_slot<<32)|plan.count;
        return source_recipe_group(ctx,(XrXirInstruction){XR_XIR_SLOT_GROUP_INIT,XR_XIR_UNIT,
            {0},{0},(int64_t)packed,{0}},compact,payloads,NULL);
    }
    for (uint32_t i=0;i<plan.count;++i) {
        SourceDestructureField *field=&plan.fields[i];
        field->symbol->next=ctx->locals;ctx->locals=field->symbol;
        source_query_binding_type(ctx,field->symbol);
        if (!source_facts_assign(ctx,field->symbol,false)) return false;
    }
    return true;
}
