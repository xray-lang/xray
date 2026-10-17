/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_input.inc.c - One file, text and overlay Source input kernel
 *
 * KEY CONCEPT:
 *   Input selection precedes the same checked construction and owned publication.
 */
#include "xxir_source_ast_owner.h"
typedef struct SourceInputCapture {
    AstNode *ast;
    uint32_t module;
    SourceSemanticEnvironment *semantic;
} SourceInputCapture;
typedef struct SourceInput {
    bool supplied;
    XrXirSourceText text;
    const XrModuleOverlayInput *overlays;
    size_t overlay_count;
    bool prelude_factory;
    SourceInputCapture *capture;
    AstNode *ast;
    const XrXirSourceAstOwner *ast_owner;
    bool ast_input;
    bool binding_readers;
    bool interactive, type_query;
    const char *module_scope;
    const SourceHistoryUnit *history, *self;
} SourceInput;
static void source_input_nul(SourceContext *ctx,const char *path,size_t line,size_t column,char **failure_path) {
    if(line>INT_MAX || column>INT_MAX) {
        source_fail(ctx,NULL,XR_XIR_BUDGET,"source location is not representable");return;
    }
    char *owned_path=NULL;
    if(failure_path && path) {
        size_t length=0;if(!source_text_length(ctx,NULL,path,&length))return;
        XrXirStatus status=XR_XIR_OK;
        owned_path=xir_compile_copy(&ctx->compile,path,length+1,&status);
        if(!owned_path){source_fail(ctx,NULL,status,"source failure path copy failed");return;}
    }
    source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source text contains an embedded NUL");
    ctx->diagnostic.origin=XR_XIR_SOURCE_DIAGNOSTIC_INPUT;
    ctx->diagnostic.line=(int)line;ctx->diagnostic.column=(int)column;
    if(failure_path)*failure_path=owned_path;
}
static XrXirStatus source_check_input(const XrXirSourceRequest *request, SourceInput input,
    XrXirSourceResult *output, XrXirSourceDiagnostic *diagnostic, char **failure_path) {
    const bool supplied = input.supplied;
    const char *logical_path = input.text.logical_path, *text = input.text.text;
    const size_t length = input.text.length, overlay_count = input.overlay_count;
    const XrModuleOverlayInput *overlays = input.overlays;
    SourceContext ctx = {0};
    XrXirSourceResult result = {0};
    XrModuleResolver *resolver = NULL;
    char *error = NULL;
    bool semantic_started = false;
    if (!request || !xir_compile_context_valid(request->context) || !request->session ||
        (!supplied && !input.ast_input && !request->entry_path) || !request->authority ||
        (supplied && !text) || !output || output->checked || output->snapshot ||
        (input.ast_input && (!input.ast || supplied || overlays || overlay_count ||
            (input.capture && (!input.ast_owner || input.ast_owner->input.program!=input.ast ||
                input.ast_owner->resources!=request->context->resources)))) ||
        (input.binding_readers && (!input.capture || request->linkage_kind!=XR_XIR_PROGRAM)) ||
        (failure_path && *failure_path)) {
        source_fail(&ctx, NULL, XR_XIR_BAD_STRUCTURE, "source request is incomplete"); goto done;
    }
    ctx.compile = *request->context;
    ctx.semantic_output=input.capture ? &input.capture->semantic : NULL;
    ctx.binding_readers=input.binding_readers;
    ctx.interactive=input.interactive;ctx.type_query=input.type_query;ctx.module_scope=input.module_scope;
    ctx.history_input=input.history;ctx.self_unit=input.self;
    ctx.remaining_blocks = ctx.compile.limits.blocks;
    ctx.remaining_instructions = ctx.compile.limits.instructions;
    XrOsIoPolicy policy = xr_compile_io_policy(ctx.compile.resources);
    if (xr_compile_session_resources(request->session) != ctx.compile.resources ||
        (request->lockfile && !xr_lockfile_uses_policy(request->lockfile, &policy)) ||
        (request->libraries && xr_xir_compile_library_catalog_context(request->libraries)->resources != ctx.compile.resources)) {
        source_fail(&ctx, NULL, XR_XIR_BAD_STRUCTURE, "source request has a foreign resource owner"); goto done;
    }
    XrXirStatus resource_status = xir_compile_resource_status(xr_compile_session_resource_status(request->session));
    if (resource_status != XR_XIR_OK) {
        source_fail(&ctx, NULL, resource_status, "source session resources are unavailable"); goto done;
    }
    if (request->linkage_kind != XR_XIR_PROGRAM && request->linkage_kind != XR_XIR_LIBRARY) {
        source_fail(&ctx,NULL,XR_XIR_BAD_STRUCTURE,"source linkage kind is invalid"); goto done;
    }
    ctx.linkage_kind = request->linkage_kind;
    XrModuleResolverConfig config = {request->stdlib_path, request->lockfile, request->libraries};
    XrModuleStatus module_status = xr_compile_module_resolver_new(ctx.compile.resources, &config, &resolver);
    if (module_status == XR_MODULE_OK && (overlays || overlay_count))
        module_status = xr_compile_module_resolver_set_overlay(resolver, overlays, overlay_count);
    if (module_status == XR_MODULE_OK)
        module_status = xr_compile_module_graph_new(ctx.compile.resources, request->session, resolver, &ctx.graph);
    if (module_status != XR_MODULE_OK) {
        source_module_fail(&ctx, NULL, module_status, "module graph allocation failed"); goto done;
    }
    if (input.ast_input) {
        XrModuleAstInput entry = {input.ast,request->authority,logical_path,request->entry_path};
        module_status = xr_compile_module_graph_build_ast(ctx.graph,&entry,&error);
    } else if (supplied) {
        if (length == SIZE_MAX) {
            source_fail(&ctx, NULL, XR_XIR_BUDGET, "source text size exhausted"); goto done;
        }
        char *terminated = source_alloc(&ctx, length + 1, 1);
        if (!terminated) goto done;
        size_t line=1,line_start=0;
        for (size_t i = 0; i < length; ++i) {
            if (!source_work(&ctx, NULL)) goto done;
            if (!text[i]) {
                source_input_nul(&ctx,request->entry_path,line,i-line_start+1,failure_path);goto done;
            }
            if(text[i]=='\n'){++line;line_start=i+1;}
        }
        if (!source_copy_bytes(&ctx, NULL, terminated, text, length)) goto done;
        module_status = xr_compile_module_graph_build_logical_source(ctx.graph,
            request->authority, logical_path, request->entry_path, terminated, &error);
        source_release_private(terminated);
    } else if (resolver->overlay) {
        char *canonical = NULL, *logical = NULL; const XrModuleOverlayEntry *entry = NULL;
        module_status = xr_compile_module_identity_from_source(ctx.compile.resources,
            request->authority, request->entry_path, &canonical, &logical);
        if (module_status == XR_MODULE_OK) module_status = xr_compile_module_overlay_lookup(
            resolver->overlay, canonical, request->authority, &entry);
        if (module_status == XR_MODULE_OK) module_status = entry ?
            xr_compile_module_graph_build_logical_source(ctx.graph, request->authority, logical,
                request->entry_path, entry->source.text, &error) :
            xr_compile_module_graph_build(ctx.graph, request->entry_path, request->authority, &error);
        xr_compile_resources_free(canonical); xr_compile_resources_free(logical);
    } else {
        module_status = xr_compile_module_graph_build(ctx.graph, request->entry_path, request->authority, &error);
    }
    if (module_status != XR_MODULE_OK) {
        const XrModuleSourceDiagnostic *source=&ctx.graph->source_diagnostic;
        source_module_fail(&ctx, NULL, module_status, source->present && source->message[0] ?
            source->message : error ? error : "module graph build failed");
        if(source->present) {
            ctx.diagnostic.origin=source->origin==XR_MODULE_DIAGNOSTIC_PARSE ?
                XR_XIR_SOURCE_DIAGNOSTIC_PARSE : source->origin==XR_MODULE_DIAGNOSTIC_INPUT ?
                XR_XIR_SOURCE_DIAGNOSTIC_INPUT : XR_XIR_SOURCE_DIAGNOSTIC_DEFAULT;
            ctx.diagnostic.module=source->module;ctx.diagnostic.line=source->line;ctx.diagnostic.column=source->column;
        }
        goto done;
    }
    if (input.prelude_factory) ctx.prelude_module=(uint32_t)ctx.graph->entry_index+1;
    if (!source_manifests_load(&ctx) || (!input.prelude_factory && !source_prelude_discover(&ctx))) goto done;
    if (!source_history_input(&ctx)) goto done;
    module_status = xr_compile_module_graph_topological_sort(ctx.graph);
    if (module_status != XR_MODULE_OK) {
        source_module_fail(&ctx, NULL, module_status, "module graph ordering failed"); goto done;
    }
    if (ctx.graph->has_cycle || ctx.graph->entry_index < 0) {
        source_fail(&ctx, NULL, XR_XIR_BAD_STRUCTURE, error ? error : "module graph is not an acyclic source closure"); goto done;
    }
    if(!source_repl_select_tail(&ctx))goto done;
    semantic_started = true;
    source_construct(&ctx, &result);
    source_comptime_receive(&ctx, &result);
done:
    /* A comptime receiver has already copied the complete final query from its
     * own construction. The original context publishes only its own result. */
    if (!result.snapshot) source_query_publish(&ctx, &result);
    if (failure_path && (semantic_started || ctx.diagnostic.origin==XR_XIR_SOURCE_DIAGNOSTIC_PARSE ||
        ctx.diagnostic.origin==XR_XIR_SOURCE_DIAGNOSTIC_INPUT) &&
        ctx.diagnostic.status != XR_XIR_OK &&
        ctx.diagnostic.line > 0 && ctx.graph &&
        ctx.diagnostic.module < (uint32_t)ctx.graph->spec_count) {
        XrModuleSpec *module = &ctx.graph->specs[ctx.diagnostic.module];
        *failure_path = module->source_path;
        module->source_path = NULL;
    }
    if (ctx.diagnostic.status == XR_XIR_OK && input.capture) {
        if (!input.capture->semantic) source_fail(&ctx,NULL,XR_XIR_BAD_STRUCTURE,"final Source semantic environment is missing");
        else {
            input.capture->module=(uint32_t)ctx.graph->entry_index;
            input.capture->ast=ctx.graph->specs[ctx.graph->entry_index].ast;
            input.capture->semantic->graph=ctx.graph;
            ctx.graph->compiler_session=NULL;ctx.graph->resolver=NULL;ctx.graph=NULL;
        }
    }
    xr_compile_resources_free(error);
    for (SourceManifest *manifest = ctx.manifests; manifest; manifest = manifest->next)
        xr_compile_declaration_manifest_free(manifest->declarations);
    xr_xir_compile_construction_free(ctx.construction);
    while (ctx.memory) { SourceMemory *next = ctx.memory->next; xr_compile_resources_free(ctx.memory); ctx.memory = next; }
    xr_compile_module_graph_free(ctx.graph); xr_compile_module_resolver_free(resolver);
    if (ctx.diagnostic.status == XR_XIR_OK) *output = result;
    else xr_xir_compile_source_result_free(&result);
    if (diagnostic) *diagnostic = ctx.diagnostic;
    return ctx.diagnostic.status;
}

