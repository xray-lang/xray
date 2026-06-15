/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_class_native_helpers.inc.c - AOT native class receiver emission
 */

static int cg_class_native_field_index(const XrStructLayout *layout, const char *field) {
    return cg_class_layout_field_index(layout, field);
}

static void emit_class_native_field_path(XiCgenCtx *ctx, FILE *out, const XiClassData *cd,
                                         uint16_t idx) {
    if (!cd || idx >= cd->inherited_field_count) {
        fprintf(out, "f%u", (unsigned) idx);
        return;
    }
    const XiClassData *super = cg_class_native_data_by_name(ctx, cd->super_name);
    fprintf(out, "base.");
    emit_class_native_field_path(ctx, out, super, idx);
}

static void emit_class_native_field_ref(XiCgenCtx *ctx, FILE *out, const XiClassData *cd,
                                        const char *object_expr, uint16_t idx) {
    fprintf(out, "(%s)->", object_expr);
    emit_class_native_field_path(ctx, out, cd, idx);
}

static const char *cg_class_native_ref_field_tag_name(uint8_t native_type) {
    return xaot_layout_ref_tag_name_for_native_type(native_type);
}

static const XiValue *cg_class_native_receiver_value(const XiCgenCtx *ctx, const XiFunc *f,
                                                     const XiValue *v);

static bool cg_class_native_receiver_ref_field(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v,
                                               uint8_t expected_native, CgClassNativeFunc *out_info,
                                               uint16_t *out_idx) {
    CgClassNativeFunc info = cg_class_native_func(ctx, f);
    if (out_info)
        memset(out_info, 0, sizeof(*out_info));
    if (out_idx)
        *out_idx = 0;
    v = cg_unwrap_identity_value(v);
    if (!info.layout || !v || v->op != XI_LOAD_FIELD || v->nargs < 1 ||
        !cg_class_native_receiver_value(ctx, f, v->args[0]))
        return false;
    int idx = cg_class_native_field_index(info.layout, (const char *) v->aux);
    const XrStructFieldLayout *field = cg_struct_field(info.layout, idx);
    if (!field || field->native_type != expected_native)
        return false;
    if (out_info)
        *out_info = info;
    if (out_idx)
        *out_idx = (uint16_t) idx;
    return true;
}

static bool emit_class_native_receiver_ref_field_ptr_expr(XiCgenCtx *ctx, FILE *out,
                                                          const XiFunc *f, const XiValue *v,
                                                          uint8_t expected_native) {
    CgClassNativeFunc info;
    uint16_t idx = 0;
    if (!cg_class_native_receiver_ref_field(ctx, f, v, expected_native, &info, &idx))
        return false;
    emit_class_native_field_ref(ctx, out, info.class_data, "p0", idx);
    return true;
}

static bool emit_class_native_map_length_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                              const XiValue *v) {
    if (!v || v->op != XI_LOAD_FIELD || v->nargs < 1 || !v->aux)
        return false;
    const char *field = (const char *) v->aux;
    if (strcmp(field, "length") != 0 && strcmp(field, "size") != 0)
        return false;
    CgClassNativeFunc info;
    uint16_t idx = 0;
    if (!cg_class_native_receiver_ref_field(ctx, f, v->args[0], XR_NATIVE_MAP_REF, &info, &idx))
        return false;
    const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
    emit_class_native_field_ref(ctx, out, info.class_data, "p0", idx);
    fprintf(out, "->len");
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool emit_class_native_map_get_nullable_direct_expr(XiCgenCtx *ctx, FILE *out,
                                                           const XiValue *v,
                                                           const CgClassNativeFunc *info,
                                                           uint16_t field_idx,
                                                           const CgMapElemInfo *map_info) {
    if (!info || !map_info || cg_rep(v) != XR_REP_TAGGED)
        return false;
    const char *find_helper =
        map_info->key.rep == XR_REP_F64 ? "xrt_map_find_f64_typed" : "xrt_map_find_i64_typed";
    const char *value_helper = map_info->value.rep == XR_REP_F64 ? "xrt_map_get_f64_value_typed"
                                                                 : "xrt_map_get_i64_value_typed";
    fprintf(out, "({ xrt_map_t *_xrm = ");
    emit_class_native_field_ref(ctx, out, info->class_data, "p0", field_idx);
    fprintf(out, "; %s _xrk = ", ctype_str(map_info->key.rep));
    emit_value_as_rep(out, v->args[1], map_info->key.rep);
    fprintf(out, "; int64_t _xri = %s(_xrm, _xrk, %s, %s); _xri >= 0 ? ", find_helper,
            map_info->key.elem_name, map_info->value.elem_name);
    if (map_info->value.rep == XR_REP_F64) {
        fprintf(out, "XR_FROM_FLOAT(%s(_xrm, _xri, %s))", value_helper, map_info->value.elem_name);
    } else if (strcmp(map_info->value.elem_name, "XR_ELEM_BOOL") == 0) {
        fprintf(out, "XR_FROM_BOOL(%s(_xrm, _xri, %s) != 0)", value_helper,
                map_info->value.elem_name);
    } else {
        fprintf(out, "XR_FROM_INT(%s(_xrm, _xri, %s))", value_helper, map_info->value.elem_name);
    }
    fprintf(out, " : XR_NULL_VAL; })");
    return true;
}

static bool emit_class_native_map_get_present_direct_expr(XiCgenCtx *ctx, FILE *out,
                                                          const XiValue *v,
                                                          const CgClassNativeFunc *info,
                                                          uint16_t field_idx,
                                                          const CgMapElemInfo *map_info) {
    if (!info || !map_info || cg_rep(v) != map_info->value.rep)
        return false;
    const char *find_helper =
        map_info->key.rep == XR_REP_F64 ? "xrt_map_find_f64_typed" : "xrt_map_find_i64_typed";
    const char *value_helper = map_info->value.rep == XR_REP_F64 ? "xrt_map_get_f64_value_typed"
                                                                 : "xrt_map_get_i64_value_typed";
    fprintf(out, "({ xrt_map_t *_xrm = ");
    emit_class_native_field_ref(ctx, out, info->class_data, "p0", field_idx);
    fprintf(out, "; %s _xrk = ", ctype_str(map_info->key.rep));
    emit_value_as_rep(out, v->args[1], map_info->key.rep);
    fprintf(out, "; int64_t _xri = %s(_xrm, _xrk, %s, %s); %s(_xrm, _xri, %s); })", find_helper,
            map_info->key.elem_name, map_info->value.elem_name, value_helper,
            map_info->value.elem_name);
    return true;
}

static bool emit_class_native_map_method_call_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                   const XiValue *v) {
    if (!v || v->op != XI_CALL_METHOD || v->nargs < 1 || !v->aux)
        return false;
    const char *method = (const char *) v->aux;
    uint16_t nargs = (uint16_t) (v->nargs - 1);
    CgClassNativeFunc info;
    uint16_t idx = 0;
    if (!cg_class_native_receiver_ref_field(ctx, f, v->args[0], XR_NATIVE_MAP_REF, &info, &idx))
        return false;

    if (nargs == 0 && (strcmp(method, "length") == 0 || strcmp(method, "size") == 0)) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
        emit_class_native_field_ref(ctx, out, info.class_data, "p0", idx);
        fprintf(out, "->len");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }
    if (nargs == 1 && strcmp(method, "get") == 0) {
        CgMapElemInfo map_info;
        if (cg_map_type_direct_info_ctx(ctx, v->args[0]->type, &map_info) &&
            emit_class_native_map_get_present_direct_expr(ctx, out, v, &info, idx, &map_info))
            return true;
        if (cg_map_type_direct_info_ctx(ctx, v->args[0]->type, &map_info) &&
            emit_class_native_map_get_nullable_direct_expr(ctx, out, v, &info, idx, &map_info))
            return true;
        if (cg_map_type_direct_info_ctx(ctx, v->args[0]->type, &map_info) &&
            cg_rep(v) == map_info.value.rep) {
            const char *helper = cg_map_direct_get_helper(&map_info);
            if (helper) {
                const char *conv_suffix =
                    emit_conversion_prefix(out, v->type, map_info.value.rep, cg_rep(v));
                fprintf(out, "%s(", helper);
                emit_class_native_field_ref(ctx, out, info.class_data, "p0", idx);
                fprintf(out, ", ");
                emit_value_as_rep(out, v->args[1], map_info.key.rep);
                fprintf(out, ", %s, %s)", map_info.key.elem_name, map_info.value.elem_name);
                emit_conversion_suffix(out, conv_suffix);
                return true;
            }
        }
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        fprintf(out, "xrt_map_get(");
        emit_class_native_field_ref(ctx, out, info.class_data, "p0", idx);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }
    if (nargs == 1 && strcmp(method, "has") == 0) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
        CgMapElemInfo map_info;
        const char *helper = cg_map_type_direct_info_ctx(ctx, v->args[0]->type, &map_info)
                                 ? cg_map_direct_has_helper(&map_info)
                                 : NULL;
        if (helper) {
            fprintf(out, "(int64_t)%s(", helper);
            emit_class_native_field_ref(ctx, out, info.class_data, "p0", idx);
            fprintf(out, ", ");
            emit_value_as_rep(out, v->args[1], map_info.key.rep);
            fprintf(out, ", %s, %s)", map_info.key.elem_name, map_info.value.elem_name);
            emit_conversion_suffix(out, conv_suffix);
            return true;
        }
        fprintf(out, "(int64_t)xrt_map_has(");
        emit_class_native_field_ref(ctx, out, info.class_data, "p0", idx);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }
    if (nargs == 1 && strcmp(method, "delete") == 0) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
        CgMapElemInfo map_info;
        const char *helper = cg_map_type_direct_info_ctx(ctx, v->args[0]->type, &map_info)
                                 ? cg_map_direct_delete_helper(&map_info)
                                 : NULL;
        fprintf(out, "(int64_t)%s(", helper ? helper : "xrt_map_delete");
        emit_class_native_field_ref(ctx, out, info.class_data, "p0", idx);
        fprintf(out, ", ");
        if (helper) {
            emit_value_as_rep(out, v->args[1], map_info.key.rep);
            fprintf(out, ", %s, %s", map_info.key.elem_name, map_info.value.elem_name);
        } else {
            emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
        }
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }
    if (nargs == 2 && strcmp(method, "set") == 0) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        CgMapElemInfo map_info;
        const char *helper = cg_map_type_direct_info_ctx(ctx, v->args[0]->type, &map_info)
                                 ? cg_map_direct_set_helper(&map_info)
                                 : NULL;
        if (helper) {
            fprintf(out, "(%s(", helper);
            emit_class_native_field_ref(ctx, out, info.class_data, "p0", idx);
            fprintf(out, ", ");
            emit_value_as_rep(out, v->args[1], map_info.key.rep);
            fprintf(out, ", ");
            emit_value_as_rep(out, v->args[2], map_info.value.rep);
            fprintf(out, ", %s, %s), XR_NULL_VAL)", map_info.key.elem_name,
                    map_info.value.elem_name);
            emit_conversion_suffix(out, conv_suffix);
            return true;
        }
        fprintf(out, "(xrt_map_set(");
        emit_class_native_field_ref(ctx, out, info.class_data, "p0", idx);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[2], XR_REP_TAGGED);
        fprintf(out, "), XR_NULL_VAL)");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }
    return false;
}

