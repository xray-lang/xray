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
    return value_plan && arg_plan && xaot_value_reps_equal(value_plan->rep, arg_plan->rep);
}

static const XiValue *xicgen_getprop_receiver_value(XiCgenCtx *ctx, const XiValue *v) {
    while (v && (v->op == XI_UNBOX || xi_copy_is_identity_alias(v) || v->op == XI_MOVE) &&
           v->nargs >= 1 && xicgen_same_rep_identity_alias(ctx, v, v->args[0])) {
        v = v->args[0];
    }
    return v;
}

static const XiEnumData *xicgen_adt_enum_for_type(XiCgenCtx *ctx, const XrType *type) {
    if (!ctx || !type)
        return NULL;
    const char *name = NULL;
    if (type->kind == XR_KIND_ENUM) {
        name = type->enum_type.enum_name ? type->enum_type.enum_name : type->instance.class_name;
    } else if ((type->kind == XR_KIND_INSTANCE || type->kind == XR_KIND_CLASS) &&
               type->instance.class_name) {
        name = type->instance.class_name;
    }
    if (!name)
        return NULL;
    if (ctx->aot_bundle && ctx->aot_bundle->modules) {
        for (uint32_t mi = 0; mi < ctx->aot_bundle->nmodules; mi++) {
            const XiModule *mod = ctx->aot_bundle->modules[mi];
            if (!mod || !mod->slot_enums)
                continue;
            for (uint16_t si = 0; si < mod->nslots; si++) {
                const XiEnumData *ed = mod->slot_enums[si];
                if (ed && ed->is_adt && ed->name && strcmp(ed->name, name) == 0)
                    return ed;
            }
        }
    }
    if (ctx->all_modules) {
        for (int mi = 0; mi < ctx->all_nmodules; mi++) {
            const XiModule *mod = ctx->all_modules[mi];
            if (!mod || !mod->slot_enums)
                continue;
            for (uint16_t si = 0; si < mod->nslots; si++) {
                const XiEnumData *ed = mod->slot_enums[si];
                if (ed && ed->is_adt && ed->name && strcmp(ed->name, name) == 0)
                    return ed;
            }
        }
    }
    for (int i = 0; i < ctx->nshared; i++) {
        const XiEnumData *ed = ctx->shared_enum[i];
        if (ed && ed->is_adt && ed->name && strcmp(ed->name, name) == 0)
            return ed;
    }
    return NULL;
}

static bool xicgen_emit_adt_field_load(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    if (!v || v->op != XI_LOAD_FIELD || v->nargs < 1 || v->aux || v->aux_int < 0)
        return false;
    if (!xi_load_field_is_adt(v) &&
        !xicgen_adt_enum_for_type(ctx, v->args[0] ? v->args[0]->type : NULL))
        return false;

    if (cg_value_plan_is_aggregate(ctx, v->args[0])) {
        XrRep from_rep = v->aux_int == 0 ? XR_REP_I64 : XR_REP_TAGGED;
        const char *conv_suffix =
            emit_conversion_prefix(out, v->type, from_rep, cg_value_plan_storage_rep(ctx, v));
        if (v->aux_int == 0) {
            fprintf(out, "(");
            emit_vref(out, v->args[0]);
            fprintf(out, ").tag");
        } else if (v->aux_int == 1) {
            fprintf(out, "(");
            emit_vref(out, v->args[0]);
            fprintf(out, ").payload0");
        } else {
            fprintf(out, "XR_NULL_VAL");
        }
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }

    const char *conv_suffix =
        emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_value_plan_storage_rep(ctx, v));
    fprintf(out, "((XrValue*)((xrt_array_t*)(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ").ptr)->data)[%" PRId64 "]", v->aux_int);
    emit_conversion_suffix(out, conv_suffix);
    return true;
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
    if (cg_value_plan_is_aggregate(ctx, v) && cg_value_plan_is_aggregate(ctx, v->args[0]) &&
        xicgen_same_rep_identity_alias(ctx, v, v->args[0])) {
        xicgen_identity(ctx, out, f, v, prefix);
        return;
    }
    if (cg_debug_boxed_adapter_enabled() && cg_value_plan_is_aggregate(ctx, v)) {
        const XaotValuePlan *vp = cg_value_plan(ctx, v);
        const XaotValuePlan *ap = cg_value_plan(ctx, v->args[0]);
        fprintf(stderr,
                "[xi_cgen][boxed] aggregate copy not identity %s v%u arg=v%u/%s "
                "value(kind=%d rep=%d flags=%u c=%s) arg(kind=%d rep=%d flags=%u c=%s)\n",
                f && f->name ? f->name : "?", (unsigned) v->id,
                v->args[0] ? (unsigned) v->args[0]->id : 0,
                v->args[0] ? xi_op_name((XiOp) v->args[0]->op) : "?", vp ? (int) vp->rep.kind : -1,
                vp ? (int) vp->rep.rep : -1, vp ? vp->rep.flags : 0,
                (vp && vp->rep.c_type) ? vp->rep.c_type : "<null>", ap ? (int) ap->rep.kind : -1,
                ap ? (int) ap->rep.rep : -1, ap ? ap->rep.flags : 0,
                (ap && ap->rep.c_type) ? ap->rep.c_type : "<null>");
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
    } else if (result_rep == XR_REP_RAWPTR) {
        if (!emit_native_rawptr_arith_expr(ctx, out, v))
            emit_codegen_abort_expr(out);
    } else if (result_rep == XR_REP_I64) {
        if (cg_arith_is_clean_narrow(ctx, f, v)) {
            fprintf(out, "(%s)(", local_ctype_str_ctx(ctx, f, v));
            cg_emit_narrow_arith_operand(ctx, f, out, v->args[0]);
            fprintf(out, " %s ", xi_to_c_template_arith_native_op(v->op));
            cg_emit_narrow_arith_operand(ctx, f, out, v->args[1]);
            fprintf(out, ")");
        } else if (emit_native_unsigned_wrap_arith_expr(ctx, out, v)) {
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
        if (!emit_native_const_div_mod_expr(out, v) &&
            !emit_native_positive_divisor_div_mod_expr(out, f, v)) {
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
    const XiValue *value = (v && v->nargs >= 1) ? v->args[0] : NULL;
    fprintf(out, "(%s[%d] = ", ctx->shared_name, (int) v->aux_int);
    if (cg_value_plan_is_struct_aggregate(ctx, value)) {
        if (!emit_struct_aggregate_box_expr(ctx, out, f, value, prefix)) {
            fprintf(stderr, "[xi_cgen] ERROR: cannot box aggregate v%u for shared slot %d in %s\n",
                    value ? value->id : 0, (int) v->aux_int, f && f->name ? f->name : "?");
            ctx->error = true;
            emit_codegen_abort_expr(out);
        }
    } else {
        emit_value_as_rep_ctx(ctx, out, value, XR_REP_TAGGED);
    }
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
    } else if (v->aux_int == XR_GLOBAL_VAR_ATOMIC || v->aux_int == XR_GLOBAL_VAR_WORKQUEUE ||
               v->aux_int == XR_GLOBAL_VAR_RESULTGROUP ||
               v->aux_int == XR_GLOBAL_VAR_COUNTDOWNLATCH ||
               v->aux_int == XR_GLOBAL_VAR_SEMAPHORE || v->aux_int == XR_GLOBAL_VAR_EVENTCOUNT ||
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

static const XiClassData *xicgen_shared_class_data(XiCgenCtx *ctx, const XiValue *callee) {
    if (!ctx || !callee)
        return NULL;
    if (callee->op == XI_GET_SHARED) {
        int slot = (int) callee->aux_int;
        const XiClassData *cd = cg_class_descriptor_slot_data(ctx, slot);
        if (cd)
            return cd;
        return slot >= 0 && slot < ctx->shared_cap ? ctx->shared_class[slot] : NULL;
    }
    if ((callee->op == XI_BOX || callee->op == XI_UNBOX) && callee->nargs >= 1)
        return xicgen_shared_class_data(ctx, callee->args[0]);
    return NULL;
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

static bool xicgen_call_is_atomic_constructor(const XiValue *callee) {
    const XiValue *origin = cg_unwrap_identity_value(callee);
    return origin && origin->op == XI_GET_BUILTIN && origin->aux_int == XR_GLOBAL_VAR_ATOMIC;
}

static bool xicgen_call_is_result_group_constructor(const XiValue *callee) {
    const XiValue *origin = cg_unwrap_identity_value(callee);
    return origin && origin->op == XI_GET_BUILTIN && origin->aux_int == XR_GLOBAL_VAR_RESULTGROUP;
}

static bool xicgen_call_is_countdown_latch_constructor(const XiValue *callee) {
    const XiValue *origin = cg_unwrap_identity_value(callee);
    return origin && origin->op == XI_GET_BUILTIN &&
           origin->aux_int == XR_GLOBAL_VAR_COUNTDOWNLATCH;
}

static bool xicgen_call_is_semaphore_constructor(const XiValue *callee) {
    const XiValue *origin = cg_unwrap_identity_value(callee);
    return origin && origin->op == XI_GET_BUILTIN && origin->aux_int == XR_GLOBAL_VAR_SEMAPHORE;
}

static bool xicgen_call_is_event_count_constructor(const XiValue *callee) {
    const XiValue *origin = cg_unwrap_identity_value(callee);
    return origin && origin->op == XI_GET_BUILTIN && origin->aux_int == XR_GLOBAL_VAR_EVENTCOUNT;
}

typedef enum {
    CG_ATOMIC_INT,
    CG_ATOMIC_FLOAT,
    CG_ATOMIC_BOOL,
    CG_ATOMIC_UNKNOWN,
} CgAtomicKind;

static const XrType *xicgen_atomic_inner_type_from_receiver(const XiValue *recv) {
    const XiValue *origin = cg_unwrap_identity_value(recv);
    const XrType *type = origin ? origin->type : (recv ? recv->type : NULL);
    if (!type || type->kind != XR_KIND_INSTANCE || !type->instance.class_name ||
        strcmp(type->instance.class_name, "Atomic") != 0 || type->instance.type_arg_count == 0)
        return NULL;
    return type->instance.type_args ? type->instance.type_args[0] : NULL;
}

static CgAtomicKind xicgen_atomic_kind_from_type(const XrType *type) {
    if (!type)
        return CG_ATOMIC_UNKNOWN;
    if (type->kind == XR_KIND_FLOAT)
        return CG_ATOMIC_FLOAT;
    if (type->kind == XR_KIND_BOOL)
        return CG_ATOMIC_BOOL;
    if (type->kind == XR_KIND_INT)
        return CG_ATOMIC_INT;
    return CG_ATOMIC_UNKNOWN;
}

static CgAtomicKind xicgen_atomic_kind_from_receiver(const XiValue *recv) {
    CgAtomicKind kind = xicgen_atomic_kind_from_type(xicgen_atomic_inner_type_from_receiver(recv));
    return kind == CG_ATOMIC_UNKNOWN ? CG_ATOMIC_INT : kind;
}

static bool xicgen_ordering_member_index(const char *name, int64_t *out_index) {
    if (!name || !out_index)
        return false;
    if (strcmp(name, "Relaxed") == 0) {
        *out_index = XR_AOT_ORDERING_RELAXED;
        return true;
    }
    if (strcmp(name, "Acquire") == 0) {
        *out_index = XR_AOT_ORDERING_ACQUIRE;
        return true;
    }
    if (strcmp(name, "Release") == 0) {
        *out_index = XR_AOT_ORDERING_RELEASE;
        return true;
    }
    if (strcmp(name, "AcquireRelease") == 0) {
        *out_index = XR_AOT_ORDERING_ACQUIRE_RELEASE;
        return true;
    }
    if (strcmp(name, "SeqCst") == 0) {
        *out_index = XR_AOT_ORDERING_SEQ_CST;
        return true;
    }
    return false;
}

static bool xicgen_value_is_ordering_member(const XiValue *value, int64_t *out_index) {
    const XiValue *origin = cg_unwrap_identity_value(value);
    if (!origin || origin->op != XI_LOAD_FIELD || origin->nargs < 1 || !origin->aux)
        return false;
    const XiValue *receiver = cg_unwrap_identity_value(origin->args[0]);
    if (!receiver || receiver->op != XI_GET_BUILTIN || receiver->aux_int != XR_GLOBAL_VAR_ORDERING)
        return false;
    return xicgen_ordering_member_index((const char *) origin->aux, out_index);
}

static bool xicgen_value_is_const_ordering(const XiValue *value, int64_t *out_index) {
    if (!out_index)
        return false;
    if (!value) {
        *out_index = XR_AOT_ORDERING_SEQ_CST;
        return true;
    }
    if (xicgen_value_is_ordering_member(value, out_index))
        return true;
    const XiValue *origin = cg_unwrap_identity_value(value);
    if (origin && origin->op == XI_CONST && origin->type && origin->type->kind == XR_KIND_INT &&
        origin->aux_int >= XR_AOT_ORDERING_RELAXED && origin->aux_int <= XR_AOT_ORDERING_SEQ_CST) {
        *out_index = origin->aux_int;
        return true;
    }
    return false;
}

typedef enum {
    CG_ATOMIC_ORDER_LOAD,
    CG_ATOMIC_ORDER_STORE,
    CG_ATOMIC_ORDER_RMW,
} CgAtomicOrderUse;

static const char *xicgen_atomic_c11_order_name(CgAtomicOrderUse use, int64_t ordering) {
    switch (use) {
        case CG_ATOMIC_ORDER_LOAD:
            switch (ordering) {
                case XR_AOT_ORDERING_RELAXED:
                    return "memory_order_relaxed";
                case XR_AOT_ORDERING_ACQUIRE:
                case XR_AOT_ORDERING_ACQUIRE_RELEASE:
                    return "memory_order_acquire";
                case XR_AOT_ORDERING_SEQ_CST:
                    return "memory_order_seq_cst";
                case XR_AOT_ORDERING_RELEASE:
                    return "memory_order_relaxed";
            }
            break;
        case CG_ATOMIC_ORDER_STORE:
            switch (ordering) {
                case XR_AOT_ORDERING_RELAXED:
                    return "memory_order_relaxed";
                case XR_AOT_ORDERING_RELEASE:
                case XR_AOT_ORDERING_ACQUIRE_RELEASE:
                    return "memory_order_release";
                case XR_AOT_ORDERING_SEQ_CST:
                    return "memory_order_seq_cst";
                case XR_AOT_ORDERING_ACQUIRE:
                    return "memory_order_relaxed";
            }
            break;
        case CG_ATOMIC_ORDER_RMW:
            switch (ordering) {
                case XR_AOT_ORDERING_RELAXED:
                    return "memory_order_relaxed";
                case XR_AOT_ORDERING_ACQUIRE:
                    return "memory_order_acquire";
                case XR_AOT_ORDERING_RELEASE:
                    return "memory_order_release";
                case XR_AOT_ORDERING_ACQUIRE_RELEASE:
                    return "memory_order_acq_rel";
                case XR_AOT_ORDERING_SEQ_CST:
                    return "memory_order_seq_cst";
            }
            break;
    }
    return "memory_order_seq_cst";
}

static void xicgen_emit_atomic_ordering_arg(FILE *out, const XiValue *value) {
    if (!value) {
        fprintf(out, "XR_AOT_ORDERING_SEQ_CST");
        return;
    }
    int64_t member_index = 0;
    if (xicgen_value_is_ordering_member(value, &member_index)) {
        fprintf(out, "%" PRId64, member_index);
        return;
    }
    const XiValue *origin = cg_unwrap_identity_value(value);
    if (origin && origin->type && origin->type->kind == XR_KIND_INT) {
        emit_value_as_rep(out, origin, XR_REP_I64);
        return;
    }
    fprintf(out, "xr_aot_atomic_ordering_from_value(");
    emit_value_as_rep(out, value, XR_REP_TAGGED);
    fprintf(out, ")");
}

static XrRep xicgen_atomic_scalar_rep(CgAtomicKind kind) {
    return kind == CG_ATOMIC_FLOAT ? XR_REP_F64 : XR_REP_I64;
}

static const char *xicgen_atomic_suffix(CgAtomicKind kind) {
    switch (kind) {
        case CG_ATOMIC_FLOAT:
            return "f64";
        case CG_ATOMIC_BOOL:
            return "bool";
        case CG_ATOMIC_INT:
        case CG_ATOMIC_UNKNOWN:
            return "i64";
    }
    return "i64";
}

static const char *xicgen_aot_context_expr(XiCgenCtx *ctx, const XiFunc *f) {
    return cg_func_needs_aot_coro_ctx(ctx, f) ? "ctx" : "&xrt_global_ctx";
}

static bool xicgen_type_is_span_like(const XrType *type) {
    return type && type->kind == XR_KIND_SPAN;
}

static const XiValue *xicgen_stack_slice_source_value(const XiValue *arg) {
    const XiValue *slice = cg_unwrap_identity_value(arg);
    return slice && slice->op == XI_SLICE && slice->nargs >= 3 ? slice : NULL;
}

static bool xicgen_direct_call_param_noescape(XiCgenCtx *ctx, const XiFunc *current,
                                              const XiFunc *target, uint16_t param_index,
                                              uint8_t depth);

static bool xicgen_method_arg_keeps_span_noescape(const XiValue *user, uint16_t arg_index) {
    if (!user || (user->op != XI_CALL_METHOD && user->op != XI_CALL_METHOD_DIRECT) || !user->aux)
        return false;
    const char *method = (const char *) user->aux;
    if (arg_index == 0 &&
        (strcmp(method, "getUnchecked") == 0 || strcmp(method, "loadLE") == 0 ||
         strcmp(method, "loadLEUnchecked") == 0 || strcmp(method, "commonPrefixUnchecked") == 0))
        return true;
    if (arg_index == 1 && strcmp(method, "appendFromUnchecked") == 0)
        return true;
    if (arg_index == 2 && strcmp(method, "writeFromUnchecked") == 0)
        return true;
    if (arg_index == 2 && strcmp(method, "wildCopyFromNonOverlappingUnchecked") == 0)
        return true;
    return false;
}

static bool xicgen_rawptr_value_only_used_noescape(XiCgenCtx *ctx, const XiFunc *f,
                                                   const XiValue *value, uint8_t depth) {
    (void) ctx;
    if (!f || !value || depth > 8)
        return false;

    const XiValue *target = cg_unwrap_identity_value(value);
    if (!target)
        return false;

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (cg_unwrap_identity_value(blk->control) == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (cg_unwrap_identity_value(phi->value.args[a]) == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user || user == target)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (cg_unwrap_identity_value(user->args[a]) != target)
                    continue;

                switch ((XiOp) user->op) {
                    case XI_RETAIN:
                    case XI_RELEASE:
                        if (a != 0)
                            return false;
                        break;
                    case XI_COPY:
                    case XI_MOVE:
                    case XI_BOX:
                    case XI_UNBOX:
                    case XI_CONVERT:
                    case XI_CHECKTYPE:
                        if (a != 0 || !xicgen_rawptr_value_only_used_noescape(
                                          ctx, f, user, (uint8_t) (depth + 1)))
                            return false;
                        break;
                    case XI_SELECT:
                        if (!user->type || !XR_TYPE_IS_POINTER(user->type) ||
                            !xicgen_rawptr_value_only_used_noescape(ctx, f, user,
                                                                    (uint8_t) (depth + 1)))
                            return false;
                        break;
                    case XI_PTR_LOAD:
                        if (a != 0)
                            return false;
                        break;
                    case XI_PTR_STORE:
                        if (a != 0)
                            return false;
                        break;
                    case XI_PTR_COPY_NONOVERLAP:
                        if (a > 1)
                            return false;
                        break;
                    default:
                        /* Pointer arithmetic keeps the address noescape when
                         * the result itself stays noescape; pointer equality
                         * never escapes. Spelled as if-tests so the template
                         * lowering guard only sees real emitter cases. */
                        if (user->op == XI_ADD || user->op == XI_SUB) {
                            if (!user->type || !XR_TYPE_IS_POINTER(user->type) ||
                                !xicgen_rawptr_value_only_used_noescape(ctx, f, user,
                                                                        (uint8_t) (depth + 1)))
                                return false;
                            break;
                        }
                        if (user->op == XI_EQ || user->op == XI_NE)
                            break;
                        return false;
                }
            }
        }
    }
    return true;
}

static bool xicgen_op_arg_keeps_span_noescape(XiCgenCtx *ctx, const XiFunc *current,
                                              const XiValue *user, uint16_t arg_index,
                                              uint8_t depth) {
    if (!user)
        return false;
    switch ((XiOp) user->op) {
        case XI_RETAIN:
        case XI_RELEASE:
        case XI_COPY:
        case XI_MOVE:
        case XI_BOX:
        case XI_UNBOX:
        case XI_CHECKTYPE:
            return arg_index == 0;
        case XI_LOAD_FIELD: {
            const char *field = (const char *) user->aux;
            return arg_index == 0 && field &&
                   (strcmp(field, "length") == 0 || strcmp(field, "size") == 0);
        }
        case XI_ARRAY_DATA_PTR:
            return arg_index == 0 && xicgen_rawptr_value_only_used_noescape(ctx, current, user,
                                                                            (uint8_t) (depth + 1));
        case XI_INDEX_GET:
            return arg_index == 0;
        case XI_BYTES_LOAD_U16_LE:
        case XI_BYTES_LOAD_U32_LE:
        case XI_BYTES_LOAD_U64_LE:
            return arg_index == 0;
        case XI_BYTES_COPY_FROM:
            return arg_index == 1;
        case XI_CALL: {
            if (arg_index == 0)
                return false;
            CgStaticFunctionCall static_call =
                cg_resolve_static_function_call(ctx, current, user->args[0]);
            return static_call.func && xicgen_direct_call_param_noescape(
                                           ctx, current, static_call.func,
                                           (uint16_t) (arg_index - 1), (uint8_t) (depth + 1));
        }
        default:
            return xicgen_method_arg_keeps_span_noescape(user, arg_index);
    }
}

static bool xicgen_direct_call_param_noescape(XiCgenCtx *ctx, const XiFunc *current,
                                              const XiFunc *target, uint16_t param_index,
                                              uint8_t depth) {
    (void) current;
    if (!ctx || !target || param_index >= target->nparams || !target->params || depth > 8)
        return false;
    const XiValue *param = target->params[param_index];
    if (!param || !xicgen_type_is_span_like(param->type))
        return false;

    for (uint32_t bi = 0; bi < target->nblocks; bi++) {
        const XiBlock *blk = target->blocks[bi];
        if (!blk)
            continue;
        if (cg_unwrap_identity_value(blk->control) == param)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (cg_unwrap_identity_value(phi->value.args[a]) == param)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user || user == param)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (cg_unwrap_identity_value(user->args[a]) != param)
                    continue;
                if (!xicgen_op_arg_keeps_span_noescape(ctx, target, user, a, depth))
                    return false;
            }
        }
    }
    return true;
}

