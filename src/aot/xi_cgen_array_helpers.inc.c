/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_array_helpers.inc.c - AOT typed array fast-path emission helpers
 */

typedef struct CgArrayElemInfo {
    const XrType *type;
    const char *elem_name;
    const char *ctype;
    XrRep rep;
} CgArrayElemInfo;

static bool cg_array_elem_info_is_u8(const CgArrayElemInfo *info);
static bool cg_array_elem_info_from_type_ctx(XiCgenCtx *ctx, const XrType *type,
                                             CgArrayElemInfo *out);
static void cg_emit_static_fixed_array_name(XiCgenCtx *ctx, FILE *out, const XiModule *module,
                                            int64_t slot);

static bool cg_value_type_is_span(const XiValue *value) {
    const XiValue *v = cg_unwrap_identity_value(value);
    return v && v->type && v->type->kind == XR_KIND_SPAN;
}

static bool cg_value_type_is_fixed_array(const XiValue *value) {
    const XiValue *v = cg_unwrap_identity_value(value);
    return v && v->type && v->type->kind == XR_KIND_FIXED_ARRAY;
}

static void emit_array_i64_arg(FILE *out, const XiValue *value);

typedef struct CgFixedArrayLaneInfo {
    const XiValue *stack_origin;
    const char *ctype;
    XrRep rep;
    uint8_t native_type;
    uint32_t count;
} CgFixedArrayLaneInfo;

typedef struct CgStaticFixedStructArrayInfo {
    const XrAggregateLayout *layout;
    uint16_t count;
} CgStaticFixedStructArrayInfo;

typedef struct CgStaticFixedTupleArrayInfo {
    XrType *tuple_type;
    uint16_t count;
} CgStaticFixedTupleArrayInfo;

typedef struct CgStaticFixedMatrixInfo {
    CgFixedArrayLaneInfo lane;
    uint16_t outer_count;
    uint16_t inner_count;
} CgStaticFixedMatrixInfo;

typedef struct CgStaticFixedCubeInfo {
    CgFixedArrayLaneInfo lane;
    uint16_t outer_count;
    uint16_t middle_count;
    uint16_t inner_count;
} CgStaticFixedCubeInfo;

static bool cg_fixed_array_lane_info_from_type(const XrType *type, CgFixedArrayLaneInfo *out) {
    if (!type || type->kind != XR_KIND_FIXED_ARRAY || !type->fixed_array.element_type ||
        type->fixed_array.length < 0 ||
        (uint64_t) type->fixed_array.length > XR_ARRAY_REF_MAX_COUNT || !out)
        return false;
    XrType *elem = type->fixed_array.element_type;
    int native = xr_type_kind_to_native(elem->kind, elem->native_width);
    if (elem->is_nullable || native < 0)
        native = XR_NATIVE_VALUE;
    *out = (CgFixedArrayLaneInfo) {
        .stack_origin = NULL,
        .ctype = cg_struct_native_c_type((uint8_t) native),
        .rep = cg_struct_native_rep((uint8_t) native),
        .native_type = (uint8_t) native,
        .count = (uint32_t) type->fixed_array.length,
    };
    return true;
}

static bool cg_fixed_array_lane_info_from_value(const XiValue *value, CgFixedArrayLaneInfo *out) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || !cg_fixed_array_lane_info_from_type(v->type, out))
        return false;
    if (v->op == XI_FIXED_ARRAY_NEW || v->op == XI_FIXED_BYTES_CONST)
        out->stack_origin = v;
    return true;
}

static bool cg_ct_static_fixed_array_value_supported(const XrCtValue *value,
                                                     const CgFixedArrayLaneInfo *info) {
    if (!value)
        return false;
    if (info && info->native_type == XR_NATIVE_STRING)
        return value->kind == XR_CT_STRING;
    XrRep rep = info ? info->rep : XR_REP_TAGGED;
    switch (value->kind) {
        case XR_CT_INT:
        case XR_CT_BOOL:
        case XR_CT_CHAR:
            return rep == XR_REP_I64 || rep == XR_REP_F64;
        case XR_CT_FLOAT:
            return rep == XR_REP_F64;
        default:
            return false;
    }
}

static bool cg_freestanding_static_fixed_array_literal_in_module(XiCgenCtx *ctx,
                                                                 const XiModule *module,
                                                                 int64_t slot,
                                                                 CgFixedArrayLaneInfo *out_info,
                                                                 const XrCtValue **out_value) {
    if (!ctx || !ctx->freestanding_profile || !module || !module->slot_const_literals || slot < 0 ||
        slot >= module->nslots)
        return false;
    const XiConstLiteral *lit = &module->slot_const_literals[slot];
    if (lit->kind != XI_CONST_LITERAL_COMPTIME_AGGREGATE || !lit->ct_value ||
        lit->ct_value->kind != XR_CT_FIXED_ARRAY || !lit->type ||
        lit->type->kind != XR_KIND_FIXED_ARRAY)
        return false;
    CgFixedArrayLaneInfo info;
    if (!cg_fixed_array_lane_info_from_type(lit->type, &info) ||
        (info.rep == XR_REP_TAGGED && info.native_type != XR_NATIVE_STRING))
        return false;
    const XrCtFixedArrayValue *array = &lit->ct_value->as.fixed_array_val;
    if (array->count != (int) info.count)
        return false;
    if (array->is_byte_blob) {
        if (info.native_type != XR_NATIVE_U8 || (array->count > 0 && !array->byte_blob))
            return false;
        if (out_info)
            *out_info = info;
        if (out_value)
            *out_value = lit->ct_value;
        return true;
    }
    if (array->count <= 0 || !array->elements)
        return false;
    for (int i = 0; i < array->count; i++) {
        if (!cg_ct_static_fixed_array_value_supported(&array->elements[i], &info))
            return false;
    }
    if (out_info)
        *out_info = info;
    if (out_value)
        *out_value = lit->ct_value;
    return true;
}

static bool cg_freestanding_static_fixed_array_literal(XiCgenCtx *ctx, int64_t slot,
                                                       CgFixedArrayLaneInfo *out_info,
                                                       const XrCtValue **out_value) {
    return cg_freestanding_static_fixed_array_literal_in_module(ctx, ctx ? ctx->module : NULL, slot,
                                                                out_info, out_value);
}

static bool cg_freestanding_static_fixed_array_value_ex(XiCgenCtx *ctx, const XiValue *value,
                                                        CgFixedArrayLaneInfo *out_info,
                                                        int64_t *out_slot,
                                                        const XiModule **out_module) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (out_module)
        *out_module = NULL;
    if (!v)
        return false;

    const XiModule *module = ctx ? ctx->module : NULL;
    int64_t slot = -1;
    if (v->op == XI_GET_SHARED) {
        slot = v->aux_int;
        const XiModule *import_module = NULL;
        int64_t import_slot = -1;
        const XiConstLiteral *import_lit =
            cg_import_slot_const_literal(ctx, NULL, (int) slot, &import_module, &import_slot);
        if (import_lit && import_lit->kind == XI_CONST_LITERAL_COMPTIME_AGGREGATE) {
            module = import_module;
            slot = import_slot;
        }
    } else if (v->op == XI_IMPORT_REF && v->aux) {
        const XiModule *import_module = NULL;
        int64_t import_slot = -1;
        const XiConstLiteral *import_lit = cg_import_ref_target_const_literal(
            ctx, (const XiImportRef *) v->aux, &import_module, &import_slot);
        if (!import_lit || import_lit->kind != XI_CONST_LITERAL_COMPTIME_AGGREGATE)
            return false;
        module = import_module;
        slot = import_slot;
    } else {
        return false;
    }

    if (!cg_freestanding_static_fixed_array_literal_in_module(ctx, module, slot, out_info, NULL))
        return false;
    const XiConstLiteral *lit = cg_module_const_literal(module, slot);
    if (cg_imported_static_const_needs_weak_symbol(ctx, module, lit)) {
        cg_report_imported_static_const_requires_weak(ctx, module, slot);
        ctx->error = true;
        return false;
    }
    if (out_slot)
        *out_slot = slot;
    if (out_module)
        *out_module = module;
    return true;
}

static bool cg_freestanding_static_fixed_array_value(XiCgenCtx *ctx, const XiValue *value,
                                                     CgFixedArrayLaneInfo *out_info,
                                                     int64_t *out_slot) {
    return cg_freestanding_static_fixed_array_value_ex(ctx, value, out_info, out_slot, NULL);
}

static bool cg_static_fixed_matrix_info_from_type(const XrType *type,
                                                  CgStaticFixedMatrixInfo *out) {
    if (!type || type->kind != XR_KIND_FIXED_ARRAY || !type->fixed_array.element_type ||
        type->fixed_array.length <= 0 || type->fixed_array.length > UINT16_MAX)
        return false;
    const XrType *row_type = type->fixed_array.element_type;
    if (!row_type || row_type->kind != XR_KIND_FIXED_ARRAY || row_type->fixed_array.length <= 0 ||
        row_type->fixed_array.length > UINT16_MAX)
        return false;
    CgFixedArrayLaneInfo lane;
    if (!cg_fixed_array_lane_info_from_type(row_type, &lane) ||
        (lane.rep == XR_REP_TAGGED && lane.native_type != XR_NATIVE_STRING))
        return false;
    if (out)
        *out = (CgStaticFixedMatrixInfo) {
            .lane = lane,
            .outer_count = (uint16_t) type->fixed_array.length,
            .inner_count = (uint16_t) row_type->fixed_array.length,
        };
    return true;
}

static bool cg_freestanding_static_fixed_matrix_literal_in_module(XiCgenCtx *ctx,
                                                                  const XiModule *module,
                                                                  int64_t slot,
                                                                  CgStaticFixedMatrixInfo *out_info,
                                                                  const XrCtValue **out_value) {
    if (!ctx || !ctx->freestanding_profile || !module || !module->slot_const_literals || slot < 0 ||
        slot >= module->nslots)
        return false;
    const XiConstLiteral *lit = &module->slot_const_literals[slot];
    if (lit->kind != XI_CONST_LITERAL_COMPTIME_AGGREGATE || !lit->ct_value ||
        lit->ct_value->kind != XR_CT_FIXED_ARRAY || !lit->type ||
        lit->type->kind != XR_KIND_FIXED_ARRAY)
        return false;
    CgStaticFixedMatrixInfo info;
    if (!cg_static_fixed_matrix_info_from_type(lit->type, &info))
        return false;
    const XrCtFixedArrayValue *outer = &lit->ct_value->as.fixed_array_val;
    if (outer->count != (int) info.outer_count || outer->count <= 0 || !outer->elements)
        return false;
    for (int row = 0; row < outer->count; row++) {
        const XrCtValue *row_value = &outer->elements[row];
        if (!row_value || row_value->kind != XR_CT_FIXED_ARRAY)
            return false;
        const XrCtFixedArrayValue *inner = &row_value->as.fixed_array_val;
        if (inner->count != (int) info.inner_count || inner->count <= 0 || !inner->elements)
            return false;
        for (int col = 0; col < inner->count; col++) {
            if (!cg_ct_static_fixed_array_value_supported(&inner->elements[col], &info.lane))
                return false;
        }
    }
    if (out_info)
        *out_info = info;
    if (out_value)
        *out_value = lit->ct_value;
    return true;
}

static bool cg_freestanding_static_fixed_matrix_literal(XiCgenCtx *ctx, int64_t slot,
                                                        CgStaticFixedMatrixInfo *out_info,
                                                        const XrCtValue **out_value) {
    return cg_freestanding_static_fixed_matrix_literal_in_module(ctx, ctx ? ctx->module : NULL,
                                                                 slot, out_info, out_value);
}

static bool cg_freestanding_static_fixed_matrix_value_ex(XiCgenCtx *ctx, const XiValue *value,
                                                         CgStaticFixedMatrixInfo *out_info,
                                                         int64_t *out_slot,
                                                         const XiModule **out_module) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (out_module)
        *out_module = NULL;
    if (!v)
        return false;

    const XiModule *module = ctx ? ctx->module : NULL;
    int64_t slot = -1;
    if (v->op == XI_GET_SHARED) {
        slot = v->aux_int;
        const XiModule *import_module = NULL;
        int64_t import_slot = -1;
        const XiConstLiteral *import_lit =
            cg_import_slot_const_literal(ctx, NULL, (int) slot, &import_module, &import_slot);
        if (import_lit && import_lit->kind == XI_CONST_LITERAL_COMPTIME_AGGREGATE) {
            module = import_module;
            slot = import_slot;
        }
    } else if (v->op == XI_IMPORT_REF && v->aux) {
        const XiModule *import_module = NULL;
        int64_t import_slot = -1;
        const XiConstLiteral *import_lit = cg_import_ref_target_const_literal(
            ctx, (const XiImportRef *) v->aux, &import_module, &import_slot);
        if (!import_lit || import_lit->kind != XI_CONST_LITERAL_COMPTIME_AGGREGATE)
            return false;
        module = import_module;
        slot = import_slot;
    } else {
        return false;
    }

    if (!cg_freestanding_static_fixed_matrix_literal_in_module(ctx, module, slot, out_info, NULL))
        return false;
    const XiConstLiteral *lit = cg_module_const_literal(module, slot);
    if (cg_imported_static_const_needs_weak_symbol(ctx, module, lit)) {
        cg_report_imported_static_const_requires_weak(ctx, module, slot);
        ctx->error = true;
        return false;
    }
    if (out_slot)
        *out_slot = slot;
    if (out_module)
        *out_module = module;
    return true;
}

static bool cg_freestanding_static_fixed_matrix_index_value(XiCgenCtx *ctx, const XiValue *value,
                                                            CgStaticFixedMatrixInfo *out_info,
                                                            int64_t *out_slot,
                                                            const XiModule **out_module,
                                                            const XiValue **out_index) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || v->op != XI_INDEX_GET || v->nargs < 2)
        return false;
    if (!cg_freestanding_static_fixed_matrix_value_ex(ctx, v->args[0], out_info, out_slot,
                                                      out_module))
        return false;
    if (out_index)
        *out_index = v->args[1];
    return true;
}

static bool cg_static_fixed_cube_info_from_type(const XrType *type, CgStaticFixedCubeInfo *out) {
    if (!type || type->kind != XR_KIND_FIXED_ARRAY || !type->fixed_array.element_type ||
        type->fixed_array.length <= 0 || type->fixed_array.length > UINT16_MAX)
        return false;
    const XrType *plane_type = type->fixed_array.element_type;
    if (!plane_type || plane_type->kind != XR_KIND_FIXED_ARRAY ||
        plane_type->fixed_array.length <= 0 || plane_type->fixed_array.length > UINT16_MAX)
        return false;
    const XrType *row_type = plane_type->fixed_array.element_type;
    if (!row_type || row_type->kind != XR_KIND_FIXED_ARRAY || row_type->fixed_array.length <= 0 ||
        row_type->fixed_array.length > UINT16_MAX)
        return false;
    CgFixedArrayLaneInfo lane;
    if (!cg_fixed_array_lane_info_from_type(row_type, &lane) ||
        (lane.rep == XR_REP_TAGGED && lane.native_type != XR_NATIVE_STRING))
        return false;
    if (out)
        *out = (CgStaticFixedCubeInfo) {
            .lane = lane,
            .outer_count = (uint16_t) type->fixed_array.length,
            .middle_count = (uint16_t) plane_type->fixed_array.length,
            .inner_count = (uint16_t) row_type->fixed_array.length,
        };
    return true;
}

static bool cg_freestanding_static_fixed_cube_literal_in_module(XiCgenCtx *ctx,
                                                                const XiModule *module,
                                                                int64_t slot,
                                                                CgStaticFixedCubeInfo *out_info,
                                                                const XrCtValue **out_value) {
    if (!ctx || !ctx->freestanding_profile || !module || !module->slot_const_literals || slot < 0 ||
        slot >= module->nslots)
        return false;
    const XiConstLiteral *lit = &module->slot_const_literals[slot];
    if (lit->kind != XI_CONST_LITERAL_COMPTIME_AGGREGATE || !lit->ct_value ||
        lit->ct_value->kind != XR_CT_FIXED_ARRAY || !lit->type ||
        lit->type->kind != XR_KIND_FIXED_ARRAY)
        return false;
    CgStaticFixedCubeInfo info;
    if (!cg_static_fixed_cube_info_from_type(lit->type, &info))
        return false;
    const XrCtFixedArrayValue *outer = &lit->ct_value->as.fixed_array_val;
    if (outer->count != (int) info.outer_count || outer->count <= 0 || !outer->elements)
        return false;
    for (int plane_idx = 0; plane_idx < outer->count; plane_idx++) {
        const XrCtValue *plane_value = &outer->elements[plane_idx];
        if (!plane_value || plane_value->kind != XR_CT_FIXED_ARRAY)
            return false;
        const XrCtFixedArrayValue *plane = &plane_value->as.fixed_array_val;
        if (plane->count != (int) info.middle_count || plane->count <= 0 || !plane->elements)
            return false;
        for (int row_idx = 0; row_idx < plane->count; row_idx++) {
            const XrCtValue *row_value = &plane->elements[row_idx];
            if (!row_value || row_value->kind != XR_CT_FIXED_ARRAY)
                return false;
            const XrCtFixedArrayValue *row = &row_value->as.fixed_array_val;
            if (row->count != (int) info.inner_count || row->count <= 0 || !row->elements)
                return false;
            for (int col = 0; col < row->count; col++) {
                if (!cg_ct_static_fixed_array_value_supported(&row->elements[col], &info.lane))
                    return false;
            }
        }
    }
    if (out_info)
        *out_info = info;
    if (out_value)
        *out_value = lit->ct_value;
    return true;
}

static bool cg_freestanding_static_fixed_cube_literal(XiCgenCtx *ctx, int64_t slot,
                                                      CgStaticFixedCubeInfo *out_info,
                                                      const XrCtValue **out_value) {
    return cg_freestanding_static_fixed_cube_literal_in_module(ctx, ctx ? ctx->module : NULL, slot,
                                                               out_info, out_value);
}

static bool cg_freestanding_static_fixed_cube_value_ex(XiCgenCtx *ctx, const XiValue *value,
                                                       CgStaticFixedCubeInfo *out_info,
                                                       int64_t *out_slot,
                                                       const XiModule **out_module) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (out_module)
        *out_module = NULL;
    if (!v)
        return false;

    const XiModule *module = ctx ? ctx->module : NULL;
    int64_t slot = -1;
    if (v->op == XI_GET_SHARED) {
        slot = v->aux_int;
        const XiModule *import_module = NULL;
        int64_t import_slot = -1;
        const XiConstLiteral *import_lit =
            cg_import_slot_const_literal(ctx, NULL, (int) slot, &import_module, &import_slot);
        if (import_lit && import_lit->kind == XI_CONST_LITERAL_COMPTIME_AGGREGATE) {
            module = import_module;
            slot = import_slot;
        }
    } else if (v->op == XI_IMPORT_REF && v->aux) {
        const XiModule *import_module = NULL;
        int64_t import_slot = -1;
        const XiConstLiteral *import_lit = cg_import_ref_target_const_literal(
            ctx, (const XiImportRef *) v->aux, &import_module, &import_slot);
        if (!import_lit || import_lit->kind != XI_CONST_LITERAL_COMPTIME_AGGREGATE)
            return false;
        module = import_module;
        slot = import_slot;
    } else {
        return false;
    }

    if (!cg_freestanding_static_fixed_cube_literal_in_module(ctx, module, slot, out_info, NULL))
        return false;
    const XiConstLiteral *lit = cg_module_const_literal(module, slot);
    if (cg_imported_static_const_needs_weak_symbol(ctx, module, lit)) {
        cg_report_imported_static_const_requires_weak(ctx, module, slot);
        ctx->error = true;
        return false;
    }
    if (out_slot)
        *out_slot = slot;
    if (out_module)
        *out_module = module;
    return true;
}

static bool cg_freestanding_static_fixed_cube_outer_index_value(
    XiCgenCtx *ctx, const XiValue *value, CgStaticFixedCubeInfo *out_info, int64_t *out_slot,
    const XiModule **out_module, const XiValue **out_outer_index) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || v->op != XI_INDEX_GET || v->nargs < 2)
        return false;
    if (!cg_freestanding_static_fixed_cube_value_ex(ctx, v->args[0], out_info, out_slot,
                                                    out_module))
        return false;
    if (out_outer_index)
        *out_outer_index = v->args[1];
    return true;
}

static bool cg_freestanding_static_fixed_cube_index_value(XiCgenCtx *ctx, const XiValue *value,
                                                          CgStaticFixedCubeInfo *out_info,
                                                          int64_t *out_slot,
                                                          const XiModule **out_module,
                                                          const XiValue **out_outer_index,
                                                          const XiValue **out_middle_index) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || v->op != XI_INDEX_GET || v->nargs < 2)
        return false;
    if (!cg_freestanding_static_fixed_cube_outer_index_value(ctx, v->args[0], out_info, out_slot,
                                                             out_module, out_outer_index))
        return false;
    if (out_middle_index)
        *out_middle_index = v->args[1];
    return true;
}

static bool cg_static_fixed_struct_array_info_from_type(const XrType *type,
                                                        CgStaticFixedStructArrayInfo *out) {
    if (!type || type->kind != XR_KIND_FIXED_ARRAY || !type->fixed_array.element_type ||
        type->fixed_array.length <= 0 || type->fixed_array.length > UINT16_MAX)
        return false;
    const XrAggregateLayout *sl = cg_type_struct_layout(type->fixed_array.element_type);
    if (!sl)
        return false;
    if (out)
        *out = (CgStaticFixedStructArrayInfo) {
            .layout = sl,
            .count = (uint16_t) type->fixed_array.length,
        };
    return true;
}

static bool cg_freestanding_static_fixed_struct_array_literal_in_module(
    XiCgenCtx *ctx, const XiModule *module, int64_t slot, CgStaticFixedStructArrayInfo *out_info,
    const XrCtValue **out_value) {
    if (!ctx || !ctx->freestanding_profile || !module || !module->slot_const_literals || slot < 0 ||
        slot >= module->nslots)
        return false;
    const XiConstLiteral *lit = &module->slot_const_literals[slot];
    if (lit->kind != XI_CONST_LITERAL_COMPTIME_AGGREGATE || !lit->ct_value ||
        lit->ct_value->kind != XR_CT_FIXED_ARRAY || !lit->type ||
        lit->type->kind != XR_KIND_FIXED_ARRAY)
        return false;
    CgStaticFixedStructArrayInfo info;
    if (!cg_static_fixed_struct_array_info_from_type(lit->type, &info))
        return false;
    const XrCtFixedArrayValue *array = &lit->ct_value->as.fixed_array_val;
    if (array->count != (int) info.count || array->count <= 0 || !array->elements)
        return false;
    for (int i = 0; i < array->count; i++) {
        const XrCtValue *elem = &array->elements[i];
        if (!elem || elem->kind != XR_CT_STRUCT_VALUE ||
            !cg_static_struct_ct_layout_supported_depth(info.layout, &elem->as.struct_val, 0))
            return false;
    }
    if (out_info)
        *out_info = info;
    if (out_value)
        *out_value = lit->ct_value;
    return true;
}

static bool
cg_freestanding_static_fixed_struct_array_value_ex(XiCgenCtx *ctx, const XiValue *value,
                                                   CgStaticFixedStructArrayInfo *out_info,
                                                   int64_t *out_slot, const XiModule **out_module) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (out_module)
        *out_module = NULL;
    if (!v)
        return false;

    const XiModule *module = ctx ? ctx->module : NULL;
    int64_t slot = -1;
    if (v->op == XI_GET_SHARED) {
        slot = v->aux_int;
        const XiModule *import_module = NULL;
        int64_t import_slot = -1;
        const XiConstLiteral *import_lit =
            cg_import_slot_const_literal(ctx, NULL, (int) slot, &import_module, &import_slot);
        if (import_lit && import_lit->kind == XI_CONST_LITERAL_COMPTIME_AGGREGATE) {
            module = import_module;
            slot = import_slot;
        }
    } else if (v->op == XI_IMPORT_REF && v->aux) {
        const XiModule *import_module = NULL;
        int64_t import_slot = -1;
        const XiConstLiteral *import_lit = cg_import_ref_target_const_literal(
            ctx, (const XiImportRef *) v->aux, &import_module, &import_slot);
        if (!import_lit || import_lit->kind != XI_CONST_LITERAL_COMPTIME_AGGREGATE)
            return false;
        module = import_module;
        slot = import_slot;
    } else {
        return false;
    }

    if (!cg_freestanding_static_fixed_struct_array_literal_in_module(ctx, module, slot, out_info,
                                                                     NULL))
        return false;
    const XiConstLiteral *lit = cg_module_const_literal(module, slot);
    if (cg_imported_static_const_needs_weak_symbol(ctx, module, lit)) {
        cg_report_imported_static_const_requires_weak(ctx, module, slot);
        ctx->error = true;
        return false;
    }
    if (out_slot)
        *out_slot = slot;
    if (out_module)
        *out_module = module;
    return true;
}

static bool cg_freestanding_static_fixed_struct_array_index_value(
    XiCgenCtx *ctx, const XiValue *value, CgStaticFixedStructArrayInfo *out_info, int64_t *out_slot,
    const XiModule **out_module, const XiValue **out_index) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || v->op != XI_INDEX_GET || v->nargs < 2)
        return false;
    if (!cg_freestanding_static_fixed_struct_array_value_ex(ctx, v->args[0], out_info, out_slot,
                                                            out_module))
        return false;
    if (out_index)
        *out_index = v->args[1];
    return true;
}

static void cg_emit_static_fixed_struct_array_type(FILE *out, const XrAggregateLayout *sl,
                                                   const char *prefix) {
    emit_static_aggregate_decl_head(out, sl);
    for (uint16_t i = 0; sl && i < sl->field_count; i++) {
        char fname[128];
        cg_struct_field_c_name(sl, i, fname, sizeof(fname));
        emit_static_struct_field_decl(out, sl, i, fname, prefix);
        fprintf(out, "; ");
    }
    fprintf(out, "}");
}

static bool cg_emit_freestanding_static_fixed_struct_array_defs(XiCgenCtx *ctx, FILE *out,
                                                                const XiModule *module,
                                                                const char *prefix) {
    if (!ctx || !out || !module || !ctx->freestanding_profile || !module->slot_const_literals)
        return false;
    bool emitted = false;
    for (uint16_t slot = 0; slot < module->nslots; slot++) {
        CgStaticFixedStructArrayInfo info;
        const XrCtValue *value = NULL;
        if (!cg_freestanding_static_fixed_struct_array_literal_in_module(ctx, module, slot, &info,
                                                                         &value))
            continue;
        const XiConstLiteral *lit = &module->slot_const_literals[slot];
        const XrCtFixedArrayValue *array = &value->as.fixed_array_val;
        cg_emit_static_const_storage(out, lit);
        cg_emit_static_fixed_struct_array_type(out, info.layout, prefix);
        fprintf(out, " ");
        cg_emit_static_fixed_array_name(ctx, out, module, slot);
        fprintf(out, "[%u]", (unsigned) (info.count > 0 ? info.count : 1));
        emit_aot_const_data_attrs(out, lit);
        fprintf(out, " = {");
        for (int i = 0; i < array->count; i++) {
            if (i > 0)
                fprintf(out, ", ");
            cg_emit_static_struct_initializer(ctx, out, info.layout,
                                              &array->elements[i].as.struct_val);
        }
        fprintf(out, "};\n");
        emitted = true;
    }
    if (emitted)
        fprintf(out, "\n");
    return emitted;
}

static bool cg_emit_imported_static_fixed_struct_array_const_decl(XiCgenCtx *ctx, FILE *out,
                                                                  const XiModule *module,
                                                                  int64_t slot,
                                                                  const XiConstLiteral *lit) {
    if (!ctx || !out || !module || !lit || !lit->data_weak)
        return false;
    CgStaticFixedStructArrayInfo info;
    if (!cg_freestanding_static_fixed_struct_array_literal_in_module(ctx, module, slot, &info,
                                                                     NULL))
        return false;
    const char *prefix = module->name ? module->name : "mod";
    fprintf(out, "extern const ");
    cg_emit_static_fixed_struct_array_type(out, info.layout, prefix);
    fprintf(out, " ");
    cg_emit_static_fixed_array_name(ctx, out, module, slot);
    fprintf(out, "[%u];\n", (unsigned) info.count);
    return true;
}