static bool cg_class_native_map_method_call_is_direct(XiCgenCtx *ctx, const XiFunc *f,
                                                      const XiValue *v) {
    if (!v || v->op != XI_CALL_METHOD || v->nargs < 1 || !v->aux)
        return false;
    const char *method = (const char *) v->aux;
    uint16_t nargs = (uint16_t) (v->nargs - 1);
    if (!cg_class_native_receiver_ref_field(ctx, f, v->args[0], XR_NATIVE_MAP_REF, NULL, NULL))
        return false;
    if (nargs == 0)
        return strcmp(method, "length") == 0 || strcmp(method, "size") == 0;
    if (nargs == 1)
        return strcmp(method, "get") == 0 || strcmp(method, "has") == 0 ||
               strcmp(method, "delete") == 0;
    if (nargs == 2)
        return strcmp(method, "set") == 0;
    return false;
}

static bool emit_class_native_map_method_call_stmt(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                   const XiValue *v) {
    if (!v || v->uses != 0 || v->op != XI_CALL_METHOD || v->nargs != 3 || !v->aux ||
        strcmp((const char *) v->aux, "set") != 0)
        return false;
    CgClassNativeFunc info;
    uint16_t idx = 0;
    if (!cg_class_native_receiver_ref_field(ctx, f, v->args[0], XR_NATIVE_MAP_REF, &info, &idx))
        return false;
    CgMapElemInfo map_info;
    const char *helper = cg_map_type_direct_info_ctx(ctx, v->args[0]->type, &map_info)
                             ? cg_map_direct_set_helper(&map_info)
                             : NULL;
    if (helper) {
        fprintf(out, "    %s(", helper);
        emit_class_native_field_ref(ctx, out, info.class_data, "p0", idx);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[1], map_info.key.rep);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[2], map_info.value.rep);
        fprintf(out, ", %s, %s);\n", map_info.key.elem_name, map_info.value.elem_name);
        return true;
    }
    fprintf(out, "    xrt_map_set(");
    emit_class_native_field_ref(ctx, out, info.class_data, "p0", idx);
    fprintf(out, ", ");
    emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep(out, v->args[2], XR_REP_TAGGED);
    fprintf(out, ");\n");
    return true;
}

static bool cg_class_native_map_method_call_value_is_elided(XiCgenCtx *ctx, const XiFunc *f,
                                                            const XiValue *v) {
    return v && v->uses == 0 && v->op == XI_CALL_METHOD && v->nargs == 3 && v->aux &&
           strcmp((const char *) v->aux, "set") == 0 &&
           cg_class_native_receiver_ref_field(ctx, f, v->args[0], XR_NATIVE_MAP_REF, NULL, NULL);
}

static bool cg_class_native_map_field_value_is_elided(XiCgenCtx *ctx, const XiFunc *f,
                                                      const XiValue *target) {
    if (!ctx || !f || !target ||
        !cg_class_native_receiver_ref_field(ctx, f, target, XR_NATIVE_MAP_REF, NULL, NULL))
        return false;
    bool saw_use = false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t i = 0; i < phi->value.nargs; i++) {
                if (phi->value.args[i] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v || v == target)
                continue;
            for (uint16_t ai = 0; ai < v->nargs; ai++) {
                if (v->args[ai] != target)
                    continue;
                saw_use = true;
                if (ai == 0 && cg_class_native_map_method_call_is_direct(ctx, f, v))
                    continue;
                if (ai == 0 && v->op == XI_LOAD_FIELD && v->aux) {
                    const char *field = (const char *) v->aux;
                    if (strcmp(field, "length") == 0 || strcmp(field, "size") == 0)
                        continue;
                }
                return false;
            }
        }
    }
    return saw_use;
}

static bool emit_class_native_set_length_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                              const XiValue *v) {
    if (!v || v->op != XI_LOAD_FIELD || v->nargs < 1 || !v->aux)
        return false;
    const char *field = (const char *) v->aux;
    if (strcmp(field, "length") != 0 && strcmp(field, "size") != 0)
        return false;
    CgClassNativeFunc info;
    uint16_t idx = 0;
    if (!cg_class_native_receiver_ref_field(ctx, f, v->args[0], XR_NATIVE_SET_REF, &info, &idx))
        return false;
    const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
    emit_class_native_field_ref(ctx, out, info.class_data, "p0", idx);
    fprintf(out, "->len");
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool emit_class_native_set_method_call_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                   const XiValue *v) {
    if (!v || v->op != XI_CALL_METHOD || v->nargs < 1 || !v->aux)
        return false;
    const char *method = (const char *) v->aux;
    uint16_t nargs = (uint16_t) (v->nargs - 1);
    CgClassNativeFunc info;
    uint16_t idx = 0;
    if (!cg_class_native_receiver_ref_field(ctx, f, v->args[0], XR_NATIVE_SET_REF, &info, &idx))
        return false;

    if (nargs == 0 && (strcmp(method, "length") == 0 || strcmp(method, "size") == 0)) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
        emit_class_native_field_ref(ctx, out, info.class_data, "p0", idx);
        fprintf(out, "->len");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }
    if (nargs == 0 && strcmp(method, "clear") == 0) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        fprintf(out, "(xrt_set_clear(");
        emit_class_native_field_ref(ctx, out, info.class_data, "p0", idx);
        fprintf(out, "), XR_NULL_VAL)");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }
    if (nargs == 0 && strcmp(method, "values") == 0) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        fprintf(out, "xrt_set_values(");
        emit_class_native_field_ref(ctx, out, info.class_data, "p0", idx);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }
    if (nargs == 1 && strcmp(method, "add") == 0) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        CgSetElemInfo set_info;
        if (cg_set_type_direct_info_ctx(ctx, v->args[0]->type, &set_info)) {
            if (set_info.rep == XR_REP_I64) {
                fprintf(out, "(%s(",
                        strcmp(set_info.elem_name, "XR_ELEM_I64") == 0 ? "xrt_set_add_i64"
                                                                       : "xrt_set_add_i64_typed");
                emit_class_native_field_ref(ctx, out, info.class_data, "p0", idx);
                fprintf(out, ", ");
                emit_value_as_rep(out, v->args[1], XR_REP_I64);
                if (strcmp(set_info.elem_name, "XR_ELEM_I64") != 0)
                    fprintf(out, ", %s", set_info.elem_name);
                fprintf(out, "), XR_NULL_VAL)");
                emit_conversion_suffix(out, conv_suffix);
                return true;
            }
            if (set_info.rep == XR_REP_F64) {
                fprintf(out, "(xrt_set_add_f64_typed(");
                emit_class_native_field_ref(ctx, out, info.class_data, "p0", idx);
                fprintf(out, ", ");
                emit_value_as_rep(out, v->args[1], XR_REP_F64);
                fprintf(out, ", %s), XR_NULL_VAL)", set_info.elem_name);
                emit_conversion_suffix(out, conv_suffix);
                return true;
            }
        }
        fprintf(out, "(xrt_set_add(");
        emit_class_native_field_ref(ctx, out, info.class_data, "p0", idx);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
        fprintf(out, "), XR_NULL_VAL)");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }
    if (nargs == 1 && strcmp(method, "has") == 0) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
        CgSetElemInfo set_info;
        if (cg_set_type_direct_info_ctx(ctx, v->args[0]->type, &set_info)) {
            if (set_info.rep == XR_REP_I64) {
                fprintf(out, "(int64_t)%s(",
                        strcmp(set_info.elem_name, "XR_ELEM_I64") == 0 ? "xrt_set_has_i64"
                                                                       : "xrt_set_has_i64_typed");
                emit_class_native_field_ref(ctx, out, info.class_data, "p0", idx);
                fprintf(out, ", ");
                emit_value_as_rep(out, v->args[1], XR_REP_I64);
                if (strcmp(set_info.elem_name, "XR_ELEM_I64") != 0)
                    fprintf(out, ", %s", set_info.elem_name);
                fprintf(out, ")");
                emit_conversion_suffix(out, conv_suffix);
                return true;
            }
            if (set_info.rep == XR_REP_F64) {
                fprintf(out, "(int64_t)xrt_set_has_f64_typed(");
                emit_class_native_field_ref(ctx, out, info.class_data, "p0", idx);
                fprintf(out, ", ");
                emit_value_as_rep(out, v->args[1], XR_REP_F64);
                fprintf(out, ", %s)", set_info.elem_name);
                emit_conversion_suffix(out, conv_suffix);
                return true;
            }
        }
        fprintf(out, "(int64_t)xrt_set_has(");
        emit_class_native_field_ref(ctx, out, info.class_data, "p0", idx);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }
    if (nargs == 1 && strcmp(method, "delete") == 0) {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
        CgSetElemInfo set_info;
        if (cg_set_type_direct_info_ctx(ctx, v->args[0]->type, &set_info)) {
            if (set_info.rep == XR_REP_I64) {
                fprintf(out, "(int64_t)%s(",
                        strcmp(set_info.elem_name, "XR_ELEM_I64") == 0
                            ? "xrt_set_delete_i64"
                            : "xrt_set_delete_i64_typed");
                emit_class_native_field_ref(ctx, out, info.class_data, "p0", idx);
                fprintf(out, ", ");
                emit_value_as_rep(out, v->args[1], XR_REP_I64);
                if (strcmp(set_info.elem_name, "XR_ELEM_I64") != 0)
                    fprintf(out, ", %s", set_info.elem_name);
                fprintf(out, ")");
                emit_conversion_suffix(out, conv_suffix);
                return true;
            }
            if (set_info.rep == XR_REP_F64) {
                fprintf(out, "(int64_t)xrt_set_delete_f64_typed(");
                emit_class_native_field_ref(ctx, out, info.class_data, "p0", idx);
                fprintf(out, ", ");
                emit_value_as_rep(out, v->args[1], XR_REP_F64);
                fprintf(out, ", %s)", set_info.elem_name);
                emit_conversion_suffix(out, conv_suffix);
                return true;
            }
        }
        fprintf(out, "(int64_t)xrt_set_delete(");
        emit_class_native_field_ref(ctx, out, info.class_data, "p0", idx);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }
    return false;
}

static bool cg_class_native_set_method_call_is_direct(XiCgenCtx *ctx, const XiFunc *f,
                                                      const XiValue *v) {
    if (!v || v->op != XI_CALL_METHOD || v->nargs < 1 || !v->aux)
        return false;
    const char *method = (const char *) v->aux;
    uint16_t nargs = (uint16_t) (v->nargs - 1);
    if (!cg_class_native_receiver_ref_field(ctx, f, v->args[0], XR_NATIVE_SET_REF, NULL, NULL))
        return false;
    if (nargs == 0)
        return strcmp(method, "length") == 0 || strcmp(method, "size") == 0 ||
               strcmp(method, "clear") == 0 || strcmp(method, "values") == 0;
    if (nargs == 1)
        return strcmp(method, "add") == 0 || strcmp(method, "has") == 0 ||
               strcmp(method, "delete") == 0;
    return false;
}

