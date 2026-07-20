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
    const XaotBundle *bundle;
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
    bundle = cg_ctx_aot_bundle(ctx);
    if (bundle) {
        const XaotMethodDispatchPlan *plan =
            xaot_bundle_find_method_dispatch_plan_for_xi_call(bundle, call);
        if (plan && plan->target_count == 1 && plan->target_start != 0 &&
            plan->target_start - 1 < bundle->ndispatch_target_cases) {
            const XaotDispatchTargetCase *target =
                &bundle->dispatch_target_cases[plan->target_start - 1];
            const XiFunc *func = xaot_bundle_find_dispatch_target_func(bundle, target, NULL);
            if (func)
                return func;
        }
    }
    const char *method_prefix = NULL;
    return cg_class_native_resolve_method_call(ctx, current, call, &method_prefix);
}

static bool cg_coro_module_import_ctx_cb(void *ud, const XiFunc *f, const XiValue *v,
                                         const char *module) {
    return cg_value_is_module_import_ctx((XiCgenCtx *) ud, f, v, module);
}

static int cg_coro_func_suspendability_cb(void *ud, const XiFunc *func) {
    XiCgenCtx *ctx = (XiCgenCtx *) ud;
    const XaotFuncPlan *plan = xaot_bundle_find_func_plan(cg_ctx_aot_bundle(ctx), func);
    return plan ? (plan->may_suspend != 0 ? 1 : 0) : -1;
}

static XiCoroResolver cg_coro_resolver_ctx(XiCgenCtx *ctx) {
    XiCoroResolver resolver;
    resolver.resolve_callee = cg_coro_resolve_callee_cb;
    resolver.resolve_method = cg_coro_resolve_method_cb;
    resolver.func_suspendability = cg_coro_func_suspendability_cb;
    resolver.value_is_module_import = cg_coro_module_import_ctx_cb;
    resolver.ud = ctx;
    return resolver;
}

static bool cg_func_needs_aot_coro_ctx(XiCgenCtx *ctx, const XiFunc *f) {
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    const XaotFuncPlan *func_plan = xaot_bundle_find_func_plan(bundle, f);
    XiCoroResolver resolver = cg_coro_resolver_ctx(ctx);
    bool is_module_init = false;
    /* Prepared whole-program effects are the canonical answer.  In
     * particular, this keeps a callee's defining translation unit consistent
     * with cross-module callers whose target flow made it transitively
     * suspendable. */
    if (func_plan)
        return func_plan->may_suspend != 0;
    if (xi_coro_func_is_suspendable(f, &resolver))
        return true;
    if (bundle && f) {
        for (uint32_t i = 0; i < bundle->nmodules; i++) {
            if (bundle->modules[i] && bundle->modules[i]->init == f) {
                is_module_init = true;
                break;
            }
        }
    }
    if (is_module_init && bundle->global_evidence_plan.evidence && f->xg_body_func_id != XG_NO_ID) {
        const XgGlobalEvidence *evidence = bundle->global_evidence_plan.evidence;
        for (uint32_t i = 0; i < evidence->nbodies; i++) {
            const XgBodySummary *body = &evidence->bodies[i];
            uint32_t effects = body->effect_bits;
            if (body->func_id != f->xg_body_func_id)
                continue;
            if (xg_body_effects_compose_closed_world_calls(evidence, body, &effects))
                return (effects & XG_BODY_MAY_SUSPEND) != 0;
            if ((body->effect_bits & XG_BODY_MAY_CALL) == 0)
                return (body->effect_bits & XG_BODY_MAY_SUSPEND) != 0;
            break;
        }
    }
    return false;
}