static bool cg_static_fixed_tuple_array_info_from_type(const XrType *type,
                                                       CgStaticFixedTupleArrayInfo *out) {
    if (!type || type->kind != XR_KIND_FIXED_ARRAY || !type->fixed_array.element_type ||
        type->fixed_array.length <= 0 || type->fixed_array.length > UINT16_MAX)
        return false;
    XrType *tuple_type = type->fixed_array.element_type;
    if (!tuple_type || tuple_type->kind != XR_KIND_TUPLE ||
        cg_static_tuple_type_count(tuple_type) <= 0)
        return false;
    if (out)
        *out = (CgStaticFixedTupleArrayInfo) {
            .tuple_type = tuple_type,
            .count = (uint16_t) type->fixed_array.length,
        };
    return true;
}

static bool cg_freestanding_static_fixed_tuple_array_literal_in_module(
    XiCgenCtx *ctx, const XiModule *module, int64_t slot, CgStaticFixedTupleArrayInfo *out_info,
    const XrCtValue **out_value) {
    if (!ctx || !ctx->freestanding_profile || !module || !module->slot_const_literals || slot < 0 ||
        slot >= module->nslots)
        return false;
    const XiConstLiteral *lit = &module->slot_const_literals[slot];
    if (lit->kind != XI_CONST_LITERAL_COMPTIME_AGGREGATE || !lit->ct_value ||
        lit->ct_value->kind != XR_CT_FIXED_ARRAY || !lit->type ||
        lit->type->kind != XR_KIND_FIXED_ARRAY)
        return false;
    CgStaticFixedTupleArrayInfo info;
    if (!cg_static_fixed_tuple_array_info_from_type(lit->type, &info))
        return false;
    const XrCtFixedArrayValue *array = &lit->ct_value->as.fixed_array_val;
    if (array->count != (int) info.count || array->count <= 0 || !array->elements)
        return false;
    for (int i = 0; i < array->count; i++) {
        const XrCtValue *elem = &array->elements[i];
        if (!elem || elem->kind != XR_CT_TUPLE ||
            !cg_static_tuple_type_supported_depth(info.tuple_type, &elem->as.tuple_val, 0))
            return false;
    }
    if (out_info)
        *out_info = info;
    if (out_value)
        *out_value = lit->ct_value;
    return true;
}

static bool cg_freestanding_static_fixed_tuple_array_value_ex(XiCgenCtx *ctx, const XiValue *value,
                                                              CgStaticFixedTupleArrayInfo *out_info,
                                                              int64_t *out_slot,
                                                              const XiModule **out_module) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (out_module)
        *out_module = NULL;
    if (!v)
        return false;

    const XiModule *module = ctx ? ctx->module : NULL;
    int64_t slot = -1;
    if (v->op == XI_GET_SHARED) {
        slot = v->aux_int;
        const XiModule *import_module = NULL;
        int64_t import_slot = -1;
        const XiConstLiteral *import_lit =
            cg_import_slot_const_literal(ctx, NULL, (int) slot, &import_module, &import_slot);
        if (import_lit && import_lit->kind == XI_CONST_LITERAL_COMPTIME_AGGREGATE) {
            module = import_module;
            slot = import_slot;
        }
    } else if (v->op == XI_IMPORT_REF && v->aux) {
        const XiModule *import_module = NULL;
        int64_t import_slot = -1;
        const XiConstLiteral *import_lit = cg_import_ref_target_const_literal(
            ctx, (const XiImportRef *) v->aux, &import_module, &import_slot);
        if (!import_lit || import_lit->kind != XI_CONST_LITERAL_COMPTIME_AGGREGATE)
            return false;
        module = import_module;
        slot = import_slot;
    } else {
        return false;
    }

    if (!cg_freestanding_static_fixed_tuple_array_literal_in_module(ctx, module, slot, out_info,
                                                                    NULL))
        return false;
    const XiConstLiteral *lit = cg_module_const_literal(module, slot);
    if (cg_imported_static_const_needs_weak_symbol(ctx, module, lit)) {
        cg_report_imported_static_const_requires_weak(ctx, module, slot);
        ctx->error = true;
        return false;
    }
    if (out_slot)
        *out_slot = slot;
    if (out_module)
        *out_module = module;
    return true;
}

static bool cg_freestanding_static_fixed_tuple_array_index_value(
    XiCgenCtx *ctx, const XiValue *value, CgStaticFixedTupleArrayInfo *out_info, int64_t *out_slot,
    const XiModule **out_module, const XiValue **out_index) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || v->op != XI_INDEX_GET || v->nargs < 2)
        return false;
    if (!cg_freestanding_static_fixed_tuple_array_value_ex(ctx, v->args[0], out_info, out_slot,
                                                           out_module))
        return false;
    if (out_index)
        *out_index = v->args[1];
    return true;
}

static void cg_emit_static_fixed_tuple_array_type(FILE *out, XrType *tuple_type) {
    fprintf(out, "struct { ");
    int count = cg_static_tuple_type_count(tuple_type);
    for (uint16_t i = 0; i < (uint16_t) count; i++)
        cg_emit_static_tuple_field_decl(out, tuple_type, i);
    fprintf(out, "}");
}

static bool cg_emit_freestanding_static_fixed_tuple_array_defs(XiCgenCtx *ctx, FILE *out,
                                                               const XiModule *module) {
    if (!ctx || !out || !module || !ctx->freestanding_profile || !module->slot_const_literals)
        return false;
    bool emitted = false;
    for (uint16_t slot = 0; slot < module->nslots; slot++) {
        CgStaticFixedTupleArrayInfo info;
        const XrCtValue *value = NULL;
        if (!cg_freestanding_static_fixed_tuple_array_literal_in_module(ctx, module, slot, &info,
                                                                        &value))
            continue;
        const XiConstLiteral *lit = &module->slot_const_literals[slot];
        const XrCtFixedArrayValue *array = &value->as.fixed_array_val;
        cg_emit_static_const_storage(out, lit);
        cg_emit_static_fixed_tuple_array_type(out, info.tuple_type);
        fprintf(out, " ");
        cg_emit_static_fixed_array_name(ctx, out, module, slot);
        fprintf(out, "[%u]", (unsigned) info.count);
        emit_aot_const_data_attrs(out, lit);
        fprintf(out, " = {");
        for (int i = 0; i < array->count; i++) {
            if (i > 0)
                fprintf(out, ", ");
            cg_emit_static_tuple_initializer(ctx, out, info.tuple_type,
                                             &array->elements[i].as.tuple_val);
        }
        fprintf(out, "};\n");
        emitted = true;
    }
    if (emitted)
        fprintf(out, "\n");
    return emitted;
}

static bool cg_emit_imported_static_fixed_tuple_array_const_decl(XiCgenCtx *ctx, FILE *out,
                                                                 const XiModule *module,
                                                                 int64_t slot,
                                                                 const XiConstLiteral *lit) {
    if (!ctx || !out || !module || !lit || !lit->data_weak)
        return false;
    CgStaticFixedTupleArrayInfo info;
    if (!cg_freestanding_static_fixed_tuple_array_literal_in_module(ctx, module, slot, &info, NULL))
        return false;
    fprintf(out, "extern const ");
    cg_emit_static_fixed_tuple_array_type(out, info.tuple_type);
    fprintf(out, " ");
    cg_emit_static_fixed_array_name(ctx, out, module, slot);
    fprintf(out, "[%u];\n", (unsigned) info.count);
    return true;
}

#define CG_STATIC_FIXED_TUPLE_ARRAY_PATH_MAX CG_STATIC_TUPLE_PATH_MAX

typedef struct CgStaticFixedTupleArrayPath {
    const XiModule *module;
    int64_t slot;
    const XiValue *access;
    const XiValue *index;
    XrType *type;
    uint16_t count;
    uint16_t depth;
    int64_t fields[CG_STATIC_FIXED_TUPLE_ARRAY_PATH_MAX];
} CgStaticFixedTupleArrayPath;

static bool cg_static_fixed_tuple_array_path_append_field(CgStaticFixedTupleArrayPath *path,
                                                          int64_t field_idx) {
    if (!path || !path->type || path->depth >= CG_STATIC_FIXED_TUPLE_ARRAY_PATH_MAX ||
        field_idx < 0 || field_idx >= cg_static_tuple_type_count(path->type))
        return false;
    XrType *elem_type = cg_static_tuple_type_element(path->type, (int) field_idx);
    if (!elem_type || elem_type->kind != XR_KIND_TUPLE)
        return false;
    path->fields[path->depth++] = field_idx;
    path->type = elem_type;
    return true;
}

static bool cg_freestanding_static_fixed_tuple_array_object_path_depth(
    XiCgenCtx *ctx, const XiValue *value, CgStaticFixedTupleArrayPath *out_path, int depth) {
    if (!ctx || !value || depth > CG_STATIC_FIXED_TUPLE_ARRAY_PATH_MAX)
        return false;
    const XiValue *v = cg_unwrap_identity_value(value);
    CgStaticFixedTupleArrayInfo info;
    int64_t slot = -1;
    const XiModule *module = NULL;
    const XiValue *index = NULL;
    if (cg_freestanding_static_fixed_tuple_array_index_value(ctx, v, &info, &slot, &module,
                                                             &index)) {
        if (out_path)
            *out_path = (CgStaticFixedTupleArrayPath) {
                .module = module,
                .slot = slot,
                .access = v,
                .index = index,
                .type = info.tuple_type,
                .count = info.count,
                .depth = 0,
            };
        return true;
    }
    if (!v || (v->op != XI_TUPLE_GET && v->op != XI_LOAD_FIELD) || v->nargs < 1)
        return false;
    CgStaticFixedTupleArrayPath parent = {0};
    if (!cg_freestanding_static_fixed_tuple_array_object_path_depth(ctx, v->args[0], &parent,
                                                                    depth + 1))
        return false;
    int64_t field_idx = -1;
    if (!cg_static_tuple_field_access_index(v, &field_idx) ||
        !cg_static_fixed_tuple_array_path_append_field(&parent, field_idx))
        return false;
    if (out_path)
        *out_path = parent;
    return true;
}

static bool
cg_freestanding_static_fixed_tuple_array_object_path(XiCgenCtx *ctx, const XiValue *value,
                                                     CgStaticFixedTupleArrayPath *out_path) {
    return cg_freestanding_static_fixed_tuple_array_object_path_depth(ctx, value, out_path, 0);
}

static void cg_emit_static_fixed_tuple_array_path_lvalue(XiCgenCtx *ctx, FILE *out,
                                                         const CgStaticFixedTupleArrayPath *path,
                                                         const char *tmp_index) {
    if (!path)
        return;
    cg_emit_static_fixed_array_name(ctx, out, path->module, path->slot);
    fprintf(out, "[");
    if (tmp_index)
        fprintf(out, "%s", tmp_index);
    else
        emit_array_i64_arg(out, path->index);
    fprintf(out, "]");
    for (uint16_t i = 0; i < path->depth; i++)
        fprintf(out, ".f%" PRId64, path->fields[i]);
}

static void cg_emit_static_fixed_tuple_array_oob_fallback(FILE *out, XrRep rep) {
    if (rep == XR_REP_F64)
        fprintf(out, "0.0");
    else if (rep == XR_REP_TAGGED)
        fprintf(out, "XR_NULL_VAL");
    else
        fprintf(out, "0");
}

static bool emit_static_fixed_tuple_array_get_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    if (!v || (v->op != XI_TUPLE_GET && v->op != XI_LOAD_FIELD) || v->nargs < 1)
        return false;
    CgStaticFixedTupleArrayPath receiver = {0};
    int64_t field_idx = -1;
    if (!cg_freestanding_static_fixed_tuple_array_object_path(ctx, v->args[0], &receiver) ||
        !cg_static_tuple_field_access_index(v, &field_idx) || field_idx < 0 ||
        field_idx >= cg_static_tuple_type_count(receiver.type))
        return false;
    XrType *elem_type = cg_static_tuple_type_element(receiver.type, (int) field_idx);
    if (elem_type && elem_type->kind == XR_KIND_TUPLE) {
        fprintf(stderr,
                "[xi_cgen] ERROR: freestanding static tuple-array nested tuple value must be "
                "consumed through scalar/string .N fields before runtime lowering\n");
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return true;
    }
    uint8_t native_type = XR_NATIVE_VALUE;
    if (!cg_static_tuple_native_for_type(elem_type, &native_type))
        return false;
    XrRep elem_rep = cg_struct_native_rep(native_type);
    bool unchecked = cg_fixed_array_index_bounds_proven(receiver.access, receiver.count);
    const char *conv_suffix =
        emit_conversion_prefix(out, v->type, elem_rep, cg_value_plan_storage_rep(ctx, v));
    if (!unchecked) {
        fprintf(out, "({ int64_t _idx = ");
        emit_value_as_rep_ctx(ctx, out, receiver.index, XR_REP_I64);
        fprintf(out, "; XR_LIKELY(_idx >= 0 && _idx < %u) ? ", (unsigned) receiver.count);
    }
    if (elem_rep == XR_REP_F64)
        fprintf(out, "(double)");
    else if (elem_rep == XR_REP_I64)
        fprintf(out, "(int64_t)");
    cg_emit_static_fixed_tuple_array_path_lvalue(ctx, out, &receiver, unchecked ? NULL : "_idx");
    fprintf(out, ".f%" PRId64, field_idx);
    if (!unchecked) {
        fprintf(out, " : (xrt_fixed_index_oob(_idx, %u), ", (unsigned) receiver.count);
        cg_emit_static_fixed_tuple_array_oob_fallback(out, elem_rep);
        fprintf(out, "); })");
    }
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool cg_static_fixed_tuple_array_index_ref_safe_uses(XiCgenCtx *ctx, const XiFunc *f,
                                                            const XiValue *target, int depth);
static bool cg_static_fixed_tuple_array_tuple_ref_safe_uses(XiCgenCtx *ctx, const XiFunc *f,
                                                            const XiValue *target, int depth);

static bool cg_static_fixed_tuple_array_const_ref_safe_uses(XiCgenCtx *ctx, const XiFunc *f,
                                                            const XiValue *target, int depth) {
    if (!ctx || !f || !target || depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t k = 0; k < phi->value.nargs; k++) {
                if (phi->value.args[k] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] != target)
                    continue;
                if (v->op == XI_INDEX_GET && a == 0 &&
                    cg_freestanding_static_fixed_tuple_array_index_value(ctx, v, NULL, NULL, NULL,
                                                                         NULL)) {
                    if (!cg_static_fixed_tuple_array_index_ref_safe_uses(ctx, f, v, depth + 1))
                        return false;
                    continue;
                }
                if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && a == 0)
                    continue;
                if (cg_is_static_const_ref_alias(v) && a == 0) {
                    if (!cg_static_fixed_tuple_array_const_ref_safe_uses(ctx, f, v, depth + 1))
                        return false;
                    continue;
                }
                return false;
            }
        }
    }
    return true;
}

static bool cg_static_fixed_tuple_array_index_ref_safe_uses(XiCgenCtx *ctx, const XiFunc *f,
                                                            const XiValue *target, int depth) {
    if (!ctx || !f || !target || depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t k = 0; k < phi->value.nargs; k++) {
                if (phi->value.args[k] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] != target)
                    continue;
                if ((v->op == XI_TUPLE_GET || v->op == XI_LOAD_FIELD) && a == 0) {
                    if (cg_freestanding_static_fixed_tuple_array_object_path(ctx, v, NULL) &&
                        !cg_static_fixed_tuple_array_tuple_ref_safe_uses(ctx, f, v, depth + 1))
                        return false;
                    continue;
                }
                if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && a == 0)
                    continue;
                if (cg_is_static_const_ref_alias(v) && a == 0) {
                    if (!cg_static_fixed_tuple_array_index_ref_safe_uses(ctx, f, v, depth + 1))
                        return false;
                    continue;
                }
                return false;
            }
        }
    }
    return true;
}

static bool cg_static_fixed_tuple_array_tuple_ref_safe_uses(XiCgenCtx *ctx, const XiFunc *f,
                                                            const XiValue *target, int depth) {
    if (!ctx || !f || !target || depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t k = 0; k < phi->value.nargs; k++) {
                if (phi->value.args[k] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] != target)
                    continue;
                if ((v->op == XI_TUPLE_GET || v->op == XI_LOAD_FIELD) && a == 0) {
                    if (cg_freestanding_static_fixed_tuple_array_object_path(ctx, v, NULL) &&
                        !cg_static_fixed_tuple_array_tuple_ref_safe_uses(ctx, f, v, depth + 1))
                        return false;
                    continue;
                }
                if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && a == 0)
                    continue;
                if (cg_is_static_const_ref_alias(v) && a == 0) {
                    if (!cg_static_fixed_tuple_array_tuple_ref_safe_uses(ctx, f, v, depth + 1))
                        return false;
                    continue;
                }
                return false;
            }
        }
    }
    return true;
}

static bool cg_value_is_elided_static_fixed_tuple_array_const_ref(XiCgenCtx *ctx, const XiFunc *f,
                                                                  const XiValue *v) {
    if (!cg_freestanding_static_fixed_tuple_array_value_ex(ctx, v, NULL, NULL, NULL))
        return false;
    return cg_static_fixed_tuple_array_const_ref_safe_uses(ctx, f, v, 0);
}

static bool cg_value_is_elided_static_fixed_tuple_array_index_ref(XiCgenCtx *ctx, const XiFunc *f,
                                                                  const XiValue *v) {
    if (!cg_freestanding_static_fixed_tuple_array_index_value(ctx, v, NULL, NULL, NULL, NULL))
        return false;
    return cg_static_fixed_tuple_array_index_ref_safe_uses(ctx, f, v, 0);
}

static bool cg_value_is_elided_static_fixed_tuple_array_tuple_ref(XiCgenCtx *ctx, const XiFunc *f,
                                                                  const XiValue *v) {
    if (!cg_freestanding_static_fixed_tuple_array_object_path(ctx, v, NULL))
        return false;
    return cg_static_fixed_tuple_array_tuple_ref_safe_uses(ctx, f, v, 0);
}

static bool cg_static_fixed_array_ref_safe_uses(XiCgenCtx *ctx, const XiFunc *f,
                                                const XiValue *target, int depth) {
    if (!ctx || !f || !target || depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t k = 0; k < phi->value.nargs; k++) {
                if (phi->value.args[k] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] != target)
                    continue;
                if (v->op == XI_ARRAY_DATA_PTR && a == 0)
                    continue;
                if (v->op == XI_INDEX_GET && a == 0)
                    continue;
                if (cg_is_static_const_ref_alias(v) && a == 0) {
                    if (!cg_static_fixed_array_ref_safe_uses(ctx, f, v, depth + 1))
                        return false;
                    continue;
                }
                if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && a == 0)
                    continue;
                return false;
            }
        }
    }
    return true;
}

static bool cg_value_is_elided_static_fixed_array_const_ref(XiCgenCtx *ctx, const XiFunc *f,
                                                            const XiValue *v) {
    if (!cg_freestanding_static_fixed_array_value(ctx, v, NULL, NULL))
        return false;
    return cg_static_fixed_array_ref_safe_uses(ctx, f, v, 0);
}

static bool cg_static_fixed_matrix_index_ref_safe_uses(XiCgenCtx *ctx, const XiFunc *f,
                                                       const XiValue *target, int depth) {
    if (!ctx || !f || !target || depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t k = 0; k < phi->value.nargs; k++) {
                if (phi->value.args[k] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] != target)
                    continue;
                if (v->op == XI_INDEX_GET && a == 0)
                    continue;
                if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && a == 0)
                    continue;
                if (cg_is_static_const_ref_alias(v) && a == 0) {
                    if (!cg_static_fixed_matrix_index_ref_safe_uses(ctx, f, v, depth + 1))
                        return false;
                    continue;
                }
                return false;
            }
        }
    }
    return true;
}

static bool cg_static_fixed_matrix_const_ref_safe_uses(XiCgenCtx *ctx, const XiFunc *f,
                                                       const XiValue *target, int depth) {
    if (!ctx || !f || !target || depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t k = 0; k < phi->value.nargs; k++) {
                if (phi->value.args[k] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] != target)
                    continue;
                if (v->op == XI_INDEX_GET && a == 0 &&
                    cg_freestanding_static_fixed_matrix_index_value(ctx, v, NULL, NULL, NULL,
                                                                    NULL)) {
                    if (!cg_static_fixed_matrix_index_ref_safe_uses(ctx, f, v, depth + 1))
                        return false;
                    continue;
                }
                if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && a == 0)
                    continue;
                if (cg_is_static_const_ref_alias(v) && a == 0) {
                    if (!cg_static_fixed_matrix_const_ref_safe_uses(ctx, f, v, depth + 1))
                        return false;
                    continue;
                }
                return false;
            }
        }
    }
    return true;
}

static bool cg_value_is_elided_static_fixed_matrix_index_ref(XiCgenCtx *ctx, const XiFunc *f,
                                                             const XiValue *v) {
    if (!cg_freestanding_static_fixed_matrix_index_value(ctx, v, NULL, NULL, NULL, NULL))
        return false;
    return cg_static_fixed_matrix_index_ref_safe_uses(ctx, f, v, 0);
}

static bool cg_value_is_elided_static_fixed_matrix_const_ref(XiCgenCtx *ctx, const XiFunc *f,
                                                             const XiValue *v) {
    if (!cg_freestanding_static_fixed_matrix_value_ex(ctx, v, NULL, NULL, NULL))
        return false;
    return cg_static_fixed_matrix_const_ref_safe_uses(ctx, f, v, 0);
}

static bool cg_static_fixed_cube_index_ref_safe_uses(XiCgenCtx *ctx, const XiFunc *f,
                                                     const XiValue *target, int depth) {
    if (!ctx || !f || !target || depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t k = 0; k < phi->value.nargs; k++) {
                if (phi->value.args[k] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] != target)
                    continue;
                if (v->op == XI_INDEX_GET && a == 0)
                    continue;
                if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && a == 0)
                    continue;
                if (cg_is_static_const_ref_alias(v) && a == 0) {
                    if (!cg_static_fixed_cube_index_ref_safe_uses(ctx, f, v, depth + 1))
                        return false;
                    continue;
                }
                return false;
            }
        }
    }
    return true;
}

static bool cg_static_fixed_cube_outer_index_ref_safe_uses(XiCgenCtx *ctx, const XiFunc *f,
                                                           const XiValue *target, int depth) {
    if (!ctx || !f || !target || depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t k = 0; k < phi->value.nargs; k++) {
                if (phi->value.args[k] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] != target)
                    continue;
                if (v->op == XI_INDEX_GET && a == 0 &&
                    cg_freestanding_static_fixed_cube_index_value(ctx, v, NULL, NULL, NULL, NULL,
                                                                  NULL)) {
                    if (!cg_static_fixed_cube_index_ref_safe_uses(ctx, f, v, depth + 1))
                        return false;
                    continue;
                }
                if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && a == 0)
                    continue;
                if (cg_is_static_const_ref_alias(v) && a == 0) {
                    if (!cg_static_fixed_cube_outer_index_ref_safe_uses(ctx, f, v, depth + 1))
                        return false;
                    continue;
                }
                return false;
            }
        }
    }
    return true;
}

static bool cg_static_fixed_cube_const_ref_safe_uses(XiCgenCtx *ctx, const XiFunc *f,
                                                     const XiValue *target, int depth) {
    if (!ctx || !f || !target || depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t k = 0; k < phi->value.nargs; k++) {
                if (phi->value.args[k] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] != target)
                    continue;
                if (v->op == XI_INDEX_GET && a == 0 &&
                    cg_freestanding_static_fixed_cube_outer_index_value(ctx, v, NULL, NULL, NULL,
                                                                        NULL)) {
                    if (!cg_static_fixed_cube_outer_index_ref_safe_uses(ctx, f, v, depth + 1))
                        return false;
                    continue;
                }
                if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && a == 0)
                    continue;
                if (cg_is_static_const_ref_alias(v) && a == 0) {
                    if (!cg_static_fixed_cube_const_ref_safe_uses(ctx, f, v, depth + 1))
                        return false;
                    continue;
                }
                return false;
            }
        }
    }
    return true;
}

static bool cg_value_is_elided_static_fixed_cube_index_ref(XiCgenCtx *ctx, const XiFunc *f,
                                                           const XiValue *v) {
    if (!cg_freestanding_static_fixed_cube_index_value(ctx, v, NULL, NULL, NULL, NULL, NULL))
        return false;
    return cg_static_fixed_cube_index_ref_safe_uses(ctx, f, v, 0);
}

static bool cg_value_is_elided_static_fixed_cube_outer_index_ref(XiCgenCtx *ctx, const XiFunc *f,
                                                                 const XiValue *v) {
    if (!cg_freestanding_static_fixed_cube_outer_index_value(ctx, v, NULL, NULL, NULL, NULL))
        return false;
    return cg_static_fixed_cube_outer_index_ref_safe_uses(ctx, f, v, 0);
}

static bool cg_value_is_elided_static_fixed_cube_const_ref(XiCgenCtx *ctx, const XiFunc *f,
                                                           const XiValue *v) {
    if (!cg_freestanding_static_fixed_cube_value_ex(ctx, v, NULL, NULL, NULL))
        return false;
    return cg_static_fixed_cube_const_ref_safe_uses(ctx, f, v, 0);
}

static void cg_emit_static_fixed_array_name(XiCgenCtx *ctx, FILE *out, const XiModule *module,
                                            int64_t slot) {
    cg_emit_static_const_data_name(ctx, out, module, slot, "_xctarr");
}

static void cg_emit_static_fixed_array_i64(FILE *out, int64_t value) {
    if (value == INT64_MIN)
        fprintf(out, "INT64_MIN");
    else
        fprintf(out, "INT64_C(%" PRId64 ")", value);
}

static void cg_emit_static_fixed_array_value(XiCgenCtx *ctx, FILE *out,
                                             const CgFixedArrayLaneInfo *info,
                                             const XrCtValue *value) {
    if (info && info->native_type == XR_NATIVE_STRING) {
        cg_emit_static_str_value_initializer(
            ctx, out,
            value && value->kind == XR_CT_STRING && value->as.string_val ? value->as.string_val
                                                                         : "");
        return;
    }
    fprintf(out, "(%s)", info->ctype);
    switch (value->kind) {
        case XR_CT_FLOAT:
            emit_c_float_literal(out, value->as.float_val);
            return;
        case XR_CT_BOOL:
            fprintf(out, "%d", value->as.bool_val ? 1 : 0);
            return;
        case XR_CT_CHAR:
            fprintf(out, "0x%X", (unsigned) value->as.rune_val);
            return;
        case XR_CT_INT:
        default:
            cg_emit_static_fixed_array_i64(out, value->as.int_val);
            return;
    }
}

static bool cg_emit_freestanding_static_fixed_array_defs(XiCgenCtx *ctx, FILE *out,
                                                         const XiModule *module) {
    if (!ctx || !out || !module || !ctx->freestanding_profile || !module->slot_const_literals)
        return false;
    bool emitted = false;
    for (uint16_t slot = 0; slot < module->nslots; slot++) {
        CgFixedArrayLaneInfo info;
        const XrCtValue *value = NULL;
        if (!cg_freestanding_static_fixed_array_literal(ctx, slot, &info, &value))
            continue;
        const XiConstLiteral *lit = &module->slot_const_literals[slot];
        const XrCtFixedArrayValue *array = &value->as.fixed_array_val;
        cg_emit_static_const_storage(out, lit);
        fprintf(out, "%s ", info.ctype);
        cg_emit_static_fixed_array_name(ctx, out, module, slot);
        fprintf(out, "[%u]", (unsigned) info.count);
        emit_aot_const_data_attrs(out, lit);
        fprintf(out, " = {");
        for (int i = 0; i < array->count; i++) {
            if (i > 0)
                fprintf(out, ", ");
            if (array->is_byte_blob) {
                fprintf(out, "0x%02X", (unsigned) array->byte_blob[i]);
            } else {
                cg_emit_static_fixed_array_value(ctx, out, &info, &array->elements[i]);
            }
        }
        fprintf(out, "};\n");
        emitted = true;
    }
    if (emitted)
        fprintf(out, "\n");
    return emitted;
}

