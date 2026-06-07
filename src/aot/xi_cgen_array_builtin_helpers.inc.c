/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_array_builtin_helpers.inc.c - AOT Array and Bytes builtin emission
 */

static bool emit_array_bytes_builtin_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                          const char *name) {
    if (!name)
        return false;
    if (strcmp(name, "array_with_capacity") == 0) {
        CgArrayElemInfo info;
        const char *elem_name =
            cg_array_elem_info_from_type(v->type, &info) ? info.elem_name : "XR_ELEM_ANY";
        fprintf(out, "xrt_array_with_capacity_value(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", %s)", elem_name);
        return true;
    }
    if (strcmp(name, "array_filled_new") == 0) {
        CgArrayElemInfo info;
        const char *elem_name =
            cg_array_elem_info_from_type(v->type, &info) ? info.elem_name : "XR_ELEM_ANY";
        fprintf(out, "xrt_array_new_filled_value(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ", %s)", elem_name);
        return true;
    }
    if (strcmp(name, "array_reserve") == 0) {
        fprintf(out, "xrt_array_reserve_value(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ")");
        return true;
    }
    if (strcmp(name, "array_resize") == 0) {
        fprintf(out, "xrt_array_resize_value(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[2], XR_REP_TAGGED);
        fprintf(out, ")");
        return true;
    }
    if (strcmp(name, "bytes_load_u32_le") == 0 || strcmp(name, "bytes_load_u64_le") == 0) {
        bool want_i64 = cg_rep(v) == XR_REP_I64;
        if (want_i64)
            fprintf(out, "XR_TO_INT(");
        fprintf(out, "%s(",
                strcmp(name, "bytes_load_u32_le") == 0 ? "xrt_bytes_load_u32_le"
                                                       : "xrt_bytes_load_u64_le");
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