static bool xicgen_direct_call_arg_can_stack_slice_view(XiCgenCtx *ctx, const XiFunc *current,
                                                        const XiValue *call, const XiFunc *target,
                                                        uint16_t arg_index, const XiValue *arg) {
    (void) ctx;
    (void) current;
    (void) call;
    const XiValue *slice = xicgen_stack_slice_source_value(arg);
    if (!slice || !target || arg_index >= target->nparams || !target->params)
        return false;
    if (!xicgen_direct_call_param_noescape(ctx, current, target, arg_index, 0))
        return false;
    if (!xicgen_type_is_span_like(slice->type))
        return false;
    return xicgen_type_is_span_like(target->params[arg_index]->type);
}

static bool
xicgen_direct_call_arg_can_borrow_stack_slice_view(XiCgenCtx *ctx, const XiFunc *current,
                                                   const XiValue *call, const XiFunc *target,
                                                   uint16_t arg_index, const XiValue *arg) {
    return xicgen_direct_call_arg_can_stack_slice_view(ctx, current, call, target, arg_index,
                                                       arg) &&
           xi_func_param_passing_mode(target, arg_index) == XR_PARAM_IN;
}

static void xicgen_emit_stack_slice_arg_name(FILE *out, const XiValue *call, uint16_t arg_index) {
    fprintf(out, "_xr_stack_slice_%u_%u", call ? call->id : 0u, (unsigned) arg_index);
}

static bool xicgen_direct_call_has_stack_slice_views(XiCgenCtx *ctx, const XiFunc *current,
                                                     const XiValue *call, const XiFunc *target) {
    if (!call || !target || call->nargs <= 1)
        return false;
    for (uint16_t a = 1; a < call->nargs; a++) {
        if (xicgen_direct_call_arg_can_stack_slice_view(ctx, current, call, target,
                                                        (uint16_t) (a - 1), call->args[a]))
            return true;
    }
    return false;
}

static void xicgen_emit_stack_slice_view_expr(XiCgenCtx *ctx, FILE *out, const XiValue *arg,
                                              bool borrowed) {
    const XiValue *slice = xicgen_stack_slice_source_value(arg);
    XR_DCHECK(slice && slice->nargs >= 3, "stack slice view arg must be XI_SLICE");
    fprintf(out, borrowed ? "xrt_array_stack_borrow_slice_view(" : "xrt_array_stack_slice_view(");
    emit_value_as_rep_ctx(ctx, out, slice->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, slice->args[1], XR_REP_I64);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, slice->args[2], XR_REP_I64);
    fprintf(out, ")");
}

static void xicgen_emit_direct_call_arg_stack_aware(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                    const XiValue *call, const XiFunc *target,
                                                    uint16_t arg_index, const XiValue *arg) {
    if (!xicgen_direct_call_arg_can_stack_slice_view(ctx, f, call, target, arg_index, arg)) {
        emit_value_as_direct_call_arg(ctx, out, f, call, target, arg_index, arg);
        return;
    }

    const XaotFuncPlan *target_plan = cg_func_plan(ctx, target);
    if (!target_plan || arg_index >= target_plan->abi.nparams || !target_plan->abi.params) {
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return;
    }
    XrRep to_rep = cg_abi_slot_storage_rep(&target_plan->abi.params[arg_index]);
    const char *conv_suffix =
        emit_conversion_prefix(out, arg ? arg->type : NULL, XR_REP_TAGGED, to_rep);
    xicgen_emit_stack_slice_arg_name(out, call, arg_index);
    emit_conversion_suffix(out, conv_suffix);
}

static bool xicgen_emit_direct_call_with_stack_slice_views(XiCgenCtx *ctx, FILE *out,
                                                           const XiFunc *f, const XiValue *call,
                                                           const char *prefix, const XiFunc *target,
                                                           const char *call_prefix,
                                                           const XiValue *callee) {
    if (!xicgen_direct_call_has_stack_slice_views(ctx, f, call, target))
        return false;

    const XaotFuncPlan *target_plan = cg_func_plan(ctx, target);
    if (!target_plan)
        return false;
    XrRep ret_rep = cg_abi_slot_storage_rep(&target_plan->abi.ret);
    if (ret_rep == XR_REP_VOID)
        return false;

    fprintf(out, "({ ");
    for (uint16_t a = 1; a < call->nargs; a++) {
        if (!xicgen_direct_call_arg_can_stack_slice_view(ctx, f, call, target, (uint16_t) (a - 1),
                                                         call->args[a]))
            continue;
        fprintf(out, "XrValue ");
        xicgen_emit_stack_slice_arg_name(out, call, (uint16_t) (a - 1));
        fprintf(out, " = ");
        bool borrowed = xicgen_direct_call_arg_can_borrow_stack_slice_view(
            ctx, f, call, target, (uint16_t) (a - 1), call->args[a]);
        xicgen_emit_stack_slice_view_expr(ctx, out, call->args[a], borrowed);
        fprintf(out, "; ");
    }

    fprintf(out, "%s _xr_call_result_%u = ",
            target_plan->abi.ret.c_type ? target_plan->abi.ret.c_type : ctype_str(ret_rep),
            call ? call->id : 0u);
    emit_fname(ctx, out, call_prefix ? call_prefix : prefix, target);
    fprintf(out, "(");
    emit_call_hidden_closure(out, f, target, callee);
    for (uint16_t a = 1; a < call->nargs; a++) {
        fprintf(out, ", ");
        xicgen_emit_direct_call_arg_stack_aware(ctx, out, f, call, target, (uint16_t) (a - 1),
                                                call->args[a]);
    }
    fprintf(out, "); ");

    for (uint16_t a = 1; a < call->nargs; a++) {
        if (!xicgen_direct_call_arg_can_stack_slice_view(ctx, f, call, target, (uint16_t) (a - 1),
                                                         call->args[a]))
            continue;
        if (xicgen_direct_call_arg_can_borrow_stack_slice_view(ctx, f, call, target,
                                                               (uint16_t) (a - 1), call->args[a]))
            continue;
        fprintf(out, "xrt_array_stack_slice_view_release(");
        xicgen_emit_stack_slice_arg_name(out, call, (uint16_t) (a - 1));
        fprintf(out, "); ");
    }
    fprintf(out, "_xr_call_result_%u; })", call ? call->id : 0u);
    return true;
}

static bool xicgen_proxy_value_only_feeds_stack_slice_direct_call(XiCgenCtx *ctx, const XiFunc *f,
                                                                  const XiValue *proxy,
                                                                  const XiValue *slice,
                                                                  uint8_t depth) {
    bool saw_stack_call = false;
    if (!ctx || !f || !proxy || !slice || depth > 8)
        return false;

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == proxy || cg_unwrap_identity_value(blk->control) == proxy)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == proxy ||
                    cg_unwrap_identity_value(phi->value.args[a]) == proxy)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user || user == proxy)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (user->args[a] != proxy && cg_unwrap_identity_value(user->args[a]) != proxy)
                    continue;
                if (user->op == XI_RETAIN || user->op == XI_RELEASE)
                    continue;
                if (user->op == XI_CALL && a >= 1) {
                    CgStaticFunctionCall static_call =
                        cg_resolve_static_function_call(ctx, f, user->args[0]);
                    if (static_call.func &&
                        xicgen_direct_call_arg_can_stack_slice_view(
                            ctx, f, user, static_call.func, (uint16_t) (a - 1), user->args[a])) {
                        saw_stack_call = true;
                        continue;
                    }
                }
                if (cg_unwrap_identity_value(user) == slice &&
                    xicgen_proxy_value_only_feeds_stack_slice_direct_call(ctx, f, user, slice,
                                                                          (uint8_t) (depth + 1))) {
                    saw_stack_call = true;
                    continue;
                }
                return false;
            }
        }
    }
    return saw_stack_call;
}

static bool xicgen_slice_value_only_used_by_stack_slice_direct_call(XiCgenCtx *ctx, const XiFunc *f,
                                                                    const XiValue *value) {
    const XiValue *target = cg_unwrap_identity_value(value);
    bool saw_stack_call = false;
    if (!ctx || !f || !target || target->op != XI_SLICE)
        return false;
    if (value != target)
        return xicgen_proxy_value_only_feeds_stack_slice_direct_call(ctx, f, value, target, 0);

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (cg_unwrap_identity_value(blk->control) == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (cg_unwrap_identity_value(phi->value.args[a]) == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user || user == target)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (cg_unwrap_identity_value(user->args[a]) != target)
                    continue;
                if (user->op == XI_RETAIN || user->op == XI_RELEASE)
                    continue;
                if (user->op == XI_CALL && a >= 1) {
                    CgStaticFunctionCall static_call =
                        cg_resolve_static_function_call(ctx, f, user->args[0]);
                    if (static_call.func &&
                        xicgen_direct_call_arg_can_stack_slice_view(
                            ctx, f, user, static_call.func, (uint16_t) (a - 1), user->args[a])) {
                        saw_stack_call = true;
                        continue;
                    }
                }
                if (cg_unwrap_identity_value(user) == target &&
                    xicgen_proxy_value_only_feeds_stack_slice_direct_call(ctx, f, user, target,
                                                                          0)) {
                    saw_stack_call = true;
                    continue;
                }
                return false;
            }
        }
    }
    return saw_stack_call;
}

/* Collects a variadic direct call's trailing arguments (beyond target->nparams)
 * into the rest Array<T> parameter and emits ", <rest_array>" in the target's
 * rest-slot ABI rep. Scalar-element arrays use typed storage (so the callee's
 * typed lane reads match); reference elements use a tagged array. The fixed
 * arguments must already have been emitted by the caller. Mirrors the VM's
 * callee-side packing (OP_CALL / vm_invoke_module). */
static void emit_vararg_rest_arg(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                 const XiFunc *target) {
    uint16_t fixed = target->nparams;
    const XrType *rest_type =
        (target->params && target->params[fixed]) ? target->params[fixed]->type : NULL;
    XrRep rest_rep = cg_func_param_abi_rep(ctx, target, fixed);
    CgArrayElemInfo rest_elem;
    bool rest_typed = cg_array_elem_info_from_type_ctx(ctx, rest_type, &rest_elem) &&
                      rest_elem.rep != XR_REP_TAGGED && rest_elem.ctype;
    if (rest_typed) {
        int64_t rest_count = (int64_t) v->nargs - 1 - (int64_t) fixed;
        if (rest_count < 0)
            rest_count = 0;
        fprintf(out, ", ({ xrt_array_t *_va%u = xrt_array_new_typed_ptr(%" PRId64 ", %s); ", v->id,
                rest_count, rest_elem.elem_name);
        int64_t idx = 0;
        for (uint16_t a = (uint16_t) (fixed + 1); a < v->nargs; a++, idx++) {
            fprintf(out, "((%s*)_va%u->data)[%" PRId64 "] = (%s)", rest_elem.ctype, v->id, idx,
                    rest_elem.ctype);
            emit_value_as_rep(out, v->args[a], rest_elem.rep);
            fprintf(out, "; ");
        }
        const char *rest_suffix = emit_conversion_prefix(out, rest_type, XR_REP_PTR, rest_rep);
        fprintf(out, "_va%u", v->id);
        emit_conversion_suffix(out, rest_suffix);
        fprintf(out, "; })");
    } else {
        fprintf(out, ", ({ XrValue _va%u = xrt_array_new(0); ", v->id);
        for (uint16_t a = (uint16_t) (fixed + 1); a < v->nargs; a++) {
            fprintf(out, "xrt_array_push(_va%u, ", v->id);
            emit_value_as_rep(out, v->args[a], XR_REP_TAGGED);
            fprintf(out, "); ");
        }
        const char *rest_suffix = emit_conversion_prefix(out, rest_type, XR_REP_TAGGED, rest_rep);
        fprintf(out, "_va%u", v->id);
        emit_conversion_suffix(out, rest_suffix);
        fprintf(out, "; })");
    }
}

/* Emits the argument list of a direct call (leading ", " before each), starting
 * at v->args[1] and mapping to the target's parameters. Variadic targets get
 * their trailing arguments packed into the rest Array via emit_vararg_rest_arg.
 * The caller has already emitted the callee name, "(", and the hidden closure
 * or NULL first operand. */
static void emit_direct_call_arg_list(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                      const XiFunc *target) {
    if (target->is_vararg) {
        uint16_t fixed = target->nparams;
        for (uint16_t a = 1; a <= fixed && a < v->nargs; a++) {
            fprintf(out, ", ");
            emit_value_as_direct_call_arg(ctx, out, f, v, target, (uint16_t) (a - 1), v->args[a]);
        }
        emit_vararg_rest_arg(ctx, out, f, v, target);
    } else {
        for (uint16_t a = 1; a < v->nargs; a++) {
            fprintf(out, ", ");
            emit_value_as_direct_call_arg(ctx, out, f, v, target, (uint16_t) (a - 1), v->args[a]);
        }
    }
}