static bool emit_class_native_set_method_call_stmt(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                   const XiValue *v) {
    if (!v || v->uses != 0 || v->op != XI_CALL_METHOD || !v->aux)
        return false;
    const char *method = (const char *) v->aux;
    uint16_t nargs = (uint16_t) (v->nargs - 1);
    if (!((nargs == 1 && strcmp(method, "add") == 0) ||
          (nargs == 0 && strcmp(method, "clear") == 0)))
        return false;
    CgClassNativeFunc info;
    uint16_t idx = 0;
    if (!cg_class_native_receiver_ref_field(ctx, f, v->args[0], XR_NATIVE_SET_REF, &info, &idx))
        return false;
    if (nargs == 1) {
        CgSetElemInfo set_info;
        if (cg_set_type_direct_info_ctx(ctx, v->args[0]->type, &set_info)) {
            if (set_info.rep == XR_REP_I64) {
                fprintf(out, "    (void)%s(",
                        strcmp(set_info.elem_name, "XR_ELEM_I64") == 0 ? "xrt_set_add_i64"
                                                                       : "xrt_set_add_i64_typed");
                emit_class_native_field_ref(ctx, out, info.class_data, "p0", idx);
                fprintf(out, ", ");
                emit_value_as_rep(out, v->args[1], XR_REP_I64);
                if (strcmp(set_info.elem_name, "XR_ELEM_I64") != 0)
                    fprintf(out, ", %s", set_info.elem_name);
                fprintf(out, ");\n");
                return true;
            }
            if (set_info.rep == XR_REP_F64) {
                fprintf(out, "    (void)xrt_set_add_f64_typed(");
                emit_class_native_field_ref(ctx, out, info.class_data, "p0", idx);
                fprintf(out, ", ");
                emit_value_as_rep(out, v->args[1], XR_REP_F64);
                fprintf(out, ", %s);\n", set_info.elem_name);
                return true;
            }
        }
        fprintf(out, "    (void)xrt_set_add(");
        emit_class_native_field_ref(ctx, out, info.class_data, "p0", idx);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ");\n");
    } else {
        fprintf(out, "    xrt_set_clear(");
        emit_class_native_field_ref(ctx, out, info.class_data, "p0", idx);
        fprintf(out, ");\n");
    }
    return true;
}

static bool cg_class_native_set_method_call_value_is_elided(XiCgenCtx *ctx, const XiFunc *f,
                                                            const XiValue *v) {
    if (!v || v->uses != 0 || v->op != XI_CALL_METHOD || !v->aux)
        return false;
    const char *method = (const char *) v->aux;
    uint16_t nargs = (uint16_t) (v->nargs - 1);
    return ((nargs == 1 && strcmp(method, "add") == 0) ||
            (nargs == 0 && strcmp(method, "clear") == 0)) &&
           cg_class_native_receiver_ref_field(ctx, f, v->args[0], XR_NATIVE_SET_REF, NULL, NULL);
}

static bool cg_class_native_set_field_value_is_elided(XiCgenCtx *ctx, const XiFunc *f,
                                                      const XiValue *target) {
    if (!ctx || !f || !target ||
        !cg_class_native_receiver_ref_field(ctx, f, target, XR_NATIVE_SET_REF, NULL, NULL))
        return false;
    bool saw_use = false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t i = 0; i < phi->value.nargs; i++) {
                if (phi->value.args[i] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v || v == target)
                continue;
            for (uint16_t ai = 0; ai < v->nargs; ai++) {
                if (v->args[ai] != target)
                    continue;
                saw_use = true;
                if (ai == 0 && cg_class_native_set_method_call_is_direct(ctx, f, v))
                    continue;
                if (ai == 0 && v->op == XI_LOAD_FIELD && v->aux) {
                    const char *field = (const char *) v->aux;
                    if (strcmp(field, "length") == 0 || strcmp(field, "size") == 0)
                        continue;
                }
                return false;
            }
        }
    }
    return saw_use;
}

static const XiValue *cg_class_native_unwrap_receiver_alias(const XiValue *v) {
    while (v && (v->op == XI_BOX || v->op == XI_UNBOX || v->op == XI_COPY || v->op == XI_MOVE) &&
           v->nargs >= 1)
        v = v->args[0];
    return v;
}

static const XiValue *cg_class_native_receiver_value_depth(const XiCgenCtx *ctx, const XiFunc *f,
                                                           const XiValue *v, uint8_t depth) {
    if (!cg_class_func_uses_native_receiver(ctx, f) || !v || depth > 8)
        return NULL;

    v = cg_class_native_unwrap_receiver_alias(v);
    if (v && v->op == XI_PARAM && v->aux_int == 0)
        return v;
    if (!v || v->op != XI_PHI || v->nargs == 0)
        return NULL;

    const XiValue *receiver = NULL;
    bool saw_receiver_source = false;
    for (uint16_t i = 0; i < v->nargs; i++) {
        const XiValue *arg = cg_class_native_unwrap_receiver_alias(v->args[i]);
        if (arg == v)
            continue;
        const XiValue *arg_receiver =
            cg_class_native_receiver_value_depth(ctx, f, arg, (uint8_t) (depth + 1));
        if (!arg_receiver)
            return NULL;
        if (receiver && receiver != arg_receiver)
            return NULL;
        receiver = arg_receiver;
        saw_receiver_source = true;
    }
    return saw_receiver_source ? receiver : NULL;
}

static const XiValue *cg_class_native_receiver_value(const XiCgenCtx *ctx, const XiFunc *f,
                                                     const XiValue *v) {
    return cg_class_native_receiver_value_depth(ctx, f, v, 0);
}

static bool cg_class_native_value_stmt_is_elided(const XiCgenCtx *ctx, const XiFunc *f,
                                                 const XiValue *v) {
    if (!v || !cg_class_func_uses_native_receiver(ctx, f))
        return false;
    if (v->op == XI_PARAM && v->aux_int == 0)
        return true;
    if ((v->op == XI_COPY || v->op == XI_MOVE) && v->nargs >= 1 &&
        cg_class_native_receiver_value(ctx, f, v->args[0]))
        return true;
    if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && v->nargs >= 1 &&
        cg_class_native_receiver_value(ctx, f, v->args[0]))
        return true;
    return false;
}

static bool emit_class_native_return_type(XiCgenCtx *ctx, FILE *out, const char *prefix,
                                          const XiFunc *f) {
    CgClassNativeFunc info = cg_class_native_func(ctx, f);
    if (!info.layout || !info.is_constructor)
        return false;
    emit_class_native_type_name(out, prefix, info.class_name);
    fprintf(out, " *");
    return true;
}

static void emit_class_native_param_decl(XiCgenCtx *ctx, FILE *out, const char *prefix,
                                         const XiFunc *f, uint16_t param_idx) {
    CgClassNativeFunc info = cg_class_native_func(ctx, f);
    if (info.layout && param_idx == 0) {
        emit_class_native_type_name(out, prefix, info.class_name);
        fprintf(out, " *p%u", (unsigned) param_idx);
        return;
    }
    fprintf(out, "%s p%u", cg_func_param_abi_c_type(ctx, f, param_idx), (unsigned) param_idx);
}

static void emit_class_native_field_box(XiCgenCtx *ctx, FILE *out, const XiClassData *cd,
                                        const XrStructLayout *layout, uint16_t idx,
                                        const char *object_expr) {
    const XrStructFieldLayout *field = cg_struct_field(layout, idx);
    if (!field) {
        fprintf(out, "XR_NULL_VAL");
        return;
    }
    const char *tag_name = cg_class_native_ref_field_tag_name(field->native_type);
    if (tag_name) {
        fprintf(out, "xr_mkptr(");
        emit_class_native_field_ref(ctx, out, cd, object_expr, idx);
        fprintf(out, ", %s)", tag_name);
        return;
    }
    switch (field->native_type) {
        case XR_NATIVE_F32:
        case XR_NATIVE_F64:
            fprintf(out, "XR_FROM_FLOAT(");
            emit_class_native_field_ref(ctx, out, cd, object_expr, idx);
            fprintf(out, ")");
            break;
        case XR_NATIVE_BOOL:
            fprintf(out, "XR_FROM_BOOL(");
            emit_class_native_field_ref(ctx, out, cd, object_expr, idx);
            fprintf(out, ")");
            break;
        default:
            fprintf(out, "XR_FROM_INT(");
            emit_class_native_field_ref(ctx, out, cd, object_expr, idx);
            fprintf(out, ")");
            break;
    }
}

static void emit_class_native_field_unbox(FILE *out, const XrStructLayout *layout, uint16_t idx) {
    const XrStructFieldLayout *field = cg_struct_field(layout, idx);
    const char *ctype = cg_struct_field_c_type(layout, idx);
    fprintf(out, "(%s)", ctype);
    switch (field ? field->native_type : XR_NATIVE_I64) {
        case XR_NATIVE_F32:
        case XR_NATIVE_F64:
            fprintf(out, "XR_TO_FLOAT");
            break;
        default:
            fprintf(out, "XR_TO_INT");
            break;
    }
}

static void emit_class_native_load_field_from_map(XiCgenCtx *ctx, FILE *out, const XiClassData *cd,
                                                  const XrStructLayout *layout, uint16_t idx,
                                                  const char *object_expr, const char *map_expr) {
    const char *name = layout && layout->field_names ? layout->field_names[idx] : NULL;
    const XrStructFieldLayout *field = cg_struct_field(layout, idx);
    fprintf(out, "    ");
    emit_class_native_field_ref(ctx, out, cd, object_expr, idx);
    fprintf(out, " = ");
    if (field && cg_class_native_ref_field_tag_name(field->native_type)) {
        fprintf(out, "(%s)(xrt_map_get((xrt_map_t*)%s.ptr, ", cg_struct_field_c_type(layout, idx),
                map_expr);
        cg_emit_str_value(ctx, out, name ? name : "?");
        fprintf(out, ")).ptr;\n");
        return;
    }
    emit_class_native_field_unbox(out, layout, idx);
    fprintf(out, "(xrt_map_get((xrt_map_t*)%s.ptr, ", map_expr);
    cg_emit_str_value(ctx, out, name ? name : "?");
    fprintf(out, "));\n");
}

static void emit_class_native_store_field_to_map(XiCgenCtx *ctx, FILE *out, const XiClassData *cd,
                                                 const XrStructLayout *layout, uint16_t idx,
                                                 const char *object_expr, const char *map_expr) {
    const char *name = layout && layout->field_names ? layout->field_names[idx] : NULL;
    fprintf(out, "    xrt_map_set((xrt_map_t*)%s.ptr, ", map_expr);
    cg_emit_str_value(ctx, out, name ? name : "?");
    fprintf(out, ", ");
    emit_class_native_field_box(ctx, out, cd, layout, idx, object_expr);
    fprintf(out, ");\n");
}

