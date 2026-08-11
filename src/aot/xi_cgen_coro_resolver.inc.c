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
 * The shared IR coroutine analysis (xi_coro_analyze.h) routes its
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

static bool cg_coro_module_member_call_ctx_cb(void *ud, const XiFunc *f, const XiValue *v,
                                              const char *module, const char *member) {
    XiCgenCtx *ctx = (XiCgenCtx *) ud;
    if (!ctx || !f || !v || !module || !member || v->nargs < 1)
        return false;
    if ((v->op == XI_CALL_METHOD || v->op == XI_CALL_METHOD_DIRECT) && v->aux &&
        strcmp((const char *) v->aux, member) == 0)
        return cg_value_is_module_import_ctx(ctx, f, v->args[0], module);
    if (v->op != XI_CALL)
        return false;

    const XiValue *callee = cg_unwrap_identity_value(v->args[0]);
    const XiImportRef *ref = (callee && callee->op == XI_IMPORT_REF && callee->aux)
                                 ? (const XiImportRef *) callee->aux
                                 : cg_import_ref_for_value(ctx, f, callee);
    return ref && ref->module_path && ref->member_name && strcmp(ref->module_path, module) == 0 &&
           strcmp(ref->member_name, member) == 0;
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
    resolver.call_suspendability = NULL;
    resolver.value_is_module_import = cg_coro_module_import_ctx_cb;
    resolver.call_is_module_member = cg_coro_module_member_call_ctx_cb;
    resolver.ud = ctx;
    return resolver;
}

static bool cg_coro_target_proves_conservative_sync(XiCgenCtx *ctx,
                                                     const XiCoroPlan *plan) {
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    const uint32_t required_evidence = XAOT_CALLABLE_EV_CLOSED_TARGET_SET |
                                       XAOT_CALLABLE_EV_SIGNATURE |
                                       XAOT_CALLABLE_EV_TARGET_EFFECTS |
                                       XAOT_CALLABLE_EV_XI_FLOW;
    if (!bundle || !plan || plan->nstates == 0)
        return false;
    for (uint32_t point_index = 0; point_index < plan->nstates; point_index++) {
        const XiCoroSuspendPoint *point = &plan->points[point_index];
        const XaotCallableInvokePlan *callable =
            point->op ? xaot_bundle_find_callable_invoke_plan(bundle, point->op) : NULL;
        if (!point->op || point->op->op != XI_CALL || !callable ||
            callable->target_count == 0 || callable->unproven_reason != XAOT_CALLABLE_PROVEN ||
            (callable->evidence & required_evidence) != required_evidence ||
            (callable->action != XAOT_CALLABLE_DIRECT_SYNC &&
             callable->action != XAOT_CALLABLE_TARGET_SWITCH))
            return false;
        for (uint16_t target_index = 0; target_index < callable->target_count; target_index++) {
            const XaotCallableTargetCase *target =
                xaot_bundle_callable_target_case(bundle, callable, target_index);
            if (!target || !target->target_func ||
                (target->effect_bits & XG_BODY_MAY_SUSPEND) != 0)
                return false;
        }
    }
    return true;
}

static bool cg_func_needs_aot_coro_ctx(XiCgenCtx *ctx, const XiFunc *f) {
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    const XaotFuncPlan *func_plan = xaot_bundle_find_func_plan(bundle, f);
    const XiCoroPlan *plan = f ? f->coro_plan : NULL;
    bool planned;

    if (!plan || !xi_coro_plan_is_current(f, plan)) {
        if (ctx) {
            ctx->error = true;
            fprintf(stderr,
                    "[xi_cgen] ERROR: missing or stale frozen coroutine plan for '%s' "
                    "(func=%p plan=%p stage=%s revision=%llu/%llu lowered=%llu/%llu)\n",
                    f && f->name ? f->name : "?", (const void *) f, (const void *) plan,
                    f ? xi_stage_name(f->stage) : "?",
                    (unsigned long long) (f ? f->ir_revision : 0),
                    (unsigned long long) (f ? f->cfg_version : 0),
                    (unsigned long long) (plan ? plan->lowered_ir_revision : 0),
                    (unsigned long long) (plan ? plan->lowered_cfg_revision : 0));
        }
        return false;
    }
    planned = plan->is_coroutine;
    /* A target plan may refine a fail-closed indirect Xi point to a non-empty
     * all-sync callable target set.  The shared state is deliberately kept so
     * code generation remains plan-driven and target-neutral.  Every other
     * disagreement, especially a target suspension missed by Xi, is fatal. */
    bool target_suspends = func_plan && func_plan->may_suspend != 0;
    bool safe_sync_refinement =
        func_plan && planned && !target_suspends &&
        cg_coro_target_proves_conservative_sync(ctx, plan);
    if (func_plan && planned != target_suspends && !safe_sync_refinement) {
        if (ctx) {
            ctx->error = true;
            fprintf(stderr,
                    "[xi_cgen] ERROR: coroutine plan/effect mismatch for '%s' "
                    "(plan=%u effect=%u states=%u first-op=%s first-callsite=%u)\n",
                    f && f->name ? f->name : "?", planned ? 1u : 0u,
                    func_plan->may_suspend ? 1u : 0u, plan->nstates,
                    plan->nstates > 0 && plan->points[0].op
                        ? xi_op_name(plan->points[0].op->op)
                        : "<none>",
                    plan->nstates > 0 && plan->points[0].op
                        ? plan->points[0].op->xg_callsite_id
                        : 0);
        }
    }
    return planned;
}
