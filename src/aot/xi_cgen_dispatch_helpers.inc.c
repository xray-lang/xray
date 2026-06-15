/*
 * xi_cgen_dispatch_helpers.inc.c - Generated Xi lowering driver helpers for AOT C
 */

static void xicgen_const(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                         const char *prefix) {
    (void) f;
    (void) prefix;
    if (v->type->kind == XR_KIND_INT)
        fprintf(out, "INT64_C(%" PRId64 ")", v->aux_int);
    else if (v->type->kind == XR_KIND_FLOAT) {
        double d;
        memcpy(&d, &v->aux_int, sizeof(double));
        fprintf(out, "%a", d);
    } else if (v->type->kind == XR_KIND_BOOL)
        fprintf(out, "%" PRId64, v->aux_int);
    else if (v->type->kind == XR_KIND_NULL)
        fprintf(out, "XR_NULL_VAL");
    else if (v->type->kind == XR_KIND_STRING) {
        cg_emit_str_value(ctx, out, (const char *) v->aux);
    } else if (v->type->kind == XR_KIND_UNKNOWN && v->aux) {
        emit_enum_type_expr(ctx, out, cg_enum_for_runtime_type(ctx, v->aux));
    } else {
        fprintf(out, "XR_NULL_VAL /* unknown const kind */");
    }
}

static void xicgen_param(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                         const char *prefix) {
    (void) prefix;
    uint16_t param_idx = (uint16_t) v->aux_int;
    XrRep from_rep = cg_func_param_abi_rep(ctx, f, param_idx);
    XrRep to_rep = cg_value_plan_storage_rep(ctx, v);
    const char *conv_suffix = emit_conversion_prefix(out, v->type, from_rep, to_rep);
    fprintf(out, "p%u", (unsigned) v->aux_int);
    emit_conversion_suffix(out, conv_suffix);
}

static void xicgen_identity(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                            const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 1, "xicgen_identity: need arg");
    emit_vref(out, v->args[0]);
}

static void xicgen_copy(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                        const char *prefix) {
    xicgen_identity(ctx, out, f, v, prefix);
}

static void xicgen_move(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                        const char *prefix) {
    xicgen_identity(ctx, out, f, v, prefix);
}

static const char *xicgen_arith_runtime_fn(uint16_t op) {
    switch (op) {
        case XI_ADD:
            return "xrt_add";
        case XI_SUB:
            return "xrt_sub";
        case XI_MUL:
            return "xrt_mul";
        default:
            return NULL;
    }
}

static const char *xicgen_arith_native_op(uint16_t op) {
    switch (op) {
        case XI_SUB:
            return "-";
        case XI_MUL:
            return "*";
        default:
            return "+";
    }
}

/* Wrap helper for native int64 add/sub/mul. Raw C operators are signed-
 * overflow UB; the language defines two's-complement wrap (matching VM and
 * xi_opt fold), so native-rep integer arithmetic must call these. */
static const char *xicgen_arith_i64_wrap_fn(uint16_t op) {
    switch (op) {
        case XI_SUB:
            return "xrt_i64_sub";
        case XI_MUL:
            return "xrt_i64_mul";
        default:
            return "xrt_i64_add";
    }
}

static void xicgen_arith(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                         const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    XrRep result_rep = cg_rep(v);
    XrRep a_rep = cg_rep(v->args[0]);
    XrRep b_rep = cg_rep(v->args[1]);
    bool any_tagged = (a_rep == XR_REP_TAGGED || b_rep == XR_REP_TAGGED);
    if (result_rep == XR_REP_TAGGED || any_tagged) {
        const char *fn = xicgen_arith_runtime_fn(v->op);
        if (result_rep == XR_REP_F64) {
            fprintf(out, "%s(", fn);
            if (a_rep != XR_REP_TAGGED) {
                fprintf(out, "XR_FROM_FLOAT(");
                emit_vref(out, v->args[0]);
                fprintf(out, ")");
            } else {
                emit_vref(out, v->args[0]);
            }
            fprintf(out, ", ");
            if (b_rep != XR_REP_TAGGED) {
                fprintf(out, "XR_FROM_FLOAT(");
                emit_vref(out, v->args[1]);
                fprintf(out, ")");
            } else {
                emit_vref(out, v->args[1]);
            }
            fprintf(out, ").f");
        } else if (result_rep == XR_REP_I64) {
            fprintf(out, "%s(", fn);
            if (a_rep != XR_REP_TAGGED) {
                fprintf(out, "XR_FROM_INT(");
                emit_vref(out, v->args[0]);
                fprintf(out, ")");
            } else {
                emit_vref(out, v->args[0]);
            }
            fprintf(out, ", ");
            if (b_rep != XR_REP_TAGGED) {
                fprintf(out, "XR_FROM_INT(");
                emit_vref(out, v->args[1]);
                fprintf(out, ")");
            } else {
                emit_vref(out, v->args[1]);
            }
            fprintf(out, ").i");
        } else {
            fprintf(out, "%s(", fn);
            emit_vref(out, v->args[0]);
            fprintf(out, ", ");
            emit_vref(out, v->args[1]);
            fprintf(out, ")");
        }
    } else if (result_rep == XR_REP_I64) {
        // Native int64: must wrap on overflow (raw + - * is signed UB in C).
        fprintf(out, "%s(", xicgen_arith_i64_wrap_fn(v->op));
        emit_vref(out, v->args[0]);
        fprintf(out, ", ");
        emit_vref(out, v->args[1]);
        fprintf(out, ")");
    } else {
        emit_binop(out, v, xicgen_arith_native_op(v->op));
    }
}

static void xicgen_add(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                       const char *prefix) {
    xicgen_arith(ctx, out, f, v, prefix);
}

static void xicgen_sub(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                       const char *prefix) {
    xicgen_arith(ctx, out, f, v, prefix);
}

static void xicgen_mul(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                       const char *prefix) {
    xicgen_arith(ctx, out, f, v, prefix);
}

static void xicgen_div_mod(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                           const char *prefix) {
    (void) f;
    (void) prefix;
    XrRep result_rep = cg_rep(v);
    XrRep a_rep = cg_rep(v->args[0]);
    XrRep b_rep = cg_rep(v->args[1]);
    bool any_tagged = (a_rep == XR_REP_TAGGED || b_rep == XR_REP_TAGGED);
    if (result_rep == XR_REP_TAGGED || any_tagged) {
        const char *fn = (v->op == XI_DIV) ? "xrt_div" : "xrt_mod";
        if (result_rep == XR_REP_F64) {
            fprintf(out, "%s(", fn);
            if (a_rep != XR_REP_TAGGED) {
                fprintf(out, "XR_FROM_FLOAT(");
                emit_vref(out, v->args[0]);
                fprintf(out, ")");
            } else {
                emit_vref(out, v->args[0]);
            }
            fprintf(out, ", ");
            if (b_rep != XR_REP_TAGGED) {
                fprintf(out, "XR_FROM_FLOAT(");
                emit_vref(out, v->args[1]);
                fprintf(out, ")");
            } else {
                emit_vref(out, v->args[1]);
            }
            fprintf(out, ").f");
        } else if (result_rep == XR_REP_I64) {
            fprintf(out, "%s(", fn);
            if (a_rep != XR_REP_TAGGED) {
                fprintf(out, "XR_FROM_INT(");
                emit_vref(out, v->args[0]);
                fprintf(out, ")");
            } else {
                emit_vref(out, v->args[0]);
            }
            fprintf(out, ", ");
            if (b_rep != XR_REP_TAGGED) {
                fprintf(out, "XR_FROM_INT(");
                emit_vref(out, v->args[1]);
                fprintf(out, ")");
            } else {
                emit_vref(out, v->args[1]);
            }
            fprintf(out, ").i");
        } else {
            fprintf(out, "%s(", fn);
            emit_vref(out, v->args[0]);
            fprintf(out, ", ");
            emit_vref(out, v->args[1]);
            fprintf(out, ")");
        }
    } else if (result_rep == XR_REP_I64) {
        if (!emit_native_const_div_mod_expr(out, v)) {
            const char *fn = (v->op == XI_DIV) ? "xrt_int_div" : "xrt_int_mod";
            fprintf(out, "%s(", fn);
            emit_vref(out, v->args[0]);
            fprintf(out, ", ");
            emit_vref(out, v->args[1]);
            fprintf(out, ")");
        }
    } else if (result_rep == XR_REP_F64) {
        if (v->op == XI_DIV) {
            fprintf(out, "(xrt_div(XR_FROM_FLOAT(");
            emit_vref(out, v->args[0]);
            fprintf(out, "), XR_FROM_FLOAT(");
            emit_vref(out, v->args[1]);
            fprintf(out, ")).f)");
        } else {
            fprintf(out, "(xrt_mod(XR_FROM_FLOAT(");
            emit_vref(out, v->args[0]);
            fprintf(out, "), XR_FROM_FLOAT(");
            emit_vref(out, v->args[1]);
            fprintf(out, ")).f)");
        }
    } else {
        emit_codegen_abort_expr(out);
        ctx->error = true;
    }
}

