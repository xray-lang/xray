/*
 * xi_cgen_dispatch_helpers.inc.c - Generated Xi lowering driver helpers for AOT C
 */

typedef struct XicgenBigIntLiteral {
    int sign;
    uint32_t len;
    uint32_t cap;
    uint32_t *limbs;
} XicgenBigIntLiteral;

static void xicgen_bigint_literal_free(XicgenBigIntLiteral *lit) {
    if (!lit)
        return;
    xr_free(lit->limbs);
    lit->limbs = NULL;
    lit->len = 0;
    lit->cap = 0;
}

static int xicgen_bigint_digit_value(char c, uint32_t base) {
    int digit = -1;
    if (c >= '0' && c <= '9')
        digit = c - '0';
    else if (c >= 'a' && c <= 'f')
        digit = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
        digit = c - 'A' + 10;
    return digit >= 0 && (uint32_t) digit < base ? digit : -1;
}

static void xicgen_bigint_literal_normalize(XicgenBigIntLiteral *lit) {
    if (!lit || !lit->limbs)
        return;
    while (lit->len > 1 && lit->limbs[lit->len - 1] == 0)
        lit->len--;
    if (lit->len == 0) {
        lit->len = 1;
        lit->limbs[0] = 0;
    }
    if (lit->len == 1 && lit->limbs[0] == 0)
        lit->sign = 1;
}

static bool xicgen_bigint_literal_parse(const char *digits, bool negate, XicgenBigIntLiteral *out) {
    if (!digits || !*digits || !out)
        return false;
    memset(out, 0, sizeof(*out));

    int sign = 1;
    if (*digits == '-') {
        sign = -1;
        digits++;
    } else if (*digits == '+') {
        digits++;
    }
    if (negate)
        sign = -sign;

    uint32_t base = 10;
    if (digits[0] == '0' && digits[1] != '\0') {
        if (digits[1] == 'x' || digits[1] == 'X') {
            base = 16;
            digits += 2;
        } else if (digits[1] == 'b' || digits[1] == 'B') {
            base = 2;
            digits += 2;
        } else if (digits[1] == 'o' || digits[1] == 'O') {
            base = 8;
            digits += 2;
        }
    }
    if (!*digits)
        return false;

    size_t ndigits = strlen(digits);
    if (ndigits > (size_t) UINT32_MAX - 1u)
        return false;
    out->cap = (uint32_t) ndigits + 1u;
    out->limbs = (uint32_t *) xr_calloc(out->cap, sizeof(uint32_t));
    if (!out->limbs)
        return false;
    out->sign = sign;
    out->len = 1;

    for (size_t i = 0; i < ndigits; i++) {
        int digit = xicgen_bigint_digit_value(digits[i], base);
        if (digit < 0) {
            xicgen_bigint_literal_free(out);
            return false;
        }
        uint64_t carry = (uint64_t) digit;
        for (uint32_t j = 0; j < out->len; j++) {
            uint64_t prod = (uint64_t) out->limbs[j] * (uint64_t) base + carry;
            out->limbs[j] = (uint32_t) prod;
            carry = prod >> 32;
        }
        if (carry != 0) {
            if (out->len >= out->cap) {
                xicgen_bigint_literal_free(out);
                return false;
            }
            out->limbs[out->len++] = (uint32_t) carry;
        }
    }

    xicgen_bigint_literal_normalize(out);
    return true;
}

static void xicgen_emit_bigint_literal_value(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                             bool negate) {
    XicgenBigIntLiteral lit;
    if (!v || !v->aux || !xicgen_bigint_literal_parse((const char *) v->aux, negate, &lit)) {
        if (ctx)
            ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: invalid BigInt literal at line %u\n",
                (unsigned) (v ? v->line : 0));
        emit_codegen_abort_expr(out);
        return;
    }

    fprintf(out,
            "({ static const struct { XrObjHeader hdr; void *klass; int8_t sign; "
            "uint8_t _pad1[3]; uint32_t len; uint32_t cap; uint32_t _pad2; "
            "uint32_t limbs[%uu]; } _xr_bigint_lit_%u = {",
            (unsigned) lit.len, (unsigned) v->id);
    fprintf(out,
            "{XR_TINSTANCE, XR_OBJ_STORAGE_BUMP, XR_RC_STICKY, 0u, 0u}, NULL, "
            "(int8_t)%d, {0}, %uu, %uu, 0u, {",
            lit.sign < 0 ? -1 : 1, (unsigned) lit.len, (unsigned) lit.len);
    for (uint32_t i = 0; i < lit.len; i++) {
        if (i > 0)
            fprintf(out, ", ");
        fprintf(out, "UINT32_C(%" PRIu32 ")", lit.limbs[i]);
    }
    fprintf(out, "}}; xr_mkptr((void *)&_xr_bigint_lit_%u, XR_TAG_BIGINT); })", (unsigned) v->id);
    xicgen_bigint_literal_free(&lit);
}

static void xicgen_const(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                         const char *prefix) {
    (void) f;
    (void) prefix;
    // A scalar const whose storage rep is TAGGED (e.g. an int/float/bool value
    // typed as a nullable primitive) must be boxed to match its XrValue C slot.
    bool boxed = cg_value_plan_storage_rep(ctx, v) == XR_REP_TAGGED;
    if (v->type->kind == XR_KIND_INT || xr_type_is_enum_metadata(v->type)) {
        if (boxed)
            fprintf(out, "XR_FROM_INT(");
        if (v->aux_int == INT64_MIN)
            fprintf(out, "INT64_MIN");
        else
            fprintf(out, "INT64_C(%" PRId64 ")", v->aux_int);
        if (boxed)
            fprintf(out, ")");
    } else if (v->type->kind == XR_KIND_POINTER) {
        if (boxed) {
            fprintf(out, "XR_FROM_INT(");
            if (v->aux_int == INT64_MIN)
                fprintf(out, "INT64_MIN");
            else
                fprintf(out, "INT64_C(%" PRId64 ")", v->aux_int);
            fprintf(out, ")");
        } else {
            fprintf(out, "(void *)(uintptr_t)");
            if (v->aux_int == INT64_MIN)
                fprintf(out, "INT64_MIN");
            else
                fprintf(out, "INT64_C(%" PRId64 ")", v->aux_int);
        }
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
    } else if (v->type->kind == XR_KIND_RUNE) {
        fprintf(out, "XR_FROM_RUNE((uint32_t)0x%X)", (unsigned) (uint32_t) v->aux_int);
    } else if (v->type->kind == XR_KIND_NULL)
        fprintf(out, "XR_NULL_VAL");
    else if (v->type->kind == XR_KIND_STRING) {
        cg_emit_str_value(ctx, out, (const char *) v->aux);
    } else if (xr_type_is_named_class(v->type, "BigInt") && v->aux) {
        xicgen_emit_bigint_literal_value(ctx, out, v, false);
    } else if (v->aux_kind == XI_AUX_KIND_ENUM_NAMESPACE && v->aux) {
        const XiEnumData *ed = (const XiEnumData *) v->aux;
        if (ctx && ctx->freestanding_profile && ed)
            fprintf(out, "XR_NULL_VAL");
        else
            emit_enum_type_expr(ctx, out, ed);
    } else if (v->type->kind == XR_KIND_UNKNOWN && v->aux) {
        const XiEnumData *ed = cg_enum_for_runtime_type(ctx, v->aux);
        if (ctx && ctx->freestanding_profile && ed)
            fprintf(out, "XR_NULL_VAL");
        else
            emit_enum_type_expr(ctx, out, ed);
    } else {
        fprintf(out, "XR_NULL_VAL /* unknown const kind */");
    }
}

static const char *xicgen_native_layout_c_type(uint8_t native_type) {
    switch (native_type) {
        case XR_NATIVE_ISIZE:
            return "ptrdiff_t";
        case XR_NATIVE_USIZE:
            return "size_t";
        default:
            return xaot_c_type_for_native_type(native_type);
    }
}

static void xicgen_target_layout_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v, const char *op) {
    const char *c_type = xicgen_native_layout_c_type((uint8_t) (v ? v->aux_int : 0));
    bool boxed = cg_value_plan_storage_rep(ctx, v) == XR_REP_TAGGED;
    if (!c_type)
        c_type = "int64_t";
    if (boxed)
        fprintf(out, "XR_FROM_INT(");
    fprintf(out, "(int64_t)%s(%s)", op, c_type);
    if (boxed)
        fprintf(out, ")");
}

static void xicgen_target_sizeof(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                 const char *prefix) {
    (void) f;
    (void) prefix;
    xicgen_target_layout_expr(ctx, out, v, "sizeof");
}

static void xicgen_target_alignof(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                  const char *prefix) {
    (void) f;
    (void) prefix;
    xicgen_target_layout_expr(ctx, out, v, "_Alignof");
}

static void xicgen_target_simd_bytes(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                     const char *prefix) {
    (void) f;
    (void) prefix;
    int bytes =
        ctx && ctx->target && (ctx->target->simd_features & XAOT_SIMD_FEATURE_AVX2) != 0 ? 32 : 16;
    if (cg_value_plan_storage_rep(ctx, v) == XR_REP_TAGGED)
        fprintf(out, "XR_FROM_INT(%d)", bytes);
    else
        fprintf(out, "%d", bytes);
}

static bool xicgen_const_literal_is_freestanding_scalar(const XiConstLiteral *lit) {
    if (!lit)
        return false;
    switch (lit->kind) {
        case XI_CONST_LITERAL_INT:
        case XI_CONST_LITERAL_FLOAT:
        case XI_CONST_LITERAL_BOOL:
        case XI_CONST_LITERAL_CHAR:
        case XI_CONST_LITERAL_STRING:
        case XI_CONST_LITERAL_NULL:
            return true;
        default:
            return false;
    }
}

static bool xicgen_const_literal_is_freestanding_erased_slot(const XiConstLiteral *lit) {
    return xicgen_const_literal_is_freestanding_scalar(lit) ||
           (lit && lit->kind == XI_CONST_LITERAL_COMPTIME_AGGREGATE);
}

static const XiConstLiteral *xicgen_freestanding_const_slot_literal(XiCgenCtx *ctx, int64_t slot) {
    if (!ctx || !ctx->freestanding_profile || !ctx->module || !ctx->module->slot_const_literals ||
        slot < 0 || slot >= ctx->module->nslots)
        return NULL;
    const XiConstLiteral *lit = &ctx->module->slot_const_literals[slot];
    return xicgen_const_literal_is_freestanding_scalar(lit) &&
                   !cg_const_literal_is_static_scalar_object(lit)
               ? lit
               : NULL;
}

static const XiConstLiteral *xicgen_freestanding_erased_const_slot(XiCgenCtx *ctx, int64_t slot) {
    if (!ctx || !ctx->freestanding_profile || !ctx->module || !ctx->module->slot_const_literals ||
        slot < 0 || slot >= ctx->module->nslots)
        return NULL;
    const XiConstLiteral *lit = &ctx->module->slot_const_literals[slot];
    return xicgen_const_literal_is_freestanding_erased_slot(lit) ? lit : NULL;
}

static XrRep xicgen_const_literal_source_rep(const XiConstLiteral *lit) {
    if (!lit)
        return XR_REP_TAGGED;
    switch (lit->kind) {
        case XI_CONST_LITERAL_FLOAT:
            return XR_REP_F64;
        case XI_CONST_LITERAL_INT:
        case XI_CONST_LITERAL_BOOL:
        case XI_CONST_LITERAL_CHAR:
            return XR_REP_I64;
        case XI_CONST_LITERAL_NULL:
        default:
            return XR_REP_TAGGED;
    }
}

static void xicgen_emit_i64_literal(FILE *out, int64_t value) {
    if (value == INT64_MIN)
        fprintf(out, "INT64_MIN");
    else
        fprintf(out, "INT64_C(%" PRId64 ")", value);
}

static void xicgen_emit_const_literal_raw(FILE *out, const XiConstLiteral *lit) {
    switch (lit ? lit->kind : XI_CONST_LITERAL_NONE) {
        case XI_CONST_LITERAL_INT:
            xicgen_emit_i64_literal(out, lit->int_value);
            return;
        case XI_CONST_LITERAL_FLOAT:
            emit_c_float_literal(out, lit->float_value);
            return;
        case XI_CONST_LITERAL_BOOL:
            fprintf(out, "INT64_C(%d)", lit->bool_value ? 1 : 0);
            return;
        case XI_CONST_LITERAL_CHAR:
            fprintf(out, "INT64_C(%" PRId64 ")", lit->int_value);
            return;
        case XI_CONST_LITERAL_NULL:
            fprintf(out, "XR_NULL_VAL");
            return;
        default:
            fprintf(out, "XR_NULL_VAL");
            return;
    }
}

static void xicgen_emit_const_slot_literal_as_value(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                                    const XiConstLiteral *lit) {
    if (lit && lit->kind == XI_CONST_LITERAL_STRING) {
        XrRep to_rep = cg_value_plan_storage_rep(ctx, v);
        const XrType *type = lit->type ? lit->type : (v ? v->type : NULL);
        const char *suffix = emit_conversion_prefix(out, type, XR_REP_TAGGED, to_rep);
        cg_emit_str_value(ctx, out, lit->string_value ? lit->string_value : "");
        emit_conversion_suffix(out, suffix);
        return;
    }
    XrRep from_rep = xicgen_const_literal_source_rep(lit);
    XrRep to_rep = cg_value_plan_storage_rep(ctx, v);
    const XrType *type = lit && lit->type ? lit->type : (v ? v->type : NULL);
    const char *suffix = emit_conversion_prefix(out, type, from_rep, to_rep);
    xicgen_emit_const_literal_raw(out, lit);
    emit_conversion_suffix(out, suffix);
}

static bool xicgen_same_rep_identity_alias(XiCgenCtx *ctx, const XiValue *v, const XiValue *arg) {
    if (!ctx || !ctx->aot_bundle || !v || !arg)
        return false;
    const XaotValuePlan *value_plan = xaot_bundle_find_value_plan(ctx->aot_bundle, v);
    const XaotValuePlan *arg_plan = xaot_bundle_find_value_plan(ctx->aot_bundle, arg);
    return value_plan && arg_plan && xaot_value_reps_equal(value_plan->rep, arg_plan->rep);
}

static const XiValue *xicgen_getprop_receiver_value(XiCgenCtx *ctx, const XiValue *v) {
    while (
        v &&
        (v->op == XI_UNBOX || xi_copy_is_identity_alias(v) || xi_op_is_identity_forward(v->op)) &&
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

static uint32_t xicgen_enum_name_expr_id(const XiValue *recv) {
    return recv && recv->id ? recv->id : 1u;
}

static bool xicgen_emit_freestanding_enum_layout_name_expr(XiCgenCtx *ctx, FILE *out,
                                                           const XiValue *recv,
                                                           const XrEnumLayout *layout,
                                                           XrRep result_rep) {
    if (!ctx || !out || !recv || !layout || !layout->variants || layout->variant_count == 0 ||
        !layout->is_zero_payload || cg_value_plan_storage_rep(ctx, recv) != XR_REP_I64)
        return false;
    for (uint32_t i = 0; i < layout->variant_count; i++) {
        if (layout->variants[i].payload_count != 0 || layout->variants[i].tag != i)
            return false;
    }

    uint32_t expr_id = xicgen_enum_name_expr_id(recv);
    char enum_buf[96];
    sanitize_c_ident_part(enum_buf, sizeof(enum_buf), layout->name ? layout->name : "Enum");
    result_rep = result_rep == XR_REP_PTR ? XR_REP_PTR : XR_REP_TAGGED;
    fprintf(out, "({ ");
    fprintf(out, "static const xrt_str_t *const _xenum_names_%s_%u[%u] = {", enum_buf,
            (unsigned) expr_id, (unsigned) layout->variant_count);
    for (uint32_t i = 0; i < layout->variant_count; i++) {
        if (i > 0)
            fprintf(out, ",");
        fprintf(out, "&_xstr_%d",
                cg_intern_str_lit(ctx, layout->variants[i].name ? layout->variants[i].name : ""));
    }
    fprintf(out, "}; ");
    fprintf(out, "int64_t _xenum_tag_%s_%u = ", enum_buf, (unsigned) expr_id);
    emit_value_as_rep_ctx(ctx, out, recv, XR_REP_I64);
    fprintf(out,
            "; const xrt_str_t *_xenum_name_%s_%u = "
            "(_xenum_tag_%s_%u >= 0 && (uint64_t)_xenum_tag_%s_%u < %u) ? "
            "_xenum_names_%s_%u[_xenum_tag_%s_%u] : _xenum_names_%s_%u[0]; ",
            enum_buf, (unsigned) expr_id, enum_buf, (unsigned) expr_id, enum_buf,
            (unsigned) expr_id, (unsigned) layout->variant_count, enum_buf, (unsigned) expr_id,
            enum_buf, (unsigned) expr_id, enum_buf, (unsigned) expr_id);
    if (result_rep == XR_REP_PTR)
        fprintf(out, "(void *)_xenum_name_%s_%u", enum_buf, (unsigned) expr_id);
    else
        fprintf(out, "xr_str_lit(_xenum_name_%s_%u)", enum_buf, (unsigned) expr_id);
    fprintf(out, "; })");
    return true;
}

static bool xicgen_emit_freestanding_enum_to_string_method(XiCgenCtx *ctx, FILE *out,
                                                           const XiValue *v, const char *method,
                                                           uint16_t nargs) {
    if (!ctx || !ctx->freestanding_profile || !v || !method || strcmp(method, "toString") != 0 ||
        nargs != 0 || v->nargs < 1 || !v->args[0] || !v->args[0]->type ||
        v->args[0]->type->kind != XR_KIND_ENUM)
        return false;
    if (xicgen_emit_freestanding_enum_layout_name_expr(ctx, out, v->args[0],
                                                       v->args[0]->type->enum_type.layout,
                                                       cg_value_plan_storage_rep(ctx, v)))
        return true;
    ctx->error = true;
    fprintf(stderr,
            "[xi_cgen] ERROR: freestanding enum.toString needs zero-payload enum layout at line "
            "%u\n",
            (unsigned) v->line);
    emit_codegen_abort_expr(out);
    return true;
}

static bool xicgen_emit_adt_field_load(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    if (!v || v->op != XI_LOAD_FIELD || v->nargs < 1 || v->aux || v->aux_int < 0)
        return false;
    if (!xi_load_field_is_adt(v) &&
        !xicgen_adt_enum_for_type(ctx, v->args[0] ? v->args[0]->type : NULL))
        return false;

    if (cg_value_plan_is_aggregate(ctx, v->args[0])) {
        const XaotValuePlan *recv_plan = cg_value_plan(ctx, v->args[0]);
        bool typed_enum = recv_plan && cg_value_rep_is_typed_adt_aggregate(recv_plan->rep);
        XrRep from_rep = v->aux_int == 0 ? XR_REP_I64 : XR_REP_TAGGED;
        const char *conv_suffix =
            emit_conversion_prefix(out, v->type, from_rep, cg_value_plan_storage_rep(ctx, v));
        if (v->aux_int == 0) {
            fprintf(out, "(");
            emit_vref(out, v->args[0]);
            fprintf(out, ").tag");
        } else if (v->aux_int > 0 && (uint64_t) v->aux_int <= 16u) {
            fprintf(out, "(");
            emit_vref(out, v->args[0]);
            if (typed_enum)
                fprintf(out, ").payload.raw[%" PRId64 "]", v->aux_int - 1);
            else
                fprintf(out, ").payloads[%" PRId64 "]", v->aux_int - 1);
        } else {
            fprintf(out, "XR_NULL_VAL");
        }
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }

    const char *conv_suffix = emit_tagged_to_value_storage_prefix(ctx, out, v);
    fprintf(out, "xrt_enum_field_get(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", %" PRId64 ")", v->aux_int);
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static XrRep xicgen_value_c_storage_rep(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v);
static void xicgen_vec_error(XiCgenCtx *ctx, FILE *out, const XiValue *value, const char *detail);

static void xicgen_param(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                         const char *prefix) {
    (void) prefix;
    uint16_t param_idx = (uint16_t) v->aux_int;
    XrRep from_rep = cg_func_param_abi_rep(ctx, f, param_idx);
    XrRep to_rep = cg_value_plan_storage_rep(ctx, v);
    const char *conv_suffix = emit_conversion_prefix_ctx(ctx, out, v->type, from_rep, to_rep);
    fprintf(out, "p%u", (unsigned) v->aux_int);
    emit_conversion_suffix(out, conv_suffix);
}

static void xicgen_identity(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                            const char *prefix) {
    (void) prefix;
    XR_DCHECK(v->nargs >= 1, "xicgen_identity: need arg");
    /* XI_COPY/XI_OWNER_FORWARD are value-level identities, but the rep planner may give
     * the result a different declared rep than its source (e.g. a native-local
     * PTR array moved into a TAGGED-declared local). Bridge that gap so the
     * emitted initializer matches the result's declared C type; when the reps
     * already agree this is a no-op and emits the bare source reference. */
    const XaotValuePlan *from_plan = cg_value_plan(ctx, v->args[0]);
    const XaotValuePlan *to_plan = cg_value_plan(ctx, v);
    if ((from_plan && from_plan->rep.kind == XAOT_VALUE_VECTOR) ||
        (to_plan && to_plan->rep.kind == XAOT_VALUE_VECTOR)) {
        if (!from_plan || !to_plan || !xaot_value_reps_equal(from_plan->rep, to_plan->rep)) {
            xicgen_vec_error(ctx, out, v, "vector identity has mismatched representation plan");
            return;
        }
        emit_vref(out, v->args[0]);
        return;
    }
    XrRep from_rep = xicgen_value_c_storage_rep(ctx, f, v->args[0]);
    XrRep to_rep = xicgen_value_c_storage_rep(ctx, f, v);
    const char *conv_suffix = emit_conversion_prefix_ctx(ctx, out, v->type, from_rep, to_rep);
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
    if (cg_value_plan_is_vector(ctx, v) || cg_value_plan_is_vector(ctx, v->args[0])) {
        xicgen_identity(ctx, out, f, v, prefix);
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
    const char *conv_suffix = emit_conversion_prefix_ctx(ctx, out, v->type, XR_REP_TAGGED, to_rep);
    fprintf(out, "xrt_value_clone_for_coro(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ")");
    emit_conversion_suffix(out, conv_suffix);
}

static void xicgen_move(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                        const char *prefix) {
    xicgen_identity(ctx, out, f, v, prefix);
}

static bool xicgen_imported_int_const_value(XiCgenCtx *ctx, const XiFunc *f, const XiValue *value,
                                            int64_t *out) {
    const XiValue *source = cg_unwrap_identity_value(value);
    const XiConstLiteral *lit = NULL;
    if (source && source->op == XI_IMPORT_REF) {
        lit =
            cg_import_ref_target_const_literal(ctx, (const XiImportRef *) source->aux, NULL, NULL);
    } else if (source && source->op == XI_GET_SHARED) {
        lit = cg_import_slot_const_literal(ctx, f, (int) source->aux_int, NULL, NULL);
    }
    if (!lit || lit->kind != XI_CONST_LITERAL_INT || cg_const_literal_is_static_scalar_object(lit))
        return false;
    if (out)
        *out = lit->int_value;
    return true;
}

static void xicgen_arith(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                         const char *prefix) {
    (void) prefix;
    XrRep result_rep = cg_rep(v);
    XrRep a_rep = cg_rep(v->args[0]);
    XrRep b_rep = cg_rep(v->args[1]);
    bool any_tagged = (a_rep == XR_REP_TAGGED || b_rep == XR_REP_TAGGED);
    bool a_imported_i64 = xicgen_imported_int_const_value(ctx, f, v->args[0], NULL);
    bool b_imported_i64 = xicgen_imported_int_const_value(ctx, f, v->args[1], NULL);
    bool imported_i64_arith = result_rep == XR_REP_I64 && v->type && v->type->kind == XR_KIND_INT &&
                              (a_imported_i64 || b_imported_i64) &&
                              (a_rep == XR_REP_I64 || a_imported_i64) &&
                              (b_rep == XR_REP_I64 || b_imported_i64);
    if (imported_i64_arith) {
        /* Representation selection can precede cross-module const resolution,
         * leaving an immutable imported integer marked TAGGED.  The bundle's
         * canonical literal is sufficient proof to emit the same wrapping
         * native operation as two ordinary I64 operands. */
        if (cg_type_is_unsigned_int(v->type)) {
            const char *ctype = cg_native_int_ctype(v->type->scalar_rep);
            if (!ctype)
                ctype = "uint64_t";
            fprintf(out, "(%s)((%s)(", ctype, ctype);
            emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
            fprintf(out, ") %s (%s)(", xi_to_c_template_arith_native_op(v->op), ctype);
            emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
            fprintf(out, "))");
        } else {
            fprintf(out, "(int64_t)((uint64_t)(");
            emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
            fprintf(out, ") %s (uint64_t)(", xi_to_c_template_arith_native_op(v->op));
            emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
            fprintf(out, "))");
        }
    } else if (result_rep != XR_REP_RAWPTR && (result_rep == XR_REP_TAGGED || any_tagged)) {
        /* A raw-pointer result is always native address arithmetic (ptr ± int)
         * and must fall through to the RAWPTR branch even when the integer
         * offset is still tagged; xrt_add cannot take a raw pointer as an
         * XrValue operand. The RAWPTR emitter unboxes a tagged offset to i64. */
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

    /* Statically-unsigned integer div/mod must be UNSIGNED (matches VM
     * OP_DIV_U/OP_MOD_U and both constant folders). The signed xrt_int_div /
     * xrt_int_mod fallback below would treat u64/usize top-bit-set payloads as
     * negative. uint64_t covers every unsigned width — narrower payloads are
     * zero-extended in the i64 value model. When the divisor is provably
     * nonzero, inline the native division; otherwise use the throwing helper.
     * emit_value_as_rep_ctx normalizes raw-i64 and boxed operands alike. */
    if (result_rep == XR_REP_I64 && cg_type_is_unsigned_int(v->type)) {
        bool boxed = cg_value_plan_storage_rep(ctx, v) == XR_REP_TAGGED;
        if (boxed)
            fprintf(out, "XR_FROM_INT(");
        if (cg_div_mod_is_trusted_nothrow(f, v)) {
            fprintf(out, "(int64_t)((uint64_t)(");
            emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
            fprintf(out, ") %s (uint64_t)(", v->op == XI_DIV ? "/" : "%");
            emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
            fprintf(out, "))");
        } else {
            fprintf(out, "%s(", v->op == XI_DIV ? "xrt_uint_div" : "xrt_uint_mod");
            emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
            fprintf(out, ", ");
            emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
            fprintf(out, ")");
        }
        if (boxed)
            fprintf(out, ")");
        return;
    }

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
        if (!emit_native_unsigned_div_mod_expr(out, v) && !emit_native_const_div_mod_expr(out, v) &&
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
    (void) f;
    (void) prefix;
    if (v && xr_type_is_named_class(v->type, "BigInt")) {
        const XiValue *arg = v->nargs > 0 ? v->args[0] : NULL;
        if (arg && arg->op == XI_CONST && xr_type_is_named_class(arg->type, "BigInt") && arg->aux) {
            xicgen_emit_bigint_literal_value(ctx, out, arg, true);
            return;
        }
        if (ctx)
            ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: AOT BigInt unary negation requires allocation at line %u\n",
                (unsigned) (v ? v->line : 0));
        emit_codegen_abort_expr(out);
        return;
    }
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

static const char *cg_exact_bit_unsigned_ctype(uint8_t native_type) {
    switch (native_type) {
        case XR_NATIVE_I8:
        case XR_NATIVE_U8:
            return "uint8_t";
        case XR_NATIVE_I16:
        case XR_NATIVE_U16:
            return "uint16_t";
        case XR_NATIVE_I32:
        case XR_NATIVE_U32:
            return "uint32_t";
        case XR_NATIVE_ISIZE:
        case XR_NATIVE_USIZE:
            return "uintptr_t";
        case XR_NATIVE_I64:
        case XR_NATIVE_U64:
        default:
            return "uint64_t";
    }
}

static const char *cg_exact_bit_width_expr(uint8_t native_type) {
    switch (native_type) {
        case XR_NATIVE_I8:
        case XR_NATIVE_U8:
            return "8u";
        case XR_NATIVE_I16:
        case XR_NATIVE_U16:
            return "16u";
        case XR_NATIVE_I32:
        case XR_NATIVE_U32:
            return "32u";
        case XR_NATIVE_ISIZE:
        case XR_NATIVE_USIZE:
            return "(sizeof(uintptr_t) * 8u)";
        case XR_NATIVE_I64:
        case XR_NATIVE_U64:
        default:
            return "64u";
    }
}

static void cg_emit_exact_bit_pattern(XiCgenCtx *ctx, FILE *out, const XiValue *value,
                                      uint8_t native_type) {
    fprintf(out, "((%s) (", cg_exact_bit_unsigned_ctype(native_type));
    emit_value_as_rep_ctx(ctx, out, value, XR_REP_I64);
    fprintf(out, "))");
}

static void xicgen_exact_bit(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                             const char *prefix) {
    (void) f;
    (void) prefix;
    if (!v || v->nargs < 1 || !v->args[0]) {
        if (ctx)
            ctx->error = true;
        emit_codegen_abort_expr(out);
        return;
    }

    uint8_t native_type = (uint8_t) v->aux_int;
    const char *width = cg_exact_bit_width_expr(native_type);
    const char *uctype = cg_exact_bit_unsigned_ctype(native_type);
    const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));

    if (v->op == XI_BIT_POPCOUNT) {
        fprintf(out, "((int64_t) __builtin_popcountll((unsigned long long) ");
        cg_emit_exact_bit_pattern(ctx, out, v->args[0], native_type);
        fprintf(out, "))");
    } else if (v->op == XI_BIT_CLZ || v->op == XI_BIT_CTZ) {
        fprintf(out, "(");
        cg_emit_exact_bit_pattern(ctx, out, v->args[0], native_type);
        fprintf(out, " == 0 ? (int64_t) %s : (int64_t) ", width);
        if (v->op == XI_BIT_CLZ) {
            fprintf(out, "(__builtin_clzll((unsigned long long) ");
            cg_emit_exact_bit_pattern(ctx, out, v->args[0], native_type);
            fprintf(out, ") - (64u - %s)))", width);
        } else {
            fprintf(out, "__builtin_ctzll((unsigned long long) ");
            cg_emit_exact_bit_pattern(ctx, out, v->args[0], native_type);
            fprintf(out, "))");
        }
    } else if (v->op == XI_BIT_BSWAP) {
        const char *result_ctype = cg_native_int_ctype(native_type);
        if (!result_ctype)
            result_ctype = "int64_t";
        fprintf(out, "((int64_t) ((%s) (", result_ctype);
        if (native_type == XR_NATIVE_I8 || native_type == XR_NATIVE_U8) {
            cg_emit_exact_bit_pattern(ctx, out, v->args[0], native_type);
        } else if (native_type == XR_NATIVE_I16 || native_type == XR_NATIVE_U16) {
            fprintf(out, "__builtin_bswap16((uint16_t) ");
            cg_emit_exact_bit_pattern(ctx, out, v->args[0], native_type);
            fprintf(out, ")");
        } else if (native_type == XR_NATIVE_I32 || native_type == XR_NATIVE_U32) {
            fprintf(out, "__builtin_bswap32((uint32_t) ");
            cg_emit_exact_bit_pattern(ctx, out, v->args[0], native_type);
            fprintf(out, ")");
        } else if (native_type == XR_NATIVE_ISIZE || native_type == XR_NATIVE_USIZE) {
            fprintf(out, "(sizeof(uintptr_t) == 8 ? (uintptr_t) __builtin_bswap64((uint64_t) ");
            cg_emit_exact_bit_pattern(ctx, out, v->args[0], native_type);
            fprintf(out, ") : (uintptr_t) __builtin_bswap32((uint32_t) ");
            cg_emit_exact_bit_pattern(ctx, out, v->args[0], native_type);
            fprintf(out, "))");
        } else {
            fprintf(out, "__builtin_bswap64((uint64_t) ");
            cg_emit_exact_bit_pattern(ctx, out, v->args[0], native_type);
            fprintf(out, ")");
        }
        fprintf(out, ")))");
    } else if (v->op == XI_BIT_MUL_HIGH && v->nargs == 2) {
        const char *result_ctype = cg_native_int_ctype(native_type);
        if (!result_ctype)
            result_ctype = "int64_t";
        fprintf(out, "((int64_t) ((%s) (", result_ctype);
        if (native_type == XR_NATIVE_U8) {
            fprintf(out, "(((uint16_t) (uint8_t) ");
            emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
            fprintf(out, " * (uint16_t) (uint8_t) ");
            emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
            fprintf(out, ") >> 8)");
        } else if (native_type == XR_NATIVE_U16) {
            fprintf(out, "(((uint32_t) (uint16_t) ");
            emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
            fprintf(out, " * (uint32_t) (uint16_t) ");
            emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
            fprintf(out, ") >> 16)");
        } else if (native_type == XR_NATIVE_U32) {
            fprintf(out, "(((uint64_t) (uint32_t) ");
            emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
            fprintf(out, " * (uint64_t) (uint32_t) ");
            emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
            fprintf(out, ") >> 32)");
        } else if (native_type == XR_NATIVE_USIZE) {
            fprintf(out, "(sizeof(uintptr_t) == 8 ? xr_u64_mul_high((uint64_t) (uintptr_t) ");
            emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
            fprintf(out, ", (uint64_t) (uintptr_t) ");
            emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
            fprintf(out, ") : (((uint64_t) (uint32_t) ");
            emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
            fprintf(out, " * (uint64_t) (uint32_t) ");
            emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
            fprintf(out, ") >> 32))");
        } else {
            fprintf(out, "xr_u64_mul_high((uint64_t) ");
            emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
            fprintf(out, ", (uint64_t) ");
            emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
            fprintf(out, ")");
        }
        fprintf(out, ")))");
    } else if ((v->op == XI_BIT_ROTL || v->op == XI_BIT_ROTR) && v->nargs == 2) {
        const char *result_ctype = cg_native_int_ctype(native_type);
        if (!result_ctype)
            result_ctype = "int64_t";
        const char *direction = v->op == XI_BIT_ROTL ? "ROTL" : "ROTR";
        fprintf(out, "((int64_t) ((%s) (", result_ctype);
        if (native_type == XR_NATIVE_ISIZE || native_type == XR_NATIVE_USIZE) {
            fprintf(out, "sizeof(uintptr_t) == 8 ? (uintptr_t) XR_BITS_%s64((uint64_t) ",
                    direction);
            cg_emit_exact_bit_pattern(ctx, out, v->args[0], native_type);
            fprintf(out, ", ");
            emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
            fprintf(out, ") : (uintptr_t) XR_BITS_%s32((uint32_t) ", direction);
            cg_emit_exact_bit_pattern(ctx, out, v->args[0], native_type);
            fprintf(out, ", ");
            emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
            fprintf(out, ")");
        } else {
            const char *bits = native_type == XR_NATIVE_I8 || native_type == XR_NATIVE_U8     ? "8"
                               : native_type == XR_NATIVE_I16 || native_type == XR_NATIVE_U16 ? "16"
                               : native_type == XR_NATIVE_I32 || native_type == XR_NATIVE_U32
                                   ? "32"
                                   : "64";
            fprintf(out, "XR_BITS_%s%s((%s) ", direction, bits, uctype);
            cg_emit_exact_bit_pattern(ctx, out, v->args[0], native_type);
            fprintf(out, ", ");
            emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
            fprintf(out, ")");
        }
        fprintf(out, ")))");
    } else {
        if (ctx)
            ctx->error = true;
        emit_codegen_abort_expr(out);
    }
    emit_conversion_suffix(out, conv_suffix);
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

static const char *xicgen_vec_lane_ctype(uint8_t native_type) {
    switch (native_type) {
        case XR_NATIVE_U8:
            return "uint8_t";
        case XR_NATIVE_U32:
            return "uint32_t";
        case XR_NATIVE_U64:
            return "uint64_t";
        default:
            return NULL;
    }
}

static const XiValue *xicgen_vec_unwrap_value(const XiValue *value) {
    for (unsigned depth = 0; value && depth < 8; depth++) {
        if ((value->op == XI_COPY || xi_op_is_identity_forward(value->op)) && value->nargs == 1 &&
            value->args[0]) {
            value = value->args[0];
            continue;
        }
        break;
    }
    return value;
}

static void xicgen_emit_vec_lanes(FILE *out, const XiValue *value) {
    value = xicgen_vec_unwrap_value(value);
    fprintf(out, "(");
    emit_vref(out, value);
    fprintf(out, ")._lanes");
}

static bool xicgen_vec_result_aggregate(XiCgenCtx *ctx, const XiValue *value,
                                        const char **ctype_out) {
    const XaotValuePlan *plan = cg_value_plan(ctx, value);
    if (!plan || plan->rep.kind != XAOT_VALUE_AGGREGATE || !plan->rep.c_type)
        return false;
    if (ctype_out)
        *ctype_out = plan->rep.c_type;
    return true;
}

static bool xicgen_vec_result_native(XiCgenCtx *ctx, const XiValue *value, const char **ctype_out) {
    const XaotValuePlan *plan = cg_value_plan(ctx, value);
    if (!plan || plan->rep.kind != XAOT_VALUE_VECTOR || !plan->rep.c_type)
        return false;
    if (ctype_out)
        *ctype_out = plan->rep.c_type;
    return true;
}

static void xicgen_vec_error(XiCgenCtx *ctx, FILE *out, const XiValue *value, const char *detail) {
    if (ctx)
        ctx->error = true;
    fprintf(stderr, "[xi_cgen] ERROR: invalid typed vector op %s v%u: %s\n",
            value ? xi_op_name(value->op) : "?", value ? (unsigned) value->id : 0,
            detail ? detail : "invalid shape");
    emit_codegen_abort_expr(out);
}

static void xicgen_emit_vec_range_check(FILE *out, const char *index_name, uint8_t lanes) {
    fprintf(out, "if (XR_UNLIKELY(%s < 0 || %s >= %uu)) xrt_fixed_index_oob(%s, %uu); ", index_name,
            index_name, (unsigned) lanes, index_name, (unsigned) lanes);
}

static void xicgen_emit_vec_native_load(XiCgenCtx *ctx, FILE *out, const XiValue *value,
                                        uint8_t native_type, bool neon, bool wide) {
    value = xicgen_vec_unwrap_value(value);
    const XaotValuePlan *plan = cg_value_plan(ctx, value);
    if (plan && plan->rep.kind == XAOT_VALUE_VECTOR) {
        emit_vref(out, value);
        return;
    }
    if (neon)
        fprintf(out, "%s((const %s *)",
                native_type == XR_NATIVE_U8    ? "vld1q_u8"
                : native_type == XR_NATIVE_U32 ? "vld1q_u32"
                                               : "vld1q_u64",
                xicgen_vec_lane_ctype(native_type));
    else if (wide)
        fprintf(out, "_mm256_loadu_si256((const __m256i *)(const void *)");
    else
        fprintf(out, "_mm_loadu_si128((const __m128i *)(const void *)");
    xicgen_emit_vec_lanes(out, value);
    fprintf(out, ")");
}

static void xicgen_emit_vec_native_store(FILE *out, const char *destination, uint8_t native_type,
                                         bool neon, bool wide, const char *value_name) {
    if (neon)
        fprintf(out, "%s((%s *)(%s), %s)",
                native_type == XR_NATIVE_U8    ? "vst1q_u8"
                : native_type == XR_NATIVE_U32 ? "vst1q_u32"
                                               : "vst1q_u64",
                xicgen_vec_lane_ctype(native_type), destination, value_name);
    else if (wide)
        fprintf(out, "_mm256_storeu_si256((__m256i *)(void *)(%s), %s)", destination, value_name);
    else
        fprintf(out, "_mm_storeu_si128((__m128i *)(void *)(%s), %s)", destination, value_name);
}

static const char *xicgen_vec_neon_type(uint8_t native_type) {
    return native_type == XR_NATIVE_U8    ? "uint8x16_t"
           : native_type == XR_NATIVE_U32 ? "uint32x4_t"
                                          : "uint64x2_t";
}

static uint8_t xicgen_vec_value_native_type(XiCgenCtx *ctx, const XiValue *value,
                                            uint8_t fallback) {
    value = xicgen_vec_unwrap_value(value);
    const XaotValuePlan *plan = cg_value_plan(ctx, value);
    if (plan && plan->rep.kind == XAOT_VALUE_VECTOR)
        return plan->rep.vector_native_type;
    if (value && xi_vec_shape_is_explicit(value->aux_int))
        return xi_vec_shape_native_type(value->aux_int);
    return fallback;
}

static const char *xicgen_vec_neon_reinterpret_name(uint8_t to, uint8_t from) {
    if (to == from)
        return NULL;
    if (to == XR_NATIVE_U8)
        return from == XR_NATIVE_U32 ? "vreinterpretq_u8_u32" : "vreinterpretq_u8_u64";
    if (to == XR_NATIVE_U32)
        return from == XR_NATIVE_U8 ? "vreinterpretq_u32_u8" : "vreinterpretq_u32_u64";
    if (to == XR_NATIVE_U64)
        return from == XR_NATIVE_U8 ? "vreinterpretq_u64_u8" : "vreinterpretq_u64_u32";
    return NULL;
}

static unsigned xicgen_vec_shuffle_lane(const XiValue *value, uint8_t lane) {
    return value ? (unsigned) ((value->aux_int >>
                                (XI_VEC_SHAPE_SHUFFLE_SHIFT + (unsigned) lane * 4u)) &
                               0xf)
                 : 0u;
}

static bool xicgen_vec_shuffle_is_u32_swap_adjacent(const XiValue *value, uint8_t lanes,
                                                    uint8_t native_type) {
    return native_type == XR_NATIVE_U32 && lanes == 4 && xicgen_vec_shuffle_lane(value, 0) == 1 &&
           xicgen_vec_shuffle_lane(value, 1) == 0 && xicgen_vec_shuffle_lane(value, 2) == 3 &&
           xicgen_vec_shuffle_lane(value, 3) == 2;
}

static bool xicgen_vec_shuffle_is_u64_swap(const XiValue *value, uint8_t lanes,
                                           uint8_t native_type) {
    return native_type == XR_NATIVE_U64 && lanes == 2 && xicgen_vec_shuffle_lane(value, 0) == 1 &&
           xicgen_vec_shuffle_lane(value, 1) == 0;
}

/* Match the target-independent dataflow
 *
 *     widenMulEven(x, x.swapAdjacent())
 *
 * produced by adjacent 32-bit partial products.  Targets can select both
 * halves directly from x; materializing the swap first is redundant.  The
 * match is expressed solely in Xi op/shape identity and therefore applies to
 * any portable SIMD user, never to a source function or module name. */
static bool xicgen_vec_widen_mul_is_adjacent_pair(const XiValue *value) {
    if (!value || value->op != XI_VEC_WIDEN_MUL || value->nargs != 2 ||
        (value->aux_int & XI_VEC_SHAPE_ODD_LANES) != 0)
        return false;
    const XiValue *lhs = xicgen_vec_unwrap_value(value->args[0]);
    const XiValue *rhs = xicgen_vec_unwrap_value(value->args[1]);
    if (!lhs || !rhs || rhs->op != XI_VEC_SHUFFLE || rhs->nargs != 1 ||
        xicgen_vec_unwrap_value(rhs->args[0]) != lhs || !xi_vec_shape_is_explicit(rhs->aux_int) ||
        xi_vec_shape_native_type(rhs->aux_int) != XR_NATIVE_U32)
        return false;
    uint8_t lanes = xi_vec_shape_lanes(rhs->aux_int);
    if (lanes != 4 && lanes != 8)
        return false;
    for (uint8_t lane = 0; lane < lanes; lane++) {
        if (xicgen_vec_shuffle_lane(rhs, lane) != (uint8_t) (lane ^ 1u))
            return false;
    }
    return true;
}

static bool xicgen_vec_native_binary_supported(XiOp op, uint8_t native_type, bool neon, bool avx2) {
    if (op == XI_VEC_BIT_AND || op == XI_VEC_BIT_OR || op == XI_VEC_BIT_XOR)
        return true;
    if (op == XI_VEC_ADD || op == XI_VEC_SUB)
        return true;
    if (op != XI_VEC_MUL)
        return false;
    if (native_type == XR_NATIVE_U64)
        return false;
    return neon || (native_type == XR_NATIVE_U32 && avx2);
}

static const char *xicgen_vec_neon_binary_name(XiOp op, uint8_t native_type) {
#define XICGEN_NEON_BY_LANE(u8_name, u32_name, u64_name)                                           \
    (native_type == XR_NATIVE_U8    ? (u8_name)                                                    \
     : native_type == XR_NATIVE_U32 ? (u32_name)                                                   \
                                    : (u64_name))
    switch (op) {
        case XI_VEC_ADD:
            return XICGEN_NEON_BY_LANE("vaddq_u8", "vaddq_u32", "vaddq_u64");
        case XI_VEC_SUB:
            return XICGEN_NEON_BY_LANE("vsubq_u8", "vsubq_u32", "vsubq_u64");
        case XI_VEC_MUL:
            return XICGEN_NEON_BY_LANE("vmulq_u8", "vmulq_u32", "vmulq_u64");
        case XI_VEC_BIT_AND:
            return XICGEN_NEON_BY_LANE("vandq_u8", "vandq_u32", "vandq_u64");
        case XI_VEC_BIT_OR:
            return XICGEN_NEON_BY_LANE("vorrq_u8", "vorrq_u32", "vorrq_u64");
        default:
            return XICGEN_NEON_BY_LANE("veorq_u8", "veorq_u32", "veorq_u64");
    }
#undef XICGEN_NEON_BY_LANE
}

static const char *xicgen_vec_x86_binary_name(XiOp op, uint8_t native_type, bool wide) {
    if (op == XI_VEC_BIT_AND)
        return wide ? "_mm256_and_si256" : "_mm_and_si128";
    if (op == XI_VEC_BIT_OR)
        return wide ? "_mm256_or_si256" : "_mm_or_si128";
    if (op == XI_VEC_BIT_XOR)
        return wide ? "_mm256_xor_si256" : "_mm_xor_si128";
    if (op == XI_VEC_MUL)
        return wide ? "_mm256_mullo_epi32" : "_mm_mullo_epi32";
    if (op == XI_VEC_ADD)
        return native_type == XR_NATIVE_U8    ? (wide ? "_mm256_add_epi8" : "_mm_add_epi8")
               : native_type == XR_NATIVE_U32 ? (wide ? "_mm256_add_epi32" : "_mm_add_epi32")
                                              : (wide ? "_mm256_add_epi64" : "_mm_add_epi64");
    return native_type == XR_NATIVE_U8    ? (wide ? "_mm256_sub_epi8" : "_mm_sub_epi8")
           : native_type == XR_NATIVE_U32 ? (wide ? "_mm256_sub_epi32" : "_mm_sub_epi32")
                                          : (wide ? "_mm256_sub_epi64" : "_mm_sub_epi64");
}

static const XiValue *xicgen_vec_proven_window(XiCgenCtx *ctx, const XiValue *v, uint8_t kind) {
    const XiValue *receiver;
    const XiValue *window;
    if (!v || !cg_span_plan_drops(ctx, v, kind, XAOT_SLICE_DROP_BOUNDS))
        return NULL;
    receiver = kind == XAOT_SLICE_ACCESS_VEC_STORE ? (v->nargs >= 2 ? v->args[1] : NULL)
                                                   : (v->nargs >= 1 ? v->args[0] : NULL);
    window = cg_unwrap_identity_value(receiver);
    return window && window->op == XI_SLICE_WINDOW && window->nargs == 3 ? window : NULL;
}

static void xicgen_emit_vec_window_offset(XiCgenCtx *ctx, FILE *out, const XiValue *window,
                                          const XiValue *relative) {
    fprintf(out, "(");
    emit_value_as_rep_ctx(ctx, out, window->args[1], XR_REP_I64);
    fprintf(out, ") + (");
    emit_value_as_rep_ctx(ctx, out, relative, XR_REP_I64);
    fprintf(out, ")");
}

/* Emit a target-selected 128-bit implementation.  Returning false is an
 * intentional per-operation scalar fallback, not target probing: the exact
 * feature set was resolved into XaotTarget before Xi C generation. */
static bool xicgen_emit_vec_native(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                   const char *prefix, uint8_t lanes, uint8_t native_type) {
    (void) f;
    (void) prefix;
    uint32_t features = ctx && ctx->simd_active && ctx->target ? ctx->target->simd_features : 0;
    bool neon = (features & XAOT_SIMD_FEATURE_NEON) != 0;
    bool x86 = (features & XAOT_SIMD_FEATURE_SSE2) != 0;
    bool avx2 = (features & XAOT_SIMD_FEATURE_AVX2) != 0;
    if (!neon && !x86)
        return false;

    unsigned lane_bytes = native_type == XR_NATIVE_U8 ? 1u : native_type == XR_NATIVE_U32 ? 4u : 8u;
    unsigned vector_bytes = (unsigned) lanes * lane_bytes;
    bool wide = vector_bytes == 32u;
    if ((wide && (!x86 || !avx2)) || (!wide && vector_bytes != 16u))
        return false;

    const char *result_type = NULL;
    const char *lane_type = xicgen_vec_lane_ctype(native_type);
    const char *vec_type = neon ? xicgen_vec_neon_type(native_type) : wide ? "__m256i" : "__m128i";
    bool native_result = xicgen_vec_result_native(ctx, v, &result_type);
    switch ((XiOp) v->op) {
        case XI_VEC_LOAD:
            if (v->nargs != 2 && !native_result)
                return false;
            if (v->nargs != 2 ||
                (!native_result && !xicgen_vec_result_aggregate(ctx, v, &result_type)))
                return false;
            fprintf(out, "({ ");
            if (!native_result)
                fprintf(out, "%s _r; ", result_type);
            const XiValue *proven_load_window =
                xicgen_vec_proven_window(ctx, v, XAOT_SLICE_ACCESS_VEC_LOAD);
            fprintf(out, "xr_span_t _s = ");
            emit_vref(out, xicgen_vec_unwrap_value(proven_load_window ? proven_load_window->args[0]
                                                                      : v->args[0]));
            fprintf(out, "; int64_t _off = ");
            if (proven_load_window)
                xicgen_emit_vec_window_offset(ctx, out, proven_load_window, v->args[1]);
            else
                emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
            if (!proven_load_window)
                fprintf(out,
                        "; if (XR_UNLIKELY(_off < 0 || _off > _s.length - %uu)) "
                        "xrt_index_oob(_off < 0 ? _off : _off + %uu, _s.length); "
                        "XR_ASSUME(_off >= 0 && _off <= _s.length - %uu)",
                        (unsigned) lanes, (unsigned) (lanes - 1), (unsigned) lanes);
            fprintf(out, "; %s _v = ", vec_type);
            if (neon)
                fprintf(out, "%s(((const %s *)_s.data) + _off)",
                        native_type == XR_NATIVE_U8    ? "vld1q_u8"
                        : native_type == XR_NATIVE_U32 ? "vld1q_u32"
                                                       : "vld1q_u64",
                        lane_type);
            else
                fprintf(out,
                        wide ? "_mm256_loadu_si256((const __m256i *)(const void *)(((const "
                               "%s *)_s.data) + _off))"
                             : "_mm_loadu_si128((const __m128i *)(const void *)(((const %s "
                               "*)_s.data) + _off))",
                        lane_type);
            if (native_result) {
                fprintf(out, "; _v; })");
            } else {
                fprintf(out, "; ");
                xicgen_emit_vec_native_store(out, "_r._lanes", native_type, neon, wide, "_v");
                fprintf(out, "; _r; })");
            }
            return true;

        case XI_VEC_STORE:
            if (v->nargs != 3)
                return false;
            const XiValue *proven_store_window =
                xicgen_vec_proven_window(ctx, v, XAOT_SLICE_ACCESS_VEC_STORE);
            fprintf(out, "({ xr_span_t _s = ");
            emit_vref(out, xicgen_vec_unwrap_value(
                               proven_store_window ? proven_store_window->args[0] : v->args[1]));
            fprintf(out, "; int64_t _off = ");
            if (proven_store_window)
                xicgen_emit_vec_window_offset(ctx, out, proven_store_window, v->args[2]);
            else
                emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_I64);
            if (!proven_store_window)
                fprintf(out,
                        "; if (XR_UNLIKELY(_off < 0 || _off > _s.length - %uu)) "
                        "xrt_index_oob(_off < 0 ? _off : _off + %uu, _s.length); "
                        "XR_ASSUME(_off >= 0 && _off <= _s.length - %uu)",
                        (unsigned) lanes, (unsigned) (lanes - 1), (unsigned) lanes);
            fprintf(out, "; %s _v = ", vec_type);
            xicgen_emit_vec_native_load(ctx, out, v->args[0], native_type, neon, wide);
            fprintf(out, "; ");
            if (neon)
                fprintf(out, "%s(((%s *)_s.data) + _off, _v)",
                        native_type == XR_NATIVE_U8    ? "vst1q_u8"
                        : native_type == XR_NATIVE_U32 ? "vst1q_u32"
                                                       : "vst1q_u64",
                        lane_type);
            else
                fprintf(out,
                        wide ? "_mm256_storeu_si256((__m256i *)(void *)(((%s *)_s.data) + "
                               "_off), _v)"
                             : "_mm_storeu_si128((__m128i *)(void *)(((%s *)_s.data) + "
                               "_off), _v)",
                        lane_type);
            fprintf(out, "; XR_NULL_VAL; })");
            return true;

        case XI_VEC_EXTRACT:
            if (v->nargs != 2)
                return false;
            fprintf(out, "({ %s _lanes[%uu]; %s _v = ", lane_type, (unsigned) lanes, vec_type);
            xicgen_emit_vec_native_load(ctx, out, v->args[0], native_type, neon, wide);
            fprintf(out, "; ");
            xicgen_emit_vec_native_store(out, "_lanes", native_type, neon, wide, "_v");
            fprintf(out, "; int64_t _lane = ");
            emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
            fprintf(out, "; ");
            xicgen_emit_vec_range_check(out, "_lane", lanes);
            fprintf(out, "_lanes[_lane]; })");
            return true;

        case XI_VEC_REPLACE:
            if (v->nargs != 3 ||
                (!native_result && !xicgen_vec_result_aggregate(ctx, v, &result_type)))
                return false;
            fprintf(out, "({ ");
            if (!native_result)
                fprintf(out, "%s _r; ", result_type);
            fprintf(out, "%s _lanes[%uu]; %s _a = ", lane_type, (unsigned) lanes, vec_type);
            xicgen_emit_vec_native_load(ctx, out, v->args[0], native_type, neon, wide);
            fprintf(out, "; ");
            xicgen_emit_vec_native_store(out, "_lanes", native_type, neon, wide, "_a");
            fprintf(out, "; int64_t _lane = ");
            emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
            fprintf(out, "; ");
            xicgen_emit_vec_range_check(out, "_lane", lanes);
            fprintf(out, "_lanes[_lane] = (%s)(", lane_type);
            emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_I64);
            fprintf(out, "); %s _v = ", vec_type);
            if (neon)
                fprintf(out, "%s((const %s *)_lanes)",
                        native_type == XR_NATIVE_U8    ? "vld1q_u8"
                        : native_type == XR_NATIVE_U32 ? "vld1q_u32"
                                                       : "vld1q_u64",
                        lane_type);
            else if (wide)
                fprintf(out, "_mm256_loadu_si256((const __m256i *)(const void *)_lanes)");
            else
                fprintf(out, "_mm_loadu_si128((const __m128i *)(const void *)_lanes)");
            if (native_result) {
                fprintf(out, "; _v; })");
            } else {
                fprintf(out, "; ");
                xicgen_emit_vec_native_store(out, "_r._lanes", native_type, neon, wide, "_v");
                fprintf(out, "; _r; })");
            }
            return true;

        case XI_VEC_SPLAT:
            if (v->nargs != 1 ||
                (!native_result && !xicgen_vec_result_aggregate(ctx, v, &result_type)))
                return false;
            fprintf(out, "({ ");
            if (!native_result)
                fprintf(out, "%s _r; ", result_type);
            fprintf(out, "%s _x = (%s)(", lane_type, lane_type);
            emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
            fprintf(out, "); %s _v = ", vec_type);
            if (neon)
                fprintf(out, "%s(_x)",
                        native_type == XR_NATIVE_U8    ? "vdupq_n_u8"
                        : native_type == XR_NATIVE_U32 ? "vdupq_n_u32"
                                                       : "vdupq_n_u64");
            else
                fprintf(out, "%s(_x)",
                        native_type == XR_NATIVE_U8 ? (wide ? "_mm256_set1_epi8" : "_mm_set1_epi8")
                        : native_type == XR_NATIVE_U32
                            ? (wide ? "_mm256_set1_epi32" : "_mm_set1_epi32")
                            : (wide ? "_mm256_set1_epi64x" : "_mm_set1_epi64x"));
            if (native_result) {
                fprintf(out, "; _v; })");
            } else {
                fprintf(out, "; ");
                xicgen_emit_vec_native_store(out, "_r._lanes", native_type, neon, wide, "_v");
                fprintf(out, "; _r; })");
            }
            return true;

        case XI_VEC_ADD:
        case XI_VEC_SUB:
        case XI_VEC_MUL:
        case XI_VEC_BIT_AND:
        case XI_VEC_BIT_OR:
        case XI_VEC_BIT_XOR:
            if (v->nargs != 2 ||
                (!native_result && !xicgen_vec_result_aggregate(ctx, v, &result_type)) ||
                !xicgen_vec_native_binary_supported((XiOp) v->op, native_type, neon, avx2))
                return false;
            fprintf(out, "({ ");
            if (!native_result)
                fprintf(out, "%s _r; ", result_type);
            fprintf(out, "%s _a = ", vec_type);
            xicgen_emit_vec_native_load(ctx, out, v->args[0], native_type, neon, wide);
            fprintf(out, ", _b = ");
            xicgen_emit_vec_native_load(ctx, out, v->args[1], native_type, neon, wide);
            fprintf(out, "; %s _v = %s(_a, _b); ", vec_type,
                    neon ? xicgen_vec_neon_binary_name((XiOp) v->op, native_type)
                         : xicgen_vec_x86_binary_name((XiOp) v->op, native_type, wide));
            if (native_result) {
                fprintf(out, "_v; })");
            } else {
                xicgen_emit_vec_native_store(out, "_r._lanes", native_type, neon, wide, "_v");
                fprintf(out, "; _r; })");
            }
            return true;

        case XI_VEC_BIT_NOT:
            if (v->nargs != 1 ||
                (!native_result && !xicgen_vec_result_aggregate(ctx, v, &result_type)))
                return false;
            fprintf(out, "({ ");
            if (!native_result)
                fprintf(out, "%s _r; ", result_type);
            fprintf(out, "%s _a = ", vec_type);
            xicgen_emit_vec_native_load(ctx, out, v->args[0], native_type, neon, wide);
            if (neon && native_type == XR_NATIVE_U64)
                fprintf(out, "; uint64x2_t _v = "
                             "vreinterpretq_u64_u32(vmvnq_u32(vreinterpretq_u32_u64(_a))); ");
            else if (neon)
                fprintf(out, "; %s _v = %s(_a); ", vec_type,
                        native_type == XR_NATIVE_U8 ? "vmvnq_u8" : "vmvnq_u32");
            else
                fprintf(out, wide ? "; __m256i _v = _mm256_xor_si256(_a, _mm256_set1_epi32(-1)); "
                                  : "; __m128i _v = _mm_xor_si128(_a, _mm_set1_epi32(-1)); ");
            if (native_result) {
                fprintf(out, "_v; })");
            } else {
                xicgen_emit_vec_native_store(out, "_r._lanes", native_type, neon, wide, "_v");
                fprintf(out, "; _r; })");
            }
            return true;

        case XI_VEC_SHL:
        case XI_VEC_SHR:
            if (v->nargs != 2 || native_type == XR_NATIVE_U8 ||
                (!native_result && !xicgen_vec_result_aggregate(ctx, v, &result_type)))
                return false;
            fprintf(out, "({ ");
            if (!native_result)
                fprintf(out, "%s _r; ", result_type);
            fprintf(out, "%s _a = ", vec_type);
            xicgen_emit_vec_native_load(ctx, out, v->args[0], native_type, neon, wide);
            fprintf(out, "; uint32_t _s = (uint32_t)(");
            emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
            fprintf(out, ") & 63u; %s _v = ", vec_type);
            if (neon) {
                const char *count_type = native_type == XR_NATIVE_U32 ? "int32x4_t" : "int64x2_t";
                const char *dup = native_type == XR_NATIVE_U32 ? "vdupq_n_s32" : "vdupq_n_s64";
                const char *shift = native_type == XR_NATIVE_U32 ? "vshlq_u32" : "vshlq_u64";
                fprintf(out, "%s(_a, (%s)%s(%s(int64_t)_s))", shift, count_type, dup,
                        v->op == XI_VEC_SHL ? "" : "-");
            } else {
                const char *shift =
                    native_type == XR_NATIVE_U32
                        ? (v->op == XI_VEC_SHL ? (wide ? "_mm256_sll_epi32" : "_mm_sll_epi32")
                                               : (wide ? "_mm256_srl_epi32" : "_mm_srl_epi32"))
                        : (v->op == XI_VEC_SHL ? (wide ? "_mm256_sll_epi64" : "_mm_sll_epi64")
                                               : (wide ? "_mm256_srl_epi64" : "_mm_srl_epi64"));
                fprintf(out, "%s(_a, _mm_cvtsi64_si128((int64_t)_s))", shift);
            }
            if (native_result) {
                fprintf(out, "; _v; })");
            } else {
                fprintf(out, "; ");
                xicgen_emit_vec_native_store(out, "_r._lanes", native_type, neon, wide, "_v");
                fprintf(out, "; _r; })");
            }
            return true;

        case XI_VEC_REINTERPRET:
            if (v->nargs != 1 ||
                (!native_result && !xicgen_vec_result_aggregate(ctx, v, &result_type)))
                return false;
            fprintf(out, "({ ");
            if (!native_result)
                fprintf(out, "%s _r; ", result_type);
            uint8_t input_native_type = xicgen_vec_value_native_type(ctx, v->args[0], native_type);
            if (neon) {
                const char *input_type = xicgen_vec_neon_type(input_native_type);
                const char *reinterpret_name =
                    xicgen_vec_neon_reinterpret_name(native_type, input_native_type);
                fprintf(out, "%s _a = ", input_type);
                xicgen_emit_vec_native_load(ctx, out, v->args[0], input_native_type, true, false);
                fprintf(out, "; %s _v = ", vec_type);
                if (reinterpret_name)
                    fprintf(out, "%s(_a)", reinterpret_name);
                else
                    fprintf(out, "_a");
            } else {
                fprintf(out, "%s _v = ", vec_type);
                xicgen_emit_vec_native_load(ctx, out, v->args[0], input_native_type, false, wide);
            }
            if (native_result) {
                fprintf(out, "; _v; })");
            } else {
                fprintf(out, "; ");
                xicgen_emit_vec_native_store(out, "_r._lanes", native_type, neon, wide, "_v");
                fprintf(out, "; _r; })");
            }
            return true;

        case XI_VEC_SHUFFLE:
            if ((v->aux_int & XI_VEC_SHAPE_UNZIP) != 0) {
                if ((!neon && !x86) || v->nargs != 2 || native_type != XR_NATIVE_U32 ||
                    lanes != 4 ||
                    (!native_result && !xicgen_vec_result_aggregate(ctx, v, &result_type)))
                    return false;
                fprintf(out, "({ ");
                if (!native_result)
                    fprintf(out, "%s _r; ", result_type);
                if (neon) {
                    fprintf(out, "uint32x4_t _a = ");
                    xicgen_emit_vec_native_load(ctx, out, v->args[0], XR_NATIVE_U32, true, false);
                    fprintf(out, ", _b = ");
                    xicgen_emit_vec_native_load(ctx, out, v->args[1], XR_NATIVE_U32, true, false);
                    fprintf(out, "; uint32x4_t _v = %s(_a, _b); ",
                            (v->aux_int & XI_VEC_SHAPE_ODD_LANES) != 0 ? "vuzp2q_u32"
                                                                       : "vuzp1q_u32");
                } else {
                    fprintf(out, "__m128i _a = ");
                    xicgen_emit_vec_native_load(ctx, out, v->args[0], XR_NATIVE_U32, false, false);
                    fprintf(out, ", _b = ");
                    xicgen_emit_vec_native_load(ctx, out, v->args[1], XR_NATIVE_U32, false, false);
                    fprintf(out,
                            "; _a = _mm_shuffle_epi32(_a, %uu); "
                            "_b = _mm_shuffle_epi32(_b, %uu); "
                            "__m128i _v = _mm_unpacklo_epi64(_a, _b); ",
                            (v->aux_int & XI_VEC_SHAPE_ODD_LANES) != 0 ? 221u : 136u,
                            (v->aux_int & XI_VEC_SHAPE_ODD_LANES) != 0 ? 221u : 136u);
                }
                if (native_result) {
                    fprintf(out, "_v; })");
                } else {
                    xicgen_emit_vec_native_store(out, "_r._lanes", native_type, neon, false, "_v");
                    fprintf(out, "; _r; })");
                }
                return true;
            }
            if (v->nargs != 1 ||
                (!native_result && !xicgen_vec_result_aggregate(ctx, v, &result_type)))
                return false;
            if ((!x86 && !neon) || (native_type == XR_NATIVE_U8 && wide))
                return false;
            if (x86 && native_type == XR_NATIVE_U8 && !avx2)
                return false;
            {
                fprintf(out, "({ ");
                if (!native_result)
                    fprintf(out, "%s _r; ", result_type);
                if (neon) {
                    const char *to_bytes =
                        xicgen_vec_neon_reinterpret_name(XR_NATIVE_U8, native_type);
                    const char *from_bytes =
                        xicgen_vec_neon_reinterpret_name(native_type, XR_NATIVE_U8);
                    fprintf(out, "%s _a = ", vec_type);
                    xicgen_emit_vec_native_load(ctx, out, v->args[0], native_type, true, false);
                    if (xicgen_vec_shuffle_is_u32_swap_adjacent(v, lanes, native_type)) {
                        fprintf(out, "; uint32x4_t _v = vrev64q_u32(_a)");
                    } else if (xicgen_vec_shuffle_is_u64_swap(v, lanes, native_type)) {
                        fprintf(out, "; uint64x2_t _v = vextq_u64(_a, _a, 1)");
                    } else {
                        fprintf(out, "; uint8x16_t _bytes = ");
                        if (to_bytes)
                            fprintf(out, "%s(_a)", to_bytes);
                        else
                            fprintf(out, "_a");
                        fprintf(out, "; const uint8_t _m[16] = {");
                        for (uint8_t lane = 0; lane < lanes; lane++) {
                            unsigned src = xicgen_vec_shuffle_lane(v, lane);
                            for (unsigned byte = 0; byte < lane_bytes; byte++)
                                fprintf(out, "%s%uu", lane || byte ? ", " : "",
                                        src * lane_bytes + byte);
                        }
                        fprintf(out,
                                "}; uint8x16_t _shuffled = "
                                "vqtbl1q_u8(_bytes, vld1q_u8(_m)); %s _v = ",
                                vec_type);
                        if (from_bytes)
                            fprintf(out, "%s(_shuffled)", from_bytes);
                        else
                            fprintf(out, "_shuffled");
                    }
                } else {
                    unsigned imm = 0;
                    if (native_type == XR_NATIVE_U32 && !wide) {
                        for (uint8_t lane = 0; lane < 4; lane++)
                            imm |= (unsigned) ((v->aux_int >>
                                                (XI_VEC_SHAPE_SHUFFLE_SHIFT + lane * 4)) &
                                               3)
                                   << (lane * 2);
                    } else if (native_type == XR_NATIVE_U64 && !wide) {
                        for (uint8_t lane = 0; lane < 2; lane++) {
                            unsigned src = (unsigned) ((v->aux_int >>
                                                        (XI_VEC_SHAPE_SHUFFLE_SHIFT + lane * 4)) &
                                                       1);
                            imm |= (src * 2u) << (lane * 4);
                            imm |= (src * 2u + 1u) << (lane * 4 + 2);
                        }
                    } else if (!avx2) {
                        return false;
                    }
                    fprintf(out, "%s _a = ", wide ? "__m256i" : "__m128i");
                    xicgen_emit_vec_native_load(ctx, out, v->args[0], native_type, false, wide);
                    if (native_type == XR_NATIVE_U8) {
                        fprintf(out, "; const uint8_t _m[16] = {");
                        for (uint8_t lane = 0; lane < 16; lane++) {
                            unsigned src = (unsigned) ((v->aux_int >>
                                                        (XI_VEC_SHAPE_SHUFFLE_SHIFT + lane * 4)) &
                                                       15);
                            fprintf(out, "%s%uu", lane ? ", " : "", src);
                        }
                        fprintf(out, "}; __m128i _v = _mm_shuffle_epi8(_a, "
                                     "_mm_loadu_si128((const __m128i *)(const void *)_m)); ");
                    } else if (native_type == XR_NATIVE_U32 && wide) {
                        unsigned wide_imm = 0;
                        for (uint8_t lane = 0; lane < 4; lane++)
                            wide_imm |= (unsigned) ((v->aux_int >>
                                                     (XI_VEC_SHAPE_SHUFFLE_SHIFT + lane * 4)) &
                                                    3)
                                        << (lane * 2);
                        fprintf(out, "; __m256i _v = _mm256_shuffle_epi32(_a, %uu); ", wide_imm);
                    } else if (native_type == XR_NATIVE_U64 && wide) {
                        unsigned wide_imm = 0;
                        for (uint8_t lane = 0; lane < 4; lane++)
                            wide_imm |= (unsigned) ((v->aux_int >>
                                                     (XI_VEC_SHAPE_SHUFFLE_SHIFT + lane * 4)) &
                                                    3)
                                        << (lane * 2);
                        fprintf(out, "; __m256i _v = _mm256_permute4x64_epi64(_a, %uu); ",
                                wide_imm);
                    } else {
                        fprintf(out, "; __m128i _v = _mm_shuffle_epi32(_a, %uu); ", imm);
                    }
                }
                if (native_result) {
                    fprintf(out, "; _v; })");
                } else {
                    fprintf(out, "; ");
                    xicgen_emit_vec_native_store(out, "_r._lanes", native_type, neon, wide, "_v");
                    fprintf(out, "; _r; })");
                }
                return true;
            }

        case XI_VEC_WIDEN_MUL:
            if (v->nargs != 2 || (lanes != 2 && lanes != 4) || native_type != XR_NATIVE_U64 ||
                (!native_result && !xicgen_vec_result_aggregate(ctx, v, &result_type)))
                return false;
            fprintf(out, "({ ");
            if (!native_result)
                fprintf(out, "%s _r; ", result_type);
            if (neon) {
                fprintf(out, "uint32x4_t _a = ");
                xicgen_emit_vec_native_load(ctx, out, v->args[0], XR_NATIVE_U32, true, false);
                if ((v->aux_int & XI_VEC_SHAPE_CONTIGUOUS_HALF) != 0) {
                    fprintf(out, ", _b = ");
                    xicgen_emit_vec_native_load(ctx, out, v->args[1], XR_NATIVE_U32, true, false);
                    if ((v->aux_int & XI_VEC_SHAPE_ODD_LANES) != 0)
                        fprintf(out, "; uint64x2_t _v = vmull_high_u32(_a, _b); ");
                    else
                        fprintf(out, "; uint64x2_t _v = vmull_u32(vget_low_u32(_a), "
                                     "vget_low_u32(_b)); ");
                } else if (xicgen_vec_widen_mul_is_adjacent_pair(v)) {
                    fprintf(out, "; uint32x2_t _al = vget_low_u32(vuzp1q_u32(_a, _a)), _bl = "
                                 "vget_low_u32(vuzp2q_u32(_a, _a)); uint64x2_t _v = "
                                 "vmull_u32(_al, _bl); ");
                } else {
                    fprintf(out, ", _b = ");
                    xicgen_emit_vec_native_load(ctx, out, v->args[1], XR_NATIVE_U32, true, false);
                    fprintf(out,
                            "; uint32x2_t _al = vget_low_u32(%s(_a, _a)), _bl = "
                            "vget_low_u32(%s(_b, _b)); uint64x2_t _v = vmull_u32(_al, _bl); ",
                            (v->aux_int & XI_VEC_SHAPE_ODD_LANES) ? "vuzp2q_u32" : "vuzp1q_u32",
                            (v->aux_int & XI_VEC_SHAPE_ODD_LANES) ? "vuzp2q_u32" : "vuzp1q_u32");
                }
            } else {
                fprintf(out, "%s _a = ", wide ? "__m256i" : "__m128i");
                xicgen_emit_vec_native_load(ctx, out, v->args[0], XR_NATIVE_U32, false, wide);
                bool adjacent_pair = xicgen_vec_widen_mul_is_adjacent_pair(v);
                bool contiguous_half = (v->aux_int & XI_VEC_SHAPE_CONTIGUOUS_HALF) != 0;
                if (adjacent_pair) {
                    fprintf(out, wide ? "; __m256i _b = _mm256_srli_epi64(_a, 32)"
                                      : "; __m128i _b = _mm_srli_epi64(_a, 32)");
                } else {
                    fprintf(out, ", _b = ");
                    xicgen_emit_vec_native_load(ctx, out, v->args[1], XR_NATIVE_U32, false, wide);
                }
                if (contiguous_half) {
                    const char *unpack = (v->aux_int & XI_VEC_SHAPE_ODD_LANES) != 0
                                             ? "_mm_unpackhi_epi32"
                                             : "_mm_unpacklo_epi32";
                    fprintf(out,
                            "; __m128i _z = _mm_setzero_si128(); "
                            "_a = %s(_a, _z); _b = %s(_b, _z)",
                            unpack, unpack);
                } else if (!adjacent_pair && (v->aux_int & XI_VEC_SHAPE_ODD_LANES) != 0)
                    fprintf(out, wide ? "; _a = _mm256_srli_epi64(_a, 32); _b = "
                                        "_mm256_srli_epi64(_b, 32)"
                                      : "; _a = _mm_srli_epi64(_a, 32); _b = "
                                        "_mm_srli_epi64(_b, 32)");
                fprintf(out, wide ? "; __m256i _v = _mm256_mul_epu32(_a, _b); "
                                  : "; __m128i _v = _mm_mul_epu32(_a, _b); ");
            }
            if (native_result) {
                fprintf(out, "; _v; })");
            } else {
                fprintf(out, "; ");
                xicgen_emit_vec_native_store(out, "_r._lanes", XR_NATIVE_U64, neon, wide, "_v");
                fprintf(out, "; _r; })");
            }
            return true;

        case XI_VEC_UNZIP:
            /* Two-input 32-bit deinterleave. NEON maps directly to uzp1/uzp2;
             * x86 falls back to the scalar lane expansion. */
            if (v->nargs != 2 || native_type != XR_NATIVE_U32 || lanes != 4 || !neon ||
                !xicgen_vec_result_aggregate(ctx, v, &result_type))
                return false;
            fprintf(out, "({ %s _r; uint32x4_t _a = vld1q_u32((const uint32_t *)", result_type);
            xicgen_emit_vec_lanes(out, v->args[0]);
            fprintf(out, "), _b = vld1q_u32((const uint32_t *)");
            xicgen_emit_vec_lanes(out, v->args[1]);
            fprintf(out, "); uint32x4_t _v = %s(_a, _b); ",
                    (v->aux_int & XI_VEC_SHAPE_ODD_LANES) ? "vuzp2q_u32" : "vuzp1q_u32");
            xicgen_emit_vec_native_store(out, "_r._lanes", XR_NATIVE_U32, true, false, "_v");
            fprintf(out, "; _r; })");
            return true;

        case XI_VEC_WIDEN_MUL_HALF:
            /* Widening multiply of a contiguous 32-bit half. NEON maps to
             * umull (low) / umull2 (high); x86 falls back to scalar. */
            if (v->nargs != 2 || native_type != XR_NATIVE_U64 || lanes != 2 || !neon ||
                !xicgen_vec_result_aggregate(ctx, v, &result_type))
                return false;
            fprintf(out, "({ %s _r; uint32x4_t _a = vld1q_u32((const uint32_t *)", result_type);
            xicgen_emit_vec_lanes(out, v->args[0]);
            fprintf(out, "), _b = vld1q_u32((const uint32_t *)");
            xicgen_emit_vec_lanes(out, v->args[1]);
            fprintf(out, "); uint64x2_t _v = %s; ",
                    (v->aux_int & XI_VEC_SHAPE_ODD_LANES)
                        ? "vmull_u32(vget_high_u32(_a), vget_high_u32(_b))"
                        : "vmull_u32(vget_low_u32(_a), vget_low_u32(_b))");
            xicgen_emit_vec_native_store(out, "_r._lanes", XR_NATIVE_U64, true, false, "_v");
            fprintf(out, "; _r; })");
            return true;

        case XI_VEC_REDUCE_ADD:
            if (v->nargs != 1 || native_type != XR_NATIVE_U64)
                return false;
            if (neon) {
                fprintf(out, "({ uint64x2_t _a = ");
                xicgen_emit_vec_native_load(ctx, out, v->args[0], native_type, true, false);
                fprintf(out, "; vaddvq_u64(_a); })");
            } else {
                fprintf(out, "({ %s _a = ", wide ? "__m256i" : "__m128i");
                xicgen_emit_vec_native_load(ctx, out, v->args[0], native_type, false, wide);
                if (wide)
                    fprintf(out, "; __m128i _lo = _mm256_castsi256_si128(_a), "
                                 "_hi = _mm256_extracti128_si256(_a, 1); _lo = "
                                 "_mm_add_epi64(_lo, _hi); _lo = _mm_add_epi64(_lo, "
                                 "_mm_srli_si128(_lo, 8)); (uint64_t)_mm_cvtsi128_si64(_lo); })");
                else
                    fprintf(out, "; _a = _mm_add_epi64(_a, _mm_srli_si128(_a, 8)); "
                                 "(uint64_t)_mm_cvtsi128_si64(_a); })");
            }
            return true;

        default:
            return false;
    }
}

static void xicgen_vec(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                       const char *prefix) {
    (void) prefix;
    if (!v || !xi_vec_shape_is_explicit(v->aux_int)) {
        xicgen_vec_error(ctx, out, v, "missing explicit lane shape");
        return;
    }
    const uint8_t lanes = xi_vec_shape_lanes(v->aux_int);
    const uint8_t native_type = xi_vec_shape_native_type(v->aux_int);
    const char *lane_type = xicgen_vec_lane_ctype(native_type);
    const char *result_type = NULL;
    if (!lane_type || lanes == 0 || lanes > 32) {
        xicgen_vec_error(ctx, out, v, "unsupported lane type/count");
        return;
    }
    if (v->op == XI_VEC_STORE &&
        cg_emit_span_readonly_void_trap(ctx, out, v, XAOT_SLICE_ACCESS_VEC_STORE))
        return;
    if (xicgen_emit_vec_native(ctx, out, f, v, prefix, lanes, native_type))
        return;

    switch ((XiOp) v->op) {
        case XI_VEC_LOAD:
            if (v->nargs != 2 || !xicgen_vec_result_aggregate(ctx, v, &result_type)) {
                xicgen_vec_error(ctx, out, v, "load needs span, offset and aggregate result");
                return;
            }
            const XiValue *proven_scalar_load_window =
                xicgen_vec_proven_window(ctx, v, XAOT_SLICE_ACCESS_VEC_LOAD);
            fprintf(out, "({ %s _r; xr_span_t _s = ", result_type);
            emit_vref(out, xicgen_vec_unwrap_value(proven_scalar_load_window
                                                       ? proven_scalar_load_window->args[0]
                                                       : v->args[0]));
            fprintf(out, "; int64_t _off = ");
            if (proven_scalar_load_window)
                xicgen_emit_vec_window_offset(ctx, out, proven_scalar_load_window, v->args[1]);
            else
                emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
            if (!proven_scalar_load_window)
                fprintf(out,
                        "; if (XR_UNLIKELY(_off < 0 || _off > _s.length - %uu)) "
                        "xrt_index_oob(_off < 0 ? _off : _off + %uu, _s.length); "
                        "XR_ASSUME(_off >= 0 && _off <= _s.length - %uu)",
                        (unsigned) lanes, (unsigned) (lanes - 1), (unsigned) lanes);
            fprintf(out,
                    "; memcpy(_r._lanes, ((const %s *)_s.data) + _off, sizeof(_r._lanes)); "
                    "_r; })",
                    lane_type);
            return;

        case XI_VEC_STORE:
            if (v->nargs != 3) {
                xicgen_vec_error(ctx, out, v, "store needs vector, span and offset");
                return;
            }
            const XiValue *proven_scalar_store_window =
                xicgen_vec_proven_window(ctx, v, XAOT_SLICE_ACCESS_VEC_STORE);
            fprintf(out, "({ xr_span_t _s = ");
            emit_vref(out, xicgen_vec_unwrap_value(proven_scalar_store_window
                                                       ? proven_scalar_store_window->args[0]
                                                       : v->args[1]));
            fprintf(out, "; int64_t _off = ");
            if (proven_scalar_store_window)
                xicgen_emit_vec_window_offset(ctx, out, proven_scalar_store_window, v->args[2]);
            else
                emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_I64);
            if (!proven_scalar_store_window)
                fprintf(out,
                        "; if (XR_UNLIKELY(_off < 0 || _off > _s.length - %uu)) "
                        "xrt_index_oob(_off < 0 ? _off : _off + %uu, _s.length); "
                        "XR_ASSUME(_off >= 0 && _off <= _s.length - %uu)",
                        (unsigned) lanes, (unsigned) (lanes - 1), (unsigned) lanes);
            fprintf(out, "; memcpy(((%s *)_s.data) + _off, ", lane_type);
            xicgen_emit_vec_lanes(out, v->args[0]);
            fprintf(out, ", %uu); XR_NULL_VAL; })",
                    (unsigned) (lanes * (native_type == XR_NATIVE_U8    ? 1
                                         : native_type == XR_NATIVE_U32 ? 4
                                                                        : 8)));
            return;

        case XI_VEC_SPLAT:
            if (v->nargs != 1 || !xicgen_vec_result_aggregate(ctx, v, &result_type)) {
                xicgen_vec_error(ctx, out, v, "splat needs scalar and aggregate result");
                return;
            }
            fprintf(out, "({ %s _r; %s _x = (%s)(", result_type, lane_type, lane_type);
            emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
            fprintf(out, "); for (uint32_t _i = 0; _i < %uu; _i++) _r._lanes[_i] = _x; _r; })",
                    (unsigned) lanes);
            return;

        case XI_VEC_EXTRACT:
            if (v->nargs != 2) {
                xicgen_vec_error(ctx, out, v, "extract needs vector and lane");
                return;
            }
            fprintf(out, "({ int64_t _lane = ");
            emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
            fprintf(out, "; ");
            xicgen_emit_vec_range_check(out, "_lane", lanes);
            fprintf(out, "(%s)", lane_type);
            xicgen_emit_vec_lanes(out, v->args[0]);
            fprintf(out, "[_lane]; })");
            return;

        case XI_VEC_REPLACE:
            if (v->nargs != 3 || !xicgen_vec_result_aggregate(ctx, v, &result_type)) {
                xicgen_vec_error(ctx, out, v,
                                 "replace needs vector, lane, scalar and aggregate result");
                return;
            }
            fprintf(out, "({ %s _r; memcpy(_r._lanes, ", result_type);
            xicgen_emit_vec_lanes(out, v->args[0]);
            fprintf(out, ", sizeof(_r._lanes)); int64_t _lane = ");
            emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
            fprintf(out, "; ");
            xicgen_emit_vec_range_check(out, "_lane", lanes);
            fprintf(out, "_r._lanes[_lane] = (%s)(", lane_type);
            emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_I64);
            fprintf(out, "); _r; })");
            return;

        case XI_VEC_ADD:
        case XI_VEC_SUB:
        case XI_VEC_MUL:
        case XI_VEC_BIT_AND:
        case XI_VEC_BIT_OR:
        case XI_VEC_BIT_XOR: {
            if (v->nargs != 2 || !xicgen_vec_result_aggregate(ctx, v, &result_type)) {
                xicgen_vec_error(ctx, out, v, "binary op needs two vectors and aggregate result");
                return;
            }
            const char *op = v->op == XI_VEC_ADD       ? "+"
                             : v->op == XI_VEC_SUB     ? "-"
                             : v->op == XI_VEC_MUL     ? "*"
                             : v->op == XI_VEC_BIT_AND ? "&"
                             : v->op == XI_VEC_BIT_OR  ? "|"
                                                       : "^";
            fprintf(out,
                    "({ %s _r; for (uint32_t _i = 0; _i < %uu; _i++) "
                    "_r._lanes[_i] = (%s)(",
                    result_type, (unsigned) lanes, lane_type);
            xicgen_emit_vec_lanes(out, v->args[0]);
            fprintf(out, "[_i] %s ", op);
            xicgen_emit_vec_lanes(out, v->args[1]);
            fprintf(out, "[_i]); _r; })");
            return;
        }

        case XI_VEC_BIT_NOT:
            if (v->nargs != 1 || !xicgen_vec_result_aggregate(ctx, v, &result_type)) {
                xicgen_vec_error(ctx, out, v, "bit-not needs vector and aggregate result");
                return;
            }
            fprintf(out,
                    "({ %s _r; for (uint32_t _i = 0; _i < %uu; _i++) "
                    "_r._lanes[_i] = (%s)~",
                    result_type, (unsigned) lanes, lane_type);
            xicgen_emit_vec_lanes(out, v->args[0]);
            fprintf(out, "[_i]; _r; })");
            return;

        case XI_VEC_SHL:
        case XI_VEC_SHR:
            if (v->nargs != 2 || !xicgen_vec_result_aggregate(ctx, v, &result_type)) {
                xicgen_vec_error(ctx, out, v, "shift needs vector, count and aggregate result");
                return;
            }
            fprintf(out, "({ %s _r; uint32_t _s = (uint32_t)(", result_type);
            emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
            fprintf(out,
                    ") & 63u; for (uint32_t _i = 0; _i < %uu; _i++) "
                    "_r._lanes[_i] = _s >= %uu ? (%s)0 : (%s)(",
                    (unsigned) lanes, native_type == XR_NATIVE_U32 ? 32u : 64u, lane_type,
                    lane_type);
            xicgen_emit_vec_lanes(out, v->args[0]);
            fprintf(out, "[_i] %s _s); _r; })", v->op == XI_VEC_SHL ? "<<" : ">>");
            return;

        case XI_VEC_REINTERPRET:
            if (v->nargs != 1 || !xicgen_vec_result_aggregate(ctx, v, &result_type)) {
                xicgen_vec_error(ctx, out, v, "reinterpret needs vector and aggregate result");
                return;
            }
            fprintf(out, "({ %s _r; memcpy(_r._lanes, ", result_type);
            xicgen_emit_vec_lanes(out, v->args[0]);
            fprintf(out, ", sizeof(_r._lanes)); _r; })");
            return;

        case XI_VEC_SHUFFLE:
            if ((v->aux_int & XI_VEC_SHAPE_UNZIP) != 0) {
                if (v->nargs != 2 || native_type != XR_NATIVE_U32 || lanes != 4 ||
                    !xicgen_vec_result_aggregate(ctx, v, &result_type)) {
                    xicgen_vec_error(ctx, out, v, "unzip needs two u32x4 vectors");
                    return;
                }
                fprintf(out, "({ %s _r; ", result_type);
                unsigned half = (v->aux_int & XI_VEC_SHAPE_ODD_LANES) != 0 ? 1u : 0u;
                for (uint8_t lane = 0; lane < lanes; lane++) {
                    unsigned source_lane = (unsigned) (lane & 1u) * 2u + half;
                    fprintf(out, "_r._lanes[%uu] = ", (unsigned) lane);
                    xicgen_emit_vec_lanes(out, v->args[lane >= 2 ? 1 : 0]);
                    fprintf(out, "[%uu]; ", source_lane);
                }
                fprintf(out, "_r; })");
                return;
            }
            if (v->nargs != 1 || !xicgen_vec_result_aggregate(ctx, v, &result_type)) {
                xicgen_vec_error(ctx, out, v, "shuffle needs vector and aggregate result");
                return;
            }
            fprintf(out, "({ %s _r; ", result_type);
            for (uint8_t lane = 0; lane < lanes; lane++) {
                uint8_t selected =
                    (uint8_t) ((v->aux_int >> (XI_VEC_SHAPE_SHUFFLE_SHIFT + lane * 4)) & 0xf);
                if (selected >= lanes) {
                    xicgen_vec_error(ctx, out, v, "shuffle lane outside shape");
                    return;
                }
                fprintf(out, "_r._lanes[%uu] = ", (unsigned) lane);
                xicgen_emit_vec_lanes(out, v->args[0]);
                fprintf(out, "[%uu]; ", (unsigned) selected);
            }
            fprintf(out, "_r; })");
            return;

        case XI_VEC_WIDEN_MUL:
            if (v->nargs != 2 || (lanes != 2 && lanes != 4) || native_type != XR_NATIVE_U64 ||
                !xicgen_vec_result_aggregate(ctx, v, &result_type)) {
                xicgen_vec_error(ctx, out, v,
                                 "widen-mul requires u32x4 -> u64x2 or u32x8 -> u64x4");
                return;
            }
            fprintf(out, "({ %s _r; ", result_type);
            for (uint8_t lane = 0; lane < lanes; lane++) {
                unsigned src =
                    (v->aux_int & XI_VEC_SHAPE_CONTIGUOUS_HALF) != 0
                        ? lane + ((v->aux_int & XI_VEC_SHAPE_ODD_LANES) != 0 ? lanes : 0u)
                        : lane * 2u + ((v->aux_int & XI_VEC_SHAPE_ODD_LANES) != 0 ? 1u : 0u);
                fprintf(out, "_r._lanes[%uu] = (uint64_t)", (unsigned) lane);
                xicgen_emit_vec_lanes(out, v->args[0]);
                fprintf(out, "[%uu] * (uint64_t)", src);
                xicgen_emit_vec_lanes(out, v->args[1]);
                fprintf(out, "[%uu]; ", src);
            }
            fprintf(out, "_r; })");
            return;

        case XI_VEC_UNZIP:
            if (v->nargs != 2 || native_type != XR_NATIVE_U32 || lanes != 4 ||
                !xicgen_vec_result_aggregate(ctx, v, &result_type)) {
                xicgen_vec_error(ctx, out, v, "unzip requires u32x4 inputs");
                return;
            }
            {
                unsigned odd = (v->aux_int & XI_VEC_SHAPE_ODD_LANES) != 0 ? 1u : 0u;
                fprintf(out, "({ %s _r; ", result_type);
                for (uint8_t lane = 0; lane < 4; lane++) {
                    unsigned which = lane < 2 ? 0u : 1u;
                    unsigned src = (lane % 2u) * 2u + odd;
                    fprintf(out, "_r._lanes[%uu] = ", (unsigned) lane);
                    xicgen_emit_vec_lanes(out, v->args[which]);
                    fprintf(out, "[%uu]; ", src);
                }
                fprintf(out, "_r; })");
            }
            return;

        case XI_VEC_WIDEN_MUL_HALF:
            if (v->nargs != 2 || native_type != XR_NATIVE_U64 || lanes != 2 ||
                !xicgen_vec_result_aggregate(ctx, v, &result_type)) {
                xicgen_vec_error(ctx, out, v, "widen-mul-half requires u32x4 -> u64x2");
                return;
            }
            {
                unsigned base = (v->aux_int & XI_VEC_SHAPE_ODD_LANES) != 0 ? 2u : 0u;
                fprintf(out, "({ %s _r; ", result_type);
                for (uint8_t lane = 0; lane < 2; lane++) {
                    unsigned src = base + lane;
                    fprintf(out, "_r._lanes[%uu] = (uint64_t)", (unsigned) lane);
                    xicgen_emit_vec_lanes(out, v->args[0]);
                    fprintf(out, "[%uu] * (uint64_t)", src);
                    xicgen_emit_vec_lanes(out, v->args[1]);
                    fprintf(out, "[%uu]; ", src);
                }
                fprintf(out, "_r; })");
            }
            return;

        case XI_VEC_REDUCE_ADD:
            if (v->nargs != 1) {
                xicgen_vec_error(ctx, out, v, "reduce-add needs vector");
                return;
            }
            fprintf(out,
                    "({ %s _sum = 0; for (uint32_t _i = 0; _i < %uu; _i++) "
                    "_sum = (%s)(_sum + ",
                    lane_type, (unsigned) lanes, lane_type);
            xicgen_emit_vec_lanes(out, v->args[0]);
            fprintf(out, "[_i]); _sum; })");
            return;

        default:
            xicgen_vec_error(ctx, out, v, "unknown typed vector opcode");
            return;
    }
}

static const XiValue *xicgen_span_shared_slot_source_in_func(const XiFunc *f, int slot,
                                                             bool *ambiguous) {
    const XiValue *source = NULL;
    if (!f || slot < 0)
        return NULL;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v || v->op != XI_SET_SHARED || (int) v->aux_int != slot || v->nargs < 1)
                continue;
            const XiValue *value = v->args[0];
            if (source && cg_unwrap_identity_value(source) != cg_unwrap_identity_value(value)) {
                if (ambiguous)
                    *ambiguous = true;
                return NULL;
            }
            source = value;
        }
    }
    return source;
}

static const XiValue *xicgen_span_shared_slot_source(const XiCgenCtx *ctx, const XiFunc *f,
                                                     int slot, bool *ambiguous) {
    const XiValue *source = xicgen_span_shared_slot_source_in_func(f, slot, ambiguous);
    if (source || (ambiguous && *ambiguous))
        return source;
    if (ctx && ctx->module && ctx->module->init && ctx->module->init != f)
        return xicgen_span_shared_slot_source_in_func(ctx->module->init, slot, ambiguous);
    return NULL;
}

static bool xicgen_value_belongs_to_func(const XiFunc *f, const XiValue *value) {
    const XiValue *unwrapped = cg_unwrap_identity_value(value);
    if (!f || !unwrapped)
        return false;
    if (unwrapped->block && unwrapped->block->func == f)
        return true;
    for (uint16_t i = 0; i < f->nparams; i++) {
        if (cg_unwrap_identity_value(f->params[i]) == unwrapped)
            return true;
    }
    return false;
}

static const XiImportRef *xicgen_freestanding_module_import_slot(const XiCgenCtx *ctx,
                                                                 const XiFunc *f, int slot) {
    if (!ctx || !ctx->freestanding_profile || slot < 0)
        return NULL;
    const XiImportRef *ref = cg_shared_slot_import_ref(f, slot);
    if (!ref && f && f->module && f->module->init != f)
        ref = cg_shared_slot_import_ref(f->module->init, slot);
    if (!ref && ctx->module && ctx->module->init && ctx->module->init != f)
        ref = cg_shared_slot_import_ref(ctx->module->init, slot);
    return ref && ref->module_path && !ref->member_name ? ref : NULL;
}

static bool xicgen_value_is_elided_static_aggregate_access(XiCgenCtx *ctx, const XiFunc *f,
                                                           const XiValue *v) {
    return cg_value_is_elided_static_struct_nested_field_ref(ctx, f, v) ||
           cg_value_is_elided_static_struct_fixed_array_field_ref(ctx, f, v) ||
           cg_value_is_elided_static_struct_nested_fixed_array_field_ref(ctx, f, v) ||
           cg_value_is_elided_static_fixed_array_const_ref(ctx, f, v) ||
           cg_value_is_elided_static_fixed_matrix_const_ref(ctx, f, v) ||
           cg_value_is_elided_static_fixed_matrix_index_ref(ctx, f, v) ||
           cg_value_is_elided_static_fixed_cube_const_ref(ctx, f, v) ||
           cg_value_is_elided_static_fixed_cube_outer_index_ref(ctx, f, v) ||
           cg_value_is_elided_static_fixed_cube_index_ref(ctx, f, v) ||
           cg_value_is_elided_static_fixed_struct_array_const_ref(ctx, f, v) ||
           cg_value_is_elided_static_fixed_struct_array_index_ref(ctx, f, v) ||
           cg_value_is_elided_static_fixed_struct_array_fixed_array_field_ref(ctx, f, v) ||
           cg_value_is_elided_static_fixed_struct_array_nested_fixed_array_field_ref(ctx, f, v) ||
           cg_value_is_elided_static_fixed_struct_array_nested_field_ref(ctx, f, v) ||
           cg_value_is_elided_static_fixed_tuple_array_const_ref(ctx, f, v) ||
           cg_value_is_elided_static_fixed_tuple_array_index_ref(ctx, f, v) ||
           cg_value_is_elided_static_fixed_tuple_array_tuple_ref(ctx, f, v) ||
           cg_value_is_elided_static_tuple_const_ref(ctx, f, v) ||
           cg_value_is_elided_static_struct_const_ref(ctx, f, v);
}

static void xicgen_get_shared(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    (void) prefix;
    const XiConstLiteral *lit = xicgen_freestanding_const_slot_literal(ctx, v ? v->aux_int : -1);
    if (lit) {
        xicgen_emit_const_slot_literal_as_value(ctx, out, v, lit);
        return;
    }
    const XiConstLiteral *static_lit = NULL;
    if (cg_freestanding_static_scalar_const_literal(ctx, v ? v->aux_int : -1, &static_lit) &&
        cg_emit_freestanding_static_scalar_const_ref(ctx, out, v, static_lit)) {
        return;
    }
    if (cg_freestanding_static_scalar_var_literal(ctx, v ? v->aux_int : -1, &static_lit) &&
        cg_emit_freestanding_static_scalar_var_ref(ctx, out, v, static_lit)) {
        return;
    }
    const XiModule *import_const_module = NULL;
    int64_t import_const_slot = -1;
    const XiConstLiteral *import_lit = cg_import_slot_const_literal(
        ctx, f, v ? (int) v->aux_int : -1, &import_const_module, &import_const_slot);
    if (import_lit && xicgen_const_literal_is_freestanding_scalar(import_lit) &&
        !cg_const_literal_is_static_scalar_object(import_lit)) {
        /* A named imported `const` scalar is immutable by construction and
         * the module bundle publishes its canonical literal.  Preserve that
         * proof at the use site instead of routing through the mutable shared
         * slot ABI; otherwise native arithmetic becomes tagged runtime calls
         * and target range analysis loses the constant.  Data-addressable
         * constants remain objects and deliberately stay on the static-data
         * path. */
        xicgen_emit_const_slot_literal_as_value(ctx, out, v, import_lit);
        return;
    }
    if (import_lit && xicgen_const_literal_is_freestanding_scalar(import_lit) &&
        (!cg_const_literal_is_static_scalar_object(import_lit) ||
         cg_imported_static_const_needs_weak_symbol(ctx, import_const_module, import_lit))) {
        xicgen_emit_const_slot_literal_as_value(ctx, out, v, import_lit);
        return;
    }
    if (import_lit &&
        cg_freestanding_static_scalar_const_literal_in_module(ctx, import_const_module,
                                                              import_const_slot, &static_lit) &&
        cg_emit_freestanding_static_scalar_const_ref_in_module(ctx, out, import_const_module,
                                                               import_const_slot, v, static_lit)) {
        return;
    }
    if (import_lit && import_lit->kind == XI_CONST_LITERAL_COMPTIME_AGGREGATE) {
        if ((f && f->module && f == f->module->init) ||
            xicgen_value_is_elided_static_aggregate_access(ctx, f, v)) {
            fprintf(out, "XR_NULL_VAL /* static const import: %s.%s */",
                    import_const_module && import_const_module->name ? import_const_module->name
                                                                     : "?",
                    cg_module_const_slot_name(import_const_module, import_const_slot)
                        ? cg_module_const_slot_name(import_const_module, import_const_slot)
                        : "?");
            return;
        }
        fprintf(stderr,
                "[xi_cgen] ERROR: freestanding imported aggregate const '%s.%s' must be consumed "
                "through static field/index access\n",
                import_const_module && import_const_module->name ? import_const_module->name : "?",
                cg_module_const_slot_name(import_const_module, import_const_slot)
                    ? cg_module_const_slot_name(import_const_module, import_const_slot)
                    : "?");
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return;
    }
    const XiConstLiteral *erased = xicgen_freestanding_erased_const_slot(ctx, v ? v->aux_int : -1);
    if (erased && erased->kind == XI_CONST_LITERAL_COMPTIME_AGGREGATE) {
        if (xicgen_value_is_elided_static_aggregate_access(ctx, f, v)) {
            fprintf(out, "XR_NULL_VAL /* static aggregate const */");
            return;
        }
        fprintf(stderr,
                "[xi_cgen] ERROR: freestanding top-level aggregate const slot %d must be "
                "consumed through comptime before static data sections land\n",
                (int) v->aux_int);
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return;
    }
    const XiConstLiteral *shared_static =
        cg_freestanding_shared_initializer_literal(ctx, v ? v->aux_int : -1);
    if (shared_static && shared_static->kind == XI_CONST_LITERAL_COMPTIME_AGGREGATE) {
        if ((f && f->module && f == f->module->init) ||
            xicgen_value_is_elided_static_aggregate_access(ctx, f, v)) {
            fprintf(out, "XR_NULL_VAL /* static shared aggregate */");
            return;
        }
        fprintf(stderr,
                "[xi_cgen] ERROR: freestanding shared aggregate slot %d must be consumed "
                "through static field/index access in the current slice\n",
                (int) v->aux_int);
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return;
    }
    if (ctx && ctx->freestanding_profile &&
        cg_enum_for_shared_slot_in_func(ctx, f, (int) v->aux_int)) {
        fprintf(out, "XR_NULL_VAL");
        return;
    }
    const XiImportRef *module_ref =
        xicgen_freestanding_module_import_slot(ctx, f, (int) v->aux_int);
    if (module_ref) {
        fprintf(out, "XR_NULL_VAL /* module import: %s */", module_ref->module_path);
        return;
    }
    if (cg_value_type_is_span(v)) {
        bool ambiguous = false;
        const XiValue *source =
            xicgen_span_shared_slot_source(ctx, f, (int) v->aux_int, &ambiguous);
        if (source) {
            if (cg_value_plan_is_span_aggregate(ctx, source)) {
                if (xicgen_value_belongs_to_func(f, source)) {
                    emit_vref(out, source);
                    return;
                }
                fprintf(stderr,
                        "[xi_cgen] ERROR: cannot read non-local Slice aggregate shared slot %d "
                        "as XrValue in %s\n",
                        (int) v->aux_int, f && f->name ? f->name : "?");
                ctx->error = true;
                emit_codegen_abort_expr(out);
            } else {
                fprintf(out, "xrt_span_from_array_slice(");
                if (xicgen_value_belongs_to_func(f, source)) {
                    emit_value_as_rep_ctx(ctx, out, source, XR_REP_TAGGED);
                } else {
                    fprintf(out, "%s[%d]", ctx->shared_name, (int) v->aux_int);
                }
                fprintf(out, ", INT64_C(0), INT64_MAX)");
            }
            return;
        }
        fprintf(stderr, "[xi_cgen] ERROR: cannot read Slice shared slot %d as XrValue in %s\n",
                (int) v->aux_int, f && f->name ? f->name : "?");
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return;
    }
    XrRep target_rep = cg_value_plan_storage_rep(ctx, v);
    const char *suffix =
        emit_conversion_prefix_ctx(ctx, out, v ? v->type : NULL, XR_REP_TAGGED, target_rep);
    fprintf(out, "%s[%d]", ctx->shared_name, (int) v->aux_int);
    emit_conversion_suffix(out, suffix);
}

static void xicgen_set_shared(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    const XiValue *value = (v && v->nargs >= 1) ? v->args[0] : NULL;
    if (xicgen_freestanding_erased_const_slot(ctx, v ? v->aux_int : -1)) {
        fprintf(out, "XR_NULL_VAL");
        return;
    }
    const XiConstLiteral *shared_init =
        cg_freestanding_shared_initializer_literal(ctx, v ? v->aux_int : -1);
    if (shared_init && ctx && ctx->module && f == ctx->module->init &&
        (shared_init->kind == XI_CONST_LITERAL_COMPTIME_AGGREGATE ||
         cg_const_value_matches_literal(value, shared_init))) {
        fprintf(out, "XR_NULL_VAL");
        return;
    }
    if (shared_init && ctx && ctx->freestanding_profile &&
        shared_init->kind == XI_CONST_LITERAL_COMPTIME_AGGREGATE) {
        const XiValue *origin = cg_trace_struct_new(value);
        int64_t static_slot = -1;
        if (origin && cg_struct_can_inline_static_whole_store(ctx, f, origin) &&
            cg_static_struct_whole_store_target(ctx, v, (const XrAggregateLayout *) origin->aux,
                                                &static_slot)) {
            fprintf(out, "(memcpy(&");
            cg_emit_static_struct_name(ctx, out, ctx ? ctx->module : NULL, static_slot);
            fprintf(out, ", &_st%u, sizeof(", origin->id);
            cg_emit_static_struct_name(ctx, out, ctx ? ctx->module : NULL, static_slot);
            fprintf(out, ")), XR_NULL_VAL)");
            return;
        }
        fprintf(stderr,
                "[xi_cgen] ERROR: freestanding profile rejects whole-value assignment to static "
                "aggregate top-level var; mutate fields directly in the current slice\n");
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return;
    }
    const XiConstLiteral *static_scalar_var = NULL;
    if (cg_freestanding_static_scalar_var_literal(ctx, v ? v->aux_int : -1, &static_scalar_var) &&
        cg_emit_freestanding_static_scalar_var_store(
            ctx, out, ctx ? ctx->module : NULL, v ? v->aux_int : -1, f, value, static_scalar_var)) {
        return;
    }
    if (ctx && ctx->freestanding_profile &&
        cg_enum_for_shared_slot_in_func(ctx, f, (int) v->aux_int)) {
        fprintf(out, "XR_NULL_VAL");
        return;
    }
    if (xicgen_freestanding_module_import_slot(ctx, f, (int) v->aux_int)) {
        fprintf(out, "XR_NULL_VAL");
        return;
    }
    if (cg_value_plan_is_span_aggregate(ctx, value)) {
        bool ambiguous = false;
        const XiValue *source =
            xicgen_span_shared_slot_source(ctx, f, (int) v->aux_int, &ambiguous);
        if (source && cg_unwrap_identity_value(source) == cg_unwrap_identity_value(value)) {
            fprintf(out, "XR_NULL_VAL");
            return;
        }
        fprintf(stderr,
                "[xi_cgen] ERROR: cannot store Slice aggregate v%u in shared slot %d in %s\n",
                value ? value->id : 0, (int) v->aux_int, f && f->name ? f->name : "?");
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return;
    }
    if (ctx && ctx->freestanding_profile) {
        fprintf(out, "(%s[%d] = ", ctx->shared_name, (int) v->aux_int);
        if (cg_value_plan_is_struct_aggregate(ctx, value)) {
            if (!emit_struct_aggregate_box_expr(ctx, out, f, value, prefix)) {
                fprintf(stderr,
                        "[xi_cgen] ERROR: cannot box aggregate v%u for freestanding shared slot "
                        "%d in %s\n",
                        value ? value->id : 0, (int) v->aux_int, f && f->name ? f->name : "?");
                ctx->error = true;
                emit_codegen_abort_expr(out);
            }
        } else {
            emit_value_as_rep_ctx(ctx, out, value, XR_REP_TAGGED);
        }
        fprintf(out, ")");
        return;
    }

    fprintf(out, "(%s[%d] = ({ XrValue _xsv = ", ctx->shared_name, (int) v->aux_int);
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
    fprintf(out, "; (XR_IS_ARRAY_REF(_xsv) && (_xsv.flags & XRT_VALUE_FLAG_ARRAY_REF_OWNED) == 0) "
                 "? xrt_array_ref_to_owned(_xsv) : _xsv; }))");
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
static bool xicgen_emit_runtime_control_method(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                               const XiValue *v);
static bool xicgen_emit_test_yield_method(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                          const XiValue *v);
static bool xicgen_emit_stdlib_import_call(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                           const XiValue *v);
static bool cg_module_has_aot_direct_calls(const char *module);
static bool cg_aot_stdlib_has_direct_member(const char *module, const char *member);
/* Resolve a `import { CONST } from "module"` reference to a generated stdlib
 * constant (path.sep, encoding.LE, ...); defined in xi_cgen_stdlib_helpers.inc.c. */
static bool cg_emit_aot_stdlib_generated_constant_import_ref(XiCgenCtx *ctx, FILE *out,
                                                             const XiValue *v,
                                                             const XiImportRef *ref);
static bool cg_emit_aot_stdlib_generated_constant_field(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                        const XiValue *v);

static void xicgen_import_ref(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    (void) f;
    (void) prefix;
    const XiImportRef *ref = (const XiImportRef *) v->aux;
    const XiModule *import_const_module = NULL;
    int64_t import_const_slot = -1;
    const XiConstLiteral *import_lit =
        cg_import_ref_target_const_literal(ctx, ref, &import_const_module, &import_const_slot);
    if (import_lit && xicgen_const_literal_is_freestanding_scalar(import_lit) &&
        !cg_const_literal_is_static_scalar_object(import_lit)) {
        xicgen_emit_const_slot_literal_as_value(ctx, out, v, import_lit);
        return;
    }
    if (import_lit && ctx && ctx->freestanding_profile) {
        const XiConstLiteral *static_lit = NULL;
        if (xicgen_const_literal_is_freestanding_scalar(import_lit) &&
            (!cg_const_literal_is_static_scalar_object(import_lit) ||
             cg_imported_static_const_needs_weak_symbol(ctx, import_const_module, import_lit))) {
            xicgen_emit_const_slot_literal_as_value(ctx, out, v, import_lit);
            return;
        }
        if (cg_freestanding_static_scalar_const_literal_in_module(ctx, import_const_module,
                                                                  import_const_slot, &static_lit) &&
            cg_emit_freestanding_static_scalar_const_ref_in_module(
                ctx, out, import_const_module, import_const_slot, v, static_lit)) {
            return;
        }
        if (import_lit->kind == XI_CONST_LITERAL_COMPTIME_AGGREGATE) {
            fprintf(out, "XR_NULL_VAL /* static const import: %s.%s */",
                    import_const_module && import_const_module->name ? import_const_module->name
                                                                     : "?",
                    cg_module_const_slot_name(import_const_module, import_const_slot)
                        ? cg_module_const_slot_name(import_const_module, import_const_slot)
                        : "?");
            return;
        }
    }
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
                    strcmp(ref->module_path, "parallel") == 0 ||
                    cg_module_has_aot_direct_calls(ref->module_path))) {
            fprintf(out, "XR_NULL_VAL /* builtin module: %s */", ref->module_path);
        } else if (xicgen_import_ref_is_core_math_member(ref)) {
            fprintf(out, "XR_NULL_VAL /* builtin math.%s */", ref->member_name);
        } else if (ref && ref->module_path && ref->member_name &&
                   xa_builtin_get_record_type(ref->module_path, ref->member_name)) {
            /* Native Record declarations are type-only imports. They have no
             * runtime namespace value to resolve after type erasure. */
            fprintf(out, "XR_NULL_VAL /* builtin record type: %s.%s */", ref->module_path,
                    ref->member_name);
        } else if (ref && ref->module_path && ref->member_name &&
                   xa_builtin_get_enum_type(ref->module_path, ref->member_name)) {
            /* Native enum declarations are namespace-only imports. Variant
             * access is lowered statically from generated enum metadata. */
            fprintf(out, "XR_NULL_VAL /* builtin enum type: %s.%s */", ref->module_path,
                    ref->member_name);
        } else if (ref && ref->module_path && ref->member_name &&
                   cg_aot_stdlib_has_direct_member(ref->module_path, ref->member_name)) {
            fprintf(out, "XR_NULL_VAL /* builtin function: %s.%s */", ref->module_path,
                    ref->member_name);
        } else if (cg_emit_aot_stdlib_generated_constant_import_ref(ctx, out, v, ref)) {
            /* Resolved to a generated stdlib constant (path.sep, encoding.LE, ...). */
        } else if (cg_import_ref_has_verified_link_dependency(ctx, ref) &&
                   cg_import_ref_value_is_dead_for_aot(ctx, f, v)) {
            fprintf(out, "XR_NULL_VAL /* unreachable verified import: %s.%s */",
                    ref && ref->module_path ? ref->module_path : "?",
                    ref && ref->member_name ? ref->member_name : "?");
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

static void xicgen_emit_assert_condition(XiCgenCtx *ctx, FILE *out, const XiValue *condition) {
    if (condition && condition->type && condition->type->kind == XR_KIND_BOOL) {
        fprintf(out, "(");
        emit_value_as_rep_ctx(ctx, out, condition, XR_REP_I64);
        fprintf(out, " != 0)");
        return;
    }
    fprintf(out, "xr_truthy(");
    emit_value_as_rep_ctx(ctx, out, condition, XR_REP_TAGGED);
    fprintf(out, ")");
}

static void xicgen_assert(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                          const char *prefix) {
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 1, "xicgen_assert: need cond");
    const char *loc = v->aux ? (const char *) v->aux : "<unknown>";
    bool invert = (v->aux_int == 1);
    fprintf(out, "({ bool _xr_assert_ok = %s(", invert ? "!" : "");
    xicgen_emit_assert_condition(ctx, out, v->args[0]);
    fprintf(out, "); if (XR_UNLIKELY(!_xr_assert_ok)) { ");
    if (ctx && ctx->freestanding_profile) {
        fprintf(out,
                "xrt_freestanding_trap(\"Assertion failed%s: %s\"); } "
                "XR_ASSUME(_xr_assert_ok); XR_NULL_VAL; })",
                invert ? " (expected false)" : "", loc);
        return;
    }
    fprintf(out,
            "fprintf(stderr, \"Assertion failed%s: %s\\n\"); abort(); } "
            "XR_ASSUME(_xr_assert_ok); XR_NULL_VAL; })",
            invert ? " (expected false)" : "", loc);
}

static void xicgen_assert_eq(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                             const char *prefix) {
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 2, "xicgen_assert_eq: need 2 args");
    const char *loc = v->aux ? (const char *) v->aux : "<unknown>";
    if (ctx && ctx->freestanding_profile) {
        fprintf(out, "(xrt_eq(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
        fprintf(out,
                ") ? XR_NULL_VAL : (xrt_freestanding_trap(\"assert_eq failed: %s\"), "
                "XR_NULL_VAL))",
                loc);
        return;
    }
    fprintf(out, "(xrt_eq(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
    fprintf(out,
            ") ? XR_NULL_VAL : (fprintf(stderr, \"assert_eq failed: %s\\n\"), abort(), "
            "XR_NULL_VAL))",
            loc);
}

static void xicgen_assert_ne(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                             const char *prefix) {
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 2, "xicgen_assert_ne: need 2 args");
    const char *loc = v->aux ? (const char *) v->aux : "<unknown>";
    if (ctx && ctx->freestanding_profile) {
        fprintf(out, "(!xrt_eq(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
        fprintf(out,
                ") ? XR_NULL_VAL : (xrt_freestanding_trap(\"assert_ne failed: %s\"), "
                "XR_NULL_VAL))",
                loc);
        return;
    }
    fprintf(out, "(!xrt_eq(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
    fprintf(out,
            ") ? XR_NULL_VAL : (fprintf(stderr, \"assert_ne failed: %s\\n\"), abort(), "
            "XR_NULL_VAL))",
            loc);
}

static void xicgen_typeid(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                          const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 1, "xicgen_typeid: need arg");
    fprintf(out, "XR_FROM_INT(xrt_typeof_id(");
    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
    fprintf(out, "))");
}

static void xicgen_typename(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                            const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 1, "xicgen_typename: need arg");
    fprintf(out, "xrt_typename(");
    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ")");
}

static bool emit_fixed_array_ref_length_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v);
static const char *xicgen_aot_context_expr(XiCgenCtx *ctx, const XiFunc *f);

static void xicgen_len(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                       const char *prefix) {
    XR_DCHECK(v->nargs == 1, "xicgen_len: need one arg");
    if (emit_class_native_map_length_expr(ctx, out, f, v) ||
        emit_class_native_set_length_expr(ctx, out, f, v) ||
        emit_class_native_array_length_expr(ctx, out, f, v) || emit_span_length_expr(ctx, out, v) ||
        emit_fixed_array_ref_length_expr(ctx, out, v) ||
        emit_typed_array_length_expr(ctx, out, f, prefix, v))
        return;
    const char *suffix =
        emit_conversion_prefix(out, v->type, XR_REP_I64, cg_value_plan_storage_rep(ctx, v));
    if (cg_value_plan_is_span_aggregate(ctx, v->args[0])) {
        fprintf(out, "(");
        emit_vref(out, v->args[0]);
        fprintf(out, ").length");
    } else if (v->args[0]->type && v->args[0]->type->kind == XR_KIND_FIXED_ARRAY) {
        fprintf(out, "INT64_C(%d)", v->args[0]->type->fixed_array.length);
    } else if (xi_value_type_is_channel(v->args[0])) {
        fprintf(out, "XR_TO_INT(xr_aot_chan_length(%s, ", xicgen_aot_context_expr(ctx, f));
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, "))");
    } else if (xi_value_type_is_work_queue(v->args[0])) {
        fprintf(out, "XR_TO_INT(xr_aot_work_queue_length(%s, ", xicgen_aot_context_expr(ctx, f));
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, "))");
    } else if (xr_type_is_named_class(v->args[0]->type, "Buffer")) {
        fprintf(out, "xrt_buffer_length(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ")");
    } else {
        fprintf(out, "({ XrValue _xr_len_value = ");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out,
                "; _xr_len_value.tag == XR_TAG_RANGE ? "
                "xrt_range_length_ptr((const xrt_range_t *)_xr_len_value.ptr) : "
                "XR_TO_INT(xrt_len_value(_xr_len_value, %d)); })",
                v->aux_int != 0 ? 1 : 0);
    }
    emit_conversion_suffix(out, suffix);
}

static void xicgen_get_builtin(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                               const char *prefix) {
    (void) f;
    (void) prefix;
    if (v->aux_int == XR_GLOBAL_VAR_PROCESS || v->aux_int == XR_GLOBAL_VAR_FILE ||
        v->aux_int == XR_GLOBAL_VAR_DIR) {
        fprintf(out, "xrt_builtins[%d]", (int) v->aux_int);
    } else if (v->aux_int == XR_GLOBAL_VAR_JSON || v->aux_int == XR_GLOBAL_VAR_STRING) {
        fprintf(out, "XR_NULL_VAL /* builtin %s namespace */",
                v->aux ? (const char *) v->aux : "native");
    } else if (v->aux_int == XR_GLOBAL_VAR_ATOMIC || v->aux_int == XR_GLOBAL_VAR_WORKQUEUE ||
               v->aux_int == XR_GLOBAL_VAR_RESULTGROUP ||
               v->aux_int == XR_GLOBAL_VAR_COUNTDOWNLATCH ||
               v->aux_int == XR_GLOBAL_VAR_SEMAPHORE || v->aux_int == XR_GLOBAL_VAR_EVENTCOUNT ||
               v->aux_int == XR_GLOBAL_VAR_PANIC_INFO) {
        /* Class token only used as a constructor receiver; the constructor call is
         * lowered directly to an exception value (see xicgen_emit_panicinfo_constructor). */
        fprintf(out, "XR_NULL_VAL /* builtin native class token: %s */",
                v->aux ? (const char *) v->aux : "?");
    } else if (ctx && ctx->freestanding_profile && cg_prelude_enum_data((int) v->aux_int) != NULL) {
        fprintf(out, "XR_NULL_VAL /* freestanding prelude enum namespace: %s */",
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

static bool xicgen_endian_member_index(const char *name, int64_t *out_index) {
    if (!name || !out_index)
        return false;
    if (strcmp(name, "Native") == 0) {
        *out_index = XR_ENDIAN_NATIVE;
        return true;
    }
    if (strcmp(name, "LE") == 0) {
        *out_index = XR_ENDIAN_LE;
        return true;
    }
    if (strcmp(name, "BE") == 0) {
        *out_index = XR_ENDIAN_BE;
        return true;
    }
    return false;
}

static bool xicgen_value_is_endian_member(const XiValue *value, int64_t *out_index) {
    const XiValue *origin = cg_unwrap_identity_value(value);
    if (!origin || origin->op != XI_LOAD_FIELD || origin->nargs < 1 || !origin->aux)
        return false;
    const XiValue *receiver = cg_unwrap_identity_value(origin->args[0]);
    if (!receiver || receiver->op != XI_GET_BUILTIN || receiver->aux_int != XR_GLOBAL_VAR_ENDIAN)
        return false;
    return xicgen_endian_member_index((const char *) origin->aux, out_index);
}

static bool xicgen_value_is_const_endian(const XiValue *value, int64_t *out_index) {
    if (!out_index)
        return false;
    if (!value) {
        *out_index = XR_ENDIAN_NATIVE;
        return true;
    }
    if (xicgen_value_is_endian_member(value, out_index))
        return true;
    const XiValue *origin = cg_unwrap_identity_value(value);
    if (origin && origin->op == XI_CONST && origin->type && origin->type->kind == XR_KIND_INT &&
        origin->aux_int >= XR_ENDIAN_NATIVE && origin->aux_int <= XR_ENDIAN_BE) {
        *out_index = origin->aux_int;
        return true;
    }
    return false;
}

static void xicgen_emit_endian_arg_i64(XiCgenCtx *ctx, FILE *out, const XiValue *value) {
    int64_t endian = XR_ENDIAN_NATIVE;
    if (xicgen_value_is_const_endian(value, &endian)) {
        if (endian == XR_ENDIAN_NATIVE) {
            fprintf(out, "XRT_TARGET_NATIVE_ENDIAN");
            return;
        }
        fprintf(out, "INT64_C(%" PRId64 ")", endian);
        return;
    }
    fprintf(out, "xrt_endian_arg(");
    emit_value_as_rep_ctx(ctx, out, value, XR_REP_TAGGED);
    fprintf(out, ")");
}

/* Raw pointer memory is already analyzer-proven to receive Endian. Do not
 * re-enter the checked runtime decoder here: an enum value carries its
 * declaration-order discriminator in XrValue.ext, and an integer value is
 * already the canonical Xi encoding. This keeps unsafe memory free of a
 * hidden type-error channel while checked Slice<byte> access retains its
 * defensive boundary decoder above. */
static void xicgen_emit_raw_endian_arg_i64(XiCgenCtx *ctx, FILE *out, const XiValue *value) {
    int64_t endian = XR_ENDIAN_NATIVE;
    if (xicgen_value_is_const_endian(value, &endian)) {
        if (endian == XR_ENDIAN_NATIVE) {
            fprintf(out, "XRT_TARGET_NATIVE_ENDIAN");
            return;
        }
        fprintf(out, "INT64_C(%" PRId64 ")", endian);
        return;
    }
    const XiValue *origin = cg_unwrap_identity_value(value);
    if (origin && origin->type && origin->type->kind == XR_KIND_INT) {
        emit_value_as_rep_ctx(ctx, out, value, XR_REP_I64);
        return;
    }
    fprintf(out, "((int64_t)(");
    emit_value_as_rep_ctx(ctx, out, value, XR_REP_TAGGED);
    fprintf(out, ").ext)");
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
    return type && type->kind == XR_KIND_SLICE;
}

static const XiValue *xicgen_stack_slice_source_value(const XiValue *arg) {
    const XiValue *slice = cg_unwrap_identity_value(arg);
    return slice && slice->op == XI_SLICE && slice->nargs >= 3 ? slice : NULL;
}

static bool xicgen_slice_can_inline_bytes_common_prefix(XiCgenCtx *ctx, const XiValue *arg) {
    const XiValue *slice = xicgen_stack_slice_source_value(arg);
    return slice && cg_span_value_u8_info(ctx, slice->args[0], NULL);
}

static bool xicgen_direct_call_param_noescape(XiCgenCtx *ctx, const XiFunc *current,
                                              const XiFunc *target, uint16_t param_index,
                                              uint8_t depth);

static bool xicgen_method_arg_keeps_span_noescape(const XiValue *user, uint16_t arg_index) {
    if (!user || (user->op != XI_CALL_METHOD && user->op != XI_CALL_METHOD_DIRECT))
        return false;
    if (arg_index == 0 &&
        (cg_call_method_matches_receiver_registry_id(user,
                                                     XA_BUILTIN_RECEIVER_METHOD_POD_SLICE_GET) ||
         cg_call_method_matches_receiver_registry_id(user,
                                                     XA_BUILTIN_RECEIVER_METHOD_U8_SLICE_LOAD) ||
         cg_call_method_matches_receiver_registry_id(user,
                                                     XA_BUILTIN_RECEIVER_METHOD_U8_SLICE_STORE) ||
         cg_call_method_matches_receiver_registry_id(
             user, XA_BUILTIN_RECEIVER_METHOD_U8_SLICE_COMMON_PREFIX) ||
         cg_call_method_matches_receiver_registry_id(
             user, XA_BUILTIN_RECEIVER_METHOD_U8_SLICE_REPEAT_FROM) ||
         cg_call_method_matches_receiver_registry_id(
             user, XA_BUILTIN_RECEIVER_METHOD_U8_SLICE_COPY_FROM) ||
         cg_call_method_matches_receiver_registry_id(user,
                                                     XA_BUILTIN_RECEIVER_METHOD_U8_SLICE_COMPARE) ||
         cg_call_method_matches_receiver_registry_id(user,
                                                     XA_BUILTIN_RECEIVER_METHOD_U8_SLICE_FILL) ||
         cg_call_method_matches_receiver_registry_id(
             user, XA_BUILTIN_RECEIVER_METHOD_POD_SLICE_COPY_FROM) ||
         cg_call_method_matches_receiver_registry_id(
             user, XA_BUILTIN_RECEIVER_METHOD_POD_SLICE_COMPARE) ||
         cg_call_method_matches_receiver_registry_id(user,
                                                     XA_BUILTIN_RECEIVER_METHOD_POD_SLICE_FILL)))
        return true;
    if (arg_index == 1 && cg_call_method_matches_receiver_registry_id(
                              user, XA_BUILTIN_RECEIVER_METHOD_U8_ARRAY_APPEND_FROM))
        return true;
    return false;
}

static bool xicgen_call_method_is_common_prefix(const XiValue *value) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || (v->op != XI_CALL_METHOD && v->op != XI_CALL_METHOD_DIRECT) || v->nargs != 2)
        return false;
    return cg_call_method_matches_receiver_registry_id(
        v, XA_BUILTIN_RECEIVER_METHOD_U8_SLICE_COMMON_PREFIX);
}

static bool xicgen_call_method_is_copy_from(const XiValue *value) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || (v->op != XI_CALL_METHOD && v->op != XI_CALL_METHOD_DIRECT) || v->nargs != 2)
        return false;
    return cg_call_method_matches_receiver_registry_id(
        v, XA_BUILTIN_RECEIVER_METHOD_U8_SLICE_COPY_FROM);
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
                    case XI_SOURCE_MOVE:
                    case XI_OWNER_FORWARD:
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
        case XI_SOURCE_MOVE:
        case XI_OWNER_FORWARD:
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
        case XI_BYTE_SLICE_LOAD_U16:
        case XI_BYTE_SLICE_LOAD_U32:
        case XI_BYTE_SLICE_LOAD_U64:
        case XI_BYTE_SLICE_LOAD_F32:
        case XI_BYTE_SLICE_LOAD_F64:
        case XI_BYTE_SLICE_STORE_U16:
        case XI_BYTE_SLICE_STORE_U32:
        case XI_BYTE_SLICE_STORE_U64:
        case XI_BYTE_SLICE_STORE_F32:
        case XI_BYTE_SLICE_STORE_F64:
        case XI_BYTE_SLICE_FILL:
        case XI_BYTE_SLICE_REPEAT:
        case XI_SLICE_WINDOW:
        case XI_SLICE_AS_BYTES:
        case XI_SLICE_FILL:
        case XI_SLICE_REINTERPRET:
            return arg_index == 0;
        case XI_SLICE_COPY:
        case XI_SLICE_COMPARE:
        case XI_BYTE_SLICE_COPY:
        case XI_BYTE_SLICE_COMPARE:
        case XI_BYTE_SLICE_COMMON_PREFIX:
            return arg_index == 0 || arg_index == 1;
        case XI_BYTE_ARRAY_APPEND_FROM:
        case XI_BYTE_ARRAY_COPY_FROM:
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
    (void) current;
    (void) call;
    const XiValue *slice = xicgen_stack_slice_source_value(arg);
    if (!slice || !target || arg_index >= target->nparams || !target->params)
        return false;
    const XaotFuncPlan *target_plan = cg_func_plan(ctx, target);
    if (target_plan && arg_index < target_plan->abi.nparams && target_plan->abi.params) {
        XaotValueRep slot_rep = xaot_abi_slot_value_rep(&target_plan->abi.params[arg_index]);
        if (slot_rep.kind == XAOT_VALUE_AGGREGATE)
            return false;
    }
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
           xi_func_param_passing_mode(target, arg_index) == XR_PARAM_READ;
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
                if (user->op == XI_BYTE_SLICE_COMMON_PREFIX && a <= 1 &&
                    xicgen_slice_can_inline_bytes_common_prefix(ctx, slice)) {
                    saw_stack_call = true;
                    continue;
                }
                if (xicgen_call_method_is_common_prefix(user) && a <= 1 &&
                    xicgen_slice_can_inline_bytes_common_prefix(ctx, slice)) {
                    saw_stack_call = true;
                    continue;
                }
                if (xicgen_call_method_is_copy_from(user) && a <= 1 &&
                    xicgen_slice_can_inline_bytes_common_prefix(ctx, slice)) {
                    saw_stack_call = true;
                    continue;
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
                if (user->op == XI_BYTE_SLICE_COMMON_PREFIX && a <= 1 &&
                    xicgen_slice_can_inline_bytes_common_prefix(ctx, target)) {
                    saw_stack_call = true;
                    continue;
                }
                if (xicgen_call_method_is_common_prefix(user) && a <= 1 &&
                    xicgen_slice_can_inline_bytes_common_prefix(ctx, target)) {
                    saw_stack_call = true;
                    continue;
                }
                if (xicgen_call_method_is_copy_from(user) && a <= 1 &&
                    xicgen_slice_can_inline_bytes_common_prefix(ctx, target)) {
                    saw_stack_call = true;
                    continue;
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
static void emit_vararg_rest_expr_values(XiCgenCtx *ctx, FILE *out, uint32_t site_id,
                                         XiValue *const *args, uint16_t nargs, const XiFunc *target,
                                         uint16_t arg_start) {
    uint16_t fixed = target->nparams;
    const XrType *rest_type =
        (target->params && target->params[fixed]) ? target->params[fixed]->type : NULL;
    XrRep rest_rep = cg_func_param_abi_rep(ctx, target, fixed);
    CgArrayElemInfo rest_elem;
    bool rest_typed = cg_array_elem_info_from_type_ctx(ctx, rest_type, &rest_elem) &&
                      rest_elem.rep != XR_REP_TAGGED && rest_elem.ctype;
    if (rest_typed) {
        int64_t rest_count = (int64_t) nargs - (int64_t) arg_start - (int64_t) fixed;
        if (rest_count < 0)
            rest_count = 0;
        fprintf(out, "({ xrt_array_t *_va%u = xrt_array_new_typed_ptr(%" PRId64 ", %s); ", site_id,
                rest_count, rest_elem.elem_name);
        int64_t idx = 0;
        for (uint16_t a = (uint16_t) (arg_start + fixed); a < nargs; a++, idx++) {
            fprintf(out, "((%s*)_va%u->data)[%" PRId64 "] = (%s)", rest_elem.ctype, site_id, idx,
                    rest_elem.ctype);
            emit_value_as_rep_ctx(ctx, out, args[a], rest_elem.rep);
            fprintf(out, "; ");
        }
        const char *rest_suffix = emit_conversion_prefix(out, rest_type, XR_REP_PTR, rest_rep);
        fprintf(out, "_va%u", site_id);
        emit_conversion_suffix(out, rest_suffix);
        fprintf(out, "; })");
    } else {
        fprintf(out, "({ XrValue _va%u = xrt_array_new(0); ", site_id);
        for (uint16_t a = (uint16_t) (arg_start + fixed); a < nargs; a++) {
            fprintf(out, "xrt_array_push(_va%u, ", site_id);
            emit_value_as_rep_ctx(ctx, out, args[a], XR_REP_TAGGED);
            fprintf(out, "); ");
        }
        const char *rest_suffix = emit_conversion_prefix(out, rest_type, XR_REP_TAGGED, rest_rep);
        fprintf(out, "_va%u", site_id);
        emit_conversion_suffix(out, rest_suffix);
        fprintf(out, "; })");
    }
}

static void emit_vararg_rest_expr_from(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                       const XiFunc *target, uint16_t arg_start) {
    (void) f;
    emit_vararg_rest_expr_values(ctx, out, v->id, v->args, v->nargs, target, arg_start);
}

static void emit_vararg_rest_arg_from(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                      const XiFunc *target, uint16_t arg_start) {
    fprintf(out, ", ");
    emit_vararg_rest_expr_from(ctx, out, f, v, target, arg_start);
}

static void emit_vararg_rest_arg(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                 const XiFunc *target) {
    emit_vararg_rest_arg_from(ctx, out, f, v, target, 1);
}

static void emit_vararg_method_rest_arg(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                        const XiValue *v, const XiFunc *target) {
    emit_vararg_rest_arg_from(ctx, out, f, v, target, 0);
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

static void xicgen_emit_map_instance_alloc(FILE *out, const char *class_name);

static const char *xicgen_options_action_name(uint8_t action) {
    switch ((XaotOptionsAction) action) {
        case XAOT_OPTIONS_DEFAULT_ELIDED:
            return "default_elided";
        case XAOT_OPTIONS_DEFAULT_FILL_TABLE:
            return "default_fill_table";
        case XAOT_OPTIONS_REQUIRED_CHECK:
            return "required_check";
        case XAOT_OPTIONS_CALLSITE_SPECIALIZED:
            return "callsite_specialized";
        case XAOT_OPTIONS_REJECT:
            return "reject";
    }
    return "unknown";
}

static const XaotOptionsPlan *xicgen_options_plan_for_callsite(const XaotBundle *bundle,
                                                               XgCallsiteId callsite_id) {
    if (!bundle || callsite_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < bundle->noptions_plans; i++) {
        const XaotOptionsPlan *plan = &bundle->options_plans[i];
        if (plan->callsite_id == callsite_id)
            return plan;
    }
    return NULL;
}

static bool xicgen_verify_options_plan_for_call(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    const XgGlobalEvidence *ev = bundle ? bundle->global_evidence_plan.evidence : NULL;
    const XgCallsiteSummary *callsite;
    const XaotOptionsPlan *plan;

    if (!v || v->xg_callsite_id == XG_NO_ID)
        return true;
    callsite = xg_global_evidence_find_callsite(ev, (XgCallsiteId) v->xg_callsite_id);
    plan = xicgen_options_plan_for_callsite(bundle, (XgCallsiteId) v->xg_callsite_id);
    if (!plan) {
        if (!callsite || (callsite->flags & XG_CALL_USES_DEFAULT_ARGS) == 0)
            return true;
        fprintf(stderr, "[xi_cgen] ERROR: missing verified options plan for direct callsite %u\n",
                callsite->callsite_id);
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return false;
    }
    if (!callsite)
        callsite = xg_global_evidence_find_callsite(ev, plan->callsite_id);
    if (!callsite) {
        fprintf(stderr,
                "[xi_cgen] ERROR: options plan %u has no evidence callsite for direct call v%u\n",
                plan->options_id, v->id);
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return false;
    }
    if (plan->action != XAOT_OPTIONS_DEFAULT_ELIDED &&
        plan->action != XAOT_OPTIONS_DEFAULT_FILL_TABLE &&
        plan->action != XAOT_OPTIONS_CALLSITE_SPECIALIZED) {
        fprintf(stderr, "[xi_cgen] ERROR: options plan action %s cannot lower direct callsite %u\n",
                xicgen_options_action_name(plan->action), callsite->callsite_id);
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return false;
    }
    if ((uint16_t) (v->nargs - 1) != (uint16_t) (plan->supplied_count + plan->default_count)) {
        fprintf(stderr,
                "[xi_cgen] ERROR: options plan arity mismatch for direct callsite %u "
                "(xi=%u supplied=%u defaults=%u)\n",
                callsite->callsite_id, (unsigned) (v->nargs - 1), (unsigned) plan->supplied_count,
                (unsigned) plan->default_count);
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return false;
    }

    fprintf(out, "/* options-plan callsite=%u action=%s supplied=%u defaults=%u */ ",
            callsite->callsite_id, xicgen_options_action_name(plan->action),
            (unsigned) plan->supplied_count, (unsigned) plan->default_count);
    return true;
}

static const XaotGenericBodyPlan *xicgen_find_generic_body_plan_for_call(XiCgenCtx *ctx,
                                                                         const XiValue *v) {
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    if (!bundle || !v || v->xg_callsite_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < bundle->ngeneric_body_plans; i++) {
        const XaotGenericBodyPlan *plan = &bundle->generic_body_plans[i];
        if (plan->root_callsite_id == (XgCallsiteId) v->xg_callsite_id)
            return plan;
    }
    return NULL;
}

static bool xicgen_generic_body_call_preflight(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                               const XiValue *v, const XiFunc **target_io,
                                               const char **target_prefix_io) {
    XaotBackendContractIssue issue = XAOT_BACKEND_CONTRACT_OK;
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    const XaotGenericBodyPlan *plan = xicgen_find_generic_body_plan_for_call(ctx, v);
    XgFuncId owner_func_id = f ? (XgFuncId) f->xg_body_func_id : XG_NO_ID;
    XgFuncId desired_body_func_id = XG_NO_ID;
    const XiFunc *target = target_io ? *target_io : NULL;
    const char *target_prefix = target_prefix_io ? *target_prefix_io : NULL;
    XgFuncId target_func_id;
    if (!plan)
        return true;

    switch ((XaotGenericBodyAction) plan->action) {
        case XAOT_GENERIC_BODY_CLONE:
            desired_body_func_id = plan->specialized_body_func_id;
            break;
        case XAOT_GENERIC_BODY_SHARE_CANONICAL_BODY:
        case XAOT_GENERIC_BODY_DIRECT_CONSTRAINT_CALL:
            desired_body_func_id = plan->origin_body_func_id;
            break;
        case XAOT_GENERIC_BODY_REJECT:
            break;
    }
    if (desired_body_func_id != XG_NO_ID) {
        const char *planned_prefix = NULL;
        const XiFunc *planned_target =
            xaot_bundle_find_body_func(bundle, desired_body_func_id, &planned_prefix);
        if (planned_target) {
            target = planned_target;
            target_prefix = planned_prefix;
        }
    }

    target_func_id = target ? (XgFuncId) target->xg_body_func_id : XG_NO_ID;
    if (xaot_backend_contract_generic_body_call_allowed(plan, (XgCallsiteId) v->xg_callsite_id,
                                                        owner_func_id, target_func_id, &issue)) {
        if (target_io)
            *target_io = target;
        if (target_prefix_io)
            *target_prefix_io = target_prefix;
        return true;
    }

    if (ctx)
        ctx->error = true;
    fprintf(stderr,
            "[xi_cgen] ERROR: generic body call preflight failed in '%s' at line %u: "
            "use=%u callsite=%u owner=%u target=%u action=%u issue=%s\n",
            f && f->name ? f->name : "?", (unsigned) v->line, (unsigned) plan->use_id,
            v ? (unsigned) v->xg_callsite_id : 0, (unsigned) owner_func_id,
            (unsigned) target_func_id, (unsigned) plan->action,
            xaot_backend_contract_issue_name(issue));
    emit_codegen_abort_expr(out);
    return false;
}

static void xicgen_call(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                        const char *prefix) {
    XR_DCHECK(v->nargs >= 1, "xicgen_call: need callee");
    XiValue *callee = v->args[0];
    if (xicgen_emit_stdlib_import_call(ctx, out, f, v))
        return;
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

    if (target && cg_func_needs_aot_coro_ctx(ctx, target)) {
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
        const char *class_name = v->type ? xr_type_get_class_name(v->type) : NULL;
        if (!class_name && static_call.class_data)
            class_name = static_call.class_data->class_name;
        if (!class_name && shared_class_data)
            class_name = shared_class_data->class_name;
        fprintf(out, "({ ");
        xicgen_emit_map_instance_alloc(out, class_name);
        emit_fname(ctx, out, call_prefix ? call_prefix : prefix, target);
        fprintf(out, "(NULL, _inst");
        for (uint16_t a = 1; a < v->nargs; a++) {
            fprintf(out, ", ");
            /* Pass each arg in the constructor's actual parameter ABI, not an
             * unconditional tagged box: a non-suspendable constructor uses the
             * native ABI (e.g. int64_t) for scalar params, so boxing here would
             * mismatch the emitted definition and fail C compilation. */
            emit_value_as_rep_ctx(ctx, out, v->args[a], cg_func_param_abi_rep(ctx, target, a));
        }
        fprintf(out, "); _inst; })");
        return;
    }

    if (target && !xicgen_verify_options_plan_for_call(ctx, out, v))
        return;
    if (target && !xicgen_generic_body_call_preflight(ctx, out, f, v, &target, &call_prefix))
        return;

    /* FFI: direct C-ABI call to an extern function. Emit `c_sym(args)` with no
     * hidden _cl closure and arguments converted to their native C reps (the
     * same conversion as a typed direct call, so e.g. tagged -> double). */
    if (target && target->is_extern) {
        const XaotExternDecl *extern_decl = NULL;
        if (!cg_mark_extern_decl_used(ctx, target, &extern_decl)) {
            emit_codegen_abort_expr(out);
            return;
        }
        /* FFI: raw pointers cross the C boundary as real C pointers but are held
         * internally as address-width ints. A pointer return converts from the
         * C pointer to whatever storage rep the planner picked for this value
         * (void* stays bare, i64 casts the address, tagged boxes it); other
         * return reps go through the normal conversion prefix/suffix. */
        const XrType *ret_type = extern_decl->ret_type;
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
        fprintf(out, "xr_ffi_%s(", extern_decl->link_symbol);
        for (uint16_t a = 1; a < v->nargs; a++) {
            if (a > 1)
                fprintf(out, ", ");
            const XrType *pt = (extern_decl->param_types && (a - 1) < extern_decl->nparams)
                                   ? extern_decl->param_types[a - 1]
                                   : NULL;
            const char *p_ptr = cg_extern_ptr_boundary_c_type(pt);
            if (cg_type_is_c_callback(pt)) {
                if (!emit_cfn_callback_arg(ctx, out, f, prefix, v, (uint16_t) (a - 1), pt,
                                           v->args[a]))
                    return;
            } else if (p_ptr) {
                /* A Ptr<T>/MutPtr<T> argument is an address-width int; emit it
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

    /* Default and shared-slot-held class constructors whose callee is a
     * GET_SHARED class object are not recognized by cg_resolve_static_function_call
     * above, but cg_class_native_ctor_call_data resolves them (including classes
     * with only an implicit constructor). Route them to the native constructor
     * expression before the indirect-call path below, which would otherwise treat
     * the class object as a callable value. */
    {
        const XiFunc *ctor_target = NULL;
        const char *ctor_prefix = NULL;
        const XiClassData *ctor_cd =
            cg_class_native_ctor_call_data(ctx, f, v, &ctor_target, &ctor_prefix);
        if (ctor_cd) {
            const char *cprefix = ctor_prefix ? ctor_prefix : call_prefix;
            if (ctor_target) {
                if (emit_class_native_constructor_expr(ctx, out, f, prefix, v, ctor_target,
                                                       cprefix))
                    return;
            } else if (emit_class_native_default_constructor_expr(ctx, out, prefix, v, ctor_cd,
                                                                  cprefix)) {
                return;
            }
        }
    }

    /* First-class CFn callee: invoke the function pointer with native, unboxed ABI
     * (no closure object, no XrValue boxing). The callee value is a raw pointer to the
     * target's NATIVE entry, whose Xray ABI takes a hidden `xrt_closure_t *_cl` first
     * param; a noncapturing function ignores it, so we pass NULL. Casting to the native
     * signature (rather than the C-ABI `_cfn` stub signature) keeps a tail-position
     * `return f(...)` musttail-compatible with the caller's own native entry. */
    {
        const XiValue *cfn_val = cg_unwrap_identity_value(callee);
        if (cfn_val && cfn_val->type && XR_TYPE_IS_C_FUNCTION(cfn_val->type)) {
            const XrType *fn_type = cfn_val->type;
            XrRep ret_rep = cg_cfn_value_storage_rep(fn_type->function.return_type, true);
            const char *conv_suffix =
                emit_conversion_prefix(out, v->type, ret_rep, cg_value_plan_storage_rep(ctx, v));
            fprintf(out, "((%s (*)(xrt_closure_t *",
                    cg_cfn_value_c_type(fn_type->function.return_type, true));
            for (int p = 0; p < fn_type->function.param_count; p++)
                fprintf(out, ", %s",
                        cg_cfn_value_c_type(xr_type_function_param_type(fn_type, p), false));
            fprintf(out, ")) ");
            emit_value_as_rep(out, callee, XR_REP_RAWPTR);
            fprintf(out, ")(NULL");
            for (uint16_t a = 1; a < v->nargs; a++) {
                fprintf(out, ", ");
                const XrType *pt = (int) (a - 1) < fn_type->function.param_count
                                       ? xr_type_function_param_type(fn_type, (int) (a - 1))
                                       : NULL;
                emit_value_as_rep(out, v->args[a], cg_cfn_value_storage_rep(pt, false));
            }
            fprintf(out, ")");
            emit_conversion_suffix(out, conv_suffix);
            return;
        }
    }

    /* Indirect call through a closure value.  A function value carries a
     * canonical descriptor whose sync_entry is the boxed entry point
     * `XrValue (xrt_closure_t *, XrValue...)` (see emit_closure_new_expr), so
     * any proven-sync closure can be invoked by casting sync_entry to that signature, passing the
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
            bool span_result = cg_value_plan_is_span_aggregate(ctx, v);
            const char *conv_suffix =
                span_result ? NULL
                            : emit_conversion_prefix(out, v->type, XR_REP_TAGGED,
                                                     cg_value_plan_storage_rep(ctx, v));
            if (span_result)
                fprintf(out, "xrt_span_from_value_ref(");
            fprintf(out, "({ xrt_closure_t *_icl = (xrt_closure_t *)");
            emit_value_as_rep(out, callee, XR_REP_TAGGED);
            fprintf(out, ".ptr; ((XrValue (*)(xrt_closure_t *");
            for (uint16_t a = 1; a < v->nargs; a++)
                fprintf(out, ", XrValue");
            fprintf(out, ")) _icl->callable->sync_entry)(_icl");
            for (uint16_t a = 1; a < v->nargs; a++) {
                fprintf(out, ", ");
                emit_value_as_rep_ctx(ctx, out, v->args[a], XR_REP_TAGGED);
            }
            fprintf(out, "); })");
            if (span_result)
                fprintf(out, ")");
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

static const XiValue *xicgen_native_int_print_source(XiCgenCtx *ctx, const XiFunc *f,
                                                     const XiValue *v) {
    if (!ctx || !v || v->nargs != 1)
        return NULL;
    const XiValue *arg = v->args[0];
    if (!arg)
        return NULL;
    const XiValue *boxed = NULL;
    if (arg->op == XI_BOX && arg->nargs >= 1) {
        boxed = arg;
        arg = arg->args[0];
    }
    if (boxed && cg_func_needs_aot_coro_ctx(ctx, f) && cg_coro_value_needs_frame(ctx, f, boxed))
        return NULL;
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
    switch (type->scalar_rep) {
        case XR_NATIVE_U8:
        case XR_NATIVE_U16:
        case XR_NATIVE_U32:
        case XR_NATIVE_U64:
        case XR_NATIVE_USIZE:
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
                    xicgen_native_int_print_source(ctx, f, user) == box->args[0]) {
                    saw_print = true;
                    continue;
                }
                return false;
            }
        }
    }
    return saw_print;
}

static void xicgen_emit_print_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v) {
    XR_DCHECK(v->nargs >= 1, "xicgen_emit_print_expr: missing print value");
    int flags = (int) v->aux_int;
    bool add_space = (flags & 1) != 0;
    bool newline = (flags & 2) != 0;
    const XiValue *native_int = xicgen_native_int_print_source(ctx, f, v);
    bool print_span = !native_int && v->args[0] && cg_value_plan_is_span_aggregate(ctx, v->args[0]);

    if (ctx && ctx->freestanding_profile) {
        if (add_space)
            fprintf(out, "(xrt_write_char(' '), ");
        if (native_int) {
            if (xicgen_type_is_unsigned_int(native_int->type)) {
                fprintf(out, "xrt_print_u64((uint64_t)");
                emit_value_as_rep_ctx(ctx, out, native_int, XR_REP_I64);
                fprintf(out, ")");
            } else {
                fprintf(out, "xrt_print_i64((int64_t)");
                emit_value_as_rep_ctx(ctx, out, native_int, XR_REP_I64);
                fprintf(out, ")");
            }
            if (newline)
                fprintf(out, ", xrt_write_char('\\n')");
        } else {
            fprintf(out, "%s(", newline ? "xrt_println" : "xrt_print");
            if (print_span) {
                fprintf(out, "xr_mkptr(");
                if (!emit_span_array_view_ptr_expr(ctx, out, v->args[0])) {
                    ctx->error = true;
                    fprintf(stderr, "[xi_cgen] ERROR: print Slice lacks typed element plan\n");
                    emit_codegen_abort_expr(out);
                }
                fprintf(out, ", XR_TAG_ARRAY)");
            } else {
                emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
            }
            fprintf(out, ")");
        }
        if (add_space)
            fprintf(out, ")");
        return;
    }

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
        if (print_span) {
            fprintf(out, "xr_mkptr(");
            if (!emit_span_array_view_ptr_expr(ctx, out, v->args[0])) {
                ctx->error = true;
                fprintf(stderr, "[xi_cgen] ERROR: print Slice lacks typed element plan\n");
                emit_codegen_abort_expr(out);
            }
            fprintf(out, ", XR_TAG_ARRAY)");
        } else {
            emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        }
        fprintf(out, ")");
    }
    if (add_space)
        fprintf(out, ")");
}

static void xicgen_print(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                         const char *prefix) {
    (void) prefix;
    xicgen_emit_print_expr(ctx, out, f, v);
}

static bool xicgen_verify_json_direct_index_access(XiCgenCtx *ctx, const XiValue *v,
                                                   uint8_t expected_kind, uint8_t alternate_kind) {
    const XaotBundle *bundle;
    const XaotJsonAccessPlan *plan;
    if (!v || v->xg_json_access_id == 0)
        return true;
    bundle = cg_ctx_aot_bundle(ctx);
    plan = xaot_bundle_find_json_access_plan(bundle, v->xg_json_access_id);
    if (!plan) {
        if (ctx)
            ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: missing verified Json access plan for Xi value v%u "
                "(json_access=%u)\n",
                v->id, v->xg_json_access_id);
        return false;
    }
    if (plan->action != XAOT_JSON_ACCESS_DIRECT_INDEX ||
        (plan->access_kind != expected_kind && plan->access_kind != alternate_kind) ||
        plan->field_ordinal != (uint16_t) v->aux_int) {
        if (ctx)
            ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: stale Json direct-index plan for Xi value v%u "
                "(json_access=%u action=%u kind=%u field=%u)\n",
                v->id, v->xg_json_access_id, (unsigned) plan->action, (unsigned) plan->access_kind,
                (unsigned) plan->field_ordinal);
        return false;
    }
    return true;
}

static const XaotJsonAccessPlan *xicgen_find_json_access_plan(XiCgenCtx *ctx, const XiValue *v) {
    const XaotBundle *bundle;
    const XaotJsonAccessPlan *plan;
    if (!v || v->xg_json_access_id == 0)
        return NULL;
    bundle = cg_ctx_aot_bundle(ctx);
    plan = xaot_bundle_find_json_access_plan(bundle, v->xg_json_access_id);
    if (!plan) {
        if (ctx)
            ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: missing verified Json access plan for Xi value v%u "
                "(json_access=%u)\n",
                v->id, v->xg_json_access_id);
        return NULL;
    }
    return plan;
}

static const XaotJsonAccessPlan *xicgen_json_shape_guard_plan(XiCgenCtx *ctx, const XiValue *v,
                                                              uint8_t expected_kind,
                                                              uint8_t alternate_kind) {
    const XaotJsonAccessPlan *plan = xicgen_find_json_access_plan(ctx, v);
    if (!plan)
        return NULL;
    if (plan->access_kind != expected_kind && plan->access_kind != alternate_kind) {
        if (ctx)
            ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: stale Json shape-guard plan kind for Xi value v%u "
                "(json_access=%u action=%u kind=%u)\n",
                v->id, v->xg_json_access_id, (unsigned) plan->action, (unsigned) plan->access_kind);
        return NULL;
    }
    if (plan->action == XAOT_JSON_ACCESS_SHAPE_GUARD_INDEX && plan->field_ordinal != UINT16_MAX &&
        plan->key_name_id != 0)
        return plan;
    if (plan->action == XAOT_JSON_ACCESS_REJECT) {
        if (ctx)
            ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: rejected Json access plan for Xi value v%u "
                "(json_access=%u)\n",
                v->id, v->xg_json_access_id);
    }
    return NULL;
}

static const XaotJsonAccessPlan *
xicgen_json_computed_key_guard_plan(XiCgenCtx *ctx, const XiValue *v, uint8_t expected_kind) {
    const XaotJsonAccessPlan *plan = xicgen_find_json_access_plan(ctx, v);
    if (!plan)
        return NULL;
    if (plan->access_kind != expected_kind) {
        if (ctx)
            ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: stale Json computed-key plan kind for Xi value v%u "
                "(json_access=%u action=%u kind=%u)\n",
                v->id, v->xg_json_access_id, (unsigned) plan->action, (unsigned) plan->access_kind);
        return NULL;
    }
    if (plan->action == XAOT_JSON_ACCESS_COMPUTED_KEY_GUARD)
        return plan;
    if (plan->action == XAOT_JSON_ACCESS_REJECT) {
        if (ctx)
            ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: rejected Json computed-key plan for Xi value v%u "
                "(json_access=%u)\n",
                v->id, v->xg_json_access_id);
    }
    return NULL;
}

static bool xicgen_verify_record_direct_field_access(XiCgenCtx *ctx, const XiValue *v,
                                                     uint8_t expected_kind) {
    const XaotBundle *bundle;
    const XaotRecordAccessPlan *plan;
    if (!v || v->xg_record_access_id == 0)
        return true;
    bundle = cg_ctx_aot_bundle(ctx);
    plan = xaot_bundle_find_record_access_plan(bundle, v->xg_record_access_id);
    if (!plan) {
        if (ctx)
            ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: missing verified Record access plan for Xi value v%u "
                "(record_access=%u)\n",
                v->id, v->xg_record_access_id);
        return false;
    }
    bool direct_field =
        plan->action == XAOT_RECORD_ACCESS_DIRECT_FIELD && plan->access_kind == expected_kind;
    bool destructure_copy = expected_kind == XG_RECORD_ACCESS_FIELD_GET &&
                            plan->action == XAOT_RECORD_ACCESS_COPY_DESTRUCTURE &&
                            plan->access_kind == XG_RECORD_ACCESS_DESTRUCTURE;
    if ((!direct_field && !destructure_copy) || plan->field_ordinal != (uint16_t) v->aux_int) {
        if (ctx)
            ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: stale Record direct-field plan for Xi value v%u "
                "(record_access=%u action=%u kind=%u field=%u)\n",
                v->id, v->xg_record_access_id, (unsigned) plan->action,
                (unsigned) plan->access_kind, (unsigned) plan->field_ordinal);
        return false;
    }
    return true;
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

static void xicgen_go(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                      const char *prefix) {
    CgStaticFunctionCall call;
    const XiFunc *target;
    const char *target_prefix;
    bool target_is_coro;
    bool target_is_sync_go;
    int link_mode;
    bool one_shot_await;
    bool result_copy_shared;
    bool fire_and_forget;
    if (!v || v->nargs < 1) {
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return;
    }
    call = cg_resolve_static_function_call(ctx, f, v->args[0]);
    target = call.func;
    target_prefix = call.prefix ? call.prefix : prefix;
    target_is_coro = target && cg_func_needs_aot_coro_ctx(ctx, target);
    target_is_sync_go = target && cg_func_needs_sync_go_wrapper_ctx(ctx, target);
    if (!target || (!target_is_coro && !target_is_sync_go) ||
        !cg_aot_frame_new_can_supply_cl_arg(f, v->args[0], target) ||
        (v->aux_int & XI_GO_AUX_DEFER_BATCH) != 0) {
        fprintf(stderr, "[xi_cgen] ERROR: unsupported direct root go target\n");
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return;
    }
    link_mode = (int) v->aux_int & 0xff;
    one_shot_await = (v->aux_int & XI_GO_AUX_ONE_SHOT_AWAIT) != 0;
    result_copy_shared = (v->aux_int & XI_GO_AUX_RESULT_COPY_SHARED) != 0;
    fire_and_forget = (v->flags & XI_FLAG_FIRE_AND_FORGET) != 0;
    fprintf(out, "xr_aot_spawn(%s, &", xicgen_aot_context_expr(ctx, f));
    emit_fname_suffix(ctx, out, target_prefix, target, "_aot_desc");
    fprintf(out, ", ");
    emit_fname_suffix(ctx, out, target_prefix, target, "_aot_frame_new");
    fprintf(out, "(");
    emit_aot_frame_new_call_args(ctx, out, f, v->args[0], target, target_is_sync_go, v->args, 1,
                                 v->nargs, v);
    fprintf(out, "), %d, %s, %s, %s, \"%s\").task_value", link_mode,
            fire_and_forget ? "true" : "false", one_shot_await ? "true" : "false",
            result_copy_shared ? "true" : "false", target->name ? target->name : "aot");
}

static void xicgen_emit_json_set_field_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    XR_DCHECK(v->nargs >= 2, "xicgen_emit_json_set_field_expr: missing operands");
    if (v->op == XI_JSON_SET_F &&
        (!xicgen_verify_json_direct_index_access(ctx, v, XG_JSON_ACCESS_FIELD_SET,
                                                 XG_JSON_ACCESS_INDEX_SET) ||
         !xicgen_verify_record_direct_field_access(ctx, v, XG_RECORD_ACCESS_FIELD_SET))) {
        emit_codegen_abort_expr(out);
        return;
    }
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

static const char *xicgen_static_string_const(const XiValue *v) {
    if (!v || v->op != XI_CONST || !v->type || v->type->kind != XR_KIND_STRING)
        return NULL;
    return v->aux ? (const char *) v->aux : "";
}

static void xicgen_emit_json_shape_guard_get(XiCgenCtx *ctx, FILE *out, const XiValue *receiver,
                                             const XaotJsonAccessPlan *plan, const char *field,
                                             const XrType *result_type, XrRep result_rep) {
    const char *conv_suffix = emit_conversion_prefix(out, result_type, XR_REP_TAGGED, result_rep);
    fprintf(out, "xrt_json_get_shape_guard_owned(");
    emit_value_as_rep_ctx(ctx, out, receiver, XR_REP_TAGGED);
    fprintf(out, ", %u, ", (unsigned) plan->field_ordinal);
    xicgen_emit_c_string_literal(out, field ? field : "?");
    fprintf(out, ")");
    emit_conversion_suffix(out, conv_suffix);
}

static void xicgen_emit_json_shape_guard_set(XiCgenCtx *ctx, FILE *out, const XiValue *receiver,
                                             const XiValue *value, const XaotJsonAccessPlan *plan,
                                             const char *field) {
    fprintf(out, "xrt_json_set_shape_guard(");
    emit_value_as_rep_ctx(ctx, out, receiver, XR_REP_TAGGED);
    fprintf(out, ", %u, ", (unsigned) plan->field_ordinal);
    xicgen_emit_c_string_literal(out, field ? field : "?");
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, value, XR_REP_TAGGED);
    fprintf(out, ")");
}

static void xicgen_emit_json_computed_key_guard_get(XiCgenCtx *ctx, FILE *out,
                                                    const XiValue *receiver, const XiValue *key,
                                                    const XrType *result_type, XrRep result_rep) {
    const char *conv_suffix = emit_conversion_prefix(out, result_type, XR_REP_TAGGED, result_rep);
    fprintf(out, "xrt_json_get_computed_key_guard_owned(");
    emit_value_as_rep_ctx(ctx, out, receiver, XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, key, XR_REP_TAGGED);
    fprintf(out, ")");
    emit_conversion_suffix(out, conv_suffix);
}

static void xicgen_emit_json_computed_key_guard_set(XiCgenCtx *ctx, FILE *out,
                                                    const XiValue *receiver, const XiValue *key,
                                                    const XiValue *value) {
    fprintf(out, "xrt_json_set_computed_key_guard(");
    emit_value_as_rep_ctx(ctx, out, receiver, XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, key, XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, value, XR_REP_TAGGED);
    fprintf(out, ")");
}

static void xicgen_emit_json_new_expr(FILE *out, const XiValue *v) {
    int64_t field_count = xi_json_field_count(v);
    uint8_t storage_mode = xi_json_storage_mode(v);
    const char **field_names = (const char **) v->aux;
    const bool is_record = v && v->type && v->type->kind == XR_KIND_RECORD;
    const char *ctor = is_record ? "xrt_record_new" : "xrt_json_new";
    const char *ctor_named = is_record ? "xrt_record_new_named" : "xrt_json_new_named";
    if (storage_mode != XR_OBJ_STORAGE_NORMAL)
        fprintf(out, "xrt_json_set_storage(");
    if (field_count <= 0 || !field_names) {
        fprintf(out, "%s(%" PRId64 ")", ctor, field_count);
        if (storage_mode != XR_OBJ_STORAGE_NORMAL)
            fprintf(out, ", %u)", (unsigned) storage_mode);
        return;
    }
    fprintf(out, "%s(%" PRId64 ", (const char*[]){", ctor_named, field_count);
    for (int64_t i = 0; i < field_count; i++) {
        if (i > 0)
            fprintf(out, ", ");
        xicgen_emit_c_string_literal(out, field_names[i] ? field_names[i] : "?");
    }
    fprintf(out, "})");
    if (storage_mode != XR_OBJ_STORAGE_NORMAL)
        fprintf(out, ", %u)", (unsigned) storage_mode);
}

static void xicgen_json_init_f(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                               const char *prefix) {
    (void) f;
    (void) prefix;
    xicgen_emit_json_set_field_expr(ctx, out, v);
}

static void xicgen_json_get_f(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 1, "xicgen_json_get_f: missing object");
    if (!xicgen_verify_json_direct_index_access(ctx, v, XG_JSON_ACCESS_FIELD_GET,
                                                XG_JSON_ACCESS_INDEX_GET) ||
        !xicgen_verify_record_direct_field_access(ctx, v, XG_RECORD_ACCESS_FIELD_GET)) {
        emit_codegen_abort_expr(out);
        return;
    }
    fprintf(out, "xrt_json_get_field(");
    emit_vref(out, v->args[0]);
    fprintf(out, ", %d)", (int) v->aux_int);
}

static void xicgen_json_set_f(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    (void) f;
    (void) prefix;
    xicgen_emit_json_set_field_expr(ctx, out, v);
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
    int64_t length = xicgen_capacity_arg_or_default(v, 0);
    XrRep storage_rep = xicgen_value_c_storage_rep(ctx, f, v);
    uint8_t storage_mode = xi_value_allocation_storage_mode(v);
    if (storage_mode != XR_OBJ_STORAGE_NORMAL)
        fprintf(out, storage_rep == XR_REP_PTR ? "xrt_array_set_storage_ptr("
                                               : "xrt_array_set_storage(");
    if (storage_rep == XR_REP_PTR) {
        if (!emit_typed_array_new_ptr_expr(ctx, out, f, v, length)) {
            fprintf(out, "(xrt_array_t*)xrt_array_new(");
            if (v->nargs >= 1 && v->args[0] && v->args[0]->op != XI_CONST)
                emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
            else
                fprintf(out, "%" PRId64, length);
            fprintf(out, ").ptr");
        }
    } else if (!emit_typed_array_new_expr(ctx, out, f, v, length)) {
        fprintf(out, "xrt_array_new(");
        if (v->nargs >= 1 && v->args[0] && v->args[0]->op != XI_CONST)
            emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
        else
            fprintf(out, "%" PRId64, length);
        fprintf(out, ")");
    }
    if (storage_mode != XR_OBJ_STORAGE_NORMAL)
        fprintf(out, ", %u)", (unsigned) storage_mode);
}

static const XaotMapShapePlan *xicgen_static_map_shape_plan(XiCgenCtx *ctx, const XiValue *v) {
    const XaotBundle *bundle;
    const XaotMapShapePlan *plan;
    if (!v || v->xg_map_shape_id == XG_NO_ID)
        return NULL;
    bundle = cg_ctx_aot_bundle(ctx);
    plan = bundle ? xaot_bundle_find_map_shape_plan(bundle, v->xg_map_shape_id) : NULL;
    if (!plan || plan->action != XAOT_MAP_SHAPE_READONLY_STATIC_TABLE) {
        if (ctx)
            ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: Map/Set construction has no verified readonly static plan "
                "(shape=%u)\n",
                v->xg_map_shape_id);
        return NULL;
    }
    return plan;
}

static uint32_t xicgen_static_table_slots(uint32_t needed) {
    uint32_t slots = XR_SWISS_GROUP;
    while ((uint64_t) slots * 2u / 3u < needed)
        slots <<= 1u;
    return slots;
}

static const XgMapEntrySummary *
xicgen_static_map_entry(XiCgenCtx *ctx, const XaotMapShapePlan *plan, uint32_t ordinal) {
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    const XgGlobalEvidence *ev = bundle ? bundle->global_evidence_plan.evidence : NULL;
    if (!ev || !plan || ordinal >= plan->entry_count || plan->entry_start == 0 ||
        (uint64_t) plan->entry_start - 1u + ordinal >= ev->nmap_entries) {
        if (ctx)
            ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: readonly static Map/Set entry evidence is stale\n");
        return NULL;
    }
    return &ev->map_entries[plan->entry_start - 1u + ordinal];
}

static bool xicgen_emit_static_map_new(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                       const XaotMapShapePlan *plan) {
    const XiMapLiteralData *data =
        v && v->aux_kind == XI_AUX_KIND_MAP_LITERAL ? (const XiMapLiteralData *) v->aux : NULL;
    if (!data || !plan || data->container_kind != XG_MAP_CONTAINER_MAP ||
        data->count != plan->entry_count || !data->keys || !data->values) {
        if (ctx)
            ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: readonly static Map literal payload is stale\n");
        emit_codegen_abort_expr(out);
        return true;
    }
    uint32_t slots = xicgen_static_table_slots(data->count);
    uint32_t entries_cap = (uint32_t) ((uint64_t) slots * 2u / 3u);
    const char *value_elem = "XR_ELEM_ANY";
    XaotContainerElemPlan value_plan;
    if (v->type && XR_TYPE_IS_MAP(v->type) && v->type->map.value_type &&
        xaot_container_elem_plan_for_type(v->type->map.value_type, &value_plan))
        value_elem = value_plan.elem_name;
    fprintf(out,
            "({ static xrt_map_t _xrt_sm_%u; static uint8_t _xrt_sm_ctrl_%u[%u]; "
            "static int32_t _xrt_sm_indices_%u[%u]; static XrMapEntry _xrt_sm_entries_%u[%u]; "
            "static uint8_t _xrt_sm_init_%u; if (!_xrt_sm_init_%u) { "
            "(void)xrt_map_static_storage_init(&_xrt_sm_%u, _xrt_sm_ctrl_%u, "
            "_xrt_sm_indices_%u, _xrt_sm_entries_%u, %u, %u, %s); ",
            v->id, v->id, slots + XR_SWISS_GROUP, v->id, slots, v->id, entries_cap, v->id, v->id,
            v->id, v->id, v->id, v->id, slots, entries_cap, value_elem);
    for (uint32_t i = 0; i < data->count; i++) {
        const XgMapEntrySummary *entry = xicgen_static_map_entry(ctx, plan, i);
        if (!entry) {
            emit_codegen_abort_expr(out);
            fprintf(out, "; })");
            return true;
        }
        fprintf(out, "xrt_map_set_prehashed(&_xrt_sm_%u, ", v->id);
        emit_value_as_rep_ctx(ctx, out, data->keys[i], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, data->values[i], XR_REP_TAGGED);
        fprintf(out, ", UINT32_C(0x%08" PRIx32 ")); ", (uint32_t) entry->prehash);
    }
    fprintf(out,
            "(void)xrt_map_static_storage_freeze(&_xrt_sm_%u); _xrt_sm_init_%u = 1; } "
            "xr_mkptr(&_xrt_sm_%u, XR_TAG_MAP); })",
            v->id, v->id, v->id);
    return true;
}

static bool xicgen_emit_static_set_new(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                       const XaotMapShapePlan *plan) {
    const XiMapLiteralData *data =
        v && v->aux_kind == XI_AUX_KIND_MAP_LITERAL ? (const XiMapLiteralData *) v->aux : NULL;
    if (!data || !plan || data->container_kind != XG_MAP_CONTAINER_SET ||
        data->count != plan->entry_count || !data->keys) {
        if (ctx)
            ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: readonly static Set literal payload is stale\n");
        emit_codegen_abort_expr(out);
        return true;
    }
    uint32_t slots = xicgen_static_table_slots(data->count);
    uint32_t entries_cap = (uint32_t) ((uint64_t) slots * 2u / 3u);
    fprintf(out,
            "({ static xrt_set_t _xrt_ss_%u; static uint8_t _xrt_ss_ctrl_%u[%u]; "
            "static int32_t _xrt_ss_indices_%u[%u]; static XrSetEntry _xrt_ss_entries_%u[%u]; "
            "static uint8_t _xrt_ss_init_%u; if (!_xrt_ss_init_%u) { "
            "(void)xrt_set_static_storage_init(&_xrt_ss_%u, _xrt_ss_ctrl_%u, "
            "_xrt_ss_indices_%u, _xrt_ss_entries_%u, %u, %u); ",
            v->id, v->id, slots + XR_SWISS_GROUP, v->id, slots, v->id, entries_cap, v->id, v->id,
            v->id, v->id, v->id, v->id, slots, entries_cap);
    for (uint32_t i = 0; i < data->count; i++) {
        const XgMapEntrySummary *entry = xicgen_static_map_entry(ctx, plan, i);
        if (!entry) {
            emit_codegen_abort_expr(out);
            fprintf(out, "; })");
            return true;
        }
        fprintf(out, "(void)xrt_set_add_prehashed(&_xrt_ss_%u, ", v->id);
        emit_value_as_rep_ctx(ctx, out, data->keys[i], XR_REP_TAGGED);
        fprintf(out, ", UINT32_C(0x%08" PRIx32 ")); ", (uint32_t) entry->prehash);
    }
    fprintf(out,
            "(void)xrt_set_static_storage_freeze(&_xrt_ss_%u); _xrt_ss_init_%u = 1; } "
            "xr_mkptr(&_xrt_ss_%u, XR_TAG_SET); })",
            v->id, v->id, v->id);
    return true;
}

static void xicgen_map_new(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                           const char *prefix) {
    (void) f;
    (void) prefix;
    const XaotMapShapePlan *static_plan = xicgen_static_map_shape_plan(ctx, v);
    if (v && v->xg_map_shape_id != XG_NO_ID) {
        if (ctx && ctx->error)
            emit_codegen_abort_expr(out);
        else
            (void) xicgen_emit_static_map_new(ctx, out, v, static_plan);
        return;
    }
    int64_t cap = xicgen_capacity_arg_or_default(v, 8);
    uint8_t flags = (uint8_t) ((v ? v->aux_int : 0) & 0x04);
    uint8_t storage_mode = xi_value_allocation_storage_mode(v);
    if (storage_mode != XR_OBJ_STORAGE_NORMAL)
        fprintf(out, "xrt_map_set_storage(");
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
    if (storage_mode != XR_OBJ_STORAGE_NORMAL)
        fprintf(out, ", %u)", (unsigned) storage_mode);
}

static void xicgen_set_new(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                           const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    const XaotMapShapePlan *static_plan = xicgen_static_map_shape_plan(ctx, v);
    if (v && v->xg_map_shape_id != XG_NO_ID) {
        if (ctx && ctx->error)
            emit_codegen_abort_expr(out);
        else
            (void) xicgen_emit_static_set_new(ctx, out, v, static_plan);
        return;
    }
    int64_t cap = xicgen_capacity_arg_or_default(v, 8);
    uint8_t flags = (uint8_t) ((v ? v->aux_int : 0) & 0x04);
    uint8_t storage_mode = xi_value_allocation_storage_mode(v);
    if (storage_mode != XR_OBJ_STORAGE_NORMAL)
        fprintf(out, "xrt_set_set_storage(");
    if (!flags && !emit_typed_set_new_expr(ctx, out, v, cap))
        fprintf(out, "xrt_set_new(%" PRId64 ")", cap);
    else if (flags)
        fprintf(out, "xrt_set_new_flags(%" PRId64 ", XR_SET_FLAG_WEAK)", cap);
    if (storage_mode != XR_OBJ_STORAGE_NORMAL)
        fprintf(out, ", %u)", (unsigned) storage_mode);
}

static void xicgen_str_concat(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    (void) f;
    (void) prefix;
    emit_str_concat_expr(ctx, out, v);
}

static void xicgen_as(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                      const char *prefix) {
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 1, "xicgen_as: need arg");

    const XaotValuePlan *value_plan = cg_value_plan(ctx, v);
    const XaotValuePlan *arg_plan = cg_value_plan(ctx, v->args[0]);
    if (v->args[0] && v->args[0]->op == XI_ERR_CATCH && value_plan && arg_plan &&
        (value_plan->rep.flags & XAOT_VALUE_FLAG_ENUM) != 0 &&
        xaot_value_reps_equal(value_plan->rep, arg_plan->rep)) {
        emit_vref(out, v->args[0]);
        return;
    }

    bool is_safe = (v->aux_int & 1) != 0;
    int32_t tid = (int32_t) (v->aux_int >> 1);
    if (tid < 0) {
        /* An unresolved named/generic cast is a semantic identity, but it is
         * not necessarily a representation identity.  Native class fields
         * and parameters can be planned as PTR while XI_AS remains TAGGED.
         * Consume the verified value plan here so the representation boundary
         * is explicit (for example PTR -> xrt_box_obj) instead of emitting an
         * ill-typed C initializer. */
        emit_value_as_rep_ctx(ctx, out, v->args[0], cg_value_plan_storage_rep(ctx, v));
        return;
    }

    if (!is_safe) {
        switch (tid) {
            case 8:
                fprintf(out, "xrt_to_int(");
                emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
                fprintf(out, ")");
                return;
            case 11:
                fprintf(out, "xrt_to_float(");
                emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
                fprintf(out, ")");
                return;
            case 12:
                if (xicgen_type_is_unsigned_int(v->args[0] ? v->args[0]->type : NULL)) {
                    fprintf(out, "xrt_uint64_to_string((uint64_t)");
                    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
                    fprintf(out, ")");
                } else {
                    fprintf(out, "xrt_to_string(");
                    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
                    fprintf(out, ")");
                }
                return;
            case 1:
                fprintf(out, "xrt_to_bool(");
                emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
                fprintf(out, ")");
                return;
            default:
                break;
        }
    }

    fprintf(out, "({ XrValue _as = ");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
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

    if (cg_value_plan_is_span_aggregate(ctx, v)) {
        if (cg_value_plan_is_span_aggregate(ctx, arg)) {
            emit_vref(out, arg);
            return;
        }
        if (cg_type_is_byte_slice(v->type)) {
            fprintf(out, "xrt_byte_slice_from_value(");
            emit_value_as_rep_ctx(ctx, out, arg, XR_REP_TAGGED);
            fprintf(out, ", XR_ERROR_CORE_BYTE_SLICE_ARG_EXPECTS_MSG)");
            return;
        }
    }

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

static bool xicgen_emit_full_fixed_array_span(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    if (!ctx || !out || !v || v->nargs < 3)
        return false;

    const XiValue *source = cg_unwrap_identity_value(v->args[0]);
    CgFixedArrayLaneInfo fixed;
    int64_t start = 0;
    int64_t end = 0;
    if (!source || !cg_fixed_array_lane_info_from_value(source, &fixed) || !fixed.ctype ||
        !cg_const_int_value(v->args[1], &start) || !cg_const_int_value(v->args[2], &end) ||
        start != 0 || (end != INT64_MAX && end != (int64_t) fixed.count))
        return false;

    /* A full view of a fixed array has compile-time layout and range.  Keep
     * the descriptor as an ordinary value; materializing an XrValue array-ref
     * only to feed xrt_span_from_array_slice would obscure both facts from C
     * optimization and introduce a spurious runtime boundary. */
    fprintf(out, "((xr_span_t){.data = (void *)(");
    emit_value_as_rep_ctx(ctx, out, source, XR_REP_RAWPTR);
    fprintf(out, "), .length = INT64_C(%u)})", (unsigned) fixed.count);
    return true;
}

static void xicgen_slice(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                         const char *prefix) {
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 3, "xicgen_slice: need source, start, and end");
    if (cg_value_plan_is_span_aggregate(ctx, v)) {
        if (xicgen_emit_full_fixed_array_span(ctx, out, v))
            return;
        fprintf(out, "({ XrValue _xr_slice_start = ");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
        fprintf(out, "; XrValue _xr_slice_end = ");
        emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_TAGGED);
        fprintf(out, "; if (!XR_IS_INT(_xr_slice_start) || !XR_IS_INT(_xr_slice_end)) "
                     "xrt_throw_error(XR_ERR_TYPE_MISMATCH, "
                     "XR_ERROR_CORE_SLICE_BOUNDS_EXPECTS_MSG); ");
        if (cg_value_plan_is_span_aggregate(ctx, v->args[0])) {
            CgArrayElemInfo elem;
            if (!cg_span_elem_info_from_value(ctx, v->args[0], &elem) || !elem.ctype) {
                cg_ctx_set_error(ctx);
                emit_codegen_abort_expr(out);
                return;
            }
            fprintf(out, "xrt_span_from_span_slice(");
            emit_vref(out, v->args[0]);
            fprintf(out,
                    ", XR_TO_INT(_xr_slice_start), XR_TO_INT(_xr_slice_end), "
                    "(uint16_t)sizeof(%s)); })",
                    elem.ctype);
            return;
        } else {
            fprintf(out, "xrt_span_from_array_slice(");
            emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        }
        fprintf(out, ", XR_TO_INT(_xr_slice_start), XR_TO_INT(_xr_slice_end)); })");
        return;
    }
    fprintf(out, "xrt_slice(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_TAGGED);
    fprintf(out, ")");
}

static void xicgen_slice_from_ptr(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                  const char *prefix) {
    (void) f;
    (void) prefix;
    XR_DCHECK(v && v->nargs == 3, "xicgen_slice_from_ptr: need pointer, count, owner");
    uint16_t elem_size = (uint16_t) ((v->aux_int >> 8) & 0xffff);
    uint16_t alignment = (uint16_t) ((v->aux_int >> 32) & 0xffff);
    fprintf(out, "({ const void *_p = ");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_RAWPTR);
    fprintf(out, "; int64_t _n = ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
    fprintf(out,
            "; if (XR_UNLIKELY(_n < 0)) xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "
            "\"mem.slice<T>() count must be non-negative\"); "
            "if (XR_UNLIKELY(_n > 0 && !_p)) xrt_throw_error(XR_ERR_TYPE_MISMATCH, "
            "\"mem.slice<T>() requires a non-null pointer for non-zero count\"); "
            "if (XR_UNLIKELY(_n > 0 && ((uintptr_t)_p %% UINT16_C(%u)) != 0)) "
            "xrt_throw_error(XR_ERR_TYPE_MISMATCH, "
            "\"mem.slice<T>() pointer does not satisfy T alignment\"); "
            "if (XR_UNLIKELY(_n > 0 && (uint64_t)_n > UINTPTR_MAX / UINT16_C(%u))) "
            "xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "
            "\"mem.slice<T>() byte range overflows address space\"); "
            "(xr_span_t){.data = (void *)_p, .length = _n}; })",
            (unsigned) alignment, (unsigned) elem_size);
}

static bool xicgen_emit_buffer_materialize_fields(FILE *out, const XrAggregateLayout *layout,
                                                  uint32_t base, unsigned depth,
                                                  const char *destination) {
    if (!out || !layout || depth > 16)
        return false;
    for (uint16_t i = 0; i < layout->field_count; i++) {
        const XrAggregateFieldLayout *field = &layout->fields[i];
        if (field->is_flexible || field->offset > layout->total_size ||
            field->size > layout->total_size - field->offset)
            return false;
        uint32_t offset = base + field->offset;
        if (field->native_type == XR_NATIVE_NESTED_AGGREGATE) {
            if (!field->sub_layout || field->sub_layout->total_size != field->size ||
                !xicgen_emit_buffer_materialize_fields(out, field->sub_layout, offset, depth + 1,
                                                       destination))
                return false;
        } else {
            fprintf(out,
                    "memcpy(%s + UINT32_C(%u), "
                    "(const uint8_t *)_xr_buf->data + UINT32_C(%u), UINT32_C(%u)); ",
                    destination, offset, offset, field->size);
        }
    }
    return true;
}

static const char *cg_ffi_pointee_c_type(XiCgenCtx *ctx, uint8_t code);
static bool cg_ffi_code_is_float(uint8_t code);
static uint8_t cg_ffi_code_width(XiCgenCtx *ctx, uint8_t code);

static void xicgen_buffer_materialize(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                      const char *prefix) {
    (void) prefix;
    if (!ctx || !out || !f || !v || v->nargs != 1 || !v->args[0]) {
        emit_codegen_abort_expr(out);
        return;
    }
    uint8_t code = XI_BUFFER_MATERIALIZE_CODE(v->aux_int);
    uint32_t size = XI_BUFFER_MATERIALIZE_SIZE(v->aux_int);
    uint16_t align = XI_BUFFER_MATERIALIZE_ALIGN(v->aux_int);
    const XrAggregateLayout *layout = (const XrAggregateLayout *) v->aux;
    const char *ctype = local_ctype_str_ctx(ctx, f, v);
    if (!ctype || !ctype[0] || size == 0 || align == 0 ||
        (code == XI_BUFFER_MATERIALIZE_AGGREGATE && !layout)) {
        cg_ctx_set_error(ctx);
        emit_codegen_abort_expr(out);
        return;
    }

    fprintf(out, "({ XrValue _xr_buffer = ");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out,
            "; xrt_buffer_object_t *_xr_buf = xrt_buffer_obj_ptr(_xr_buffer); "
            "if (XR_UNLIKELY(!_xr_buf || _xr_buf->length != INT64_C(%u) || "
            "_xr_buf->align != (size_t)UINT16_C(%u) || !_xr_buf->data || "
            "((uintptr_t)_xr_buf->data %% (uintptr_t)UINT16_C(%u)) != 0)) "
            "xrt_throw_error(XR_ERR_TYPE_MISMATCH, "
            "\"mem.assumeInitialized<T>() Buffer size/alignment evidence mismatch\"); ",
            size, (unsigned) align, (unsigned) align);
    if (code == XI_BUFFER_MATERIALIZE_AGGREGATE) {
        XrRep destination_rep = cg_value_plan_storage_rep(ctx, v);
        char aggregate_type[128];
        const char *field_destination = "(uint8_t *)&_xr_out";
        if (destination_rep == XR_REP_TAGGED) {
            cg_struct_heap_type_name(aggregate_type, sizeof(aggregate_type), prefix, layout);
            fprintf(out,
                    "%s *_xr_out = (%s *)xrt_arc_alloc(sizeof(%s)); "
                    "memset(_xr_out, 0, sizeof(%s)); ",
                    aggregate_type, aggregate_type, aggregate_type, aggregate_type);
            field_destination = "(uint8_t *)_xr_out";
        } else {
            fprintf(out, "%s _xr_out = {0}; ", ctype);
        }
        bool emitted = layout->total_size == size &&
                       xicgen_emit_buffer_materialize_fields(out, layout, 0, 0, field_destination);
        if (!emitted) {
            cg_ctx_set_error(ctx);
            fprintf(out, "xrt_throw_error(XR_ERR_TYPE_MISMATCH, "
                         "\"mem.assumeInitialized<T>() invalid aggregate field layout\"); ");
        }
        if (destination_rep == XR_REP_TAGGED)
            fprintf(out,
                    "xrt_release(_xr_buffer); xr_aggregate_ref(_xr_out, "
                    "(uint16_t)sizeof(%s)); })",
                    aggregate_type);
        else
            fprintf(out, "xrt_release(_xr_buffer); _xr_out; })");
        return;
    }

    uint8_t width = cg_ffi_code_width(ctx, code);
    XrRep from_rep = cg_ffi_code_is_float(code) ? XR_REP_F64 : XR_REP_I64;
    if (cg_ffi_code_is_float(code)) {
        if ((XrFFIType) code == XR_FFI_T_F32)
            fprintf(out, "double _xr_scalar = (double)xr_raw_f32_from_bits("
                         "xr_raw_load_u32_unaligned(_xr_buf->data)); ");
        else
            fprintf(out, "double _xr_scalar = xr_raw_f64_from_bits("
                         "xr_raw_load_u64_unaligned(_xr_buf->data)); ");
    } else {
        fprintf(out,
                "int64_t _xr_scalar = (int64_t)(%s)xr_raw_load_u%u_unaligned("
                "_xr_buf->data); ",
                cg_ffi_pointee_c_type(ctx, code), (unsigned) width * 8u);
    }
    fprintf(out, "xrt_release(_xr_buffer); ");
    const char *suffix =
        emit_conversion_prefix(out, v->type, from_rep, cg_value_plan_storage_rep(ctx, v));
    fprintf(out, "_xr_scalar");
    emit_conversion_suffix(out, suffix);
    fprintf(out, "; })");
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
        const char *name;   /* owned: static string literal (table below) */
        const char *c_name; /* owned: static string literal (libm symbol) */
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
        xicgen_emit_print_expr(ctx, out, f, v);
    } else if (strcmp(bn, "str_concat") == 0) {
        xicgen_str_concat(ctx, out, f, v, prefix);
    } else if (strcmp(bn, "array_new") == 0) {
        xicgen_array_new(ctx, out, f, v, prefix);
    } else if (emit_array_bytes_builtin_expr(ctx, out, f, v, bn)) {
        /* Expression emitted by the array/bytes helper. */
    } else if (strcmp(bn, "string_byte_slice") == 0) {
        XR_DCHECK(v->nargs >= 1, "builtin string_byte_slice: need string arg");
        if (cg_value_plan_is_span_aggregate(ctx, v)) {
            fprintf(out, "xrt_span_from_string_bytes(");
            emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
            fprintf(out, ")");
        } else {
            const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED,
                                                             cg_value_plan_storage_rep(ctx, v));
            fprintf(out, "xrt_str_to_bytes(");
            emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
            fprintf(out, ")");
            emit_conversion_suffix(out, conv_suffix);
        }
    } else if (strcmp(bn, "StringBuilder") == 0) {
        fprintf(out, "xrt_strbuf_new()");
    } else if (strcmp(bn, "map_new") == 0) {
        xicgen_map_new(ctx, out, f, v, prefix);
    } else if (strcmp(bn, "set_new") == 0) {
        xicgen_set_new(ctx, out, f, v, prefix);
    } else if (strcmp(bn, "json_new") == 0) {
        xicgen_emit_json_new_expr(out, v);
    } else if (strcmp(bn, "json_init_f") == 0 || strcmp(bn, "json_set_f") == 0) {
        xicgen_emit_json_set_field_expr(ctx, out, v);
    } else if (strcmp(bn, "json_get_f") == 0) {
        if (!xicgen_verify_json_direct_index_access(ctx, v, XG_JSON_ACCESS_FIELD_GET,
                                                    XG_JSON_ACCESS_INDEX_GET) ||
            !xicgen_verify_record_direct_field_access(ctx, v, XG_RECORD_ACCESS_FIELD_GET)) {
            emit_codegen_abort_expr(out);
            return;
        }
        fprintf(out, "xrt_json_get_field(");
        emit_vref(out, v->args[0]);
        fprintf(out, ", %d)", (int) v->aux_int);
    } else if (strcmp(bn, "copy") == 0) {
        /* copy(x): the sole explicit deep-copy operation. */
        XR_DCHECK(v->nargs >= 1, "builtin copy: need arg");
        if (cg_value_plan_is_span_aggregate(ctx, v->args[0])) {
            CgArrayElemInfo copy_info;
            if (!cg_span_elem_info_from_value(ctx, v->args[0], &copy_info) || !copy_info.ctype ||
                !copy_info.elem_name) {
                cg_ctx_set_error(ctx);
                emit_codegen_abort_expr(out);
                return;
            }
            if (cg_value_plan_is_span_aggregate(ctx, v)) {
                /* Slice is always a borrowed data+length view.  copy(Slice) materializes
                 * Array<T>; an owned Slice result would hide an untracked owner root. */
                cg_ctx_set_error(ctx);
                emit_codegen_abort_expr(out);
                return;
            }
            const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED,
                                                             cg_value_plan_storage_rep(ctx, v));
            fprintf(out, "xrt_span_to_owned_array(");
            emit_vref(out, v->args[0]);
            fprintf(out, ", (uint8_t)%s, (uint16_t)sizeof(%s), UINT8_C(0), UINT8_C(%u))",
                    copy_info.elem_name, copy_info.ctype, copy_info.rep == XR_REP_TAGGED ? 1u : 0u);
            emit_conversion_suffix(out, conv_suffix);
            return;
        }
        const XiClassData *clone_class =
            cg_class_data_for_type_name(ctx, v->args[0] ? v->args[0]->type : NULL);
        if (clone_class && (clone_class->derive_flags & XR_DERIVE_CLONE) != 0) {
            const XaotDerivedClonePlan *clone_plan =
                cg_class_native_derived_clone_plan(ctx, clone_class, true);
            const char *clone_prefix = cg_class_native_prefix_for_data(ctx, clone_class, prefix);
            XrRep result_rep = cg_value_plan_storage_rep(ctx, v);
            if (!clone_plan || !clone_prefix ||
                (clone_class->instance_layout && result_rep != XR_REP_PTR &&
                 result_rep != XR_REP_TAGGED) ||
                (!clone_class->instance_layout && result_rep != XR_REP_TAGGED)) {
                cg_ctx_set_error(ctx);
                emit_codegen_abort_expr(out);
                return;
            }
            if (!clone_class->instance_layout) {
                emit_class_boxed_clone_name(out, clone_prefix, clone_class);
                fprintf(out, "(");
                emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
                fprintf(out, ")");
                return;
            }
            if (result_rep == XR_REP_TAGGED)
                fprintf(out, "xrt_box_obj(");
            emit_class_native_clone_name(out, clone_prefix, clone_class);
            fprintf(out, "(");
            emit_class_native_instance_base_ref(ctx, out, f, v->args[0]);
            fprintf(out, ")");
            if (result_rep == XR_REP_TAGGED)
                fprintf(out, ")");
            return;
        }
        bool is_json = v->args[0] && XR_TYPE_HAS_OBJECT_SHAPE(v->args[0]->type);
        uint8_t storage_mode = xi_value_allocation_storage_mode(v);
        const char *conv_suffix = emit_tagged_to_value_storage_prefix(ctx, out, v);
        if (storage_mode != XR_OBJ_STORAGE_NORMAL)
            fprintf(out, "xrt_value_set_storage(");
        fprintf(out, "%s(", is_json ? "xrt_json_clone_for_coro" : "xrt_value_clone_for_coro");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ")");
        if (storage_mode != XR_OBJ_STORAGE_NORMAL)
            fprintf(out, ", %u)", (unsigned) storage_mode);
        emit_conversion_suffix(out, conv_suffix);
    } else if (strcmp(bn, "chr") == 0) {
        XR_DCHECK(v->nargs >= 1, "builtin chr: need arg");
        fprintf(out, "xrt_chr(");
        emit_boxed_value_ref(out, v->args[0]);
        fprintf(out, ")");
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
    } else if (strcmp(bn, "typeOf") == 0) {
        xicgen_typeid(ctx, out, f, v, prefix);
    } else if (strcmp(bn, "typeName") == 0) {
        xicgen_typename(ctx, out, f, v, prefix);
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
    fprintf(out, "%s(", time_helper);
    if (cg_time_module_helper_has_tagged_arg(time_helper))
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ")");
    if (cg_rep(v) == XR_REP_I64 || cg_rep(v) == XR_REP_F64)
        fprintf(out, ")");
    return true;
}

static bool xicgen_emit_typed_array_method(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                           const XiValue *v, const char *prefix, const char *method,
                                           uint16_t nargs) {
    if (nargs == 1 &&
        cg_call_method_matches_receiver_registry_id(v, XA_BUILTIN_RECEIVER_METHOD_ARRAY_PUSH) &&
        emit_typed_array_push_expr(ctx, out, f, prefix, v, v->args[0], v->args[1]))
        return true;
    if (nargs == 2 &&
        cg_call_method_matches_receiver_registry_id(v, XA_BUILTIN_RECEIVER_METHOD_ARRAY_SET) &&
        emit_typed_array_set_unchecked_expr(ctx, out, f, prefix, v))
        return true;
    if (nargs == 1 &&
        cg_call_method_matches_receiver_registry_id(v, XA_BUILTIN_RECEIVER_METHOD_ARRAY_RESERVE) &&
        emit_typed_array_reserve_expr(ctx, out, f, prefix, v))
        return true;
    if (nargs >= 1 && nargs <= 2 &&
        cg_call_method_matches_receiver_registry_id(v, XA_BUILTIN_RECEIVER_METHOD_ARRAY_RESIZE) &&
        emit_typed_array_resize_zero_expr(ctx, out, f, prefix, v))
        return true;
    if (nargs == 1 &&
        cg_call_method_matches_receiver_registry_id(
            v, XA_BUILTIN_RECEIVER_METHOD_U8_ARRAY_APPEND_FROM) &&
        emit_byte_array_append_from_expr(ctx, out, f, prefix, v))
        return true;
    if (nargs == 2 &&
        cg_call_method_matches_receiver_registry_id(
            v, XA_BUILTIN_RECEIVER_METHOD_U8_ARRAY_REPEAT_FROM) &&
        emit_byte_array_repeat_from_expr(ctx, out, f, prefix, v))
        return true;
    if (nargs >= 1 && nargs <= 3 &&
        cg_call_method_matches_receiver_registry_id(v, XA_BUILTIN_RECEIVER_METHOD_ARRAY_FILL) &&
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

static bool xicgen_emit_net_handle_method(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                          const XiValue *v, const char *method, uint16_t nargs) {
    if (!method || nargs != 0 || !v || v->nargs < 1)
        return false;
    const char *recv_class = cg_class_native_receiver_class_name(ctx, f, v->args[0]);
    if (!recv_class)
        return false;

    const char *helper = NULL;
    if (strcmp(recv_class, "NetConn") == 0) {
        if (strcmp(method, "fd") == 0)
            helper = "xrt_net_fd";
        else if (strcmp(method, "close") == 0)
            helper = "xrt_net_close";
        else if (strcmp(method, "isClosed") == 0)
            helper = "xrt_net_is_closed";
        else if (strcmp(method, "isTLS") == 0)
            helper = "xrt_net_is_tls";
    } else if (strcmp(recv_class, "NetListener") == 0) {
        if (strcmp(method, "fd") == 0)
            helper = "xrt_net_fd";
        else if (strcmp(method, "port") == 0)
            helper = "xrt_net_listener_port";
        else if (strcmp(method, "close") == 0)
            helper = "xrt_net_close";
        else if (strcmp(method, "isClosed") == 0)
            helper = "xrt_net_is_closed";
    }
    if (!helper)
        return false;

    const char *conv_suffix = emit_tagged_to_value_storage_prefix(ctx, out, v);
    fprintf(out, "%s(", helper);
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ")");
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool xicgen_emit_enum_method(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                    const char *method) {
    const XiEnumData *recv_enum = cg_enum_for_namespace_value(v->args[0]);
    if (!recv_enum)
        recv_enum = cg_enum_for_shared_value_in_func(ctx, f, v->args[0]);
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
    } else if (recv_enum->is_adt && cg_value_plan_is_aggregate(ctx, v)) {
        emit_adt_enum_construct_expr(ctx, out, recv_enum, enum_member, v);
    } else if (emit_static_enum_member_value_expr(ctx, out, v, recv_enum, (uint32_t) enum_member)) {
        /* Static enum variants do not need the namespace map. */
    } else {
        if (ctx && ctx->freestanding_profile) {
            fprintf(stderr,
                    "[xi_cgen] ERROR: freestanding enum member '%s.%s' needs static lowering\n",
                    recv_enum->name ? recv_enum->name : "?", method ? method : "?");
            ctx->error = true;
            emit_codegen_abort_expr(out);
            return true;
        }
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
    const XiEnumData *recv_enum = cg_enum_for_namespace_value(v->args[0]);
    if (!recv_enum)
        recv_enum = cg_enum_for_shared_value_in_func(ctx, f, v->args[0]);
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
    if (cg_func_needs_aot_coro_ctx(ctx, mfunc)) {
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
    if (mfunc->is_vararg) {
        uint16_t fixed = mfunc->nparams;
        for (uint16_t a = 0; a < fixed && a < v->nargs; a++) {
            fprintf(out, ", ");
            emit_value_as_direct_call_arg(ctx, out, f, v, mfunc, a, v->args[a]);
        }
        emit_vararg_method_rest_arg(ctx, out, f, v, mfunc);
    } else {
        for (uint16_t a = 0; a < v->nargs; a++) {
            fprintf(out, ", ");
            emit_value_as_direct_call_arg(ctx, out, f, v, mfunc, a, v->args[a]);
        }
    }
    fprintf(out, ")");
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool xicgen_path_is_parallel_stdlib_module(const char *file) {
    if (!file)
        return false;
    const char *suffixes[] = {"stdlib/parallel/parallel.xr",
                              "<embedded stdlib>/parallel/parallel.xr"};
    for (uint8_t i = 0; i < (uint8_t) (sizeof(suffixes) / sizeof(suffixes[0])); i++) {
        size_t flen = strlen(file);
        size_t slen = strlen(suffixes[i]);
        if (flen >= slen && strcmp(file + flen - slen, suffixes[i]) == 0)
            return true;
    }
    return false;
}

static bool xicgen_class_data_has_instance_method(const XiClassData *class_data,
                                                  const char *method) {
    if (!class_data || !class_data->methods || !method)
        return false;
    for (uint16_t mi = 0; mi < class_data->nmethod; mi++) {
        const XiClassMethod *m = &class_data->methods[mi];
        if (!m->is_static && m->name && strcmp(m->name, method) == 0)
            return true;
    }
    return false;
}

static bool xicgen_class_data_is_parallel_plan(XiCgenCtx *ctx, const XiClassData *class_data) {
    if (!class_data)
        return false;
    const char *name =
        class_data->generic_origin_name ? class_data->generic_origin_name : class_data->class_name;
    if (!name || strcmp(name, "Plan") != 0)
        return false;
    const char *required_methods[] = {"_begin", "_end",    "_stateFor", "forEach",
                                      "map",    "mapInto", "reduce",    "close"};
    for (uint8_t i = 0; i < (uint8_t) (sizeof(required_methods) / sizeof(required_methods[0]));
         i++) {
        if (!xicgen_class_data_has_instance_method(class_data, required_methods[i]))
            return false;
    }
    if (class_data->class_info &&
        xicgen_path_is_parallel_stdlib_module(class_data->class_info->location.file))
        return true;
    const XiModule *decl_module = cg_class_native_decl_module_for_data(ctx, class_data);
    if (decl_module && xicgen_path_is_parallel_stdlib_module(decl_module->path))
        return true;
    const XiModule *slot_module = cg_class_native_module_for_data(ctx, class_data);
    return slot_module && xicgen_path_is_parallel_stdlib_module(slot_module->path);
}

static const XiClassData *xicgen_parallel_plan_ctor_result_class(XiCgenCtx *ctx, const XiFunc *f,
                                                                 const XiValue *value,
                                                                 uint8_t depth) {
    if (!ctx || !value || depth > 8)
        return NULL;
    const XiValue *v = cg_unwrap_identity_value(value);
    while (v && ((xi_copy_is_identity_alias(v) || xi_op_is_identity_forward(v->op) ||
                  v->op == XI_BOX || v->op == XI_UNBOX || cg_class_native_copy_wrapper(v)) &&
                 v->nargs >= 1)) {
        if (++depth > 8)
            return NULL;
        v = cg_unwrap_identity_value(v->args[0]);
    }
    if (!v || v->nargs < 1)
        return NULL;
    CgStaticFunctionCall call;
    memset(&call, 0, sizeof(call));
    const XiClassData *class_data = NULL;
    if (v->op == XI_CALL) {
        const XiValue *callee = cg_unwrap_identity_value(v->args[0]);
        call = cg_resolve_static_function_call(ctx, f, callee);
        if (call.is_class_constructor)
            class_data = call.class_data;
        if (!class_data)
            class_data = xicgen_shared_class_data(ctx, callee);
        if (!class_data && callee && callee->op == XI_CLASS_CREATE && callee->aux)
            class_data = (const XiClassData *) callee->aux;
        if (!class_data)
            class_data = cg_class_native_class_value_data(ctx, f, callee);
        if (!class_data)
            class_data = cg_class_data_for_type_name(ctx, v->type);
    } else if (v->op == XI_CALL_METHOD && v->aux) {
        call = cg_resolve_module_member_call(ctx, f, v, (const char *) v->aux);
        if (call.is_class_constructor)
            class_data = call.class_data;
        if (!class_data && strcmp((const char *) v->aux, "constructor") == 0)
            class_data = cg_class_native_class_value_data(ctx, f, v->args[0]);
        if (!class_data && strcmp((const char *) v->aux, "constructor") == 0)
            class_data = cg_class_data_for_type_name(ctx, v->type);
    }
    return xicgen_class_data_is_parallel_plan(ctx, class_data) ? class_data : NULL;
}

typedef struct {
    uint16_t set_count;
    bool invalid;
    const XiClassData *class_data;
} XicgenParallelPlanSlotInit;

static void xicgen_scan_parallel_plan_slot_init(XiCgenCtx *ctx, const XiFunc *f, int slot,
                                                XicgenParallelPlanSlotInit *out) {
    if (!ctx || !f || !out || out->invalid)
        return;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v || v->op != XI_SET_SHARED || (int) v->aux_int != slot || v->nargs < 1)
                continue;
            out->set_count++;
            if (out->set_count > 1) {
                out->invalid = true;
                return;
            }
            const XiClassData *class_data =
                xicgen_parallel_plan_ctor_result_class(ctx, f, v->args[0], 0);
            if (!class_data) {
                out->invalid = true;
                return;
            }
            out->class_data = class_data;
        }
    }
    for (uint16_t ci = 0; ci < f->nchildren; ci++)
        xicgen_scan_parallel_plan_slot_init(ctx, f->children[ci], slot, out);
}

static const XiClassData *xicgen_parallel_plan_shared_slot_init_class(XiCgenCtx *ctx,
                                                                      const XiFunc *f, int slot) {
    const XiModule *module = f && f->module ? f->module : (ctx ? ctx->module : NULL);
    int nshared = module && module->init ? (int) module->init->nshared : 0;
    if (!ctx || !module || !module->init || slot < 0 || slot >= nshared)
        return NULL;
    XicgenParallelPlanSlotInit init;
    memset(&init, 0, sizeof(init));
    xicgen_scan_parallel_plan_slot_init(ctx, module->init, slot, &init);
    return !init.invalid && init.set_count == 1 ? init.class_data : NULL;
}

static const XiClassData *xicgen_parallel_plan_class_for_call(XiCgenCtx *ctx, const XiFunc *f,
                                                              const XiValue *v) {
    if (!ctx || !v || v->nargs != 1 || !v->args || !v->args[0])
        return NULL;
    const XiValue *receiver = cg_unwrap_identity_value(v->args[0]);
    const XiClassData *class_data =
        cg_class_native_instance_data(ctx, f, receiver ? receiver : v->args[0]);
    if (!xicgen_class_data_is_parallel_plan(ctx, class_data))
        class_data = xicgen_shared_class_data(ctx, receiver ? receiver : v->args[0]);
    if (!xicgen_class_data_is_parallel_plan(ctx, class_data) && receiver &&
        receiver->op == XI_GET_SHARED) {
        class_data = xicgen_parallel_plan_shared_slot_init_class(ctx, f, (int) receiver->aux_int);
    }
    if (!xicgen_class_data_is_parallel_plan(ctx, class_data))
        class_data = cg_class_data_for_type_name(ctx, receiver ? receiver->type : v->args[0]->type);
    if (!xicgen_class_data_is_parallel_plan(ctx, class_data)) {
        const char *recv_class =
            cg_class_native_receiver_class_name(ctx, f, receiver ? receiver : v->args[0]);
        class_data = recv_class ? cg_class_native_data_by_name(ctx, recv_class) : NULL;
    }
    return xicgen_class_data_is_parallel_plan(ctx, class_data) ? class_data : NULL;
}

static void xicgen_accumulate_parallel_plan_class(XiCgenCtx *ctx, const XiClassData *class_data,
                                                  const XiClassData **concrete,
                                                  const XiClassData **skeleton, bool *ambiguous) {
    if (!xicgen_class_data_is_parallel_plan(ctx, class_data) || !concrete || !skeleton ||
        !ambiguous)
        return;
    if (!class_data->is_monomorphized) {
        if (!*skeleton)
            *skeleton = class_data;
        return;
    }
    if (!*concrete) {
        *concrete = class_data;
        return;
    }
    if (!cg_class_native_data_matches(*concrete, class_data))
        *ambiguous = true;
}

static const XiClassData *xicgen_find_parallel_plan_class_data(XiCgenCtx *ctx) {
    if (!ctx)
        return NULL;
    const XiClassData *concrete = NULL;
    const XiClassData *skeleton = NULL;
    bool ambiguous = false;
    for (int mi = 0; mi < ctx->all_nmodules; mi++) {
        const XiModule *module = ctx->all_modules ? ctx->all_modules[mi] : NULL;
        if (!module)
            continue;
        for (uint16_t ci = 0; ci < module->nclasses; ci++) {
            const XiClassData *class_data = module->classes ? module->classes[ci] : NULL;
            xicgen_accumulate_parallel_plan_class(ctx, class_data, &concrete, &skeleton,
                                                  &ambiguous);
        }
        for (uint16_t si = 0; si < module->nslots; si++) {
            const XiClassData *class_data = module->slot_classes ? module->slot_classes[si] : NULL;
            xicgen_accumulate_parallel_plan_class(ctx, class_data, &concrete, &skeleton,
                                                  &ambiguous);
        }
    }
    for (int i = 0; i < ctx->nimports; i++) {
        const XiClassData *class_data = ctx->imports[i].target_class;
        xicgen_accumulate_parallel_plan_class(ctx, class_data, &concrete, &skeleton, &ambiguous);
    }
    return ambiguous ? NULL : (concrete ? concrete : skeleton);
}

static const XiFunc *xicgen_parallel_plan_lifecycle_target(XiCgenCtx *ctx,
                                                           const XiClassData *class_data,
                                                           const char *method,
                                                           const char **out_prefix) {
    if (out_prefix)
        *out_prefix = NULL;
    if (!ctx || !class_data || !method)
        return NULL;
    const XiModule *decl_module = cg_class_native_decl_module_for_data(ctx, class_data);
    if (decl_module && decl_module->init && class_data->methods) {
        for (uint16_t mi = 0; mi < class_data->nmethod; mi++) {
            const XiClassMethod *m = &class_data->methods[mi];
            if (m->is_static || !m->name || strcmp(m->name, method) != 0)
                continue;
            const XiFunc *target =
                cg_lookup_method_in_class_data(class_data, decl_module->init, (int) mi);
            if (target) {
                if (out_prefix)
                    *out_prefix = decl_module->name;
                return target;
            }
        }
    }
    for (int i = 0; i < ctx->nimports; i++) {
        const CgImportEntry *imp = &ctx->imports[i];
        if (!cg_class_native_data_matches(imp->target_class, class_data) || !imp->exporter_func)
            continue;
        for (uint16_t mi = 0; mi < class_data->nmethod; mi++) {
            const XiClassMethod *m = &class_data->methods[mi];
            if (m->is_static || !m->name || strcmp(m->name, method) != 0)
                continue;
            const XiFunc *target =
                cg_lookup_method_in_class_data(class_data, imp->exporter_func, (int) mi);
            if (target) {
                if (out_prefix)
                    *out_prefix = imp->target_mod_name;
                return target;
            }
        }
    }
    return NULL;
}

static bool xicgen_emit_parallel_plan_lifecycle_method(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                       const XiValue *v, const char *prefix,
                                                       const char *method, uint16_t nargs) {
    if (!method || nargs != 0 ||
        (strcmp(method, "_begin") != 0 && strcmp(method, "_end") != 0 &&
         strcmp(method, "close") != 0))
        return false;
    const XiClassData *class_data = xicgen_parallel_plan_class_for_call(ctx, f, v);
    if (class_data && class_data->is_generic_skeleton &&
        (v->lowering_flags & XI_LOWERING_FLAG_PARALLEL_PLAN_LIFECYCLE)) {
        const XiClassData *closed_world = xicgen_find_parallel_plan_class_data(ctx);
        if (closed_world && closed_world->is_monomorphized)
            class_data = closed_world;
    }
    if (!class_data && (v->lowering_flags & XI_LOWERING_FLAG_PARALLEL_PLAN_LIFECYCLE) &&
        v->xg_callsite_id == XG_NO_ID && v->xg_method_id == XG_NO_ID)
        class_data = xicgen_find_parallel_plan_class_data(ctx);
    if (!class_data)
        return false;
    const char *method_prefix = NULL;
    const XiFunc *target =
        xicgen_parallel_plan_lifecycle_target(ctx, class_data, method, &method_prefix);
    if (!target) {
        ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: verified parallel.Plan lifecycle method '%s' has no direct "
                "target at line %u\n",
                method, (unsigned) v->line);
        emit_codegen_abort_expr(out);
        return true;
    }
    return xicgen_emit_direct_method(ctx, out, f, v, prefix, target, method_prefix);
}

static const XgClassSummary *xicgen_dispatch_evidence_class(const XaotBundle *bundle,
                                                            XgClassId class_id) {
    const XgGlobalEvidence *ev = bundle ? bundle->global_evidence_plan.evidence : NULL;
    if (!ev || class_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < ev->nclasses; i++) {
        if (ev->classes[i].class_id == class_id)
            return &ev->classes[i];
    }
    return NULL;
}

static bool xicgen_class_data_name_matches_id(const XiClassData *class_data, uint32_t name_id);

static const XgGlobalEvidence *xicgen_global_evidence(XiCgenCtx *ctx) {
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    return bundle ? bundle->global_evidence_plan.evidence : NULL;
}

static const XgMethodSummary *xicgen_evidence_method(const XgGlobalEvidence *ev,
                                                     XgMethodId method_id) {
    if (!ev || method_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < ev->nmethods; i++) {
        if (ev->methods[i].method_id == method_id)
            return &ev->methods[i];
    }
    return NULL;
}

static const XgBodySummary *xicgen_evidence_body_for_func(const XgGlobalEvidence *ev,
                                                          const XiFunc *func) {
    if (!ev || !func || func->xg_body_func_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < ev->nbodies; i++) {
        if (ev->bodies[i].func_id == (XgFuncId) func->xg_body_func_id)
            return &ev->bodies[i];
    }
    return NULL;
}

static bool xicgen_evidence_interface_extends_reaches(const XgGlobalEvidence *ev,
                                                      XgInterfaceId from, XgInterfaceId target,
                                                      uint32_t depth) {
    if (!ev || from == XG_NO_ID || target == XG_NO_ID || depth > 64)
        return false;
    for (uint32_t i = 0; i < ev->ninterface_extends; i++) {
        const XgInterfaceExtendsSummary *edge = &ev->interface_extends[i];
        if (edge->child_interface_id != from)
            continue;
        if (edge->parent_interface_id == target)
            return true;
        if (xicgen_evidence_interface_extends_reaches(ev, edge->parent_interface_id, target,
                                                      depth + 1))
            return true;
    }
    return false;
}

static bool xicgen_evidence_interface_impl_matches(const XgGlobalEvidence *ev,
                                                   XgInterfaceId implementor_interface,
                                                   XgInterfaceId receiver_interface) {
    return implementor_interface == receiver_interface ||
           xicgen_evidence_interface_extends_reaches(ev, implementor_interface, receiver_interface,
                                                     0);
}

static bool xicgen_evidence_class_implements_interface(const XgGlobalEvidence *ev,
                                                       XgClassId class_id,
                                                       XgInterfaceId interface_id) {
    if (!ev || class_id == XG_NO_ID || interface_id == XG_NO_ID)
        return false;
    for (uint32_t i = 0; i < ev->ninterface_impls; i++) {
        const XgInterfaceImplSummary *impl = &ev->interface_impls[i];
        if (impl->implementor_class_id == class_id &&
            xicgen_evidence_interface_impl_matches(ev, impl->interface_id, interface_id))
            return true;
    }
    return false;
}

static const XgInterfaceMethodSummary *
xicgen_evidence_interface_method_for_slot(const XgGlobalEvidence *ev, XgInterfaceId interface_id,
                                          uint32_t slot) {
    if (!ev || interface_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < ev->ninterface_methods; i++) {
        const XgInterfaceMethodSummary *method = &ev->interface_methods[i];
        if (method->ordinal == slot && (method->owner_interface_id == interface_id ||
                                        xicgen_evidence_interface_extends_reaches(
                                            ev, interface_id, method->owner_interface_id, 0)))
            return method;
    }
    return NULL;
}

static const XgMethodSummary *
xicgen_evidence_find_method_by_signature_in_class(const XgGlobalEvidence *ev,
                                                  const XgClassSummary *cls, uint32_t name_id,
                                                  uint32_t signature_key) {
    if (!ev || !cls || cls->method_start == 0 || name_id == 0 || signature_key == 0)
        return NULL;
    for (uint32_t i = 0; i < cls->method_count; i++) {
        uint32_t idx = cls->method_start - 1 + i;
        if (idx >= ev->nmethods)
            return NULL;
        const XgMethodSummary *method = &ev->methods[idx];
        if (method->owner_class_id == cls->class_id && method->name_id == name_id &&
            method->signature_key == signature_key)
            return method;
    }
    return NULL;
}

static const XgMethodSummary *xicgen_evidence_find_method_by_signature_in_hierarchy(
    const XgGlobalEvidence *ev, XgClassId class_id, uint32_t name_id, uint32_t signature_key) {
    if (!ev || class_id == XG_NO_ID)
        return NULL;
    for (;;) {
        const XgClassSummary *cur = NULL;
        for (uint32_t i = 0; i < ev->nclasses; i++) {
            if (ev->classes[i].class_id == class_id) {
                cur = &ev->classes[i];
                break;
            }
        }
        if (!cur)
            return NULL;
        const XgMethodSummary *method =
            xicgen_evidence_find_method_by_signature_in_class(ev, cur, name_id, signature_key);
        if (method)
            return method;
        if (cur->parent_class_id == XG_NO_ID || cur->parent_class_id == class_id)
            return NULL;
        class_id = cur->parent_class_id;
    }
}

static bool xicgen_class_data_name_matches_id(const XiClassData *class_data, uint32_t name_id);

static XgClassId xicgen_class_id_for_data(XiCgenCtx *ctx, const XiClassData *cd) {
    const XgGlobalEvidence *ev = xicgen_global_evidence(ctx);
    if (!ev || !cd)
        return XG_NO_ID;
    for (uint32_t i = 0; i < ev->nclasses; i++) {
        if (xicgen_class_data_name_matches_id(cd, ev->classes[i].name_id))
            return ev->classes[i].class_id;
    }
    return XG_NO_ID;
}

static bool xicgen_func_is_boxed_dispatch_target(XiCgenCtx *ctx, const XiFunc *func) {
    const XgGlobalEvidence *ev = xicgen_global_evidence(ctx);
    const XgBodySummary *body = xicgen_evidence_body_for_func(ev, func);
    const XgMethodSummary *method = body ? xicgen_evidence_method(ev, body->owner_method_id) : NULL;
    if (!ev || !body || body->kind != XG_BODY_METHOD || !method ||
        method->owner_class_id == XG_NO_ID)
        return false;

    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    for (uint32_t i = 0; bundle && i < bundle->nmethod_dispatch_plans; i++) {
        const XaotMethodDispatchPlan *plan = &bundle->method_dispatch_plans[i];
        if ((plan->kind != XAOT_DISPATCH_ITABLE && plan->kind != XAOT_DISPATCH_TYPE_SWITCH) ||
            plan->receiver_static_interface_id == XG_NO_ID || plan->dispatch_slot == UINT32_MAX)
            continue;
        const XgInterfaceMethodSummary *iface_method = xicgen_evidence_interface_method_for_slot(
            ev, plan->receiver_static_interface_id, plan->dispatch_slot);
        if (!iface_method || iface_method->name_id != method->name_id ||
            iface_method->signature_key != method->signature_key)
            continue;
        if (xicgen_evidence_class_implements_interface(ev, method->owner_class_id,
                                                       plan->receiver_static_interface_id))
            return true;
    }
    return false;
}

static bool xicgen_class_data_name_matches_id(const XiClassData *class_data, uint32_t name_id) {
    if (!class_data || name_id == 0)
        return false;
    if (xg_name_id(class_data->class_name) == name_id)
        return true;
    if (xg_name_id(class_data->display_name) == name_id)
        return true;
    return xg_name_id(class_data->generic_origin_name) == name_id;
}

static const XiClassData *xicgen_find_class_data_in_module(const XiModule *module,
                                                           const XgClassSummary *class_summary) {
    if (!module || !class_summary || class_summary->name_id == 0)
        return NULL;
    for (uint16_t i = 0; i < module->nclasses; i++) {
        const XiClassData *cd = module->classes ? module->classes[i] : NULL;
        if (xicgen_class_data_name_matches_id(cd, class_summary->name_id))
            return cd;
    }
    return NULL;
}

static const XiClassData *xicgen_find_dispatch_class_data(XiCgenCtx *ctx, const XaotBundle *bundle,
                                                          XgClassId class_id) {
    const XgClassSummary *class_summary = xicgen_dispatch_evidence_class(bundle, class_id);
    if (!ctx || !class_summary)
        return NULL;
    for (uint32_t mi = 0; bundle && mi < bundle->nmodules; mi++) {
        const XiModule *module = bundle->modules ? bundle->modules[mi] : NULL;
        if (class_summary->module_id != 0 && class_summary->module_id != mi + 1)
            continue;
        const XiClassData *cd = xicgen_find_class_data_in_module(module, class_summary);
        if (cd)
            return cd;
    }
    if (ctx->module) {
        const XiClassData *cd = xicgen_find_class_data_in_module(ctx->module, class_summary);
        if (cd)
            return cd;
    }
    for (int i = 0; i < ctx->all_nmodules; i++) {
        const XiModule *module = ctx->all_modules ? ctx->all_modules[i] : NULL;
        const XiClassData *cd = xicgen_find_class_data_in_module(module, class_summary);
        if (cd)
            return cd;
    }
    for (int i = 0; i < ctx->nimports; i++) {
        const XiClassData *cd = ctx->imports[i].target_class;
        if (xicgen_class_data_name_matches_id(cd, class_summary->name_id))
            return cd;
    }
    return NULL;
}

static bool xicgen_emit_planned_type_switch_method(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                   const XiValue *v, const char *prefix,
                                                   const XaotMethodDispatchPlan *dispatch_plan) {
    const XaotBundle *bundle;
    bool void_like;
    XaotBackendContractIssue issue = XAOT_BACKEND_CONTRACT_OK;
    if (!dispatch_plan || dispatch_plan->kind != XAOT_DISPATCH_TYPE_SWITCH)
        return false;
    if (dispatch_plan->receiver_static_interface_id == XG_NO_ID)
        return false;
    bundle = cg_ctx_aot_bundle(ctx);
    if (!xaot_backend_dispatch_plan_target_range_valid(bundle, dispatch_plan, &issue)) {
        ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: verified AOT type-switch dispatch plan at line %u has no "
                "target cases\n",
                (unsigned) v->line);
        emit_codegen_abort_expr(out);
        return true;
    }

    void_like = cg_is_void_like(v);
    fprintf(out, "({ XrValue _xr_ts_recv_%u = ", v->id);
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out, "; bool _xr_ts_matched_%u = false; ", v->id);
    if (!void_like) {
        fprintf(out,
                "%s _xr_ts_result_%u; memset(&_xr_ts_result_%u, 0, sizeof(_xr_ts_result_%u)); ",
                local_ctype_str_ctx(ctx, f, v), v->id, v->id, v->id);
    }

    for (uint16_t i = 0; i < dispatch_plan->target_count; i++) {
        const XaotDispatchTargetCase *target =
            &bundle->dispatch_target_cases[dispatch_plan->target_start - 1 + i];
        const XiClassData *target_class =
            xicgen_find_dispatch_class_data(ctx, bundle, target->receiver_class_id);
        const char *target_prefix = NULL;
        const XiFunc *target_func =
            xaot_bundle_find_dispatch_target_func(bundle, target, &target_prefix);
        if (!target_class || !target_func) {
            ctx->error = true;
            fprintf(stderr,
                    "[xi_cgen] ERROR: verified AOT type-switch target %u/%u for method '%s' at "
                    "line %u has no Xi class/function\n",
                    (unsigned) target->receiver_class_id, (unsigned) target->method_id,
                    v->aux ? (const char *) v->aux : "?", (unsigned) v->line);
            emit_codegen_abort_expr(out);
            return true;
        }
        fprintf(out, "%s (xrt_instanceof(_xr_ts_recv_%u, (uint16_t)", i == 0 ? "if" : "else if",
                v->id);
        if (!emit_class_native_type_id_expr(ctx, out, target_class)) {
            ctx->error = true;
            fprintf(stderr,
                    "[xi_cgen] ERROR: verified AOT type-switch class %u for method '%s' at line "
                    "%u has no native type id\n",
                    (unsigned) target->receiver_class_id, v->aux ? (const char *) v->aux : "?",
                    (unsigned) v->line);
            emit_codegen_abort_expr(out);
            return true;
        }
        fprintf(out, ")) { ");
        if (void_like) {
            fprintf(out, "(void)(");
            if (!xicgen_emit_direct_method(ctx, out, f, v, prefix, target_func, target_prefix)) {
                ctx->error = true;
                emit_codegen_abort_expr(out);
                return true;
            }
            fprintf(out, "); ");
        } else {
            fprintf(out, "_xr_ts_result_%u = ", v->id);
            if (!xicgen_emit_direct_method(ctx, out, f, v, prefix, target_func, target_prefix)) {
                ctx->error = true;
                emit_codegen_abort_expr(out);
                return true;
            }
            fprintf(out, "; ");
        }
        fprintf(out, "_xr_ts_matched_%u = true; } ", v->id);
    }
    fprintf(out,
            "if (!_xr_ts_matched_%u) { fprintf(stderr, \"xray AOT: verified interface "
            "type-switch dispatch missed\\n\"); abort(); } ",
            v->id);
    if (void_like)
        fprintf(out, "XR_NULL_VAL; })");
    else
        fprintf(out, "_xr_ts_result_%u; })", v->id);
    return true;
}

static bool xicgen_emit_planned_itable_method(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                              const XiValue *v, const char *prefix,
                                              const XaotMethodDispatchPlan *dispatch_plan) {
    const XaotBundle *bundle;
    bool void_like;
    XaotBackendContractIssue issue = XAOT_BACKEND_CONTRACT_OK;
    (void) f;
    (void) prefix;
    if (!dispatch_plan || dispatch_plan->kind != XAOT_DISPATCH_ITABLE)
        return false;
    if (dispatch_plan->receiver_static_interface_id == XG_NO_ID)
        return false;
    bundle = cg_ctx_aot_bundle(ctx);
    if (!xaot_backend_contract_check_mandatory_dispatch(
            bundle, dispatch_plan, XAOT_BACKEND_DISPATCH_SUPPORT_ITABLE, &issue)) {
        ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: verified AOT itable dispatch plan at line %u is not "
                "emittable: %s\n",
                (unsigned) v->line, xaot_backend_contract_issue_name(issue));
        emit_codegen_abort_expr(out);
        return true;
    }

    void_like = cg_is_void_like(v);
    fprintf(out, "({ XrValue _xr_it_recv_%u = ", v->id);
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out, "; XrtMethodFn _xr_it_fn_%u = xrt_itable_method(_xr_it_recv_%u, %uu, %uu); ",
            v->id, v->id, (unsigned) dispatch_plan->receiver_static_interface_id,
            (unsigned) dispatch_plan->dispatch_slot);
    fprintf(out, "XrValue _xr_it_raw_%u = ((XrValue (*)(xrt_closure_t *", v->id);
    for (uint16_t a = 0; a < v->nargs; a++)
        fprintf(out, ", XrValue");
    fprintf(out, "))_xr_it_fn_%u)(NULL, _xr_it_recv_%u", v->id, v->id);
    for (uint16_t a = 1; a < v->nargs; a++) {
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, v->args[a], XR_REP_TAGGED);
    }
    fprintf(out, "); ");
    if (void_like) {
        fprintf(out, "(void)_xr_it_raw_%u; ", v->id);
        fprintf(out, "XR_NULL_VAL; })");
    } else if (cg_value_plan_is_span_aggregate(ctx, v)) {
        fprintf(out, "xrt_span_from_value_ref(_xr_it_raw_%u); })", v->id);
    } else {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        fprintf(out, "_xr_it_raw_%u", v->id);
        emit_conversion_suffix(out, conv_suffix);
        fprintf(out, "; })");
    }
    return true;
}

static bool xicgen_emit_vtable_target_method(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                             const XiValue *v, const char *prefix,
                                             const XiFunc *target_func, const char *target_prefix) {
    if (cg_func_needs_aot_coro_ctx(ctx, target_func)) {
        ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: unsupported AOT sync vtable method call to suspendable function "
                "'%s'\n",
                target_func->name ? target_func->name : "?");
        emit_codegen_abort_expr(out);
        return true;
    }
    if (cg_class_func_uses_native_receiver(ctx, target_func)) {
        CgClassNativeFunc target_info = cg_class_native_func(ctx, target_func);
        const char *conv_suffix =
            emit_direct_call_return_conversion_prefix(ctx, out, f, v, target_func);
        if (!target_info.class_data) {
            ctx->error = true;
            emit_codegen_abort_expr(out);
            return true;
        }
        if (ctx->error) {
            emit_codegen_abort_expr(out);
            return true;
        }
        emit_fname(ctx, out, target_prefix ? target_prefix : prefix, target_func);
        fprintf(out, "(NULL, (");
        emit_class_native_type_name(
            out,
            cg_class_native_prefix_for_data(ctx, target_info.class_data,
                                            target_prefix ? target_prefix : prefix),
            target_info.class_data->class_name);
        fprintf(out, "*)_xr_vt_recv_%u.ptr", v->id);
        for (uint16_t a = 1; a < v->nargs; a++) {
            fprintf(out, ", ");
            emit_value_as_rep_ctx(ctx, out, v->args[a], cg_func_param_abi_rep(ctx, target_func, a));
        }
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }
    return xicgen_emit_direct_method(ctx, out, f, v, prefix, target_func, target_prefix);
}

static bool xicgen_emit_planned_vtable_method(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                              const XiValue *v, const char *prefix,
                                              const XaotMethodDispatchPlan *dispatch_plan) {
    const XaotBundle *bundle;
    bool void_like;
    XaotBackendContractIssue issue = XAOT_BACKEND_CONTRACT_OK;
    if (!dispatch_plan || dispatch_plan->kind != XAOT_DISPATCH_VTABLE)
        return false;
    if (dispatch_plan->receiver_static_class_id == XG_NO_ID)
        return false;
    bundle = cg_ctx_aot_bundle(ctx);
    if (!xaot_backend_dispatch_plan_target_range_valid(bundle, dispatch_plan, &issue)) {
        ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: verified AOT vtable dispatch plan at line %u has no target "
                "cases\n",
                (unsigned) v->line);
        emit_codegen_abort_expr(out);
        return true;
    }

    void_like = cg_is_void_like(v);
    fprintf(out, "({ XrValue _xr_vt_recv_%u = ", v->id);
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out, "; bool _xr_vt_matched_%u = false; ", v->id);
    if (!void_like) {
        fprintf(out,
                "%s _xr_vt_result_%u; memset(&_xr_vt_result_%u, 0, sizeof(_xr_vt_result_%u)); ",
                local_ctype_str_ctx(ctx, f, v), v->id, v->id, v->id);
    }

    for (uint16_t i = 0; i < dispatch_plan->target_count; i++) {
        const XaotDispatchTargetCase *target =
            &bundle->dispatch_target_cases[dispatch_plan->target_start - 1 + i];
        const XiClassData *target_class =
            xicgen_find_dispatch_class_data(ctx, bundle, target->receiver_class_id);
        const char *target_prefix = NULL;
        const XiFunc *target_func =
            xaot_bundle_find_dispatch_target_func(bundle, target, &target_prefix);
        if (!target_class || !target_func) {
            ctx->error = true;
            fprintf(stderr,
                    "[xi_cgen] ERROR: verified AOT vtable target %u/%u for method '%s' at line %u "
                    "has no Xi class/function\n",
                    (unsigned) target->receiver_class_id, (unsigned) target->method_id,
                    v->aux ? (const char *) v->aux : "?", (unsigned) v->line);
            emit_codegen_abort_expr(out);
            return true;
        }
        fprintf(out, "%s (xrt_instance_exact_type(_xr_vt_recv_%u, (uint16_t)",
                i == 0 ? "if" : "else if", v->id);
        if (!emit_class_native_type_id_expr(ctx, out, target_class)) {
            ctx->error = true;
            fprintf(stderr,
                    "[xi_cgen] ERROR: verified AOT vtable class %u for method '%s' at line %u has "
                    "no native type id\n",
                    (unsigned) target->receiver_class_id, v->aux ? (const char *) v->aux : "?",
                    (unsigned) v->line);
            emit_codegen_abort_expr(out);
            return true;
        }
        /* R2-3: classes whose constructors fall back to the map-backed
         * instance form (xicgen_emit_map_instance_alloc) carry only a
         * class-name string — no registered type id — so the exact-tid probe
         * alone can never match them and the dispatch aborted on every
         * polymorphic call. Match the map form by exact class name too, the
         * same dual-form identity rule the user-hash machinery already uses
         * (xrt_user_hash_eq_exact). Native-form instances keep matching via
         * the tid probe. */
        fprintf(out, ") || xrt_map_backed_class_exact(_xr_vt_recv_%u, ", v->id);
        xicgen_emit_c_string_literal(out,
                                     target_class->class_name ? target_class->class_name : "?");
        fprintf(out, ")) { ");
        if (void_like) {
            fprintf(out, "(void)(");
            if (!xicgen_emit_vtable_target_method(ctx, out, f, v, prefix, target_func,
                                                  target_prefix)) {
                ctx->error = true;
                emit_codegen_abort_expr(out);
                return true;
            }
            fprintf(out, "); ");
        } else {
            fprintf(out, "_xr_vt_result_%u = ", v->id);
            if (!xicgen_emit_vtable_target_method(ctx, out, f, v, prefix, target_func,
                                                  target_prefix)) {
                ctx->error = true;
                emit_codegen_abort_expr(out);
                return true;
            }
            fprintf(out, "; ");
        }
        fprintf(out, "_xr_vt_matched_%u = true; } ", v->id);
    }
    fprintf(out,
            "if (!_xr_vt_matched_%u) { fprintf(stderr, \"xray AOT: verified class vtable dispatch "
            "missed\\n\"); abort(); } ",
            v->id);
    if (void_like)
        fprintf(out, "XR_NULL_VAL; })");
    else
        fprintf(out, "_xr_vt_result_%u; })", v->id);
    return true;
}

static bool xicgen_emit_planned_direct_method(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                              const XiValue *v, const char *prefix,
                                              const XaotMethodDispatchPlan *dispatch_plan) {
    const XaotBundle *bundle;
    const XaotDispatchTargetCase *target;
    const XiFunc *target_func;
    const char *target_prefix = NULL;
    XaotBackendContractIssue issue = XAOT_BACKEND_CONTRACT_OK;
    if (!dispatch_plan || dispatch_plan->kind != XAOT_DISPATCH_DIRECT)
        return false;
    if (dispatch_plan->receiver_static_class_id == XG_NO_ID &&
        dispatch_plan->receiver_static_interface_id == XG_NO_ID)
        return false;
    bundle = cg_ctx_aot_bundle(ctx);
    if (!xaot_backend_dispatch_plan_target_range_valid(bundle, dispatch_plan, &issue)) {
        ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: verified AOT direct dispatch plan at line %u has no target\n",
                (unsigned) v->line);
        emit_codegen_abort_expr(out);
        return true;
    }
    target = &bundle->dispatch_target_cases[dispatch_plan->target_start - 1];
    target_func = xaot_bundle_find_dispatch_target_func(bundle, target, &target_prefix);
    if (!target_func) {
        ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: verified AOT direct dispatch target %u for method '%s' at "
                "line %u has no Xi function\n",
                (unsigned) target->method_id, v->aux ? (const char *) v->aux : "?",
                (unsigned) v->line);
        emit_codegen_abort_expr(out);
        return true;
    }
    return xicgen_emit_direct_method(ctx, out, f, v, prefix, target_func, target_prefix);
}

static bool xicgen_is_stringbuilder_receiver(const XiValue *v) {
    const XrType *type = v ? v->type : NULL;
    return type && type->kind == XR_KIND_INSTANCE && type->instance.class_name &&
           strcmp(type->instance.class_name, "StringBuilder") == 0;
}

static const XaotCapacityPlan *xicgen_stringbuilder_capacity_plan(XiCgenCtx *ctx,
                                                                  const XiValue *v) {
    if (!ctx || !v || v->xg_capacity_op_id == XG_NO_ID)
        return NULL;
    return xaot_bundle_find_capacity_plan(cg_ctx_aot_bundle(ctx),
                                          (XgCapacityOpId) v->xg_capacity_op_id);
}

static const XiValue *xicgen_stringbuilder_append_arg(const XiValue *v) {
    const XiValue *arg = v && v->nargs >= 2 ? v->args[1] : NULL;
    return cg_unwrap_identity_value(arg);
}

/* UTF-8 byte length of a Unicode scalar; 0 for surrogate / out-of-range code
 * points. Mirrors xrt_rune_utf8_encode's accepted range so the compile-time
 * reserve stays byte-exact with the runtime append. */
static int xicgen_rune_scalar_utf8_len(int64_t cp) {
    if (cp < 0)
        return 0;
    if (cp <= 0x7F)
        return 1;
    if (cp <= 0x7FF)
        return 2;
    if (cp <= 0xFFFF)
        return (cp >= 0xD800 && cp <= 0xDFFF) ? 0 : 3;
    if (cp <= 0x10FFFF)
        return 4;
    return 0;
}

/* Exact formatted byte length for a StringBuilder.append literal argument whose
 * length is a compile-time constant: string, rune, bool, or null. out_is_string
 * distinguishes the string fast path (append_string_no_grow) from the scalar
 * path (append_scalar_no_grow). Kept in sync with the producer's
 * body_string_builder_append_has_exact_count and the runtime formatting. */
static bool xicgen_stringbuilder_exact_append_len(const XiValue *arg, int64_t *out_length,
                                                  bool *out_is_string) {
    if (!arg || arg->op != XI_CONST || !arg->type)
        return false;
    switch (arg->type->kind) {
        case XR_KIND_STRING: {
            const char *literal = xicgen_static_string_const(arg);
            if (!literal)
                return false;
            size_t length = strlen(literal);
            if (length > (size_t) INT64_MAX)
                return false;
            if (out_length)
                *out_length = (int64_t) length;
            if (out_is_string)
                *out_is_string = true;
            return true;
        }
        case XR_KIND_RUNE: {
            int n = xicgen_rune_scalar_utf8_len(arg->aux_int);
            if (n <= 0)
                return false;
            if (out_length)
                *out_length = n;
            if (out_is_string)
                *out_is_string = false;
            return true;
        }
        case XR_KIND_BOOL:
            if (out_length)
                *out_length = arg->aux_int ? 4 : 5;
            if (out_is_string)
                *out_is_string = false;
            return true;
        case XR_KIND_NULL:
            if (out_length)
                *out_length = 4;
            if (out_is_string)
                *out_is_string = false;
            return true;
        default:
            return false;
    }
}

static bool xicgen_stringbuilder_literal_append_plan(XiCgenCtx *ctx, const XiValue *v,
                                                     int64_t *out_length) {
    const XaotCapacityPlan *plan = xicgen_stringbuilder_capacity_plan(ctx, v);
    const XiValue *arg = xicgen_stringbuilder_append_arg(v);
    const uint32_t required = XAOT_CAPACITY_EV_GLOBAL_ROW | XAOT_CAPACITY_EV_RECEIVER_TYPE |
                              XAOT_CAPACITY_EV_ELEM_TYPE | XAOT_CAPACITY_EV_EXACT_COUNT |
                              XAOT_CAPACITY_EV_MAY_GROW;
    int64_t length = 0;
    if (!v || (v->op != XI_CALL_METHOD && v->op != XI_CALL_METHOD_DIRECT) || v->nargs != 2 ||
        !xicgen_is_stringbuilder_receiver(v->args[0]) ||
        !cg_method_name_is(v, "append", cg_method_sym("append")) || !plan ||
        plan->sequence_kind != XG_SEQ_STRING_BUILDER || plan->op_kind != XG_CAPACITY_APPEND ||
        plan->action != XAOT_CAPACITY_RESERVE_ONCE || plan->count_expr_id == 0 ||
        (plan->evidence & required) != required ||
        !xicgen_stringbuilder_exact_append_len(arg, &length, NULL))
        return false;
    if (out_length)
        *out_length = length;
    return true;
}

static const XiValue *xicgen_stringbuilder_receiver_origin(const XiValue *receiver) {
    const XiValue *cur = receiver;
    for (uint32_t depth = 0; cur && depth < 64; depth++) {
        cur = cg_unwrap_identity_value(cur);
        if (!cur || (cur->op != XI_CALL_METHOD && cur->op != XI_CALL_METHOD_DIRECT) ||
            cur->nargs < 1 || !xicgen_is_stringbuilder_receiver(cur->args[0]) ||
            !cg_method_name_is(cur, "append", cg_method_sym("append")))
            return cur;
        cur = cur->args[0];
    }
    return cur;
}

static bool xicgen_stringbuilder_chain_gap_value(const XiValue *v) {
    return v &&
           (v->op == XI_CONST || v->op == XI_ERR_CHECK || v->op == XI_BOX || v->op == XI_UNBOX ||
            xi_op_is_identity_forward(v->op) || xi_copy_is_identity_alias(v));
}

static bool xicgen_stringbuilder_previous_append_in_run(XiCgenCtx *ctx, const XiBlock *blk,
                                                        uint32_t index,
                                                        const XiValue *receiver_origin) {
    for (uint32_t i = index; i > 0; i--) {
        const XiValue *candidate = blk->values[i - 1];
        if (!candidate || xicgen_stringbuilder_chain_gap_value(candidate))
            continue;
        int64_t ignored = 0;
        return xicgen_stringbuilder_literal_append_plan(ctx, candidate, &ignored) &&
               xicgen_stringbuilder_receiver_origin(candidate->args[0]) == receiver_origin;
    }
    return false;
}

static void xicgen_emit_stringbuilder_literal_append_reserve(XiCgenCtx *ctx, FILE *out,
                                                             const XiBlock *blk, uint32_t index) {
    if (!ctx || !out || !blk || index >= blk->nvalues)
        return;
    const XiValue *first = blk->values[index];
    int64_t total = 0;
    if (!xicgen_stringbuilder_literal_append_plan(ctx, first, &total))
        return;
    const XiValue *receiver_origin = xicgen_stringbuilder_receiver_origin(first->args[0]);
    if (!receiver_origin ||
        xicgen_stringbuilder_previous_append_in_run(ctx, blk, index, receiver_origin))
        return;

    for (uint32_t i = index + 1; i < blk->nvalues; i++) {
        const XiValue *candidate = blk->values[i];
        if (!candidate || xicgen_stringbuilder_chain_gap_value(candidate))
            continue;
        int64_t length = 0;
        if (!xicgen_stringbuilder_literal_append_plan(ctx, candidate, &length) ||
            xicgen_stringbuilder_receiver_origin(candidate->args[0]) != receiver_origin)
            break;
        if (XR_UNLIKELY(length > INT64_MAX - total)) {
            ctx->error = true;
            fprintf(stderr,
                    "[xi_cgen] ERROR: StringBuilder literal append reserve overflow at line %u\n",
                    (unsigned) first->line);
            return;
        }
        total += length;
    }

    fprintf(out, "    xrt_strbuf_reserve_extra_exact(");
    emit_value_as_rep_ctx(ctx, out, first->args[0], XR_REP_TAGGED);
    fprintf(out, ", INT64_C(%" PRId64 "));\n", total);
}

static bool xicgen_emit_stringbuilder_method(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                             const char *method, uint16_t nargs) {
    const XrType *recv_type = v->nargs > 0 && v->args[0] ? v->args[0]->type : NULL;
    bool recv_is_stringbuilder = recv_type && recv_type->kind == XR_KIND_INSTANCE &&
                                 recv_type->instance.class_name &&
                                 strcmp(recv_type->instance.class_name, "StringBuilder") == 0;
    if (!recv_is_stringbuilder || !method)
        return false;
    const XaotCapacityPlan *plan = xicgen_stringbuilder_capacity_plan(ctx, v);

    if (strcmp(method, "toString") == 0 && nargs == 0) {
        if (!plan && v->xg_capacity_op_id == XG_NO_ID)
            return false;
        const uint32_t required = XAOT_CAPACITY_EV_GLOBAL_ROW | XAOT_CAPACITY_EV_RECEIVER_TYPE |
                                  XAOT_CAPACITY_EV_ELEM_TYPE;
        if (!plan || plan->sequence_kind != XG_SEQ_STRING_BUILDER ||
            plan->op_kind != XG_CAPACITY_TO_STRING ||
            plan->action != XAOT_CAPACITY_BUILDER_FINISH ||
            (plan->evidence & required) != required ||
            plan->unproven_reason != XAOT_CAPACITY_UNPROVEN_NONE) {
            ctx->error = true;
            fprintf(stderr,
                    "[xi_cgen] ERROR: StringBuilder.toString lacks verified builder-finish plan at "
                    "line %u\n",
                    (unsigned) v->line);
            emit_codegen_abort_expr(out);
            return true;
        }
        const char *suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        fprintf(out, "xrt_strbuf_finish(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ")");
        emit_conversion_suffix(out, suffix);
        return true;
    }
    if (strcmp(method, "clear") == 0 && nargs == 0) {
        if (!plan && v->xg_capacity_op_id == XG_NO_ID)
            return false;
        const uint32_t required = XAOT_CAPACITY_EV_GLOBAL_ROW | XAOT_CAPACITY_EV_RECEIVER_TYPE |
                                  XAOT_CAPACITY_EV_ELEM_TYPE;
        if (!plan || plan->sequence_kind != XG_SEQ_STRING_BUILDER ||
            plan->op_kind != XG_CAPACITY_CLEAR || plan->action != XAOT_CAPACITY_CLEAR_DIRECT ||
            (plan->evidence & required) != required ||
            plan->unproven_reason != XAOT_CAPACITY_UNPROVEN_NONE) {
            ctx->error = true;
            fprintf(stderr,
                    "[xi_cgen] ERROR: StringBuilder.clear lacks verified direct-clear plan at line "
                    "%u\n",
                    (unsigned) v->line);
            emit_codegen_abort_expr(out);
            return true;
        }
        fprintf(out, "(xrt_strbuf_clear(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, "), ");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ")");
        return true;
    }
    if (strcmp(method, "append") != 0 || nargs != 1)
        return false;

    int64_t literal_length = 0;
    bool literal_plan = xicgen_stringbuilder_literal_append_plan(ctx, v, &literal_length);
    bool literal_is_string = false;
    if (literal_plan)
        xicgen_stringbuilder_exact_append_len(xicgen_stringbuilder_append_arg(v), NULL,
                                              &literal_is_string);
    (void) literal_length;
    if (v->xg_capacity_op_id != XG_NO_ID &&
        (!plan || plan->sequence_kind != XG_SEQ_STRING_BUILDER ||
         plan->op_kind != XG_CAPACITY_APPEND)) {
        ctx->error = true;
        fprintf(
            stderr,
            "[xi_cgen] ERROR: StringBuilder.append has stale capacity-plan binding at line %u\n",
            (unsigned) v->line);
        emit_codegen_abort_expr(out);
        return true;
    }
    if (plan && plan->action == XAOT_CAPACITY_RESERVE_ONCE && !literal_plan) {
        ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: StringBuilder.append reserve plan lacks an exact literal at line "
                "%u\n",
                (unsigned) v->line);
        emit_codegen_abort_expr(out);
        return true;
    }
    if (plan && plan->action != XAOT_CAPACITY_RESERVE_ONCE &&
        plan->action != XAOT_CAPACITY_CHECKED_GROW) {
        ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: unsupported StringBuilder.append capacity plan at line %u\n",
                (unsigned) v->line);
        emit_codegen_abort_expr(out);
        return true;
    }
    /* StringBuilder.append takes Json (tagged): render the argument as a tagged
     * value. If the rep planner inserted a lossy tagged->i64 UNBOX for the arg
     * (e.g. a null literal), see through it to the original tagged source so the
     * builder appends the real value ("null") instead of its unboxed int (0). */
    const XiValue *append_arg = cg_unwrap_identity_value(v->args[1]);
    const char *append_fn = !literal_plan       ? "(xrt_strbuf_append("
                            : literal_is_string ? "(xrt_strbuf_append_string_no_grow("
                                                : "(xrt_strbuf_append_scalar_no_grow(";
    fprintf(out, "%s", append_fn);
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, append_arg, XR_REP_TAGGED);
    fprintf(out, "), ");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
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

static CgAtomicI64DirectOp xicgen_atomic_i64_direct_op(const XiValue *v, XaIntrinsicId intrinsic_id,
                                                       uint16_t nargs) {
    if (!v || v->nargs < 1)
        return CG_ATOMIC_I64_DIRECT_NONE;
    switch (intrinsic_id) {
        case XA_INTRINSIC_ATOMIC_LOAD:
            return nargs <= 1 ? CG_ATOMIC_I64_DIRECT_LOAD : CG_ATOMIC_I64_DIRECT_NONE;
        case XA_INTRINSIC_ATOMIC_STORE:
            return nargs >= 1 && nargs <= 2 && v->nargs >= 2 ? CG_ATOMIC_I64_DIRECT_STORE
                                                             : CG_ATOMIC_I64_DIRECT_NONE;
        case XA_INTRINSIC_ATOMIC_ADD:
            return nargs >= 1 && nargs <= 2 && v->nargs >= 2 ? CG_ATOMIC_I64_DIRECT_ADD
                                                             : CG_ATOMIC_I64_DIRECT_NONE;
        case XA_INTRINSIC_ATOMIC_SUB:
            return nargs >= 1 && nargs <= 2 && v->nargs >= 2 ? CG_ATOMIC_I64_DIRECT_SUB
                                                             : CG_ATOMIC_I64_DIRECT_NONE;
        case XA_INTRINSIC_ATOMIC_FETCH_ADD:
            return nargs >= 1 && nargs <= 2 && v->nargs >= 2 ? CG_ATOMIC_I64_DIRECT_FETCH_ADD
                                                             : CG_ATOMIC_I64_DIRECT_NONE;
        case XA_INTRINSIC_ATOMIC_FETCH_SUB:
            return nargs >= 1 && nargs <= 2 && v->nargs >= 2 ? CG_ATOMIC_I64_DIRECT_FETCH_SUB
                                                             : CG_ATOMIC_I64_DIRECT_NONE;
        case XA_INTRINSIC_ATOMIC_SWAP:
            return nargs >= 1 && nargs <= 2 && v->nargs >= 2 ? CG_ATOMIC_I64_DIRECT_SWAP
                                                             : CG_ATOMIC_I64_DIRECT_NONE;
        default:
            break;
    }
    return CG_ATOMIC_I64_DIRECT_NONE;
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

static bool xicgen_span_slice_is_nothrow(XiCgenCtx *ctx, const XiValue *value) {
    const XiValue *v = cg_unwrap_identity_value(value);
    return v && v->op == XI_SLICE && v->nargs >= 3 && cg_value_plan_is_span_aggregate(ctx, v);
}

/* Vector span loads/stores are semantically checked, but both the native-SIMD
 * and portable aggregate C emitters implement failure with xrt_index_oob(), a
 * noreturn trap/exception transfer.  They never communicate failure through
 * xrt_pending_error.  Treat that lowering contract as nothrow with respect to
 * Xi's pending-error channel so the ERR_CHECK mechanically inserted after the
 * intrinsic does not turn every vector access into a TLS load on the hot path.
 *
 * Keep the predicate deliberately structural and fail closed: if the value is
 * not one of the exact span-backed shapes accepted by xicgen_vec(), the normal
 * may-throw path remains in force. */
static bool xicgen_vec_span_access_uses_direct_trap(XiCgenCtx *ctx, const XiValue *value) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || !xi_vec_shape_is_explicit(v->aux_int))
        return false;
    if (v->op == XI_VEC_LOAD)
        return v->nargs == 2 && v->args[0] && v->args[0]->type &&
               v->args[0]->type->kind == XR_KIND_SLICE &&
               cg_value_plan_is_span_aggregate(ctx, v->args[0]);
    if (v->op == XI_VEC_STORE)
        return v->nargs == 3 && v->args[1] && v->args[1]->type &&
               v->args[1]->type->kind == XR_KIND_SLICE &&
               cg_value_plan_is_span_aggregate(ctx, v->args[1]);
    return false;
}

static bool xicgen_span_window_uses_direct_trap(XiCgenCtx *ctx, const XiValue *value) {
    const XiValue *v = cg_unwrap_identity_value(value);
    return v && v->op == XI_SLICE_WINDOW && v->nargs == 3 && v->args[0] && v->args[0]->type &&
           v->args[0]->type->kind == XR_KIND_SLICE &&
           cg_value_plan_is_span_aggregate(ctx, v->args[0]) &&
           cg_value_plan_is_span_aggregate(ctx, v);
}

/* A verified span-access plan that eliminates the runtime helper commits CGen
 * to the inline lowering for that exact Xi value. Inline validation failures
 * transfer through xrt_throw_error/xrt_index_oob, both noreturn; they never
 * return with xrt_pending_error set. Consume the plan directly instead of
 * re-deriving individual byte/span method shapes in the ERR_CHECK path. */
static bool xicgen_span_access_plan_uses_direct_trap(XiCgenCtx *ctx, const XiValue *value) {
    const XiValue *v = cg_unwrap_identity_value(value);
    const XaotSliceAccessPlan *plan =
        v ? xaot_bundle_find_span_access_plan(cg_ctx_aot_bundle(ctx), v) : NULL;
    return plan && (plan->eliminated_checks & XAOT_SLICE_DROP_HELPER) != 0;
}

static bool xicgen_byte_slice_common_prefix_method_drops_helper(XiCgenCtx *ctx,
                                                                const XiValue *call) {
    const XiValue *v = cg_unwrap_identity_value(call);
    if (!xicgen_call_method_is_common_prefix(v))
        return false;
    return cg_span_plan_drops(ctx, v, XAOT_SLICE_ACCESS_BYTE_COMMON_PREFIX, XAOT_SLICE_DROP_HELPER);
}

static bool xicgen_byte_slice_copy_method_drops_helper(XiCgenCtx *ctx, const XiValue *call) {
    const XiValue *v = cg_unwrap_identity_value(call);
    if (!xicgen_call_method_is_copy_from(v))
        return false;
    return cg_span_plan_drops(ctx, v, XAOT_SLICE_ACCESS_BYTE_COPY, XAOT_SLICE_DROP_HELPER);
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

    if (cg_array_call_is_direct_byte_array_mutator_trusted_nothrow(ctx, current, v))
        return true;
    if (cg_array_call_is_byte_array_append_trusted_nothrow(ctx, current, v))
        return true;
    if (cg_array_call_is_byte_array_repeat_trusted_nothrow(ctx, current, v))
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
        cg_func_needs_aot_coro_ctx(ctx, target))
        return false;
    return !xicgen_func_has_error_flow(ctx, target, (uint8_t) (depth + 1));
}

static bool xicgen_value_is_proven_nothrow(XiCgenCtx *ctx, const XiFunc *current,
                                           const XiValue *value, uint8_t depth) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v)
        return false;
    /* Hosted/freestanding ASSERT lowers to an immediate abort/trap on failure;
     * it never publishes through xrt_pending_error.  Treat the successful edge
     * as a usable proof without adding a redundant TLS poll. */
    if (v->op == XI_ASSERT)
        return true;
    if (v->xa_intrinsic_id != XA_INTRINSIC_NONE) {
        const XaIntrinsicDesc *desc = xa_intrinsic_by_id((XaIntrinsicId) v->xa_intrinsic_id);
        if (desc && desc->effect != XA_INTRINSIC_EFFECT_MAY_THROW &&
            desc->effect != XA_INTRINSIC_EFFECT_READ_MAY_THROW &&
            desc->effect != XA_INTRINSIC_EFFECT_WRITE_MAY_THROW)
            return true;
    }
    if (v->op == XI_ARRAY_DATA_PTR || v->op == XI_PTR_LOAD)
        return true;
    if (xicgen_span_window_uses_direct_trap(ctx, v))
        return true;
    if (xicgen_vec_span_access_uses_direct_trap(ctx, v))
        return true;
    if (xicgen_span_access_plan_uses_direct_trap(ctx, v))
        return true;
    if (xicgen_value_is_nothrow_native_scalar(current, v))
        return true;
    if (xicgen_span_slice_is_nothrow(ctx, v))
        return true;
    if (cg_array_index_get_trusted_nothrow(ctx, current, v))
        return true;
    if (cg_byte_slice_load_trusted_nothrow(ctx, current, v))
        return true;
    if (cg_span_common_prefix_trusted_nothrow(ctx, v))
        return true;
    if (xicgen_byte_slice_common_prefix_method_drops_helper(ctx, v))
        return true;
    if ((v->op == XI_CALL_METHOD || v->op == XI_CALL_METHOD_DIRECT) && v->aux &&
        strcmp((const char *) v->aux, "close") == 0 &&
        xicgen_parallel_plan_class_for_call(ctx, current, v))
        return true;
    return xicgen_call_is_nothrow_direct_depth(ctx, current, v, depth);
}

/* ARC and representation cleanup can legally be scheduled between a
 * may-throw producer and its XI_ERR_CHECK.  Those intervening values cannot
 * publish a pending error, so recover the nearest preceding MAY_THROW value
 * instead of assuming physical adjacency.  Stop at an earlier ERR_CHECK to
 * avoid attributing a check across an already-consumed error boundary. */
static const XiValue *xicgen_prev_pending_error_source(const XiValue *check) {
    if (!check || check->op != XI_ERR_CHECK || !check->block)
        return NULL;
    for (uint32_t i = 0; i < check->block->nvalues; i++) {
        if (check->block->values[i] != check)
            continue;
        while (i > 0) {
            const XiValue *candidate = check->block->values[--i];
            if (!candidate)
                continue;
            candidate = cg_unwrap_identity_value(candidate);
            if (!candidate)
                continue;
            if (candidate->op == XI_ERR_CHECK)
                return NULL;
            if ((candidate->flags & XI_FLAG_MAY_THROW) != 0)
                return candidate;
        }
        break;
    }
    return NULL;
}

static bool xicgen_assert_is_parallel_body_safe(XiCgenCtx *ctx, const XiFunc *current,
                                                const XiValue *value) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || v->op != XI_ASSERT || v->nargs < 1 || !v->args[0])
        return false;

    const XiValue *cond = cg_unwrap_identity_value(v->args[0]);
    if (cond && cond->op == XI_CONST && cond->type && cond->type->kind == XR_KIND_BOOL) {
        bool truth = cond->aux_int != 0;
        return v->aux_int == 1 ? !truth : truth;
    }
    return xicgen_value_is_proven_nothrow(ctx, current, cond, 0);
}

static bool xicgen_err_check_after_proven_nothrow(XiCgenCtx *ctx, const XiFunc *current,
                                                  const XiValue *check) {
    if (!check || check->op != XI_ERR_CHECK || cg_value_type_is_bool(check))
        return false;
    /* Keep the ordinary adjacent producer as the primary source.  Besides
     * calls, Xi can place a proven-nothrow scalar/pointer value immediately
     * before an ERR_CHECK; restricting recovery to MAY_THROW values would
     * incorrectly resurrect the TLS probe for those established cases.
     * Vector stores are the exceptional shape that can have ARC/rep cleanup
     * between the producer and check, so only fall back to the backwards
     * MAY_THROW search when the adjacent value is not itself proven safe. */
    const XiValue *adjacent = cg_class_native_prev_error_source_value(check);
    if (xicgen_value_is_proven_nothrow(ctx, current, adjacent, 0))
        return true;
    return xicgen_value_is_proven_nothrow(ctx, current, xicgen_prev_pending_error_source(check), 0);
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
                                          XaIntrinsicId intrinsic_id, uint16_t nargs) {
    if (!v || v->nargs < 1)
        return false;

    int64_t ordering = XR_AOT_ORDERING_SEQ_CST;
    if (!xicgen_value_is_const_ordering(ordering_arg, &ordering))
        return false;

    CgAtomicI64DirectOp op = xicgen_atomic_i64_direct_op(v, intrinsic_id, nargs);
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
                                      XaIntrinsicId intrinsic_id, uint16_t nargs) {
    if (!v || v->nargs < 1 || !xi_value_type_is_atomic(v->args[0]))
        return false;

    CgAtomicKind kind = xicgen_atomic_kind_from_receiver(v->args[0]);
    const XiValue *ordering_arg = NULL;
    if ((intrinsic_id == XA_INTRINSIC_ATOMIC_LOAD || intrinsic_id == XA_INTRINSIC_ATOMIC_TOGGLE) &&
        nargs == 1) {
        ordering_arg = v->nargs >= 2 ? v->args[1] : NULL;
    } else if ((intrinsic_id == XA_INTRINSIC_ATOMIC_STORE ||
                intrinsic_id == XA_INTRINSIC_ATOMIC_ADD ||
                intrinsic_id == XA_INTRINSIC_ATOMIC_SUB ||
                intrinsic_id == XA_INTRINSIC_ATOMIC_FETCH_ADD ||
                intrinsic_id == XA_INTRINSIC_ATOMIC_FETCH_SUB ||
                intrinsic_id == XA_INTRINSIC_ATOMIC_SWAP) &&
               nargs == 2) {
        ordering_arg = v->nargs >= 3 ? v->args[2] : NULL;
    } else if (intrinsic_id == XA_INTRINSIC_ATOMIC_COMPARE_EXCHANGE && nargs == 3) {
        ordering_arg = v->nargs >= 4 ? v->args[3] : NULL;
    }

    bool is_load = intrinsic_id == XA_INTRINSIC_ATOMIC_LOAD && nargs <= 1;
    bool is_store =
        intrinsic_id == XA_INTRINSIC_ATOMIC_STORE && nargs >= 1 && nargs <= 2 && v->nargs >= 2;
    bool is_add =
        intrinsic_id == XA_INTRINSIC_ATOMIC_ADD && nargs >= 1 && nargs <= 2 && v->nargs >= 2;
    bool is_sub =
        intrinsic_id == XA_INTRINSIC_ATOMIC_SUB && nargs >= 1 && nargs <= 2 && v->nargs >= 2;
    bool is_fetch_add =
        intrinsic_id == XA_INTRINSIC_ATOMIC_FETCH_ADD && nargs >= 1 && nargs <= 2 && v->nargs >= 2;
    bool is_fetch_sub =
        intrinsic_id == XA_INTRINSIC_ATOMIC_FETCH_SUB && nargs >= 1 && nargs <= 2 && v->nargs >= 2;
    bool is_swap =
        intrinsic_id == XA_INTRINSIC_ATOMIC_SWAP && nargs >= 1 && nargs <= 2 && v->nargs >= 2;
    bool is_compare_exchange = intrinsic_id == XA_INTRINSIC_ATOMIC_COMPARE_EXCHANGE && nargs >= 2 &&
                               nargs <= 3 && v->nargs >= 3;
    bool is_toggle = intrinsic_id == XA_INTRINSIC_ATOMIC_TOGGLE && nargs <= 1;
    bool is_to_string = intrinsic_id == XA_INTRINSIC_ATOMIC_TO_STRING && nargs == 0;

    if (!is_load && !is_store && !is_add && !is_sub && !is_fetch_add && !is_fetch_sub && !is_swap &&
        !is_compare_exchange && !is_toggle && !is_to_string) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported canonical Atomic intrinsic id %u\n",
                (unsigned) intrinsic_id);
        emit_codegen_abort_expr(out);
        return true;
    }

    if ((is_add || is_sub || is_fetch_add || is_fetch_sub) && kind == CG_ATOMIC_BOOL) {
        const XaIntrinsicDesc *desc = xa_intrinsic_by_id(intrinsic_id);
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: Atomic<bool> intrinsic '%s' is not supported in AOT\n",
                desc ? desc->key : "?");
        emit_codegen_abort_expr(out);
        return true;
    }

    if (kind == CG_ATOMIC_INT &&
        xicgen_emit_atomic_i64_direct(out, v, ordering_arg, intrinsic_id, nargs))
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

static void xicgen_atomic(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                          const char *prefix) {
    (void) f;
    (void) prefix;
    const XaIntrinsicDesc *desc = v ? xa_intrinsic_by_id((XaIntrinsicId) v->xa_intrinsic_id) : NULL;
    uint16_t nargs = v && v->nargs > 0 ? (uint16_t) (v->nargs - 1) : 0;
    if (!desc || desc->family != XA_INTRINSIC_FAMILY_ATOMIC ||
        !xicgen_emit_atomic_method(ctx, out, v, desc->id, nargs)) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: invalid canonical Atomic intrinsic id %u\n",
                v ? v->xa_intrinsic_id : 0u);
        emit_codegen_abort_expr(out);
    }
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
        fprintf(out, "xr_aot_bridge_value_to_xrt(xr_aot_chan_try_recv_sync(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, "))");
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
    if (ctx && ctx->freestanding_profile) {
        fprintf(out, "XR_NULL_VAL");
        return true;
    }
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

    if (strcmp(method, "encode") == 0 && nargs == 1 && v->nargs >= 2) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        fprintf(out, "xrt_json_encode(");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }

    if (strcmp(method, "parse") == 0 && nargs == 1 && v->nargs >= 2) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        fprintf(out, "xrt_json_parse(");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }

    if (strcmp(method, "containsKey") == 0 && nargs == 2 && v->nargs >= 3) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
        fprintf(out, "xrt_json_static_has(");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_TAGGED);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }

    if (strcmp(method, "get") == 0 && (nargs == 2 || nargs == 3) && v->nargs >= 3) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        fprintf(out, "xrt_json_static_get(");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_TAGGED);
        fprintf(out, ", ");
        if (nargs == 3 && v->nargs >= 4)
            emit_value_as_rep_ctx(ctx, out, v->args[3], XR_REP_TAGGED);
        else
            fprintf(out, "XR_NULL_VAL");
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }

    if (strcmp(method, "stringify") == 0 && nargs == 1 && v->nargs >= 2) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        fprintf(out, "xrt_json_stringify(");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }

    if (strcmp(method, "size") == 0 && nargs == 1 && v->nargs >= 2) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
        fprintf(out, "xrt_json_static_size(");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }

    if (strcmp(method, "isEmpty") == 0 && nargs == 1 && v->nargs >= 2) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
        fprintf(out, "xrt_json_static_is_empty(");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }

    {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT Json static method '%s'\n",
                method ? method : "?");
        emit_codegen_abort_expr(out);
        return true;
    }
}

static bool xicgen_emit_bigint_method(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                      const char *method, uint16_t nargs) {
    if (!v || v->nargs < 1 || !method || nargs != 0 ||
        !xr_type_is_named_class(v->args[0]->type, "BigInt"))
        return false;

    if (strcmp(method, "sign") == 0) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
        fprintf(out, "xrt_bigint_sign_value(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }

    if (strcmp(method, "isZero") == 0 || strcmp(method, "isNegative") == 0 ||
        strcmp(method, "isPositive") == 0) {
        const char *helper = strcmp(method, "isZero") == 0 ? "xrt_bigint_is_zero_value"
                                                           : (strcmp(method, "isNegative") == 0
                                                                  ? "xrt_bigint_is_negative_value"
                                                                  : "xrt_bigint_is_positive_value");
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
        fprintf(out, "%s(", helper);
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }

    if (strcmp(method, "toInt") == 0) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        fprintf(out, "xrt_bigint_to_int_value(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }

    if (strcmp(method, "toFloat") == 0) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_F64, cg_rep(v));
        fprintf(out, "xrt_bigint_to_float_value(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }

    return false;
}

/* A rune is already carried as its Unicode scalar value in the native scalar
 * representation.  Keep the lossless rune -> u32 projection out of tagged
 * runtime method dispatch; freestanding profiles deliberately do not carry
 * the hosted method table. */
static bool xicgen_emit_rune_numeric_method(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                            const char *method, uint16_t nargs) {
    if (!v || v->nargs < 1 || !method || nargs != 0 || strcmp(method, "toUInt32") != 0)
        return false;
    const XiValue *recv = v->args[0];
    if (!recv || !recv->type || recv->type->kind != XR_KIND_RUNE)
        return false;

    const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
    fprintf(out, "(uint32_t)(");
    emit_value_as_rep_ctx(ctx, out, recv, XR_REP_I64);
    fprintf(out, ")");
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

/* Direct scalar lowering for the remaining int arithmetic methods. Exact
 * width bit methods are canonical XI_BIT_* operations before C generation;
 * method-name dispatch here would discard their width contract. */
static bool xicgen_emit_int_numeric_method(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                           const char *method, uint16_t nargs) {
    if (!v || v->nargs < 1 || !method)
        return false;
    const XiValue *recv = v->args[0];
    if (!recv || !recv->type || recv->type->kind != XR_KIND_INT)
        return false;

    if (nargs == 0)
        return false;

    if (nargs != 1 || v->nargs < 2)
        return false;
    const XiValue *arg = v->args[1];
    if (!arg || !arg->type || arg->type->kind != XR_KIND_INT)
        return false;

    /* 1-arg int -> int (wrapping/saturating arithmetic). */
    const char *fn1 = NULL;
    if (strcmp(method, "wrappingAdd") == 0)
        fn1 = "xr_i64_add_wrap";
    else if (strcmp(method, "wrappingSub") == 0)
        fn1 = "xr_i64_sub_wrap";
    else if (strcmp(method, "wrappingMul") == 0)
        fn1 = "xr_i64_mul_wrap";
    else if (strcmp(method, "saturatingAdd") == 0)
        fn1 = "xr_i64_saturating_add";
    else if (strcmp(method, "saturatingSub") == 0)
        fn1 = "xr_i64_saturating_sub";
    else if (strcmp(method, "saturatingMul") == 0)
        fn1 = "xr_i64_saturating_mul";
    if (fn1) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
        fprintf(out, "%s(", fn1);
        emit_value_as_rep_ctx(ctx, out, recv, XR_REP_I64);
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, arg, XR_REP_I64);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }

    /* 1-arg int -> bool (overflow predicates). */
    const char *pred = NULL;
    if (strcmp(method, "addOverflows") == 0)
        pred = "xr_arith_core_add_overflows";
    else if (strcmp(method, "subOverflows") == 0)
        pred = "xr_arith_core_sub_overflows";
    else if (strcmp(method, "mulOverflows") == 0)
        pred = "xr_arith_core_mul_overflows";
    if (pred) {
        bool box_bool = cg_rep(v) == XR_REP_TAGGED;
        const char *conv_suffix =
            box_bool ? NULL : emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
        if (box_bool)
            fprintf(out, "XR_FROM_BOOL(");
        fprintf(out, "(%s(", pred);
        emit_value_as_rep_ctx(ctx, out, recv, XR_REP_I64);
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, arg, XR_REP_I64);
        fprintf(out, ") != 0)");
        if (box_bool)
            fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }
    return false;
}

static void xicgen_byte_slice_copy(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                   const char *prefix);
static void xicgen_byte_slice_common_prefix(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                            const XiValue *v, const char *prefix);

static bool xicgen_runtime_method_plan_allows_helper(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                                     const char *method, uint16_t nargs,
                                                     const XaotMethodDispatchPlan *dispatch_plan) {
    XaotBackendContractIssue issue = XAOT_BACKEND_CONTRACT_OK;
    uint32_t method_name_id = method ? xg_name_id(method) : 0;
    uint32_t source_span_id = v ? v->line : 0;
    if (xaot_backend_contract_runtime_helper_allowed(dispatch_plan, method_name_id, nargs,
                                                     source_span_id, &issue))
        return true;

    if (issue == XAOT_BACKEND_CONTRACT_RUNTIME_HELPER_FOR_OPTIMIZED_PLAN) {
        ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: verified AOT dispatch plan kind %u for method '%s' at line %u "
                "cannot use runtime method helper\n",
                dispatch_plan ? (unsigned) dispatch_plan->kind : 0, method ? method : "?",
                v ? (unsigned) v->line : 0);
        emit_codegen_abort_expr(out);
        return false;
    }

    ctx->error = true;
    fprintf(stderr,
            "[xi_cgen] ERROR: runtime method helper for '%s' at line %u does not match verified "
            "AOT dispatch plan\n",
            method ? method : "?", v ? (unsigned) v->line : 0);
    emit_codegen_abort_expr(out);
    return false;
}

static bool xicgen_key_access_runtime_method_preflight(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                                       const char *method, uint16_t nargs) {
    if (!v || v->xg_key_access_id == 0)
        return true;
    uint8_t container_kind = 0;
    uint8_t op = 0;
    const XrType *recv_type = v->nargs >= 1 && v->args[0] ? v->args[0]->type : NULL;
    if (recv_type && recv_type->kind == XR_KIND_MAP)
        container_kind = XG_MAP_CONTAINER_MAP;
    else if (recv_type && recv_type->kind == XR_KIND_SET)
        container_kind = XG_MAP_CONTAINER_SET;
    if (container_kind == 0 || !cg_key_access_method_op(container_kind, method, nargs, &op)) {
        ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: key-access id %u is attached to unsupported runtime method "
                "'%s' at line %u\n",
                v->xg_key_access_id, method ? method : "?", (unsigned) v->line);
        emit_codegen_abort_expr(out);
        return false;
    }
    const XaotKeyAccessPlan *plan =
        cg_verified_key_access_plan(ctx, v, container_kind, op, method ? method : "Map/Set");
    if (!cg_key_access_plan_action_has_backend(ctx, plan, v, method ? method : "Map/Set") ||
        (ctx && ctx->error)) {
        emit_codegen_abort_expr(out);
        return false;
    }
    return true;
}

static void xicgen_emit_runtime_method(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                       const char *method, uint16_t nargs,
                                       const XaotMethodDispatchPlan *dispatch_plan) {
    if (xicgen_emit_rune_numeric_method(ctx, out, v, method, nargs))
        return;
    if (xicgen_emit_int_numeric_method(ctx, out, v, method, nargs))
        return;
    if (xicgen_emit_json_static_method(ctx, out, v, method, nargs))
        return;
    if (xicgen_emit_bigint_method(ctx, out, v, method, nargs))
        return;
    if (xicgen_emit_freestanding_enum_to_string_method(ctx, out, v, method, nargs))
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
    if (method && (strcmp(method, "asBytes") == 0 || strcmp(method, "asMutBytes") == 0) &&
        nargs == 0 && v->nargs >= 1 && xr_type_is_named_class(v->args[0]->type, "Buffer")) {
        if (cg_value_plan_is_span_aggregate(ctx, v)) {
            fprintf(out, "%s(",
                    strcmp(method, "asBytes") == 0 ? "xrt_buffer_as_bytes"
                                                   : "xrt_buffer_as_mut_bytes");
            emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
            fprintf(out, ")");
            return;
        }
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        fprintf(out, "XR_NULL_VAL");
        emit_conversion_suffix(out, conv_suffix);
        return;
    }
    if (method && strcmp(method, "borrowPtr") == 0 && nargs == 0 && v->nargs >= 1 &&
        xr_type_is_named_class(v->args[0]->type, "Buffer")) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        fprintf(out, "xrt_buffer_borrow_ptr(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return;
    }
    if (method && strcmp(method, "test") == 0 && nargs == 1 && v->nargs >= 2 &&
        xr_type_is_named_class(v->args[0]->type, "Regex")) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        fprintf(out, "xrt_regex_test(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", xr_str_data(");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
        fprintf(out, "), xr_str_len(");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
        fprintf(out, "))");
        emit_conversion_suffix(out, conv_suffix);
        return;
    }
    int sym = cg_method_sym(method);
    if (xicgen_emit_stringbuilder_method(ctx, out, v, method, nargs))
        return;
    /* string.copyArray<byte>(): the VM dispatches this by name (no stable method-symbol
     * id), so lower it directly to the runtime helper. Mirrors VM m_to_bytes. */
    if (sym < 0 && method && strcmp(method, "copyBytes") == 0 && nargs == 0 && v->nargs >= 1) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        fprintf(out, "xrt_str_to_bytes(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return;
    }
    if (xicgen_byte_slice_copy_method_drops_helper(ctx, v)) {
        xicgen_byte_slice_copy(ctx, out, f, v, NULL);
        return;
    }
    if (xicgen_byte_slice_common_prefix_method_drops_helper(ctx, v)) {
        xicgen_byte_slice_common_prefix(ctx, out, f, v, NULL);
        return;
    }
    if (sym < 0) {
        ctx->error = true;
        XgFuncId owner_func_id = (v && v->block && v->block->func)
                                     ? (XgFuncId) v->block->func->xg_body_func_id
                                     : XG_NO_ID;
        fprintf(stderr,
                "[xi_cgen] ERROR: unsupported AOT method '%s' "
                "(value=%u callsite=%u method_id=%u owner=%u line=%u)\n",
                method ? method : "?", v ? (unsigned) v->id : 0,
                v ? (unsigned) v->xg_callsite_id : 0, v ? (unsigned) v->xg_method_id : 0,
                (unsigned) owner_func_id, v ? (unsigned) v->line : 0);
        emit_codegen_abort_expr(out);
        return;
    }
    if (emit_class_native_map_method_call_expr(ctx, out, f, v))
        return;
    if (ctx->error) {
        emit_codegen_abort_expr(out);
        return;
    }
    if (emit_class_native_set_method_call_expr(ctx, out, f, v))
        return;
    if (ctx->error) {
        emit_codegen_abort_expr(out);
        return;
    }
    if (emit_class_native_array_method_call_expr(ctx, out, f, v))
        return;
    if (emit_local_typed_map_method_call_expr(ctx, out, f, v))
        return;
    if (ctx->error) {
        emit_codegen_abort_expr(out);
        return;
    }
    if (emit_local_typed_set_method_call_expr(ctx, out, f, v))
        return;
    if (ctx->error) {
        emit_codegen_abort_expr(out);
        return;
    }
    if (emit_tagged_map_method_key_access_expr(ctx, out, v))
        return;
    if (ctx->error) {
        emit_codegen_abort_expr(out);
        return;
    }
    if (emit_tagged_set_method_key_access_expr(ctx, out, v))
        return;
    if (ctx->error) {
        emit_codegen_abort_expr(out);
        return;
    }
    if (!xicgen_key_access_runtime_method_preflight(ctx, out, v, method, nargs))
        return;
    if (!xicgen_runtime_method_plan_allows_helper(ctx, out, v, method, nargs, dispatch_plan))
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
static void xicgen_emit_map_instance_alloc(FILE *out, const char *class_name) {
    fprintf(out, "XrValue _inst = xrt_map_new(4); xrt_map_set_class_name(_inst, ");
    xicgen_emit_c_string_literal(out, class_name ? class_name : "?");
    fprintf(out, "); ");
}

static bool xicgen_class_data_same_identity(const XiClassData *a, const XiClassData *b) {
    if (a == b)
        return true;
    if (!a || !b)
        return false;
    if (a->class_name && b->class_name && strcmp(a->class_name, b->class_name) == 0)
        return true;
    if (a->display_name && b->display_name && strcmp(a->display_name, b->display_name) == 0)
        return true;
    if (a->class_name && b->display_name && strcmp(a->class_name, b->display_name) == 0)
        return true;
    return a->display_name && b->class_name && strcmp(a->display_name, b->class_name) == 0;
}

static const XiFunc *xicgen_find_constructor_for_class_data(XiCgenCtx *ctx, const XiFunc *f,
                                                            const XiClassData *class_data,
                                                            const char **out_prefix) {
    if (out_prefix)
        *out_prefix = NULL;
    if (!class_data)
        return NULL;

    const XiFunc *ctor = NULL;
    if (ctx && ctx->module && ctx->module->init) {
        ctor = cg_find_constructor(ctx->module->init, class_data);
        if (ctor) {
            if (out_prefix)
                *out_prefix = cg_module_prefix_for_func(ctx, ctor);
            return ctor;
        }
    }
    ctor = cg_find_constructor(f, class_data);
    if (ctor) {
        if (out_prefix)
            *out_prefix = cg_module_prefix_for_func(ctx, ctor);
        return ctor;
    }

    for (int i = 0; ctx && i < ctx->nimports; i++) {
        const CgImportEntry *imp = &ctx->imports[i];
        if (!xicgen_class_data_same_identity(imp->target_class, class_data))
            continue;
        ctor = imp->target_func;
        if (!ctor && imp->target_class && imp->exporter_func)
            ctor = cg_find_constructor(imp->exporter_func, imp->target_class);
        if (ctor) {
            if (out_prefix)
                *out_prefix = imp->target_mod_name;
            return ctor;
        }
    }

    for (int mi = 0; ctx && mi < ctx->all_nmodules; mi++) {
        const XiModule *mod = ctx->all_modules ? ctx->all_modules[mi] : NULL;
        if (!mod || !mod->init)
            continue;
        for (uint16_t ci = 0; ci < mod->nclasses; ci++) {
            const XiClassData *cd = mod->classes ? mod->classes[ci] : NULL;
            if (!xicgen_class_data_same_identity(cd, class_data))
                continue;
            ctor = cg_find_constructor(mod->init, cd);
            if (ctor) {
                if (out_prefix)
                    *out_prefix = mod->name;
                return ctor;
            }
        }
        for (uint16_t si = 0; si < mod->nslots; si++) {
            const XiClassData *cd = mod->slot_classes ? mod->slot_classes[si] : NULL;
            if (!xicgen_class_data_same_identity(cd, class_data))
                continue;
            ctor = cg_find_constructor(mod->init, cd);
            if (ctor) {
                if (out_prefix)
                    *out_prefix = mod->name;
                return ctor;
            }
        }
    }
    return NULL;
}

static bool xicgen_emit_resolved_user_constructor(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                  const XiValue *v, const char *prefix,
                                                  const XiFunc *ctor, const char *call_prefix,
                                                  const XiClassData *class_data) {
    const char *resolved_prefix = call_prefix;
    if (!ctor && class_data)
        ctor = xicgen_find_constructor_for_class_data(ctx, f, class_data, &resolved_prefix);
    if (!ctor && class_data &&
        emit_class_native_default_constructor_expr(ctx, out, prefix, v, class_data, call_prefix))
        return true;
    if (!ctor)
        return false;
    if (!resolved_prefix)
        resolved_prefix = cg_module_prefix_for_func(ctx, ctor);
    if (emit_class_native_constructor_expr(ctx, out, f, prefix, v, ctor, resolved_prefix))
        return true;
    if (cg_func_needs_aot_coro_ctx(ctx, ctor)) {
        ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: unsupported AOT sync class constructor call to suspendable "
                "function '%s'\n",
                ctor->name ? ctor->name : "?");
        emit_codegen_abort_expr(out);
        return true;
    }
    const char *class_name = v->type ? xr_type_get_class_name(v->type) : NULL;
    if (!class_name && class_data)
        class_name = class_data->class_name ? class_data->class_name : class_data->display_name;
    if (!class_name)
        return false;
    fprintf(out, "({ ");
    xicgen_emit_map_instance_alloc(out, class_name);
    emit_fname(ctx, out, resolved_prefix ? resolved_prefix : prefix, ctor);
    fprintf(out, "(NULL, _inst");
    for (uint16_t a = 1; a < v->nargs; a++) {
        fprintf(out, ", ");
        /* Match the constructor's actual parameter ABI (see note above). */
        emit_value_as_rep_ctx(ctx, out, v->args[a], cg_func_param_abi_rep(ctx, ctor, a));
    }
    fprintf(out, "); _inst; })");
    return true;
}

static bool xicgen_emit_user_constructor(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                         const XiValue *v, const char *prefix) {
    CgStaticFunctionCall static_call = cg_resolve_static_function_call(ctx, f, v->args[0]);
    if (static_call.is_class_constructor &&
        xicgen_emit_resolved_user_constructor(ctx, out, f, v, prefix, static_call.func,
                                              static_call.prefix, static_call.class_data))
        return true;

    const char *class_name = v->type ? xr_type_get_class_name(v->type) : NULL;
    if (!class_name)
        return false;
    const char *call_prefix = NULL;
    const XiFunc *ctor = cg_lookup_class_ctor_global(ctx, class_name, &call_prefix);
    return xicgen_emit_resolved_user_constructor(ctx, out, f, v, prefix, ctor, call_prefix, NULL);
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
    const XiClassData *recv_data = cg_class_native_class_value_data(ctx, f, v->args[0]);
    const char *recv_class =
        recv_data ? recv_data->class_name : cg_class_native_receiver_class_name(ctx, f, v->args[0]);
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
    uint16_t call_argc = v->nargs > 0 ? (uint16_t) (v->nargs - 1) : 0;
    if (sfunc && !sfunc->is_vararg && sfunc->nparams != call_argc)
        sfunc = NULL;
    /* Never guess a static owner from the set of imported classes. An ordinary
     * instance method can share the same name and arity; explicit import/shared-
     * slot class receivers are resolved by cg_class_native_class_value_data
     * above. */
    if (!sfunc)
        return false;
    if (cg_func_needs_aot_coro_ctx(ctx, sfunc)) {
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
    if (target && cg_func_needs_aot_coro_ctx(ctx, target)) {
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
        const char *class_name = v->type ? xr_type_get_class_name(v->type) : NULL;
        if (!class_name && call.class_data)
            class_name = call.class_data->class_name;
        fprintf(out, "({ ");
        xicgen_emit_map_instance_alloc(out, class_name);
        emit_fname(ctx, out, call.prefix ? call.prefix : prefix, target);
        fprintf(out, "(NULL, _inst");
        for (uint16_t a = 1; a < v->nargs; a++) {
            fprintf(out, ", ");
            /* Match the constructor's actual parameter ABI (see note above). */
            emit_value_as_rep_ctx(ctx, out, v->args[a], cg_func_param_abi_rep(ctx, target, a));
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
    if (v->xg_map_shape_id != XG_NO_ID && method && strcmp(method, "add") == 0) {
        const XaotMapShapePlan *plan = xicgen_static_map_shape_plan(ctx, v);
        if (!plan || plan->container_kind != XG_MAP_CONTAINER_SET)
            emit_codegen_abort_expr(out);
        else
            fprintf(out, "((void)0)");
        return;
    }
    bool is_super = v->op == XI_CALL_METHOD && (v->aux_int & 1) != 0;
    const XiFunc *mfunc = NULL;
    const char *method_prefix = NULL;
    const XaotMethodDispatchPlan *dispatch_plan =
        is_super ? NULL
                 : xaot_bundle_find_method_dispatch_plan_for_xi_call(cg_ctx_aot_bundle(ctx), v);
    const XgMethodSummary *dispatch_method =
        dispatch_plan
            ? xicgen_evidence_method(xicgen_global_evidence(ctx), dispatch_plan->method_id)
            : NULL;
    bool dispatch_is_static = dispatch_method && (dispatch_method->flags & XG_METHOD_STATIC) != 0;

    if (!is_super && xicgen_emit_import_module_member_call(ctx, out, f, v, prefix, method))
        return;
    if (xicgen_emit_time_method(ctx, out, f, v))
        return;
    if (xicgen_emit_runtime_control_method(ctx, out, f, v))
        return;
    if (xicgen_emit_test_yield_method(ctx, out, f, v))
        return;
    if (xicgen_emit_stdlib_method(ctx, out, f, v))
        return;
    if (!is_super && method && strcmp(method, "constructor") == 0 &&
        xicgen_emit_panicinfo_constructor(ctx, out, v))
        return;
    if (!is_super && method && strcmp(method, "constructor") == 0 &&
        xicgen_emit_user_constructor(ctx, out, f, v, prefix))
        return;
    if (is_super)
        mfunc = xicgen_lookup_super_method(ctx, f, method, &method_prefix);
    else
        mfunc = xicgen_lookup_receiver_method(ctx, f, v, method, &method_prefix);

    uint16_t nargs = (uint16_t) (v->nargs - 1);
    if (xicgen_emit_typed_array_method(ctx, out, f, v, prefix, method, nargs))
        return;
    if (xicgen_emit_net_handle_method(ctx, out, f, v, method, nargs))
        return;
    if (xicgen_emit_enum_method(ctx, out, f, v, method))
        return;
    if (xicgen_emit_task_method(ctx, out, f, v, method, nargs))
        return;
    if (xicgen_emit_parallel_plan_lifecycle_method(ctx, out, f, v, prefix, method, nargs))
        return;
    /* Static methods carry a class namespace in args[0], but their Xi function
     * ABI has no receiver. Global dispatch evidence may still produce a direct
     * plan for the callsite; route that plan through the static emitter so the
     * namespace operand is dropped before ABI argument conversion. */
    if (!is_super && dispatch_is_static && xicgen_emit_static_method(ctx, out, f, v, prefix))
        return;
    if (xicgen_emit_planned_direct_method(ctx, out, f, v, prefix, dispatch_plan))
        return;
    if (xicgen_emit_planned_vtable_method(ctx, out, f, v, prefix, dispatch_plan))
        return;
    if (xicgen_emit_planned_type_switch_method(ctx, out, f, v, prefix, dispatch_plan))
        return;
    if (xicgen_emit_planned_itable_method(ctx, out, f, v, prefix, dispatch_plan))
        return;
    if (!dispatch_plan && xicgen_emit_direct_method(ctx, out, f, v, prefix, mfunc, method_prefix))
        return;
    if (!is_super && !dispatch_plan && xicgen_emit_static_method(ctx, out, f, v, prefix))
        return;
    if (dispatch_plan && dispatch_plan->kind != XAOT_DISPATCH_RUNTIME_FALLBACK) {
        XaotBackendContractIssue issue = XAOT_BACKEND_CONTRACT_OK;
        (void) xaot_backend_contract_check_mandatory_dispatch(
            cg_ctx_aot_bundle(ctx), dispatch_plan,
            XAOT_BACKEND_DISPATCH_SUPPORT_DIRECT | XAOT_BACKEND_DISPATCH_SUPPORT_VTABLE |
                XAOT_BACKEND_DISPATCH_SUPPORT_ITABLE | XAOT_BACKEND_DISPATCH_SUPPORT_TYPE_SWITCH |
                XAOT_BACKEND_DISPATCH_SUPPORT_RUNTIME_HELPER,
            &issue);
        ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: verified AOT dispatch plan kind %u for method '%s' at line %u "
                "was not emitted\n",
                (unsigned) dispatch_plan->kind, method ? method : "?", (unsigned) v->line);
        emit_codegen_abort_expr(out);
        return;
    }
    xicgen_emit_runtime_method(ctx, out, f, v, method, nargs, dispatch_plan);
}

static bool xicgen_interface_abi_requires_itable(const XaotInterfaceAbiPlan *abi) {
    return abi && (abi->flags & XAOT_INTERFACE_ABI_NEEDS_ITABLE) != 0 &&
           abi->itable_source == XAOT_INTERFACE_ABI_SOURCE_DISPATCH_SLOT &&
           abi->method_slot_count > 0;
}

static const XiFunc *xicgen_find_itable_target_func(XiCgenCtx *ctx, XgClassId class_id,
                                                    XgInterfaceId interface_id, uint32_t slot,
                                                    const char **out_prefix) {
    const XgGlobalEvidence *ev = xicgen_global_evidence(ctx);
    const XgInterfaceMethodSummary *iface_method =
        xicgen_evidence_interface_method_for_slot(ev, interface_id, slot);
    if (out_prefix)
        *out_prefix = NULL;
    if (!iface_method)
        return NULL;
    const XgMethodSummary *method = xicgen_evidence_find_method_by_signature_in_hierarchy(
        ev, class_id, iface_method->name_id, iface_method->signature_key);
    if (!method)
        return NULL;
    XgFuncId body_func_id = XG_NO_ID;
    for (uint32_t i = 0; ev && i < ev->nbodies; i++) {
        const XgBodySummary *body = &ev->bodies[i];
        if (body->kind != XG_BODY_METHOD || body->owner_method_id != method->method_id)
            continue;
        if (body_func_id != XG_NO_ID && body_func_id != body->func_id)
            return NULL;
        body_func_id = body->func_id;
    }
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    if (body_func_id != XG_NO_ID)
        return xaot_bundle_find_body_func(bundle, body_func_id, out_prefix);
    return xaot_bundle_find_method_func(bundle, method->method_id, out_prefix);
}

static uint32_t xicgen_count_class_itable_entries(XiCgenCtx *ctx, const XiClassData *cd,
                                                  XgClassId class_id) {
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    const XgGlobalEvidence *ev = xicgen_global_evidence(ctx);
    uint32_t count = 0;
    if (!bundle || !ev || !cd || class_id == XG_NO_ID)
        return 0;
    for (uint32_t i = 0; i < bundle->ninterface_abi_plans; i++) {
        const XaotInterfaceAbiPlan *abi = &bundle->interface_abi_plans[i];
        if (xicgen_interface_abi_requires_itable(abi) &&
            xicgen_evidence_class_implements_interface(ev, class_id, abi->interface_id))
            count++;
    }
    return count;
}

static void xicgen_emit_class_itable_init(XiCgenCtx *ctx, FILE *out, const XiClassData *cd,
                                          const char *prefix, const char *type_id_expr) {
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    const XgGlobalEvidence *ev = xicgen_global_evidence(ctx);
    XgClassId class_id = xicgen_class_id_for_data(ctx, cd);
    uint32_t entry_count = xicgen_count_class_itable_entries(ctx, cd, class_id);
    if (!bundle || !ev || !cd || class_id == XG_NO_ID || entry_count == 0)
        return;

    for (uint32_t i = 0; i < bundle->ninterface_abi_plans; i++) {
        const XaotInterfaceAbiPlan *abi = &bundle->interface_abi_plans[i];
        if (!xicgen_interface_abi_requires_itable(abi) ||
            !xicgen_evidence_class_implements_interface(ev, class_id, abi->interface_id))
            continue;
        fprintf(out, "static XrtMethodFn _xr_it_methods_%u_%u[] = {", (unsigned) class_id,
                (unsigned) abi->interface_id);
        for (uint32_t slot = 0; slot < abi->method_slot_count; slot++) {
            const char *target_prefix = NULL;
            const XiFunc *target = xicgen_find_itable_target_func(ctx, class_id, abi->interface_id,
                                                                  slot, &target_prefix);
            if (!target || cg_func_needs_aot_coro_ctx(ctx, target)) {
                ctx->error = true;
                fprintf(stderr,
                        "[xi_cgen] ERROR: verified AOT itable target for class %u interface %u "
                        "slot %u was not emitted\n",
                        (unsigned) class_id, (unsigned) abi->interface_id, (unsigned) slot);
                fprintf(out, "%sNULL", slot > 0 ? ", " : "");
                continue;
            }
            fprintf(out, "%s(XrtMethodFn)", slot > 0 ? ", " : "");
            emit_typed_abi_fname(ctx, out, target_prefix ? target_prefix : prefix, target);
        }
        fprintf(out, "}; ");
    }

    fprintf(out, "static const XrtInterfaceMethodTable _xr_itable_%u[] = {", (unsigned) class_id);
    uint32_t emitted = 0;
    for (uint32_t i = 0; i < bundle->ninterface_abi_plans; i++) {
        const XaotInterfaceAbiPlan *abi = &bundle->interface_abi_plans[i];
        if (!xicgen_interface_abi_requires_itable(abi) ||
            !xicgen_evidence_class_implements_interface(ev, class_id, abi->interface_id))
            continue;
        fprintf(out, "%s{%uu, _xr_it_methods_%u_%u, %uu}", emitted > 0 ? ", " : "",
                (unsigned) abi->interface_id, (unsigned) class_id, (unsigned) abi->interface_id,
                (unsigned) abi->method_slot_count);
        emitted++;
    }
    fprintf(out, "}; xrt_type_set_itable(%s, _xr_itable_%u, %uu); ", type_id_expr,
            (unsigned) class_id, (unsigned) emitted);
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
    bool emit_type_names = cg_emit_type_name_for_class(ctx, cd);
    bool is_mono = cd->is_monomorphized && cd->display_name;
    int origin_slot = -1;
    if (is_mono)
        origin_slot = cg_find_class_slot(ctx, cd->generic_origin_name ? cd->generic_origin_name
                                                                      : cd->display_name);
    fprintf(out, "({ ");
    if (emit_type_names && is_mono) {
        if (cd->mono_type_arg_count > 0 && cd->mono_type_arg_names) {
            fprintf(out, "static const char *_ta_%s[] = {", name);
            for (int ti = 0; ti < cd->mono_type_arg_count; ti++) {
                fprintf(out, "%s\"%s\"", ti > 0 ? ", " : "",
                        cd->mono_type_arg_names[ti] ? cd->mono_type_arg_names[ti] : "unknown");
            }
            fprintf(out, "}; ");
        }
    }
    fprintf(out, "uint16_t _tid = ");
    emit_class_native_type_register_expr(ctx, out, cd, prefix);
    fprintf(out, "; ");
    xicgen_emit_class_itable_init(ctx, out, cd, prefix, "_tid");
    if (emit_type_names) {
        fprintf(out, "xrt_type_set_name(_tid, \"%s\", NULL); ", name);
    }
    if (is_mono) {
        if (origin_slot >= 0) {
            fprintf(out, "xrt_type_set_generic_origin(_tid, (uint16_t)%s[%d].i); ",
                    ctx && ctx->shared_name ? ctx->shared_name : "xrt_shared", origin_slot);
        } else {
            fprintf(out, "xrt_type_set_generic_origin(_tid, 0); ");
        }
        if (emit_type_names) {
            fprintf(out, "xrt_type_set_generic_name(_tid, \"%s\", ", cd->display_name);
            if (cd->mono_type_arg_count > 0 && cd->mono_type_arg_names) {
                fprintf(out, "_ta_%s, %d", name, cd->mono_type_arg_count);
            } else {
                fprintf(out, "NULL, 0");
            }
            fprintf(out, "); ");
        }
    }
    emit_class_native_type_derive_init(ctx, out, cd, prefix, "_tid");
    fprintf(out, "XR_FROM_INT(_tid); })");
}

static void xicgen_throw(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                         const char *prefix) {
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 1, "xicgen_throw: need arg");
    if (ctx && ctx->freestanding_profile) {
        fprintf(out, "xrt_freestanding_trap(\"freestanding panic\")");
        return;
    }
    fprintf(out, "xrt_throw_exc(");
    emit_vref(out, v->args[0]);
    fprintf(out, ")");
}

static void xicgen_ownership_call(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                  const char *prefix, const char *fn_name) {
    (void) prefix;
    XR_DCHECK(v->nargs >= 1, "xicgen_ownership_call: need arg");
    const XiValue *arg = v->args[0];
    if (cg_value_plan_is_aggregate(ctx, cg_unwrap_identity_value(arg)) ||
        cg_value_plan_is_vector(ctx, cg_unwrap_identity_value(arg))) {
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

static bool xicgen_emit_json_decode_field_specs(FILE *out, const XrType *record_type,
                                                int64_t field_count, int depth) {
    if (!out || !record_type || !XR_TYPE_IS_RECORD(record_type) ||
        !record_type->object.field_names || !record_type->object.field_types ||
        record_type->object.field_count != field_count || depth > 16)
        return false;
    fprintf(out, "(const XrJsonDecodeFieldSpec[]){");
    for (int64_t i = 0; i < field_count; i++) {
        if (i > 0)
            fprintf(out, ", ");
        const XrType *field_type = record_type->object.field_types[i];
        uint8_t value_kind = xr_type_json_value_kind(field_type);
        fprintf(out, "{");
        xicgen_emit_c_string_literal(
            out, record_type->object.field_names[i] ? record_type->object.field_names[i] : "?");
        fprintf(out, ", %u, ", (unsigned) value_kind);
        if (xr_json_value_kind_base(value_kind) == XR_JSON_VALUE_RECORD && field_type &&
            XR_TYPE_IS_RECORD(field_type) && field_type->object.field_count > 0) {
            if (!xicgen_emit_json_decode_field_specs(out, field_type,
                                                     field_type->object.field_count, depth + 1))
                return false;
            fprintf(out, ", %u", (unsigned) field_type->object.field_count);
        } else {
            fprintf(out, "NULL, 0");
        }
        fprintf(out, "}");
    }
    fprintf(out, "}");
    return true;
}

static void xicgen_json_decode(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                               const char *prefix) {
    (void) f;
    (void) prefix;
    int64_t field_count = v ? v->aux_int : 0;
    const XrType *record_type = v ? v->type : NULL;
    const char *conv_suffix = emit_conversion_prefix(out, v ? v->type : NULL, XR_REP_TAGGED,
                                                     v ? cg_rep(v) : XR_REP_TAGGED);
    if (!v || v->nargs < 1 || field_count <= 0 || !record_type || !XR_TYPE_IS_RECORD(record_type) ||
        !record_type->object.field_types || record_type->object.field_count != field_count) {
        fprintf(out, "XR_NULL_VAL");
        emit_conversion_suffix(out, conv_suffix);
        return;
    }
    fprintf(out, "xrt_json_decode_record(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", %" PRId64 ", ", field_count);
    if (!xicgen_emit_json_decode_field_specs(out, record_type, field_count, 0)) {
        fprintf(out, "NULL");
    }
    fprintf(out, ")");
    emit_conversion_suffix(out, conv_suffix);
}

static void xicgen_struct_new(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    XR_DCHECK(v->nargs >= 1, "xicgen_struct_new: need class arg");
    if (cg_value_plan_is_struct_aggregate(ctx, v)) {
        emit_value_plan_zero_expr(ctx, out, v);
    } else if (cg_struct_inline_local_storage(ctx, f, v)) {
        /* Inlined struct initialization is emitted by the statement path. */
        fprintf(out, "XR_NULL_VAL");
    } else {
        emit_struct_fallback_new_expr(out, (XrAggregateLayout *) v->aux, prefix);
    }
}

static const XiValue *xicgen_struct_place_load(const XiValue *object) {
    const XiValue *value = object;
    while (value && cg_is_identity_copy_or_move(value) && value->nargs >= 1)
        value = value->args[0];
    return value && value->op == XI_PLACE_LOAD && value->nargs == 1 ? value : NULL;
}

static bool xicgen_emit_struct_place_field_lvalue(XiCgenCtx *ctx, FILE *out, const XiValue *object,
                                                  const XrAggregateLayout *layout,
                                                  int64_t field_index) {
    const XiValue *load = xicgen_struct_place_load(object);
    if (!load || !load->args[0] || !layout)
        return false;
    const XaotValuePlan *load_plan = cg_value_plan(ctx, load);
    const char *c_type =
        load_plan && load_plan->rep.kind == XAOT_VALUE_AGGREGATE ? load_plan->rep.c_type : NULL;
    if (!c_type)
        return false;
    char field_name[128];
    cg_struct_field_c_name(layout, field_index, field_name, sizeof(field_name));
    fprintf(out, "(*(%s *)(", c_type);
    emit_value_as_rep_ctx(ctx, out, load->args[0], XR_REP_RAWPTR);
    fprintf(out, ")).%s", field_name);
    return true;
}

static void xicgen_struct_get(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    XR_DCHECK(v->nargs >= 1, "xicgen_struct_get: need struct arg");
    if (emit_static_fixed_struct_array_field_get_expr(ctx, out, v))
        return;
    if (emit_static_struct_field_get_expr(ctx, out, v))
        return;
    if (xicgen_emit_struct_place_field_lvalue(ctx, out, v->args[0], (XrAggregateLayout *) v->aux,
                                              v->aux_int))
        return;
    if (cg_value_plan_is_struct_aggregate(ctx, v->args[0])) {
        char fname[128];
        XrAggregateLayout *sl = (XrAggregateLayout *) v->aux;
        cg_struct_field_c_name(sl, v->aux_int, fname, sizeof(fname));
        emit_vref(out, v->args[0]);
        fprintf(out, ".%s", fname);
        return;
    }
    const XiValue *origin = cg_trace_struct_new(v->args[0]);
    if (origin && cg_struct_inline_local_storage(ctx, f, origin)) {
        emit_struct_inline_field_get_expr(out, (XrAggregateLayout *) origin->aux, origin,
                                          v->aux_int, cg_value_plan_storage_rep(ctx, v),
                                          cg_value_plan_is_struct_aggregate(ctx, v));
    } else {
        XrAggregateLayout *sl = (XrAggregateLayout *) v->aux;
        emit_struct_fallback_field_get(ctx, out, f, sl, v->aux_int, v->args[0], v->type,
                                       cg_value_plan_storage_rep(ctx, v),
                                       cg_value_plan_is_struct_aggregate(ctx, v), prefix);
    }
}

static void xicgen_struct_set(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    XR_DCHECK(v->nargs >= 2, "xicgen_struct_set: need struct + value");
    XrAggregateLayout *sl = (XrAggregateLayout *) v->aux;
    const XiValue *place_load = xicgen_struct_place_load(v->args[0]);
    const XaotValuePlan *place_load_plan = place_load ? cg_value_plan(ctx, place_load) : NULL;
    if (place_load_plan && place_load_plan->rep.kind == XAOT_VALUE_AGGREGATE &&
        place_load_plan->rep.c_type) {
        const XrAggregateFieldLayout *field = cg_struct_field(sl, v->aux_int);
        if (field && field->native_type == XR_NATIVE_NESTED_AGGREGATE) {
            fprintf(out, "(memcpy(&");
            if (!xicgen_emit_struct_place_field_lvalue(ctx, out, v->args[0], sl, v->aux_int)) {
                ctx->error = true;
                emit_codegen_abort_expr(out);
                return;
            }
            fprintf(out, ", ");
            emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
            fprintf(out, ".ptr, sizeof(");
            if (!xicgen_emit_struct_place_field_lvalue(ctx, out, v->args[0], sl, v->aux_int)) {
                ctx->error = true;
                emit_codegen_abort_expr(out);
                return;
            }
            fprintf(out, ")), ");
            emit_struct_set_result_value(ctx, out, v->args[1]);
            fprintf(out, ")");
            return;
        }
        if (field && field->native_type == XR_NATIVE_ARRAY) {
            fprintf(out, "(memmove(&");
            if (!xicgen_emit_struct_place_field_lvalue(ctx, out, v->args[0], sl, v->aux_int)) {
                ctx->error = true;
                emit_codegen_abort_expr(out);
                return;
            }
            fprintf(out, "[0], ");
            emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
            fprintf(out, ".ptr, sizeof(");
            if (!xicgen_emit_struct_place_field_lvalue(ctx, out, v->args[0], sl, v->aux_int)) {
                ctx->error = true;
                emit_codegen_abort_expr(out);
                return;
            }
            fprintf(out, ")), ");
            emit_struct_set_result_value(ctx, out, v->args[1]);
            fprintf(out, ")");
            return;
        }
        fprintf(out, "(");
        if (!xicgen_emit_struct_place_field_lvalue(ctx, out, v->args[0], sl, v->aux_int)) {
            ctx->error = true;
            emit_codegen_abort_expr(out);
            return;
        }
        fprintf(out, " = ");
        emit_struct_field_store_value(ctx, out, sl, v->aux_int, v->args[1]);
        fprintf(out, ")");
        return;
    }
    if (cg_value_plan_is_struct_aggregate(ctx, v->args[0])) {
        if (emit_struct_heap_field_set_expr(ctx, out, f, sl, v->aux_int, v->args[0], v->args[1],
                                            prefix))
            return;
        fprintf(stderr,
                "[xi_cgen] ERROR: cannot emit aggregate struct field set v%u field %" PRId64
                " in %s\n",
                v->args[0] ? v->args[0]->id : 0, v->aux_int, f && f->name ? f->name : "?");
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return;
    }
    const XiValue *origin = cg_trace_struct_new(v->args[0]);
    if (origin && cg_struct_inline_local_storage(ctx, f, origin)) {
        emit_struct_inline_field_set_expr(ctx, out, (XrAggregateLayout *) origin->aux, origin,
                                          v->aux_int, v->args[1]);
    } else {
        XrAggregateLayout *sl = (XrAggregateLayout *) v->aux;
        emit_struct_fallback_field_set(ctx, out, f, sl, v->aux_int, v->args[0], v->args[1], prefix);
    }
}

static bool xicgen_fixed_array_new_info(const XiValue *v, uint8_t *native_out,
                                        uint32_t *count_out) {
    if (!v || !v->type || v->type->kind != XR_KIND_FIXED_ARRAY ||
        !v->type->fixed_array.element_type || v->type->fixed_array.length < 0 ||
        (uint64_t) v->type->fixed_array.length > XR_ARRAY_REF_MAX_COUNT)
        return false;
    XrType *elem = v->type->fixed_array.element_type;
    int native = xr_type_kind_to_native(elem->kind, elem->scalar_rep);
    if (elem->is_nullable || native == XR_NATIVE_STRING || native < 0)
        native = XR_NATIVE_VALUE;
    if (native_out)
        *native_out = (uint8_t) native;
    if (count_out)
        *count_out = (uint32_t) v->type->fixed_array.length;
    return true;
}

static void xicgen_fixed_array_new(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                   const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    uint8_t native = 0;
    uint32_t count = 0;
    if (!xicgen_fixed_array_new_info(v, &native, &count)) {
        emit_codegen_abort_expr(out);
        return;
    }
    fprintf(out, "xr_array_ref(_fa%u, %u, %u)", v->id, (unsigned) native, (unsigned) count);
}

static void xicgen_fixed_bytes_const(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                     const char *prefix) {
    if (!v || v->aux_int < 0 || (uint64_t) v->aux_int > XR_ARRAY_REF_MAX_COUNT ||
        (v->aux_int > 0 && !v->aux) || !v->type || v->type->kind != XR_KIND_FIXED_ARRAY ||
        v->type->fixed_array.length != v->aux_int || !v->type->fixed_array.element_type ||
        xr_type_kind_to_native(v->type->fixed_array.element_type->kind,
                               v->type->fixed_array.element_type->scalar_rep) != XR_NATIVE_U8) {
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return;
    }
    xicgen_fixed_array_new(ctx, out, f, v, prefix);
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
    const XiValue *present_bool_get = NULL;
    bool bool_const_value = false;
    if (cg_aot_compare_present_bool_map_get_const(ctx, v, &present_bool_get, &bool_const_value)) {
        fprintf(out, "((");
        emit_value_as_rep_ctx(ctx, out, present_bool_get, XR_REP_I64);
        fprintf(out, " != 0) %s %d)", v->op == XI_EQ ? "==" : "!=", bool_const_value ? 1 : 0);
        return;
    }
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
    XrRep a0_rep = cg_value_plan_storage_rep(ctx, v->args[0]);
    XrRep a1_rep = cg_value_plan_storage_rep(ctx, v->args[1]);
    XrRep arg_rep = (a0_rep == XR_REP_TAGGED || a1_rep == XR_REP_TAGGED) ? XR_REP_TAGGED : a0_rep;
    if (arg_rep == XR_REP_TAGGED) {
        fprintf(out, "%s(", xi_to_c_template_compare_runtime_fn(v->op));
        if (xi_to_c_template_compare_swaps_tagged_args(v->op)) {
            emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
            fprintf(out, ", ");
            emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        } else {
            emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
            fprintf(out, ", ");
            emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
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

static bool xicgen_unsigned_narrow_lowbits_binop(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                 const XiValue *v) {
    if (!ctx || !out || !v)
        return false;

    const char *cast_ctype = cg_unsigned_narrow_cast_ctype(v->op);
    if (!cast_ctype)
        return false;

    const char *op_ctype = "uint32_t";
    const XiValue *arg = cg_unsigned_narrow_lowbits_binop_arg(v);
    if (!arg)
        return false;

    const char *op = xi_to_c_template_arith_native_op(arg->op);
    if (!op || !*op)
        op = xi_to_c_template_bitwise_binary_op(arg->op);
    if (!op || !*op)
        return false;

    fprintf(out, "(%s)((%s)(", cast_ctype, op_ctype);
    cg_emit_narrow_arith_operand(ctx, f, out, arg->args[0]);
    fprintf(out, ") %s (%s)(", op, op_ctype);
    cg_emit_narrow_arith_operand(ctx, f, out, arg->args[1]);
    fprintf(out, "))");
    return true;
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
            if (xicgen_unsigned_narrow_lowbits_binop(ctx, out, f, v))
                return;
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

static bool xicgen_enum_descriptor_token(XiCgenCtx *ctx, const XiValue *v, uint32_t *out_layout_id,
                                         uint8_t *out_kind) {
    const XrType *type = v ? v->type : NULL;
    const XrType *owner =
        v && v->enum_metadata_owner ? v->enum_metadata_owner : xr_type_enum_metadata_owner(type);
    if (!ctx || !ctx->aot_bundle || !v || !owner)
        return false;
    const XaotEnumPlan *plan = xaot_bundle_find_enum_plan_for_type(ctx->aot_bundle, owner);
    XrEnumMetadataKind kind = v->enum_metadata_kind != 0
                                  ? (XrEnumMetadataKind) v->enum_metadata_kind
                                  : xr_type_enum_metadata_kind(type);
    if (!plan || plan->layout_id == 0 || kind == XR_ENUM_METADATA_NONE)
        return false;
    if (out_layout_id)
        *out_layout_id = plan->layout_id;
    if (out_kind)
        *out_kind = (uint8_t) kind;
    return true;
}

static void xicgen_enum_descriptor_box(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                       const char *prefix) {
    (void) f;
    (void) prefix;
    uint32_t layout_id = 0;
    uint8_t kind = 0;
    if (!v || v->nargs != 1 || !v->args[0] ||
        !xicgen_enum_descriptor_token(ctx, v, &layout_id, &kind)) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: invalid erased enum descriptor box\n");
        emit_codegen_abort_expr(out);
        return;
    }
    fprintf(out, "xrt_enum_descriptor_box_new(%u, %u, ", layout_id, (unsigned) kind);
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
    fprintf(out, ")");
}

static void xicgen_enum_descriptor_unbox(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                         const XiValue *v, const char *prefix) {
    (void) f;
    (void) prefix;
    if (!v || v->nargs != 1 || !v->args[0]) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: invalid erased enum descriptor unbox\n");
        emit_codegen_abort_expr(out);
        return;
    }
    fprintf(out, "xrt_enum_descriptor_unbox(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ")");
}

static uint32_t xicgen_enum_layout_id_for_data(const XiEnumData *ed) {
    if (!ed)
        return 0;
    if (ed->layout_id != 0)
        return ed->layout_id;
    const XrType *type = (const XrType *) ed->runtime_type;
    if (type && type->kind == XR_KIND_ENUM)
        return type->enum_type.layout && type->enum_type.layout->layout_id != 0
                   ? type->enum_type.layout->layout_id
                   : type->enum_type.layout_id;
    return 0;
}

static bool xicgen_is_enum_evidence(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v,
                                    const XrType *target, uint32_t *out_layout_id) {
    uint32_t layout_id = 0;
    bool found = false;
    if (target && target->kind == XR_KIND_ENUM) {
        layout_id = cg_enum_layout_id_for_type(ctx, target);
        found = true;
    }
    if ((!found || layout_id == 0) && v && v->nargs >= 2) {
        const XiEnumData *ed = cg_enum_for_shared_value_in_func(ctx, f, v->args[1]);
        if (!ed)
            ed = cg_resolve_imported_enum_value(ctx, f, v->args[1]);
        if (ed) {
            uint32_t data_layout_id = xicgen_enum_layout_id_for_data(ed);
            if (data_layout_id != 0)
                layout_id = data_layout_id;
            found = true;
        }
    }
    if (out_layout_id)
        *out_layout_id = layout_id;
    return found;
}

static void xicgen_emit_enum_is_predicate(FILE *out, const XiValue *value, uint32_t layout_id) {
    fprintf(out, "(");
    emit_vref(out, value);
    fprintf(out, ".tag == XR_TAG_ENUM");
    if (layout_id != 0) {
        fprintf(out, " && (xrt_enum_value_layout_id(");
        emit_vref(out, value);
        fprintf(out, ") == 0 || xrt_enum_value_layout_id(");
        emit_vref(out, value);
        fprintf(out, ") == %u)", (unsigned) layout_id);
    }
    fprintf(out, ")");
}

static void xicgen_is(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                      const char *prefix) {
    (void) prefix;
    XR_DCHECK(v->nargs >= 1, "xicgen_is: missing arg");
    struct XrType *target = (struct XrType *) v->aux;
    if (xr_type_is_enum_metadata(target)) {
        const XrType *owner = xr_type_enum_metadata_owner(target);
        const XaotEnumPlan *plan = ctx && ctx->aot_bundle && owner
                                       ? xaot_bundle_find_enum_plan_for_type(ctx->aot_bundle, owner)
                                       : NULL;
        XrEnumMetadataKind kind = xr_type_enum_metadata_kind(target);
        if (!plan || plan->layout_id == 0 || kind == XR_ENUM_METADATA_NONE) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: enum descriptor is-check lacks verified plan\n");
            emit_codegen_abort_expr(out);
            return;
        }
        uint32_t source_layout_id = 0;
        uint8_t source_kind = 0;
        if (v->args[0] &&
            xicgen_enum_descriptor_token(ctx, v->args[0], &source_layout_id, &source_kind)) {
            fprintf(out, "%d",
                    source_layout_id == plan->layout_id && source_kind == (uint8_t) kind);
            return;
        }
        fprintf(out, "xrt_enum_descriptor_matches(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", %u, %u)", plan->layout_id, (unsigned) kind);
        return;
    }
    uint32_t enum_layout_id = 0;
    if (xicgen_is_enum_evidence(ctx, f, v, target, &enum_layout_id)) {
        const XaotValuePlan *input_plan = cg_value_plan(ctx, v->args[0]);
        if (ctx && ctx->freestanding_profile && v->args[0] && v->args[0]->op == XI_ERR_CATCH &&
            input_plan && (input_plan->rep.flags & XAOT_VALUE_FLAG_ENUM) != 0) {
            uint32_t input_layout_id = cg_enum_layout_id_for_type(ctx, input_plan->rep.type);
            fprintf(out, "%d",
                    input_layout_id == 0 || enum_layout_id == 0 ||
                        input_layout_id == enum_layout_id);
            return;
        }
        xicgen_emit_enum_is_predicate(out, v->args[0], enum_layout_id);
        return;
    }
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
        case XR_KIND_ENUM:
            xicgen_emit_enum_is_predicate(out, v->args[0], cg_enum_layout_id_for_type(ctx, target));
            break;
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

static bool emit_fixed_array_ref_length_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    if (!v || v->op != XI_LEN || v->nargs != 1)
        return false;
    if (!cg_value_type_is_fixed_array(v->args[0]))
        return false;
    const char *conv_suffix =
        emit_conversion_prefix(out, v->type, XR_REP_I64, cg_value_plan_storage_rep(ctx, v));
    int length = v->args[0]->type ? v->args[0]->type->fixed_array.length : 0;
    fprintf(out, "INT64_C(%d)", length);
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool xicgen_load_field_receiver_is_map_backed_class(XiCgenCtx *ctx, const XiValue *recv) {
    const XrType *type = recv ? recv->type : NULL;
    const char *class_name = type ? xr_type_get_class_name((XrType *) (uintptr_t) type) : NULL;
    if (!class_name || cg_type_is_json(type))
        return false;
    const XiClassData *cd = cg_class_native_data_by_name(ctx, class_name);
    return cd && !cg_class_native_value_type_data(ctx, recv);
}

static void xicgen_load_field(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    XR_DCHECK(v->nargs >= 1, "xicgen_load_field: need object");
    const char *field = (const char *) v->aux;
    if (field && v->args[0] && v->args[0]->type && v->args[0]->type->kind == XR_KIND_ENUM) {
        const char *helper = NULL;
        if (strcmp(field, "name") == 0) {
            const XrType *recv_type = v->args[0]->type;
            if (xicgen_emit_freestanding_enum_layout_name_expr(ctx, out, v->args[0],
                                                               recv_type->enum_type.layout,
                                                               cg_value_plan_storage_rep(ctx, v)))
                return;
            helper = "xrt_enum_box_name";
        } else if (strcmp(field, "ordinal") == 0) {
            if (ctx && cg_value_plan_storage_rep(ctx, v->args[0]) == XR_REP_I64) {
                const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64,
                                                                 cg_value_plan_storage_rep(ctx, v));
                emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
                emit_conversion_suffix(out, conv_suffix);
                return;
            }
            helper = "xrt_enum_box_ordinal";
        }
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
    /* Bare ADT-enum variant access (e.g. `Recv.Empty`) builds an ADT aggregate,
     * not the enum singleton. Non-payload variants must still use the aggregate
     * representation so pattern matching can read the same logical tag as
     * payload variants. */
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
    if (field) {
        const XiValue *recv = cg_unwrap_identity_value(v->args[0]);
        if (recv && recv->op == XI_GET_BUILTIN &&
            emit_static_prelude_enum_member_value_expr(ctx, out, v, (int) recv->aux_int, field))
            return;
    }
    if (field) {
        const XiEnumData *recv_enum = cg_enum_for_namespace_value(v->args[0]);
        if (!recv_enum)
            recv_enum = cg_enum_for_shared_value_in_func(ctx, f, v->args[0]);
        if (!recv_enum)
            recv_enum = cg_resolve_imported_enum_value(ctx, f, v->args[0]);
        /* Optimisation may replace the enum namespace receiver with an
         * identity/constant value before C emission.  The result still has a
         * concrete enum type, so recover the declaration from the prepared
         * enum-domain plan instead of falling back to a runtime namespace
         * map. */
        if (!recv_enum && ctx && ctx->aot_bundle && v->type && v->type->kind == XR_KIND_ENUM) {
            const XaotEnumPlan *enum_plan =
                xaot_bundle_find_enum_plan_for_type(ctx->aot_bundle, v->type);
            if (enum_plan)
                recv_enum = enum_plan->enum_data;
        }
        int midx = cg_enum_member_index(recv_enum, field);
        if (recv_enum && midx >= 0 &&
            emit_static_enum_member_value_expr(ctx, out, v, recv_enum, (uint32_t) midx))
            return;
        const XaBuiltinEnum *builtin_enum =
            cg_resolve_imported_builtin_enum_value(ctx, f, v->args[0]);
        int builtin_midx = cg_builtin_enum_member_index(builtin_enum, field);
        if (builtin_enum && builtin_midx >= 0 &&
            emit_static_builtin_enum_member_value_expr(ctx, out, v, builtin_enum,
                                                       (uint32_t) builtin_midx))
            return;
    }
    if (emit_static_fixed_tuple_array_get_expr(ctx, out, v))
        return;
    if (emit_static_tuple_get_expr(ctx, out, v))
        return;
    if (emit_static_fixed_struct_array_field_get_expr(ctx, out, v))
        return;
    if (emit_static_struct_field_get_expr(ctx, out, v))
        return;
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
         * module const/var exports (path.sep, ...) read xrt_shared_<mod>[slot]
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
        const char *conv_suffix = emit_tagged_to_value_storage_prefix(ctx, out, v);
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
    const char *thread_helper =
        xi_value_type_is_thread(v->args[0]) ? cg_thread_field_helper(field) : NULL;
    if (thread_helper) {
        if (cg_rep(v) == XR_REP_I64)
            fprintf(out, "XR_TO_INT(");
        else if (cg_rep(v) == XR_REP_F64)
            fprintf(out, "XR_TO_FLOAT(");
        fprintf(out, "%s(", thread_helper);
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
    if (field && cg_emit_aot_stdlib_generated_constant_field(ctx, out, f, v))
        return;
    if (field && emit_class_native_getter_field_expr(ctx, out, f, prefix, v))
        return;
    bool map_backed_class_field =
        field && xicgen_load_field_receiver_is_map_backed_class(ctx, v->args[0]);
    bool json_receiver = cg_value_type_is_json(v->args[0]);
    int sym = cg_method_sym(field);
    const XiValue *receiver = xicgen_getprop_receiver_value(ctx, v->args[0]);
    if (sym >= 0 && !map_backed_class_field && !json_receiver) {
        const char *conv_suffix = emit_tagged_to_value_storage_prefix(ctx, out, v);
        fprintf(out, "xrt_getprop(");
        emit_value_as_rep_ctx(ctx, out, receiver, XR_REP_TAGGED);
        fprintf(out, ", %d)", sym);
        emit_conversion_suffix(out, conv_suffix);
    } else {
        if (json_receiver) {
            const XaotJsonAccessPlan *json_plan = xicgen_json_shape_guard_plan(
                ctx, v, XG_JSON_ACCESS_FIELD_GET, XG_JSON_ACCESS_INDEX_GET);
            if (ctx && ctx->error) {
                emit_codegen_abort_expr(out);
                return;
            }
            if (json_plan) {
                xicgen_emit_json_shape_guard_get(ctx, out, receiver, json_plan, field, v->type,
                                                 cg_value_plan_storage_rep(ctx, v));
                return;
            }
        }
        const char *conv_suffix = emit_tagged_to_value_storage_prefix(ctx, out, v);
        if (json_receiver) {
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
        const XaotJsonAccessPlan *json_plan = xicgen_json_shape_guard_plan(
            ctx, v, XG_JSON_ACCESS_FIELD_SET, XG_JSON_ACCESS_INDEX_SET);
        if (ctx && ctx->error) {
            emit_codegen_abort_expr(out);
            return;
        }
        if (json_plan) {
            xicgen_emit_json_shape_guard_set(ctx, out, v->args[0], v->args[1], json_plan, field);
            return;
        }
        fprintf(out, "xrt_json_set_name(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        xicgen_emit_c_string_literal(out, field ? field : "?");
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ")");
    } else {
        fprintf(out, "xrt_setprop_name(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        xicgen_emit_c_string_literal(out, field ? field : "?");
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ")");
    }
}

static const XaotKeyAccessPlan *xicgen_checked_key_access_plan(XiCgenCtx *ctx, const XiValue *v,
                                                               uint8_t expected_op) {
    if (!v || v->xg_key_access_id == 0)
        return NULL;
    const char *site = expected_op == XG_KEY_ACCESS_INDEX_GET ? "Map.index_get" : "Map.index_set";
    const XaotKeyAccessPlan *plan =
        cg_verified_key_access_plan(ctx, v, XG_MAP_CONTAINER_MAP, expected_op, site);
    if (!cg_key_access_plan_action_has_backend(ctx, plan, v, site))
        return NULL;
    return plan;
}

static bool xicgen_map_index_plan_is_prehashed(const XaotKeyAccessPlan *plan) {
    return plan && plan->action == XAOT_KEY_ACCESS_PREHASHED_LOOKUP && plan->key_prehash != 0;
}

static bool xicgen_map_index_plan_is_bool_direct(const XaotKeyAccessPlan *plan) {
    return plan && plan->action == XAOT_KEY_ACCESS_BOOL_DIRECT_LOOKUP;
}

static bool xicgen_map_index_plan_is_small_scan(const XaotKeyAccessPlan *plan) {
    return plan && plan->action == XAOT_KEY_ACCESS_INLINE_SMALL_SCAN;
}

static bool xicgen_map_index_plan_is_dense_index(const XaotKeyAccessPlan *plan) {
    return plan && plan->action == XAOT_KEY_ACCESS_DIRECT_DENSE_INDEX;
}

static void xicgen_emit_map_ptr_from_tagged(XiCgenCtx *ctx, FILE *out, const XiValue *value) {
    fprintf(out, "((xrt_map_t*)(");
    emit_value_as_rep_ctx(ctx, out, value, XR_REP_TAGGED);
    fprintf(out, ").ptr)");
}

static bool xicgen_emit_map_index_get_builtin_hash_eq(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                                      const XaotKeyAccessPlan *plan) {
    if (!v || v->nargs < 2 || !v->args[0] || !v->args[0]->type ||
        v->args[0]->type->kind != XR_KIND_MAP ||
        !cg_key_access_plan_uses_builtin_hash_eq_backend(ctx, plan))
        return false;
    const char *conv_suffix = emit_tagged_to_value_storage_prefix(ctx, out, v);
    fprintf(out, "xrt_map_index_get_owned(");
    xicgen_emit_map_ptr_from_tagged(ctx, out, v->args[0]);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ")");
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool xicgen_emit_map_index_set_builtin_hash_eq(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                                      const XaotKeyAccessPlan *plan) {
    if (!v || v->nargs < 3 || !v->args[0] || !v->args[0]->type ||
        v->args[0]->type->kind != XR_KIND_MAP ||
        !cg_key_access_plan_uses_builtin_hash_eq_backend(ctx, plan))
        return false;
    fprintf(out, "xrt_map_set(");
    xicgen_emit_map_ptr_from_tagged(ctx, out, v->args[0]);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_TAGGED);
    fprintf(out, ")");
    return true;
}

static bool xicgen_emit_map_index_get_prehashed(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                                const XaotKeyAccessPlan *plan) {
    if (!v || v->nargs < 2 || !v->args[0] || !v->args[0]->type ||
        v->args[0]->type->kind != XR_KIND_MAP || !xicgen_map_index_plan_is_prehashed(plan))
        return false;
    const char *conv_suffix = emit_tagged_to_value_storage_prefix(ctx, out, v);
    fprintf(out, "xrt_map_index_get_prehashed_owned(");
    xicgen_emit_map_ptr_from_tagged(ctx, out, v->args[0]);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ", UINT32_C(0x%08" PRIx32 "))", (uint32_t) plan->key_prehash);
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool xicgen_emit_map_index_get_bool_direct(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                                  const XaotKeyAccessPlan *plan) {
    if (!v || v->nargs < 2 || !v->args[0] || !v->args[0]->type ||
        v->args[0]->type->kind != XR_KIND_MAP || !xicgen_map_index_plan_is_bool_direct(plan))
        return false;
    const char *conv_suffix = emit_tagged_to_value_storage_prefix(ctx, out, v);
    fprintf(out, "xrt_boolmap_index_get_v((xrt_boolmap_t*)");
    xicgen_emit_map_ptr_from_tagged(ctx, out, v->args[0]);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ")");
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool xicgen_emit_map_index_get_small_scan(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                                 const XaotKeyAccessPlan *plan) {
    if (!v || v->nargs < 2 || !v->args[0] || !v->args[0]->type ||
        v->args[0]->type->kind != XR_KIND_MAP || !xicgen_map_index_plan_is_small_scan(plan))
        return false;
    const char *conv_suffix = emit_tagged_to_value_storage_prefix(ctx, out, v);
    fprintf(out, "xrt_map_index_get_small_owned(");
    xicgen_emit_map_ptr_from_tagged(ctx, out, v->args[0]);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ")");
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool xicgen_emit_map_index_get_dense_index(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                                  const XaotKeyAccessPlan *plan) {
    if (!v || v->nargs < 2 || !v->args[0] || !v->args[0]->type ||
        v->args[0]->type->kind != XR_KIND_MAP || !xicgen_map_index_plan_is_dense_index(plan))
        return false;
    bool dense_enum = cg_key_access_plan_is_dense_enum_index(ctx, plan, "Map.index_get");
    if (ctx && ctx->error) {
        emit_codegen_abort_expr(out);
        return true;
    }
    const char *conv_suffix = emit_tagged_to_value_storage_prefix(ctx, out, v);
    fprintf(out, dense_enum ? "xrt_map_index_get_dense_enum_owned("
                            : "xrt_map_index_get_dense_i64_owned(");
    xicgen_emit_map_ptr_from_tagged(ctx, out, v->args[0]);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ")");
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool xicgen_emit_map_index_get_user_hash_eq(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                                   const XaotKeyAccessPlan *plan) {
    if (!v || v->nargs < 2 || !v->args[0] || !v->args[0]->type ||
        v->args[0]->type->kind != XR_KIND_MAP || !plan ||
        plan->action != XAOT_KEY_ACCESS_SPECIALIZED_HASH_LOOKUP)
        return false;
    CgUserHashEqDirectPlan user_hash_eq;
    if (!cg_user_hash_eq_direct_plan(ctx, plan, v->args[1], &user_hash_eq, "Map.index_get")) {
        if (ctx && ctx->error) {
            emit_codegen_abort_expr(out);
            return true;
        }
        return false;
    }
    const char *conv_suffix = emit_tagged_to_value_storage_prefix(ctx, out, v);
    fprintf(out, "xrt_map_index_get_user_hash_eq_owned(");
    xicgen_emit_map_ptr_from_tagged(ctx, out, v->args[0]);
    fprintf(out, ", ");
    cg_emit_user_hash_eq_tagged_key(ctx, out, v->args[1]);
    if (!cg_emit_user_hash_eq_direct_args(ctx, out, &user_hash_eq)) {
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }
    fprintf(out, ")");
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool xicgen_emit_map_index_get_derived_hash_eq(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                                      const XaotKeyAccessPlan *plan) {
    if (!v || v->nargs < 2 || !v->args[0] || !v->args[0]->type ||
        v->args[0]->type->kind != XR_KIND_MAP || !plan ||
        plan->action != XAOT_KEY_ACCESS_SPECIALIZED_HASH_LOOKUP)
        return false;
    CgDerivedHashEqPlan derived_hash_eq;
    if (!cg_derived_hash_eq_plan(ctx, plan, v->args[1], &derived_hash_eq, "Map.index_get")) {
        if (ctx && ctx->error) {
            emit_codegen_abort_expr(out);
            return true;
        }
        return false;
    }
    const char *conv_suffix = emit_tagged_to_value_storage_prefix(ctx, out, v);
    fprintf(out, "xrt_map_index_get_user_hash_eq_owned(");
    xicgen_emit_map_ptr_from_tagged(ctx, out, v->args[0]);
    fprintf(out, ", ");
    cg_emit_user_hash_eq_tagged_key(ctx, out, v->args[1]);
    if (!cg_emit_derived_hash_eq_args(ctx, out, &derived_hash_eq)) {
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }
    fprintf(out, ")");
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool xicgen_emit_map_index_set_prehashed(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                                const XaotKeyAccessPlan *plan) {
    if (!v || v->nargs < 3 || !v->args[0] || !v->args[0]->type ||
        v->args[0]->type->kind != XR_KIND_MAP || !xicgen_map_index_plan_is_prehashed(plan))
        return false;
    fprintf(out, "xrt_map_set_prehashed(");
    xicgen_emit_map_ptr_from_tagged(ctx, out, v->args[0]);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_TAGGED);
    fprintf(out, ", UINT32_C(0x%08" PRIx32 "))", (uint32_t) plan->key_prehash);
    return true;
}

static bool xicgen_emit_map_index_set_derived_hash_eq(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                                      const XaotKeyAccessPlan *plan) {
    if (!v || v->nargs < 3 || !v->args[0] || !v->args[0]->type ||
        v->args[0]->type->kind != XR_KIND_MAP || !plan ||
        plan->action != XAOT_KEY_ACCESS_SPECIALIZED_HASH_LOOKUP)
        return false;
    CgDerivedHashEqPlan derived_hash_eq;
    if (!cg_derived_hash_eq_plan(ctx, plan, v->args[1], &derived_hash_eq, "Map.index_set")) {
        if (ctx && ctx->error) {
            emit_codegen_abort_expr(out);
            return true;
        }
        return false;
    }
    fprintf(out, "xrt_map_set_user_hash_eq(");
    xicgen_emit_map_ptr_from_tagged(ctx, out, v->args[0]);
    fprintf(out, ", ");
    cg_emit_user_hash_eq_tagged_key(ctx, out, v->args[1]);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_TAGGED);
    if (!cg_emit_derived_hash_eq_args(ctx, out, &derived_hash_eq))
        return true;
    fprintf(out, ")");
    return true;
}

static bool xicgen_emit_map_index_set_user_hash_eq(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                                   const XaotKeyAccessPlan *plan) {
    if (!v || v->nargs < 3 || !v->args[0] || !v->args[0]->type ||
        v->args[0]->type->kind != XR_KIND_MAP || !plan ||
        plan->action != XAOT_KEY_ACCESS_SPECIALIZED_HASH_LOOKUP)
        return false;
    CgUserHashEqDirectPlan user_hash_eq;
    if (!cg_user_hash_eq_direct_plan(ctx, plan, v->args[1], &user_hash_eq, "Map.index_set")) {
        if (ctx && ctx->error) {
            emit_codegen_abort_expr(out);
            return true;
        }
        return false;
    }
    fprintf(out, "xrt_map_set_user_hash_eq(");
    xicgen_emit_map_ptr_from_tagged(ctx, out, v->args[0]);
    fprintf(out, ", ");
    cg_emit_user_hash_eq_tagged_key(ctx, out, v->args[1]);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_TAGGED);
    if (!cg_emit_user_hash_eq_direct_args(ctx, out, &user_hash_eq))
        return true;
    fprintf(out, ")");
    return true;
}

static void xicgen_enum_variant_at(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                   const char *prefix) {
    (void) f;
    (void) prefix;
    XR_DCHECK(v && v->nargs >= 2, "xicgen_enum_variant_at: missing operands");
    const char *suffix =
        emit_conversion_prefix(out, v->type, XR_REP_I64, cg_value_plan_storage_rep(ctx, v));
    fprintf(out, "({ int64_t _n = ");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
    fprintf(out, "; int64_t _i = ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
    fprintf(out, "; if (_i < 0 || _i >= _n) xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "
                 "\"enum variant index out of bounds\"); _i; })");
    emit_conversion_suffix(out, suffix);
}

static void xicgen_enum_payload_at(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                   const char *prefix) {
    (void) f;
    (void) prefix;
    XR_DCHECK(v && v->nargs >= 2, "xicgen_enum_payload_at: missing operands");
    const char *suffix =
        emit_conversion_prefix(out, v->type, XR_REP_I64, cg_value_plan_storage_rep(ctx, v));
    fprintf(out, "({ uint64_t _p = (uint64_t)(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
    fprintf(out, "); int64_t _i = ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
    fprintf(out, "; uint32_t _n = (uint32_t)(_p >> 32); if (_i < 0 || (uint64_t)_i >= _n) "
                 "xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "
                 "\"enum payload field index out of bounds\"); "
                 "(int64_t)(((uint64_t)(uint32_t)_p << 32) | (uint32_t)_i); })");
    emit_conversion_suffix(out, suffix);
}

static uint32_t xicgen_enum_meta_use_bit(int64_t field) {
    switch ((XaEnumMetaField) field) {
        case XA_ENUM_META_NAME:
            return XAOT_ENUM_USE_VARIANT_NAME;
        case XA_ENUM_META_PAYLOAD_COUNT:
            return XAOT_ENUM_USE_PAYLOAD_COUNT;
        case XA_ENUM_META_PAYLOAD_NAME:
            return XAOT_ENUM_USE_PAYLOAD_NAME;
        case XA_ENUM_META_PAYLOAD_TYPE:
            return XAOT_ENUM_USE_PAYLOAD_TYPE;
        default:
            return 0;
    }
}

static const XaotEnumPlan *xicgen_enum_meta_plan(XiCgenCtx *ctx, const XiFunc *f,
                                                 const XiValue *v) {
    if (!v || v->nargs < 2)
        return NULL;
    if (ctx && ctx->aot_bundle && v->enum_metadata_owner) {
        const XaotEnumPlan *plan =
            xaot_bundle_find_enum_plan_for_type(ctx->aot_bundle, v->enum_metadata_owner);
        if (plan)
            return plan;
    }
    const XiValue *enum_value = cg_unwrap_identity_value(v->args[0]);
    const XiEnumData *ed = cg_enum_for_shared_value_in_func(ctx, f, enum_value);
    if (!ed)
        ed = cg_resolve_imported_enum_value(ctx, f, enum_value);
    /* A module initializer may still hold the canonical enum namespace as
     * its compile-time runtime-type constant before the SET_SHARED.  Resolve
     * that identity directly instead of forcing a temporary runtime Map. */
    if (!ed && enum_value && enum_value->op == XI_CONST && enum_value->aux)
        ed = cg_enum_for_runtime_type(ctx, enum_value->aux);
    return ctx && ctx->aot_bundle && ed ? xaot_bundle_find_enum_plan(ctx->aot_bundle, ed) : NULL;
}

static uint32_t xicgen_enum_payload_field_count(const XaotEnumPlan *plan) {
    uint64_t total = 0;
    if (!plan || !plan->members)
        return 0;
    for (uint32_t i = 0; i < plan->member_count; i++) {
        int count = plan->members[i].payload_count;
        if (count > 0)
            total += (uint32_t) count;
    }
    return total <= UINT32_MAX ? (uint32_t) total : UINT32_MAX;
}

static void xicgen_emit_enum_payload_offsets(FILE *out, const XaotEnumPlan *plan) {
    uint32_t offset = 0;
    fprintf(out, "static const uint32_t _xenum_offsets[%u] = {0",
            (unsigned) plan->member_count + 1u);
    for (uint32_t i = 0; i < plan->member_count; i++) {
        int count = plan->members[i].payload_count;
        if (count > 0)
            offset += (uint32_t) count;
        fprintf(out, ",%u", (unsigned) offset);
    }
    fprintf(out, "}; ");
}

static void xicgen_emit_enum_payload_values(XiCgenCtx *ctx, FILE *out, const XaotEnumPlan *plan,
                                            bool strings) {
    uint32_t total = xicgen_enum_payload_field_count(plan);
    fprintf(out, "static const %s _xenum_values[%u] = {", strings ? "XrValue" : "uint16_t",
            (unsigned) (total > 0 ? total : 1u));
    if (total == 0) {
        fprintf(out, strings ? "{0}" : "0");
    } else {
        bool first = true;
        for (uint32_t i = 0; i < plan->member_count; i++) {
            const XiEnumMemberData *member = &plan->members[i];
            for (int p = 0; p < member->payload_count; p++) {
                if (!first)
                    fprintf(out, ",");
                first = false;
                if (strings) {
                    const char *name = member->payload_names && member->payload_names[p]
                                           ? member->payload_names[p]
                                           : "";
                    cg_emit_static_str_value_initializer(ctx, out, name);
                } else {
                    fprintf(out, "%u",
                            (unsigned) xr_type_to_tid(
                                member->payload_types ? member->payload_types[p] : NULL));
                }
            }
        }
    }
    fprintf(out, "}; ");
}

static void xicgen_enum_meta_get(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                 const char *prefix) {
    (void) prefix;
    const XaotEnumPlan *plan = xicgen_enum_meta_plan(ctx, f, v);
    const XiEnumData *ed = plan ? plan->enum_data : NULL;
    uint32_t required_bit = xicgen_enum_meta_use_bit(v ? v->aux_int : 0);
    if (!plan || !ed || !plan->members || required_bit == 0 ||
        (plan->descriptor_use_bits & required_bit) == 0) {
        ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: enum metadata access lacks verified field reachability\n");
        emit_codegen_abort_expr(out);
        return;
    }

    bool string_result = v->aux_int == XA_ENUM_META_NAME || v->aux_int == XA_ENUM_META_PAYLOAD_NAME;
    XrRep source_rep = string_result ? XR_REP_TAGGED : XR_REP_I64;
    const char *suffix =
        emit_conversion_prefix(out, v->type, source_rep, cg_value_plan_storage_rep(ctx, v));
    if (v->aux_int == XA_ENUM_META_NAME) {
        fprintf(out, "({ int64_t _i = ");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
        fprintf(out, "; static const XrValue _xenum_names[%u] = {",
                (unsigned) (ed->member_count > 0 ? ed->member_count : 1u));
        for (uint32_t i = 0; i < ed->member_count; i++) {
            if (i > 0)
                fprintf(out, ",");
            cg_emit_static_str_value_initializer(
                ctx, out, plan->members[i].name ? plan->members[i].name : "");
        }
        if (ed->member_count == 0)
            fprintf(out, "{0}");
        fprintf(out,
                "}; if (_i < 0 || (uint64_t)_i >= UINT64_C(%u)) "
                "xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "
                "\"enum variant ordinal out of bounds\"); _xenum_names[_i]; })",
                (unsigned) ed->member_count);
    } else if (v->aux_int == XA_ENUM_META_PAYLOAD_COUNT) {
        fprintf(out, "({ int64_t _i = ");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
        fprintf(out, "; static const uint16_t _xenum_counts[%u] = {",
                (unsigned) (ed->member_count > 0 ? ed->member_count : 1u));
        for (uint32_t i = 0; i < ed->member_count; i++) {
            if (i > 0)
                fprintf(out, ",");
            fprintf(out, "%u", (unsigned) plan->members[i].payload_count);
        }
        if (ed->member_count == 0)
            fprintf(out, "0");
        fprintf(out,
                "}; if (_i < 0 || (uint64_t)_i >= UINT64_C(%u)) "
                "xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "
                "\"enum variant ordinal out of bounds\"); (int64_t)_xenum_counts[_i]; })",
                (unsigned) ed->member_count);
    } else {
        fprintf(out, "({ uint64_t _p = (uint64_t)(");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
        fprintf(out, "); uint32_t _v = (uint32_t)(_p >> 32), _f = (uint32_t)_p; ");
        xicgen_emit_enum_payload_offsets(out, plan);
        xicgen_emit_enum_payload_values(ctx, out, plan, string_result);
        fprintf(out,
                "if (_v >= UINT32_C(%u)) "
                "xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "
                "\"enum variant ordinal out of bounds\"); "
                "uint32_t _begin = _xenum_offsets[_v], _end = _xenum_offsets[_v + 1u]; "
                "if (_f >= _end - _begin) "
                "xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "
                "\"enum payload field index out of bounds\"); "
                "%s_xenum_values[_begin + _f]; })",
                (unsigned) ed->member_count, string_result ? "" : "(int64_t)");
    }
    emit_conversion_suffix(out, suffix);
}

static void xicgen_index_get(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                             const char *prefix) {
    XR_DCHECK(v->nargs >= 2, "xicgen_index_get: need obj and key");
    if (v->aux_kind == XI_AUX_KIND_ENUM_CASE) {
        const XaotEnumPlan *enum_plan =
            ctx && ctx->aot_bundle && v->enum_metadata_owner
                ? xaot_bundle_find_enum_plan_for_type(ctx->aot_bundle, v->enum_metadata_owner)
                : NULL;
        const XiEnumData *ed =
            enum_plan ? enum_plan->enum_data : cg_enum_for_shared_value_in_func(ctx, f, v->args[0]);
        if (!ed)
            ed = cg_resolve_imported_enum_value(ctx, f, v->args[0]);
        if (!enum_plan || !enum_plan->value_iteration_reachable || !ed || ed->is_adt ||
            ed->member_count == 0) {
            ctx->error = true;
            fprintf(stderr,
                    "[xi_cgen] ERROR: enum case iteration lacks verified unit-only reachability\n");
            emit_codegen_abort_expr(out);
            return;
        }
        if (cg_value_plan_storage_rep(ctx, v) == XR_REP_I64) {
            fprintf(out, "({ int64_t _xenum_i = ");
            emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
            fprintf(out,
                    "; if (_xenum_i < 0 || (uint64_t)_xenum_i >= UINT64_C(%u)) "
                    "xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "
                    "\"enum case ordinal out of bounds\"); _xenum_i; })",
                    (unsigned) ed->member_count);
            return;
        }
        const char *conv_suffix =
            emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_value_plan_storage_rep(ctx, v));
        fprintf(out, "({ int64_t _xenum_i = ");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
        fprintf(out, "; XrValue _xenum_v = XR_NULL_VAL; switch (_xenum_i) {");
        for (uint32_t i = 0; i < ed->member_count; i++) {
            fprintf(out, " case %u: _xenum_v = ", (unsigned) i);
            emit_static_enum_member_value_expr(ctx, out, v, ed, i);
            fprintf(out, "; break;");
        }
        fprintf(out, " default: xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "
                     "\"enum case ordinal out of bounds\"); } _xenum_v; })");
        emit_conversion_suffix(out, conv_suffix);
        return;
    }
    const XaotKeyAccessPlan *key_plan =
        xicgen_checked_key_access_plan(ctx, v, XG_KEY_ACCESS_INDEX_GET);
    if (ctx && ctx->error) {
        emit_codegen_abort_expr(out);
        return;
    }
    if (emit_struct_fixed_array_index_get_expr(ctx, out, f, v, prefix) ||
        emit_fixed_array_index_get_expr(ctx, out, f, v) ||
        emit_span_index_get_expr(ctx, out, f, v) ||
        emit_typed_array_index_get_expr(ctx, out, f, v, prefix) ||
        emit_class_native_array_index_get_expr(ctx, out, f, v))
        return;
    if (xicgen_emit_map_index_get_bool_direct(ctx, out, v, key_plan))
        return;
    if (xicgen_emit_map_index_get_dense_index(ctx, out, v, key_plan))
        return;
    if (xicgen_emit_map_index_get_derived_hash_eq(ctx, out, v, key_plan))
        return;
    if (xicgen_emit_map_index_get_user_hash_eq(ctx, out, v, key_plan))
        return;
    if (xicgen_emit_map_index_get_builtin_hash_eq(ctx, out, v, key_plan))
        return;
    if (xicgen_emit_map_index_get_prehashed(ctx, out, v, key_plan))
        return;
    if (xicgen_emit_map_index_get_small_scan(ctx, out, v, key_plan))
        return;
    if (cg_value_type_is_json(v->args[0])) {
        const char *static_key = xicgen_static_string_const(v->args[1]);
        const XaotJsonAccessPlan *json_plan =
            static_key ? xicgen_json_shape_guard_plan(ctx, v, XG_JSON_ACCESS_INDEX_GET,
                                                      XG_JSON_ACCESS_FIELD_GET)
                       : NULL;
        if (!static_key)
            json_plan = xicgen_json_computed_key_guard_plan(ctx, v, XG_JSON_ACCESS_INDEX_GET);
        if (ctx && ctx->error) {
            emit_codegen_abort_expr(out);
            return;
        }
        if (json_plan) {
            if (static_key)
                xicgen_emit_json_shape_guard_get(ctx, out, v->args[0], json_plan, static_key,
                                                 v->type, cg_value_plan_storage_rep(ctx, v));
            else
                xicgen_emit_json_computed_key_guard_get(ctx, out, v->args[0], v->args[1], v->type,
                                                        cg_value_plan_storage_rep(ctx, v));
            return;
        }
    }
    const char *conv_suffix = emit_tagged_to_value_storage_prefix(ctx, out, v);
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
    if (v->xg_map_shape_id != XG_NO_ID) {
        const XaotMapShapePlan *plan = xicgen_static_map_shape_plan(ctx, v);
        if (!plan || plan->container_kind != XG_MAP_CONTAINER_MAP)
            emit_codegen_abort_expr(out);
        else
            fprintf(out, "((void)0)");
        return;
    }
    const XaotKeyAccessPlan *key_plan = xicgen_checked_key_access_plan(ctx, v, XG_KEY_ACCESS_SET);
    if (ctx && ctx->error) {
        emit_codegen_abort_expr(out);
        return;
    }
    if (emit_struct_fixed_array_index_set_expr(ctx, out, f, v, prefix))
        return;
    if (emit_fixed_array_index_set_expr(ctx, out, f, v))
        return;
    if (emit_span_index_set_expr(ctx, out, f, v))
        return;
    if (emit_typed_array_index_set_expr(ctx, out, f, v, prefix))
        return;
    if (emit_class_native_array_index_set_expr(ctx, out, f, v))
        return;
    if (xicgen_emit_map_index_set_derived_hash_eq(ctx, out, v, key_plan))
        return;
    if (xicgen_emit_map_index_set_user_hash_eq(ctx, out, v, key_plan))
        return;
    if (xicgen_emit_map_index_set_builtin_hash_eq(ctx, out, v, key_plan))
        return;
    if (xicgen_emit_map_index_set_prehashed(ctx, out, v, key_plan))
        return;
    if (cg_value_type_is_json(v->args[0])) {
        const char *static_key = xicgen_static_string_const(v->args[1]);
        const XaotJsonAccessPlan *json_plan =
            static_key ? xicgen_json_shape_guard_plan(ctx, v, XG_JSON_ACCESS_INDEX_SET,
                                                      XG_JSON_ACCESS_FIELD_SET)
                       : NULL;
        if (!static_key)
            json_plan = xicgen_json_computed_key_guard_plan(ctx, v, XG_JSON_ACCESS_INDEX_SET);
        if (ctx && ctx->error) {
            emit_codegen_abort_expr(out);
            return;
        }
        if (json_plan) {
            if (static_key)
                xicgen_emit_json_shape_guard_set(ctx, out, v->args[0], v->args[2], json_plan,
                                                 static_key);
            else
                xicgen_emit_json_computed_key_guard_set(ctx, out, v->args[0], v->args[1],
                                                        v->args[2]);
            return;
        }
    }
    fprintf(out, "xrt_index_set(");
    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep(out, v->args[2], XR_REP_TAGGED);
    fprintf(out, ")");
}

static const XgRecordFieldSummary *xicgen_record_field_by_shape_ordinal(const XgGlobalEvidence *ev,
                                                                        XgRecordShapeId shape_id,
                                                                        uint16_t ordinal) {
    if (!ev || shape_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < ev->nrecord_fields; i++) {
        const XgRecordFieldSummary *field = &ev->record_fields[i];
        if (field->shape_id == shape_id && field->field_ordinal == ordinal)
            return field;
    }
    return NULL;
}

static bool xicgen_record_shape_field_ordinal_by_name_id(const XgGlobalEvidence *ev,
                                                         XgRecordShapeId shape_id, uint32_t name_id,
                                                         uint16_t *out_ordinal) {
    if (!ev || shape_id == XG_NO_ID || name_id == 0 || !out_ordinal)
        return false;
    for (uint32_t i = 0; i < ev->nrecord_fields; i++) {
        const XgRecordFieldSummary *field = &ev->record_fields[i];
        if (field->shape_id == shape_id && field->name_id == name_id) {
            *out_ordinal = field->field_ordinal;
            return true;
        }
    }
    return false;
}

static bool xicgen_emit_record_merge_copy_table(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                                const XaotRecordMergePlan *plan) {
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    const XgGlobalEvidence *ev = bundle ? bundle->global_evidence_plan.evidence : NULL;
    if (!ev || !v || !plan || plan->base_field_count == 0 || plan->result_field_count == 0) {
        if (ctx)
            ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: Record merge plan has no field evidence table\n");
        emit_codegen_abort_expr(out);
        return true;
    }

    for (uint16_t src_ord = 0; src_ord < plan->base_field_count; src_ord++) {
        const XgRecordFieldSummary *src_field =
            xicgen_record_field_by_shape_ordinal(ev, plan->base_shape_id, src_ord);
        uint16_t dst_ord = UINT16_MAX;
        if (!src_field || src_field->name_id == 0 ||
            !xicgen_record_shape_field_ordinal_by_name_id(ev, plan->result_shape_id,
                                                          src_field->name_id, &dst_ord)) {
            if (ctx)
                ctx->error = true;
            fprintf(stderr,
                    "[xi_cgen] ERROR: Record merge plan %u cannot rederive copy table "
                    "(src_ord=%u)\n",
                    plan->merge_id, (unsigned) src_ord);
            emit_codegen_abort_expr(out);
            return true;
        }
    }

    fprintf(out, "xrt_record_merge_copy_table(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ", %u, (const uint16_t[]){", (unsigned) plan->base_field_count);
    for (uint16_t src_ord = 0; src_ord < plan->base_field_count; src_ord++) {
        const XgRecordFieldSummary *src_field =
            xicgen_record_field_by_shape_ordinal(ev, plan->base_shape_id, src_ord);
        uint16_t dst_ord = UINT16_MAX;
        (void) xicgen_record_shape_field_ordinal_by_name_id(ev, plan->result_shape_id,
                                                            src_field->name_id, &dst_ord);
        if (src_ord > 0)
            fprintf(out, ", ");
        fprintf(out, "%u, %u", (unsigned) dst_ord, (unsigned) src_ord);
    }
    fprintf(out, "})");
    return true;
}

/* Object spread merge: verified Records use a direct ordinal copy table; Json stays dynamic. */
static void xicgen_json_merge(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 2, "xicgen_json_merge: need dst and src");
    const XaotRecordMergePlan *record_plan =
        v->xg_record_merge_id != XG_NO_ID
            ? xaot_bundle_find_record_merge_plan(cg_ctx_aot_bundle(ctx), v->xg_record_merge_id)
            : NULL;
    if (record_plan && (record_plan->action == XAOT_RECORD_MERGE_COPY_WITH_OVERWRITE ||
                        record_plan->action == XAOT_RECORD_MERGE_COPY_APPEND)) {
        xicgen_emit_record_merge_copy_table(ctx, out, v, record_plan);
        return;
    }
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
        /* Tuple storage is always tagged. Use the prepared value plan here:
         * cg_rep() can describe an indexed fixed-width scalar as tagged even
         * though AOT materialized it in a native C local. Bypassing the plan
         * then places the raw integer bits into XrValue (notably for later
         * tuple lanes), which reads back as null/zero. */
        emit_value_as_rep_ctx(ctx, out, v->args[a], XR_REP_TAGGED);
    }
    fprintf(out, "})");
}

static void xicgen_tuple_get(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                             const char *prefix) {
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 1, "xicgen_tuple_get: need tuple");
    if (emit_static_fixed_tuple_array_get_expr(ctx, out, v))
        return;
    if (emit_static_tuple_get_expr(ctx, out, v))
        return;
    const char *conv_suffix = emit_load_conversion_prefix(ctx, out, v, XR_REP_TAGGED);
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
    } else if (v->type->kind == XR_KIND_RUNE) {
        /* char(x): tagged XR_TAG_RUNE result, validated Unicode scalar. */
        fprintf(out, "xrt_to_rune(");
        emit_boxed_value_ref(out, v->args[0]);
        fprintf(out, ")");
    } else if (v->type->kind == XR_KIND_POINTER) {
        const char *suffix = emit_conversion_prefix(out, v->type, src_rep, dst_rep);
        emit_vref(out, v->args[0]);
        emit_conversion_suffix(out, suffix);
    } else {
        emit_vref(out, v->args[0]);
    }
}

static void xicgen_byte_array_ptr_arg(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                      const char *prefix, uint16_t arg_index) {
    XR_DCHECK(v != NULL && arg_index < v->nargs, "xicgen byte_array pointer arg out of range");
    emit_typed_array_ptr_expr(ctx, out, f, v->args[arg_index], prefix);
}

static void xicgen_byte_array_i64_arg(FILE *out, const XiValue *v, uint16_t arg_index) {
    XR_DCHECK(v != NULL && arg_index < v->nargs, "xicgen byte_array i64 arg out of range");
    emit_value_as_rep(out, v->args[arg_index], XR_REP_I64);
}

static void xicgen_byte_array_box_result(FILE *out, bool boxed) {
    if (boxed)
        fprintf(out, ", XR_TAG_ARRAY)");
}

static bool xicgen_emit_byte_slice_load(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                        const char *core_helper, const char *le_unchecked_helper,
                                        const char *ctype, int64_t byte_width,
                                        const char *bounds_message_expr) {
    CgArrayElemInfo info;
    if (!v || v->nargs != 3 || !cg_span_value_u8_info(ctx, v->args[0], &info))
        return false;
    if (!cg_span_plan_drops(ctx, v, XAOT_SLICE_ACCESS_BYTE_LOAD, XAOT_SLICE_DROP_HELPER))
        return false;
    (void) info;
    const char *conv_suffix =
        emit_conversion_prefix(out, v->type, XR_REP_I64, cg_value_plan_storage_rep(ctx, v));
    int64_t endian = XR_ENDIAN_NATIVE;
    if (le_unchecked_helper && xicgen_value_is_const_endian(v->args[2], &endian) &&
        endian == XR_ENDIAN_LE) {
        bool drop_bounds =
            cg_span_plan_drops(ctx, v, XAOT_SLICE_ACCESS_BYTE_LOAD, XAOT_SLICE_DROP_BOUNDS);
        fprintf(out, "({ xr_span_t _s = ");
        emit_span_ref_expr(out, v->args[0]);
        fprintf(out, "; int64_t _off = ");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
        if (!drop_bounds) {
            fprintf(out,
                    "; if (XR_UNLIKELY(!_s.data || _off < 0 || _s.length < INT64_C(%" PRId64
                    ") || _off > _s.length - INT64_C(%" PRId64 "))) "
                    "xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, %s)",
                    byte_width, byte_width, bounds_message_expr);
        }
        fprintf(out, "; %s _v = (%s)%s(_s, _off); (int64_t)_v; })", ctype, ctype,
                le_unchecked_helper);
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }
    fprintf(out, "({ xr_span_t _s = ");
    emit_span_ref_expr(out, v->args[0]);
    fprintf(out, "; int64_t _off = ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
    fprintf(out, "; bool _ok = false; %s _v = %s(_s.data, _s.length, XR_ELEM_U8, _off, ", ctype,
            core_helper);
    xicgen_emit_endian_arg_i64(ctx, out, v->args[2]);
    fprintf(out,
            ", &_ok); if (!_ok) xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, %s); "
            "(int64_t)_v; })",
            bounds_message_expr);
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool xicgen_emit_byte_slice_float_load(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                              const char *core_helper, const char *ctype,
                                              const char *bounds_message_expr) {
    CgArrayElemInfo info;
    if (!v || v->nargs != 3 || !cg_span_value_u8_info(ctx, v->args[0], &info))
        return false;
    if (!cg_span_plan_drops(ctx, v, XAOT_SLICE_ACCESS_BYTE_LOAD, XAOT_SLICE_DROP_HELPER))
        return false;
    (void) info;
    const char *conv_suffix =
        emit_conversion_prefix(out, v->type, XR_REP_F64, cg_value_plan_storage_rep(ctx, v));
    fprintf(out, "({ xr_span_t _s = ");
    emit_span_ref_expr(out, v->args[0]);
    fprintf(out, "; int64_t _off = ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
    fprintf(out, "; bool _ok = false; %s _v = %s(_s.data, _s.length, XR_ELEM_U8, _off, ", ctype,
            core_helper);
    xicgen_emit_endian_arg_i64(ctx, out, v->args[2]);
    fprintf(out,
            ", &_ok); if (!_ok) xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, %s); "
            "(double)_v; })",
            bounds_message_expr);
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool xicgen_emit_byte_slice_store(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                         const char *checked_helper, const char *core_helper,
                                         const char *value_ctype, const char *bounds_message_expr) {
    CgArrayElemInfo info;
    if (!v || v->nargs != 4 || !cg_span_value_u8_info(ctx, v->args[0], &info))
        return false;
    if (cg_emit_span_readonly_void_trap(ctx, out, v, XAOT_SLICE_ACCESS_BYTE_STORE))
        return true;
    (void) info;
    if (cg_span_plan_drops(ctx, v, XAOT_SLICE_ACCESS_BYTE_STORE, XAOT_SLICE_DROP_HELPER)) {
        fprintf(out, "({ xr_span_t _s = ");
        emit_span_ref_expr(out, v->args[0]);
        fprintf(out, "; int64_t _off = ");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
        fprintf(out, "; ");
        fprintf(out, "if (XR_UNLIKELY(!%s(_s.data, _s.length, XR_ELEM_U8, _off, (%s)", core_helper,
                value_ctype);
        emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_I64);
        fprintf(out, ", ");
        xicgen_emit_endian_arg_i64(ctx, out, v->args[3]);
        fprintf(out, "))) xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, %s); XR_NULL_VAL; })",
                bounds_message_expr);
        return true;
    }
    fprintf(out, "%s(", checked_helper);
    emit_span_ref_expr(out, v->args[0]);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_I64);
    fprintf(out, ", ");
    xicgen_emit_endian_arg_i64(ctx, out, v->args[3]);
    fprintf(out, ")");
    return true;
}

static bool xicgen_emit_byte_slice_float_store(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                               const char *checked_helper, const char *core_helper,
                                               const char *value_ctype,
                                               const char *bounds_message_expr) {
    CgArrayElemInfo info;
    if (!v || v->nargs != 4 || !cg_span_value_u8_info(ctx, v->args[0], &info))
        return false;
    if (cg_emit_span_readonly_void_trap(ctx, out, v, XAOT_SLICE_ACCESS_BYTE_STORE))
        return true;
    (void) info;
    if (cg_span_plan_drops(ctx, v, XAOT_SLICE_ACCESS_BYTE_STORE, XAOT_SLICE_DROP_HELPER)) {
        fprintf(out, "({ xr_span_t _s = ");
        emit_span_ref_expr(out, v->args[0]);
        fprintf(out, "; int64_t _off = ");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
        fprintf(out, "; ");
        fprintf(out, "if (XR_UNLIKELY(!%s(_s.data, _s.length, XR_ELEM_U8, _off, (%s)", core_helper,
                value_ctype);
        emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_F64);
        fprintf(out, ", ");
        xicgen_emit_endian_arg_i64(ctx, out, v->args[3]);
        fprintf(out, "))) xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, %s); XR_NULL_VAL; })",
                bounds_message_expr);
        return true;
    }
    fprintf(out, "%s(", checked_helper);
    emit_span_ref_expr(out, v->args[0]);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_F64);
    fprintf(out, ", ");
    xicgen_emit_endian_arg_i64(ctx, out, v->args[3]);
    fprintf(out, ")");
    return true;
}

static void xicgen_byte_slice_load_u16(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                       const char *prefix) {
    (void) f;
    (void) prefix;
    if (xicgen_emit_byte_slice_load(ctx, out, v, "xr_array_core_bytes_load_u16",
                                    "xrt_byte_slice_load_u16_le_unchecked_raw", "uint16_t", 2,
                                    "XR_ERROR_CORE_BYTE_SLICE_LOAD_U16_OOB_MSG"))
        return;
    const char *conv_suffix =
        emit_conversion_prefix(out, v->type, XR_REP_I64, cg_value_plan_storage_rep(ctx, v));
    fprintf(out, "XR_TO_INT(xrt_byte_slice_load_u16_value(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_TAGGED);
    fprintf(out, "))");
    emit_conversion_suffix(out, conv_suffix);
}

static void xicgen_byte_slice_load_u32(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                       const char *prefix) {
    (void) f;
    (void) prefix;
    if (xicgen_emit_byte_slice_load(ctx, out, v, "xr_array_core_bytes_load_u32",
                                    "xrt_byte_slice_load_u32_le_unchecked_raw", "uint32_t", 4,
                                    "XR_ERROR_CORE_BYTE_SLICE_LOAD_U32_OOB_MSG"))
        return;
    const char *conv_suffix =
        emit_conversion_prefix(out, v->type, XR_REP_I64, cg_value_plan_storage_rep(ctx, v));
    fprintf(out, "XR_TO_INT(xrt_byte_slice_load_u32_value(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_TAGGED);
    fprintf(out, "))");
    emit_conversion_suffix(out, conv_suffix);
}

static void xicgen_byte_slice_load_u64(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                       const char *prefix) {
    (void) f;
    (void) prefix;
    if (xicgen_emit_byte_slice_load(ctx, out, v, "xr_array_core_bytes_load_u64",
                                    "xrt_byte_slice_load_u64_le_unchecked_raw", "uint64_t", 8,
                                    "XR_ERROR_CORE_BYTE_SLICE_LOAD_U64_OOB_MSG"))
        return;
    const char *conv_suffix =
        emit_conversion_prefix(out, v->type, XR_REP_I64, cg_value_plan_storage_rep(ctx, v));
    fprintf(out, "XR_TO_INT(xrt_byte_slice_load_u64_value(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_TAGGED);
    fprintf(out, "))");
    emit_conversion_suffix(out, conv_suffix);
}

static void xicgen_byte_slice_load_f32(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                       const char *prefix) {
    (void) f;
    (void) prefix;
    if (xicgen_emit_byte_slice_float_load(ctx, out, v, "xr_array_core_bytes_load_f32", "float",
                                          "XR_ERROR_CORE_BYTE_SLICE_LOAD_F32_OOB_MSG"))
        return;
    const char *conv_suffix =
        emit_conversion_prefix(out, v->type, XR_REP_F64, cg_value_plan_storage_rep(ctx, v));
    fprintf(out, "XR_TO_FLOAT(xrt_byte_slice_load_f32_value(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_TAGGED);
    fprintf(out, "))");
    emit_conversion_suffix(out, conv_suffix);
}

static void xicgen_byte_slice_load_f64(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                       const char *prefix) {
    (void) f;
    (void) prefix;
    if (xicgen_emit_byte_slice_float_load(ctx, out, v, "xr_array_core_bytes_load_f64", "double",
                                          "XR_ERROR_CORE_BYTE_SLICE_LOAD_F64_OOB_MSG"))
        return;
    const char *conv_suffix =
        emit_conversion_prefix(out, v->type, XR_REP_F64, cg_value_plan_storage_rep(ctx, v));
    fprintf(out, "XR_TO_FLOAT(xrt_byte_slice_load_f64_value(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_TAGGED);
    fprintf(out, "))");
    emit_conversion_suffix(out, conv_suffix);
}

static void xicgen_byte_slice_store_u16(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                        const XiValue *v, const char *prefix) {
    (void) f;
    (void) prefix;
    if (xicgen_emit_byte_slice_store(ctx, out, v, "xrt_byte_slice_store_u16_checked_raw",
                                     "xr_array_core_bytes_store_u16", "uint16_t",
                                     "XR_ERROR_CORE_BYTE_SLICE_STORE_U16_OOB_MSG"))
        return;
    fprintf(out, "xrt_byte_slice_store_u16_value(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[3], XR_REP_TAGGED);
    fprintf(out, ")");
}

static void xicgen_byte_slice_store_u32(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                        const XiValue *v, const char *prefix) {
    (void) f;
    (void) prefix;
    if (xicgen_emit_byte_slice_store(ctx, out, v, "xrt_byte_slice_store_u32_checked_raw",
                                     "xr_array_core_bytes_store_u32", "uint32_t",
                                     "XR_ERROR_CORE_BYTE_SLICE_STORE_U32_OOB_MSG"))
        return;
    fprintf(out, "xrt_byte_slice_store_u32_value(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[3], XR_REP_TAGGED);
    fprintf(out, ")");
}

static void xicgen_byte_slice_store_u64(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                        const XiValue *v, const char *prefix) {
    (void) f;
    (void) prefix;
    if (xicgen_emit_byte_slice_store(ctx, out, v, "xrt_byte_slice_store_u64_checked_raw",
                                     "xr_array_core_bytes_store_u64", "uint64_t",
                                     "XR_ERROR_CORE_BYTE_SLICE_STORE_U64_OOB_MSG"))
        return;
    fprintf(out, "xrt_byte_slice_store_u64_value(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[3], XR_REP_TAGGED);
    fprintf(out, ")");
}

static void xicgen_byte_slice_store_f32(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                        const XiValue *v, const char *prefix) {
    (void) f;
    (void) prefix;
    if (xicgen_emit_byte_slice_float_store(ctx, out, v, "xrt_byte_slice_store_f32_checked_raw",
                                           "xr_array_core_bytes_store_f32", "float",
                                           "XR_ERROR_CORE_BYTE_SLICE_STORE_F32_OOB_MSG"))
        return;
    fprintf(out, "xrt_byte_slice_store_f32_value(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[3], XR_REP_TAGGED);
    fprintf(out, ")");
}

static void xicgen_byte_slice_store_f64(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                        const XiValue *v, const char *prefix) {
    (void) f;
    (void) prefix;
    if (xicgen_emit_byte_slice_float_store(ctx, out, v, "xrt_byte_slice_store_f64_checked_raw",
                                           "xr_array_core_bytes_store_f64", "double",
                                           "XR_ERROR_CORE_BYTE_SLICE_STORE_F64_OOB_MSG"))
        return;
    fprintf(out, "xrt_byte_slice_store_f64_value(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[3], XR_REP_TAGGED);
    fprintf(out, ")");
}

static void xicgen_byte_slice_fill(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                   const char *prefix) {
    (void) prefix;
    if (cg_emit_span_readonly_span_trap(ctx, out, v, XAOT_SLICE_ACCESS_BYTE_FILL))
        return;
    const XaotBulkPlan *bulk = cg_required_bulk_plan(ctx, f, v, XG_BULK_FILL, "Slice<byte>.fill");
    if (v->xg_bulk_op_id != XG_NO_ID && !bulk) {
        emit_codegen_abort_expr(out);
        return;
    }
    if (bulk && bulk->action == XAOT_BULK_RUNTIME_HELPER) {
        fprintf(out, "xrt_byte_slice_fill_checked_raw(");
        emit_span_ref_expr(out, v->args[0]);
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
        fprintf(out, ")");
        return;
    }
    bool inline_memset =
        bulk ? bulk->action == XAOT_BULK_INLINE_MEMSET
             : cg_span_plan_drops(ctx, v, XAOT_SLICE_ACCESS_BYTE_FILL, XAOT_SLICE_DROP_HELPER);
    bool typed_loop = bulk && bulk->action == XAOT_BULK_TYPED_LOOP;
    if (bulk && !inline_memset && !typed_loop) {
        cg_ctx_set_error(ctx);
        emit_codegen_abort_expr(out);
        return;
    }
    if (inline_memset || typed_loop) {
        fprintf(out, "({ xr_span_t _s = ");
        emit_span_ref_expr(out, v->args[0]);
        fprintf(out, "; ");
        fprintf(out, "if (XR_UNLIKELY(_s.length < 0 || (_s.length > 0 && !_s.data))) "
                     "xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "
                     "XR_ERROR_CORE_BYTE_SLICE_FILL_OOB_MSG); if (_s.length > 0) ");
        if (inline_memset)
            fprintf(out, "memset(_s.data, (uint8_t)");
        else
            fprintf(out, "for (int64_t _i = 0; _i < _s.length; _i++) "
                         "((uint8_t*)_s.data)[_i] = (uint8_t)");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
        if (inline_memset)
            fprintf(out, ", (size_t)_s.length)");
        fprintf(out, "; _s; })");
        return;
    }
    fprintf(out, "xrt_byte_slice_fill_checked_raw(");
    emit_span_ref_expr(out, v->args[0]);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
    fprintf(out, ")");
}

static void xicgen_emit_byte_slice_operand(XiCgenCtx *ctx, FILE *out, const XiValue *arg,
                                           const char *message_expr) {
    if (cg_span_value_u8_info(ctx, arg, NULL)) {
        emit_span_ref_expr(out, arg);
        return;
    }
    fprintf(out, "xrt_byte_slice_from_value(");
    emit_value_as_rep_ctx(ctx, out, arg, XR_REP_TAGGED);
    fprintf(out, ", %s)", message_expr);
}

static bool xicgen_emit_byte_slice_expr(XiCgenCtx *ctx, FILE *out, const XiValue *arg) {
    /* A typed Slice value has already materialised and validated its span.
     * Reuse that SSA value before considering source reconstruction; otherwise
     * a named slice local used by a bulk operation repeats both slice-bound
     * conversions and their guards in the same hot path. */
    if (cg_span_value_u8_info(ctx, arg, NULL)) {
        emit_span_ref_expr(out, arg);
        return true;
    }
    const XiValue *slice = xicgen_stack_slice_source_value(arg);
    if (slice && xicgen_slice_can_inline_bytes_common_prefix(ctx, slice)) {
        fprintf(out, "({ XrValue _xr_slice_start = ");
        emit_value_as_rep_ctx(ctx, out, slice->args[1], XR_REP_TAGGED);
        fprintf(out, "; XrValue _xr_slice_end = ");
        emit_value_as_rep_ctx(ctx, out, slice->args[2], XR_REP_TAGGED);
        fprintf(out, "; if (!XR_IS_INT(_xr_slice_start) || !XR_IS_INT(_xr_slice_end)) "
                     "xrt_throw_error(XR_ERR_TYPE_MISMATCH, "
                     "XR_ERROR_CORE_SLICE_BOUNDS_EXPECTS_MSG); xrt_span_from_span_slice(");
        emit_span_ref_expr(out, slice->args[0]);
        fprintf(out, ", XR_TO_INT(_xr_slice_start), XR_TO_INT(_xr_slice_end), UINT16_C(1)); })");
        return true;
    }
    return false;
}

static void xicgen_byte_slice_copy(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                   const char *prefix) {
    (void) prefix;
    if (cg_emit_span_readonly_span_trap(ctx, out, v, XAOT_SLICE_ACCESS_BYTE_COPY))
        return;
    const XaotBulkPlan *bulk =
        cg_required_bulk_plan(ctx, f, v, XG_BULK_COPY, "Slice<byte>.copyFrom");
    if (v->xg_bulk_op_id != XG_NO_ID && !bulk) {
        emit_codegen_abort_expr(out);
        return;
    }
    if (bulk && bulk->action == XAOT_BULK_RUNTIME_HELPER) {
        fprintf(out, "xrt_byte_slice_copy_checked_raw(");
        emit_span_ref_expr(out, v->args[0]);
        fprintf(out, ", ");
        xicgen_emit_byte_slice_operand(ctx, out, v->args[1],
                                       "XR_ERROR_CORE_BYTE_SLICE_COPY_SOURCE_MSG");
        fprintf(out, ")");
        return;
    }
    bool inline_copy =
        bulk ? bulk->action == XAOT_BULK_INLINE_MEMCPY ||
                   bulk->action == XAOT_BULK_INLINE_MEMMOVE || bulk->action == XAOT_BULK_TYPED_LOOP
             : cg_span_plan_drops(ctx, v, XAOT_SLICE_ACCESS_BYTE_COPY, XAOT_SLICE_DROP_HELPER);
    if (bulk && !inline_copy) {
        cg_ctx_set_error(ctx);
        emit_codegen_abort_expr(out);
        return;
    }
    if (inline_copy) {
        fprintf(out, "({ xr_span_t _dst = ");
        if (!xicgen_emit_byte_slice_expr(ctx, out, v->args[0])) {
            ctx->error = true;
            emit_codegen_abort_expr(out);
        }
        fprintf(out, "; xr_span_t _src = ");
        if (!xicgen_emit_byte_slice_expr(ctx, out, v->args[1])) {
            ctx->error = true;
            emit_codegen_abort_expr(out);
        }
        fprintf(out, "; ");
        fprintf(out, "if (XR_UNLIKELY(_src.length < 0 || _dst.length < 0 || _src.length > "
                     "_dst.length || (_src.length > 0 && (!_dst.data || !_src.data)))) "
                     "xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "
                     "XR_ERROR_CORE_BYTE_SLICE_COPY_OOB_MSG); ");
        if (bulk && bulk->action == XAOT_BULK_INLINE_MEMCPY)
            fprintf(out, "if (_src.length > 0) memcpy(_dst.data, _src.data, "
                         "(size_t)_src.length);");
        else if (bulk && bulk->action == XAOT_BULK_INLINE_MEMMOVE)
            fprintf(out, "if (_src.length > 0) memmove(_dst.data, _src.data, "
                         "(size_t)_src.length);");
        else if (bulk && bulk->action == XAOT_BULK_TYPED_LOOP)
            fprintf(out, "if (_src.length > 0 && (uintptr_t)_dst.data <= (uintptr_t)_src.data) { "
                         "for (int64_t _i = 0; _i < _src.length; _i++) "
                         "((uint8_t*)_dst.data)[_i] = ((const uint8_t*)_src.data)[_i]; } else { "
                         "for (int64_t _i = _src.length; _i > 0; _i--) "
                         "((uint8_t*)_dst.data)[_i - 1] = ((const uint8_t*)_src.data)[_i - 1]; }");
        else
            fprintf(out, "xr_array_core_copy_or_move_bytes(_dst.data, _src.data, _src.length);");
        fprintf(out, " _dst; })");
        return;
    }
    fprintf(out, "xrt_byte_slice_copy_checked_raw(");
    emit_span_ref_expr(out, v->args[0]);
    fprintf(out, ", ");
    xicgen_emit_byte_slice_operand(ctx, out, v->args[1],
                                   "XR_ERROR_CORE_BYTE_SLICE_COPY_SOURCE_MSG");
    fprintf(out, ")");
}

static void xicgen_byte_slice_compare(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                      const char *prefix) {
    (void) prefix;
    const XaotBulkPlan *bulk =
        cg_required_bulk_plan(ctx, f, v, XG_BULK_COMPARE, "Slice<byte>.compare");
    if (v->xg_bulk_op_id != XG_NO_ID && !bulk) {
        emit_codegen_abort_expr(out);
        return;
    }
    const char *conv_suffix =
        emit_conversion_prefix(out, v->type, XR_REP_I64, cg_value_plan_storage_rep(ctx, v));
    if (bulk && bulk->action == XAOT_BULK_RUNTIME_HELPER) {
        fprintf(out, "xrt_byte_slice_compare_checked_raw(");
        emit_span_ref_expr(out, v->args[0]);
        fprintf(out, ", ");
        xicgen_emit_byte_slice_operand(ctx, out, v->args[1],
                                       "XR_ERROR_CORE_BYTE_SLICE_COMPARE_OPERAND_MSG");
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return;
    }
    bool inline_compare =
        bulk ? bulk->action == XAOT_BULK_INLINE_MEMCMP
             : cg_span_plan_drops(ctx, v, XAOT_SLICE_ACCESS_BYTE_COMPARE, XAOT_SLICE_DROP_HELPER);
    if (bulk && !inline_compare) {
        cg_ctx_set_error(ctx);
        emit_codegen_abort_expr(out);
        emit_conversion_suffix(out, conv_suffix);
        return;
    }
    if (inline_compare) {
        fprintf(out, "({ xr_span_t _left = ");
        emit_span_ref_expr(out, v->args[0]);
        fprintf(out, "; xr_span_t _right = ");
        emit_span_ref_expr(out, v->args[1]);
        fprintf(out,
                "; if (XR_UNLIKELY(_left.length < 0 || _right.length < 0 || "
                "(_left.length > 0 && !_left.data) || (_right.length > 0 && !_right.data))) "
                "xrt_throw_error(XR_ERR_TYPE_MISMATCH, "
                "XR_ERROR_CORE_BYTE_SLICE_COMPARE_NO_DATA_MSG); int64_t _n = "
                "_left.length < _right.length ? _left.length : "
                "_right.length; int _cmp = _n > 0 ? memcmp(_left.data, _right.data, "
                "(size_t)_n) : 0; _cmp < 0 ? INT64_C(-1) : (_cmp > 0 ? INT64_C(1) : "
                "(_left.length < _right.length ? INT64_C(-1) : (_left.length > _right.length ? "
                "INT64_C(1) : INT64_C(0)))); })");
        emit_conversion_suffix(out, conv_suffix);
        return;
    }
    fprintf(out, "xrt_byte_slice_compare_checked_raw(");
    emit_span_ref_expr(out, v->args[0]);
    fprintf(out, ", ");
    xicgen_emit_byte_slice_operand(ctx, out, v->args[1],
                                   "XR_ERROR_CORE_BYTE_SLICE_COMPARE_OPERAND_MSG");
    fprintf(out, ")");
    emit_conversion_suffix(out, conv_suffix);
}

static bool xicgen_emit_byte_slice_data_len_expr(XiCgenCtx *ctx, FILE *out, const XiValue *arg,
                                                 const char *name) {
    const XiValue *slice = xicgen_stack_slice_source_value(arg);
    if (slice && xicgen_slice_can_inline_bytes_common_prefix(ctx, slice)) {
        fprintf(out, "xr_span_t _%s_src = ", name);
        emit_span_ref_expr(out, slice->args[0]);
        fprintf(out, "; XrArrayCoreRange _%s_range = xr_array_core_slice_range(_%s_src.length, ",
                name, name);
        emit_value_as_rep_ctx(ctx, out, slice->args[1], XR_REP_I64);
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, slice->args[2], XR_REP_I64);
        fprintf(out,
                "); const void *_%s_data = (_%s_range.count > 0 && _%s_src.data) ? "
                "(const void *)((const uint8_t *)_%s_src.data + _%s_range.start) : "
                "_%s_src.data; int64_t _%s_len = _%s_range.count; ",
                name, name, name, name, name, name, name, name);
        return true;
    }
    if (cg_span_value_u8_info(ctx, arg, NULL)) {
        fprintf(out, "xr_span_t _%s = ", name);
        emit_span_ref_expr(out, arg);
        fprintf(out, "; const void *_%s_data = _%s.data; int64_t _%s_len = _%s.length; ", name,
                name, name, name);
        return true;
    }
    return false;
}

static void xicgen_byte_slice_common_prefix(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                            const XiValue *v, const char *prefix) {
    (void) f;
    (void) prefix;
    const char *conv_suffix =
        emit_conversion_prefix(out, v->type, XR_REP_I64, cg_value_plan_storage_rep(ctx, v));
    if (cg_span_plan_drops(ctx, v, XAOT_SLICE_ACCESS_BYTE_COMMON_PREFIX, XAOT_SLICE_DROP_HELPER)) {
        fprintf(out, "({ ");
        if (!xicgen_emit_byte_slice_data_len_expr(ctx, out, v->args[0], "left") ||
            !xicgen_emit_byte_slice_data_len_expr(ctx, out, v->args[1], "right")) {
            ctx->error = true;
            emit_codegen_abort_expr(out);
            fprintf(out, "; })");
            emit_conversion_suffix(out, conv_suffix);
            return;
        }
        fprintf(out, "; int64_t _prefix = 0; if (XR_UNLIKELY(_left_len < 0 || _right_len < 0 || "
                     "(_left_len > 0 && !_left_data) || (_right_len > 0 && !_right_data))) "
                     "xrt_throw_error(XR_ERR_TYPE_MISMATCH, "
                     "XR_ERROR_CORE_BYTE_SLICE_COMMON_PREFIX_NO_DATA_MSG); else _prefix = "
                     "xr_array_core_bytes_common_prefix_raw(_left_data, "
                     "_left_len, _right_data, _right_len); _prefix; })");
        emit_conversion_suffix(out, conv_suffix);
        return;
    }
    fprintf(out, "xrt_byte_slice_common_prefix_checked_raw(");
    emit_span_ref_expr(out, v->args[0]);
    fprintf(out, ", ");
    xicgen_emit_byte_slice_operand(ctx, out, v->args[1],
                                   "XR_ERROR_CORE_BYTE_SLICE_COMMON_PREFIX_OPERAND_MSG");
    fprintf(out, ")");
    emit_conversion_suffix(out, conv_suffix);
}

static void xicgen_byte_slice_repeat(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                     const char *prefix) {
    (void) prefix;
    if (cg_emit_span_readonly_span_trap(ctx, out, v, XAOT_SLICE_ACCESS_BYTE_REPEAT))
        return;
    const XaotBulkPlan *bulk =
        cg_required_bulk_plan(ctx, f, v, XG_BULK_REPEAT, "Slice<byte>.repeatFrom");
    if (v->xg_bulk_op_id != XG_NO_ID && !bulk) {
        emit_codegen_abort_expr(out);
        return;
    }
    if (bulk && bulk->action == XAOT_BULK_RUNTIME_HELPER) {
        fprintf(out, "xrt_byte_slice_repeat_from_checked_raw(");
        emit_span_ref_expr(out, v->args[0]);
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_I64);
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, v->args[3], XR_REP_I64);
        fprintf(out, ")");
        return;
    }
    bool inline_repeat =
        bulk ? bulk->action == XAOT_BULK_TYPED_LOOP
             : cg_span_plan_drops(ctx, v, XAOT_SLICE_ACCESS_BYTE_REPEAT, XAOT_SLICE_DROP_HELPER);
    if (bulk && !inline_repeat) {
        cg_ctx_set_error(ctx);
        emit_codegen_abort_expr(out);
        return;
    }
    if (inline_repeat) {
        fprintf(out, "({ xr_span_t _span = ");
        emit_span_ref_expr(out, v->args[0]);
        fprintf(out, "; int64_t _dst_offset = ");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
        fprintf(out, "; int64_t _distance = ");
        emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_I64);
        fprintf(out, "; int64_t _count = ");
        emit_value_as_rep_ctx(ctx, out, v->args[3], XR_REP_I64);
        fprintf(out, "; ");
        fprintf(out, "if (XR_UNLIKELY(_span.length < 0 || _dst_offset < 0 || _distance <= 0 || "
                     "_count < 0 || _distance > _dst_offset || _dst_offset > _span.length || "
                     "_count > _span.length - _dst_offset || (_count > 0 && !_span.data))) "
                     "xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "
                     "XR_ERROR_CORE_BYTE_SLICE_REPEAT_OOB_MSG); "
                     "xr_array_core_bytes_repeat_copy(_span.data, _dst_offset, _distance, "
                     "_count); _span; })");
        return;
    }
    fprintf(out, "xrt_byte_slice_repeat_from_checked_raw(");
    emit_span_ref_expr(out, v->args[0]);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_I64);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[3], XR_REP_I64);
    fprintf(out, ")");
}

static void xicgen_span_window(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                               const char *prefix) {
    (void) prefix;
    XR_DCHECK(v && v->nargs == 3, "xicgen_span_window: need source, start, and count");
    bool bounds_proven =
        cg_span_plan_drops(ctx, v, XAOT_SLICE_ACCESS_WINDOW, XAOT_SLICE_DROP_BOUNDS);
    int64_t fixed_count = 0;
    bool has_fixed_count = (cg_const_int_value(v->args[2], &fixed_count) ||
                            xicgen_imported_int_const_value(ctx, f, v->args[2], &fixed_count)) &&
                           fixed_count >= 0;
    CgArrayElemInfo elem;
    bool has_static_elem = cg_span_elem_info_from_value(ctx, v->args[0], &elem) && elem.ctype;
    if (!has_static_elem) {
        cg_ctx_set_error(ctx);
        emit_codegen_abort_expr(out);
        return;
    }
    fprintf(out, "({ xr_span_t _src = ");
    emit_span_ref_expr(out, v->args[0]);
    fprintf(out, "; int64_t _start = ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
    if (has_fixed_count) {
        fprintf(out, "; XR_ASSUME(_src.length >= 0); ");
        if (!bounds_proven)
            fprintf(out,
                    "if (XR_UNLIKELY(_start < 0 || "
                    "_start > _src.length - INT64_C(%" PRId64 "))) "
                    "xrt_index_oob(_start, _src.length); ",
                    fixed_count);
        fprintf(out,
                "XR_ASSUME(_start >= 0 && "
                "_start <= _src.length - INT64_C(%" PRId64 ")); xr_span_t _out = _src; ",
                fixed_count);
        if (fixed_count > 0) {
            fprintf(out,
                    "XR_ASSUME(_src.data != NULL); _out.data = (void *)((uint8_t *)_src.data + "
                    "(size_t)_start * ");
            fprintf(out, "sizeof(%s)", elem.ctype);
            fprintf(out, "); ");
        }
        fprintf(out, "_out.length = INT64_C(%" PRId64 "); _out; })", fixed_count);
        return;
    }
    fprintf(out, "; int64_t _count = ");
    emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_I64);
    fprintf(out, "; ");
    if (!bounds_proven)
        fprintf(out, "if (XR_UNLIKELY(_src.length < 0 || _start < 0 || _count < 0 || "
                     "_start > _src.length || _count > _src.length - _start)) "
                     "xrt_index_oob((_start < 0 || _start > _src.length) ? _start : _count, "
                     "_src.length); ");
    fprintf(out, "XR_ASSUME(_src.length >= 0 && _start >= 0 && _count >= 0 && "
                 "_start <= _src.length && _count <= _src.length - _start); "
                 "xr_span_t _out = _src; _out.data = (_src.data && _count > 0) ? "
                 "(void *)((uint8_t *)_src.data + (size_t)_start * ");
    fprintf(out, "sizeof(%s)", elem.ctype);
    fprintf(out, ") : _src.data; _out.length = _count; _out; })");
}

static void xicgen_span_as_bytes(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                 const char *prefix) {
    (void) f;
    (void) prefix;
    const XaotSliceAccessPlan *plan = cg_span_access_plan(ctx, v, XAOT_SLICE_ACCESS_SLICE_AS_BYTES);
    CgArrayElemInfo info;
    if (!cg_span_elem_info_from_value(ctx, v->args[0], &info) || !info.ctype) {
        cg_ctx_set_error(ctx);
        emit_codegen_abort_expr(out);
        return;
    }
    if (plan && (plan->eliminated_checks & XAOT_SLICE_DROP_HELPER) == XAOT_SLICE_DROP_HELPER) {
        bool overflow_check_dropped =
            (plan->eliminated_checks & XAOT_SLICE_DROP_OVERFLOW) == XAOT_SLICE_DROP_OVERFLOW;
        fprintf(out, "({ xr_span_t _s = ");
        emit_span_ref_expr(out, v->args[0]);
        fprintf(out, "; if (XR_UNLIKELY(_s.length < 0");
        if (!overflow_check_dropped)
            fprintf(out, " || _s.length > INT64_MAX / (int64_t)sizeof(%s)", info.ctype);
        fprintf(out,
                ")) xrt_throw_error("
                "XR_ERR_INDEX_OUT_OF_BOUNDS, \"Slice.asArray<byte>() byte length overflow\"); "
                "xr_span_t _out = _s; _out.length = _s.length * (int64_t)sizeof(%s); "
                "_out; })",
                info.ctype);
        return;
    }
    fprintf(out, "xrt_span_as_bytes_checked_raw(");
    emit_span_ref_expr(out, v->args[0]);
    fprintf(out, ", (uint16_t)sizeof(%s), true)", info.ctype);
}

static void xicgen_span_fill(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                             const char *prefix) {
    (void) prefix;
    if (cg_emit_span_readonly_span_trap(ctx, out, v, XAOT_SLICE_ACCESS_SLICE_FILL))
        return;
    const XaotBulkPlan *bulk = cg_required_bulk_plan(ctx, f, v, XG_BULK_FILL, "Slice.fill");
    if (v->xg_bulk_op_id != XG_NO_ID && !bulk) {
        emit_codegen_abort_expr(out);
        return;
    }
    CgArrayElemInfo info;
    if (!cg_span_elem_info_from_value(ctx, v->args[0], &info) || info.rep == XR_REP_TAGGED) {
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return;
    }
    if (bulk && bulk->action != XAOT_BULK_INLINE_MEMSET && bulk->action != XAOT_BULK_TYPED_LOOP &&
        bulk->action != XAOT_BULK_RUNTIME_HELPER) {
        cg_ctx_set_error(ctx);
        emit_codegen_abort_expr(out);
        return;
    }
    bool zero_bits = cg_array_fill_value_is_zero_bits_literal(v->args[1]);
    bool byte_pattern = cg_array_elem_info_is_memset_byte_pattern(&info);
    bool use_memset = bulk ? bulk->action == XAOT_BULK_INLINE_MEMSET : zero_bits;
    if (use_memset && !zero_bits && !byte_pattern) {
        cg_ctx_set_error(ctx);
        emit_codegen_abort_expr(out);
        return;
    }
    fprintf(out, "({ xr_span_t _s = ");
    emit_span_ref_expr(out, v->args[0]);
    fprintf(out, "; ");
    fprintf(out,
            "if (XR_UNLIKELY(_s.length < 0 || _s.length > INT64_MAX / "
            "(int64_t)sizeof(%s))) xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "
            "\"Slice.fill(value) byte length overflow\"); ",
            info.ctype);
    fprintf(out, "if (XR_UNLIKELY(_s.length > 0 && !_s.data)) "
                 "xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "
                 "\"Slice.fill(value) range out of bounds\"); ");
    if (use_memset) {
        fprintf(out, "if (_s.length > 0) memset(_s.data, ");
        if (zero_bits)
            fprintf(out, "0");
        else
            emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
        fprintf(out, ", (size_t)_s.length * sizeof(%s)); _s; })", info.ctype);
        return;
    }
    fprintf(out, "%s _fill = ", info.ctype);
    emit_typed_array_store_value(ctx, out, &info, v->args[1]);
    fprintf(out,
            "; for (int64_t _i = 0; _i < _s.length; _i++) "
            "((%s*)_s.data)[_i] = _fill; _s; })",
            info.ctype);
}

static void xicgen_span_copy(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                             const char *prefix) {
    (void) prefix;
    if (cg_emit_span_readonly_span_trap(ctx, out, v, XAOT_SLICE_ACCESS_SLICE_COPY))
        return;
    const XaotBulkPlan *bulk = cg_required_bulk_plan(ctx, f, v, XG_BULK_COPY, "Slice.copyFrom");
    if (v->xg_bulk_op_id != XG_NO_ID && !bulk) {
        emit_codegen_abort_expr(out);
        return;
    }
    if (bulk && bulk->action == XAOT_BULK_RUNTIME_HELPER) {
        CgArrayElemInfo helper_info;
        if (!cg_span_elem_info_from_value(ctx, v->args[0], &helper_info) || !helper_info.ctype) {
            cg_ctx_set_error(ctx);
            emit_codegen_abort_expr(out);
            return;
        }
        fprintf(out, "xrt_span_copy_checked_raw(");
        emit_span_ref_expr(out, v->args[0]);
        fprintf(out, ", ");
        emit_span_ref_expr(out, v->args[1]);
        fprintf(out, ", (uint16_t)sizeof(%s))", helper_info.ctype);
        return;
    }
    CgArrayElemInfo info;
    bool have_native_info =
        cg_span_elem_info_from_value(ctx, v->args[0], &info) && info.rep != XR_REP_TAGGED;
    bool inline_copy =
        bulk ? bulk->action == XAOT_BULK_INLINE_MEMCPY ||
                   bulk->action == XAOT_BULK_INLINE_MEMMOVE || bulk->action == XAOT_BULK_TYPED_LOOP
             : cg_span_plan_drops(ctx, v, XAOT_SLICE_ACCESS_SLICE_COPY, XAOT_SLICE_DROP_HELPER);
    if (inline_copy && have_native_info) {
        fprintf(out, "({ xr_span_t _dst = ");
        emit_span_ref_expr(out, v->args[0]);
        fprintf(out, "; xr_span_t _src = ");
        emit_span_ref_expr(out, v->args[1]);
        fprintf(out, "; ");
        fprintf(out,
                "if (XR_UNLIKELY(_dst.length < 0 || _src.length < 0 || _dst.length > "
                "INT64_MAX / (int64_t)sizeof(%s) || _src.length > INT64_MAX / "
                "(int64_t)sizeof(%s))) xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "
                "\"Slice.copyFrom(src) byte length overflow\"); int64_t _dst_bytes = "
                "_dst.length * (int64_t)sizeof(%s); int64_t _src_bytes = _src.length * "
                "(int64_t)sizeof(%s); if (XR_UNLIKELY(_src_bytes > _dst_bytes || "
                "(_src_bytes > 0 && (!_dst.data || !_src.data)))) xrt_throw_error("
                "XR_ERR_INDEX_OUT_OF_BOUNDS, \"Slice.copyFrom(src) range out of bounds\"); ",
                info.ctype, info.ctype, info.ctype, info.ctype);
        if (bulk && bulk->action == XAOT_BULK_INLINE_MEMCPY)
            fprintf(out, "if (_src_bytes > 0) memcpy(_dst.data, _src.data, (size_t)_src_bytes); ");
        else if (!bulk || bulk->action == XAOT_BULK_INLINE_MEMMOVE)
            fprintf(out, "if (_src_bytes > 0) memmove(_dst.data, _src.data, (size_t)_src_bytes); ");
        else
            fprintf(out,
                    "if (_src.length > 0 && (uintptr_t)_dst.data <= (uintptr_t)_src.data) { "
                    "for (int64_t _i = 0; _i < _src.length; _i++) "
                    "((%s*)_dst.data)[_i] = ((const %s*)_src.data)[_i]; } else { "
                    "for (int64_t _i = _src.length; _i > 0; _i--) "
                    "((%s*)_dst.data)[_i - 1] = ((const %s*)_src.data)[_i - 1]; } ",
                    info.ctype, info.ctype, info.ctype, info.ctype);
        fprintf(out, "_dst; })");
        return;
    }
    if (bulk) {
        cg_ctx_set_error(ctx);
        emit_codegen_abort_expr(out);
        return;
    }
    fprintf(out, "xrt_span_copy_checked_raw(");
    emit_span_ref_expr(out, v->args[0]);
    fprintf(out, ", ");
    emit_span_ref_expr(out, v->args[1]);
    CgArrayElemInfo fallback_info;
    if (!cg_span_elem_info_from_value(ctx, v->args[0], &fallback_info) || !fallback_info.ctype) {
        cg_ctx_set_error(ctx);
        emit_codegen_abort_expr(out);
        return;
    }
    fprintf(out, ", (uint16_t)sizeof(%s))", fallback_info.ctype);
}

static void xicgen_span_compare(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                const char *prefix) {
    (void) prefix;
    const XaotBulkPlan *bulk = cg_required_bulk_plan(ctx, f, v, XG_BULK_COMPARE, "Slice.compare");
    if (v->xg_bulk_op_id != XG_NO_ID && !bulk) {
        emit_codegen_abort_expr(out);
        return;
    }
    const char *conv_suffix =
        emit_conversion_prefix(out, v->type, XR_REP_I64, cg_value_plan_storage_rep(ctx, v));
    CgArrayElemInfo info;
    if (bulk && bulk->action == XAOT_BULK_RUNTIME_HELPER) {
        if (!cg_span_elem_info_from_value(ctx, v->args[0], &info) || !info.ctype) {
            cg_ctx_set_error(ctx);
            emit_codegen_abort_expr(out);
            emit_conversion_suffix(out, conv_suffix);
            return;
        }
        fprintf(out, "xrt_span_compare_checked_raw(");
        emit_span_ref_expr(out, v->args[0]);
        fprintf(out, ", ");
        emit_span_ref_expr(out, v->args[1]);
        fprintf(out, ", (uint16_t)sizeof(%s))", info.ctype);
        emit_conversion_suffix(out, conv_suffix);
        return;
    }
    bool inline_compare =
        bulk ? bulk->action == XAOT_BULK_INLINE_MEMCMP
             : cg_span_plan_drops(ctx, v, XAOT_SLICE_ACCESS_SLICE_COMPARE, XAOT_SLICE_DROP_HELPER);
    if (inline_compare && cg_span_elem_info_from_value(ctx, v->args[0], &info) &&
        info.rep != XR_REP_TAGGED) {
        fprintf(out, "({ xr_span_t _left = ");
        emit_span_ref_expr(out, v->args[0]);
        fprintf(out, "; xr_span_t _right = ");
        emit_span_ref_expr(out, v->args[1]);
        fprintf(out,
                "; if (XR_UNLIKELY(_left.length < 0 || _right.length < 0 || _left.length > "
                "INT64_MAX / (int64_t)sizeof(%s) || _right.length > INT64_MAX / "
                "(int64_t)sizeof(%s))) xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "
                "\"Slice.compare(other) byte length overflow\"); int64_t _left_bytes = "
                "_left.length * (int64_t)sizeof(%s); int64_t _right_bytes = _right.length * "
                "(int64_t)sizeof(%s); int64_t _n = _left_bytes < _right_bytes ? _left_bytes : "
                "_right_bytes; if (XR_UNLIKELY(_n > 0 && (!_left.data || !_right.data))) "
                "xrt_throw_error(XR_ERR_TYPE_MISMATCH, \"Slice.compare(other) span has no "
                "data\"); int _cmp = _n > 0 ? memcmp(_left.data, _right.data, (size_t)_n) : "
                "0; _cmp < 0 ? INT64_C(-1) : (_cmp > 0 ? INT64_C(1) : (_left.length < "
                "_right.length ? INT64_C(-1) : (_left.length > _right.length ? INT64_C(1) : "
                "INT64_C(0)))); })",
                info.ctype, info.ctype, info.ctype, info.ctype);
        emit_conversion_suffix(out, conv_suffix);
        return;
    }
    if (bulk) {
        cg_ctx_set_error(ctx);
        emit_codegen_abort_expr(out);
        emit_conversion_suffix(out, conv_suffix);
        return;
    }
    fprintf(out, "xrt_span_compare_checked_raw(");
    emit_span_ref_expr(out, v->args[0]);
    fprintf(out, ", ");
    emit_span_ref_expr(out, v->args[1]);
    if (!cg_span_elem_info_from_value(ctx, v->args[0], &info) || !info.ctype) {
        cg_ctx_set_error(ctx);
        emit_codegen_abort_expr(out);
        emit_conversion_suffix(out, conv_suffix);
        return;
    }
    fprintf(out, ", (uint16_t)sizeof(%s))", info.ctype);
    emit_conversion_suffix(out, conv_suffix);
}

static void xicgen_span_reinterpret(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                    const char *prefix) {
    (void) f;
    (void) prefix;
    uint8_t elem_type = (uint8_t) (v->aux_int & 0xff);
    uint16_t elem_size = (uint16_t) ((v->aux_int >> 8) & 0xffff);
    uint8_t elem_tid = (uint8_t) ((v->aux_int >> 24) & 0xff);
    uint16_t alignment = (uint16_t) ((v->aux_int >> 32) & 0xffff);
    uint16_t layout_marker = v->aux ? 1u : 0u;
    const XaotSliceAccessPlan *plan = cg_span_access_plan(ctx, v, XAOT_SLICE_ACCESS_REINTERPRET);
    if (plan && (plan->eliminated_checks & XAOT_SLICE_DROP_HELPER) == XAOT_SLICE_DROP_HELPER) {
        bool overflow_check_dropped =
            (plan->eliminated_checks & XAOT_SLICE_DROP_OVERFLOW) == XAOT_SLICE_DROP_OVERFLOW;
        bool length_rel_proven = (plan->evidence & XAOT_SLICE_EV_LENGTH_REL_PROVEN) != 0;
        fprintf(out, "({ xr_span_t _s = ");
        emit_span_ref_expr(out, v->args[0]);
        fprintf(out, "; ");
        if (!overflow_check_dropped) {
            fprintf(out, "if (XR_UNLIKELY(_s.length < 0)) "
                         "xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "
                         "XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_OVERFLOW_MSG); ");
        }
        if (!length_rel_proven) {
            fprintf(out,
                    "if (XR_UNLIKELY(_s.length %% (int64_t)%u != 0)) "
                    "xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "
                    "XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_DIVISIBLE_MSG); ",
                    (unsigned) elem_size);
        }
        if (alignment > 1) {
            fprintf(out,
                    "if (XR_UNLIKELY(_s.length > 0 && (!_s.data || "
                    "((uintptr_t)_s.data %% (uintptr_t)%u) != 0))) "
                    "xrt_throw_error(XR_ERR_TYPE_MISMATCH, "
                    "XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_MISALIGNED_MSG); ",
                    (unsigned) alignment);
        }
        fprintf(out, "xr_span_t _out = _s; _out.length = _s.length / (int64_t)%u; _out; })",
                (unsigned) elem_size);
        return;
    }
    fprintf(out, "xrt_span_reinterpret_checked_raw(");
    emit_span_ref_expr(out, v->args[0]);
    fprintf(out, ", (uint8_t)%u, (uint16_t)%u, (uint8_t)%u, (uint16_t)%u, (uint16_t)%u)",
            (unsigned) elem_type, (unsigned) elem_size, (unsigned) elem_tid, (unsigned) alignment,
            (unsigned) layout_marker);
}

static void xicgen_byte_array_copy_within(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                          const XiValue *v, const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    /* Route through the validating value helper so a bad receiver/integer args or
     * an out-of-range copy raise the same panics as the VM, instead of silently
     * no-op'ing via the raw path. */
    bool boxed = cg_rep(v) == XR_REP_TAGGED;
    if (!boxed)
        fprintf(out, "((xrt_array_t *)(");
    fprintf(out, "xrt_byte_array_copy_within_value(");
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

static void xicgen_byte_array_copy_from(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                        const XiValue *v, const char *prefix) {
    bool boxed = cg_rep(v) == XR_REP_TAGGED;
    if (boxed)
        fprintf(out, "xr_mkptr(");
    fprintf(out, "xrt_byte_array_copy_from_checked_raw(");
    xicgen_byte_array_ptr_arg(ctx, out, f, v, prefix, 0);
    fprintf(out, ", ");
    xicgen_byte_array_ptr_arg(ctx, out, f, v, prefix, 1);
    fprintf(out, ", ");
    xicgen_byte_array_i64_arg(out, v, 2);
    fprintf(out, ", ");
    xicgen_byte_array_i64_arg(out, v, 3);
    fprintf(out, ", ");
    xicgen_byte_array_i64_arg(out, v, 4);
    fprintf(out, ")");
    xicgen_byte_array_box_result(out, boxed);
}

static void xicgen_byte_array_append_from(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                          const XiValue *v, const char *prefix) {
    if (emit_byte_array_append_from_expr(ctx, out, f, prefix, v))
        return;

    bool boxed = cg_rep(v) == XR_REP_TAGGED;
    if (!boxed)
        fprintf(out, "((xrt_array_t *)(");
    fprintf(out, "xrt_byte_array_append_from_value(");
    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ")");
    if (!boxed)
        fprintf(out, ").ptr)");
}

static void xicgen_byte_array_repeat_from(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                          const XiValue *v, const char *prefix) {
    if (emit_byte_array_repeat_from_expr(ctx, out, f, prefix, v))
        return;

    bool boxed = cg_rep(v) == XR_REP_TAGGED;
    if (!boxed)
        fprintf(out, "((xrt_array_t *)(");
    fprintf(out, "xrt_byte_array_repeat_from_tail_value(");
    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep(out, v->args[2], XR_REP_TAGGED);
    fprintf(out, ")");
    if (!boxed)
        fprintf(out, ").ptr)");
}

static const XrAbiScalarDesc *cg_ffi_scalar_desc(uint8_t code) {
    code = xr_ffi_ptr_aux_type(code);
    const XrAbiScalarDesc *desc = xr_abi_scalar_desc(code);
    return desc && desc->is_memory_scalar ? desc : NULL;
}

static const char *cg_ffi_pointee_c_type(XiCgenCtx *ctx, uint8_t code) {
    const XrAbiScalarDesc *desc = cg_ffi_scalar_desc(code);
    if (desc)
        return desc->c_type;
    cg_ctx_set_error(ctx);
    return "xr_codegen_invalid_ffi_scalar";
}

static bool cg_ffi_code_is_float(uint8_t code) {
    const XrAbiScalarDesc *desc = cg_ffi_scalar_desc(code);
    return desc && desc->is_float;
}

static bool cg_ffi_code_is_ptr(uint8_t code) {
    const XrAbiScalarDesc *desc = cg_ffi_scalar_desc(code);
    return desc && desc->is_pointer;
}

static uint8_t cg_ffi_code_width(XiCgenCtx *ctx, uint8_t code) {
    const XrAbiScalarDesc *desc = cg_ffi_scalar_desc(code);
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    if (!desc || !bundle || !xr_target_data_layout_validate(&bundle->target_data_layout)) {
        cg_ctx_set_error(ctx);
        return 0;
    }
    uint8_t width = xr_abi_scalar_width(desc, (uint8_t) bundle->target_data_layout.pointer.size);
    if (width == 0)
        cg_ctx_set_error(ctx);
    return width;
}

/* Unsafe Array<T>/Slice<T> data pointer borrow. VM/tagged values keep the address
 * as an integer; AOT hot code carries Ptr/MutPtr as a non-owning C pointer. */
static const XiConstLiteral *xicgen_static_addr_resolve_literal(XiCgenCtx *ctx, const XiFunc *f,
                                                                int64_t slot, bool want_mutable,
                                                                const XiModule **out_module,
                                                                int64_t *out_slot) {
    if (out_module)
        *out_module = NULL;
    if (out_slot)
        *out_slot = -1;
    if (!ctx || !ctx->module || slot < 0 || slot >= ctx->module->nslots)
        return NULL;

    const XiModule *module = ctx->module;
    int64_t target_slot = slot;
    const XiConstLiteral *lit = NULL;
    const XiModule *import_module = NULL;
    int64_t import_slot = -1;
    if (!want_mutable && slot <= INT_MAX)
        lit = cg_import_slot_const_literal(ctx, f, (int) slot, &import_module, &import_slot);
    if (lit && import_module && import_slot >= 0) {
        module = import_module;
        target_slot = import_slot;
    } else if (want_mutable) {
        lit = cg_module_shared_initializer_literal(module, target_slot);
    } else {
        lit = cg_module_const_literal(module, target_slot);
    }
    if (!lit || lit->kind == XI_CONST_LITERAL_NONE)
        return NULL;
    if (want_mutable && !lit->data_mutable)
        return NULL;
    if (cg_imported_static_const_needs_weak_symbol(ctx, module, lit)) {
        cg_report_imported_static_const_requires_weak(ctx, module, target_slot);
        ctx->error = true;
        return NULL;
    }
    if (out_module)
        *out_module = module;
    if (out_slot)
        *out_slot = target_slot;
    return lit;
}

static bool xicgen_emit_static_addr_symbol_name(XiCgenCtx *ctx, FILE *out, const XiModule *module,
                                                int64_t slot, const XiConstLiteral *lit,
                                                bool want_mutable) {
    const XiConstLiteral *static_lit = NULL;
    if (cg_freestanding_static_scalar_const_literal_in_module(ctx, module, slot, &static_lit)) {
        switch (static_lit->kind) {
            case XI_CONST_LITERAL_INT:
            case XI_CONST_LITERAL_FLOAT:
            case XI_CONST_LITERAL_BOOL:
            case XI_CONST_LITERAL_CHAR:
                cg_emit_static_scalar_const_name(ctx, out, module, slot);
                return true;
            case XI_CONST_LITERAL_STRING:
                cg_emit_static_string_const_name(ctx, out, module, slot);
                return true;
            case XI_CONST_LITERAL_NULL:
                cg_emit_static_value_const_name(ctx, out, module, slot);
                return true;
            default:
                return false;
        }
    }
    if (want_mutable &&
        cg_freestanding_static_scalar_var_literal_in_module(ctx, module, slot, &static_lit)) {
        switch (static_lit->kind) {
            case XI_CONST_LITERAL_INT:
            case XI_CONST_LITERAL_FLOAT:
            case XI_CONST_LITERAL_BOOL:
            case XI_CONST_LITERAL_CHAR:
                cg_emit_static_scalar_var_name(ctx, out, module, slot);
                return true;
            default:
                return false;
        }
    }

    CgFixedArrayLaneInfo fixed_array_info;
    if (cg_freestanding_static_fixed_array_literal_in_module(ctx, module, slot, &fixed_array_info,
                                                             NULL)) {
        cg_emit_static_fixed_array_name(ctx, out, module, slot);
        return true;
    }
    CgStaticFixedMatrixInfo matrix_info;
    if (cg_freestanding_static_fixed_matrix_literal_in_module(ctx, module, slot, &matrix_info,
                                                              NULL)) {
        cg_emit_static_fixed_array_name(ctx, out, module, slot);
        return true;
    }
    CgStaticFixedCubeInfo cube_info;
    if (cg_freestanding_static_fixed_cube_literal_in_module(ctx, module, slot, &cube_info, NULL)) {
        cg_emit_static_fixed_array_name(ctx, out, module, slot);
        return true;
    }
    CgStaticFixedStructArrayInfo struct_array_info;
    if (cg_freestanding_static_fixed_struct_array_literal_in_module(ctx, module, slot,
                                                                    &struct_array_info, NULL)) {
        cg_emit_static_fixed_array_name(ctx, out, module, slot);
        return true;
    }
    CgStaticFixedTupleArrayInfo tuple_array_info;
    if (cg_freestanding_static_fixed_tuple_array_literal_in_module(ctx, module, slot,
                                                                   &tuple_array_info, NULL)) {
        cg_emit_static_fixed_array_name(ctx, out, module, slot);
        return true;
    }
    XrType *tuple_type = NULL;
    if (cg_freestanding_static_tuple_literal_in_module(ctx, module, slot, &tuple_type, NULL)) {
        cg_emit_static_tuple_name(ctx, out, module, slot);
        return true;
    }
    const XrAggregateLayout *layout = NULL;
    if (cg_freestanding_static_struct_literal_in_module(ctx, module, slot, &layout, NULL)) {
        cg_emit_static_struct_name(ctx, out, module, slot);
        return true;
    }

    fprintf(stderr,
            "[xi_cgen] ERROR: %s requires a materialized freestanding static %sobject; "
            "'%s.%s' is not addressable\n",
            "static address", want_mutable ? "mutable " : "const ",
            module && module->name ? module->name : "?",
            cg_module_const_slot_name(module, slot) ? cg_module_const_slot_name(module, slot)
                                                    : "?");
    (void) lit;
    if (ctx)
        ctx->error = true;
    return false;
}

static void xicgen_static_addr(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                               const char *prefix) {
    (void) prefix;
    const XiModule *module = NULL;
    int64_t slot = -1;
    bool want_mutable = v && v->type && XR_TYPE_IS_POINTER(v->type) && v->type->ptr_is_mut;
    const XiConstLiteral *lit = xicgen_static_addr_resolve_literal(ctx, f, v ? v->aux_int : -1,
                                                                   want_mutable, &module, &slot);
    const XaotAddressPlan *address = xaot_address_plan_find(cg_ctx_aot_bundle(ctx), v);
    if (!address ||
        (address->provenance.address_identity != XR_ADDRESS_MODULE_STABLE &&
         address->provenance.address_identity != XR_ADDRESS_SYSTEM_STABLE) ||
        address->provenance.escape != XR_POINTER_ESCAPE_STABLE ||
        (want_mutable && address->provenance.mutability == XR_STORAGE_READONLY) ||
        (!want_mutable && address->provenance.domain != XR_STORAGE_MODULE_STATIC)) {
        fprintf(stderr,
                "[xi_cgen] ERROR: %s requires verified stable %s storage provenance for '%s.%s'\n",
                "static address", want_mutable ? "mutable" : "readonly",
                module && module->name ? module->name : "?",
                module && slot >= 0 && cg_module_const_slot_name(module, slot)
                    ? cg_module_const_slot_name(module, slot)
                    : "?");
        ctx->error = true;
        lit = NULL;
    }
    const char *conv_suffix = emit_conversion_prefix(out, v ? v->type : NULL, XR_REP_RAWPTR,
                                                     cg_value_plan_storage_rep(ctx, v));
    fprintf(out, "(void *)(&");
    if (!lit || !xicgen_emit_static_addr_symbol_name(ctx, out, module, slot, lit, want_mutable))
        fprintf(out, "((char *)0)[0]");
    fprintf(out, ")");
    emit_conversion_suffix(out, conv_suffix);
}

static void xicgen_array_data_ptr(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                  const char *prefix) {
    if (!v || v->nargs < 1) {
        emit_codegen_abort_expr(out);
        return;
    }
    const XaotAddressPlan *address = xaot_address_plan_find(cg_ctx_aot_bundle(ctx), v);
    bool owner_borrow = address && address->provenance.origin == XR_POINTER_ORIGIN_OWNER_BORROW &&
                        address->provenance.escape == XR_POINTER_ESCAPE_CALL_BOUND;
    bool static_borrow = address && address->provenance.origin == XR_POINTER_ORIGIN_MODULE &&
                         address->provenance.domain == XR_STORAGE_MODULE_STATIC &&
                         address->provenance.mutability == XR_STORAGE_READONLY &&
                         address->provenance.address_identity == XR_ADDRESS_MODULE_STABLE &&
                         address->provenance.escape == XR_POINTER_ESCAPE_STABLE;
    if (!owner_borrow && !static_borrow) {
        fprintf(stderr, "[xi_cgen] ERROR: pointer projection has no verified address plan\n");
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return;
    }
    const char *conv_suffix =
        emit_conversion_prefix(out, v->type, XR_REP_RAWPTR, cg_value_plan_storage_rep(ctx, v));
    CgFixedArrayLaneInfo fixed;
    if (cg_fixed_array_lane_info_from_value(v->args[0], &fixed)) {
        fprintf(out, "(void *)(");
        emit_fixed_array_lane_ptr_expr(ctx, out, v->args[0], &fixed);
        fprintf(out, ")");
    } else if (cg_value_plan_is_span_aggregate(ctx, v->args[0])) {
        fprintf(out, "(void *)((");
        emit_span_ref_expr(out, v->args[0]);
        fprintf(out, ").data)");
    } else {
        fprintf(out, "(void *)(");
        emit_typed_array_ptr_expr(ctx, out, f, v->args[0], prefix);
        fprintf(out, "->data)");
    }
    emit_conversion_suffix(out, conv_suffix);
}

static void xicgen_static_bytes_ptr(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                    const char *prefix) {
    (void) f;
    (void) prefix;
    const XaotAddressPlan *address = xaot_address_plan_find(cg_ctx_aot_bundle(ctx), v);
    bool static_literal = address && address->provenance.origin == XR_POINTER_ORIGIN_STATIC &&
                          address->provenance.domain == XR_STORAGE_MODULE_STATIC &&
                          address->provenance.mutability == XR_STORAGE_READONLY &&
                          address->provenance.address_identity == XR_ADDRESS_MODULE_STABLE &&
                          address->provenance.escape == XR_POINTER_ESCAPE_STABLE;
    if (!v || v->nargs != 0 || v->aux_int < 0 || !v->aux || !static_literal) {
        fprintf(stderr, "[xi_cgen] ERROR: static byte pointer has no verified literal plan\n");
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return;
    }

    const XaotFixedBytesPlan *plan = xaot_bundle_find_fixed_bytes_plan(cg_ctx_aot_bundle(ctx), v);
    const XaotFixedBytesBlob *blob =
        plan ? xaot_bundle_find_fixed_bytes_blob(cg_ctx_aot_bundle(ctx), plan->blob_id) : NULL;
    if (!plan || !blob || plan->action != XAOT_FIXED_BYTES_READONLY_PTR ||
        plan->length != (uint32_t) v->aux_int || blob->length != plan->length) {
        fprintf(stderr, "[xi_cgen] ERROR: static byte pointer has no verified blob plan\n");
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return;
    }

    const char *conv_suffix =
        emit_conversion_prefix(out, v->type, XR_REP_RAWPTR, cg_value_plan_storage_rep(ctx, v));
    fprintf(out, "(void *)((const uint8_t *)_xbytes_%u)", plan->blob_id);
    emit_conversion_suffix(out, conv_suffix);
}

static XaotValueRep xicgen_place_pointee_value_rep(XiCgenCtx *ctx, const XiFunc *f,
                                                   const XiValue *place) {
    XaotValueRep rep;
    memset(&rep, 0, sizeof(rep));
    if (place && place->op == XI_LOCAL_ADDR && place->nargs == 1 && place->args[0]) {
        const XaotValuePlan *source_plan = cg_value_plan(ctx, place->args[0]);
        if (source_plan)
            return source_plan->rep;
    }
    if (place && place->op == XI_PARAM && place->aux_int >= 0) {
        const XaotFuncPlan *func_plan = cg_func_plan(ctx, f);
        uint16_t index = (uint16_t) place->aux_int;
        if (func_plan && func_plan->abi.params && index < func_plan->abi.nparams &&
            (func_plan->abi.params[index].flags & XAOT_ABI_SLOT_BORROWED_PLACE) != 0)
            return func_plan->abi.params[index].pointee_rep;
    }
    return xaot_value_rep_for_type(place ? place->type : NULL);
}

static const char *xicgen_place_pointee_c_type(XiCgenCtx *ctx, const XiFunc *f,
                                               const XiValue *place, XaotValueRep *rep_out) {
    XaotValueRep rep = xicgen_place_pointee_value_rep(ctx, f, place);
    if (rep_out)
        *rep_out = rep;
    return rep.c_type ? rep.c_type : ctype_str(xaot_value_storage_rep(rep));
}

static void xicgen_local_addr(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    (void) f;
    (void) prefix;
    if (!v || v->nargs != 1 || !v->args[0]) {
        emit_codegen_abort_expr(out);
        return;
    }
    if (v->type && v->type->kind == XR_KIND_SLICE && cg_type_is_byte_slice(v->type) &&
        v->args[0]->type && v->args[0]->type->kind == XR_KIND_ARRAY) {
        fprintf(out, "(void *)((xr_span_t[]){xrt_byte_slice_from_value(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", XR_ERROR_CORE_BYTE_SLICE_ARG_EXPECTS_MSG)})");
        return;
    }
    CgFixedArrayLaneInfo fixed;
    if (cg_fixed_array_lane_info_from_value(v->args[0], &fixed)) {
        fprintf(out, "(void *)(");
        emit_fixed_array_lane_ptr_expr(ctx, out, v->args[0], &fixed);
        fprintf(out, ")");
        return;
    }
    fprintf(out, "(void *)(&");
    emit_vref(out, v->args[0]);
    fprintf(out, ")");
}

static void xicgen_place_load(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    (void) prefix;
    if (!v || v->nargs != 1 || !v->args[0]) {
        emit_codegen_abort_expr(out);
        return;
    }
    CgFixedArrayLaneInfo fixed;
    if (cg_fixed_array_lane_info_from_type(v->type, &fixed)) {
        if (cg_value_plan_storage_rep(ctx, v) == XR_REP_RAWPTR) {
            emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_RAWPTR);
            return;
        }
        fprintf(out, "xr_array_ref((void *)(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_RAWPTR);
        fprintf(out, "), %u, %u)", (unsigned) fixed.native_type, (unsigned) fixed.count);
        return;
    }
    XaotValueRep pointee_rep;
    const char *cty = xicgen_place_pointee_c_type(ctx, f, v->args[0], &pointee_rep);
    XrRep from_rep = xaot_value_storage_rep(pointee_rep);
    XrRep to_rep = cg_value_plan_storage_rep(ctx, v);
    const char *conv_suffix = emit_conversion_prefix(out, v->type, from_rep, to_rep);
    fprintf(out, "(*(%s *)(", cty);
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_RAWPTR);
    fprintf(out, "))");
    emit_conversion_suffix(out, conv_suffix);
}

static void xicgen_place_store(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                               const char *prefix) {
    (void) prefix;
    if (!v || v->nargs != 2 || !v->args[0] || !v->args[1]) {
        emit_codegen_abort_expr(out);
        return;
    }
    XaotValueRep pointee_rep;
    const char *cty = xicgen_place_pointee_c_type(ctx, f, v->args[0], &pointee_rep);
    XrRep pointee_storage_rep = xaot_value_storage_rep(pointee_rep);
    fprintf(out, "(*(%s *)(", cty);
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_RAWPTR);
    fprintf(out, ")) = ");
    const XaotValuePlan *value_plan = cg_value_plan(ctx, v->args[1]);
    if (pointee_rep.kind == XAOT_VALUE_AGGREGATE && value_plan &&
        value_plan->rep.kind == XAOT_VALUE_AGGREGATE)
        emit_vref(out, v->args[1]);
    else
        emit_value_as_rep_ctx(ctx, out, v->args[1], pointee_storage_rep);
}

static void xicgen_emit_raw_endian_prefix(FILE *out, uint8_t width, bool constant_endian,
                                          int64_t endian) {
    if (width == 1 || (constant_endian && endian == XR_ENDIAN_NATIVE))
        return;
    if (constant_endian && endian == XR_ENDIAN_LE)
        fprintf(out, "xr_raw_u%u_from_le(", (unsigned) width * 8u);
    else if (constant_endian && endian == XR_ENDIAN_BE)
        fprintf(out, "xr_raw_u%u_from_be(", (unsigned) width * 8u);
    else
        fprintf(out, "xr_raw_u%u_from_endian(", (unsigned) width * 8u);
}

static void xicgen_emit_raw_endian_suffix(XiCgenCtx *ctx, FILE *out, uint8_t width,
                                          bool constant_endian, int64_t endian,
                                          const XiValue *endian_value) {
    if (width == 1 || (constant_endian && endian == XR_ENDIAN_NATIVE))
        return;
    if (!constant_endian) {
        fprintf(out, ", ");
        xicgen_emit_raw_endian_arg_i64(ctx, out, endian_value);
    }
    fprintf(out, ")");
}

static void xicgen_emit_raw_load_bits(XiCgenCtx *ctx, FILE *out, const XiValue *ptr, uint8_t width,
                                      const XiValue *endian_value, bool constant_endian,
                                      int64_t endian) {
    xicgen_emit_raw_endian_prefix(out, width, constant_endian, endian);
    fprintf(out, "xr_raw_load_u%u_unaligned(", (unsigned) width * 8u);
    emit_value_as_rep_ctx(ctx, out, ptr, XR_REP_RAWPTR);
    fprintf(out, ")");
    xicgen_emit_raw_endian_suffix(ctx, out, width, constant_endian, endian, endian_value);
}

static void xicgen_emit_raw_store_bits(XiCgenCtx *ctx, FILE *out, const XiValue *value,
                                       uint8_t code, const XiValue *endian_value,
                                       bool constant_endian, int64_t endian) {
    uint8_t width = cg_ffi_code_width(ctx, code);
    xicgen_emit_raw_endian_prefix(out, width, constant_endian, endian);
    if ((XrFFIType) code == XR_FFI_T_F32) {
        fprintf(out, "xr_raw_f32_to_bits((float)(");
        emit_value_as_rep_ctx(ctx, out, value, XR_REP_F64);
        fprintf(out, "))");
    } else if ((XrFFIType) code == XR_FFI_T_F64) {
        fprintf(out, "xr_raw_f64_to_bits(");
        emit_value_as_rep_ctx(ctx, out, value, XR_REP_F64);
        fprintf(out, ")");
    } else {
        fprintf(out, "(%s)(", cg_ffi_pointee_c_type(ctx, code));
        emit_value_as_rep_ctx(ctx, out, value, XR_REP_I64);
        fprintf(out, ")");
    }
    xicgen_emit_raw_endian_suffix(ctx, out, width, constant_endian, endian, endian_value);
}

/* R[dst] = raw_load<T>(addr). All scalar accesses use fixed-size memcpy in
 * xr_raw_scalar_core.h, even when the source pointer is nominally aligned. */
static void xicgen_ptr_load(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                            const char *prefix) {
    (void) f;
    (void) prefix;
    if (!v || v->nargs != 2 || !v->args[0] || !v->args[1]) {
        emit_codegen_abort_expr(out);
        return;
    }
    uint8_t aux = (uint8_t) (v->aux_int & 0xff);
    uint8_t code = xr_ffi_ptr_aux_type(aux);
    int64_t endian = XR_ENDIAN_NATIVE;
    bool constant_endian = xicgen_value_is_const_endian(v->args[1], &endian);
    XrRep from_rep = cg_ffi_code_is_float(code)
                         ? XR_REP_F64
                         : (cg_ffi_code_is_ptr(code) ? XR_REP_RAWPTR : XR_REP_I64);
    const char *conv_suffix =
        emit_conversion_prefix(out, v->type, from_rep, cg_value_plan_storage_rep(ctx, v));
    if (cg_ffi_code_is_ptr(code)) {
        if (!constant_endian || endian != XR_ENDIAN_NATIVE) {
            cg_ctx_set_error(ctx);
            emit_codegen_abort_expr(out);
            emit_conversion_suffix(out, conv_suffix);
            return;
        }
        fprintf(out, "xr_raw_load_ptr_unaligned(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_RAWPTR);
        fprintf(out, ")");
    } else if (cg_ffi_code_is_float(code)) {
        uint8_t width = cg_ffi_code_width(ctx, code);
        if ((XrFFIType) code == XR_FFI_T_F32)
            fprintf(out, "(double)xr_raw_f32_from_bits(");
        else
            fprintf(out, "xr_raw_f64_from_bits(");
        xicgen_emit_raw_load_bits(ctx, out, v->args[0], width, v->args[1], constant_endian, endian);
        fprintf(out, ")");
    } else {
        fprintf(out, "(int64_t)(%s)(", cg_ffi_pointee_c_type(ctx, code));
        xicgen_emit_raw_load_bits(ctx, out, v->args[0], cg_ffi_code_width(ctx, code), v->args[1],
                                  constant_endian, endian);
        fprintf(out, ")");
    }
    emit_conversion_suffix(out, conv_suffix);
}

/* raw_store<T>(addr, value) — unchecked, unaligned and alias-safe. */
static void xicgen_ptr_store(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                             const char *prefix) {
    (void) f;
    (void) prefix;
    if (!v || v->nargs != 3 || !v->args[0] || !v->args[1] || !v->args[2]) {
        emit_codegen_abort_expr(out);
        return;
    }
    uint8_t aux = (uint8_t) (v->aux_int & 0xff);
    uint8_t code = xr_ffi_ptr_aux_type(aux);
    int64_t endian = XR_ENDIAN_NATIVE;
    bool constant_endian = xicgen_value_is_const_endian(v->args[2], &endian);
    if (cg_ffi_code_is_ptr(code)) {
        if (!constant_endian || endian != XR_ENDIAN_NATIVE) {
            cg_ctx_set_error(ctx);
            emit_codegen_abort_expr(out);
            return;
        }
        fprintf(out, "xr_raw_store_ptr_unaligned(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_RAWPTR);
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_RAWPTR);
        fprintf(out, ")");
    } else {
        uint8_t width = cg_ffi_code_width(ctx, code);
        fprintf(out, "xr_raw_store_u%u_unaligned(", (unsigned) width * 8u);
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_RAWPTR);
        fprintf(out, ", ");
        xicgen_emit_raw_store_bits(ctx, out, v->args[1], code, v->args[2], constant_endian, endian);
        fprintf(out, ")");
    }
}

/* memcpy(dst, src, byte_count) for MutPtr<T>.copyFromNonOverlapping. */
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

static void xicgen_emit_par_for_plan_state_arg(XiCgenCtx *ctx, FILE *out, const XiFunc *body,
                                               const char *state_name) {
    const XrType *state_type = (body && body->nparams > 0 && body->params && body->params[0])
                                   ? body->params[0]->type
                                   : NULL;
    XrRep state_rep = cg_func_param_abi_rep(ctx, body, 0);
    const char *state_suffix = emit_conversion_prefix(out, state_type, XR_REP_TAGGED, state_rep);
    fprintf(out, "%s", state_name ? state_name : "XR_NULL_VAL");
    emit_conversion_suffix(out, state_suffix);
}

static void xicgen_emit_par_range_i64_arg(XiCgenCtx *ctx, FILE *out, const XiFunc *body,
                                          uint16_t param_index, const char *expr);

static void xicgen_emit_par_for_plan_state_call(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                const XiFunc *body, const XiValue *closure,
                                                const char *prefix, const char *state_name,
                                                const char *iter_name, const char *worker_name,
                                                const char *scoped_closure_name) {
    fprintf(out, "            ");
    emit_fname(ctx, out, prefix, body);
    fprintf(out, "(");
    if (scoped_closure_name)
        fprintf(out, "%s", scoped_closure_name);
    else
        emit_call_hidden_closure(out, f, body, closure);
    fprintf(out, ", ");
    xicgen_emit_par_for_plan_state_arg(ctx, out, body, state_name);
    fprintf(out, ", ");
    xicgen_emit_par_range_i64_arg(ctx, out, body, 1, iter_name);
    fprintf(out, ", ");
    xicgen_emit_par_range_i64_arg(ctx, out, body, 2, worker_name);
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
    uint16_t expected = data && data->plan_state ? 3 : (data && data->range_body ? 3 : 2);
    return body && body->nparams == expected;
}

static void xicgen_emit_par_for_range_wrapper(XiCgenCtx *ctx, FILE *out, const XiFunc *owner,
                                              const XiValue *v, const char *prefix) {
    if (!xicgen_par_for_value_is_range_wrappable(v))
        return;
    const XiParallelForData *data = (const XiParallelForData *) v->aux;
    const XiFunc *body = data->body_func;
    fprintf(out, "static void ");
    xicgen_emit_par_for_range_wrapper_name(ctx, out, owner, v, prefix);
    if (data->plan_state) {
        fprintf(out, "(xrt_closure_t *_cl, XrValue _xr_states, int64_t _xr_begin, int64_t _xr_end, "
                     "int64_t _xr_worker) {\n");
    } else {
        fprintf(out, "(xrt_closure_t *_cl, int64_t _xr_begin, int64_t _xr_end, int64_t _xr_worker) "
                     "{\n");
    }
    if (data->plan_state) {
        fprintf(out,
                "    XrValue _xr_state = xrt_index_get(_xr_states, XR_FROM_INT(_xr_worker));\n");
        fprintf(out, "    for (int64_t _xr_i = _xr_begin; _xr_i < _xr_end; _xr_i++) {\n");
        xicgen_emit_par_for_plan_state_call(ctx, out, owner, body, NULL, prefix, "_xr_state",
                                            "_xr_i", "_xr_worker", "_cl");
        fprintf(out, "    }\n");
    } else if (data->range_body) {
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

static void xicgen_emit_par_map_range_wrapper_name(XiCgenCtx *ctx, FILE *out, const XiFunc *owner,
                                                   const XiValue *par_map, const char *prefix) {
    emit_fname(ctx, out, prefix, owner);
    fprintf(out, "_par_map_range_%u", par_map ? par_map->id : 0);
}

static bool xicgen_par_map_value_is_range_wrappable(const XiValue *v) {
    if (!v || v->op != XI_PAR_MAP || v->nargs < 4 || v->aux_kind != XI_AUX_KIND_PAR_MAP || !v->aux)
        return false;
    const XiParallelMapData *data = (const XiParallelMapData *) v->aux;
    const XiFunc *body = data ? data->body_func : NULL;
    if (!body)
        return false;
    if (data->plan_state)
        return body->nparams == 3;
    return body->nparams == 2 || (data->direct_lane_writes && body->nparams == 3);
}

static void xicgen_emit_par_reduce_body_call_value(XiCgenCtx *ctx, FILE *out, const XiFunc *owner,
                                                   const XiFunc *body, const XiValue *closure,
                                                   const char *prefix, const char *iter_name,
                                                   const char *worker_name,
                                                   const char *closure_name);

static void xicgen_emit_par_map_body_call_as_rep(XiCgenCtx *ctx, FILE *out, const XiFunc *owner,
                                                 const XiFunc *body, const XiValue *closure,
                                                 const char *prefix, const char *iter_name,
                                                 const char *worker_name, const char *closure_name,
                                                 XrRep target_rep) {
    XrRep body_rep = cg_func_return_abi_rep(ctx, body);
    const char *suffix =
        emit_conversion_prefix(out, body ? body->return_type : NULL, body_rep, target_rep);
    xicgen_emit_par_reduce_body_call_value(ctx, out, owner, body, closure, prefix, iter_name,
                                           worker_name, closure_name);
    emit_conversion_suffix(out, suffix);
}

static void xicgen_emit_par_map_plan_state_body_call_as_rep(
    XiCgenCtx *ctx, FILE *out, const XiFunc *owner, const XiFunc *body, const XiValue *closure,
    const char *prefix, const char *state_name, const char *iter_name, const char *worker_name,
    const char *closure_name, XrRep target_rep) {
    XrRep body_rep = cg_func_return_abi_rep(ctx, body);
    const char *suffix =
        emit_conversion_prefix(out, body ? body->return_type : NULL, body_rep, target_rep);
    emit_fname(ctx, out, prefix, body);
    fprintf(out, "(");
    if (closure_name)
        fprintf(out, "%s", closure_name);
    else
        emit_call_hidden_closure(out, owner, body, closure);
    fprintf(out, ", ");
    xicgen_emit_par_for_plan_state_arg(ctx, out, body, state_name);
    fprintf(out, ", ");
    xicgen_emit_par_range_i64_arg(ctx, out, body, 1, iter_name);
    fprintf(out, ", ");
    xicgen_emit_par_range_i64_arg(ctx, out, body, 2, worker_name);
    fprintf(out, ")");
    emit_conversion_suffix(out, suffix);
}

static bool xicgen_par_map_body_has_native_result(XiCgenCtx *ctx, const XiValue *v,
                                                  const CgArrayElemInfo *info) {
    if (!ctx || !v || v->op != XI_PAR_MAP || v->aux_kind != XI_AUX_KIND_PAR_MAP || !v->aux ||
        !info || info->rep == XR_REP_TAGGED)
        return false;
    const XiParallelMapData *data = (const XiParallelMapData *) v->aux;
    const XiFunc *body = data ? data->body_func : NULL;
    return data && data->element_type && body &&
           body->native_callback_kind == XI_NATIVE_CALLBACK_PAR_MAP_SCALAR_BODY &&
           cg_func_return_abi_rep(ctx, body) == info->rep &&
           cg_func_param_abi_rep(ctx, body, 0) == XR_REP_I64 &&
           cg_func_param_abi_rep(ctx, body, 1) == XR_REP_I64;
}

static bool xicgen_par_map_array_elem_info(XiCgenCtx *ctx, const XiValue *v, CgArrayElemInfo *out) {
    if (!ctx || !v || !out || v->op != XI_PAR_MAP || v->aux_kind != XI_AUX_KIND_PAR_MAP || !v->aux)
        return false;
    const XiParallelMapData *data = (const XiParallelMapData *) v->aux;
    uint16_t into_arg = data->plan_state ? 5 : 4;
    if (data->into_result && v->nargs > into_arg &&
        cg_array_elem_info_from_type_ctx(ctx, v->args[into_arg] ? v->args[into_arg]->type : NULL,
                                         out))
        return true;
    return cg_array_elem_info_from_type_ctx(ctx, v->type, out);
}

static void xicgen_emit_par_map_zero_value(FILE *out, const CgArrayElemInfo *info) {
    const char *elem = (info && info->elem_name) ? info->elem_name : "XR_ELEM_ANY";
    if (strcmp(elem, "XR_ELEM_RUNE") == 0)
        fprintf(out, "XR_FROM_RUNE(0)");
    else if (strcmp(elem, "XR_ELEM_F32") == 0 || strcmp(elem, "XR_ELEM_F64") == 0)
        fprintf(out, "XR_FROM_FLOAT(0.0)");
    else if (strcmp(elem, "XR_ELEM_BOOL") == 0)
        fprintf(out, "XR_FALSE_VAL");
    else if (strcmp(elem, "XR_ELEM_ANY") == 0)
        fprintf(out, "XR_NULL_VAL");
    else
        fprintf(out, "XR_FROM_INT(0)");
}

static void xicgen_emit_par_map_range_wrapper(XiCgenCtx *ctx, FILE *out, const XiFunc *owner,
                                              const XiValue *v, const char *prefix) {
    if (!xicgen_par_map_value_is_range_wrappable(v))
        return;
    const XiParallelMapData *data = (const XiParallelMapData *) v->aux;
    const XiFunc *body = data->body_func;
    CgArrayElemInfo info;
    bool have_info = xicgen_par_map_array_elem_info(ctx, v, &info);
    bool native_result = have_info && xicgen_par_map_body_has_native_result(ctx, v, &info);

    fprintf(out, "static void ");
    xicgen_emit_par_map_range_wrapper_name(ctx, out, owner, v, prefix);
    if (data->plan_state) {
        fprintf(out, "(xrt_closure_t *_cl, XrValue _xr_states, int64_t _xr_begin, int64_t _xr_end, "
                     "int64_t _xr_worker) {\n");
    } else {
        fprintf(out, "(xrt_closure_t *_cl, int64_t _xr_begin, int64_t _xr_end, int64_t _xr_worker) "
                     "{\n");
    }
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
    if (data->plan_state)
        fprintf(out,
                "    XrValue _xr_state = xrt_index_get(_xr_states, XR_FROM_INT(_xr_worker));\n");
    fprintf(out, "    for (int64_t _xr_i = _xr_begin; _xr_i < _xr_end; _xr_i++) {\n");
    if (data->direct_lane_writes) {
        xicgen_emit_par_for_body_call(ctx, out, owner, body, NULL, prefix, "_xr_i", "_xr_worker",
                                      "_cl");
    } else {
        fprintf(out, "        int64_t _xr_idx = _xr_i - _xr_start;\n");
        if (native_result) {
            fprintf(out, "        ((%s*)_xr_out->data)[_xr_idx] = (%s)", info.ctype, info.ctype);
            if (data->plan_state)
                xicgen_emit_par_map_plan_state_body_call_as_rep(ctx, out, owner, body, NULL, prefix,
                                                                "_xr_state", "_xr_i", "_xr_worker",
                                                                "_cl", info.rep);
            else
                xicgen_emit_par_map_body_call_as_rep(ctx, out, owner, body, NULL, prefix, "_xr_i",
                                                     "_xr_worker", "_cl", info.rep);
            fprintf(out, ";\n");
        } else {
            fprintf(out, "        XrValue _xr_item = ");
            if (data->plan_state)
                xicgen_emit_par_map_plan_state_body_call_as_rep(ctx, out, owner, body, NULL, prefix,
                                                                "_xr_state", "_xr_i", "_xr_worker",
                                                                "_cl", XR_REP_TAGGED);
            else
                xicgen_emit_par_map_body_call_as_rep(ctx, out, owner, body, NULL, prefix, "_xr_i",
                                                     "_xr_worker", "_cl", XR_REP_TAGGED);
            fprintf(out, ";\n");
            fprintf(out, "        xrt_array_write_preallocated(_xr_out, _xr_idx, _xr_item);\n");
        }
    }
    fprintf(out, "    }\n");
    fprintf(out, "}\n\n");
}

static void xicgen_emit_par_map_range_wrappers(XiCgenCtx *ctx, FILE *out, const XiFunc *owner,
                                               const char *prefix) {
    if (!ctx || !out || !owner)
        return;
    for (uint32_t bi = 0; bi < owner->nblocks; bi++) {
        const XiBlock *blk = owner->blocks ? owner->blocks[bi] : NULL;
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values ? blk->values[vi] : NULL;
            if (xicgen_par_map_value_is_range_wrappable(v))
                xicgen_emit_par_map_range_wrapper(ctx, out, owner, v, prefix);
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
    uint16_t expected_body_params =
        data && data->plan_state ? 3 : (data && data->range_body ? 3 : 2);
    if (!ctype || !body || !combine || body->nparams != expected_body_params ||
        combine->nparams != 2)
        return false;
    if (!cg_func_return_abi_is_struct_aggregate(ctx, body) ||
        !cg_func_return_abi_is_struct_aggregate(ctx, combine))
        return false;
    if (data && data->plan_state) {
        if (cg_func_param_abi_rep(ctx, body, 1) != XR_REP_I64 ||
            cg_func_param_abi_rep(ctx, body, 2) != XR_REP_I64)
            return false;
    } else if (cg_func_param_abi_rep(ctx, body, 0) != XR_REP_I64 ||
               cg_func_param_abi_rep(ctx, body, 1) != XR_REP_I64 ||
               (data->range_body && cg_func_param_abi_rep(ctx, body, 2) != XR_REP_I64)) {
        return false;
    }
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
    uint16_t expected_body_params =
        data && data->plan_state ? 3 : (data && data->range_body ? 3 : 2);
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

static void xicgen_emit_par_reduce_plan_state_body_call_value(
    XiCgenCtx *ctx, FILE *out, const XiFunc *owner, const XiFunc *body, const XiValue *closure,
    const char *prefix, const char *state_name, const char *iter_name, const char *worker_name,
    const char *closure_name) {
    emit_fname(ctx, out, prefix, body);
    fprintf(out, "(");
    if (closure_name)
        fprintf(out, "%s", closure_name);
    else
        emit_call_hidden_closure(out, owner, body, closure);
    fprintf(out, ", ");
    xicgen_emit_par_for_plan_state_arg(ctx, out, body, state_name);
    fprintf(out, ", ");
    xicgen_emit_par_range_i64_arg(ctx, out, body, 1, iter_name);
    fprintf(out, ", ");
    xicgen_emit_par_range_i64_arg(ctx, out, body, 2, worker_name);
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
        if (data->plan_state) {
            fprintf(out, "(xrt_closure_t *_cl, XrValue _xr_states, int64_t _xr_begin, "
                         "int64_t _xr_end, int64_t _xr_worker, void *_xr_out_void) {\n");
            fprintf(out, "    XrValue _xr_state = xrt_index_get(_xr_states, "
                         "XR_FROM_INT(_xr_worker));\n");
        } else {
            fprintf(out, "(xrt_closure_t *_cl, int64_t _xr_begin, int64_t _xr_end, "
                         "int64_t _xr_worker, void *_xr_out_void) {\n");
        }
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
            if (data->plan_state)
                xicgen_emit_par_reduce_plan_state_body_call_value(
                    ctx, out, owner, body, NULL, prefix, "_xr_state", "_xr_i", "_xr_worker", "_cl");
            else
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
    if (data->plan_state) {
        fprintf(out, "(xrt_closure_t *_cl, XrValue _xr_states, int64_t _xr_begin, int64_t _xr_end, "
                     "int64_t _xr_worker, int64_t *_xr_out) {\n");
        fprintf(out, "    XrValue _xr_state = xrt_index_get(_xr_states, "
                     "XR_FROM_INT(_xr_worker));\n");
    } else {
        fprintf(out, "(xrt_closure_t *_cl, int64_t _xr_begin, int64_t _xr_end, int64_t _xr_worker, "
                     "int64_t *_xr_out) {\n");
    }
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
        if (data->plan_state)
            xicgen_emit_par_reduce_plan_state_body_call_value(
                ctx, out, owner, body, NULL, prefix, "_xr_state", "_xr_i", "_xr_worker", "_cl");
        else
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

static bool xicgen_par_map_uses_stack_body_closure(const XiValue *par_map, const XiValue *closure,
                                                   const XiFunc *body) {
    if (!par_map || !closure || !body || par_map->op != XI_PAR_MAP || par_map->nargs < 4)
        return false;
    if (par_map->args[3] != closure)
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
                      (ai == 3 && xicgen_par_map_uses_stack_body_closure(user, target, body)) ||
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
                                               const XiValue *closure, const char *prefix,
                                               bool native_entry) {
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
    (void) native_entry;
    fprintf(out, "        ");
    emit_callable_descriptor(ctx, out, prefix, par_for->id, closure, body, 0, 0, NULL);
    fprintf(out, "\n        xrt_closure_init(_xr_par_closure_%u, &_xr_callable_%u, %u);\n",
            par_for->id, par_for->id, ncap);
    fprintf(out, "        { xrt_closure_t *_c = _xr_par_closure_%u; ", par_for->id);
    emit_closure_upval_initializers(ctx, out, f, closure, false);
    fprintf(out, "}\n");
}

static void xicgen_emit_par_map_scoped_closure(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                               const XiValue *par_map, const XiFunc *body,
                                               const XiValue *closure, const char *prefix,
                                               const char *result_value_name,
                                               const char *start_name) {
    const XiParallelMapData *data = (const XiParallelMapData *) (par_map ? par_map->aux : NULL);
    uint16_t ncap = body ? body->ncaptures : 0;
    uint16_t total = (uint16_t) (ncap + 2u);
    fprintf(out,
            "        union { XrObjHeader hdr; long double align; unsigned char "
            "bytes[sizeof(XrObjHeader) + sizeof(xrt_closure_t) + %u * sizeof(XrValue)]; } "
            "_xr_pm_closure_storage_%u;\n",
            total, par_map->id);
    fprintf(out,
            "        memset(&_xr_pm_closure_storage_%u, 0, sizeof(_xr_pm_closure_storage_%u));\n",
            par_map->id, par_map->id);
    fprintf(out,
            "        XrObjHeader *_xr_pm_closure_hdr_%u = (XrObjHeader "
            "*)_xr_pm_closure_storage_%u.bytes;\n",
            par_map->id, par_map->id);
    fprintf(out, "        _xr_pm_closure_hdr_%u->extra = XR_OBJ_STORAGE_STACK;\n", par_map->id);
    fprintf(out,
            "        xrt_closure_t *_xr_pm_closure_%u = (xrt_closure_t *)((char "
            "*)_xr_pm_closure_hdr_%u + sizeof(XrObjHeader));\n",
            par_map->id, par_map->id);
    fprintf(out, "        ");
    emit_callable_descriptor(ctx, out, prefix, par_map->id, closure, body, 0, 0, NULL);
    fprintf(out, "\n        xrt_closure_init(_xr_pm_closure_%u, &_xr_callable_%u, %u);\n",
            par_map->id, par_map->id, total);
    fprintf(out, "        { xrt_closure_t *_c = _xr_pm_closure_%u; ", par_map->id);
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

    if (cg_array_call_is_direct_byte_array_mutator_trusted_nothrow(ctx, current, call))
        return NULL;
    if (cg_array_call_is_byte_array_append_trusted_nothrow(ctx, current, call))
        return NULL;
    if (cg_array_call_is_typed_fill_trusted_nothrow(ctx, current, call))
        return NULL;
    if (cg_array_builtin_call_is_trusted_nothrow(ctx, current, call))
        return NULL;
    if (xicgen_byte_slice_common_prefix_method_drops_helper(ctx, call))
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
            if (xicgen_assert_is_parallel_body_safe(ctx, body, value))
                continue;
            if (xicgen_value_is_proven_nothrow(ctx, body, value, 0))
                continue;
            if (value->op == XI_INDEX_SET && cg_array_index_access_bounds_proven(ctx, body, value))
                continue;
            if (cg_div_mod_is_trusted_nothrow(body, value))
                continue;
            if (cg_byte_slice_load_trusted_nothrow(ctx, body, value))
                continue;
            if (cg_array_builtin_call_is_trusted_nothrow(ctx, body, value))
                continue;
            if (xicgen_err_check_after_proven_nothrow(ctx, body, value))
                continue;
            if (cg_array_err_check_after_byte_slice_load_trusted(ctx, body, value))
                continue;
            if (cg_array_err_check_after_index_get_trusted(ctx, body, value))
                continue;
            if (cg_array_err_check_after_direct_byte_array_mutator_trusted(ctx, body, value))
                continue;
            if (cg_array_err_check_after_byte_array_append_trusted(ctx, body, value))
                continue;
            if (cg_array_err_check_after_typed_fill_trusted(ctx, body, value))
                continue;
            if (cg_array_builtin_err_check_after_trusted_nothrow(ctx, body, value))
                continue;
            if (value->op == XI_PAR_FOR && value->aux_kind == XI_AUX_KIND_PAR_FOR && value->aux) {
                const XiParallelForData *data = (const XiParallelForData *) value->aux;
                const XiFunc *nested_body = data ? data->body_func : NULL;
                if (!nested_body || cg_func_needs_aot_coro_ctx(ctx, nested_body))
                    return value;
                if (xicgen_par_for_stack_contains(stack, depth, nested_body))
                    continue;
                if (depth >= XICGEN_PAR_FOR_BODY_DEPTH_MAX)
                    return value;
                const XiValue *unsupported = xicgen_find_par_for_unsupported_body_value_depth(
                    ctx, nested_body, stack, depth);
                if (unsupported)
                    return unsupported;
                continue;
            }
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
        case XR_KIND_SLICE:
            return "Slice";
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
        case XR_KIND_ERROR:
            return "<error>";
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
            return type->ptr_is_mut ? "MutPtr" : "Ptr";
        case XR_KIND_RUNE:
            return "rune";
        case XR_KIND_RECORD:
            return "record";
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
    fprintf(stderr, "[xi_cgen] ERROR: parallel.reduce AOT %s must use int64(%s) ABI: '%s'\n",
            role ? role : "callback", param_count == 3 ? "int64, int64, int64" : "int64, int64",
            func->name ? func->name : "?");
    return false;
}

static bool xicgen_par_reduce_validate_plan_i64_body(XiCgenCtx *ctx, const XiFunc *func,
                                                     const char *role) {
    if (!ctx || !func || func->nparams != 3)
        return false;
    bool ok = cg_func_return_abi_rep(ctx, func) == XR_REP_I64 &&
              cg_func_param_abi_rep(ctx, func, 1) == XR_REP_I64 &&
              cg_func_param_abi_rep(ctx, func, 2) == XR_REP_I64;
    if (ok)
        return true;
    fprintf(stderr,
            "[xi_cgen] ERROR: parallel.Plan.reduce AOT %s must use int64(state, int64, "
            "int64) ABI: '%s'\n",
            role ? role : "callback", func->name ? func->name : "?");
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

static bool xicgen_par_map_validate_scalar_func(XiCgenCtx *ctx, const XiFunc *func,
                                                const CgArrayElemInfo *info) {
    if (!ctx || !func || !info || info->rep == XR_REP_TAGGED || func->nparams != 2)
        return false;
    if (cg_func_return_abi_rep(ctx, func) == info->rep &&
        cg_func_param_abi_rep(ctx, func, 0) == XR_REP_I64 &&
        cg_func_param_abi_rep(ctx, func, 1) == XR_REP_I64)
        return true;
    fprintf(stderr,
            "[xi_cgen] ERROR: parallel.map AOT scalar body must use %s(int64, int64) ABI: '%s'\n",
            xicgen_rep_label(info->rep), func->name ? func->name : "?");
    return false;
}

static bool xicgen_par_reduce_validate_nothrow_body(XiCgenCtx *ctx, const XiFunc *func,
                                                    const char *role) {
    if (!ctx || !func)
        return false;
    if (cg_func_needs_aot_coro_ctx(ctx, func)) {
        fprintf(stderr, "[xi_cgen] ERROR: parallel.reduce AOT %s cannot be suspendable yet: '%s'\n",
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
            "[xi_cgen] ERROR: parallel.reduce AOT %s cannot throw or suspend yet: '%s' "
            "contains %s%s%s%s at v%u line %u\n",
            role ? role : "callback", func->name ? func->name : "?",
            xi_op_name((XiOp) unsupported->op), detail ? " '" : "", detail ? detail : "",
            detail ? "'" : "", unsupported->id, unsupported->line);
    return false;
}

typedef enum XicgenParallelCallbackMode {
    XICGEN_PAR_CALLBACK_INVALID = 0,
    XICGEN_PAR_CALLBACK_PARALLEL,
    XICGEN_PAR_CALLBACK_SERIAL,
} XicgenParallelCallbackMode;

/* Worker callbacks cannot unwind through another native thread.  A callback
 * that may throw therefore executes on the invoking thread, preserving the
 * ordinary exception/defer stack.  This is a semantic fallback, not a hidden
 * failure: proven no-throw callbacks still take the parallel runtime path,
 * while genuinely suspendable callbacks remain rejected until the runtime has
 * an explicit cross-scheduler callback protocol. */
static XicgenParallelCallbackMode xicgen_parallel_callback_mode(XiCgenCtx *ctx, const XiFunc *func,
                                                                const char *role) {
    if (!ctx || !func)
        return XICGEN_PAR_CALLBACK_INVALID;
    if (cg_func_needs_aot_coro_ctx(ctx, func)) {
        fprintf(stderr, "[xi_cgen] ERROR: parallel AOT %s cannot be suspendable: '%s'\n",
                role ? role : "callback", func->name ? func->name : "?");
        return XICGEN_PAR_CALLBACK_INVALID;
    }
    return xicgen_find_par_for_unsupported_body_value(ctx, func) ? XICGEN_PAR_CALLBACK_SERIAL
                                                                 : XICGEN_PAR_CALLBACK_PARALLEL;
}

static void xicgen_par_reduce_emit_serial_int64max(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                   const XiValue *v, const XiFunc *body,
                                                   const XiFunc *combine, const char *prefix,
                                                   const char *closure_name) {
    const XiParallelReduceData *data = (const XiParallelReduceData *) (v ? v->aux : NULL);
    fprintf(out, "            if (_xr_pr_start_%u <= _xr_pr_end_%u) {\n", v->id, v->id);
    if (data && data->plan_state) {
        fprintf(out,
                "                XrValue _xr_pr_state_%u = xrt_index_get(_xr_pr_states_%u, "
                "XR_FROM_INT(0));\n",
                v->id, v->id);
    }
    fprintf(out, "                for (int64_t _xr_pr_i_%u = _xr_pr_start_%u; ; _xr_pr_i_%u++) {\n",
            v->id, v->id, v->id);
    fprintf(out, "                    int64_t _xr_pr_item_%u = ", v->id);
    char iter_name[64];
    char state_name[64];
    snprintf(iter_name, sizeof(iter_name), "_xr_pr_i_%u", v->id);
    snprintf(state_name, sizeof(state_name), "_xr_pr_state_%u", v->id);
    if (data && data->plan_state)
        xicgen_emit_par_reduce_plan_state_body_call_value(ctx, out, f, body, v->args[4], prefix,
                                                          state_name, iter_name, "0", closure_name);
    else
        xicgen_emit_par_reduce_body_call_value(ctx, out, f, body, v->args[4], prefix, iter_name,
                                               "0", closure_name);
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
    const XiParallelReduceData *data = (const XiParallelReduceData *) (v ? v->aux : NULL);
    fprintf(out, "            if (_xr_pr_start_%u <= _xr_pr_end_%u) {\n", v->id, v->id);
    if (data && data->plan_state) {
        fprintf(out,
                "                XrValue _xr_pr_state_%u = xrt_index_get(_xr_pr_states_%u, "
                "XR_FROM_INT(0));\n",
                v->id, v->id);
    }
    fprintf(out, "                for (int64_t _xr_pr_i_%u = _xr_pr_start_%u; ; _xr_pr_i_%u++) {\n",
            v->id, v->id, v->id);
    fprintf(out, "                    %s _xr_pr_item_%u = ", agg_ctype, v->id);
    char iter_name[64];
    char state_name[64];
    snprintf(iter_name, sizeof(iter_name), "_xr_pr_i_%u", v->id);
    snprintf(state_name, sizeof(state_name), "_xr_pr_state_%u", v->id);
    if (data && data->plan_state)
        xicgen_emit_par_reduce_plan_state_body_call_value(ctx, out, f, body, v->args[4], prefix,
                                                          state_name, iter_name, "0", closure_name);
    else
        xicgen_emit_par_reduce_body_call_value(ctx, out, f, body, v->args[4], prefix, iter_name,
                                               "0", closure_name);
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
    if (data->plan_state && v->nargs < 7) {
        xicgen_par_reduce_emit_abort_expr(ctx, out);
        return;
    }
    bool i64_accumulator = xicgen_par_reduce_value_has_i64_accumulator(v);
    bool struct_accumulator = xicgen_par_reduce_value_has_struct_accumulator(ctx, v);
    const char *agg_ctype = struct_accumulator ? xicgen_par_reduce_struct_agg_c_type(ctx, v) : NULL;
    if (data->range_body && data->inclusive_end) {
        fprintf(stderr,
                "[xi_cgen] ERROR: parallel.reduce range-body AOT requires an exclusive range\n");
        xicgen_par_reduce_emit_abort_value(ctx, out, agg_ctype);
        return;
    }
    if (!i64_accumulator && !struct_accumulator) {
        fprintf(stderr,
                "[xi_cgen] ERROR: parallel.reduce AOT supports only native int or native struct "
                "accumulators; accumulator type '%s' is not native-reducible yet\n",
                xicgen_type_label_noalloc(data->accumulator_type));
        xicgen_par_reduce_emit_abort_value(ctx, out, agg_ctype);
        return;
    }
    uint16_t expected_body_params = data->plan_state ? 3 : (data->range_body ? 3 : 2);
    bool i64_body_ok =
        !i64_accumulator ||
        (data->plan_state
             ? xicgen_par_reduce_validate_plan_i64_body(ctx, body, "body")
             : xicgen_par_reduce_validate_i64_func(ctx, body, "body", expected_body_params));
    if (!body || !combine ||
        (i64_accumulator &&
         (!i64_body_ok || !xicgen_par_reduce_validate_i64_func(ctx, combine, "combine", 2))) ||
        (struct_accumulator && !agg_ctype) ||
        !xicgen_par_reduce_validate_nothrow_body(ctx, body, "body") ||
        !xicgen_par_reduce_validate_nothrow_body(ctx, combine, "combine")) {
        xicgen_par_reduce_emit_abort_value(ctx, out, agg_ctype);
        return;
    }
    if (combine->ncaptures != 0) {
        fprintf(stderr,
                "[xi_cgen] ERROR: parallel.reduce AOT combine cannot capture values yet: '%s'\n",
                combine->name ? combine->name : "?");
        xicgen_par_reduce_emit_abort_value(ctx, out, agg_ctype);
        return;
    }

    bool scoped_closure = xicgen_par_reduce_uses_stack_body_closure(v, v->args[4], body);
    char scoped_closure_name[64];
    scoped_closure_name[0] = '\0';
    const char *aot_ctx_expr = xicgen_aot_context_expr(ctx, f);

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
    if (data->plan_state) {
        fprintf(out, "        XrValue _xr_pr_states_%u = ", v->id);
        emit_value_as_rep_ctx(ctx, out, v->args[6], XR_REP_TAGGED);
        fprintf(out, ";\n");
    }
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
        xicgen_emit_par_for_scoped_closure(ctx, out, f, v, body, v->args[4], prefix,
                                           data->plan_state);
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
            if (data->plan_state) {
                fprintf(out,
                        "            if (!xr_parallel_reduce_state_agg(%s, _xr_pr_start_%u, "
                        "_xr_pr_end_excl_%u, _xr_pr_workers_%u, sizeof(%s), &_xr_pr_out_%u, "
                        "(XrParallelReduceRangeStateAggFn)",
                        aot_ctx_expr, v->id, v->id, v->id, agg_ctype, v->id);
                xicgen_emit_par_reduce_range_wrapper_name(ctx, out, f, v, prefix);
                fprintf(out, ", (XrParallelReduceCombineAggFn)");
                xicgen_emit_par_reduce_combine_wrapper_name(ctx, out, f, v, prefix);
                fprintf(out, ", ");
                if (scoped_closure)
                    fprintf(out, "%s", scoped_closure_name);
                else
                    emit_call_hidden_closure(out, f, body, v->args[4]);
                fprintf(out, ", _xr_pr_states_%u, &_xr_pr_out_%u)) abort();\n", v->id, v->id);
            } else {
                fprintf(out,
                        "            if (!xr_parallel_reduce_agg(%s, _xr_pr_start_%u, "
                        "_xr_pr_end_excl_%u, _xr_pr_workers_%u, sizeof(%s), &_xr_pr_out_%u, "
                        "(XrParallelReduceRangeAggFn)",
                        aot_ctx_expr, v->id, v->id, v->id, agg_ctype, v->id);
                xicgen_emit_par_reduce_range_wrapper_name(ctx, out, f, v, prefix);
                fprintf(out, ", (XrParallelReduceCombineAggFn)");
                xicgen_emit_par_reduce_combine_wrapper_name(ctx, out, f, v, prefix);
                fprintf(out, ", ");
                if (scoped_closure)
                    fprintf(out, "%s", scoped_closure_name);
                else
                    emit_call_hidden_closure(out, f, body, v->args[4]);
                fprintf(out, ", &_xr_pr_out_%u)) abort();\n", v->id);
            }
            fprintf(out, "        }\n");
        } else {
            if (data->plan_state) {
                fprintf(out,
                        "        if (!xr_parallel_reduce_state_agg(%s, _xr_pr_start_%u, "
                        "_xr_pr_end_%u, _xr_pr_workers_%u, sizeof(%s), &_xr_pr_out_%u, "
                        "(XrParallelReduceRangeStateAggFn)",
                        aot_ctx_expr, v->id, v->id, v->id, agg_ctype, v->id);
                xicgen_emit_par_reduce_range_wrapper_name(ctx, out, f, v, prefix);
                fprintf(out, ", (XrParallelReduceCombineAggFn)");
                xicgen_emit_par_reduce_combine_wrapper_name(ctx, out, f, v, prefix);
                fprintf(out, ", ");
                if (scoped_closure)
                    fprintf(out, "%s", scoped_closure_name);
                else
                    emit_call_hidden_closure(out, f, body, v->args[4]);
                fprintf(out, ", _xr_pr_states_%u, &_xr_pr_out_%u)) abort();\n", v->id, v->id);
            } else {
                fprintf(out,
                        "        if (!xr_parallel_reduce_agg(%s, _xr_pr_start_%u, _xr_pr_end_%u, "
                        "_xr_pr_workers_%u, sizeof(%s), &_xr_pr_out_%u, "
                        "(XrParallelReduceRangeAggFn)",
                        aot_ctx_expr, v->id, v->id, v->id, agg_ctype, v->id);
                xicgen_emit_par_reduce_range_wrapper_name(ctx, out, f, v, prefix);
                fprintf(out, ", (XrParallelReduceCombineAggFn)");
                xicgen_emit_par_reduce_combine_wrapper_name(ctx, out, f, v, prefix);
                fprintf(out, ", ");
                if (scoped_closure)
                    fprintf(out, "%s", scoped_closure_name);
                else
                    emit_call_hidden_closure(out, f, body, v->args[4]);
                fprintf(out, ", &_xr_pr_out_%u)) abort();\n", v->id);
            }
        }
    } else if (data->inclusive_end) {
        fprintf(out, "        if (_xr_pr_end_%u == INT64_MAX) {\n", v->id);
        xicgen_par_reduce_emit_serial_int64max(ctx, out, f, v, body, combine, prefix,
                                               scoped_closure ? scoped_closure_name : NULL);
        fprintf(out, "        } else {\n");
        fprintf(out, "            int64_t _xr_pr_end_excl_%u = _xr_pr_end_%u + 1;\n", v->id, v->id);
        if (data->plan_state) {
            fprintf(out,
                    "            if (!xr_parallel_reduce_state_i64(%s, _xr_pr_start_%u, "
                    "_xr_pr_end_excl_%u, _xr_pr_workers_%u, _xr_pr_out_%u, "
                    "(XrParallelReduceRangeStateI64Fn)",
                    aot_ctx_expr, v->id, v->id, v->id, v->id);
            xicgen_emit_par_reduce_range_wrapper_name(ctx, out, f, v, prefix);
            fprintf(out, ", (XrParallelReduceCombineI64Fn)");
            xicgen_emit_par_reduce_combine_wrapper_name(ctx, out, f, v, prefix);
            fprintf(out, ", ");
            if (scoped_closure)
                fprintf(out, "%s", scoped_closure_name);
            else
                emit_call_hidden_closure(out, f, body, v->args[4]);
            fprintf(out, ", _xr_pr_states_%u, &_xr_pr_out_%u)) abort();\n", v->id, v->id);
        } else {
            fprintf(out,
                    "            if (!xr_parallel_reduce_i64(%s, _xr_pr_start_%u, "
                    "_xr_pr_end_excl_%u, _xr_pr_workers_%u, _xr_pr_out_%u, "
                    "(XrParallelReduceRangeI64Fn)",
                    aot_ctx_expr, v->id, v->id, v->id, v->id);
            xicgen_emit_par_reduce_range_wrapper_name(ctx, out, f, v, prefix);
            fprintf(out, ", (XrParallelReduceCombineI64Fn)");
            xicgen_emit_par_reduce_combine_wrapper_name(ctx, out, f, v, prefix);
            fprintf(out, ", ");
            if (scoped_closure)
                fprintf(out, "%s", scoped_closure_name);
            else
                emit_call_hidden_closure(out, f, body, v->args[4]);
            fprintf(out, ", &_xr_pr_out_%u)) abort();\n", v->id);
        }
        fprintf(out, "        }\n");
    } else {
        if (data->plan_state) {
            fprintf(out,
                    "        if (!xr_parallel_reduce_state_i64(%s, _xr_pr_start_%u, "
                    "_xr_pr_end_%u, _xr_pr_workers_%u, _xr_pr_out_%u, "
                    "(XrParallelReduceRangeStateI64Fn)",
                    aot_ctx_expr, v->id, v->id, v->id, v->id);
            xicgen_emit_par_reduce_range_wrapper_name(ctx, out, f, v, prefix);
            fprintf(out, ", (XrParallelReduceCombineI64Fn)");
            xicgen_emit_par_reduce_combine_wrapper_name(ctx, out, f, v, prefix);
            fprintf(out, ", ");
            if (scoped_closure)
                fprintf(out, "%s", scoped_closure_name);
            else
                emit_call_hidden_closure(out, f, body, v->args[4]);
            fprintf(out, ", _xr_pr_states_%u, &_xr_pr_out_%u)) abort();\n", v->id, v->id);
        } else {
            fprintf(out,
                    "        if (!xr_parallel_reduce_i64(%s, _xr_pr_start_%u, _xr_pr_end_%u, "
                    "_xr_pr_workers_%u, _xr_pr_out_%u, (XrParallelReduceRangeI64Fn)",
                    aot_ctx_expr, v->id, v->id, v->id, v->id);
            xicgen_emit_par_reduce_range_wrapper_name(ctx, out, f, v, prefix);
            fprintf(out, ", (XrParallelReduceCombineI64Fn)");
            xicgen_emit_par_reduce_combine_wrapper_name(ctx, out, f, v, prefix);
            fprintf(out, ", ");
            if (scoped_closure)
                fprintf(out, "%s", scoped_closure_name);
            else
                emit_call_hidden_closure(out, f, body, v->args[4]);
            fprintf(out, ", &_xr_pr_out_%u)) abort();\n", v->id);
        }
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

static void xicgen_par_map_emit_store(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                      const XiFunc *body, const char *prefix, const char *iter_name,
                                      const char *worker_name, const char *closure_name,
                                      const char *state_name, const char *out_ptr_name,
                                      const char *idx_name, bool native_result,
                                      const CgArrayElemInfo *info) {
    const XiParallelMapData *data = (const XiParallelMapData *) (v ? v->aux : NULL);
    if (native_result && info) {
        fprintf(out, "            ((%s*)%s->data)[%s] = (%s)", info->ctype, out_ptr_name, idx_name,
                info->ctype);
        if (data && data->plan_state)
            xicgen_emit_par_map_plan_state_body_call_as_rep(ctx, out, f, body, v->args[3], prefix,
                                                            state_name, iter_name, worker_name,
                                                            closure_name, info->rep);
        else
            xicgen_emit_par_map_body_call_as_rep(ctx, out, f, body, v->args[3], prefix, iter_name,
                                                 worker_name, closure_name, info->rep);
        fprintf(out, ";\n");
        return;
    }
    fprintf(out, "            XrValue _xr_pm_item_%u = ", v->id);
    if (data && data->plan_state)
        xicgen_emit_par_map_plan_state_body_call_as_rep(ctx, out, f, body, v->args[3], prefix,
                                                        state_name, iter_name, worker_name,
                                                        closure_name, XR_REP_TAGGED);
    else
        xicgen_emit_par_map_body_call_as_rep(ctx, out, f, body, v->args[3], prefix, iter_name,
                                             worker_name, closure_name, XR_REP_TAGGED);
    fprintf(out, ";\n");
    fprintf(out, "            xrt_array_write_preallocated(%s, %s, _xr_pm_item_%u);\n",
            out_ptr_name, idx_name, v->id);
}

static void xicgen_par_map_emit_serial_exclusive(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                 const XiValue *v, const XiFunc *body,
                                                 const char *prefix, const char *iter_name,
                                                 const char *idx_name, const char *closure_name,
                                                 const char *state_name, const char *out_ptr_name,
                                                 bool native_result, const CgArrayElemInfo *info) {
    const XiParallelMapData *data = (const XiParallelMapData *) (v ? v->aux : NULL);
    if (data && data->plan_state) {
        fprintf(out,
                "                XrValue %s = xrt_index_get(_xr_pm_states_%u, "
                "XR_FROM_INT(0));\n",
                state_name, v->id);
    }
    fprintf(out,
            "                for (int64_t %s = _xr_pm_start_%u; %s < "
            "_xr_pm_end_excl_%u; %s++) {\n",
            iter_name, v->id, iter_name, v->id, iter_name);
    fprintf(out, "                    int64_t %s = %s - _xr_pm_start_%u;\n", idx_name, iter_name,
            v->id);
    xicgen_par_map_emit_store(ctx, out, f, v, body, prefix, iter_name, "0", closure_name,
                              data && data->plan_state ? state_name : NULL, out_ptr_name, idx_name,
                              native_result, info);
    fprintf(out, "                }\n");
}

static void xicgen_par_map(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                           const char *prefix) {
    if (!ctx || !out || !f || !v || v->nargs < 4 || v->aux_kind != XI_AUX_KIND_PAR_MAP || !v->aux) {
        xicgen_par_reduce_emit_abort_expr(ctx, out);
        return;
    }

    const XiParallelMapData *data = (const XiParallelMapData *) v->aux;
    const XiFunc *body = data->body_func;
    const char *aot_ctx_expr = xicgen_aot_context_expr(ctx, f);
    if (data->direct_lane_writes) {
        uint16_t expected_params = body ? body->nparams : 0;
        bool range_body = expected_params == 3;
        if (!body || (expected_params != 2 && expected_params != 3) ||
            cg_func_return_abi_rep(ctx, body) != XR_REP_VOID ||
            cg_func_param_abi_rep(ctx, body, 0) != XR_REP_I64 ||
            cg_func_param_abi_rep(ctx, body, 1) != XR_REP_I64 ||
            (range_body && cg_func_param_abi_rep(ctx, body, 2) != XR_REP_I64) ||
            !xicgen_par_reduce_validate_nothrow_body(ctx, body, "map lane body")) {
            xicgen_par_reduce_emit_abort_expr(ctx, out);
            return;
        }
        if (range_body && data->inclusive_end) {
            fprintf(stderr, "[xi_cgen] ERROR: parallel.map direct-lane initializer AOT requires an "
                            "exclusive range\n");
            xicgen_par_reduce_emit_abort_expr(ctx, out);
            return;
        }

        fprintf(out, "({\n");
        fprintf(out, "        int64_t _xr_pm_start_%u = ", v->id);
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
        fprintf(out, ";\n");
        fprintf(out, "        int64_t _xr_pm_end_%u = ", v->id);
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
        fprintf(out, ";\n");
        fprintf(out, "        int64_t _xr_pm_workers_%u = ", v->id);
        emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_I64);
        fprintf(out, ";\n");
        if (data->inclusive_end) {
            fprintf(out, "        bool _xr_pm_serial_max_%u = _xr_pm_end_%u == INT64_MAX;\n", v->id,
                    v->id);
            fprintf(out,
                    "        int64_t _xr_pm_end_excl_%u = _xr_pm_serial_max_%u ? _xr_pm_end_%u : "
                    "_xr_pm_end_%u + 1;\n",
                    v->id, v->id, v->id, v->id);
        } else {
            fprintf(out, "        bool _xr_pm_serial_max_%u = false;\n", v->id);
            fprintf(out, "        int64_t _xr_pm_end_excl_%u = _xr_pm_end_%u;\n", v->id, v->id);
        }
        fprintf(out,
                "        int64_t _xr_pm_count_%u = _xr_pm_serial_max_%u ? (_xr_pm_end_%u >= "
                "_xr_pm_start_%u ? _xr_pm_end_%u - _xr_pm_start_%u + 1 : 0) : (_xr_pm_end_excl_%u "
                "> _xr_pm_start_%u ? _xr_pm_end_excl_%u - _xr_pm_start_%u : 0);\n",
                v->id, v->id, v->id, v->id, v->id, v->id, v->id, v->id, v->id, v->id);

        uint16_t lane_count = data->lane_count;
        if (lane_count < 1 || lane_count > 16 || v->nargs < (uint16_t) (4u + lane_count)) {
            fprintf(stderr, "[xi_cgen] ERROR: parallel.map lane metadata mismatch\n");
            xicgen_par_reduce_emit_abort_expr(ctx, out);
            fprintf(out, ";\n    })");
            return;
        }
        if (!data->into_result && lane_count != 1) {
            fprintf(stderr,
                    "[xi_cgen] ERROR: returning parallel.map direct lanes require one lane\n");
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
            fprintf(out, "        XrValue _xr_pm_result_%u_%u = ", v->id, (unsigned) i);
            emit_value_as_rep_ctx(ctx, out, v->args[4 + i], XR_REP_TAGGED);
            fprintf(out, ";\n");
            fprintf(
                out,
                "        xrt_array_t *_xr_pm_out_%u_%u = (xrt_array_t*)_xr_pm_result_%u_%u.ptr;\n",
                v->id, (unsigned) i, v->id, (unsigned) i);
            if (typed_full_overwrite) {
                fprintf(out,
                        "        if (_xr_pm_out_%u_%u->data_storage == XR_ARRAY_DATA_BORROWED) "
                        "abort();\n",
                        v->id, (unsigned) i);
                fprintf(out,
                        "        if (_xr_pm_count_%u > _xr_pm_out_%u_%u->capacity) "
                        "xrt_array_reserve_raw(_xr_pm_out_%u_%u, _xr_pm_count_%u);\n",
                        v->id, v->id, (unsigned) i, v->id, (unsigned) i, v->id);
                fprintf(out, "        _xr_pm_out_%u_%u->length = _xr_pm_count_%u;\n", v->id,
                        (unsigned) i, v->id);
            } else {
                fprintf(out,
                        "        xrt_array_resize_value(_xr_pm_result_%u_%u, "
                        "XR_FROM_INT(_xr_pm_count_%u), ",
                        v->id, (unsigned) i, v->id);
                xicgen_emit_par_map_zero_value(out, have_lane_info ? &lane_info : NULL);
                fprintf(out, ");\n");
            }
        }

        bool scoped_closure = xicgen_par_map_uses_stack_body_closure(v, v->args[3], body);
        char closure_name[64];
        closure_name[0] = '\0';
        if (scoped_closure) {
            snprintf(closure_name, sizeof(closure_name), "_xr_pm_closure_%u", v->id);
            uint16_t ncap = body ? body->ncaptures : 0;
            fprintf(out,
                    "        union { XrObjHeader hdr; long double align; unsigned char "
                    "bytes[sizeof(XrObjHeader) + sizeof(xrt_closure_t) + %u * sizeof(XrValue)]; } "
                    "_xr_pm_closure_storage_%u;\n",
                    ncap, v->id);
            fprintf(out,
                    "        memset(&_xr_pm_closure_storage_%u, 0, "
                    "sizeof(_xr_pm_closure_storage_%u));\n",
                    v->id, v->id);
            fprintf(out,
                    "        XrObjHeader *_xr_pm_closure_hdr_%u = (XrObjHeader "
                    "*)_xr_pm_closure_storage_%u.bytes;\n",
                    v->id, v->id);
            fprintf(out, "        _xr_pm_closure_hdr_%u->extra = XR_OBJ_STORAGE_STACK;\n", v->id);
            fprintf(out,
                    "        xrt_closure_t *_xr_pm_closure_%u = (xrt_closure_t *)((char "
                    "*)_xr_pm_closure_hdr_%u + sizeof(XrObjHeader));\n",
                    v->id, v->id);
            fprintf(out, "        ");
            emit_callable_descriptor(ctx, out, prefix, v->id, v->args[3], body, 0, 0, NULL);
            fprintf(out, "\n        xrt_closure_init(_xr_pm_closure_%u, &_xr_callable_%u, %u);\n",
                    v->id, v->id, ncap);
            fprintf(out, "        { xrt_closure_t *_c = _xr_pm_closure_%u; ", v->id);
            emit_closure_upval_initializers(ctx, out, f, v->args[3], false);
            fprintf(out, "}\n");
        }

        char iter_name[64];
        snprintf(iter_name, sizeof(iter_name), "_xr_pm_i_%u", v->id);
        fprintf(out, "        if (_xr_pm_count_%u > 0) {\n", v->id);
        fprintf(out, "            if (_xr_pm_serial_max_%u) {\n", v->id);
        if (range_body) {
            fprintf(out, "                abort();\n");
        } else {
            fprintf(out, "                for (int64_t %s = _xr_pm_start_%u; ; %s++) {\n",
                    iter_name, v->id, iter_name);
            xicgen_emit_par_for_body_call(ctx, out, f, body, v->args[3], prefix, iter_name, "0",
                                          scoped_closure ? closure_name : NULL);
            fprintf(out, "                    if (%s == _xr_pm_end_%u) break;\n", iter_name, v->id);
            fprintf(out, "                }\n");
        }
        fprintf(out,
                "            } else if (!xr_parallel_for_range_i64(%s, _xr_pm_start_%u, "
                "_xr_pm_end_excl_%u, _xr_pm_workers_%u, (XrParallelRangeI64Fn)",
                aot_ctx_expr, v->id, v->id, v->id);
        xicgen_emit_par_map_range_wrapper_name(ctx, out, f, v, prefix);
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
            fprintf(out, "_xr_pm_out_%u_0", v->id);
        else
            fprintf(out, "_xr_pm_result_%u_0", v->id);
        fprintf(out, ";\n");
        fprintf(out, "    })");
        return;
    }

    CgArrayElemInfo info;
    uint16_t output_arg_index = data->plan_state ? 5 : 4;
    bool into_result = data->into_result && v->nargs > output_arg_index;
    bool have_info = xicgen_par_map_array_elem_info(ctx, v, &info);
    bool typed_full_overwrite =
        have_info && info.elem_name && strcmp(info.elem_name, "XR_ELEM_ANY") != 0;
    bool native_result = have_info && xicgen_par_map_body_has_native_result(ctx, v, &info);
    uint16_t expected_params = data->plan_state ? 3 : 2;
    XicgenParallelCallbackMode callback_mode = xicgen_parallel_callback_mode(ctx, body, "map body");
    if (!body || body->nparams != expected_params ||
        (body->native_callback_kind == XI_NATIVE_CALLBACK_PAR_MAP_SCALAR_BODY &&
         !xicgen_par_map_validate_scalar_func(ctx, body, have_info ? &info : NULL)) ||
        callback_mode == XICGEN_PAR_CALLBACK_INVALID) {
        xicgen_par_reduce_emit_abort_expr(ctx, out);
        return;
    }

    XrRep storage_rep = xicgen_value_c_storage_rep(ctx, f, v);
    fprintf(out, "({\n");
    fprintf(out, "        int64_t _xr_pm_start_%u = ", v->id);
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
    fprintf(out, ";\n");
    fprintf(out, "        int64_t _xr_pm_end_%u = ", v->id);
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
    fprintf(out, ";\n");
    fprintf(out, "        int64_t _xr_pm_workers_%u = ", v->id);
    emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_I64);
    fprintf(out, ";\n");
    if (data->plan_state) {
        fprintf(out, "        XrValue _xr_pm_states_%u = ", v->id);
        emit_value_as_rep_ctx(ctx, out, v->args[4], XR_REP_TAGGED);
        fprintf(out, ";\n");
    }
    if (data->inclusive_end) {
        fprintf(out, "        bool _xr_pm_serial_max_%u = _xr_pm_end_%u == INT64_MAX;\n", v->id,
                v->id);
        fprintf(out,
                "        int64_t _xr_pm_end_excl_%u = _xr_pm_serial_max_%u ? _xr_pm_end_%u : "
                "_xr_pm_end_%u + 1;\n",
                v->id, v->id, v->id, v->id);
    } else {
        fprintf(out, "        bool _xr_pm_serial_max_%u = false;\n", v->id);
        fprintf(out, "        int64_t _xr_pm_end_excl_%u = _xr_pm_end_%u;\n", v->id, v->id);
    }
    fprintf(out,
            "        int64_t _xr_pm_count_%u = _xr_pm_serial_max_%u ? (_xr_pm_end_%u >= "
            "_xr_pm_start_%u ? _xr_pm_end_%u - _xr_pm_start_%u + 1 : 0) : (_xr_pm_end_excl_%u > "
            "_xr_pm_start_%u ? _xr_pm_end_excl_%u - _xr_pm_start_%u : 0);\n",
            v->id, v->id, v->id, v->id, v->id, v->id, v->id, v->id, v->id, v->id);
    if (into_result) {
        fprintf(out, "        XrValue _xr_pm_result_%u = ", v->id);
        emit_value_as_rep_ctx(ctx, out, v->args[output_arg_index], XR_REP_TAGGED);
        fprintf(out, ";\n");
        fprintf(out, "        xrt_array_t *_xr_pm_out_%u = (xrt_array_t*)_xr_pm_result_%u.ptr;\n",
                v->id, v->id);
        if (typed_full_overwrite) {
            fprintf(out,
                    "        if (_xr_pm_out_%u->data_storage == XR_ARRAY_DATA_BORROWED) abort();\n",
                    v->id);
            fprintf(out,
                    "        if (_xr_pm_count_%u > _xr_pm_out_%u->capacity) "
                    "xrt_array_reserve_raw(_xr_pm_out_%u, _xr_pm_count_%u);\n",
                    v->id, v->id, v->id, v->id);
            fprintf(out, "        _xr_pm_out_%u->length = _xr_pm_count_%u;\n", v->id, v->id);
        } else {
            fprintf(
                out,
                "        xrt_array_resize_value(_xr_pm_result_%u, XR_FROM_INT(_xr_pm_count_%u), ",
                v->id, v->id);
            xicgen_emit_par_map_zero_value(out, have_info ? &info : NULL);
            fprintf(out, ");\n");
        }
    } else if (storage_rep == XR_REP_PTR) {
        fprintf(out, "        xrt_array_t *_xr_pm_out_%u = ", v->id);
        if (typed_full_overwrite)
            fprintf(out, "xrt_array_new_typed_uninit_ptr(_xr_pm_count_%u, %s)", v->id,
                    info.elem_name);
        else if (have_info)
            fprintf(out, "xrt_array_new_typed_ptr(_xr_pm_count_%u, %s)", v->id, info.elem_name);
        else
            fprintf(out, "(xrt_array_t*)xrt_array_new(_xr_pm_count_%u).ptr", v->id);
        fprintf(out, ";\n");
        if (typed_full_overwrite)
            fprintf(out, "        _xr_pm_out_%u->length = _xr_pm_count_%u;\n", v->id, v->id);
        fprintf(out, "        XrValue _xr_pm_result_%u = xr_mkptr(_xr_pm_out_%u, XR_TAG_ARRAY);\n",
                v->id, v->id);
    } else {
        fprintf(out, "        XrValue _xr_pm_result_%u = ", v->id);
        if (typed_full_overwrite)
            fprintf(out, "xrt_array_new_typed_uninit(_xr_pm_count_%u, %s)", v->id, info.elem_name);
        else if (have_info)
            fprintf(out, "xrt_array_new_typed(_xr_pm_count_%u, %s)", v->id, info.elem_name);
        else
            fprintf(out, "xrt_array_new(_xr_pm_count_%u)", v->id);
        fprintf(out, ";\n");
        fprintf(out, "        xrt_array_t *_xr_pm_out_%u = (xrt_array_t*)_xr_pm_result_%u.ptr;\n",
                v->id, v->id);
        if (typed_full_overwrite)
            fprintf(out, "        _xr_pm_out_%u->length = _xr_pm_count_%u;\n", v->id, v->id);
    }

    char result_value_name[64];
    char start_name[64];
    snprintf(result_value_name, sizeof(result_value_name), "_xr_pm_result_%u", v->id);
    snprintf(start_name, sizeof(start_name), "_xr_pm_start_%u", v->id);
    xicgen_emit_par_map_scoped_closure(ctx, out, f, v, body, v->args[3], prefix, result_value_name,
                                       start_name);

    char out_ptr_name[64];
    char iter_name[64];
    char idx_name[64];
    char closure_name[64];
    char state_name[64];
    snprintf(out_ptr_name, sizeof(out_ptr_name), "_xr_pm_out_%u", v->id);
    snprintf(iter_name, sizeof(iter_name), "_xr_pm_i_%u", v->id);
    snprintf(idx_name, sizeof(idx_name), "_xr_pm_idx_%u", v->id);
    snprintf(closure_name, sizeof(closure_name), "_xr_pm_closure_%u", v->id);
    snprintf(state_name, sizeof(state_name), "_xr_pm_state_%u", v->id);

    fprintf(out, "        if (_xr_pm_count_%u > 0) {\n", v->id);
    fprintf(out, "            if (_xr_pm_serial_max_%u) {\n", v->id);
    if (data->plan_state) {
        fprintf(out,
                "                XrValue %s = xrt_index_get(_xr_pm_states_%u, "
                "XR_FROM_INT(0));\n",
                state_name, v->id);
    }
    fprintf(out, "                for (int64_t %s = _xr_pm_start_%u; ; %s++) {\n", iter_name, v->id,
            iter_name);
    fprintf(out, "            int64_t %s = %s - _xr_pm_start_%u;\n", idx_name, iter_name, v->id);
    xicgen_par_map_emit_store(ctx, out, f, v, body, prefix, iter_name, "0", closure_name,
                              data->plan_state ? state_name : NULL, out_ptr_name, idx_name,
                              native_result, have_info ? &info : NULL);
    fprintf(out, "                    if (%s == _xr_pm_end_%u) break;\n", iter_name, v->id);
    fprintf(out, "                }\n");
    if (callback_mode == XICGEN_PAR_CALLBACK_SERIAL) {
        fprintf(out, "            } else {\n");
        xicgen_par_map_emit_serial_exclusive(ctx, out, f, v, body, prefix, iter_name, idx_name,
                                             closure_name, state_name, out_ptr_name, native_result,
                                             have_info ? &info : NULL);
        fprintf(out, "            }\n");
    } else if (data->plan_state) {
        fprintf(out,
                "            } else if (!xr_parallel_for_range_state_i64(%s, _xr_pm_start_%u, "
                "_xr_pm_end_excl_%u, _xr_pm_workers_%u, (XrParallelRangeStateI64Fn)",
                aot_ctx_expr, v->id, v->id, v->id);
        xicgen_emit_par_map_range_wrapper_name(ctx, out, f, v, prefix);
        fprintf(out, ", _xr_pm_closure_%u, _xr_pm_states_%u)) abort();\n", v->id, v->id);
    } else {
        fprintf(out,
                "            } else if (!xr_parallel_for_range_i64(%s, _xr_pm_start_%u, "
                "_xr_pm_end_excl_%u, _xr_pm_workers_%u, (XrParallelRangeI64Fn)",
                aot_ctx_expr, v->id, v->id, v->id);
        xicgen_emit_par_map_range_wrapper_name(ctx, out, f, v, prefix);
        fprintf(out, ", _xr_pm_closure_%u)) abort();\n", v->id);
    }
    fprintf(out, "        }\n");
    fprintf(out, "        ");
    if (into_result)
        fprintf(out, "XR_NULL_VAL");
    else if (storage_rep == XR_REP_PTR)
        fprintf(out, "_xr_pm_out_%u", v->id);
    else
        fprintf(out, "_xr_pm_result_%u", v->id);
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
                "[xi_cgen] ERROR: parallel.forEach range-body AOT currently requires an exclusive "
                "range: "
                "'%s'\n",
                body && body->name ? body->name : "?");
        fprintf(out, "    abort();\n");
        return;
    }
    uint16_t expected_params = data->plan_state ? 3 : (data->range_body ? 3 : 2);
    if (!body || body->nparams != expected_params) {
        ctx->error = true;
        fprintf(out, "    abort();\n");
        return;
    }
    bool abi_ok = cg_func_return_abi_rep(ctx, body) == XR_REP_VOID;
    if (data->plan_state) {
        abi_ok = abi_ok && cg_func_param_abi_rep(ctx, body, 1) == XR_REP_I64 &&
                 cg_func_param_abi_rep(ctx, body, 2) == XR_REP_I64;
    } else {
        abi_ok = abi_ok && cg_func_param_abi_rep(ctx, body, 0) == XR_REP_I64 &&
                 cg_func_param_abi_rep(ctx, body, 1) == XR_REP_I64 &&
                 (!data->range_body || cg_func_param_abi_rep(ctx, body, 2) == XR_REP_I64);
    }
    if (!abi_ok) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: parallel %s AOT body must use %s ABI: '%s'\n",
                data->plan_state ? "Plan.forEach" : (data->range_body ? "range" : "for"),
                data->plan_state
                    ? "void(state, int64, int64)"
                    : (data->range_body ? "void(int64, int64, int64)" : "void(int64, int64)"),
                body->name ? body->name : "?");
        fprintf(out, "    abort();\n");
        return;
    }
    if (cg_func_needs_aot_coro_ctx(ctx, body)) {
        ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: parallel.forEach AOT body cannot be suspendable yet: '%s'\n",
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
                "[xi_cgen] ERROR: parallel.forEach AOT body cannot throw or suspend yet: '%s' "
                "contains %s%s%s%s at v%u line %u\n",
                body->name ? body->name : "?", xi_op_name((XiOp) unsupported->op),
                detail ? " '" : "", detail ? detail : "", detail ? "'" : "", unsupported->id,
                unsupported->line);
        fprintf(out, "    abort();\n");
        return;
    }

    char iter_name[64];
    char plan_state_name[64];
    snprintf(iter_name, sizeof(iter_name), "_xr_par_i_%u", v->id);
    snprintf(plan_state_name, sizeof(plan_state_name), "_xr_par_state_%u", v->id);
    const char *aot_ctx_expr = xicgen_aot_context_expr(ctx, f);

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
    if (data->plan_state) {
        fprintf(out, "        XrValue _xr_par_states_%u = ", v->id);
        emit_value_as_rep_ctx(ctx, out, v->args[4], XR_REP_TAGGED);
        fprintf(out, ";\n");
    }
    bool scoped_closure = xicgen_par_for_uses_stack_callback_closure(v, v->args[3], body);
    char scoped_closure_name[64];
    scoped_closure_name[0] = '\0';
    if (scoped_closure) {
        snprintf(scoped_closure_name, sizeof(scoped_closure_name), "_xr_par_closure_%u", v->id);
        xicgen_emit_par_for_scoped_closure(ctx, out, f, v, body, v->args[3], prefix,
                                           data->plan_state);
    }
    if (data->inclusive_end) {
        fprintf(out, "        if (_xr_par_end_%u == INT64_MAX) {\n", v->id);
        if (data->range_body) {
            fprintf(out, "            abort();\n");
        } else if (data->plan_state) {
            fprintf(out,
                    "            XrValue _xr_par_state_%u = "
                    "xrt_index_get(_xr_par_states_%u, XR_FROM_INT(0));\n",
                    v->id, v->id);
            fprintf(out, "            if (_xr_par_start_%u <= _xr_par_end_%u) {\n", v->id, v->id);
            fprintf(out, "                for (int64_t %s = _xr_par_start_%u; ; %s++) {\n",
                    iter_name, v->id, iter_name);
            xicgen_emit_par_for_plan_state_call(ctx, out, f, body, v->args[3], prefix,
                                                plan_state_name, iter_name, "0",
                                                scoped_closure ? scoped_closure_name : NULL);
            fprintf(out, "                    if (%s == _xr_par_end_%u) break;\n", iter_name,
                    v->id);
            fprintf(out, "                }\n");
            fprintf(out, "            }\n");
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
        if (data->plan_state) {
            fprintf(out,
                    "            if (!xr_parallel_for_range_state_i64(%s, _xr_par_start_%u, "
                    "_xr_par_end_excl_%u, _xr_par_workers_%u, (XrParallelRangeStateI64Fn)",
                    aot_ctx_expr, v->id, v->id, v->id);
        } else {
            fprintf(out,
                    "            if (!xr_parallel_for_range_i64(%s, _xr_par_start_%u, "
                    "_xr_par_end_excl_%u, _xr_par_workers_%u, (XrParallelRangeI64Fn)",
                    aot_ctx_expr, v->id, v->id, v->id);
        }
        xicgen_emit_par_for_range_wrapper_name(ctx, out, f, v, prefix);
        fprintf(out, ", ");
        if (scoped_closure)
            fprintf(out, "%s", scoped_closure_name);
        else
            emit_call_hidden_closure(out, f, body, v->args[3]);
        if (data->plan_state)
            fprintf(out, ", _xr_par_states_%u", v->id);
        fprintf(out, ")) abort();\n");
        fprintf(out, "        }\n");
    } else {
        if (data->plan_state) {
            fprintf(out,
                    "        if (!xr_parallel_for_range_state_i64(%s, _xr_par_start_%u, "
                    "_xr_par_end_%u, _xr_par_workers_%u, (XrParallelRangeStateI64Fn)",
                    aot_ctx_expr, v->id, v->id, v->id);
        } else {
            fprintf(out,
                    "        if (!xr_parallel_for_range_i64(%s, _xr_par_start_%u, _xr_par_end_%u, "
                    "_xr_par_workers_%u, (XrParallelRangeI64Fn)",
                    aot_ctx_expr, v->id, v->id, v->id);
        }
        xicgen_emit_par_for_range_wrapper_name(ctx, out, f, v, prefix);
        fprintf(out, ", ");
        if (scoped_closure)
            fprintf(out, "%s", scoped_closure_name);
        else
            emit_call_hidden_closure(out, f, body, v->args[3]);
        if (data->plan_state)
            fprintf(out, ", _xr_par_states_%u", v->id);
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
        case XI_PAR_MAP:
            xicgen_par_map(ctx, out, f, v, prefix);
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
