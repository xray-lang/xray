/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_nominal_access.inc.c - Declaration-bound field and construction authority
 */
static bool nominal_access_work(const XrXirCompileContext *work, uint64_t amount) {
    if (!work ||!xir_compile_work(work, amount)) return false;
     return true;
}
XR_FUNC XrXirStatus xr_xir_compile_nominal_access(const XrXirCompileContext *compile_context, const XrXirModule *module, uint32_t function, uint32_t declaration, uint32_t field, XrXirNominalAccess access) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *work = &compile_state;
    if (!nominal_access_work(work, 1)) return XR_XIR_BUDGET;
    if (!module || !module->declarations || !module->types || !module->types->nominals ||
        function >= module->function_count || declaration >= module->types->nominals->count ||
        access < XR_XIR_NOMINAL_CONSTRUCT || access > XR_XIR_NOMINAL_TYPE ||
        ((access == XR_XIR_NOMINAL_CONSTRUCT || access == XR_XIR_NOMINAL_TYPE) && field)) return XR_XIR_BAD_STRUCTURE;
    const XrXirDeclarations *scope = module->declarations;
    const XrXirFunctionIdentity *caller = &scope->functions[function];
    const XrXirNominalTable *table = module->types->nominals;
    XrXirNominalIdentity identity;
    if (table->declarations) {
        const XrXirNominalDeclaration *d = &table->declarations[declaration];
        identity = (XrXirNominalIdentity) {d->module, d->name, d->exported, d->parameter_count, NULL, d->field_count, d->kind, d->variants, d->variant_count, d->flags};
    } else identity = table->identities[declaration];
    uint32_t declaring_module = UINT32_MAX;
    for (uint32_t m = 0; m < scope->module_count; ++m) {
        const XrXirSourceModule *candidate = &scope->modules[m];
        if (!nominal_access_work(work, (uint64_t) identity.module.length + 1)) return XR_XIR_BUDGET;
        if (candidate->name_length == identity.module.length &&
            !memcmp(candidate->name, identity.module.bytes, identity.module.length)) { declaring_module = m; break; }
    }
    if (declaring_module == UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    if (declaring_module != caller->module) {
        if (!identity.exported) return XR_XIR_BAD_TYPE;
        if (!nominal_access_work(work, scope->modules[caller->module].dependency_count)) return XR_XIR_BUDGET;
        if (!xr_xir_module_imports(scope, caller->module, declaring_module)) return XR_XIR_BAD_TYPE;
    }
    if (access == XR_XIR_NOMINAL_TYPE) return XR_XIR_OK;
    if (access != XR_XIR_NOMINAL_CONSTRUCT && field >= identity.field_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t first = access == XR_XIR_NOMINAL_CONSTRUCT ? 0 : field;
    uint32_t end = access == XR_XIR_NOMINAL_CONSTRUCT ? identity.field_count : field + 1;
    for (uint32_t f = first; f < end; ++f) {
        if (!nominal_access_work(work, 1)) return XR_XIR_BUDGET;
        uint32_t flags = table->declarations ? table->declarations[declaration].fields[f].flags : identity.fields[f].flags;
        if ((flags & (XR_XIR_FIELD_PRIVATE | XR_XIR_FIELD_PROTECTED)) && caller->nominal_owner != declaration + 1)
            return XR_XIR_BAD_TYPE;
        if (access == XR_XIR_NOMINAL_WRITE && !(flags & XR_XIR_FIELD_MUTABLE)) return XR_XIR_BAD_TYPE;
    }
    return XR_XIR_OK;
}

static XrXirStatus type_access_edge(const XrXirTypes *types, XrXirType type,
    uint32_t earlier, unsigned char *pending, XrXirCompileContext *remaining) {
    if (!xir_compile_work(remaining, 1)) return XR_XIR_BUDGET;

    if (!xr_xir_type_node(types, type)) return XR_XIR_OK;
    uint32_t index = (uint32_t) type - XR_XIR_CONSTRUCTED_TYPE_BASE;
    if (index >= earlier) return XR_XIR_BAD_TYPE;
    pending[index] = 1; return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_compile_type_access_scratch(const XrXirCompileContext *compile_context, const XrXirModule *module, uint32_t function, XrXirType type, XirTypeScratch *scratch) {
    XrXirStatus allocation_status = XR_XIR_OK;
    if (!xir_compile_context_valid(compile_context) || !scratch ||
        scratch->resources!=compile_context->resources) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *remaining = &compile_state;
    if (!module || !remaining || function >= module->function_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirTypes *types = module->types;
    if (!types || !types->nominals || !xr_xir_type_node(types, type)) return XR_XIR_OK;
    uint32_t count = (uint32_t) type - XR_XIR_CONSTRUCTED_TYPE_BASE + 1;
    if (!xir_compile_work(remaining, count)) return XR_XIR_BUDGET;

    unsigned char *pending = xir_type_scratch_pending(compile_context, scratch, count, &allocation_status);
    if (!pending) {  return allocation_status; }
    pending[count - 1] = 1;
    XrXirStatus status = XR_XIR_OK;
    for (uint32_t at = count; at && status == XR_XIR_OK; --at) {
        uint32_t i = at - 1;
        if (!pending[i]) continue;
        const XrXirTypeNode *node = &types->nodes[i];
        if (node->kind == XR_XIR_TYPE_NOMINAL) {
            status = xr_xir_compile_nominal_access(remaining, module, function, node->nominal.declaration, 0, XR_XIR_NOMINAL_TYPE);
            for (uint32_t a = 0; a < node->nominal.argument_count && status == XR_XIR_OK; ++a)
                status = type_access_edge(types, node->nominal.arguments[a], i, pending, remaining);
        } else if (node->kind == XR_XIR_TYPE_CALLABLE) {
            status = type_access_edge(types, node->result, i, pending, remaining);
            for (uint32_t p = 0; p < node->parameter_count && status == XR_XIR_OK; ++p)
                status = type_access_edge(types, node->parameters[p].type, i, pending, remaining);
        } else if (node->kind == XR_XIR_TYPE_ARRAY || node->kind == XR_XIR_TYPE_CELL || node->kind == XR_XIR_TYPE_NULLABLE)
            status = type_access_edge(types, node->element, i, pending, remaining);
        else status = XR_XIR_BAD_TYPE;
    }
    return status;
}

XR_FUNC XrXirStatus xr_xir_compile_type_access(const XrXirCompileContext *compile_context,
    const XrXirModule *module, uint32_t function, XrXirType type) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XirTypeScratch scratch={compile_context->resources,NULL,0};
    XrXirStatus status=xr_xir_compile_type_access_scratch(compile_context,module,function,type,&scratch);
    xir_type_scratch_free(&scratch);return status;
}
