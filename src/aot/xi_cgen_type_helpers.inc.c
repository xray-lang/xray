static bool cg_type_is_channel(const XrType *type) {
    if (!type)
        return false;
    if (type->kind == XR_KIND_CHANNEL)
        return true;
    if (type->kind == XR_KIND_UNION) {
        for (uint8_t i = 0; i < type->union_type.member_count; i++) {
            if (cg_type_is_channel(type->union_type.members[i]))
                return true;
        }
    }
    return false;
}

static bool cg_value_type_is_channel(const XiValue *v) {
    while (v && (v->op == XI_BOX || v->op == XI_UNBOX || v->op == XI_COPY) && v->nargs >= 1)
        v = v->args[0];
    return v && cg_type_is_channel(v->type);
}

static bool cg_value_type_is_bool(const XiValue *v) {
    return v && v->type && v->type->kind == XR_KIND_BOOL;
}

static bool cg_type_is_task(const XrType *type) {
    if (!type)
        return false;
    if (type->kind == XR_KIND_INSTANCE)
        return type->instance.class_name && strcmp(type->instance.class_name, "Task") == 0;
    if (type->kind == XR_KIND_UNION) {
        for (uint8_t i = 0; i < type->union_type.member_count; i++) {
            if (cg_type_is_task(type->union_type.members[i]))
                return true;
        }
    }
    return false;
}

static bool cg_value_type_is_task(const XiValue *v) {
    while (v && (v->op == XI_BOX || v->op == XI_UNBOX || v->op == XI_COPY) && v->nargs >= 1)
        v = v->args[0];
    return v && cg_type_is_task(v->type);
}

static const char *cg_task_field_helper(const char *field) {
    if (!field)
        return NULL;
    if (strcmp(field, "done") == 0)
        return "xr_aot_task_done";
    if (strcmp(field, "cancelled") == 0)
        return "xr_aot_task_cancelled";
    if (strcmp(field, "result") == 0)
        return "xr_aot_task_result";
    if (strcmp(field, "error") == 0)
        return "xr_aot_task_error";
    return NULL;
}

static bool cg_task_field_needs_xrt_bridge(const char *field) {
    return field && (strcmp(field, "result") == 0 || strcmp(field, "error") == 0);
}

static bool cg_channel_method_may_suspend(const XiValue *v) {
    if (!v || v->op != XI_CALL_METHOD || v->nargs < 1 || !cg_value_type_is_channel(v->args[0]))
        return false;
    const char *method = (const char *) v->aux;
    return method && (strcmp(method, "send") == 0 || strcmp(method, "recv") == 0 ||
                      strcmp(method, "sendTimeout") == 0 || strcmp(method, "recvTimeout") == 0);
}

typedef struct CgSetElemInfo {
    const char *elem_name;
    XrRep rep;
} CgSetElemInfo;

typedef struct CgMapElemInfo {
    CgSetElemInfo key;
    CgSetElemInfo value;
} CgMapElemInfo;

static bool cg_set_elem_info_from_elem_type(const XrType *elem, CgSetElemInfo *out) {
    if (!elem || elem->is_nullable || !out)
        return false;

    memset(out, 0, sizeof(*out));
    if (elem->native_width != 0) {
        switch (elem->native_width) {
            case XR_NATIVE_I8:
                *out = (CgSetElemInfo) {"XR_ELEM_I8", XR_REP_I64};
                return true;
            case XR_NATIVE_U8:
                *out = (CgSetElemInfo) {"XR_ELEM_U8", XR_REP_I64};
                return true;
            case XR_NATIVE_I16:
                *out = (CgSetElemInfo) {"XR_ELEM_I16", XR_REP_I64};
                return true;
            case XR_NATIVE_U16:
                *out = (CgSetElemInfo) {"XR_ELEM_U16", XR_REP_I64};
                return true;
            case XR_NATIVE_I32:
                *out = (CgSetElemInfo) {"XR_ELEM_I32", XR_REP_I64};
                return true;
            case XR_NATIVE_U32:
                *out = (CgSetElemInfo) {"XR_ELEM_U32", XR_REP_I64};
                return true;
            case XR_NATIVE_U64:
                *out = (CgSetElemInfo) {"XR_ELEM_U64", XR_REP_I64};
                return true;
            case XR_NATIVE_F32:
                *out = (CgSetElemInfo) {"XR_ELEM_F32", XR_REP_F64};
                return true;
            case XR_NATIVE_F64:
                *out = (CgSetElemInfo) {"XR_ELEM_F64", XR_REP_F64};
                return true;
            case XR_NATIVE_BOOL:
                *out = (CgSetElemInfo) {"XR_ELEM_BOOL", XR_REP_I64};
                return true;
            default:
                break;
        }
    }

    if (elem->kind == XR_KIND_INT) {
        *out = (CgSetElemInfo) {"XR_ELEM_I64", XR_REP_I64};
        return true;
    }
    if (elem->kind == XR_KIND_FLOAT) {
        *out = (CgSetElemInfo) {"XR_ELEM_F64", XR_REP_F64};
        return true;
    }
    if (elem->kind == XR_KIND_BOOL) {
        *out = (CgSetElemInfo) {"XR_ELEM_BOOL", XR_REP_I64};
        return true;
    }
    return false;
}