static bool emit_class_native_receiver_field_load_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                       const XiValue *v) {
    CgClassNativeFunc info = cg_class_native_func(ctx, f);
    if (!info.layout || !v || v->op != XI_LOAD_FIELD || v->nargs < 1 ||
        !cg_class_native_receiver_value(ctx, f, v->args[0]))
        return false;
    int idx = cg_class_native_field_index(info.layout, (const char *) v->aux);
    if (idx < 0)
        return false;
    const XrStructFieldLayout *field = cg_struct_field(info.layout, idx);
    const char *tag_name = field ? cg_class_native_ref_field_tag_name(field->native_type) : NULL;
    if (tag_name) {
        fprintf(out, "xr_mkptr(");
        emit_class_native_field_ref(ctx, out, info.class_data, "p0", (uint16_t) idx);
        fprintf(out, ", %s)", tag_name);
        return true;
    }
    const char *conv_suffix =
        emit_conversion_prefix(out, v->type, cg_struct_field_rep(info.layout, idx), cg_rep(v));
    emit_class_native_field_ref(ctx, out, info.class_data, "p0", (uint16_t) idx);
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool emit_class_native_receiver_field_store_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                        const XiValue *v) {
    CgClassNativeFunc info = cg_class_native_func(ctx, f);
    if (!info.layout || !v || v->op != XI_STORE_FIELD || v->nargs < 2 ||
        !cg_class_native_receiver_value(ctx, f, v->args[0]))
        return false;
    int idx = cg_class_native_field_index(info.layout, (const char *) v->aux);
    if (idx < 0)
        return false;
    const XrStructFieldLayout *field = cg_struct_field(info.layout, idx);
    fprintf(out, "(");
    emit_class_native_field_ref(ctx, out, info.class_data, "p0", (uint16_t) idx);
    if (field && cg_class_native_ref_field_tag_name(field->native_type)) {
        fprintf(out, " = (%s)(", cg_struct_field_c_type(info.layout, idx));
        emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ").ptr");
    } else {
        fprintf(out, " = (%s)", cg_struct_field_c_type(info.layout, idx));
        emit_value_as_rep(out, v->args[1], cg_struct_field_rep(info.layout, idx));
    }
    fprintf(out, ")");
    return true;
}

static bool emit_class_native_return_stmt(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                          const XiBlock *blk) {
    if (!cg_class_func_is_native_constructor(ctx, f) || !blk || !blk->control ||
        !cg_class_native_receiver_value(ctx, f, blk->control))
        return false;
    fprintf(out, "    return p0;\n");
    return true;
}

static void emit_class_native_boxed_adapter(XiCgenCtx *ctx, FILE *out, const char *prefix,
                                            const XiFunc *f) {
    CgClassNativeFunc info = cg_class_native_func(ctx, f);
    if (!info.layout)
        return;
    fprintf(out, "static XrValue ");
    emit_typed_abi_fname(ctx, out, prefix, f);
    fprintf(out, "(xrt_closure_t *_cl");
    for (uint16_t i = 0; i < f->nparams; i++)
        fprintf(out, ", XrValue p%u", i);
    fprintf(out, ") {\n");
    fprintf(out, "    ");
    emit_class_native_type_name(out, prefix, info.class_name);
    fprintf(out, " _recv;\n");
    fprintf(out, "    memset(&_recv, 0, sizeof(_recv));\n");
    if (!info.is_constructor) {
        for (uint16_t i = 0; i < info.layout->field_count; i++)
            emit_class_native_load_field_from_map(ctx, out, info.class_data, info.layout, i,
                                                  "&_recv", "p0");
    }
    XrRep ret_rep = cg_func_return_abi_rep(ctx, f);
    if (!info.is_constructor && ret_rep != XR_REP_TAGGED) {
        fprintf(out, "    %s _result = ", ctype_str(ret_rep));
    } else if (!info.is_constructor) {
        fprintf(out, "    XrValue _result = ");
    } else {
        fprintf(out, "    (void)");
    }
    emit_fname(ctx, out, prefix, f);
    fprintf(out, "(_cl, &_recv");
    for (uint16_t i = 1; i < f->nparams; i++) {
        fprintf(out, ", ");
        XrRep rep = cg_func_param_abi_rep(ctx, f, i);
        const XrType *param_type = f->params && f->params[i] ? f->params[i]->type : NULL;
        const char *param_suffix = emit_conversion_prefix(out, param_type, XR_REP_TAGGED, rep);
        fprintf(out, "p%u", (unsigned) i);
        emit_conversion_suffix(out, param_suffix);
    }
    fprintf(out, ");\n");
    for (uint16_t i = 0; i < info.layout->field_count; i++)
        emit_class_native_store_field_to_map(ctx, out, info.class_data, info.layout, i, "&_recv",
                                             "p0");
    if (info.is_constructor) {
        fprintf(out, "    return p0;\n");
    } else {
        fprintf(out, "    return ");
        const char *conv_suffix =
            emit_conversion_prefix(out, f->return_type, ret_rep, XR_REP_TAGGED);
        fprintf(out, "_result");
        emit_conversion_suffix(out, conv_suffix);
        fprintf(out, ";\n");
    }
    fprintf(out, "}\n\n");
}

static const XiClassData *cg_class_native_class_for_ctor_target(const XiCgenCtx *ctx,
                                                                const XiFunc *target) {
    if (!ctx || !target)
        return NULL;
    if (ctx->module) {
        for (uint16_t ci = 0; ci < ctx->module->nclasses; ci++) {
            const XiClassData *cd = ctx->module->classes[ci];
            if (cd && cd->instance_layout && cg_find_constructor(ctx->module->init, cd) == target)
                return cd;
        }
    }
    for (int i = 0; i < ctx->nimports; i++) {
        const CgImportEntry *imp = &ctx->imports[i];
        if (imp->target_func == target && imp->target_class && imp->target_class->instance_layout)
            return imp->target_class;
    }
    return NULL;
}

static const XiClassData *cg_class_native_ctor_call_data(XiCgenCtx *ctx, const XiFunc *f,
                                                         const XiValue *call,
                                                         const XiFunc **out_target,
                                                         const char **out_prefix) {
    if (out_target)
        *out_target = NULL;
    if (out_prefix)
        *out_prefix = NULL;
    if (!ctx || !call || call->op != XI_CALL || call->nargs < 1)
        return NULL;
    const XiValue *callee = cg_unwrap_identity_value(call->args[0]);
    CgStaticFunctionCall static_call = cg_resolve_static_function_call(ctx, f, callee);
    const XiFunc *target = static_call.func;
    const char *prefix = static_call.prefix;
    const XiClassData *cd = NULL;

    if (callee && callee->op == XI_GET_SHARED) {
        int slot = (int) callee->aux_int;
        if (slot >= 0 && slot < ctx->shared_cap)
            cd = ctx->shared_class[slot];
    } else if (callee && callee->op == XI_CLASS_CREATE && callee->aux) {
        cd = (const XiClassData *) callee->aux;
        target = cg_find_constructor(f, cd);
    } else if (callee && callee->op == XI_IMPORT_REF && callee->aux) {
        for (int i = 0; i < ctx->nimports; i++) {
            const CgImportEntry *imp = &ctx->imports[i];
            if (imp->target_func == target && imp->target_class) {
                cd = imp->target_class;
                prefix = imp->target_mod_name;
                break;
            }
        }
    }
    if (!cd && target)
        cd = cg_class_native_class_for_ctor_target(ctx, target);
    if (!cd || !cd->instance_layout || !target || !cg_class_func_is_native_constructor(ctx, target))
        return NULL;
    if (out_target)
        *out_target = target;
    if (out_prefix)
        *out_prefix = prefix;
    return cd;
}

static const XiValue *cg_class_native_trace_ctor_origin(XiCgenCtx *ctx, const XiFunc *f,
                                                        const XiValue *v, int depth) {
    if (!v || depth > 8)
        return NULL;
    while (v && (v->op == XI_COPY || v->op == XI_MOVE) && v->nargs >= 1) {
        if (++depth > 8)
            return NULL;
        v = v->args[0];
    }
    if (!v || v->op != XI_CALL)
        return NULL;
    return cg_class_native_ctor_call_data(ctx, f, v, NULL, NULL) ? v : NULL;
}

static bool cg_class_native_ctor_uses_safe(XiCgenCtx *ctx, const XiFunc *f, const XiValue *target,
                                           const XiValue *origin, int depth) {
    if (!ctx || !f || !target || !origin || depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t ai = 0; ai < phi->value.nargs; ai++) {
                if (phi->value.args[ai] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t ai = 0; ai < v->nargs; ai++) {
                if (v->args[ai] != target)
                    continue;
                if ((v->op == XI_CALL_METHOD || v->op == XI_CALL_METHOD_DIRECT) && ai == 0) {
                    const char *prefix = NULL;
                    const char *method = (const char *) v->aux;
                    const XiFunc *mfunc = NULL;
                    const char *recv_class = target->type && target->type->kind == XR_KIND_INSTANCE
                                                 ? target->type->instance.class_name
                                                 : NULL;
                    if (v->op == XI_CALL_METHOD_DIRECT)
                        mfunc =
                            cg_lookup_method_by_index(ctx, recv_class, (int) v->aux_int, &prefix);
                    else
                        mfunc = cg_lookup_method(ctx, method, recv_class, &prefix);
                    if (!cg_class_func_uses_native_receiver(ctx, mfunc))
                        return false;
                    continue;
                }
                if ((v->op == XI_COPY || v->op == XI_MOVE) && ai == 0) {
                    if (cg_class_native_trace_ctor_origin(ctx, f, v, depth + 1) != origin)
                        return false;
                    if (!cg_class_native_ctor_uses_safe(ctx, f, v, origin, depth + 1))
                        return false;
                    continue;
                }
                if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && ai == 0)
                    continue;
                return false;
            }
        }
    }
    return true;
}

static bool cg_class_native_ctor_can_inline(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    if (!ctx || !f || cg_has_exception_handling(f))
        return false;
    const XiValue *origin = cg_class_native_trace_ctor_origin(ctx, f, v, 0);
    return origin == v && cg_class_native_ctor_uses_safe(ctx, f, v, origin, 0);
}

static bool emit_class_native_ctor_value_stmt(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                              const char *prefix, const XiValue *v) {
    if (!cg_class_native_ctor_can_inline(ctx, f, v))
        return false;
    const XiFunc *target = NULL;
    const char *ctor_prefix = NULL;
    const XiClassData *cd = cg_class_native_ctor_call_data(ctx, f, v, &target, &ctor_prefix);
    if (!cd || !target)
        return false;
    fprintf(out, "    ");
    emit_class_native_type_name(out, ctor_prefix ? ctor_prefix : prefix, cd->class_name);
    fprintf(out, " _ci%u;\n", v->id);
    fprintf(out, "    memset(&_ci%u, 0, sizeof(_ci%u));\n", v->id, v->id);
    fprintf(out, "    ");
    emit_class_native_type_name(out, ctor_prefix ? ctor_prefix : prefix, cd->class_name);
    fprintf(out, " *");
    emit_vref(out, v);
    fprintf(out, " = &_ci%u;\n", v->id);
    fprintf(out, "    (void)");
    emit_fname(ctx, out, ctor_prefix ? ctor_prefix : prefix, target);
    fprintf(out, "(NULL, ");
    emit_vref(out, v);
    for (uint16_t i = 1; i < v->nargs; i++) {
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[i], cg_func_param_abi_rep(ctx, target, i));
    }
    fprintf(out, ");\n");
    return true;
}