static void xicgen_call(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                        const char *prefix) {
    XR_DCHECK(v->nargs >= 1, "xicgen_call: need callee");
    XiValue *callee = v->args[0];
    CgStaticFunctionCall static_call = cg_resolve_static_function_call(ctx, f, callee);
    const XiFunc *target = static_call.func;
    const char *call_prefix = static_call.prefix;
    const XiClassData *shared_class_data = xicgen_shared_class_data(ctx, callee);
    if (!target && shared_class_data) {
        /* Shared-slot classes are lowered by the module init function, so the
         * class data's child indices are relative to it. Resolve there first:
         * indexing the current function's children (e.g. defer closures)
         * with those indices would silently pick an unrelated function. */
        if (ctx && ctx->module && ctx->module->init)
            target = cg_find_constructor(ctx->module->init, shared_class_data);
        if (!target)
            target = cg_find_constructor(f, shared_class_data);
    }
    bool is_class_call = static_call.is_class_constructor || shared_class_data ||
                         xicgen_resolve_direct_class_ctor(f, callee, &target);

    if (xicgen_call_is_atomic_constructor(callee)) {
        const XiValue *initial = v->nargs >= 2 ? v->args[1] : NULL;
        CgAtomicKind kind =
            xicgen_atomic_kind_from_type(initial ? initial->type : (v->type ? v->type : NULL));
        if (kind == CG_ATOMIC_UNKNOWN)
            kind = CG_ATOMIC_INT;
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        fprintf(out, "xr_aot_atomic_new_%s(%s, ", xicgen_atomic_suffix(kind),
                xicgen_aot_context_expr(ctx, f));
        if (!initial) {
            fprintf(out, kind == CG_ATOMIC_FLOAT ? "0.0" : "0");
        } else if (kind == CG_ATOMIC_FLOAT) {
            emit_value_as_rep(out, initial, XR_REP_F64);
        } else if (kind == CG_ATOMIC_BOOL) {
            fprintf(out, "(");
            emit_value_as_rep(out, initial, XR_REP_I64);
            fprintf(out, ") != 0");
        } else {
            emit_value_as_rep(out, initial, XR_REP_I64);
        }
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return;
    }

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

    if (xicgen_call_is_countdown_latch_constructor(callee)) {
        fprintf(out, "xr_aot_countdown_latch_new(%s, ", xicgen_aot_context_expr(ctx, f));
        if (v->nargs >= 2)
            emit_value_as_rep(out, v->args[1], XR_REP_I64);
        else
            fprintf(out, "0");
        fprintf(out, ")");
        return;
    }

    if (xicgen_call_is_semaphore_constructor(callee)) {
        fprintf(out, "xr_aot_semaphore_new(%s, ", xicgen_aot_context_expr(ctx, f));
        if (v->nargs >= 2)
            emit_value_as_rep(out, v->args[1], XR_REP_I64);
        else
            fprintf(out, "0");
        fprintf(out, ")");
        return;
    }

    if (xicgen_call_is_event_count_constructor(callee)) {
        fprintf(out, "xr_aot_event_count_new(%s, ", xicgen_aot_context_expr(ctx, f));
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
            /* Pass each arg in the constructor's actual parameter ABI, not an
             * unconditional tagged box: a non-suspendable constructor uses the
             * native ABI (e.g. int64_t) for scalar params, so boxing here would
             * mismatch the emitted definition and fail C compilation. */
            emit_value_as_rep(out, v->args[a], cg_func_param_abi_rep(ctx, target, a));
        }
        fprintf(out, "); _inst; })");
        return;
    }

    /* FFI: direct C-ABI call to an @extern function. Emit `c_sym(args)` with no
     * hidden _cl closure and arguments converted to their native C reps (the
     * same conversion as a typed direct call, so e.g. tagged -> double). */
    if (target && target->is_extern) {
        /* FFI: raw pointers cross the C boundary as real C pointers but are held
         * internally as address-width ints. A pointer return converts from the
         * C pointer to whatever storage rep the planner picked for this value
         * (void* stays bare, i64 casts the address, tagged boxes it); other
         * return reps go through the normal conversion prefix/suffix. */
        const XrType *ret_type = target->return_type;
        bool ret_is_ptr = ret_type && ret_type->kind == XR_KIND_POINTER;
        const char *conv_suffix = NULL;
        if (ret_is_ptr) {
            XrRep vrep = xicgen_value_c_storage_rep(ctx, f, v);
            conv_suffix = emit_conversion_prefix(out, ret_type, XR_REP_RAWPTR, vrep);
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
        emit_conversion_suffix(out, conv_suffix);
        return;
    }

    if (target) {
        const char *conv_suffix = emit_direct_call_return_conversion_prefix(ctx, out, f, v, target);
        if (ctx->error) {
            emit_codegen_abort_expr(out);
            return;
        }
        if (xicgen_emit_direct_call_with_stack_slice_views(ctx, out, f, v, prefix, target,
                                                           call_prefix, callee)) {
            emit_conversion_suffix(out, conv_suffix);
            return;
        }
        emit_fname(ctx, out, call_prefix ? call_prefix : prefix, target);
        fprintf(out, "(");
        emit_call_hidden_closure(out, f, target, callee);
        emit_direct_call_arg_list(ctx, out, f, v, target);
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

static void xicgen_emit_uintptr_compare_arg(XiCgenCtx *ctx, FILE *out, const XiValue *arg) {
    if (cg_rep(arg) == XR_REP_RAWPTR) {
        fprintf(out, "(uintptr_t)(");
        emit_value_as_rep_ctx(ctx, out, arg, XR_REP_RAWPTR);
        fprintf(out, ")");
        return;
    }
    if (cg_rep(arg) == XR_REP_PTR) {
        fprintf(out, "(uintptr_t)(");
        emit_value_as_rep_ctx(ctx, out, arg, XR_REP_PTR);
        fprintf(out, ")");
        return;
    }
    fprintf(out, "(uintptr_t)(");
    emit_value_as_rep_ctx(ctx, out, arg, XR_REP_I64);
    fprintf(out, ")");
}

static bool xicgen_compare_uses_rawptr(const XiValue *v) {
    if (!v || v->nargs < 2)
        return false;
    return cg_rep(v->args[0]) == XR_REP_RAWPTR || cg_rep(v->args[1]) == XR_REP_RAWPTR;
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
    (void) f;
    (void) prefix;
    int64_t cap = xicgen_capacity_arg_or_default(v, 8);
    uint8_t flags = (uint8_t) ((v ? v->aux_int : 0) & 0x02);
    if (!flags && !emit_typed_map_new_expr(ctx, out, v, cap)) {
        /* Untyped storage (e.g. string keys) with a scalar declared value
         * type: record the value elem so values() returns lanes matching
         * the consumer's static Array<V> layout. */
        XaotContainerElemPlan vplan;
        if (v && v->type && XR_TYPE_IS_MAP(v->type) && v->type->map.value_type &&
            xaot_container_elem_plan_for_type(v->type->map.value_type, &vplan)) {
            fprintf(out, "xrt_map_new_vt(%" PRId64 ", %s)", cap, vplan.elem_name);
        } else {
            fprintf(out, "xrt_map_new(%" PRId64 ")", cap);
        }
    } else if (flags) {
        fprintf(out, "xrt_map_new_flags(%" PRId64 ", XR_MAP_FLAG_WEAK)", cap);
    }
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
            if (!emit_bytes_new_native_local_expr(ctx, out, f, v)) {
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
    } else if (emit_array_bytes_builtin_expr(ctx, out, f, v, bn)) {
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
    if (nargs == 1 && method && strcmp(method, "pushUnchecked") == 0 &&
        emit_typed_array_push_unchecked_expr(ctx, out, f, prefix, v->args[0], v->args[1]))
        return true;
    if (nargs == 2 && method && strcmp(method, "setUnchecked") == 0 &&
        emit_typed_array_set_unchecked_expr(ctx, out, f, prefix, v))
        return true;
    if (method && strcmp(method, "reserve") == 0 && nargs == 1 &&
        emit_typed_array_reserve_expr(ctx, out, f, prefix, v))
        return true;
    if (method && strcmp(method, "resize") == 0 && nargs >= 1 && nargs <= 2 &&
        emit_typed_array_resize_zero_expr(ctx, out, f, prefix, v))
        return true;
    if (nargs == 3 && method && strcmp(method, "appendFromUnchecked") == 0 &&
        emit_bytes_append_from_unchecked_expr(ctx, out, f, prefix, v))
        return true;
    if (nargs == 2 && method && strcmp(method, "repeatFromUnchecked") == 0 &&
        emit_bytes_repeat_from_unchecked_expr(ctx, out, f, prefix, v))
        return true;
    if (nargs == 4 && method && strcmp(method, "writeFromUnchecked") == 0 &&
        emit_bytes_write_from_unchecked_expr(ctx, out, f, prefix, v))
        return true;
    if (nargs == 3 && method && strcmp(method, "repeatAtUnchecked") == 0 &&
        emit_bytes_repeat_at_unchecked_expr(ctx, out, f, prefix, v))
        return true;
    if (nargs == 4 && method && strcmp(method, "wildCopyFromNonOverlappingUnchecked") == 0 &&
        emit_bytes_wild_copy_from_nonoverlapping_unchecked_expr(ctx, out, f, prefix, v))
        return true;
    if (nargs == 3 && method && strcmp(method, "wildRepeatAtUnchecked") == 0 &&
        emit_bytes_wild_repeat_at_unchecked_expr(ctx, out, f, prefix, v))
        return true;
    if (nargs == 1 && method && strcmp(method, "setLengthUnchecked") == 0 &&
        emit_bytes_set_length_unchecked_expr(ctx, out, f, prefix, v))
        return true;
    if (nargs == 3 && method && strcmp(method, "commonPrefixUnchecked") == 0 &&
        emit_bytes_common_prefix_unchecked_expr(ctx, out, f, prefix, v))
        return true;
    if (method && strcmp(method, "fill") == 0 && nargs >= 1 && nargs <= 3 &&
        emit_typed_array_fill_expr(ctx, out, f, prefix, v))
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

static bool xicgen_emit_enum_method(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                    const char *method) {
    const XiEnumData *recv_enum = cg_enum_for_shared_value_in_func(ctx, f, v->args[0]);
    if (!recv_enum)
        recv_enum = cg_resolve_imported_enum_value(ctx, f, v->args[0]);
    if (!recv_enum)
        recv_enum = xicgen_adt_enum_for_type(ctx, v->type);
    int enum_member = cg_enum_member_index(recv_enum, method);
    if (!recv_enum || enum_member < 0)
        return false;
    if (recv_enum->is_adt && recv_enum->members &&
        recv_enum->members[enum_member].payload_count > 0) {
        emit_adt_enum_construct_expr(ctx, out, recv_enum, enum_member, v);
    } else {
        fprintf(out, "xrt_map_get_owned((xrt_map_t*)");
        emit_vref(out, v->args[0]);
        fprintf(out, ".ptr, ");
        cg_emit_str_value(ctx, out, method);
        fprintf(out, ")");
    }
    return true;
}

static bool xicgen_enum_method_call_is_aggregate_adt_construct(XiCgenCtx *ctx, const XiFunc *f,
                                                               const XiValue *call) {
    const XiValue *v = cg_unwrap_identity_value(call);
    if (!ctx || !v || (v->op != XI_CALL_METHOD && v->op != XI_CALL_METHOD_DIRECT) || !v->aux ||
        v->nargs < 1)
        return false;
    const XiEnumData *recv_enum = cg_enum_for_shared_value_in_func(ctx, f, v->args[0]);
    if (!recv_enum)
        recv_enum = cg_resolve_imported_enum_value(ctx, f, v->args[0]);
    if (!recv_enum)
        recv_enum = xicgen_adt_enum_for_type(ctx, v->type);
    const char *method = (const char *) v->aux;
    int enum_member = cg_enum_member_index(recv_enum, method);
    if (recv_enum && recv_enum->is_adt && enum_member >= 0 && cg_value_plan_is_aggregate(ctx, v))
        return true;

    if (v->op != XI_CALL_METHOD || !cg_value_plan_is_aggregate(ctx, v))
        return false;
    CgStaticFunctionCall module_call = cg_resolve_module_member_call(ctx, f, v, method);
    if (module_call.func)
        return false;
    const char *method_prefix = NULL;
    if (cg_class_native_resolve_method_call(ctx, f, v, &method_prefix))
        return false;
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
    /* StringBuilder.append takes Json (tagged): render the argument as a tagged
     * value. If the rep planner inserted a lossy tagged->i64 UNBOX for the arg
     * (e.g. a null literal), see through it to the original tagged source so the
     * builder appends the real value ("null") instead of its unboxed int (0). */
    const XiValue *append_arg = v->args[1];
    while (append_arg && append_arg->op == XI_UNBOX && append_arg->nargs >= 1)
        append_arg = append_arg->args[0];
    fprintf(out, "(xrt_strbuf_append(");
    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep(out, append_arg, XR_REP_TAGGED);
    fprintf(out, "), ");
    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ")");
    return true;
}

static void xicgen_emit_atomic_arg(FILE *out, const XiValue *arg, CgAtomicKind kind) {
    if (!arg) {
        fprintf(out, kind == CG_ATOMIC_FLOAT ? "0.0" : "0");
        return;
    }
    if (kind == CG_ATOMIC_FLOAT) {
        emit_value_as_rep(out, arg, XR_REP_F64);
    } else if (kind == CG_ATOMIC_BOOL) {
        fprintf(out, "(");
        emit_value_as_rep(out, arg, XR_REP_I64);
        fprintf(out, ") != 0");
    } else {
        emit_value_as_rep(out, arg, XR_REP_I64);
    }
}

static void xicgen_emit_atomic_i64_ref(FILE *out, const XiValue *recv) {
    fprintf(out, "&xr_aot_atomic_view(");
    emit_value_as_rep(out, recv, XR_REP_TAGGED);
    fprintf(out, ")->value");
}

typedef enum {
    CG_ATOMIC_I64_DIRECT_NONE,
    CG_ATOMIC_I64_DIRECT_LOAD,
    CG_ATOMIC_I64_DIRECT_STORE,
    CG_ATOMIC_I64_DIRECT_ADD,
    CG_ATOMIC_I64_DIRECT_SUB,
    CG_ATOMIC_I64_DIRECT_FETCH_ADD,
    CG_ATOMIC_I64_DIRECT_FETCH_SUB,
    CG_ATOMIC_I64_DIRECT_SWAP,
} CgAtomicI64DirectOp;

static CgAtomicI64DirectOp xicgen_atomic_i64_direct_op(const XiValue *v, const char *method,
                                                       uint16_t nargs) {
    if (!v || v->nargs < 1 || !method)
        return CG_ATOMIC_I64_DIRECT_NONE;
    if (strcmp(method, "load") == 0 && (nargs == 0 || nargs == 1))
        return CG_ATOMIC_I64_DIRECT_LOAD;
    if (strcmp(method, "store") == 0 && (nargs == 1 || nargs == 2) && v->nargs >= 2)
        return CG_ATOMIC_I64_DIRECT_STORE;
    if (strcmp(method, "add") == 0 && (nargs == 1 || nargs == 2) && v->nargs >= 2)
        return CG_ATOMIC_I64_DIRECT_ADD;
    if (strcmp(method, "sub") == 0 && (nargs == 1 || nargs == 2) && v->nargs >= 2)
        return CG_ATOMIC_I64_DIRECT_SUB;
    if (strcmp(method, "fetchAdd") == 0 && (nargs == 1 || nargs == 2) && v->nargs >= 2)
        return CG_ATOMIC_I64_DIRECT_FETCH_ADD;
    if (strcmp(method, "fetchSub") == 0 && (nargs == 1 || nargs == 2) && v->nargs >= 2)
        return CG_ATOMIC_I64_DIRECT_FETCH_SUB;
    if (strcmp(method, "swap") == 0 && (nargs == 1 || nargs == 2) && v->nargs >= 2)
        return CG_ATOMIC_I64_DIRECT_SWAP;
    return CG_ATOMIC_I64_DIRECT_NONE;
}

static const XiValue *xicgen_atomic_i64_direct_ordering_arg(const XiValue *v, const char *method,
                                                            uint16_t nargs) {
    if (!v || !method)
        return NULL;
    if (strcmp(method, "load") == 0 && nargs == 1)
        return v->nargs >= 2 ? v->args[1] : NULL;
    if ((strcmp(method, "store") == 0 || strcmp(method, "add") == 0 || strcmp(method, "sub") == 0 ||
         strcmp(method, "fetchAdd") == 0 || strcmp(method, "fetchSub") == 0 ||
         strcmp(method, "swap") == 0) &&
        nargs == 2)
        return v->nargs >= 3 ? v->args[2] : NULL;
    return NULL;
}

static bool xicgen_atomic_call_is_i64_direct_nothrow(const XiValue *call) {
    const XiValue *v = cg_unwrap_identity_value(call);
    if (!v || (v->op != XI_CALL_METHOD && v->op != XI_CALL_METHOD_DIRECT) || v->nargs < 1 ||
        !v->aux || !xi_value_type_is_atomic(v->args[0]))
        return false;

    if (xicgen_atomic_kind_from_receiver(v->args[0]) != CG_ATOMIC_INT)
        return false;

    const char *method = (const char *) v->aux;
    uint16_t nargs = (uint16_t) (v->nargs - 1);
    if (xicgen_atomic_i64_direct_op(v, method, nargs) == CG_ATOMIC_I64_DIRECT_NONE)
        return false;

    int64_t ordering = XR_AOT_ORDERING_SEQ_CST;
    return xicgen_value_is_const_ordering(xicgen_atomic_i64_direct_ordering_arg(v, method, nargs),
                                          &ordering);
}

static bool xicgen_atomic_err_check_after_direct_nothrow(const XiValue *check) {
    if (!check || check->op != XI_ERR_CHECK || cg_value_type_is_bool(check))
        return false;
    return xicgen_atomic_call_is_i64_direct_nothrow(cg_class_native_prev_error_source_value(check));
}

static bool xicgen_func_has_error_flow(XiCgenCtx *ctx, const XiFunc *f, uint8_t depth);

static bool xicgen_value_is_nothrow_native_scalar(const XiFunc *f, const XiValue *value) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v)
        return false;

    /* Use-shape analysis over patterned ops (not lowering dispatch): the
     * if-chain form keeps the template-lowering guard focused on real
     * handwritten `case XI_*:` emitters. */
    XiOp op = (XiOp) v->op;
    if (op == XI_EQ || op == XI_NE || op == XI_LT || op == XI_LE || op == XI_GT || op == XI_GE)
        return v->nargs >= 2 && cg_rep(v->args[0]) != XR_REP_TAGGED &&
               cg_rep(v->args[1]) != XR_REP_TAGGED;
    if (op == XI_NOT)
        return v->nargs >= 1 && cg_rep(v->args[0]) != XR_REP_TAGGED;
    if (op == XI_ADD || op == XI_SUB || op == XI_MUL || op == XI_BAND || op == XI_BOR ||
        op == XI_BXOR || op == XI_SHL || op == XI_SHR)
        return v->nargs >= 2 && cg_rep(v) == XR_REP_I64 && cg_rep(v->args[0]) == XR_REP_I64 &&
               cg_rep(v->args[1]) == XR_REP_I64;
    if (op == XI_BNOT || op == XI_NEG)
        return v->nargs >= 1 && cg_rep(v) == XR_REP_I64 && cg_rep(v->args[0]) == XR_REP_I64;
    if (op == XI_DIV || op == XI_MOD)
        return cg_div_mod_is_trusted_nothrow(f, v);
    return false;
}

static bool xicgen_runtime_method_call_is_direct_nothrow(const XiValue *call) {
    const XiValue *v = cg_unwrap_identity_value(call);
    if (!v || (v->op != XI_CALL_METHOD && v->op != XI_CALL_METHOD_DIRECT) || v->nargs < 1 ||
        !v->aux)
        return false;

    const char *method = (const char *) v->aux;
    uint16_t nargs = (uint16_t) (v->nargs - 1);

    if (xi_value_type_is_result_group(v->args[0])) {
        return (strcmp(method, "add") == 0 && nargs == 1) ||
               (strcmp(method, "flush") == 0 && nargs == 0) ||
               (strcmp(method, "reset") == 0 && nargs == 1) ||
               (strcmp(method, "close") == 0 && nargs == 0);
    }
    if (xi_value_type_is_countdown_latch(v->args[0])) {
        return (strcmp(method, "reset") == 0 && nargs == 1) ||
               (strcmp(method, "done") == 0 && (nargs == 0 || nargs == 1)) ||
               (strcmp(method, "tryWait") == 0 && nargs == 0) ||
               (strcmp(method, "close") == 0 && nargs == 0);
    }
    if (xi_value_type_is_semaphore(v->args[0])) {
        return (strcmp(method, "release") == 0 && (nargs == 0 || nargs == 1)) ||
               (strcmp(method, "tryAcquire") == 0 && nargs == 0) ||
               (strcmp(method, "close") == 0 && nargs == 0);
    }
    if (xi_value_type_is_event_count(v->args[0])) {
        return (strcmp(method, "advance") == 0 && (nargs == 0 || nargs == 1)) ||
               (strcmp(method, "close") == 0 && nargs == 0);
    }
    return false;
}

static bool xicgen_call_is_nothrow_direct_depth(XiCgenCtx *ctx, const XiFunc *current,
                                                const XiValue *call, uint8_t depth) {
    const XiValue *v = cg_unwrap_identity_value(call);
    if (!ctx || !current || !v || depth > 8)
        return false;

    if (xicgen_atomic_call_is_i64_direct_nothrow(v))
        return true;
    if (cg_array_call_is_unchecked_bytes_trusted_nothrow(ctx, current, v))
        return true;
    if (cg_array_call_is_typed_fill_trusted_nothrow(ctx, current, v))
        return true;
    if (cg_array_builtin_call_is_trusted_nothrow(ctx, current, v))
        return true;
    if (xicgen_runtime_method_call_is_direct_nothrow(v))
        return true;
    if (cg_class_native_call_is_nothrow_direct(ctx, current, v))
        return true;

    if (v->op != XI_CALL || v->nargs < 1)
        return false;

    CgStaticFunctionCall direct = cg_resolve_static_function_call(ctx, current, v->args[0]);
    const XiFunc *target = direct.func;
    if (!target || target == current || target->is_extern || direct.is_class_constructor ||
        cg_func_needs_aot_coro(target))
        return false;
    return !xicgen_func_has_error_flow(ctx, target, (uint8_t) (depth + 1));
}

static bool xicgen_value_is_proven_nothrow(XiCgenCtx *ctx, const XiFunc *current,
                                           const XiValue *value, uint8_t depth) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v)
        return false;
    if (xicgen_value_is_nothrow_native_scalar(current, v))
        return true;
    if (cg_array_bytes_load_le_unchecked_trusted_nothrow(ctx, current, v))
        return true;
    return xicgen_call_is_nothrow_direct_depth(ctx, current, v, depth);
}

static bool xicgen_err_check_after_proven_nothrow(XiCgenCtx *ctx, const XiFunc *current,
                                                  const XiValue *check) {
    if (!check || check->op != XI_ERR_CHECK || cg_value_type_is_bool(check))
        return false;
    return xicgen_value_is_proven_nothrow(ctx, current,
                                          cg_class_native_prev_error_source_value(check), 0);
}

static bool xicgen_func_has_error_flow(XiCgenCtx *ctx, const XiFunc *f, uint8_t depth) {
    if (!ctx || !f || depth > 8)
        return true;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *block = f->blocks ? f->blocks[bi] : NULL;
        if (!block)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            const XiValue *value = block->values ? block->values[vi] : NULL;
            if (!value)
                continue;
            if (value->op == XI_ERR_CHECK && xicgen_err_check_after_proven_nothrow(ctx, f, value))
                continue;
            if (value->op == XI_THROW || value->op == XI_ERR_SET || value->op == XI_ERR_RETURN ||
                value->op == XI_ERR_CHECK || value->op == XI_ERR_CATCH || value->op == XI_TRY ||
                value->op == XI_CATCH || value->op == XI_END_TRY || value->op == XI_DEFER)
                return true;
            if (value->flags & XI_FLAG_MAY_SUSPEND)
                return true;
            if ((value->flags & XI_FLAG_MAY_THROW) &&
                !xicgen_value_is_proven_nothrow(ctx, f, value, (uint8_t) (depth + 1)))
                return true;
        }
    }
    return false;
}

static bool xicgen_emit_atomic_i64_direct(FILE *out, const XiValue *v, const XiValue *ordering_arg,
                                          const char *method, uint16_t nargs) {
    if (!v || v->nargs < 1 || !method)
        return false;

    int64_t ordering = XR_AOT_ORDERING_SEQ_CST;
    if (!xicgen_value_is_const_ordering(ordering_arg, &ordering))
        return false;

    CgAtomicI64DirectOp op = xicgen_atomic_i64_direct_op(v, method, nargs);
    if (op == CG_ATOMIC_I64_DIRECT_NONE)
        return false;

    if (op == CG_ATOMIC_I64_DIRECT_LOAD) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
        fprintf(out, "atomic_load_explicit(");
        xicgen_emit_atomic_i64_ref(out, v->args[0]);
        fprintf(out, ", %s)", xicgen_atomic_c11_order_name(CG_ATOMIC_ORDER_LOAD, ordering));
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }

    if (op == CG_ATOMIC_I64_DIRECT_STORE || op == CG_ATOMIC_I64_DIRECT_ADD ||
        op == CG_ATOMIC_I64_DIRECT_SUB) {
        if (cg_is_void_like(v)) {
            if (op == CG_ATOMIC_I64_DIRECT_STORE) {
                fprintf(out, "atomic_store_explicit(");
                xicgen_emit_atomic_i64_ref(out, v->args[0]);
                fprintf(out, ", ");
                xicgen_emit_atomic_arg(out, v->args[1], CG_ATOMIC_INT);
                fprintf(out, ", %s)",
                        xicgen_atomic_c11_order_name(CG_ATOMIC_ORDER_STORE, ordering));
            } else {
                fprintf(out, "atomic_fetch_%s_explicit(",
                        op == CG_ATOMIC_I64_DIRECT_ADD ? "add" : "sub");
                xicgen_emit_atomic_i64_ref(out, v->args[0]);
                fprintf(out, ", ");
                xicgen_emit_atomic_arg(out, v->args[1], CG_ATOMIC_INT);
                fprintf(out, ", %s)", xicgen_atomic_c11_order_name(CG_ATOMIC_ORDER_RMW, ordering));
            }
            return true;
        }

        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        fprintf(out, "(");
        if (op == CG_ATOMIC_I64_DIRECT_STORE) {
            fprintf(out, "atomic_store_explicit(");
            xicgen_emit_atomic_i64_ref(out, v->args[0]);
            fprintf(out, ", ");
            xicgen_emit_atomic_arg(out, v->args[1], CG_ATOMIC_INT);
            fprintf(out, ", %s)", xicgen_atomic_c11_order_name(CG_ATOMIC_ORDER_STORE, ordering));
        } else {
            fprintf(out, "atomic_fetch_%s_explicit(",
                    op == CG_ATOMIC_I64_DIRECT_ADD ? "add" : "sub");
            xicgen_emit_atomic_i64_ref(out, v->args[0]);
            fprintf(out, ", ");
            xicgen_emit_atomic_arg(out, v->args[1], CG_ATOMIC_INT);
            fprintf(out, ", %s)", xicgen_atomic_c11_order_name(CG_ATOMIC_ORDER_RMW, ordering));
        }
        fprintf(out, ", XR_NULL_VAL)");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }

    if (op == CG_ATOMIC_I64_DIRECT_FETCH_ADD || op == CG_ATOMIC_I64_DIRECT_FETCH_SUB ||
        op == CG_ATOMIC_I64_DIRECT_SWAP) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
        if (op == CG_ATOMIC_I64_DIRECT_SWAP)
            fprintf(out, "atomic_exchange_explicit(");
        else
            fprintf(out, "atomic_fetch_%s_explicit(",
                    op == CG_ATOMIC_I64_DIRECT_FETCH_ADD ? "add" : "sub");
        xicgen_emit_atomic_i64_ref(out, v->args[0]);
        fprintf(out, ", ");
        xicgen_emit_atomic_arg(out, v->args[1], CG_ATOMIC_INT);
        fprintf(out, ", %s)", xicgen_atomic_c11_order_name(CG_ATOMIC_ORDER_RMW, ordering));
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }

    return false;
}

