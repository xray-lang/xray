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
    const char *elem_name;
    const char *ctype;
    XrRep rep;
} CgArrayElemInfo;

static const XrType *cg_array_elem_type_from_type(const XrType *type) {
    if (!type)
        return NULL;
    if (type->kind == XR_KIND_ARRAY)
        return type->container.element_type;
    if (type->kind == XR_KIND_FIXED_ARRAY)
        return type->fixed_array.element_type;
    return NULL;
}

static bool cg_array_elem_info_from_type(const XrType *type, CgArrayElemInfo *out) {
    const XrType *elem = cg_array_elem_type_from_type(type);
    if (!elem || elem->is_nullable || !out)
        return false;

    memset(out, 0, sizeof(*out));
    if (elem->native_width != 0) {
        const char *elem_name = xaot_elem_name_for_native_type(elem->native_width);
        const char *ctype = xaot_c_type_for_native_type(elem->native_width);
        if (elem_name && ctype) {
            *out = (CgArrayElemInfo) {elem_name, ctype,
                                      xaot_storage_rep_for_native_type(elem->native_width)};
            return true;
        }
    }

    if (elem->kind == XR_KIND_INT) {
        *out = (CgArrayElemInfo) {"XR_ELEM_I64", "int64_t", XR_REP_I64};
        return true;
    }
    if (elem->kind == XR_KIND_FLOAT) {
        *out = (CgArrayElemInfo) {"XR_ELEM_F64", "double", XR_REP_F64};
        return true;
    }
    if (elem->kind == XR_KIND_BOOL) {
        *out = (CgArrayElemInfo) {"XR_ELEM_BOOL", "uint8_t", XR_REP_I64};
        return true;
    }
    return false;
}

typedef enum CgArrayStorageUse {
    CG_ARRAY_STORAGE_MUTABLE,
    CG_ARRAY_STORAGE_READ
} CgArrayStorageUse;

static bool cg_array_value_storage_info_depth(XiCgenCtx *ctx, const XiFunc *f,
                                              const XiValue *array_value, CgArrayElemInfo *out,
                                              CgArrayStorageUse use, uint8_t depth) {
    const XiValue *v = cg_unwrap_identity_value(array_value);
    if (!v || depth > 8 || !cg_array_elem_info_from_type(v->type, out))
        return false;
    if (v->op == XI_ARRAY_NEW)
        return true;
    if (cg_class_native_receiver_ref_field(ctx, f, v, XR_NATIVE_ARRAY_REF, NULL, NULL))
        return true;
    if (v->op == XI_CALL_BUILTIN) {
        const char *name = (const char *) v->aux;
        if (name &&
            (strcmp(name, "array_new") == 0 || strcmp(name, "Bytes") == 0 ||
             strcmp(name, "array_with_capacity") == 0 || strcmp(name, "array_filled_new") == 0))
            return true;
        if (name && (strcmp(name, "array_reserve") == 0 || strcmp(name, "array_resize") == 0) &&
            v->nargs >= 1)
            return cg_array_value_storage_info_depth(ctx, f, v->args[0], out, use, depth + 1);
        if (use == CG_ARRAY_STORAGE_READ && name && strcmp(name, "slice") == 0 && v->nargs >= 1)
            return cg_array_value_storage_info_depth(ctx, f, v->args[0], out, CG_ARRAY_STORAGE_READ,
                                                     depth + 1);
    }
    if (v->op == XI_CALL_METHOD && v->nargs >= 1) {
        const char *method = (const char *) v->aux;
        if (use == CG_ARRAY_STORAGE_READ && method && strcmp(method, "slice") == 0)
            return cg_array_value_storage_info_depth(ctx, f, v->args[0], out, CG_ARRAY_STORAGE_READ,
                                                     depth + 1);
        if (method && strcmp(method, "filter") == 0)
            return cg_array_value_storage_info_depth(ctx, f, v->args[0], out, CG_ARRAY_STORAGE_READ,
                                                     depth + 1);
        if (method && strcmp(method, "map") == 0)
            return cg_array_elem_info_from_type(v->type, out);
    }
    if (v->op == XI_PHI) {
        bool has_base = false;
        if (v->nargs == 0)
            return false;
        for (uint16_t i = 0; i < v->nargs; i++) {
            CgArrayElemInfo arg_info;
            const XiValue *arg = cg_unwrap_identity_value(v->args[i]);
            if (arg == v)
                continue;
            if (!cg_array_value_storage_info_depth(ctx, f, arg, &arg_info, use, depth + 1))
                return false;
            if (strcmp(arg_info.elem_name, out->elem_name) != 0)
                return false;
            has_base = true;
        }
        return has_base;
    }
    return false;
}

static bool cg_array_value_storage_info(XiCgenCtx *ctx, const XiFunc *f, const XiValue *array_value,
                                        CgArrayElemInfo *out, CgArrayStorageUse use) {
    return cg_array_value_storage_info_depth(ctx, f, array_value, out, use, 0);
}

