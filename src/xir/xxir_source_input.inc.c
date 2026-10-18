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
typedef struct SourceInputCapture { AstNode *ast; uint32_t module; } SourceInputCapture;
typedef struct SourceInstanceOperation {
    const XirSourceInstanceRequest *request;
    uint32_t *providers;
} SourceInstanceOperation;
typedef struct SourceComptimeInstancesOperation {
    const XirSourceComptimeRequest *request;
    XirSourceComptimeInstances *output;
} SourceComptimeInstancesOperation;
typedef struct SourceInput {
    bool supplied;
    XrXirSourceText text;
    const XrModuleOverlayInput *overlays;
    size_t overlay_count;
    bool prelude_factory;
    SourceInputCapture *capture;
    const size_t *included;
    size_t included_count;
    AstNode *program;
    const SourceInstanceOperation *instances;
    const SourceComptimeInstancesOperation *comptime_instances;
} SourceInput;
static bool source_graph_fail(SourceContext *ctx,XrModuleStatus status,const char *message) {
    const XrModuleParseDiagnostic *parse=ctx->graph?&ctx->graph->parse_diagnostic:NULL;
    source_module_fail(ctx,NULL,status,parse&&parse->present&&parse->message[0]?parse->message:message);
    if(parse&&parse->present) {
        ctx->diagnostic.module=parse->module;ctx->diagnostic.line=parse->line;ctx->diagnostic.column=parse->column;
    }
    return false;
}
static bool source_include_corpus(SourceContext *ctx, SourceInput input, char **error) {
    for (size_t i = 0; i < input.included_count; ++i) {
        if (!source_work(ctx, NULL)) return false;
        if (input.included[i] >= input.overlay_count)
            return source_fail(ctx, NULL, XR_XIR_BAD_STRUCTURE, "source corpus index is invalid");
        const XrModuleOverlayInput *source = &input.overlays[input.included[i]];
        char *canonical = NULL, *logical = NULL;
        const XrModuleOverlayEntry *owned = NULL;
        XrModuleStatus status = xr_compile_module_identity_from_source(ctx->compile.resources,
            &source->authority, source->source_path, &canonical, &logical);
        if (status == XR_MODULE_OK) status = xr_compile_module_overlay_lookup(
            ctx->graph->resolver->overlay, canonical, &source->authority, &owned);
        if (status == XR_MODULE_OK && !owned) status = XR_MODULE_INVALID;
        if (status == XR_MODULE_OK) status = xr_compile_module_graph_include_logical_source(
            ctx->graph, &source->authority, logical, source->source_path, owned->source.text, error);
        xr_compile_resources_free(canonical); xr_compile_resources_free(logical);
        if (status != XR_MODULE_OK)
            return source_graph_fail(ctx,status,"source corpus inclusion failed");
    }
    return true;
}
static XrXirStatus source_check_input(const XrXirSourceRequest *request, SourceInput input,
    XrXirSourceResult *output, XrXirSourceDiagnostic *diagnostic, XrXirSourceFailure *failure) {
    const SourceInstanceOperation *instances=input.instances;
    const SourceComptimeInstancesOperation *comptime_instances=input.comptime_instances;
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
        (!supplied && !request->entry_path) || !request->authority ||
        (supplied && !text) || !output || output->checked || output->snapshot ||
        (input.included_count && (!input.included || !overlays)) ||
        !xr_xir_compile_source_failure_empty(failure)) {
        source_fail(&ctx, NULL, XR_XIR_BAD_STRUCTURE, "source request is incomplete"); goto done;
    }
    ctx.compile = *request->context;
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
    if (supplied) {
        if (length == SIZE_MAX) {
            source_fail(&ctx, NULL, XR_XIR_BUDGET, "source text size exhausted"); goto done;
        }
        char *terminated = source_alloc(&ctx, length + 1, 1);
        if (!terminated) goto done;
        for (size_t i = 0; i < length; ++i) {
            if (!source_work(&ctx, NULL)) goto done;
            if (!text[i]) {
                source_fail(&ctx, NULL, XR_XIR_BAD_STRUCTURE, "source text contains an embedded NUL"); goto done;
            }
        }
        if (!source_copy_bytes(&ctx, NULL, terminated, text, length)) goto done;
        if (input.program) {
            XrModuleSourceAstInput syntax = {request->authority, logical_path,
                request->entry_path, terminated, input.program};
            module_status = xr_compile_module_graph_build_ast(ctx.graph, &syntax, &error);
        } else {
            module_status = xr_compile_module_graph_build_logical_source(ctx.graph,
                request->authority, logical_path, request->entry_path, terminated, &error);
        }
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
        source_graph_fail(&ctx,module_status,error?error:"module graph build failed");goto done;
    }
    if (!source_include_corpus(&ctx, input, &error)) goto done;
    if (input.prelude_factory) ctx.prelude_module=(uint32_t)ctx.graph->entry_index+1;
    if (!source_manifests_load(&ctx) || (!input.prelude_factory && !source_prelude_discover(&ctx))) goto done;
    module_status = xr_compile_module_graph_topological_sort(ctx.graph);
    if (module_status != XR_MODULE_OK) {
        source_module_fail(&ctx, NULL, module_status, "module graph ordering failed"); goto done;
    }
    if (ctx.graph->has_cycle || ctx.graph->entry_index < 0) {
        source_fail(&ctx, NULL, XR_XIR_BAD_STRUCTURE, error ? error : "module graph is not an acyclic source closure"); goto done;
    }
    semantic_started = true;
    source_construct(&ctx, &result);
    if (comptime_instances) source_comptime_instances_prepare(&ctx,result.checked,comptime_instances->request,comptime_instances->output);
    else {
        source_comptime_receive(&ctx, &result);
        if (instances) source_instance_receive(&ctx,&result,instances->request,instances->providers);
    }
