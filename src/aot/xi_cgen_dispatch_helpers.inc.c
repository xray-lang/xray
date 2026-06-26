/*
 * xi_cgen_dispatch_helpers.inc.c - Generated Xi lowering driver helpers for AOT C
 */

static void xicgen_const(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                         const char *prefix) {
    (void) f;
    (void) prefix;
    // A scalar const whose storage rep is TAGGED (e.g. an int/float/bool value
    // typed as a nullable primitive) must be boxed to match its XrValue C slot.
    bool boxed = cg_value_plan_storage_rep(ctx, v) == XR_REP_TAGGED;
    if (v->type->kind == XR_KIND_INT) {
        if (boxed)
            fprintf(out, "XR_FROM_INT(");
        if (v->aux_int == INT64_MIN)
            fprintf(out, "INT64_MIN");
        else
            fprintf(out, "INT64_C(%" PRId64 ")", v->aux_int);
        if (boxed)
            fprintf(out, ")");
    } else if (v->type->kind == XR_KIND_FLOAT) {
        double d;
        memcpy(&d, &v->aux_int, sizeof(double));
        if (boxed)
            fprintf(out, "XR_FROM_FLOAT(");
        emit_c_float_literal(out, d);
        if (boxed)
            fprintf(out, ")");
    } else if (v->type->kind == XR_KIND_BOOL) {
        if (boxed)
            fprintf(out, "XR_FROM_BOOL(");
        fprintf(out, "%" PRId64, v->aux_int);
        if (boxed)
            fprintf(out, ")");
    } else if (v->type->kind == XR_KIND_CHAR) {
        fprintf(out, "XR_FROM_CHAR((uint32_t)0x%X)", (unsigned) (uint32_t) v->aux_int);
    } else if (v->type->kind == XR_KIND_NULL)
        fprintf(out, "XR_NULL_VAL");
    else if (v->type->kind == XR_KIND_STRING) {
        cg_emit_str_value(ctx, out, (const char *) v->aux);
    } else if (v->type->kind == XR_KIND_UNKNOWN && v->aux) {
        emit_enum_type_expr(ctx, out, cg_enum_for_runtime_type(ctx, v->aux));
    } else {
        fprintf(out, "XR_NULL_VAL /* unknown const kind */");
    }
}

static bool xicgen_same_rep_identity_alias(XiCgenCtx *ctx, const XiValue *v, const XiValue *arg) {
    if (!ctx || !ctx->aot_bundle || !v || !arg)
        return false;
    const XaotValuePlan *value_plan = xaot_bundle_find_value_plan(ctx->aot_bundle, v);
    const XaotValuePlan *arg_plan = xaot_bundle_find_value_plan(ctx->aot_bundle, arg);
    return value_plan && arg_plan &&
           xaot_value_storage_rep(value_plan->rep) == xaot_value_storage_rep(arg_plan->rep);
}

static const XiValue *xicgen_getprop_receiver_value(XiCgenCtx *ctx, const XiValue *v) {
    while (v && (v->op == XI_UNBOX || xi_copy_is_identity_alias(v) || v->op == XI_MOVE) &&
           v->nargs >= 1 && xicgen_same_rep_identity_alias(ctx, v, v->args[0])) {
        v = v->args[0];
    }
    return v;
}

static XrRep xicgen_value_c_storage_rep(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v);

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
    (void) prefix;
    XR_DCHECK(v->nargs >= 1, "xicgen_identity: need arg");
    /* XI_COPY/XI_MOVE are value-level identities, but the rep planner may give
     * the result a different declared rep than its source (e.g. a native-local
     * PTR array moved into a TAGGED-declared local). Bridge that gap so the
     * emitted initializer matches the result's declared C type; when the reps
     * already agree this is a no-op and emits the bare source reference. */
    XrRep from_rep = xicgen_value_c_storage_rep(ctx, f, v->args[0]);
    XrRep to_rep = xicgen_value_c_storage_rep(ctx, f, v);
    const char *conv_suffix = emit_conversion_prefix(out, v->type, from_rep, to_rep);
    emit_vref(out, v->args[0]);
    emit_conversion_suffix(out, conv_suffix);
}

static bool xicgen_copy_needs_value_clone(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    (void) ctx;
    (void) f;
    if (!v || v->op != XI_COPY || v->nargs < 1 || !v->args[0])
        return false;
    return xi_copy_is_value_clone(v);
}

static void xicgen_copy(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                        const char *prefix) {
    (void) prefix;
    XR_DCHECK(v->nargs >= 1, "xicgen_copy: need arg");
    if (xi_copy_is_cell_read(v) && v->args[0] && cg_value_has_cell(ctx, v->args[0])) {
        char cell_expr[64];
        snprintf(cell_expr, sizeof(cell_expr), "cell_%u", (unsigned) v->args[0]->var_id);
        emit_cell_get_for_rep(out, v, cell_expr);
        return;
    }
    if (!xicgen_copy_needs_value_clone(ctx, f, v)) {
        xicgen_identity(ctx, out, f, v, prefix);
        return;
    }

    XrRep to_rep = cg_value_decl_storage_rep(ctx, f, v);
    const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, to_rep);
    fprintf(out, "xrt_value_clone_for_coro(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ")");
    emit_conversion_suffix(out, conv_suffix);
}

static void xicgen_move(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                        const char *prefix) {
    xicgen_identity(ctx, out, f, v, prefix);
}

static void xicgen_arith(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                         const char *prefix) {
    (void) prefix;
    XrRep result_rep = cg_rep(v);
    XrRep a_rep = cg_rep(v->args[0]);
    XrRep b_rep = cg_rep(v->args[1]);
    bool any_tagged = (a_rep == XR_REP_TAGGED || b_rep == XR_REP_TAGGED);
    if (result_rep == XR_REP_TAGGED || any_tagged) {
        const char *fn = xi_to_c_template_arith_runtime_fn(v->op);
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
        if (cg_arith_is_clean_narrow(ctx, f, v)) {
            fprintf(out, "(%s)(", local_ctype_str_ctx(ctx, f, v));
            cg_emit_narrow_arith_operand(ctx, f, out, v->args[0]);
            fprintf(out, " %s ", xi_to_c_template_arith_native_op(v->op));
            cg_emit_narrow_arith_operand(ctx, f, out, v->args[1]);
            fprintf(out, ")");
        } else if (!emit_native_i64_wrap_arith_expr(out, v)) {
            // Native int64: must wrap on overflow (raw + - * is signed UB in C).
            fprintf(out, "%s(", xi_to_c_template_arith_i64_wrap_fn(v->op));
            emit_vref(out, v->args[0]);
            fprintf(out, ", ");
            emit_vref(out, v->args[1]);
            fprintf(out, ")");
        }
    } else {
        emit_binop(out, v, xi_to_c_template_arith_native_op(v->op));
    }
}

#define XICGEN_DEFINE_TEMPLATE_ARITH_DRIVER(ident, driver)                                         \
    static void driver(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,               \
                       const char *prefix) {                                                       \
        xicgen_arith(ctx, out, f, v, prefix);                                                      \
    }

XI_TO_C_TEMPLATE_ARITH_DRIVERS(XICGEN_DEFINE_TEMPLATE_ARITH_DRIVER)

#undef XICGEN_DEFINE_TEMPLATE_ARITH_DRIVER

static void xicgen_div_mod(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                           const char *prefix) {
    (void) f;
    (void) prefix;
    XrRep result_rep = cg_rep(v);
    XrRep a_rep = cg_rep(v->args[0]);
    XrRep b_rep = cg_rep(v->args[1]);
    bool any_tagged = (a_rep == XR_REP_TAGGED || b_rep == XR_REP_TAGGED);
    if (result_rep == XR_REP_TAGGED || any_tagged) {
        const char *fn = xi_to_c_template_div_mod_runtime_fn(v->op);
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
            const char *fn = xi_to_c_template_div_mod_int_fn(v->op);
            fprintf(out, "%s(", fn);
            emit_vref(out, v->args[0]);
            fprintf(out, ", ");
            emit_vref(out, v->args[1]);
            fprintf(out, ")");
        }
    } else if (result_rep == XR_REP_F64) {
        fprintf(out, "(%s(XR_FROM_FLOAT(", xi_to_c_template_div_mod_runtime_fn(v->op));
        emit_vref(out, v->args[0]);
        fprintf(out, "), XR_FROM_FLOAT(");
        emit_vref(out, v->args[1]);
        fprintf(out, ")).f)");
    } else {
        emit_codegen_abort_expr(out);
        ctx->error = true;
    }
}

#define XICGEN_DEFINE_TEMPLATE_DIV_MOD_DRIVER(ident, driver)                                       \
    static void driver(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,               \
                       const char *prefix) {                                                       \
        xicgen_div_mod(ctx, out, f, v, prefix);                                                    \
    }

XI_TO_C_TEMPLATE_DIV_MOD_DRIVERS(XICGEN_DEFINE_TEMPLATE_DIV_MOD_DRIVER)

#undef XICGEN_DEFINE_TEMPLATE_DIV_MOD_DRIVER

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

static void xicgen_template_bitwise_binary(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                           const XiValue *v, const char *prefix) {
    (void) f;
    (void) prefix;
    emit_bitwise_binop_ctx(ctx, out, v, xi_to_c_template_bitwise_binary_op(v->op));
}

#define XICGEN_DEFINE_TEMPLATE_BITWISE_BINARY_DRIVER(ident, driver)                                \
    static void driver(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,               \
                       const char *prefix) {                                                       \
        xicgen_template_bitwise_binary(ctx, out, f, v, prefix);                                    \
    }

XI_TO_C_TEMPLATE_BITWISE_BINARY_DRIVERS(XICGEN_DEFINE_TEMPLATE_BITWISE_BINARY_DRIVER)

#undef XICGEN_DEFINE_TEMPLATE_BITWISE_BINARY_DRIVER

static void xicgen_template_bitwise_unary(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                          const XiValue *v, const char *prefix) {
    (void) f;
    (void) prefix;
    emit_bitwise_unop_ctx(ctx, out, v, xi_to_c_template_bitwise_unary_op(v->op));
}

#define XICGEN_DEFINE_TEMPLATE_BITWISE_UNARY_DRIVER(ident, driver)                                 \
    static void driver(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,               \
                       const char *prefix) {                                                       \
        xicgen_template_bitwise_unary(ctx, out, f, v, prefix);                                     \
    }

XI_TO_C_TEMPLATE_BITWISE_UNARY_DRIVERS(XICGEN_DEFINE_TEMPLATE_BITWISE_UNARY_DRIVER)

