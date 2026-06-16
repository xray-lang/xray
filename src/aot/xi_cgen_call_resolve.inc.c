/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_call_resolve.inc.c - AOT static function call resolution helpers
 */

static CgStaticFunctionCall cg_resolve_import_function_call(XiCgenCtx *ctx,
                                                            const XiImportRef *ref) {
    if (!ctx || !ref)
        return cg_no_static_function_call();

    if (ref->resolved_mod_index >= 0 && ref->resolved_mod_index < ctx->all_nmodules) {
        const XiModule *target_module = ctx->all_modules[ref->resolved_mod_index];
        const char *target_module_name = target_module ? target_module->name : NULL;
        for (int ii = 0; ii < ctx->nimports; ii++) {
            CgImportEntry *imp = &ctx->imports[ii];
            if (!imp->target_func)
                continue;
            if (imp->target_mod_name && target_module_name &&
                strcmp(imp->target_mod_name, target_module_name) == 0 && imp->member_name &&
                ref->member_name && strcmp(imp->member_name, ref->member_name) == 0) {
                return imp->target_class
                           ? cg_static_class_constructor_call(imp->target_func,
                                                              imp->target_mod_name)
                           : cg_static_function_call(imp->target_func, imp->target_mod_name);
            }
        }
    }

    for (int ii = 0; ii < ctx->nimports; ii++) {
        CgImportEntry *imp = &ctx->imports[ii];
        if (!imp->target_func)
            continue;
        if (imp->module_path && ref->module_path &&
            strcmp(imp->module_path, ref->module_path) == 0 && imp->member_name &&
            ref->member_name && strcmp(imp->member_name, ref->member_name) == 0) {
            return imp->target_class
                       ? cg_static_class_constructor_call(imp->target_func, imp->target_mod_name)
                       : cg_static_function_call(imp->target_func, imp->target_mod_name);
        }
    }
    return cg_no_static_function_call();
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
    XiModule *mod = ctx && ctx->module ? ctx->module : (current ? current->module : NULL);
    if (mod && mod->slot_funcs && slot < (int) mod->nslots && mod->slot_funcs[slot])
        return mod->slot_funcs[slot];
    return NULL;
}

static CgStaticFunctionCall cg_resolve_static_function_call(XiCgenCtx *ctx, const XiFunc *current,
                                                            const XiValue *callee) {
    if (!callee)
        return cg_no_static_function_call();

    if ((callee->op == XI_BOX || callee->op == XI_UNBOX || callee->op == XI_COPY ||
         callee->op == XI_MOVE) &&
        callee->nargs >= 1) {
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
        if (slot >= 0 && slot < ctx->nshared && ctx->shared_funcs[slot])
            return cg_static_function_call(ctx->shared_funcs[slot], NULL);
        const XiFunc *local = cg_resolve_local_shared_function(ctx, current, slot);
        if (local)
            return cg_static_function_call(local, cg_module_prefix_for_func(ctx, local));
        const XiFunc *module_init = ctx->module ? ctx->module->init : current;
        const XiImportRef *ref = cg_shared_slot_import_ref(module_init, slot);
        CgStaticFunctionCall imported = cg_resolve_import_function_call(ctx, ref);
        if (imported.func)
            return imported;
    }

    if (callee->op == XI_IMPORT_REF && callee->aux)
        return cg_resolve_import_function_call(ctx, (const XiImportRef *) callee->aux);

    return cg_no_static_function_call();
}