static void xicgen_div(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                       const char *prefix) {
    xicgen_div_mod(ctx, out, f, v, prefix);
}

static void xicgen_mod(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                       const char *prefix) {
    xicgen_div_mod(ctx, out, f, v, prefix);
}

static void xicgen_neg(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                       const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    XrRep result_rep = cg_rep(v);
    XrRep a_rep = cg_rep(v->args[0]);
    if (result_rep == XR_REP_TAGGED || a_rep == XR_REP_TAGGED) {
        fprintf(out, "xrt_neg(");
        emit_vref(out, v->args[0]);
        fprintf(out, ")");
    } else if (result_rep == XR_REP_I64) {
        // -INT64_MIN is signed UB in C; negate via uint64 (wrap, matches
        // tagged xrt_neg).
        fprintf(out, "(int64_t) (-(uint64_t) (");
        emit_vref(out, v->args[0]);
        fprintf(out, "))");
    } else {
        fprintf(out, "-");
        emit_vref(out, v->args[0]);
    }
}

static void xicgen_bitwise_binop(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                 const char *prefix, const char *op) {
    (void) f;
    (void) prefix;
    emit_bitwise_binop_ctx(ctx, out, v, op);
}

static void xicgen_band(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                        const char *prefix) {
    xicgen_bitwise_binop(ctx, out, f, v, prefix, "&");
}

static void xicgen_bor(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                       const char *prefix) {
    xicgen_bitwise_binop(ctx, out, f, v, prefix, "|");
}

static void xicgen_bxor(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                        const char *prefix) {
    xicgen_bitwise_binop(ctx, out, f, v, prefix, "^");
}

static void xicgen_bnot(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                        const char *prefix) {
    (void) f;
    (void) prefix;
    emit_bitwise_unop_ctx(ctx, out, v, "~");
}

static void xicgen_not(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                       const char *prefix) {
    (void) f;
    (void) prefix;
    fprintf(out, "!(");
    emit_condition_expr_ctx(ctx, out, v->args[0]);
    fprintf(out, ")");
}

static void xicgen_select(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                          const char *prefix) {
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs == 3, "xicgen_select: need cond, true, false");
    fprintf(out, "(");
    emit_condition_expr_ctx(ctx, out, v->args[0]);
    fprintf(out, " ? ");
    emit_vref(out, v->args[1]);
    fprintf(out, " : ");
    emit_vref(out, v->args[2]);
    fprintf(out, ")");
}

static void xicgen_get_shared(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    (void) f;
    (void) prefix;
    fprintf(out, "%s[%d]", ctx->shared_name, (int) v->aux_int);
}

static void xicgen_set_shared(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    (void) f;
    (void) prefix;
    fprintf(out, "(%s[%d] = ", ctx->shared_name, (int) v->aux_int);
    emit_vref(out, v->args[0]);
    fprintf(out, ")");
}

static void xicgen_import_ref(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    (void) f;
    (void) prefix;
    const XiImportRef *ref = (const XiImportRef *) v->aux;
    bool found = false;
    if (ref && ref->resolved_mod_index >= 0 && ref->resolved_shared_slot >= 0 &&
        ref->resolved_mod_index < ctx->all_nmodules && ctx->all_modules[ref->resolved_mod_index]) {
        const char *tname = ctx->all_modules[ref->resolved_mod_index]->name;
        fprintf(out, "xrt_shared_%s[%d]", tname ? tname : "mod", ref->resolved_shared_slot);
        found = true;
    }
    if (!found && ref) {
        for (int ii = 0; ii < ctx->nimports; ii++) {
            if (ctx->imports[ii].module_path && ref->module_path &&
                strcmp(ctx->imports[ii].module_path, ref->module_path) == 0 &&
                ctx->imports[ii].member_name && ref->member_name &&
                strcmp(ctx->imports[ii].member_name, ref->member_name) == 0) {
                fprintf(out, "xrt_shared_%s[%d]", ctx->imports[ii].target_mod_name,
                        ctx->imports[ii].shared_slot);
                found = true;
                break;
            }
        }
    }
    if (!found) {
        if (ref && ref->module_path && !ref->member_name && strcmp(ref->module_path, "time") == 0) {
            fprintf(out, "XR_NULL_VAL /* builtin module: time */");
        } else {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: unresolved AOT import '%s.%s'\n",
                    ref && ref->module_path ? ref->module_path : "?",
                    ref && ref->member_name ? ref->member_name : "?");
            emit_codegen_abort_expr(out);
        }
    }
}

static void xicgen_closure_new(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                               const char *prefix) {
    emit_closure_new_expr(ctx, out, f, prefix, v);
}

static bool xicgen_upval_needs_cell(const XiFunc *f, const XiValue *v) {
    return v->aux_int >= 0 && v->aux_int < f->ncaptures && f->captures[v->aux_int].needs_cell;
}

static void xicgen_load_upval(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    (void) ctx;
    (void) prefix;
    if (xicgen_upval_needs_cell(f, v)) {
        char cell_expr[64];
        snprintf(cell_expr, sizeof(cell_expr), "_cl->upvals[%d]", (int) v->aux_int);
        emit_cell_get_for_rep(out, v, cell_expr);
    } else {
        fprintf(out, "_cl->upvals[%d]", (int) v->aux_int);
    }
}

static void xicgen_store_upval(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                               const char *prefix) {
    (void) ctx;
    (void) prefix;
    if (xicgen_upval_needs_cell(f, v)) {
        fprintf(out, "(xrt_cell_set(_cl->upvals[%d], ", (int) v->aux_int);
        emit_boxed_value_ref(out, v->args[0]);
        fprintf(out, "), XR_NULL_VAL)");
    } else {
        fprintf(out, "(_cl->upvals[%d] = ", (int) v->aux_int);
        emit_vref(out, v->args[0]);
        fprintf(out, ")");
    }
}

static void xicgen_assert(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                          const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 1, "xicgen_assert: need cond");
    const char *loc = v->aux ? (const char *) v->aux : "<unknown>";
    bool invert = (v->aux_int == 1);
    if (invert) {
        fprintf(out, "(xr_truthy(");
        emit_vref(out, v->args[0]);
        fprintf(out,
                ") ? (fprintf(stderr, \"Assertion failed (expected false): %s\\n\"), "
                "abort(), XR_NULL_VAL) : XR_NULL_VAL)",
                loc);
    } else {
        fprintf(out, "(!xr_truthy(");
        emit_vref(out, v->args[0]);
        fprintf(out,
                ") ? (fprintf(stderr, \"Assertion failed: %s\\n\"), abort(), XR_NULL_VAL) "
                ": XR_NULL_VAL)",
                loc);
    }
}

static void xicgen_assert_eq(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                             const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 2, "xicgen_assert_eq: need 2 args");
    const char *loc = v->aux ? (const char *) v->aux : "<unknown>";
    fprintf(out, "(xrt_eq(");
    emit_vref(out, v->args[0]);
    fprintf(out, ", ");
    emit_vref(out, v->args[1]);
    fprintf(out,
            ") ? XR_NULL_VAL : (fprintf(stderr, \"assert_eq failed: %s\\n\"), abort(), "
            "XR_NULL_VAL))",
            loc);
}

static void xicgen_assert_ne(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                             const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 2, "xicgen_assert_ne: need 2 args");
    const char *loc = v->aux ? (const char *) v->aux : "<unknown>";
    fprintf(out, "(!xrt_eq(");
    emit_vref(out, v->args[0]);
    fprintf(out, ", ");
    emit_vref(out, v->args[1]);
    fprintf(out,
            ") ? XR_NULL_VAL : (fprintf(stderr, \"assert_ne failed: %s\\n\"), abort(), "
            "XR_NULL_VAL))",
            loc);
}

static void xicgen_typeof(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                          const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 1, "xicgen_typeof: need arg");
    if (v->aux_int == 1) {
        fprintf(out, "xrt_typeof_str(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ")");
    } else {
        fprintf(out, "XR_FROM_INT(xrt_typeof_id(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, "))");
    }
}

static void xicgen_get_builtin(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                               const char *prefix) {
    (void) f;
    (void) prefix;
    if (v->aux_int == XR_GLOBAL_VAR_PROCESS || v->aux_int == XR_GLOBAL_VAR_FILE ||
        v->aux_int == XR_GLOBAL_VAR_DIR) {
        fprintf(out, "xrt_builtins[%d]", (int) v->aux_int);
    } else {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT builtin global '%s'\n",
                v->aux ? (const char *) v->aux : "?");
        emit_codegen_abort_expr(out);
    }
}

static bool xicgen_call_is_shared_class(const XiCgenCtx *ctx, const XiValue *callee) {
    if (!ctx || !callee)
        return false;
    if (callee->op == XI_GET_SHARED) {
        int slot = (int) callee->aux_int;
        return slot >= 0 && slot < ctx->shared_cap && ctx->shared_class[slot];
    }
    if ((callee->op == XI_BOX || callee->op == XI_UNBOX) && callee->nargs >= 1)
        return xicgen_call_is_shared_class(ctx, callee->args[0]);
    return false;
}

