/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_arith_helpers.inc.c - AOT arithmetic fast-path emission helpers
 */

static bool cg_const_int_value(const XiValue *value, int64_t *out) {
    const XiValue *v = value;
    for (uint8_t depth = 0; depth < 4; depth++) {
        v = cg_unwrap_identity_value(v);
        if (!v || (v->op != XI_BOX && v->op != XI_UNBOX) || v->nargs != 1)
            break;
        v = v->args[0];
    }
    v = cg_unwrap_identity_value(v);
    if (!v || v->op != XI_CONST || !v->type || v->type->kind != XR_KIND_INT || !out)
        return false;
    *out = v->aux_int;
    return true;
}

static bool cg_const_int_value_in_func(XiCgenCtx *ctx, const XiFunc *f, const XiValue *value,
                                       int64_t *out) {
    if (cg_const_int_value(value, out))
        return true;
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!ctx || !f || !v || v->op != XI_GET_SHARED || v->aux_int < 0 || v->aux_int > UINT16_MAX ||
        !out)
        return false;
    uint16_t slot = (uint16_t) v->aux_int;
    for (const XiFunc *cur = f; cur; cur = cur->parent_func) {
        if (!cur->shared_const_literals || slot >= cur->shared_const_literal_count ||
            !cur->slot_owned_consts || slot >= cur->nshared || !cur->slot_owned_consts[slot])
            continue;
        const XiConstLiteral *lit = &cur->shared_const_literals[slot];
        if (lit->kind == XI_CONST_LITERAL_INT) {
            *out = lit->int_value;
            return true;
        }
    }
    const XiModule *module = cg_module_for_func(ctx, f);
    const XiConstLiteral *lit = cg_module_const_literal(module, slot);
    if (!lit || lit->kind == XI_CONST_LITERAL_NONE)
        lit = cg_import_slot_const_literal(ctx, f, slot, NULL, NULL);
    if (!lit || lit->kind != XI_CONST_LITERAL_INT)
        return false;
    *out = lit->int_value;
    return true;
}

/* Kind token for the shared xi.div/xi.mod owner, spelled into generated C. */
static const char *cg_int_div_mod_kind_token(uint16_t op, bool unsigned_kind) {
    if (unsigned_kind)
        return op == XI_DIV ? "XR_INT_DIV_MOD_DIV_U" : "XR_INT_DIV_MOD_MOD_U";
    return op == XI_DIV ? "XR_INT_DIV_MOD_DIV" : "XR_INT_DIV_MOD_MOD";
}

/* Open an owner-adapter call for a divisor the plan already discharged. The
 * proof token is what keeps the emitted expression equal to the bare machine
 * divide it replaces: a positive divisor makes the signed wrap rule
 * unreachable, and a nonzero divisor drops the zero probe. Proving something
 * about the divisor is therefore a narrower rule from the same owner, never a
 * private lowering. */
static bool emit_int_div_mod_proven_head(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                         bool unsigned_kind, const char *proof) {
    const char *adapter = cg_int_div_mod_adapter_name(ctx);
    if (!adapter || !proof || (ctx && ctx->c_dialect == XI_CGEN_C_DIALECT_C90))
        return false;
    fprintf(out, "%s(%s, %s, ", adapter, cg_int_div_mod_kind_token(v->op, unsigned_kind), proof);
    return true;
}

static bool emit_native_const_div_mod_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                           const XiValue *v) {
    if (!v || v->nargs < 2 || cg_value_plan_storage_rep(ctx, v) != XR_REP_I64 ||
        cg_value_plan_storage_rep(ctx, v->args[0]) != XR_REP_I64)
        return false;
    if (v->op != XI_DIV && v->op != XI_MOD)
        return false;

    int64_t divisor = 0;
    if (!cg_const_int_value_in_func(ctx, f, v->args[1], &divisor) || divisor == 0)
        return false;
    if (!emit_int_div_mod_proven_head(ctx, out, v, false, "XR_INT_DIV_MOD_PROOF_NONZERO"))
        return false;

    emit_vref(out, v->args[0]);
    fprintf(out, ", INT64_C(%" PRId64 "))", divisor);
    return true;
}