static bool cg_array_index_get_reads_f32_storage(const XiValue *v) {
    CgArrayElemInfo info;
    return v && v->op == XI_INDEX_GET && v->nargs >= 1 &&
           cg_array_value_storage_info(NULL, NULL, v->args[0], &info, CG_ARRAY_STORAGE_READ) &&
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
        if (name && (strcmp(name, "array_new") == 0 || strcmp(name, "Bytes") == 0))
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

static bool cg_array_value_precedes_in_block(const XiValue *before, const XiValue *after) {
    if (!before || !after || before->block != after->block)
        return false;
    for (uint32_t i = 0; i < after->block->nvalues; i++) {
        const XiValue *cur = after->block->values[i];
        if (cur == before)
            return true;
        if (cur == after)
            return false;
    }
    return false;
}

static bool cg_array_native_receiver_array_store_info(XiCgenCtx *ctx, const XiFunc *f,
                                                      const XiValue *v, uint16_t expected_idx,
                                                      CgClassNativeFunc *out_info,
                                                      uint16_t *out_idx) {
    CgClassNativeFunc info = cg_class_native_func(ctx, f);
    if (out_info)
        memset(out_info, 0, sizeof(*out_info));
    if (out_idx)
        *out_idx = 0;
    if (!info.layout || !v || v->op != XI_STORE_FIELD || v->nargs < 2 ||
        !cg_class_native_receiver_value(ctx, f, v->args[0]))
        return false;
    int idx = cg_class_native_field_index(info.layout, (const char *) v->aux);
    if (idx < 0 || (expected_idx != UINT16_MAX && (uint16_t) idx != expected_idx))
        return false;
    const XrStructFieldLayout *field = cg_struct_field(info.layout, idx);
    if (!field || field->native_type != XR_NATIVE_ARRAY_REF)
        return false;
    if (out_info)
        *out_info = info;
    if (out_idx)
        *out_idx = (uint16_t) idx;
    return true;
}

static bool cg_array_native_receiver_array_store(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v,
                                                 uint16_t expected_idx) {
    return cg_array_native_receiver_array_store_info(ctx, f, v, expected_idx, NULL, NULL);
}

static const XiValue *cg_array_class_field_fresh_store_origin(XiCgenCtx *ctx, const XiFunc *f,
                                                              const XiValue *field_value,
                                                              const XiValue *site) {
    CgClassNativeFunc info;
    uint16_t field_idx = 0;
    const XiValue *v = cg_unwrap_identity_value(field_value);
    if (!ctx || !f || !v || !site ||
        !cg_class_native_receiver_ref_field(ctx, f, v, XR_NATIVE_ARRAY_REF, &info, &field_idx))
        return NULL;

    xi_ensure_dominators((XiFunc *) f);

    const XiValue *store = NULL;
    const XiValue *origin = NULL;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *cur = blk->values[vi];
            if (!cg_array_native_receiver_array_store(ctx, f, cur, field_idx))
                continue;
            const XiValue *cur_origin = cg_array_single_origin(cur->args[1], 0);
            if (!cur_origin || store)
                return NULL;
            store = cur;
            origin = cur_origin;
        }
    }
    if (!store || !origin || !xi_dominates(store->block, site->block))
        return NULL;
    if (store->block == site->block && !cg_array_value_precedes_in_block(store, site))
        return NULL;
    return origin;
}

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

    origin = cg_array_class_field_fresh_store_origin(ctx, f, push->args[0], push);
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

static bool emit_bool_accumulate_diamond_stmt(FILE *out, const XiBlock *blk) {
    if (!out || !blk || blk->kind != XI_BLOCK_IF || !blk->control ||
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
        then_idx >= phi->value.nargs || cg_rep(&phi->value) != XR_REP_I64)
        return false;
    const XiValue *base = phi->value.args[pred_idx];
    const XiValue *add = phi->value.args[then_idx];
    const XiValue *step = NULL;
    if (!base || !add ||
        cg_unwrap_identity_value(then_blk->values[0]) != cg_unwrap_identity_value(add) ||
        cg_rep(base) != XR_REP_I64 || !cg_array_add_base_const_step(add, base, &step))
        return false;

    fprintf(out, "    phi%u = ", phi->value.id);
    emit_value_as_rep(out, base, XR_REP_I64);
    fprintf(out, " + ((");
    emit_vref(out, blk->control);
    fprintf(out, " != 0) ? ");
    emit_value_as_rep(out, step, XR_REP_I64);
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
    if (!origin || !cg_array_value_available_at(cap_value, origin))
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
        if (!origin || !cg_array_value_available_at(cap_value, origin))
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
            if (!origin || !cg_array_value_available_at(cap_value, origin))
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
    const char *method = (const char *) push->aux;
    if (!method || strcmp(method, "push") != 0)
        return false;
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
    if (!origin || !cg_array_value_available_at(cap_value, origin))
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
                       strcmp(method, "reduce") == 0 || strcmp(method, "slice") == 0))
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
    CgArrayFillLoop fill;
} CgArrayClassFieldAlloc;

