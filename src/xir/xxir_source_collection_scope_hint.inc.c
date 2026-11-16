/* Copyright (c) 2026 Xinglei Xu. MIT License. */
/* Parser type aliases expand before Source type resolution. Ordinary blocks
 * and non-generic declarations share their parser's current alias scope;
 * only a declaration/expression with generic binders opens a temporary scope. */
typedef struct SourceCollectionAliasLookup {
    SourceContext *ctx;
    AstNode *site;
    const char *name;
    uint32_t depth;
    bool active_shadowed,found,shadowed;
} SourceCollectionAliasLookup;
static bool source_collection_alias_at(AstNode *node,void *opaque) {
    SourceCollectionAliasLookup *lookup=opaque;SourceContext *ctx=lookup->ctx;
    if(lookup->found || !node)return true;
    if(!source_request_poll(ctx,node))return false;
    if(node==lookup->site){lookup->found=true;lookup->shadowed=lookup->active_shadowed;return true;}
    if(lookup->depth==128)return source_fail(ctx,node,XR_XIR_BUDGET,"collection constructor scope depth exhausted");
    XrGenericParam **parameters=NULL;int count=0;
    switch(node->type) {
    case AST_FUNCTION_DECL:case AST_FUNCTION_EXPR:
        parameters=node->as.function_decl.type_params;count=node->as.function_decl.type_param_count;break;
    case AST_CLASS_DECL:case AST_STRUCT_DECL:
        parameters=node->as.class_decl.type_params;count=node->as.class_decl.type_param_count;break;
    case AST_ENUM_DECL:
        parameters=node->as.enum_decl.type_params;count=node->as.enum_decl.type_param_count;break;
    case AST_METHOD_DECL:
        parameters=node->as.method_decl.type_params;count=node->as.method_decl.type_param_count;break;
    case AST_INTERFACE_DECL:
        parameters=node->as.interface_decl.type_params;count=node->as.interface_decl.type_param_count;break;
    case AST_INTERFACE_METHOD:
        parameters=node->as.interface_method.type_params;count=node->as.interface_method.type_param_count;break;
    default:break;
    }
    if(count<0 || (count && !parameters))return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"collection constructor generic scope is malformed");
    bool saved=lookup->active_shadowed;
    for(int n=0;n<count;++n) {
        if(!parameters[n])return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"collection constructor generic binder is missing");
        if(source_text_same(ctx,node,parameters[n]->name,lookup->name))lookup->active_shadowed=true;
        if(ctx->diagnostic.status!=XR_XIR_OK)return false;
    }
    /* The parser registers an alias in its enclosing scope before its RHS.
     * Type-alias RHS type references are already expanded parser-owned data. */
    if(node->type==AST_TYPE_ALIAS && source_text_same(ctx,node,node->as.type_alias.name,lookup->name))
        lookup->active_shadowed=true;
    if(ctx->diagnostic.status!=XR_XIR_OK)return false;
    ++lookup->depth;
    bool valid=xr_ast_for_each_child(node,source_collection_alias_at,lookup);
    --lookup->depth;
    if(count)lookup->active_shadowed=saved;
    return valid || source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"collection constructor scope syntax is unknown");
}
static bool source_collection_union_constructor_visible(SourceContext *ctx,AstNode *node,bool map,bool *out) {
    *out=false;
    const char *name=map?"Map":"Array";
    bool shadowed=source_native_type_shadowed(ctx,name);
    if(ctx->diagnostic.status!=XR_XIR_OK)return false;
    if(shadowed)return true;
    SourceCollectionAliasLookup lookup={.ctx=ctx,.site=node,.name=name};
    if(!source_collection_alias_at(ctx->graph->specs[ctx->module].ast,&lookup))return false;
    if(!lookup.found)return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"collection repair lost its lexical site");
    *out=!lookup.shadowed;return true;
}
