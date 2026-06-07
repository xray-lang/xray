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
        const char *s = (const char *) v->aux;
        fprintf(out, "xr_box_str(");
        emit_c_string_literal(out, s);
        fprintf(out, ")");
    } else if (v->type->kind == XR_KIND_UNKNOWN && v->aux) {
        emit_enum_type_expr(out, cg_enum_for_runtime_type(ctx, v->aux));
    } else {
        fprintf(out, "XR_NULL_VAL /* unknown const kind */");
    }
}

static void xicgen_param(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                         const char *prefix) {
    (void) prefix;
    fprintf(out, "p%u", (unsigned) v->aux_int);
    if (cg_func_param_abi_rep(ctx, f, (uint16_t) v->aux_int) != XR_REP_TAGGED)
        return;
    if (cg_rep(v) == XR_REP_I64)
        fprintf(out, ".i");
    else if (cg_rep(v) == XR_REP_F64)
        fprintf(out, ".f");
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
    } else {
        fprintf(out, "-");
        emit_vref(out, v->args[0]);
    }
}

static void xicgen_bitwise_binop(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                 const char *prefix, const char *op) {
    (void) ctx;
    (void) f;
    (void) prefix;
    emit_bitwise_binop(out, v, op);
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
    (void) ctx;
    (void) f;
    (void) prefix;
    emit_bitwise_unop(out, v, "~");
}

static void xicgen_not(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                       const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    fprintf(out, "!(");
    emit_condition_expr(out, v->args[0]);
    fprintf(out, ")");
}

static void xicgen_select(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                          const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs == 3, "xicgen_select: need cond, true, false");
    fprintf(out, "(");
    emit_condition_expr(out, v->args[0]);
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
    (void) f;
    emit_closure_new_expr(ctx, out, prefix, v);
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

static void xicgen_shl(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                       const char *prefix) {
    xicgen_bitwise_binop(ctx, out, f, v, prefix, "<<");
}

static void xicgen_shr(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                       const char *prefix) {
    xicgen_bitwise_binop(ctx, out, f, v, prefix, ">>");
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
    xicgen_f32_roundtrip(out, v, cg_array_index_get_reads_f32_storage(v->args[0]));
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
    (void) ctx;
    (void) f;
    (void) prefix;
    struct XrType *sty = v->args[0]->type;
    if (sty && sty->kind == XR_KIND_NULL) {
        emit_vref(out, v->args[0]);
    } else if (sty && sty->kind == XR_KIND_FLOAT) {
        fprintf(out, "XR_FROM_FLOAT(");
        emit_vref(out, v->args[0]);
        fprintf(out, ")");
    } else if (sty && sty->kind == XR_KIND_BOOL) {
        fprintf(out, "XR_FROM_BOOL(");
        emit_vref(out, v->args[0]);
        fprintf(out, ")");
    } else if (sty && sty->kind == XR_KIND_STRING) {
        emit_vref(out, v->args[0]);
    } else {
        fprintf(out, "XR_FROM_INT(");
        emit_vref(out, v->args[0]);
        fprintf(out, ")");
    }
}

static void xicgen_unbox(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                         const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    XrRep ur = cg_rep(v);
    emit_vref(out, v->args[0]);
    if (ur == XR_REP_F64)
        fprintf(out, ".f");
    else if (ur == XR_REP_I64)
        fprintf(out, ".i");
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
    bool wrapped = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
    fprintf(out, "(int64_t)xrt_bytes_load_u32_le_raw(");
    xicgen_bytes_ptr_arg(ctx, out, f, v, prefix, 0);
    fprintf(out, ", ");
    xicgen_bytes_i64_arg(out, v, 1);
    fprintf(out, ")");
    emit_conversion_suffix(out, wrapped);
}

static void xicgen_bytes_load_u64_le(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                     const char *prefix) {
    bool wrapped = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
    fprintf(out, "(int64_t)xrt_bytes_load_u64_le_raw(");
    xicgen_bytes_ptr_arg(ctx, out, f, v, prefix, 0);
    fprintf(out, ", ");
    xicgen_bytes_i64_arg(out, v, 1);
    fprintf(out, ")");
    emit_conversion_suffix(out, wrapped);
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
