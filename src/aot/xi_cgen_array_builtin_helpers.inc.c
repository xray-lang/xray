/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_array_builtin_helpers.inc.c - AOT Array and Array<byte> builtin emission
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
    if (!v || v->op != XI_CALL_BUILTIN || v->nargs < 1 || !v->args[0] ||
        !v->args[0]->type || !XR_TYPE_IS_INT(v->args[0]->type))
        return false;
    (void) ctx;
    (void) f;
    return v->array_intrinsic_kind == XI_ARRAY_INTRINSIC_WITH_CAPACITY ||
           v->array_intrinsic_kind == XI_ARRAY_INTRINSIC_FILLED_NEW;
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
    if (v && v->array_intrinsic_kind != XI_ARRAY_INTRINSIC_NONE) {
        XrCValueEmissionView emission = {0};
        uint32_t count_semantic = XR_SEMANTIC_INDEX_NONE;
        uint32_t fill_semantic = XR_SEMANTIC_INDEX_NONE;
        bool with_capacity =
            v->array_intrinsic_kind == XI_ARRAY_INTRINSIC_WITH_CAPACITY;
        bool filled = v->array_intrinsic_kind == XI_ARRAY_INTRINSIC_FILLED_NEW;
        uint8_t expected_materialization = with_capacity
            ? XR_C_VALUE_MATERIALIZATION_ARRAY_WITH_CAPACITY
            : XR_C_VALUE_MATERIALIZATION_ARRAY_FILLED_NEW;
        const char *expected_symbol = with_capacity
            ? "xrt_array_with_capacity_value"
            : "xrt_array_new_filled_value";
        const char *storage_symbol = NULL;
        uint8_t xi_storage = XR_TARGET_ARRAY_STORAGE_NONE;
        CgValueEmissionStatus status =
            cg_value_emission_view(ctx, f, v, &emission);
        bool count_identity = v->nargs > 0 && v->args[0] &&
            cg_value_semantic_id(ctx, f, v->args[0], &count_semantic);
        bool fill_identity = !filled ||
            (v->nargs > 1 && v->args[1] &&
             cg_value_semantic_id(ctx, f, v->args[1], &fill_semantic));
        switch (v->array_element_storage) {
            case XR_ELEM_I8: xi_storage = XR_TARGET_ARRAY_STORAGE_I8; break;
            case XR_ELEM_U8: xi_storage = XR_TARGET_ARRAY_STORAGE_U8; break;
            case XR_ELEM_I16: xi_storage = XR_TARGET_ARRAY_STORAGE_I16; break;
            case XR_ELEM_U16: xi_storage = XR_TARGET_ARRAY_STORAGE_U16; break;
            case XR_ELEM_I32: xi_storage = XR_TARGET_ARRAY_STORAGE_I32; break;
            case XR_ELEM_U32: xi_storage = XR_TARGET_ARRAY_STORAGE_U32; break;
            case XR_ELEM_I64: xi_storage = XR_TARGET_ARRAY_STORAGE_I64; break;
            case XR_ELEM_U64: xi_storage = XR_TARGET_ARRAY_STORAGE_U64; break;
            case XR_ELEM_F32: xi_storage = XR_TARGET_ARRAY_STORAGE_F32; break;
            case XR_ELEM_F64: xi_storage = XR_TARGET_ARRAY_STORAGE_F64; break;
            case XR_ELEM_BOOL: xi_storage = XR_TARGET_ARRAY_STORAGE_BOOL; break;
            case XR_ELEM_RUNE: xi_storage = XR_TARGET_ARRAY_STORAGE_RUNE; break;
            default: break;
        }
        switch (emission.recipe_discriminant) {
            case XR_TARGET_ARRAY_STORAGE_I8: storage_symbol = "XR_ELEM_I8"; break;
            case XR_TARGET_ARRAY_STORAGE_U8: storage_symbol = "XR_ELEM_U8"; break;
            case XR_TARGET_ARRAY_STORAGE_I16: storage_symbol = "XR_ELEM_I16"; break;
            case XR_TARGET_ARRAY_STORAGE_U16: storage_symbol = "XR_ELEM_U16"; break;
            case XR_TARGET_ARRAY_STORAGE_I32: storage_symbol = "XR_ELEM_I32"; break;
            case XR_TARGET_ARRAY_STORAGE_U32: storage_symbol = "XR_ELEM_U32"; break;
            case XR_TARGET_ARRAY_STORAGE_I64: storage_symbol = "XR_ELEM_I64"; break;
            case XR_TARGET_ARRAY_STORAGE_U64: storage_symbol = "XR_ELEM_U64"; break;
            case XR_TARGET_ARRAY_STORAGE_F32: storage_symbol = "XR_ELEM_F32"; break;
            case XR_TARGET_ARRAY_STORAGE_F64: storage_symbol = "XR_ELEM_F64"; break;
            case XR_TARGET_ARRAY_STORAGE_BOOL: storage_symbol = "XR_ELEM_BOOL"; break;
            case XR_TARGET_ARRAY_STORAGE_RUNE: storage_symbol = "XR_ELEM_RUNE"; break;
            default: break;
        }
        if ((!with_capacity && !filled) ||
            v->nargs != (with_capacity ? 1u : 2u) ||
            status != CG_VALUE_EMISSION_FOUND ||
            emission.rep != XR_C_VALUE_REP_TAGGED ||
            emission.target_register_kind != XR_MACHINE_REP_DYN_VALUE ||
            emission.target_memory_kind != XR_MACHINE_REP_DYN_VALUE ||
            emission.materialization != expected_materialization ||
            !emission.c_type || strcmp(emission.c_type, "XrValue") != 0 ||
            !emission.recipe_symbol ||
            strcmp(emission.recipe_symbol, expected_symbol) != 0 ||
            !storage_symbol || xi_storage != emission.recipe_discriminant ||
            !count_identity || !fill_identity ||
            emission.recipe_operand_value != count_semantic ||
            emission.recipe_argument_value !=
                (filled ? fill_semantic : UINT32_MAX) ||
            emission.recipe_argument_count != 0 ||
            emission.recipe_arguments != NULL ||
            emission.recipe_layout_id != 0 ||
            xi_value_allocation_storage_mode(v) != XR_OBJ_STORAGE_NORMAL) {
            (void) cg_value_emission_fail(
                ctx, "Array intrinsic C emission recipe is missing or stale");
            emit_codegen_abort_expr(out);
            return true;
        }
        fprintf(out, "%s(", emission.recipe_symbol);
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        if (filled) {
            fprintf(out, ", ");
            emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
        }
        fprintf(out, ", %s)", storage_symbol);
        return true;
    }
    if (!name)
        return false;
    if (strcmp(name, "array_copy_new") == 0) {
        XrRep storage_rep = xicgen_value_c_storage_rep(ctx, f, v);
        uint8_t storage_mode = xi_value_allocation_storage_mode(v);
        CgArrayElemInfo info;
        const char *elem_name =
            cg_array_elem_info_from_type_ctx(ctx, v->type, &info) ? info.elem_name : "XR_ELEM_ANY";
        if (storage_mode != XR_OBJ_STORAGE_NORMAL)
            fprintf(out, storage_rep == XR_REP_PTR ? "xrt_array_set_storage_ptr("
                                                   : "xrt_array_set_storage(");
        if (storage_rep == XR_REP_PTR)
            fprintf(out, "((xrt_array_t*)");
        fprintf(out, "xrt_array_new_copy_value(");
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", %s)", elem_name);
        if (storage_rep == XR_REP_PTR)
            fprintf(out, ".ptr)");
        if (storage_mode != XR_OBJ_STORAGE_NORMAL)
            fprintf(out, ", %u)", (unsigned) storage_mode);
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
            CgArrayElemInfo info;
            XrRep target_rep = xicgen_value_c_storage_rep(ctx, f, v);
            if (v->nargs >= 2 && (xicgen_value_c_storage_rep(ctx, f, v->args[0]) == XR_REP_PTR ||
                                  cg_array_value_storage_info(ctx, f, v->args[0], &info,
                                                              CG_ARRAY_STORAGE_MUTABLE))) {
                const char *suffix = emit_conversion_prefix(out, v->type, XR_REP_PTR, target_rep);
                fprintf(out, "({ xrt_array_t *_arr = ");
                emit_typed_array_ptr_expr(ctx, out, f, v->args[0], NULL);
                fprintf(out, "; _arr->length = 0; _arr; })");
                emit_conversion_suffix(out, suffix);
                return true;
            }
            const char *suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, target_rep);
            fprintf(out, "({ XrValue _arr = ");
            emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
            fprintf(out, "; ((xrt_array_t*)_arr.ptr)->length = 0; _arr; })");
            emit_conversion_suffix(out, suffix);
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
    (void) ctx;
    return false;
}