static bool xicgen_resolve_direct_class_ctor(const XiFunc *f, const XiValue *callee,
                                             const XiFunc **target) {
    if (!callee)
        return false;
    if (callee->op == XI_CLASS_CREATE && callee->aux) {
        const XiFunc *ctor = cg_find_constructor(f, (const XiClassData *) callee->aux);
        if (ctor) {
            *target = ctor;
            return true;
        }
    }
    if (callee->op == XI_BOX && callee->nargs >= 1)
        return xicgen_resolve_direct_class_ctor(f, callee->args[0], target);
    return false;
}

static bool xicgen_call_is_work_queue_constructor(const XiValue *callee) {
    const XiValue *origin = cg_unwrap_identity_value(callee);
    return origin && origin->op == XI_GET_BUILTIN && origin->aux_int == XR_GLOBAL_VAR_WORKQUEUE;
}

static void xicgen_call(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                        const char *prefix) {
    XR_DCHECK(v->nargs >= 1, "xicgen_call: need callee");
    XiValue *callee = v->args[0];
    CgStaticFunctionCall static_call = cg_resolve_static_function_call(ctx, f, callee);
    const XiFunc *target = static_call.func;
    const char *call_prefix = static_call.prefix;
    bool is_class_call = static_call.is_class_constructor ||
                         xicgen_call_is_shared_class(ctx, callee) ||
                         xicgen_resolve_direct_class_ctor(f, callee, &target);

    if (xicgen_call_is_work_queue_constructor(callee)) {
        fprintf(out, "xr_aot_work_queue_new(ctx, ");
        if (v->nargs >= 2)
            emit_value_as_rep(out, v->args[1], XR_REP_I64);
        else
            fprintf(out, "0");
        fprintf(out, ", ");
        if (v->nargs >= 3)
            emit_value_as_rep(out, v->args[2], XR_REP_I64);
        else
            fprintf(out, "0");
        fprintf(out, ")");
        return;
    }

    if (target && cg_func_needs_aot_coro(target)) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT sync call to suspendable function '%s'\n",
                target->name ? target->name : "?");
        emit_codegen_abort_expr(out);
        return;
    }

    if (target && is_class_call) {
        if (emit_class_native_constructor_boxed_expr(ctx, out, f, prefix, v, target, call_prefix))
            return;
        fprintf(out, "({ XrValue _inst = xrt_map_new(4); ");
        emit_fname(ctx, out, call_prefix ? call_prefix : prefix, target);
        fprintf(out, "(NULL, _inst");
        for (uint16_t a = 1; a < v->nargs; a++) {
            fprintf(out, ", ");
            emit_value_as_rep(out, v->args[a], XR_REP_TAGGED);
        }
        fprintf(out, "); _inst; })");
        return;
    }

    if (target) {
        const char *conv_suffix = emit_direct_call_return_conversion_prefix(ctx, out, f, v, target);
        if (ctx->error) {
            emit_codegen_abort_expr(out);
            return;
        }
        emit_fname(ctx, out, call_prefix ? call_prefix : prefix, target);
        fprintf(out, "(");
        emit_call_hidden_closure(out, f, target, callee);
        for (uint16_t a = 1; a < v->nargs; a++) {
            fprintf(out, ", ");
            emit_value_as_direct_call_arg(ctx, out, f, v, target, (uint16_t) (a - 1), v->args[a]);
        }
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return;
    }

    /* Indirect call through a closure value.  A function value is always a
     * closure whose stored `fn` is the boxed entry point
     * `XrValue (xrt_closure_t *, XrValue...)` (see emit_closure_new_expr), so
     * any closure can be invoked by casting fn to that signature, passing the
     * arguments boxed, and converting the boxed result to the call's rep.
     * Static targets are handled above; this covers function values flowing
     * through params / phis / containers / returns that cannot be statically
     * resolved.  Coroutine-suspending callees are not reachable here (they go
     * through the coroutine emitter). */
    {
        const XiValue *fn_val = cg_unwrap_identity_value(callee);
        if (fn_val && fn_val->type && XR_TYPE_IS_FUNCTION(fn_val->type)) {
            const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED,
                                                             cg_value_plan_storage_rep(ctx, v));
            fprintf(out, "({ xrt_closure_t *_icl = (xrt_closure_t *)");
            emit_value_as_rep(out, callee, XR_REP_TAGGED);
            fprintf(out, ".ptr; ((XrValue (*)(xrt_closure_t *");
            for (uint16_t a = 1; a < v->nargs; a++)
                fprintf(out, ", XrValue");
            fprintf(out, ")) _icl->fn)(_icl");
            for (uint16_t a = 1; a < v->nargs; a++) {
                fprintf(out, ", ");
                emit_value_as_rep(out, v->args[a], XR_REP_TAGGED);
            }
            fprintf(out, "); })");
            emit_conversion_suffix(out, conv_suffix);
            return;
        }
    }

    ctx->error = true;
    fprintf(stderr,
            "[xi_cgen] ERROR: unsupported AOT indirect call in %s at v%u: callee_op=%s "
            "slot=%" PRId64 "\n",
            f && f->name ? f->name : "?", v ? v->id : 0, callee ? xi_op_name(callee->op) : "?",
            callee ? callee->aux_int : -1);
    emit_codegen_abort_expr(out);
}

static void xicgen_emit_print_expr(FILE *out, const XiValue *v) {
    XR_DCHECK(v->nargs >= 1, "xicgen_emit_print_expr: missing print value");
    int flags = (int) v->aux_int;
    bool add_space = (flags & 1) != 0;
    bool newline = (flags & 2) != 0;
    if (add_space)
        fprintf(out, "(putchar(' '), ");
    fprintf(out, "%s(", newline ? "xrt_println" : "xrt_print");
    emit_vref(out, v->args[0]);
    fprintf(out, ")");
    if (add_space)
        fprintf(out, ")");
}

static void xicgen_print(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                         const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    xicgen_emit_print_expr(out, v);
}

static void xicgen_reject_unsupported(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                      const char *prefix) {
    (void) f;
    (void) prefix;
    ctx->error = true;
    fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT Xi op %s\n", xi_op_name(v->op));
    emit_codegen_abort_expr(out);
}

static void xicgen_chan_recv_status(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                    const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 1, "xicgen_chan_recv_status: missing recv value");
    fprintf(out, "xr_aot_recv_is_value(");
    emit_vref(out, v->args[0]);
    fprintf(out, ")");
}

static void xicgen_emit_json_set_field_expr(FILE *out, const XiValue *v) {
    XR_DCHECK(v->nargs >= 2, "xicgen_emit_json_set_field_expr: missing operands");
    fprintf(out, "xrt_json_set_field(");
    emit_vref(out, v->args[0]);
    fprintf(out, ", %d, ", (int) v->aux_int);
    emit_vref(out, v->args[1]);
    fprintf(out, ")");
}

static void xicgen_json_init_f(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                               const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    xicgen_emit_json_set_field_expr(out, v);
}

static void xicgen_json_get_f(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 1, "xicgen_json_get_f: missing object");
    fprintf(out, "xrt_json_get_field(");
    emit_vref(out, v->args[0]);
    fprintf(out, ", %d)", (int) v->aux_int);
}

static void xicgen_json_set_f(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    xicgen_emit_json_set_field_expr(out, v);
}

static int64_t xicgen_capacity_arg_or_default(const XiValue *v, int64_t fallback) {
    if (v && v->nargs >= 1 && v->args[0] && v->args[0]->op == XI_CONST)
        return v->args[0]->aux_int;
    return fallback;
}

static XrRep xicgen_value_c_storage_rep(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    if (ctx && f && !cg_func_needs_aot_coro_ctx(ctx, f) &&
        cg_array_value_uses_native_local(ctx, f, v))
        return XR_REP_PTR;
    return cg_value_plan_storage_rep(ctx, v);
}

static void xicgen_array_new(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                             const char *prefix) {
    (void) prefix;
    int64_t cap = xicgen_capacity_arg_or_default(v, 4);
    if (xicgen_value_c_storage_rep(ctx, f, v) == XR_REP_PTR) {
        if (!emit_typed_array_new_ptr_expr(ctx, out, f, v, cap))
            fprintf(out, "(xrt_array_t*)xrt_array_new(%" PRId64 ").ptr", cap);
    } else if (!emit_typed_array_new_expr(ctx, out, f, v, cap)) {
        fprintf(out, "xrt_array_new(%" PRId64 ")", cap);
    }
}

static void xicgen_map_new(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                           const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    int64_t cap = xicgen_capacity_arg_or_default(v, 8);
    uint8_t flags = (uint8_t) ((v ? v->aux_int : 0) & 0x02);
    if (!flags && !emit_typed_map_new_expr(ctx, out, v, cap))
        fprintf(out, "xrt_map_new(%" PRId64 ")", cap);
    else if (flags)
        fprintf(out, "xrt_map_new_flags(%" PRId64 ", XR_MAP_FLAG_WEAK)", cap);
}

static void xicgen_set_new(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                           const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    int64_t cap = xicgen_capacity_arg_or_default(v, 8);
    uint8_t flags = (uint8_t) ((v ? v->aux_int : 0) & 0x02);
    if (!flags && !emit_typed_set_new_expr(ctx, out, v, cap))
        fprintf(out, "xrt_set_new(%" PRId64 ")", cap);
    else if (flags)
        fprintf(out, "xrt_set_new_flags(%" PRId64 ", XR_SET_FLAG_WEAK)", cap);
}

