/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_nominal_access.inc.c - Declaration-bound field and construction authority
 */
static bool nominal_access_work(uint64_t *work, uint64_t amount) {
    if (!work || amount > *work) return false;
    *work -= amount; return true;
}
XR_FUNC XrXirStatus xr_xir_nominal_access(const XrXirModule *module, uint32_t function,
    uint32_t declaration, uint32_t field, XrXirNominalAccess access, uint64_t *work) {
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
        identity = (XrXirNominalIdentity) {d->module, d->name, d->exported, d->parameter_count, NULL, d->field_count};
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