static bool cg_array_block_has_no_side_effect_between(const XiValue *start, const XiValue *end) {
    if (!start || !end || start->block != end->block)
        return false;
    bool after_start = false;
    for (uint32_t i = 0; i < start->block->nvalues; i++) {
        const XiValue *cur = start->block->values[i];
        if (!cur)
            continue;
        if (cur == start) {
            after_start = true;
            continue;
        }
        if (cur == end)
            return after_start;
        if (after_start && (cur->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM)))
            return false;
    }
    return false;
}

static bool cg_array_origin_is_directly_used_only_by_store(const XiFunc *f, const XiValue *origin,
                                                           const XiValue *store) {
    if (!f || !origin || !store)
        return false;
    bool saw_store_use = false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == origin)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t ai = 0; ai < phi->value.nargs; ai++) {
                if (phi->value.args[ai] == origin)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v || v == origin)
                continue;
            for (uint16_t ai = 0; ai < v->nargs; ai++) {
                if (v->args[ai] != origin)
                    continue;
                if (v == store && ai == 1) {
                    saw_store_use = true;
                    continue;
                }
                return false;
            }
        }
    }
    return saw_store_use;
}

static bool cg_array_class_field_alloc_info(XiCgenCtx *ctx, const XiFunc *f, const XiValue *origin,
                                            CgArrayClassFieldAlloc *out) {
    CgArrayElemInfo elem;
    if (!ctx || ctx->pre_decl_all || !f || !origin ||
        !cg_array_elem_info_from_type(origin->type, &elem))
        return false;
    CgArrayFillLoop fill;
    if (!cg_array_unique_fill_loop_for_origin(ctx, f, origin, &fill) || !fill.storage_value)
        return false;

    const XiValue *store = NULL;
    CgClassNativeFunc class_info;
    uint16_t field_idx = 0;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *cur = blk->values[vi];
            if (!cur || cur->op != XI_STORE_FIELD || cur->nargs < 2)
                continue;
            const XiValue *cur_origin = cg_array_single_origin(cur->args[1], 0);
            if (cur_origin != origin)
                continue;
            uint16_t cur_idx = 0;
            CgClassNativeFunc cur_info;
            if (!cg_array_native_receiver_array_store_info(ctx, f, cur, UINT16_MAX, &cur_info,
                                                           &cur_idx))
                return false;
            if (store)
                return false;
            store = cur;
            class_info = cur_info;
            field_idx = cur_idx;
        }
    }
    if (!store || store->block != origin->block ||
        !cg_array_block_has_no_side_effect_between(origin, store) ||
        !cg_array_origin_is_directly_used_only_by_store(f, origin, store))
        return false;

    if (out) {
        out->origin = origin;
        out->store = store;
        out->class_info = class_info;
        out->field_idx = field_idx;
        out->elem = elem;
        out->fill = fill;
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
    if (!cg_array_class_field_alloc_info(ctx, f, origin, &info) || info.store != store)
        return false;

    fprintf(out, "    ");
    emit_class_native_field_ref(ctx, out, info.class_info.class_data, "p0", info.field_idx);
    fprintf(out, " = (xrt_array_t*)");
    if (!emit_typed_array_new_expr(ctx, out, f, origin, 4))
        return false;
    fprintf(out, ".ptr;\n");

    fprintf(out, "    %s *", info.elem.ctype);
    emit_typed_array_data_cache_ref(out, origin);
    fprintf(out, " = (%s*)", info.elem.ctype);
    emit_class_native_field_ref(ctx, out, info.class_info.class_data, "p0", info.field_idx);
    fprintf(out, "->data;\n");
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

static bool cg_array_can_cache_data_for_origin(XiCgenCtx *ctx, const XiValue *origin,
                                               CgArrayElemInfo *info_out, CgArrayFillLoop *out) {
    if (!ctx || ctx->pre_decl_all || !origin)
        return false;
    if (info_out && !cg_array_elem_info_from_type(origin->type, info_out))
        return false;
    if (!info_out) {
        CgArrayElemInfo info;
        if (!cg_array_elem_info_from_type(origin->type, &info))
            return false;
    }
    const XiFunc *f = origin && origin->block ? origin->block->func : NULL;
    return cg_array_unique_fill_loop_for_origin(ctx, f, origin, out);
}

static bool cg_array_is_slice_result(const XiValue *value) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v)
        return false;
    if (v->op == XI_CALL_BUILTIN) {
        const char *name = (const char *) v->aux;
        return name && strcmp(name, "slice") == 0 && v->nargs >= 1;
    }
    if (v->op == XI_CALL_METHOD) {
        const char *method = (const char *) v->aux;
        return method && strcmp(method, "slice") == 0 && v->nargs >= 1;
    }
    return false;
}