static void xicgen_str_concat(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    (void) f;
    (void) prefix;
    emit_str_concat_expr(ctx, out, v);
}

static void xicgen_as(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                      const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 1, "xicgen_as: need arg");

    bool is_safe = (v->aux_int & 1) != 0;
    int32_t tid = (int32_t) (v->aux_int >> 1);
    if (tid < 0) {
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        return;
    }

    if (!is_safe) {
        switch (tid) {
            case 8:
                fprintf(out, "xrt_to_int(");
                emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
                fprintf(out, ")");
                return;
            case 11:
                fprintf(out, "xrt_to_float(");
                emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
                fprintf(out, ")");
                return;
            case 12:
                fprintf(out, "xrt_to_string(");
                emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
                fprintf(out, ")");
                return;
            case 1:
                fprintf(out, "xrt_to_bool(");
                emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
                fprintf(out, ")");
                return;
            default:
                break;
        }
    }

    const char *tname = v->aux ? (const char *) v->aux : "unknown";
    char err_buf[128];
    snprintf(err_buf, sizeof(err_buf), "Type cast failed: expected %s", tname);
    fprintf(out, "({ XrValue _as = ");
    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
    fprintf(out, "; (xrt_typeof_id(_as) == %" PRId32 ") ? _as : ", tid);
    if (is_safe) {
        fprintf(out, "XR_NULL_VAL; })");
    } else {
        fprintf(out, "(xrt_throw_exc(");
        cg_emit_str_value(ctx, out, err_buf);
        fprintf(out, "), XR_NULL_VAL); })");
    }
}

static void xicgen_slice(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                         const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 3, "xicgen_slice: need source, start, and end");
    fprintf(out, "xrt_slice(");
    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep(out, v->args[2], XR_REP_TAGGED);
    fprintf(out, ")");
}

static void xicgen_range(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                         const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 2, "xicgen_range: need start and end");
    fprintf(out, "xrt_range_from_i64(");
    emit_value_as_rep(out, v->args[0], XR_REP_I64);
    fprintf(out, ", ");
    emit_value_as_rep(out, v->args[1], XR_REP_I64);
    fprintf(out, ")");
}

static void xicgen_call_builtin(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                const char *prefix) {
    const char *bn = v->aux ? (const char *) v->aux : "";

    if (strcmp(bn, "print") == 0) {
        xicgen_emit_print_expr(out, v);
    } else if (strcmp(bn, "str_concat") == 0) {
        xicgen_str_concat(ctx, out, f, v, prefix);
    } else if (strcmp(bn, "array_new") == 0) {
        xicgen_array_new(ctx, out, f, v, prefix);
    } else if (strcmp(bn, "Bytes") == 0) {
        if (xicgen_value_c_storage_rep(ctx, f, v) == XR_REP_PTR) {
            if (!emit_bytes_new_native_local_expr(out, v)) {
                if (v->nargs == 0) {
                    fprintf(out, "(xrt_array_t*)xrt_bytes_new_len(0).ptr");
                } else if (v->nargs == 1) {
                    fprintf(out, "(xrt_array_t*)xrt_bytes_new_1(");
                    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
                    fprintf(out, ").ptr");
                } else {
                    emit_codegen_abort_expr(out);
                    ctx->error = true;
                }
            }
        } else if (v->nargs == 0) {
            fprintf(out, "xrt_bytes_new_len(0)");
        } else if (v->nargs == 1) {
            if (v->args[0] && v->args[0]->type && v->args[0]->type->kind == XR_KIND_INT) {
                fprintf(out, "xrt_bytes_new_len(");
                emit_value_as_rep(out, v->args[0], XR_REP_I64);
                fprintf(out, ")");
            } else {
                fprintf(out, "xrt_bytes_new_1(");
                emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
                fprintf(out, ")");
            }
        } else if (v->nargs == 2) {
            fprintf(out, "xrt_bytes_new_fill(");
            emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
            fprintf(out, ", ");
            emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
            fprintf(out, ")");
        } else {
            emit_codegen_abort_expr(out);
            ctx->error = true;
        }
    } else if (emit_array_bytes_builtin_expr(ctx, out, v, bn)) {
        /* Expression emitted by the array/bytes helper. */
    } else if (strcmp(bn, "StringBuilder") == 0) {
        fprintf(out, "xrt_strbuf_new()");
    } else if (strcmp(bn, "map_new") == 0) {
        xicgen_map_new(ctx, out, f, v, prefix);
    } else if (strcmp(bn, "set_new") == 0) {
        xicgen_set_new(ctx, out, f, v, prefix);
    } else if (strcmp(bn, "json_new") == 0) {
        int64_t fc = v->aux_int > 0 ? v->aux_int : 0;
        fprintf(out, "xrt_json_new(%" PRId64 ")", fc);
    } else if (strcmp(bn, "json_init_f") == 0 || strcmp(bn, "json_set_f") == 0) {
        xicgen_emit_json_set_field_expr(out, v);
    } else if (strcmp(bn, "json_get_f") == 0) {
        fprintf(out, "xrt_json_get_field(");
        emit_vref(out, v->args[0]);
        fprintf(out, ", %d)", (int) v->aux_int);
    } else if (strcmp(bn, "iter_new") == 0) {
        XR_DCHECK(v->nargs >= 1, "builtin iter_new: need arg");
        fprintf(out, "xrt_method_0(");
        emit_vref(out, v->args[0]);
        fprintf(out, ", %d)", XRT_SYM_ITERATOR);
    } else if (strcmp(bn, "iter_valid") == 0) {
        XR_DCHECK(v->nargs >= 1, "builtin iter_valid: need arg");
        fprintf(out, "xr_truthy(xrt_method_0(");
        emit_vref(out, v->args[0]);
        fprintf(out, ", %d))", XRT_SYM_HAS_NEXT);
    } else if (strcmp(bn, "iter_next") == 0) {
        XR_DCHECK(v->nargs >= 1, "builtin iter_next: need arg");
        fprintf(out, "xrt_method_0(");
        emit_vref(out, v->args[0]);
        fprintf(out, ", %d)", XRT_SYM_NEXT);
    } else if (strcmp(bn, "slice") == 0) {
        xicgen_slice(ctx, out, f, v, prefix);
    } else if (strcmp(bn, "range") == 0) {
        xicgen_range(ctx, out, f, v, prefix);
    } else if (strcmp(bn, "typeof") == 0) {
        xicgen_typeof(ctx, out, f, v, prefix);
    } else if (strcmp(bn, "regex_compile") == 0) {
        XR_DCHECK(v->nargs >= 2, "builtin regex_compile: need 2 args");
        fprintf(out, "xr_regex_compile_literal(iso, ");
        emit_vref(out, v->args[0]);
        fprintf(out, ", ");
        emit_vref(out, v->args[1]);
        fprintf(out, ")");
    } else {
        fprintf(stderr, "[xi_cgen] ERROR: unknown builtin '%s'\n", bn);
        emit_codegen_abort_expr(out);
        ctx->error = true;
    }
}

static const XiFunc *xicgen_lookup_super_method(XiCgenCtx *ctx, const XiFunc *f, const char *method,
                                                const char **method_prefix) {
    if (!ctx || !ctx->module)
        return NULL;
    const char *parent_class = NULL;
    XiModule *mod = ctx->module;
    for (uint16_t s = 0; s < mod->nslots && !parent_class; s++) {
        const XiClassData *cd = mod->slot_classes ? mod->slot_classes[s] : NULL;
        if (!cd || !cd->super_name)
            continue;
        for (uint16_t ci = 0; ci < cd->ninst + cd->nstat; ci++) {
            if (cd->child_idx && cd->child_idx[ci] < mod->init->nchildren &&
                mod->init->children[cd->child_idx[ci]] == f) {
                parent_class = cd->super_name;
                break;
            }
        }
    }
    if (!parent_class)
        return NULL;
    if (method && strcmp(method, "constructor") == 0)
        return cg_lookup_class_ctor(ctx, parent_class);
    return cg_lookup_method(ctx, method, parent_class, method_prefix);
}

static const XiFunc *xicgen_lookup_receiver_method(XiCgenCtx *ctx, const XiFunc *f,
                                                   const XiValue *v, const char *method,
                                                   const char **method_prefix) {
    const char *recv_class = cg_class_native_receiver_class_name(ctx, f, v->args[0]);
    if (v->op == XI_CALL_METHOD_DIRECT)
        return cg_lookup_method_by_index(ctx, recv_class, (int) v->aux_int, method_prefix);
    return cg_lookup_method(ctx, method, recv_class, method_prefix);
}

