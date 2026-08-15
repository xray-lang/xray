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
    if (!plan)
        ctx->error = true;
    return plan;
}

static XrRep cg_abi_slot_storage_rep(const XaotAbiSlot *slot) {
    return xaot_value_storage_rep(xaot_abi_slot_value_rep(slot));
}

static bool cg_abi_slot_is_aggregate(const XaotAbiSlot *slot) {
    XaotValueRep rep = xaot_abi_slot_value_rep(slot);
    return rep.kind == XAOT_VALUE_AGGREGATE;
}

static const XaotValuePlan *cg_value_plan_optional(XiCgenCtx *ctx, const XiValue *v) {
    if (!ctx || !v)
        return NULL;
    return xaot_bundle_find_value_plan(ctx->aot_bundle, v);
}

/* Frozen values covered by immutable C emission intentionally have no
 * XaotValuePlan. Only an unsupported verified value family or an exact backend
 * representation adapter may require a Xaot row. */
static const XaotValuePlan *cg_value_plan_require_legacy(XiCgenCtx *ctx,
                                                         const XiValue *v) {
    if (!ctx || !v)
        return NULL;

    XrCValueEmissionView emission = {0};
    CgValueEmissionStatus emission_status =
        cg_value_emission_view(ctx, NULL, v, &emission);
    if (emission_status == CG_VALUE_EMISSION_FOUND ||
        emission_status == CG_VALUE_EMISSION_ERROR)
        return NULL;
    if (emission_status == CG_VALUE_EMISSION_NOT_CONFIGURED) {
        (void) cg_value_emission_fail(ctx, "legacy value lookup has no CGen authority");
        return NULL;
    }

    const XaotValuePlan *plan = cg_value_plan_optional(ctx, v);
    if (!plan) {
        fprintf(stderr, "[xi_cgen] ERROR: required legacy Xaot row is missing for v%u\n",
                (unsigned) v->id);
        ctx->error = true;
    }
    return plan;
}

static XrRep cg_value_plan_storage_rep(XiCgenCtx *ctx, const XiValue *v) {
    XrCValueEmissionView emission = {0};
    CgValueEmissionStatus emission_status =
        cg_value_emission_view(ctx, NULL, v, &emission);
    XrRep storage_rep = XR_REP_VOID;
    if (emission_status == CG_VALUE_EMISSION_FOUND)
        return cg_value_emission_storage_rep(ctx, &emission, &storage_rep)
                   ? storage_rep
                   : XR_REP_VOID;
    if (emission_status == CG_VALUE_EMISSION_ERROR)
        return XR_REP_VOID;
    const XaotValuePlan *plan = cg_value_plan_require_legacy(ctx, v);
    return plan ? xaot_value_storage_rep(plan->rep) : XR_REP_VOID;
}

static bool cg_value_plan_is_aggregate(XiCgenCtx *ctx, const XiValue *v) {
    const XaotValuePlan *plan = cg_value_plan_require_legacy(ctx, v);
    return plan && plan->rep.kind == XAOT_VALUE_AGGREGATE;
}

static bool cg_value_plan_is_vector(XiCgenCtx *ctx, const XiValue *v) {
    const XaotValuePlan *plan = cg_value_plan_require_legacy(ctx, v);
    return plan && plan->rep.kind == XAOT_VALUE_VECTOR;
}

static bool cg_value_rep_is_adt_aggregate(XaotValueRep rep) {
    return rep.kind == XAOT_VALUE_AGGREGATE && (rep.flags & XAOT_VALUE_FLAG_ENUM) != 0;
}

static bool cg_value_rep_is_typed_adt_aggregate(XaotValueRep rep) {
    return cg_value_rep_is_adt_aggregate(rep) && rep.c_type &&
           strcmp(rep.c_type, "XrAotEnumAggregate") != 0;
}

static bool cg_value_rep_is_struct_aggregate(XaotValueRep rep) {
    return rep.kind == XAOT_VALUE_AGGREGATE && (rep.flags & XAOT_VALUE_FLAG_STRUCT) != 0;
}

static bool cg_value_rep_is_span_aggregate(XaotValueRep rep) {
    return rep.kind == XAOT_VALUE_AGGREGATE && (rep.flags & XAOT_VALUE_FLAG_SLICE) != 0;
}

static bool cg_type_is_byte_slice(const XrType *type) {
    return xr_type_is_u8_slice(type);
}

static bool cg_fixed_array_type_info(const XrType *type, uint8_t *native_out, uint32_t *count_out) {
    if (!type || type->kind != XR_KIND_FIXED_ARRAY || !type->fixed_array.element_type ||
        type->fixed_array.length < 0 ||
        (uint64_t) type->fixed_array.length > XR_ARRAY_REF_MAX_COUNT)
        return false;
    XrType *elem = type->fixed_array.element_type;
    int native = xr_type_kind_to_native(elem->kind, elem->scalar_rep);
    if (elem->is_nullable || native == XR_NATIVE_STRING || native < 0)
        native = XR_NATIVE_VALUE;
    if (!xaot_layout_c_type_for_native_type((uint8_t) native))
        return false;
    if (native_out)
        *native_out = (uint8_t) native;
    if (count_out)
        *count_out = (uint32_t) type->fixed_array.length;
    return true;
}

static const XrType *cg_func_param_type(const XiFunc *f, uint16_t arg_index) {
    if (!f || arg_index >= f->nparams || !f->params || !f->params[arg_index])
        return NULL;
    return f->params[arg_index]->type;
}

static bool cg_value_plan_is_adt_aggregate(XiCgenCtx *ctx, const XiValue *v) {
    const XaotValuePlan *plan = cg_value_plan_require_legacy(ctx, v);
    return plan && cg_value_rep_is_adt_aggregate(plan->rep);
}

static bool cg_value_plan_is_struct_aggregate(XiCgenCtx *ctx, const XiValue *v) {
    const XaotValuePlan *plan = cg_value_plan_require_legacy(ctx, v);
    return plan && cg_value_rep_is_struct_aggregate(plan->rep);
}