static bool cg_array_is_hof_result(const XiValue *value) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || v->op != XI_CALL_METHOD)
        return false;
    const char *method = (const char *) v->aux;
    return method && (strcmp(method, "map") == 0 || strcmp(method, "filter") == 0) && v->nargs >= 2;
}

static bool cg_array_value_arg_matches(const XiValue *v, const XiValue *target,
                                       uint16_t first_arg) {
    if (!v || !target)
        return false;
    for (uint16_t i = first_arg; i < v->nargs; i++) {
        if (cg_array_same_value(v->args[i], target))
            return true;
    }
    return false;
}

static bool cg_array_value_has_uncacheable_use(const XiValue *value) {
    const XiValue *target = cg_unwrap_identity_value(value);
    const XiFunc *f = (target && target->block) ? target->block->func : NULL;
    if (!f)
        return true;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *cur = blk->values[vi];
            if (!cur || cur == target)
                continue;
            switch ((XiOp) cur->op) {
                case XI_INDEX_GET:
                case XI_LOAD_FIELD:
                    break;
                case XI_INDEX_SET:
                case XI_STORE_FIELD:
                case XI_CALL:
                case XI_CALL_BUILTIN:
                case XI_CALL_METHOD:
                case XI_CALL_METHOD_DIRECT:
                case XI_GO:
                case XI_SET_SHARED:
                    if (cg_array_value_arg_matches(cur, target, 0))
                        return true;
                    break;
                default:
                    if ((cur->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM)) &&
                        cg_array_value_arg_matches(cur, target, 0))
                        return true;
                    break;
            }
        }
    }
    return false;
}

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
    if (strcmp(name, "Bytes") != 0)
        return false;
    if (v->nargs == 0)
        return true;
    return v->nargs == 1 && v->args[0] && v->args[0]->type && v->args[0]->type->kind == XR_KIND_INT;
}

static bool cg_array_native_local_arg_use_is_safe(const XiValue *user, uint16_t arg_index) {
    if (!user)
        return false;
    switch ((XiOp) user->op) {
        case XI_INDEX_GET:
            return arg_index == 0;
        case XI_INDEX_SET:
            return arg_index == 0;
        case XI_LOAD_FIELD: {
            const char *field = (const char *) user->aux;
            return arg_index == 0 && field &&
                   (strcmp(field, "length") == 0 || strcmp(field, "size") == 0);
        }
        case XI_CALL_METHOD: {
            const char *method = (const char *) user->aux;
            return arg_index == 0 && method && strcmp(method, "push") == 0;
        }
        case XI_RETAIN:
        case XI_RELEASE:
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

static bool cg_array_value_has_index_get_use(const XiFunc *f, const XiValue *target) {
    target = cg_unwrap_identity_value(target);
    if (!f || !target)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *cur = blk->values[vi];
            if (!cur || cur->op != XI_INDEX_GET || cur->nargs < 1)
                continue;
            if (cg_unwrap_identity_value(cur->args[0]) == target)
                return true;
        }
    }
    return false;
}

static bool cg_array_class_field_read_cacheable(XiCgenCtx *ctx, const XiFunc *f,
                                                const XiValue *value, CgArrayElemInfo *info_out) {
    CgArrayElemInfo info;
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!ctx || ctx->pre_decl_all || !f || !v || !cg_array_elem_info_from_type(v->type, &info) ||
        !cg_class_native_receiver_ref_field(ctx, f, v, XR_NATIVE_ARRAY_REF, NULL, NULL) ||
        !cg_array_value_has_index_get_use(f, v))
        return false;

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *cur = blk->values[vi];
            if (!cur || cur == v)
                continue;
            if (cur->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM))
                return false;
        }
    }

    if (info_out)
        *info_out = info;
    return true;
}

static bool cg_array_native_local_data_cacheable(XiCgenCtx *ctx, const XiValue *value,
                                                 CgArrayElemInfo *info_out) {
    const XiValue *target = cg_unwrap_identity_value(value);
    const XiFunc *f = target && target->block ? target->block->func : NULL;
    CgArrayElemInfo info;
    if (!target || !f || !cg_array_value_uses_native_local(ctx, f, target) ||
        !cg_array_elem_info_from_type(target->type, &info))
        return false;

    bool has_index_use = false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *cur = blk->values[vi];
            if (!cur || cur == target)
                continue;
            for (uint16_t a = 0; a < cur->nargs; a++) {
                if (!cg_array_same_value(cur->args[a], target))
                    continue;
                switch ((XiOp) cur->op) {
                    case XI_INDEX_GET:
                        if (a != 0)
                            return false;
                        has_index_use = true;
                        break;
                    case XI_INDEX_SET:
                        if (a != 0 || !(cg_array_index_access_bounds_proven(ctx, f, cur) ||
                                        cg_array_index_set_counted_loop_bounds_proven(ctx, f, cur)))
                            return false;
                        has_index_use = true;
                        break;
                    case XI_LOAD_FIELD: {
                        const char *field = (const char *) cur->aux;
                        if (a != 0 || !field ||
                            (strcmp(field, "length") != 0 && strcmp(field, "size") != 0))
                            return false;
                        break;
                    }
                    case XI_RETAIN:
                    case XI_RELEASE:
                        if (a != 0)
                            return false;
                        break;
                    default:
                        return false;
                }
            }
        }
    }

    if (!has_index_use)
        return false;
    if (info_out)
        *info_out = info;
    return true;
}

