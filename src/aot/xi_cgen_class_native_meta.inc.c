/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_class_native_meta.inc.c - AOT native class receiver metadata
 */

typedef struct {
    const XiClassData *class_data;
    const XiFunc *func;
    const XrStructLayout *layout;
    const char *class_name;
    bool is_constructor;
} CgClassNativeFunc;

static CgClassNativeFunc cg_class_native_no_func(void) {
    CgClassNativeFunc info;
    memset(&info, 0, sizeof(info));
    return info;
}

static CgClassNativeFunc cg_class_native_func(const XiCgenCtx *ctx, const XiFunc *f) {
    XaotClassNativeFunc plan = xaot_class_native_func(cg_ctx_aot_bundle(ctx), f);
    CgClassNativeFunc info = cg_class_native_no_func();
    if (!plan.layout)
        return info;
    info.class_data = plan.class_data;
    info.func = plan.func;
    info.layout = plan.layout;
    info.class_name = plan.class_name;
    info.is_constructor = plan.is_constructor;
    return info;
}

static bool cg_class_func_uses_native_receiver(const XiCgenCtx *ctx, const XiFunc *f) {
    CgClassNativeFunc info = cg_class_native_func(ctx, f);
    return info.layout != NULL;
}

static bool cg_class_func_is_native_constructor(const XiCgenCtx *ctx, const XiFunc *f) {
    CgClassNativeFunc info = cg_class_native_func(ctx, f);
    return info.layout != NULL && info.is_constructor;
}

static const XiClassData *cg_class_native_data_by_name(const XiCgenCtx *ctx, const char *name) {
    if (!ctx || !name)
        return NULL;
    if (ctx->module) {
        for (uint16_t i = 0; i < ctx->module->nclasses; i++) {
            const XiClassData *cd = ctx->module->classes[i];
            if (cd && cd->class_name && strcmp(cd->class_name, name) == 0)
                return cd;
        }
    }
    for (int i = 0; i < ctx->nimports; i++) {
        const XiClassData *cd = ctx->imports[i].target_class;
        if (cd && cd->class_name && strcmp(cd->class_name, name) == 0)
            return cd;
    }
    return NULL;
}

static void emit_class_native_type_name(FILE *out, const char *prefix, const char *class_name) {
    char prefix_buf[128];
    char class_buf[128];
    sanitize_c_ident_part(prefix_buf, sizeof(prefix_buf), prefix ? prefix : "mod");
    sanitize_c_ident_part(class_buf, sizeof(class_buf), class_name ? class_name : "Class");
    fprintf(out, "xrt_native_%s_%s", prefix_buf, class_buf);
}

static bool cg_class_native_data_matches(const XiClassData *a, const XiClassData *b) {
    if (!a || !b)
        return false;
    if (a == b)
        return true;
    return a->class_name && b->class_name && strcmp(a->class_name, b->class_name) == 0;
}

static int cg_class_native_slot_in_module(const XiModule *module, const XiClassData *cd) {
    if (!module || !cd || !module->slot_classes)
        return -1;
    for (uint16_t s = 0; s < module->nslots; s++) {
        if (cg_class_native_data_matches(module->slot_classes[s], cd))
            return (int) s;
    }
    return -1;
}

static const XiModule *cg_class_native_module_for_data(const XiCgenCtx *ctx,
                                                       const XiClassData *cd) {
    if (!ctx || !cd)
        return NULL;
    if (ctx->module && cg_class_native_slot_in_module(ctx->module, cd) >= 0)
        return ctx->module;
    for (int i = 0; i < ctx->all_nmodules; i++) {
        const XiModule *module = ctx->all_modules ? ctx->all_modules[i] : NULL;
        if (module && cg_class_native_slot_in_module(module, cd) >= 0)
            return module;
    }
    return NULL;
}

static const char *cg_class_native_prefix_for_data(const XiCgenCtx *ctx, const XiClassData *cd,
                                                   const char *fallback) {
    const XiModule *module = cg_class_native_module_for_data(ctx, cd);
    if (module && module->name)
        return module->name;
    for (int i = 0; ctx && i < ctx->nimports; i++) {
        const CgImportEntry *imp = &ctx->imports[i];
        if (cg_class_native_data_matches(imp->target_class, cd) && imp->target_mod_name)
            return imp->target_mod_name;
    }
    return fallback;
}

static const XiClassData *cg_class_native_data_for_abi_type(const XiCgenCtx *ctx,
                                                            const XrType *type) {
    if (!type || (type->kind != XR_KIND_CLASS && type->kind != XR_KIND_INSTANCE) ||
        !type->instance.class_name)
        return NULL;
    const XiClassData *cd = cg_class_native_data_by_name(ctx, type->instance.class_name);
    if (!cd || !cd->instance_layout || !cg_class_native_module_for_data(ctx, cd))
        return NULL;
    return cd;
}

static bool emit_class_native_abi_type_name(const XiCgenCtx *ctx, FILE *out, const char *prefix,
                                            const XrType *type) {
    const XiClassData *cd = cg_class_native_data_for_abi_type(ctx, type);
    if (!cd)
        return false;
    emit_class_native_type_name(out, cg_class_native_prefix_for_data(ctx, cd, prefix),
                                cd->class_name);
    return true;
}

static bool emit_class_native_type_id_expr(XiCgenCtx *ctx, FILE *out, const XiClassData *cd) {
    const XiModule *module = cg_class_native_module_for_data(ctx, cd);
    if (!module)
        return false;
    int slot = cg_class_native_slot_in_module(module, cd);
    if (slot < 0)
        return false;
    if (module == ctx->module) {
        fprintf(out, "%s[%d].i", ctx && ctx->shared_name ? ctx->shared_name : "xrt_shared", slot);
    } else {
        fprintf(out, "xrt_shared_%s[%d].i", module->name ? module->name : "mod", slot);
    }
    return true;
}