static bool cg_value_plan_is_span_aggregate(XiCgenCtx *ctx, const XiValue *v) {
    v = cg_unwrap_identity_value(v);
    XrCValueEmissionView emission = {0};
    CgValueEmissionStatus emission_status =
        cg_value_emission_view(ctx, NULL, v, &emission);
    if (emission_status == CG_VALUE_EMISSION_FOUND)
        return emission.rep == XR_C_VALUE_REP_VIEW && emission.c_type &&
               strcmp(emission.c_type, "xr_span_t") == 0;
    if (emission_status == CG_VALUE_EMISSION_ERROR ||
        emission_status == CG_VALUE_EMISSION_NOT_CONFIGURED)
        return false;
    const XaotValuePlan *plan = cg_value_plan_require_legacy(ctx, v);
    return plan && cg_value_rep_is_span_aggregate(plan->rep);
}

/* Defined in xi_cgen_array_helpers.inc.c, which is included later: boxes a span
 * through a typed borrowed xrt_array_t view carrying its element type, and
 * reports whether that view can be built at all. */
static bool emit_span_array_view_ptr_expr(XiCgenCtx *ctx, FILE *out, const XiValue *value);
static bool cg_span_value_has_elem_info(XiCgenCtx *ctx, const XiValue *value);
static void emit_value_as_rep_ctx(XiCgenCtx *ctx, FILE *out, const XiValue *v, XrRep target_rep);

/*
 * Emit v as a TAGGED value that carries full value semantics for display and
 * string conversion.
 *
 * A Slice's tagged form is xrt_span_to_value_ref: a bare data+length pair whose
 * element type lives only in the compile-time plan. Every runtime consumer that
 * has to interpret elements -- string(), string concatenation, the shared
 * formatter -- would therefore see an opaque reference and fall back to a
 * placeholder, while print() renders the elements correctly through a typed
 * borrowed view. Route the value-semantic consumers through that same view so
 * one shape feeds all of them. The Slice value ABI is untouched: this is a
 * borrowed view built at the use site, not a change to how slices are passed.
 */
static void emit_value_as_display_tagged(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    /* An erased element type leaves no view to build; the ordinary tagged form
     * is then the only thing we can emit. */
    if (ctx && v && cg_value_plan_is_span_aggregate(ctx, v) &&
        cg_span_value_has_elem_info(ctx, cg_unwrap_identity_value(v))) {
        fprintf(out, "xr_mkptr(");
        (void) emit_span_array_view_ptr_expr(ctx, out, cg_unwrap_identity_value(v));
        fprintf(out, ", XR_TAG_ARRAY)");
        return;
    }
    emit_value_as_rep_ctx(ctx, out, v, XR_REP_TAGGED);
}

static void emit_aggregate_zero_expr(FILE *out, XaotValueRep rep) {
    if (cg_value_rep_is_span_aggregate(rep)) {
        fprintf(out, "xrt_span_empty()");
        return;
    }
    if (cg_value_rep_is_struct_aggregate(rep)) {
        fprintf(out, "((%s){0})", rep.c_type ? rep.c_type : "XrValue");
        return;
    }
    if (cg_value_rep_is_typed_adt_aggregate(rep))
        fprintf(out, "%s_from_base(", rep.c_type);
    fprintf(out, "xrt_enum_aggregate_zero()");
    if (cg_value_rep_is_typed_adt_aggregate(rep))
        fprintf(out, ")");
}

static void emit_value_plan_zero_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    const XaotValuePlan *plan = cg_value_plan_require_legacy(ctx, v);
    if (plan && plan->rep.kind == XAOT_VALUE_AGGREGATE) {
        emit_aggregate_zero_expr(out, plan->rep);
        return;
    }
    if (plan && plan->rep.kind == XAOT_VALUE_VECTOR && plan->rep.c_type) {
        if (strcmp(plan->rep.c_type, "svuint8_t") == 0) {
            fprintf(out, "svdup_n_u8(0)");
            return;
        }
        if (strcmp(plan->rep.c_type, "svuint32_t") == 0) {
            fprintf(out, "svdup_n_u32(0)");
            return;
        }
        if (strcmp(plan->rep.c_type, "svuint64_t") == 0) {
            fprintf(out, "svdup_n_u64(0)");
            return;
        }
        fprintf(out, "((%s){0})", plan->rep.c_type);
        return;
    }
    fprintf(out, "XR_NULL_VAL");
}

static void emit_adt_aggregate_as_base_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    const XaotValuePlan *plan = cg_value_plan_require_legacy(ctx, v);
    if (plan && cg_value_rep_is_typed_adt_aggregate(plan->rep))
        fprintf(out, "%s_to_base(", plan->rep.c_type);
    emit_vref(out, v);
    if (plan && cg_value_rep_is_typed_adt_aggregate(plan->rep))
        fprintf(out, ")");
}

static void emit_adt_base_to_value_rep_prefix(FILE *out, XaotValueRep rep) {
    if (cg_value_rep_is_typed_adt_aggregate(rep))
        fprintf(out, "%s_from_base(", rep.c_type);
}

