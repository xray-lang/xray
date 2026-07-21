/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_call_resolve.inc.c - AOT static function call resolution helpers
 */

static bool cg_import_entry_matches_ref(const XiCgenCtx *ctx, const CgImportEntry *imp,
                                        const XiImportRef *ref, const char *member_name) {
    if (!ctx || !imp || !ref || !member_name || !imp->member_name)
        return false;
    if (ctx->all_modules && ref->resolved_mod_index >= 0 &&
        ref->resolved_mod_index < ctx->all_nmodules) {
        const XiModule *target_module = ctx->all_modules[ref->resolved_mod_index];
        const char *target_module_name = target_module ? target_module->name : NULL;
        if (imp->target_mod_name && target_module_name &&
            strcmp(imp->target_mod_name, target_module_name) == 0) {
            if (ref->resolved_shared_slot >= 0)
                return imp->shared_slot == ref->resolved_shared_slot;
            return strcmp(imp->member_name, member_name) == 0;
        }
    }
    return strcmp(imp->member_name, member_name) == 0 && imp->module_path && ref->module_path &&
           strcmp(imp->module_path, ref->module_path) == 0;
}

static CgStaticFunctionCall cg_import_entry_static_call(const CgImportEntry *imp) {
    if (!imp)
        return cg_no_static_function_call();
    const XiFunc *target = imp->target_func;
    if (!target && imp->target_class && imp->exporter_func)
        target = cg_find_constructor(imp->exporter_func, imp->target_class);
    if (!target && !imp->target_class)
        return cg_no_static_function_call();
    return imp->target_class ? cg_static_class_constructor_data_call(target, imp->target_mod_name,
                                                                     imp->target_class)
                             : cg_static_function_call(target, imp->target_mod_name);
}

static CgStaticFunctionCall cg_module_export_static_call(const XiModule *mod,
                                                         const char *member_name) {
    if (!mod || !member_name)
        return cg_no_static_function_call();
    for (uint16_t ei = 0; ei < mod->nexports; ei++) {
        const XiModuleExport *exp = &mod->exports[ei];
        if (!exp->name || strcmp(exp->name, member_name) != 0)
            continue;
        const XiFunc *target = exp->function;
        const XiClassData *target_class = exp->class_data;
        if (!target && target_class && mod->init)
            target = cg_find_constructor(mod->init, target_class);
        if (!target && !target_class)
            return cg_no_static_function_call();
        return target_class ? cg_static_class_constructor_data_call(target, mod->name, target_class)
                            : cg_static_function_call(target, mod->name);
    }
    return cg_no_static_function_call();
}

static CgStaticFunctionCall cg_resolve_module_export_static_call(XiCgenCtx *ctx,
                                                                 const XiImportRef *module_ref,
                                                                 const char *member_name) {
    if (!ctx || !module_ref || !member_name)
        return cg_no_static_function_call();
    if (module_ref->resolved_mod_index >= 0 && module_ref->resolved_mod_index < ctx->all_nmodules) {
        CgStaticFunctionCall call = cg_module_export_static_call(
            ctx->all_modules[module_ref->resolved_mod_index], member_name);
        if (call.func || call.is_class_constructor)
            return call;
    }
    if (!module_ref->module_path)
        return cg_no_static_function_call();
    for (int mi = 0; mi < ctx->all_nmodules; mi++) {
        const XiModule *mod = ctx->all_modules[mi];
        if (!mod)
            continue;
        if ((mod->path && strcmp(mod->path, module_ref->module_path) == 0) ||
            (mod->name && strcmp(mod->name, module_ref->module_path) == 0)) {
            CgStaticFunctionCall call = cg_module_export_static_call(mod, member_name);
            if (call.func || call.is_class_constructor)
                return call;
        }
    }
    return cg_no_static_function_call();
}

static CgStaticFunctionCall cg_resolve_import_function_call(XiCgenCtx *ctx,
                                                            const XiImportRef *ref) {
    if (!ctx || !ref || !ref->member_name)
        return cg_no_static_function_call();

    for (int ii = 0; ii < ctx->nimports; ii++) {
        CgImportEntry *imp = &ctx->imports[ii];
        if (cg_import_entry_matches_ref(ctx, imp, ref, ref->member_name))
            return cg_import_entry_static_call(imp);
    }
    return cg_resolve_module_export_static_call(ctx, ref, ref->member_name);
}

static const XiImportRef *cg_import_ref_for_value(XiCgenCtx *ctx, const XiFunc *f,
                                                  const XiValue *value) {
    const XiValue *v = cg_unwrap_identity_value(value);
    const XiImportRef *ref = cg_value_import_ref(v);
    if (ref)
        return ref;
    if (!v || v->op != XI_GET_SHARED)
        return NULL;
    int slot = (int) v->aux_int;
    ref = cg_shared_slot_import_ref(f, slot);
    if (!ref && f && f->module && f->module->init != f)
        ref = cg_shared_slot_import_ref(f->module->init, slot);
    if (!ref && (!f || !f->module) && ctx && ctx->module && ctx->module->init &&
        ctx->module->init != f)
        ref = cg_shared_slot_import_ref(ctx->module->init, slot);
    return ref;
}

static const XiImportRef *cg_module_import_ref_for_value(XiCgenCtx *ctx, const XiFunc *f,
                                                         const XiValue *value) {
    const XiImportRef *ref = cg_import_ref_for_value(ctx, f, value);
    return ref && !ref->member_name ? ref : NULL;
}