static bool cg_emit_freestanding_static_fixed_matrix_defs(XiCgenCtx *ctx, FILE *out,
                                                          const XiModule *module) {
    if (!ctx || !out || !module || !ctx->freestanding_profile || !module->slot_const_literals)
        return false;
    bool emitted = false;
    for (uint16_t slot = 0; slot < module->nslots; slot++) {
        CgStaticFixedMatrixInfo info;
        const XrCtValue *value = NULL;
        if (!cg_freestanding_static_fixed_matrix_literal(ctx, slot, &info, &value))
            continue;
        const XiConstLiteral *lit = &module->slot_const_literals[slot];
        const XrCtFixedArrayValue *outer = &value->as.fixed_array_val;
        cg_emit_static_const_storage(out, lit);
        fprintf(out, "%s ", info.lane.ctype);
        cg_emit_static_fixed_array_name(ctx, out, module, slot);
        fprintf(out, "[%u][%u]", (unsigned) info.outer_count, (unsigned) info.inner_count);
        emit_aot_const_data_attrs(out, lit);
        fprintf(out, " = {");
        for (int row = 0; row < outer->count; row++) {
            const XrCtFixedArrayValue *inner = &outer->elements[row].as.fixed_array_val;
            if (row > 0)
                fprintf(out, ", ");
            fprintf(out, "{");
            for (int col = 0; col < inner->count; col++) {
                if (col > 0)
                    fprintf(out, ", ");
                cg_emit_static_fixed_array_value(ctx, out, &info.lane, &inner->elements[col]);
            }
            fprintf(out, "}");
        }
        fprintf(out, "};\n");
        emitted = true;
    }
    if (emitted)
        fprintf(out, "\n");
    return emitted;
}

static bool cg_emit_freestanding_static_fixed_cube_defs(XiCgenCtx *ctx, FILE *out,
                                                        const XiModule *module) {
    if (!ctx || !out || !module || !ctx->freestanding_profile || !module->slot_const_literals)
        return false;
    bool emitted = false;
    for (uint16_t slot = 0; slot < module->nslots; slot++) {
        CgStaticFixedCubeInfo info;
        const XrCtValue *value = NULL;
        if (!cg_freestanding_static_fixed_cube_literal(ctx, slot, &info, &value))
            continue;
        const XiConstLiteral *lit = &module->slot_const_literals[slot];
        const XrCtFixedArrayValue *outer = &value->as.fixed_array_val;
        cg_emit_static_const_storage(out, lit);
        fprintf(out, "%s ", info.lane.ctype);
        cg_emit_static_fixed_array_name(ctx, out, module, slot);
        fprintf(out, "[%u][%u][%u]", (unsigned) info.outer_count, (unsigned) info.middle_count,
                (unsigned) info.inner_count);
        emit_aot_const_data_attrs(out, lit);
        fprintf(out, " = {");
        for (int plane_idx = 0; plane_idx < outer->count; plane_idx++) {
            const XrCtFixedArrayValue *plane = &outer->elements[plane_idx].as.fixed_array_val;
            if (plane_idx > 0)
                fprintf(out, ", ");
            fprintf(out, "{");
            for (int row_idx = 0; row_idx < plane->count; row_idx++) {
                const XrCtFixedArrayValue *row = &plane->elements[row_idx].as.fixed_array_val;
                if (row_idx > 0)
                    fprintf(out, ", ");
                fprintf(out, "{");
                for (int col = 0; col < row->count; col++) {
                    if (col > 0)
                        fprintf(out, ", ");
                    cg_emit_static_fixed_array_value(ctx, out, &info.lane, &row->elements[col]);
                }
                fprintf(out, "}");
            }
            fprintf(out, "}");
        }
        fprintf(out, "};\n");
        emitted = true;
    }
    if (emitted)
        fprintf(out, "\n");
    return emitted;
}

static void emit_fixed_array_lane_ptr_expr(XiCgenCtx *ctx, FILE *out, const XiValue *value,
                                           const CgFixedArrayLaneInfo *info) {
    const XiModule *static_module = NULL;
    int64_t static_slot = -1;
    if (ctx && ctx->freestanding_profile &&
        cg_freestanding_static_fixed_array_value_ex(ctx, value, NULL, &static_slot,
                                                    &static_module)) {
        cg_emit_static_fixed_array_name(ctx, out, static_module, static_slot);
        return;
    }
    if (info->stack_origin) {
        fprintf(out, "_fa%u", info->stack_origin->id);
        return;
    }
    if (cg_value_plan_storage_rep(ctx, value) == XR_REP_PTR) {
        fprintf(out, "((%s*)", info->ctype);
        emit_value_as_rep_ctx(ctx, out, value, XR_REP_PTR);
        fprintf(out, ")");
        return;
    }
    if (cg_value_plan_storage_rep(ctx, value) == XR_REP_RAWPTR) {
        fprintf(out, "((%s*)", info->ctype);
        emit_value_as_rep_ctx(ctx, out, value, XR_REP_RAWPTR);
        fprintf(out, ")");
        return;
    }
    fprintf(out, "((%s*)(", info->ctype);
    emit_value_as_rep_ctx(ctx, out, value, XR_REP_TAGGED);
    fprintf(out, ").ptr)");
}

static void emit_fixed_array_lane_load_prefix(FILE *out, const CgFixedArrayLaneInfo *info,
                                              XrRep target_rep) {
    if (info->rep == XR_REP_F64 && target_rep == XR_REP_F64 && info->native_type == XR_NATIVE_F64)
        fprintf(out, "(double)");
}

static void emit_fixed_array_lane_oob_fallback(FILE *out, const CgFixedArrayLaneInfo *info) {
    if (info->rep == XR_REP_F64)
        fprintf(out, "0.0");
    else if (info->rep == XR_REP_TAGGED)
        fprintf(out, "XR_NULL_VAL");
    else
        fprintf(out, "0");
}

static bool
cg_static_fixed_struct_array_field_result_supported(const XrAggregateFieldLayout *field) {
    return field && (field->native_type == XR_NATIVE_STRING ||
                     cg_static_struct_native_scalar_supported(field->native_type));
}

static bool cg_static_fixed_struct_array_field_value(XiCgenCtx *ctx, const XiValue *value,
                                                     CgStaticFixedStructArrayInfo *out_info,
                                                     int64_t *out_slot, const XiModule **out_module,
                                                     const XiValue **out_index,
                                                     int64_t *out_field_idx,
                                                     const XrAggregateFieldLayout **out_field) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || (v->op != XI_AGG_GET && v->op != XI_LOAD_FIELD) || v->nargs < 1)
        return false;
    CgStaticFixedStructArrayInfo info;
    int64_t slot = -1;
    const XiModule *module = NULL;
    const XiValue *index = NULL;
    if (!cg_freestanding_static_fixed_struct_array_index_value(ctx, v->args[0], &info, &slot,
                                                               &module, &index))
        return false;
    int64_t field_idx = -1;
    if (!cg_static_struct_field_access_index(v, info.layout, &field_idx))
        return false;
    const XrAggregateFieldLayout *field = &info.layout->fields[field_idx];
    if (!cg_static_fixed_struct_array_field_result_supported(field))
        return false;
    if (out_info)
        *out_info = info;
    if (out_slot)
        *out_slot = slot;
    if (out_module)
        *out_module = module;
    if (out_index)
        *out_index = index;
    if (out_field_idx)
        *out_field_idx = field_idx;
    if (out_field)
        *out_field = field;
    return true;
}

static void cg_emit_static_fixed_struct_array_field_lvalue_with_tmp(
    XiCgenCtx *ctx, FILE *out, const XiModule *module, int64_t slot, const XiValue *index,
    const char *tmp_index, const XrAggregateLayout *sl, int64_t field_idx) {
    char fname[128];
    cg_emit_static_fixed_array_name(ctx, out, module, slot);
    fprintf(out, "[");
    if (tmp_index)
        fprintf(out, "%s", tmp_index);
    else
        emit_array_i64_arg(out, index);
    fprintf(out, "]");
    cg_struct_field_c_name(sl, field_idx, fname, sizeof(fname));
    fprintf(out, ".%s", fname);
}

static void cg_emit_static_fixed_struct_array_field_lvalue(XiCgenCtx *ctx, FILE *out,
                                                           const XiModule *module, int64_t slot,
                                                           const XiValue *index, bool use_tmp_index,
                                                           const XrAggregateLayout *sl,
                                                           int64_t field_idx) {
    cg_emit_static_fixed_struct_array_field_lvalue_with_tmp(
        ctx, out, module, slot, index, use_tmp_index ? "_idx" : NULL, sl, field_idx);
}

static bool cg_static_fixed_struct_array_fixed_array_field_value(
    XiCgenCtx *ctx, const XiValue *value, CgStaticFixedStructArrayInfo *out_info, int64_t *out_slot,
    const XiModule **out_module, const XiValue **out_elem_access, const XiValue **out_elem_index,
    int64_t *out_field_idx, const XrAggregateFieldLayout **out_field) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || (v->op != XI_AGG_GET && v->op != XI_LOAD_FIELD) || v->nargs < 1)
        return false;
    CgStaticFixedStructArrayInfo info;
    int64_t slot = -1;
    const XiModule *module = NULL;
    const XiValue *elem_index = NULL;
    if (!cg_freestanding_static_fixed_struct_array_index_value(ctx, v->args[0], &info, &slot,
                                                               &module, &elem_index))
        return false;
    int64_t field_idx = -1;
    if (!cg_static_struct_field_access_index(v, info.layout, &field_idx))
        return false;
    const XrAggregateFieldLayout *field = &info.layout->fields[field_idx];
    if (!cg_static_struct_native_fixed_array_supported(field))
        return false;
    if (out_info)
        *out_info = info;
    if (out_slot)
        *out_slot = slot;
    if (out_module)
        *out_module = module;
    if (out_elem_access)
        *out_elem_access = cg_unwrap_identity_value(v->args[0]);
    if (out_elem_index)
        *out_elem_index = elem_index;
    if (out_field_idx)
        *out_field_idx = field_idx;
    if (out_field)
        *out_field = field;
    return true;
}

static bool cg_static_fixed_struct_array_nested_field_value(
    XiCgenCtx *ctx, const XiValue *value, CgStaticFixedStructArrayInfo *out_info, int64_t *out_slot,
    const XiModule **out_module, const XiValue **out_elem_access, const XiValue **out_elem_index,
    int64_t *out_parent_field_idx, const XrAggregateLayout **out_nested_layout) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || (v->op != XI_AGG_GET && v->op != XI_LOAD_FIELD) || v->nargs < 1)
        return false;
    CgStaticFixedStructArrayInfo info;
    int64_t slot = -1;
    const XiModule *module = NULL;
    const XiValue *elem_index = NULL;
    if (!cg_freestanding_static_fixed_struct_array_index_value(ctx, v->args[0], &info, &slot,
                                                               &module, &elem_index))
        return false;
    int64_t parent_field_idx = -1;
    if (!cg_static_struct_field_access_index(v, info.layout, &parent_field_idx))
        return false;
    const XrAggregateFieldLayout *field = &info.layout->fields[parent_field_idx];
    if (!cg_static_struct_native_nested_layout_supported(field))
        return false;
    if (out_info)
        *out_info = info;
    if (out_slot)
        *out_slot = slot;
    if (out_module)
        *out_module = module;
    if (out_elem_access)
        *out_elem_access = cg_unwrap_identity_value(v->args[0]);
    if (out_elem_index)
        *out_elem_index = elem_index;
    if (out_parent_field_idx)
        *out_parent_field_idx = parent_field_idx;
    if (out_nested_layout)
        *out_nested_layout = field->sub_layout;
    return true;
}

static void cg_emit_static_fixed_struct_array_nested_field_lvalue(
    XiCgenCtx *ctx, FILE *out, const XiModule *module, int64_t slot, const XiValue *index,
    const char *tmp_index, const XrAggregateLayout *parent, int64_t parent_field_idx,
    const XrAggregateLayout *nested, int64_t nested_field_idx) {
    char fname[128];
    cg_emit_static_fixed_struct_array_field_lvalue_with_tmp(ctx, out, module, slot, index,
                                                            tmp_index, parent, parent_field_idx);
    cg_struct_field_c_name(nested, nested_field_idx, fname, sizeof(fname));
    fprintf(out, ".%s", fname);
}

static bool cg_static_fixed_struct_array_nested_fixed_array_field_value(
    XiCgenCtx *ctx, const XiValue *value, CgStaticFixedStructArrayInfo *out_info, int64_t *out_slot,
    const XiModule **out_module, const XiValue **out_elem_access, const XiValue **out_elem_index,
    int64_t *out_parent_field_idx, const XrAggregateLayout **out_nested_layout,
    int64_t *out_nested_field_idx, const XrAggregateFieldLayout **out_field) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || (v->op != XI_AGG_GET && v->op != XI_LOAD_FIELD) || v->nargs < 1)
        return false;
    CgStaticFixedStructArrayInfo info;
    int64_t slot = -1;
    const XiModule *module = NULL;
    const XiValue *elem_access = NULL;
    const XiValue *elem_index = NULL;
    int64_t parent_field_idx = -1;
    const XrAggregateLayout *nested_layout = NULL;
    if (!cg_static_fixed_struct_array_nested_field_value(ctx, v->args[0], &info, &slot, &module,
                                                         &elem_access, &elem_index,
                                                         &parent_field_idx, &nested_layout) ||
        !nested_layout)
        return false;
    int64_t nested_field_idx = -1;
    if (!cg_static_struct_field_access_index(v, nested_layout, &nested_field_idx))
        return false;
    const XrAggregateFieldLayout *field = &nested_layout->fields[nested_field_idx];
    if (!cg_static_struct_native_fixed_array_supported(field))
        return false;
    if (out_info)
        *out_info = info;
    if (out_slot)
        *out_slot = slot;
    if (out_module)
        *out_module = module;
    if (out_elem_access)
        *out_elem_access = elem_access;
    if (out_elem_index)
        *out_elem_index = elem_index;
    if (out_parent_field_idx)
        *out_parent_field_idx = parent_field_idx;
    if (out_nested_layout)
        *out_nested_layout = nested_layout;
    if (out_nested_field_idx)
        *out_nested_field_idx = nested_field_idx;
    if (out_field)
        *out_field = field;
    return true;
}

static void
cg_emit_static_fixed_struct_array_field_oob_fallback(FILE *out,
                                                     const XrAggregateFieldLayout *field) {
    XrRep rep = field ? cg_struct_native_rep(field->native_type) : XR_REP_TAGGED;
    if (rep == XR_REP_F64)
        fprintf(out, "0.0");
    else if (rep == XR_REP_TAGGED)
        fprintf(out, "XR_NULL_VAL");
    else
        fprintf(out, "0");
}

static bool emit_static_fixed_struct_array_field_get_expr(XiCgenCtx *ctx, FILE *out,
                                                          const XiValue *v) {
    CgStaticFixedStructArrayInfo nested_info;
    int64_t nested_slot = -1;
    const XiModule *nested_module = NULL;
    const XiValue *nested_elem_access = NULL;
    const XiValue *nested_elem_index = NULL;
    int64_t nested_parent_field_idx = -1;
    const XrAggregateLayout *nested_layout = NULL;
    if (v && v->nargs >= 1 &&
        cg_static_fixed_struct_array_nested_field_value(
            ctx, v->args[0], &nested_info, &nested_slot, &nested_module, &nested_elem_access,
            &nested_elem_index, &nested_parent_field_idx, &nested_layout) &&
        nested_layout) {
        int64_t nested_field_idx = -1;
        if (!cg_static_struct_field_access_index(v, nested_layout, &nested_field_idx))
            return false;
        const XrAggregateFieldLayout *nested_field = &nested_layout->fields[nested_field_idx];
        if (!cg_static_fixed_struct_array_field_result_supported(nested_field))
            return false;
        XrRep field_rep = cg_struct_native_rep(nested_field->native_type);
        bool unchecked = cg_fixed_array_index_bounds_proven(nested_elem_access, nested_info.count);
        const char *conv_suffix =
            emit_conversion_prefix(out, v->type, field_rep, cg_value_plan_storage_rep(ctx, v));
        if (!unchecked) {
            fprintf(out, "({ int64_t _idx = ");
            emit_value_as_rep_ctx(ctx, out, nested_elem_index, XR_REP_I64);
            fprintf(out, "; XR_LIKELY(_idx >= 0 && _idx < %u) ? ", (unsigned) nested_info.count);
        }
        if (field_rep == XR_REP_F64)
            fprintf(out, "(double)");
        else if (field_rep == XR_REP_I64)
            fprintf(out, "(int64_t)");
        cg_emit_static_fixed_struct_array_nested_field_lvalue(
            ctx, out, nested_module, nested_slot, nested_elem_index, unchecked ? NULL : "_idx",
            nested_info.layout, nested_parent_field_idx, nested_layout, nested_field_idx);
        if (!unchecked) {
            fprintf(out, " : (xrt_fixed_index_oob(_idx, %u), ", (unsigned) nested_info.count);
            cg_emit_static_fixed_struct_array_field_oob_fallback(out, nested_field);
            fprintf(out, "); })");
        }
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }

    CgStaticFixedStructArrayInfo info;
    int64_t slot = -1;
    const XiModule *module = NULL;
    const XiValue *index = NULL;
    int64_t field_idx = -1;
    const XrAggregateFieldLayout *field = NULL;
    if (!cg_static_fixed_struct_array_field_value(ctx, v, &info, &slot, &module, &index, &field_idx,
                                                  &field))
        return false;

    XrRep field_rep = cg_struct_native_rep(field->native_type);
    bool unchecked = cg_fixed_array_index_bounds_proven(v->args[0], info.count);
    const char *conv_suffix =
        emit_conversion_prefix(out, v->type, field_rep, cg_value_plan_storage_rep(ctx, v));
    if (!unchecked) {
        fprintf(out, "({ int64_t _idx = ");
        emit_value_as_rep_ctx(ctx, out, index, XR_REP_I64);
        fprintf(out, "; XR_LIKELY(_idx >= 0 && _idx < %u) ? ", (unsigned) info.count);
    }
    if (field_rep == XR_REP_F64)
        fprintf(out, "(double)");
    else if (field_rep == XR_REP_I64)
        fprintf(out, "(int64_t)");
    cg_emit_static_fixed_struct_array_field_lvalue(ctx, out, module, slot, index, !unchecked,
                                                   info.layout, field_idx);
    if (!unchecked) {
        fprintf(out, " : (xrt_fixed_index_oob(_idx, %u), ", (unsigned) info.count);
        cg_emit_static_fixed_struct_array_field_oob_fallback(out, field);
        fprintf(out, "); })");
    }
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool cg_static_fixed_struct_array_fixed_array_field_ref_safe_uses(XiCgenCtx *ctx,
                                                                         const XiFunc *f,
                                                                         const XiValue *target,
                                                                         int depth);
static bool cg_static_fixed_struct_array_nested_field_ref_safe_uses(XiCgenCtx *ctx, const XiFunc *f,
                                                                    const XiValue *target,
                                                                    int depth);

static bool cg_static_fixed_struct_array_index_ref_safe_uses(XiCgenCtx *ctx, const XiFunc *f,
                                                             const XiValue *target, int depth) {
    if (!ctx || !f || !target || depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t k = 0; k < phi->value.nargs; k++) {
                if (phi->value.args[k] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] != target)
                    continue;
                if ((v->op == XI_AGG_GET || v->op == XI_LOAD_FIELD) && a == 0 &&
                    cg_static_fixed_struct_array_field_value(ctx, v, NULL, NULL, NULL, NULL, NULL,
                                                             NULL))
                    continue;
                if ((v->op == XI_AGG_GET || v->op == XI_LOAD_FIELD) && a == 0 &&
                    cg_static_fixed_struct_array_fixed_array_field_value(ctx, v, NULL, NULL, NULL,
                                                                         NULL, NULL, NULL, NULL)) {
                    if (!cg_static_fixed_struct_array_fixed_array_field_ref_safe_uses(ctx, f, v,
                                                                                      depth + 1))
                        return false;
                    continue;
                }
                if ((v->op == XI_AGG_GET || v->op == XI_LOAD_FIELD) && a == 0 &&
                    cg_static_fixed_struct_array_nested_field_value(ctx, v, NULL, NULL, NULL, NULL,
                                                                    NULL, NULL, NULL)) {
                    if (!cg_static_fixed_struct_array_nested_field_ref_safe_uses(ctx, f, v,
                                                                                 depth + 1))
                        return false;
                    continue;
                }
                if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && a == 0)
                    continue;
                if (cg_is_static_const_ref_alias(v) && a == 0) {
                    if (!cg_static_fixed_struct_array_index_ref_safe_uses(ctx, f, v, depth + 1))
                        return false;
                    continue;
                }
                return false;
            }
        }
    }
    return true;
}

static bool cg_static_fixed_struct_array_fixed_array_field_ref_safe_uses(XiCgenCtx *ctx,
                                                                         const XiFunc *f,
                                                                         const XiValue *target,
                                                                         int depth) {
    if (!ctx || !f || !target || depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t k = 0; k < phi->value.nargs; k++) {
                if (phi->value.args[k] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] != target)
                    continue;
                if (v->op == XI_INDEX_GET && a == 0)
                    continue;
                if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && a == 0)
                    continue;
                if (cg_is_static_const_ref_alias(v) && a == 0) {
                    if (!cg_static_fixed_struct_array_fixed_array_field_ref_safe_uses(ctx, f, v,
                                                                                      depth + 1))
                        return false;
                    continue;
                }
                return false;
            }
        }
    }
    return true;
}

static bool cg_static_fixed_struct_array_nested_field_ref_safe_uses(XiCgenCtx *ctx, const XiFunc *f,
                                                                    const XiValue *target,
                                                                    int depth) {
    if (!ctx || !f || !target || depth > 8)
        return false;
    const XrAggregateLayout *nested_layout = NULL;
    if (!cg_static_fixed_struct_array_nested_field_value(ctx, target, NULL, NULL, NULL, NULL, NULL,
                                                         NULL, &nested_layout) ||
        !nested_layout)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t k = 0; k < phi->value.nargs; k++) {
                if (phi->value.args[k] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] != target)
                    continue;
                if ((v->op == XI_AGG_GET || v->op == XI_LOAD_FIELD) && a == 0) {
                    int64_t field_idx = -1;
                    if (!cg_static_struct_field_access_index(v, nested_layout, &field_idx))
                        return false;
                    const XrAggregateFieldLayout *field = &nested_layout->fields[field_idx];
                    if (cg_static_struct_native_fixed_array_supported(field)) {
                        if (!cg_static_fixed_struct_array_fixed_array_field_ref_safe_uses(
                                ctx, f, v, depth + 1))
                            return false;
                        continue;
                    }
                    if (!cg_static_fixed_struct_array_field_result_supported(field))
                        return false;
                    continue;
                }
                if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && a == 0)
                    continue;
                if (cg_is_static_const_ref_alias(v) && a == 0) {
                    if (!cg_static_fixed_struct_array_nested_field_ref_safe_uses(ctx, f, v,
                                                                                 depth + 1))
                        return false;
                    continue;
                }
                return false;
            }
        }
    }
    return true;
}

static bool cg_static_fixed_struct_array_const_ref_safe_uses(XiCgenCtx *ctx, const XiFunc *f,
                                                             const XiValue *target, int depth) {
    if (!ctx || !f || !target || depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t k = 0; k < phi->value.nargs; k++) {
                if (phi->value.args[k] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] != target)
                    continue;
                if (v->op == XI_INDEX_GET && a == 0 &&
                    cg_freestanding_static_fixed_struct_array_index_value(ctx, v, NULL, NULL, NULL,
                                                                          NULL)) {
                    if (!cg_static_fixed_struct_array_index_ref_safe_uses(ctx, f, v, depth + 1))
                        return false;
                    continue;
                }
                if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && a == 0)
                    continue;
                if (cg_is_static_const_ref_alias(v) && a == 0) {
                    if (!cg_static_fixed_struct_array_const_ref_safe_uses(ctx, f, v, depth + 1))
                        return false;
                    continue;
                }
                return false;
            }
        }
    }
    return true;
}

static bool cg_value_is_elided_static_fixed_struct_array_index_ref(XiCgenCtx *ctx, const XiFunc *f,
                                                                   const XiValue *v) {
    if (!cg_freestanding_static_fixed_struct_array_index_value(ctx, v, NULL, NULL, NULL, NULL))
        return false;
    return cg_static_fixed_struct_array_index_ref_safe_uses(ctx, f, v, 0);
}

static bool cg_value_is_elided_static_fixed_struct_array_fixed_array_field_ref(XiCgenCtx *ctx,
                                                                               const XiFunc *f,
                                                                               const XiValue *v) {
    if (!cg_static_fixed_struct_array_fixed_array_field_value(ctx, v, NULL, NULL, NULL, NULL, NULL,
                                                              NULL, NULL))
        return false;
    return cg_static_fixed_struct_array_fixed_array_field_ref_safe_uses(ctx, f, v, 0);
}

static bool cg_value_is_elided_static_fixed_struct_array_nested_fixed_array_field_ref(
    XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    if (!cg_static_fixed_struct_array_nested_fixed_array_field_value(ctx, v, NULL, NULL, NULL, NULL,
                                                                     NULL, NULL, NULL, NULL, NULL))
        return false;
    return cg_static_fixed_struct_array_fixed_array_field_ref_safe_uses(ctx, f, v, 0);
}

