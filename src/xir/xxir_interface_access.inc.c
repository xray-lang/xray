/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_interface_access.inc.c - Declaration-owned interface naming authority
 */
#include "xxir_interface.h"

static XrXirStatus interface_access_owner(const XrXirDeclarations *declarations,
    XrXirLiteral name, XrXirBudget *remaining, uint32_t *owner) {
    for (uint32_t m = 0; m < declarations->module_count; ++m) {
        const XrXirSourceModule *candidate = &declarations->modules[m];
        uint64_t work = candidate->name_length == name.length ? (uint64_t)name.length + 1 : 1;
        if (!spend(&remaining->work, work)) return XR_XIR_BUDGET;
        if (candidate->name_length == name.length && !memcmp(candidate->name, name.bytes, name.length)) {
            *owner = m; return XR_XIR_OK;
        }
    }
    return XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus interface_parent_access(const XrXirModule *module,
    uint32_t owner, const XrXirInterfaceApplication *application, XrXirBudget *remaining) {
    const XrXirDeclarations *scope = module->declarations;
    const XrXirInterfaceDeclaration *parent = &module->types->interfaces->declarations[application->declaration];
    uint32_t parent_owner = 0;
    XrXirStatus status = interface_access_owner(scope, parent->module, remaining, &parent_owner);
    if (status != XR_XIR_OK) return status;
    if (owner != parent_owner) {
        if (!parent->exported) return XR_XIR_BAD_TYPE;
        if (!spend(&remaining->work, scope->modules[owner].dependency_count)) return XR_XIR_BUDGET;
        if (!xr_xir_module_imports(scope, owner, parent_owner)) return XR_XIR_BAD_TYPE;
    }
    for (uint32_t a = 0; a < application->argument_count; ++a) {
        if (!spend(&remaining->work, 1)) return XR_XIR_BUDGET;
        status = xr_xir_type_access(module, scope->modules[owner].initializer, application->arguments[a], remaining);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus interface_constraint_access(const XrXirModule *module, uint32_t owner,
    const XrXirConstraint *constraints, uint32_t count, XrXirBudget *remaining) {
    for (uint32_t p = 0; p < count; ++p)
        for (uint32_t a = 0; a < constraints[p].interface_count; ++a) {
            if (!spend(&remaining->work, 1)) return XR_XIR_BUDGET;
            XrXirStatus status = interface_parent_access(module, owner, &constraints[p].interfaces[a], remaining);
            if (status != XR_XIR_OK) return status;
        }
    return XR_XIR_OK;
}
/* Descriptor and module structure must be verified before naming authority. */
static XrXirStatus verify_interface_access(const XrXirModule *module, XrXirBudget *remaining) {
    const XrXirInterfaceTable *table = module->types ? module->types->interfaces : NULL;
    if (!table) return XR_XIR_OK;
    const XrXirDeclarations *scope = module->declarations;
    if (!scope) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t d = 0; d < table->count; ++d) {
        const XrXirInterfaceDeclaration *declaration = &table->declarations[d];
        uint32_t owner = 0;
        XrXirStatus status = interface_access_owner(scope, declaration->module, remaining, &owner);
        if (status != XR_XIR_OK) return status;
        status = interface_constraint_access(module, owner, declaration->constraints, declaration->parameter_count, remaining);
        if (status != XR_XIR_OK) return status;
        for (uint32_t p = 0; p < declaration->parent_count; ++p) {
            if (!spend(&remaining->work, 1)) return XR_XIR_BUDGET;
            status = interface_parent_access(module, owner, &declaration->parents[p], remaining);
            if (status != XR_XIR_OK) return status;
        }
        for (uint32_t m = 0; m < declaration->method_count; ++m) {
            if (!spend(&remaining->work, 1)) return XR_XIR_BUDGET;
            status = xr_xir_type_access(module, scope->modules[owner].initializer,
                declaration->methods[m].signature, remaining);
            if (status != XR_XIR_OK) return status;
        }
    }
    const XrXirNominalTable *nominals = module->types->nominals;
    for (uint32_t n = 0; nominals && nominals->declarations && n < nominals->count; ++n) {
        const XrXirNominalDeclaration *declaration = &nominals->declarations[n];
        uint32_t owner = 0;
        XrXirStatus status = interface_access_owner(scope, declaration->module, remaining, &owner);
        if (status == XR_XIR_OK) status = interface_constraint_access(module, owner,
            declaration->constraints, declaration->parameter_count, remaining);
        if (status != XR_XIR_OK) return status;
    }
    for (uint32_t f = 0; module->generics && f < module->function_count; ++f) {
        const XrXirGeneric *generic = &module->generics[f];
        XrXirStatus status = interface_constraint_access(module, scope->functions[f].module,
            generic->constraints, generic->parameter_count, remaining);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