static void emit_adt_base_to_value_rep_suffix(FILE *out, XaotValueRep rep) {
    if (cg_value_rep_is_typed_adt_aggregate(rep))
        fprintf(out, ")");
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
    if (cg_class_func_is_native_constructor(ctx, f))
        return XR_REP_PTR;
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

/* FFI: a raw pointer (Ptr<T>/MutPtr<T>) is stored as a native pointer in AOT
 * code, while tagged/VM boundaries encode it as an address-width integer.
 * Returns the boundary C type ("void *" / "const void *") when `t` is a pointer, else NULL.
 * Pointee precision is unnecessary at the call boundary: deref/index sites cast
 * to the exact element type. */
static const char *cg_extern_ptr_boundary_c_type(const XrType *t) {
    return xaot_raw_pointer_c_type(t);
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
            return type->scalar_rep == XR_NATIVE_F32 ? "float" : "double";
        case XR_KIND_INT:
            switch (type->scalar_rep) {
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
                case XR_NATIVE_ISIZE:
                    return "ptrdiff_t";
                case XR_NATIVE_USIZE:
                    return "size_t";
                default:
                    return "int64_t";
            }
        case XR_KIND_POINTER:
            return xaot_raw_pointer_c_type(type);
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
        const XrType *pt = xr_type_function_param_type(fn_type, i);
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
        if (xi_func_param_passing_mode(f, i) != XR_PARAM_READ)
            return false;
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
        if (xi_func_param_passing_mode(f, i) != XR_PARAM_READ)
            return false;
        const XrType *actual = f->params && f->params[i] ? f->params[i]->type : NULL;
        const XrType *want = xr_type_function_param_type(expected, i);
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
            const XrType *pt = xr_type_function_param_type(fn_type, i);
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

static bool cg_require_backend_tree(XiCgenCtx *ctx, const XiFunc *f) {
    if (!ctx || !f)
        return false;
    if (f->stage != XI_STAGE_BACKEND) {
        fprintf(stderr, "[xi_cgen] ERROR: function '%s' is at stage %s; Backend required\n",
                f->name ? f->name : "<anonymous>", xi_stage_name(f->stage));
        ctx->error = true;
        return false;
    }
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i] && !cg_require_backend_tree(ctx, f->children[i]))
            return false;
    }
    return true;
}