XrXirStatus xr_xir_compile_source_check(const XrXirSourceRequest *request,
    XrXirSourceResult *output, XrXirSourceDiagnostic *diagnostic, char **failure_path) {
    return source_check_input(request, (SourceInput){0}, output, diagnostic, failure_path);
}
XrXirStatus xr_xir_compile_source_check_text(const XrXirSourceRequest *request,
    const XrXirSourceText *text,
    XrXirSourceResult *output, XrXirSourceDiagnostic *diagnostic, char **failure_path) {
    SourceInput input = {.supplied=true,.text=text ? *text : (XrXirSourceText){0}};
    return source_check_input(request, input, output, diagnostic, failure_path);
}

XrXirStatus xr_xir_compile_source_check_overlay(const XrXirSourceRequest *request,
    const XrModuleOverlayInput *overlays, size_t count, XrXirSourceResult *output,
    XrXirSourceDiagnostic *diagnostic, char **failure_path) {
    SourceInput input = {.overlays=overlays,.overlay_count=count};
    return source_check_input(request, input, output, diagnostic, failure_path);
}

XR_FUNC XrXirStatus xr_xir_compile_source_check_ast(const XrXirSourceRequest *request,
    const XrXirSourceAst *ast, XrXirSourceResult *output,
    XrXirSourceDiagnostic *diagnostic, char **failure_path) {
    SourceInput input = {.text = {.logical_path = ast ? ast->logical_path : NULL},
        .ast = ast ? ast->program : NULL, .ast_input = true};
    return source_check_input(request,input,output,diagnostic,failure_path);
}