static bool xicgen_emit_time_method(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v) {
    if (!cg_is_time_module_call_ctx(ctx, f, v))
        return false;
    const char *method = (const char *) v->aux;
    const char *time_helper = cg_time_module_helper_ctx(ctx, f, v);
    if (!time_helper) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT time method '%s'\n",
                method ? method : "?");
        emit_codegen_abort_expr(out);
        return true;
    }
    if (cg_rep(v) == XR_REP_I64)
        fprintf(out, "XR_TO_INT(");
    else if (cg_rep(v) == XR_REP_F64)
        fprintf(out, "XR_TO_FLOAT(");
    fprintf(out, "%s()", time_helper);
    if (cg_rep(v) == XR_REP_I64 || cg_rep(v) == XR_REP_F64)
        fprintf(out, ")");
    return true;
}

static bool xicgen_emit_typed_array_method(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                           const XiValue *v, const char *prefix, const char *method,
                                           uint16_t nargs) {
    if (nargs == 0 && method && strcmp(method, "length") == 0 &&
        emit_typed_array_length_expr(ctx, out, f, prefix, v))
        return true;
    if (nargs == 1 && method && strcmp(method, "push") == 0 &&
        emit_typed_array_push_expr(ctx, out, f, prefix, v, v->args[0], v->args[1]))
        return true;
    if (!method)
        return false;
    return (nargs == 1 && strcmp(method, "map") == 0 &&
            emit_typed_array_map_expr(ctx, out, f, prefix, v)) ||
           (nargs == 1 && strcmp(method, "filter") == 0 &&
            emit_typed_array_filter_expr(ctx, out, f, prefix, v)) ||
           (nargs == 2 && strcmp(method, "reduce") == 0 &&
            emit_typed_array_reduce_expr(ctx, out, f, prefix, v));
}

static bool xicgen_emit_enum_method(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                    const char *method) {
    const XiEnumData *recv_enum = cg_enum_for_shared_value(ctx, v->args[0]);
    int enum_member = cg_enum_member_index(recv_enum, method);
    if (!recv_enum || enum_member < 0)
        return false;
    if (recv_enum->is_adt && recv_enum->members &&
        recv_enum->members[enum_member].payload_count > 0) {
        emit_adt_enum_construct_expr(out, enum_member, v);
    } else {
        fprintf(out, "xrt_map_get((xrt_map_t*)");
        emit_vref(out, v->args[0]);
        fprintf(out, ".ptr, ");
        cg_emit_str_value(ctx, out, method);
        fprintf(out, ")");
    }
    return true;
}

static bool xicgen_emit_task_method(XiCgenCtx *ctx, FILE *out, const XiValue *v, const char *method,
                                    uint16_t nargs) {
    if (v->op != XI_CALL_METHOD || !cg_value_type_is_task(v->args[0]))
        return false;
    if (nargs == 0 && method && strcmp(method, "cancel") == 0) {
        if (cg_rep(v) == XR_REP_I64)
            fprintf(out, "XR_TO_INT(");
        else if (cg_rep(v) == XR_REP_F64)
            fprintf(out, "XR_TO_FLOAT(");
        fprintf(out, "xr_aot_task_cancel(NULL, ");
        emit_vref(out, v->args[0]);
        fprintf(out, ")");
        if (cg_rep(v) == XR_REP_I64 || cg_rep(v) == XR_REP_F64)
            fprintf(out, ")");
    } else if (nargs == 0 && method && strcmp(method, "poll") == 0) {
        if (cg_rep(v) == XR_REP_I64)
            fprintf(out, "XR_TO_INT(");
        else if (cg_rep(v) == XR_REP_F64)
            fprintf(out, "XR_TO_FLOAT(");
        if (cg_rep(v) == XR_REP_TAGGED)
            fprintf(out, "xr_aot_bridge_value_to_xrt(");
        fprintf(out, "xr_aot_task_poll(NULL, ");
        emit_vref(out, v->args[0]);
        fprintf(out, ")");
        if (cg_rep(v) == XR_REP_TAGGED)
            fprintf(out, ")");
        if (cg_rep(v) == XR_REP_I64 || cg_rep(v) == XR_REP_F64)
            fprintf(out, ")");
    } else {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT Task method '%s'\n",
                method ? method : "?");
        emit_codegen_abort_expr(out);
    }
    return true;
}

static bool xicgen_emit_direct_method(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                      const char *prefix, const XiFunc *mfunc,
                                      const char *method_prefix) {
    if (!mfunc)
        return false;
    if (cg_func_needs_aot_coro(mfunc)) {
        ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: unsupported AOT sync method call to suspendable function '%s'\n",
                mfunc->name ? mfunc->name : "?");
        emit_codegen_abort_expr(out);
        return true;
    }
    if (emit_class_native_method_call_expr(ctx, out, f, prefix, v, mfunc, method_prefix))
        return true;
    const char *conv_suffix = emit_direct_call_return_conversion_prefix(ctx, out, f, v, mfunc);
    if (ctx->error) {
        emit_codegen_abort_expr(out);
        return true;
    }
    emit_fname(ctx, out, method_prefix ? method_prefix : prefix, mfunc);
    fprintf(out, "(NULL");
    for (uint16_t a = 0; a < v->nargs; a++) {
        fprintf(out, ", ");
        emit_value_as_direct_call_arg(ctx, out, f, v, mfunc, a, v->args[a]);
    }
    fprintf(out, ")");
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool xicgen_emit_stringbuilder_append(FILE *out, const XiValue *v, const char *method,
                                             uint16_t nargs) {
    const XrType *recv_type = v->nargs > 0 && v->args[0] ? v->args[0]->type : NULL;
    bool recv_is_stringbuilder = recv_type && recv_type->kind == XR_KIND_INSTANCE &&
                                 recv_type->instance.class_name &&
                                 strcmp(recv_type->instance.class_name, "StringBuilder") == 0;
    if (!recv_is_stringbuilder || !method || strcmp(method, "append") != 0 || nargs != 1)
        return false;
    fprintf(out, "(xrt_strbuf_append(");
    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
    fprintf(out, "), ");
    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ")");
    return true;
}

static void xicgen_emit_runtime_method(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                       const char *method, uint16_t nargs) {
    int sym = cg_method_sym(method);
    if (sym < 0 && xicgen_emit_stringbuilder_append(out, v, method, nargs))
        return;
    if (sym < 0) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT method '%s'\n", method ? method : "?");
        emit_codegen_abort_expr(out);
        return;
    }
    if (emit_class_native_map_method_call_expr(ctx, out, f, v))
        return;
    if (emit_class_native_set_method_call_expr(ctx, out, f, v))
        return;
    const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
    if (nargs == 0) {
        fprintf(out, "xrt_method_0(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", %d)", sym);
    } else if (nargs == 1) {
        fprintf(out, "xrt_method_1(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", %d, ", sym);
        emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ")");
    } else if (nargs == 2) {
        fprintf(out, "xrt_method_2(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", %d, ", sym);
        emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[2], XR_REP_TAGGED);
        fprintf(out, ")");
    } else {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT method call with %u args\n",
                (unsigned) nargs);
        emit_codegen_abort_expr(out);
    }
    emit_conversion_suffix(out, conv_suffix);
}

static void xicgen_call_method(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                               const char *prefix) {
    XR_DCHECK(v->nargs >= 1, "xicgen_call_method: need receiver");
    const char *method = (const char *) v->aux;
    bool is_super = v->op == XI_CALL_METHOD && (v->aux_int & 1) != 0;
    const XiFunc *mfunc = NULL;
    const char *method_prefix = NULL;

    if (xicgen_emit_time_method(ctx, out, f, v))
        return;
    if (is_super)
        mfunc = xicgen_lookup_super_method(ctx, f, method, &method_prefix);
    else
        mfunc = xicgen_lookup_receiver_method(ctx, f, v, method, &method_prefix);

    uint16_t nargs = (uint16_t) (v->nargs - 1);
    if (xicgen_emit_typed_array_method(ctx, out, f, v, prefix, method, nargs))
        return;
    if (xicgen_emit_enum_method(ctx, out, v, method))
        return;
    if (xicgen_emit_task_method(ctx, out, v, method, nargs))
        return;
    if (xicgen_emit_direct_method(ctx, out, f, v, prefix, mfunc, method_prefix))
        return;
    xicgen_emit_runtime_method(ctx, out, f, v, method, nargs);
}

static void xicgen_class_create(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    const XiClassData *cd = (const XiClassData *) v->aux;
    if (!cd) {
        fprintf(out, "XR_NULL_VAL /* class descriptor: no data */");
        return;
    }
    const char *name = cd->class_name ? cd->class_name : "?";
    if (cd->is_monomorphized && cd->display_name) {
        fprintf(out, "({ ");
        if (cd->mono_type_arg_count > 0 && cd->mono_type_arg_names) {
            fprintf(out, "static const char *_ta_%s[] = {", name);
            for (int ti = 0; ti < cd->mono_type_arg_count; ti++) {
                fprintf(out, "%s\"%s\"", ti > 0 ? ", " : "",
                        cd->mono_type_arg_names[ti] ? cd->mono_type_arg_names[ti] : "unknown");
            }
            fprintf(out, "}; ");
        }
        fprintf(out, "uint16_t _tid = xrt_type_register(\"%s\", 0, NULL, 0, NULL, 0); ", name);
        fprintf(out,
                "uint16_t _orig = 0; "
                "for (uint16_t _i = 1; _i < xrt_type_count; _i++) "
                "{ if (xrt_type_table[_i].name && strcmp(xrt_type_table[_i].name, \"%s\") == 0) "
                "{ _orig = _i; break; } } ",
                cd->display_name);
        fprintf(out, "xrt_type_set_generic(_tid, _orig, \"%s\", ", cd->display_name);
        if (cd->mono_type_arg_count > 0 && cd->mono_type_arg_names) {
            fprintf(out, "_ta_%s, %d", name, cd->mono_type_arg_count);
        } else {
            fprintf(out, "NULL, 0");
        }
        fprintf(out, "); XR_FROM_INT(_tid); })");
    } else {
        fprintf(out, "XR_FROM_INT(xrt_type_register(\"%s\", 0, NULL, 0, NULL, 0))", name);
    }
}