#undef XICGEN_DEFINE_TEMPLATE_BITWISE_UNARY_DRIVER

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
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ")");
}

static bool xicgen_import_ref_is_core_math_member(const XiImportRef *ref) {
    if (!ref || !ref->module_path || strcmp(ref->module_path, "math") != 0 || !ref->member_name)
        return false;
    static const char *members[] = {
        "abs",  "floor", "ceil",  "round",    "sqrt",     "pow",   "sin",   "cos",      "tan",
        "asin", "acos",  "atan",  "atan2",    "log",      "log10", "log2",  "exp",      "sinh",
        "cosh", "tanh",  "hypot", "cbrt",     "trunc",    "fmod",  "log1p", "expm1",    "min",
        "max",  "clamp", "lerp",  "degToRad", "radToDeg", "sign",  "isNaN", "isFinite",
    };
    for (int i = 0; i < (int) (sizeof(members) / sizeof(members[0])); i++) {
        if (strcmp(ref->member_name, members[i]) == 0)
            return true;
    }
    return false;
}

/* Both defined in xi_cgen_stdlib_helpers.inc.c (included later in this TU). */
static bool xicgen_emit_stdlib_method(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v);
static bool cg_module_has_aot_direct_calls(const char *module);
/* Resolve a `import { CONST } from "module"` reference to a generated stdlib
 * constant (path.sep, encoding.LE, ...); defined in xi_cgen_stdlib_helpers.inc.c. */
static bool cg_emit_aot_stdlib_generated_constant_import_ref(XiCgenCtx *ctx, FILE *out,
                                                             const XiValue *v,
                                                             const XiImportRef *ref);

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
        bool known_module_import = false;
        if (ref && ref->module_path && !ref->member_name) {
            known_module_import =
                ref->resolved_mod_index >= 0 && ref->resolved_mod_index < ctx->all_nmodules;
            for (int ii = 0; !known_module_import && ii < ctx->nimports; ii++) {
                known_module_import = ctx->imports[ii].module_path &&
                                      strcmp(ctx->imports[ii].module_path, ref->module_path) == 0;
            }
        }
        if (known_module_import) {
            fprintf(out, "XR_NULL_VAL /* module import: %s */", ref->module_path);
        } else if (ref && ref->module_path && !ref->member_name &&
                   (strcmp(ref->module_path, "time") == 0 ||
                    strcmp(ref->module_path, "math") == 0 || strcmp(ref->module_path, "log") == 0 ||
                    cg_module_has_aot_direct_calls(ref->module_path))) {
            fprintf(out, "XR_NULL_VAL /* builtin module: %s */", ref->module_path);
        } else if (xicgen_import_ref_is_core_math_member(ref)) {
            fprintf(out, "XR_NULL_VAL /* builtin math.%s */", ref->member_name);
        } else if (cg_emit_aot_stdlib_generated_constant_import_ref(ctx, out, v, ref)) {
            /* Resolved to a generated stdlib constant (path.sep, encoding.LE, ...). */
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
    (void) prefix;
    if (xicgen_upval_needs_cell(f, v)) {
        char cell_expr[64];
        snprintf(cell_expr, sizeof(cell_expr), "_cl->upvals[%d]", (int) v->aux_int);
        emit_cell_get_for_rep(out, v, cell_expr);
    } else {
        char up_expr[64];
        snprintf(up_expr, sizeof(up_expr), "_cl->upvals[%d]", (int) v->aux_int);
        emit_upval_get_for_rep(out, cg_value_decl_storage_rep(ctx, f, v), up_expr);
    }
}

static void xicgen_store_upval(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                               const char *prefix) {
    (void) prefix;
    if (xicgen_upval_needs_cell(f, v)) {
        fprintf(out, "(xrt_cell_set(_cl->upvals[%d], ", (int) v->aux_int);
        emit_boxed_value_ref(out, v->args[0]);
        fprintf(out, "), XR_NULL_VAL)");
    } else {
        fprintf(out, "(_cl->upvals[%d] = ", (int) v->aux_int);
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
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
    } else if (v->aux_int == XR_GLOBAL_VAR_JSON) {
        fprintf(out, "XR_NULL_VAL /* builtin Json namespace */");
    } else if (v->aux_int == XR_GLOBAL_VAR_WORKQUEUE || v->aux_int == XR_GLOBAL_VAR_RESULTGROUP ||
               v->aux_int == XR_GLOBAL_VAR_PANIC_INFO) {
        /* Class token only used as a constructor receiver; the constructor call is
         * lowered directly to an exception value (see xicgen_emit_panicinfo_constructor). */
        fprintf(out, "XR_NULL_VAL /* builtin native class token: %s */",
                v->aux ? (const char *) v->aux : "?");
    } else if (emit_prelude_enum_type_expr(out, (int) v->aux_int)) {
        /* Prelude enum type object: standalone AOT uses the same lightweight
         * map representation as user enums, avoiding a full isolate solely for
         * enum member lookup. */
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

static bool xicgen_call_is_result_group_constructor(const XiValue *callee) {
    const XiValue *origin = cg_unwrap_identity_value(callee);
    return origin && origin->op == XI_GET_BUILTIN && origin->aux_int == XR_GLOBAL_VAR_RESULTGROUP;
}

static const char *xicgen_aot_context_expr(XiCgenCtx *ctx, const XiFunc *f) {
    return cg_func_needs_aot_coro_ctx(ctx, f) ? "ctx" : "&xrt_global_ctx";
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
        fprintf(out, "xr_aot_work_queue_new(%s, ", xicgen_aot_context_expr(ctx, f));
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

    if (xicgen_call_is_result_group_constructor(callee)) {
        fprintf(out, "xr_aot_result_group_new(%s, ", xicgen_aot_context_expr(ctx, f));
        if (v->nargs >= 2)
            emit_value_as_rep(out, v->args[1], XR_REP_I64);
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

    if (!target && static_call.is_class_constructor && static_call.class_data) {
        if (emit_class_native_default_constructor_expr(ctx, out, prefix, v, static_call.class_data,
                                                       call_prefix))
            return;
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT class constructor call\n");
        emit_codegen_abort_expr(out);
        return;
    }

    if (target && is_class_call) {
        if (emit_class_native_constructor_expr(ctx, out, f, prefix, v, target, call_prefix))
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

    /* FFI: direct C-ABI call to an @extern function. Emit `c_sym(args)` with no
     * hidden _cl closure and arguments converted to their native C reps (the
     * same conversion as a typed direct call, so e.g. tagged -> double). */
    if (target && target->is_extern) {
        /* FFI: raw pointers cross the C boundary as real C pointers but are held
         * internally as address-width ints, so cast int64 <-> pointer here. A
         * pointer return is cast back to the int64 address; other return reps go
         * through the normal conversion prefix/suffix. */
        const XrType *ret_type = target->return_type;
        bool ret_is_ptr = ret_type && ret_type->kind == XR_KIND_POINTER;
        const char *conv_suffix = NULL;
        if (ret_is_ptr) {
            fprintf(out, "(int64_t)(intptr_t)(");
        } else {
            conv_suffix = emit_direct_call_return_conversion_prefix(ctx, out, f, v, target);
            if (ctx->error) {
                emit_codegen_abort_expr(out);
                return;
            }
        }
        fprintf(out, "xr_ffi_%s(",
                target->extern_symbol ? target->extern_symbol : (target->name ? target->name : ""));
        for (uint16_t a = 1; a < v->nargs; a++) {
            if (a > 1)
                fprintf(out, ", ");
            const XrType *pt =
                (target->params && (a - 1) < target->nparams && target->params[a - 1])
                    ? target->params[a - 1]->type
                    : NULL;
            const char *p_ptr = cg_extern_ptr_boundary_c_type(pt);
            if (cg_type_is_c_callback(pt)) {
                if (!emit_cfn_callback_arg(ctx, out, f, prefix, v, (uint16_t) (a - 1), pt,
                                           v->args[a]))
                    return;
            } else if (p_ptr) {
                /* A RawPtr<T>/RawMut<T> argument is an address-width int; emit it
                 * as I64 and cast straight to the C pointer type. Routing through
                 * the ABI-slot rep would re-box it to a tagged XrValue. */
                fprintf(out, "(%s)(intptr_t)(", p_ptr);
                emit_value_as_rep_ctx(ctx, out, v->args[a], XR_REP_I64);
                fprintf(out, ")");
            } else {
                emit_value_as_direct_call_arg(ctx, out, f, v, target, (uint16_t) (a - 1),
                                              v->args[a]);
            }
        }
        fprintf(out, ")");
        if (ret_is_ptr)
            fprintf(out, ")");
        else
            emit_conversion_suffix(out, conv_suffix);
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
        /* A function-valued upvalue can lose its static function type through
         * capture, but a value used as a call target is always a closure, so a
         * LOAD_UPVAL callee is invoked through the same boxed-entry path. */
        if (fn_val &&
            ((fn_val->type && XR_TYPE_IS_FUNCTION(fn_val->type)) || fn_val->op == XI_LOAD_UPVAL)) {
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

static const XiValue *xicgen_native_int_print_source(XiCgenCtx *ctx, const XiValue *v) {
    if (!ctx || !v || v->nargs != 1)
        return NULL;
    const XiValue *arg = v->args[0];
    if (!arg)
        return NULL;
    if (arg->op == XI_BOX && arg->nargs >= 1)
        arg = arg->args[0];
    if (!arg->type || arg->type->kind != XR_KIND_INT || arg->type->is_nullable)
        return NULL;  // nullable ints print via the tagged path so null -> "null"
    XrRep rep = cg_value_plan_storage_rep(ctx, arg);
    if (rep != XR_REP_I64 && rep != XR_REP_TAGGED)
        return NULL;
    return arg;
}

static bool xicgen_type_is_unsigned_int(const XrType *type) {
    if (!type || type->kind != XR_KIND_INT || type->is_nullable)
        return false;
    switch (type->native_width) {
        case XR_NATIVE_U8:
        case XR_NATIVE_U16:
        case XR_NATIVE_U32:
        case XR_NATIVE_U64:
            return true;
        default:
            return false;
    }
}

static bool xicgen_type_is_int_like(const XrType *type) {
    return type && type->kind == XR_KIND_INT && !type->is_nullable;
}

static bool xicgen_compare_uses_unsigned(const XiValue *v) {
    if (!v || v->nargs < 2)
        return false;
    if (v->op != XI_LT && v->op != XI_LE && v->op != XI_GT && v->op != XI_GE)
        return false;
    const XrType *left = v->args[0] ? v->args[0]->type : NULL;
    const XrType *right = v->args[1] ? v->args[1]->type : NULL;
    return xicgen_type_is_int_like(left) && xicgen_type_is_int_like(right) &&
           (xicgen_type_is_unsigned_int(left) || xicgen_type_is_unsigned_int(right));
}

static bool xicgen_box_only_feeds_native_int_print(XiCgenCtx *ctx, const XiFunc *f,
                                                   const XiValue *box) {
    if (!ctx || !f || !box || box->op != XI_BOX)
        return false;
    bool saw_print = false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == box)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t ai = 0; ai < phi->value.nargs; ai++) {
                if (phi->value.args[ai] == box)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user || user == box)
                continue;
            for (uint16_t ai = 0; ai < user->nargs; ai++) {
                if (user->args[ai] != box)
                    continue;
                if (user->op == XI_PRINT && ai == 0 &&
                    xicgen_native_int_print_source(ctx, user) == box->args[0]) {
                    saw_print = true;
                    continue;
                }
                return false;
            }
        }
    }
    return saw_print;
}

static void xicgen_emit_print_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    XR_DCHECK(v->nargs >= 1, "xicgen_emit_print_expr: missing print value");
    int flags = (int) v->aux_int;
    bool add_space = (flags & 1) != 0;
    bool newline = (flags & 2) != 0;
    const XiValue *native_int = xicgen_native_int_print_source(ctx, v);

    if (add_space)
        fprintf(out, "(putchar(' '), ");
    if (native_int) {
        if (xicgen_type_is_unsigned_int(native_int->type)) {
            fprintf(out, "printf(\"%%llu\", (unsigned long long)(uint64_t)");
            emit_value_as_rep_ctx(ctx, out, native_int, XR_REP_I64);
            fprintf(out, ")");
        } else {
            fprintf(out, "printf(\"%%lld\", (long long)");
            emit_value_as_rep_ctx(ctx, out, native_int, XR_REP_I64);
            fprintf(out, ")");
        }
        if (newline)
            fprintf(out, ", putchar('\\n')");
    } else {
        fprintf(out, "%s(", newline ? "xrt_println" : "xrt_print");
        emit_vref(out, v->args[0]);
        fprintf(out, ")");
    }
    if (add_space)
        fprintf(out, ")");
}

static void xicgen_print(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                         const char *prefix) {
    (void) f;
    (void) prefix;
    xicgen_emit_print_expr(ctx, out, v);
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

static void xicgen_emit_c_string_literal(FILE *out, const char *s) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *) (s ? s : ""); *p; p++) {
        switch (*p) {
            case '\\':
                fputs("\\\\", out);
                break;
            case '"':
                fputs("\\\"", out);
                break;
            case '\n':
                fputs("\\n", out);
                break;
            case '\r':
                fputs("\\r", out);
                break;
            case '\t':
                fputs("\\t", out);
                break;
            default:
                if (*p < 0x20 || *p >= 0x7f)
                    fprintf(out, "\\%03o", (unsigned) *p);
                else
                    fputc((int) *p, out);
                break;
        }
    }
    fputc('"', out);
}

