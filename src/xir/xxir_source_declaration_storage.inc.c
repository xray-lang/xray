/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_declaration_storage.inc.c - One owned declaration inventory
 */
static bool source_declaration_storage(SourceContext *ctx,uint32_t nominals,uint32_t interfaces) {
    uint32_t count=ctx->module_count,capacity=ctx->function_capacity,slots=ctx->slot_count;
    if(ctx->repl_tail) {
        if(slots==UINT32_MAX)return source_fail(ctx,ctx->repl_tail,XR_XIR_BUDGET,"implicit result slot capacity exhausted");
        ++slots;
    }
    if(ctx->function_value_capacity>UINT32_MAX-slots)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"function value slot capacity exhausted");
    slots+=ctx->function_value_capacity;
    ctx->slot_capacity=slots;
    ctx->functions = source_alloc(ctx, capacity, sizeof(*ctx->functions));
    ctx->bodies = source_alloc(ctx, capacity, sizeof(*ctx->bodies));
    ctx->identities = source_alloc(ctx, capacity, sizeof(*ctx->identities));
    ctx->generics = source_alloc(ctx, capacity, sizeof(*ctx->generics));
    ctx->modules = source_alloc(ctx, count, sizeof(*ctx->modules));
    ctx->names = source_alloc(ctx, count, sizeof(*ctx->names));
    ctx->slots = source_alloc(ctx, slots, sizeof(*ctx->slots));
    ctx->nominals.declarations = source_alloc(ctx, nominals, sizeof(*ctx->nominals.declarations));
    ctx->nominal_members = source_alloc(ctx, nominals, sizeof(*ctx->nominal_members));
    ctx->nominal_variants = source_alloc(ctx, nominals, sizeof(*ctx->nominal_variants));
    ctx->nominal_defaults = source_alloc(ctx, nominals, sizeof(*ctx->nominal_defaults));
    ctx->nominal_sources = source_alloc(ctx, nominals, sizeof(*ctx->nominal_sources));
    ctx->nominal_methods = source_alloc(ctx, nominals, sizeof(*ctx->nominal_methods));
    ctx->nominal_defaultable = source_alloc(ctx, nominals, sizeof(*ctx->nominal_defaultable));
    ctx->nominal_constructors = source_alloc(ctx, nominals, sizeof(*ctx->nominal_constructors));
    if (nominals) ctx->types.nominals = &ctx->nominals;
    ctx->interfaces.declarations = interfaces ? source_alloc(ctx,interfaces,sizeof(*ctx->interfaces.declarations)) : NULL;
    ctx->interface_sources = interfaces ? source_alloc(ctx,interfaces,sizeof(*ctx->interface_sources)) : NULL;
    ctx->interface_members = interfaces ? source_alloc(ctx,interfaces,sizeof(*ctx->interface_members)) : NULL;
    ctx->interface_member_declarations = interfaces ? source_alloc(ctx,interfaces,sizeof(*ctx->interface_member_declarations)) : NULL;
    if (interfaces) ctx->types.interfaces = &ctx->interfaces;
    if (ctx->diagnostic.status != XR_XIR_OK) return false;
    return true;
}

#include "xxir_source_module_identity.inc.c"
static bool source_declaration_modules(SourceContext *ctx) {
    uint32_t count=ctx->module_count;
    for (uint32_t m = 0; m < count; ++m) {
        XrModuleSpec *spec = &ctx->graph->specs[m];
        uint32_t *deps = source_alloc(ctx, (size_t) spec->dep_count, sizeof(*deps));
        if (!deps) return false;
        for (int i = 0; i < spec->dep_count; ++i) deps[i] = (uint32_t) spec->dep_indices[i];
        const char *name=source_declaration_module_name(ctx,spec);
        if(!name)return false;
        size_t length=source_text_size(ctx,name);
        if(length>UINT32_MAX)return source_fail(ctx,NULL,XR_XIR_BUDGET,"module declaration name size exhausted");
        ctx->modules[m] = (XrXirSourceModule) {name, (uint32_t)length, deps, (uint32_t) spec->dep_count, m, 0,XR_XIR_MODULE_FRESH_STORAGE};
        ctx->bodies[m].module = m;
        ctx->functions[m] = (XrXirFunction) {"$init", 5, NULL, 0, XR_XIR_UNIT, NULL, 0, NULL, 0, NULL, 0};
        ctx->identities[m].module = m;
    }
    return true;
}