static bool cg_array_can_cache_data_for_value(XiCgenCtx *ctx, const XiValue *value,
                                              CgArrayElemInfo *info_out) {
    CgArrayElemInfo info;
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!ctx || ctx->pre_decl_all || !v)
        return false;
    if (cg_array_native_local_data_cacheable(ctx, v, info_out))
        return true;
    if (cg_array_can_cache_data_for_origin(ctx, v, info_out, NULL))
        return true;
    const XiFunc *f = v && v->block ? v->block->func : NULL;
    if (cg_array_class_field_read_cacheable(ctx, f, v, info_out))
        return true;
    bool cacheable_producer = cg_array_is_slice_result(v);
    if (!cacheable_producer && cg_array_is_hof_result(v))
        cacheable_producer = !cg_array_value_has_uncacheable_use(v);
    if (!cacheable_producer)
        return false;
    if (!cg_array_value_storage_info(ctx, f, v, &info, CG_ARRAY_STORAGE_READ))
        return false;
    if (info_out)
        *info_out = info;
    return true;
}

static const XiValue *cg_array_class_field_cached_origin(XiCgenCtx *ctx, const XiFunc *f,
                                                         const XiValue *value) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!ctx || !f || !v ||
        !cg_class_native_receiver_ref_field(ctx, f, v, XR_NATIVE_ARRAY_REF, NULL, NULL))
        return NULL;
    const XiValue *origin = cg_array_class_field_fresh_store_origin(ctx, f, v, v);
    CgArrayClassFieldAlloc info;
    return cg_array_class_field_alloc_info(ctx, f, origin, &info) ? origin : NULL;
}