static void xicgen_emit_json_new_expr(FILE *out, const XiValue *v) {
    int64_t field_count = xi_json_field_count(v);
    const char **field_names = (const char **) v->aux;
    const bool is_record = v && v->type && v->type->kind == XR_KIND_RECORD;
    const char *ctor = is_record ? "xrt_record_new" : "xrt_json_new";
    const char *ctor_named = is_record ? "xrt_record_new_named" : "xrt_json_new_named";
    if (field_count <= 0 || !field_names) {
        fprintf(out, "%s(%" PRId64 ")", ctor, field_count);
        return;
    }
    fprintf(out, "%s(%" PRId64 ", (const char*[]){", ctor_named, field_count);
    for (int64_t i = 0; i < field_count; i++) {
        if (i > 0)
            fprintf(out, ", ");
        xicgen_emit_c_string_literal(out, field_names[i] ? field_names[i] : "?");
    }
    fprintf(out, "})");
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
                if (xicgen_type_is_unsigned_int(v->args[0] ? v->args[0]->type : NULL)) {
                    fprintf(out, "xrt_uint64_to_string((uint64_t)");
                    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
                    fprintf(out, ")");
                } else {
                    fprintf(out, "xrt_to_string(");
                    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
                    fprintf(out, ")");
                }
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

    fprintf(out, "({ XrValue _as = ");
    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
    fprintf(out, "; (xrt_typeof_id(_as) == %" PRId32 ") ? _as : ", tid);
    if (is_safe) {
        fprintf(out, "XR_NULL_VAL; })");
    } else {
        /* Unsafe `as` mismatch: same TypeError shape (message + code 404) as the
         * VM OP_CHECKTYPE path via the shared runtime helper. */
        fprintf(out, "(xrt_throw_type_mismatch(%" PRId32 ", xrt_typeof_id(_as)), XR_NULL_VAL); })",
                tid);
    }
}

static void xicgen_checktype(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                             const char *prefix) {
    (void) prefix;
    XR_DCHECK(v->nargs >= 1, "xicgen_checktype: need arg");

    const XiValue *arg = v->args[0];
    XrRep arg_rep = xicgen_value_c_storage_rep(ctx, f, arg);
    XrRep out_rep = xicgen_value_c_storage_rep(ctx, f, v);
    int32_t tid = (int32_t) (v->aux_int >> 1);
    bool allow_null = (v->aux_int & 1) != 0;

    fprintf(out, "({ XrValue _ct = ");
    const char *arg_suffix =
        emit_conversion_prefix(out, arg ? arg->type : NULL, arg_rep, XR_REP_TAGGED);
    emit_vref(out, arg);
    emit_conversion_suffix(out, arg_suffix);
    fprintf(out, "; int64_t _ct_tid = xrt_typeof_id(_ct); ");
    fprintf(out, "((_ct_tid == %" PRId32 ")", tid);
    if (allow_null)
        fprintf(out, " || (_ct_tid == 0)");
    fprintf(out, ") ? ");
    const char *ok_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, out_rep);
    fprintf(out, "_ct");
    emit_conversion_suffix(out, ok_suffix);
    /* Mismatch: identical TypeError shape (message + code 404) as VM OP_CHECKTYPE. */
    fprintf(out, " : (xrt_throw_type_mismatch(%" PRId32 ", _ct_tid), ", tid);
    if (out_rep == XR_REP_TAGGED) {
        fprintf(out, "XR_NULL_VAL");
    } else if (out_rep == XR_REP_F64) {
        fprintf(out, "0.0");
    } else if (out_rep == XR_REP_PTR) {
        fprintf(out, "NULL");
    } else {
        fprintf(out, "INT64_C(0)");
    }
    fprintf(out, "); })");
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
    fprintf(out, ", %s)", v->aux_int ? "true" : "false");
}

static bool xicgen_math_result_rep(const char *name, XrRep *out_rep) {
    if (!name || !out_rep)
        return false;
    if (strcmp(name, "random") == 0) {
        *out_rep = XR_REP_F64;
        return true;
    }
    if (strcmp(name, "randomInt") == 0) {
        *out_rep = XR_REP_I64;
        return true;
    }
    if (strcmp(name, "floor") == 0 || strcmp(name, "ceil") == 0 || strcmp(name, "round") == 0 ||
        strcmp(name, "trunc") == 0 || strcmp(name, "sign") == 0 || strcmp(name, "isNaN") == 0 ||
        strcmp(name, "isFinite") == 0) {
        *out_rep = XR_REP_I64;
        return true;
    }
    *out_rep = XR_REP_F64;
    return true;
}

static void xicgen_emit_math_arg(FILE *out, const XiValue *v) {
    if (cg_rep(v) == XR_REP_TAGGED) {
        fprintf(out, "xrt_math_number(");
        emit_vref(out, v);
        fprintf(out, ")");
        return;
    }
    emit_value_as_rep(out, v, XR_REP_F64);
}

static bool xicgen_math_all_args_rep(const XiValue *v, XrRep rep) {
    if (!v || v->nargs == 0)
        return false;
    for (uint16_t i = 0; i < v->nargs; i++) {
        if (!v->args[i] || cg_rep(v->args[i]) != rep)
            return false;
    }
    return true;
}

static void xicgen_emit_math_i64_arg(FILE *out, const XiValue *v) {
    emit_value_as_rep(out, v, XR_REP_I64);
}

static void xicgen_emit_math_i64_minmax(FILE *out, const XiValue *v, bool is_min, uint16_t index) {
    if (index >= v->nargs) {
        fprintf(out, "INT64_C(0)");
        return;
    }
    if (index + 1 == v->nargs) {
        xicgen_emit_math_i64_arg(out, v->args[index]);
        return;
    }
    fprintf(out, "(((");
    xicgen_emit_math_i64_arg(out, v->args[index]);
    fprintf(out, ") %c (", is_min ? '<' : '>');
    xicgen_emit_math_i64_minmax(out, v, is_min, (uint16_t) (index + 1));
    fprintf(out, ")) ? (");
    xicgen_emit_math_i64_arg(out, v->args[index]);
    fprintf(out, ") : (");
    xicgen_emit_math_i64_minmax(out, v, is_min, (uint16_t) (index + 1));
    fprintf(out, "))");
}

static void xicgen_emit_math_f64_minmax(FILE *out, const XiValue *v, bool is_min, uint16_t index) {
    if (index >= v->nargs) {
        fprintf(out, "NAN");
        return;
    }
    if (index + 1 == v->nargs) {
        xicgen_emit_math_arg(out, v->args[index]);
        return;
    }
    fprintf(out, "(isnan(");
    xicgen_emit_math_arg(out, v->args[index]);
    fprintf(out, ") ? NAN : (isnan(");
    xicgen_emit_math_f64_minmax(out, v, is_min, (uint16_t) (index + 1));
    fprintf(out, ") ? NAN : f%s(", is_min ? "min" : "max");
    xicgen_emit_math_arg(out, v->args[index]);
    fprintf(out, ", ");
    xicgen_emit_math_f64_minmax(out, v, is_min, (uint16_t) (index + 1));
    fprintf(out, ")))");
}

