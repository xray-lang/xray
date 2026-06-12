/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_abi_helpers.inc.c - AOT function ABI and scalar boundary helpers
 */

static const XaotFuncPlan *cg_func_plan(XiCgenCtx *ctx, const XiFunc *f) {
    const XaotFuncPlan *plan;

    if (!ctx || !f)
        return NULL;
    plan = xaot_bundle_find_func_plan(ctx->aot_bundle, f);
    if (!plan) {
        fprintf(stderr, "[xi_cgen] ERROR: missing AOT function plan for %s\n",
                f->name ? f->name : "?");
        ctx->error = true;
    }
    return plan;
}

static XrRep cg_abi_slot_storage_rep(const XaotAbiSlot *slot) {
    const XaotRepInfo *info;

    if (!slot)
        return XR_REP_TAGGED;
    if (slot->cls == XAOT_ARG_TAGGED)
        return XR_REP_TAGGED;
    info = xaot_rep_info(slot->rep.rep);
    return info ? info->storage_rep : XR_REP_TAGGED;
}

static const XaotValuePlan *cg_value_plan(XiCgenCtx *ctx, const XiValue *v) {
    const XaotValuePlan *plan;

    if (!ctx || !v)
        return NULL;
    plan = xaot_bundle_find_value_plan(ctx->aot_bundle, v);
    if (!plan) {
        fprintf(stderr, "[xi_cgen] ERROR: missing AOT value plan for v%u\n", v->id);
        ctx->error = true;
    }
    return plan;
}

static XrRep cg_value_plan_storage_rep(XiCgenCtx *ctx, const XiValue *v) {
    const XaotValuePlan *plan = cg_value_plan(ctx, v);
    return plan ? xaot_value_storage_rep(plan->rep) : XR_REP_TAGGED;
}

static XrRep cg_type_aot_storage_rep(const XrType *type) {
    XaotValueRep rep = xaot_value_rep_for_type(type);
    const XaotRepInfo *info = xaot_rep_info(rep.rep);
    return info ? info->storage_rep : XR_REP_TAGGED;
}

static bool cg_func_uses_typed_abi(XiCgenCtx *ctx, const XiFunc *f) {
    const XaotFuncPlan *plan = cg_func_plan(ctx, f);
    return plan && plan->abi.kind == XAOT_ABI_NATIVE;
}

static XrRep cg_func_return_abi_rep(XiCgenCtx *ctx, const XiFunc *f) {
    const XaotFuncPlan *plan = cg_func_plan(ctx, f);
    return plan ? cg_abi_slot_storage_rep(&plan->abi.ret) : XR_REP_TAGGED;
}

static const char *cg_func_return_abi_c_type(XiCgenCtx *ctx, const XiFunc *f) {
    const XaotFuncPlan *plan = cg_func_plan(ctx, f);
    return plan && plan->abi.ret.c_type ? plan->abi.ret.c_type : "XrValue";
}

static XrRep cg_func_param_abi_rep(XiCgenCtx *ctx, const XiFunc *f, uint16_t param_idx) {
    const XaotFuncPlan *plan = cg_func_plan(ctx, f);
    if (!plan || param_idx >= plan->abi.nparams || !plan->abi.params)
        return XR_REP_TAGGED;
    return cg_abi_slot_storage_rep(&plan->abi.params[param_idx]);
}

static const char *cg_func_param_abi_c_type(XiCgenCtx *ctx, const XiFunc *f, uint16_t param_idx) {
    const XaotFuncPlan *plan = cg_func_plan(ctx, f);
    if (!plan || param_idx >= plan->abi.nparams || !plan->abi.params ||
        !plan->abi.params[param_idx].c_type)
        return "XrValue";
    return plan->abi.params[param_idx].c_type;
}

static void emit_typed_abi_fname(XiCgenCtx *ctx, FILE *out, const char *prefix, const XiFunc *f) {
    emit_fname_suffix(ctx, out, prefix, f, "_boxed");
}

static void cg_prepare_func_tree_for_cgen(XiFunc *f) {
    if (!f)
        return;
    if (f->stage < XI_STAGE_REPPED) {
        XiRepPolicy policy = xi_rep_policy_native_boundary();
        xi_opt_select_rep_with_policy(f, &policy);
        xi_opt_box_elim(f);
    }
    if (f->stage < XI_STAGE_BACKEND)
        xi_backend_lower(f);
}

static bool emit_conversion_prefix(FILE *out, const XrType *type, XrRep from_rep, XrRep to_rep) {
    if (from_rep == to_rep)
        return false;
    if (to_rep == XR_REP_TAGGED) {
        if (from_rep == XR_REP_F64)
            fprintf(out, "XR_FROM_FLOAT(");
        else if (type && type->kind == XR_KIND_BOOL)
            fprintf(out, "XR_FROM_BOOL(");
        else
            fprintf(out, "XR_FROM_INT(");
        return true;
    }
    if (to_rep == XR_REP_I64) {
        if (from_rep == XR_REP_TAGGED)
            fprintf(out, "XR_TO_INT(");
        else
            fprintf(out, "(int64_t)(");
        return true;
    }
    if (to_rep == XR_REP_F64) {
        if (from_rep == XR_REP_TAGGED)
            fprintf(out, "XR_TO_FLOAT(");
        else
            fprintf(out, "(double)(");
        return true;
    }
    return false;
}

static void emit_conversion_suffix(FILE *out, bool wrapped) {
    if (wrapped)
        fprintf(out, ")");
}