done:
    /* A comptime receiver has already copied the complete final query from its
     * own construction. The original context publishes only its own result. */
    if (!comptime_instances && !result.snapshot) source_query_publish(&ctx, &result);
    const XrModuleParseDiagnostic *parse=ctx.graph?&ctx.graph->parse_diagnostic:NULL;
    if (failure && (semantic_started||(parse&&parse->present)) && ctx.diagnostic.status != XR_XIR_OK &&
        ctx.diagnostic.line > 0 && ctx.graph &&
        ctx.diagnostic.module < (uint32_t)ctx.graph->spec_count) {
        XrModuleSpec *module = &ctx.graph->specs[ctx.diagnostic.module];
        XrXirSourceFailure owned={.path=module->source_path};
        if(parse&&parse->present) {
            owned.origin=XR_XIR_SOURCE_DIAGNOSTIC_PARSE;owned.has_utf16_position=parse->has_utf16_position;
            if(parse->has_utf16_position){owned.utf16_line=parse->utf16_position.line;owned.utf16_column=parse->utf16_position.column;}
        }
        *failure=owned;
        module->source_path = NULL;
    }
    if (ctx.diagnostic.status == XR_XIR_OK && input.capture) {
        input.capture->module = (uint32_t)ctx.graph->entry_index;
        input.capture->ast = ctx.graph->specs[ctx.graph->entry_index].ast;
        ctx.graph->specs[ctx.graph->entry_index].ast = NULL;
    }
    xr_compile_resources_free(error);
    for (SourceManifest *manifest = ctx.manifests; manifest; manifest = manifest->next)
        xr_compile_declaration_manifest_free(manifest->declarations);
    xr_xir_compile_construction_free(ctx.construction);
    source_release_retired_arrays(&ctx);
    while (ctx.memory) { SourceMemory *next = ctx.memory->next; xr_compile_resources_free(ctx.memory); ctx.memory = next; }
    xr_compile_module_graph_free(ctx.graph); xr_compile_module_resolver_free(resolver);
    if (ctx.diagnostic.status == XR_XIR_OK && !comptime_instances) *output = result;
    else xr_xir_compile_source_result_free(&result);
    if (diagnostic) *diagnostic = ctx.diagnostic;
    return ctx.diagnostic.status;
}

XrXirStatus xr_xir_compile_source_check(const XrXirSourceRequest *request,
    XrXirSourceResult *output, XrXirSourceDiagnostic *diagnostic, XrXirSourceFailure *failure) {
    return source_check_input(request, (SourceInput){0}, output, diagnostic, failure);
}
XrXirStatus xr_xir_compile_source_check_text(const XrXirSourceRequest *request,
    const XrXirSourceText *text,
    XrXirSourceResult *output, XrXirSourceDiagnostic *diagnostic, XrXirSourceFailure *failure) {
    SourceInput input = {.supplied=true, .text=text ? *text : (XrXirSourceText){0}};
    return source_check_input(request, input, output, diagnostic, failure);
}

