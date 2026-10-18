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
    if (!source_work(ctx,node)) return false;
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
            if (!source_work(ctx,node)) return false;
            if (source_text_same(ctx,node,plan->fields[i].pattern->as.identifier.name,pattern->as.identifier.name))
                return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"duplicate product binding name");
        }
        SourceDestructureField *fields=source_query_append(ctx,plan->fields,&plan->count,
            &plan->capacity,sizeof(*fields));
        if (!fields) return false;
        plan->fields=fields;fields[plan->count-1]=(SourceDestructureField){.pattern=pattern};
        return true;
    }
    bool tuple=pattern->type==PATTERN_TUPLE,object=pattern->type==PATTERN_OBJECT;
    int count=tuple ? pattern->as.array.element_count : object ? pattern->as.object.field_count : -1;
    if (count<0 || (count && (tuple ? !pattern->as.array.elements :
        (!pattern->as.object.field_names || !pattern->as.object.patterns))))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"product binding fields are missing");
    for (int i=0;i<count;++i) {
        if (!source_work(ctx,node)) return false;
        if (object) {
            size_t length=0;
            if (!pattern->as.object.field_names[i] ||
                !source_text_length(ctx,node,pattern->as.object.field_names[i],&length) || !length)
                return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"object binding field name is missing");
            for (int j=0;j<i;++j) {
                if (!source_work(ctx,node)) return false;
                if (source_text_same(ctx,node,pattern->as.object.field_names[i],pattern->as.object.field_names[j]))
                    return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"duplicate object binding field");
            }
        }
        const XrDestructurePattern *child=tuple ? pattern->as.array.elements[i] : pattern->as.object.patterns[i];
        if (!source_destructure_collect(ctx,node,child,plan,depth+1)) return false;
    }
    return true;
}
static bool source_destructure_pattern(SourceContext *ctx,AstNode *node,SourceDestructurePlan *plan) {
    const XrDestructurePattern *pattern=node->as.destructure_decl.pattern;
    if (!pattern || (pattern->type!=PATTERN_TUPLE && pattern->type!=PATTERN_OBJECT) ||
        !node->as.destructure_decl.initializer)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"product binding and initializer required");
    return source_destructure_collect(ctx,node,pattern,plan,0);
}
static XrXirSourceRange source_destructure_range(SourceContext *ctx,const XrDestructurePattern *field) {
    return (XrXirSourceRange){ctx->module,field->line,field->column,field->end_line,field->end_column};
}
static bool source_destructure_object_type(SourceContext *ctx,XrXirType type) {
    return xr_xir_type_is_struct(&ctx->types,type) || xr_xir_type_is_class(&ctx->types,type);
}
static bool source_destructure_object_field(SourceContext *ctx,AstNode *node,
    SourceValue receiver,const char *name,const XrDestructurePattern *pattern,SourceValue *value) {
    AstNode selection={0};AstNode *site=node;
    if (pattern->field_span.line>0) {
        selection.type=AST_MEMBER_ACCESS;selection.line=pattern->field_span.line;
        selection.column=pattern->field_span.column;
        selection.end_line=selection.line;selection.end_column=selection.column+(int)source_text_size(ctx,name);
        selection.as.member_access.name=(char *)name;selection.as.member_access.name_span=pattern->field_span;
        site=&selection;
    }
    uint32_t index=0;XrXirType type;
    bool class=xr_xir_type_is_class(&ctx->types,receiver.type);
    return source_struct_field(ctx,site,receiver.type,name,false,&index,&type) &&
        source_recipe_record(ctx,(XrXirInstruction){class ? XR_XIR_CLASS_GET : XR_XIR_STRUCT_GET,type,{receiver.id,0},{0},index,{0}},value);
}
static bool source_destructure_project(SourceContext *ctx,AstNode *node,
    const XrDestructurePattern *pattern,SourceValue initial,SourceDestructurePlan *plan,uint32_t depth) {
    if (!source_work(ctx,node)) return false;
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
    bool tuple=pattern->type==PATTERN_TUPLE;
    uint32_t count=(uint32_t)(tuple ? pattern->as.array.element_count : pattern->as.object.field_count);
    XrXirCallableParameter *fields=NULL;
    if (tuple) {
        if (!count) return initial.type==XR_XIR_UNIT ||
            source_fail(ctx,node,XR_XIR_BAD_TYPE,"empty binding requires Unit");
        const XrXirTypeNode *signature=xr_xir_tuple_signature(&ctx->types,initial.type);
        if (!signature || signature->parameter_count!=count)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Tuple binding requires its exact ordered arity");
        fields=source_alloc(ctx,count,sizeof(*fields));
        if (!fields || !source_copy_bytes(ctx,node,fields,signature->parameters,count*sizeof(*fields))) return false;
    } else if (!source_destructure_object_type(ctx,initial.type))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"object binding requires statically known fields");
    for (uint32_t i=0;i<count;++i) {
        SourceValue value;const XrDestructurePattern *child=tuple ?
            pattern->as.array.elements[i] : pattern->as.object.patterns[i];
        if (tuple) {
            if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_TUPLE_FIELD,fields[i].type,
                {initial.id,0},{0},i,{0}},&value)) return false;
        } else if (!source_destructure_object_field(ctx,node,initial,pattern->as.object.field_names[i],child,&value))
            return false;
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
            if (!source_work(ctx,node)) return false;
            if (source_text_same(ctx,node,prior->name,name))
                return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"duplicate local product binding name");
        }
    }
    SourceValue initial={0};
    if (!expression(ctx,node->as.destructure_decl.initializer,&initial) ||
        !source_destructure_project(ctx,node,node->as.destructure_decl.pattern,initial,&plan,0)) return false;
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
        if (!node->as.destructure_decl.is_const) {
            if (!source_root_join_record(ctx,&field->value)) return false;
            XrXirType cell;
            if (!source_cell_type(ctx,field->type,&cell) ||
                !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_NEW,cell,
                    {field->type==XR_XIR_UNIT ? 0 : field->value.id,0},{0},0,{0}},&field->value)) return false;
        }
        bool unit=field->type==XR_XIR_UNIT && node->as.destructure_decl.is_const;
        *field->symbol=(SourceName){.next=NULL,.name=field->pattern->as.identifier.name,.parameters=NULL,.node=node,.kind=unit ? SOURCE_UNIT_LOCAL : SOURCE_LOCAL,
            .index=unit ? UINT32_MAX : field->value.id,.module=ctx->module,.type=field->type,.mutable=!node->as.destructure_decl.is_const,
            .construction=false,.declaration=0,.inferred=true,.checked_library=false};
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
        if (!source_query_declare(ctx,field->symbol,XR_XIR_SOURCE_BINDING,ctx->bodies[ctx->function].declaration,
            source_destructure_range(ctx,field->pattern))) return false;
        source_query_binding_type(ctx,field->symbol);
        if (!source_facts_assign(ctx,field->symbol,false)) return false;
    }
    return true;
}