static bool xicgen_emit_atomic_method(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                      const char *method, uint16_t nargs) {
    if (!v || v->nargs < 1 || !method || !xi_value_type_is_atomic(v->args[0]))
        return false;

    CgAtomicKind kind = xicgen_atomic_kind_from_receiver(v->args[0]);
    const XiValue *ordering_arg = NULL;
    if ((strcmp(method, "load") == 0 && nargs == 1) ||
        (strcmp(method, "toggle") == 0 && nargs == 1)) {
        ordering_arg = v->nargs >= 2 ? v->args[1] : NULL;
    } else if ((strcmp(method, "store") == 0 || strcmp(method, "add") == 0 ||
                strcmp(method, "sub") == 0 || strcmp(method, "fetchAdd") == 0 ||
                strcmp(method, "fetchSub") == 0 || strcmp(method, "swap") == 0) &&
               nargs == 2) {
        ordering_arg = v->nargs >= 3 ? v->args[2] : NULL;
    } else if (strcmp(method, "compareExchange") == 0 && nargs == 3) {
        ordering_arg = v->nargs >= 4 ? v->args[3] : NULL;
    }

    bool is_load = strcmp(method, "load") == 0 && (nargs == 0 || nargs == 1);
    bool is_store = strcmp(method, "store") == 0 && (nargs == 1 || nargs == 2) && v->nargs >= 2;
    bool is_add = strcmp(method, "add") == 0 && (nargs == 1 || nargs == 2) && v->nargs >= 2;
    bool is_sub = strcmp(method, "sub") == 0 && (nargs == 1 || nargs == 2) && v->nargs >= 2;
    bool is_fetch_add =
        strcmp(method, "fetchAdd") == 0 && (nargs == 1 || nargs == 2) && v->nargs >= 2;
    bool is_fetch_sub =
        strcmp(method, "fetchSub") == 0 && (nargs == 1 || nargs == 2) && v->nargs >= 2;
    bool is_swap = strcmp(method, "swap") == 0 && (nargs == 1 || nargs == 2) && v->nargs >= 2;
    bool is_compare_exchange =
        strcmp(method, "compareExchange") == 0 && (nargs == 2 || nargs == 3) && v->nargs >= 3;
    bool is_toggle = strcmp(method, "toggle") == 0 && (nargs == 0 || nargs == 1);
    bool is_to_string = strcmp(method, "toString") == 0 && nargs == 0;

    if (!is_load && !is_store && !is_add && !is_sub && !is_fetch_add && !is_fetch_sub && !is_swap &&
        !is_compare_exchange && !is_toggle && !is_to_string) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT Atomic method '%s'\n", method);
        emit_codegen_abort_expr(out);
        return true;
    }

    if ((is_add || is_sub || is_fetch_add || is_fetch_sub) && kind == CG_ATOMIC_BOOL) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: Atomic<bool>.%s is not supported in AOT\n", method);
        emit_codegen_abort_expr(out);
        return true;
    }

    if (kind == CG_ATOMIC_INT && xicgen_emit_atomic_i64_direct(out, v, ordering_arg, method, nargs))
        return true;

    if (is_load) {
        XrRep expr_rep = xicgen_atomic_scalar_rep(kind);
        bool box_bool = kind == CG_ATOMIC_BOOL && cg_rep(v) == XR_REP_TAGGED;
        const char *conv_suffix =
            box_bool ? NULL : emit_conversion_prefix(out, v->type, expr_rep, cg_rep(v));
        if (box_bool)
            fprintf(out, "XR_FROM_BOOL(");
        fprintf(out, "xr_aot_atomic_load_%s(", xicgen_atomic_suffix(kind));
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        xicgen_emit_atomic_ordering_arg(out, ordering_arg);
        fprintf(out, ")");
        if (box_bool)
            fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }

    if (is_store || is_add || is_sub) {
        if (cg_is_void_like(v)) {
            fprintf(out, "xr_aot_atomic_%s_%s(", is_store ? "store" : (is_add ? "add" : "sub"),
                    xicgen_atomic_suffix(kind));
            emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
            fprintf(out, ", ");
            xicgen_emit_atomic_arg(out, v->args[1], kind);
            fprintf(out, ", ");
            xicgen_emit_atomic_ordering_arg(out, ordering_arg);
            fprintf(out, ")");
            return true;
        }

        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        fprintf(out, "(xr_aot_atomic_%s_%s(", is_store ? "store" : (is_add ? "add" : "sub"),
                xicgen_atomic_suffix(kind));
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        xicgen_emit_atomic_arg(out, v->args[1], kind);
        fprintf(out, ", ");
        xicgen_emit_atomic_ordering_arg(out, ordering_arg);
        fprintf(out, "), XR_NULL_VAL)");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }

    if (is_fetch_add || is_fetch_sub || is_swap) {
        XrRep expr_rep = xicgen_atomic_scalar_rep(kind);
        const char *helper = is_swap ? "swap" : (is_fetch_add ? "fetch_add" : "fetch_sub");
        bool box_bool = kind == CG_ATOMIC_BOOL && cg_rep(v) == XR_REP_TAGGED;
        const char *conv_suffix =
            box_bool ? NULL : emit_conversion_prefix(out, v->type, expr_rep, cg_rep(v));
        if (box_bool)
            fprintf(out, "XR_FROM_BOOL(");
        fprintf(out, "xr_aot_atomic_%s_%s(", helper, xicgen_atomic_suffix(kind));
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        xicgen_emit_atomic_arg(out, v->args[1], kind);
        fprintf(out, ", ");
        xicgen_emit_atomic_ordering_arg(out, ordering_arg);
        fprintf(out, ")");
        if (box_bool)
            fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }

    if (is_compare_exchange) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        if (kind == CG_ATOMIC_FLOAT) {
            fprintf(out,
                    "({ double _atomic_prev_%u = 0.0; bool _atomic_ok_%u = "
                    "xr_aot_atomic_compare_exchange_f64(",
                    v->id, v->id);
        } else if (kind == CG_ATOMIC_BOOL) {
            fprintf(out,
                    "({ bool _atomic_prev_%u = false; bool _atomic_ok_%u = "
                    "xr_aot_atomic_compare_exchange_bool(",
                    v->id, v->id);
        } else {
            fprintf(out,
                    "({ int64_t _atomic_prev_%u = 0; bool _atomic_ok_%u = "
                    "xr_aot_atomic_compare_exchange_i64(",
                    v->id, v->id);
        }
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        xicgen_emit_atomic_arg(out, v->args[1], kind);
        fprintf(out, ", ");
        xicgen_emit_atomic_arg(out, v->args[2], kind);
        fprintf(out, ", ");
        xicgen_emit_atomic_ordering_arg(out, ordering_arg);
        fprintf(out, ", &_atomic_prev_%u); xrt_tuple_make(2, (XrValue[]){", v->id);
        if (kind == CG_ATOMIC_FLOAT)
            fprintf(out, "XR_FROM_FLOAT(_atomic_prev_%u)", v->id);
        else if (kind == CG_ATOMIC_BOOL)
            fprintf(out, "XR_FROM_BOOL(_atomic_prev_%u)", v->id);
        else
            fprintf(out, "XR_FROM_INT(_atomic_prev_%u)", v->id);
        fprintf(out, ", XR_FROM_BOOL(_atomic_ok_%u)}); })", v->id);
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }

    if (is_toggle) {
        bool box_bool = cg_rep(v) == XR_REP_TAGGED;
        const char *conv_suffix =
            box_bool ? NULL : emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
        if (box_bool)
            fprintf(out, "XR_FROM_BOOL(");
        fprintf(out, "xr_aot_atomic_toggle_bool(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        xicgen_emit_atomic_ordering_arg(out, ordering_arg);
        fprintf(out, ")");
        if (box_bool)
            fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }

    if (is_to_string) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        fprintf(out, "xrt_to_string(xr_aot_atomic_load_value(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", XR_AOT_ORDERING_SEQ_CST))");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }

    return false;
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
    bool is_push_range =
        strcmp(method, "pushRange") == 0 && (nargs == 2 || nargs == 3) && v->nargs >= 3;
    bool is_try_pop = strcmp(method, "tryPop") == 0 && (nargs == 0 || nargs == 1);
    bool is_close = strcmp(method, "close") == 0 && nargs == 0;
    bool is_closed = strcmp(method, "isClosed") == 0 && nargs == 0;
    if (!is_push && !is_push_range && !is_try_pop && !is_close && !is_closed)
        return false;

    if (is_push) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
        fprintf(out, "xr_aot_work_queue_push_bool_sync(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ", ");
        if (nargs == 2 && v->nargs >= 3)
            emit_value_as_rep(out, v->args[2], XR_REP_I64);
        else
            fprintf(out, "-1");
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
    } else if (is_push_range) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
        fprintf(out, "xr_aot_work_queue_push_range_i64_sync(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[1], XR_REP_I64);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[2], XR_REP_I64);
        fprintf(out, ", ");
        if (nargs == 3 && v->nargs >= 4)
            emit_value_as_rep(out, v->args[3], XR_REP_I64);
        else
            fprintf(out, "-1");
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
    } else if (is_try_pop) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
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
        emit_conversion_suffix(out, conv_suffix);
    } else if (is_close) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        fprintf(out, "({ xr_aot_work_queue_close_void_sync(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, "); XR_NULL_VAL; })");
        emit_conversion_suffix(out, conv_suffix);
    } else if (is_closed) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        fprintf(out, "xr_aot_work_queue_is_closed_sync(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
    }
    return true;
}

static bool xicgen_emit_result_group_method(FILE *out, const XiValue *v, const char *method,
                                            uint16_t nargs) {
    if (!v || v->nargs < 1 || !method || !xi_value_type_is_result_group(v->args[0]))
        return false;
    bool is_add = strcmp(method, "add") == 0 && nargs == 1 && v->nargs >= 2;
    bool is_flush = strcmp(method, "flush") == 0 && nargs == 0;
    bool is_reset = strcmp(method, "reset") == 0 && nargs == 1 && v->nargs >= 2;
    bool is_try_recv = strcmp(method, "tryRecv") == 0 && nargs == 0;
    bool is_close = strcmp(method, "close") == 0 && nargs == 0;
    if (!is_add && !is_flush && !is_reset && !is_try_recv && !is_close)
        return false;

    if (is_add) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
        fprintf(out, "xr_aot_result_group_add_bool_sync(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[1], XR_REP_I64);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    } else if (is_flush) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        fprintf(out, "({ xr_aot_result_group_flush_void_sync(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, "); XR_NULL_VAL; })");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    } else if (is_reset) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
        fprintf(out, "xr_aot_result_group_reset_bool_sync(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[1], XR_REP_I64);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    } else if (is_try_recv) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        fprintf(out, "({ XrValue _rg_trv_%u = XR_NULL_VAL; bool _rg_trok_%u = ", v->id, v->id);
        fprintf(out, "xr_aot_result_group_try_recv_sync(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out,
                ", &_rg_trv_%u); xrt_tuple_make(2, (XrValue[]){_rg_trv_%u, "
                "XR_FROM_BOOL(_rg_trok_%u)}); })",
                v->id, v->id, v->id);
        emit_conversion_suffix(out, conv_suffix);
        return true;
    } else if (is_close) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        fprintf(out, "({ xr_aot_result_group_close_void_sync(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, "); XR_NULL_VAL; })");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }
    return false;
}

static bool xicgen_emit_countdown_latch_method(FILE *out, const XiValue *v, const char *method,
                                               uint16_t nargs) {
    if (!v || v->nargs < 1 || !method || !xi_value_type_is_countdown_latch(v->args[0]))
        return false;
    bool is_reset = strcmp(method, "reset") == 0 && nargs == 1 && v->nargs >= 2;
    bool is_done = strcmp(method, "done") == 0 && (nargs == 0 || nargs == 1);
    bool is_try_wait = strcmp(method, "tryWait") == 0 && nargs == 0;
    bool is_close = strcmp(method, "close") == 0 && nargs == 0;
    if (!is_reset && !is_done && !is_try_wait && !is_close)
        return false;

    if (is_reset) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
        fprintf(out, "xr_aot_countdown_latch_reset_bool_sync(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[1], XR_REP_I64);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
    } else if (is_done) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
        fprintf(out, "xr_aot_countdown_latch_done_i64_sync(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        if (nargs == 1 && v->nargs >= 2)
            emit_value_as_rep(out, v->args[1], XR_REP_I64);
        else
            fprintf(out, "1");
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
    } else if (is_try_wait) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
        fprintf(out, "xr_aot_countdown_latch_try_wait_bool_sync(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
    } else if (is_close) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        fprintf(out, "({ xr_aot_countdown_latch_close_void_sync(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, "); XR_NULL_VAL; })");
        emit_conversion_suffix(out, conv_suffix);
    }
    return true;
}

static bool xicgen_emit_semaphore_method(FILE *out, const XiValue *v, const char *method,
                                         uint16_t nargs) {
    if (!v || v->nargs < 1 || !method || !xi_value_type_is_semaphore(v->args[0]))
        return false;
    bool is_release = strcmp(method, "release") == 0 && (nargs == 0 || nargs == 1);
    bool is_try_acquire = strcmp(method, "tryAcquire") == 0 && nargs == 0;
    bool is_close = strcmp(method, "close") == 0 && nargs == 0;
    if (!is_release && !is_try_acquire && !is_close)
        return false;

    if (is_release) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
        fprintf(out, "xr_aot_semaphore_release_i64_sync(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        if (nargs == 1 && v->nargs >= 2)
            emit_value_as_rep(out, v->args[1], XR_REP_I64);
        else
            fprintf(out, "1");
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
    } else if (is_try_acquire) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
        fprintf(out, "xr_aot_semaphore_try_acquire_bool_sync(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
    } else if (is_close) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        fprintf(out, "({ xr_aot_semaphore_close_void_sync(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, "); XR_NULL_VAL; })");
        emit_conversion_suffix(out, conv_suffix);
    }
    return true;
}

static bool xicgen_emit_event_count_method(FILE *out, const XiValue *v, const char *method,
                                           uint16_t nargs) {
    if (!v || v->nargs < 1 || !method || !xi_value_type_is_event_count(v->args[0]))
        return false;
    bool is_advance = strcmp(method, "advance") == 0 && (nargs == 0 || nargs == 1);
    bool is_close = strcmp(method, "close") == 0 && nargs == 0;
    if (!is_advance && !is_close)
        return false;

    if (is_advance) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
        fprintf(out, "xr_aot_event_count_advance_i64_sync(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        if (nargs == 1 && v->nargs >= 2)
            emit_value_as_rep(out, v->args[1], XR_REP_I64);
        else
            fprintf(out, "1");
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
    } else if (is_close) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        fprintf(out, "({ xr_aot_event_count_close_void_sync(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, "); XR_NULL_VAL; })");
        emit_conversion_suffix(out, conv_suffix);
    }
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
    if (xicgen_emit_atomic_method(ctx, out, v, method, nargs))
        return;
    if (xicgen_emit_channel_method(out, v, method, nargs))
        return;
    if (xicgen_emit_work_queue_method(out, v, method, nargs))
        return;
    if (xicgen_emit_result_group_method(out, v, method, nargs))
        return;
    if (xicgen_emit_countdown_latch_method(out, v, method, nargs))
        return;
    if (xicgen_emit_semaphore_method(out, v, method, nargs))
        return;
    if (xicgen_emit_event_count_method(out, v, method, nargs))
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
    /* string.toBytes(): the VM dispatches this by name (no stable method-symbol
     * id), so lower it directly to the runtime helper. Mirrors VM m_to_bytes. */
    if (sym < 0 && method && strcmp(method, "toBytes") == 0 && nargs == 0 && v->nargs >= 1) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        fprintf(out, "xrt_str_to_bytes(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return;
    }
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
    } else if (nargs == 4) {
        fprintf(out, "xrt_method_4(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", %d, ", sym);
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, v->args[3], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, v->args[4], XR_REP_TAGGED);
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
        /* Match the constructor's actual parameter ABI (see note above). */
        emit_value_as_rep(out, v->args[a], cg_func_param_abi_rep(ctx, ctor, a));
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
            /* Match the constructor's actual parameter ABI (see note above). */
            emit_value_as_rep(out, v->args[a], cg_func_param_abi_rep(ctx, target, a));
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
    emit_direct_call_arg_list(ctx, out, f, v, target);
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
    if (xicgen_emit_enum_method(ctx, out, f, v, method))
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
    if (cg_value_plan_is_aggregate(ctx, cg_unwrap_identity_value(arg))) {
        fprintf(out, "((void)0)");
        return;
    }
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
    XR_DCHECK(v->nargs >= 1, "xicgen_struct_new: need class arg");
    if (cg_value_plan_is_struct_aggregate(ctx, v)) {
        emit_value_plan_zero_expr(ctx, out, v);
    } else if (cg_struct_can_inline(f, v)) {
        /* Inlined struct initialization is emitted by the statement path. */
        fprintf(out, "XR_NULL_VAL");
    } else {
        emit_struct_fallback_new_expr(out, (XrStructLayout *) v->aux, prefix);
    }
}

static void xicgen_struct_get(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    XR_DCHECK(v->nargs >= 1, "xicgen_struct_get: need struct arg");
    if (cg_value_plan_is_struct_aggregate(ctx, v->args[0])) {
        char fname[128];
        XrStructLayout *sl = (XrStructLayout *) v->aux;
        cg_struct_field_c_name(sl, v->aux_int, fname, sizeof(fname));
        emit_vref(out, v->args[0]);
        fprintf(out, ".%s", fname);
        return;
    }
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
    if (cg_value_plan_is_struct_aggregate(ctx, v->args[0])) {
        char fname[128];
        XrStructLayout *sl = (XrStructLayout *) v->aux;
        cg_struct_field_c_name(sl, v->aux_int, fname, sizeof(fname));
        fprintf(out, "(");
        emit_vref(out, v->args[0]);
        fprintf(out, ".%s = ", fname);
        emit_struct_field_store_value(out, sl, v->aux_int, v->args[1]);
        fprintf(out, ")");
        return;
    }
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
    (void) prefix;
    emit_shift_binop_ctx(ctx, out, f, v, xi_to_c_template_shift_fn(v->op));
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
    if (xicgen_compare_uses_rawptr(v)) {
        fprintf(out, "(");
        xicgen_emit_uintptr_compare_arg(ctx, out, v->args[0]);
        fprintf(out, " %s ", xi_to_c_template_compare_native_op(v->op));
        xicgen_emit_uintptr_compare_arg(ctx, out, v->args[1]);
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
    /* Bare ADT-enum variant access (e.g. `Recv.Empty`) builds a tagged instance,
     * not the enum singleton. ADT-enum values are always tagged instances
     * (field[0] = ordinal), so a bare non-payload variant must match that
     * representation; otherwise the array-based pattern-match tag read would
     * dereference an enum singleton and crash. */
    if (field) {
        const XiEnumData *adt = xicgen_adt_enum_for_type(ctx, v->type);
        if (adt && adt->members) {
            int midx = cg_enum_member_index(adt, field);
            if (midx >= 0 && (uint32_t) midx < adt->member_count &&
                adt->members[midx].payload_count == 0) {
                emit_adt_enum_construct_expr(ctx, out, adt, midx, v);
                return;
            }
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
        if (cg_value_is_module_import_ctx(ctx, f, v->args[0], "os")) {
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
        /* A field access on a namespace module import (`import mod; mod.X`)
         * resolves to the target module's export shared slot. This lets pure-Xray
         * module const/let exports (path.sep, ...) read xrt_shared_<mod>[slot]
         * instead of a dynamic getprop on the module import object, which is null
         * at AOT. Named imports (`import { X } from mod`) already resolve this way
         * through the import ref (see xicgen_import_ref). */
        {
            const XiImportRef *mod_ref = cg_module_import_ref_for_value(ctx, f, v->args[0]);
            if (mod_ref && mod_ref->resolved_mod_index >= 0 &&
                mod_ref->resolved_mod_index < ctx->all_nmodules &&
                ctx->all_modules[mod_ref->resolved_mod_index]) {
                const XiModule *tmod = ctx->all_modules[mod_ref->resolved_mod_index];
                for (uint16_t ei = 0; ei < tmod->nexports; ei++) {
                    if (tmod->exports[ei].name && strcmp(tmod->exports[ei].name, field) == 0) {
                        const char *tname = tmod->name ? tmod->name : "mod";
                        const char *conv_suffix = emit_conversion_prefix(
                            out, v->type, XR_REP_TAGGED, cg_value_plan_storage_rep(ctx, v));
                        fprintf(out, "xrt_shared_%s[%d]", tname,
                                (int) tmod->exports[ei].shared_slot);
                        emit_conversion_suffix(out, conv_suffix);
                        return;
                    }
                }
            }
        }
    }
    if (!field && v->aux_int >= 0) {
        if (xicgen_emit_adt_field_load(ctx, out, v))
            return;
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
    const char *countdown_latch_helper = xi_value_type_is_countdown_latch(v->args[0])
                                             ? cg_countdown_latch_field_helper(field)
                                             : NULL;
    if (countdown_latch_helper) {
        if (cg_rep(v) == XR_REP_I64)
            fprintf(out, "XR_TO_INT(");
        else if (cg_rep(v) == XR_REP_F64)
            fprintf(out, "XR_TO_FLOAT(");
        fprintf(out, "%s(NULL, ", countdown_latch_helper);
        emit_vref(out, v->args[0]);
        fprintf(out, ")");
        if (cg_rep(v) == XR_REP_I64 || cg_rep(v) == XR_REP_F64)
            fprintf(out, ")");
        return;
    }
    const char *semaphore_helper =
        xi_value_type_is_semaphore(v->args[0]) ? cg_semaphore_field_helper(field) : NULL;
    if (semaphore_helper) {
        if (cg_rep(v) == XR_REP_I64)
            fprintf(out, "XR_TO_INT(");
        else if (cg_rep(v) == XR_REP_F64)
            fprintf(out, "XR_TO_FLOAT(");
        fprintf(out, "%s(NULL, ", semaphore_helper);
        emit_vref(out, v->args[0]);
        fprintf(out, ")");
        if (cg_rep(v) == XR_REP_I64 || cg_rep(v) == XR_REP_F64)
            fprintf(out, ")");
        return;
    }
    const char *event_count_helper =
        xi_value_type_is_event_count(v->args[0]) ? cg_event_count_field_helper(field) : NULL;
    if (event_count_helper) {
        if (cg_rep(v) == XR_REP_I64)
            fprintf(out, "XR_TO_INT(");
        else if (cg_rep(v) == XR_REP_F64)
            fprintf(out, "XR_TO_FLOAT(");
        fprintf(out, "%s(NULL, ", event_count_helper);
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
        } else if (src_rep == XR_REP_RAWPTR) {
            fprintf(out, "(int64_t)(uintptr_t)");
            emit_vref(out, v->args[0]);
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

static void xicgen_bytes_load_u16_le(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                     const char *prefix) {
    CgArrayElemInfo info;
    if (v && v->nargs == 2 &&
        cg_array_value_storage_info(ctx, f, v->args[0], &info, CG_ARRAY_STORAGE_READ) &&
        cg_array_elem_info_is_u8(&info)) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
        fprintf(out, "%s(",
                (v->aux_int & 1) ? "xrt_bytes_load_u16_le_unchecked_raw"
                                 : "xrt_bytes_load_u16_le_checked_raw");
        emit_typed_array_ptr_expr(ctx, out, f, v->args[0], prefix);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[1], XR_REP_I64);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return;
    }
    const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
    fprintf(out, "XR_TO_INT(xrt_bytes_load_u16_le(");
    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
    fprintf(out, "))");
    emit_conversion_suffix(out, conv_suffix);
}

static void xicgen_bytes_load_u32_le(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                     const char *prefix) {
    CgArrayElemInfo info;
    if (v && v->nargs == 2 &&
        cg_array_value_storage_info(ctx, f, v->args[0], &info, CG_ARRAY_STORAGE_READ) &&
        cg_array_elem_info_is_u8(&info)) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
        fprintf(out, "%s(",
                (v->aux_int & 1) ? "xrt_bytes_load_u32_le_unchecked_raw"
                                 : "xrt_bytes_load_u32_le_checked_raw");
        emit_typed_array_ptr_expr(ctx, out, f, v->args[0], prefix);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[1], XR_REP_I64);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return;
    }
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
    CgArrayElemInfo info;
    if (v && v->nargs == 2 &&
        cg_array_value_storage_info(ctx, f, v->args[0], &info, CG_ARRAY_STORAGE_READ) &&
        cg_array_elem_info_is_u8(&info)) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
        fprintf(out, "%s(",
                (v->aux_int & 1) ? "xrt_bytes_load_u64_le_unchecked_raw"
                                 : "xrt_bytes_load_u64_le_checked_raw");
        emit_typed_array_ptr_expr(ctx, out, f, v->args[0], prefix);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[1], XR_REP_I64);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return;
    }
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
    code = xr_ffi_ptr_aux_type(code);
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
    code = xr_ffi_ptr_aux_type(code);
    return (XrFFIType) code == XR_FFI_T_F32 || (XrFFIType) code == XR_FFI_T_F64;
}

