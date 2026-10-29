/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_intern.inc.c - Declaration names and canonical Source types
 *
 * KEY CONCEPT:
 *   Canonical type nodes share the declaration checker's finite ownership ledger.
 */
static SourceName *find_name(SourceContext *ctx, SourceName *names, const char *name) {
    if(names && ctx->items) {
        uint32_t module=0,item=0;
        if(xr_hir_items_binding_module(ctx->items,names,&module)) {
            if(!xr_hir_items_find(ctx->items,module,0,name,&item))return NULL;
            return xr_hir_items_binding(ctx->items,item);
        }
    }
    for (SourceName *p = names; p; p = p->next) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return NULL;
        if (source_text_same(ctx, NULL, p->name, name)) return p;
    }
    return NULL;
}
static SourceName *source_name_bind(SourceContext *ctx,SourceName **head,const char *name,
    AstNode *node,uint32_t item) {
    SourceName *symbol=source_alloc(ctx,1,sizeof(*symbol));
    if(!symbol)return NULL;
    if(item&&!xr_hir_items_bind(ctx->items,item-1,symbol)) {
        source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"declaration was bound twice");return NULL;
    }
    symbol->name=name;symbol->node=node;symbol->hir_item=item;
    symbol->next=*head;*head=symbol;return symbol;
}
static bool source_method_parameter_scope(SourceContext *ctx,AstNode *node,uint32_t *out) {
    uint32_t item=0;
    if(!xr_hir_items_find_scope(ctx->items,ctx->module,node->node_id,&item))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"method parameter scope was not reserved");
    *out=item+1;return true;
}
/* Requirements claim the same declarations without creating body bindings. */
static bool source_parameter_item(SourceContext *ctx,AstNode *node,const XrParamNode *param,
    uint32_t parent,uint32_t *item) {
    XrHirItemSpec spec={param->name,ctx->module,parent,node->node_id,param->line,param->column,XR_HIR_ITEM_PARAMETER};
    XrCompileResourceStatus status=xr_hir_items_claim(ctx->items,&spec,item);
    if(status!=XR_COMPILE_RESOURCE_OK)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"parameter declaration was not reserved");
    return true;
}
static SourceName *source_parameter_name(SourceContext *ctx,AstNode *node,const XrParamNode *param,
    uint32_t parent) {
    uint32_t item=0;
    if(!source_parameter_item(ctx,node,param,parent,&item))return NULL;
    return source_name_bind(ctx,&ctx->locals,param->name,node,item+1);
}
static SourceName *source_member_name(SourceContext *ctx,SourceName *owner,SourceName **head,
    AstNode *node,const char *name) {
    uint32_t item=0;
    XrHirItemSpec spec={name,owner->module,owner->hir_item,node->node_id,node->line,node->column,
        XR_HIR_ITEM_METHOD};
    if(!owner->hir_item||xr_hir_items_claim(ctx->items,&spec,&item)!=XR_COMPILE_RESOURCE_OK) {
        source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"method declaration was not reserved");return NULL;
    }
    return source_name_bind(ctx,head,name,node,item+1);
}
static SourceName *add_name(SourceContext *ctx, SourceName **head, const char *name, AstNode *node) {
    if(name && ctx->interactive && ctx->names && ctx->graph && ctx->graph->entry_index>=0 &&
        head==&ctx->names[ctx->graph->entry_index] && source_text_same(ctx,node,name,"it")) {
        source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"it is reserved for the last successful non-Unit REPL result");return NULL;
    }
    uint32_t module_index=UINT32_MAX;
    for(uint32_t m=0;ctx->names&&m<ctx->module_count;++m)if(head==&ctx->names[m]){module_index=m;break;}
    bool module=module_index!=UINT32_MAX;
    uint32_t item=0;
    if(module && ctx->items && name && *name) {
        XrHirItemSpec spec={name,module_index,0,node?node->node_id:0,
            node?node->line:0,node?node->column:0,XR_HIR_ITEM_DECLARATION};
        XrCompileResourceStatus status=xr_hir_items_claim(ctx->items,&spec,&item);
        if(status!=XR_COMPILE_RESOURCE_OK || xr_hir_items_errors(ctx->items)) {
            source_fail(ctx,node,status==XR_COMPILE_RESOURCE_MEMORY_LIMIT?XR_XIR_MEMORY_LIMIT:
                status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:
                status==XR_COMPILE_RESOURCE_OVERFLOW?XR_XIR_OVERFLOW:XR_XIR_BAD_STRUCTURE,"duplicate or empty declaration name");
            return NULL;
        }
    } else if (!name || !*name || find_name(ctx, *head, name)) {
        source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "duplicate or empty declaration name"); return NULL;
    }
    return source_name_bind(ctx,head,name,node,module&&ctx->items?item+1:0);
}
static bool source_type(SourceContext *ctx, XrTypeRef *ref, XrXirType *type);
static bool source_nominal_type(SourceContext *ctx, const char *name, XrXirType *type);
static SourceName *source_nominal_name(SourceContext *ctx, const char *name);
static bool source_error_type_ref(SourceContext *ctx,XrTypeRef *ref,XrXirType *output);
static bool source_nominal_arguments(SourceContext *ctx, const char *name, XrTypeRef **arguments,
    uint32_t count, XrXirType *type);
