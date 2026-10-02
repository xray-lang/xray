/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_manifest.inc.c - Package-owned obligations bound before body checking
 */
static char *source_manifest_text(SourceContext *ctx, const char *text) {
    if (!text) return NULL;
    size_t length = 0;
    if (!source_text_length(ctx, NULL, text, &length)) return NULL;
    char *copy = source_alloc(ctx, length + 1, 1);
    if (copy && !source_copy_bytes(ctx, NULL, copy, text, length + 1)) return NULL;
    return copy;
}
static bool source_manifest_same(SourceContext *ctx, const XrModuleIdentityAuthority *a, const XrModuleIdentityAuthority *b) {
    return a->kind == b->kind && source_text_same(ctx, NULL, a->physical_root, b->physical_root) &&
        source_text_same(ctx, NULL, a->namespace_id ? a->namespace_id : "", b->namespace_id ? b->namespace_id : "");
}
static bool source_manifest_failure(SourceContext *ctx, XrDeclarationStatus status) {
    return source_fail(ctx, NULL, status == XR_DECLARATION_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY :
        status == XR_DECLARATION_LIMIT ? XR_XIR_BUDGET :
        status == XR_DECLARATION_IO ? XR_XIR_IO : XR_XIR_BAD_STRUCTURE,
        "declaration manifest admission failed");
}
static bool source_manifest_module(SourceContext *ctx, SourceManifest *manifest, const char *logical) {
    if (!source_work(ctx, NULL)) return false;
    XrOsIoPolicy policy = xr_compile_io_policy(ctx->compile.resources);
    XrFileBytes bytes = {0};
    XrFileReadStatus read = xr_os_io_read_under_root(&policy, manifest->authority.physical_root, logical, SIZE_MAX - 1, &bytes);
    if (read != XR_FILE_READ_OK)
        return source_fail(ctx, NULL, read == XR_FILE_READ_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY :
            read == XR_FILE_READ_LIMIT ? XR_XIR_BUDGET :
            read == XR_FILE_READ_IO ? XR_XIR_IO :
            read == XR_FILE_READ_MISSING ? XR_XIR_UNRESOLVED : XR_XIR_BAD_STRUCTURE,
            "declaration source is missing or outside its authority");
    for (size_t i = 0; i < bytes.size; ++i) {
        if (!source_work(ctx, NULL)) { xr_compile_resources_free(bytes.data); return false; }
        if (!bytes.data[i]) {
            xr_compile_resources_free(bytes.data);
            return source_fail(ctx, NULL, XR_XIR_BAD_STRUCTURE, "declaration source contains a NUL byte");
        }
    }
    XrFingerprint fingerprint;
    XrCompileResourceStatus hashed = xr_compile_module_source_fingerprint(ctx->compile.resources, bytes.data, &fingerprint);
    xr_compile_resources_free(bytes.data);
    if (hashed != XR_COMPILE_RESOURCE_OK)
        return source_fail(ctx, NULL, XR_XIR_BUDGET, "declaration fingerprint work exhausted");
    char *path = NULL;
    XrOsIoStatus joined = xr_path_join_owned(&policy, manifest->authority.physical_root, logical, &path);
    if (joined != XR_OS_IO_OK)
        return source_fail(ctx, NULL, joined == XR_OS_IO_BUDGET ? XR_XIR_BUDGET :
            joined == XR_OS_IO_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY :
            joined == XR_OS_IO_IO ? XR_XIR_IO : XR_XIR_BAD_STRUCTURE, "declaration path construction failed");
    char *error = NULL;
    XrModuleStatus result = xr_compile_module_graph_include(ctx->graph, path, &manifest->authority, &error);
    if (result) source_module_fail(ctx, NULL, result, error ? error : "declaration source discovery failed");
    xr_compile_resources_free(error); xr_compile_resources_free(path);
    if (result) return false;
    char *canonical = NULL;
    result = xr_compile_module_identity_from_logical(ctx->compile.resources, &manifest->authority, logical, &canonical);
    if (result != XR_MODULE_OK)
        return source_module_fail(ctx, NULL, result, "declaration identity construction failed");
    int index = -1;
    result = xr_compile_module_graph_find(ctx->graph, canonical, &index);
    xr_compile_resources_free(canonical);
    if (result != XR_MODULE_OK)
        return source_module_fail(ctx, NULL, result, "declaration source lookup failed");
    if (index < 0 || !source_span_same(ctx, NULL, &fingerprint, &ctx->graph->specs[index].source_content_fingerprint, sizeof(fingerprint)))
        return source_fail(ctx, NULL, XR_XIR_BAD_STRUCTURE, "declaration source changed during discovery");
    return true;
}
static bool source_manifest_load(SourceContext *ctx, const XrModuleIdentityAuthority *authority) {
    SourceManifest *manifest = source_alloc(ctx, 1, sizeof(*manifest));
    if (!manifest) return false;
    manifest->authority.kind = authority->kind;
    manifest->authority.namespace_id = source_manifest_text(ctx, authority->namespace_id);
    manifest->authority.physical_root = source_manifest_text(ctx, authority->physical_root);
    manifest->next = ctx->manifests; ctx->manifests = manifest;
    if (ctx->diagnostic.status != XR_XIR_OK) return false;
    XrDeclarationInputLimits limits = {{SIZE_MAX - 1, 128},
        {ctx->compile.limits.functions, ctx->compile.limits.parameters}};
    XrDeclarationStatus status = xr_compile_declaration_manifest_load(ctx->compile.resources,
        manifest->authority.physical_root, &limits, &manifest->declarations);
    if (status == XR_DECLARATION_ABSENT) return true;
    if (status != XR_DECLARATION_OK) return source_manifest_failure(ctx, status);
    for (uint32_t i = 0; i < manifest->declarations->count; ++i)
        if (!source_manifest_module(ctx, manifest, manifest->declarations->records[i].module)) return false;
    return true;
}
static bool source_manifests_load(SourceContext *ctx) {
    for (int m = 0; m < ctx->graph->spec_count; ++m) {
        ctx->module = (uint32_t)m;
        if (!source_work(ctx, NULL)) return false;
        const XrModuleIdentityAuthority *authority = &ctx->graph->specs[m].authority;
        if (authority->kind == XR_MODULE_IDENTITY_MEMORY || authority->kind == XR_MODULE_IDENTITY_STDLIB) continue;
        if (!authority->physical_root)
            return source_fail(ctx, NULL, XR_XIR_BAD_STRUCTURE, "declaration authority lacks a physical root");
        bool found = false;
        for (SourceManifest *manifest = ctx->manifests; manifest; manifest = manifest->next) {
            if (!source_work(ctx, NULL)) return false;
            if (source_manifest_same(ctx, &manifest->authority, authority)) { found = true; break; }
        }
        if (!found && !source_manifest_load(ctx, authority)) return false;
    }
    return true;
}
static bool source_manifest_parameters(SourceContext *ctx, uint32_t function, const XrDeclarationRecord *record) {
    AstNode *node = ctx->bodies[function].node;
    bool method = node->type == AST_METHOD_DECL;
    XrParamNode **parameters = method ? node->as.method_decl.params : node->as.function_decl.params;
    int count = method ? node->as.method_decl.param_count : node->as.function_decl.param_count;
    uint32_t offset = method && !node->as.method_decl.is_static ? 1 : 0;
    for (uint32_t p = 0; p < record->parameter_count; ++p) {
        uint32_t selected = UINT32_MAX;
        for (int i = 0; i < count; ++i) {
            if (!source_work(ctx, node)) return false;
            if (!source_text_same(ctx, node, parameters[i]->name, record->parameters[p])) continue;
            if (selected != UINT32_MAX)
                return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "ambiguous declaration parameter name");
            selected = (uint32_t)i;
        }
        if (selected == UINT32_MAX)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "declaration parameter name is missing");
        if (!source_parameter_promise(ctx, function, selected + offset)) return false;
    }
    return true;
}
static bool source_manifest_requirement(SourceContext *ctx, SourceDeclarationTarget target,
    const XrDeclarationRecord *record) {
    SourceName *owner = ctx->interface_sources[target.interface];
    AstNode *node = owner->node->as.interface_decl.methods[target.member];
    InterfaceMethodNode *method = &node->as.interface_method;
    XrXirInterfaceMethod *requirement = (XrXirInterfaceMethod *)&ctx->interfaces.declarations[target.interface].methods[target.member];
    XrXirTypeNode signature = *xr_xir_callable_signature(&ctx->types,requirement->signature);
    if (record->no_suspend) signature.flags |= XR_XIR_CALLABLE_NO_SUSPEND;
    XrXirCallableParameter *parameters = signature.parameter_count ?
        source_alloc(ctx,signature.parameter_count,sizeof(*parameters)) : NULL;
    if (signature.parameter_count && !parameters) return false;
    if (signature.parameter_count && !source_copy_bytes(ctx, node, parameters, signature.parameters,
        signature.parameter_count * sizeof(*parameters))) return false;
    signature.parameters = parameters;
    for (uint32_t p = 0; p < record->parameter_count; ++p) {
        uint32_t selected = UINT32_MAX;
        for (int a = 0; a < method->param_count; ++a) {
            if (!source_work(ctx,node)) return false;
            if (source_text_same(ctx, node, record->parameters[p],method->params[a]->name)) selected = (uint32_t)a;
        }
        if (selected == UINT32_MAX)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"interface declaration parameter name is missing");
        const XrXirTypeNode *found = xr_xir_callable_signature(&ctx->types,parameters[selected].type);
        if (!found || found->flags)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"interface callable promise target is invalid or duplicated");
        XrXirTypeNode qualified = *found; qualified.flags = XR_XIR_CALLABLE_NO_SUSPEND;
        if (!source_intern_type(ctx,qualified,&parameters[selected].type)) return false;
    }
    if (!source_intern_type(ctx,signature,&requirement->signature)) return false;
    SourceTypeScope saved = ctx->type_scope; uint32_t module = ctx->module;
    bool ok = source_interface_method_scope(ctx,owner,target.member) &&
        source_interface_method_query(ctx,owner,target.member,requirement);
    ctx->type_scope = saved; ctx->module = module; return ok;
}
static bool source_manifests_bind(SourceContext *ctx) {
    for (SourceManifest *manifest = ctx->manifests; manifest; manifest = manifest->next) {
        if (!manifest->declarations) continue;
        for (uint32_t i = 0; i < manifest->declarations->count; ++i) {
            if (!source_work(ctx, NULL)) return false;
            const XrDeclarationRecord *record = &manifest->declarations->records[i];
            char *canonical = NULL;
            XrModuleStatus identity_status = xr_compile_module_identity_from_logical(ctx->compile.resources, &manifest->authority, record->module, &canonical);
            if (identity_status != XR_MODULE_OK)
                return source_module_fail(ctx, NULL, identity_status, "declaration identity construction failed");
            size_t canonical_length = 0, name_length = 0, owner_length = 0;
            bool lengths = source_text_length(ctx, NULL, canonical, &canonical_length) &&
                source_text_length(ctx, NULL, record->name, &name_length) &&
                (!record->owner || source_text_length(ctx, NULL, record->owner, &owner_length));
            if (!lengths || canonical_length > UINT32_MAX || name_length > UINT32_MAX || owner_length > UINT32_MAX) {
                xr_compile_resources_free(canonical);
                return source_fail(ctx, NULL, XR_XIR_BUDGET, "declaration selector text exceeds its representation");
            }
            SourceDeclarationSelector selector = {{canonical, (uint32_t)canonical_length},
                {record->name, (uint32_t)name_length}, {record->owner, (uint32_t)owner_length}};
            SourceDeclarationTarget target = {0};
            bool found = source_declaration_target(ctx,&selector,&target); xr_compile_resources_free(canonical);
            if (!found) return false;
            if (target.requirement) {
                if (!source_manifest_requirement(ctx,target,record)) return false;
            } else {
                if (record->no_suspend) ctx->identities[target.function].promises = XR_XIR_FUNCTION_NO_SUSPEND;
                if (!source_manifest_parameters(ctx,target.function,record)) return false;
            }
        }
    }
    return true;
}