static bool emit_native_positive_divisor_div_mod_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                      const XiValue *v) {
    if (!v || v->nargs < 2 || cg_value_plan_storage_rep(ctx, v) != XR_REP_I64 ||
        cg_value_plan_storage_rep(ctx, v->args[0]) != XR_REP_I64 ||
        cg_value_plan_storage_rep(ctx, v->args[1]) != XR_REP_I64)
        return false;
    if (v->op != XI_DIV && v->op != XI_MOD)
        return false;
    if (!xi_value_known_positive_at(f, v->args[1], v->block))
        return false;
    if (!emit_int_div_mod_proven_head(ctx, out, v, false, "XR_INT_DIV_MOD_PROOF_POSITIVE"))
        return false;

    emit_vref(out, v->args[0]);
    fprintf(out, ", ");
    emit_vref(out, v->args[1]);
    fprintf(out, ")");
    return true;
}

/* XI_DIV / XI_MOD on statically-unsigned integer operands are unsigned. The i64
 * value slot has no signedness tag, so the signed division below would divide
 * u64/usize top-bit-set values as negative. Mirrors the VM emitter's
 * xi_emit_divmod_uses_unsigned (operand-based: both int-like, either unsigned)
 * so VM and AOT select the same signedness. */
static bool cg_divmod_uses_unsigned(const XiValue *v) {
    if (!v || (v->op != XI_DIV && v->op != XI_MOD) || v->nargs < 2)
        return false;
    const XrType *left = v->args[0] ? v->args[0]->type : NULL;
    const XrType *right = v->args[1] ? v->args[1]->type : NULL;
    bool left_int = left && left->kind == XR_KIND_INT;
    bool right_int = right && right->kind == XR_KIND_INT;
    return left_int && right_int &&
           (cg_type_is_unsigned_int(left) || cg_type_is_unsigned_int(right));
}

/* Emit unsigned div/mod via xrt_uint_div / xrt_uint_mod (same divide-by-zero
 * throw as the signed path; clang folds the check and constant divisors after
 * inlining). Must be tried before the signed const / positive-divisor paths,
 * which select the signed owner kinds. */
static bool emit_native_unsigned_div_mod_expr(FILE *out, const XiValue *v) {
    if (!v || v->nargs < 2 || cg_rep(v) != XR_REP_I64 || cg_rep(v->args[0]) != XR_REP_I64 ||
        cg_rep(v->args[1]) != XR_REP_I64)
        return false;
    if (!cg_divmod_uses_unsigned(v))
        return false;
    fprintf(out, "%s(", v->op == XI_DIV ? "xrt_uint_div" : "xrt_uint_mod");
    emit_vref(out, v->args[0]);
    fprintf(out, ", ");
    emit_vref(out, v->args[1]);
    fprintf(out, ")");
    return true;
}

static bool cg_div_mod_is_trusted_nothrow(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    if (!v || (v->op != XI_DIV && v->op != XI_MOD) || v->nargs < 2 ||
        cg_value_plan_storage_rep(ctx, v) != XR_REP_I64 ||
        cg_value_plan_storage_rep(ctx, v->args[0]) != XR_REP_I64)
        return false;

    int64_t divisor = 0;
    if (cg_const_int_value_in_func(ctx, f, v->args[1], &divisor))
        return divisor != 0;

    if (cg_value_plan_storage_rep(ctx, v->args[1]) != XR_REP_I64)
        return false;

    return xi_value_known_positive_at(f, v->args[1], v->block);
}