XR_FUNC XrXirStatus xr_xir_compile_prelude_library(const XrXirCompileContext *context,
    XrXirSourceResult *output,XrXirSourceDiagnostic *diagnostic) {
    if (!xir_compile_context_valid(context) || !output || output->checked || output->snapshot) {
        if (diagnostic) *diagnostic=(XrXirSourceDiagnostic){.status=XR_XIR_BAD_STRUCTURE};
        return XR_XIR_BAD_STRUCTURE;
    }
    XrCompilerSession *session=NULL;
    XrCompilerSessionStatus opened=xr_compile_session_new(context->resources,&session);
    if (opened!=XR_COMPILER_SESSION_OK) {
        XrXirStatus status=opened==XR_COMPILER_SESSION_BUDGET ? XR_XIR_BUDGET :
            opened==XR_COMPILER_SESSION_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BAD_STRUCTURE;
        if (diagnostic) *diagnostic=(XrXirSourceDiagnostic){.status=status};
        return status;
    }
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_STDLIB,"prelude",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_LIBRARY,NULL};
    size_t length=0;
    while (true) {
        if (!xir_compile_work(context,1)) {
            xr_compile_session_free(session);
            if (diagnostic) *diagnostic=(XrXirSourceDiagnostic){.status=XR_XIR_BUDGET};
            return XR_XIR_BUDGET;
        }
        if (!xir_prelude_source.text[length]) break;
        if (length==SIZE_MAX-1) {
            xr_compile_session_free(session);
            if (diagnostic) *diagnostic=(XrXirSourceDiagnostic){.status=XR_XIR_BUDGET};
            return XR_XIR_BUDGET;
        }
        ++length;
    }
    SourceInput input={.supplied=true,.text={XIR_PRELUDE_LOGICAL,xir_prelude_source.text,length},.prelude_factory=true};
    XrXirSourceResult result={0};
    XrXirStatus status=source_check_input(&request,input,&result,diagnostic,NULL);
    if (status==XR_XIR_OK) {
        bool admitted=false;
        status=library_prelude_module(context,xr_xir_compile_artifact_module(result.checked),0,&admitted);
        if (status==XR_XIR_OK && !admitted) status=XR_XIR_BAD_STRUCTURE;
    }
    if (diagnostic) diagnostic->status=status;
    xr_compile_session_free(session);
    if (status==XR_XIR_OK) { *output=result; return status; }
    xr_xir_compile_source_result_free(&result); return status;
}
