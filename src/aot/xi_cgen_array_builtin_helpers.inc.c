/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_array_builtin_helpers.inc.c - AOT Array and Bytes builtin emission
 */

static XrRep xicgen_value_c_storage_rep(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v);

static bool cg_array_const_zero_value_relaxed(const XiValue *v) {
    v = cg_unwrap_identity_value(v);
    return v && v->op == XI_CONST && v->aux_int == 0;
}

static bool cg_array_builtin_resize_zero_is_trusted(XiCgenCtx *ctx, const XiFunc *f,
                                                    const XiValue *call) {
    const XiValue *v = cg_unwrap_identity_value(call);
    if (!v || v->op != XI_CALL_BUILTIN || !v->aux)
        return false;
    const char *name = (const char *) v->aux;
    if (strcmp(name, "array_resize") != 0 || v->nargs < 2 ||
        !cg_array_const_zero_value_relaxed(v->args[1]))
        return false;
    (void) ctx;
    (void) f;
    return true;
}

static bool cg_array_builtin_fresh_storage_is_trusted(XiCgenCtx *ctx, const XiFunc *f,
                                                      const XiValue *call) {
    const XiValue *v = cg_unwrap_identity_value(call);
    if (!v || v->op != XI_CALL_BUILTIN || !v->aux || v->nargs < 1 || !v->args[0] ||
        !v->args[0]->type || !XR_TYPE_IS_INT(v->args[0]->type))
        return false;
    const char *name = (const char *) v->aux;
    (void) ctx;
    (void) f;
    return strcmp(name, "array_with_capacity") == 0 || strcmp(name, "array_filled_new") == 0;
}

static bool cg_array_builtin_call_is_trusted_nothrow(XiCgenCtx *ctx, const XiFunc *f,
                                                     const XiValue *call) {
    if (cg_array_builtin_resize_zero_is_trusted(ctx, f, call))
        return true;
    if (cg_array_builtin_fresh_storage_is_trusted(ctx, f, call))
        return true;
    return false;
}

static bool cg_array_builtin_err_check_after_trusted_nothrow(XiCgenCtx *ctx, const XiFunc *f,
                                                             const XiValue *check) {
    if (!check || check->op != XI_ERR_CHECK || cg_value_type_is_bool(check))
        return false;
    return cg_array_builtin_call_is_trusted_nothrow(ctx, f,
                                                    cg_class_native_prev_error_source_value(check));
}

static bool emit_array_bytes_builtin_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                          const XiValue *v, const char *name) {
    if (!name)
        return false;
    if (strcmp(name, "array_with_capacity") == 0) {
        CgArrayElemInfo info;
        const char *elem_name =
            cg_array_elem_info_from_type_ctx(ctx, v->type, &info) ? info.elem_name : "XR_ELEM_ANY";
        if (xicgen_value_c_storage_rep(ctx, f, v) == XR_REP_PTR) {
            fprintf(out, "((xrt_array_t*)xrt_array_with_capacity_value(");
            emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
            fprintf(out, ", %s).ptr)", elem_name);
            return true;
        }
        fprintf(out, "xrt_array_with_capacity_value(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", %s)", elem_name);
        return true;
    }
    if (strcmp(name, "array_filled_new") == 0) {
        CgArrayElemInfo info;
        const char *elem_name =
            cg_array_elem_info_from_type_ctx(ctx, v->type, &info) ? info.elem_name : "XR_ELEM_ANY";
        fprintf(out, "xrt_array_new_filled_value(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ", %s)", elem_name);
        return true;
    }
    if (strcmp(name, "array_clear") == 0) {
        fprintf(out, "xrt_array_clear_value(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ")");
        return true;
    }
    if (strcmp(name, "array_reserve") == 0) {
        CgArrayElemInfo info;
        XrRep target_rep = xicgen_value_c_storage_rep(ctx, f, v);
        if (v->nargs >= 2 && xicgen_value_c_storage_rep(ctx, f, v->args[0]) == XR_REP_PTR) {
            const char *suffix = emit_conversion_prefix(out, v->type, XR_REP_PTR, target_rep);
            fprintf(out, "xrt_array_reserve_trusted_raw(");
            emit_typed_array_ptr_expr(ctx, out, f, v->args[0], NULL);
            fprintf(out, ", ");
            emit_value_as_rep(out, v->args[1], XR_REP_I64);
            fprintf(out, ")");
            emit_conversion_suffix(out, suffix);
            return true;
        }
        if (v->nargs >= 2 &&
            cg_array_value_storage_info(ctx, f, v->args[0], &info, CG_ARRAY_STORAGE_MUTABLE)) {
            bool boxed = target_rep == XR_REP_TAGGED;
            if (boxed)
                fprintf(out, "xr_mkptr(");
            fprintf(out, "xrt_array_reserve_trusted_raw(");
            emit_typed_array_ptr_expr(ctx, out, f, v->args[0], NULL);
            fprintf(out, ", ");
            emit_value_as_rep(out, v->args[1], XR_REP_I64);
            fprintf(out, ")");
            if (boxed)
                fprintf(out, ", XR_TAG_ARRAY)");
            return true;
        }
        const char *suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, target_rep);
        fprintf(out, "xrt_array_reserve_value(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ")");
        emit_conversion_suffix(out, suffix);
        return true;
    }
    if (strcmp(name, "array_resize") == 0) {
        if (cg_array_builtin_resize_zero_is_trusted(ctx, f, v)) {
            fprintf(out, "({ XrValue _arr = ");
            emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
            fprintf(out, "; ((xrt_array_t*)_arr.ptr)->length = 0; _arr; })");
            return true;
        }
        fprintf(out, "xrt_array_resize_value(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[2], XR_REP_TAGGED);
        fprintf(out, ")");
        return true;
    }
    if (strcmp(name, "bytes_load_u16_le") == 0 || strcmp(name, "bytes_load_u32_le") == 0 ||
        strcmp(name, "bytes_load_u64_le") == 0) {
        bool want_i64 = cg_rep(v) == XR_REP_I64;
        if (want_i64)
            fprintf(out, "XR_TO_INT(");
        const char *helper = "xrt_bytes_load_u64_le";
        if (strcmp(name, "bytes_load_u16_le") == 0)
            helper = "xrt_bytes_load_u16_le";
        else if (strcmp(name, "bytes_load_u32_le") == 0)
            helper = "xrt_bytes_load_u32_le";
        fprintf(out, "%s(", helper);
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ")");
        if (want_i64)
            fprintf(out, ")");
        return true;
    }
    if (strcmp(name, "bytes_copy_within") == 0 || strcmp(name, "bytes_repeat_from") == 0) {
        fprintf(out, "%s(",
                strcmp(name, "bytes_copy_within") == 0 ? "xrt_bytes_copy_within_value"
                                                       : "xrt_bytes_repeat_from_value");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[2], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[3], XR_REP_TAGGED);
        fprintf(out, ")");
        return true;
    }
    if (strcmp(name, "bytes_copy_from") == 0) {
        fprintf(out, "xrt_bytes_copy_from_value(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[2], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[3], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[4], XR_REP_TAGGED);
        fprintf(out, ")");
        return true;
    }
    (void) ctx;
    return false;
}