static void xicgen_emit_math_i64_clamp(FILE *out, const XiValue *x, const XiValue *lo,
                                       const XiValue *hi) {
    fprintf(out, "(((");
    xicgen_emit_math_i64_arg(out, lo);
    fprintf(out, ") <= (");
    xicgen_emit_math_i64_arg(out, hi);
    fprintf(out, ")) ? (((");
    xicgen_emit_math_i64_arg(out, x);
    fprintf(out, ") < (");
    xicgen_emit_math_i64_arg(out, lo);
    fprintf(out, ")) ? (");
    xicgen_emit_math_i64_arg(out, lo);
    fprintf(out, ") : (((");
    xicgen_emit_math_i64_arg(out, x);
    fprintf(out, ") > (");
    xicgen_emit_math_i64_arg(out, hi);
    fprintf(out, ")) ? (");
    xicgen_emit_math_i64_arg(out, hi);
    fprintf(out, ") : (");
    xicgen_emit_math_i64_arg(out, x);
    fprintf(out, "))) : (((");
    xicgen_emit_math_i64_arg(out, x);
    fprintf(out, ") < (");
    xicgen_emit_math_i64_arg(out, hi);
    fprintf(out, ")) ? (");
    xicgen_emit_math_i64_arg(out, hi);
    fprintf(out, ") : (((");
    xicgen_emit_math_i64_arg(out, x);
    fprintf(out, ") > (");
    xicgen_emit_math_i64_arg(out, lo);
    fprintf(out, ")) ? (");
    xicgen_emit_math_i64_arg(out, lo);
    fprintf(out, ") : (");
    xicgen_emit_math_i64_arg(out, x);
    fprintf(out, "))))");
}

static void xicgen_emit_math_f64_clamp(FILE *out, const XiValue *x, const XiValue *lo,
                                       const XiValue *hi) {
    fprintf(out, "(isnan(");
    xicgen_emit_math_arg(out, x);
    fprintf(out, ") || isnan(");
    xicgen_emit_math_arg(out, lo);
    fprintf(out, ") || isnan(");
    xicgen_emit_math_arg(out, hi);
    fprintf(out, ") ? NAN : (((");
    xicgen_emit_math_arg(out, lo);
    fprintf(out, ") <= (");
    xicgen_emit_math_arg(out, hi);
    fprintf(out, ")) ? fmin(fmax(");
    xicgen_emit_math_arg(out, x);
    fprintf(out, ", ");
    xicgen_emit_math_arg(out, lo);
    fprintf(out, "), ");
    xicgen_emit_math_arg(out, hi);
    fprintf(out, ") : fmin(fmax(");
    xicgen_emit_math_arg(out, x);
    fprintf(out, ", ");
    xicgen_emit_math_arg(out, hi);
    fprintf(out, "), ");
    xicgen_emit_math_arg(out, lo);
    fprintf(out, ")))");
}

static bool xicgen_emit_math_raw_expr(FILE *out, const XiValue *v, const char *name) {
    if (!name || !v)
        return false;

    if (strcmp(name, "random") == 0 && v->nargs == 0) {
        fprintf(out, "xrt_math_random_f64()");
        return true;
    }
    if (strcmp(name, "randomInt") == 0 && v->nargs == 2) {
        fprintf(out, "xrt_math_random_i64(");
        emit_value_as_rep(out, v->args[0], XR_REP_I64);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[1], XR_REP_I64);
        fprintf(out, ")");
        return true;
    }
    if (strcmp(name, "abs") == 0 && v->nargs == 1 && cg_rep(v) == XR_REP_TAGGED) {
        fprintf(out, "xrt_math_abs(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ")");
        return true;
    }
    if (strcmp(name, "min") == 0) {
        if (v->nargs == 0 && cg_rep(v) == XR_REP_TAGGED) {
            fprintf(out, "XR_NULL_VAL");
            return true;
        }
        if (v->nargs == 1 && cg_rep(v) == XR_REP_TAGGED) {
            emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
            return true;
        }
        if (cg_rep(v) == XR_REP_I64 && xicgen_math_all_args_rep(v, XR_REP_I64))
            xicgen_emit_math_i64_minmax(out, v, true, 0);
        else
            xicgen_emit_math_f64_minmax(out, v, true, 0);
        return true;
    }
    if (strcmp(name, "max") == 0) {
        if (v->nargs == 0 && cg_rep(v) == XR_REP_TAGGED) {
            fprintf(out, "XR_NULL_VAL");
            return true;
        }
        if (v->nargs == 1 && cg_rep(v) == XR_REP_TAGGED) {
            emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
            return true;
        }
        if (cg_rep(v) == XR_REP_I64 && xicgen_math_all_args_rep(v, XR_REP_I64))
            xicgen_emit_math_i64_minmax(out, v, false, 0);
        else
            xicgen_emit_math_f64_minmax(out, v, false, 0);
        return true;
    }
    if (strcmp(name, "clamp") == 0 && v->nargs == 3) {
        if (cg_rep(v) == XR_REP_I64 && xicgen_math_all_args_rep(v, XR_REP_I64))
            xicgen_emit_math_i64_clamp(out, v->args[0], v->args[1], v->args[2]);
        else
            xicgen_emit_math_f64_clamp(out, v->args[0], v->args[1], v->args[2]);
        return true;
    }
    if (strcmp(name, "lerp") == 0 && v->nargs == 3) {
        fprintf(out, "(");
        xicgen_emit_math_arg(out, v->args[0]);
        fprintf(out, " + (");
        xicgen_emit_math_arg(out, v->args[1]);
        fprintf(out, " - ");
        xicgen_emit_math_arg(out, v->args[0]);
        fprintf(out, ") * ");
        xicgen_emit_math_arg(out, v->args[2]);
        fprintf(out, ")");
        return true;
    }
    if (strcmp(name, "degToRad") == 0 && v->nargs == 1) {
        fprintf(out, "(");
        xicgen_emit_math_arg(out, v->args[0]);
        fprintf(out, " * 0.01745329251994329577)");
        return true;
    }
    if (strcmp(name, "radToDeg") == 0 && v->nargs == 1) {
        fprintf(out, "(");
        xicgen_emit_math_arg(out, v->args[0]);
        fprintf(out, " * 57.2957795130823208768)");
        return true;
    }
    if (strcmp(name, "sign") == 0 && v->nargs == 1) {
        fprintf(out, "((int64_t)((");
        xicgen_emit_math_arg(out, v->args[0]);
        fprintf(out, ") > 0.0) - (int64_t)((");
        xicgen_emit_math_arg(out, v->args[0]);
        fprintf(out, ") < 0.0))");
        return true;
    }
    if (strcmp(name, "isNaN") == 0 && v->nargs == 1) {
        fprintf(out, "isnan(");
        xicgen_emit_math_arg(out, v->args[0]);
        fprintf(out, ")");
        return true;
    }
    if (strcmp(name, "isFinite") == 0 && v->nargs == 1) {
        fprintf(out, "isfinite(");
        xicgen_emit_math_arg(out, v->args[0]);
        fprintf(out, ")");
        return true;
    }

    struct {
        const char *name;
        const char *c_name;
        uint16_t nargs;
        bool returns_int;
    } table[] = {
        {"abs", "fabs", 1, false},    {"floor", "floor", 1, true},  {"ceil", "ceil", 1, true},
        {"round", "round", 1, true},  {"sqrt", "sqrt", 1, false},   {"pow", "pow", 2, false},
        {"sin", "sin", 1, false},     {"cos", "cos", 1, false},     {"tan", "tan", 1, false},
        {"asin", "asin", 1, false},   {"acos", "acos", 1, false},   {"atan", "atan", 1, false},
        {"atan2", "atan2", 2, false}, {"log", "log", 1, false},     {"log10", "log10", 1, false},
        {"log2", "log2", 1, false},   {"exp", "exp", 1, false},     {"sinh", "sinh", 1, false},
        {"cosh", "cosh", 1, false},   {"tanh", "tanh", 1, false},   {"hypot", "hypot", 2, false},
        {"cbrt", "cbrt", 1, false},   {"trunc", "trunc", 1, true},  {"fmod", "fmod", 2, false},
        {"log1p", "log1p", 1, false}, {"expm1", "expm1", 1, false},
    };

    for (int i = 0; i < (int) (sizeof(table) / sizeof(table[0])); i++) {
        if (strcmp(name, table[i].name) != 0)
            continue;
        if (v->nargs != table[i].nargs)
            return false;
        if (table[i].returns_int)
            fprintf(out, "(int64_t)");
        fprintf(out, "%s(", table[i].c_name);
        xicgen_emit_math_arg(out, v->args[0]);
        if (table[i].nargs == 2) {
            fprintf(out, ", ");
            xicgen_emit_math_arg(out, v->args[1]);
        }
        fprintf(out, ")");
        return true;
    }
    return false;
}

static bool xicgen_emit_math_builtin_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                          const XiValue *v, const char *bn) {
    if (!bn || strncmp(bn, "math.", 5) != 0)
        return false;
    const char *name = bn + 5;
    XrRep expr_rep = XR_REP_TAGGED;
    if (strcmp(name, "abs") == 0 && cg_rep(v) == XR_REP_TAGGED) {
        expr_rep = XR_REP_TAGGED;
    } else if ((strcmp(name, "min") == 0 || strcmp(name, "max") == 0) && v &&
               cg_rep(v) == XR_REP_I64 && xicgen_math_all_args_rep(v, XR_REP_I64)) {
        expr_rep = XR_REP_I64;
    } else if (strcmp(name, "clamp") == 0 && v && cg_rep(v) == XR_REP_I64 &&
               xicgen_math_all_args_rep(v, XR_REP_I64)) {
        expr_rep = XR_REP_I64;
    } else if ((strcmp(name, "min") == 0 || strcmp(name, "max") == 0) && v && v->nargs == 0 &&
               cg_rep(v) == XR_REP_TAGGED) {
        expr_rep = XR_REP_TAGGED;
    } else if ((strcmp(name, "min") == 0 || strcmp(name, "max") == 0) && v && v->nargs == 1 &&
               cg_rep(v) == XR_REP_TAGGED) {
        expr_rep = XR_REP_TAGGED;
    } else if (!xicgen_math_result_rep(name, &expr_rep)) {
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return true;
    }
    XrRep target_rep = xicgen_value_c_storage_rep(ctx, f, v);
    const char *suffix = emit_conversion_prefix(out, v ? v->type : NULL, expr_rep, target_rep);
    if (!xicgen_emit_math_raw_expr(out, v, name)) {
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT math builtin '%s'\n", name);
        emit_codegen_abort_expr(out);
        ctx->error = true;
    }
    emit_conversion_suffix(out, suffix);
    return true;
}