static const char *cg_ptr_box_suffix_for_type(const XrType *type) {
    if (!type)
        return ", XR_TAG_PTR)";
    switch (type->kind) {
        case XR_KIND_ARRAY:
        case XR_KIND_SLICE:
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
        if (from_rep == XR_REP_I64 && type && type->kind == XR_KIND_RUNE) {
            fprintf(out, "XR_FROM_RUNE((uint32_t)");
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
            fprintf(out, type && type->kind == XR_KIND_RUNE ? "XR_TO_RUNE(" : "XR_TO_INT(");
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

static const XaotEnumPlan *cg_unit_enum_scalar_plan(XiCgenCtx *ctx, const XrType *type) {
    if (!ctx || ctx->freestanding_profile || !ctx->aot_bundle || !type)
        return NULL;
    const XaotEnumPlan *plan = xaot_bundle_find_enum_plan_for_type(ctx->aot_bundle, type);
    return plan && plan->enum_data && plan->members && plan->member_count > 0 &&
                   plan->max_payload == 0
               ? plan
               : NULL;
}

static bool cg_mark_enum_scalar_sidecar(XiCgenCtx *ctx, const XaotEnumPlan *plan,
                                        uint32_t *out_index) {
    if (!ctx || !ctx->aot_bundle || !plan || !ctx->aot_bundle->enum_plans ||
        plan < ctx->aot_bundle->enum_plans ||
        plan >= ctx->aot_bundle->enum_plans + ctx->aot_bundle->nenum_plans) {
        if (ctx)
            ctx->error = true;
        return false;
    }
    uint32_t index = (uint32_t) (plan - ctx->aot_bundle->enum_plans);
    if (index >= ctx->enum_scalar_sidecar_cap || !ctx->enum_scalar_sidecar_used) {
        ctx->error = true;
        return false;
    }
    ctx->enum_scalar_sidecar_used[index] = 1;
    if (out_index)
        *out_index = index;
    return true;
}

static void cg_reset_enum_scalar_sidecars(XiCgenCtx *ctx) {
    if (ctx && ctx->enum_scalar_sidecar_used && ctx->enum_scalar_sidecar_cap > 0)
        memset(ctx->enum_scalar_sidecar_used, 0, ctx->enum_scalar_sidecar_cap);
}

static void emit_enum_scalar_sidecar_defs(XiCgenCtx *ctx, FILE *out) {
    if (!ctx || !out || !ctx->aot_bundle || !ctx->enum_scalar_sidecar_used)
        return;
    const char *module_prefix =
        ctx->module && ctx->module->name && ctx->module->name[0] ? ctx->module->name : "mod";
    for (uint32_t index = 0; index < ctx->aot_bundle->nenum_plans; index++) {
        if (index >= ctx->enum_scalar_sidecar_cap || !ctx->enum_scalar_sidecar_used[index])
            continue;
        const XaotEnumPlan *plan = &ctx->aot_bundle->enum_plans[index];
        if (!plan->enum_data || !plan->members || plan->member_count == 0 || plan->max_payload != 0)
            continue;
        /* A scalar sidecar exists only when a concrete enum crosses a tagged
         * boundary.  That erased value may subsequently flow to generic
         * formatting, `.name`, or `toString()` outside the typed call site, so
         * its nominal name table is part of the boundary representation even
         * when the local descriptor-use bitmap did not request `.name`.
         * Enums that remain fully typed still emit no sidecar or name table. */
        fprintf(out, "static const char *const _xenum_scalar_names_%s_%u[%u] = {", module_prefix,
                (unsigned) index, (unsigned) plan->member_count);
        for (uint32_t i = 0; i < plan->member_count; i++) {
            if (i > 0)
                fprintf(out, ",");
            emit_c_string_literal(out, plan->members[i].name ? plan->members[i].name : "");
        }
        fprintf(out, "};\n");
        fprintf(out,
                "static const XrAotEnumScalarLayout _xenum_scalar_layout_%s_%u = "
                "{{XR_TENUM_SCALAR_LAYOUT, XR_OBJ_IMMORTAL, XR_RC_STICKY, 0, 0}, ",
                module_prefix, (unsigned) index);
        emit_c_string_literal(out, plan->enum_data->name ? plan->enum_data->name : "");
        fprintf(out, ", _xenum_scalar_names_%s_%u", module_prefix, (unsigned) index);
        fprintf(out, ", %u, %u};\n\n", (unsigned) plan->member_count, (unsigned) plan->layout_id);
    }
}

/* Context-aware scalar unit-enum conversion.  Hot typed code carries only an
 * ordinal.  If that value crosses a genuinely tagged boundary, point it at one
 * immutable layout sidecar and keep the ordinal in XrValue.ext; this preserves
 * enum identity/names without a per-case table of boxes or a heap allocation. */
static const char *emit_conversion_prefix_ctx(XiCgenCtx *ctx, FILE *out, const XrType *type,
                                              XrRep from_rep, XrRep to_rep) {
    const XaotEnumPlan *plan = cg_unit_enum_scalar_plan(ctx, type);
    if (!plan)
        return emit_conversion_prefix(out, type, from_rep, to_rep);
    if (from_rep == XR_REP_TAGGED && to_rep == XR_REP_I64) {
        fprintf(out, "XR_TO_INT(xrt_enum_box_ordinal(");
        return "))";
    }
    if (from_rep == XR_REP_I64 && to_rep == XR_REP_TAGGED) {
        uint32_t index = 0;
        if (!cg_mark_enum_scalar_sidecar(ctx, plan, &index))
            return emit_conversion_prefix(out, type, from_rep, to_rep);
        const char *module_prefix =
            ctx->module && ctx->module->name && ctx->module->name[0] ? ctx->module->name : "mod";
        fprintf(out, "xrt_enum_scalar_box(&_xenum_scalar_layout_%s_%u, (", module_prefix,
                (unsigned) index);
        return "))";
    }
    return emit_conversion_prefix(out, type, from_rep, to_rep);
}

static void emit_conversion_suffix(FILE *out, const char *suffix) {
    if (suffix)
        fprintf(out, "%s", suffix);
}

/* Convert a source expression (rep from_rep) to value v's storage rep.
 * When from_rep is TAGGED and v is a typed ADT (enum) aggregate, a dynamically
 * read XrValue (getprop, index load, map/json get) is a boxed enum that must be
 * unpacked and rebuilt via <Enum>_from_base(xrt_value_to_enum_aggregate(...));
 * emit_conversion_prefix only knows XrRep and would leave the boxed XrValue
 * assigned straight into the enum struct. All other cases fall through to the
 * plain conversion. */
static const char *emit_load_conversion_prefix(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                               XrRep from_rep) {
    const XaotValuePlan *plan = cg_value_plan_require_legacy(ctx, v);
    if (from_rep == XR_REP_TAGGED && plan && cg_value_rep_is_typed_adt_aggregate(plan->rep) &&
        plan->rep.c_type) {
        fprintf(out, "%s_from_base(xrt_value_to_enum_aggregate(", plan->rep.c_type);
        return "))";
    }
    return emit_conversion_prefix_ctx(ctx, out, v->type, from_rep,
                                      cg_value_plan_storage_rep(ctx, v));
}

static const char *emit_tagged_to_value_storage_prefix(XiCgenCtx *ctx, FILE *out,
                                                       const XiValue *v) {
    return emit_load_conversion_prefix(ctx, out, v, XR_REP_TAGGED);
}

static XaotValueRep cg_func_param_abi_value_rep(XiCgenCtx *ctx, const XiFunc *f,
                                                uint16_t param_idx) {
    const XaotFuncPlan *plan = cg_func_plan(ctx, f);
    if (!plan || param_idx >= plan->abi.nparams || !plan->abi.params)
        return xaot_value_rep_for_type(cg_func_param_type(f, param_idx));
    return xaot_abi_slot_value_rep(&plan->abi.params[param_idx]);
}

static void emit_boxed_value_as_func_param_abi(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                               uint16_t param_idx, const char *boxed_expr) {
    const XrType *param_type = cg_func_param_type(f, param_idx);
    const XaotFuncPlan *func_plan = cg_func_plan(ctx, f);
    const XaotAbiSlot *slot =
        func_plan && func_plan->abi.params && param_idx < func_plan->abi.nparams
            ? &func_plan->abi.params[param_idx]
            : NULL;
    XaotValueRep param_value_rep = cg_func_param_abi_value_rep(ctx, f, param_idx);
    if (cg_value_rep_is_span_aggregate(param_value_rep)) {
        fprintf(out, "xrt_span_from_value_ref(%s)", boxed_expr ? boxed_expr : "XR_NULL_VAL");
        return;
    }
    if (slot && (slot->flags & XAOT_ABI_SLOT_BORROWED_PLACE) != 0 &&
        slot->pointee_rep.kind == XAOT_VALUE_AGGREGATE &&
        cg_value_rep_is_struct_aggregate(slot->pointee_rep)) {
        fprintf(out, "(%s *)(*(XrValue *)(uintptr_t)XR_TO_INT(%s)).ptr",
                slot->pointee_rep.c_type ? slot->pointee_rep.c_type : "void",
                boxed_expr ? boxed_expr : "XR_NULL_VAL");
        return;
    }

    XrRep param_rep = cg_func_param_abi_rep(ctx, f, param_idx);
    const char *param_suffix =
        emit_conversion_prefix_ctx(ctx, out, param_type, XR_REP_TAGGED, param_rep);
    fprintf(out, "%s", boxed_expr ? boxed_expr : "XR_NULL_VAL");
    emit_conversion_suffix(out, param_suffix);
}

static bool cg_value_box_inner_native_rep(XiCgenCtx *ctx, const XiValue *v, XrRep *out_rep) {
    if (!v || v->op != XI_BOX || v->nargs < 1 || !v->args[0])
        return false;

    const XiFunc *owner = (v->block) ? v->block->func : NULL;
    if (ctx && owner && cg_func_needs_aot_coro_ctx(ctx, owner))
        return false;

    const XiValue *inner = v->args[0];
    const XaotValuePlan *inner_plan = ctx ? cg_value_plan_require_legacy(ctx, inner) : NULL;
    if (inner_plan && inner_plan->rep.kind == XAOT_VALUE_AGGREGATE)
        return false;

    XrRep inner_rep = ctx ? cg_value_plan_storage_rep(ctx, inner)
                          : (inner_plan ? xaot_value_storage_rep(inner_plan->rep) : cg_rep(inner));
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
    const char *conv_suffix = emit_conversion_prefix_ctx(ctx, out, v->type, from_rep, target_rep);
    const XiFunc *vf = v->block ? v->block->func : NULL;
    const char *prefix = (ctx && ctx->module && ctx->module->name) ? ctx->module->name : NULL;
    emit_value_rhs(ctx, out, vf, v, prefix);
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static XrRep cg_emitted_value_storage_rep(XiCgenCtx *ctx, const XiValue *v,
                                          const XaotValuePlan *plan) {
    XrRep rep = ctx ? cg_value_plan_storage_rep(ctx, v)
                    : (plan ? xaot_value_storage_rep(plan->rep) : cg_rep(v));
    const XiFunc *owner = v && v->block ? v->block->func : NULL;
    if (!ctx || !v || v->op != XI_PARAM || !owner || !cg_func_needs_aot_coro_ctx(ctx, owner))
        return rep;

    uint32_t param_count = owner->nparams + (owner->is_vararg ? 1u : 0u);
    if (v->aux_int < 0 || (uint64_t) v->aux_int >= param_count)
        return rep;
    if (v->aux_int == 0 && cg_class_func_uses_native_receiver(ctx, owner))
        return XR_REP_PTR;
    /* Coroutine factories accept XrValue at the stable boundary, then
     * emit_coro_frame_init converts each parameter into the prepared frame
     * representation.  Every later read must describe that physical slot,
     * not the pre-plan Xi representation: returning cg_rep(v) here made a
     * scalar frame field look tagged and emitted XR_TO_INT(int64_t), while a
     * native receiver looked tagged and emitted `.ptr` on void*. */
    return rep;
}

static void emit_unit_materialized_as_rep(FILE *out, XrRep target_rep) {
    switch (target_rep) {
        case XR_REP_F64:
            fprintf(out, "0.0");
            return;
        case XR_REP_PTR:
        case XR_REP_RAWPTR:
            fprintf(out, "(void*)0");
            return;
        case XR_REP_VOID:
            fprintf(out, "((void)0)");
            return;
        case XR_REP_I64:
            fprintf(out, "INT64_C(0)");
            return;
        case XR_REP_TAGGED:
        default:
            fprintf(out, "XR_NULL_VAL");
            return;
    }
}

static void emit_value_as_rep(FILE *out, const XiValue *v, XrRep target_rep) {
    if (cg_is_void_like(v)) {
        emit_unit_materialized_as_rep(out, target_rep);
        return;
    }
    XrRep from_rep = cg_rep(v);
    if (target_rep == XR_REP_TAGGED && (from_rep == XR_REP_PTR || from_rep == XR_REP_RAWPTR) && v &&
        v->type && v->type->kind == XR_KIND_FIXED_ARRAY) {
        uint8_t native = XR_NATIVE_VALUE;
        uint32_t count = 0;
        if (cg_fixed_array_type_info(v->type, &native, &count)) {
            fprintf(out, "xr_array_ref((void *)(");
            emit_vref(out, v);
            fprintf(out, "), %u, %u)", (unsigned) native, (unsigned) count);
            return;
        }
    }
    const char *conv_suffix = emit_conversion_prefix(out, v ? v->type : NULL, from_rep, target_rep);
    emit_vref(out, v);
    emit_conversion_suffix(out, conv_suffix);
}

static void emit_value_as_rep_ctx(XiCgenCtx *ctx, FILE *out, const XiValue *v, XrRep target_rep) {
    XrRep inner_rep;
    if (cg_is_void_like(v)) {
        emit_unit_materialized_as_rep(out, target_rep);
        return;
    }
    const XiFunc *literal_func = v && v->block ? v->block->func : NULL;
    const XiValue *literal = v;
    while (literal && literal->nargs >= 1 && literal_func && literal_func->phi_coalesce &&
           literal->id < literal_func->phi_coalesce_count &&
           literal_func->phi_coalesce[literal->id] != literal->id &&
           (xi_copy_is_identity_alias(literal) || xi_op_is_identity_forward(literal->op)))
        literal = literal->args[0];
    if (literal && literal != v && literal->op == XI_CONST && literal->type &&
        (literal->type->kind == XR_KIND_INT || literal->type->kind == XR_KIND_BOOL ||
         literal->type->kind == XR_KIND_RUNE || literal->type->kind == XR_KIND_FLOAT ||
         literal->type->kind == XR_KIND_NULL)) {
        const XaotValuePlan *literal_plan = cg_value_plan_require_legacy(ctx, literal);
        XrRep from_rep = cg_value_plan_storage_rep(ctx, literal);
        if (emit_const_value_as_rep_expr(ctx, out, literal, from_rep, target_rep))
            return;
    }
    if (target_rep != XR_REP_TAGGED && cg_value_box_inner_native_rep(ctx, v, &inner_rep)) {
        const XiValue *inner = v->args[0];
        if (emit_const_value_as_rep_expr(ctx, out, inner, inner_rep, target_rep))
            return;
        const char *conv_suffix =
            emit_conversion_prefix_ctx(ctx, out, inner->type, inner_rep, target_rep);
        emit_vref(out, inner);
        emit_conversion_suffix(out, conv_suffix);
        return;
    }
    const XaotValuePlan *plan = (ctx && v) ? cg_value_plan_require_legacy(ctx, v) : NULL;
    if (v && v->op == XI_CONST) {
        XrRep from_rep = ctx ? cg_value_plan_storage_rep(ctx, v)
                             : (plan ? xaot_value_storage_rep(plan->rep) : cg_rep(v));
        if (v->type && v->aux_kind == XI_AUX_KIND_ENUM_NAMESPACE) {
            const char *conv_suffix =
                emit_conversion_prefix_ctx(ctx, out, v->type, from_rep, target_rep);
            emit_vref(out, v);
            emit_conversion_suffix(out, conv_suffix);
            return;
        }
        emit_const_value_as_rep_expr(ctx, out, v, from_rep, target_rep);
        return;
    }
    if (plan && plan->rep.kind == XAOT_VALUE_AGGREGATE) {
        if (target_rep == XR_REP_TAGGED && cg_value_rep_is_span_aggregate(plan->rep)) {
            fprintf(out, "xrt_span_to_value_ref((xr_span_t *)&");
            emit_vref(out, v);
            fprintf(out, ")");
            return;
        }
        if (target_rep == XR_REP_TAGGED && cg_value_rep_is_adt_aggregate(plan->rep)) {
            fprintf(out, "xrt_enum_aggregate_box(");
            emit_adt_aggregate_as_base_expr(ctx, out, v);
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
                    "[xi_cgen] ERROR: cannot convert aggregate v%u (%s in %s, kind=%d, "
                    "flags=0x%x, c_type=%s, semantic_kind=%d) to storage rep %d\n",
                    v ? v->id : 0, v ? xi_op_name((XiOp) v->op) : "?",
                    vf && vf->name ? vf->name : "?", plan ? (int) plan->rep.kind : -1,
                    plan ? (unsigned) plan->rep.flags : 0,
                    plan && plan->rep.c_type ? plan->rep.c_type : "?",
                    v && v->type ? (int) v->type->kind : -1, (int) target_rep);
            ctx->error = true;
        }
        emit_codegen_abort_expr(out);
        return;
    }
    XrRep from_rep = cg_emitted_value_storage_rep(ctx, v, plan);
    if (target_rep == XR_REP_TAGGED && (from_rep == XR_REP_PTR || from_rep == XR_REP_RAWPTR) && v &&
        v->type && v->type->kind == XR_KIND_FIXED_ARRAY) {
        uint8_t native = XR_NATIVE_VALUE;
        uint32_t count = 0;
        if (cg_fixed_array_type_info(v->type, &native, &count)) {
            fprintf(out, "xr_array_ref((void *)(");
            emit_vref(out, v);
            fprintf(out, "), %u, %u)", (unsigned) native, (unsigned) count);
            return;
        }
    }
    const char *conv_suffix =
        emit_conversion_prefix_ctx(ctx, out, v ? v->type : NULL, from_rep, target_rep);
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
    call_plan = cg_value_plan_require_legacy(ctx, call);
    if (call_plan && (target_plan->abi.ret.rep.kind == XAOT_VALUE_AGGREGATE ||
                      call_plan->rep.kind == XAOT_VALUE_AGGREGATE)) {
        XaotValueRep target_rep = xaot_abi_slot_value_rep(&target_plan->abi.ret);
        if (target_rep.kind != XAOT_VALUE_AGGREGATE &&
            xaot_value_storage_rep(target_rep) == XR_REP_TAGGED &&
            cg_value_rep_is_adt_aggregate(call_plan->rep)) {
            if (cg_value_rep_is_typed_adt_aggregate(call_plan->rep)) {
                fprintf(out, "%s_from_base(xrt_enum_aggregate_take_from_boxed(",
                        call_plan->rep.c_type);
                return "))";
            }
            fprintf(out, "xrt_enum_aggregate_take_from_boxed(");
            return ")";
        }
        if (target_rep.kind == XAOT_VALUE_AGGREGATE &&
            call_plan->rep.kind != XAOT_VALUE_AGGREGATE &&
            xaot_value_storage_rep(call_plan->rep) == XR_REP_TAGGED &&
            cg_value_rep_is_adt_aggregate(target_rep)) {
            fprintf(out, "xrt_enum_aggregate_box(");
            if (cg_value_rep_is_typed_adt_aggregate(target_rep)) {
                fprintf(out, "%s_to_base(", target_rep.c_type);
                return "))";
            }
            return ")";
        }
        if (!xaot_value_reps_equal(target_rep, call_plan->rep)) {
            fprintf(stderr,
                    "[xi_cgen] ERROR: aggregate direct-call return ABI mismatch at v%u "
                    "target=%s target_type=%s call_type=%s target_flags=0x%x call_flags=0x%x "
                    "receiver_op=%s\n",
                    call ? call->id : 0, target && target->name ? target->name : "?",
                    target_rep.c_type ? target_rep.c_type : "?",
                    call_plan->rep.c_type ? call_plan->rep.c_type : "?",
                    (unsigned) target_rep.flags, (unsigned) call_plan->rep.flags,
                    call && call->nargs > 0 && call->args[0] ? xi_op_name(call->args[0]->op) : "?");
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
    const XrType *ret_type =
        (target && target->return_type) ? target->return_type : (call ? call->type : NULL);
    return emit_conversion_prefix_ctx(ctx, out, ret_type, actual_rep, result_rep);
}

static bool emit_checked_tagged_direct_call_scalar_arg(XiCgenCtx *ctx, FILE *out,
                                                       const XiFunc *target, const XiValue *call,
                                                       uint16_t arg_index, const XiValue *arg,
                                                       XrRep from_rep, XrRep to_rep) {
    if (!ctx || !out || !target || from_rep != XR_REP_TAGGED || to_rep != XR_REP_I64)
        return false;
    const XrType *param_type = cg_func_param_type(target, arg_index);
    const char *helper = NULL;
    if (param_type && param_type->kind == XR_KIND_INT) {
        helper = "xrt_expect_int_arg";
    } else if (param_type && param_type->kind == XR_KIND_BOOL) {
        helper = "xrt_expect_bool_arg";
    } else if (param_type && param_type->kind == XR_KIND_RUNE) {
        helper = "xrt_expect_rune_arg";
    } else {
        return false;
    }

    (void) call;
    (void) arg_index;
    fprintf(out, "%s(", helper);
    emit_value_as_rep_ctx(ctx, out, arg, XR_REP_TAGGED);
    fprintf(out, ")");
    return true;
}

static bool emit_cfn_value_rawptr(XiCgenCtx *ctx, FILE *out, const XiFunc *current,
                                  const XrType *expected_type, const XiValue *value);

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
        fprintf(stderr,
                "[xi_cgen] ERROR: AOT direct-call argument ABI mismatch in %s at v%u "
                "target=%s arg=%u call_nargs=%u target_nparams=%u abi_nparams=%u\n",
                f && f->name ? f->name : "?", call ? call->id : 0,
                target && target->name ? target->name : "?", (unsigned) arg_index,
                call ? (unsigned) call->nargs : 0u, target ? (unsigned) target->nparams : 0u,
                target_plan ? (unsigned) target_plan->abi.nparams : 0u);
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return;
    }

    slot = &target_plan->abi.params[arg_index];
    slot_rep = xaot_abi_slot_value_rep(slot);
    XrCCallArgumentEmissionView direct_array_ref_argument = {0};
    if (cg_direct_local_array_ref_argument_emission(
            ctx, f, call, arg_index, arg, &direct_array_ref_argument)) {
        const char *slot_c_type =
            cg_func_param_abi_c_type(ctx, target, arg_index);
        if (xaot_value_storage_rep(slot_rep) != XR_REP_RAWPTR ||
            !slot_c_type ||
            strcmp(slot_c_type, direct_array_ref_argument.c_type) != 0) {
            fprintf(stderr,
                    "[xi_cgen] ERROR: direct-local Array ref emission row "
                    "disagrees with callee ABI at v%u arg %u\n",
                    call ? call->id : 0, (unsigned) arg_index);
            ctx->error = true;
            emit_codegen_abort_expr(out);
            return;
        }
        fprintf(out, "(%s)(", direct_array_ref_argument.c_type);
        emit_vref(out, arg);
        fprintf(out, ")");
        return;
    }
    if (ctx->error) {
        emit_codegen_abort_expr(out);
        return;
    }
    XrCValueEmissionView arg_emission = {0};
    CgValueEmissionStatus arg_emission_status =
        cg_value_emission_view(ctx, f, arg, &arg_emission);
    XaotValueRep frozen_arg_rep = {0};
    bool frozen_arg = arg_emission_status == CG_VALUE_EMISSION_FOUND &&
                      cg_value_emission_xaot_rep(ctx, &arg_emission,
                                                 &frozen_arg_rep.rep);
    if (frozen_arg) {
        const XaotRepInfo *info = xaot_rep_info(frozen_arg_rep.rep);
        frozen_arg_rep.kind = frozen_arg_rep.rep == XAOT_REP_SLICE
                                  ? XAOT_VALUE_AGGREGATE
                                  : frozen_arg_rep.rep == XAOT_REP_TAGGED
                                        ? XAOT_VALUE_TAGGED
                                        : frozen_arg_rep.rep == XAOT_REP_VOID
                                              ? XAOT_VALUE_VOID
                                              : XAOT_VALUE_SCALAR;
        frozen_arg_rep.type = arg->type;
        frozen_arg_rep.c_type = info ? info->c_type : NULL;
        if (frozen_arg_rep.rep == XAOT_REP_SLICE)
            frozen_arg_rep.flags = XAOT_VALUE_FLAG_SLICE;
        else if (arg_emission.target_register_kind ==
                     XR_MACHINE_REP_ENUM_ORDINAL &&
                 arg_emission.target_memory_kind ==
                     XR_MACHINE_REP_ENUM_ORDINAL &&
                 frozen_arg_rep.rep == XAOT_REP_I64)
            frozen_arg_rep.flags = XAOT_VALUE_FLAG_ENUM;
    }
    arg_plan = frozen_arg ? NULL : cg_value_plan_require_legacy(ctx, arg);
    const XrType *param_type = cg_func_param_type(target, arg_index);
    if (param_type && XR_TYPE_IS_C_FUNCTION(param_type)) {
        if (arg_plan && xaot_value_storage_rep(arg_plan->rep) == XR_REP_RAWPTR && arg->type &&
            XR_TYPE_IS_C_FUNCTION(arg->type)) {
            emit_value_as_rep_ctx(ctx, out, arg, XR_REP_RAWPTR);
        } else {
            emit_cfn_value_rawptr(ctx, out, f, param_type, arg);
        }
        return;
    }
    if (slot_rep.kind == XAOT_VALUE_AGGREGATE) {
        if (frozen_arg && xaot_value_reps_equal(frozen_arg_rep, slot_rep)) {
            emit_vref(out, arg);
            return;
        }
        if (arg_plan && xaot_value_reps_equal(arg_plan->rep, slot_rep)) {
            emit_vref(out, arg);
            return;
        }
        if (arg_plan && arg_plan->rep.kind != XAOT_VALUE_AGGREGATE &&
            cg_value_rep_is_span_aggregate(slot_rep) &&
            cg_type_is_byte_slice(cg_func_param_type(target, arg_index))) {
            fprintf(out, "xrt_byte_slice_from_value(");
            emit_value_as_rep_ctx(ctx, out, arg, XR_REP_TAGGED);
            fprintf(out, ", XR_ERROR_CORE_BYTE_SLICE_ARG_EXPECTS_MSG)");
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
            cg_value_rep_is_span_aggregate(arg_plan->rep)) {
            /* A function whose result or control flow requires the tagged ABI
             * still accepts a native Slice argument. Box the frame-local span
             * descriptor for the duration of the direct call; safe Slice
             * values cannot escape or be retained by the callee. */
            emit_value_as_rep_ctx(ctx, out, arg, XR_REP_TAGGED);
            return;
        }
        if (xaot_value_storage_rep(slot_rep) == XR_REP_TAGGED &&
            cg_value_rep_is_adt_aggregate(arg_plan->rep)) {
            /* READ keeps the caller's inline payload owners alive after the
             * call.  The temporary tagged box therefore needs independent
             * owners for every payload lane.  Move/ref boundaries have their
             * own transfer contract and keep the zero-retain box path. */
            fprintf(out, xi_func_param_passing_mode(target, arg_index) == XR_PARAM_READ
                             ? "xrt_enum_aggregate_box_from_borrowed("
                             : "xrt_enum_aggregate_box(");
            emit_adt_aggregate_as_base_expr(ctx, out, arg);
            fprintf(out, ")");
            return;
        }
        fprintf(stderr,
                "[xi_cgen] ERROR: cannot pass aggregate v%u from '%s' to non-tagged ABI "
                "slot %u of '%s' (arg_kind=%d param_kind=%d slot_kind=%u slot_flags=0x%x)\n",
                arg ? arg->id : 0, f && f->name ? f->name : "?", (unsigned) arg_index,
                target && target->name ? target->name : "?",
                arg && arg->type ? (int) arg->type->kind : -1,
                target && arg_index < target->nparams && target->params &&
                        target->params[arg_index] && target->params[arg_index]->type
                    ? (int) target->params[arg_index]->type->kind
                    : -1,
                (unsigned) slot_rep.kind, (unsigned) slot->flags);
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

    XrRep inner_rep;
    if (to_rep != XR_REP_TAGGED && !cg_func_needs_aot_coro_ctx(ctx, f) &&
        cg_value_box_inner_native_rep(ctx, arg, &inner_rep)) {
        const XiValue *inner = arg->args[0];
        conv_suffix = emit_conversion_prefix_ctx(ctx, out, inner->type, inner_rep, to_rep);
        emit_vref(out, inner);
        emit_conversion_suffix(out, conv_suffix);
        return;
    }
    if (emit_checked_tagged_direct_call_scalar_arg(ctx, out, target, call, arg_index, arg, from_rep,
                                                   to_rep))
        return;
    if ((from_rep == XR_REP_PTR || from_rep == XR_REP_RAWPTR) &&
        (to_rep == XR_REP_PTR || to_rep == XR_REP_RAWPTR)) {
        /* C implicitly converts void pointers at call boundaries; C++ does not.
         * Preserve the prepared pointer ABI explicitly so one generated source
         * is valid under both language front ends. */
        fprintf(out, "(%s)(", cg_func_param_abi_c_type(ctx, target, arg_index));
        emit_vref(out, arg);
        fprintf(out, ")");
        return;
    }
    conv_suffix = emit_conversion_prefix_ctx(ctx, out, arg ? arg->type : NULL, from_rep, to_rep);
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

static void cg_cfn_stub_targets_add(XiCgenCtx *ctx, const XiFunc *f) {
    if (!ctx || !f)
        return;
    for (int i = 0; i < ctx->ncfn_stub_targets; i++) {
        if (ctx->cfn_stub_targets[i] == f)
            return;
    }
    if (ctx->ncfn_stub_targets == ctx->cfn_stub_targets_cap) {
        int cap = ctx->cfn_stub_targets_cap ? ctx->cfn_stub_targets_cap * 2 : 8;
        const XiFunc **grown =
            (const XiFunc **) xr_realloc(ctx->cfn_stub_targets, (size_t) cap * sizeof(*grown));
        if (!grown)
            return; /* out of memory: the stub is simply not elided */
        ctx->cfn_stub_targets = grown;
        ctx->cfn_stub_targets_cap = cap;
    }
    ctx->cfn_stub_targets[ctx->ncfn_stub_targets++] = f;
}

/* Record every function handed to an FFI call as an argument. A function value
 * can only cross the C boundary as a callback, so the argument position alone
 * identifies the reference without consulting the extern declaration -- which
 * matters because the declaration lookup marks the decl used. */
static void cg_cfn_stub_targets_scan_func(XiCgenCtx *ctx, const XiFunc *f) {
    if (!f)
        return;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        const XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            const XiValue *v = blk->values[i];
            if (!v || v->op != XI_CALL || v->nargs < 1)
                continue;
            CgStaticFunctionCall target = cg_resolve_static_function_call(ctx, f, v->args[0]);
            if (!target.func || !target.func->is_extern)
                continue;
            for (uint16_t a = 1; a < v->nargs; a++) {
                CgStaticFunctionCall cb = cg_resolve_static_function_call(ctx, f, v->args[a]);
                if (cb.func && !cb.func->is_extern)
                    cg_cfn_stub_targets_add(ctx, cb.func);
            }
        }
    }
    for (uint16_t c = 0; c < f->nchildren; c++)
        cg_cfn_stub_targets_scan_func(ctx, f->children[c]);
}

/* Is this function's C-ABI `_cfn` stub actually referenced?
 *
 * cg_func_can_have_cfn_stub answers whether a stub is POSSIBLE; emitting one
 * for every top-level noncapturing function makes a static wrapper nobody
 * calls, which is a -Wunused-function error under the freestanding profile.
 * The stub exists for exactly one consumer -- emit_cfn_callback_arg -- so the
 * question is whether any FFI call passes this function as an argument. */
static bool cg_func_needs_cfn_stub(XiCgenCtx *ctx, const XiFunc *f) {
    if (!ctx || !cg_func_can_have_cfn_stub(ctx, f))
        return false;
    if (!ctx->cfn_stub_targets_built) {
        ctx->cfn_stub_targets_built = true;
        if (ctx->all_modules) {
            for (int m = 0; m < ctx->all_nmodules; m++) {
                if (ctx->all_modules[m])
                    cg_cfn_stub_targets_scan_func(ctx, ctx->all_modules[m]->init);
            }
        } else if (ctx->module) {
            cg_cfn_stub_targets_scan_func(ctx, ctx->module->init);
        }
    }
    for (int i = 0; i < ctx->ncfn_stub_targets; i++) {
        if (ctx->cfn_stub_targets[i] == f)
            return true;
    }
    return false;
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

/* Emit a first-class CFn value as a bare function-pointer address, for storing into
 * a raw-pointer array element / struct field / any RAWPTR slot. Internal CFn values
 * use the function's NATIVE entry (Xray ABI: hidden `xrt_closure_t *_cl` first param,
 * passed NULL for a noncapturing function). Using the native entry — rather than the
 * C-ABI `_cfn` stub — lets a tail-position indirect call `return f(...)` be a musttail
 * tail jump, because the caller's own native entry shares that exact signature (the
 * threaded-interpreter topology). The `_cfn` stub remains for the FFI boundary only.
 * The target must be a top-level noncapturing function with an exact CFn signature;
 * otherwise fail closed. */
static bool emit_cfn_value_rawptr(XiCgenCtx *ctx, FILE *out, const XiFunc *current,
                                  const XrType *expected_type, const XiValue *value) {
    CgStaticFunctionCall cb = cg_resolve_static_function_call(ctx, current, value);
    if (!cb.func || cb.is_class_constructor || cb.func->is_extern ||
        !cg_func_can_have_cfn_stub(ctx, cb.func) ||
        (expected_type && !cg_cfn_xray_func_matches_expected(cb.func, expected_type))) {
        fprintf(stderr,
                "[xi_cgen] ERROR: unsupported first-class CFn value at v%u; expected a top-level "
                "noncapturing function with an exact CFn signature\n",
                value ? value->id : 0);
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return false;
    }
    fprintf(out, "(void *)");
    emit_fname(ctx, out, cb.prefix, cb.func);
    return true;
}