static void emit_value_as_rep(FILE *out, const XiValue *v, XrRep target_rep) {
    bool wrapped = emit_conversion_prefix(out, v ? v->type : NULL, cg_rep(v), target_rep);
    emit_vref(out, v);
    emit_conversion_suffix(out, wrapped);
}

static void emit_value_as_rep_ctx(XiCgenCtx *ctx, FILE *out, const XiValue *v, XrRep target_rep) {
    XrRep from_rep = ctx ? cg_value_plan_storage_rep(ctx, v) : cg_rep(v);
    bool wrapped = emit_conversion_prefix(out, v ? v->type : NULL, from_rep, target_rep);
    emit_vref(out, v);
    emit_conversion_suffix(out, wrapped);
}

static const XaotBoundaryStep *cg_value_boundary_step(XiCgenCtx *ctx, const XiFunc *f,
                                                      const XiValue *value, const XiValue *input,
                                                      XaotBoundaryReason reason) {
    const XaotBoundaryStep *step;

    if (!ctx || !f || !value || !input) {
        if (ctx)
            ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: incomplete AOT boundary lookup\n");
        return NULL;
    }

    step = xaot_bundle_find_boundary_step(ctx->aot_bundle, XAOT_BOUNDARY_STEP_VALUE_REP, f, value,
                                          input);
    if (!step) {
        fprintf(stderr, "[xi_cgen] ERROR: missing AOT boundary step for %s %u\n",
                xi_op_name(value->op), value->id);
        ctx->error = true;
        return NULL;
    }
    if (step->reason != reason) {
        fprintf(stderr, "[xi_cgen] ERROR: AOT boundary reason mismatch for %s %u: %s != %s\n",
                xi_op_name(value->op), value->id, xaot_boundary_reason_name(step->reason),
                xaot_boundary_reason_name(reason));
        ctx->error = true;
        return NULL;
    }
    return step;
}

static const XaotBoundaryStep *
cg_direct_call_boundary_step(XiCgenCtx *ctx, XaotBoundaryStepKind kind, const XiFunc *f,
                             const XiValue *call, const XiValue *input, const XiFunc *target,
                             uint16_t arg_index) {
    const XaotBoundaryStep *step;

    if (!ctx || !f || !call || !target) {
        if (ctx)
            ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: incomplete AOT direct-call boundary lookup\n");
        return NULL;
    }

    step =
        xaot_bundle_find_boundary_step_ex(ctx->aot_bundle, kind, f, call, input, target, arg_index);
    if (!step) {
        fprintf(stderr, "[xi_cgen] ERROR: missing AOT direct-call boundary step at v%u\n",
                call->id);
        ctx->error = true;
        return NULL;
    }
    if (step->reason != XAOT_BOUNDARY_DIRECT_CALL) {
        fprintf(stderr, "[xi_cgen] ERROR: AOT direct-call boundary reason mismatch at v%u: %s\n",
                call->id, xaot_boundary_reason_name(step->reason));
        ctx->error = true;
        return NULL;
    }
    return step;
}

static bool emit_direct_call_return_conversion_prefix(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                      const XiValue *call, const XiFunc *target) {
    const XaotFuncPlan *target_plan = cg_func_plan(ctx, target);
    XrRep actual_rep;
    XrRep result_rep;
    const XaotBoundaryStep *step = NULL;

    if (!target_plan)
        return false;
    actual_rep = cg_abi_slot_storage_rep(&target_plan->abi.ret);
    result_rep = cg_value_plan_storage_rep(ctx, call);
    if (ctx->error)
        return false;
    if (actual_rep != result_rep) {
        step = cg_direct_call_boundary_step(ctx, XAOT_BOUNDARY_STEP_DIRECT_CALL_RET, f, call, NULL,
                                            target, UINT16_MAX);
        if (!step)
            return false;
        actual_rep = xaot_value_storage_rep(step->from_rep);
        result_rep = xaot_value_storage_rep(step->to_rep);
    }
    return emit_conversion_prefix(out, call ? call->type : NULL, actual_rep, result_rep);
}

static void emit_value_as_direct_call_arg(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                          const XiValue *call, const XiFunc *target,
                                          uint16_t arg_index, const XiValue *arg) {
    const XaotFuncPlan *target_plan = cg_func_plan(ctx, target);
    const XaotAbiSlot *slot;
    XrRep from_rep;
    XrRep to_rep;
    bool wrapped;

    if (!target_plan || arg_index >= target_plan->abi.nparams || !target_plan->abi.params) {
        fprintf(stderr, "[xi_cgen] ERROR: AOT direct-call argument ABI mismatch at v%u\n",
                call ? call->id : 0);
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return;
    }

    slot = &target_plan->abi.params[arg_index];
    from_rep = cg_value_plan_storage_rep(ctx, arg);
    to_rep = cg_abi_slot_storage_rep(slot);
    if (ctx->error) {
        emit_codegen_abort_expr(out);
        return;
    }

    if (from_rep != to_rep) {
        const XaotBoundaryStep *step = cg_direct_call_boundary_step(
            ctx, XAOT_BOUNDARY_STEP_DIRECT_CALL_ARG, f, call, arg, target, arg_index);
        if (!step) {
            emit_codegen_abort_expr(out);
            return;
        }
        from_rep = xaot_value_storage_rep(step->from_rep);
        to_rep = xaot_value_storage_rep(step->to_rep);
    }

    wrapped = emit_conversion_prefix(out, arg ? arg->type : NULL, from_rep, to_rep);
    emit_vref(out, arg);
    emit_conversion_suffix(out, wrapped);
}
