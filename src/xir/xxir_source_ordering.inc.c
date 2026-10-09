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
#include "xxir_prelude_source.h"
#include "xxir_library_prelude.inc.c"
static const XrXirModule *source_library_module(SourceContext *ctx,uint32_t module);
typedef struct SourcePreludeScan { SourceContext *ctx; uint32_t depth; bool needed; } SourcePreludeScan;
static bool source_prelude_word(SourcePreludeScan *scan,const char *name) {
    if (name && (source_text_same(scan->ctx,NULL,name,"Atomic") ||
        source_text_same(scan->ctx,NULL,name,"Ordering") ||
        source_text_same(scan->ctx,NULL,name,"CryptoError"))) scan->needed=true;
    return scan->ctx->diagnostic.status==XR_XIR_OK;
}
static bool source_prelude_ref(SourcePreludeScan *scan,const XrTypeRef *ref) {
    if (!ref || scan->needed) return true;
    if (!source_work(scan->ctx,NULL)) return false;
    if (scan->depth==128) return source_fail(scan->ctx,NULL,XR_XIR_BUDGET,"native source discovery depth exhausted");
    if (!source_prelude_word(scan,ref->name)) return false;
    ++scan->depth;
    bool ok=true;
    for (uint32_t i=0;i<ref->nchildren && ok && !scan->needed;++i) {
        if (!ref->children) ok=source_fail(scan->ctx,NULL,XR_XIR_BAD_TYPE,"source type children are missing");
        else ok=source_prelude_ref(scan,ref->children[i]);
    }
    --scan->depth;return ok;
}
static bool source_prelude_refs(SourcePreludeScan *scan,XrTypeRef **refs,int count) {
    if (count<0 || (count && !refs)) return source_fail(scan->ctx,NULL,XR_XIR_BAD_TYPE,"source type list is malformed");
    for (int i=0;i<count && !scan->needed;++i)
        if (!source_prelude_ref(scan,refs[i])) return false;
    return true;
}
static bool source_prelude_params(SourcePreludeScan *scan,XrParamNode **params,int count) {
    if (count<0 || (count && !params)) return source_fail(scan->ctx,NULL,XR_XIR_BAD_TYPE,"source parameter list is malformed");
    for (int i=0;i<count && !scan->needed;++i)
        if (!source_work(scan->ctx,NULL) || !params[i] || !source_prelude_ref(scan,params[i]->type)) return false;
    return true;
}
static bool source_prelude_generics(SourcePreludeScan *scan,XrGenericParam **params,int count) {
    if (count<0 || (count && !params)) return source_fail(scan->ctx,NULL,XR_XIR_BAD_TYPE,"source generic list is malformed");
    for (int i=0;i<count && !scan->needed;++i) {
        if (!source_work(scan->ctx,NULL)) return false;
        if (!params[i]) return source_fail(scan->ctx,NULL,XR_XIR_BAD_TYPE,"source generic parameter is missing");
        if (!source_prelude_refs(scan,params[i]->constraints,params[i]->constraint_count)) return false;
    }
    return true;
}
static bool source_prelude_signature(SourcePreludeScan *scan,AstNode *node) {
    switch (node->type) {
    case AST_VAR_DECL:case AST_CONST_DECL:return source_prelude_ref(scan,node->as.var_decl.type_annotation);
    case AST_FIELD_DECL:return source_prelude_ref(scan,node->as.field_decl.field_type);
    case AST_FUNCTION_DECL:case AST_FUNCTION_EXPR:
        return source_prelude_generics(scan,node->as.function_decl.type_params,node->as.function_decl.type_param_count) &&
            source_prelude_ref(scan,node->as.function_decl.return_type) &&
            source_prelude_params(scan,node->as.function_decl.params,node->as.function_decl.param_count) &&
            source_prelude_refs(scan,node->as.function_decl.throws_types,node->as.function_decl.throws_count);
    case AST_METHOD_DECL:
        return source_prelude_generics(scan,node->as.method_decl.type_params,node->as.method_decl.type_param_count) &&
            source_prelude_generics(scan,node->as.method_decl.conditions,node->as.method_decl.condition_count) &&
            source_prelude_ref(scan,node->as.method_decl.return_type) &&
            source_prelude_params(scan,node->as.method_decl.params,node->as.method_decl.param_count);
    case AST_INTERFACE_METHOD:
        return source_prelude_generics(scan,node->as.interface_method.type_params,node->as.interface_method.type_param_count) &&
            source_prelude_ref(scan,node->as.interface_method.return_type) &&
            source_prelude_params(scan,node->as.interface_method.params,node->as.interface_method.param_count);
    case AST_CLASS_DECL:case AST_STRUCT_DECL:case AST_UNION_DECL:
        return source_prelude_generics(scan,node->as.class_decl.type_params,node->as.class_decl.type_param_count) &&
            source_prelude_refs(scan,node->as.class_decl.interfaces,node->as.class_decl.interface_count);
    case AST_INTERFACE_DECL:
        return source_prelude_generics(scan,node->as.interface_decl.type_params,node->as.interface_decl.type_param_count) &&
            source_prelude_refs(scan,node->as.interface_decl.extends,node->as.interface_decl.extends_count);
    case AST_ENUM_DECL:
        return source_prelude_generics(scan,node->as.enum_decl.type_params,node->as.enum_decl.type_param_count) &&
            source_prelude_refs(scan,node->as.enum_decl.interfaces,node->as.enum_decl.interface_count);
    case AST_STRUCT_LITERAL:return source_prelude_refs(scan,node->as.struct_literal.type_args,node->as.struct_literal.type_arg_count);
    case AST_INTERFACE_PROPERTY:return source_prelude_ref(scan,node->as.interface_property.prop_type);
    case AST_CALL_EXPR:return source_prelude_refs(scan,node->as.call_expr.type_args,node->as.call_expr.type_arg_count);
    case AST_FUNCTION_REF:return source_prelude_refs(scan,node->as.function_ref.type_args,node->as.function_ref.type_arg_count);
    case AST_NEW_EXPR:
        return source_prelude_word(scan,node->as.new_expr.class_name) &&
            source_prelude_refs(scan,node->as.new_expr.type_args,node->as.new_expr.type_arg_count);
    case AST_IS_EXPR:return source_prelude_ref(scan,node->as.is_expr.type);
    case AST_AS_EXPR:return source_prelude_ref(scan,node->as.as_expr.type);
    case AST_ENUM_MEMBER:return source_prelude_refs(scan,node->as.enum_member.payload_types,node->as.enum_member.payload_count);
    case AST_FOR_IN_STMT:return source_prelude_ref(scan,node->as.for_in_stmt.item_type);
    case AST_TYPE_ALIAS:return source_prelude_generics(scan,node->as.type_alias.type_params,node->as.type_alias.type_param_count) &&
        source_prelude_ref(scan,node->as.type_alias.resolved_type) &&
        source_prelude_refs(scan,node->as.type_alias.field_types,node->as.type_alias.field_count);
    case AST_TRY_CATCH:
        if (node->as.try_catch.catch_count < 0 ||
            (node->as.try_catch.catch_count && !node->as.try_catch.catch_clauses))
            return source_fail(scan->ctx,node,XR_XIR_BAD_STRUCTURE,"catch discovery inventory is malformed");
        for (int i=0;i<node->as.try_catch.catch_count && !scan->needed;++i) {
            XrCatchClause *clause=node->as.try_catch.catch_clauses[i];
            if (!source_work(scan->ctx,node)) return false;
            if (!clause) return source_fail(scan->ctx,node,XR_XIR_BAD_STRUCTURE,"catch discovery clause is missing");
            if (!source_prelude_ref(scan,clause->type)) return false;
        }
        return true;
    default:return true;
    }
}
static bool source_prelude_scan(AstNode *node,void *pointer) {
    SourcePreludeScan *scan=pointer;
    if (!node || scan->needed) return true;
    if (!source_work(scan->ctx,node)) return false;
    if (scan->depth==128) return source_fail(scan->ctx,node,XR_XIR_BUDGET,"native source discovery depth exhausted");
    if (node->type==AST_VARIABLE && !source_prelude_word(scan,node->as.variable.name)) return false;
    if (!source_prelude_signature(scan,node)) return false;
    ++scan->depth;
    bool ok=xr_ast_for_each_child(node,source_prelude_scan,scan);
    --scan->depth;
    return ok || source_fail(scan->ctx,node,XR_XIR_BAD_STRUCTURE,"unknown source discovery syntax");
}
static bool source_prelude_discover(SourceContext *ctx) {
    uint32_t original=(uint32_t)ctx->graph->spec_count;
    bool *needed=source_alloc(ctx,original,sizeof(*needed));
    if (original && !needed) return false;
    bool any=false;
    for (uint32_t m=0;m<original;++m) {
        ctx->module=m; SourcePreludeScan scan={ctx,0,false};
        if (!source_prelude_scan(ctx->graph->specs[m].ast,&scan)) return false;
        needed[m]=scan.needed; any|=scan.needed;
    }
    if (!any) return true;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_STDLIB,"prelude",NULL};
    int index=-1;
    XrModuleStatus status=xr_compile_module_graph_include_catalog_logical(ctx->graph,&authority,XIR_PRELUDE_LOGICAL,&index);
    if (status!=XR_MODULE_OK) return source_module_fail(ctx,NULL,status,"canonical prelude Catalog selection failed");
    if (index<0) {
        char *error=NULL;
        status=xr_compile_module_graph_include_logical_source(ctx->graph,&authority,XIR_PRELUDE_LOGICAL,
            NULL,xir_prelude_source.text,&error);
        if (status!=XR_MODULE_OK) source_module_fail(ctx,NULL,status,error ? error : "canonical prelude Source discovery failed");
        xr_compile_resources_free(error);
        if (status!=XR_MODULE_OK) return false;
        status=xr_compile_module_graph_find(ctx->graph,XIR_PRELUDE_CANONICAL,&index);
    }
    if (status!=XR_MODULE_OK || index<0)
        return source_module_fail(ctx,NULL,status==XR_MODULE_OK ? XR_MODULE_INVALID : status,"canonical prelude identity is missing");
    if (ctx->graph->specs[index].representation==XR_MODULE_CHECKED_LIBRARY) {
        const XrXirModule *library=source_library_module(ctx,(uint32_t)index); bool admitted=false;
        if (!library) return false;
        XrXirStatus checked=library_prelude_module(&ctx->compile,library,ctx->graph->specs[index].resource->checked_module,&admitted);
        if (checked!=XR_XIR_OK || !admitted)
            return source_fail(ctx,NULL,checked==XR_XIR_OK ? XR_XIR_BAD_STRUCTURE : checked,"canonical prelude Catalog inventory is invalid");
    }
    ctx->prelude_module=(uint32_t)index+1;
    for (uint32_t m=0;m<original;++m) {
        if (!source_work(ctx,NULL)) return false;
        if (needed[m] && m!=(uint32_t)index) {
            status=xr_compile_module_graph_add_dependency(ctx->graph,(int)m,index);
            if (status!=XR_MODULE_OK) return source_module_fail(ctx,NULL,status,"canonical prelude dependency failed");
        }
    }
    return true;
}