static bool emit_class_native_constructor_boxed_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                     const char *prefix, const XiValue *v,
                                                     const XiFunc *target,
                                                     const char *ctor_prefix) {
    CgClassNativeFunc info = cg_class_native_func(ctx, target);
    if (!info.layout || !info.is_constructor)
        return false;
    int64_t cap = info.layout->field_count > 0 ? (int64_t) info.layout->field_count * 2 : 4;
    fprintf(out, "({ XrValue _inst = xrt_map_new(%" PRId64 "); ", cap);
    emit_typed_abi_fname(ctx, out, ctor_prefix ? ctor_prefix : prefix, target);
    fprintf(out, "(NULL, _inst");
    for (uint16_t a = 1; a < v->nargs; a++) {
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[a], XR_REP_TAGGED);
    }
    fprintf(out, "); _inst; })");
    (void) f;
    return true;
}

static void emit_class_shared_native_storage_name(const XiCgenCtx *ctx, FILE *out, int slot) {
    fprintf(out, "%s_native_%d", ctx && ctx->shared_name ? ctx->shared_name : "xrt_shared", slot);
}

static void emit_class_shared_native_export_storage_name(FILE *out,
                                                         const CgSharedNativeExport *exp) {
    fprintf(out, "xrt_shared_%s_native_%d", exp && exp->module_name ? exp->module_name : "mod",
            exp ? exp->slot : 0);
}

static int cg_class_shared_native_module_index(const XiCgenCtx *ctx, const XiModule *module) {
    if (!ctx || !module || !ctx->all_modules || ctx->all_nmodules <= 0)
        return -1;
    for (int i = 0; i < ctx->all_nmodules; i++) {
        if (ctx->all_modules[i] == module)
            return i;
    }
    return -1;
}

static bool cg_class_shared_native_slot_is_exported(const XiCgenCtx *ctx, int slot) {
    if (!ctx || !ctx->module || slot < 0)
        return false;
    for (uint16_t ei = 0; ctx->module->exports && ei < ctx->module->nexports; ei++) {
        if ((int) ctx->module->exports[ei].shared_slot == slot)
            return true;
    }
    return false;
}

static bool cg_class_shared_native_slot_active(const XiCgenCtx *ctx, int slot) {
    return ctx && slot >= 0 && slot < ctx->shared_cap &&
           ctx->shared_native_instances[slot].active &&
           ctx->shared_native_instances[slot].class_data &&
           ctx->shared_native_instances[slot].class_data->instance_layout;
}

static const CgSharedNativeExport *
cg_class_shared_native_export_for_module_slot(const XiCgenCtx *ctx, int module_index, int slot) {
    if (!ctx || module_index < 0 || slot < 0)
        return NULL;
    for (int i = 0; i < ctx->nshared_native_exports; i++) {
        const CgSharedNativeExport *exp = &ctx->shared_native_exports[i];
        if (exp->active && exp->module_index == module_index && exp->slot == slot &&
            exp->class_data && exp->class_data->instance_layout)
            return exp;
    }
    return NULL;
}

static const CgSharedNativeExport *
cg_class_shared_native_export_for_import_ref(const XiCgenCtx *ctx, const XiImportRef *ref) {
    if (!ctx || !ref)
        return NULL;
    if (ref->resolved_mod_index >= 0 && ref->resolved_shared_slot >= 0) {
        const CgSharedNativeExport *exp = cg_class_shared_native_export_for_module_slot(
            ctx, ref->resolved_mod_index, ref->resolved_shared_slot);
        if (exp)
            return exp;
    }
    for (int i = 0; i < ctx->nimports; i++) {
        const CgImportEntry *imp = &ctx->imports[i];
        if (!imp->target_mod_name || !imp->member_name || !ref->member_name || !ref->module_path ||
            !imp->module_path)
            continue;
        if (strcmp(imp->module_path, ref->module_path) != 0 ||
            strcmp(imp->member_name, ref->member_name) != 0)
            continue;
        for (int m = 0; m < ctx->all_nmodules; m++) {
            const XiModule *mod = ctx->all_modules ? ctx->all_modules[m] : NULL;
            if (mod && mod->name && strcmp(mod->name, imp->target_mod_name) == 0)
                return cg_class_shared_native_export_for_module_slot(ctx, m, imp->shared_slot);
        }
    }
    return NULL;
}

static bool cg_class_shared_native_slot_for_value_depth(const XiCgenCtx *ctx, const XiValue *v,
                                                        int *out_slot, uint8_t depth) {
    if (!ctx || !v || depth > 8)
        return false;
    while (v &&
           (v->op == XI_BOX || v->op == XI_UNBOX || v->op == XI_COPY || v->op == XI_MOVE ||
            v->op == XI_RETAIN || v->op == XI_RELEASE) &&
           v->nargs >= 1) {
        if (++depth > 8)
            return false;
        v = v->args[0];
    }
    if (!v)
        return false;
    if (v->op == XI_GET_SHARED) {
        int slot = (int) v->aux_int;
        if (!cg_class_shared_native_slot_active(ctx, slot))
            return false;
        if (out_slot)
            *out_slot = slot;
        return true;
    }
    if (v->op != XI_PHI || v->nargs == 0)
        return false;
    bool saw_arg = false;
    int slot = -1;
    for (uint16_t i = 0; i < v->nargs; i++) {
        const XiValue *arg = v->args[i];
        if (!arg || arg == v)
            continue;
        int arg_slot = -1;
        if (!cg_class_shared_native_slot_for_value_depth(ctx, arg, &arg_slot,
                                                         (uint8_t) (depth + 1)))
            return false;
        if (!saw_arg) {
            saw_arg = true;
            slot = arg_slot;
            continue;
        }
        if (slot != arg_slot)
            return false;
    }
    if (!saw_arg || !cg_class_shared_native_slot_active(ctx, slot))
        return false;
    if (out_slot)
        *out_slot = slot;
    return true;
}

static bool cg_class_shared_native_slot_for_value(const XiCgenCtx *ctx, const XiValue *v,
                                                  int *out_slot) {
    return cg_class_shared_native_slot_for_value_depth(ctx, v, out_slot, 0);
}

static bool cg_class_shared_native_exports_match(const CgSharedNativeExport *a,
                                                 const CgSharedNativeExport *b) {
    return a && b && a->module_index == b->module_index && a->slot == b->slot;
}

static const CgSharedNativeExport *
cg_class_imported_shared_native_export_for_local_slot(const XiCgenCtx *ctx, const XiFunc *f,
                                                      int slot) {
    if (!ctx || slot < 0)
        return NULL;
    const XiImportRef *ref = cg_shared_slot_import_ref(f, slot);
    if (!ref && ctx->module && ctx->module->init != f)
        ref = cg_shared_slot_import_ref(ctx->module->init, slot);
    return cg_class_shared_native_export_for_import_ref(ctx, ref);
}

static bool cg_class_imported_shared_native_export_for_value_depth(
    const XiCgenCtx *ctx, const XiFunc *f, const XiValue *v, const CgSharedNativeExport **out_exp,
    uint8_t depth) {
    if (!ctx || !v || depth > 8)
        return false;
    while (v &&
           (v->op == XI_BOX || v->op == XI_UNBOX || v->op == XI_COPY || v->op == XI_MOVE ||
            v->op == XI_RETAIN || v->op == XI_RELEASE) &&
           v->nargs >= 1) {
        if (++depth > 8)
            return false;
        v = v->args[0];
    }
    if (!v)
        return false;
    if (v->op == XI_IMPORT_REF) {
        const CgSharedNativeExport *exp =
            cg_class_shared_native_export_for_import_ref(ctx, (const XiImportRef *) v->aux);
        if (!exp)
            return false;
        if (out_exp)
            *out_exp = exp;
        return true;
    }
    if (v->op == XI_GET_SHARED) {
        const CgSharedNativeExport *exp =
            cg_class_imported_shared_native_export_for_local_slot(ctx, f, (int) v->aux_int);
        if (!exp)
            return false;
        if (out_exp)
            *out_exp = exp;
        return true;
    }
    if (v->op != XI_PHI || v->nargs == 0)
        return false;
    bool saw_arg = false;
    const CgSharedNativeExport *merged = NULL;
    for (uint16_t i = 0; i < v->nargs; i++) {
        const XiValue *arg = v->args[i];
        if (!arg || arg == v)
            continue;
        const CgSharedNativeExport *arg_exp = NULL;
        if (!cg_class_imported_shared_native_export_for_value_depth(ctx, f, arg, &arg_exp,
                                                                    (uint8_t) (depth + 1)))
            return false;
        if (!saw_arg) {
            saw_arg = true;
            merged = arg_exp;
            continue;
        }
        if (!cg_class_shared_native_exports_match(merged, arg_exp))
            return false;
    }
    if (!saw_arg || !merged)
        return false;
    if (out_exp)
        *out_exp = merged;
    return true;
}

static bool cg_class_imported_shared_native_export_for_value(const XiCgenCtx *ctx, const XiFunc *f,
                                                             const XiValue *v,
                                                             const CgSharedNativeExport **out_exp) {
    return cg_class_imported_shared_native_export_for_value_depth(ctx, f, v, out_exp, 0);
}

static const XiValue *cg_class_native_instance_origin(XiCgenCtx *ctx, const XiFunc *f,
                                                      const XiValue *v) {
    if (cg_class_native_receiver_value(ctx, f, v))
        return v;
    if (cg_class_shared_native_slot_for_value(ctx, v, NULL))
        return v;
    if (cg_class_imported_shared_native_export_for_value(ctx, f, v, NULL))
        return v;
    const XiValue *origin = cg_class_native_trace_ctor_origin(ctx, f, v, 0);
    return origin && cg_class_native_ctor_can_inline(ctx, f, origin) ? origin : NULL;
}

static const XiClassData *cg_class_native_instance_data(XiCgenCtx *ctx, const XiFunc *f,
                                                        const XiValue *v) {
    if (cg_class_native_receiver_value(ctx, f, v)) {
        CgClassNativeFunc info = cg_class_native_func(ctx, f);
        return info.class_data;
    }
    int slot = -1;
    if (cg_class_shared_native_slot_for_value(ctx, v, &slot))
        return ctx->shared_native_instances[slot].class_data;
    const CgSharedNativeExport *exp = NULL;
    if (cg_class_imported_shared_native_export_for_value(ctx, f, v, &exp))
        return exp->class_data;
    const XiValue *origin = cg_class_native_instance_origin(ctx, f, v);
    if (!origin)
        return NULL;
    return cg_class_native_ctor_call_data(ctx, f, origin, NULL, NULL);
}

static const char *cg_class_native_receiver_class_name(XiCgenCtx *ctx, const XiFunc *f,
                                                       const XiValue *recv) {
    if (recv && recv->type &&
        (recv->type->kind == XR_KIND_CLASS || recv->type->kind == XR_KIND_INSTANCE) &&
        recv->type->instance.class_name)
        return recv->type->instance.class_name;
    const XiClassData *data = cg_class_native_instance_data(ctx, f, recv);
    return data ? data->class_name : NULL;
}

static void emit_class_native_instance_base_ref(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                const XiValue *v) {
    if (cg_class_native_receiver_value(ctx, f, v)) {
        fprintf(out, "p0");
        return;
    }
    int slot = -1;
    if (cg_class_shared_native_slot_for_value(ctx, v, &slot)) {
        fprintf(out, "&");
        emit_class_shared_native_storage_name(ctx, out, slot);
        return;
    }
    const CgSharedNativeExport *exp = NULL;
    if (cg_class_imported_shared_native_export_for_value(ctx, f, v, &exp)) {
        fprintf(out, "&");
        emit_class_shared_native_export_storage_name(out, exp);
        return;
    }
    const XiValue *origin = cg_class_native_instance_origin(ctx, f, v);
    emit_vref(out, origin ? origin : v);
}