static bool
cg_value_is_static_fixed_struct_array_nested_fixed_array_field_ref(XiCgenCtx *ctx,
                                                                   const XiValue *value) {
    return cg_static_fixed_struct_array_nested_fixed_array_field_value(
        ctx, value, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
}

static bool cg_value_is_elided_static_fixed_struct_array_nested_field_ref(XiCgenCtx *ctx,
                                                                          const XiFunc *f,
                                                                          const XiValue *v) {
    if (!cg_static_fixed_struct_array_nested_field_value(ctx, v, NULL, NULL, NULL, NULL, NULL, NULL,
                                                         NULL))
        return false;
    return cg_static_fixed_struct_array_nested_field_ref_safe_uses(ctx, f, v, 0);
}

static bool cg_value_is_elided_static_fixed_struct_array_const_ref(XiCgenCtx *ctx, const XiFunc *f,
                                                                   const XiValue *v) {
    if (!cg_freestanding_static_fixed_struct_array_value_ex(ctx, v, NULL, NULL, NULL))
        return false;
    return cg_static_fixed_struct_array_const_ref_safe_uses(ctx, f, v, 0);
}

static void emit_fixed_array_lane_store_value(XiCgenCtx *ctx, FILE *out,
                                              const CgFixedArrayLaneInfo *info,
                                              const XiValue *value) {
    if (info->rep == XR_REP_TAGGED) {
        emit_value_as_rep_ctx(ctx, out, value, XR_REP_TAGGED);
        return;
    }
    fprintf(out, "(%s)", info->ctype);
    emit_value_as_rep_ctx(ctx, out, value, info->rep);
}

static bool emit_fixed_array_index_get_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                            const XiValue *v) {
    CgFixedArrayLaneInfo info;
    if (!v || v->op != XI_INDEX_GET || v->nargs < 2)
        return false;

    (void) f;
    int64_t static_slot = -1;
    const XiModule *static_module = NULL;
    bool have_info = cg_fixed_array_lane_info_from_value(v->args[0], &info);
    bool static_slot_read = false;
    if (have_info) {
        static_slot_read = cg_freestanding_static_fixed_array_value_ex(
            ctx, v->args[0], &info, &static_slot, &static_module);
    } else {
        static_slot_read = cg_freestanding_static_fixed_array_value_ex(
            ctx, v->args[0], &info, &static_slot, &static_module);
        have_info = static_slot_read;
    }
    if (!have_info)
        return false;
    const XrAggregateLayout *static_struct_layout = NULL;
    int64_t static_struct_slot = -1;
    int64_t static_struct_field_idx = -1;
    const XrAggregateFieldLayout *static_struct_field = NULL;
    const XiModule *static_struct_module = NULL;
    bool static_struct_field_read = cg_freestanding_static_struct_fixed_array_field_value(
        ctx, v->args[0], &static_struct_layout, &static_struct_slot, &static_struct_field_idx,
        &static_struct_field, &static_struct_module);
    if (static_struct_field_read &&
        (static_struct_field->elem_count != info.count ||
         cg_struct_native_rep(static_struct_field->elem_native_type) != info.rep))
        static_struct_field_read = false;
    const XrAggregateLayout *static_struct_nested_parent = NULL;
    const XrAggregateLayout *static_struct_nested_layout = NULL;
    int64_t static_struct_nested_slot = -1;
    int64_t static_struct_nested_parent_field_idx = -1;
    int64_t static_struct_nested_field_idx = -1;
    const XrAggregateFieldLayout *static_struct_nested_field = NULL;
    const XiModule *static_struct_nested_module = NULL;
    bool static_struct_nested_field_read =
        cg_freestanding_static_struct_nested_fixed_array_field_value(
            ctx, v->args[0], &static_struct_nested_parent, &static_struct_nested_slot,
            &static_struct_nested_parent_field_idx, &static_struct_nested_layout,
            &static_struct_nested_field_idx, &static_struct_nested_field,
            &static_struct_nested_module);
    if (static_struct_nested_field_read &&
        (static_struct_nested_field->elem_count != info.count ||
         cg_struct_native_rep(static_struct_nested_field->elem_native_type) != info.rep))
        static_struct_nested_field_read = false;
    CgStaticFixedMatrixInfo static_matrix_info;
    int64_t static_matrix_slot = -1;
    const XiModule *static_matrix_module = NULL;
    const XiValue *static_matrix_outer_index = NULL;
    const XiValue *static_matrix_access = cg_unwrap_identity_value(v->args[0]);
    bool static_matrix_read = cg_freestanding_static_fixed_matrix_index_value(
        ctx, v->args[0], &static_matrix_info, &static_matrix_slot, &static_matrix_module,
        &static_matrix_outer_index);
    if (static_matrix_read &&
        (static_matrix_info.inner_count != info.count || static_matrix_info.lane.rep != info.rep ||
         static_matrix_info.lane.native_type != info.native_type))
        static_matrix_read = false;
    CgStaticFixedCubeInfo static_cube_info;
    int64_t static_cube_slot = -1;
    const XiModule *static_cube_module = NULL;
    const XiValue *static_cube_outer_index = NULL;
    const XiValue *static_cube_middle_index = NULL;
    const XiValue *static_cube_middle_access = cg_unwrap_identity_value(v->args[0]);
    const XiValue *static_cube_outer_access =
        (static_cube_middle_access && static_cube_middle_access->op == XI_INDEX_GET &&
         static_cube_middle_access->nargs >= 1)
            ? cg_unwrap_identity_value(static_cube_middle_access->args[0])
            : NULL;
    bool static_cube_read = cg_freestanding_static_fixed_cube_index_value(
        ctx, v->args[0], &static_cube_info, &static_cube_slot, &static_cube_module,
        &static_cube_outer_index, &static_cube_middle_index);
    if (static_cube_read &&
        (static_cube_info.inner_count != info.count || static_cube_info.lane.rep != info.rep ||
         static_cube_info.lane.native_type != info.native_type))
        static_cube_read = false;
    CgStaticFixedStructArrayInfo static_struct_array_info;
    int64_t static_struct_array_slot = -1;
    int64_t static_struct_array_field_idx = -1;
    const XiModule *static_struct_array_module = NULL;
    const XiValue *static_struct_array_access = NULL;
    const XiValue *static_struct_array_elem_index = NULL;
    const XrAggregateFieldLayout *static_struct_array_field = NULL;
    bool static_struct_array_field_read = cg_static_fixed_struct_array_fixed_array_field_value(
        ctx, v->args[0], &static_struct_array_info, &static_struct_array_slot,
        &static_struct_array_module, &static_struct_array_access, &static_struct_array_elem_index,
        &static_struct_array_field_idx, &static_struct_array_field);
    if (static_struct_array_field_read &&
        (static_struct_array_field->elem_count != info.count ||
         cg_struct_native_rep(static_struct_array_field->elem_native_type) != info.rep))
        static_struct_array_field_read = false;
    CgStaticFixedStructArrayInfo static_struct_array_nested_info;
    int64_t static_struct_array_nested_slot = -1;
    int64_t static_struct_array_nested_parent_field_idx = -1;
    int64_t static_struct_array_nested_field_idx = -1;
    const XiModule *static_struct_array_nested_module = NULL;
    const XiValue *static_struct_array_nested_access = NULL;
    const XiValue *static_struct_array_nested_elem_index = NULL;
    const XrAggregateLayout *static_struct_array_nested_layout = NULL;
    const XrAggregateFieldLayout *static_struct_array_nested_field = NULL;
    bool static_struct_array_nested_field_read =
        cg_static_fixed_struct_array_nested_fixed_array_field_value(
            ctx, v->args[0], &static_struct_array_nested_info, &static_struct_array_nested_slot,
            &static_struct_array_nested_module, &static_struct_array_nested_access,
            &static_struct_array_nested_elem_index, &static_struct_array_nested_parent_field_idx,
            &static_struct_array_nested_layout, &static_struct_array_nested_field_idx,
            &static_struct_array_nested_field);
    if (static_struct_array_nested_field_read &&
        (static_struct_array_nested_field->elem_count != info.count ||
         cg_struct_native_rep(static_struct_array_nested_field->elem_native_type) != info.rep))
        static_struct_array_nested_field_read = false;
    bool unchecked = cg_fixed_array_index_bounds_proven(v, info.count);
    XrRep target_rep = cg_value_plan_storage_rep(ctx, v);
    if (static_matrix_read) {
        bool outer_unchecked = cg_fixed_array_index_bounds_proven(static_matrix_access,
                                                                  static_matrix_info.outer_count);
        bool guarded = !outer_unchecked || !unchecked;
        const char *conv_suffix = emit_conversion_prefix(out, v->type, info.rep, target_rep);
        if (guarded)
            fprintf(out, "({ ");
        if (!outer_unchecked) {
            fprintf(out, "int64_t _outer_idx = ");
            emit_value_as_rep_ctx(ctx, out, static_matrix_outer_index, XR_REP_I64);
            fprintf(out,
                    "; if (XR_UNLIKELY(_outer_idx < 0 || _outer_idx >= %u)) "
                    "xrt_fixed_index_oob(_outer_idx, %u); ",
                    (unsigned) static_matrix_info.outer_count,
                    (unsigned) static_matrix_info.outer_count);
        }
        if (!unchecked) {
            fprintf(out, "int64_t _idx = ");
            emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
            fprintf(out,
                    "; if (XR_UNLIKELY(_idx < 0 || _idx >= %u)) "
                    "xrt_fixed_index_oob(_idx, %u); ",
                    (unsigned) info.count, (unsigned) info.count);
        }
        emit_fixed_array_lane_load_prefix(out, &info, target_rep);
        cg_emit_static_fixed_array_name(ctx, out, static_matrix_module, static_matrix_slot);
        fprintf(out, "[");
        if (outer_unchecked)
            emit_array_i64_arg(out, static_matrix_outer_index);
        else
            fprintf(out, "_outer_idx");
        fprintf(out, "][");
        if (unchecked)
            emit_array_i64_arg(out, v->args[1]);
        else
            fprintf(out, "_idx");
        fprintf(out, "]");
        if (guarded)
            fprintf(out, "; })");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }
    if (static_cube_read) {
        bool outer_unchecked = cg_fixed_array_index_bounds_proven(static_cube_outer_access,
                                                                  static_cube_info.outer_count);
        bool middle_unchecked = cg_fixed_array_index_bounds_proven(static_cube_middle_access,
                                                                   static_cube_info.middle_count);
        bool guarded = !outer_unchecked || !middle_unchecked || !unchecked;
        const char *conv_suffix = emit_conversion_prefix(out, v->type, info.rep, target_rep);
        if (guarded)
            fprintf(out, "({ ");
        if (!outer_unchecked) {
            fprintf(out, "int64_t _outer_idx = ");
            emit_value_as_rep_ctx(ctx, out, static_cube_outer_index, XR_REP_I64);
            fprintf(out,
                    "; if (XR_UNLIKELY(_outer_idx < 0 || _outer_idx >= %u)) "
                    "xrt_fixed_index_oob(_outer_idx, %u); ",
                    (unsigned) static_cube_info.outer_count,
                    (unsigned) static_cube_info.outer_count);
        }
        if (!middle_unchecked) {
            fprintf(out, "int64_t _middle_idx = ");
            emit_value_as_rep_ctx(ctx, out, static_cube_middle_index, XR_REP_I64);
            fprintf(out,
                    "; if (XR_UNLIKELY(_middle_idx < 0 || _middle_idx >= %u)) "
                    "xrt_fixed_index_oob(_middle_idx, %u); ",
                    (unsigned) static_cube_info.middle_count,
                    (unsigned) static_cube_info.middle_count);
        }
        if (!unchecked) {
            fprintf(out, "int64_t _idx = ");
            emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
            fprintf(out,
                    "; if (XR_UNLIKELY(_idx < 0 || _idx >= %u)) "
                    "xrt_fixed_index_oob(_idx, %u); ",
                    (unsigned) info.count, (unsigned) info.count);
        }
        emit_fixed_array_lane_load_prefix(out, &info, target_rep);
        cg_emit_static_fixed_array_name(ctx, out, static_cube_module, static_cube_slot);
        fprintf(out, "[");
        if (outer_unchecked)
            emit_array_i64_arg(out, static_cube_outer_index);
        else
            fprintf(out, "_outer_idx");
        fprintf(out, "][");
        if (middle_unchecked)
            emit_array_i64_arg(out, static_cube_middle_index);
        else
            fprintf(out, "_middle_idx");
        fprintf(out, "][");
        if (unchecked)
            emit_array_i64_arg(out, v->args[1]);
        else
            fprintf(out, "_idx");
        fprintf(out, "]");
        if (guarded)
            fprintf(out, "; })");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }
    if (static_struct_array_field_read) {
        bool outer_unchecked = cg_fixed_array_index_bounds_proven(static_struct_array_access,
                                                                  static_struct_array_info.count);
        bool guarded = !outer_unchecked || !unchecked;
        const char *conv_suffix = emit_conversion_prefix(out, v->type, info.rep, target_rep);
        if (guarded)
            fprintf(out, "({ ");
        if (!outer_unchecked) {
            fprintf(out, "int64_t _outer_idx = ");
            emit_value_as_rep_ctx(ctx, out, static_struct_array_elem_index, XR_REP_I64);
            fprintf(out,
                    "; if (XR_UNLIKELY(_outer_idx < 0 || _outer_idx >= %u)) "
                    "xrt_fixed_index_oob(_outer_idx, %u); ",
                    (unsigned) static_struct_array_info.count,
                    (unsigned) static_struct_array_info.count);
        }
        if (!unchecked) {
            fprintf(out, "int64_t _idx = ");
            emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
            fprintf(out,
                    "; if (XR_UNLIKELY(_idx < 0 || _idx >= %u)) "
                    "xrt_fixed_index_oob(_idx, %u); ",
                    (unsigned) info.count, (unsigned) info.count);
        }
        emit_fixed_array_lane_load_prefix(out, &info, target_rep);
        cg_emit_static_fixed_struct_array_field_lvalue_with_tmp(
            ctx, out, static_struct_array_module, static_struct_array_slot,
            static_struct_array_elem_index, outer_unchecked ? NULL : "_outer_idx",
            static_struct_array_info.layout, static_struct_array_field_idx);
        fprintf(out, "[");
        if (unchecked)
            emit_array_i64_arg(out, v->args[1]);
        else
            fprintf(out, "_idx");
        fprintf(out, "]");
        if (guarded)
            fprintf(out, "; })");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }
    if (static_struct_array_nested_field_read) {
        bool outer_unchecked = cg_fixed_array_index_bounds_proven(
            static_struct_array_nested_access, static_struct_array_nested_info.count);
        bool guarded = !outer_unchecked || !unchecked;
        const char *conv_suffix = emit_conversion_prefix(out, v->type, info.rep, target_rep);
        if (guarded)
            fprintf(out, "({ ");
        if (!outer_unchecked) {
            fprintf(out, "int64_t _outer_idx = ");
            emit_value_as_rep_ctx(ctx, out, static_struct_array_nested_elem_index, XR_REP_I64);
            fprintf(out,
                    "; if (XR_UNLIKELY(_outer_idx < 0 || _outer_idx >= %u)) "
                    "xrt_fixed_index_oob(_outer_idx, %u); ",
                    (unsigned) static_struct_array_nested_info.count,
                    (unsigned) static_struct_array_nested_info.count);
        }
        if (!unchecked) {
            fprintf(out, "int64_t _idx = ");
            emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
            fprintf(out,
                    "; if (XR_UNLIKELY(_idx < 0 || _idx >= %u)) "
                    "xrt_fixed_index_oob(_idx, %u); ",
                    (unsigned) info.count, (unsigned) info.count);
        }
        emit_fixed_array_lane_load_prefix(out, &info, target_rep);
        cg_emit_static_fixed_struct_array_nested_field_lvalue(
            ctx, out, static_struct_array_nested_module, static_struct_array_nested_slot,
            static_struct_array_nested_elem_index, outer_unchecked ? NULL : "_outer_idx",
            static_struct_array_nested_info.layout, static_struct_array_nested_parent_field_idx,
            static_struct_array_nested_layout, static_struct_array_nested_field_idx);
        fprintf(out, "[");
        if (unchecked)
            emit_array_i64_arg(out, v->args[1]);
        else
            fprintf(out, "_idx");
        fprintf(out, "]");
        if (guarded)
            fprintf(out, "; })");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }
    const char *conv_suffix = emit_conversion_prefix(out, v->type, info.rep, target_rep);
    if (!unchecked) {
        fprintf(out, "({ int64_t _idx = ");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
        fprintf(out, "; XR_LIKELY(_idx >= 0 && _idx < %u) ? ", (unsigned) info.count);
    }
    emit_fixed_array_lane_load_prefix(out, &info, target_rep);
    if (static_slot_read)
        cg_emit_static_fixed_array_name(ctx, out, static_module, static_slot);
    else if (static_struct_nested_field_read)
        cg_emit_static_struct_nested_field_lvalue(
            ctx, out, static_struct_nested_module, static_struct_nested_parent,
            static_struct_nested_slot, static_struct_nested_parent_field_idx,
            static_struct_nested_layout, static_struct_nested_field_idx);
    else if (static_struct_field_read)
        cg_emit_static_struct_field_lvalue_in_module(ctx, out, static_struct_module,
                                                     static_struct_layout, static_struct_slot,
                                                     static_struct_field_idx);
    else
        emit_fixed_array_lane_ptr_expr(ctx, out, v->args[0], &info);
    fprintf(out, "[");
    if (unchecked)
        emit_array_i64_arg(out, v->args[1]);
    else
        fprintf(out, "_idx");
    fprintf(out, "]");
    if (!unchecked) {
        fprintf(out, " : (xrt_fixed_index_oob(_idx, %u), ", (unsigned) info.count);
        emit_fixed_array_lane_oob_fallback(out, &info);
        fprintf(out, "); })");
    }
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool emit_fixed_array_index_set_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                            const XiValue *v) {
    CgFixedArrayLaneInfo info;
    if (!v || v->op != XI_INDEX_SET || v->nargs < 3 ||
        !cg_fixed_array_lane_info_from_value(v->args[0], &info))
        return false;

    bool unchecked = cg_fixed_array_index_bounds_proven(v, info.count);
    (void) f;
    fprintf(out, "({ ");
    if (!unchecked) {
        fprintf(out, "int64_t _idx = ");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
        fprintf(out, "; if (XR_UNLIKELY(_idx < 0 || _idx >= %u)) xrt_fixed_index_oob(_idx, %u); ",
                (unsigned) info.count, (unsigned) info.count);
    }
    emit_fixed_array_lane_ptr_expr(ctx, out, v->args[0], &info);
    fprintf(out, "[");
    if (unchecked)
        emit_array_i64_arg(out, v->args[1]);
    else
        fprintf(out, "_idx");
    fprintf(out, "] = ");
    emit_fixed_array_lane_store_value(ctx, out, &info, v->args[2]);
    fprintf(out, "; XR_NULL_VAL; })");
    return true;
}

static bool cg_span_elem_info_from_value(XiCgenCtx *ctx, const XiValue *value,
                                         CgArrayElemInfo *out) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || !v->type || v->type->kind != XR_KIND_SPAN)
        return false;
    return cg_array_elem_info_from_type_ctx(ctx, v->type, out);
}

static bool cg_array_get_const_int_literal(const XiValue *value, int64_t *out) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || v->op != XI_CONST || !v->type || v->type->kind != XR_KIND_INT || !out)
        return false;
    *out = v->aux_int;
    return true;
}

static void emit_array_i64_arg(FILE *out, const XiValue *value) {
    int64_t literal = 0;
    if (cg_array_get_const_int_literal(value, &literal)) {
        fprintf(out, "INT64_C(%" PRId64 ")", literal);
        return;
    }
    emit_value_as_rep(out, value, XR_REP_I64);
}

static bool cg_array_elem_info_from_container_plan(const XaotContainerElemPlan *plan,
                                                   CgArrayElemInfo *out) {
    if (!plan || !plan->elem_name || !plan->c_type || !out)
        return false;
    *out = (CgArrayElemInfo) {plan->type, plan->elem_name, plan->c_type, plan->storage_rep};
    return true;
}

static bool cg_array_elem_info_from_type(const XrType *type, CgArrayElemInfo *out) {
    XaotContainerPlan plan;
    return xaot_container_plan_for_type(type, &plan) && plan.kind == XAOT_CONTAINER_ARRAY &&
           cg_array_elem_info_from_container_plan(&plan.elem, out);
}

static bool cg_array_elem_info_from_type_ctx(XiCgenCtx *ctx, const XrType *type,
                                             CgArrayElemInfo *out) {
    const XaotContainerTypePlan *plan =
        xaot_bundle_find_container_plan(cg_ctx_aot_bundle(ctx), type);
    if (plan && plan->plan.kind == XAOT_CONTAINER_ARRAY)
        return cg_array_elem_info_from_container_plan(&plan->plan.elem, out);
    return cg_array_elem_info_from_type(type, out);
}

static bool cg_array_value_type_is_u8_contiguous(XiCgenCtx *ctx, const XiValue *value) {
    const XiValue *v = cg_unwrap_identity_value(value);
    CgArrayElemInfo info;
    return v && cg_array_elem_info_from_type_ctx(ctx, v->type, &info) &&
           cg_array_elem_info_is_u8(&info);
}

static bool cg_call_method_matches_receiver_registry_id(const XiValue *v,
                                                        XaBuiltinReceiverMethodId method_id) {
    const XaBuiltinReceiverMethodSpec *spec = xa_builtin_receiver_method_by_id(method_id);
    v = cg_unwrap_identity_value(v);
    return spec && v && (v->op == XI_CALL_METHOD || v->op == XI_CALL_METHOD_DIRECT) &&
           v->nargs >= 1 && v->args[0] &&
           cg_method_name_is(v, spec->source_name, cg_method_sym(spec->source_name)) &&
           cg_builtin_receiver_registry_matches(v->args[0]->type, spec->receiver);
}

static bool cg_array_elem_info_from_storage_plan(const XaotArrayStoragePlan *plan,
                                                 CgArrayElemInfo *out) {
    return plan && cg_array_elem_info_from_container_plan(&plan->elem, out);
}

static bool cg_array_elem_info_from_cache_plan(const XaotArrayCachePlan *plan,
                                               CgArrayElemInfo *out) {
    return plan && cg_array_elem_info_from_container_plan(&plan->elem, out);
}

typedef enum CgArrayStorageUse {
    CG_ARRAY_STORAGE_MUTABLE,
    CG_ARRAY_STORAGE_READ
} CgArrayStorageUse;

static bool cg_array_value_storage_info(XiCgenCtx *ctx, const XiFunc *f, const XiValue *array_value,
                                        CgArrayElemInfo *out, CgArrayStorageUse use) {
    const XaotArrayStoragePlan *plan;
    const XiValue *v = cg_unwrap_identity_value(array_value);
    uint32_t required_flag =
        use == CG_ARRAY_STORAGE_READ ? XAOT_ARRAY_STORAGE_READ : XAOT_ARRAY_STORAGE_MUTABLE;
    (void) f;
    if (v && v->type && v->type->kind == XR_KIND_FIXED_ARRAY)
        return false;
    plan = xaot_bundle_find_array_storage_plan(cg_ctx_aot_bundle(ctx), v);
    if (plan)
        return (plan->flags & required_flag) != 0 &&
               cg_array_elem_info_from_storage_plan(plan, out);
    if (v && v->type && xaot_type_contains_unresolved_type_param(v->type))
        return false;
    return v && cg_array_elem_info_from_type_ctx(ctx, v->type, out);
}

static bool cg_array_value_u8_unchecked_info(XiCgenCtx *ctx, const XiFunc *f,
                                             const XiValue *array_value, CgArrayElemInfo *out,
                                             CgArrayStorageUse use) {
    CgArrayElemInfo info;
    const XiValue *v = cg_unwrap_identity_value(array_value);
    if (cg_array_value_storage_info(ctx, f, array_value, &info, use) &&
        cg_array_elem_info_is_u8(&info)) {
        if (out)
            *out = info;
        return true;
    }
    if (v && cg_array_elem_info_from_type_ctx(ctx, v->type, &info) &&
        cg_array_elem_info_is_u8(&info)) {
        if (out)
            *out = info;
        return true;
    }
    return false;
}

static bool cg_array_value_has_fresh_owned_origin(XiCgenCtx *ctx, const XiValue *array_value) {
    const XaotArrayStoragePlan *plan = xaot_bundle_find_array_storage_plan(
        cg_ctx_aot_bundle(ctx), cg_unwrap_identity_value(array_value));
    const XiValue *origin = plan && plan->origin ? cg_unwrap_identity_value(plan->origin)
                                                 : cg_unwrap_identity_value(array_value);
    if (!origin)
        return false;
    if (origin->op == XI_ARRAY_NEW)
        return true;
    if (origin->op == XI_CALL_BUILTIN && origin->aux) {
        const char *name = (const char *) origin->aux;
        return strcmp(name, "array_copy_new") == 0 || strcmp(name, "array_with_capacity") == 0 ||
               strcmp(name, "array_filled_new") == 0;
    }
    return false;
}

static bool cg_array_index_get_reads_f32_storage(XiCgenCtx *ctx, const XiFunc *f,
                                                 const XiValue *v) {
    CgArrayElemInfo info;
    return v && v->op == XI_INDEX_GET && v->nargs >= 1 &&
           cg_array_value_storage_info(ctx, f, v->args[0], &info, CG_ARRAY_STORAGE_READ) &&
           strcmp(info.elem_name, "XR_ELEM_F32") == 0;
}

static bool cg_array_same_value(const XiValue *a, const XiValue *b) {
    const XiValue *ua = cg_unwrap_identity_value(a);
    const XiValue *ub = cg_unwrap_identity_value(b);
    return ua == ub;
}

typedef struct CgArrayFillLoop {
    const XiValue *origin;
    const XiValue *storage_value;
    const XiValue *cap_value;
    const XiValue *push;
    const XiValue *index_value;
    const XiValue *next_index_value;
    const XiBlock *exit_block;
} CgArrayFillLoop;

static uint16_t cg_array_pred_index(const XiBlock *blk, const XiBlock *pred) {
    if (!blk || !pred)
        return UINT16_MAX;
    for (uint16_t i = 0; i < blk->npreds; i++) {
        if (blk->preds[i] == pred)
            return i;
    }
    return UINT16_MAX;
}

static const XiValue *cg_array_single_origin(const XiValue *array_value, uint8_t depth) {
    const XiValue *v = cg_unwrap_identity_value(array_value);
    if (!v || depth > 8)
        return NULL;
    if (v->op == XI_ARRAY_NEW)
        return v;
    if (v->op == XI_CALL_BUILTIN) {
        const char *name = (const char *) v->aux;
        if (name && (strcmp(name, "array_new") == 0 || strcmp(name, "array_copy_new") == 0))
            return v;
    }
    if (v->op != XI_PHI)
        return NULL;

    const XiValue *origin = NULL;
    for (uint16_t i = 0; i < v->nargs; i++) {
        const XiValue *arg = cg_unwrap_identity_value(v->args[i]);
        if (arg == v)
            continue;
        const XiValue *arg_origin = cg_array_single_origin(arg, depth + 1);
        if (!arg_origin)
            return NULL;
        if (origin && origin != arg_origin)
            return NULL;
        origin = arg_origin;
    }
    return origin;
}

static const XiValue *cg_array_class_field_cached_origin(XiCgenCtx *ctx, const XiFunc *f,
                                                         const XiValue *value);

static const XiValue *cg_array_fill_receiver_origin(XiCgenCtx *ctx, const XiFunc *f,
                                                    const XiValue *push,
                                                    const XiValue **out_storage) {
    if (out_storage)
        *out_storage = NULL;
    if (!push || push->nargs < 1)
        return NULL;

    const XiValue *origin = cg_array_single_origin(push->args[0], 0);
    if (origin) {
        if (out_storage)
            *out_storage = origin;
        return origin;
    }

    origin = cg_array_class_field_cached_origin(ctx, f, push->args[0]);
    if (origin && out_storage)
        *out_storage = cg_unwrap_identity_value(push->args[0]);
    return origin;
}

static bool cg_array_value_available_at(const XiValue *value, const XiValue *site) {
    value = cg_unwrap_identity_value(value);
    if (!value || !site)
        return false;
    if (value->op == XI_PARAM)
        return true;
    if (value->op == XI_CONST)
        return true;
    if (value->block != site->block)
        return false;
    for (uint32_t i = 0; i < site->block->nvalues; i++) {
        const XiValue *cur = site->block->values[i];
        if (cur == value)
            return true;
        if (cur == site)
            return false;
    }
    return false;
}

static bool cg_array_const_int_value(const XiValue *v, int64_t expected) {
    v = cg_unwrap_identity_value(v);
    return v && v->op == XI_CONST && v->type && v->type->kind == XR_KIND_INT &&
           v->aux_int == expected;
}

static bool cg_array_fill_origin_starts_empty(const XiValue *origin) {
    origin = cg_unwrap_identity_value(origin);
    if (!origin)
        return false;
    if (origin->op == XI_ARRAY_NEW) {
        if (origin->nargs == 0)
            return true;
        return cg_array_const_int_value(origin->args[0], 0);
    }
    if (origin->op == XI_CALL_BUILTIN && origin->aux) {
        const char *name = (const char *) origin->aux;
        if (strcmp(name, "array_with_capacity") == 0)
            return true;
    }
    return false;
}

static bool cg_array_is_add_one_from_phi(const XiValue *v, const XiValue *phi) {
    v = cg_unwrap_identity_value(v);
    if (!v || !phi || v->op != XI_ADD || v->nargs < 2)
        return false;
    const XiValue *lhs = cg_unwrap_identity_value(v->args[0]);
    const XiValue *rhs = cg_unwrap_identity_value(v->args[1]);
    return (lhs == phi && cg_array_const_int_value(rhs, 1)) ||
           (rhs == phi && cg_array_const_int_value(lhs, 1));
}

static const XiValue *cg_array_phi_from_add_one(const XiValue *v) {
    v = cg_unwrap_identity_value(v);
    if (!v || v->op != XI_ADD || v->nargs < 2)
        return NULL;
    const XiValue *lhs = cg_unwrap_identity_value(v->args[0]);
    const XiValue *rhs = cg_unwrap_identity_value(v->args[1]);
    if (lhs && lhs->op == XI_PHI && cg_array_const_int_value(rhs, 1))
        return lhs;
    if (rhs && rhs->op == XI_PHI && cg_array_const_int_value(lhs, 1))
        return rhs;
    return NULL;
}

