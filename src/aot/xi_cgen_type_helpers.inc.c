static const char *cg_channel_field_helper(const char *field) {
    if (!field)
        return NULL;
    if (strcmp(field, "length") == 0)
        return "xr_aot_chan_length";
    if (strcmp(field, "capacity") == 0)
        return "xr_aot_chan_capacity";
    if (strcmp(field, "isClosed") == 0)
        return "xr_aot_chan_is_closed";
    return NULL;
}

static const char *cg_work_queue_field_helper(const char *field) {
    if (!field)
        return NULL;
    if (strcmp(field, "length") == 0)
        return "xr_aot_work_queue_length";
    if (strcmp(field, "shardCount") == 0)
        return "xr_aot_work_queue_shard_count";
    if (strcmp(field, "isClosed") == 0)
        return "xr_aot_work_queue_is_closed";
    return NULL;
}

static const char *cg_result_group_field_helper(const char *field) {
    if (!field)
        return NULL;
    if (strcmp(field, "length") == 0)
        return "xr_aot_result_group_length";
    if (strcmp(field, "pendingCount") == 0)
        return "xr_aot_result_group_pending_count";
    if (strcmp(field, "batchSize") == 0)
        return "xr_aot_result_group_batch_size";
    if (strcmp(field, "isClosed") == 0)
        return "xr_aot_result_group_is_closed";
    return NULL;
}

static const char *cg_countdown_latch_field_helper(const char *field) {
    if (!field)
        return NULL;
    if (strcmp(field, "remaining") == 0)
        return "xr_aot_countdown_latch_remaining";
    if (strcmp(field, "isClosed") == 0)
        return "xr_aot_countdown_latch_is_closed";
    return NULL;
}

static const char *cg_semaphore_field_helper(const char *field) {
    if (!field)
        return NULL;
    if (strcmp(field, "available") == 0)
        return "xr_aot_semaphore_available";
    if (strcmp(field, "isClosed") == 0)
        return "xr_aot_semaphore_is_closed";
    return NULL;
}

static const char *cg_event_count_field_helper(const char *field) {
    if (!field)
        return NULL;
    if (strcmp(field, "epoch") == 0)
        return "xr_aot_event_count_epoch";
    if (strcmp(field, "isClosed") == 0)
        return "xr_aot_event_count_is_closed";
    return NULL;
}

static bool cg_value_type_is_bool(const XiValue *v) {
    return v && v->type && v->type->kind == XR_KIND_BOOL;
}

static bool cg_type_is_json(const XrType *type) {
    if (!type)
        return false;
    if (XR_TYPE_HAS_OBJECT_SHAPE(type))
        return true;
    if (type->kind == XR_KIND_INSTANCE && type->instance.class_name &&
        (strcmp(type->instance.class_name, "PathInfo") == 0 ||
         strcmp(type->instance.class_name, "ExecResult") == 0))
        return true;
    if (type->kind == XR_KIND_UNION) {
        for (uint8_t i = 0; i < type->union_type.member_count; i++) {
            if (cg_type_is_json(type->union_type.members[i]))
                return true;
        }
    }
    return false;
}

static bool cg_value_type_is_json(const XiValue *v) {
    while (v && (v->op == XI_BOX || v->op == XI_UNBOX || xi_copy_is_identity_alias(v)) &&
           v->nargs >= 1)
        v = v->args[0];
    return v && cg_type_is_json(v->type);
}

static const char *cg_task_field_helper(const char *field) {
    if (!field)
        return NULL;
    if (strcmp(field, "done") == 0)
        return "xr_aot_task_done";
    if (strcmp(field, "status") == 0)
        return "xr_aot_task_status";
    return NULL;
}

typedef struct CgSetElemInfo {
    const char *elem_name;
    XrRep rep;
} CgSetElemInfo;

typedef struct CgMapElemInfo {
    CgSetElemInfo key;
    CgSetElemInfo value;
} CgMapElemInfo;