static SourceName *source_prelude_name(SourceContext *ctx,const char *name) {
    if (!ctx->prelude_module || !ctx->names || !name || (!source_text_same(ctx,NULL,name,"Ordering") &&
        !source_text_same(ctx,NULL,name,"CryptoError"))) return NULL;
    uint32_t module=ctx->prelude_module-1;
    if (module>=ctx->module_count) return NULL;
    bool dependency=ctx->module==module;
    const XrModuleSpec *consumer=&ctx->graph->specs[ctx->module];
    for (int i=0;i<consumer->dep_count && !dependency;++i) {
        if (!source_work(ctx,NULL)) return NULL;
        dependency=consumer->dep_indices[i]==(int)module;
    }
    return dependency ? find_name(ctx,ctx->names[module],name) : NULL;
}
static SourceName *source_prelude_lookup(SourceContext *ctx,const char *name) {
    if (source_native_type_shadowed(ctx,name)) return NULL;
    return source_prelude_name(ctx,name);
}
static bool source_prelude_bind(SourceContext *ctx,SourceName *symbol,XrXirNominalDeclaration *record) {
    if (!ctx->prelude_module || ctx->module!=ctx->prelude_module-1) return true;
    bool owned=false;
    XrModuleStatus status=xr_compile_module_graph_owns_top_level_decl(ctx->graph,&ctx->graph->specs[ctx->module],symbol->node,&owned);
    if (status!=XR_MODULE_OK) return source_module_fail(ctx,symbol->node,status,"canonical prelude declaration lookup failed");
    const XirPreludeEnum *expected=NULL;
    for (uint32_t e=0;e<xir_prelude_source.enum_count;++e) {
        if (!source_work(ctx,symbol->node)) return false;
        if (source_text_same(ctx,symbol->node,symbol->name,xir_prelude_source.enums[e].name)) expected=&xir_prelude_source.enums[e];
    }
    if (!owned || !expected || symbol->node->type!=AST_ENUM_DECL ||
        symbol->node->as.enum_decl.type_param_count || record->kind!=XR_XIR_NOMINAL_ENUM || record->exported!=1)
        return source_fail(ctx,symbol->node,XR_XIR_BAD_STRUCTURE,"canonical prelude enum owner is invalid");
    /* CryptoError remains an ordinary owned enum with no native privileges. */
    if (!expected->native_id) return true;
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_ORDERING);
    if (!source_native_admit(ctx,native) || !native || native->id!=expected->native_id ||
        !source_text_same(ctx,symbol->node,symbol->name,native->name) ||
        !source_span_same(ctx,symbol->node,(const char *)native->source_fingerprint.bytes,
            (const char *)xir_prelude_source.input_sha256,sizeof(xir_prelude_source.input_sha256)))
        return source_fail(ctx,symbol->node,XR_XIR_BAD_STRUCTURE,"canonical native prelude input differs");
    record->native.native_id=native->id;
    return source_copy_bytes(ctx,symbol->node,record->native.source_fingerprint,native->source_fingerprint.bytes,
        sizeof(record->native.source_fingerprint));
}

static bool source_ordering_type(SourceContext *ctx,XrXirType *type) {
    SourceName *name=source_prelude_name(ctx,"Ordering");
    if (!name || name->kind!=SOURCE_NOMINAL)
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"governed Ordering type is outside this module's authority");
    *type=name->type;return true;
}