static bool emit_typed_array_data_cache_decl(XiCgenCtx *ctx, FILE *out, const XiValue *value) {
    CgArrayElemInfo info;
    if (!cg_array_can_cache_data_for_value(ctx, value, &info))
        return false;
    const XiFunc *f = value && value->block ? value->block->func : NULL;
    fprintf(out, "    %s *", info.ctype);
    emit_typed_array_data_cache_ref(out, value);
    fprintf(out, " = (%s*)", info.ctype);
    emit_typed_array_ptr_expr(ctx, out, f, value, NULL);
    fprintf(out, "->data;\n");
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
    const XiValue *origin = cg_array_single_origin(array_value, 0);
    if (!cg_array_can_cache_data_for_origin(ctx, origin, NULL, NULL))
        return false;
    if (out_origin)
        *out_origin = origin;
    return true;
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
            fprintf(out, "->len = ");
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

static bool cg_array_values_share_storage(XiCgenCtx *ctx, const XiFunc *f, const XiValue *lhs,
                                          const XiValue *rhs) {
    if (cg_array_same_value(lhs, rhs))
        return true;

    uint16_t lhs_idx = 0;
    uint16_t rhs_idx = 0;
    return cg_class_native_receiver_ref_field(ctx, f, cg_unwrap_identity_value(lhs),
                                              XR_NATIVE_ARRAY_REF, NULL, &lhs_idx) &&
           cg_class_native_receiver_ref_field(ctx, f, cg_unwrap_identity_value(rhs),
                                              XR_NATIVE_ARRAY_REF, NULL, &rhs_idx) &&
           lhs_idx == rhs_idx;
}

static bool cg_array_length_value_matches(XiCgenCtx *ctx, const XiFunc *f,
                                          const XiValue *length_value, const XiValue *array_value) {
    const XiValue *v = cg_unwrap_identity_value(length_value);
    if (!v || v->op != XI_LOAD_FIELD || v->nargs < 1)
        return false;
    const char *field = (const char *) v->aux;
    if (!field || (strcmp(field, "length") != 0 && strcmp(field, "size") != 0))
        return false;
    return cg_array_values_share_storage(ctx, f, v->args[0], array_value);
}

static bool cg_array_control_proves_index_lt_len(XiCgenCtx *ctx, const XiFunc *f,
                                                 const XiValue *control, const XiValue *array_value,
                                                 const XiValue *index_value,
                                                 const XiValue **out_len) {
    const XiValue *v = cg_unwrap_identity_value(control);
    if (!v || v->op != XI_LT || v->nargs < 2)
        return false;
    if (!cg_array_same_value(v->args[0], index_value))
        return false;
    if (!cg_array_length_value_matches(ctx, f, v->args[1], array_value))
        return false;
    if (out_len)
        *out_len = cg_unwrap_identity_value(v->args[1]);
    return true;
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

static bool cg_array_index_access_bounds_proven(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    if (!v || (v->op != XI_INDEX_GET && v->op != XI_INDEX_SET) || v->nargs < 2 || !v->block)
        return false;
    if (!f)
        f = v->block->func;
    const XiValue *array_value = v->args[0];
    const XiValue *index_value = v->args[1];
    if (!cg_array_value_known_nonnegative(index_value, NULL, 0))
        return false;
    if (!cg_array_block_has_no_side_effect_before(v->block, v))
        return false;
    if (v->block->npreds == 0)
        return false;
    for (uint16_t i = 0; i < v->block->npreds; i++) {
        const XiBlock *pred = v->block->preds[i];
        const XiValue *len_value = NULL;
        if (!pred || pred->kind != XI_BLOCK_IF || pred->succs[0] != v->block)
            return false;
        if (!cg_array_control_proves_index_lt_len(ctx, f, pred->control, array_value, index_value,
                                                  &len_value))
            return false;
        if (!cg_array_block_has_no_side_effect_after(pred, len_value))
            return false;
    }
    return true;
}

static bool cg_array_index_get_bounds_proven(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    return cg_array_index_access_bounds_proven(ctx, f, v);
}

static bool cg_array_index_set_counted_loop_bounds_proven(XiCgenCtx *ctx, const XiFunc *f,
                                                          const XiValue *v) {
    if (!v || v->op != XI_INDEX_SET || v->nargs < 2 || !v->block)
        return false;
    const XiBlock *loop = v->block;
    if (loop->kind != XI_BLOCK_IF || loop->succs[0] != loop)
        return false;
    if (!cg_array_block_has_no_side_effect_before(loop, v))
        return false;
    const XiValue *cond = cg_unwrap_identity_value(loop->control);
    if (!cond || cond->op != XI_LT || cond->nargs < 2)
        return false;
    const XiValue *index = cg_array_phi_from_add_one(cond->args[0]);
    if (!index || !cg_array_same_value(index, v->args[1]))
        return false;
    const XiValue *bound = cg_array_loop_bound_base(cond->args[1], loop, loop);
    if (!bound || !cg_array_length_value_matches(ctx, f, bound, v->args[0]))
        return false;
    if (!cg_array_loop_index_is_counted(index, loop, loop))
        return false;
    return cg_array_single_block_entry_checked(loop, bound);
}

static void emit_typed_array_store_value(FILE *out, const CgArrayElemInfo *info,
                                         const XiValue *value) {
    fprintf(out, "(%s)", info->ctype);
    emit_value_as_rep(out, value, info->rep);
}

static bool emit_typed_array_new_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                      int64_t cap) {
    CgArrayElemInfo info;
    if (!cg_array_elem_info_from_type(v->type, &info))
        return false;
    CgArrayFillLoop fill;
    if (cg_array_unique_fill_loop_for_origin(ctx, f, v, &fill)) {
        fprintf(out, "xrt_array_new_typed_uninit(");
        emit_value_as_rep(out, fill.cap_value, XR_REP_I64);
        fprintf(out, ", %s)", info.elem_name);
    } else {
        fprintf(out, "xrt_array_new_typed(%" PRId64 ", %s)", cap, info.elem_name);
    }
    return true;
}

static bool emit_typed_array_new_ptr_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                          const XiValue *v, int64_t cap) {
    CgArrayElemInfo info;
    if (!cg_array_elem_info_from_type(v->type, &info))
        return false;
    CgArrayFillLoop fill;
    if (cg_array_unique_fill_loop_for_origin(ctx, f, v, &fill)) {
        fprintf(out, "xrt_array_new_typed_uninit_ptr(");
        emit_value_as_rep(out, fill.cap_value, XR_REP_I64);
        fprintf(out, ", %s)", info.elem_name);
    } else {
        fprintf(out, "xrt_array_new_typed_ptr(%" PRId64 ", %s)", cap, info.elem_name);
    }
    return true;
}

static bool emit_bytes_new_native_local_expr(FILE *out, const XiValue *v) {
    if (!out || !v || v->op != XI_CALL_BUILTIN || !v->aux ||
        strcmp((const char *) v->aux, "Bytes") != 0)
        return false;
    if (v->nargs == 0) {
        fprintf(out, "xrt_array_new_typed_ptr(0, XR_ELEM_U8)");
        return true;
    }
    if (v->nargs == 1 && v->args[0] && v->args[0]->type && v->args[0]->type->kind == XR_KIND_INT) {
        fprintf(out, "({ int64_t _n = ");
        emit_value_as_rep(out, v->args[0], XR_REP_I64);
        fprintf(out, "; if (_n < 0) _n = 0; ");
        fprintf(out, "xrt_array_t *_b = xrt_array_new_typed_ptr(_n, XR_ELEM_U8); ");
        fprintf(out, "_b->len = _n; _b; })");
        return true;
    }
    return false;
}