static bool cg_array_add_base_const_step(const XiValue *value, const XiValue *base,
                                         const XiValue **out_step) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || !base || v->op != XI_ADD || v->nargs < 2)
        return false;
    const XiValue *lhs = cg_unwrap_identity_value(v->args[0]);
    const XiValue *rhs = cg_unwrap_identity_value(v->args[1]);
    if (cg_array_same_value(lhs, base) && rhs && rhs->op == XI_CONST && rhs->type &&
        rhs->type->kind == XR_KIND_INT) {
        if (out_step)
            *out_step = rhs;
        return true;
    }
    if (cg_array_same_value(rhs, base) && lhs && lhs->op == XI_CONST && lhs->type &&
        lhs->type->kind == XR_KIND_INT) {
        if (out_step)
            *out_step = lhs;
        return true;
    }
    return false;
}

static bool emit_bool_accumulate_diamond_stmt(XiCgenCtx *ctx, FILE *out, const XiBlock *blk) {
    if (!ctx || !out || !blk || blk->kind != XI_BLOCK_IF || !blk->control ||
        !cg_value_type_is_bool(blk->control))
        return false;
    const XiBlock *then_blk = blk->succs[0];
    const XiBlock *merge_blk = blk->succs[1];
    if (!then_blk || !merge_blk || then_blk->kind != XI_BLOCK_PLAIN ||
        then_blk->succs[0] != merge_blk || !merge_blk->phis || merge_blk->phis->next ||
        then_blk->nvalues != 1)
        return false;
    const XiPhi *phi = merge_blk->phis;
    uint16_t pred_idx = cg_array_pred_index(merge_blk, blk);
    uint16_t then_idx = cg_array_pred_index(merge_blk, then_blk);
    if (pred_idx == UINT16_MAX || then_idx == UINT16_MAX || pred_idx >= phi->value.nargs ||
        then_idx >= phi->value.nargs || cg_value_plan_storage_rep(ctx, &phi->value) != XR_REP_I64)
        return false;
    const XiValue *base = phi->value.args[pred_idx];
    const XiValue *add = phi->value.args[then_idx];
    const XiValue *step = NULL;
    if (!base || !add ||
        cg_unwrap_identity_value(then_blk->values[0]) != cg_unwrap_identity_value(add) ||
        cg_value_plan_storage_rep(ctx, base) != XR_REP_I64 ||
        !cg_array_add_base_const_step(add, base, &step))
        return false;

    /* Route the destination through emit_vref so a coalesced accumulator phi
     * resolves to its representative C variable (this bypasses emit_phi_copies,
     * which would otherwise be the only phi-name remap site). */
    fprintf(out, "    ");
    emit_vref(out, &phi->value);
    fprintf(out, " = ");
    emit_value_as_rep_ctx(ctx, out, base, XR_REP_I64);
    fprintf(out, " + ((");
    emit_condition_expr_ctx(ctx, out, blk->control);
    fprintf(out, ") ? ");
    emit_value_as_rep_ctx(ctx, out, step, XR_REP_I64);
    fprintf(out, " : 0);\n");
    fprintf(out, "    goto L%u;\n", merge_blk->id);
    return true;
}

static const XiValue *cg_array_loop_bound_base(const XiValue *bound, const XiBlock *guard,
                                               const XiBlock *body) {
    const XiValue *v = cg_unwrap_identity_value(bound);
    if (!v)
        return NULL;
    if (v->op != XI_PHI || v->block != guard)
        return v;

    uint16_t body_idx = cg_array_pred_index(guard, body);
    if (body_idx == UINT16_MAX || body_idx >= v->nargs)
        return NULL;

    const XiValue *base = NULL;
    for (uint16_t i = 0; i < v->nargs; i++) {
        const XiValue *arg = cg_unwrap_identity_value(v->args[i]);
        if (i == body_idx) {
            if (arg != v)
                return NULL;
            continue;
        }
        if (!arg)
            return NULL;
        if (base && base != arg)
            return NULL;
        base = arg;
    }
    return base;
}

static bool cg_array_loop_index_is_counted(const XiValue *index, const XiBlock *guard,
                                           const XiBlock *body) {
    const XiValue *phi = cg_unwrap_identity_value(index);
    if (!phi || phi->op != XI_PHI || phi->block != guard)
        return false;
    uint16_t body_idx = cg_array_pred_index(guard, body);
    if (body_idx == UINT16_MAX || body_idx >= phi->nargs)
        return false;

    bool has_zero_base = false;
    for (uint16_t i = 0; i < phi->nargs; i++) {
        const XiValue *arg = cg_unwrap_identity_value(phi->args[i]);
        if (i != body_idx) {
            if (!cg_array_const_int_value(arg, 0))
                return false;
            has_zero_base = true;
            continue;
        }
        if (!cg_array_is_add_one_from_phi(arg, phi))
            return false;
    }
    return has_zero_base;
}

static bool cg_array_block_allows_unchecked_push(const XiBlock *body, const XiValue *push) {
    if (!body || !push)
        return false;
    bool seen_push = false;
    for (uint32_t i = 0; i < body->nvalues; i++) {
        const XiValue *v = body->values[i];
        if (!v)
            continue;
        if (v == push) {
            seen_push = true;
            continue;
        }
        if (!seen_push) {
            if (v->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM))
                return false;
            continue;
        }
        if ((v->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM)) && v->op != XI_ERR_CHECK)
            return false;
    }
    return seen_push;
}

static const XiBlock *cg_array_fill_loop_guard(const XiBlock *body) {
    const XiBlock *guard = NULL;
    if (!body || body->kind != XI_BLOCK_PLAIN || !body->succs[0])
        return NULL;
    for (uint16_t i = 0; i < body->npreds; i++) {
        const XiBlock *pred = body->preds[i];
        if (!pred || pred->kind != XI_BLOCK_IF || pred->succs[0] != body || body->succs[0] != pred)
            continue;
        if (guard && guard != pred)
            return NULL;
        guard = pred;
    }
    return guard;
}

static bool cg_array_loop_entry_checked(const XiBlock *loop, const XiBlock *backedge,
                                        const XiValue *cap_value) {
    if (!loop || !cap_value)
        return false;
    bool saw_entry = false;
    for (uint16_t i = 0; i < loop->npreds; i++) {
        const XiBlock *pred = loop->preds[i];
        if (pred == loop || pred == backedge)
            continue;
        if (!pred || pred->kind != XI_BLOCK_IF || pred->succs[0] != loop)
            return false;
        const XiValue *cond = cg_unwrap_identity_value(pred->control);
        if (!cond || cond->op != XI_LT || cond->nargs < 2)
            return false;
        if (!cg_array_const_int_value(cond->args[0], 0) ||
            !cg_array_same_value(cond->args[1], cap_value))
            return false;
        saw_entry = true;
    }
    return saw_entry;
}

static bool cg_array_single_block_entry_checked(const XiBlock *loop, const XiValue *cap_value) {
    return cg_array_loop_entry_checked(loop, loop, cap_value);
}

static bool cg_array_single_block_fill_loop_match(XiCgenCtx *ctx, const XiFunc *f,
                                                  const XiValue *push, CgArrayFillLoop *out) {
    const XiBlock *loop = push ? push->block : NULL;
    if (!loop || loop->kind != XI_BLOCK_IF || loop->succs[0] != loop)
        return false;
    if (!cg_array_block_allows_unchecked_push(loop, push))
        return false;
    const XiValue *cond = cg_unwrap_identity_value(loop->control);
    if (!cond || cond->op != XI_LT || cond->nargs < 2)
        return false;
    const XiValue *index = cg_array_phi_from_add_one(cond->args[0]);
    if (!index || !cg_array_loop_index_is_counted(index, loop, loop))
        return false;
    const XiValue *cap_value = cg_array_loop_bound_base(cond->args[1], loop, loop);
    if (!cap_value || !cg_array_single_block_entry_checked(loop, cap_value))
        return false;
    const XiValue *storage_value = NULL;
    const XiValue *origin = cg_array_fill_receiver_origin(ctx, f, push, &storage_value);
    if (!origin || !cg_array_fill_origin_starts_empty(origin) ||
        !cg_array_value_available_at(cap_value, origin))
        return false;
    if (out)
        *out = (CgArrayFillLoop) {origin, storage_value, cap_value,     push,
                                  index,  cond->args[0], loop->succs[1]};
    return true;
}

static bool cg_array_block_has_only_fill_loop_pure_values(const XiBlock *blk) {
    if (!blk)
        return false;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        const XiValue *v = blk->values[i];
        if (!v)
            continue;
        if ((v->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM)) && v->op != XI_ERR_CHECK)
            return false;
    }
    return true;
}

static bool cg_array_paths_reach_latch_without_effects(const XiBlock *blk, const XiBlock *latch,
                                                       uint8_t depth) {
    if (!blk || !latch || depth > 12)
        return false;
    if (!cg_array_block_has_only_fill_loop_pure_values(blk))
        return false;
    if (blk == latch)
        return true;
    if (blk->kind == XI_BLOCK_PLAIN)
        return blk->succs[0] &&
               cg_array_paths_reach_latch_without_effects(blk->succs[0], latch, depth + 1);
    if (blk->kind == XI_BLOCK_IF)
        return blk->succs[0] && blk->succs[1] &&
               cg_array_paths_reach_latch_without_effects(blk->succs[0], latch, depth + 1) &&
               cg_array_paths_reach_latch_without_effects(blk->succs[1], latch, depth + 1);
    return false;
}

static bool cg_array_header_paths_reach_latch(const XiBlock *header, const XiBlock *latch) {
    if (!header || !latch)
        return false;
    if (header->kind == XI_BLOCK_PLAIN)
        return header->succs[0] &&
               cg_array_paths_reach_latch_without_effects(header->succs[0], latch, 0);
    if (header->kind == XI_BLOCK_IF)
        return header->succs[0] && header->succs[1] &&
               cg_array_paths_reach_latch_without_effects(header->succs[0], latch, 0) &&
               cg_array_paths_reach_latch_without_effects(header->succs[1], latch, 0);
    return false;
}

static bool cg_array_branchy_fill_loop_match(XiCgenCtx *ctx, const XiFunc *f, const XiValue *push,
                                             CgArrayFillLoop *out) {
    const XiBlock *header = push ? push->block : NULL;
    if (!header || !cg_array_block_allows_unchecked_push(header, push))
        return false;

    for (uint16_t pi = 0; pi < header->npreds; pi++) {
        const XiBlock *latch = header->preds[pi];
        if (!latch || latch == header || latch->kind != XI_BLOCK_IF || latch->succs[0] != header)
            continue;

        const XiValue *cond = cg_unwrap_identity_value(latch->control);
        if (!cond || cond->op != XI_LT || cond->nargs < 2)
            continue;
        const XiValue *index = cg_array_phi_from_add_one(cond->args[0]);
        if (!index || !cg_array_loop_index_is_counted(index, header, latch))
            continue;
        const XiValue *cap_value = cg_array_loop_bound_base(cond->args[1], header, latch);
        if (!cap_value || !cg_array_loop_entry_checked(header, latch, cap_value))
            continue;
        if (!cg_array_header_paths_reach_latch(header, latch))
            continue;

        const XiValue *storage_value = NULL;
        const XiValue *origin = cg_array_fill_receiver_origin(ctx, f, push, &storage_value);
        if (!origin || !cg_array_fill_origin_starts_empty(origin) ||
            !cg_array_value_available_at(cap_value, origin))
            continue;
        if (out)
            *out = (CgArrayFillLoop) {origin, storage_value, cap_value,      push,
                                      index,  cond->args[0], latch->succs[1]};
        return true;
    }
    return false;
}

static bool cg_array_unrotated_branchy_fill_loop_match(XiCgenCtx *ctx, const XiFunc *f,
                                                       const XiValue *push, CgArrayFillLoop *out) {
    const XiBlock *body = push ? push->block : NULL;
    if (!body || !cg_array_block_allows_unchecked_push(body, push))
        return false;

    for (uint16_t pi = 0; pi < body->npreds; pi++) {
        const XiBlock *guard = body->preds[pi];
        if (!guard || guard->kind != XI_BLOCK_IF || guard->succs[0] != body)
            continue;
        const XiValue *cond = cg_unwrap_identity_value(guard->control);
        if (!cond || cond->op != XI_LT || cond->nargs < 2)
            continue;
        const XiValue *index = cg_unwrap_identity_value(cond->args[0]);
        if (!index)
            continue;

        for (uint16_t gi = 0; gi < guard->npreds; gi++) {
            const XiBlock *latch = guard->preds[gi];
            if (!latch || latch == guard || latch == body || latch->succs[0] != guard)
                continue;
            if (!cg_array_loop_index_is_counted(index, guard, latch))
                continue;
            const XiValue *cap_value = cg_array_loop_bound_base(cond->args[1], guard, latch);
            if (!cap_value)
                continue;
            if (!cg_array_header_paths_reach_latch(body, latch))
                continue;

            const XiValue *storage_value = NULL;
            const XiValue *origin = cg_array_fill_receiver_origin(ctx, f, push, &storage_value);
            if (!origin || !cg_array_fill_origin_starts_empty(origin) ||
                !cg_array_value_available_at(cap_value, origin))
                continue;
            if (out)
                *out = (CgArrayFillLoop) {origin, storage_value, cap_value,      push,
                                          index,  NULL,          guard->succs[1]};
            return true;
        }
    }
    return false;
}

static bool cg_array_fill_loop_match(XiCgenCtx *ctx, const XiFunc *f, const XiValue *push,
                                     CgArrayFillLoop *out) {
    if (!push || push->op != XI_CALL_METHOD || push->nargs != 2 || !push->block)
        return false;
    if (!f)
        f = push->block->func;
    if (!cg_call_method_matches_receiver_registry_id(push, XA_BUILTIN_RECEIVER_METHOD_ARRAY_PUSH))
        return false;
    if (push->xg_capacity_op_id != XG_NO_ID) {
        const XaotCapacityPlan *plan = xaot_bundle_find_capacity_plan(
            cg_ctx_aot_bundle(ctx), (XgCapacityOpId) push->xg_capacity_op_id);
        const uint32_t required = XAOT_CAPACITY_EV_EXACT_COUNT | XAOT_CAPACITY_EV_LOOP_APPEND |
                                  XAOT_CAPACITY_EV_NO_CLOBBER;
        if (!plan || plan->action != XAOT_CAPACITY_RESERVE_ONCE ||
            (plan->evidence & required) != required)
            return false;
    }
    if (cg_array_single_block_fill_loop_match(ctx, f, push, out))
        return true;
    if (cg_array_branchy_fill_loop_match(ctx, f, push, out))
        return true;
    if (cg_array_unrotated_branchy_fill_loop_match(ctx, f, push, out))
        return true;

    const XiBlock *body = push->block;
    if (!body || body->kind != XI_BLOCK_PLAIN || !body->succs[0])
        return false;
    const XiBlock *guard = cg_array_fill_loop_guard(body);
    if (!guard)
        return false;
    if (!cg_array_block_allows_unchecked_push(body, push))
        return false;

    const XiValue *cond = cg_unwrap_identity_value(guard->control);
    if (!cond || cond->op != XI_LT || cond->nargs < 2)
        return false;
    const XiValue *index = cg_unwrap_identity_value(cond->args[0]);
    if (!cg_array_loop_index_is_counted(index, guard, body))
        return false;
    const XiValue *cap_value = cg_array_loop_bound_base(cond->args[1], guard, body);
    if (!cap_value)
        return false;

    const XiValue *storage_value = NULL;
    const XiValue *origin = cg_array_fill_receiver_origin(ctx, f, push, &storage_value);
    if (!origin || !cg_array_fill_origin_starts_empty(origin) ||
        !cg_array_value_available_at(cap_value, origin))
        return false;
    if (out)
        *out = (CgArrayFillLoop) {origin, storage_value, cap_value,      push,
                                  index,  NULL,          guard->succs[1]};
    return true;
}

static bool cg_array_value_mutates_origin_directly(const XiValue *v, const XiValue *origin) {
    if (!v || !origin)
        return false;
    if (v->op == XI_INDEX_SET && v->nargs >= 1 && cg_array_single_origin(v->args[0], 0) == origin)
        return true;
    if (v->op == XI_CALL_METHOD && v->nargs >= 1 &&
        cg_array_single_origin(v->args[0], 0) == origin) {
        const char *method = (const char *) v->aux;
        if (method && (strcmp(method, "map") == 0 || strcmp(method, "filter") == 0 ||
                       strcmp(method, "reduce") == 0))
            return false;
        if (v->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM))
            return true;
    }
    return false;
}

static bool cg_array_origin_has_only_fill_mutation(const XiFunc *f, const XiValue *origin,
                                                   const XiValue *fill_push) {
    if (!f || !origin || !fill_push)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (v == fill_push)
                continue;
            if (cg_array_value_mutates_origin_directly(v, origin))
                return false;
        }
    }
    return true;
}

static bool cg_array_unique_fill_loop_for_origin(XiCgenCtx *ctx, const XiFunc *f,
                                                 const XiValue *origin, CgArrayFillLoop *out) {
    if (!origin || !origin->block || !origin->block->func)
        return false;
    if (!f)
        f = origin->block->func;
    CgArrayFillLoop found;
    bool have = false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            CgArrayFillLoop cur;
            const XiValue *v = blk->values[vi];
            if (!cg_array_fill_loop_match(ctx, f, v, &cur) || cur.origin != origin)
                continue;
            if (have)
                return false;
            found = cur;
            have = true;
        }
    }
    if (!have || !cg_array_origin_has_only_fill_mutation(f, origin, found.push))
        return false;
    if (out)
        *out = found;
    return true;
}

typedef struct CgArrayClassFieldAlloc {
    const XiValue *origin;
    const XiValue *store;
    CgClassNativeFunc class_info;
    uint16_t field_idx;
    CgArrayElemInfo elem;
} CgArrayClassFieldAlloc;

static bool cg_array_class_field_alloc_info(XiCgenCtx *ctx, const XiFunc *f, const XiValue *origin,
                                            CgArrayClassFieldAlloc *out) {
    const XaotArrayClassFieldAllocPlan *plan;
    CgArrayElemInfo elem;
    if (!ctx || ctx->pre_decl_all || !f || !origin)
        return false;
    plan = xaot_bundle_find_array_class_field_alloc_plan(cg_ctx_aot_bundle(ctx),
                                                         cg_unwrap_identity_value(origin));
    if (!plan || plan->func != f || !cg_array_elem_info_from_container_plan(&plan->elem, &elem))
        return false;

    if (out) {
        memset(out, 0, sizeof(*out));
        out->origin = plan->origin;
        out->store = plan->store;
        out->class_info.class_data = plan->class_data;
        out->class_info.func = f;
        out->class_info.layout = plan->class_data ? plan->class_data->instance_layout : NULL;
        out->class_info.class_name = plan->class_data ? plan->class_data->class_name : NULL;
        out->field_idx = plan->field_idx;
        out->elem = elem;
    }
    return true;
}

static bool cg_array_class_field_alloc_value_is_elided(XiCgenCtx *ctx, const XiFunc *f,
                                                       const XiValue *v) {
    CgArrayClassFieldAlloc info;
    return cg_array_class_field_alloc_info(ctx, f, v, &info);
}

static void emit_typed_array_data_cache_ref(FILE *out, const XiValue *origin);
static bool emit_typed_array_new_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                      int64_t cap);

static bool emit_typed_array_class_field_alloc_store_stmt(XiCgenCtx *ctx, FILE *out,
                                                          const XiFunc *f, const XiValue *store) {
    if (!store || store->op != XI_STORE_FIELD || store->nargs < 2)
        return false;
    const XiValue *origin = cg_array_single_origin(store->args[1], 0);
    CgArrayClassFieldAlloc info;
    const XaotArrayClassFieldAllocPlan *plan =
        xaot_bundle_find_array_class_field_alloc_plan_for_store(cg_ctx_aot_bundle(ctx), store);
    if (!plan || !cg_array_class_field_alloc_info(ctx, f, plan->origin, &info) ||
        info.store != store || (origin && origin != plan->origin))
        return false;

    fprintf(out, "    ");
    emit_class_native_receiver_field_ref(ctx, out, f, info.class_info.class_data, store->args[0],
                                         info.field_idx);
    fprintf(out, " = (xrt_array_t*)");
    if (!emit_typed_array_new_expr(ctx, out, f, origin, 4))
        return false;
    fprintf(out, ".ptr;\n");

    fprintf(out, "    %s *", info.elem.ctype);
    emit_typed_array_data_cache_ref(out, origin);
    /* Fresh allocation stored into the field above: XRT_DATA_ALIGN contract. */
    fprintf(out, " = (%s*)XR_ASSUME_ALIGNED(", info.elem.ctype);
    emit_class_native_receiver_field_ref(ctx, out, f, info.class_info.class_data, store->args[0],
                                         info.field_idx);
    fprintf(out, "->data, XRT_DATA_ALIGN);\n");
    return true;
}

static void emit_typed_array_data_cache_ref(FILE *out, const XiValue *origin) {
    fprintf(out, "_ad%u", origin ? origin->id : 0);
}

static void emit_aot_hot_region_begin(FILE *out, const char *kind) {
    fprintf(out, "/* XR_AOT_HOT_REGION_BEGIN %s */ ", kind ? kind : "unknown");
}

static void emit_aot_hot_region_end(FILE *out, const char *kind) {
    fprintf(out, " /* XR_AOT_HOT_REGION_END %s */", kind ? kind : "unknown");
}

static void emit_typed_array_ptr_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                      const XiValue *value, const char *prefix);

static bool cg_array_is_native_local_alloc(const XiValue *value) {
    const XiValue *v = cg_unwrap_identity_value(value);
    CgArrayElemInfo info;
    if (!v || v != value || !cg_array_elem_info_from_type(v->type, &info))
        return false;
    if (v->op == XI_ARRAY_NEW)
        return true;
    if (v->op != XI_CALL_BUILTIN || !v->aux)
        return false;
    const char *name = (const char *) v->aux;
    if (strcmp(name, "array_new") == 0)
        return true;
    if (strcmp(name, "array_with_capacity") == 0)
        return true;
    return strcmp(name, "array_copy_new") == 0;
}

static bool cg_array_native_local_arg_use_is_safe(const XiValue *user, uint16_t arg_index) {
    if (!user)
        return false;
    switch ((XiOp) user->op) {
        case XI_INDEX_GET:
            return arg_index == 0;
        case XI_INDEX_SET:
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
        case XI_SPAN_WINDOW:
        case XI_SPAN_AS_BYTES:
        case XI_SPAN_FILL:
        case XI_SPAN_REINTERPRET:
        case XI_ARRAY_DATA_PTR:
        case XI_BYTE_ARRAY_COPY_WITHIN:
        case XI_BYTE_ARRAY_REPEAT_FROM:
            return arg_index == 0;
        case XI_SPAN_COPY:
        case XI_SPAN_COMPARE:
        case XI_BYTE_SLICE_COPY:
        case XI_BYTE_SLICE_COMPARE:
        case XI_BYTE_SLICE_COMMON_PREFIX:
            return arg_index == 0 || arg_index == 1;
        case XI_BYTE_ARRAY_APPEND_FROM:
        case XI_BYTE_ARRAY_COPY_FROM:
            return arg_index == 0 || arg_index == 1;
        case XI_LEN:
            return arg_index == 0;
        case XI_CALL_METHOD: {
            if (arg_index == 0 && (cg_call_method_matches_receiver_registry_id(
                                       user, XA_BUILTIN_RECEIVER_METHOD_ARRAY_PUSH) ||
                                   cg_call_method_matches_receiver_registry_id(
                                       user, XA_BUILTIN_RECEIVER_METHOD_ARRAY_RESERVE) ||
                                   cg_call_method_matches_receiver_registry_id(
                                       user, XA_BUILTIN_RECEIVER_METHOD_U8_ARRAY_APPEND_FROM) ||
                                   cg_call_method_matches_receiver_registry_id(
                                       user, XA_BUILTIN_RECEIVER_METHOD_U8_ARRAY_REPEAT_FROM)))
                return true;
            if (arg_index == 1 && cg_call_method_matches_receiver_registry_id(
                                      user, XA_BUILTIN_RECEIVER_METHOD_U8_ARRAY_APPEND_FROM))
                return true;
            return false;
        }
        case XI_RETAIN:
        case XI_RELEASE:
            return arg_index == 0;
        case XI_BOX:
        case XI_UNBOX:
        case XI_COPY:
        case XI_MOVE:
            return arg_index == 0;
        default:
            return false;
    }
}

static bool cg_array_value_uses_native_local(XiCgenCtx *ctx, const XiFunc *f,
                                             const XiValue *value) {
    const XiValue *target = cg_unwrap_identity_value(value);
    if (!ctx || ctx->pre_decl_all || !f || !target || target != value ||
        cg_value_has_cell(ctx, target) || !cg_array_is_native_local_alloc(target))
        return false;

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *cur = blk->values[vi];
            if (!cur || cur == target)
                continue;
            for (uint16_t a = 0; a < cur->nargs; a++) {
                if (cur->args[a] == target && !cg_array_native_local_arg_use_is_safe(cur, a))
                    return false;
            }
        }
    }
    return true;
}

static bool cg_array_can_cache_data_for_value(XiCgenCtx *ctx, const XiValue *value,
                                              CgArrayElemInfo *info_out) {
    CgArrayElemInfo scratch;
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!ctx || ctx->pre_decl_all || !v)
        return false;
    const XaotArrayCachePlan *plan = xaot_bundle_find_array_cache_plan(cg_ctx_aot_bundle(ctx), v);
    if (plan)
        return cg_array_elem_info_from_cache_plan(plan, info_out ? info_out : &scratch);
    return false;
}

static const XiValue *cg_array_class_field_cached_origin(XiCgenCtx *ctx, const XiFunc *f,
                                                         const XiValue *value) {
    const XiValue *v = cg_unwrap_identity_value(value);
    XaotClassNativeFunc info;
    uint16_t field_idx = 0;
    if (!ctx || !f || !v ||
        !xaot_class_native_receiver_ref_field(cg_ctx_aot_bundle(ctx), f, v, XR_NATIVE_ARRAY_REF,
                                              &info, &field_idx))
        return NULL;
    const XaotArrayClassFieldAllocPlan *plan =
        xaot_bundle_find_array_class_field_alloc_plan_for_field(cg_ctx_aot_bundle(ctx), f,
                                                                info.class_data, field_idx);
    return plan ? plan->origin : NULL;
}

/* `restrict` qualifier for a data cache local, emitted only when prepare
 * proved the pointer unique over its backing (XaotAliasPlan). */
static const char *cg_array_cache_restrict_str(XiCgenCtx *ctx, const XiValue *origin) {
    const XaotAliasPlan *alias = xaot_bundle_find_alias_plan(cg_ctx_aot_bundle(ctx), origin);
    return alias && alias->kind == XAOT_ALIAS_UNIQUE_DATA ? "XRT_RESTRICT " : "";
}

