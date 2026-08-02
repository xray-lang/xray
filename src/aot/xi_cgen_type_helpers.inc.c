static const char *cg_channel_field_helper(const char *field) {
    if (!field)
        return NULL;
    if (strcmp(field, "capacity") == 0)
        return "xr_aot_chan_capacity";
    if (strcmp(field, "isClosed") == 0)
        return "xr_aot_chan_is_closed";
    return NULL;
}

static const char *cg_work_queue_field_helper(const char *field) {
    if (!field)
        return NULL;
    if (strcmp(field, "shardCount") == 0)
        return "xr_aot_work_queue_shard_count";
    if (strcmp(field, "isClosed") == 0)
        return "xr_aot_work_queue_is_closed";
    return NULL;
}

static const char *cg_result_group_field_helper(const char *field) {
    if (!field)
        return NULL;
    if (strcmp(field, "readyCount") == 0)
        return "xr_aot_result_group_ready_count";
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
    /* __ExecResult is the private core.def Json-backed handle used by os.exec;
     * its VM representation is a Json object. PathInfo was such a handle too,
     * but the path module migrated
     * to a pure-Xray class, so it must go through normal class field access. */
    if (xr_type_is_builtin_named_class(type, "__ExecResult"))
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

static const char *cg_thread_field_helper(const char *field) {
    if (!field)
        return NULL;
    if (strcmp(field, "done") == 0)
        return "xrt_thread_done_value";
    return NULL;
}

typedef struct CgSetElemInfo {
    const char *elem_name; /* owned: static element-kind token literal */
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

static bool cg_key_access_method_op(uint8_t container_kind, const char *method, uint16_t nargs,
                                    uint8_t *out_op) {
    uint8_t op = 0;
    if (!method)
        return false;
    if (container_kind == XG_MAP_CONTAINER_MAP) {
        if (nargs == 1 && strcmp(method, "get") == 0)
            op = XG_KEY_ACCESS_GET;
        else if (nargs == 1 && strcmp(method, "containsKey") == 0)
            op = XG_KEY_ACCESS_HAS;
        else if (nargs == 1 && strcmp(method, "delete") == 0)
            op = XG_KEY_ACCESS_DELETE;
        else if (nargs == 2 && strcmp(method, "set") == 0)
            op = XG_KEY_ACCESS_SET;
        else if (nargs == 0 && strcmp(method, "clear") == 0)
            op = XG_KEY_ACCESS_CLEAR;
    } else if (container_kind == XG_MAP_CONTAINER_SET) {
        if (nargs == 1 && strcmp(method, "contains") == 0)
            op = XG_KEY_ACCESS_HAS;
        else if (nargs == 1 && strcmp(method, "add") == 0)
            op = XG_KEY_ACCESS_ADD;
        else if (nargs == 1 && strcmp(method, "delete") == 0)
            op = XG_KEY_ACCESS_DELETE;
        else if (nargs == 0 && strcmp(method, "clear") == 0)
            op = XG_KEY_ACCESS_CLEAR;
    }
    if (op == 0)
        return false;
    if (out_op)
        *out_op = op;
    return true;
}

static const XaotKeyAccessPlan *cg_verified_key_access_plan(XiCgenCtx *ctx, const XiValue *v,
                                                            uint8_t expected_container,
                                                            uint8_t expected_op, const char *site) {
    if (!v || v->xg_key_access_id == 0)
        return NULL;
    const XaotKeyAccessPlan *plan =
        xaot_bundle_find_key_access_plan(cg_ctx_aot_bundle(ctx), v->xg_key_access_id);
    if (!plan) {
        cg_ctx_set_error(ctx);
        fprintf(stderr,
                "[xi_cgen] ERROR: missing verified key-access plan for %s Xi value v%u "
                "(key_access=%u)\n",
                site ? site : "Map/Set", v->id, v->xg_key_access_id);
        return NULL;
    }
    if (plan->container_kind != expected_container || plan->op != expected_op ||
        (plan->source_span_id != 0 && v->line > 0 && plan->source_span_id != (uint32_t) v->line)) {
        cg_ctx_set_error(ctx);
        fprintf(stderr,
                "[xi_cgen] ERROR: stale key-access plan for %s Xi value v%u "
                "(key_access=%u container=%u op=%u span=%u action=%u)\n",
                site ? site : "Map/Set", v->id, v->xg_key_access_id,
                (unsigned) plan->container_kind, (unsigned) plan->op,
                (unsigned) plan->source_span_id, (unsigned) plan->action);
        return NULL;
    }
    if (plan->action == XAOT_KEY_ACCESS_REJECT) {
        cg_ctx_set_error(ctx);
        fprintf(stderr,
                "[xi_cgen] ERROR: rejected key-access plan for %s Xi value v%u "
                "(key_access=%u reason=%u)\n",
                site ? site : "Map/Set", v->id, v->xg_key_access_id,
                (unsigned) plan->unproven_reason);
        return NULL;
    }
    return plan;
}

static const XaotMapShapePlan *
cg_key_access_receiver_shape_plan(XiCgenCtx *ctx, const XaotKeyAccessPlan *plan, const char *site) {
    if (!plan || plan->receiver_shape_id == XG_NO_ID)
        return NULL;
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    const XaotMapShapePlan *shape =
        bundle ? xaot_bundle_find_map_shape_plan(bundle, plan->receiver_shape_id) : NULL;
    if (!shape) {
        cg_ctx_set_error(ctx);
        fprintf(stderr,
                "[xi_cgen] ERROR: missing receiver map-shape plan for %s "
                "(shape=%u key_access=%u)\n",
                site ? site : "Map/Set", plan->receiver_shape_id, plan->access_id);
        return NULL;
    }
    return shape;
}

static bool cg_key_access_plan_is_dense_enum_index(XiCgenCtx *ctx, const XaotKeyAccessPlan *plan,
                                                   const char *site) {
    if (!plan || plan->action != XAOT_KEY_ACCESS_DIRECT_DENSE_INDEX)
        return false;
    const XaotMapShapePlan *shape = cg_key_access_receiver_shape_plan(ctx, plan, site);
    return shape && shape->action == XAOT_MAP_SHAPE_DENSE_ENUM_TABLE;
}

static bool cg_key_access_plan_action_has_backend(XiCgenCtx *ctx, const XaotKeyAccessPlan *plan,
                                                  const XiValue *v, const char *site) {
    if (!plan)
        return true;
    switch ((XaotKeyAccessAction) plan->action) {
        case XAOT_KEY_ACCESS_PREHASHED_LOOKUP:
        case XAOT_KEY_ACCESS_BOOL_DIRECT_LOOKUP:
        case XAOT_KEY_ACCESS_SPECIALIZED_HASH_LOOKUP:
        case XAOT_KEY_ACCESS_GENERIC_HASH_LOOKUP:
        case XAOT_KEY_ACCESS_INLINE_SMALL_SCAN:
        case XAOT_KEY_ACCESS_DIRECT_DENSE_INDEX:
            return true;
        case XAOT_KEY_ACCESS_REJECT:
        default:
            cg_ctx_set_error(ctx);
            fprintf(stderr,
                    "[xi_cgen] ERROR: key-access plan action %u for %s Xi value v%u "
                    "(key_access=%u) has no CGen backend emitter yet\n",
                    (unsigned) plan->action, site ? site : "Map/Set", v ? v->id : 0,
                    v ? v->xg_key_access_id : 0);
            return false;
    }
}

static bool cg_key_access_plan_action_allows_hash_helper(XiCgenCtx *ctx,
                                                         const XaotKeyAccessPlan *plan,
                                                         const XiValue *v, const char *site) {
    if (!cg_key_access_plan_action_has_backend(ctx, plan, v, site))
        return false;
    if (!plan)
        return true;
    return plan->action == XAOT_KEY_ACCESS_PREHASHED_LOOKUP ||
           plan->action == XAOT_KEY_ACCESS_BOOL_DIRECT_LOOKUP ||
           plan->action == XAOT_KEY_ACCESS_SPECIALIZED_HASH_LOOKUP;
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