static void emit_typed_array_ptr_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                      const XiValue *value, const char *prefix) {
    (void) prefix;
    if (emit_class_native_receiver_ref_field_ptr_expr(ctx, out, f, value, XR_NATIVE_ARRAY_REF))
        return;
    if (cg_array_value_uses_native_local(ctx, f, value)) {
        emit_vref(out, value);
        return;
    }
    fprintf(out, "(");
    fprintf(out, "(xrt_array_t*)");
    emit_vref(out, value);
    fprintf(out, ".ptr)");
}

static bool emit_typed_array_index_get_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                            const XiValue *v, const char *prefix) {
    CgArrayElemInfo info;
    if (!v || v->nargs < 2 ||
        !cg_array_value_storage_info(ctx, f, v->args[0], &info, CG_ARRAY_STORAGE_READ))
        return false;

    bool unchecked = cg_array_index_get_bounds_proven(ctx, f, v);
    const XiValue *cached_origin = NULL;
    bool use_cache = cg_array_data_cache_for_value(ctx, v->args[0], &cached_origin);
    bool wrapped = emit_conversion_prefix(out, v->type, info.rep, cg_rep(v));
    if (unchecked) {
        if (use_cache)
            emit_aot_hot_region_begin(out, "typed_array_raw_access");
        if (info.rep == XR_REP_F64) {
            fprintf(out, "(double)");
        } else {
            fprintf(out, "(int64_t)");
        }
        if (use_cache) {
            emit_typed_array_data_cache_ref(out, cached_origin);
            fprintf(out, "[");
        } else {
            fprintf(out, "((%s*)", info.ctype);
            emit_typed_array_ptr_expr(ctx, out, f, v->args[0], prefix);
            fprintf(out, "->data)[");
        }
        emit_value_as_rep(out, v->args[1], XR_REP_I64);
        fprintf(out, "]");
        if (use_cache)
            emit_aot_hot_region_end(out, "typed_array_raw_access");
    } else if (info.rep == XR_REP_F64) {
        fprintf(out, "({ xrt_array_t *_a = ");
        emit_typed_array_ptr_expr(ctx, out, f, v->args[0], prefix);
        fprintf(out, "; int64_t _idx = ");
        emit_value_as_rep(out, v->args[1], XR_REP_I64);
        fprintf(out, "; if (_idx < 0) _idx += _a->len; ");
        fprintf(out, "(_idx >= 0 && _idx < _a->len) ? (double)");
        if (use_cache) {
            emit_typed_array_data_cache_ref(out, cached_origin);
            fprintf(out, "[_idx]");
        } else {
            fprintf(out, "((%s*)_a->data)[_idx]", info.ctype);
        }
        fprintf(out, " : 0.0; })");
    } else {
        fprintf(out, "({ xrt_array_t *_a = ");
        emit_typed_array_ptr_expr(ctx, out, f, v->args[0], prefix);
        fprintf(out, "; int64_t _idx = ");
        emit_value_as_rep(out, v->args[1], XR_REP_I64);
        fprintf(out, "; if (_idx < 0) _idx += _a->len; ");
        fprintf(out, "(_idx >= 0 && _idx < _a->len) ? (int64_t)");
        if (use_cache) {
            emit_typed_array_data_cache_ref(out, cached_origin);
            fprintf(out, "[_idx]");
        } else {
            fprintf(out, "((%s*)_a->data)[_idx]", info.ctype);
        }
        fprintf(out, " : 0; })");
    }
    emit_conversion_suffix(out, wrapped);
    return true;
}