static bool cg_class_native_can_pass_instance_as(XiCgenCtx *ctx, const XiClassData *source,
                                                 const XiClassData *target) {
    if (!source || !target)
        return false;
    if (source == target || (source->class_name && target->class_name &&
                             strcmp(source->class_name, target->class_name) == 0))
        return true;
    const XiClassData *cur = source;
    for (uint8_t depth = 0; cur && depth < 8; depth++) {
        if (!cur->super_name || cur->inherited_field_count == 0)
            return false;
        cur = cg_class_native_data_by_name(ctx, cur->super_name);
        if (cur && cur->class_name && target->class_name &&
            strcmp(cur->class_name, target->class_name) == 0)
            return true;
    }
    return false;
}

static bool emit_class_native_instance_ref_as(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                              const XiValue *v, const XiClassData *target) {
    const XiClassData *source = cg_class_native_instance_data(ctx, f, v);
    if (!cg_class_native_can_pass_instance_as(ctx, source, target))
        return false;
    if (source == target || (source->class_name && target->class_name &&
                             strcmp(source->class_name, target->class_name) == 0)) {
        emit_class_native_instance_base_ref(ctx, out, f, v);
        return true;
    }
    fprintf(out, "&((");
    emit_class_native_instance_base_ref(ctx, out, f, v);
    fprintf(out, ")->base");
    const XiClassData *cur = source;
    for (uint8_t depth = 0; cur && depth < 8; depth++) {
        XR_DCHECK(cur->super_name != NULL && cur->inherited_field_count > 0,
                  "native class receiver path must be validated before emission");
        cur = cg_class_native_data_by_name(ctx, cur->super_name);
        if (cur && cur->class_name && target->class_name &&
            strcmp(cur->class_name, target->class_name) == 0) {
            fprintf(out, ")");
            return true;
        }
        fprintf(out, ".base");
    }
    return false;
}

static bool emit_class_native_method_call_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                               const char *prefix, const XiValue *v,
                                               const XiFunc *mfunc, const char *method_prefix) {
    if (!cg_class_func_uses_native_receiver(ctx, mfunc))
        return false;
    CgClassNativeFunc target_info = cg_class_native_func(ctx, mfunc);
    XrRep actual_rep = cg_func_return_abi_rep(ctx, mfunc);
    const XiClassData *source_info = cg_class_native_instance_data(ctx, f, v->args[0]);
    if (cg_class_native_instance_origin(ctx, f, v->args[0]) &&
        cg_class_native_can_pass_instance_as(ctx, source_info, target_info.class_data)) {
        bool emit_ctor_stmt_expr = target_info.is_constructor;
        const char *conv_suffix = emit_ctor_stmt_expr
                                      ? NULL
                                      : emit_conversion_prefix(out, v->type, actual_rep, cg_rep(v));
        if (emit_ctor_stmt_expr)
            fprintf(out, "({ (void)");
        emit_fname(ctx, out, method_prefix ? method_prefix : prefix, mfunc);
        fprintf(out, "(NULL, ");
        if (!emit_class_native_instance_ref_as(ctx, out, f, v->args[0], target_info.class_data))
            return false;
        for (uint16_t a = 1; a < v->nargs; a++) {
            fprintf(out, ", ");
            emit_value_as_rep(out, v->args[a], cg_func_param_abi_rep(ctx, mfunc, a));
        }
        fprintf(out, ")");
        if (emit_ctor_stmt_expr)
            fprintf(out, "; XR_NULL_VAL; })");
        else
            emit_conversion_suffix(out, conv_suffix);
    } else {
        const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
        emit_typed_abi_fname(ctx, out, method_prefix ? method_prefix : prefix, mfunc);
        fprintf(out, "(NULL");
        for (uint16_t a = 0; a < v->nargs; a++) {
            fprintf(out, ", ");
            emit_value_as_rep(out, v->args[a], XR_REP_TAGGED);
        }
        fprintf(out, ")");
        emit_conversion_suffix(out, conv_suffix);
    }
    return true;
}

static const XiValue *cg_class_native_prev_block_value(const XiValue *site) {
    if (!site || !site->block)
        return NULL;
    const XiValue *prev = NULL;
    for (uint32_t i = 0; i < site->block->nvalues; i++) {
        const XiValue *cur = site->block->values[i];
        if (cur == site)
            break;
        if (cur)
            prev = cur;
    }
    return prev;
}

static const XiFunc *cg_class_native_resolve_method_call(XiCgenCtx *ctx, const XiFunc *current,
                                                         const XiValue *call,
                                                         const char **out_prefix) {
    if (out_prefix)
        *out_prefix = NULL;
    if (!ctx || !call || (call->op != XI_CALL_METHOD && call->op != XI_CALL_METHOD_DIRECT) ||
        call->nargs < 1)
        return NULL;
    const char *method = (const char *) call->aux;
    bool is_super = call->op == XI_CALL_METHOD && (call->aux_int & 1) != 0;
    if (is_super && ctx->module) {
        const char *parent_class = NULL;
        XiModule *mod = ctx->module;
        for (uint16_t s = 0; s < mod->nslots && !parent_class; s++) {
            const XiClassData *cd = mod->slot_classes ? mod->slot_classes[s] : NULL;
            if (!cd || !cd->super_name)
                continue;
            for (uint16_t ci = 0; ci < cd->ninst + cd->nstat; ci++) {
                if (cd->child_idx && cd->child_idx[ci] < mod->init->nchildren &&
                    mod->init->children[cd->child_idx[ci]] == current) {
                    parent_class = cd->super_name;
                    break;
                }
            }
        }
        if (!parent_class)
            return NULL;
        return method && strcmp(method, "constructor") == 0
                   ? cg_lookup_class_ctor(ctx, parent_class)
                   : cg_lookup_method(ctx, method, parent_class, out_prefix);
    }
    if (is_super)
        return NULL;
    const char *recv_class = cg_class_native_receiver_class_name(ctx, current, call->args[0]);
    if (call->op == XI_CALL_METHOD_DIRECT)
        return cg_lookup_method_by_index(ctx, recv_class, (int) call->aux_int, out_prefix);
    return cg_lookup_method(ctx, method, recv_class, out_prefix);
}

static bool cg_class_shared_native_value_traces_to_slot_depth(const XiValue *v, int slot,
                                                              uint8_t depth) {
    if (!v || slot < 0 || depth > 8)
        return false;
    while (v &&
           (v->op == XI_BOX || v->op == XI_UNBOX || v->op == XI_COPY || v->op == XI_MOVE ||
            v->op == XI_RETAIN || v->op == XI_RELEASE) &&
           v->nargs >= 1) {
        if (++depth > 8)
            return false;
        v = v->args[0];
    }
    if (!v)
        return false;
    if (v->op == XI_GET_SHARED)
        return (int) v->aux_int == slot;
    if (v->op != XI_PHI || v->nargs == 0)
        return false;
    bool saw_arg = false;
    for (uint16_t i = 0; i < v->nargs; i++) {
        const XiValue *arg = v->args[i];
        if (!arg || arg == v)
            continue;
        if (!cg_class_shared_native_value_traces_to_slot_depth(arg, slot, (uint8_t) (depth + 1)))
            return false;
        saw_arg = true;
    }
    return saw_arg;
}

static bool cg_class_shared_native_value_traces_to_slot(const XiValue *v, int slot) {
    return cg_class_shared_native_value_traces_to_slot_depth(v, slot, 0);
}

static bool cg_class_shared_native_method_call_accepts_class(XiCgenCtx *ctx, const XiFunc *current,
                                                             const XiValue *call,
                                                             const XiClassData *source) {
    if (!ctx || !call || !source ||
        (call->op != XI_CALL_METHOD && call->op != XI_CALL_METHOD_DIRECT) || call->nargs < 1)
        return false;
    (void) current;
    const char *method_prefix = NULL;
    const char *method = (const char *) call->aux;
    const XiFunc *mfunc = call->op == XI_CALL_METHOD_DIRECT
                              ? cg_lookup_method_by_index(ctx, source->class_name,
                                                          (int) call->aux_int, &method_prefix)
                              : cg_lookup_method(ctx, method, source->class_name, &method_prefix);
    (void) method_prefix;
    if (!mfunc || cg_func_needs_aot_coro(mfunc) || !cg_class_func_uses_native_receiver(ctx, mfunc))
        return false;
    CgClassNativeFunc target_info = cg_class_native_func(ctx, mfunc);
    return cg_class_native_can_pass_instance_as(ctx, source, target_info.class_data);
}

static bool cg_class_shared_native_method_call_accepts_slot(XiCgenCtx *ctx, const XiFunc *current,
                                                            const XiValue *call, int slot) {
    if (!ctx || slot < 0 || slot >= ctx->shared_cap)
        return false;
    return cg_class_shared_native_method_call_accepts_class(
        ctx, current, call, ctx->shared_native_instances[slot].class_data);
}

static bool cg_class_shared_native_alias_safe_uses(XiCgenCtx *ctx, const XiFunc *f,
                                                   const XiValue *target, int slot, uint8_t depth) {
    if (!ctx || !f || !target || slot < 0 || depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t ai = 0; ai < phi->value.nargs; ai++) {
                if (phi->value.args[ai] != target)
                    continue;
                if (!cg_class_shared_native_value_traces_to_slot(&phi->value, slot))
                    return false;
                if (!cg_class_shared_native_alias_safe_uses(ctx, f, &phi->value, slot,
                                                            (uint8_t) (depth + 1)))
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t ai = 0; ai < v->nargs; ai++) {
                if (v->args[ai] != target)
                    continue;
                if ((v->op == XI_CALL_METHOD || v->op == XI_CALL_METHOD_DIRECT) && ai == 0 &&
                    cg_class_shared_native_method_call_accepts_slot(ctx, f, v, slot))
                    continue;
                if ((v->op == XI_COPY || v->op == XI_MOVE || v->op == XI_BOX ||
                     v->op == XI_UNBOX) &&
                    ai == 0) {
                    if (!cg_class_shared_native_value_traces_to_slot(v, slot))
                        return false;
                    if (!cg_class_shared_native_alias_safe_uses(ctx, f, v, slot,
                                                                (uint8_t) (depth + 1)))
                        return false;
                    continue;
                }
                if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && ai == 0)
                    continue;
                return false;
            }
        }
    }
    return true;
}

static bool cg_class_shared_native_get_uses_safe_in_func(XiCgenCtx *ctx, const XiFunc *f,
                                                         int slot) {
    if (!ctx || !f || slot < 0)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v || v->op != XI_GET_SHARED || (int) v->aux_int != slot)
                continue;
            if (!cg_class_shared_native_alias_safe_uses(ctx, f, v, slot, 0))
                return false;
        }
    }
    for (uint16_t ci = 0; ci < f->nchildren; ci++) {
        if (!cg_class_shared_native_get_uses_safe_in_func(ctx, f->children[ci], slot))
            return false;
    }
    return true;
}