static bool cg_set_elem_info_from_type(const XrType *type, CgSetElemInfo *out) {
    if (!type || type->kind != XR_KIND_SET)
        return false;
    return cg_set_elem_info_from_elem_type(type->container.element_type, out);
}

static bool cg_set_type_is_i64_storage(const XrType *type) {
    CgSetElemInfo info;
    return cg_set_elem_info_from_type(type, &info) && strcmp(info.elem_name, "XR_ELEM_I64") == 0;
}

static bool cg_set_type_direct_info(const XrType *type, CgSetElemInfo *out) {
    CgSetElemInfo info;
    if (!cg_set_elem_info_from_type(type, &info))
        return false;
    if (strcmp(info.elem_name, "XR_ELEM_ANY") == 0)
        return false;
    if (out)
        *out = info;
    return true;
}

static bool cg_map_elem_info_from_type(const XrType *type, CgMapElemInfo *out) {
    if (!type || type->kind != XR_KIND_MAP || !out)
        return false;
    memset(out, 0, sizeof(*out));
    return cg_set_elem_info_from_elem_type(type->map.key_type, &out->key) &&
           cg_set_elem_info_from_elem_type(type->map.value_type, &out->value);
}

static bool cg_map_type_direct_info(const XrType *type, CgMapElemInfo *out) {
    CgMapElemInfo info;
    if (!cg_map_elem_info_from_type(type, &info))
        return false;
    if ((info.key.rep != XR_REP_I64 && info.key.rep != XR_REP_F64) ||
        (info.value.rep != XR_REP_I64 && info.value.rep != XR_REP_F64))
        return false;
    if (out)
        *out = info;
    return true;
}

static const char *cg_map_direct_get_helper(const CgMapElemInfo *info) {
    if (!info)
        return NULL;
    if (strcmp(info->key.elem_name, "XR_ELEM_F32") == 0 &&
        strcmp(info->value.elem_name, "XR_ELEM_F32") == 0)
        return "xrt_map_get_f32_f32_typed";
    if (strcmp(info->key.elem_name, "XR_ELEM_BOOL") == 0 &&
        strcmp(info->value.elem_name, "XR_ELEM_F32") == 0)
        return "xrt_map_get_bool_f32_typed";
    if (strcmp(info->key.elem_name, "XR_ELEM_BOOL") == 0 &&
        strcmp(info->value.elem_name, "XR_ELEM_I64") == 0)
        return "xrt_map_get_bool_i64_typed";
    if (info->key.rep == XR_REP_I64 && info->value.rep == XR_REP_I64)
        return "xrt_map_get_i64_i64_typed";
    if (info->key.rep == XR_REP_I64 && info->value.rep == XR_REP_F64)
        return "xrt_map_get_i64_f64_typed";
    if (info->key.rep == XR_REP_F64 && info->value.rep == XR_REP_I64)
        return "xrt_map_get_f64_i64_typed";
    if (info->key.rep == XR_REP_F64 && info->value.rep == XR_REP_F64)
        return "xrt_map_get_f64_f64_typed";
    return NULL;
}