static bool emit_typed_array_index_set_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                            const XiValue *v, const char *prefix) {
    CgArrayElemInfo info;
    if (!v || v->nargs < 3 ||
        !cg_array_value_storage_info(ctx, f, v->args[0], &info, CG_ARRAY_STORAGE_MUTABLE))
        return false;

    if (cg_array_index_access_bounds_proven(ctx, f, v) ||
        cg_array_index_set_counted_loop_bounds_proven(ctx, f, v)) {
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
        emit_value_as_rep(out, v->args[1], XR_REP_I64);
        fprintf(out, "] = ");
        emit_typed_array_store_value(out, &info, v->args[2]);
        if (use_cache)
            emit_aot_hot_region_end(out, "typed_array_raw_access");
        fprintf(out, "; XR_NULL_VAL; })");
        return true;
    }

    fprintf(out, "({ xrt_array_t *_a = ");
    emit_typed_array_ptr_expr(ctx, out, f, v->args[0], prefix);
    fprintf(out, "; int64_t _idx = ");
    emit_value_as_rep(out, v->args[1], XR_REP_I64);
    fprintf(out, "; if (_idx < 0) _idx += _a->len; ");
    fprintf(out,
            "if (_idx >= 0) { if (_idx >= _a->cap) { if (_a->cap == 0) { "
            "fprintf(stderr, \"xrt_array_set: cannot grow array slice\\n\"); abort(); } "
            "while (_idx >= _a->cap) _a->cap *= 2; void *_tmp = XRT_REALLOC(_a->data, "
            "(size_t)_a->cap * sizeof(%s)); if (!_tmp) { fprintf(stderr, "
            "\"xrt_array_set: out of memory\\n\"); abort(); } _a->data = _tmp; } "
            "if (_idx > _a->len) { memset((uint8_t*)_a->data + (size_t)_a->len * "
            "sizeof(%s), 0, (size_t)(_idx - _a->len) * sizeof(%s)); "
            "_a->len = _idx; } ((%s*)_a->data)[_idx] = ",
            info.ctype, info.ctype, info.ctype, info.ctype);
    emit_typed_array_store_value(out, &info, v->args[2]);
    fprintf(out, "; if (_idx == _a->len) _a->len++; } XR_NULL_VAL; })");
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
        if (use_final_len) {
            fprintf(out, "({ ");
        } else {
            fprintf(out, "({ xrt_array_t *_a = ");
            emit_typed_array_ptr_expr(ctx, out, f, recv, prefix);
            fprintf(out, "; ");
        }
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
        emit_typed_array_store_value(out, &info, arg);
        if (use_cache && use_final_len)
            emit_aot_hot_region_end(out, "typed_array_raw_access");
        if (!use_final_len) {
            fprintf(out, "; _a->len = ");
            if (fill.next_index_value && cg_array_value_available_at(fill.next_index_value, call)) {
                emit_value_as_rep(out, fill.next_index_value, XR_REP_I64);
            } else {
                fprintf(out, "(");
                emit_value_as_rep(out, fill.index_value, XR_REP_I64);
                fprintf(out, " + 1)");
            }
        }
        fprintf(out, "; XR_NULL_VAL; })");
        return true;
    }

    fprintf(out, "({ xrt_array_t *_a = ");
    emit_typed_array_ptr_expr(ctx, out, f, recv, prefix);
    {
        fprintf(out,
                "; if (_a->len >= _a->cap) { _a->cap *= 2; "
                "void *_tmp = XRT_REALLOC(_a->data, (size_t)_a->cap * sizeof(%s)); "
                "if (!_tmp) { fprintf(stderr, \"xrt_array_push: out of memory\\n\"); abort(); } "
                "_a->data = _tmp; } ((%s*)_a->data)[_a->len++] = ",
                info.ctype, info.ctype);
    }
    emit_typed_array_store_value(out, &info, arg);
    fprintf(out, "; XR_NULL_VAL; })");
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
    const char *method = (const char *) call->aux;
    return method && strcmp(method, "push") == 0 &&
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
                    case XI_LOAD_FIELD: {
                        const char *field = (const char *) v->aux;
                        if (ai == 0 && field &&
                            (strcmp(field, "length") == 0 || strcmp(field, "size") == 0))
                            continue;
                        return false;
                    }
                    case XI_CALL_METHOD: {
                        const char *method = (const char *) v->aux;
                        if (ai == 0 && method &&
                            (strcmp(method, "length") == 0 || strcmp(method, "push") == 0 ||
                             strcmp(method, "slice") == 0 || strcmp(method, "map") == 0 ||
                             strcmp(method, "filter") == 0 || strcmp(method, "reduce") == 0))
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

    bool wrapped = emit_conversion_prefix(out, v->type, XR_REP_I64, cg_rep(v));
    emit_typed_array_ptr_expr(ctx, out, f, v->args[0], prefix);
    fprintf(out, "->len");
    emit_conversion_suffix(out, wrapped);
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
        !cg_array_elem_info_from_type(v->type, &info.dst_info) ||
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
    fprintf(out, ".ptr; int64_t _n = _src->len; XrValue _outv = ");
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
    fprintf(out, "(NULL, (%s)_srcd[_i]); } _out->len = _n; _outv; })", ctype_str(map.param_rep));
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
        !cg_array_elem_info_from_type(v->type, &info.dst_info) ||
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
    fprintf(out, ".ptr; int64_t _n = _src->len; XrValue _outv = ");
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
    fprintf(out, "_out->len = _out_len; _outv; })");
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
    fprintf(out, ".ptr; int64_t _n = _src->len; %s _acc = (%s)", ctype_str(reduce.acc_rep),
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
    if (!v || v->nargs != 2 || !cg_array_elem_info_from_type(v->type, &info))
        return false;
    if (emit_typed_array_map_inline_expr(ctx, out, current, prefix, v))
        return true;

    fprintf(out, "xrt_array_map_typed(");
    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ", %s)", info.elem_name);
    return true;
}

static bool emit_typed_array_filter_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *current,
                                         const char *prefix, const XiValue *v) {
    CgArrayElemInfo info;
    if (!v || v->nargs != 2 || !cg_array_elem_info_from_type(v->type, &info))
        return false;
    if (emit_typed_array_filter_inline_expr(ctx, out, current, prefix, v))
        return true;

    fprintf(out, "xrt_array_filter_typed(");
    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ")");
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

    fprintf(out, "xrt_array_reduce_typed(");
    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
    fprintf(out, ", ");
    emit_value_as_rep(out, v->args[2], XR_REP_TAGGED);
    fprintf(out, ")");
    return true;
}