static bool cg_class_shared_native_import_alias_safe_uses(XiCgenCtx *ctx, const XiModule *module,
                                                          const XiFunc *f, const XiValue *target,
                                                          int exporter_index, int exporter_slot,
                                                          const XiClassData *source, uint8_t depth);

static bool cg_class_shared_native_import_slot_uses_safe_in_func(
    XiCgenCtx *ctx, const XiModule *module, const XiFunc *f, int local_slot, int exporter_index,
    int exporter_slot, const XiClassData *source, uint8_t depth);

static bool cg_class_shared_native_import_ref_targets_slot(const XiImportRef *ref,
                                                           int exporter_index, int exporter_slot) {
    return ref && ref->resolved_mod_index == exporter_index &&
           ref->resolved_shared_slot == exporter_slot;
}

static const XiImportRef *cg_class_shared_native_import_ref_for_local_slot(const XiModule *module,
                                                                           const XiFunc *f,
                                                                           int local_slot) {
    const XiImportRef *ref = cg_shared_slot_import_ref(f, local_slot);
    if (!ref && module && module->init != f)
        ref = cg_shared_slot_import_ref(module->init, local_slot);
    return ref;
}

static bool cg_class_shared_native_value_traces_to_export_slot_depth(
    XiCgenCtx *ctx, const XiModule *module, const XiFunc *f, const XiValue *v, int exporter_index,
    int exporter_slot, uint8_t depth) {
    (void) ctx;
    if (!module || !v || exporter_index < 0 || exporter_slot < 0 || depth > 8)
        return false;
    while (v &&
           (v->op == XI_BOX || v->op == XI_UNBOX || v->op == XI_COPY || v->op == XI_MOVE ||
            v->op == XI_RETAIN || v->op == XI_RELEASE) &&
           v->nargs >= 1) {
        if (++depth > 8)
            return false;
        v = v->args[0];
    }
    if (!v)
        return false;
    if (v->op == XI_IMPORT_REF)
        return cg_class_shared_native_import_ref_targets_slot((const XiImportRef *) v->aux,
                                                              exporter_index, exporter_slot);
    if (v->op == XI_GET_SHARED) {
        const XiImportRef *ref =
            cg_class_shared_native_import_ref_for_local_slot(module, f, (int) v->aux_int);
        return cg_class_shared_native_import_ref_targets_slot(ref, exporter_index, exporter_slot);
    }
    if (v->op != XI_PHI || v->nargs == 0)
        return false;
    bool saw_arg = false;
    for (uint16_t i = 0; i < v->nargs; i++) {
        const XiValue *arg = v->args[i];
        if (!arg || arg == v)
            continue;
        if (!cg_class_shared_native_value_traces_to_export_slot_depth(
                ctx, module, f, arg, exporter_index, exporter_slot, (uint8_t) (depth + 1)))
            return false;
        saw_arg = true;
    }
    return saw_arg;
}

static bool cg_class_shared_native_value_traces_to_export_slot(XiCgenCtx *ctx,
                                                               const XiModule *module,
                                                               const XiFunc *f, const XiValue *v,
                                                               int exporter_index,
                                                               int exporter_slot) {
    return cg_class_shared_native_value_traces_to_export_slot_depth(
        ctx, module, f, v, exporter_index, exporter_slot, 0);
}

static bool cg_class_shared_native_import_alias_safe_uses(XiCgenCtx *ctx, const XiModule *module,
                                                          const XiFunc *f, const XiValue *target,
                                                          int exporter_index, int exporter_slot,
                                                          const XiClassData *source,
                                                          uint8_t depth) {
    if (!ctx || !module || !f || !target || !source || depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t ai = 0; ai < phi->value.nargs; ai++) {
                if (phi->value.args[ai] != target)
                    continue;
                if (!cg_class_shared_native_value_traces_to_export_slot(
                        ctx, module, f, &phi->value, exporter_index, exporter_slot))
                    return false;
                if (!cg_class_shared_native_import_alias_safe_uses(ctx, module, f, &phi->value,
                                                                   exporter_index, exporter_slot,
                                                                   source, (uint8_t) (depth + 1)))
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t ai = 0; ai < v->nargs; ai++) {
                if (v->args[ai] != target)
                    continue;
                if ((v->op == XI_CALL_METHOD || v->op == XI_CALL_METHOD_DIRECT) && ai == 0 &&
                    cg_class_shared_native_method_call_accepts_class(ctx, f, v, source))
                    continue;
                if ((v->op == XI_COPY || v->op == XI_MOVE || v->op == XI_BOX ||
                     v->op == XI_UNBOX) &&
                    ai == 0) {
                    if (!cg_class_shared_native_value_traces_to_export_slot(
                            ctx, module, f, v, exporter_index, exporter_slot))
                        return false;
                    if (!cg_class_shared_native_import_alias_safe_uses(
                            ctx, module, f, v, exporter_index, exporter_slot, source,
                            (uint8_t) (depth + 1)))
                        return false;
                    continue;
                }
                if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && ai == 0)
                    continue;
                if (v->op == XI_SET_SHARED && ai == 0) {
                    if (!cg_class_shared_native_import_slot_uses_safe_in_func(
                            ctx, module, module->init, (int) v->aux_int, exporter_index,
                            exporter_slot, source, (uint8_t) (depth + 1)))
                        return false;
                    continue;
                }
                return false;
            }
        }
    }
    return true;
}

static bool cg_class_shared_native_import_slot_uses_safe_in_func(
    XiCgenCtx *ctx, const XiModule *module, const XiFunc *f, int local_slot, int exporter_index,
    int exporter_slot, const XiClassData *source, uint8_t depth) {
    if (!ctx || !module || !f || local_slot < 0 || !source || depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v || v->op != XI_GET_SHARED || (int) v->aux_int != local_slot)
                continue;
            if (!cg_class_shared_native_value_traces_to_export_slot(ctx, module, f, v,
                                                                    exporter_index, exporter_slot))
                return false;
            if (!cg_class_shared_native_import_alias_safe_uses(ctx, module, f, v, exporter_index,
                                                               exporter_slot, source,
                                                               (uint8_t) (depth + 1)))
                return false;
        }
    }
    for (uint16_t ci = 0; ci < f->nchildren; ci++) {
        if (!cg_class_shared_native_import_slot_uses_safe_in_func(
                ctx, module, f->children[ci], local_slot, exporter_index, exporter_slot, source,
                (uint8_t) (depth + 1)))
            return false;
    }
    return true;
}

static bool cg_class_shared_native_import_ref_uses_safe_in_func(
    XiCgenCtx *ctx, const XiModule *module, const XiFunc *f, int exporter_index, int exporter_slot,
    const XiClassData *source, uint8_t depth) {
    if (!ctx || !module || !f || !source || depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v || v->op != XI_IMPORT_REF ||
                !cg_class_shared_native_import_ref_targets_slot((const XiImportRef *) v->aux,
                                                                exporter_index, exporter_slot))
                continue;
            if (!cg_class_shared_native_import_alias_safe_uses(ctx, module, f, v, exporter_index,
                                                               exporter_slot, source,
                                                               (uint8_t) (depth + 1)))
                return false;
        }
    }
    for (uint16_t ci = 0; ci < f->nchildren; ci++) {
        if (!cg_class_shared_native_import_ref_uses_safe_in_func(ctx, module, f->children[ci],
                                                                 exporter_index, exporter_slot,
                                                                 source, (uint8_t) (depth + 1)))
            return false;
    }
    return true;
}

static bool cg_class_shared_native_external_uses_safe(XiCgenCtx *ctx, int slot,
                                                      const XiClassData *source) {
    if (!cg_class_shared_native_slot_is_exported(ctx, slot))
        return true;
    int exporter_index = cg_class_shared_native_module_index(ctx, ctx->module);
    if (exporter_index < 0 || !source)
        return false;
    for (int mi = 0; mi < ctx->all_nmodules; mi++) {
        const XiModule *module = ctx->all_modules ? ctx->all_modules[mi] : NULL;
        if (!module || module == ctx->module || !module->init)
            continue;
        if (!cg_class_shared_native_import_ref_uses_safe_in_func(ctx, module, module->init,
                                                                 exporter_index, slot, source, 0))
            return false;
    }
    return true;
}

typedef struct {
    uint16_t set_count;
    bool invalid;
    const XiValue *ctor_call;
    const XiFunc *ctor;
    const char *ctor_prefix;
    const XiClassData *class_data;
} CgSharedNativeCtorCandidate;

static void cg_class_shared_native_scan_sets_in_func(XiCgenCtx *ctx, const XiFunc *f, int slot,
                                                     CgSharedNativeCtorCandidate *candidate) {
    if (!ctx || !f || !candidate || candidate->invalid)
        return;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v || v->op != XI_SET_SHARED || (int) v->aux_int != slot || v->nargs < 1)
                continue;
            candidate->set_count++;
            if (candidate->set_count > 1) {
                candidate->invalid = true;
                return;
            }
            const XiValue *origin = cg_class_native_trace_ctor_origin(ctx, f, v->args[0], 0);
            const XiFunc *ctor = NULL;
            const char *ctor_prefix = NULL;
            const XiClassData *cd =
                cg_class_native_ctor_call_data(ctx, f, origin, &ctor, &ctor_prefix);
            if (!origin || !ctor || !cd || !cd->instance_layout) {
                candidate->invalid = true;
                return;
            }
            candidate->ctor_call = origin;
            candidate->ctor = ctor;
            candidate->ctor_prefix = ctor_prefix;
            candidate->class_data = cd;
        }
    }
    for (uint16_t ci = 0; ci < f->nchildren; ci++)
        cg_class_shared_native_scan_sets_in_func(ctx, f->children[ci], slot, candidate);
}

static bool cg_class_shared_native_slot_can_activate(XiCgenCtx *ctx, int slot,
                                                     CgSharedNativeCtorCandidate *out) {
    if (!ctx || !ctx->module || !ctx->module->init || slot < 0 || slot >= ctx->nshared ||
        slot >= ctx->shared_cap || !out)
        return false;
    CgSharedNativeCtorCandidate candidate;
    memset(&candidate, 0, sizeof(candidate));
    cg_class_shared_native_scan_sets_in_func(ctx, ctx->module->init, slot, &candidate);
    if (candidate.invalid || candidate.set_count != 1 || !candidate.ctor_call || !candidate.ctor ||
        !candidate.class_data)
        return false;
    ctx->shared_native_instances[slot].class_data = candidate.class_data;
    if (!cg_class_shared_native_get_uses_safe_in_func(ctx, ctx->module->init, slot)) {
        memset(&ctx->shared_native_instances[slot], 0, sizeof(ctx->shared_native_instances[slot]));
        return false;
    }
    if (!cg_class_shared_native_external_uses_safe(ctx, slot, candidate.class_data)) {
        memset(&ctx->shared_native_instances[slot], 0, sizeof(ctx->shared_native_instances[slot]));
        return false;
    }
    *out = candidate;
    return true;
}