static void xicgen_call_builtin(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                const char *prefix) {
    const char *bn = v->aux ? (const char *) v->aux : "";

    if (bn[0] == '\0' && v->aux_int == 0 && v->nargs == 0) {
        fprintf(out, "XR_FROM_BOOL(false)");
    } else if (strcmp(bn, "print") == 0) {
        xicgen_emit_print_expr(ctx, out, v);
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
        xicgen_emit_json_new_expr(out, v);
    } else if (strcmp(bn, "json_init_f") == 0 || strcmp(bn, "json_set_f") == 0) {
        xicgen_emit_json_set_field_expr(out, v);
    } else if (strcmp(bn, "json_get_f") == 0) {
        fprintf(out, "xrt_json_get_field(");
        emit_vref(out, v->args[0]);
        fprintf(out, ", %d)", (int) v->aux_int);
    } else if (strcmp(bn, "copy") == 0 || strcmp(bn, "to_shared") == 0) {
        /* copy(x): explicit deep copy. The internal to_shared form is used
         * only for shared-slot stores; standalone AOT has one RC heap, so it
         * is still a single recursive clone rather than a VM heap promotion. */
        XR_DCHECK(v->nargs >= 1, "builtin copy: need arg");
        bool is_json = v->args[0] && XR_TYPE_HAS_OBJECT_SHAPE(v->args[0]->type);
        const char *conv_suffix =
            emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_value_plan_storage_rep(ctx, v));
        fprintf(out, "%s(", is_json ? "xrt_json_clone_for_coro" : "xrt_value_clone_for_coro");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
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
    } else if (xicgen_emit_math_builtin_expr(ctx, out, f, v, bn)) {
        /* Expression emitted by the math helper. */
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

static const XiFunc *cg_lookup_class_ctor_global(XiCgenCtx *ctx, const char *class_name,
                                                 const char **out_prefix);

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
    /* The parent may live in another module (the derived class extends an
     * imported base), so resolve through the cross-module lookups. */
    if (method && strcmp(method, "constructor") == 0)
        return cg_lookup_class_ctor_global(ctx, parent_class, method_prefix);
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
            emit_typed_array_reduce_expr(ctx, out, f, prefix, v)) ||
           (nargs == 1 && strcmp(method, "find") == 0 &&
            emit_typed_array_predicate_hof_expr(ctx, out, f, prefix, v, "xrt_array_find_typed")) ||
           (nargs == 1 && strcmp(method, "findIndex") == 0 &&
            emit_typed_array_predicate_hof_expr(ctx, out, f, prefix, v,
                                                "xrt_array_find_index_typed")) ||
           (nargs == 1 && strcmp(method, "every") == 0 &&
            emit_typed_array_predicate_hof_expr(ctx, out, f, prefix, v, "xrt_array_every_typed")) ||
           (nargs == 1 && strcmp(method, "some") == 0 &&
            emit_typed_array_predicate_hof_expr(ctx, out, f, prefix, v, "xrt_array_some_typed"));
}

static bool xicgen_emit_enum_method(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                    const char *method) {
    const XiEnumData *recv_enum = cg_enum_for_shared_value(ctx, v->args[0]);
    int enum_member = cg_enum_member_index(recv_enum, method);
    if (!recv_enum || enum_member < 0)
        return false;
    if (recv_enum->is_adt && recv_enum->members &&
        recv_enum->members[enum_member].payload_count > 0) {
        emit_adt_enum_construct_expr(out, recv_enum, enum_member, v);
    } else {
        fprintf(out, "xrt_map_get_owned((xrt_map_t*)");
        emit_vref(out, v->args[0]);
        fprintf(out, ".ptr, ");
        cg_emit_str_value(ctx, out, method);
        fprintf(out, ")");
    }
    return true;
}

