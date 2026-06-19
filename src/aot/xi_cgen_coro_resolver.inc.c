/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_coro_resolver.inc.c - AOT seam wiring shared xi_coro analysis to the
 * codegen bundle.
 *
 * The shared IR coroutine analysis (xi_coro_analyze.h) routes its two
 * context-dependent queries through an XiCoroResolver so it never depends on
 * AOT bundle types.  These builders supply the AOT implementations:
 *   - the intra resolver answers only direct module imports and performs no
 *     interprocedural recursion (cg_func_needs_aot_coro: does this body itself
 *     contain a suspension?);
 *   - the ctx resolver adds bundle-aware module-import detection (shared slots)
 *     and cross-module callee resolution (cg_func_needs_aot_coro_ctx: does this
 *     function transitively suspend?).
 */

static const XiFunc *cg_coro_resolve_callee_cb(void *ud, const XiFunc *current,
                                               const XiValue *callee) {
    return cg_resolve_static_function_call((XiCgenCtx *) ud, current, callee).func;
}

static const XiFunc *cg_coro_resolve_method_cb(void *ud, const XiFunc *current,
                                               const XiValue *call) {
    XiCgenCtx *ctx = (XiCgenCtx *) ud;
    if (!ctx || !call || (call->op != XI_CALL_METHOD && call->op != XI_CALL_METHOD_DIRECT))
        return NULL;
    const char *method = (const char *) call->aux;
    bool is_super = call->op == XI_CALL_METHOD && (call->aux_int & 1) != 0;
    if (!is_super && method) {
        CgStaticFunctionCall module_call =
            cg_resolve_module_member_call(ctx, current, call, method);
        if (module_call.func)
            return module_call.func;
    }
    const char *method_prefix = NULL;
    return cg_class_native_resolve_method_call(ctx, current, call, &method_prefix);
}

static bool cg_coro_module_import_intra_cb(void *ud, const XiFunc *f, const XiValue *v,
                                           const char *module) {
    (void) ud;
    return cg_value_is_module_import(f, v, module);
}

static bool cg_coro_module_import_ctx_cb(void *ud, const XiFunc *f, const XiValue *v,
                                         const char *module) {
    return cg_value_is_module_import_ctx((XiCgenCtx *) ud, f, v, module);
}

static XiCoroResolver cg_coro_resolver_intra(void) {
    XiCoroResolver resolver;
    resolver.resolve_callee = NULL;
    resolver.resolve_method = NULL;
    resolver.value_is_module_import = cg_coro_module_import_intra_cb;
    resolver.ud = NULL;
    return resolver;
}

static XiCoroResolver cg_coro_resolver_ctx(XiCgenCtx *ctx) {
    XiCoroResolver resolver;
    resolver.resolve_callee = cg_coro_resolve_callee_cb;
    resolver.resolve_method = cg_coro_resolve_method_cb;
    resolver.value_is_module_import = cg_coro_module_import_ctx_cb;
    resolver.ud = ctx;
    return resolver;
}

static bool cg_func_needs_aot_coro(const XiFunc *f) {
    XiCoroResolver resolver = cg_coro_resolver_intra();
    return xi_coro_func_is_suspendable(f, &resolver);
}

static bool cg_func_needs_aot_coro_ctx(XiCgenCtx *ctx, const XiFunc *f) {
    XiCoroResolver resolver = cg_coro_resolver_ctx(ctx);
    return xi_coro_func_is_suspendable(f, &resolver);
}