static void xicgen_throw(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                         const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 1, "xicgen_throw: need arg");
    fprintf(out, "xrt_throw_exc(");
    emit_vref(out, v->args[0]);
    fprintf(out, ")");
}

static void xicgen_ownership_call(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                  const char *prefix, const char *fn_name) {
    (void) ctx;
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 1, "xicgen_ownership_call: need arg");
    fprintf(out, "%s(", fn_name);
    emit_vref(out, v->args[0]);
    fprintf(out, ")");
}

static void xicgen_retain(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                          const char *prefix) {
    xicgen_ownership_call(ctx, out, f, v, prefix, "xrt_retain");
}

static void xicgen_release(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                           const char *prefix) {
    xicgen_ownership_call(ctx, out, f, v, prefix, "xrt_release");
}

static void xicgen_stack_alloc(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                               const char *prefix) {
    (void) f;
    int32_t orig_op = v->aux_int;
    if (orig_op == XI_ARRAY_NEW) {
        int64_t cap =
            (v->nargs >= 1 && v->args[0] && v->args[0]->op == XI_CONST) ? v->args[0]->aux_int : 4;
        fprintf(out, "xrt_array_stack_new(%" PRId64 ")", cap);
    } else if (orig_op == XI_MAP_NEW) {
        /* map: fallback to heap until stack map storage is available */
        int64_t cap =
            (v->nargs >= 1 && v->args[0] && v->args[0]->op == XI_CONST) ? v->args[0]->aux_int : 8;
        uint8_t flags = (uint8_t) (v->aux_int & 0x02);
        if (flags)
            fprintf(out, "xrt_map_new_flags(%" PRId64 ", XR_MAP_FLAG_WEAK)", cap);
        else if (!emit_typed_map_new_expr(ctx, out, v, cap))
            fprintf(out, "xrt_map_new(%" PRId64 ")", cap);
    } else if (orig_op == XI_SET_NEW) {
        int64_t cap =
            (v->nargs >= 1 && v->args[0] && v->args[0]->op == XI_CONST) ? v->args[0]->aux_int : 8;
        uint8_t flags = (uint8_t) (v->aux_int & 0x02);
        if (flags)
            fprintf(out, "xrt_set_new_flags(%" PRId64 ", XR_SET_FLAG_WEAK)", cap);
        else if (!emit_typed_set_new_expr(ctx, out, v, cap))
            fprintf(out, "xrt_set_new(%" PRId64 ")", cap);
    } else if (orig_op == XI_STR_CONCAT) {
        emit_str_concat_expr(ctx, out, v);
    } else if (orig_op == XI_CLOSURE_NEW) {
        emit_closure_new_expr(ctx, out, f, prefix, v);
    } else {
        emit_codegen_abort_expr(out);
    }
}

static void xicgen_json_new(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                            const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    int64_t field_count = v->aux_int > 0 ? v->aux_int : 0;
    fprintf(out, "xrt_json_new(%" PRId64 ")", field_count);
}

static void xicgen_struct_new(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    (void) ctx;
    XR_DCHECK(v->nargs >= 1, "xicgen_struct_new: need class arg");
    if (cg_struct_can_inline(f, v)) {
        /* Inlined struct initialization is emitted by the statement path. */
        fprintf(out, "XR_NULL_VAL");
    } else {
        emit_struct_fallback_new_expr(out, (XrStructLayout *) v->aux, prefix);
    }
}

static void xicgen_struct_get(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    XR_DCHECK(v->nargs >= 1, "xicgen_struct_get: need struct arg");
    const XiValue *origin = cg_trace_struct_new(v->args[0]);
    if (origin && cg_struct_can_inline(f, origin)) {
        emit_struct_inline_field_get_expr(out, (XrStructLayout *) origin->aux, origin, v->aux_int);
    } else {
        XrStructLayout *sl = (XrStructLayout *) v->aux;
        emit_struct_fallback_field_get(ctx, out, f, sl, v->aux_int, v->args[0], v->type, cg_rep(v),
                                       prefix);
    }
}

static void xicgen_struct_set(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    XR_DCHECK(v->nargs >= 2, "xicgen_struct_set: need struct + value");
    const XiValue *origin = cg_trace_struct_new(v->args[0]);
    if (origin && cg_struct_can_inline(f, origin)) {
        emit_struct_inline_field_set_expr(out, (XrStructLayout *) origin->aux, origin, v->aux_int,
                                          v->args[1]);
    } else {
        XrStructLayout *sl = (XrStructLayout *) v->aux;
        emit_struct_fallback_field_set(ctx, out, f, sl, v->aux_int, v->args[0], v->args[1], prefix);
    }
}

static void xicgen_shl(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                       const char *prefix) {
    (void) f;
    (void) prefix;
    emit_shift_binop_ctx(ctx, out, v, "xrt_i64_shl");
}

static void xicgen_shr(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                       const char *prefix) {
    (void) f;
    (void) prefix;
    emit_shift_binop_ctx(ctx, out, v, "xrt_i64_shr");
}

static void xicgen_compare(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                           const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    XrRep a0_rep = cg_rep(v->args[0]);
    XrRep a1_rep = cg_rep(v->args[1]);
    XrRep arg_rep = (a0_rep == XR_REP_TAGGED || a1_rep == XR_REP_TAGGED) ? XR_REP_TAGGED : a0_rep;
    if (arg_rep == XR_REP_TAGGED) {
        switch (v->op) {
            case XI_EQ:
                fprintf(out, "xrt_eq(");
                break;
            case XI_NE:
                fprintf(out, "!xrt_eq(");
                break;
            case XI_LT:
                fprintf(out, "xrt_lt(");
                break;
            case XI_LE:
                fprintf(out, "xrt_le(");
                break;
            case XI_GT:
                fprintf(out, "xrt_lt(");
                break;
            case XI_GE:
                fprintf(out, "xrt_le(");
                break;
            default:
                XR_CHECK(false, "xicgen_compare: unsupported tagged compare op");
        }
        if (v->op == XI_GT || v->op == XI_GE) {
            emit_boxed_value_ref(out, v->args[1]);
            fprintf(out, ", ");
            emit_boxed_value_ref(out, v->args[0]);
        } else {
            emit_boxed_value_ref(out, v->args[0]);
            fprintf(out, ", ");
            emit_boxed_value_ref(out, v->args[1]);
        }
        fprintf(out, ")");
    } else {
        const char *op = NULL;
        switch (v->op) {
            case XI_EQ:
                op = "==";
                break;
            case XI_NE:
                op = "!=";
                break;
            case XI_LT:
                op = "<";
                break;
            case XI_LE:
                op = "<=";
                break;
            case XI_GT:
                op = ">";
                break;
            case XI_GE:
                op = ">=";
                break;
            default:
                XR_CHECK(false, "xicgen_compare: unsupported native compare op");
        }
        emit_binop(out, v, op);
    }
}

static void xicgen_eq(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                      const char *prefix) {
    xicgen_compare(ctx, out, f, v, prefix);
}

static void xicgen_ne(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                      const char *prefix) {
    xicgen_compare(ctx, out, f, v, prefix);
}

static void xicgen_strict_compare(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                  const char *prefix, const char *op) {
    (void) ctx;
    (void) f;
    (void) prefix;
    fprintf(out, "(");
    emit_vref(out, v->args[0]);
    fprintf(out, ".i %s ", op);
    emit_vref(out, v->args[1]);
    fprintf(out, ".i)");
}

static void xicgen_eq_strict(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                             const char *prefix) {
    xicgen_strict_compare(ctx, out, f, v, prefix, "==");
}

static void xicgen_ne_strict(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                             const char *prefix) {
    xicgen_strict_compare(ctx, out, f, v, prefix, "!=");
}

static void xicgen_lt(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                      const char *prefix) {
    xicgen_compare(ctx, out, f, v, prefix);
}

static void xicgen_le(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                      const char *prefix) {
    xicgen_compare(ctx, out, f, v, prefix);
}

static void xicgen_gt(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                      const char *prefix) {
    xicgen_compare(ctx, out, f, v, prefix);
}

static void xicgen_ge(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                      const char *prefix) {
    xicgen_compare(ctx, out, f, v, prefix);
}

static void xicgen_cast_i64_arg(FILE *out, const XiValue *v, const char *ctype) {
    fprintf(out, "(int64_t)(%s)", ctype);
    emit_vref(out, v->args[0]);
}