static bool cg_ffi_code_is_ptr(uint8_t code) {
    code = xr_ffi_ptr_aux_type(code);
    return (XrFFIType) code == XR_FFI_T_PTR;
}

static const char *cg_ffi_ptr_le_load_helper(uint8_t code) {
    code = xr_ffi_ptr_aux_type(code);
    switch ((XrFFIType) code) {
        case XR_FFI_T_U16:
            return "xrt_ptr_load_u16_le_unchecked_raw";
        case XR_FFI_T_U32:
            return "xrt_ptr_load_u32_le_unchecked_raw";
        case XR_FFI_T_U64:
            return "xrt_ptr_load_u64_le_unchecked_raw";
        default:
            return NULL;
    }
}

/* Unsafe Array<T>/Span<T> data pointer borrow. VM/tagged values keep the address
 * as an integer; AOT hot code carries RawPtr/RawMut as a non-owning C pointer. */
static void xicgen_array_data_ptr(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                  const char *prefix) {
    if (!v || v->nargs < 1) {
        emit_codegen_abort_expr(out);
        return;
    }
    const char *conv_suffix =
        emit_conversion_prefix(out, v->type, XR_REP_RAWPTR, cg_value_plan_storage_rep(ctx, v));
    fprintf(out, "(void *)(");
    emit_typed_array_ptr_expr(ctx, out, f, v->args[0], prefix);
    fprintf(out, "->data)");
    emit_conversion_suffix(out, conv_suffix);
}

/* R[dst] = *(T*)addr — inline typed load from a raw address. */
static void xicgen_ptr_load(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                            const char *prefix) {
    (void) f;
    (void) prefix;
    uint8_t aux = (uint8_t) (v->aux_int & 0xff);
    uint8_t code = xr_ffi_ptr_aux_type(aux);
    if ((aux & XR_FFI_PTR_AUX_LITTLE_ENDIAN) != 0) {
        const char *helper = cg_ffi_ptr_le_load_helper(code);
        if (!helper) {
            emit_codegen_abort_expr(out);
            return;
        }
        const char *conv_suffix =
            emit_conversion_prefix(out, v->type, XR_REP_I64, cg_value_plan_storage_rep(ctx, v));
        fprintf(out, "%s(", helper);
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_RAWPTR);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return;
    }
    const char *cty = cg_ffi_pointee_c_type(code);
    XrRep from_rep = cg_ffi_code_is_float(code)
                         ? XR_REP_F64
                         : (cg_ffi_code_is_ptr(code) ? XR_REP_RAWPTR : XR_REP_I64);
    const char *conv_suffix =
        emit_conversion_prefix(out, v->type, from_rep, cg_value_plan_storage_rep(ctx, v));
    if (cg_ffi_code_is_float(code))
        fprintf(out, "(double)");
    else if (cg_ffi_code_is_ptr(code))
        fprintf(out, "(void *)");
    else
        fprintf(out, "(int64_t)");
    fprintf(out, "(*(%s *)(", cty);
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_RAWPTR);
    fprintf(out, "))");
    emit_conversion_suffix(out, conv_suffix);
}

/* *(T*)addr = value — inline typed store to a raw address (void statement). */
static void xicgen_ptr_store(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                             const char *prefix) {
    (void) f;
    (void) prefix;
    uint8_t code = xr_ffi_ptr_aux_type((uint8_t) (v->aux_int & 0xff));
    const char *cty = cg_ffi_pointee_c_type(code);
    fprintf(out, "(*(%s *)(", cty);
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_RAWPTR);
    fprintf(out, ")) = ");
    if (cg_ffi_code_is_float(code)) {
        fprintf(out, "(%s)(", cty);
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_F64);
        fprintf(out, ")");
    } else if (cg_ffi_code_is_ptr(code)) {
        fprintf(out, "(void *)(");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_RAWPTR);
        fprintf(out, ")");
    } else {
        fprintf(out, "(%s)(", cty);
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
        fprintf(out, ")");
    }
}

/* memcpy(dst, src, byte_count) for RawMut<T>.copyFromNonOverlappingUnchecked. */
static void xicgen_ptr_copy_nonoverlap(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                       const char *prefix) {
    (void) f;
    (void) prefix;
    if (!v || v->nargs < 3) {
        emit_codegen_abort_expr(out);
        return;
    }
    fprintf(out, "memcpy(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_RAWPTR);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_RAWPTR);
    int64_t byte_count = 0;
    if (cg_const_int_value(v->args[2], &byte_count) && byte_count >= 0) {
        fprintf(out, ", (size_t)INT64_C(%" PRId64 "))", byte_count);
    } else {
        fprintf(out, ", (size_t)(");
        emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_I64);
        fprintf(out, "))");
    }
}

static void xicgen_gen_call(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                            const char *prefix);

static void xicgen_emit_par_for_item_arg(XiCgenCtx *ctx, FILE *out, const XiFunc *body,
                                         const char *iter_name) {
    const XrType *param_type = (body && body->nparams > 0 && body->params && body->params[0])
                                   ? body->params[0]->type
                                   : NULL;
    XrRep param_rep = cg_func_param_abi_rep(ctx, body, 0);
    const char *suffix = emit_conversion_prefix(out, param_type, XR_REP_I64, param_rep);
    fprintf(out, "%s", iter_name);
    emit_conversion_suffix(out, suffix);
}

static void xicgen_emit_par_for_body_call(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                          const XiFunc *body, const XiValue *closure,
                                          const char *prefix, const char *iter_name,
                                          const char *worker_name,
                                          const char *scoped_closure_name) {
    fprintf(out, "            ");
    emit_fname(ctx, out, prefix, body);
    fprintf(out, "(");
    if (scoped_closure_name)
        fprintf(out, "%s", scoped_closure_name);
    else
        emit_call_hidden_closure(out, f, body, closure);
    fprintf(out, ", ");
    xicgen_emit_par_for_item_arg(ctx, out, body, iter_name);
    fprintf(out, ", ");
    const XrType *worker_type = (body && body->nparams > 1 && body->params && body->params[1])
                                    ? body->params[1]->type
                                    : NULL;
    XrRep worker_rep = cg_func_param_abi_rep(ctx, body, 1);
    const char *worker_suffix = emit_conversion_prefix(out, worker_type, XR_REP_I64, worker_rep);
    fprintf(out, "%s", worker_name);
    emit_conversion_suffix(out, worker_suffix);
    fprintf(out, ");\n");
}

static void xicgen_emit_par_range_i64_arg(XiCgenCtx *ctx, FILE *out, const XiFunc *body,
                                          uint16_t param_index, const char *expr) {
    const XrType *type =
        (body && body->nparams > param_index && body->params && body->params[param_index])
            ? body->params[param_index]->type
            : NULL;
    XrRep rep = cg_func_param_abi_rep(ctx, body, param_index);
    const char *suffix = emit_conversion_prefix(out, type, XR_REP_I64, rep);
    fprintf(out, "%s", expr ? expr : "0");
    emit_conversion_suffix(out, suffix);
}

static void xicgen_emit_par_range_body_call(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                            const XiFunc *body, const XiValue *closure,
                                            const char *prefix, const char *begin_name,
                                            const char *end_name, const char *worker_name,
                                            const char *scoped_closure_name) {
    fprintf(out, "    ");
    emit_fname(ctx, out, prefix, body);
    fprintf(out, "(");
    if (scoped_closure_name)
        fprintf(out, "%s", scoped_closure_name);
    else
        emit_call_hidden_closure(out, f, body, closure);
    fprintf(out, ", ");
    xicgen_emit_par_range_i64_arg(ctx, out, body, 0, begin_name);
    fprintf(out, ", ");
    xicgen_emit_par_range_i64_arg(ctx, out, body, 1, end_name);
    fprintf(out, ", ");
    xicgen_emit_par_range_i64_arg(ctx, out, body, 2, worker_name);
    fprintf(out, ");\n");
}

static void xicgen_emit_par_for_range_wrapper_name(XiCgenCtx *ctx, FILE *out, const XiFunc *owner,
                                                   const XiValue *par_for, const char *prefix) {
    emit_fname(ctx, out, prefix, owner);
    fprintf(out, "_par_range_%u", par_for ? par_for->id : 0);
}

static bool xicgen_par_for_value_is_range_wrappable(const XiValue *v) {
    if (!v || v->op != XI_PAR_FOR || v->nargs < 4 || v->aux_kind != XI_AUX_KIND_PAR_FOR || !v->aux)
        return false;
    const XiParallelForData *data = (const XiParallelForData *) v->aux;
    const XiFunc *body = data ? data->body_func : NULL;
    return body && body->nparams == (data->range_body ? 3 : 2);
}

static void xicgen_emit_par_for_range_wrapper(XiCgenCtx *ctx, FILE *out, const XiFunc *owner,
                                              const XiValue *v, const char *prefix) {
    if (!xicgen_par_for_value_is_range_wrappable(v))
        return;
    const XiParallelForData *data = (const XiParallelForData *) v->aux;
    const XiFunc *body = data->body_func;
    fprintf(out, "static void ");
    xicgen_emit_par_for_range_wrapper_name(ctx, out, owner, v, prefix);
    fprintf(out,
            "(xrt_closure_t *_cl, int64_t _xr_begin, int64_t _xr_end, int64_t _xr_worker) {\n");
    if (data->range_body) {
        xicgen_emit_par_range_body_call(ctx, out, owner, body, NULL, prefix, "_xr_begin", "_xr_end",
                                        "_xr_worker", "_cl");
    } else {
        fprintf(out, "    for (int64_t _xr_i = _xr_begin; _xr_i < _xr_end; _xr_i++) {\n");
        xicgen_emit_par_for_body_call(ctx, out, owner, body, NULL, prefix, "_xr_i", "_xr_worker",
                                      "_cl");
        fprintf(out, "    }\n");
    }
    fprintf(out, "}\n\n");
}

static void xicgen_emit_par_for_range_wrappers(XiCgenCtx *ctx, FILE *out, const XiFunc *owner,
                                               const char *prefix) {
    if (!ctx || !out || !owner)
        return;
    for (uint32_t bi = 0; bi < owner->nblocks; bi++) {
        const XiBlock *blk = owner->blocks ? owner->blocks[bi] : NULL;
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values ? blk->values[vi] : NULL;
            if (xicgen_par_for_value_is_range_wrappable(v))
                xicgen_emit_par_for_range_wrapper(ctx, out, owner, v, prefix);
        }
    }
}

static void xicgen_emit_par_collect_range_wrapper_name(XiCgenCtx *ctx, FILE *out,
                                                       const XiFunc *owner,
                                                       const XiValue *par_collect,
                                                       const char *prefix) {
    emit_fname(ctx, out, prefix, owner);
    fprintf(out, "_par_collect_range_%u", par_collect ? par_collect->id : 0);
}

static bool xicgen_par_collect_value_is_range_wrappable(const XiValue *v) {
    if (!v || v->op != XI_PAR_COLLECT || v->nargs < 4 || v->aux_kind != XI_AUX_KIND_PAR_COLLECT ||
        !v->aux)
        return false;
    const XiParallelCollectData *data = (const XiParallelCollectData *) v->aux;
    const XiFunc *body = data ? data->body_func : NULL;
    return body && (body->nparams == 2 || (data->direct_lane_writes && body->nparams == 3));
}

static void xicgen_emit_par_reduce_body_call_value(XiCgenCtx *ctx, FILE *out, const XiFunc *owner,
                                                   const XiFunc *body, const XiValue *closure,
                                                   const char *prefix, const char *iter_name,
                                                   const char *worker_name,
                                                   const char *closure_name);

static void xicgen_emit_par_collect_body_call_as_rep(XiCgenCtx *ctx, FILE *out, const XiFunc *owner,
                                                     const XiFunc *body, const XiValue *closure,
                                                     const char *prefix, const char *iter_name,
                                                     const char *worker_name,
                                                     const char *closure_name, XrRep target_rep) {
    XrRep body_rep = cg_func_return_abi_rep(ctx, body);
    const char *suffix =
        emit_conversion_prefix(out, body ? body->return_type : NULL, body_rep, target_rep);
    xicgen_emit_par_reduce_body_call_value(ctx, out, owner, body, closure, prefix, iter_name,
                                           worker_name, closure_name);
    emit_conversion_suffix(out, suffix);
}

static bool xicgen_par_collect_body_has_native_result(XiCgenCtx *ctx, const XiValue *v,
                                                      const CgArrayElemInfo *info) {
    if (!ctx || !v || v->op != XI_PAR_COLLECT || v->aux_kind != XI_AUX_KIND_PAR_COLLECT ||
        !v->aux || !info || info->rep == XR_REP_TAGGED)
        return false;
    const XiParallelCollectData *data = (const XiParallelCollectData *) v->aux;
    const XiFunc *body = data ? data->body_func : NULL;
    return data && data->element_type && body &&
           body->native_callback_kind == XI_NATIVE_CALLBACK_PAR_COLLECT_SCALAR_BODY &&
           cg_func_return_abi_rep(ctx, body) == info->rep &&
           cg_func_param_abi_rep(ctx, body, 0) == XR_REP_I64 &&
           cg_func_param_abi_rep(ctx, body, 1) == XR_REP_I64;
}

static bool xicgen_par_collect_array_elem_info(XiCgenCtx *ctx, const XiValue *v,
                                               CgArrayElemInfo *out) {
    if (!ctx || !v || !out || v->op != XI_PAR_COLLECT || v->aux_kind != XI_AUX_KIND_PAR_COLLECT ||
        !v->aux)
        return false;
    const XiParallelCollectData *data = (const XiParallelCollectData *) v->aux;
    if (data->into_result && v->nargs >= 5 &&
        cg_array_elem_info_from_type_ctx(ctx, v->args[4] ? v->args[4]->type : NULL, out))
        return true;
    return cg_array_elem_info_from_type_ctx(ctx, v->type, out);
}

static void xicgen_emit_par_collect_zero_value(FILE *out, const CgArrayElemInfo *info) {
    const char *elem = (info && info->elem_name) ? info->elem_name : "XR_ELEM_ANY";
    if (strcmp(elem, "XR_ELEM_CHAR") == 0)
        fprintf(out, "XR_FROM_CHAR(0)");
    else if (strcmp(elem, "XR_ELEM_F32") == 0 || strcmp(elem, "XR_ELEM_F64") == 0)
        fprintf(out, "XR_FROM_FLOAT(0.0)");
    else if (strcmp(elem, "XR_ELEM_BOOL") == 0)
        fprintf(out, "XR_FALSE_VAL");
    else if (strcmp(elem, "XR_ELEM_ANY") == 0)
        fprintf(out, "XR_NULL_VAL");
    else
        fprintf(out, "XR_FROM_INT(0)");
}

static void xicgen_emit_par_collect_range_wrapper(XiCgenCtx *ctx, FILE *out, const XiFunc *owner,
                                                  const XiValue *v, const char *prefix) {
    if (!xicgen_par_collect_value_is_range_wrappable(v))
        return;
    const XiParallelCollectData *data = (const XiParallelCollectData *) v->aux;
    const XiFunc *body = data->body_func;
    CgArrayElemInfo info;
    bool have_info = xicgen_par_collect_array_elem_info(ctx, v, &info);
    bool native_result = have_info && xicgen_par_collect_body_has_native_result(ctx, v, &info);

    fprintf(out, "static void ");
    xicgen_emit_par_collect_range_wrapper_name(ctx, out, owner, v, prefix);
    fprintf(out,
            "(xrt_closure_t *_cl, int64_t _xr_begin, int64_t _xr_end, int64_t _xr_worker) {\n");
    if (data->direct_lane_writes && body && body->nparams == 3) {
        xicgen_emit_par_range_body_call(ctx, out, owner, body, NULL, prefix, "_xr_begin", "_xr_end",
                                        "_xr_worker", "_cl");
        fprintf(out, ";\n");
        fprintf(out, "}\n\n");
        return;
    }
    if (!data->direct_lane_writes) {
        fprintf(out, "    xrt_array_t *_xr_out = (xrt_array_t *)_cl->upvals[%u].ptr;\n",
                (unsigned) data->result_capture_index);
        fprintf(out, "    int64_t _xr_start = XR_TO_INT(_cl->upvals[%u]);\n",
                (unsigned) data->start_capture_index);
    }
    fprintf(out, "    for (int64_t _xr_i = _xr_begin; _xr_i < _xr_end; _xr_i++) {\n");
    if (data->direct_lane_writes) {
        xicgen_emit_par_for_body_call(ctx, out, owner, body, NULL, prefix, "_xr_i", "_xr_worker",
                                      "_cl");
    } else {
        fprintf(out, "        int64_t _xr_idx = _xr_i - _xr_start;\n");
        if (native_result) {
            fprintf(out, "        ((%s*)_xr_out->data)[_xr_idx] = (%s)", info.ctype, info.ctype);
            xicgen_emit_par_collect_body_call_as_rep(ctx, out, owner, body, NULL, prefix, "_xr_i",
                                                     "_xr_worker", "_cl", info.rep);
            fprintf(out, ";\n");
        } else {
            fprintf(out, "        XrValue _xr_item = ");
            xicgen_emit_par_collect_body_call_as_rep(ctx, out, owner, body, NULL, prefix, "_xr_i",
                                                     "_xr_worker", "_cl", XR_REP_TAGGED);
            fprintf(out, ";\n");
            fprintf(out, "        xrt_array_write_preallocated(_xr_out, _xr_idx, _xr_item);\n");
        }
    }
    fprintf(out, "    }\n");
    fprintf(out, "}\n\n");
}

static void xicgen_emit_par_collect_range_wrappers(XiCgenCtx *ctx, FILE *out, const XiFunc *owner,
                                                   const char *prefix) {
    if (!ctx || !out || !owner)
        return;
    for (uint32_t bi = 0; bi < owner->nblocks; bi++) {
        const XiBlock *blk = owner->blocks ? owner->blocks[bi] : NULL;
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values ? blk->values[vi] : NULL;
            if (xicgen_par_collect_value_is_range_wrappable(v))
                xicgen_emit_par_collect_range_wrapper(ctx, out, owner, v, prefix);
        }
    }
}

static void xicgen_emit_par_reduce_range_wrapper_name(XiCgenCtx *ctx, FILE *out,
                                                      const XiFunc *owner,
                                                      const XiValue *par_reduce,
                                                      const char *prefix) {
    emit_fname(ctx, out, prefix, owner);
    fprintf(out, "_par_reduce_range_%u", par_reduce ? par_reduce->id : 0);
}

static void xicgen_emit_par_reduce_combine_wrapper_name(XiCgenCtx *ctx, FILE *out,
                                                        const XiFunc *owner,
                                                        const XiValue *par_reduce,
                                                        const char *prefix) {
    emit_fname(ctx, out, prefix, owner);
    fprintf(out, "_par_reduce_combine_%u", par_reduce ? par_reduce->id : 0);
}

static bool xicgen_par_reduce_value_has_i64_accumulator(const XiValue *v) {
    if (!v || v->op != XI_PAR_REDUCE || v->nargs < 6 || v->aux_kind != XI_AUX_KIND_PAR_REDUCE ||
        !v->aux)
        return false;
    const XiParallelReduceData *data = (const XiParallelReduceData *) v->aux;
    return data && data->accumulator_type && XR_TYPE_IS_INT(data->accumulator_type);
}

static const char *xicgen_par_reduce_struct_agg_c_type(XiCgenCtx *ctx, const XiValue *v) {
    const XaotValuePlan *plan = cg_value_plan(ctx, v);
    if (!plan || !cg_value_rep_is_struct_aggregate(plan->rep))
        return NULL;
    return plan->rep.c_type;
}

static bool xicgen_par_reduce_value_has_struct_accumulator(XiCgenCtx *ctx, const XiValue *v) {
    if (!ctx || !v || v->op != XI_PAR_REDUCE || v->nargs < 6 ||
        v->aux_kind != XI_AUX_KIND_PAR_REDUCE || !v->aux)
        return false;
    const XiParallelReduceData *data = (const XiParallelReduceData *) v->aux;
    const XiFunc *body = data ? data->body_func : NULL;
    const XiFunc *combine = data ? data->combine_func : NULL;
    const char *ctype = xicgen_par_reduce_struct_agg_c_type(ctx, v);
    uint16_t expected_body_params = data && data->range_body ? 3 : 2;
    if (!ctype || !body || !combine || body->nparams != expected_body_params ||
        combine->nparams != 2)
        return false;
    if (!cg_func_return_abi_is_struct_aggregate(ctx, body) ||
        !cg_func_return_abi_is_struct_aggregate(ctx, combine))
        return false;
    if (cg_func_param_abi_rep(ctx, body, 0) != XR_REP_I64 ||
        cg_func_param_abi_rep(ctx, body, 1) != XR_REP_I64 ||
        (data->range_body && cg_func_param_abi_rep(ctx, body, 2) != XR_REP_I64))
        return false;
    return strcmp(cg_func_return_abi_c_type(ctx, body), ctype) == 0 &&
           strcmp(cg_func_return_abi_c_type(ctx, combine), ctype) == 0 &&
           strcmp(cg_func_param_abi_c_type(ctx, combine, 0), ctype) == 0 &&
           strcmp(cg_func_param_abi_c_type(ctx, combine, 1), ctype) == 0;
}

