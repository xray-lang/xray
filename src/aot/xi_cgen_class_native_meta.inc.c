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