static void xicgen_narrow_i8(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                             const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    xicgen_cast_i64_arg(out, v, "int8_t");
}

static void xicgen_narrow_u8(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                             const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    xicgen_cast_i64_arg(out, v, "uint8_t");
}

static void xicgen_narrow_i16(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    xicgen_cast_i64_arg(out, v, "int16_t");
}

static void xicgen_narrow_u16(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    xicgen_cast_i64_arg(out, v, "uint16_t");
}

static void xicgen_narrow_i32(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    xicgen_cast_i64_arg(out, v, "int32_t");
}

static void xicgen_narrow_u32(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    xicgen_cast_i64_arg(out, v, "uint32_t");
}

static void xicgen_f32_roundtrip(FILE *out, const XiValue *v, bool preserve_loaded_float32) {
    fputs(preserve_loaded_float32 ? "" : "(double)(float)", out);
    emit_vref(out, v->args[0]);
}

static void xicgen_narrow_f32(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    xicgen_f32_roundtrip(out, v, false);
}

static void xicgen_widen_i8(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                            const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    xicgen_cast_i64_arg(out, v, "int8_t");
}

static void xicgen_widen_u8(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                            const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    xicgen_cast_i64_arg(out, v, "uint8_t");
}

static void xicgen_widen_i16(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                             const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    xicgen_cast_i64_arg(out, v, "int16_t");
}

static void xicgen_widen_u16(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                             const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    xicgen_cast_i64_arg(out, v, "uint16_t");
}

static void xicgen_widen_i32(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                             const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    xicgen_cast_i64_arg(out, v, "int32_t");
}

static void xicgen_widen_u32(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                             const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    xicgen_cast_i64_arg(out, v, "uint32_t");
}

static void xicgen_widen_f32(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                             const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    xicgen_f32_roundtrip(out, v, cg_array_index_get_reads_f32_storage(ctx, f, v->args[0]));
}

static void xicgen_isnull(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                          const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    fprintf(out, "(");
    emit_vref(out, v->args[0]);
    fprintf(out, ".tag == XR_TAG_NULL)");
}

static void xicgen_box(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                       const char *prefix) {
    (void) prefix;
    if (v->nargs < 1 || !v->args[0]) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: XI_BOX has no input\n");
        emit_codegen_abort_expr(out);
        return;
    }
    if (!cg_value_boundary_step(ctx, f, v, v->args[0], XAOT_BOUNDARY_BOX)) {
        emit_codegen_abort_expr(out);
        return;
    }
    struct XrType *sty = v->args[0]->type;
    XrRep from_rep = xicgen_value_c_storage_rep(ctx, f, v->args[0]);
    const char *conv_suffix = emit_conversion_prefix(out, sty, from_rep, XR_REP_TAGGED);
    emit_vref(out, v->args[0]);
    emit_conversion_suffix(out, conv_suffix);
}

static void xicgen_unbox(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                         const char *prefix) {
    (void) prefix;
    if (v->nargs < 1 || !v->args[0]) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: XI_UNBOX has no input\n");
        emit_codegen_abort_expr(out);
        return;
    }
    if (!cg_value_boundary_step(ctx, f, v, v->args[0], XAOT_BOUNDARY_UNBOX)) {
        emit_codegen_abort_expr(out);
        return;
    }
    XrRep from_rep = xicgen_value_c_storage_rep(ctx, f, v->args[0]);
    XrRep to_rep = xicgen_value_c_storage_rep(ctx, f, v);
    const char *conv_suffix = emit_conversion_prefix(out, v->type, from_rep, to_rep);
    emit_vref(out, v->args[0]);
    emit_conversion_suffix(out, conv_suffix);
}

static void xicgen_is(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                      const char *prefix) {
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 1, "xicgen_is: missing arg");
    struct XrType *target = (struct XrType *) v->aux;
    if (!target) {
        fprintf(out, "0 /* XI_IS: NULL target type */");
        return;
    }
    switch (target->kind) {
        case XR_KIND_INT:
            fprintf(out, "(");
            emit_vref(out, v->args[0]);
            fprintf(out, ".tag == %u)", XR_TAG_I64);
            break;
        case XR_KIND_FLOAT:
            fprintf(out, "(");
            emit_vref(out, v->args[0]);
            fprintf(out, ".tag == %u)", XR_TAG_F64);
            break;
        case XR_KIND_BOOL:
            fprintf(out, "(");
            emit_vref(out, v->args[0]);
            fprintf(out, ".tag == %u)", XR_TAG_BOOL);
            break;
        case XR_KIND_NULL:
            fprintf(out, "(");
            emit_vref(out, v->args[0]);
            fprintf(out, ".tag == %u)", XR_TAG_NULL);
            break;
        case XR_KIND_STRING:
            fprintf(out, "XR_IS_STR(");
            emit_vref(out, v->args[0]);
            fprintf(out, ")");
            break;
        case XR_KIND_INSTANCE:
        case XR_KIND_CLASS: {
            const char *cname = target->instance.class_name;
            int slot = cg_find_class_slot(ctx, cname);
            if (slot >= 0) {
                fprintf(out, "xrt_instanceof(");
                emit_vref(out, v->args[0]);
                fprintf(out, ", (uint16_t)%s[%d].i)", ctx->shared_name, slot);
            } else {
                fprintf(out, "(");
                emit_vref(out, v->args[0]);
                fprintf(out, ".tag == %u) /* is %s: class not resolved */", (unsigned) XR_TAG_PTR,
                        cname ? cname : "?");
            }
            break;
        }
        default: {
            uint8_t tag = xr_type_to_xr_tag(target);
            if (tag != 0xFF) {
                fprintf(out, "(");
                emit_vref(out, v->args[0]);
                fprintf(out, ".tag == %u)", (unsigned) tag);
            } else {
                fprintf(out, "0 /* unsupported is-check */");
            }
            break;
        }
    }
}

static void xicgen_load_field(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    XR_DCHECK(v->nargs >= 1, "xicgen_load_field: need object");
    if (emit_class_cached_field_load_expr(ctx, out, v))
        return;
    if (emit_class_native_receiver_field_load_expr(ctx, out, f, v))
        return;
    const char *field = (const char *) v->aux;
    if (!field && v->aux_int >= 0) {
        const char *conv_suffix =
            emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_value_plan_storage_rep(ctx, v));
        fprintf(out, "xrt_index_get(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", XR_FROM_INT(%" PRId64 "))", v->aux_int);
        emit_conversion_suffix(out, conv_suffix);
        return;
    }
    const char *task_helper =
        cg_value_type_is_task(v->args[0]) ? cg_task_field_helper(field) : NULL;
    if (task_helper) {
        if (cg_rep(v) == XR_REP_I64)
            fprintf(out, "XR_TO_INT(");
        else if (cg_rep(v) == XR_REP_F64)
            fprintf(out, "XR_TO_FLOAT(");
        if (cg_task_field_needs_xrt_bridge(field))
            fprintf(out, "xr_aot_bridge_value_to_xrt(");
        fprintf(out, "%s(NULL, ", task_helper);
        emit_vref(out, v->args[0]);
        fprintf(out, ")");
        if (cg_task_field_needs_xrt_bridge(field))
            fprintf(out, ")");
        if (cg_rep(v) == XR_REP_I64 || cg_rep(v) == XR_REP_F64)
            fprintf(out, ")");
        return;
    }
    const char *channel_helper =
        cg_value_type_is_channel(v->args[0]) ? cg_channel_field_helper(field) : NULL;
    if (channel_helper) {
        if (cg_rep(v) == XR_REP_I64)
            fprintf(out, "XR_TO_INT(");
        else if (cg_rep(v) == XR_REP_F64)
            fprintf(out, "XR_TO_FLOAT(");
        fprintf(out, "%s(NULL, ", channel_helper);
        emit_vref(out, v->args[0]);
        fprintf(out, ")");
        if (cg_rep(v) == XR_REP_I64 || cg_rep(v) == XR_REP_F64)
            fprintf(out, ")");
        return;
    }
    if (field && (strcmp(field, "length") == 0 || strcmp(field, "size") == 0) &&
        emit_class_native_map_length_expr(ctx, out, f, v))
        return;
    if (field && (strcmp(field, "length") == 0 || strcmp(field, "size") == 0) &&
        emit_class_native_set_length_expr(ctx, out, f, v))
        return;
    if (field && (strcmp(field, "length") == 0 || strcmp(field, "size") == 0) &&
        emit_typed_array_length_expr(ctx, out, f, prefix, v))
        return;
    int sym = cg_method_sym(field);
    if (sym >= 0) {
        const char *conv_suffix =
            emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_value_plan_storage_rep(ctx, v));
        fprintf(out, "xrt_getprop(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", %d)", sym);
        emit_conversion_suffix(out, conv_suffix);
    } else {
        const char *conv_suffix =
            emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_value_plan_storage_rep(ctx, v));
        fprintf(out, "xrt_map_get((xrt_map_t*)");
        emit_vref(out, v->args[0]);
        fprintf(out, ".ptr, ");
        cg_emit_str_value(ctx, out, field ? field : "?");
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
    }
}

