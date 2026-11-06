/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_path_prelude.inc.c - Discover the real Path owner without importing values
 *
 * KEY CONCEPT:
 *   Type spelling observes one ordinary declaration in the complete checking graph.
 */
#include "xxir_path_prelude.h"
static bool source_path_named(SourceContext *ctx,const char *name) {
    return name&&source_text_same(ctx,NULL,name,"Path");
}
static bool source_path_shadow(SourceContext *ctx,AstNode *node) {
    const char *name=node->type==AST_CLASS_DECL?node->as.class_decl.name:
        node->type==AST_STRUCT_DECL?node->as.struct_decl.name:
        node->type==AST_ENUM_DECL?node->as.enum_decl.name:
        node->type==AST_FUNCTION_DECL?node->as.function_decl.name:
        node->type==AST_TYPE_ALIAS?node->as.type_alias.name:
        node->type==AST_VAR_DECL||node->type==AST_CONST_DECL?node->as.var_decl.name:NULL;
    if(source_path_named(ctx,name))return true;
    if(node->type==AST_IMPORT_STMT) {
        ImportStmtNode *import=&node->as.import_stmt;
        if(!import->member_count)return source_path_named(ctx,import->alias);
        for(int i=0;i<import->member_count;++i) {
            if(!source_request_poll(ctx,node))return true;
            if(source_path_named(ctx,import->members[i].alias?import->members[i].alias:import->members[i].name))return true;
        }
    }
    return false;
}
static bool source_path_scan(AstNode *node,void *pointer) {
    SourcePreludeScan *scan=pointer;if(!node||scan->needed)return true;
    if(!source_request_poll(scan->ctx,node))return false;
    if(scan->depth==128)return source_fail(scan->ctx,node,XR_XIR_BUDGET,"Path type discovery depth exhausted");
    XrGenericParam **parameters=NULL;int count=0;
    if(node->type==AST_FUNCTION_DECL||node->type==AST_FUNCTION_EXPR) {
        parameters=node->as.function_decl.type_params;count=node->as.function_decl.type_param_count;
    } else if(node->type==AST_CLASS_DECL||node->type==AST_STRUCT_DECL) {
        parameters=node->as.class_decl.type_params;count=node->as.class_decl.type_param_count;
    } else if(node->type==AST_METHOD_DECL) {
        parameters=node->as.method_decl.type_params;count=node->as.method_decl.type_param_count;
    }
    if(count<0||(count&&!parameters))return source_fail(scan->ctx,node,XR_XIR_BAD_TYPE,"Path type scope is malformed");
    for(int i=0;i<count;++i)
        if(parameters[i]&&source_path_named(scan->ctx,parameters[i]->name))return true;
    if(node->type==AST_PROGRAM) {
        for(int i=0;i<node->as.program.count;++i)
            if(source_path_shadow(scan->ctx,node->as.program.statements[i]))return scan->ctx->diagnostic.status==XR_XIR_OK;
    }
    if(node->type==AST_BLOCK) {
        ++scan->depth;bool valid=true;
        for(int i=0;i<node->as.block.count&&!scan->needed;++i) {
            AstNode *item=node->as.block.statements[i];
            if(!source_path_scan(item,scan)){valid=false;break;}
            if(source_path_shadow(scan->ctx,item))break;
        }
        --scan->depth;return valid&&scan->ctx->diagnostic.status==XR_XIR_OK;
    }
    if(!source_prelude_signature(scan,node))return false;
    ++scan->depth;bool valid=xr_ast_for_each_child(node,source_path_scan,scan);--scan->depth;
    return valid||source_fail(scan->ctx,node,XR_XIR_BAD_STRUCTURE,"Path type discovery syntax is unknown");
}
static bool source_path_owner(SourceContext *ctx,uint32_t module,const char *logical) {
    const XrModuleSpec *spec=ctx->graph->specs+module;
    if(spec->authority.kind!=XR_MODULE_IDENTITY_STDLIB||!spec->authority.namespace_id||
        !source_text_same(ctx,NULL,spec->authority.namespace_id,xir_path_prelude_module())||
        !source_text_same(ctx,NULL,spec->logical_path,logical))
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"Path declaration has a foreign module owner");
    if(source_owned_checked(spec))return true;
    if(!spec->source_text||!spec->ast||spec->ast->type!=AST_PROGRAM)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"Path declaration has no complete source owner");
    XrFingerprint fingerprint;xr_module_source_fingerprint(spec->source_text,&fingerprint);
    return source_span_same(ctx,NULL,&fingerprint,&spec->source_content_fingerprint,sizeof(fingerprint))||
        source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"Path declaration source changed during discovery");
}
static bool source_path_discover(SourceContext *ctx) {
    bool needed=false;uint32_t saved=ctx->module;
    for(uint32_t m=0;m<(uint32_t)ctx->graph->spec_count&&!needed;++m) {
        if(source_owned_checked(ctx->graph->specs+m))continue;
        ctx->module=m;SourcePreludeScan scan={ctx,0,false,true};
        if(!source_path_scan(ctx->graph->specs[m].ast,&scan)) {
            return false;
        }
        needed=scan.needed;
    }
    ctx->module=saved;if(!needed)return ctx->diagnostic.status==XR_XIR_OK;
    char logical[64],canonical[160];if(!xir_path_prelude_coordinates(logical,canonical))
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"Path builtin declaration route is missing");
    int index=-1;XrModuleStatus status=xr_compile_module_graph_find(ctx->graph,canonical,&index);
    if(status!=XR_MODULE_OK)return source_module_fail(ctx,NULL,status,"Path declaration graph lookup failed");
    if(index<0) {
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_STDLIB,xir_path_prelude_module(),NULL};
        status=xr_compile_module_graph_include_catalog_logical(ctx->graph,&authority,logical,&index);
        if(status!=XR_MODULE_OK)return source_module_fail(ctx,NULL,status,"Path Catalog selection failed");
    }
    if(index<0) {
        XrModuleId id={0};char *error=NULL;const XrModuleSpec *entry=ctx->graph->specs+ctx->graph->entry_index;
        status=xr_compile_module_resolver_resolve(ctx->graph->resolver,xir_path_prelude_module(),
            entry->source_path,&entry->authority,&id,&error);
        if(status==XR_MODULE_OK)status=id.source_path?
            xr_compile_module_graph_include(ctx->graph,id.source_path,&id.authority,&error):XR_MODULE_NOT_FOUND;
        if(status!=XR_MODULE_OK)source_module_fail(ctx,NULL,status,error?error:"Path declaration source discovery failed");
        xr_compile_resources_free(error);xr_compile_module_id_cleanup(&id);if(status!=XR_MODULE_OK)return false;
        status=xr_compile_module_graph_find(ctx->graph,canonical,&index);
    }
    return status==XR_MODULE_OK&&index>=0?source_path_owner(ctx,(uint32_t)index,logical):
        source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"Path declaration module is missing");
}
static bool source_path_prelude_type(SourceContext *ctx,XrXirType *type) {
    char logical[64],canonical[160];if(!xir_path_prelude_coordinates(logical,canonical))return false;
    int module=-1;XrModuleStatus status=xr_compile_module_graph_find(ctx->graph,canonical,&module);
    if(status!=XR_MODULE_OK||module<0||!source_path_owner(ctx,(uint32_t)module,logical))
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"Path type has no complete declaring module");
    SourceName *symbol=find_name(ctx,ctx->names[module],"Path");
    XrXirNominalTable table={ctx->nominals.declarations,ctx->nominals.count,NULL};
    XrXirDeclarations declarations={.modules=ctx->modules,.module_count=ctx->module_count};
    if(!symbol||symbol->kind!=SOURCE_NOMINAL||!xir_path_prelude_type(&declarations,&table,symbol->index))
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"Path type has no unique exported ordinary class");
    *type=symbol->type;return true;
}