static bool cg_set_elem_info_from_plan(const XaotContainerElemPlan *plan, CgSetElemInfo *out) {
    if (!plan || !plan->elem_name || !out)
        return false;
    *out = (CgSetElemInfo) {plan->elem_name, plan->storage_rep};
    return true;
}

static bool cg_set_type_direct_info_ctx(XiCgenCtx *ctx, const XrType *type, CgSetElemInfo *out) {
    const XaotContainerTypePlan *plan =
        xaot_bundle_find_container_plan(cg_ctx_aot_bundle(ctx), type);
    if (!plan || plan->plan.kind != XAOT_CONTAINER_SET ||
        (plan->plan.flags & XAOT_CONTAINER_DIRECT_HELPERS) == 0)
        return false;
    return cg_set_elem_info_from_plan(&plan->plan.elem, out);
}

static bool cg_map_type_direct_info_ctx(XiCgenCtx *ctx, const XrType *type, CgMapElemInfo *out) {
    const XaotContainerTypePlan *plan =
        xaot_bundle_find_container_plan(cg_ctx_aot_bundle(ctx), type);
    if (!plan || plan->plan.kind != XAOT_CONTAINER_MAP ||
        (plan->plan.flags & XAOT_CONTAINER_DIRECT_HELPERS) == 0 || !out)
        return false;
    memset(out, 0, sizeof(*out));
    return cg_set_elem_info_from_plan(&plan->plan.key, &out->key) &&
           cg_set_elem_info_from_plan(&plan->plan.value, &out->value);
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

/* Map<bool,i64> / Map<bool,f32>: the value layouts codegen routes to the
 * bool-direct helpers and therefore stores in the 2-slot xrt_boolmap_t. Every
 * other bool-keyed value (i32, f64, refs, ...) keeps the generic typed map. */
static bool cg_map_info_is_boolmap(const CgMapElemInfo *info) {
    return info && strcmp(info->key.elem_name, "XR_ELEM_BOOL") == 0 &&
           (strcmp(info->value.elem_name, "XR_ELEM_I64") == 0 ||
            strcmp(info->value.elem_name, "XR_ELEM_F32") == 0);
}

static bool cg_map_type_is_boolmap_ctx(XiCgenCtx *ctx, const XrType *type) {
    CgMapElemInfo info;
    return cg_map_type_direct_info_ctx(ctx, type, &info) && cg_map_info_is_boolmap(&info);
}

static bool emit_typed_map_new_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v, int64_t cap) {
    CgMapElemInfo info;
    const XaotContainerTypePlan *plan =
        v ? xaot_bundle_find_container_plan(cg_ctx_aot_bundle(ctx), v->type) : NULL;
    if (!out || !v || !plan || plan->plan.kind != XAOT_CONTAINER_MAP ||
        !cg_set_elem_info_from_plan(&plan->plan.key, &info.key) ||
        !cg_set_elem_info_from_plan(&plan->plan.value, &info.value))
        return false;
    if (cg_map_info_is_boolmap(&info)) {
        fprintf(out, "xrt_boolmap_new_typed(%" PRId64 ", %s)", cap, info.value.elem_name);
        return true;
    }
    fprintf(out, "xrt_map_new_typed(%" PRId64 ", %s, %s)", cap, info.key.elem_name,
            info.value.elem_name);
    return true;
}

static bool emit_typed_set_new_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v, int64_t cap) {
    CgSetElemInfo info;
    const XaotContainerTypePlan *plan =
        v ? xaot_bundle_find_container_plan(cg_ctx_aot_bundle(ctx), v->type) : NULL;
    if (!out || !v || !plan || plan->plan.kind != XAOT_CONTAINER_SET ||
        !cg_set_elem_info_from_plan(&plan->plan.elem, &info))
        return false;
    fprintf(out, "xrt_set_new_typed(%" PRId64 ", %s)", cap, info.elem_name);
    return true;
}