static bool xicgen_par_reduce_value_is_range_wrappable(XiCgenCtx *ctx, const XiValue *v) {
    if (!v || v->op != XI_PAR_REDUCE || v->nargs < 6 || v->aux_kind != XI_AUX_KIND_PAR_REDUCE ||
        !v->aux)
        return false;
    const XiParallelReduceData *data = (const XiParallelReduceData *) v->aux;
    const XiFunc *body = data ? data->body_func : NULL;
    const XiFunc *combine = data ? data->combine_func : NULL;
    uint16_t expected_body_params = data && data->range_body ? 3 : 2;
    return data && body && combine && body->nparams == expected_body_params &&
           combine->nparams == 2 &&
           (xicgen_par_reduce_value_has_i64_accumulator(v) ||
            xicgen_par_reduce_value_has_struct_accumulator(ctx, v));
}

static void xicgen_emit_par_reduce_body_call_value(XiCgenCtx *ctx, FILE *out, const XiFunc *owner,
                                                   const XiFunc *body, const XiValue *closure,
                                                   const char *prefix, const char *iter_name,
                                                   const char *worker_name,
                                                   const char *closure_name) {
    emit_fname(ctx, out, prefix, body);
    fprintf(out, "(");
    if (closure_name)
        fprintf(out, "%s", closure_name);
    else
        emit_call_hidden_closure(out, owner, body, closure);
    fprintf(out, ", ");
    xicgen_emit_par_for_item_arg(ctx, out, body, iter_name);
    fprintf(out, ", ");
    const XrType *worker_type = (body && body->nparams > 1 && body->params && body->params[1])
                                    ? body->params[1]->type
                                    : NULL;
    XrRep worker_rep = cg_func_param_abi_rep(ctx, body, 1);
    const char *worker_suffix = emit_conversion_prefix(out, worker_type, XR_REP_I64, worker_rep);
    fprintf(out, "%s", worker_name);
    emit_conversion_suffix(out, worker_suffix);
    fprintf(out, ")");
}

static void xicgen_emit_par_reduce_range_body_call_value(
    XiCgenCtx *ctx, FILE *out, const XiFunc *owner, const XiFunc *body, const XiValue *closure,
    const char *prefix, const char *begin_name, const char *end_name, const char *worker_name,
    const char *closure_name) {
    emit_fname(ctx, out, prefix, body);
    fprintf(out, "(");
    if (closure_name)
        fprintf(out, "%s", closure_name);
    else
        emit_call_hidden_closure(out, owner, body, closure);
    fprintf(out, ", ");
    xicgen_emit_par_for_item_arg(ctx, out, body, begin_name);
    fprintf(out, ", ");
    const XrType *end_type = (body && body->nparams > 1 && body->params && body->params[1])
                                 ? body->params[1]->type
                                 : NULL;
    XrRep end_rep = cg_func_param_abi_rep(ctx, body, 1);
    const char *end_suffix = emit_conversion_prefix(out, end_type, XR_REP_I64, end_rep);
    fprintf(out, "%s", end_name);
    emit_conversion_suffix(out, end_suffix);
    fprintf(out, ", ");
    const XrType *worker_type = (body && body->nparams > 2 && body->params && body->params[2])
                                    ? body->params[2]->type
                                    : NULL;
    XrRep worker_rep = cg_func_param_abi_rep(ctx, body, 2);
    const char *worker_suffix = emit_conversion_prefix(out, worker_type, XR_REP_I64, worker_rep);
    fprintf(out, "%s", worker_name);
    emit_conversion_suffix(out, worker_suffix);
    fprintf(out, ")");
}

static void xicgen_emit_par_reduce_combine_call_value(XiCgenCtx *ctx, FILE *out,
                                                      const XiFunc *combine, const char *prefix,
                                                      const char *acc_name,
                                                      const char *value_name) {
    emit_fname(ctx, out, prefix, combine);
    fprintf(out, "(NULL, ");
    const XrType *acc_type =
        (combine && combine->nparams > 0 && combine->params && combine->params[0])
            ? combine->params[0]->type
            : NULL;
    XrRep acc_rep = cg_func_param_abi_rep(ctx, combine, 0);
    const char *acc_suffix = emit_conversion_prefix(out, acc_type, XR_REP_I64, acc_rep);
    fprintf(out, "%s", acc_name);
    emit_conversion_suffix(out, acc_suffix);
    fprintf(out, ", ");
    const XrType *value_type =
        (combine && combine->nparams > 1 && combine->params && combine->params[1])
            ? combine->params[1]->type
            : NULL;
    XrRep value_rep = cg_func_param_abi_rep(ctx, combine, 1);
    const char *value_suffix = emit_conversion_prefix(out, value_type, XR_REP_I64, value_rep);
    fprintf(out, "%s", value_name);
    emit_conversion_suffix(out, value_suffix);
    fprintf(out, ")");
}

static void xicgen_emit_par_reduce_combine_call_aggregate(XiCgenCtx *ctx, FILE *out,
                                                          const XiFunc *combine, const char *prefix,
                                                          const char *acc_expr,
                                                          const char *value_expr) {
    emit_fname(ctx, out, prefix, combine);
    fprintf(out, "(NULL, %s, %s)", acc_expr, value_expr);
}

static void xicgen_emit_par_reduce_range_wrapper(XiCgenCtx *ctx, FILE *out, const XiFunc *owner,
                                                 const XiValue *v, const char *prefix) {
    if (!xicgen_par_reduce_value_is_range_wrappable(ctx, v))
        return;
    const XiParallelReduceData *data = (const XiParallelReduceData *) v->aux;
    const XiFunc *body = data->body_func;
    const XiFunc *combine = data->combine_func;

    const char *agg_ctype = xicgen_par_reduce_struct_agg_c_type(ctx, v);
    if (agg_ctype) {
        fprintf(out, "static bool ");
        xicgen_emit_par_reduce_range_wrapper_name(ctx, out, owner, v, prefix);
        fprintf(out, "(xrt_closure_t *_cl, int64_t _xr_begin, int64_t _xr_end, int64_t _xr_worker, "
                     "void *_xr_out_void) {\n");
        fprintf(out, "    %s *_xr_out = (%s *)_xr_out_void;\n", agg_ctype, agg_ctype);
        fprintf(out, "    %s _xr_acc = ", agg_ctype);
        if (data->range_body) {
            xicgen_emit_par_reduce_range_body_call_value(
                ctx, out, owner, body, NULL, prefix, "_xr_begin", "_xr_end", "_xr_worker", "_cl");
            fprintf(out, ";\n");
        } else {
            fprintf(out, "((%s){0});\n", agg_ctype);
            fprintf(out, "    bool _xr_has = false;\n");
            fprintf(out, "    for (int64_t _xr_i = _xr_begin; _xr_i < _xr_end; _xr_i++) {\n");
            fprintf(out, "        %s _xr_item = ", agg_ctype);
            xicgen_emit_par_reduce_body_call_value(ctx, out, owner, body, NULL, prefix, "_xr_i",
                                                   "_xr_worker", "_cl");
            fprintf(out, ";\n");
            fprintf(out, "        if (_xr_has) {\n");
            fprintf(out, "            _xr_acc = ");
            xicgen_emit_par_reduce_combine_call_aggregate(ctx, out, combine, prefix, "_xr_acc",
                                                          "_xr_item");
            fprintf(out, ";\n");
            fprintf(out, "        } else {\n");
            fprintf(out, "            _xr_acc = _xr_item;\n");
            fprintf(out, "            _xr_has = true;\n");
            fprintf(out, "        }\n");
            fprintf(out, "    }\n");
        }
        fprintf(out, "    if (_xr_out) *_xr_out = _xr_acc;\n");
        fprintf(out, "    return true;\n");
        fprintf(out, "}\n\n");

        fprintf(out, "static void ");
        xicgen_emit_par_reduce_combine_wrapper_name(ctx, out, owner, v, prefix);
        fprintf(out, "(xrt_closure_t *_cl, void *_xr_acc_void, const void *_xr_value_void) {\n");
        fprintf(out, "    (void)_cl;\n");
        fprintf(out, "    %s *_xr_acc = (%s *)_xr_acc_void;\n", agg_ctype, agg_ctype);
        fprintf(out, "    const %s *_xr_value = (const %s *)_xr_value_void;\n", agg_ctype,
                agg_ctype);
        fprintf(out, "    *_xr_acc = ");
        xicgen_emit_par_reduce_combine_call_aggregate(ctx, out, combine, prefix, "*_xr_acc",
                                                      "*_xr_value");
        fprintf(out, ";\n");
        fprintf(out, "}\n\n");
        return;
    }

    fprintf(out, "static bool ");
    xicgen_emit_par_reduce_range_wrapper_name(ctx, out, owner, v, prefix);
    fprintf(out, "(xrt_closure_t *_cl, int64_t _xr_begin, int64_t _xr_end, int64_t _xr_worker, "
                 "int64_t *_xr_out) {\n");
    fprintf(out, "    int64_t _xr_acc = ");
    if (data->range_body) {
        xicgen_emit_par_reduce_range_body_call_value(ctx, out, owner, body, NULL, prefix,
                                                     "_xr_begin", "_xr_end", "_xr_worker", "_cl");
        fprintf(out, ";\n");
    } else {
        fprintf(out, "0;\n");
        fprintf(out, "    bool _xr_has = false;\n");
        fprintf(out, "    for (int64_t _xr_i = _xr_begin; _xr_i < _xr_end; _xr_i++) {\n");
        fprintf(out, "        int64_t _xr_item = ");
        xicgen_emit_par_reduce_body_call_value(ctx, out, owner, body, NULL, prefix, "_xr_i",
                                               "_xr_worker", "_cl");
        fprintf(out, ";\n");
        fprintf(out, "        if (_xr_has) {\n");
        fprintf(out, "            _xr_acc = ");
        xicgen_emit_par_reduce_combine_call_value(ctx, out, combine, prefix, "_xr_acc", "_xr_item");
        fprintf(out, ";\n");
        fprintf(out, "        } else {\n");
        fprintf(out, "            _xr_acc = _xr_item;\n");
        fprintf(out, "            _xr_has = true;\n");
        fprintf(out, "        }\n");
        fprintf(out, "    }\n");
    }
    fprintf(out, "    if (_xr_out) *_xr_out = _xr_acc;\n");
    fprintf(out, "    return true;\n");
    fprintf(out, "}\n\n");

    fprintf(out, "static int64_t ");
    xicgen_emit_par_reduce_combine_wrapper_name(ctx, out, owner, v, prefix);
    fprintf(out, "(xrt_closure_t *_cl, int64_t _xr_acc, int64_t _xr_value) {\n");
    fprintf(out, "    (void)_cl;\n");
    fprintf(out, "    return ");
    xicgen_emit_par_reduce_combine_call_value(ctx, out, combine, prefix, "_xr_acc", "_xr_value");
    fprintf(out, ";\n");
    fprintf(out, "}\n\n");
}

static void xicgen_emit_par_reduce_range_wrappers(XiCgenCtx *ctx, FILE *out, const XiFunc *owner,
                                                  const char *prefix) {
    if (!ctx || !out || !owner)
        return;
    for (uint32_t bi = 0; bi < owner->nblocks; bi++) {
        const XiBlock *blk = owner->blocks ? owner->blocks[bi] : NULL;
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values ? blk->values[vi] : NULL;
            if (xicgen_par_reduce_value_is_range_wrappable(ctx, v))
                xicgen_emit_par_reduce_range_wrapper(ctx, out, owner, v, prefix);
        }
    }
}

static bool xicgen_par_for_uses_stack_callback_closure(const XiValue *par_for,
                                                       const XiValue *closure, const XiFunc *body) {
    if (!par_for || !closure || !body || par_for->op != XI_PAR_FOR || par_for->nargs < 4)
        return false;
    if (par_for->args[3] != closure)
        return false;
    return closure->op == XI_STACK_ALLOC && closure->aux_int == XI_CLOSURE_NEW &&
           closure->aux == body;
}

static bool xicgen_par_collect_uses_stack_body_closure(const XiValue *par_collect,
                                                       const XiValue *closure, const XiFunc *body) {
    if (!par_collect || !closure || !body || par_collect->op != XI_PAR_COLLECT ||
        par_collect->nargs < 4)
        return false;
    if (par_collect->args[3] != closure)
        return false;
    return closure->op == XI_STACK_ALLOC && closure->aux_int == XI_CLOSURE_NEW &&
           closure->aux == body;
}

static bool xicgen_par_reduce_uses_stack_body_closure(const XiValue *par_reduce,
                                                      const XiValue *closure, const XiFunc *body) {
    if (!par_reduce || !closure || !body || par_reduce->op != XI_PAR_REDUCE ||
        par_reduce->nargs < 6)
        return false;
    if (par_reduce->args[4] != closure)
        return false;
    return closure->op == XI_STACK_ALLOC && closure->aux_int == XI_CLOSURE_NEW &&
           closure->aux == body;
}

static bool xicgen_par_reduce_uses_stack_combine_closure(const XiValue *par_reduce,
                                                         const XiValue *closure,
                                                         const XiFunc *combine) {
    if (!par_reduce || !closure || !combine || par_reduce->op != XI_PAR_REDUCE ||
        par_reduce->nargs < 6)
        return false;
    if (par_reduce->args[5] != closure)
        return false;
    return closure->op == XI_STACK_ALLOC && closure->aux_int == XI_CLOSURE_NEW &&
           closure->aux == combine;
}

static bool xicgen_par_for_stack_closure_value_is_elided(XiCgenCtx *ctx, const XiFunc *f,
                                                         const XiValue *target) {
    (void) ctx;
    bool saw_use = false;
    if (!f || !target || target->op != XI_STACK_ALLOC || target->aux_int != XI_CLOSURE_NEW ||
        !target->aux)
        return false;
    const XiFunc *body = (const XiFunc *) target->aux;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks ? f->blocks[bi] : NULL;
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values ? blk->values[vi] : NULL;
            if (!user || user == target)
                continue;
            for (uint16_t ai = 0; ai < user->nargs; ai++) {
                if (user->args[ai] != target)
                    continue;
                if (!((ai == 3 && xicgen_par_for_uses_stack_callback_closure(user, target, body)) ||
                      (ai == 3 && xicgen_par_collect_uses_stack_body_closure(user, target, body)) ||
                      (ai == 4 && xicgen_par_reduce_uses_stack_body_closure(user, target, body)) ||
                      (ai == 5 &&
                       xicgen_par_reduce_uses_stack_combine_closure(user, target, body))))
                    return false;
                saw_use = true;
            }
        }
    }
    return saw_use;
}

static void xicgen_emit_par_for_scoped_closure(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                               const XiValue *par_for, const XiFunc *body,
                                               const XiValue *closure, const char *prefix) {
    uint16_t ncap = body ? body->ncaptures : 0;
    fprintf(out,
            "        union { XrObjHeader hdr; long double align; unsigned char "
            "bytes[sizeof(XrObjHeader) + sizeof(xrt_closure_t) + %u * sizeof(XrValue)]; } "
            "_xr_par_closure_storage_%u;\n",
            ncap, par_for->id);
    fprintf(out,
            "        memset(&_xr_par_closure_storage_%u, 0, sizeof(_xr_par_closure_storage_%u));\n",
            par_for->id, par_for->id);
    fprintf(out,
            "        XrObjHeader *_xr_par_closure_hdr_%u = (XrObjHeader "
            "*)_xr_par_closure_storage_%u.bytes;\n",
            par_for->id, par_for->id);
    fprintf(out, "        _xr_par_closure_hdr_%u->extra = XR_OBJ_STORAGE_STACK;\n", par_for->id);
    fprintf(out,
            "        xrt_closure_t *_xr_par_closure_%u = (xrt_closure_t *)((char "
            "*)_xr_par_closure_hdr_%u + sizeof(XrObjHeader));\n",
            par_for->id, par_for->id);
    fprintf(out, "        xrt_closure_init(_xr_par_closure_%u, (void*)", par_for->id);
    emit_closure_entry_pointer(ctx, out, prefix, body);
    fprintf(out, ", %u);\n", ncap);
    fprintf(out, "        { xrt_closure_t *_c = _xr_par_closure_%u; ", par_for->id);
    emit_closure_upval_initializers(ctx, out, f, closure, false);
    fprintf(out, "}\n");
}

static void xicgen_emit_par_collect_scoped_closure(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                   const XiValue *par_collect, const XiFunc *body,
                                                   const XiValue *closure, const char *prefix,
                                                   const char *result_value_name,
                                                   const char *start_name) {
    const XiParallelCollectData *data =
        (const XiParallelCollectData *) (par_collect ? par_collect->aux : NULL);
    uint16_t ncap = body ? body->ncaptures : 0;
    uint16_t total = (uint16_t) (ncap + 2u);
    fprintf(out,
            "        union { XrObjHeader hdr; long double align; unsigned char "
            "bytes[sizeof(XrObjHeader) + sizeof(xrt_closure_t) + %u * sizeof(XrValue)]; } "
            "_xr_pc_closure_storage_%u;\n",
            total, par_collect->id);
    fprintf(out,
            "        memset(&_xr_pc_closure_storage_%u, 0, sizeof(_xr_pc_closure_storage_%u));\n",
            par_collect->id, par_collect->id);
    fprintf(out,
            "        XrObjHeader *_xr_pc_closure_hdr_%u = (XrObjHeader "
            "*)_xr_pc_closure_storage_%u.bytes;\n",
            par_collect->id, par_collect->id);
    fprintf(out, "        _xr_pc_closure_hdr_%u->extra = XR_OBJ_STORAGE_STACK;\n", par_collect->id);
    fprintf(out,
            "        xrt_closure_t *_xr_pc_closure_%u = (xrt_closure_t *)((char "
            "*)_xr_pc_closure_hdr_%u + sizeof(XrObjHeader));\n",
            par_collect->id, par_collect->id);
    fprintf(out, "        xrt_closure_init(_xr_pc_closure_%u, (void*)", par_collect->id);
    emit_closure_entry_pointer(ctx, out, prefix, body);
    fprintf(out, ", %u);\n", total);
    fprintf(out, "        { xrt_closure_t *_c = _xr_pc_closure_%u; ", par_collect->id);
    emit_closure_upval_initializers(ctx, out, f, closure, false);
    fprintf(out, "_c->upvals[%u] = %s; _c->upvals[%u] = XR_FROM_INT(%s); }\n",
            data ? (unsigned) data->result_capture_index : (unsigned) ncap,
            result_value_name ? result_value_name : "XR_NULL_VAL",
            data ? (unsigned) data->start_capture_index : (unsigned) (ncap + 1u),
            start_name ? start_name : "0");
}

enum {
    XICGEN_PAR_FOR_BODY_DEPTH_MAX = 32
};

static bool xicgen_par_for_stack_contains(const XiFunc *const *stack, int depth,
                                          const XiFunc *func) {
    for (int i = 0; i < depth; i++) {
        if (stack[i] == func)
            return true;
    }
    return false;
}

static const XiValue *xicgen_find_par_for_unsupported_body_value_depth(XiCgenCtx *ctx,
                                                                       const XiFunc *body,
                                                                       const XiFunc **stack,
                                                                       int depth);

static const XiValue *xicgen_find_par_for_unsupported_call_value(XiCgenCtx *ctx,
                                                                 const XiFunc *current,
                                                                 const XiValue *call,
                                                                 const XiFunc **stack, int depth) {
    const XiFunc *target = NULL;
    if (!ctx || !current || !call)
        return NULL;

    if (xicgen_atomic_call_is_i64_direct_nothrow(call))
        return NULL;
    if (cg_array_call_is_unchecked_bytes_trusted_nothrow(ctx, current, call))
        return NULL;
    if (cg_array_call_is_typed_fill_trusted_nothrow(ctx, current, call))
        return NULL;
    if (cg_array_builtin_call_is_trusted_nothrow(ctx, current, call))
        return NULL;
    if (xicgen_enum_method_call_is_aggregate_adt_construct(ctx, current, call))
        return NULL;

    if (call->op == XI_CALL && call->nargs >= 1) {
        CgStaticFunctionCall static_call =
            cg_resolve_static_function_call(ctx, current, call->args[0]);
        target = static_call.func;
    } else if ((call->op == XI_CALL_METHOD || call->op == XI_CALL_METHOD_DIRECT) &&
               call->nargs >= 1) {
        const char *method = (const char *) call->aux;
        if (method) {
            CgStaticFunctionCall module_call =
                cg_resolve_module_member_call(ctx, current, call, method);
            target = module_call.func;
        }
        if (!target) {
            const char *method_prefix = NULL;
            target = cg_class_native_resolve_method_call(ctx, current, call, &method_prefix);
        }
    }

    if (!target)
        return call;
    if (xicgen_par_for_stack_contains(stack, depth, target))
        return NULL;
    if (depth >= XICGEN_PAR_FOR_BODY_DEPTH_MAX)
        return call;
    return xicgen_find_par_for_unsupported_body_value_depth(ctx, target, stack, depth);
}

static const XiValue *xicgen_find_par_for_unsupported_body_value_depth(XiCgenCtx *ctx,
                                                                       const XiFunc *body,
                                                                       const XiFunc **stack,
                                                                       int depth) {
    if (!body)
        return NULL;
    if (depth >= XICGEN_PAR_FOR_BODY_DEPTH_MAX)
        return NULL;
    stack[depth++] = body;

    for (uint32_t bi = 0; bi < body->nblocks; bi++) {
        const XiBlock *block = body->blocks ? body->blocks[bi] : NULL;
        if (!block)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            const XiValue *value = block->values ? block->values[vi] : NULL;
            if (!value)
                continue;
            if ((value->flags & (XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND)) == 0)
                continue;
            if (xicgen_value_is_proven_nothrow(ctx, body, value, 0))
                continue;
            if (value->op == XI_INDEX_SET && cg_array_index_access_bounds_proven(ctx, body, value))
                continue;
            if (cg_div_mod_is_trusted_nothrow(body, value))
                continue;
            if (cg_array_bytes_load_le_unchecked_trusted_nothrow(ctx, body, value))
                continue;
            if (cg_array_builtin_call_is_trusted_nothrow(ctx, body, value))
                continue;
            if (xicgen_atomic_err_check_after_direct_nothrow(value))
                continue;
            if (cg_array_err_check_after_bytes_load_le_unchecked_trusted(ctx, body, value))
                continue;
            if (cg_array_err_check_after_unchecked_bytes_trusted(ctx, body, value))
                continue;
            if (cg_array_err_check_after_typed_fill_trusted(ctx, body, value))
                continue;
            if (cg_array_builtin_err_check_after_trusted_nothrow(ctx, body, value))
                continue;
            if (value->op == XI_CALL || value->op == XI_CALL_METHOD ||
                value->op == XI_CALL_METHOD_DIRECT) {
                const XiValue *unsupported =
                    xicgen_find_par_for_unsupported_call_value(ctx, body, value, stack, depth);
                if (!unsupported)
                    continue;
                return unsupported;
            }
            return value;
        }
    }
    return NULL;
}