static bool emit_typed_array_data_cache_decl(XiCgenCtx *ctx, FILE *out, const XiValue *value) {
    CgArrayElemInfo info;
    const XiValue *v = cg_unwrap_identity_value(value);
    const XaotArrayCachePlan *plan = xaot_bundle_find_array_cache_plan(cg_ctx_aot_bundle(ctx), v);
    if (plan) {
        if (!cg_array_elem_info_from_cache_plan(plan, &info))
            return false;
        if (!cg_array_data_cache_decl_mark(ctx, plan->value))
            return false;
        /* Slice views alias foreign storage at an arbitrary element offset,
         * so the alignment promise only holds for non-view caches. */
        bool aligned = (plan->flags & XAOT_ARRAY_CACHE_VIEW) == 0;
        fprintf(out, "    %s *%s", info.ctype, cg_array_cache_restrict_str(ctx, plan->value));
        emit_typed_array_data_cache_ref(out, plan->value);
        fprintf(out, " = (%s*)", info.ctype);
        if (cg_value_plan_is_span_aggregate(ctx, plan->storage_value)) {
            emit_vref(out, cg_unwrap_identity_value(plan->storage_value));
            fprintf(out, ".data;\n");
            return true;
        }
        if (aligned)
            fprintf(out, "XR_ASSUME_ALIGNED(");
        const XiFunc *f = plan->storage_value && plan->storage_value->block
                              ? plan->storage_value->block->func
                              : NULL;
        emit_typed_array_ptr_expr(ctx, out, f, plan->storage_value, NULL);
        fprintf(out, "->data");
        if (aligned)
            fprintf(out, ", XRT_DATA_ALIGN)");
        fprintf(out, ";\n");
        return true;
    }
    if (!cg_array_can_cache_data_for_value(ctx, v, &info))
        return false;
    if (!cg_array_data_cache_decl_mark(ctx, v))
        return false;
    const XiFunc *f = v && v->block ? v->block->func : NULL;
    fprintf(out, "    %s *%s", info.ctype, cg_array_cache_restrict_str(ctx, v));
    emit_typed_array_data_cache_ref(out, v);
    if (cg_value_plan_is_span_aggregate(ctx, v)) {
        fprintf(out, " = (%s*)", info.ctype);
        emit_vref(out, v);
        fprintf(out, ".data;\n");
        return true;
    }
    /* Fresh local allocation: XRT_DATA_ALIGN contract (see xrt_coll.h). */
    fprintf(out, " = (%s*)XR_ASSUME_ALIGNED(", info.ctype);
    emit_typed_array_ptr_expr(ctx, out, f, v, NULL);
    fprintf(out, "->data, XRT_DATA_ALIGN);\n");
    return true;
}

static const XiValue *cg_array_single_cacheable_value(XiCgenCtx *ctx, const XiValue *array_value,
                                                      uint8_t depth) {
    const XiValue *v = cg_unwrap_identity_value(array_value);
    if (!v || depth > 8)
        return NULL;
    if (cg_array_can_cache_data_for_value(ctx, v, NULL))
        return v;
    if (v->op != XI_PHI)
        return NULL;

    const XiValue *cached = NULL;
    for (uint16_t i = 0; i < v->nargs; i++) {
        const XiValue *arg = cg_unwrap_identity_value(v->args[i]);
        if (arg == v)
            continue;
        const XiValue *arg_cached = cg_array_single_cacheable_value(ctx, arg, depth + 1);
        if (!arg_cached)
            return NULL;
        if (cached && cached != arg_cached)
            return NULL;
        cached = arg_cached;
    }
    return cached;
}

static bool cg_array_data_cache_for_value(XiCgenCtx *ctx, const XiValue *array_value,
                                          const XiValue **out_origin) {
    const XiValue *v = cg_unwrap_identity_value(array_value);
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    const XaotArrayCachePlan *plan = xaot_bundle_find_array_cache_plan(bundle, v);
    if (plan) {
        if (out_origin)
            *out_origin = plan->value;
        return true;
    }
    if (v && v->op == XI_PHI) {
        const XiValue *cached = cg_array_single_cacheable_value(ctx, array_value, 0);
        if (cached) {
            if (out_origin)
                *out_origin = cached;
            return true;
        }
    }
    const XaotArrayStoragePlan *storage = xaot_bundle_find_array_storage_plan(bundle, v);
    if (storage && storage->origin) {
        plan = xaot_bundle_find_array_cache_plan(bundle, storage->origin);
        if (plan) {
            if (out_origin)
                *out_origin = plan->value;
            return true;
        }
    }
    if (cg_array_can_cache_data_for_value(ctx, v, NULL)) {
        if (out_origin)
            *out_origin = v;
        return true;
    }
    const XiFunc *f = v && v->block ? v->block->func : NULL;
    const XiValue *field_origin = cg_array_class_field_cached_origin(ctx, f, v);
    if (field_origin) {
        if (out_origin)
            *out_origin = field_origin;
        return true;
    }
    const XiValue *cached = cg_array_single_cacheable_value(ctx, array_value, 0);
    if (cached) {
        if (out_origin)
            *out_origin = cached;
        return true;
    }
    return false;
}

static bool cg_array_can_use_final_len_store(XiCgenCtx *ctx, const CgArrayFillLoop *fill) {
    if (!fill || !fill->origin || !fill->storage_value || !fill->cap_value || !fill->exit_block)
        return false;
    return cg_array_data_cache_for_value(ctx, fill->origin, NULL);
}

static void emit_typed_array_final_len_expr(FILE *out, const XiValue *cap_value) {
    fprintf(out, "(");
    emit_value_as_rep(out, cap_value, XR_REP_I64);
    fprintf(out, " > 0 ? ");
    emit_value_as_rep(out, cap_value, XR_REP_I64);
    fprintf(out, " : 0)");
}

static void emit_typed_array_final_len_stores(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                              const XiBlock *blk) {
    if (!ctx || !f || !blk)
        return;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *cur_blk = f->blocks[bi];
        if (!cur_blk)
            continue;
        for (uint32_t vi = 0; vi < cur_blk->nvalues; vi++) {
            CgArrayFillLoop fill;
            const XiValue *v = cur_blk->values[vi];
            if (!cg_array_fill_loop_match(ctx, f, v, &fill) || fill.exit_block != blk ||
                !cg_array_can_use_final_len_store(ctx, &fill))
                continue;
            fprintf(out, "    ");
            emit_typed_array_ptr_expr(ctx, out, f, fill.storage_value, NULL);
            fprintf(out, "->length = ");
            emit_typed_array_final_len_expr(out, fill.cap_value);
            fprintf(out, ";\n");
        }
    }
}

static bool cg_array_value_known_nonnegative(const XiValue *v, const XiValue *root, uint8_t depth);

static bool cg_array_phi_arg_nonnegative(const XiValue *phi, const XiValue *arg, bool *has_base,
                                         uint8_t depth) {
    const XiValue *v = cg_unwrap_identity_value(arg);
    if (!v)
        return false;
    if (v == phi)
        return true;
    if (v->op == XI_CONST && v->type && v->type->kind == XR_KIND_INT && v->aux_int >= 0) {
        *has_base = true;
        return true;
    }
    if (v->op == XI_ADD && v->nargs >= 2) {
        const XiValue *lhs = cg_unwrap_identity_value(v->args[0]);
        const XiValue *rhs = cg_unwrap_identity_value(v->args[1]);
        if (lhs == phi && cg_array_value_known_nonnegative(rhs, phi, depth + 1))
            return true;
        if (rhs == phi && cg_array_value_known_nonnegative(lhs, phi, depth + 1))
            return true;
    }
    if (cg_array_value_known_nonnegative(v, phi, depth + 1)) {
        *has_base = true;
        return true;
    }
    return false;
}

static bool cg_array_value_known_nonnegative(const XiValue *v, const XiValue *root, uint8_t depth) {
    v = cg_unwrap_identity_value(v);
    if (!v || depth > 8)
        return false;
    if (v == root && depth > 0)
        return false;
    if (v->type && v->type->kind == XR_KIND_INT && xi_range_known_nonneg(xi_range_of(v)))
        return true;
    if (v->op == XI_CONST && v->type && v->type->kind == XR_KIND_INT)
        return v->aux_int >= 0;
    switch ((XiOp) v->op) {
        case XI_NARROW_U8:
        case XI_NARROW_U16:
        case XI_NARROW_U32:
        case XI_WIDEN_U8:
        case XI_WIDEN_U16:
        case XI_WIDEN_U32:
            return true;
        case XI_ADD:
            return v->nargs >= 2 && cg_array_value_known_nonnegative(v->args[0], root, depth + 1) &&
                   cg_array_value_known_nonnegative(v->args[1], root, depth + 1);
        case XI_PHI: {
            bool has_base = false;
            if (v->nargs == 0)
                return false;
            for (uint16_t i = 0; i < v->nargs; i++) {
                if (!cg_array_phi_arg_nonnegative(v, v->args[i], &has_base, depth + 1))
                    return false;
            }
            return has_base;
        }
        default:
            return false;
    }
}

static bool cg_array_block_has_no_side_effect_after(const XiBlock *blk, const XiValue *start) {
    bool seen = start == NULL;
    bool found = false;
    if (!blk)
        return false;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        const XiValue *v = blk->values[i];
        if (!v)
            continue;
        if (v == start) {
            seen = true;
            found = true;
            continue;
        }
        if (seen && (v->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM)))
            return false;
    }
    if (!found && start != NULL) {
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            const XiValue *v = blk->values[i];
            if (v && (v->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM)))
                return false;
        }
        return true;
    }
    return seen;
}

static bool cg_array_block_has_no_side_effect_before(const XiBlock *blk, const XiValue *target) {
    if (!blk)
        return false;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        const XiValue *v = blk->values[i];
        if (!v)
            continue;
        if (v == target)
            return true;
        if (v->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM))
            return false;
    }
    return false;
}

/* In-bounds decisions either come from an explicit unsafe unchecked index op
 * (aux bit set by lowering) or from the XaotBoundsPlan computed by prepare
 * and re-derived by the verifier. Unproven plan rows are recorded too, so
 * evidence must be checked before using the planned raw path. */
static bool cg_array_index_access_bounds_proven(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    if (v && (v->op == XI_INDEX_GET || v->op == XI_INDEX_SET) && (v->aux_int & 1))
        return true;
    const XaotBoundsPlan *plan = xaot_bundle_find_bounds_plan(cg_ctx_aot_bundle(ctx), v);
    (void) f;
    return plan != NULL && plan->evidence != 0;
}

static const XaotSpanAccessPlan *cg_span_index_access_plan(XiCgenCtx *ctx, const XiValue *v,
                                                           uint8_t kind) {
    const XaotSpanAccessPlan *plan = xaot_bundle_find_span_access_plan(cg_ctx_aot_bundle(ctx), v);
    return plan && plan->kind == kind ? plan : NULL;
}

static bool cg_span_index_plan_drops(XiCgenCtx *ctx, const XiValue *v, uint8_t kind,
                                     uint32_t drops) {
    const XaotSpanAccessPlan *plan = cg_span_index_access_plan(ctx, v, kind);
    return plan && (plan->eliminated_checks & drops) == drops;
}

static bool cg_span_index_bounds_proven(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v,
                                        uint8_t kind) {
    (void) f;
    if (v && (v->op == XI_INDEX_GET || v->op == XI_INDEX_SET) && (v->aux_int & 1))
        return true;
    return cg_span_index_plan_drops(ctx, v, kind, XAOT_SPAN_DROP_BOUNDS);
}

static void emit_typed_array_store_value(XiCgenCtx *ctx, FILE *out, const CgArrayElemInfo *info,
                                         const XiValue *value) {
    if (info->rep == XR_REP_TAGGED) {
        emit_value_as_rep_ctx(ctx, out, value, XR_REP_TAGGED);
        return;
    }
    /* First-class CFn element: store the bare `_cfn` stub address (raw pointer),
     * resolved from a static top-level function (module-level resolve via ctx->module). */
    if (info->rep == XR_REP_RAWPTR && info->type && XR_TYPE_IS_C_FUNCTION(info->type)) {
        emit_cfn_value_rawptr(ctx, out, NULL, info->type, value);
        return;
    }
    fprintf(out, "(%s)", info->ctype);
    emit_value_as_rep_ctx(ctx, out, value, info->rep);
}

static void emit_typed_array_load_value(FILE *out, const CgArrayElemInfo *info, bool borrowed) {
    if (info->rep == XR_REP_TAGGED) {
        if (!borrowed)
            fprintf(out, "xrt_value_to_owned(");
    } else if (info->rep == XR_REP_F64) {
        fprintf(out, "(double)");
    } else if (info->rep == XR_REP_RAWPTR) {
        fprintf(out, "(void *)");
    } else {
        fprintf(out, "(int64_t)");
    }
}

static void emit_typed_array_load_value_end(FILE *out, const CgArrayElemInfo *info, bool borrowed) {
    if (info->rep == XR_REP_TAGGED && !borrowed)
        fprintf(out, ")");
}

static bool emit_typed_array_new_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                      int64_t cap) {
    CgArrayElemInfo info;
    if (!cg_array_elem_info_from_type_ctx(ctx, v->type, &info))
        return false;
    CgArrayFillLoop fill;
    if (cg_array_unique_fill_loop_for_origin(ctx, f, v, &fill)) {
        fprintf(out, "xrt_array_new_typed_uninit(");
        emit_value_as_rep(out, fill.cap_value, XR_REP_I64);
        fprintf(out, ", %s)", info.elem_name);
    } else if (v->nargs >= 1 && v->args[0] && v->args[0]->op != XI_CONST) {
        fprintf(out, "xrt_array_new_typed(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
        fprintf(out, ", %s)", info.elem_name);
    } else {
        fprintf(out, "xrt_array_new_typed(%" PRId64 ", %s)", cap, info.elem_name);
    }
    return true;
}

static bool emit_typed_array_new_ptr_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                          const XiValue *v, int64_t cap) {
    CgArrayElemInfo info;
    if (!cg_array_elem_info_from_type_ctx(ctx, v->type, &info))
        return false;
    CgArrayFillLoop fill;
    if (cg_array_unique_fill_loop_for_origin(ctx, f, v, &fill)) {
        fprintf(out, "xrt_array_new_typed_uninit_ptr(");
        emit_value_as_rep(out, fill.cap_value, XR_REP_I64);
        fprintf(out, ", %s)", info.elem_name);
    } else if (v->nargs >= 1 && v->args[0] && v->args[0]->op != XI_CONST) {
        fprintf(out, "xrt_array_new_typed_ptr(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
        fprintf(out, ", %s)", info.elem_name);
    } else {
        fprintf(out, "xrt_array_new_typed_ptr(%" PRId64 ", %s)", cap, info.elem_name);
    }
    return true;
}

static void emit_typed_array_ptr_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                      const XiValue *value, const char *prefix) {
    (void) prefix;
    if (emit_class_native_receiver_ref_field_ptr_expr(ctx, out, f, value, XR_NATIVE_ARRAY_REF))
        return;
    if (cg_array_value_uses_native_local(ctx, f, value)) {
        emit_vref(out, value);
        if (f && cg_func_needs_aot_coro_ctx(ctx, f) &&
            cg_value_plan_storage_rep(ctx, value) != XR_REP_PTR)
            fprintf(out, ".ptr");
        return;
    }
    fprintf(out, "(");
    fprintf(out, "(xrt_array_t*)");
    emit_vref(out, value);
    if (cg_value_plan_storage_rep(ctx, value) != XR_REP_PTR)
        fprintf(out, ".ptr");
    fprintf(out, ")");
}

static bool cg_class_native_array_receiver_ref_field(XiCgenCtx *ctx, const XiFunc *f,
                                                     const XiValue *recv,
                                                     CgClassNativeFunc *out_info,
                                                     uint16_t *out_idx) {
    return cg_class_native_receiver_ref_field(ctx, f, recv, XR_NATIVE_ARRAY_REF, out_info, out_idx);
}

static void emit_class_native_array_field_box(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                              const CgClassNativeFunc *info, const XiValue *recv,
                                              uint16_t idx) {
    fprintf(out, "xr_mkptr(");
    emit_class_native_receiver_field_ref(ctx, out, f, info->class_data, recv, idx);
    fprintf(out, ", XR_TAG_ARRAY)");
}

static bool emit_class_native_array_length_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                const XiValue *v) {
    if (!v || v->op != XI_LEN || v->nargs != 1)
        return false;
    CgClassNativeFunc info;
    uint16_t idx = 0;
    if (!cg_class_native_array_receiver_ref_field(ctx, f, v->args[0], &info, &idx))
        return false;
    const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
    emit_class_native_receiver_field_ref(ctx, out, f, info.class_data, v->args[0], idx);
    fprintf(out, "->length");
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool emit_class_native_array_index_get_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                   const XiValue *v) {
    if (!v || v->op != XI_INDEX_GET || v->nargs < 2)
        return false;
    CgClassNativeFunc info;
    uint16_t idx = 0;
    if (!cg_class_native_array_receiver_ref_field(ctx, f, v->args[0], &info, &idx))
        return false;
    const char *conv_suffix =
        emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_value_plan_storage_rep(ctx, v));
    fprintf(out, "xrt_index_get(");
    emit_class_native_array_field_box(ctx, out, f, &info, v->args[0], idx);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ")");
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool emit_class_native_array_index_set_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                   const XiValue *v) {
    if (!v || v->op != XI_INDEX_SET || v->nargs < 3)
        return false;
    CgClassNativeFunc info;
    uint16_t idx = 0;
    if (!cg_class_native_array_receiver_ref_field(ctx, f, v->args[0], &info, &idx))
        return false;
    fprintf(out, "xrt_index_set(");
    emit_class_native_array_field_box(ctx, out, f, &info, v->args[0], idx);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_TAGGED);
    fprintf(out, ")");
    return true;
}

static bool emit_class_native_array_method_call_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                     const XiValue *v) {
    if (!v || v->op != XI_CALL_METHOD || v->nargs < 1 || !v->aux)
        return false;
    CgClassNativeFunc info;
    uint16_t idx = 0;
    if (!cg_class_native_array_receiver_ref_field(ctx, f, v->args[0], &info, &idx))
        return false;
    const char *method = (const char *) v->aux;
    int sym = cg_method_sym(method);
    if (sym < 0)
        return false;
    uint16_t nargs = (uint16_t) (v->nargs - 1);
    if (nargs > 3)
        return false;
    const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
    if (nargs == 0) {
        fprintf(out, "xrt_method_0(");
        emit_class_native_array_field_box(ctx, out, f, &info, v->args[0], idx);
        fprintf(out, ", %d)", sym);
    } else if (nargs == 1) {
        fprintf(out, "xrt_method_1(");
        emit_class_native_array_field_box(ctx, out, f, &info, v->args[0], idx);
        fprintf(out, ", %d, ", sym);
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ")");
    } else if (nargs == 2) {
        fprintf(out, "xrt_method_2(");
        emit_class_native_array_field_box(ctx, out, f, &info, v->args[0], idx);
        fprintf(out, ", %d, ", sym);
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_TAGGED);
        fprintf(out, ")");
    } else if (nargs == 3) {
        fprintf(out, "xrt_method_3(");
        emit_class_native_array_field_box(ctx, out, f, &info, v->args[0], idx);
        fprintf(out, ", %d, ", sym);
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, v->args[2], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, v->args[3], XR_REP_TAGGED);
        fprintf(out, ")");
    }
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool emit_class_native_array_method_call_stmt(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                     const XiValue *v) {
    if (!v || v->uses != 0 || v->op != XI_CALL_METHOD || v->nargs < 1 || !v->aux)
        return false;
    CgClassNativeFunc info;
    uint16_t idx = 0;
    if (!cg_class_native_array_receiver_ref_field(ctx, f, v->args[0], &info, &idx))
        return false;
    const char *method = (const char *) v->aux;
    uint16_t nargs = (uint16_t) (v->nargs - 1);
    if (nargs == 1 && strcmp(method, "push") == 0) {
        fprintf(out, "    xrt_array_push(");
        emit_class_native_array_field_box(ctx, out, f, &info, v->args[0], idx);
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ");\n");
        return true;
    }
    if (nargs > 2 || cg_method_sym(method) < 0)
        return false;
    fprintf(out, "    ");
    if (!emit_class_native_array_method_call_expr(ctx, out, f, v))
        return false;
    fprintf(out, ";\n");
    return true;
}

static bool cg_class_native_array_method_call_value_is_elided(XiCgenCtx *ctx, const XiFunc *f,
                                                              const XiValue *v) {
    if (!v || v->uses != 0 || v->op != XI_CALL_METHOD || v->nargs < 1 || !v->aux)
        return false;
    if (!cg_class_native_array_receiver_ref_field(ctx, f, v->args[0], NULL, NULL))
        return false;
    uint16_t nargs = (uint16_t) (v->nargs - 1);
    return nargs <= 2 && cg_method_sym((const char *) v->aux) >= 0;
}

static bool emit_typed_array_index_get_expr_as_rep(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                   const XiValue *v, const char *prefix,
                                                   XrRep target_rep) {
    CgArrayElemInfo info;
    if (!v || v->nargs < 2 ||
        !cg_array_value_storage_info(ctx, f, v->args[0], &info, CG_ARRAY_STORAGE_READ))
        return false;

    bool unchecked = cg_array_index_access_bounds_proven(ctx, f, v);
    const XiValue *cached_origin = NULL;
    bool use_cache = cg_array_data_cache_for_value(ctx, v->args[0], &cached_origin);
    bool borrowed_tagged = target_rep == XR_REP_TAGGED && info.rep == XR_REP_TAGGED &&
                           cg_tagged_array_index_get_can_borrow(ctx, f, v);
    const char *conv_suffix = emit_conversion_prefix(out, v->type, info.rep, target_rep);
    if (unchecked) {
        if (use_cache) {
            emit_aot_hot_region_begin(out, "typed_array_raw_access");
            emit_typed_array_load_value(out, &info, borrowed_tagged);
            emit_typed_array_data_cache_ref(out, cached_origin);
            fprintf(out, "[");
        } else {
            fprintf(out, "({ xrt_array_t *_a = ");
            emit_typed_array_ptr_expr(ctx, out, f, v->args[0], prefix);
            fprintf(out, "; ");
            emit_typed_array_load_value(out, &info, borrowed_tagged);
            fprintf(out, "((%s*)_a->data)[", info.ctype);
        }
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
        fprintf(out, "]");
        emit_typed_array_load_value_end(out, &info, borrowed_tagged);
        if (use_cache) {
            emit_aot_hot_region_end(out, "typed_array_raw_access");
        } else {
            fprintf(out, "; })");
        }
    } else if (info.rep == XR_REP_F64) {
        /* Bounds-checked read: an index outside [0, length) — including negatives —
         * throws E0430 (spec §3), matching the VM. No from-end wraparound. */
        fprintf(out, "({ xrt_array_t *_a = ");
        emit_typed_array_ptr_expr(ctx, out, f, v->args[0], prefix);
        fprintf(out, "; int64_t _idx = ");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
        fprintf(out, "; XR_LIKELY(_idx >= 0 && _idx < _a->length) ? (double)");
        if (use_cache) {
            emit_typed_array_data_cache_ref(out, cached_origin);
            fprintf(out, "[_idx]");
        } else {
            fprintf(out, "((%s*)_a->data)[_idx]", info.ctype);
        }
        fprintf(out, " : (xrt_index_oob(_idx, _a->length), 0.0); })");
    } else if (info.rep == XR_REP_TAGGED) {
        fprintf(out, "({ xrt_array_t *_a = ");
        emit_typed_array_ptr_expr(ctx, out, f, v->args[0], prefix);
        fprintf(out, "; int64_t _idx = ");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
        fprintf(out, "; XR_LIKELY(_idx >= 0 && _idx < _a->length) ? ");
        emit_typed_array_load_value(out, &info, borrowed_tagged);
        if (use_cache) {
            emit_typed_array_data_cache_ref(out, cached_origin);
            fprintf(out, "[_idx]");
        } else {
            fprintf(out, "((XrValue*)_a->data)[_idx]");
        }
        emit_typed_array_load_value_end(out, &info, borrowed_tagged);
        fprintf(out, " : (xrt_index_oob(_idx, _a->length), XR_NULL_VAL); })");
    } else if (info.rep == XR_REP_RAWPTR) {
        fprintf(out, "({ xrt_array_t *_a = ");
        emit_typed_array_ptr_expr(ctx, out, f, v->args[0], prefix);
        fprintf(out, "; int64_t _idx = ");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
        fprintf(out, "; XR_LIKELY(_idx >= 0 && _idx < _a->length) ? (void *)");
        if (use_cache) {
            emit_typed_array_data_cache_ref(out, cached_origin);
            fprintf(out, "[_idx]");
        } else {
            fprintf(out, "((%s*)_a->data)[_idx]", info.ctype);
        }
        fprintf(out, " : (xrt_index_oob(_idx, _a->length), (void *)0); })");
    } else {
        fprintf(out, "({ xrt_array_t *_a = ");
        emit_typed_array_ptr_expr(ctx, out, f, v->args[0], prefix);
        fprintf(out, "; int64_t _idx = ");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
        fprintf(out, "; XR_LIKELY(_idx >= 0 && _idx < _a->length) ? (int64_t)");
        if (use_cache) {
            emit_typed_array_data_cache_ref(out, cached_origin);
            fprintf(out, "[_idx]");
        } else {
            fprintf(out, "((%s*)_a->data)[_idx]", info.ctype);
        }
        fprintf(out, " : (xrt_index_oob(_idx, _a->length), 0); })");
    }
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool emit_typed_array_index_get_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                            const XiValue *v, const char *prefix) {
    return emit_typed_array_index_get_expr_as_rep(ctx, out, f, v, prefix,
                                                  cg_value_decl_storage_rep(ctx, f, v));
}

static void emit_span_ref_expr(FILE *out, const XiValue *value) {
    emit_vref(out, cg_unwrap_identity_value(value));
}

static bool cg_span_value_u8_info(XiCgenCtx *ctx, const XiValue *value, CgArrayElemInfo *out) {
    CgArrayElemInfo info;
    if (!cg_value_plan_is_span_aggregate(ctx, value) ||
        !cg_span_elem_info_from_value(ctx, value, &info) || !cg_array_elem_info_is_u8(&info))
        return false;
    if (out)
        *out = info;
    return true;
}

static void emit_span_array_view_ptr_expr(FILE *out, const XiValue *value) {
    fprintf(out, "xrt_array_stack_borrow_span_view(");
    emit_span_ref_expr(out, value);
    fprintf(out, ")");
}

