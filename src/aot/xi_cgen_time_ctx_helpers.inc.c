/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_time_ctx_helpers.inc.c - AOT time module recognition with module context
 */

static bool cg_value_is_module_import_ctx(const XiCgenCtx *ctx, const XiFunc *f, const XiValue *v,
                                          const char *module_name) {
    if (cg_value_is_module_import(f, v, module_name))
        return true;
    v = cg_unwrap_identity_value(v);
    if (!ctx || !ctx->module || !ctx->module->init || !v || v->op != XI_GET_SHARED)
        return false;
    return cg_shared_slot_is_module_import(ctx->module->init, (int) v->aux_int, module_name);
}

static bool cg_is_time_sleep_call_ctx(const XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    if (!v || v->op != XI_CALL_METHOD || v->nargs != 2)
        return false;
    const char *method = (const char *) v->aux;
    if (!method || strcmp(method, "sleep") != 0)
        return false;
    return cg_value_is_module_import_ctx(ctx, f, v->args[0], "time");
}

static bool cg_is_time_module_call_ctx(const XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    if (!v || (v->op != XI_CALL_METHOD && v->op != XI_CALL_METHOD_DIRECT) || v->nargs < 1)
        return false;
    return cg_value_is_module_import_ctx(ctx, f, v->args[0], "time");
}

static const char *cg_time_module_helper_ctx(const XiCgenCtx *ctx, const XiFunc *f,
                                             const XiValue *v) {
    if (!cg_is_time_module_call_ctx(ctx, f, v))
        return NULL;
    const char *method = (const char *) v->aux;
    if (!method)
        return NULL;
    uint16_t call_argc = (uint16_t) (v->nargs - 1);
    if (call_argc == 0 && strcmp(method, "now") == 0)
        return "xrt_time_now";
    bool runtime_clock = cg_func_needs_aot_coro_ctx((XiCgenCtx *) ctx, f);
    if (call_argc == 0 && strcmp(method, "monotonic") == 0)
        return runtime_clock ? "xr_aot_time_monotonic" : "xrt_time_monotonic";
    if (call_argc == 0 && strcmp(method, "nanos") == 0)
        return runtime_clock ? "xr_aot_time_nanos" : "xrt_time_nanos";
    if (call_argc == 0 && strcmp(method, "micros") == 0)
        return runtime_clock ? "xr_aot_time_micros" : "xrt_time_micros";
    if (call_argc == 0 && strcmp(method, "clock") == 0)
        return "xrt_time_clock";
    if (call_argc == 0 && strcmp(method, "localOffset") == 0)
        return "xrt_time_local_offset";
    if (call_argc == 1 && strcmp(method, "localOffsetAt") == 0)
        return "xrt_time_local_offset_at";
    return NULL;
}

static bool cg_time_module_helper_has_tagged_arg(const char *helper) {
    return helper && strcmp(helper, "xrt_time_local_offset_at") == 0;
}