XrXirStatus xr_xir_compile_source_check_ast(const XrXirSourceRequest *request,
    const XrXirSourceAst *syntax, XrXirSourceResult *output,
    XrXirSourceDiagnostic *diagnostic, XrXirSourceFailure *failure) {
    if (!syntax || !syntax->program) {
        if (diagnostic) *diagnostic = (XrXirSourceDiagnostic){.status=XR_XIR_BAD_STRUCTURE};
        return XR_XIR_BAD_STRUCTURE;
    }
    SourceInput input = {.supplied=true, .text=syntax->source, .program=syntax->program};
    return source_check_input(request, input, output, diagnostic, failure);
}

XrXirStatus xr_xir_compile_source_check_overlay(const XrXirSourceRequest *request,
    const XrModuleOverlayInput *overlays, size_t count, XrXirSourceResult *output,
    XrXirSourceDiagnostic *diagnostic, XrXirSourceFailure *failure) {
    SourceInput input = {.overlays=overlays, .overlay_count=count};
    return source_check_input(request, input, output, diagnostic, failure);
}
XR_FUNC XrXirStatus xr_xir_compile_source_check_corpus(const XrXirSourceCorpusRequest *request,
    XrXirSourceResult *output, XrXirSourceDiagnostic *diagnostic, XrXirSourceFailure *failure) {
    SourceInput input = {0};
    if (request) input = (SourceInput){.overlays=request->overlays,
        .overlay_count=request->overlay_count, .included=request->included,
        .included_count=request->included_count};
    return source_check_input(request ? request->entry : NULL, input, output, diagnostic, failure);
}

XR_FUNC void xr_xir_compile_source_instance_constants_free(XirSourceInstanceConstants *result) {
    if (!result) return;
    xr_xir_compile_source_result_free(&result->source);
    xr_compile_resources_free(result->functions);*result=(XirSourceInstanceConstants){0};
}
XR_FUNC XrXirStatus xr_xir_compile_source_instance_constants(const XirSourceInstanceRequest *request,
    XirSourceInstanceConstants *output,XrXirSourceDiagnostic *diagnostic) {
    if (!request || !request->source || !xir_compile_context_valid(request->source->context) ||
        !request->selections || !request->count || request->count>request->source->context->limits.functions ||
        (request->text && (request->overlays || request->overlay_count)) ||
        !output || output->source.checked || output->source.snapshot || output->functions || output->count ||
        request->limits.steps>XR_XIR_CTFE_MAX_STEPS || request->limits.frame_bytes>XR_XIR_CTFE_MAX_FRAME_BYTES ||
        request->limits.output_bytes>XR_XIR_CTFE_MAX_OUTPUT_BYTES || request->call_depth>XR_XIR_CTFE_MAX_CALL_DEPTH) {
        if (diagnostic) *diagnostic=(XrXirSourceDiagnostic){.status=XR_XIR_BAD_STRUCTURE};
        return XR_XIR_BAD_STRUCTURE;
    }
    XirSourceInstanceConstants result={0};XrXirStatus status=XR_XIR_OK;
    uint64_t bytes=(uint64_t)request->count*sizeof(*result.functions);
    if (bytes>SIZE_MAX) {
        if (diagnostic) *diagnostic=(XrXirSourceDiagnostic){.status=XR_XIR_BUDGET};
        return XR_XIR_BUDGET;
    }
    result.functions=xir_compile_calloc(request->source->context,request->count,sizeof(*result.functions),&status);
    if (!result.functions) {
        if (diagnostic) *diagnostic=(XrXirSourceDiagnostic){.status=status};
        return status;
    }
    SourceInstanceOperation operation={request,result.functions};
    SourceInput input={.supplied=request->text!=NULL,.text=request->text?*request->text:(XrXirSourceText){0},
        .overlays=request->overlays,.overlay_count=request->overlay_count,.instances=&operation};
    status=source_check_input(request->source,input,&result.source,diagnostic,NULL);
    if (status==XR_XIR_OK && !xir_compile_work(request->source->context,sizeof(*output))) status=XR_XIR_BUDGET;
    if (status!=XR_XIR_OK) {
        xr_xir_compile_source_instance_constants_free(&result);
        if (diagnostic) diagnostic->status=status;
        return status;
    }
    result.count=request->count;*output=result;return XR_XIR_OK;
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
    SourceInput input={.supplied=true, .text={XIR_PRELUDE_LOGICAL,xir_prelude_source.text,length}, .prelude_factory=true};
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
#include "xxir_source_comptime_instances_input.inc.c"