static bool xicgen_emit_task_method(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                    const char *method, uint16_t nargs) {
    if (v->op != XI_CALL_METHOD || !xi_value_type_is_task(v->args[0]))
        return false;
    const char *aot_ctx = xicgen_aot_context_expr(ctx, f);
    if (nargs == 0 && method && strcmp(method, "cancel") == 0) {
        if (cg_rep(v) == XR_REP_I64)
            fprintf(out, "XR_TO_INT(");
        else if (cg_rep(v) == XR_REP_F64)
            fprintf(out, "XR_TO_FLOAT(");
        fprintf(out, "xr_aot_task_cancel(%s, ", aot_ctx);
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
        fprintf(out, "xr_aot_task_poll(%s, ", aot_ctx);
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

static bool xicgen_emit_channel_method(FILE *out, const XiValue *v, const char *method,
                                       uint16_t nargs) {
    if (!v || v->nargs < 1 || !method || !xi_value_type_is_channel(v->args[0]))
        return false;
    bool is_try_send = strcmp(method, "trySend") == 0 && nargs == 1 && v->nargs >= 2;
    bool is_try_recv = strcmp(method, "tryRecv") == 0 && nargs == 0;
    bool is_close = strcmp(method, "close") == 0 && nargs == 0;
    bool is_closed = strcmp(method, "isClosed") == 0 && nargs == 0;
    if (!is_try_send && !is_try_recv && !is_close && !is_closed)
        return false;

    const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
    if (is_try_send) {
        XrRep send_rep = v->nargs >= 2 ? cg_rep(v->args[1]) : XR_REP_TAGGED;
        const char *helper = "xr_aot_chan_try_send_sync";
        if (send_rep == XR_REP_I64)
            helper = "xr_aot_chan_try_send_sync_i64";
        else if (send_rep == XR_REP_F64)
            helper = "xr_aot_chan_try_send_sync_f64";
        fprintf(out, "%s(", helper);
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[1],
                          (send_rep == XR_REP_I64 || send_rep == XR_REP_F64) ? send_rep
                                                                             : XR_REP_TAGGED);
        fprintf(out, ")");
    } else if (is_try_recv) {
        fprintf(out, "xr_aot_chan_try_recv_sync(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ")");
    } else if (is_close) {
        fprintf(out, "xr_aot_chan_close_sync(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ")");
    } else if (is_closed) {
        fprintf(out, "xr_aot_chan_is_closed_sync(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ")");
    }
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool xicgen_emit_work_queue_method(FILE *out, const XiValue *v, const char *method,
                                          uint16_t nargs) {
    if (!v || v->nargs < 1 || !method || !xi_value_type_is_work_queue(v->args[0]))
        return false;
    bool is_push = strcmp(method, "push") == 0 && (nargs == 1 || nargs == 2) && v->nargs >= 2;
    bool is_try_pop = strcmp(method, "tryPop") == 0 && (nargs == 0 || nargs == 1);
    bool is_close = strcmp(method, "close") == 0 && nargs == 0;
    bool is_closed = strcmp(method, "isClosed") == 0 && nargs == 0;
    if (!is_push && !is_try_pop && !is_close && !is_closed)
        return false;

    const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
    if (is_push) {
        fprintf(out, "xr_aot_work_queue_push_sync(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ", ");
        if (nargs == 2 && v->nargs >= 3)
            emit_value_as_rep(out, v->args[2], XR_REP_I64);
        else
            fprintf(out, "-1");
        fprintf(out, ")");
    } else if (is_try_pop) {
        /* Pack the runtime's (out-param value, ok) into an AOT-native tuple so
         * the destructuring XI_TUPLE_GET reads it like every other AOT tuple. */
        fprintf(out, "({ XrValue _wq_tpv_%u = XR_NULL_VAL; bool _wq_tpok_%u = ", v->id, v->id);
        fprintf(out, "xr_aot_work_queue_try_pop_sync(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        if (nargs == 1 && v->nargs >= 2)
            emit_value_as_rep(out, v->args[1], XR_REP_I64);
        else
            fprintf(out, "-1");
        fprintf(out,
                ", &_wq_tpv_%u); xrt_tuple_make(2, (XrValue[]){"
                "xr_aot_bridge_value_to_xrt(_wq_tpv_%u), XR_FROM_BOOL(_wq_tpok_%u)}); })",
                v->id, v->id, v->id);
    } else if (is_close) {
        fprintf(out, "xr_aot_work_queue_close_sync(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ")");
    } else if (is_closed) {
        fprintf(out, "xr_aot_work_queue_is_closed_sync(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ")");
    }
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool xicgen_emit_result_group_method(FILE *out, const XiValue *v, const char *method,
                                            uint16_t nargs) {
    if (!v || v->nargs < 1 || !method || !xi_value_type_is_result_group(v->args[0]))
        return false;
    bool is_add = strcmp(method, "add") == 0 && nargs == 1 && v->nargs >= 2;
    bool is_flush = strcmp(method, "flush") == 0 && nargs == 0;
    bool is_try_recv = strcmp(method, "tryRecv") == 0 && nargs == 0;
    bool is_close = strcmp(method, "close") == 0 && nargs == 0;
    if (!is_add && !is_flush && !is_try_recv && !is_close)
        return false;

    const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
    if (is_add) {
        fprintf(out, "xr_aot_result_group_add_sync(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[1], XR_REP_I64);
        fprintf(out, ")");
    } else if (is_flush) {
        fprintf(out, "xr_aot_result_group_flush_sync(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ")");
    } else if (is_try_recv) {
        fprintf(out, "({ XrValue _rg_trv_%u = XR_NULL_VAL; bool _rg_trok_%u = ", v->id, v->id);
        fprintf(out, "xr_aot_result_group_try_recv_sync(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out,
                ", &_rg_trv_%u); xrt_tuple_make(2, (XrValue[]){_rg_trv_%u, "
                "XR_FROM_BOOL(_rg_trok_%u)}); })",
                v->id, v->id, v->id);
    } else if (is_close) {
        fprintf(out, "xr_aot_result_group_close_sync(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ")");
    }
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool xicgen_receiver_is_builtin_global(const XiValue *receiver, int global_index) {
    const XiValue *origin = cg_unwrap_identity_value(receiver);
    return origin && origin->op == XI_GET_BUILTIN && origin->aux_int == global_index;
}

/* PanicInfo(message="", cause=null) constructs the lightweight exception value
 * shared with the runtime helpers (a json-named object with message/stack/cause/
 * code/data). The match-non-exhaustive and force-unwrap lowerings emit this. */
static bool xicgen_emit_panicinfo_constructor(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    if (!v || v->nargs < 1 ||
        !xicgen_receiver_is_builtin_global(v->args[0], XR_GLOBAL_VAR_PANIC_INFO))
        return false;
    const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
    if (v->nargs >= 2) {
        fprintf(out, "xrt_exception_from_message_value(");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ")");
    } else {
        fprintf(out, "xrt_exception_new_value(0, NULL, 0)");
    }
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool xicgen_emit_json_static_method(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                           const char *method, uint16_t nargs) {
    if (!v || v->op != XI_CALL_METHOD || v->nargs < 1 || !method ||
        !xicgen_receiver_is_builtin_global(v->args[0], XR_GLOBAL_VAR_JSON))
        return false;
    if (strcmp(method, "encode") != 0 || nargs != 1 || v->nargs < 2) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT Json static method '%s'\n",
                method ? method : "?");
        emit_codegen_abort_expr(out);
        return true;
    }
    const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
    fprintf(out, "xrt_json_encode(");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ")");
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static void xicgen_emit_runtime_method(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                       const char *method, uint16_t nargs) {
    if (xicgen_emit_json_static_method(ctx, out, v, method, nargs))
        return;
    if (xicgen_emit_channel_method(out, v, method, nargs))
        return;
    if (xicgen_emit_work_queue_method(out, v, method, nargs))
        return;
    if (xicgen_emit_result_group_method(out, v, method, nargs))
        return;
    /* Enum `for-in` lowering calls EnumType.getMember(i); a user enum is a
     * map keyed by member name, so index its values in insertion order. */
    if (strcmp(method, "getMember") == 0 && nargs == 1) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        fprintf(out, "xrt_map_value_at((xrt_map_t *)(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ").ptr, ");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return;
    }
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
    if (emit_class_native_array_method_call_expr(ctx, out, f, v))
        return;
    if (emit_local_typed_map_method_call_expr(ctx, out, f, v))
        return;
    if (emit_local_typed_set_method_call_expr(ctx, out, f, v))
        return;
    const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
    if (nargs == 0) {
        fprintf(out, "xrt_method_0(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", %d)", sym);
    } else if (nargs == 1) {
        fprintf(out, "xrt_method_1(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", %d, ", sym);
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ")");
    } else if (nargs == 2) {
        fprintf(out, "xrt_method_2(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", %d, ", sym);
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_TAGGED);
        fprintf(out, ")");
    } else if (nargs == 3) {
        fprintf(out, "xrt_method_3(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", %d, ", sym);
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, v->args[3], XR_REP_TAGGED);
        fprintf(out, ")");
    } else {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT method call with %u args\n",
                (unsigned) nargs);
        emit_codegen_abort_expr(out);
    }
    emit_conversion_suffix(out, conv_suffix);
}

/* Resolve a class constructor XiFunc + module prefix by class name, searching
 * the current module first and then every module (cross-module `new`). */
static const XiFunc *cg_lookup_class_ctor_global(XiCgenCtx *ctx, const char *class_name,
                                                 const char **out_prefix) {
    if (out_prefix)
        *out_prefix = NULL;
    if (!class_name)
        return NULL;
    const XiFunc *ctor = cg_lookup_class_ctor(ctx, class_name);
    if (ctor) {
        if (out_prefix)
            *out_prefix = cg_module_prefix_for_func(ctx, ctor);
        return ctor;
    }
    for (int i = 0; ctx && i < ctx->all_nmodules; i++) {
        XiModule *mod = ctx->all_modules[i];
        if (!mod || mod == ctx->module)
            continue;
        for (uint16_t s = 0; s < mod->nslots; s++) {
            const XiClassData *cd = mod->slot_classes ? mod->slot_classes[s] : NULL;
            if (!cd || !cd->class_name)
                continue;
            if (strcmp(cd->class_name, class_name) == 0 ||
                (cd->display_name && strcmp(cd->display_name, class_name) == 0)) {
                const XiFunc *c = cg_find_constructor(mod->init, cd);
                if (c) {
                    if (out_prefix)
                        *out_prefix = mod->name;
                    return c;
                }
            }
        }
    }
    return NULL;
}

/* `new X(args)` lowers to XI_CALL_METHOD with method "constructor" and the class
 * object (typed `any`) as the receiver, so the receiver-based method dispatch
 * cannot recover the class. Resolve the constructor from the call's result type
 * (always the constructed instance type) and emit it exactly like a bare class
 * call `X(args)`, keeping AOT consistent with the VM for the `new` form. */
static bool xicgen_emit_user_constructor(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                         const XiValue *v, const char *prefix) {
    const char *class_name = v->type ? xr_type_get_class_name(v->type) : NULL;
    if (!class_name)
        return false;
    const char *call_prefix = NULL;
    const XiFunc *ctor = cg_lookup_class_ctor_global(ctx, class_name, &call_prefix);
    if (!ctor)
        return false;
    if (emit_class_native_constructor_expr(ctx, out, f, prefix, v, ctor, call_prefix))
        return true;
    fprintf(out, "({ XrValue _inst = xrt_map_new(4); ");
    emit_fname(ctx, out, call_prefix ? call_prefix : prefix, ctor);
    fprintf(out, "(NULL, _inst");
    for (uint16_t a = 1; a < v->nargs; a++) {
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[a], XR_REP_TAGGED);
    }
    fprintf(out, "); _inst; })");
    return true;
}

/* Resolve a static method by class and name, walking the (possibly
 * cross-module) inheritance chain.  Static methods have no receiver, so they
 * are dispatched directly to the resolved function. */
static const XiFunc *cg_lookup_static_method(XiCgenCtx *ctx, const char *class_name,
                                             const char *method, const char **out_prefix) {
    if (out_prefix)
        *out_prefix = NULL;
    if (!ctx || !class_name || !method)
        return NULL;
    const char *cur = class_name;
    for (int depth = 0; cur && depth < 16; depth++) {
        const XiClassData *cd = cg_class_native_data_by_name(ctx, cur);
        if (!cd)
            return NULL;
        const XiModule *mod = cg_class_native_module_for_data(ctx, cd);
        if (mod && mod->init && cd->methods && cd->child_idx) {
            for (uint16_t mi = 0; mi < cd->nmethod; mi++) {
                const XiClassMethod *m = &cd->methods[mi];
                if (!m->is_static || m->is_static_constructor || !m->name ||
                    strcmp(m->name, method) != 0)
                    continue;
                uint16_t idx = cd->child_idx[mi];
                if (idx < mod->init->nchildren) {
                    if (out_prefix)
                        *out_prefix = mod->name;
                    return mod->init->children[idx];
                }
            }
        }
        cur = cd->super_name;
    }
    return NULL;
}

/* Emit a static method call `Class.method(args)` as a direct call to the static
 * function, dropping the class receiver (args[0]) since static methods take no
 * `this`. */
static bool xicgen_emit_static_method(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                      const char *prefix) {
    const char *method = (const char *) v->aux;
    /* The receiver of a static call is the class object, not an instance, so its
     * value type is unresolved; recover the class from the shared slot it loads. */
    const char *recv_class = cg_class_native_receiver_class_name(ctx, f, v->args[0]);
    if (!recv_class) {
        const XiValue *rv = cg_unwrap_identity_value(v->args[0]);
        if (rv && rv->op == XI_GET_SHARED) {
            int slot = (int) rv->aux_int;
            if (slot >= 0 && slot < ctx->nshared && slot < ctx->shared_cap &&
                ctx->shared_class[slot])
                recv_class = ctx->shared_class[slot]->class_name;
        }
    }
    if (!method)
        return false;
    const char *sprefix = NULL;
    const XiFunc *sfunc =
        recv_class ? cg_lookup_static_method(ctx, recv_class, method, &sprefix) : NULL;
    /* An imported class receiver loads from a slot the importer does not map to
     * the class, so when the class name is unresolved, search the imported
     * classes for the one declaring this static method. */
    if (!sfunc) {
        for (int i = 0; i < ctx->nimports && !sfunc; i++) {
            const XiClassData *cd = ctx->imports[i].target_class;
            if (cd && cd->class_name)
                sfunc = cg_lookup_static_method(ctx, cd->class_name, method, &sprefix);
        }
    }
    if (!sfunc)
        return false;
    if (cg_func_needs_aot_coro(sfunc)) {
        ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: unsupported AOT sync call to suspendable static method '%s'\n",
                method);
        emit_codegen_abort_expr(out);
        return true;
    }
    const char *conv_suffix = emit_direct_call_return_conversion_prefix(ctx, out, f, v, sfunc);
    if (ctx->error) {
        emit_codegen_abort_expr(out);
        return true;
    }
    emit_fname(ctx, out, sprefix ? sprefix : prefix, sfunc);
    fprintf(out, "(NULL");
    for (uint16_t a = 1; a < v->nargs; a++) {
        fprintf(out, ", ");
        emit_value_as_direct_call_arg(ctx, out, f, v, sfunc, (uint16_t) (a - 1), v->args[a]);
    }
    fprintf(out, ")");
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool xicgen_emit_import_module_member_call(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                  const XiValue *v, const char *prefix,
                                                  const char *method) {
    CgStaticFunctionCall call = cg_resolve_module_member_call(ctx, f, v, method);
    const XiFunc *target = call.func;
    if (!target && !(call.is_class_constructor && call.class_data))
        return false;
    if (target && cg_func_needs_aot_coro(target)) {
        ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: unsupported AOT sync module-member call to suspendable "
                "function '%s'\n",
                target->name ? target->name : "?");
        emit_codegen_abort_expr(out);
        return true;
    }
    if (call.is_class_constructor) {
        if (!target && emit_class_native_default_constructor_expr(ctx, out, prefix, v,
                                                                  call.class_data, call.prefix))
            return true;
        if (emit_class_native_constructor_expr(ctx, out, f, prefix, v, target, call.prefix))
            return true;
        if (!target) {
            ctx->error = true;
            fprintf(stderr,
                    "[xi_cgen] ERROR: unsupported AOT module-member class constructor '%s'\n",
                    method ? method : "?");
            emit_codegen_abort_expr(out);
            return true;
        }
        fprintf(out, "({ XrValue _inst = xrt_map_new(4); ");
        emit_fname(ctx, out, call.prefix ? call.prefix : prefix, target);
        fprintf(out, "(NULL, _inst");
        for (uint16_t a = 1; a < v->nargs; a++) {
            fprintf(out, ", ");
            emit_value_as_rep(out, v->args[a], XR_REP_TAGGED);
        }
        fprintf(out, "); _inst; })");
        return true;
    }
    const char *conv_suffix = emit_direct_call_return_conversion_prefix(ctx, out, f, v, target);
    if (ctx->error) {
        emit_codegen_abort_expr(out);
        return true;
    }
    emit_fname(ctx, out, call.prefix ? call.prefix : prefix, target);
    fprintf(out, "(NULL");
    for (uint16_t a = 1; a < v->nargs; a++) {
        fprintf(out, ", ");
        emit_value_as_direct_call_arg(ctx, out, f, v, target, (uint16_t) (a - 1), v->args[a]);
    }
    fprintf(out, ")");
    emit_conversion_suffix(out, conv_suffix);
    return true;
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
    if (xicgen_emit_stdlib_method(ctx, out, f, v))
        return;
    if (!is_super && method && strcmp(method, "constructor") == 0 &&
        xicgen_emit_panicinfo_constructor(ctx, out, v))
        return;
    if (!is_super && method && strcmp(method, "constructor") == 0 &&
        xicgen_emit_user_constructor(ctx, out, f, v, prefix))
        return;
    if (!is_super && xicgen_emit_import_module_member_call(ctx, out, f, v, prefix, method))
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
    if (xicgen_emit_task_method(ctx, out, f, v, method, nargs))
        return;
    if (xicgen_emit_direct_method(ctx, out, f, v, prefix, mfunc, method_prefix))
        return;
    if (!is_super && xicgen_emit_static_method(ctx, out, f, v, prefix))
        return;
    xicgen_emit_runtime_method(ctx, out, f, v, method, nargs);
}

static void xicgen_class_create(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                const char *prefix) {
    (void) f;
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
        fprintf(out, "uint16_t _tid = ");
        emit_class_native_type_register_expr(ctx, out, cd, prefix);
        fprintf(out, "; ");
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
        fprintf(out, "XR_FROM_INT(");
        emit_class_native_type_register_expr(ctx, out, cd, prefix);
        fprintf(out, ")");
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
    (void) prefix;
    XR_DCHECK(v->nargs >= 1, "xicgen_ownership_call: need arg");
    const XiValue *arg = v->args[0];
    fprintf(out, "%s(", fn_name);
    XrRep from_rep = xicgen_value_c_storage_rep(ctx, f, arg);
    const char *conv_suffix =
        emit_conversion_prefix(out, arg ? arg->type : NULL, from_rep, XR_REP_TAGGED);
    emit_vref(out, arg);
    emit_conversion_suffix(out, conv_suffix);
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
        int64_t cap =
            (v->nargs >= 1 && v->args[0] && v->args[0]->op == XI_CONST) ? v->args[0]->aux_int : 8;
        fprintf(out, "xrt_map_stack_new(%" PRId64 ")", cap);
    } else if (orig_op == XI_SET_NEW) {
        int64_t cap =
            (v->nargs >= 1 && v->args[0] && v->args[0]->op == XI_CONST) ? v->args[0]->aux_int : 8;
        fprintf(out, "xrt_set_stack_new(%" PRId64 ")", cap);
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
    xicgen_emit_json_new_expr(out, v);
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

static void xicgen_template_shift(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                  const char *prefix) {
    (void) f;
    (void) prefix;
    emit_shift_binop_ctx(ctx, out, v, xi_to_c_template_shift_fn(v->op));
}

#define XICGEN_DEFINE_TEMPLATE_SHIFT_DRIVER(ident, driver)                                         \
    static void driver(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,               \
                       const char *prefix) {                                                       \
        xicgen_template_shift(ctx, out, f, v, prefix);                                             \
    }

XI_TO_C_TEMPLATE_SHIFT_DRIVERS(XICGEN_DEFINE_TEMPLATE_SHIFT_DRIVER)

#undef XICGEN_DEFINE_TEMPLATE_SHIFT_DRIVER

static void xicgen_compare(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                           const char *prefix) {
    (void) f;
    (void) prefix;
    if (xicgen_compare_uses_unsigned(v)) {
        fprintf(out, "((uint64_t)");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
        fprintf(out, " %s (uint64_t)", xi_to_c_template_compare_native_op(v->op));
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
        fprintf(out, ")");
        return;
    }
    XrRep a0_rep = cg_rep(v->args[0]);
    XrRep a1_rep = cg_rep(v->args[1]);
    XrRep arg_rep = (a0_rep == XR_REP_TAGGED || a1_rep == XR_REP_TAGGED) ? XR_REP_TAGGED : a0_rep;
    if (arg_rep == XR_REP_TAGGED) {
        fprintf(out, "%s(", xi_to_c_template_compare_runtime_fn(v->op));
        if (xi_to_c_template_compare_swaps_tagged_args(v->op)) {
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
        emit_binop(out, v, xi_to_c_template_compare_native_op(v->op));
    }
}

#define XICGEN_DEFINE_TEMPLATE_COMPARE_DRIVER(ident, driver)                                       \
    static void driver(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,               \
                       const char *prefix) {                                                       \
        xicgen_compare(ctx, out, f, v, prefix);                                                    \
    }

XI_TO_C_TEMPLATE_COMPARE_DRIVERS(XICGEN_DEFINE_TEMPLATE_COMPARE_DRIVER)

#undef XICGEN_DEFINE_TEMPLATE_COMPARE_DRIVER

static void xicgen_cast_i64_arg(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                const char *ctype) {
    const XiValue *arg = v->nargs > 0 ? v->args[0] : NULL;
    if (arg && ctype) {
        const char *arg_ctype = local_ctype_str_ctx(ctx, f, arg);
        if (arg_ctype && strcmp(arg_ctype, ctype) == 0) {
            const char *res_ctype = local_ctype_str_ctx(ctx, f, v);
            if (res_ctype && strcmp(res_ctype, ctype) == 0) {
                emit_vref(out, arg);
            } else {
                fprintf(out, "(int64_t)");
                emit_vref(out, arg);
            }
            return;
        }
    }
    fprintf(out, "(int64_t)(%s)", ctype);
    emit_value_as_rep_ctx(ctx, out, arg, XR_REP_I64);
}

static void xicgen_f32_roundtrip(FILE *out, const XiValue *v, bool preserve_loaded_float32) {
    fputs(preserve_loaded_float32 ? "" : "(double)(float)", out);
    emit_vref(out, v->args[0]);
}

static void xicgen_template_width(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                  const char *prefix) {
    (void) prefix;
    switch (xi_to_c_template_width_kind(v->op)) {
        case AOT_WIDTH_TEMPLATE_CAST_I64:
            xicgen_cast_i64_arg(ctx, out, f, v, xi_to_c_template_width_cast_type(v->op));
            return;
        case AOT_WIDTH_TEMPLATE_F32_ROUNDTRIP:
            xicgen_f32_roundtrip(out, v,
                                 xi_to_c_template_width_preserves_loaded_f32(v->op) &&
                                     cg_array_index_get_reads_f32_storage(ctx, f, v->args[0]));
            return;
        case AOT_WIDTH_TEMPLATE_INVALID:
            break;
    }
    emit_codegen_abort_expr(out);
}

#define XICGEN_DEFINE_TEMPLATE_WIDTH_DRIVER(ident, driver)                                         \
    static void driver(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,               \
                       const char *prefix) {                                                       \
        xicgen_template_width(ctx, out, f, v, prefix);                                             \
    }

XI_TO_C_TEMPLATE_WIDTH_DRIVERS(XICGEN_DEFINE_TEMPLATE_WIDTH_DRIVER)

#undef XICGEN_DEFINE_TEMPLATE_WIDTH_DRIVER

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
            fprintf(out, ".tag == XR_TAG_I64)");
            break;
        case XR_KIND_FLOAT:
            fprintf(out, "(");
            emit_vref(out, v->args[0]);
            fprintf(out, ".tag == XR_TAG_F64)");
            break;
        case XR_KIND_BOOL:
            fprintf(out, "(");
            emit_vref(out, v->args[0]);
            fprintf(out, ".tag == XR_TAG_BOOL)");
            break;
        case XR_KIND_NULL:
            fprintf(out, "(");
            emit_vref(out, v->args[0]);
            fprintf(out, ".tag == XR_TAG_NULL)");
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
    const char *field = (const char *) v->aux;
    if (field && v->args[0] && v->args[0]->type && v->args[0]->type->kind == XR_KIND_ENUM) {
        const char *helper = NULL;
        if (strcmp(field, "name") == 0)
            helper = "xrt_enum_value_name";
        else if (strcmp(field, "value") == 0)
            helper = "xrt_enum_value_raw";
        else if (strcmp(field, "ordinal") == 0)
            helper = "xrt_enum_value_ordinal";
        if (helper) {
            const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED,
                                                             cg_value_plan_storage_rep(ctx, v));
            fprintf(out, "%s(", helper);
            emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
            fprintf(out, ")");
            emit_conversion_suffix(out, conv_suffix);
            return;
        }
    }
    if (emit_class_cached_field_load_expr(ctx, out, v))
        return;
    if (emit_class_native_receiver_field_load_expr(ctx, out, f, v))
        return;
    if (emit_class_native_instance_field_load_expr(ctx, out, f, v, prefix))
        return;
    if (field) {
        const char *helper = NULL;
        int64_t int_const = 0;
        bool has_int_const = false;
        if (cg_value_is_module_import_ctx(ctx, f, v->args[0], "path")) {
            if (strcmp(field, "sep") == 0)
                helper = "xrt_path_sep";
            else if (strcmp(field, "delimiter") == 0)
                helper = "xrt_path_delimiter";
        } else if (cg_value_is_module_import_ctx(ctx, f, v->args[0], "os")) {
            if (strcmp(field, "platform") == 0)
                helper = "xrt_os_platform";
            else if (strcmp(field, "arch") == 0)
                helper = "xrt_os_arch";
            else if (strcmp(field, "sep") == 0)
                helper = "xrt_os_sep";
            else if (strcmp(field, "eol") == 0)
                helper = "xrt_os_eol";
        } else if (cg_value_is_module_import_ctx(ctx, f, v->args[0], "log")) {
            if (strcmp(field, "DEBUG") == 0) {
                int_const = 10;
                has_int_const = true;
            } else if (strcmp(field, "INFO") == 0) {
                int_const = 20;
                has_int_const = true;
            } else if (strcmp(field, "WARN") == 0) {
                int_const = 30;
                has_int_const = true;
            } else if (strcmp(field, "ERROR") == 0) {
                int_const = 40;
                has_int_const = true;
            } else if (strcmp(field, "FATAL") == 0) {
                int_const = 50;
                has_int_const = true;
            }
        }
        if (has_int_const) {
            const char *conv_suffix =
                emit_conversion_prefix(out, v->type, XR_REP_I64, cg_value_plan_storage_rep(ctx, v));
            fprintf(out, "INT64_C(%" PRId64 ")", int_const);
            emit_conversion_suffix(out, conv_suffix);
            return;
        }
        if (helper) {
            const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED,
                                                             cg_value_plan_storage_rep(ctx, v));
            fprintf(out, "%s()", helper);
            emit_conversion_suffix(out, conv_suffix);
            return;
        }
    }
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
        xi_value_type_is_task(v->args[0]) ? cg_task_field_helper(field) : NULL;
    if (task_helper) {
        if (cg_rep(v) == XR_REP_I64)
            fprintf(out, "XR_TO_INT(");
        else if (cg_rep(v) == XR_REP_F64)
            fprintf(out, "XR_TO_FLOAT(");
        fprintf(out, "%s(%s, ", task_helper, xicgen_aot_context_expr(ctx, f));
        emit_vref(out, v->args[0]);
        fprintf(out, ")");
        if (cg_rep(v) == XR_REP_I64 || cg_rep(v) == XR_REP_F64)
            fprintf(out, ")");
        return;
    }
    const char *channel_helper =
        xi_value_type_is_channel(v->args[0]) ? cg_channel_field_helper(field) : NULL;
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
    const char *work_queue_helper =
        xi_value_type_is_work_queue(v->args[0]) ? cg_work_queue_field_helper(field) : NULL;
    if (work_queue_helper) {
        if (cg_rep(v) == XR_REP_I64)
            fprintf(out, "XR_TO_INT(");
        else if (cg_rep(v) == XR_REP_F64)
            fprintf(out, "XR_TO_FLOAT(");
        fprintf(out, "%s(NULL, ", work_queue_helper);
        emit_vref(out, v->args[0]);
        fprintf(out, ")");
        if (cg_rep(v) == XR_REP_I64 || cg_rep(v) == XR_REP_F64)
            fprintf(out, ")");
        return;
    }
    const char *result_group_helper =
        xi_value_type_is_result_group(v->args[0]) ? cg_result_group_field_helper(field) : NULL;
    if (result_group_helper) {
        if (cg_rep(v) == XR_REP_I64)
            fprintf(out, "XR_TO_INT(");
        else if (cg_rep(v) == XR_REP_F64)
            fprintf(out, "XR_TO_FLOAT(");
        fprintf(out, "%s(NULL, ", result_group_helper);
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
        emit_class_native_array_length_expr(ctx, out, f, v))
        return;
    if (field && (strcmp(field, "length") == 0 || strcmp(field, "size") == 0) &&
        emit_typed_array_length_expr(ctx, out, f, prefix, v))
        return;
    int sym = cg_method_sym(field);
    const XiValue *receiver = xicgen_getprop_receiver_value(ctx, v->args[0]);
    if (sym >= 0) {
        const char *conv_suffix =
            emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_value_plan_storage_rep(ctx, v));
        fprintf(out, "xrt_getprop(");
        emit_value_as_rep_ctx(ctx, out, receiver, XR_REP_TAGGED);
        fprintf(out, ", %d)", sym);
        emit_conversion_suffix(out, conv_suffix);
    } else {
        const char *conv_suffix =
            emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_value_plan_storage_rep(ctx, v));
        if (cg_value_type_is_json(v->args[0])) {
            fprintf(out, "xrt_json_get_name_owned(");
            emit_value_as_rep_ctx(ctx, out, receiver, XR_REP_TAGGED);
            fprintf(out, ", ");
            xicgen_emit_c_string_literal(out, field ? field : "?");
            fprintf(out, ")");
        } else {
            fprintf(out, "xrt_getprop_name(");
            emit_value_as_rep_ctx(ctx, out, receiver, XR_REP_TAGGED);
            fprintf(out, ", ");
            xicgen_emit_c_string_literal(out, field ? field : "?");
            fprintf(out, ")");
        }
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
    if (emit_class_native_instance_field_store_expr(ctx, out, f, v, prefix))
        return;
    const char *field = (const char *) v->aux;
    if (cg_value_type_is_json(v->args[0])) {
        fprintf(out, "xrt_json_set_name(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        xicgen_emit_c_string_literal(out, field ? field : "?");
        fprintf(out, ", ");
        emit_boxed_value_ref(out, v->args[1]);
        fprintf(out, ")");
    } else {
        fprintf(out, "xrt_setprop_name(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        xicgen_emit_c_string_literal(out, field ? field : "?");
        fprintf(out, ", ");
        emit_boxed_value_ref(out, v->args[1]);
        fprintf(out, ")");
    }
}

static void xicgen_index_get(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                             const char *prefix) {
    XR_DCHECK(v->nargs >= 2, "xicgen_index_get: need obj and key");
    if (emit_struct_fixed_array_index_get_expr(ctx, out, f, v, prefix) ||
        emit_typed_array_index_get_expr(ctx, out, f, v, prefix) ||
        emit_class_native_array_index_get_expr(ctx, out, f, v))
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
    if (emit_class_native_array_index_set_expr(ctx, out, f, v))
        return;
    fprintf(out, "xrt_index_set(");
    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep(out, v->args[2], XR_REP_TAGGED);
    fprintf(out, ")");
}

/* Object spread merge: `xrt_json_merge(dst, src)`. */
static void xicgen_json_merge(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 2, "xicgen_json_merge: need dst and src");
    fprintf(out, "xrt_json_merge(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ")");
}