static const char *cg_map_direct_has_helper(const CgMapElemInfo *info) {
    if (!info)
        return NULL;
    if (strcmp(info->key.elem_name, "XR_ELEM_F32") == 0 &&
        strcmp(info->value.elem_name, "XR_ELEM_F32") == 0)
        return "xrt_map_has_f32_f32_typed";
    if (strcmp(info->key.elem_name, "XR_ELEM_BOOL") == 0 &&
        strcmp(info->value.elem_name, "XR_ELEM_F32") == 0)
        return "xrt_map_has_bool_f32_typed";
    if (strcmp(info->key.elem_name, "XR_ELEM_BOOL") == 0 &&
        strcmp(info->value.elem_name, "XR_ELEM_I64") == 0)
        return "xrt_map_has_bool_i64_typed";
    if (info->key.rep == XR_REP_I64)
        return "xrt_map_has_i64_typed";
    if (info->key.rep == XR_REP_F64)
        return "xrt_map_has_f64_typed";
    return NULL;
}

static const char *cg_map_direct_delete_helper(const CgMapElemInfo *info) {
    if (!info)
        return NULL;
    if (strcmp(info->key.elem_name, "XR_ELEM_F32") == 0 &&
        strcmp(info->value.elem_name, "XR_ELEM_F32") == 0)
        return "xrt_map_delete_f32_f32_typed";
    if (strcmp(info->key.elem_name, "XR_ELEM_BOOL") == 0 &&
        strcmp(info->value.elem_name, "XR_ELEM_F32") == 0)
        return "xrt_map_delete_bool_f32_typed";
    if (strcmp(info->key.elem_name, "XR_ELEM_BOOL") == 0 &&
        strcmp(info->value.elem_name, "XR_ELEM_I64") == 0)
        return "xrt_map_delete_bool_i64_typed";
    if (info->key.rep == XR_REP_I64)
        return "xrt_map_delete_i64_typed";
    if (info->key.rep == XR_REP_F64)
        return "xrt_map_delete_f64_typed";
    return NULL;
}

static const char *cg_map_direct_set_helper(const CgMapElemInfo *info) {
    if (!info)
        return NULL;
    if (strcmp(info->key.elem_name, "XR_ELEM_F32") == 0 &&
        strcmp(info->value.elem_name, "XR_ELEM_F32") == 0)
        return "xrt_map_set_f32_f32_typed";
    if (strcmp(info->key.elem_name, "XR_ELEM_BOOL") == 0 &&
        strcmp(info->value.elem_name, "XR_ELEM_F32") == 0)
        return "xrt_map_set_bool_f32_typed";
    if (strcmp(info->key.elem_name, "XR_ELEM_BOOL") == 0 &&
        strcmp(info->value.elem_name, "XR_ELEM_I64") == 0)
        return "xrt_map_set_bool_i64_typed";
    if (info->key.rep == XR_REP_I64 && info->value.rep == XR_REP_I64)
        return "xrt_map_set_i64_i64_typed";
    if (info->key.rep == XR_REP_I64 && info->value.rep == XR_REP_F64)
        return "xrt_map_set_i64_f64_typed";
    if (info->key.rep == XR_REP_F64 && info->value.rep == XR_REP_I64)
        return "xrt_map_set_f64_i64_typed";
    if (info->key.rep == XR_REP_F64 && info->value.rep == XR_REP_F64)
        return "xrt_map_set_f64_f64_typed";
    return NULL;
}

static bool emit_typed_map_new_expr(FILE *out, const XiValue *v, int64_t cap) {
    CgMapElemInfo info;
    if (!out || !v || !cg_map_elem_info_from_type(v->type, &info))
        return false;
    fprintf(out, "xrt_map_new_typed(%" PRId64 ", %s, %s)", cap, info.key.elem_name,
            info.value.elem_name);
    return true;
}

static bool emit_typed_set_new_expr(FILE *out, const XiValue *v, int64_t cap) {
    CgSetElemInfo info;
    if (!out || !v || !cg_set_elem_info_from_type(v->type, &info))
        return false;
    fprintf(out, "xrt_set_new_typed(%" PRId64 ", %s)", cap, info.elem_name);
    return true;
}
