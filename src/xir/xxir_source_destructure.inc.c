/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_destructure.inc.c - Prepared owned positional binding groups
 *
 * KEY CONCEPT:
 *   The initializer and every projection precede publication of new names.
 */
static bool source_destructure_pattern(SourceContext *ctx, AstNode *node, uint32_t *count) {
    XrDestructurePattern *pattern = node->as.destructure_decl.pattern;
    if (!pattern || pattern->type != PATTERN_TUPLE || !node->as.destructure_decl.initializer ||
        pattern->as.array.element_count < 0 ||
        (pattern->as.array.element_count && !pattern->as.array.elements))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"flat Tuple binding and initializer required");
    *count = (uint32_t)pattern->as.array.element_count;
    for (uint32_t i = 0; i < *count; ++i) {
        if (!source_work(ctx,node)) return false;
        XrDestructurePattern *field = pattern->as.array.elements[i];
        if (!field || (field->type != PATTERN_IDENTIFIER && field->type != PATTERN_SKIP))
            return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"nested binding requires its complete pattern contract");
        if (field->type == PATTERN_SKIP) continue;
        size_t length = 0;
        if (field->as.identifier.type || !field->as.identifier.name ||
            !source_text_length(ctx,node,field->as.identifier.name,&length) || !length ||
            source_text_same(ctx,node,field->as.identifier.name,"_"))
            return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Tuple binding needs a declared identifier or discard");
        for (uint32_t j = 0; j < i; ++j) {
            if (!source_work(ctx,node)) return false;
            XrDestructurePattern *prior = pattern->as.array.elements[j];
            if (prior->type == PATTERN_IDENTIFIER &&
                source_text_same(ctx,node,field->as.identifier.name,prior->as.identifier.name))
                return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"duplicate Tuple binding name");
        }
    }
    return ctx->diagnostic.status == XR_XIR_OK;
}
static XrXirSourceRange source_destructure_range(SourceContext *ctx,const XrDestructurePattern *field) {
    return (XrXirSourceRange){ctx->module,field->line,field->column,field->end_line,field->end_column};
}
static bool source_destructure_binding(SourceContext *ctx, AstNode *node, bool top) {
    uint32_t count = 0;
    if (!source_destructure_pattern(ctx,node,&count)) return false;
    XrDestructurePattern *pattern = node->as.destructure_decl.pattern;
    for (uint32_t i = 0; i < count; ++i) {
        XrDestructurePattern *field = pattern->as.array.elements[i];
        if (field->type == PATTERN_SKIP) continue;
        for (SourceName *prior = top ? ctx->scope : ctx->locals; prior != ctx->scope; prior = prior->next) {
            if (!source_work(ctx,node)) return false;
            if (source_text_same(ctx,node,prior->name,field->as.identifier.name))
                return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"duplicate local Tuple binding name");
        }
    }
    SourceValue initial = {0};
    if (!expression(ctx,node->as.destructure_decl.initializer,&initial)) return false;
    if (!count)
        return initial.type == XR_XIR_UNIT || source_fail(ctx,node,XR_XIR_BAD_TYPE,"empty binding requires Unit");
    const XrXirTypeNode *tuple = xr_xir_tuple_signature(&ctx->types,initial.type);
    if (!tuple || tuple->parameter_count != count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Tuple binding requires its exact ordered arity");
    XrXirCallableParameter *fields = source_alloc(ctx,count,sizeof(*fields));
    SourceValue *values = source_alloc(ctx,count,sizeof(*values));
    SourceName **symbols = source_alloc(ctx,count,sizeof(*symbols));
    if (!fields || !values || !symbols ||
        !source_copy_bytes(ctx,node,fields,tuple->parameters,count*sizeof(*fields))) return false;
    /* No type-pool pointer survives creation of a mutable binding's Cell type. */
    for (uint32_t i = 0; i < count; ++i)
        if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_TUPLE_FIELD,fields[i].type,
            {initial.id,0},{0},i,{0}},&values[i])) return false;
    uint32_t bindings=0,first_slot=0,payloads=0;
    SourceValue *compact=top ? source_alloc(ctx,count,sizeof(*compact)) : NULL;
    if(top && !compact)return false;
    for (uint32_t i = 0; i < count; ++i) {
        XrDestructurePattern *field = pattern->as.array.elements[i];
        if (field->type == PATTERN_SKIP) continue;
        if(top) {
            symbols[i]=find_name(ctx,ctx->names[ctx->module],field->as.identifier.name);
            if(!symbols[i] || symbols[i]->kind!=SOURCE_SLOT || symbols[i]->node!=node ||
                (bindings && symbols[i]->index!=first_slot+bindings))
                return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"module Tuple binding slot inventory mismatch");
            if(!bindings)first_slot=symbols[i]->index;
            ++bindings;
            symbols[i]->type=fields[i].type;ctx->slots[symbols[i]->index].type=fields[i].type;
            if(fields[i].type==XR_XIR_UNIT)symbols[i]->kind=SOURCE_UNIT_SLOT;
            else compact[payloads++]=values[i];
            source_query_binding_type(ctx,symbols[i]);
            continue;
        }
        symbols[i] = source_alloc(ctx,1,sizeof(*symbols[i]));
        if (!symbols[i]) return false;
        if (!node->as.destructure_decl.is_const && fields[i].type != XR_XIR_UNIT) {
            XrXirType cell;
            if (!source_cell_type(ctx,fields[i].type,&cell) ||
                !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_NEW,cell,
                    {values[i].id,0},{0},0,{0}},&values[i])) return false;
        }
        *symbols[i] = (SourceName){NULL,field->as.identifier.name,NULL,node,
            fields[i].type == XR_XIR_UNIT ? SOURCE_UNIT_LOCAL : SOURCE_LOCAL,
            fields[i].type == XR_XIR_UNIT ? UINT32_MAX : values[i].id,
            ctx->module,fields[i].type,!node->as.destructure_decl.is_const,false,0};
    }
    if(top) {
        if(!bindings)return true;
        uint64_t packed=((uint64_t)first_slot<<32)|bindings;
        return source_recipe_group(ctx,(XrXirInstruction){XR_XIR_SLOT_GROUP_INIT,XR_XIR_UNIT,{0},{0},(int64_t)packed,{0}},
            compact,payloads,NULL);
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (!symbols[i]) continue;
        symbols[i]->next = ctx->locals; ctx->locals = symbols[i];
        if (!source_query_declare(ctx,symbols[i],XR_XIR_SOURCE_BINDING,ctx->bodies[ctx->function].declaration,
                source_destructure_range(ctx,pattern->as.array.elements[i]))) return false;
        source_query_binding_type(ctx,symbols[i]);
        if (!source_facts_assign(ctx,symbols[i],false)) return false;
    }
    return true;
}
