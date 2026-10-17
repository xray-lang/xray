/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_imports.inc.c - Checked import identities and lexical observations
 *
 * KEY CONCEPT:
 *   Resolution supplies module identity while parser facts supply spellings.
 *   Missing lexical bindings remain explicit rather than inferred from text.
 */
static XrNameSpan source_import_binding_span(const ImportStmtNode *decl,int index) {
    if(decl->member_count)
        return decl->members[index].alias?decl->members[index].alias_span:decl->members[index].name_span;
    return decl->alias_span.line?decl->alias_span:decl->module_name_span;
}
static bool source_import_marker(SourceContext *ctx,uint32_t declaration,XrXirSourceRange range,XrXirSourceMarkerRole role) {
    XrXirSourceMarker *facts=source_query_append(ctx,ctx->syntax.markers,&ctx->syntax.marker_count,
        &ctx->syntax_marker_capacity,sizeof(*facts));
    if(!facts)return false;ctx->syntax.markers=facts;
    XrXirSourceMarker marker={range,declaration,role};
    return source_copy_bytes(ctx,NULL,&facts[ctx->syntax.marker_count-1],&marker,sizeof(marker));
}
static bool source_import_syntax(SourceContext *ctx,const ImportStmtNode *decl,int index,uint32_t declaration,XrXirSourceRange range) {
    if(decl->member_count) {
        return decl->members[index].alias||!range.line||
            source_import_marker(ctx,declaration,range,XR_XIR_SOURCE_MARKER_IMPORT_BINDING);
    }
    if(decl->alias_span.line)return true;
    if(range.line)return source_import_marker(ctx,declaration,range,XR_XIR_SOURCE_MARKER_IMPORT_BINDING);
    XrNameSpan end=decl->specifier_end;
    if(!end.line)return true;
    XrXirSourceRange insertion={ctx->module,end.line,end.column,end.line,end.column};
    return source_import_marker(ctx,declaration,insertion,XR_XIR_SOURCE_MARKER_IMPORT_IMPLICIT);
}
static bool declare_import(SourceContext *ctx, AstNode *node) {
    ImportStmtNode *decl = &node->as.import_stmt;
    XrModuleSpec *spec = &ctx->graph->specs[ctx->module];
    XrModuleId id = {0}; char *error = NULL;
    XrModuleStatus resolved = xr_compile_module_resolver_resolve(ctx->graph->resolver, decl->module_name, spec->source_path, &spec->authority, &id, &error);
    int target = -1;
    if (resolved == XR_MODULE_OK && id.canonical && id.logical_path)
        resolved = xr_compile_module_graph_find(ctx->graph, id.canonical, &target);
    if (target >= 0 && !source_resolved_identity(ctx, &ctx->graph->specs[target], &id)) target = -1;
    xr_compile_resources_free(error); xr_compile_module_id_cleanup(&id);
    if (target < 0) {
        ctx->query_ready = false;
        return resolved != XR_MODULE_OK ?
            source_module_fail(ctx, node, resolved, "import resolution failed") :
            source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "import does not resolve to the parsed graph");
    }
    for (int i = 0; i < (decl->member_count ? decl->member_count : 1); ++i) {
        const char *name = decl->member_count ? (decl->members[i].alias ? decl->members[i].alias : decl->members[i].name) : decl->alias;
        SourceName *symbol = add_name(ctx, &ctx->names[ctx->module], name, node);
        if (!symbol) return false;
        symbol->kind = decl->member_count ? SOURCE_IMPORT : SOURCE_MODULE;
        symbol->module = (uint32_t) target;
        symbol->imported = decl->member_count ? decl->members[i].name : NULL;
        XrNameSpan span=source_import_binding_span(decl,i);
        XrXirSourceRange range={ctx->module,0,0,0,0};
        if(span.line)range=source_query_name_span(ctx,span,name);
        if(ctx->diagnostic.status!=XR_XIR_OK)return false;
        if (!source_query_declare(ctx, symbol, decl->member_count ? XR_XIR_SOURCE_IMPORT : XR_XIR_SOURCE_MODULE,0,range)) return false;
        if(!source_import_syntax(ctx,decl,i,symbol->declaration,range))return false;
    }
    return true;
}
static bool source_import_selectors(SourceContext *ctx,SourceName *binding) {
    SourceName *target=imported_declaration(ctx,binding,binding->imported);
    if(!target)return false;
    const ImportStmtNode *decl=&binding->node->as.import_stmt;
    for(int i=0;i<decl->member_count;++i) {
        if(!source_work(ctx,binding->node))return false;
        const ImportMember *member=&decl->members[i];
        const char *local=member->alias?member->alias:member->name;
        if(!source_text_same(ctx,binding->node,local,binding->name))continue;
        if(!member->name_span.line)return true;
        XrXirSourceRange range=source_query_name_span(ctx,member->name_span,member->name);
        return ctx->diagnostic.status==XR_XIR_OK&&
            source_query_target_reference(ctx,range,target->declaration,XR_XIR_SOURCE_IMPORT_SELECTOR);
    }
    return source_fail(ctx,binding->node,XR_XIR_BAD_STRUCTURE,"import binding has no parser member");
}
