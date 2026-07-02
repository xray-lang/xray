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
    return xaot_value_storage_rep(xaot_abi_slot_value_rep(slot));
}

static bool cg_abi_slot_is_aggregate(const XaotAbiSlot *slot) {
    XaotValueRep rep = xaot_abi_slot_value_rep(slot);
    return rep.kind == XAOT_VALUE_AGGREGATE;
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

static bool cg_value_plan_is_aggregate(XiCgenCtx *ctx, const XiValue *v) {
    const XaotValuePlan *plan = cg_value_plan(ctx, v);
    return plan && plan->rep.kind == XAOT_VALUE_AGGREGATE;
}

static bool cg_value_rep_is_adt_aggregate(XaotValueRep rep) {
    return rep.kind == XAOT_VALUE_AGGREGATE && (rep.flags & XAOT_VALUE_FLAG_ADT) != 0;
}

static bool cg_value_rep_is_struct_aggregate(XaotValueRep rep) {
    return rep.kind == XAOT_VALUE_AGGREGATE && (rep.flags & XAOT_VALUE_FLAG_STRUCT) != 0;
}

static bool cg_value_plan_is_adt_aggregate(XiCgenCtx *ctx, const XiValue *v) {
    const XaotValuePlan *plan = cg_value_plan(ctx, v);
    return plan && cg_value_rep_is_adt_aggregate(plan->rep);
}

static bool cg_value_plan_is_struct_aggregate(XiCgenCtx *ctx, const XiValue *v) {
    const XaotValuePlan *plan = cg_value_plan(ctx, v);
    return plan && cg_value_rep_is_struct_aggregate(plan->rep);
}

static void emit_aggregate_zero_expr(FILE *out, XaotValueRep rep) {
    if (cg_value_rep_is_struct_aggregate(rep)) {
        fprintf(out, "((%s){0})", rep.c_type ? rep.c_type : "XrValue");
        return;
    }
    fprintf(out, "xrt_adt_value_zero()");
}

static void emit_value_plan_zero_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    const XaotValuePlan *plan = cg_value_plan(ctx, v);
    if (plan && plan->rep.kind == XAOT_VALUE_AGGREGATE) {
        emit_aggregate_zero_expr(out, plan->rep);
        return;
    }
    fprintf(out, "XR_NULL_VAL");
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

static bool cg_func_return_abi_is_aggregate(XiCgenCtx *ctx, const XiFunc *f) {
    const XaotFuncPlan *plan = cg_func_plan(ctx, f);
    return plan && cg_abi_slot_is_aggregate(&plan->abi.ret);
}

static bool cg_func_return_abi_is_adt_aggregate(XiCgenCtx *ctx, const XiFunc *f) {
    const XaotFuncPlan *plan = cg_func_plan(ctx, f);
    return plan && cg_value_rep_is_adt_aggregate(xaot_abi_slot_value_rep(&plan->abi.ret));
}

static bool cg_func_return_abi_is_struct_aggregate(XiCgenCtx *ctx, const XiFunc *f) {
    const XaotFuncPlan *plan = cg_func_plan(ctx, f);
    return plan && cg_value_rep_is_struct_aggregate(xaot_abi_slot_value_rep(&plan->abi.ret));
}

static XaotValueRep cg_func_return_abi_value_rep(XiCgenCtx *ctx, const XiFunc *f) {
    const XaotFuncPlan *plan = cg_func_plan(ctx, f);
    return plan ? xaot_abi_slot_value_rep(&plan->abi.ret)
                : xaot_value_rep_for_type(f ? f->return_type : NULL);
}

static const char *cg_func_return_abi_c_type(XiCgenCtx *ctx, const XiFunc *f) {
    const XaotFuncPlan *plan = cg_func_plan(ctx, f);
    return plan && plan->abi.ret.c_type ? plan->abi.ret.c_type : "XrValue";
}

/* FFI: a raw pointer (RawPtr<T>/RawMut<T>) is stored as a native pointer in AOT
 * code, while tagged/VM boundaries encode it as an address-width integer.
 * Returns the boundary C type ("void *" / "const void *") when `t` is a pointer, else NULL.
 * Pointee precision is unnecessary at the call boundary: deref/index sites cast
 * to the exact element type. */
static const char *cg_extern_ptr_boundary_c_type(const XrType *t) {
    if (t && t->kind == XR_KIND_POINTER)
        return t->ptr_is_mut ? "void *" : "const void *";
    return NULL;
}

static bool cg_type_is_c_callback(const XrType *type) {
    return type && XR_TYPE_IS_C_FUNCTION(type);
}

static const char *cg_cfn_value_c_type(const XrType *type, bool is_return) {
    if (!type)
        return is_return ? "void" : "int64_t";
    switch (type->kind) {
        case XR_KIND_UNIT:
            return is_return ? "void" : NULL;
        case XR_KIND_BOOL:
            return "uint8_t";
        case XR_KIND_FLOAT:
            return type->native_width == XR_NATIVE_F32 ? "float" : "double";
        case XR_KIND_INT:
            switch (type->native_width) {
                case XR_NATIVE_I8:
                    return "int8_t";
                case XR_NATIVE_U8:
                    return "uint8_t";
                case XR_NATIVE_I16:
                    return "int16_t";
                case XR_NATIVE_U16:
                    return "uint16_t";
                case XR_NATIVE_I32:
                    return "int32_t";
                case XR_NATIVE_U32:
                    return "uint32_t";
                case XR_NATIVE_U64:
                    return "uint64_t";
                default:
                    return "int64_t";
            }
        case XR_KIND_POINTER:
            return type->ptr_is_mut ? "void *" : "const void *";
        default:
            return NULL;
    }
}

static XrRep cg_cfn_value_storage_rep(const XrType *type, bool is_return) {
    if (!type || (is_return && type->kind == XR_KIND_UNIT))
        return XR_REP_VOID;
    if (type->kind == XR_KIND_FLOAT)
        return XR_REP_F64;
    if (type->kind == XR_KIND_POINTER)
        return XR_REP_RAWPTR;
    if (type->kind == XR_KIND_INT || type->kind == XR_KIND_BOOL)
        return XR_REP_I64;
    return XR_REP_TAGGED;
}

static bool cg_cfn_value_type_supported(const XrType *type, bool is_return) {
    return cg_cfn_value_c_type(type, is_return) != NULL;
}

static bool cg_cfn_function_signature_supported(const XrType *fn_type) {
    if (!cg_type_is_c_callback(fn_type) || fn_type->function.is_variadic)
        return false;
    if (!cg_cfn_value_type_supported(fn_type->function.return_type, true))
        return false;
    for (int i = 0; i < fn_type->function.param_count; i++) {
        const XrType *pt = fn_type->function.param_types ? fn_type->function.param_types[i] : NULL;
        if (!cg_cfn_value_type_supported(pt, false))
            return false;
    }
    return true;
}

static bool cg_cfn_xray_func_signature_supported(const XiFunc *f) {
    if (!f || f->is_vararg)
        return false;
    if (!cg_cfn_value_type_supported(f->return_type, true))
        return false;
    for (uint16_t i = 0; i < f->nparams; i++) {
        const XrType *pt = f->params && f->params[i] ? f->params[i]->type : NULL;
        if (!cg_cfn_value_type_supported(pt, false))
            return false;
    }
    return true;
}

static bool cg_cfn_xray_func_matches_expected(const XiFunc *f, const XrType *expected) {
    if (!f || !cg_type_is_c_callback(expected))
        return false;
    if ((int) f->nparams != expected->function.param_count)
        return false;
    if (!xr_type_equals(f->return_type, expected->function.return_type))
        return false;
    for (uint16_t i = 0; i < f->nparams; i++) {
        const XrType *actual = f->params && f->params[i] ? f->params[i]->type : NULL;
        const XrType *want =
            expected->function.param_types ? expected->function.param_types[i] : NULL;
        if (!xr_type_equals((XrType *) actual, (XrType *) want))
            return false;
    }
    return true;
}

static void emit_cfn_pointer_type(XiCgenCtx *ctx, FILE *out, const XrType *fn_type,
                                  const char *name) {
    if (!cg_cfn_function_signature_supported(fn_type)) {
        if (ctx)
            ctx->error = true;
        fprintf(out, "void *");
        if (name && name[0])
            fprintf(out, "%s", name);
        return;
    }

    fprintf(out, "%s (*%s)(", cg_cfn_value_c_type(fn_type->function.return_type, true),
            name ? name : "");
    if (fn_type->function.param_count == 0) {
        fprintf(out, "void");
    } else {
        for (int i = 0; i < fn_type->function.param_count; i++) {
            if (i > 0)
                fprintf(out, ", ");
            const XrType *pt =
                fn_type->function.param_types ? fn_type->function.param_types[i] : NULL;
            fprintf(out, "%s", cg_cfn_value_c_type(pt, false));
        }
    }
    fprintf(out, ")");
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

static void emit_cfn_stub_fname(XiCgenCtx *ctx, FILE *out, const char *prefix, const XiFunc *f) {
    emit_fname_suffix(ctx, out, prefix, f, "_cfn");
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

static const char *cg_ptr_box_suffix_for_type(const XrType *type) {
    if (!type)
        return ", XR_TAG_PTR)";
    switch (type->kind) {
        case XR_KIND_ARRAY:
        case XR_KIND_SPAN:
            return ", XR_TAG_ARRAY)";
        case XR_KIND_MAP:
            return ", XR_TAG_MAP)";
        case XR_KIND_SET:
            return ", XR_TAG_SET)";
        case XR_KIND_TUPLE:
            return ", XR_TAG_TUPLE)";
        default:
            return ", XR_TAG_PTR)";
    }
}

static bool cg_type_is_class_instance_ptr(const XrType *type) {
    return type && (type->kind == XR_KIND_CLASS || type->kind == XR_KIND_INSTANCE) &&
           type->instance.class_name != NULL;
}

static const char *emit_conversion_prefix(FILE *out, const XrType *type, XrRep from_rep,
                                          XrRep to_rep) {
    if (from_rep == to_rep)
        return NULL;
    /* A void source (unit-returning call) produces no C value: run the
     * expression inside a statement expression and materialize the unit
     * result in the target rep. Without this, the fallthrough cases below
     * would wrap a void call in XR_FROM_INT and fail C compilation. */
    if (from_rep == XR_REP_VOID) {
        fprintf(out, "({ ");
        if (to_rep == XR_REP_TAGGED)
            return "; XR_NULL_VAL; })";
        if (to_rep == XR_REP_F64)
            return "; 0.0; })";
        if (to_rep == XR_REP_PTR || to_rep == XR_REP_RAWPTR)
            return "; (void*)0; })";
        return "; 0; })";
    }
    if (to_rep == XR_REP_TAGGED) {
        if (from_rep == XR_REP_RAWPTR) {
            fprintf(out, "XR_FROM_INT((int64_t)(uintptr_t)(");
            return "))";
        }
        if (from_rep == XR_REP_PTR && type && type->kind == XR_KIND_STRING) {
            fprintf(out, "xr_str_value_from_ptr(");
            return ")";
        }
        if (from_rep == XR_REP_PTR && cg_type_is_class_instance_ptr(type)) {
            fprintf(out, "xrt_box_obj(");
            return ")";
        }
        if (from_rep == XR_REP_PTR) {
            fprintf(out, "xr_mkptr(");
            return cg_ptr_box_suffix_for_type(type);
        }
        if (from_rep == XR_REP_I64 && type && type->kind == XR_KIND_CHAR) {
            fprintf(out, "XR_FROM_CHAR((uint32_t)");
            return ")";
        }
        if (from_rep == XR_REP_F64)
            fprintf(out, "XR_FROM_FLOAT(");
        else if (type && type->kind == XR_KIND_BOOL)
            fprintf(out, "XR_FROM_BOOL(");
        else
            fprintf(out, "XR_FROM_INT(");
        return ")";
    }
    if (to_rep == XR_REP_PTR) {
        if (from_rep == XR_REP_TAGGED) {
            fprintf(out, "(");
            return ").ptr";
        }
        if (from_rep == XR_REP_RAWPTR) {
            fprintf(out, "(void *)(");
            return ")";
        }
        fprintf(out, "(void *)(");
        return ")";
    }
    if (to_rep == XR_REP_RAWPTR) {
        if (from_rep == XR_REP_TAGGED) {
            fprintf(out, "(void *)(uintptr_t)XR_TO_INT(");
            return ")";
        }
        if (from_rep == XR_REP_I64) {
            fprintf(out, "(void *)(uintptr_t)(");
            return ")";
        }
        if (from_rep == XR_REP_PTR) {
            fprintf(out, "(void *)(");
            return ")";
        }
        fprintf(out, "(void *)(");
        return ")";
    }
    if (to_rep == XR_REP_I64) {
        if (from_rep == XR_REP_RAWPTR)
            fprintf(out, "(int64_t)(uintptr_t)(");
        else if (from_rep == XR_REP_TAGGED)
            fprintf(out, type && type->kind == XR_KIND_CHAR ? "XR_TO_CHAR(" : "XR_TO_INT(");
        else
            fprintf(out, "(int64_t)(");
        return ")";
    }
    if (to_rep == XR_REP_F64) {
        if (from_rep == XR_REP_TAGGED)
            fprintf(out, "XR_TO_FLOAT(");
        else
            fprintf(out, "(double)(");
        return ")";
    }
    return NULL;
}

static void emit_conversion_suffix(FILE *out, const char *suffix) {
    if (suffix)
        fprintf(out, "%s", suffix);
}

static bool cg_value_box_inner_native_rep(XiCgenCtx *ctx, const XiValue *v, XrRep *out_rep) {
    if (!v || v->op != XI_BOX || v->nargs < 1 || !v->args[0])
        return false;

    const XiFunc *owner = (v->block) ? v->block->func : NULL;
    if (ctx && owner && cg_func_needs_aot_coro_ctx(ctx, owner))
        return false;

    const XiValue *inner = v->args[0];
    const XaotValuePlan *inner_plan = ctx ? cg_value_plan(ctx, inner) : NULL;
    if (inner_plan && inner_plan->rep.kind == XAOT_VALUE_AGGREGATE)
        return false;

    XrRep inner_rep = inner_plan ? xaot_value_storage_rep(inner_plan->rep)
                                 : (ctx ? cg_value_plan_storage_rep(ctx, inner) : cg_rep(inner));
    if (inner_rep == XR_REP_TAGGED || inner_rep == XR_REP_VOID)
        return false;
    if (out_rep)
        *out_rep = inner_rep;
    return true;
}

static bool emit_const_value_as_rep_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                         XrRep from_rep, XrRep target_rep) {
    if (!v || v->op != XI_CONST)
        return false;
    const char *conv_suffix = emit_conversion_prefix(out, v->type, from_rep, target_rep);
    const XiFunc *vf = v->block ? v->block->func : NULL;
    const char *prefix = (ctx && ctx->module && ctx->module->name) ? ctx->module->name : NULL;
    emit_value_rhs(ctx, out, vf, v, prefix);
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static void emit_value_as_rep(FILE *out, const XiValue *v, XrRep target_rep) {
    const char *conv_suffix =
        emit_conversion_prefix(out, v ? v->type : NULL, cg_rep(v), target_rep);
    emit_vref(out, v);
    emit_conversion_suffix(out, conv_suffix);
}

static void emit_value_as_rep_ctx(XiCgenCtx *ctx, FILE *out, const XiValue *v, XrRep target_rep) {
    XrRep inner_rep;
    if (target_rep != XR_REP_TAGGED && cg_value_box_inner_native_rep(ctx, v, &inner_rep)) {
        const XiValue *inner = v->args[0];
        if (emit_const_value_as_rep_expr(ctx, out, inner, inner_rep, target_rep))
            return;
        const char *conv_suffix = emit_conversion_prefix(out, inner->type, inner_rep, target_rep);
        emit_vref(out, inner);
        emit_conversion_suffix(out, conv_suffix);
        return;
    }
    const XaotValuePlan *plan = (ctx && v) ? cg_value_plan(ctx, v) : NULL;
    if (v && v->op == XI_CONST) {
        XrRep from_rep = plan ? xaot_value_storage_rep(plan->rep)
                              : (ctx ? cg_value_plan_storage_rep(ctx, v) : cg_rep(v));
        emit_const_value_as_rep_expr(ctx, out, v, from_rep, target_rep);
        return;
    }
    if (plan && plan->rep.kind == XAOT_VALUE_AGGREGATE) {
        if (target_rep == XR_REP_TAGGED && cg_value_rep_is_adt_aggregate(plan->rep)) {
            fprintf(out, "xrt_adt_value_box(");
            emit_vref(out, v);
            fprintf(out, ")");
            return;
        }
        if (target_rep == XR_REP_TAGGED && cg_value_rep_is_struct_aggregate(plan->rep)) {
            const XiFunc *vf = (v && v->block) ? v->block->func : NULL;
            const char *prefix =
                (ctx && ctx->module && ctx->module->name) ? ctx->module->name : NULL;
            if (emit_struct_aggregate_box_expr(ctx, out, vf, v, prefix))
                return;
        }
        if (ctx) {
            const XiFunc *vf = (v && v->block) ? v->block->func : NULL;
            fprintf(stderr,
                    "[xi_cgen] ERROR: cannot convert aggregate v%u (%s in %s) to storage rep %d\n",
                    v ? v->id : 0, v ? xi_op_name((XiOp) v->op) : "?",
                    vf && vf->name ? vf->name : "?", (int) target_rep);
            ctx->error = true;
        }
        emit_codegen_abort_expr(out);
        return;
    }
    XrRep from_rep = plan ? xaot_value_storage_rep(plan->rep)
                          : (ctx ? cg_value_plan_storage_rep(ctx, v) : cg_rep(v));
    const char *conv_suffix = emit_conversion_prefix(out, v ? v->type : NULL, from_rep, target_rep);
    emit_vref(out, v);
    emit_conversion_suffix(out, conv_suffix);
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

static const char *emit_direct_call_return_conversion_prefix(XiCgenCtx *ctx, FILE *out,
                                                             const XiFunc *f, const XiValue *call,
                                                             const XiFunc *target) {
    const XaotFuncPlan *target_plan = cg_func_plan(ctx, target);
    const XaotValuePlan *call_plan;
    XrRep actual_rep;
    XrRep result_rep;
    const XaotBoundaryStep *step = NULL;

    if (!target_plan)
        return NULL;
    call_plan = cg_value_plan(ctx, call);
    if (call_plan && (target_plan->abi.ret.rep.kind == XAOT_VALUE_AGGREGATE ||
                      call_plan->rep.kind == XAOT_VALUE_AGGREGATE)) {
        XaotValueRep target_rep = xaot_abi_slot_value_rep(&target_plan->abi.ret);
        if (target_rep.kind == XAOT_VALUE_AGGREGATE &&
            call_plan->rep.kind != XAOT_VALUE_AGGREGATE &&
            xaot_value_storage_rep(call_plan->rep) == XR_REP_TAGGED &&
            cg_value_rep_is_adt_aggregate(target_rep)) {
            fprintf(out, "xrt_adt_value_box(");
            return ")";
        }
        if (!xaot_value_reps_equal(target_rep, call_plan->rep)) {
            fprintf(stderr, "[xi_cgen] ERROR: aggregate direct-call return ABI mismatch at v%u\n",
                    call ? call->id : 0);
            ctx->error = true;
        }
        return NULL;
    }
    actual_rep = cg_abi_slot_storage_rep(&target_plan->abi.ret);
    result_rep = cg_value_plan_storage_rep(ctx, call);
    if (ctx->error)
        return NULL;
    if (actual_rep != result_rep) {
        step = xaot_bundle_find_boundary_step_ex(
            ctx->aot_bundle, XAOT_BOUNDARY_STEP_DIRECT_CALL_RET, f, call, NULL, target, UINT16_MAX);
        if (step) {
            actual_rep = xaot_value_storage_rep(step->from_rep);
            result_rep = xaot_value_storage_rep(step->to_rep);
        }
    }
    return emit_conversion_prefix(out, call ? call->type : NULL, actual_rep, result_rep);
}

static void emit_value_as_direct_call_arg(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                          const XiValue *call, const XiFunc *target,
                                          uint16_t arg_index, const XiValue *arg) {
    const XaotFuncPlan *target_plan = cg_func_plan(ctx, target);
    const XaotValuePlan *arg_plan;
    const XaotAbiSlot *slot;
    XaotValueRep slot_rep;
    XrRep from_rep;
    XrRep to_rep;
    const char *conv_suffix;

    if (!target_plan || arg_index >= target_plan->abi.nparams || !target_plan->abi.params) {
        fprintf(stderr, "[xi_cgen] ERROR: AOT direct-call argument ABI mismatch at v%u\n",
                call ? call->id : 0);
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return;
    }

    slot = &target_plan->abi.params[arg_index];
    slot_rep = xaot_abi_slot_value_rep(slot);
    arg_plan = cg_value_plan(ctx, arg);
    if (slot_rep.kind == XAOT_VALUE_AGGREGATE) {
        if (arg_plan && xaot_value_reps_equal(arg_plan->rep, slot_rep)) {
            emit_vref(out, arg);
            return;
        }
        fprintf(stderr,
                "[xi_cgen] ERROR: cannot pass non-aggregate v%u to aggregate ABI slot %u "
                "of '%s'\n",
                arg ? arg->id : 0, (unsigned) arg_index,
                target && target->name ? target->name : "?");
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return;
    }
    if (arg_plan && arg_plan->rep.kind == XAOT_VALUE_AGGREGATE) {
        if (xaot_value_storage_rep(slot_rep) == XR_REP_TAGGED &&
            cg_value_rep_is_adt_aggregate(arg_plan->rep)) {
            fprintf(out, "xrt_adt_value_box(");
            emit_vref(out, arg);
            fprintf(out, ")");
            return;
        }
        fprintf(stderr, "[xi_cgen] ERROR: cannot pass aggregate v%u to non-tagged ABI slot\n",
                arg ? arg->id : 0);
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return;
    }
    from_rep = cg_value_plan_storage_rep(ctx, arg);
    to_rep = cg_abi_slot_storage_rep(slot);
    if (ctx->error) {
        emit_codegen_abort_expr(out);
        return;
    }

    if (from_rep != to_rep) {
        const XaotBoundaryStep *step = xaot_bundle_find_boundary_step_ex(
            ctx->aot_bundle, XAOT_BOUNDARY_STEP_DIRECT_CALL_ARG, f, call, arg, target, arg_index);
        if (step) {
            from_rep = xaot_value_storage_rep(step->from_rep);
            to_rep = xaot_value_storage_rep(step->to_rep);
        }
    }

    conv_suffix = emit_conversion_prefix(out, arg ? arg->type : NULL, from_rep, to_rep);
    XrRep inner_rep;
    if (to_rep != XR_REP_TAGGED && !cg_func_needs_aot_coro_ctx(ctx, f) &&
        cg_value_box_inner_native_rep(ctx, arg, &inner_rep)) {
        const XiValue *inner = arg->args[0];
        conv_suffix = emit_conversion_prefix(out, inner->type, inner_rep, to_rep);
        emit_vref(out, inner);
        emit_conversion_suffix(out, conv_suffix);
        return;
    }
    emit_vref(out, arg);
    emit_conversion_suffix(out, conv_suffix);
}

static const XiModule *cg_cfn_module_for_func(XiCgenCtx *ctx, const XiFunc *f) {
    if (!f)
        return NULL;

    const XiFunc *root = f;
    while (root->parent_func)
        root = root->parent_func;
    if (root->module && root->module->init == root)
        return root->module;

    if (ctx && ctx->module && ctx->module->init && cg_func_tree_contains(ctx->module->init, f))
        return ctx->module;

    if (ctx && ctx->all_modules) {
        for (int i = 0; i < ctx->all_nmodules; i++) {
            const XiModule *mod = ctx->all_modules[i];
            if (mod && mod->init && cg_func_tree_contains(mod->init, f))
                return mod;
        }
    }

    return NULL;
}

static bool cg_cfn_func_has_module_level_storage(XiCgenCtx *ctx, const XiFunc *f) {
    const XiModule *mod = cg_cfn_module_for_func(ctx, f);
    return f && mod && mod->init && f->parent_func == mod->init;
}

static bool cg_func_can_have_cfn_stub(XiCgenCtx *ctx, const XiFunc *f) {
    if (!f || f->is_extern || !cg_cfn_func_has_module_level_storage(ctx, f) || f->ncaptures > 0 ||
        cg_func_needs_aot_coro_ctx(ctx, f))
        return false;
    return cg_cfn_xray_func_signature_supported(f);
}

static bool emit_cfn_callback_arg(XiCgenCtx *ctx, FILE *out, const XiFunc *current,
                                  const char *prefix, const XiValue *call, uint16_t arg_index,
                                  const XrType *expected_type, const XiValue *arg) {
    CgStaticFunctionCall cb = cg_resolve_static_function_call(ctx, current, arg);
    const char *cb_prefix = NULL;

    if (!cb.func || cb.is_class_constructor || cb.func->is_extern ||
        !cg_func_can_have_cfn_stub(ctx, cb.func) ||
        !cg_cfn_xray_func_matches_expected(cb.func, expected_type)) {
        fprintf(stderr,
                "[xi_cgen] ERROR: unsupported CFn callback argument at v%u arg %u; expected a "
                "top-level noncapturing function with an exact CFn signature\n",
                call ? call->id : 0, (unsigned) arg_index + 1);
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return false;
    }

    cb_prefix = cb.prefix ? cb.prefix : prefix;
    emit_cfn_stub_fname(ctx, out, cb_prefix, cb.func);
    return true;
}