static bool source_intern_type_raw(SourceContext *ctx, XrXirTypeNode node, XrXirType *type) {
    uint32_t span = xr_xir_type_span(&ctx->types,
        node.kind == XR_XIR_TYPE_CALLABLE ? node.result : node.element);
    if (node.kind == XR_XIR_TYPE_MAP || node.kind == XR_XIR_TYPE_CALLABLE ||
        node.kind == XR_XIR_TYPE_TASK || node.kind == XR_XIR_TYPE_ITERATOR) {
        uint32_t value_span = xr_xir_type_span(&ctx->types,
            node.kind == XR_XIR_TYPE_CALLABLE ? node.element : node.result);
        if (value_span > span) span = value_span;
    }
    for (uint32_t p = 0; p < node.parameter_count; ++p) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (!node.parameters[p].type && node.kind != XR_XIR_TYPE_TUPLE)
            return source_fail(ctx, NULL, XR_XIR_BAD_TYPE, "callable parameter must be a value type");
        uint32_t component = xr_xir_type_span(&ctx->types, node.parameters[p].type);
        if (component > span) span = component;
    }
    node.parameter_span = span;
    if (node.kind == XR_XIR_TYPE_NOMINAL) {
        for (uint32_t a = 0; a < node.nominal.argument_count; ++a) {
            if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
            uint32_t component = xr_xir_type_span(&ctx->types, node.nominal.arguments[a]);
            if (component > node.parameter_span) node.parameter_span = component;
        }
    }
    for (uint32_t i = 0; i < ctx->types.count; ++i) {
        const XrXirTypeNode *s = &ctx->types.nodes[i];
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (s->kind != node.kind || s->element != node.element || s->parameter_count != node.parameter_count ||
            s->result != node.result || s->flags != node.flags) continue;
        if (node.kind == XR_XIR_TYPE_NOMINAL && (s->nominal.declaration != node.nominal.declaration ||
            s->nominal.argument_count != node.nominal.argument_count)) continue;
        bool same = true;
        for (uint32_t a = 0; a < node.nominal.argument_count; ++a) {
            if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
            if (s->nominal.arguments[a] != node.nominal.arguments[a]) same = false;
        }
        for (uint32_t p = 0; p < node.parameter_count; ++p) {
            if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
            if (s->parameters[p].type != node.parameters[p].type ||
                s->parameters[p].mode != node.parameters[p].mode) same = false;
        }
        if (same) { *type = (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + i); return true; }
    }
    const uint32_t limit = XR_XIR_CONSTRUCTED_TYPE_LIMIT - XR_XIR_CONSTRUCTED_TYPE_BASE;
    if (ctx->types.count == limit)
        return source_fail(ctx, NULL, XR_XIR_BUDGET, "constructed type identity budget exhausted");
    if (ctx->types.count == ctx->type_capacity) {
        uint32_t capacity = ctx->type_capacity ? ctx->type_capacity * 2 : 8;
        if (capacity > limit) capacity = limit;
        XrXirTypeNode *table = source_alloc(ctx, capacity, sizeof(*table));
        if (!table) return false;
        if (!source_copy_bytes(ctx, NULL, table, ctx->types.nodes, ctx->types.count * sizeof(*table))) return false;
        source_retire_array(ctx,ctx->types.nodes);
        ctx->types.nodes = table; ctx->type_capacity = capacity;
    }
    XrXirTypeNode *table = (XrXirTypeNode *) ctx->types.nodes;
    table[ctx->types.count] = node;
    *type = (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + ctx->types.count++); return true;
}
static bool source_intern_type(SourceContext *ctx, XrXirTypeNode node, XrXirType *type) {
    if (node.kind != XR_XIR_TYPE_UNION) return source_intern_type_raw(ctx,node,type);
    XrXirType *members = node.parameter_count ? source_alloc(ctx,node.parameter_count,sizeof(*members)) : NULL;
    if (node.parameter_count && (!members || !node.parameters)) return false;
    for (uint32_t p=0;p<node.parameter_count;++p) {
        if (node.parameters[p].mode) return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"error union member has a passing mode");
        members[p]=node.parameters[p].type;
    }
    XrXirErrorUnion normalized={0};
    XrXirStatus status=xr_xir_compile_error_union(&ctx->compile,&ctx->types,members,node.parameter_count,&normalized);
    source_retire_array(ctx,members);
    if (status!=XR_XIR_OK) return source_fail(ctx,NULL,status,"error union normalization failed");
    if (!normalized.count) { *type=normalized.scalar;return true; }
    XrXirCallableParameter *owned=source_alloc(ctx,normalized.count,sizeof(*owned));
    if (owned) memcpy(owned,normalized.members,(size_t)normalized.count*sizeof(*owned));
    xr_compile_resources_free(normalized.members);
    if (!owned) return false;
    node=(XrXirTypeNode){.kind=XR_XIR_TYPE_UNION,.parameters=owned,.parameter_count=normalized.count};
    return source_intern_type_raw(ctx,node,type);
}
static bool source_signature(SourceContext *ctx, const XrXirCallableParameter *parameters,
    uint32_t count, XrXirType result, XrXirType error_type, XrXirType *type) {
    return source_intern_type(ctx, (XrXirTypeNode) {XR_XIR_TYPE_CALLABLE, error_type,
        parameters, count, result, XR_XIR_CALLABLE_ROOT_UNRESOLVED, 0, {0}}, type);
}
static bool source_reference_promise(SourceContext *ctx, AstNode *node,
    uint32_t function, SourceExpectedType expected, XrXirType *type) {
    const XrXirTypeNode *context = expected.present ? xr_xir_callable_signature(&ctx->types, expected.type) : NULL;
    if (!context || !(context->flags & XR_XIR_CALLABLE_NO_SUSPEND)) return true;
    if (!(ctx->identities[function].promises & XR_XIR_FUNCTION_NO_SUSPEND))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "qualified reference requires an explicit target promise");
    XrXirTypeNode signature = *xr_xir_callable_signature(&ctx->types, *type);
    signature.flags |= XR_XIR_CALLABLE_NO_SUSPEND;
    return source_intern_type(ctx, signature, type);
}
static bool source_cell_type(SourceContext *ctx, XrXirType element, XrXirType *type) {
    return source_intern_type(ctx, (XrXirTypeNode) {XR_XIR_TYPE_CELL, element,
        NULL, 0, XR_XIR_UNIT, 0, 0, {0}}, type);
}
static bool source_nullable_type(SourceContext *ctx, XrXirType element, XrXirType *type) {
    if (xr_xir_type_is_cell(&ctx->types,element))
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"nullable element must be an ordinary value type");
    return source_intern_type(ctx,(XrXirTypeNode){XR_XIR_TYPE_NULLABLE,element,NULL,0,XR_XIR_UNIT,0,0,{0}},type);
}
static XrGenericParam **source_type_parameters(SourceContext *ctx, int *count) {
    if (ctx->type_scope.active) {
        *count = (int)ctx->type_scope.count;
        return ctx->type_scope.parameters;
    }
    SourceFunction *body = &ctx->bodies[ctx->function];
    *count = (int)body->type_parameter_count;
    return body->type_parameters;
}