static bool emit_span_length_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    if (!v || v->op != XI_LEN || v->nargs != 1)
        return false;
    if (!cg_value_plan_is_span_aggregate(ctx, v->args[0]))
        return false;
    const char *conv_suffix =
        emit_conversion_prefix(out, v->type, XR_REP_I64, cg_value_plan_storage_rep(ctx, v));
    fprintf(out, "(");
    emit_span_ref_expr(out, v->args[0]);
    fprintf(out, ").length");
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool emit_span_index_get_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v) {
    (void) f;
    CgArrayElemInfo info;
    if (!v || v->op != XI_INDEX_GET || v->nargs < 2 ||
        !cg_value_plan_is_span_aggregate(ctx, v->args[0]) ||
        !cg_span_elem_info_from_value(ctx, v->args[0], &info))
        return false;

    XrRep target_rep = cg_value_plan_storage_rep(ctx, v);
    bool unchecked = cg_span_index_bounds_proven(ctx, f, v, XAOT_SPAN_ACCESS_INDEX_GET);
    bool borrowed_tagged = target_rep == XR_REP_TAGGED && info.rep == XR_REP_TAGGED &&
                           cg_tagged_array_index_get_can_borrow(ctx, f, v);
    const char *conv_suffix = emit_conversion_prefix(out, v->type, info.rep, target_rep);
    fprintf(out, "({ xr_span_t _s = ");
    emit_span_ref_expr(out, v->args[0]);
    fprintf(out, "; int64_t _idx = ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
    fprintf(out, "; ");
    if (!unchecked)
        fprintf(out, "XR_LIKELY(_idx >= 0 && _idx < _s.length) ? ");
    if (info.rep == XR_REP_F64) {
        fprintf(out, "(double)((%s*)_s.data)[_idx]", info.ctype);
        if (!unchecked)
            fprintf(out, " : (xrt_index_oob(_idx, _s.length), 0.0)");
    } else if (info.rep == XR_REP_TAGGED) {
        emit_typed_array_load_value(out, &info, borrowed_tagged);
        fprintf(out, "((XrValue*)_s.data)[_idx]");
        emit_typed_array_load_value_end(out, &info, borrowed_tagged);
        if (!unchecked)
            fprintf(out, " : (xrt_index_oob(_idx, _s.length), XR_NULL_VAL)");
    } else {
        fprintf(out, "(int64_t)((%s*)_s.data)[_idx]", info.ctype);
        if (!unchecked)
            fprintf(out, " : (xrt_index_oob(_idx, _s.length), 0)");
    }
    fprintf(out, "; })");
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool emit_span_index_set_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v) {
    (void) f;
    CgArrayElemInfo info;
    if (!v || v->op != XI_INDEX_SET || v->nargs < 3 ||
        !cg_value_plan_is_span_aggregate(ctx, v->args[0]) ||
        !cg_span_elem_info_from_value(ctx, v->args[0], &info))
        return false;

    bool unchecked = cg_span_index_bounds_proven(ctx, f, v, XAOT_SPAN_ACCESS_INDEX_SET);
    bool skip_readonly =
        cg_span_index_plan_drops(ctx, v, XAOT_SPAN_ACCESS_INDEX_SET, XAOT_SPAN_DROP_READONLY);
    fprintf(out, "({ xr_span_t _s = ");
    emit_span_ref_expr(out, v->args[0]);
    fprintf(out, "; int64_t _idx = ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
    fprintf(out, "; ");
    if (!skip_readonly) {
        fprintf(out, "if (XR_UNLIKELY(xrt_span_is_readonly(_s))) ");
        fprintf(
            out,
            "xrt_throw_error(XR_ERR_CMP_CONST_ASSIGN, \"cannot write through readonly Span\"); ");
    }
    if (!unchecked)
        fprintf(out, "if (XR_LIKELY(_idx >= 0 && _idx < _s.length)) { ");
    fprintf(out, "((%s*)_s.data)[_idx] = ", info.ctype);
    emit_typed_array_store_value(ctx, out, &info, v->args[2]);
    fprintf(out, "; ");
    if (info.rep == XR_REP_TAGGED)
        fprintf(out,
                "/* Span writes through owner storage; owner barrier metadata is unchanged. */ ");
    if (!unchecked)
        fprintf(out, "} else { xrt_index_oob(_idx, _s.length); } ");
    fprintf(out, "XR_NULL_VAL; })");
    return true;
}

static bool emit_typed_array_index_set_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                            const XiValue *v, const char *prefix) {
    CgArrayElemInfo info;
    if (!v || v->nargs < 3 ||
        !cg_array_value_storage_info(ctx, f, v->args[0], &info, CG_ARRAY_STORAGE_MUTABLE))
        return false;

    if (cg_array_index_access_bounds_proven(ctx, f, v)) {
        const XiValue *cached_origin = NULL;
        bool use_cache = cg_array_data_cache_for_value(ctx, v->args[0], &cached_origin);
        fprintf(out, "({ ");
        if (use_cache) {
            emit_aot_hot_region_begin(out, "typed_array_raw_access");
            emit_typed_array_data_cache_ref(out, cached_origin);
            fprintf(out, "[");
        } else {
            fprintf(out, "((%s*)", info.ctype);
            emit_typed_array_ptr_expr(ctx, out, f, v->args[0], prefix);
            fprintf(out, "->data)[");
        }
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
        fprintf(out, "] = ");
        emit_typed_array_store_value(ctx, out, &info, v->args[2]);
        if (use_cache)
            emit_aot_hot_region_end(out, "typed_array_raw_access");
        fprintf(out, "; XR_NULL_VAL; })");
        return true;
    }

    fprintf(out, "({ xrt_array_t *_a = ");
    emit_typed_array_ptr_expr(ctx, out, f, v->args[0], prefix);
    fprintf(out, "; int64_t _idx = ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
    /* Index assignment is strict: no negative-from-end wraparound (that is
     * slice-only), matching index reads. An out-of-range index traps. */
    fprintf(out, "; ");
    fprintf(out, "if (XR_LIKELY(_idx >= 0 && _idx < _a->length)) { ((%s*)_a->data)[_idx] = ",
            info.ctype);
    emit_typed_array_store_value(ctx, out, &info, v->args[2]);
    fprintf(out, "; } else { xrt_index_oob(_idx, _a->length); } XR_NULL_VAL; })");
    return true;
}

static bool emit_typed_array_push_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                       const char *prefix, const XiValue *call, const XiValue *recv,
                                       const XiValue *arg) {
    CgArrayElemInfo info;
    if (!cg_array_value_storage_info(ctx, f, recv, &info, CG_ARRAY_STORAGE_MUTABLE))
        return false;

    CgArrayFillLoop fill;
    bool unchecked = cg_array_fill_loop_match(ctx, f, call, &fill);
    if (unchecked) {
        CgArrayFillLoop unique;
        unchecked = cg_array_unique_fill_loop_for_origin(ctx, f, fill.origin, &unique) &&
                    unique.push == call;
    }

    if (unchecked) {
        const XiValue *cached_origin = NULL;
        bool use_cache = cg_array_data_cache_for_value(ctx, fill.origin, &cached_origin);
        bool use_final_len = use_cache && cg_array_can_use_final_len_store(ctx, &fill);
        fprintf(out, "({ xrt_array_t *_a = ");
        emit_typed_array_ptr_expr(ctx, out, f, recv, prefix);
        fprintf(out, "; ");
        if (use_cache) {
            if (use_final_len)
                emit_aot_hot_region_begin(out, "typed_array_raw_access");
            emit_typed_array_data_cache_ref(out, cached_origin);
            fprintf(out, "[");
        } else {
            fprintf(out, "((%s*)_a->data)[", info.ctype);
        }
        emit_value_as_rep(out, fill.index_value, XR_REP_I64);
        fprintf(out, "] = ");
        emit_typed_array_store_value(ctx, out, &info, arg);
        if (use_cache && use_final_len)
            emit_aot_hot_region_end(out, "typed_array_raw_access");
        if (!use_final_len) {
            fprintf(out, "; _a->length = ");
            if (fill.next_index_value && cg_array_value_available_at(fill.next_index_value, call)) {
                emit_value_as_rep(out, fill.next_index_value, XR_REP_I64);
            } else {
                fprintf(out, "(");
                emit_value_as_rep(out, fill.index_value, XR_REP_I64);
                fprintf(out, " + 1)");
            }
        }
        if (info.rep == XR_REP_TAGGED)
            fprintf(out, "; XR_ARRAY_MARK_MUTATED(_a)");
        fprintf(out, "; XR_NULL_VAL; })");
        return true;
    }

    fprintf(out, "({ xrt_array_t *_a = ");
    emit_typed_array_ptr_expr(ctx, out, f, recv, prefix);
    {
        fprintf(out,
                "; if (_a->data_storage == XR_ARRAY_DATA_BORROWED) { fprintf(stderr, "
                "\"xrt_array_push: cannot push to array slice\\n\"); abort(); } "
                "if (XR_UNLIKELY(_a->length >= _a->capacity)) { "
                "xrt_array_data_grow(_a, _a->capacity == 0 ? 4 : _a->capacity * 2); } "
                "((%s*)_a->data)[_a->length++] = ",
                info.ctype);
    }
    emit_typed_array_store_value(ctx, out, &info, arg);
    if (info.rep == XR_REP_TAGGED)
        fprintf(out, "; XR_ARRAY_MARK_MUTATED(_a)");
    fprintf(out, "; XR_NULL_VAL; })");
    return true;
}

static bool emit_typed_array_set_unchecked_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                const char *prefix, const XiValue *call) {
    CgArrayElemInfo info;
    if (!call || call->nargs != 3)
        return false;
    if (!cg_array_value_storage_info(ctx, f, call->args[0], &info, CG_ARRAY_STORAGE_MUTABLE))
        return false;
    if (info.rep == XR_REP_TAGGED)
        return false;

    const XiValue *cached_origin = NULL;
    bool use_cache = cg_array_data_cache_for_value(ctx, call->args[0], &cached_origin);
    fprintf(out, "({ ");
    if (use_cache) {
        emit_aot_hot_region_begin(out, "typed_array_raw_access");
        emit_typed_array_data_cache_ref(out, cached_origin);
        fprintf(out, "[");
    } else {
        fprintf(out, "((%s*)", info.ctype);
        emit_typed_array_ptr_expr(ctx, out, f, call->args[0], prefix);
        fprintf(out, "->data)[");
    }
    emit_value_as_rep(out, call->args[1], XR_REP_I64);
    fprintf(out, "] = ");
    emit_typed_array_store_value(ctx, out, &info, call->args[2]);
    if (use_cache)
        emit_aot_hot_region_end(out, "typed_array_raw_access");
    fprintf(out, "; XR_NULL_VAL; })");
    return true;
}

static bool emit_typed_array_resize_zero_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                              const char *prefix, const XiValue *call) {
    if (!call || call->nargs < 2 || call->nargs > 3)
        return false;
    const XiValue *len = cg_unwrap_identity_value(call->args[1]);
    if (!len || len->op != XI_CONST || !len->type || len->type->kind != XR_KIND_INT ||
        len->aux_int != 0)
        return false;

    CgArrayElemInfo info;
    if (!cg_array_value_storage_info(ctx, f, call->args[0], &info, CG_ARRAY_STORAGE_MUTABLE))
        return false;
    if (!cg_array_value_has_fresh_owned_origin(ctx, call->args[0]))
        return false;

    const char *conv_suffix = emit_conversion_prefix(out, call->type, XR_REP_TAGGED, cg_rep(call));
    fprintf(out, "({ xrt_array_t *_a = ");
    emit_typed_array_ptr_expr(ctx, out, f, call->args[0], prefix);
    fprintf(out, "; _a->length = 0");
    if (info.rep == XR_REP_TAGGED)
        fprintf(out, "; XR_ARRAY_MARK_MUTATED(_a)");
    fprintf(out, "; xr_mkptr(_a, XR_TAG_ARRAY); })");
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static void emit_byte_array_result_suffix(FILE *out, bool boxed);

static bool emit_typed_array_reserve_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                          const char *prefix, const XiValue *call) {
    if (!call || call->nargs != 2)
        return false;

    CgArrayElemInfo info;
    if (!cg_array_value_storage_info(ctx, f, call->args[0], &info, CG_ARRAY_STORAGE_MUTABLE))
        return false;

    bool value_used = call->uses != 0;
    bool boxed = value_used && cg_rep(call) == XR_REP_TAGGED;
    if (boxed)
        fprintf(out, "xr_mkptr(");
    fprintf(out, "xrt_array_reserve_trusted_raw(");
    emit_typed_array_ptr_expr(ctx, out, f, call->args[0], prefix);
    fprintf(out, ", ");
    emit_value_as_rep(out, call->args[1], XR_REP_I64);
    fprintf(out, ")");
    emit_byte_array_result_suffix(out, boxed);
    return true;
}

static bool cg_array_elem_info_is_u8(const CgArrayElemInfo *info) {
    return info && xr_type_is_exact_u8(info->type);
}

static bool cg_array_elem_info_is_memset_byte_pattern(const CgArrayElemInfo *info) {
    return info && info->elem_name &&
           (strcmp(info->elem_name, "XR_ELEM_BOOL") == 0 ||
            strcmp(info->elem_name, "XR_ELEM_I8") == 0 || cg_array_elem_info_is_u8(info));
}

static void emit_byte_array_result_suffix(FILE *out, bool boxed) {
    if (boxed)
        fprintf(out, ", XR_TAG_ARRAY)");
}

static bool cg_byte_array_unchecked_int_arg(const XiValue *arg) {
    return arg && arg->type && arg->type->kind == XR_KIND_INT;
}

static bool emit_byte_array_append_from_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                             const char *prefix, const XiValue *call) {
    CgArrayElemInfo dst_info;
    if (!call || call->nargs != 2)
        return false;
    if (!cg_array_value_u8_unchecked_info(ctx, f, call->args[0], &dst_info,
                                          CG_ARRAY_STORAGE_MUTABLE) ||
        !cg_span_value_u8_info(ctx, call->args[1], NULL))
        return false;

    const XaotBulkPlan *bulk =
        cg_required_bulk_plan(ctx, f, call, XG_BULK_COPY, "Array<byte>.appendFrom");
    if (call->xg_bulk_op_id != XG_NO_ID && !bulk) {
        emit_codegen_abort_expr(out);
        return true;
    }
    if (bulk && bulk->action != XAOT_BULK_INLINE_MEMCPY &&
        bulk->action != XAOT_BULK_INLINE_MEMMOVE && bulk->action != XAOT_BULK_TYPED_LOOP &&
        bulk->action != XAOT_BULK_RUNTIME_HELPER) {
        cg_ctx_set_error(ctx);
        emit_codegen_abort_expr(out);
        return true;
    }

    bool boxed = cg_rep(call) == XR_REP_TAGGED;
    if (boxed)
        fprintf(out, "xr_mkptr(");
    if (bulk && bulk->action == XAOT_BULK_RUNTIME_HELPER) {
        fprintf(out, "xrt_byte_array_append_from_span_slow_raw(");
        emit_typed_array_ptr_expr(ctx, out, f, call->args[0], prefix);
        fprintf(out, ", ");
        emit_span_ref_expr(out, call->args[1]);
        fprintf(out, ")");
        emit_byte_array_result_suffix(out, boxed);
        return true;
    }
    fprintf(out, "({ xrt_array_t *_dst = ");
    emit_typed_array_ptr_expr(ctx, out, f, call->args[0], prefix);
    fprintf(out, "; xr_span_t _src = ");
    emit_span_ref_expr(out, call->args[1]);
    fprintf(out,
            "; xrt_array_t *_res = _dst; if (XR_LIKELY(_dst && _src.length >= 0 && _src.length "
            "<= INT64_MAX - _dst->length)) { int64_t _old_length = _dst->length; int64_t "
            "_new_length = _old_length + _src.length; if (XR_LIKELY(_new_length <= "
            "_dst->capacity");
    if (!bulk || bulk->action == XAOT_BULK_INLINE_MEMCPY)
        fprintf(out, " && _src.guard != _dst");
    fprintf(out, " && (_new_length == 0 || _dst->data) && (_src.length == 0 || _src.data))) { "
                 "if (_src.length > 0) { uint8_t *_dp = (uint8_t*)_dst->data + _old_length; ");
    if (bulk && bulk->action == XAOT_BULK_INLINE_MEMMOVE)
        fprintf(out, "memmove(_dp, _src.data, (size_t)_src.length);");
    else if (bulk && bulk->action == XAOT_BULK_INLINE_MEMCPY)
        fprintf(out, "memcpy(_dp, _src.data, (size_t)_src.length);");
    else if (bulk && bulk->action == XAOT_BULK_TYPED_LOOP)
        fprintf(out, "for (int64_t _i = 0; _i < _src.length; _i++) "
                     "_dp[_i] = ((const uint8_t*)_src.data)[_i];");
    else
        fprintf(out, "xr_array_core_copy_nonoverlap_bytes(_dp, _src.data, _src.length);");
    fprintf(out, " } _dst->length = _new_length; } else { _res = "
                 "xrt_byte_array_append_from_span_slow_raw(_dst, _src); } } else { _res = "
                 "xrt_byte_array_append_from_span_slow_raw(_dst, _src); } _res; })");
    emit_byte_array_result_suffix(out, boxed);
    return true;
}

static bool emit_byte_array_repeat_from_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                             const char *prefix, const XiValue *call) {
    CgArrayElemInfo info;
    if (!call || call->nargs != 3)
        return false;
    if (!cg_array_value_u8_unchecked_info(ctx, f, call->args[0], &info, CG_ARRAY_STORAGE_MUTABLE))
        return false;
    if (!cg_byte_array_unchecked_int_arg(call->args[1]) ||
        !cg_byte_array_unchecked_int_arg(call->args[2]))
        return false;

    const XaotBulkPlan *bulk =
        cg_required_bulk_plan(ctx, f, call, XG_BULK_REPEAT, "Array<byte>.repeatFrom");
    if (call->xg_bulk_op_id != XG_NO_ID && !bulk) {
        emit_codegen_abort_expr(out);
        return true;
    }
    if (bulk && bulk->action != XAOT_BULK_TYPED_LOOP && bulk->action != XAOT_BULK_RUNTIME_HELPER) {
        cg_ctx_set_error(ctx);
        emit_codegen_abort_expr(out);
        return true;
    }

    bool boxed = cg_rep(call) == XR_REP_TAGGED;
    if (boxed)
        fprintf(out, "xr_mkptr(");
    if (bulk && bulk->action == XAOT_BULK_TYPED_LOOP) {
        fprintf(out, "({ xrt_array_t *_a = ");
        emit_typed_array_ptr_expr(ctx, out, f, call->args[0], prefix);
        fprintf(out, "; int64_t _distance = ");
        emit_value_as_rep(out, call->args[1], XR_REP_I64);
        fprintf(out, "; int64_t _count = ");
        emit_value_as_rep(out, call->args[2], XR_REP_I64);
        fprintf(out,
                "; if (XR_UNLIKELY(!_a || _a->elem_type != XR_ELEM_U8)) "
                "xrt_throw_error(XR_ERR_TYPE_MISMATCH, "
                "XR_ERROR_CORE_BYTE_ARRAY_REPEAT_FROM_RECEIVER_MSG); if (XR_UNLIKELY("
                "_a->data_storage == XR_ARRAY_DATA_BORROWED || _distance <= 0 || _count < 0 || "
                "_distance > _a->length || _count > INT64_MAX - _a->length)) "
                "xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "
                "XR_ERROR_CORE_BYTE_ARRAY_REPEAT_FROM_OOB_MSG); int64_t _dst = _a->length; "
                "int64_t _new_length = _dst + _count; if (_new_length > _a->capacity) "
                "xrt_array_reserve_trusted_raw(_a, _new_length); if (XR_UNLIKELY("
                "_new_length > _a->capacity || (_new_length > 0 && !_a->data))) "
                "xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "
                "XR_ERROR_CORE_BYTE_ARRAY_REPEAT_FROM_OOB_MSG); "
                "xr_array_core_bytes_repeat_copy(_a->data, _dst, _distance, _count); "
                "_a->length = _new_length; _a; })");
    } else {
        fprintf(out, "xrt_byte_array_repeat_from_tail_raw(");
        emit_typed_array_ptr_expr(ctx, out, f, call->args[0], prefix);
        fprintf(out, ", ");
        emit_value_as_rep(out, call->args[1], XR_REP_I64);
        fprintf(out, ", ");
        emit_value_as_rep(out, call->args[2], XR_REP_I64);
        fprintf(out, ")");
    }
    emit_byte_array_result_suffix(out, boxed);
    return true;
}

static bool cg_array_call_is_direct_byte_array_mutator_trusted_nothrow(XiCgenCtx *ctx,
                                                                       const XiFunc *f,
                                                                       const XiValue *call) {
    const XiValue *v = cg_unwrap_identity_value(call);
    if (!v || (v->op != XI_CALL_METHOD && v->op != XI_CALL_METHOD_DIRECT))
        return false;

    CgArrayElemInfo dst_info;
    if (cg_call_method_matches_receiver_registry_id(v, XA_BUILTIN_RECEIVER_METHOD_ARRAY_PUSH) &&
        v->nargs == 2) {
        return (cg_array_value_storage_info(ctx, f, v->args[0], &dst_info,
                                            CG_ARRAY_STORAGE_MUTABLE) &&
                cg_array_elem_info_is_u8(&dst_info)) ||
               cg_array_value_type_is_u8_contiguous(ctx, v->args[0]);
    }
    if (cg_call_method_matches_receiver_registry_id(v, XA_BUILTIN_RECEIVER_METHOD_ARRAY_SET) &&
        v->nargs == 3) {
        return (cg_array_value_storage_info(ctx, f, v->args[0], &dst_info,
                                            CG_ARRAY_STORAGE_MUTABLE) &&
                cg_array_elem_info_is_u8(&dst_info)) ||
               cg_array_value_type_is_u8_contiguous(ctx, v->args[0]);
    }
    return false;
}

static bool cg_array_call_is_byte_array_append_trusted_nothrow(XiCgenCtx *ctx, const XiFunc *f,
                                                               const XiValue *call) {
    const XiValue *v = cg_unwrap_identity_value(call);
    if (!v)
        return false;

    CgArrayElemInfo dst_info;
    if (v->op == XI_BYTE_ARRAY_APPEND_FROM) {
        return v->nargs == 2 &&
               cg_array_value_u8_unchecked_info(ctx, f, v->args[0], &dst_info,
                                                CG_ARRAY_STORAGE_MUTABLE) &&
               cg_span_value_u8_info(ctx, v->args[1], NULL);
    }
    if (v->op != XI_CALL_METHOD && v->op != XI_CALL_METHOD_DIRECT)
        return false;
    return cg_call_method_matches_receiver_registry_id(
               v, XA_BUILTIN_RECEIVER_METHOD_U8_ARRAY_APPEND_FROM) &&
           v->nargs == 2 &&
           cg_array_value_u8_unchecked_info(ctx, f, v->args[0], &dst_info,
                                            CG_ARRAY_STORAGE_MUTABLE) &&
           cg_span_value_u8_info(ctx, v->args[1], NULL);
}

static bool cg_array_call_is_byte_array_repeat_trusted_nothrow(XiCgenCtx *ctx, const XiFunc *f,
                                                               const XiValue *call) {
    const XiValue *v = cg_unwrap_identity_value(call);
    if (!v)
        return false;

    CgArrayElemInfo dst_info;
    if (v->op == XI_BYTE_ARRAY_REPEAT_FROM) {
        return v->nargs == 3 &&
               cg_array_value_u8_unchecked_info(ctx, f, v->args[0], &dst_info,
                                                CG_ARRAY_STORAGE_MUTABLE) &&
               cg_byte_array_unchecked_int_arg(v->args[1]) &&
               cg_byte_array_unchecked_int_arg(v->args[2]);
    }
    if (v->op != XI_CALL_METHOD && v->op != XI_CALL_METHOD_DIRECT)
        return false;
    return cg_call_method_matches_receiver_registry_id(
               v, XA_BUILTIN_RECEIVER_METHOD_U8_ARRAY_REPEAT_FROM) &&
           v->nargs == 3 &&
           cg_array_value_u8_unchecked_info(ctx, f, v->args[0], &dst_info,
                                            CG_ARRAY_STORAGE_MUTABLE) &&
           cg_byte_array_unchecked_int_arg(v->args[1]) &&
           cg_byte_array_unchecked_int_arg(v->args[2]);
}

static bool cg_byte_slice_load_trusted_nothrow(XiCgenCtx *ctx, const XiFunc *f,
                                               const XiValue *value) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || v->nargs != 3)
        return false;

    switch ((XiOp) v->op) {
        case XI_BYTE_SLICE_LOAD_U16:
        case XI_BYTE_SLICE_LOAD_U32:
        case XI_BYTE_SLICE_LOAD_U64:
        case XI_BYTE_SLICE_LOAD_F32:
        case XI_BYTE_SLICE_LOAD_F64:
            break;
        default:
            return false;
    }

    if (cg_span_value_u8_info(ctx, v->args[0], NULL))
        return true;

    if ((v->aux_int & 1) == 0)
        return false;

    CgArrayElemInfo info;
    return (cg_array_value_storage_info(ctx, f, v->args[0], &info, CG_ARRAY_STORAGE_READ) &&
            cg_array_elem_info_is_u8(&info)) ||
           cg_span_value_u8_info(ctx, v->args[0], NULL);
}

static bool cg_array_err_check_after_byte_slice_load_trusted(XiCgenCtx *ctx, const XiFunc *f,
                                                             const XiValue *check) {
    if (!check || check->op != XI_ERR_CHECK || cg_value_type_is_bool(check))
        return false;
    return cg_byte_slice_load_trusted_nothrow(ctx, f,
                                              cg_class_native_prev_error_source_value(check));
}

static bool cg_array_index_get_trusted_nothrow(XiCgenCtx *ctx, const XiFunc *f,
                                               const XiValue *value) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || v->op != XI_INDEX_GET || v->nargs < 2)
        return false;

    CgArrayElemInfo info;
    if (cg_value_plan_is_span_aggregate(ctx, v->args[0]) &&
        cg_span_elem_info_from_value(ctx, v->args[0], &info))
        return info.rep != XR_REP_TAGGED;

    if (cg_array_value_storage_info(ctx, f, v->args[0], &info, CG_ARRAY_STORAGE_READ))
        return info.rep != XR_REP_TAGGED;

    return false;
}

static bool cg_array_err_check_after_index_get_trusted(XiCgenCtx *ctx, const XiFunc *f,
                                                       const XiValue *check) {
    if (!check || check->op != XI_ERR_CHECK || cg_value_type_is_bool(check))
        return false;
    return cg_array_index_get_trusted_nothrow(ctx, f,
                                              cg_class_native_prev_error_source_value(check));
}

static bool cg_span_common_prefix_trusted_nothrow(XiCgenCtx *ctx, const XiValue *value) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || v->op != XI_BYTE_SLICE_COMMON_PREFIX || v->nargs != 2)
        return false;
    return cg_span_plan_drops(ctx, v, XAOT_SPAN_ACCESS_BYTE_COMMON_PREFIX, XAOT_SPAN_DROP_HELPER);
}

static bool cg_array_err_check_after_direct_byte_array_mutator_trusted(XiCgenCtx *ctx,
                                                                       const XiFunc *f,
                                                                       const XiValue *check) {
    if (!check || check->op != XI_ERR_CHECK || cg_value_type_is_bool(check))
        return false;
    return cg_array_call_is_direct_byte_array_mutator_trusted_nothrow(
        ctx, f, cg_class_native_prev_error_source_value(check));
}

static bool cg_array_err_check_after_byte_array_append_trusted(XiCgenCtx *ctx, const XiFunc *f,
                                                               const XiValue *check) {
    if (!check || check->op != XI_ERR_CHECK || cg_value_type_is_bool(check))
        return false;
    const XiBlock *block = check->block;
    if (!block)
        return false;

    bool seen_check = false;
    for (uint32_t i = block->nvalues; i > 0; i--) {
        const XiValue *cur = block->values[i - 1];
        if (!cur)
            continue;
        if (cur == check) {
            seen_check = true;
            continue;
        }
        if (!seen_check)
            continue;
        if (cg_array_call_is_byte_array_append_trusted_nothrow(ctx, f, cur) ||
            cg_array_call_is_byte_array_repeat_trusted_nothrow(ctx, f, cur))
            return true;
        if (cur->op == XI_RETAIN || cur->op == XI_RELEASE)
            continue;
        if (cur->flags &
            (XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM | XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND))
            return false;
    }
    return false;
}

static bool cg_array_call_is_typed_fill_trusted_nothrow(XiCgenCtx *ctx, const XiFunc *f,
                                                        const XiValue *call) {
    const XiValue *v = cg_unwrap_identity_value(call);
    if (!v || (v->op != XI_CALL_METHOD && v->op != XI_CALL_METHOD_DIRECT))
        return false;
    if (!cg_call_method_matches_receiver_registry_id(v, XA_BUILTIN_RECEIVER_METHOD_ARRAY_FILL) ||
        v->nargs < 2 || v->nargs > 4)
        return false;
    CgArrayElemInfo info;
    if (!cg_array_value_storage_info(ctx, f, v->args[0], &info, CG_ARRAY_STORAGE_MUTABLE))
        return false;
    return info.rep != XR_REP_TAGGED;
}

static bool cg_array_err_check_after_typed_fill_trusted(XiCgenCtx *ctx, const XiFunc *f,
                                                        const XiValue *check) {
    if (!check || check->op != XI_ERR_CHECK || cg_value_type_is_bool(check))
        return false;
    return cg_array_call_is_typed_fill_trusted_nothrow(
        ctx, f, cg_class_native_prev_error_source_value(check));
}

static bool cg_array_fill_value_is_zero_bits_literal(const XiValue *value) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || v->op != XI_CONST || !v->type)
        return false;
    if (v->type->kind == XR_KIND_INT || v->type->kind == XR_KIND_BOOL ||
        v->type->kind == XR_KIND_RUNE)
        return v->aux_int == 0;
    if (v->type->kind == XR_KIND_FLOAT) {
        double f = 0.0;
        memcpy(&f, &v->aux_int, sizeof(double));
        return f == 0.0;
    }
    return false;
}