static const XiValue *xicgen_find_par_for_unsupported_body_value(XiCgenCtx *ctx,
                                                                 const XiFunc *body) {
    const XiFunc *stack[XICGEN_PAR_FOR_BODY_DEPTH_MAX];
    memset(stack, 0, sizeof(stack));
    return xicgen_find_par_for_unsupported_body_value_depth(ctx, body, stack, 0);
}

static void xicgen_par_reduce_emit_abort_expr(XiCgenCtx *ctx, FILE *out) {
    if (ctx)
        ctx->error = true;
    fprintf(out, "(abort(), INT64_C(0))");
}

static void xicgen_par_reduce_emit_abort_value(XiCgenCtx *ctx, FILE *out, const char *agg_ctype) {
    if (ctx)
        ctx->error = true;
    if (agg_ctype && agg_ctype[0]) {
        fprintf(out, "({ abort(); ((%s){0}); })", agg_ctype);
        return;
    }
    fprintf(out, "(abort(), INT64_C(0))");
}

static const char *xicgen_type_label_noalloc(const XrType *type) {
    if (!type)
        return "?";
    if (type->alias_name)
        return type->alias_name;
    if ((type->kind == XR_KIND_INSTANCE || type->kind == XR_KIND_CLASS ||
         type->kind == XR_KIND_INTERFACE) &&
        type->instance.class_name)
        return type->instance.class_name;
    if (type->kind == XR_KIND_ENUM && type->enum_type.enum_name)
        return type->enum_type.enum_name;
    if (XR_TYPE_HAS_OBJECT_SHAPE(type) && type->object.type_name)
        return type->object.type_name;

    switch (type->kind) {
        case XR_KIND_INT:
            return "int";
        case XR_KIND_FLOAT:
            return "float";
        case XR_KIND_STRING:
            return "String";
        case XR_KIND_BOOL:
            return "bool";
        case XR_KIND_NULL:
            return "null";
        case XR_KIND_ARRAY:
            return "Array";
        case XR_KIND_MAP:
            return "Map";
        case XR_KIND_SET:
            return "Set";
        case XR_KIND_CHANNEL:
            return "Channel";
        case XR_KIND_JSON:
            return "Json";
        case XR_KIND_CLASS:
            return "class";
        case XR_KIND_INSTANCE:
            return "instance";
        case XR_KIND_INTERFACE:
            return "interface";
        case XR_KIND_FUNCTION:
            return "function";
        case XR_KIND_UNKNOWN:
            return "unknown";
        case XR_KIND_NEVER:
            return "never";
        case XR_KIND_UNIT:
            return "()";
        case XR_KIND_ENUM:
            return "enum";
        case XR_KIND_TYPE_PARAM:
            return "type parameter";
        case XR_KIND_TUPLE:
            return "tuple";
        case XR_KIND_UNION:
            return "union";
        case XR_KIND_FIXED_ARRAY:
            return "fixed array";
        case XR_KIND_POINTER:
            return type->ptr_is_mut ? "RawMut" : "RawPtr";
        case XR_KIND_CHAR:
            return "char";
        case XR_KIND_RECORD:
            return "record";
        case XR_KIND_SPAN:
            return "Span";
        case XR_KIND_COUNT:
            return "type";
    }
    return "type";
}

static bool xicgen_par_reduce_validate_i64_func(XiCgenCtx *ctx, const XiFunc *func,
                                                const char *role, uint16_t param_count) {
    if (!ctx || !func || func->nparams != param_count)
        return false;
    bool ok = cg_func_return_abi_rep(ctx, func) == XR_REP_I64;
    for (uint16_t i = 0; ok && i < param_count; i++)
        ok = cg_func_param_abi_rep(ctx, func, i) == XR_REP_I64;
    if (ok)
        return true;
    fprintf(stderr, "[xi_cgen] ERROR: parallel reduce AOT %s must use int64(%s) ABI: '%s'\n",
            role ? role : "callback", param_count == 3 ? "int64, int64, int64" : "int64, int64",
            func->name ? func->name : "?");
    return false;
}

static const char *xicgen_rep_label(XrRep rep) {
    switch (rep) {
        case XR_REP_I64:
            return "i64";
        case XR_REP_F64:
            return "f64";
        case XR_REP_PTR:
            return "ptr";
        case XR_REP_TAGGED:
            return "tagged";
        case XR_REP_VOID:
            return "void";
        case XR_REP_STR:
            return "str";
        case XR_REP_RAWPTR:
            return "rawptr";
        case XR_REP_COUNT:
            break;
    }
    return "rep";
}

static bool xicgen_par_collect_validate_scalar_func(XiCgenCtx *ctx, const XiFunc *func,
                                                    const CgArrayElemInfo *info) {
    if (!ctx || !func || !info || info->rep == XR_REP_TAGGED || func->nparams != 2)
        return false;
    if (cg_func_return_abi_rep(ctx, func) == info->rep &&
        cg_func_param_abi_rep(ctx, func, 0) == XR_REP_I64 &&
        cg_func_param_abi_rep(ctx, func, 1) == XR_REP_I64)
        return true;
    fprintf(
        stderr,
        "[xi_cgen] ERROR: parallel collect AOT scalar body must use %s(int64, int64) ABI: '%s'\n",
        xicgen_rep_label(info->rep), func->name ? func->name : "?");
    return false;
}

static bool xicgen_par_reduce_validate_nothrow_body(XiCgenCtx *ctx, const XiFunc *func,
                                                    const char *role) {
    if (!ctx || !func)
        return false;
    if (cg_func_needs_aot_coro_ctx(ctx, func)) {
        fprintf(stderr, "[xi_cgen] ERROR: parallel reduce AOT %s cannot be suspendable yet: '%s'\n",
                role ? role : "callback", func->name ? func->name : "?");
        return false;
    }
    const XiValue *unsupported = xicgen_find_par_for_unsupported_body_value(ctx, func);
    if (!unsupported)
        return true;
    const char *detail = NULL;
    if (unsupported->op == XI_CALL_BUILTIN && unsupported->aux)
        detail = (const char *) unsupported->aux;
    else if ((unsupported->op == XI_CALL_METHOD || unsupported->op == XI_CALL_METHOD_DIRECT) &&
             unsupported->aux)
        detail = (const char *) unsupported->aux;
    fprintf(stderr,
            "[xi_cgen] ERROR: parallel reduce AOT %s cannot throw or suspend yet: '%s' "
            "contains %s%s%s%s at v%u line %u\n",
            role ? role : "callback", func->name ? func->name : "?",
            xi_op_name((XiOp) unsupported->op), detail ? " '" : "", detail ? detail : "",
            detail ? "'" : "", unsupported->id, unsupported->line);
    return false;
}

static void xicgen_par_reduce_emit_serial_int64max(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                   const XiValue *v, const XiFunc *body,
                                                   const XiFunc *combine, const char *prefix,
                                                   const char *closure_name) {
    fprintf(out, "            if (_xr_pr_start_%u <= _xr_pr_end_%u) {\n", v->id, v->id);
    fprintf(out, "                for (int64_t _xr_pr_i_%u = _xr_pr_start_%u; ; _xr_pr_i_%u++) {\n",
            v->id, v->id, v->id);
    fprintf(out, "                    int64_t _xr_pr_item_%u = ", v->id);
    char iter_name[64];
    snprintf(iter_name, sizeof(iter_name), "_xr_pr_i_%u", v->id);
    xicgen_emit_par_reduce_body_call_value(ctx, out, f, body, v->args[4], prefix, iter_name, "0",
                                           closure_name);
    fprintf(out, ";\n");
    fprintf(out, "                    _xr_pr_out_%u = ", v->id);
    char out_name[64];
    char item_name[64];
    snprintf(out_name, sizeof(out_name), "_xr_pr_out_%u", v->id);
    snprintf(item_name, sizeof(item_name), "_xr_pr_item_%u", v->id);
    xicgen_emit_par_reduce_combine_call_value(ctx, out, combine, prefix, out_name, item_name);
    fprintf(out, ";\n");
    fprintf(out, "                    if (_xr_pr_i_%u == _xr_pr_end_%u) break;\n", v->id, v->id);
    fprintf(out, "                }\n");
    fprintf(out, "            }\n");
}

static void xicgen_par_reduce_emit_serial_agg_int64max(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                       const XiValue *v, const XiFunc *body,
                                                       const XiFunc *combine, const char *prefix,
                                                       const char *closure_name,
                                                       const char *agg_ctype) {
    fprintf(out, "            if (_xr_pr_start_%u <= _xr_pr_end_%u) {\n", v->id, v->id);
    fprintf(out, "                for (int64_t _xr_pr_i_%u = _xr_pr_start_%u; ; _xr_pr_i_%u++) {\n",
            v->id, v->id, v->id);
    fprintf(out, "                    %s _xr_pr_item_%u = ", agg_ctype, v->id);
    char iter_name[64];
    snprintf(iter_name, sizeof(iter_name), "_xr_pr_i_%u", v->id);
    xicgen_emit_par_reduce_body_call_value(ctx, out, f, body, v->args[4], prefix, iter_name, "0",
                                           closure_name);
    fprintf(out, ";\n");
    fprintf(out, "                    _xr_pr_out_%u = ", v->id);
    char out_name[64];
    char item_name[64];
    snprintf(out_name, sizeof(out_name), "_xr_pr_out_%u", v->id);
    snprintf(item_name, sizeof(item_name), "_xr_pr_item_%u", v->id);
    xicgen_emit_par_reduce_combine_call_aggregate(ctx, out, combine, prefix, out_name, item_name);
    fprintf(out, ";\n");
    fprintf(out, "                    if (_xr_pr_i_%u == _xr_pr_end_%u) break;\n", v->id, v->id);
    fprintf(out, "                }\n");
    fprintf(out, "            }\n");
}

static void xicgen_par_reduce(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    if (!ctx || !out || !f || !v || v->nargs < 6 || v->aux_kind != XI_AUX_KIND_PAR_REDUCE ||
        !v->aux) {
        xicgen_par_reduce_emit_abort_expr(ctx, out);
        return;
    }

    const XiParallelReduceData *data = (const XiParallelReduceData *) v->aux;
    const XiFunc *body = data->body_func;
    const XiFunc *combine = data->combine_func;
    bool i64_accumulator = xicgen_par_reduce_value_has_i64_accumulator(v);
    bool struct_accumulator = xicgen_par_reduce_value_has_struct_accumulator(ctx, v);
    const char *agg_ctype = struct_accumulator ? xicgen_par_reduce_struct_agg_c_type(ctx, v) : NULL;
    if (data->range_body && data->inclusive_end) {
        fprintf(stderr, "[xi_cgen] ERROR: parallel range reduce AOT requires an exclusive range\n");
        xicgen_par_reduce_emit_abort_value(ctx, out, agg_ctype);
        return;
    }
    if (!i64_accumulator && !struct_accumulator) {
        fprintf(stderr,
                "[xi_cgen] ERROR: parallel reduce AOT supports only native int or native struct "
                "accumulators; accumulator type '%s' is not native-reducible yet\n",
                xicgen_type_label_noalloc(data->accumulator_type));
        xicgen_par_reduce_emit_abort_value(ctx, out, agg_ctype);
        return;
    }
    uint16_t expected_body_params = data->range_body ? 3 : 2;
    if (!body || !combine ||
        (i64_accumulator &&
         (!xicgen_par_reduce_validate_i64_func(ctx, body, "body", expected_body_params) ||
          !xicgen_par_reduce_validate_i64_func(ctx, combine, "combine", 2))) ||
        (struct_accumulator && !agg_ctype) ||
        !xicgen_par_reduce_validate_nothrow_body(ctx, body, "body") ||
        !xicgen_par_reduce_validate_nothrow_body(ctx, combine, "combine")) {
        xicgen_par_reduce_emit_abort_value(ctx, out, agg_ctype);
        return;
    }
    if (combine->ncaptures != 0) {
        fprintf(stderr,
                "[xi_cgen] ERROR: parallel reduce AOT combine cannot capture values yet: '%s'\n",
                combine->name ? combine->name : "?");
        xicgen_par_reduce_emit_abort_value(ctx, out, agg_ctype);
        return;
    }

    bool scoped_closure = xicgen_par_reduce_uses_stack_body_closure(v, v->args[4], body);
    char scoped_closure_name[64];
    scoped_closure_name[0] = '\0';

    fprintf(out, "({\n");
    fprintf(out, "        int64_t _xr_pr_start_%u = ", v->id);
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
    fprintf(out, ";\n");
    fprintf(out, "        int64_t _xr_pr_end_%u = ", v->id);
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
    fprintf(out, ";\n");
    fprintf(out, "        int64_t _xr_pr_workers_%u = ", v->id);
    emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_I64);
    fprintf(out, ";\n");
    if (struct_accumulator) {
        fprintf(out, "        %s _xr_pr_out_%u = ", agg_ctype, v->id);
        emit_vref(out, v->args[3]);
        fprintf(out, ";\n");
    } else {
        fprintf(out, "        int64_t _xr_pr_out_%u = ", v->id);
        emit_value_as_rep_ctx(ctx, out, v->args[3], XR_REP_I64);
        fprintf(out, ";\n");
    }
    if (scoped_closure) {
        snprintf(scoped_closure_name, sizeof(scoped_closure_name), "_xr_par_closure_%u", v->id);
        xicgen_emit_par_for_scoped_closure(ctx, out, f, v, body, v->args[4], prefix);
    }
    if (struct_accumulator) {
        if (data->inclusive_end) {
            fprintf(out, "        if (_xr_pr_end_%u == INT64_MAX) {\n", v->id);
            xicgen_par_reduce_emit_serial_agg_int64max(ctx, out, f, v, body, combine, prefix,
                                                       scoped_closure ? scoped_closure_name : NULL,
                                                       agg_ctype);
            fprintf(out, "        } else {\n");
            fprintf(out, "            int64_t _xr_pr_end_excl_%u = _xr_pr_end_%u + 1;\n", v->id,
                    v->id);
            fprintf(out,
                    "            if (!xr_aot_parallel_reduce_agg(_xr_pr_start_%u, "
                    "_xr_pr_end_excl_%u, _xr_pr_workers_%u, sizeof(%s), &_xr_pr_out_%u, "
                    "(XrAotParReduceRangeAggFn)",
                    v->id, v->id, v->id, agg_ctype, v->id);
            xicgen_emit_par_reduce_range_wrapper_name(ctx, out, f, v, prefix);
            fprintf(out, ", (XrAotParReduceCombineAggFn)");
            xicgen_emit_par_reduce_combine_wrapper_name(ctx, out, f, v, prefix);
            fprintf(out, ", ");
            if (scoped_closure)
                fprintf(out, "%s", scoped_closure_name);
            else
                emit_call_hidden_closure(out, f, body, v->args[4]);
            fprintf(out, ", &_xr_pr_out_%u)) abort();\n", v->id);
            fprintf(out, "        }\n");
        } else {
            fprintf(out,
                    "        if (!xr_aot_parallel_reduce_agg(_xr_pr_start_%u, _xr_pr_end_%u, "
                    "_xr_pr_workers_%u, sizeof(%s), &_xr_pr_out_%u, "
                    "(XrAotParReduceRangeAggFn)",
                    v->id, v->id, v->id, agg_ctype, v->id);
            xicgen_emit_par_reduce_range_wrapper_name(ctx, out, f, v, prefix);
            fprintf(out, ", (XrAotParReduceCombineAggFn)");
            xicgen_emit_par_reduce_combine_wrapper_name(ctx, out, f, v, prefix);
            fprintf(out, ", ");
            if (scoped_closure)
                fprintf(out, "%s", scoped_closure_name);
            else
                emit_call_hidden_closure(out, f, body, v->args[4]);
            fprintf(out, ", &_xr_pr_out_%u)) abort();\n", v->id);
        }
    } else if (data->inclusive_end) {
        fprintf(out, "        if (_xr_pr_end_%u == INT64_MAX) {\n", v->id);
        xicgen_par_reduce_emit_serial_int64max(ctx, out, f, v, body, combine, prefix,
                                               scoped_closure ? scoped_closure_name : NULL);
        fprintf(out, "        } else {\n");
        fprintf(out, "            int64_t _xr_pr_end_excl_%u = _xr_pr_end_%u + 1;\n", v->id, v->id);
        fprintf(out,
                "            if (!xr_aot_parallel_reduce_i64(_xr_pr_start_%u, "
                "_xr_pr_end_excl_%u, _xr_pr_workers_%u, _xr_pr_out_%u, "
                "(XrAotParReduceRangeI64Fn)",
                v->id, v->id, v->id, v->id);
        xicgen_emit_par_reduce_range_wrapper_name(ctx, out, f, v, prefix);
        fprintf(out, ", (XrAotParReduceCombineI64Fn)");
        xicgen_emit_par_reduce_combine_wrapper_name(ctx, out, f, v, prefix);
        fprintf(out, ", ");
        if (scoped_closure)
            fprintf(out, "%s", scoped_closure_name);
        else
            emit_call_hidden_closure(out, f, body, v->args[4]);
        fprintf(out, ", &_xr_pr_out_%u)) abort();\n", v->id);
        fprintf(out, "        }\n");
    } else {
        fprintf(out,
                "        if (!xr_aot_parallel_reduce_i64(_xr_pr_start_%u, _xr_pr_end_%u, "
                "_xr_pr_workers_%u, _xr_pr_out_%u, (XrAotParReduceRangeI64Fn)",
                v->id, v->id, v->id, v->id);
        xicgen_emit_par_reduce_range_wrapper_name(ctx, out, f, v, prefix);
        fprintf(out, ", (XrAotParReduceCombineI64Fn)");
        xicgen_emit_par_reduce_combine_wrapper_name(ctx, out, f, v, prefix);
        fprintf(out, ", ");
        if (scoped_closure)
            fprintf(out, "%s", scoped_closure_name);
        else
            emit_call_hidden_closure(out, f, body, v->args[4]);
        fprintf(out, ", &_xr_pr_out_%u)) abort();\n", v->id);
    }
    fprintf(out, "        ");
    if (struct_accumulator) {
        fprintf(out, "_xr_pr_out_%u", v->id);
    } else {
        XrRep result_rep = cg_value_plan_storage_rep(ctx, v);
        if (result_rep == XR_REP_TAGGED)
            fprintf(out, "XR_FROM_INT(_xr_pr_out_%u)", v->id);
        else
            fprintf(out, "_xr_pr_out_%u", v->id);
    }
    fprintf(out, ";\n");
    fprintf(out, "    })");
}

static void xicgen_par_collect_emit_store(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                          const XiValue *v, const XiFunc *body, const char *prefix,
                                          const char *iter_name, const char *worker_name,
                                          const char *closure_name, const char *out_ptr_name,
                                          const char *idx_name, bool native_result,
                                          const CgArrayElemInfo *info) {
    if (native_result && info) {
        fprintf(out, "            ((%s*)%s->data)[%s] = (%s)", info->ctype, out_ptr_name, idx_name,
                info->ctype);
        xicgen_emit_par_collect_body_call_as_rep(ctx, out, f, body, v->args[3], prefix, iter_name,
                                                 worker_name, closure_name, info->rep);
        fprintf(out, ";\n");
        return;
    }
    fprintf(out, "            XrValue _xr_pc_item_%u = ", v->id);
    xicgen_emit_par_collect_body_call_as_rep(ctx, out, f, body, v->args[3], prefix, iter_name,
                                             worker_name, closure_name, XR_REP_TAGGED);
    fprintf(out, ";\n");
    fprintf(out, "            xrt_array_write_preallocated(%s, %s, _xr_pc_item_%u);\n",
            out_ptr_name, idx_name, v->id);
}

