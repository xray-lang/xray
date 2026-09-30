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
    size_t length = strlen(text);
    char *copy = source_alloc(ctx, length + 1, 1);
    if (copy) memcpy(copy, text, length + 1);
    return copy;
}
static bool source_manifest_same(const XrModuleIdentityAuthority *a, const XrModuleIdentityAuthority *b) {
    return a->kind == b->kind && !strcmp(a->physical_root, b->physical_root) &&
        !strcmp(a->namespace_id ? a->namespace_id : "", b->namespace_id ? b->namespace_id : "");
}
static bool source_manifest_failure(SourceContext *ctx, XrDeclarationStatus status) {
    return source_fail(ctx, NULL, status == XR_DECLARATION_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY :
        status == XR_DECLARATION_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE,
        "declaration manifest admission failed");
}
static uint64_t source_manifest_bytes(const XrDeclarationManifest *manifest) {
    if (!manifest) return 0;
    uint64_t bytes = sizeof(*manifest) + (uint64_t)manifest->count * sizeof(*manifest->records);
    for (uint32_t i = 0; i < manifest->count; ++i) {
        const XrDeclarationRecord *record = &manifest->records[i];
        bytes += strlen(record->module) + 1 + strlen(record->name) + 1;
        if (record->owner) bytes += strlen(record->owner) + 1;
        bytes += (uint64_t)record->parameter_count * sizeof(*record->parameters);
        for (uint32_t p = 0; p < record->parameter_count; ++p) bytes += strlen(record->parameters[p]) + 1;
    }
    return bytes;
}
static bool source_manifest_module(SourceContext *ctx, SourceManifest *manifest, const char *logical) {
    if (!source_work(ctx, NULL)) return false;
    uint64_t available = ctx->budget.metadata_bytes - ctx->allocated;
    size_t limit = available > SIZE_MAX - 1 ? SIZE_MAX - 1 : (size_t)available;
    XrFileBytes bytes = {0};
    XrFileReadStatus read = xr_file_read_under_root(manifest->authority.physical_root, logical, limit, &bytes);
    if (read != XR_FILE_READ_OK)
        return source_fail(ctx, NULL, read == XR_FILE_READ_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY :
            read == XR_FILE_READ_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE,
            "declaration source is missing or outside its authority");
    if (memchr(bytes.data, 0, bytes.size)) {
        xr_free(bytes.data);
        return source_fail(ctx, NULL, XR_XIR_BAD_STRUCTURE, "declaration source contains a NUL byte");
    }
    XrFingerprint fingerprint;
    xr_module_source_fingerprint(bytes.data, &fingerprint); xr_free(bytes.data);
    size_t root_size = strlen(manifest->authority.physical_root), logical_size = strlen(logical);
    if (root_size > SIZE_MAX - logical_size - 2)
        return source_fail(ctx, NULL, XR_XIR_BUDGET, "declaration path budget exhausted");
    char *path = source_alloc(ctx, root_size + logical_size + 2, 1);
    if (!path) return false;
    memcpy(path, manifest->authority.physical_root, root_size); path[root_size] = '/';
    memcpy(path + root_size + 1, logical, logical_size + 1);
    char *error = NULL;
    int result = xr_module_graph_include(ctx->graph, path, &manifest->authority, &error);
    if (result) source_fail(ctx, NULL, XR_XIR_BAD_STRUCTURE, error ? error : "declaration source discovery failed");
    xr_free(error);
    if (result) return false;
    char *canonical = NULL;
    if (!xr_module_identity_from_logical(&manifest->authority, logical, &canonical))
        return source_fail(ctx, NULL, XR_XIR_OUT_OF_MEMORY, "declaration identity allocation failed");
    int index = xr_module_graph_find(ctx->graph, canonical); xr_free(canonical);
    if (index < 0 || memcmp(&fingerprint, &ctx->graph->specs[index].source_content_fingerprint, sizeof(fingerprint)))
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
    uint64_t available = (ctx->budget.metadata_bytes - ctx->allocated) / 3;
    size_t bytes = available > SIZE_MAX ? SIZE_MAX : (size_t)available;
    size_t work = ctx->budget.work > SIZE_MAX ? SIZE_MAX : (size_t)ctx->budget.work;
    uint32_t record_work = work > UINT32_MAX ? UINT32_MAX : (uint32_t)work;
    XrDeclarationInputBudget budget = {{bytes, bytes, work, 128},
        {bytes, ctx->budget.functions, ctx->budget.parameters, record_work}};
    size_t consumed_work = 0;
    XrDeclarationStatus status = xr_declaration_manifest_load(manifest->authority.physical_root,
        budget, &manifest->declarations, &consumed_work);
    ctx->budget.work -= consumed_work;
    if (status == XR_DECLARATION_ABSENT) return true;
    if (status != XR_DECLARATION_OK) return source_manifest_failure(ctx, status);
    uint64_t owned_bytes = source_manifest_bytes(manifest->declarations);
    if (owned_bytes > ctx->budget.metadata_bytes - ctx->allocated)
        return source_fail(ctx, NULL, XR_XIR_BUDGET, "declaration ownership budget exhausted");
    ctx->allocated += owned_bytes;
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
            if (source_manifest_same(&manifest->authority, authority)) { found = true; break; }
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
            if (strcmp(parameters[i]->name, record->parameters[p])) continue;
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
    if (signature.parameter_count) memcpy(parameters,signature.parameters,signature.parameter_count * sizeof(*parameters));
    signature.parameters = parameters;
    for (uint32_t p = 0; p < record->parameter_count; ++p) {
        uint32_t selected = UINT32_MAX;
        for (int a = 0; a < method->param_count; ++a) {
            if (!source_work(ctx,node)) return false;
            if (!strcmp(record->parameters[p],method->params[a]->name)) selected = (uint32_t)a;
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
    source_interface_scope(ctx,owner);
    bool ok = source_interface_method_query(ctx,owner,target.member,requirement->signature);
    ctx->type_scope = saved; ctx->module = module; return ok;
}
static bool source_manifests_bind(SourceContext *ctx) {
    for (SourceManifest *manifest = ctx->manifests; manifest; manifest = manifest->next) {
        if (!manifest->declarations) continue;
        for (uint32_t i = 0; i < manifest->declarations->count; ++i) {
            if (!source_work(ctx, NULL)) return false;
            const XrDeclarationRecord *record = &manifest->declarations->records[i];
            char *canonical = NULL;
            if (!xr_module_identity_from_logical(&manifest->authority, record->module, &canonical))
                return source_fail(ctx, NULL, XR_XIR_OUT_OF_MEMORY, "declaration identity allocation failed");
            SourceDeclarationSelector selector = {{canonical, (uint32_t)strlen(canonical)},
                {record->name, (uint32_t)strlen(record->name)},
                {record->owner, record->owner ? (uint32_t)strlen(record->owner) : 0}};
            SourceDeclarationTarget target = {0};
            bool found = source_declaration_target(ctx,&selector,&target); xr_free(canonical);
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