static bool emit_native_unsigned_wrap_arith_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    if (!ctx || !out || !v || v->nargs < 2 || cg_value_plan_storage_rep(ctx, v) != XR_REP_I64 ||
        cg_value_plan_storage_rep(ctx, v->args[0]) != XR_REP_I64 ||
        cg_value_plan_storage_rep(ctx, v->args[1]) != XR_REP_I64)
        return false;
    if (!cg_type_is_unsigned_int(v->type))
        return false;

    const char *ctype = NULL;
    switch (v->type->scalar_rep) {
        case XR_NATIVE_U8:
        case XR_NATIVE_U16:
        case XR_NATIVE_U32:
            ctype = "uint32_t";
            break;
        case XR_NATIVE_U64:
            ctype = "uint64_t";
            break;
        case XR_NATIVE_USIZE:
            ctype = "size_t";
            break;
        default:
            return false;
    }

    const char *op = NULL;
    switch (v->op) {
        case XI_ADD:
            op = "+";
            break;
        case XI_SUB:
            op = "-";
            break;
        case XI_MUL:
            op = "*";
            break;
        default:
            return false;
    }

    fprintf(out, "((%s)(", ctype);
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
    fprintf(out, ") %s (%s)(", op, ctype);
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
    fprintf(out, "))");
    return true;
}

static bool emit_native_i64_wrap_arith_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    if (!ctx || !out || !v || v->nargs < 2 || cg_value_plan_storage_rep(ctx, v) != XR_REP_I64 ||
        cg_value_plan_storage_rep(ctx, v->args[0]) != XR_REP_I64 ||
        cg_value_plan_storage_rep(ctx, v->args[1]) != XR_REP_I64)
        return false;

    const char *op = NULL;
    switch (v->op) {
        case XI_ADD:
            op = "+";
            break;
        case XI_SUB:
            op = "-";
            break;
        case XI_MUL:
            op = "*";
            break;
        default:
            return false;
    }

    fprintf(out, "(int64_t)((uint64_t)(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
    fprintf(out, ") %s (uint64_t)(", op);
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
    fprintf(out, "))");
    return true;
}

static bool emit_native_rawptr_arith_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    if (!out || !v || v->nargs < 2 || cg_value_plan_storage_rep(ctx, v) != XR_REP_RAWPTR)
        return false;
    if (v->op != XI_ADD && v->op != XI_SUB)
        return false;

    const XiValue *ptr = NULL;
    const XiValue *offset = NULL;
    bool subtract = v->op == XI_SUB;
    /* Exactly one operand is the raw pointer base; the other is an integer
     * offset. The offset may still be tagged (an imported const folds to a
     * tagged XrValue, or an `as int` cast produced a tagged int); unboxing it to
     * i64 below keeps this native address arithmetic instead of falling back to
     * the generic xrt_add, which cannot take a raw pointer as an XrValue
     * argument (that fallback emits illegal C). The offset rep is restricted to
     * i64/tagged so a managed pointer never masquerades as an offset. */
    XrRep r0 = cg_value_plan_storage_rep(ctx, v->args[0]);
    XrRep r1 = cg_value_plan_storage_rep(ctx, v->args[1]);
    if (r0 == XR_REP_RAWPTR && (r1 == XR_REP_I64 || r1 == XR_REP_TAGGED)) {
        ptr = v->args[0];
        offset = v->args[1];
    } else if (v->op == XI_ADD && r1 == XR_REP_RAWPTR &&
               (r0 == XR_REP_I64 || r0 == XR_REP_TAGGED)) {
        ptr = v->args[1];
        offset = v->args[0];
    } else {
        return false;
    }

    bool is_mutable = ptr->type && ptr->type->kind == XR_KIND_POINTER && ptr->type->ptr_is_mut;
    /* Xi operands are materialized SSA values. The runtime-neutral helper
     * therefore preserves single evaluation, carries the unsafe non-null proof,
     * and keeps both generated dialects inside standard C. */
    fprintf(out, "%s(", is_mutable ? "xr_raw_mut_ptr_offset" : "xr_raw_const_ptr_offset");
    emit_value_as_rep_ctx(ctx, out, ptr, XR_REP_RAWPTR);
    fprintf(out, ", (intptr_t)(");
    emit_value_as_rep_ctx(ctx, out, offset, XR_REP_I64);
    fprintf(out, "), %d)", subtract ? 1 : 0);
    return true;
}