static void xicgen_par_collect(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                               const char *prefix) {
    if (!ctx || !out || !f || !v || v->nargs < 4 || v->aux_kind != XI_AUX_KIND_PAR_COLLECT ||
        !v->aux) {
        xicgen_par_reduce_emit_abort_expr(ctx, out);
        return;
    }

    const XiParallelCollectData *data = (const XiParallelCollectData *) v->aux;
    const XiFunc *body = data->body_func;
    if (data->direct_lane_writes) {
        uint16_t expected_params = body ? body->nparams : 0;
        bool range_body = expected_params == 3;
        if (!body || (expected_params != 2 && expected_params != 3) ||
            cg_func_return_abi_rep(ctx, body) != XR_REP_VOID ||
            cg_func_param_abi_rep(ctx, body, 0) != XR_REP_I64 ||
            cg_func_param_abi_rep(ctx, body, 1) != XR_REP_I64 ||
            (range_body && cg_func_param_abi_rep(ctx, body, 2) != XR_REP_I64) ||
            !xicgen_par_reduce_validate_nothrow_body(ctx, body, "collect lane body")) {
            xicgen_par_reduce_emit_abort_expr(ctx, out);
            return;
        }
        if (range_body && data->inclusive_end) {
            fprintf(stderr, "[xi_cgen] ERROR: parallel collect local initializer AOT requires an "
                            "exclusive range\n");
            xicgen_par_reduce_emit_abort_expr(ctx, out);
            return;
        }

        fprintf(out, "({\n");
        fprintf(out, "        int64_t _xr_pc_start_%u = ", v->id);
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
        fprintf(out, ";\n");
        fprintf(out, "        int64_t _xr_pc_end_%u = ", v->id);
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
        fprintf(out, ";\n");
        fprintf(out, "        int64_t _xr_pc_workers_%u = ", v->id);
        emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_I64);
        fprintf(out, ";\n");
        if (data->inclusive_end) {
            fprintf(out, "        bool _xr_pc_serial_max_%u = _xr_pc_end_%u == INT64_MAX;\n", v->id,
                    v->id);
            fprintf(out,
                    "        int64_t _xr_pc_end_excl_%u = _xr_pc_serial_max_%u ? _xr_pc_end_%u : "
                    "_xr_pc_end_%u + 1;\n",
                    v->id, v->id, v->id, v->id);
        } else {
            fprintf(out, "        bool _xr_pc_serial_max_%u = false;\n", v->id);
            fprintf(out, "        int64_t _xr_pc_end_excl_%u = _xr_pc_end_%u;\n", v->id, v->id);
        }
        fprintf(out,
                "        int64_t _xr_pc_count_%u = _xr_pc_serial_max_%u ? (_xr_pc_end_%u >= "
                "_xr_pc_start_%u ? _xr_pc_end_%u - _xr_pc_start_%u + 1 : 0) : (_xr_pc_end_excl_%u "
                "> _xr_pc_start_%u ? _xr_pc_end_excl_%u - _xr_pc_start_%u : 0);\n",
                v->id, v->id, v->id, v->id, v->id, v->id, v->id, v->id, v->id, v->id);

        uint16_t lane_count = data->lane_count;
        if (lane_count < 1 || lane_count > 16 || v->nargs < (uint16_t) (4u + lane_count)) {
            fprintf(stderr, "[xi_cgen] ERROR: parallel collect lane metadata mismatch\n");
            xicgen_par_reduce_emit_abort_expr(ctx, out);
            fprintf(out, ";\n    })");
            return;
        }
        if (!data->into_result && lane_count != 1) {
            fprintf(stderr,
                    "[xi_cgen] ERROR: returning parallel collect direct lanes require one lane\n");
            xicgen_par_reduce_emit_abort_expr(ctx, out);
            fprintf(out, ";\n    })");
            return;
        }
        for (uint16_t i = 0; i < lane_count; i++) {
            CgArrayElemInfo lane_info;
            bool have_lane_info = cg_array_value_storage_info(ctx, f, v->args[4 + i], &lane_info,
                                                              CG_ARRAY_STORAGE_MUTABLE);
            bool typed_full_overwrite = have_lane_info && lane_info.elem_name &&
                                        strcmp(lane_info.elem_name, "XR_ELEM_ANY") != 0;
            fprintf(out, "        XrValue _xr_pc_result_%u_%u = ", v->id, (unsigned) i);
            emit_value_as_rep_ctx(ctx, out, v->args[4 + i], XR_REP_TAGGED);
            fprintf(out, ";\n");
            fprintf(
                out,
                "        xrt_array_t *_xr_pc_out_%u_%u = (xrt_array_t*)_xr_pc_result_%u_%u.ptr;\n",
                v->id, (unsigned) i, v->id, (unsigned) i);
            if (typed_full_overwrite) {
                fprintf(out,
                        "        if (_xr_pc_out_%u_%u->data_storage == XR_ARRAY_DATA_BORROWED) "
                        "abort();\n",
                        v->id, (unsigned) i);
                fprintf(out,
                        "        if (_xr_pc_count_%u > _xr_pc_out_%u_%u->capacity) "
                        "xrt_array_reserve_raw(_xr_pc_out_%u_%u, _xr_pc_count_%u);\n",
                        v->id, v->id, (unsigned) i, v->id, (unsigned) i, v->id);
                fprintf(out, "        _xr_pc_out_%u_%u->length = _xr_pc_count_%u;\n", v->id,
                        (unsigned) i, v->id);
            } else {
                fprintf(out,
                        "        xrt_array_resize_value(_xr_pc_result_%u_%u, "
                        "XR_FROM_INT(_xr_pc_count_%u), ",
                        v->id, (unsigned) i, v->id);
                xicgen_emit_par_collect_zero_value(out, have_lane_info ? &lane_info : NULL);
                fprintf(out, ");\n");
            }
        }

        bool scoped_closure = xicgen_par_collect_uses_stack_body_closure(v, v->args[3], body);
        char closure_name[64];
        closure_name[0] = '\0';
        if (scoped_closure) {
            snprintf(closure_name, sizeof(closure_name), "_xr_pc_closure_%u", v->id);
            uint16_t ncap = body ? body->ncaptures : 0;
            fprintf(out,
                    "        union { XrObjHeader hdr; long double align; unsigned char "
                    "bytes[sizeof(XrObjHeader) + sizeof(xrt_closure_t) + %u * sizeof(XrValue)]; } "
                    "_xr_pc_closure_storage_%u;\n",
                    ncap, v->id);
            fprintf(out,
                    "        memset(&_xr_pc_closure_storage_%u, 0, "
                    "sizeof(_xr_pc_closure_storage_%u));\n",
                    v->id, v->id);
            fprintf(out,
                    "        XrObjHeader *_xr_pc_closure_hdr_%u = (XrObjHeader "
                    "*)_xr_pc_closure_storage_%u.bytes;\n",
                    v->id, v->id);
            fprintf(out, "        _xr_pc_closure_hdr_%u->extra = XR_OBJ_STORAGE_STACK;\n", v->id);
            fprintf(out,
                    "        xrt_closure_t *_xr_pc_closure_%u = (xrt_closure_t *)((char "
                    "*)_xr_pc_closure_hdr_%u + sizeof(XrObjHeader));\n",
                    v->id, v->id);
            fprintf(out, "        xrt_closure_init(_xr_pc_closure_%u, (void*)", v->id);
            emit_closure_entry_pointer(ctx, out, prefix, body);
            fprintf(out, ", %u);\n", ncap);
            fprintf(out, "        { xrt_closure_t *_c = _xr_pc_closure_%u; ", v->id);
            emit_closure_upval_initializers(ctx, out, f, v->args[3], false);
            fprintf(out, "}\n");
        }

        char iter_name[64];
        snprintf(iter_name, sizeof(iter_name), "_xr_pc_i_%u", v->id);
        fprintf(out, "        if (_xr_pc_count_%u > 0) {\n", v->id);
        fprintf(out, "            if (_xr_pc_serial_max_%u) {\n", v->id);
        if (range_body) {
            fprintf(out, "                abort();\n");
        } else {
            fprintf(out, "                for (int64_t %s = _xr_pc_start_%u; ; %s++) {\n",
                    iter_name, v->id, iter_name);
            xicgen_emit_par_for_body_call(ctx, out, f, body, v->args[3], prefix, iter_name, "0",
                                          scoped_closure ? closure_name : NULL);
            fprintf(out, "                    if (%s == _xr_pc_end_%u) break;\n", iter_name, v->id);
            fprintf(out, "                }\n");
        }
        fprintf(out,
                "            } else if (!xr_aot_parallel_for_range_i64(_xr_pc_start_%u, "
                "_xr_pc_end_excl_%u, _xr_pc_workers_%u, (XrAotParForRangeI64Fn)",
                v->id, v->id, v->id);
        xicgen_emit_par_collect_range_wrapper_name(ctx, out, f, v, prefix);
        fprintf(out, ", ");
        if (scoped_closure)
            fprintf(out, "%s", closure_name);
        else
            emit_call_hidden_closure(out, f, body, v->args[3]);
        fprintf(out, ")) abort();\n");
        fprintf(out, "        }\n");
        fprintf(out, "        ");
        if (data->into_result)
            fprintf(out, "XR_NULL_VAL");
        else if (xicgen_value_c_storage_rep(ctx, f, v) == XR_REP_PTR)
            fprintf(out, "_xr_pc_out_%u_0", v->id);
        else
            fprintf(out, "_xr_pc_result_%u_0", v->id);
        fprintf(out, ";\n");
        fprintf(out, "    })");
        return;
    }

    CgArrayElemInfo info;
    bool into_result = data->into_result && v->nargs >= 5;
    bool have_info = xicgen_par_collect_array_elem_info(ctx, v, &info);
    bool typed_full_overwrite =
        have_info && info.elem_name && strcmp(info.elem_name, "XR_ELEM_ANY") != 0;
    bool native_result = have_info && xicgen_par_collect_body_has_native_result(ctx, v, &info);
    if (!body || body->nparams != 2 ||
        (body->native_callback_kind == XI_NATIVE_CALLBACK_PAR_COLLECT_SCALAR_BODY &&
         !xicgen_par_collect_validate_scalar_func(ctx, body, have_info ? &info : NULL)) ||
        !xicgen_par_reduce_validate_nothrow_body(ctx, body, "collect body")) {
        xicgen_par_reduce_emit_abort_expr(ctx, out);
        return;
    }

    XrRep storage_rep = xicgen_value_c_storage_rep(ctx, f, v);
    fprintf(out, "({\n");
    fprintf(out, "        int64_t _xr_pc_start_%u = ", v->id);
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
    fprintf(out, ";\n");
    fprintf(out, "        int64_t _xr_pc_end_%u = ", v->id);
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
    fprintf(out, ";\n");
    fprintf(out, "        int64_t _xr_pc_workers_%u = ", v->id);
    emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_I64);
    fprintf(out, ";\n");
    if (data->inclusive_end) {
        fprintf(out, "        bool _xr_pc_serial_max_%u = _xr_pc_end_%u == INT64_MAX;\n", v->id,
                v->id);
        fprintf(out,
                "        int64_t _xr_pc_end_excl_%u = _xr_pc_serial_max_%u ? _xr_pc_end_%u : "
                "_xr_pc_end_%u + 1;\n",
                v->id, v->id, v->id, v->id);
    } else {
        fprintf(out, "        bool _xr_pc_serial_max_%u = false;\n", v->id);
        fprintf(out, "        int64_t _xr_pc_end_excl_%u = _xr_pc_end_%u;\n", v->id, v->id);
    }
    fprintf(out,
            "        int64_t _xr_pc_count_%u = _xr_pc_serial_max_%u ? (_xr_pc_end_%u >= "
            "_xr_pc_start_%u ? _xr_pc_end_%u - _xr_pc_start_%u + 1 : 0) : (_xr_pc_end_excl_%u > "
            "_xr_pc_start_%u ? _xr_pc_end_excl_%u - _xr_pc_start_%u : 0);\n",
            v->id, v->id, v->id, v->id, v->id, v->id, v->id, v->id, v->id, v->id);
    if (into_result) {
        fprintf(out, "        XrValue _xr_pc_result_%u = ", v->id);
        emit_value_as_rep_ctx(ctx, out, v->args[4], XR_REP_TAGGED);
        fprintf(out, ";\n");
        fprintf(out, "        xrt_array_t *_xr_pc_out_%u = (xrt_array_t*)_xr_pc_result_%u.ptr;\n",
                v->id, v->id);
        if (typed_full_overwrite) {
            fprintf(out,
                    "        if (_xr_pc_out_%u->data_storage == XR_ARRAY_DATA_BORROWED) abort();\n",
                    v->id);
            fprintf(out,
                    "        if (_xr_pc_count_%u > _xr_pc_out_%u->capacity) "
                    "xrt_array_reserve_raw(_xr_pc_out_%u, _xr_pc_count_%u);\n",
                    v->id, v->id, v->id, v->id);
            fprintf(out, "        _xr_pc_out_%u->length = _xr_pc_count_%u;\n", v->id, v->id);
        } else {
            fprintf(
                out,
                "        xrt_array_resize_value(_xr_pc_result_%u, XR_FROM_INT(_xr_pc_count_%u), ",
                v->id, v->id);
            xicgen_emit_par_collect_zero_value(out, have_info ? &info : NULL);
            fprintf(out, ");\n");
        }
    } else if (storage_rep == XR_REP_PTR) {
        fprintf(out, "        xrt_array_t *_xr_pc_out_%u = ", v->id);
        if (typed_full_overwrite)
            fprintf(out, "xrt_array_new_typed_uninit_ptr(_xr_pc_count_%u, %s)", v->id,
                    info.elem_name);
        else if (have_info)
            fprintf(out, "xrt_array_new_typed_ptr(_xr_pc_count_%u, %s)", v->id, info.elem_name);
        else
            fprintf(out, "(xrt_array_t*)xrt_array_new(_xr_pc_count_%u).ptr", v->id);
        fprintf(out, ";\n");
        if (typed_full_overwrite)
            fprintf(out, "        _xr_pc_out_%u->length = _xr_pc_count_%u;\n", v->id, v->id);
        fprintf(out, "        XrValue _xr_pc_result_%u = xr_mkptr(_xr_pc_out_%u, XR_TAG_ARRAY);\n",
                v->id, v->id);
    } else {
        fprintf(out, "        XrValue _xr_pc_result_%u = ", v->id);
        if (typed_full_overwrite)
            fprintf(out, "xrt_array_new_typed_uninit(_xr_pc_count_%u, %s)", v->id, info.elem_name);
        else if (have_info)
            fprintf(out, "xrt_array_new_typed(_xr_pc_count_%u, %s)", v->id, info.elem_name);
        else
            fprintf(out, "xrt_array_new(_xr_pc_count_%u)", v->id);
        fprintf(out, ";\n");
        fprintf(out, "        xrt_array_t *_xr_pc_out_%u = (xrt_array_t*)_xr_pc_result_%u.ptr;\n",
                v->id, v->id);
        if (typed_full_overwrite)
            fprintf(out, "        _xr_pc_out_%u->length = _xr_pc_count_%u;\n", v->id, v->id);
    }

    char result_value_name[64];
    char start_name[64];
    snprintf(result_value_name, sizeof(result_value_name), "_xr_pc_result_%u", v->id);
    snprintf(start_name, sizeof(start_name), "_xr_pc_start_%u", v->id);
    xicgen_emit_par_collect_scoped_closure(ctx, out, f, v, body, v->args[3], prefix,
                                           result_value_name, start_name);

    char out_ptr_name[64];
    char iter_name[64];
    char idx_name[64];
    char closure_name[64];
    snprintf(out_ptr_name, sizeof(out_ptr_name), "_xr_pc_out_%u", v->id);
    snprintf(iter_name, sizeof(iter_name), "_xr_pc_i_%u", v->id);
    snprintf(idx_name, sizeof(idx_name), "_xr_pc_idx_%u", v->id);
    snprintf(closure_name, sizeof(closure_name), "_xr_pc_closure_%u", v->id);

    fprintf(out, "        if (_xr_pc_count_%u > 0) {\n", v->id);
    fprintf(out, "            if (_xr_pc_serial_max_%u) {\n", v->id);
    fprintf(out, "                for (int64_t %s = _xr_pc_start_%u; ; %s++) {\n", iter_name, v->id,
            iter_name);
    fprintf(out, "            int64_t %s = %s - _xr_pc_start_%u;\n", idx_name, iter_name, v->id);
    xicgen_par_collect_emit_store(ctx, out, f, v, body, prefix, iter_name, "0", closure_name,
                                  out_ptr_name, idx_name, native_result, have_info ? &info : NULL);
    fprintf(out, "                    if (%s == _xr_pc_end_%u) break;\n", iter_name, v->id);
    fprintf(out, "                }\n");
    fprintf(out,
            "            } else if (!xr_aot_parallel_for_range_i64(_xr_pc_start_%u, "
            "_xr_pc_end_excl_%u, _xr_pc_workers_%u, (XrAotParForRangeI64Fn)",
            v->id, v->id, v->id);
    xicgen_emit_par_collect_range_wrapper_name(ctx, out, f, v, prefix);
    fprintf(out, ", _xr_pc_closure_%u)) abort();\n", v->id);
    fprintf(out, "        }\n");
    fprintf(out, "        ");
    if (into_result)
        fprintf(out, "XR_NULL_VAL");
    else if (storage_rep == XR_REP_PTR)
        fprintf(out, "_xr_pc_out_%u", v->id);
    else
        fprintf(out, "_xr_pc_result_%u", v->id);
    fprintf(out, ";\n");
    fprintf(out, "    })");
}

static void xicgen_par_for(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                           const char *prefix) {
    if (!ctx || !out || !f || !v || v->nargs < 4 || v->aux_kind != XI_AUX_KIND_PAR_FOR || !v->aux) {
        if (ctx)
            ctx->error = true;
        fprintf(out, "    abort();\n");
        return;
    }

    const XiParallelForData *data = (const XiParallelForData *) v->aux;
    const XiFunc *body = data->body_func;
    if (data->range_body && data->inclusive_end && data->end_name) {
        ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: parallel range AOT currently requires an exclusive range: "
                "'%s'\n",
                body && body->name ? body->name : "?");
        fprintf(out, "    abort();\n");
        return;
    }
    uint16_t expected_params = data->range_body ? 3 : 2;
    if (!body || body->nparams != expected_params) {
        ctx->error = true;
        fprintf(out, "    abort();\n");
        return;
    }
    bool abi_ok = cg_func_return_abi_rep(ctx, body) == XR_REP_VOID &&
                  cg_func_param_abi_rep(ctx, body, 0) == XR_REP_I64 &&
                  cg_func_param_abi_rep(ctx, body, 1) == XR_REP_I64 &&
                  (!data->range_body || cg_func_param_abi_rep(ctx, body, 2) == XR_REP_I64);
    if (!abi_ok) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: parallel %s AOT body must use %s ABI: '%s'\n",
                data->range_body ? "range" : "for",
                data->range_body ? "void(int64, int64, int64)" : "void(int64, int64)",
                body->name ? body->name : "?");
        fprintf(out, "    abort();\n");
        return;
    }
    if (cg_func_needs_aot_coro_ctx(ctx, body)) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: parallel for AOT body cannot be suspendable yet: '%s'\n",
                body->name ? body->name : "?");
        fprintf(out, "    abort();\n");
        return;
    }
    const XiValue *unsupported = xicgen_find_par_for_unsupported_body_value(ctx, body);
    if (unsupported) {
        ctx->error = true;
        const char *detail = NULL;
        if (unsupported->op == XI_CALL_BUILTIN && unsupported->aux)
            detail = (const char *) unsupported->aux;
        else if ((unsupported->op == XI_CALL_METHOD || unsupported->op == XI_CALL_METHOD_DIRECT) &&
                 unsupported->aux)
            detail = (const char *) unsupported->aux;
        fprintf(stderr,
                "[xi_cgen] ERROR: parallel for AOT body cannot throw or suspend yet: '%s' "
                "contains %s%s%s%s at v%u line %u\n",
                body->name ? body->name : "?", xi_op_name((XiOp) unsupported->op),
                detail ? " '" : "", detail ? detail : "", detail ? "'" : "", unsupported->id,
                unsupported->line);
        fprintf(out, "    abort();\n");
        return;
    }

    char iter_name[64];
    snprintf(iter_name, sizeof(iter_name), "_xr_par_i_%u", v->id);

    fprintf(out, "    {\n");
    fprintf(out, "        int64_t _xr_par_start_%u = ", v->id);
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
    fprintf(out, ";\n");
    fprintf(out, "        int64_t _xr_par_end_%u = ", v->id);
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
    fprintf(out, ";\n");
    fprintf(out, "        int64_t _xr_par_workers_%u = ", v->id);
    emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_I64);
    fprintf(out, ";\n");
    bool scoped_closure = xicgen_par_for_uses_stack_callback_closure(v, v->args[3], body);
    char scoped_closure_name[64];
    scoped_closure_name[0] = '\0';
    if (scoped_closure) {
        snprintf(scoped_closure_name, sizeof(scoped_closure_name), "_xr_par_closure_%u", v->id);
        xicgen_emit_par_for_scoped_closure(ctx, out, f, v, body, v->args[3], prefix);
    }
    if (data->inclusive_end) {
        fprintf(out, "        if (_xr_par_end_%u == INT64_MAX) {\n", v->id);
        if (data->range_body) {
            fprintf(out, "            abort();\n");
        } else {
            fprintf(out, "            if (_xr_par_start_%u <= _xr_par_end_%u) {\n", v->id, v->id);
            fprintf(out, "                for (int64_t %s = _xr_par_start_%u; ; %s++) {\n",
                    iter_name, v->id, iter_name);
            xicgen_emit_par_for_body_call(ctx, out, f, body, v->args[3], prefix, iter_name, "0",
                                          scoped_closure ? scoped_closure_name : NULL);
            fprintf(out, "                    if (%s == _xr_par_end_%u) break;\n", iter_name,
                    v->id);
            fprintf(out, "                }\n");
            fprintf(out, "            }\n");
        }
        fprintf(out, "        } else {\n");
        fprintf(out, "            int64_t _xr_par_end_excl_%u = _xr_par_end_%u + 1;\n", v->id,
                v->id);
        fprintf(out,
                "            if (!xr_aot_parallel_for_range_i64(_xr_par_start_%u, "
                "_xr_par_end_excl_%u, _xr_par_workers_%u, (XrAotParForRangeI64Fn)",
                v->id, v->id, v->id);
        xicgen_emit_par_for_range_wrapper_name(ctx, out, f, v, prefix);
        fprintf(out, ", ");
        if (scoped_closure)
            fprintf(out, "%s", scoped_closure_name);
        else
            emit_call_hidden_closure(out, f, body, v->args[3]);
        fprintf(out, ")) abort();\n");
        fprintf(out, "        }\n");
    } else {
        fprintf(out,
                "        if (!xr_aot_parallel_for_range_i64(_xr_par_start_%u, _xr_par_end_%u, "
                "_xr_par_workers_%u, (XrAotParForRangeI64Fn)",
                v->id, v->id, v->id);
        xicgen_emit_par_for_range_wrapper_name(ctx, out, f, v, prefix);
        fprintf(out, ", ");
        if (scoped_closure)
            fprintf(out, "%s", scoped_closure_name);
        else
            emit_call_hidden_closure(out, f, body, v->args[3]);
        fprintf(out, ")) abort();\n");
    }
    fprintf(out, "    }\n");
}

static bool xi_to_c_emit_generated(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                   const char *prefix) {
    switch (v->op) {
        case XI_PAR_FOR:
            xicgen_par_for(ctx, out, f, v, prefix);
            return true;
        case XI_PAR_COLLECT:
            xicgen_par_collect(ctx, out, f, v, prefix);
            return true;
        case XI_PAR_REDUCE:
            xicgen_par_reduce(ctx, out, f, v, prefix);
            return true;
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