static void cg_class_shared_native_register_export(XiCgenCtx *ctx, int slot) {
    if (!ctx || !ctx->module || !cg_class_shared_native_slot_active(ctx, slot) ||
        !cg_class_shared_native_slot_is_exported(ctx, slot))
        return;
    int module_index = cg_class_shared_native_module_index(ctx, ctx->module);
    if (module_index < 0)
        return;
    for (int i = 0; i < ctx->nshared_native_exports; i++) {
        CgSharedNativeExport *exp = &ctx->shared_native_exports[i];
        if (exp->active && exp->module_index == module_index && exp->slot == slot)
            return;
    }
    if (!cg_reserve_shared_native_exports(ctx, ctx->nshared_native_exports + 1))
        return;
    CgSharedNativeExport *exp = &ctx->shared_native_exports[ctx->nshared_native_exports++];
    memset(exp, 0, sizeof(*exp));
    exp->active = true;
    exp->module = ctx->module;
    exp->module_name = ctx->module->name;
    exp->module_index = module_index;
    exp->slot = slot;
    exp->class_data = ctx->shared_native_instances[slot].class_data;
}

static void cg_collect_shared_native_instances(XiCgenCtx *ctx) {
    if (!ctx || !ctx->module || !ctx->module->init)
        return;
    memset(ctx->shared_native_instances, 0,
           (size_t) ctx->shared_cap * sizeof(*ctx->shared_native_instances));
    int limit = ctx->nshared < ctx->shared_cap ? ctx->nshared : ctx->shared_cap;
    for (int slot = 0; slot < limit; slot++) {
        CgSharedNativeCtorCandidate candidate;
        memset(&candidate, 0, sizeof(candidate));
        if (!cg_class_shared_native_slot_can_activate(ctx, slot, &candidate))
            continue;
        ctx->shared_native_instances[slot].active = true;
        ctx->shared_native_instances[slot].class_data = candidate.class_data;
        ctx->shared_native_instances[slot].ctor = candidate.ctor;
        ctx->shared_native_instances[slot].ctor_prefix = candidate.ctor_prefix;
        ctx->shared_native_instances[slot].ctor_call = candidate.ctor_call;
        cg_class_shared_native_register_export(ctx, slot);
    }
}

static void emit_class_shared_native_storage_decls(XiCgenCtx *ctx, FILE *out, const char *prefix) {
    if (!ctx || !out)
        return;
    int limit = ctx->nshared < ctx->shared_cap ? ctx->nshared : ctx->shared_cap;
    for (int slot = 0; slot < limit; slot++) {
        if (!cg_class_shared_native_slot_active(ctx, slot))
            continue;
        const CgSharedNativeInstance *inst = &ctx->shared_native_instances[slot];
        fprintf(out, "static ");
        emit_class_native_type_name(out, inst->ctor_prefix ? inst->ctor_prefix : prefix,
                                    inst->class_data->class_name);
        fprintf(out, " ");
        emit_class_shared_native_storage_name(ctx, out, slot);
        fprintf(out, ";\n");
    }
}

static bool cg_class_shared_native_ctor_value_is_elided(const XiCgenCtx *ctx, const XiFunc *f,
                                                        const XiValue *v, int *out_slot) {
    (void) f;
    if (!ctx || !v || v->op != XI_CALL)
        return false;
    int limit = ctx->nshared < ctx->shared_cap ? ctx->nshared : ctx->shared_cap;
    for (int slot = 0; slot < limit; slot++) {
        if (!cg_class_shared_native_slot_active(ctx, slot))
            continue;
        if (ctx->shared_native_instances[slot].ctor_call == v) {
            if (out_slot)
                *out_slot = slot;
            return true;
        }
    }
    return false;
}

static bool cg_class_shared_native_set_is_elided(const XiCgenCtx *ctx, const XiFunc *f,
                                                 const XiValue *v) {
    if (!ctx || !v || v->op != XI_SET_SHARED || v->nargs < 1)
        return false;
    int slot = (int) v->aux_int;
    if (cg_class_shared_native_slot_active(ctx, slot)) {
        const XiValue *origin =
            cg_class_native_trace_ctor_origin((XiCgenCtx *) ctx, f, v->args[0], 0);
        return origin == ctx->shared_native_instances[slot].ctor_call;
    }
    return cg_class_imported_shared_native_export_for_value(ctx, f, v->args[0], NULL);
}

static bool cg_class_shared_native_value_is_elided(XiCgenCtx *ctx, const XiFunc *f,
                                                   const XiValue *v) {
    if (!ctx || !f || !v)
        return false;
    int slot = -1;
    const XiValue *target = v;
    if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && v->nargs >= 1)
        target = v->args[0];
    if (cg_class_shared_native_slot_for_value(ctx, target, &slot))
        return cg_class_shared_native_alias_safe_uses(ctx, f, target, slot, 0);
    const CgSharedNativeExport *exp = NULL;
    if (!cg_class_imported_shared_native_export_for_value(ctx, f, target, &exp))
        return false;
    return cg_class_shared_native_import_alias_safe_uses(
        ctx, ctx->module, f, target, exp->module_index, exp->slot, exp->class_data, 0);
}

static bool emit_class_shared_native_ctor_value_stmt(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                     const char *prefix, const XiValue *v) {
    int slot = -1;
    if (!cg_class_shared_native_ctor_value_is_elided(ctx, f, v, &slot))
        return false;
    const CgSharedNativeInstance *inst = &ctx->shared_native_instances[slot];
    if (!inst->ctor || !inst->class_data)
        return false;
    fprintf(out, "    memset(&");
    emit_class_shared_native_storage_name(ctx, out, slot);
    fprintf(out, ", 0, sizeof(");
    emit_class_shared_native_storage_name(ctx, out, slot);
    fprintf(out, "));\n");
    fprintf(out, "    (void)");
    emit_fname(ctx, out, inst->ctor_prefix ? inst->ctor_prefix : prefix, inst->ctor);
    fprintf(out, "(NULL, &");
    emit_class_shared_native_storage_name(ctx, out, slot);
    for (uint16_t a = 1; a < v->nargs; a++) {
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[a], cg_func_param_abi_rep(ctx, inst->ctor, a));
    }
    fprintf(out, ");\n");
    return true;
}

static bool cg_class_native_func_has_error_flow(XiCgenCtx *ctx, const XiFunc *f, uint8_t depth);

static bool cg_class_native_call_is_nothrow_direct_depth(XiCgenCtx *ctx, const XiFunc *current,
                                                         const XiValue *call, uint8_t depth) {
    if (!ctx || !current || !call || depth > 8)
        return false;
    if (cg_class_native_map_method_call_is_direct(ctx, current, call))
        return true;
    if (cg_class_native_set_method_call_is_direct(ctx, current, call))
        return true;
    if (call->op == XI_CALL) {
        const XiFunc *target = NULL;
        const XiClassData *cd = cg_class_native_ctor_call_data(ctx, current, call, &target, NULL);
        return cd && target && cg_class_func_is_native_constructor(ctx, target) &&
               cg_class_native_ctor_can_inline(ctx, current, call) &&
               !cg_class_native_func_has_error_flow(ctx, target, (uint8_t) (depth + 1));
    }
    const char *method_prefix = NULL;
    const XiFunc *mfunc = cg_class_native_resolve_method_call(ctx, current, call, &method_prefix);
    (void) method_prefix;
    if (!mfunc || cg_func_needs_aot_coro(mfunc) ||
        !cg_class_func_uses_native_receiver(ctx, mfunc) ||
        cg_class_native_func_has_error_flow(ctx, mfunc, (uint8_t) (depth + 1)))
        return false;
    CgClassNativeFunc target_info = cg_class_native_func(ctx, mfunc);
    const XiClassData *source_info = cg_class_native_instance_data(ctx, current, call->args[0]);
    return cg_class_native_instance_origin(ctx, current, call->args[0]) &&
           cg_class_native_can_pass_instance_as(ctx, source_info, target_info.class_data);
}

static bool cg_class_native_call_is_nothrow_direct(XiCgenCtx *ctx, const XiFunc *current,
                                                   const XiValue *call) {
    return cg_class_native_call_is_nothrow_direct_depth(ctx, current, call, 0);
}

static bool cg_class_native_err_check_is_dead(XiCgenCtx *ctx, const XiFunc *current,
                                              const XiValue *check) {
    if (!check || check->op != XI_ERR_CHECK || cg_value_type_is_bool(check))
        return false;
    return cg_class_native_call_is_nothrow_direct(ctx, current,
                                                  cg_class_native_prev_block_value(check));
}

static bool cg_class_native_const_int_value(const XiValue *value, int64_t *out) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || v->op != XI_CONST || !v->type || v->type->kind != XR_KIND_INT || !out)
        return false;
    *out = v->aux_int;
    return true;
}

static bool cg_class_native_value_is_nothrow_arith(const XiValue *v) {
    if (!v || (v->op != XI_DIV && v->op != XI_MOD) || v->nargs < 2 || cg_rep(v) != XR_REP_I64 ||
        cg_rep(v->args[0]) != XR_REP_I64 || cg_rep(v->args[1]) != XR_REP_I64)
        return false;
    int64_t divisor = 0;
    return cg_class_native_const_int_value(v->args[1], &divisor) && divisor != 0;
}

static bool cg_class_native_func_has_error_flow(XiCgenCtx *ctx, const XiFunc *f, uint8_t depth) {
    if (!ctx || !f || depth > 8)
        return true;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            if (v->op == XI_TRY || v->op == XI_CATCH || v->op == XI_ERR_SET ||
                v->op == XI_ERR_RETURN || v->op == XI_ERR_CATCH || v->op == XI_THROW)
                return true;
            if (cg_class_native_err_check_is_dead(ctx, f, v))
                continue;
            if (v->flags & XI_FLAG_MAY_SUSPEND)
                return true;
            if (v->flags & XI_FLAG_MAY_THROW) {
                if (cg_class_native_value_is_nothrow_arith(v))
                    continue;
                if (!cg_class_native_call_is_nothrow_direct_depth(ctx, f, v, (uint8_t) (depth + 1)))
                    return true;
            }
        }
    }
    return false;
}

static bool cg_class_native_err_check_after_nothrow_call(XiCgenCtx *ctx, const XiFunc *current,
                                                         const XiValue *check) {
    if (!check || check->op != XI_ERR_CHECK || cg_value_type_is_bool(check))
        return false;
    return cg_class_native_call_is_nothrow_direct(ctx, current,
                                                  cg_class_native_prev_block_value(check));
}

static void emit_class_native_typedefs(XiCgenCtx *ctx, FILE *out, XiModule *module,
                                       const char *prefix) {
    (void) ctx;
    if (!module || !module->classes)
        return;
    for (uint16_t ci = 0; ci < module->nclasses; ci++) {
        const XiClassData *cd = module->classes[ci];
        if (!cd || !cd->instance_layout)
            continue;
        fprintf(out, "typedef struct ");
        emit_class_native_type_name(out, prefix, cd->class_name);
        fprintf(out, " { ");
        if (cd->inherited_field_count > 0 && cd->super_name) {
            emit_class_native_type_name(out, prefix, cd->super_name);
            fprintf(out, " base; ");
        }
        for (uint16_t fi = cd->inherited_field_count; fi < cd->instance_layout->field_count; fi++)
            fprintf(out, "%s f%u; ", cg_struct_field_c_type(cd->instance_layout, fi),
                    (unsigned) fi);
        fprintf(out, "} ");
        emit_class_native_type_name(out, prefix, cd->class_name);
        fprintf(out, ";\n");
    }
}