static const XiEnumData *cg_resolve_imported_enum_value(XiCgenCtx *ctx, const XiFunc *f,
                                                        const XiValue *value) {
    const XiImportRef *ref = cg_import_ref_for_value(ctx, f, value);
    if (!ctx || !ref || !ref->member_name)
        return NULL;

    if (ref->resolved_mod_index >= 0 && ref->resolved_mod_index < ctx->all_nmodules &&
        ref->resolved_shared_slot >= 0) {
        const XiModule *mod = ctx->all_modules[ref->resolved_mod_index];
        if (mod && mod->slot_enums && ref->resolved_shared_slot < (int) mod->nslots)
            return mod->slot_enums[ref->resolved_shared_slot];
    }

    for (int ii = 0; ii < ctx->nimports; ii++) {
        CgImportEntry *imp = &ctx->imports[ii];
        if (imp->target_enum && cg_import_entry_matches_ref(ctx, imp, ref, ref->member_name))
            return imp->target_enum;
    }
    return NULL;
}

static const XaBuiltinEnum *cg_resolve_imported_builtin_enum_value(XiCgenCtx *ctx, const XiFunc *f,
                                                                   const XiValue *value) {
    const XiImportRef *ref = cg_import_ref_for_value(ctx, f, value);
    if (!ref || !ref->module_path || !ref->member_name)
        return NULL;
    return xa_builtin_get_enum_type(ref->module_path, ref->member_name);
}

static CgStaticFunctionCall cg_resolve_module_member_call(XiCgenCtx *ctx, const XiFunc *f,
                                                          const XiValue *call,
                                                          const char *member_name) {
    if (!ctx || !call || call->nargs < 1 || !member_name)
        return cg_no_static_function_call();
    const XiImportRef *module_ref = cg_module_import_ref_for_value(ctx, f, call->args[0]);
    if (!module_ref)
        return cg_no_static_function_call();
    for (int ii = 0; ii < ctx->nimports; ii++) {
        CgImportEntry *imp = &ctx->imports[ii];
        if (cg_import_entry_matches_ref(ctx, imp, module_ref, member_name))
            return cg_import_entry_static_call(imp);
    }
    return cg_resolve_module_export_static_call(ctx, module_ref, member_name);
}

static const XiFunc *cg_resolve_local_shared_function(XiCgenCtx *ctx, const XiFunc *current,
                                                      int slot) {
    if (slot < 0)
        return NULL;
    for (const XiFunc *f = current; f; f = f->parent_func) {
        if (f->shared_slot_funcs && slot < (int) f->shared_slot_func_count &&
            f->shared_slot_funcs[slot])
            return f->shared_slot_funcs[slot];
    }
    XiModule *mod = current && current->module ? current->module : (ctx ? ctx->module : NULL);
    if (mod && mod->slot_funcs && slot < (int) mod->nslots && mod->slot_funcs[slot])
        return mod->slot_funcs[slot];
    return NULL;
}

static CgStaticFunctionCall cg_resolve_static_function_call(XiCgenCtx *ctx, const XiFunc *current,
                                                            const XiValue *callee) {
    if (!callee)
        return cg_no_static_function_call();

    if ((callee->op == XI_BOX || callee->op == XI_UNBOX || xi_copy_is_identity_alias(callee) ||
         callee->op == XI_MOVE) &&
        callee->nargs >= 1 && callee->args && callee->args[0]) {
        CgStaticFunctionCall inner = cg_resolve_static_function_call(ctx, current, callee->args[0]);
        if (inner.func)
            return inner;
    }

    if (callee->op == XI_CLOSURE_NEW && callee->aux) {
        const XiFunc *target = (const XiFunc *) callee->aux;
        return cg_static_function_call(target, cg_module_prefix_for_func(ctx, target));
    }

    if (callee->op == XI_STACK_ALLOC && callee->aux_int == XI_CLOSURE_NEW && callee->aux) {
        const XiFunc *target = (const XiFunc *) callee->aux;
        return cg_static_function_call(target, cg_module_prefix_for_func(ctx, target));
    }

    if (callee->op == XI_CONST && callee->type && callee->type->kind == XR_KIND_NULL && current &&
        current->name) {
        return cg_static_function_call(current, NULL);
    }

    if (callee->op == XI_GET_SHARED && ctx) {
        int slot = (int) callee->aux_int;
        const XiFunc *local = cg_resolve_local_shared_function(ctx, current, slot);
        if (local)
            return cg_static_function_call(local, cg_module_prefix_for_func(ctx, local));
        if ((!current || current->module == ctx->module) && slot >= 0 && slot < ctx->nshared &&
            ctx->shared_funcs && ctx->shared_funcs[slot])
            return cg_static_function_call(ctx->shared_funcs[slot], NULL);
        const XiFunc *module_init = current && current->module ? current->module->init
                                    : ctx->module              ? ctx->module->init
                                                               : current;
        const XiImportRef *ref = cg_shared_slot_import_ref(module_init, slot);
        CgStaticFunctionCall imported = cg_resolve_import_function_call(ctx, ref);
        if (imported.func || imported.is_class_constructor)
            return imported;
    }

    if (callee->op == XI_IMPORT_REF && callee->aux)
        return cg_resolve_import_function_call(ctx, (const XiImportRef *) callee->aux);

    return cg_no_static_function_call();
}