/* Array spread append: `xrt_array_push(dst, val)`.  Always routed through the
 * tagged runtime helper (no typed-array data cache): the destination grows, so
 * prepare marks it uncacheable (array_value_has_uncacheable_use). */
static void xicgen_array_push(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 2, "xicgen_array_push: need array and value");
    fprintf(out, "xrt_array_push(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ")");
}

/* Array spread splice: `xrt_array_extend(dst, src)`. */
static void xicgen_array_extend(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                const char *prefix) {
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 2, "xicgen_array_extend: need dst and src");
    fprintf(out, "xrt_array_extend(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
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
        if (xicgen_type_is_unsigned_int(v->args[0] ? v->args[0]->type : NULL)) {
            fprintf(out, "xrt_uint64_to_string((uint64_t)");
            emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
            fprintf(out, ")");
        } else {
            fprintf(out, "xrt_to_string(");
            emit_boxed_value_ref(out, v->args[0]);
            fprintf(out, ")");
        }
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
    } else if (v->type->kind == XR_KIND_CHAR) {
        /* char(x): tagged XR_TAG_CHAR result, validated Unicode scalar. */
        fprintf(out, "xrt_to_char(");
        emit_boxed_value_ref(out, v->args[0]);
        fprintf(out, ")");
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
    (void) ctx;
    (void) f;
    (void) prefix;
    /* Route through the validating value helper so bounds/type errors raise the
     * same panics as the VM OP_BYTES_LOAD_U32_LE (offset is int64; an out-of-range
     * offset throws instead of silently truncating to int32 like the raw path). */
    const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
    fprintf(out, "XR_TO_INT(xrt_bytes_load_u32_le(");
    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
    fprintf(out, "))");
    emit_conversion_suffix(out, conv_suffix);
}