static void xicgen_store_field(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                               const char *prefix) {
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 2, "xicgen_store_field: need object and value");
    if (emit_class_cached_field_store_expr(ctx, out, v))
        return;
    if (emit_class_native_receiver_field_store_expr(ctx, out, f, v))
        return;
    const char *field = (const char *) v->aux;
    fprintf(out, "(xrt_map_set((xrt_map_t*)");
    emit_vref(out, v->args[0]);
    fprintf(out, ".ptr, ");
    cg_emit_str_value(ctx, out, field ? field : "?");
    fprintf(out, ", ");
    emit_boxed_value_ref(out, v->args[1]);
    fprintf(out, "), ");
    emit_vref(out, v->args[1]);
    fprintf(out, ")");
}

static void xicgen_index_get(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                             const char *prefix) {
    XR_DCHECK(v->nargs >= 2, "xicgen_index_get: need obj and key");
    if (emit_struct_fixed_array_index_get_expr(ctx, out, f, v, prefix) ||
        emit_typed_array_index_get_expr(ctx, out, f, v, prefix))
        return;
    const char *conv_suffix =
        emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_value_plan_storage_rep(ctx, v));
    fprintf(out, "xrt_index_get(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ")");
    emit_conversion_suffix(out, conv_suffix);
}

static void xicgen_index_set(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                             const char *prefix) {
    XR_DCHECK(v->nargs >= 3, "xicgen_index_set: need obj, key, and value");
    if (emit_struct_fixed_array_index_set_expr(ctx, out, f, v, prefix))
        return;
    if (emit_typed_array_index_set_expr(ctx, out, f, v, prefix))
        return;
    fprintf(out, "xrt_index_set(");
    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep(out, v->args[2], XR_REP_TAGGED);
    fprintf(out, ")");
}

static void xicgen_tuple_new(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                             const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    if (v->nargs == 0) {
        fprintf(out, "xrt_tuple_new(0)");
        return;
    }
    fprintf(out, "xrt_tuple_make(%" PRIu16 ", (XrValue[]){", v->nargs);
    for (uint16_t a = 0; a < v->nargs; a++) {
        if (a > 0)
            fprintf(out, ", ");
        emit_value_as_rep(out, v->args[a], XR_REP_TAGGED);
    }
    fprintf(out, "})");
}

static void xicgen_tuple_get(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                             const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 1, "xicgen_tuple_get: need tuple");
    const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
    fprintf(out, "xrt_tuple_get(");
    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", %" PRId64 ")", v->aux_int);
    emit_conversion_suffix(out, conv_suffix);
}

static void xicgen_convert(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                           const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    XrRep dst_rep = cg_rep(v);
    XrRep src_rep = cg_rep(v->args[0]);
    if (v->type->kind == XR_KIND_FLOAT) {
        if (dst_rep == XR_REP_TAGGED) {
            fprintf(out, "xrt_to_float(");
            emit_boxed_value_ref(out, v->args[0]);
            fprintf(out, ")");
        } else if (src_rep == XR_REP_TAGGED) {
            fprintf(out, "XR_TO_FLOAT(xrt_to_float(");
            emit_vref(out, v->args[0]);
            fprintf(out, "))");
        } else {
            fprintf(out, "(double)");
            emit_vref(out, v->args[0]);
        }
    } else if (v->type->kind == XR_KIND_INT) {
        if (dst_rep == XR_REP_TAGGED) {
            fprintf(out, "xrt_to_int(");
            emit_boxed_value_ref(out, v->args[0]);
            fprintf(out, ")");
        } else if (src_rep == XR_REP_TAGGED) {
            fprintf(out, "XR_TO_INT(xrt_to_int(");
            emit_vref(out, v->args[0]);
            fprintf(out, "))");
        } else {
            fprintf(out, "(int64_t)");
            emit_vref(out, v->args[0]);
        }
    } else if (v->type->kind == XR_KIND_STRING) {
        fprintf(out, "xrt_to_string(");
        emit_boxed_value_ref(out, v->args[0]);
        fprintf(out, ")");
    } else if (v->type->kind == XR_KIND_BOOL) {
        if (dst_rep == XR_REP_TAGGED) {
            fprintf(out, "xrt_to_bool(");
            emit_boxed_value_ref(out, v->args[0]);
            fprintf(out, ")");
        } else if (src_rep == XR_REP_TAGGED) {
            fprintf(out, "XR_TO_INT(xrt_to_bool(");
            emit_vref(out, v->args[0]);
            fprintf(out, "))");
        } else {
            fprintf(out, "(");
            emit_vref(out, v->args[0]);
            fprintf(out, " != 0)");
        }
    } else {
        emit_vref(out, v->args[0]);
    }
}

static void xicgen_bytes_ptr_arg(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                 const char *prefix, uint16_t arg_index) {
    XR_DCHECK(v != NULL && arg_index < v->nargs, "xicgen bytes pointer arg out of range");
    emit_typed_array_ptr_expr(ctx, out, f, v->args[arg_index], prefix);
}

static void xicgen_bytes_i64_arg(FILE *out, const XiValue *v, uint16_t arg_index) {
    XR_DCHECK(v != NULL && arg_index < v->nargs, "xicgen bytes i64 arg out of range");
    emit_value_as_rep(out, v->args[arg_index], XR_REP_I64);
}

static void xicgen_bytes_box_array_result(FILE *out, bool boxed) {
    if (boxed)
        fprintf(out, ", XR_TAG_ARRAY)");
}

static void xicgen_bytes_load_u32_le(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                     const char *prefix) {
    const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
    fprintf(out, "(int64_t)xrt_bytes_load_u32_le_raw(");
    xicgen_bytes_ptr_arg(ctx, out, f, v, prefix, 0);
    fprintf(out, ", ");
    xicgen_bytes_i64_arg(out, v, 1);
    fprintf(out, ")");
    emit_conversion_suffix(out, conv_suffix);
}

static void xicgen_bytes_load_u64_le(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                     const char *prefix) {
    const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
    fprintf(out, "(int64_t)xrt_bytes_load_u64_le_raw(");
    xicgen_bytes_ptr_arg(ctx, out, f, v, prefix, 0);
    fprintf(out, ", ");
    xicgen_bytes_i64_arg(out, v, 1);
    fprintf(out, ")");
    emit_conversion_suffix(out, conv_suffix);
}

static void xicgen_bytes_copy_within(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                     const char *prefix) {
    bool boxed = cg_rep(v) == XR_REP_TAGGED;
    if (boxed)
        fprintf(out, "xr_mkptr(");
    fprintf(out, "xrt_bytes_copy_within_raw(");
    xicgen_bytes_ptr_arg(ctx, out, f, v, prefix, 0);
    fprintf(out, ", ");
    xicgen_bytes_i64_arg(out, v, 1);
    fprintf(out, ", ");
    xicgen_bytes_i64_arg(out, v, 2);
    fprintf(out, ", ");
    xicgen_bytes_i64_arg(out, v, 3);
    fprintf(out, ")");
    xicgen_bytes_box_array_result(out, boxed);
}

static void xicgen_bytes_copy_from(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                   const char *prefix) {
    bool boxed = cg_rep(v) == XR_REP_TAGGED;
    if (boxed)
        fprintf(out, "xr_mkptr(");
    fprintf(out, "xrt_bytes_copy_from_raw(");
    xicgen_bytes_ptr_arg(ctx, out, f, v, prefix, 0);
    fprintf(out, ", ");
    xicgen_bytes_ptr_arg(ctx, out, f, v, prefix, 1);
    fprintf(out, ", ");
    xicgen_bytes_i64_arg(out, v, 2);
    fprintf(out, ", ");
    xicgen_bytes_i64_arg(out, v, 3);
    fprintf(out, ", ");
    xicgen_bytes_i64_arg(out, v, 4);
    fprintf(out, ")");
    xicgen_bytes_box_array_result(out, boxed);
}

static void xicgen_bytes_repeat_from(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                     const char *prefix) {
    bool boxed = cg_rep(v) == XR_REP_TAGGED;
    if (boxed)
        fprintf(out, "xr_mkptr(");
    fprintf(out, "xrt_bytes_repeat_from_raw(");
    xicgen_bytes_ptr_arg(ctx, out, f, v, prefix, 0);
    fprintf(out, ", ");
    xicgen_bytes_i64_arg(out, v, 1);
    fprintf(out, ", ");
    xicgen_bytes_i64_arg(out, v, 2);
    fprintf(out, ", ");
    xicgen_bytes_i64_arg(out, v, 3);
    fprintf(out, ")");
    xicgen_bytes_box_array_result(out, boxed);
}

static bool xi_to_c_emit_generated(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                   const char *prefix) {
    switch (v->op) {
#define XICGEN_GENERATED_CASE(op, name, driver)                                                    \
    case XI_##op:                                                                                  \
        (void) name;                                                                               \
        driver(ctx, out, f, v, prefix);                                                            \
        return true;
        XI_TO_C_LOWERING_DRIVERS(XICGEN_GENERATED_CASE)
#undef XICGEN_GENERATED_CASE
        case XI_OP_COUNT:
            break;
    }
    return false;
}
