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

static CgClassNativeFunc cg_class_native_method_func(const XiCgenCtx *ctx, const XiFunc *f) {
    if (!ctx || !f)
        return cg_class_native_no_func();
    for (int i = 0; i < ctx->nmethod; i++) {
        const CgMethodEntry *entry = &ctx->methods[i];
        if (entry->func != f || !entry->instance_layout)
            continue;
        CgClassNativeFunc info = cg_class_native_no_func();
        info.class_data = entry->class_data;
        info.func = f;
        info.layout = entry->instance_layout;
        info.class_name = entry->class_name;
        info.is_constructor = false;
        return info;
    }
    return cg_class_native_no_func();
}

static CgClassNativeFunc cg_class_native_constructor_func(const XiCgenCtx *ctx, const XiFunc *f) {
    if (!ctx || !ctx->module || !f)
        return cg_class_native_no_func();
    for (uint16_t i = 0; i < ctx->module->nclasses; i++) {
        const XiClassData *cd = ctx->module->classes[i];
        if (!cd || !cd->instance_layout)
            continue;
        const XiFunc *ctor = cg_find_constructor(ctx->module->init, cd);
        if (ctor != f)
            continue;
        CgClassNativeFunc info = cg_class_native_no_func();
        info.class_data = cd;
        info.func = f;
        info.layout = cd->instance_layout;
        info.class_name = cd->class_name;
        info.is_constructor = true;
        return info;
    }
    for (int i = 0; i < ctx->nimports; i++) {
        const CgImportEntry *imp = &ctx->imports[i];
        const XiClassData *cd = imp->target_class;
        if (!cd || !cd->instance_layout || imp->target_func != f)
            continue;
        CgClassNativeFunc info = cg_class_native_no_func();
        info.class_data = cd;
        info.func = f;
        info.layout = cd->instance_layout;
        info.class_name = cd->class_name;
        info.is_constructor = true;
        return info;
    }
    return cg_class_native_no_func();
}

static CgClassNativeFunc cg_class_native_func(const XiCgenCtx *ctx, const XiFunc *f) {
    CgClassNativeFunc method = cg_class_native_method_func(ctx, f);
    if (method.layout)
        return method;
    return cg_class_native_constructor_func(ctx, f);
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