static void xicgen_bytes_load_u64_le(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                     const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
    fprintf(out, "XR_TO_INT(xrt_bytes_load_u64_le(");
    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
    fprintf(out, "))");
    emit_conversion_suffix(out, conv_suffix);
}

static void xicgen_bytes_copy_within(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                     const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    /* Route through the validating value helper so a bad receiver/integer args or
     * an out-of-range copy raise the same panics as the VM, instead of silently
     * no-op'ing via the raw path. */
    bool boxed = cg_rep(v) == XR_REP_TAGGED;
    if (!boxed)
        fprintf(out, "((xrt_array_t *)(");
    fprintf(out, "xrt_bytes_copy_within_value(");
    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep(out, v->args[2], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep(out, v->args[3], XR_REP_TAGGED);
    fprintf(out, ")");
    if (!boxed)
        fprintf(out, ").ptr)");
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

/* FFI raw-pointer access: pick the exact C scalar type for a pointee width.
 * XI_PTR_LOAD/STORE carry the XrFFIType code in aux_int. */
static const char *cg_ffi_pointee_c_type(uint8_t code) {
    switch ((XrFFIType) code) {
        case XR_FFI_T_I8:
            return "int8_t";
        case XR_FFI_T_U8:
            return "uint8_t";
        case XR_FFI_T_I16:
            return "int16_t";
        case XR_FFI_T_U16:
            return "uint16_t";
        case XR_FFI_T_I32:
            return "int32_t";
        case XR_FFI_T_U32:
            return "uint32_t";
        case XR_FFI_T_I64:
            return "int64_t";
        case XR_FFI_T_U64:
            return "uint64_t";
        case XR_FFI_T_F32:
            return "float";
        case XR_FFI_T_F64:
            return "double";
        case XR_FFI_T_BOOL:
            return "uint8_t";
        case XR_FFI_T_PTR:
            return "void *";
        case XR_FFI_T_VOID:
        default:
            return "int64_t";
    }
}

static bool cg_ffi_code_is_float(uint8_t code) {
    return (XrFFIType) code == XR_FFI_T_F32 || (XrFFIType) code == XR_FFI_T_F64;
}

static bool cg_ffi_code_is_ptr(uint8_t code) {
    return (XrFFIType) code == XR_FFI_T_PTR;
}

/* R[dst] = *(T*)addr — inline typed load from a raw address. */
static void xicgen_ptr_load(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                            const char *prefix) {
    (void) f;
    (void) prefix;
    uint8_t code = (uint8_t) (v->aux_int & 0xff);
    const char *cty = cg_ffi_pointee_c_type(code);
    XrRep from_rep = cg_ffi_code_is_float(code) ? XR_REP_F64 : XR_REP_I64;
    const char *conv_suffix =
        emit_conversion_prefix(out, v->type, from_rep, cg_value_plan_storage_rep(ctx, v));
    if (cg_ffi_code_is_float(code))
        fprintf(out, "(double)");
    else if (cg_ffi_code_is_ptr(code))
        fprintf(out, "(int64_t)(uintptr_t)");
    else
        fprintf(out, "(int64_t)");
    fprintf(out, "(*(%s *)(uintptr_t)(", cty);
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
    fprintf(out, "))");
    emit_conversion_suffix(out, conv_suffix);
}

/* *(T*)addr = value — inline typed store to a raw address (void statement). */
static void xicgen_ptr_store(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                             const char *prefix) {
    (void) f;
    (void) prefix;
    uint8_t code = (uint8_t) (v->aux_int & 0xff);
    const char *cty = cg_ffi_pointee_c_type(code);
    fprintf(out, "(*(%s *)(uintptr_t)(", cty);
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
    fprintf(out, ")) = ");
    if (cg_ffi_code_is_float(code)) {
        fprintf(out, "(%s)(", cty);
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_F64);
        fprintf(out, ")");
    } else if (cg_ffi_code_is_ptr(code)) {
        fprintf(out, "(void *)(uintptr_t)(");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
        fprintf(out, ")");
    } else {
        fprintf(out, "(%s)(", cty);
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
        fprintf(out, ")");
    }
}

static void xicgen_gen_call(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                            const char *prefix);

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
