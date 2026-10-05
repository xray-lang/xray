/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_ordering.inc.c - Governed enum source in the ordinary checking graph
 *
 * KEY CONCEPT:
 *   Discovery only adds source; ordinary lookup and declaration proof grant authority.
 */
typedef struct SourceOrderingScan { SourceContext *ctx; uint32_t depth; bool needed; } SourceOrderingScan;
static bool source_ordering_word(SourceOrderingScan *scan,const char *name) {
    if (name && (source_text_same(scan->ctx,NULL,name,"Atomic") ||
        source_text_same(scan->ctx,NULL,name,"Ordering"))) scan->needed=true;
    return scan->ctx->diagnostic.status==XR_XIR_OK;
}
static bool source_ordering_ref(SourceOrderingScan *scan,const XrTypeRef *ref) {
    if (!ref || scan->needed) return true;
    if (!source_work(scan->ctx,NULL)) return false;
    if (scan->depth==128) return source_fail(scan->ctx,NULL,XR_XIR_BUDGET,"native source discovery depth exhausted");
    if (!source_ordering_word(scan,ref->name)) return false;
    ++scan->depth;
    bool ok=true;
    for (uint32_t i=0;i<ref->nchildren && ok && !scan->needed;++i) {
        if (!ref->children) ok=source_fail(scan->ctx,NULL,XR_XIR_BAD_TYPE,"source type children are missing");
        else ok=source_ordering_ref(scan,ref->children[i]);
    }
    --scan->depth;return ok;
}
static bool source_ordering_refs(SourceOrderingScan *scan,XrTypeRef **refs,int count) {
    if (count<0 || (count && !refs)) return source_fail(scan->ctx,NULL,XR_XIR_BAD_TYPE,"source type list is malformed");
    for (int i=0;i<count && !scan->needed;++i)
        if (!source_ordering_ref(scan,refs[i])) return false;
    return true;
}
static bool source_ordering_params(SourceOrderingScan *scan,XrParamNode **params,int count) {
    if (count<0 || (count && !params)) return source_fail(scan->ctx,NULL,XR_XIR_BAD_TYPE,"source parameter list is malformed");
    for (int i=0;i<count && !scan->needed;++i)
        if (!source_work(scan->ctx,NULL) || !params[i] || !source_ordering_ref(scan,params[i]->type)) return false;
    return true;
}
static bool source_ordering_generics(SourceOrderingScan *scan,XrGenericParam **params,int count) {
    if (count<0 || (count && !params)) return source_fail(scan->ctx,NULL,XR_XIR_BAD_TYPE,"source generic list is malformed");
    for (int i=0;i<count && !scan->needed;++i) {
        if (!source_work(scan->ctx,NULL)) return false;
        if (!params[i]) return source_fail(scan->ctx,NULL,XR_XIR_BAD_TYPE,"source generic parameter is missing");
        if (!source_ordering_refs(scan,params[i]->constraints,params[i]->constraint_count)) return false;
    }
    return true;
}
static bool source_ordering_signature(SourceOrderingScan *scan,AstNode *node) {
    switch (node->type) {
    case AST_VAR_DECL:case AST_CONST_DECL:return source_ordering_ref(scan,node->as.var_decl.type_annotation);
    case AST_FIELD_DECL:return source_ordering_ref(scan,node->as.field_decl.field_type);
    case AST_FUNCTION_DECL:case AST_FUNCTION_EXPR:
        return source_ordering_generics(scan,node->as.function_decl.type_params,node->as.function_decl.type_param_count) &&
            source_ordering_ref(scan,node->as.function_decl.return_type) &&
            source_ordering_params(scan,node->as.function_decl.params,node->as.function_decl.param_count) &&
            source_ordering_refs(scan,node->as.function_decl.throws_types,node->as.function_decl.throws_count);
    case AST_METHOD_DECL:
        return source_ordering_generics(scan,node->as.method_decl.type_params,node->as.method_decl.type_param_count) &&
            source_ordering_generics(scan,node->as.method_decl.conditions,node->as.method_decl.condition_count) &&
            source_ordering_ref(scan,node->as.method_decl.return_type) &&
            source_ordering_params(scan,node->as.method_decl.params,node->as.method_decl.param_count);
    case AST_INTERFACE_METHOD:
        return source_ordering_generics(scan,node->as.interface_method.type_params,node->as.interface_method.type_param_count) &&
            source_ordering_ref(scan,node->as.interface_method.return_type) &&
            source_ordering_params(scan,node->as.interface_method.params,node->as.interface_method.param_count);
    case AST_CLASS_DECL:case AST_STRUCT_DECL:case AST_UNION_DECL:
        return source_ordering_generics(scan,node->as.class_decl.type_params,node->as.class_decl.type_param_count) &&
            source_ordering_refs(scan,node->as.class_decl.interfaces,node->as.class_decl.interface_count);
    case AST_INTERFACE_DECL:
        return source_ordering_generics(scan,node->as.interface_decl.type_params,node->as.interface_decl.type_param_count) &&
            source_ordering_refs(scan,node->as.interface_decl.extends,node->as.interface_decl.extends_count);
    case AST_ENUM_DECL:
        return source_ordering_generics(scan,node->as.enum_decl.type_params,node->as.enum_decl.type_param_count) &&
            source_ordering_refs(scan,node->as.enum_decl.interfaces,node->as.enum_decl.interface_count);
    case AST_STRUCT_LITERAL:return source_ordering_refs(scan,node->as.struct_literal.type_args,node->as.struct_literal.type_arg_count);
    case AST_INTERFACE_PROPERTY:return source_ordering_ref(scan,node->as.interface_property.prop_type);
    case AST_CALL_EXPR:return source_ordering_refs(scan,node->as.call_expr.type_args,node->as.call_expr.type_arg_count);
    case AST_FUNCTION_REF:return source_ordering_refs(scan,node->as.function_ref.type_args,node->as.function_ref.type_arg_count);
    case AST_NEW_EXPR:
        return source_ordering_word(scan,node->as.new_expr.class_name) &&
            source_ordering_refs(scan,node->as.new_expr.type_args,node->as.new_expr.type_arg_count);
    case AST_IS_EXPR:return source_ordering_ref(scan,node->as.is_expr.type);
    case AST_AS_EXPR:return source_ordering_ref(scan,node->as.as_expr.type);
    case AST_ENUM_MEMBER:return source_ordering_refs(scan,node->as.enum_member.payload_types,node->as.enum_member.payload_count);
    case AST_FOR_IN_STMT:return source_ordering_ref(scan,node->as.for_in_stmt.item_type);
    case AST_TYPE_ALIAS:return source_ordering_generics(scan,node->as.type_alias.type_params,node->as.type_alias.type_param_count) &&
        source_ordering_ref(scan,node->as.type_alias.resolved_type) &&
        source_ordering_refs(scan,node->as.type_alias.field_types,node->as.type_alias.field_count);
    default:return true;
    }
}
static bool source_ordering_scan(AstNode *node,void *pointer) {
    SourceOrderingScan *scan=pointer;
    if (!node || scan->needed) return true;
    if (!source_work(scan->ctx,node)) return false;
    if (scan->depth==128) return source_fail(scan->ctx,node,XR_XIR_BUDGET,"native source discovery depth exhausted");
    if (node->type==AST_VARIABLE && !source_ordering_word(scan,node->as.variable.name)) return false;
    if (!source_ordering_signature(scan,node)) return false;
    ++scan->depth;
    bool ok=xr_ast_for_each_child(node,source_ordering_scan,scan);
    --scan->depth;
    return ok || source_fail(scan->ctx,node,XR_XIR_BAD_STRUCTURE,"unknown source discovery syntax");
}
static bool source_ordering_discover(SourceContext *ctx) {
    uint32_t original=(uint32_t)ctx->graph->spec_count;
    bool *needed=source_alloc(ctx,original,sizeof(*needed));
    if (original && !needed) return false;
    bool any=false;
    for (uint32_t m=0;m<original;++m) {
        ctx->module=m;SourceOrderingScan scan={ctx,0,false};
        if (!source_ordering_scan(ctx->graph->specs[m].ast,&scan)) return false;
        needed[m]=scan.needed;any|=scan.needed;
    }
    if (!any) return true;
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_ORDERING);
    if (!source_native_admit(ctx,native)) return false;
    if (!native || native->member_count!=5 || native->parameter_count ||
        native->kind!=XR_NATIVE_DECLARATION_VALUE)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"governed Ordering shape is invalid");
    char text[256];
    if (source_format(ctx,text,sizeof(text),"export enum %s { %s, %s, %s, %s, %s }\n",native->name,
        native->members[0].name,native->members[1].name,native->members[2].name,
        native->members[3].name,native->members[4].name)!=XR_DIAG_OK) return false;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_STDLIB,"prelude",NULL};
    char *error=NULL;
    XrModuleStatus status=xr_compile_module_graph_include_logical_source(ctx->graph,&authority,
        "prelude/builtin_symbols.def",native->source_path,text,&error);
    if (status!=XR_MODULE_OK) source_module_fail(ctx,NULL,status,error ? error : "governed source discovery failed");
    xr_compile_resources_free(error);
    if (status!=XR_MODULE_OK) return false;
    char *canonical=NULL;int index=-1;
    status=xr_compile_module_identity_from_logical(ctx->compile.resources,&authority,"prelude/builtin_symbols.def",&canonical);
    if (status==XR_MODULE_OK) status=xr_compile_module_graph_find(ctx->graph,canonical,&index);
    xr_compile_resources_free(canonical);
    if (status!=XR_MODULE_OK || index<0)
        return source_module_fail(ctx,NULL,status==XR_MODULE_OK ? XR_MODULE_INVALID : status,"governed source identity is missing");
    ctx->ordering_module=(uint32_t)index+1;
    for (uint32_t m=0;m<original;++m) {
        if (!source_work(ctx,NULL)) return false;
        if (needed[m] && m!=(uint32_t)index) {
            status=xr_compile_module_graph_add_dependency(ctx->graph,(int)m,index);
            if (status!=XR_MODULE_OK) return source_module_fail(ctx,NULL,status,"governed source dependency failed");
        }
    }
    return true;
}
static SourceName *source_ordering_name(SourceContext *ctx,const char *name) {
    if (!ctx->ordering_module || !ctx->names || !name || !source_text_same(ctx,NULL,name,"Ordering")) return NULL;
    uint32_t module=ctx->ordering_module-1;
    if (module>=ctx->module_count) return NULL;
    bool dependency=ctx->module==module;
    const XrModuleSpec *consumer=&ctx->graph->specs[ctx->module];
    for (int i=0;i<consumer->dep_count && !dependency;++i) {
        if (!source_work(ctx,NULL)) return NULL;
        dependency=consumer->dep_indices[i]==(int)module;
    }
    return dependency ? find_name(ctx,ctx->names[module],name) : NULL;
}
static SourceName *source_ordering_lookup(SourceContext *ctx,const char *name) {
    if (source_native_type_shadowed(ctx,name)) return NULL;
    return source_ordering_name(ctx,name);
}
static bool source_ordering_bind(SourceContext *ctx,SourceName *symbol,XrXirNominalDeclaration *record) {
    if (!ctx->ordering_module || ctx->module!=ctx->ordering_module-1) return true;
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_ORDERING);
    bool owned=false;
    XrModuleStatus status=xr_compile_module_graph_owns_top_level_decl(ctx->graph,&ctx->graph->specs[ctx->module],symbol->node,&owned);
    if (status!=XR_MODULE_OK) return source_module_fail(ctx,symbol->node,status,"governed source declaration lookup failed");
    if (!owned || !source_native_admit(ctx,native) || !source_text_same(ctx,symbol->node,symbol->name,native->name) ||
        symbol->node->type!=AST_ENUM_DECL || record->parameter_count || record->kind!=XR_XIR_NOMINAL_ENUM)
        return source_fail(ctx,symbol->node,XR_XIR_BAD_STRUCTURE,"governed enum declaration owner is invalid");
    record->native.native_id=native->id;
    return source_copy_bytes(ctx,symbol->node,record->native.source_fingerprint,native->source_fingerprint.bytes,
        sizeof(record->native.source_fingerprint));
}
static bool source_ordering_type(SourceContext *ctx,XrXirType *type) {
    SourceName *name=source_ordering_name(ctx,"Ordering");
    if (!name || name->kind!=SOURCE_NOMINAL)
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"governed Ordering type is outside this module's authority");
    *type=name->type;return true;
}