static bool emit_typed_array_fill_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                       const char *prefix, const XiValue *call) {
    if (!call || call->nargs < 2 || call->nargs > 4)
        return false;

    CgArrayElemInfo info;
    if (!cg_array_value_storage_info(ctx, f, call->args[0], &info, CG_ARRAY_STORAGE_MUTABLE))
        return false;

    const XaotBulkPlan *bulk = cg_required_bulk_plan(ctx, f, call, XG_BULK_FILL, "Array.fill");
    if (call->xg_bulk_op_id != XG_NO_ID && !bulk) {
        emit_codegen_abort_expr(out);
        return true;
    }
    if (bulk && bulk->action != XAOT_BULK_INLINE_MEMSET && bulk->action != XAOT_BULK_TYPED_LOOP &&
        bulk->action != XAOT_BULK_RUNTIME_HELPER) {
        cg_ctx_set_error(ctx);
        emit_codegen_abort_expr(out);
        return true;
    }

    const char *conv_suffix = emit_conversion_prefix(out, call->type, XR_REP_TAGGED, cg_rep(call));
    fprintf(out, "({ xrt_array_t *_a = ");
    emit_typed_array_ptr_expr(ctx, out, f, call->args[0], prefix);
    bool zero_bits = cg_array_fill_value_is_zero_bits_literal(call->args[1]);
    bool byte_pattern = cg_array_elem_info_is_memset_byte_pattern(&info);
    bool use_memset =
        bulk ? bulk->action == XAOT_BULK_INLINE_MEMSET
             : info.elem_name && strcmp(info.elem_name, "XR_ELEM_ANY") != 0 && zero_bits;
    if (use_memset && (zero_bits || byte_pattern)) {
        fprintf(out, "; int64_t _start = ");
        if (call->nargs >= 3)
            emit_value_as_rep_ctx(ctx, out, call->args[2], XR_REP_I64);
        else
            fprintf(out, "0");
        fprintf(out, "; int64_t _end = ");
        if (call->nargs >= 4)
            emit_value_as_rep_ctx(ctx, out, call->args[3], XR_REP_I64);
        else
            fprintf(out, "_a->length");
        fprintf(out, "; XrArrayCoreRange _r = xr_array_core_fill_range(_a->length, _start, _end); "
                     "if (_r.count > 0) memset((uint8_t*)_a->data + "
                     "(size_t)_r.start * (size_t)_a->elem_size, ");
        if (zero_bits)
            fprintf(out, "0");
        else
            emit_value_as_rep_ctx(ctx, out, call->args[1], XR_REP_I64);
        fprintf(out, ", (size_t)_r.count * (size_t)_a->elem_size); "
                     "xr_mkptr(_a, XR_TAG_ARRAY); })");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }
    if (bulk && bulk->action == XAOT_BULK_INLINE_MEMSET) {
        cg_ctx_set_error(ctx);
        fprintf(out, "; ");
        emit_codegen_abort_expr(out);
        fprintf(out, "; })");
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }
    if (bulk && bulk->action == XAOT_BULK_TYPED_LOOP) {
        fprintf(out, "; int64_t _start = ");
        if (call->nargs >= 3)
            emit_value_as_rep_ctx(ctx, out, call->args[2], XR_REP_I64);
        else
            fprintf(out, "0");
        fprintf(out, "; int64_t _end = ");
        if (call->nargs >= 4)
            emit_value_as_rep_ctx(ctx, out, call->args[3], XR_REP_I64);
        else
            fprintf(out, "_a->length");
        if (info.rep == XR_REP_TAGGED) {
            fprintf(out,
                    "; XrArrayCoreRange _r = xr_array_core_fill_range(_a->length, _start, _end); "
                    "XrValue _fill = ");
            emit_value_as_rep_ctx(ctx, out, call->args[1], XR_REP_TAGGED);
            fprintf(out,
                    "; for (int64_t _i = 0; _i < _r.count; _i++) { "
                    "int64_t _idx = _r.start + _i; XrValue _old = ((XrValue*)_a->data)[_idx]; "
                    "xrt_retain(_fill); ((XrValue*)_a->data)[_idx] = _fill; xrt_release(_old); } "
                    "XR_ARRAY_MARK_MUTATED(_a); xr_mkptr(_a, XR_TAG_ARRAY); })");
        } else {
            fprintf(out,
                    "; XrArrayCoreRange _r = xr_array_core_fill_range(_a->length, _start, _end); "
                    "%s _fill = ",
                    info.ctype);
            emit_typed_array_store_value(ctx, out, &info, call->args[1]);
            fprintf(out,
                    "; for (int64_t _i = 0; _i < _r.count; _i++) "
                    "((%s*)_a->data)[_r.start + _i] = _fill; xr_mkptr(_a, XR_TAG_ARRAY); })",
                    info.ctype);
        }
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }
    fprintf(out, "; xrt_array_fill_value(xr_mkptr(_a, XR_TAG_ARRAY), ");
    emit_value_as_rep_ctx(ctx, out, call->args[1], XR_REP_TAGGED);
    fprintf(out, ", ");
    if (call->nargs >= 3)
        emit_value_as_rep_ctx(ctx, out, call->args[2], XR_REP_TAGGED);
    else
        fprintf(out, "XR_FROM_INT(0)");
    fprintf(out, ", ");
    if (call->nargs >= 4)
        emit_value_as_rep_ctx(ctx, out, call->args[3], XR_REP_TAGGED);
    else
        fprintf(out, "XR_FROM_INT(_a->length)");
    fprintf(out, "); })");
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool cg_array_err_check_after_unchecked_fill_push(XiCgenCtx *ctx, const XiFunc *f,
                                                         const XiValue *check) {
    if (!check || check->op != XI_ERR_CHECK || !check->block)
        return false;
    const XiValue *prev = NULL;
    for (uint32_t i = 0; i < check->block->nvalues; i++) {
        const XiValue *cur = check->block->values[i];
        if (cur == check)
            break;
        if (cur)
            prev = cur;
    }
    CgArrayFillLoop fill;
    CgArrayFillLoop unique;
    return cg_array_fill_loop_match(ctx, f, prev, &fill) &&
           cg_array_unique_fill_loop_for_origin(ctx, f, fill.origin, &unique) &&
           unique.push == prev;
}

static bool cg_array_call_is_typed_push(XiCgenCtx *ctx, const XiFunc *f, const XiValue *call) {
    CgArrayElemInfo info;
    if (!call || call->op != XI_CALL_METHOD || call->nargs != 2)
        return false;
    return cg_call_method_matches_receiver_registry_id(call,
                                                       XA_BUILTIN_RECEIVER_METHOD_ARRAY_PUSH) &&
           cg_array_value_storage_info(ctx, f, call->args[0], &info, CG_ARRAY_STORAGE_MUTABLE);
}

static bool cg_array_typed_push_value_is_elided(XiCgenCtx *ctx, const XiFunc *f,
                                                const XiValue *target) {
    if (!f || !cg_array_call_is_typed_push(ctx, f, target))
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t k = 0; k < phi->value.nargs; k++) {
                if (phi->value.args[k] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] != target)
                    continue;
                if (v->op == XI_ERR_CHECK && a == 0)
                    continue;
                return false;
            }
        }
    }
    return true;
}

static bool cg_array_err_check_after_typed_push(XiCgenCtx *ctx, const XiFunc *f,
                                                const XiValue *check) {
    if (!check || check->op != XI_ERR_CHECK || !check->block)
        return false;
    const XiValue *prev = NULL;
    for (uint32_t i = 0; i < check->block->nvalues; i++) {
        const XiValue *cur = check->block->values[i];
        if (cur == check)
            break;
        if (cur)
            prev = cur;
    }
    return cg_array_call_is_typed_push(ctx, f, prev);
}

static bool cg_array_class_field_value_is_elided(XiCgenCtx *ctx, const XiFunc *f,
                                                 const XiValue *target) {
    if (!ctx || !f || !target ||
        !cg_class_native_receiver_ref_field(ctx, f, target, XR_NATIVE_ARRAY_REF, NULL, NULL))
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
                switch ((XiOp) v->op) {
                    case XI_INDEX_GET:
                    case XI_INDEX_SET:
                        if (ai == 0)
                            continue;
                        return false;
                    case XI_LEN:
                        if (ai == 0)
                            continue;
                        return false;
                    case XI_CALL_METHOD: {
                        const char *method = (const char *) v->aux;
                        if (ai == 0 && method &&
                            (strcmp(method, "length") == 0 || strcmp(method, "push") == 0 ||
                             strcmp(method, "map") == 0 || strcmp(method, "filter") == 0 ||
                             strcmp(method, "reduce") == 0))
                            continue;
                        return false;
                    }
                    case XI_RETAIN:
                    case XI_RELEASE:
                        if (ai == 0)
                            continue;
                        return false;
                    default:
                        return false;
                }
            }
        }
    }
    return saw_use;
}

static bool emit_typed_array_length_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                         const char *prefix, const XiValue *v) {
    CgArrayElemInfo info;
    if (!v || v->nargs < 1 ||
        !cg_array_value_storage_info(ctx, f, v->args[0], &info, CG_ARRAY_STORAGE_READ))
        return false;

    const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
    emit_typed_array_ptr_expr(ctx, out, f, v->args[0], prefix);
    fprintf(out, "->length");
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool cg_array_func_is_inline_pure(const XiFunc *f) {
    if (!f)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            if (v->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM))
                return false;
        }
    }
    return true;
}

typedef struct CgArrayInlineMap {
    CgArrayElemInfo src_info;
    CgArrayElemInfo dst_info;
    const XiFunc *target;
    const char *target_prefix;
    XrRep param_rep;
    XrRep return_rep;
} CgArrayInlineMap;

static bool cg_array_call_method_is(const XiValue *v, const char *name) {
    return v && v->op == XI_CALL_METHOD && v->aux && name &&
           strcmp((const char *) v->aux, name) == 0;
}

static bool cg_array_inline_map_info(XiCgenCtx *ctx, const XiFunc *current, const char *prefix,
                                     const XiValue *v, CgArrayInlineMap *out) {
    CgArrayInlineMap info;
    if (!ctx || !cg_array_call_method_is(v, "map") || v->nargs != 2 || !out ||
        !cg_array_elem_info_from_type_ctx(ctx, v->type, &info.dst_info) ||
        !cg_array_value_storage_info(ctx, current, v->args[0], &info.src_info,
                                     CG_ARRAY_STORAGE_READ))
        return false;

    CgStaticFunctionCall cb = cg_resolve_static_function_call(ctx, current, v->args[1]);
    info.target = cb.func;
    if (!info.target || info.target->ncaptures != 0 || info.target->nparams != 1 ||
        !cg_func_uses_typed_abi(ctx, info.target) || !cg_array_func_is_inline_pure(info.target))
        return false;

    info.target_prefix = cb.prefix ? cb.prefix : prefix;
    info.param_rep = cg_func_param_abi_rep(ctx, info.target, 0);
    info.return_rep = cg_func_return_abi_rep(ctx, info.target);
    if ((info.param_rep != XR_REP_I64 && info.param_rep != XR_REP_F64) ||
        info.return_rep != XR_REP_I64)
        return false;
    *out = info;
    return true;
}

static bool emit_typed_array_map_inline_expr_cached(XiCgenCtx *ctx, FILE *out,
                                                    const XiFunc *current, const char *prefix,
                                                    const XiValue *v, const XiValue *cache_value) {
    CgArrayInlineMap map;
    if (!cg_array_inline_map_info(ctx, current, prefix, v, &map))
        return false;
    const XiValue *cached_origin = NULL;
    bool use_cache = cg_array_data_cache_for_value(ctx, v->args[0], &cached_origin);
    fprintf(out, "({ xrt_array_t *_src = (xrt_array_t*)");
    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ".ptr; int64_t _n = _src->length; XrValue _outv = ");
    fprintf(out, "xrt_array_new_typed_uninit(_n, %s); ", map.dst_info.elem_name);
    fprintf(out, "xrt_array_t *_out = (xrt_array_t*)_outv.ptr; ");
    fprintf(out, "%s *_srcd = ", map.src_info.ctype);
    if (use_cache) {
        emit_typed_array_data_cache_ref(out, cached_origin);
        fprintf(out, "; ");
    } else {
        fprintf(out, "(%s*)_src->data; ", map.src_info.ctype);
    }
    fprintf(out, "%s *_dstd = (%s*)_out->data; ", map.dst_info.ctype, map.dst_info.ctype);
    if (cache_value) {
        emit_typed_array_data_cache_ref(out, cache_value);
        fprintf(out, " = _dstd; ");
    }
    fprintf(out, "for (int64_t _i = 0; _i < _n; _i++) { _dstd[_i] = (%s)", map.dst_info.ctype);
    emit_fname(ctx, out, map.target_prefix, map.target);
    /* Yield the result array in the destination value's representation: a
     * bare pointer when select_rep chose PTR for this value, otherwise the
     * tagged XrValue. Without this the statement-expression always produced
     * an XrValue and `void *vN = (XrValue)` failed to compile. */
    fprintf(out, "(NULL, (%s)_srcd[_i]); } _out->length = _n; %s; })", ctype_str(map.param_rep),
            cg_rep(v) == XR_REP_PTR ? "_outv.ptr" : "_outv");
    return true;
}

static bool emit_typed_array_map_inline_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *current,
                                             const char *prefix, const XiValue *v) {
    return emit_typed_array_map_inline_expr_cached(ctx, out, current, prefix, v, NULL);
}

static bool emit_typed_array_map_inline_stmt(XiCgenCtx *ctx, FILE *out, const XiFunc *current,
                                             const char *prefix, const XiValue *v) {
    CgArrayInlineMap map;
    if (!cg_array_inline_map_info(ctx, current, prefix, v, &map) ||
        !cg_array_can_cache_data_for_value(ctx, v, NULL))
        return false;
    fprintf(out, "    %s *", map.dst_info.ctype);
    emit_typed_array_data_cache_ref(out, v);
    fprintf(out, " = NULL;\n");
    fprintf(out, "    ");
    if (!ctx->pre_decl_all)
        fprintf(out, "%s ", ctype_str(cg_rep(v)));
    emit_vref(out, v);
    fprintf(out, " = ");
    emit_typed_array_map_inline_expr_cached(ctx, out, current, prefix, v, v);
    fprintf(out, ";\n");
    return true;
}

static bool cg_array_inline_filter_info(XiCgenCtx *ctx, const XiFunc *current, const char *prefix,
                                        const XiValue *v, CgArrayInlineMap *out) {
    CgArrayInlineMap info;
    if (!ctx || !cg_array_call_method_is(v, "filter") || v->nargs != 2 || !out ||
        !cg_array_elem_info_from_type_ctx(ctx, v->type, &info.dst_info) ||
        !cg_array_value_storage_info(ctx, current, v->args[0], &info.src_info,
                                     CG_ARRAY_STORAGE_READ))
        return false;
    if (strcmp(info.src_info.elem_name, info.dst_info.elem_name) != 0)
        return false;

    CgStaticFunctionCall cb = cg_resolve_static_function_call(ctx, current, v->args[1]);
    info.target = cb.func;
    if (!info.target || info.target->ncaptures != 0 || info.target->nparams != 1 ||
        !cg_func_uses_typed_abi(ctx, info.target) || !cg_array_func_is_inline_pure(info.target))
        return false;

    info.target_prefix = cb.prefix ? cb.prefix : prefix;
    info.param_rep = cg_func_param_abi_rep(ctx, info.target, 0);
    info.return_rep = cg_func_return_abi_rep(ctx, info.target);
    if ((info.param_rep != XR_REP_I64 && info.param_rep != XR_REP_F64) ||
        info.return_rep != XR_REP_I64)
        return false;
    *out = info;
    return true;
}

static bool emit_typed_array_filter_inline_expr_cached(XiCgenCtx *ctx, FILE *out,
                                                       const XiFunc *current, const char *prefix,
                                                       const XiValue *v,
                                                       const XiValue *cache_value) {
    CgArrayInlineMap filter;
    if (!cg_array_inline_filter_info(ctx, current, prefix, v, &filter))
        return false;
    const XiValue *cached_origin = NULL;
    bool use_cache = cg_array_data_cache_for_value(ctx, v->args[0], &cached_origin);
    fprintf(out, "({ xrt_array_t *_src = (xrt_array_t*)");
    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ".ptr; int64_t _n = _src->length; XrValue _outv = ");
    fprintf(out, "xrt_array_new_typed_uninit(_n, %s); ", filter.dst_info.elem_name);
    fprintf(out, "xrt_array_t *_out = (xrt_array_t*)_outv.ptr; ");
    fprintf(out, "%s *_srcd = ", filter.src_info.ctype);
    if (use_cache) {
        emit_typed_array_data_cache_ref(out, cached_origin);
        fprintf(out, "; ");
    } else {
        fprintf(out, "(%s*)_src->data; ", filter.src_info.ctype);
    }
    fprintf(out, "%s *_dstd = (%s*)_out->data; int64_t _out_len = 0; ", filter.dst_info.ctype,
            filter.dst_info.ctype);
    if (cache_value) {
        emit_typed_array_data_cache_ref(out, cache_value);
        fprintf(out, " = _dstd; ");
    }
    fprintf(out, "for (int64_t _i = 0; _i < _n; _i++) { %s _x = _srcd[_i]; if (",
            filter.src_info.ctype);
    emit_fname(ctx, out, filter.target_prefix, filter.target);
    fprintf(out, "(NULL, (%s)_x) != 0) { _dstd[_out_len++] = (%s)_x; } } ",
            ctype_str(filter.param_rep), filter.dst_info.ctype);
    /* Yield in the destination value's representation (PTR -> bare pointer,
     * else tagged XrValue); matches the `<ctype> vN =` declaration site. */
    fprintf(out, "_out->length = _out_len; %s; })",
            cg_rep(v) == XR_REP_PTR ? "_outv.ptr" : "_outv");
    return true;
}

static bool emit_typed_array_filter_inline_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *current,
                                                const char *prefix, const XiValue *v) {
    return emit_typed_array_filter_inline_expr_cached(ctx, out, current, prefix, v, NULL);
}

static bool emit_typed_array_filter_inline_stmt(XiCgenCtx *ctx, FILE *out, const XiFunc *current,
                                                const char *prefix, const XiValue *v) {
    CgArrayInlineMap filter;
    if (!cg_array_inline_filter_info(ctx, current, prefix, v, &filter) ||
        !cg_array_can_cache_data_for_value(ctx, v, NULL))
        return false;
    fprintf(out, "    %s *", filter.dst_info.ctype);
    emit_typed_array_data_cache_ref(out, v);
    fprintf(out, " = NULL;\n");
    fprintf(out, "    ");
    if (!ctx->pre_decl_all)
        fprintf(out, "%s ", ctype_str(cg_rep(v)));
    emit_vref(out, v);
    fprintf(out, " = ");
    emit_typed_array_filter_inline_expr_cached(ctx, out, current, prefix, v, v);
    fprintf(out, ";\n");
    return true;
}

typedef struct CgArrayInlineReduce {
    CgArrayElemInfo src_info;
    const XiFunc *target;
    const char *target_prefix;
    XrRep acc_rep;
    XrRep elem_param_rep;
} CgArrayInlineReduce;

static bool cg_array_inline_reduce_info(XiCgenCtx *ctx, const XiFunc *current, const char *prefix,
                                        const XiValue *v, CgArrayInlineReduce *out) {
    CgArrayInlineReduce info;
    memset(&info, 0, sizeof(info));
    if (!ctx || !cg_array_call_method_is(v, "reduce") || v->nargs != 3 || !out ||
        !cg_array_value_storage_info(ctx, current, v->args[0], &info.src_info,
                                     CG_ARRAY_STORAGE_READ))
        return false;

    CgStaticFunctionCall cb = cg_resolve_static_function_call(ctx, current, v->args[1]);
    info.target = cb.func;
    if (!info.target || info.target->ncaptures != 0 || info.target->nparams != 2 ||
        !cg_func_uses_typed_abi(ctx, info.target) || !cg_array_func_is_inline_pure(info.target))
        return false;

    info.target_prefix = cb.prefix ? cb.prefix : prefix;
    info.acc_rep = cg_rep(v);
    info.elem_param_rep = cg_func_param_abi_rep(ctx, info.target, 1);
    if ((info.acc_rep != XR_REP_I64 && info.acc_rep != XR_REP_F64) ||
        cg_func_param_abi_rep(ctx, info.target, 0) != info.acc_rep ||
        (info.elem_param_rep != XR_REP_I64 && info.elem_param_rep != XR_REP_F64) ||
        info.elem_param_rep != info.src_info.rep ||
        cg_func_return_abi_rep(ctx, info.target) != info.acc_rep)
        return false;
    *out = info;
    return true;
}

static bool emit_typed_array_reduce_inline_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *current,
                                                const char *prefix, const XiValue *v) {
    CgArrayInlineReduce reduce;
    if (!cg_array_inline_reduce_info(ctx, current, prefix, v, &reduce))
        return false;
    const XiValue *cached_origin = NULL;
    bool use_cache = cg_array_data_cache_for_value(ctx, v->args[0], &cached_origin);
    fprintf(out, "({ xrt_array_t *_src = (xrt_array_t*)");
    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ".ptr; int64_t _n = _src->length; %s _acc = (%s)", ctype_str(reduce.acc_rep),
            ctype_str(reduce.acc_rep));
    emit_value_as_rep(out, v->args[2], reduce.acc_rep);
    fprintf(out, "; %s *_srcd = ", reduce.src_info.ctype);
    if (use_cache) {
        emit_typed_array_data_cache_ref(out, cached_origin);
        fprintf(out, "; ");
    } else {
        fprintf(out, "(%s*)_src->data; ", reduce.src_info.ctype);
    }
    fprintf(out, "for (int64_t _i = 0; _i < _n; _i++) { _acc = (%s)", ctype_str(reduce.acc_rep));
    emit_fname(ctx, out, reduce.target_prefix, reduce.target);
    fprintf(out, "(NULL, (%s)_acc, (%s)_srcd[_i]); } _acc; })", ctype_str(reduce.acc_rep),
            ctype_str(reduce.elem_param_rep));
    return true;
}

static bool cg_array_func_has_error_flow(const XiFunc *f) {
    if (!f)
        return true;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (v && (v->flags & (XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND)))
                return true;
        }
    }
    return false;
}

static bool cg_array_call_is_inline_hof(XiCgenCtx *ctx, const XiFunc *current, const char *prefix,
                                        const XiValue *call) {
    const char *method = (call && call->op == XI_CALL_METHOD) ? (const char *) call->aux : NULL;
    CgArrayInlineMap map;
    CgArrayInlineReduce reduce;
    if (!method)
        return false;
    if (strcmp(method, "map") == 0 && cg_array_inline_map_info(ctx, current, prefix, call, &map))
        return !cg_array_func_has_error_flow(map.target);
    if (strcmp(method, "filter") == 0 &&
        cg_array_inline_filter_info(ctx, current, prefix, call, &map))
        return !cg_array_func_has_error_flow(map.target);
    if (strcmp(method, "reduce") == 0 &&
        cg_array_inline_reduce_info(ctx, current, prefix, call, &reduce))
        return !cg_array_func_has_error_flow(reduce.target);
    return false;
}

static bool cg_array_err_check_after_inline_hof(XiCgenCtx *ctx, const XiFunc *current,
                                                const char *prefix, const XiValue *check) {
    if (!check || check->op != XI_ERR_CHECK || !check->block)
        return false;
    const XiValue *prev = NULL;
    for (uint32_t i = 0; i < check->block->nvalues; i++) {
        const XiValue *cur = check->block->values[i];
        if (cur == check)
            break;
        if (cur)
            prev = cur;
    }
    return cg_array_call_is_inline_hof(ctx, current, prefix, prev);
}

static bool cg_array_value_wraps_closure_new(const XiValue *value) {
    const XiValue *v = cg_unwrap_identity_value(value);
    return v && v->op == XI_CLOSURE_NEW;
}

static bool cg_array_value_only_feeds_inline_map(XiCgenCtx *ctx, const XiFunc *current,
                                                 const char *prefix, const XiValue *target,
                                                 uint8_t depth, bool *used) {
    const XiFunc *f = (target && target->block) ? target->block->func : NULL;
    if (!ctx || !f || !target || depth > 8 || !used)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *cur = blk->values[vi];
            if (!cur || cur == target)
                continue;
            for (uint16_t ai = 0; ai < cur->nargs; ai++) {
                if (cur->args[ai] != target)
                    continue;
                const char *method = (const char *) cur->aux;
                if ((cur->op == XI_BOX || cur->op == XI_UNBOX || cur->op == XI_COPY ||
                     cur->op == XI_MOVE) &&
                    ai == 0 && cg_array_value_wraps_closure_new(cur)) {
                    if (!cg_array_value_only_feeds_inline_map(ctx, current, prefix, cur, depth + 1,
                                                              used))
                        return false;
                    continue;
                }
                if (cur->op != XI_CALL_METHOD || ai != 1 || !method ||
                    ((strcmp(method, "map") != 0 ||
                      !cg_array_inline_map_info(ctx, current, prefix, cur,
                                                &(CgArrayInlineMap) {0})) &&
                     (strcmp(method, "filter") != 0 ||
                      !cg_array_inline_filter_info(ctx, current, prefix, cur,
                                                   &(CgArrayInlineMap) {0})) &&
                     (strcmp(method, "reduce") != 0 ||
                      !cg_array_inline_reduce_info(ctx, current, prefix, cur,
                                                   &(CgArrayInlineReduce) {0}))))
                    return false;
                *used = true;
            }
        }
    }
    return true;
}

static bool cg_array_closure_value_only_used_by_inline_map(XiCgenCtx *ctx, const XiFunc *current,
                                                           const char *prefix,
                                                           const XiValue *value) {
    bool used = false;
    return cg_array_value_wraps_closure_new(value) &&
           cg_array_value_only_feeds_inline_map(ctx, current, prefix, value, 0, &used) && used;
}

static bool emit_typed_array_map_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *current,
                                      const char *prefix, const XiValue *v) {
    CgArrayElemInfo info;
    if (!v || v->nargs != 2 || !cg_array_elem_info_from_type_ctx(ctx, v->type, &info))
        return false;
    if (emit_typed_array_map_inline_expr(ctx, out, current, prefix, v))
        return true;

    const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
    fprintf(out, "xrt_array_map_typed(");
    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ", %s)", info.elem_name);
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool emit_typed_array_filter_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *current,
                                         const char *prefix, const XiValue *v) {
    CgArrayElemInfo info;
    if (!v || v->nargs != 2 || !cg_array_elem_info_from_type_ctx(ctx, v->type, &info))
        return false;
    if (emit_typed_array_filter_inline_expr(ctx, out, current, prefix, v))
        return true;

    const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
    fprintf(out, "xrt_array_filter_typed(");
    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ")");
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool emit_typed_array_reduce_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *current,
                                         const char *prefix, const XiValue *v) {
    CgArrayElemInfo info;
    if (!v || v->nargs != 3 ||
        !cg_array_value_storage_info(ctx, current, v->args[0], &info, CG_ARRAY_STORAGE_READ))
        return false;
    if (emit_typed_array_reduce_inline_expr(ctx, out, current, prefix, v))
        return true;

    const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
    fprintf(out, "xrt_array_reduce_typed(");
    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep(out, v->args[2], XR_REP_TAGGED);
    fprintf(out, ")");
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

/* find / findIndex / every / some all take a single predicate closure and return
 * a scalar (element / int / bool). They share the same emit shape: confirm the
 * receiver is an array, then call the runtime HOF helper with tagged operands. */
static bool emit_typed_array_predicate_hof_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *current,
                                                const char *prefix, const XiValue *v,
                                                const char *helper) {
    (void) ctx;
    (void) current;
    (void) prefix;
    // Guard on the receiver's static type, not a storage plan: these HOFs operate
    // on the tagged array value and do not need the typed-storage prepare pass.
    if (!v || v->nargs != 2 || !v->args[0] || !v->args[0]->type ||
        v->args[0]->type->kind != XR_KIND_ARRAY)
        return false;
    const char *conv_suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
    fprintf(out, "%s(", helper);
    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ")");
    emit_conversion_suffix(out, conv_suffix);
    return true;
}
