/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_struct_helpers.inc.c - AOT native struct emission helpers
 */

static const char *cg_struct_native_c_type(uint8_t native_type) {
    const char *c_type = xaot_layout_c_type_for_native_type(native_type);
    return c_type ? c_type : "XrValue";
}

static XrRep cg_struct_native_rep(uint8_t native_type) {
    return xaot_layout_storage_rep_for_native_type(native_type);
}

static bool cg_struct_native_heap_supported_depth(const XrStructLayout *sl, int depth) {
    if (!sl || sl->field_count == 0 || sl->field_count > XR_MAX_STRUCT_FIELDS)
        return false;
    if (depth > 8)
        return false;
    for (uint16_t i = 0; i < sl->field_count; i++) {
        uint8_t native_type = sl->fields[i].native_type;
        if (xaot_layout_native_field_direct_heap_supported(native_type))
            continue;
        if (xaot_layout_native_field_uses_elem_layout(native_type)) {
            if (sl->fields[i].elem_count == 0 ||
                cg_struct_native_rep(sl->fields[i].elem_native_type) == XR_REP_TAGGED)
                return false;
            continue;
        }
        if (xaot_layout_native_field_uses_nested_layout(native_type)) {
            if (!cg_struct_native_heap_supported_depth(sl->fields[i].sub_layout, depth + 1))
                return false;
            continue;
        }
        return false;
    }
    return true;
}

static bool cg_struct_native_heap_supported(const XrStructLayout *sl) {
    return cg_struct_native_heap_supported_depth(sl, 0);
}

static uint64_t cg_struct_layout_hash_depth(const XrStructLayout *sl, int depth) {
    uint64_t h = UINT64_C(1469598103934665603);
    if (!sl)
        return h;
    if (depth > 8)
        return h ^ UINT64_C(0x9e3779b97f4a7c15);
    h ^= sl->field_count;
    h *= UINT64_C(1099511628211);
    for (uint16_t i = 0; i < sl->field_count; i++) {
        h ^= sl->fields[i].native_type;
        h *= UINT64_C(1099511628211);
        h ^= sl->fields[i].elem_native_type;
        h *= UINT64_C(1099511628211);
        h ^= sl->fields[i].elem_count;
        h *= UINT64_C(1099511628211);
        h ^= sl->fields[i].size;
        h *= UINT64_C(1099511628211);
        h ^= sl->fields[i].sub_layout_id;
        h *= UINT64_C(1099511628211);
        if (sl->fields[i].native_type == XR_NATIVE_STRUCT) {
            h ^= cg_struct_layout_hash_depth(sl->fields[i].sub_layout, depth + 1);
            h *= UINT64_C(1099511628211);
        }
    }
    return h;
}

static uint64_t cg_struct_layout_hash(const XrStructLayout *sl) {
    return cg_struct_layout_hash_depth(sl, 0);
}

static bool cg_struct_layout_same_shape_depth(const XrStructLayout *a, const XrStructLayout *b,
                                              int depth) {
    if (a == b)
        return true;
    if (!a || !b || a->field_count != b->field_count || depth > 8)
        return false;
    for (uint16_t i = 0; i < a->field_count; i++) {
        if (a->fields[i].native_type != b->fields[i].native_type ||
            a->fields[i].elem_native_type != b->fields[i].elem_native_type ||
            a->fields[i].elem_count != b->fields[i].elem_count ||
            a->fields[i].size != b->fields[i].size)
            return false;
        if (a->fields[i].native_type == XR_NATIVE_STRUCT &&
            !cg_struct_layout_same_shape_depth(a->fields[i].sub_layout, b->fields[i].sub_layout,
                                               depth + 1))
            return false;
    }
    return true;
}

static bool cg_struct_layout_same_shape(const XrStructLayout *a, const XrStructLayout *b) {
    return cg_struct_layout_same_shape_depth(a, b, 0);
}

static void cg_struct_heap_type_name(char *buf, size_t buflen, const char *prefix,
                                     const XrStructLayout *sl) {
    snprintf(buf, buflen, "xrt_struct_%s_%016" PRIx64, prefix ? prefix : "mod",
             cg_struct_layout_hash(sl));
}

static const XrStructFieldLayout *cg_struct_field(const XrStructLayout *sl, int64_t idx);
static const char *cg_struct_field_c_type(const XrStructLayout *sl, int64_t idx);

static void emit_struct_field_decl(FILE *out, const XrStructLayout *sl, int64_t idx,
                                   const char *name, const char *prefix) {
    const XrStructFieldLayout *field = cg_struct_field(sl, idx);
    if (field && field->native_type == XR_NATIVE_STRUCT && field->sub_layout) {
        char tname[128];
        cg_struct_heap_type_name(tname, sizeof(tname), prefix, field->sub_layout);
        fprintf(out, "%s %s", tname, name);
        return;
    }
    if (field && field->native_type == XR_NATIVE_ARRAY) {
        fprintf(out, "%s %s[%u]", cg_struct_native_c_type(field->elem_native_type), name,
                (unsigned) field->elem_count);
        return;
    }
    fprintf(out, "%s %s", cg_struct_field_c_type(sl, idx), name);
}

static void emit_struct_native_typedef(FILE *out, const XrStructLayout *sl, const char *prefix) {
    char tname[128];
    cg_struct_heap_type_name(tname, sizeof(tname), prefix, sl);
    fprintf(out, "typedef struct %s { uint32_t _size; uint32_t _layout; ", tname);
    for (uint16_t i = 0; i < sl->field_count; i++) {
        char fname[32];
        snprintf(fname, sizeof(fname), "f%u", i);
        emit_struct_field_decl(out, sl, i, fname, prefix);
        fprintf(out, "; ");
    }
    fprintf(out, "} %s;\n", tname);
}

#define CG_STRUCT_TYPEDEF_MAX 128

static void cg_collect_struct_layout(const XrStructLayout *sl, const XrStructLayout **layouts,
                                     uint64_t *hashes, int *count) {
    if (!cg_struct_native_heap_supported(sl) || !layouts || !hashes || !count ||
        *count >= CG_STRUCT_TYPEDEF_MAX)
        return;
    for (uint16_t i = 0; i < sl->field_count; i++) {
        if (sl->fields[i].native_type == XR_NATIVE_STRUCT)
            cg_collect_struct_layout(sl->fields[i].sub_layout, layouts, hashes, count);
    }
    uint64_t hash = cg_struct_layout_hash(sl);
    for (int i = 0; i < *count; i++) {
        if (hashes[i] == hash)
            return;
    }
    layouts[*count] = sl;
    hashes[*count] = hash;
    (*count)++;
}

static void cg_collect_struct_layouts_from_func(const XiFunc *f, const XrStructLayout **layouts,
                                                uint64_t *hashes, int *count) {
    if (!f)
        return;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            if (v->op == XI_STRUCT_NEW || v->op == XI_STRUCT_GET || v->op == XI_STRUCT_SET)
                cg_collect_struct_layout((const XrStructLayout *) v->aux, layouts, hashes, count);
        }
    }
    for (uint16_t ci = 0; ci < f->nchildren; ci++)
        cg_collect_struct_layouts_from_func(f->children[ci], layouts, hashes, count);
}

static void emit_struct_native_typedefs(FILE *out, const XiFunc *f, const char *prefix) {
    const XrStructLayout *layouts[CG_STRUCT_TYPEDEF_MAX];
    uint64_t hashes[CG_STRUCT_TYPEDEF_MAX];
    int count = 0;
    cg_collect_struct_layouts_from_func(f, layouts, hashes, &count);
    for (int i = 0; i < count; i++)
        emit_struct_native_typedef(out, layouts[i], prefix);
}

static const XrStructFieldLayout *cg_struct_field(const XrStructLayout *sl, int64_t idx) {
    if (!sl || idx < 0 || idx >= sl->field_count)
        return NULL;
    return &sl->fields[idx];
}

static const char *cg_struct_field_c_type(const XrStructLayout *sl, int64_t idx) {
    const XrStructFieldLayout *field = cg_struct_field(sl, idx);
    return field ? cg_struct_native_c_type(field->native_type) : "XrValue";
}

static XrRep cg_struct_field_rep(const XrStructLayout *sl, int64_t idx) {
    const XrStructFieldLayout *field = cg_struct_field(sl, idx);
    return field ? cg_struct_native_rep(field->native_type) : XR_REP_TAGGED;
}

static const XiValue *cg_trace_struct_new_depth(const XiValue *v, int depth) {
    if (!v || depth > 8)
        return NULL;

    while (v && (v->op == XI_COPY || v->op == XI_MOVE) && v->nargs >= 1) {
        if (++depth > 8)
            return NULL;
        v = v->args[0];
    }

    if (!v)
        return NULL;
    if (v->op == XI_STRUCT_NEW)
        return v;
    if (v->op != XI_PHI)
        return NULL;

    const XiValue *origin = NULL;
    for (uint16_t i = 0; i < v->nargs; i++) {
        const XiValue *arg = v->args[i];
        if (!arg || arg == v)
            continue;
        const XiValue *arg_origin = cg_trace_struct_new_depth(arg, depth + 1);
        if (!arg_origin)
            return NULL;
        if (!origin)
            origin = arg_origin;
        else if (origin != arg_origin)
            return NULL;
    }
    return origin;
}

static const XiValue *cg_trace_struct_new(const XiValue *v) {
    return cg_trace_struct_new_depth(v, 0);
}

static const XiValue *cg_trace_struct_new_identity(const XiValue *v) {
    while (v && (v->op == XI_COPY || v->op == XI_MOVE || v->op == XI_RETAIN) && v->nargs >= 1)
        v = v->args[0];
    return (v && v->op == XI_STRUCT_NEW) ? v : NULL;
}

static const XrStructLayout *cg_struct_layout_for_shared_slot_in_func(const XiFunc *f, int slot) {
    if (!f || slot < 0)
        return NULL;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v || v->op != XI_SET_SHARED || (int) v->aux_int != slot || v->nargs < 1)
                continue;
            const XiValue *origin = cg_trace_struct_new_identity(v->args[0]);
            if (origin && cg_struct_native_heap_supported((const XrStructLayout *) origin->aux))
                return (const XrStructLayout *) origin->aux;
        }
    }
    return NULL;
}

static const XrStructLayout *cg_struct_layout_for_shared_slot(const XiCgenCtx *ctx, const XiFunc *f,
                                                              int slot) {
    const XrStructLayout *sl = cg_struct_layout_for_shared_slot_in_func(f, slot);
    if (sl)
        return sl;
    if (ctx && ctx->module && ctx->module->init != f)
        return cg_struct_layout_for_shared_slot_in_func(ctx->module->init, slot);
    return NULL;
}

static bool cg_value_traces_to_heap_struct_shared_depth(const XiCgenCtx *ctx, const XiFunc *f,
                                                        const XiValue *v,
                                                        const XrStructLayout **out_layout,
                                                        int *out_slot, int depth) {
    if (!v || depth > 8)
        return false;
    while (v && (v->op == XI_COPY || v->op == XI_MOVE || v->op == XI_RETAIN) && v->nargs >= 1) {
        if (++depth > 8)
            return false;
        v = v->args[0];
    }
    if (!v)
        return false;

    if (v->op == XI_GET_SHARED) {
        int slot = (int) v->aux_int;
        const XrStructLayout *sl = cg_struct_layout_for_shared_slot(ctx, f, slot);
        if (!cg_struct_native_heap_supported(sl))
            return false;
        if (out_layout)
            *out_layout = sl;
        if (out_slot)
            *out_slot = slot;
        return true;
    }

    if (v->op != XI_PHI || v->nargs == 0)
        return false;

    bool saw_base = false;
    const XrStructLayout *sl = NULL;
    int slot = -1;
    for (uint16_t i = 0; i < v->nargs; i++) {
        const XiValue *arg = v->args[i];
        if (!arg || arg == v)
            continue;
        const XrStructLayout *arg_layout = NULL;
        int arg_slot = -1;
        if (!cg_value_traces_to_heap_struct_shared_depth(ctx, f, arg, &arg_layout, &arg_slot,
                                                         depth + 1))
            return false;
        if (!saw_base) {
            saw_base = true;
            sl = arg_layout;
            slot = arg_slot;
            continue;
        }
        if (slot != arg_slot || !cg_struct_layout_same_shape(sl, arg_layout))
            return false;
    }
    if (!saw_base || !cg_struct_native_heap_supported(sl))
        return false;
    if (out_layout)
        *out_layout = sl;
    if (out_slot)
        *out_slot = slot;
    return true;
}

static bool cg_value_traces_to_heap_struct_shared(const XiCgenCtx *ctx, const XiFunc *f,
                                                  const XiValue *v,
                                                  const XrStructLayout **out_layout,
                                                  int *out_slot) {
    return cg_value_traces_to_heap_struct_shared_depth(ctx, f, v, out_layout, out_slot, 0);
}

static bool cg_heap_struct_shared_alias_safe_uses(const XiCgenCtx *ctx, const XiFunc *f,
                                                  const XiValue *target, int slot, int depth) {
    if (!f || !target || depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t k = 0; k < phi->value.nargs; k++) {
                if (phi->value.args[k] != target)
                    continue;
                if (&phi->value == target)
                    continue;
                int alias_slot = -1;
                if (!cg_value_traces_to_heap_struct_shared(ctx, f, &phi->value, NULL,
                                                           &alias_slot) ||
                    alias_slot != slot)
                    return false;
                if (!cg_heap_struct_shared_alias_safe_uses(ctx, f, &phi->value, slot, depth + 1))
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
                if ((v->op == XI_STRUCT_GET || v->op == XI_STRUCT_SET) && a == 0)
                    continue;
                if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && a == 0)
                    continue;
                if ((v->op == XI_COPY || v->op == XI_MOVE) && a == 0) {
                    int alias_slot = -1;
                    if (!cg_value_traces_to_heap_struct_shared(ctx, f, v, NULL, &alias_slot) ||
                        alias_slot != slot)
                        return false;
                    if (!cg_heap_struct_shared_alias_safe_uses(ctx, f, v, slot, depth + 1))
                        return false;
                    continue;
                }
                return false;
            }
        }
    }
    return true;
}

static bool cg_value_is_elided_heap_struct_alias(const XiCgenCtx *ctx, const XiFunc *f,
                                                 const XiValue *v) {
    const XiValue *target = v;
    if (!v)
        return false;
    if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && v->nargs >= 1)
        target = v->args[0];
    int slot = -1;
    if (!cg_value_traces_to_heap_struct_shared(ctx, f, target, NULL, &slot))
        return false;
    return cg_heap_struct_shared_alias_safe_uses(ctx, f, target, slot, 0);
}

/* Check whether `target` is only observed through field operations or
 * same-origin identity nodes. */
static bool cg_struct_uses_safe_depth(const XiFunc *f, const XiValue *target, const XiValue *origin,
                                      int depth) {
    if (depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t k = 0; k < phi->value.nargs; k++) {
                if (phi->value.args[k] != target)
                    continue;
                if (&phi->value == target)
                    continue;
                if (cg_trace_struct_new(&phi->value) != origin)
                    return false;
                if (!cg_struct_uses_safe_depth(f, &phi->value, origin, depth + 1))
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
                if ((v->op == XI_STRUCT_GET || v->op == XI_STRUCT_SET) && a == 0)
                    continue;
                if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && a == 0)
                    continue;
                if ((v->op == XI_COPY || v->op == XI_MOVE) && a == 0) {
                    if (cg_trace_struct_new(v) != origin)
                        return false;
                    if (!cg_struct_uses_safe_depth(f, v, origin, depth + 1))
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

/* Check if XI_STRUCT_NEW value can be inlined as a local C struct.
 * True iff all transitive same-origin uses are field get/set ops. */
static bool cg_struct_can_inline(const XiFunc *f, const XiValue *target) {
    XR_DCHECK(f != NULL && target != NULL, "cg_struct_can_inline: NULL");
    const XiValue *origin = cg_trace_struct_new(target);
    return origin == target && cg_struct_uses_safe_depth(f, target, origin, 0);
}

static bool cg_value_traces_to_inlined_struct(const XiFunc *f, const XiValue *v) {
    const XiValue *origin = cg_trace_struct_new(v);
    return origin && cg_struct_can_inline(f, origin);
}

static bool cg_value_only_used_by_inlined_struct_new(const XiFunc *f, const XiValue *target) {
    bool seen = false;
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
                if (v->op == XI_STRUCT_NEW && a == 0 && cg_struct_can_inline(f, v)) {
                    seen = true;
                    continue;
                }
                return false;
            }
        }
    }
    return seen;
}

static void emit_struct_field_ref(FILE *out, const XiValue *origin, int64_t idx) {
    fprintf(out, "_st%u.f%d", origin ? origin->id : 0, (int) idx);
}

static void emit_struct_inline_field_get_expr(FILE *out, const XrStructLayout *sl,
                                              const XiValue *origin, int64_t idx) {
    const XrStructFieldLayout *field = cg_struct_field(sl, idx);
    if (field && field->native_type == XR_NATIVE_STRUCT) {
        fprintf(out, "xr_mkptr(&");
        emit_struct_field_ref(out, origin, idx);
        fprintf(out, ", XR_TAG_STRUCT_REF)");
        return;
    }
    if (field && field->native_type == XR_NATIVE_ARRAY) {
        fprintf(out, "xr_array_ref(&");
        emit_struct_field_ref(out, origin, idx);
        fprintf(out, "[0], %u, %u)", (unsigned) field->elem_native_type,
                (unsigned) field->elem_count);
        return;
    }
    if (field && field->native_type == XR_NATIVE_ARRAY_REF) {
        fprintf(out, "xr_mkptr(");
        emit_struct_field_ref(out, origin, idx);
        fprintf(out, ", XR_TAG_ARRAY)");
        return;
    }
    if (field && field->native_type == XR_NATIVE_MAP_REF) {
        fprintf(out, "xr_mkptr(");
        emit_struct_field_ref(out, origin, idx);
        fprintf(out, ", XR_TAG_MAP)");
        return;
    }
    emit_struct_field_ref(out, origin, idx);
}

static void emit_struct_field_store_value(FILE *out, const XrStructLayout *sl, int64_t idx,
                                          const XiValue *value) {
    const XrStructFieldLayout *field = cg_struct_field(sl, idx);
    if (field && field->native_type == XR_NATIVE_ARRAY_REF) {
        fprintf(out, "(xrt_array_t*)(");
        emit_value_as_rep(out, value, XR_REP_TAGGED);
        fprintf(out, ").ptr");
        return;
    }
    if (field && field->native_type == XR_NATIVE_MAP_REF) {
        fprintf(out, "(xrt_map_t*)(");
        emit_value_as_rep(out, value, XR_REP_TAGGED);
        fprintf(out, ").ptr");
        return;
    }
    XrRep field_rep = cg_struct_field_rep(sl, idx);
    if (field_rep != XR_REP_TAGGED)
        fprintf(out, "(%s)", cg_struct_field_c_type(sl, idx));
    emit_value_as_rep(out, value, field_rep);
}

static void emit_struct_inline_field_set_expr(FILE *out, const XrStructLayout *sl,
                                              const XiValue *origin, int64_t idx,
                                              const XiValue *value) {
    const XrStructFieldLayout *field = cg_struct_field(sl, idx);
    if (field && field->native_type == XR_NATIVE_STRUCT) {
        fprintf(out, "(memcpy(&");
        emit_struct_field_ref(out, origin, idx);
        fprintf(out, ", ");
        emit_value_as_rep(out, value, XR_REP_TAGGED);
        fprintf(out, ".ptr, sizeof(");
        emit_struct_field_ref(out, origin, idx);
        fprintf(out, ")), ");
        emit_value_as_rep(out, value, cg_rep(value));
        fprintf(out, ")");
        return;
    }
    if (field && field->native_type == XR_NATIVE_ARRAY) {
        fprintf(out, "(xrt_fixed_array_copy(&");
        emit_struct_field_ref(out, origin, idx);
        fprintf(out, "[0], ");
        emit_value_as_rep(out, value, XR_REP_TAGGED);
        fprintf(out, ", %u, %u), ", (unsigned) field->elem_native_type,
                (unsigned) field->elem_count);
        emit_value_as_rep(out, value, cg_rep(value));
        fprintf(out, ")");
        return;
    }
    fprintf(out, "(");
    emit_struct_field_ref(out, origin, idx);
    fprintf(out, " = ");
    emit_struct_field_store_value(out, sl, idx, value);
    fprintf(out, ")");
}

static bool cg_value_is_nested_struct_field_ref(const XiValue *v) {
    if (!v || v->op != XI_STRUCT_GET)
        return false;
    const XrStructLayout *sl = (const XrStructLayout *) v->aux;
    const XrStructFieldLayout *field = cg_struct_field(sl, v->aux_int);
    return field && field->native_type == XR_NATIVE_STRUCT && field->sub_layout &&
           cg_struct_native_heap_supported(sl) &&
           cg_struct_native_heap_supported(field->sub_layout);
}

static bool cg_nested_struct_ref_safe_uses(const XiFunc *f, const XiValue *target, int depth) {
    if (!f || !target || depth > 8)
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
                if ((v->op == XI_STRUCT_GET || v->op == XI_STRUCT_SET) && a == 0)
                    continue;
                if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && a == 0)
                    continue;
                if ((v->op == XI_COPY || v->op == XI_MOVE) && a == 0) {
                    if (!cg_nested_struct_ref_safe_uses(f, v, depth + 1))
                        return false;
                    continue;
                }
                return false;
            }
        }
    }
    return true;
}

static bool cg_value_is_elided_nested_struct_ref(const XiFunc *f, const XiValue *v) {
    const XiValue *target = v;
    while (target && (target->op == XI_COPY || target->op == XI_MOVE) && target->nargs >= 1)
        target = target->args[0];
    return cg_value_is_nested_struct_field_ref(target) &&
           cg_nested_struct_ref_safe_uses(f, target, 0);
}

static void emit_struct_field_boxed_value(FILE *out, const XrStructLayout *sl, int64_t idx,
                                          const XiValue *value) {
    const XrStructFieldLayout *field = cg_struct_field(sl, idx);
    if (!field) {
        emit_value_as_rep(out, value, XR_REP_TAGGED);
        return;
    }

    switch (field->native_type) {
        case XR_NATIVE_F32:
        case XR_NATIVE_F64:
            fprintf(out, "XR_FROM_FLOAT(");
            emit_value_as_rep(out, value, XR_REP_F64);
            fprintf(out, ")");
            break;
        case XR_NATIVE_BOOL:
            fprintf(out, "XR_FROM_BOOL(");
            emit_value_as_rep(out, value, XR_REP_I64);
            fprintf(out, ")");
            break;
        case XR_NATIVE_I8:
        case XR_NATIVE_I16:
        case XR_NATIVE_I32:
        case XR_NATIVE_I64:
        case XR_NATIVE_U8:
        case XR_NATIVE_U16:
        case XR_NATIVE_U32:
        case XR_NATIVE_U64:
            fprintf(out, "XR_FROM_INT(");
            emit_value_as_rep(out, value, XR_REP_I64);
            fprintf(out, ")");
            break;
        default:
            emit_value_as_rep(out, value, XR_REP_TAGGED);
            break;
    }
}

static void emit_struct_runtime_field_get(XiCgenCtx *ctx, FILE *out, const XrStructLayout *sl,
                                          int64_t idx, const XiValue *object,
                                          const XrType *result_type, XrRep result_rep) {
    const char *fname =
        (sl && sl->field_names && idx >= 0 && idx < sl->field_count) ? sl->field_names[idx] : NULL;
    const char *conv_suffix = emit_conversion_prefix(out, result_type, XR_REP_TAGGED, result_rep);
    fprintf(out, "xrt_map_get((xrt_map_t*)");
    emit_vref(out, object);
    fprintf(out, ".ptr, ");
    cg_emit_str_value(ctx, out, fname ? fname : "?");
    fprintf(out, ")");
    emit_conversion_suffix(out, conv_suffix);
}

static void emit_struct_runtime_field_set(XiCgenCtx *ctx, FILE *out, const XrStructLayout *sl,
                                          int64_t idx, const XiValue *object,
                                          const XiValue *value) {
    const char *fname =
        (sl && sl->field_names && idx >= 0 && idx < sl->field_count) ? sl->field_names[idx] : NULL;
    fprintf(out, "(xrt_map_set((xrt_map_t*)");
    emit_vref(out, object);
    fprintf(out, ".ptr, ");
    cg_emit_str_value(ctx, out, fname ? fname : "?");
    fprintf(out, ", ");
    emit_struct_field_boxed_value(out, sl, idx, value);
    fprintf(out, "), ");
    emit_value_as_rep(out, value, cg_rep(value));
    fprintf(out, ")");
}

static void emit_struct_fallback_new_expr(FILE *out, const XrStructLayout *sl, const char *prefix) {
    if (cg_struct_native_heap_supported(sl)) {
        char tname[128];
        cg_struct_heap_type_name(tname, sizeof(tname), prefix, sl);
        fprintf(out,
                "({ %s *_s = (%s*)xrt_arc_alloc(sizeof(%s)); _s->_size = "
                "(uint32_t)sizeof(%s); _s->_layout = UINT32_C(%" PRIu32 "); "
                "xr_mkptr(_s, XR_TAG_STRUCT_REF); })",
                tname, tname, tname, tname, (uint32_t) cg_struct_layout_hash(sl));
        return;
    }

    int64_t cap = sl && sl->field_count > 0 ? (int64_t) sl->field_count * 2 : 8;
    fprintf(out, "xrt_map_new(%" PRId64 ")", cap);
}

static void emit_struct_heap_object_ptr_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                             const XrStructLayout *sl, const XiValue *object) {
    const XrStructLayout *alias_layout = NULL;
    int slot = -1;
    if (cg_value_traces_to_heap_struct_shared(ctx, f, object, &alias_layout, &slot) &&
        cg_struct_layout_hash(alias_layout) == cg_struct_layout_hash(sl)) {
        fprintf(out, "%s[%d].ptr", ctx->shared_name, slot);
        return;
    }
    emit_vref(out, object);
    fprintf(out, ".ptr");
}

static bool emit_struct_heap_nested_object_ptr_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                    const XiValue *object, const char *prefix);

static void emit_struct_heap_field_lvalue(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                          const XrStructLayout *sl, int64_t idx,
                                          const XiValue *object, const char *prefix) {
    const XiValue *origin = cg_trace_struct_new(object);
    if (origin && cg_struct_can_inline(f, origin) &&
        cg_struct_layout_same_shape((const XrStructLayout *) origin->aux, sl)) {
        emit_struct_field_ref(out, origin, idx);
        return;
    }
    char tname[128];
    cg_struct_heap_type_name(tname, sizeof(tname), prefix, sl);
    fprintf(out, "((%s*)", tname);
    if (!emit_struct_heap_nested_object_ptr_expr(ctx, out, f, object, prefix))
        emit_struct_heap_object_ptr_expr(ctx, out, f, sl, object);
    fprintf(out, ")->f%d", (int) idx);
}

static bool emit_struct_heap_nested_object_ptr_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                    const XiValue *object, const char *prefix) {
    const XiValue *target = object;
    while (target && (target->op == XI_COPY || target->op == XI_MOVE) && target->nargs >= 1)
        target = target->args[0];
    if (!cg_value_is_elided_nested_struct_ref(f, target))
        return false;
    const XrStructLayout *parent = (const XrStructLayout *) target->aux;
    fprintf(out, "&");
    emit_struct_heap_field_lvalue(ctx, out, f, parent, target->aux_int, target->args[0], prefix);
    return true;
}

static bool emit_struct_heap_field_get_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                            const XrStructLayout *sl, int64_t idx,
                                            const XiValue *object, const char *prefix) {
    if (!cg_struct_native_heap_supported(sl) || idx < 0 || idx >= sl->field_count)
        return false;
    const XrStructFieldLayout *field = cg_struct_field(sl, idx);
    if (field && field->native_type == XR_NATIVE_STRUCT) {
        fprintf(out, "xr_mkptr(&");
        emit_struct_heap_field_lvalue(ctx, out, f, sl, idx, object, prefix);
        fprintf(out, ", XR_TAG_STRUCT_REF)");
        return true;
    }
    if (field && field->native_type == XR_NATIVE_ARRAY) {
        fprintf(out, "xr_array_ref(&");
        emit_struct_heap_field_lvalue(ctx, out, f, sl, idx, object, prefix);
        fprintf(out, "[0], %u, %u)", (unsigned) field->elem_native_type,
                (unsigned) field->elem_count);
        return true;
    }
    if (field && field->native_type == XR_NATIVE_ARRAY_REF) {
        fprintf(out, "xr_mkptr(");
        emit_struct_heap_field_lvalue(ctx, out, f, sl, idx, object, prefix);
        fprintf(out, ", XR_TAG_ARRAY)");
        return true;
    }
    if (field && field->native_type == XR_NATIVE_MAP_REF) {
        fprintf(out, "xr_mkptr(");
        emit_struct_heap_field_lvalue(ctx, out, f, sl, idx, object, prefix);
        fprintf(out, ", XR_TAG_MAP)");
        return true;
    }
    emit_struct_heap_field_lvalue(ctx, out, f, sl, idx, object, prefix);
    return true;
}

static void emit_struct_fallback_field_get(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                           const XrStructLayout *sl, int64_t idx,
                                           const XiValue *object, const XrType *result_type,
                                           XrRep result_rep, const char *prefix) {
    if (emit_struct_heap_field_get_expr(ctx, out, f, sl, idx, object, prefix))
        return;
    emit_struct_runtime_field_get(ctx, out, sl, idx, object, result_type, result_rep);
}

static bool emit_struct_heap_field_set_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                            const XrStructLayout *sl, int64_t idx,
                                            const XiValue *object, const XiValue *value,
                                            const char *prefix) {
    if (!cg_struct_native_heap_supported(sl) || idx < 0 || idx >= sl->field_count)
        return false;
    const XrStructFieldLayout *field = cg_struct_field(sl, idx);
    if (field && field->native_type == XR_NATIVE_STRUCT) {
        fprintf(out, "(memcpy(&");
        emit_struct_heap_field_lvalue(ctx, out, f, sl, idx, object, prefix);
        fprintf(out, ", ");
        emit_value_as_rep(out, value, XR_REP_TAGGED);
        fprintf(out, ".ptr, sizeof(");
        emit_struct_heap_field_lvalue(ctx, out, f, sl, idx, object, prefix);
        fprintf(out, ")), ");
        emit_value_as_rep(out, value, cg_rep(value));
        fprintf(out, ")");
        return true;
    }
    if (field && field->native_type == XR_NATIVE_ARRAY) {
        fprintf(out, "(xrt_fixed_array_copy(&");
        emit_struct_heap_field_lvalue(ctx, out, f, sl, idx, object, prefix);
        fprintf(out, "[0], ");
        emit_value_as_rep(out, value, XR_REP_TAGGED);
        fprintf(out, ", %u, %u), ", (unsigned) field->elem_native_type,
                (unsigned) field->elem_count);
        emit_value_as_rep(out, value, cg_rep(value));
        fprintf(out, ")");
        return true;
    }
    fprintf(out, "(");
    emit_struct_heap_field_lvalue(ctx, out, f, sl, idx, object, prefix);
    fprintf(out, " = ");
    emit_struct_field_store_value(out, sl, idx, value);
    fprintf(out, ")");
    return true;
}

static void emit_struct_fallback_field_set(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                           const XrStructLayout *sl, int64_t idx,
                                           const XiValue *object, const XiValue *value,
                                           const char *prefix) {
    if (emit_struct_heap_field_set_expr(ctx, out, f, sl, idx, object, value, prefix))
        return;
    emit_struct_runtime_field_set(ctx, out, sl, idx, object, value);
}

static const XiValue *cg_trace_fixed_array_field_ref(const XiValue *v) {
    while (v && (v->op == XI_COPY || v->op == XI_MOVE) && v->nargs >= 1)
        v = v->args[0];
    if (!v || v->op != XI_STRUCT_GET || v->nargs < 1)
        return NULL;
    const XrStructLayout *sl = (const XrStructLayout *) v->aux;
    const XrStructFieldLayout *field = cg_struct_field(sl, v->aux_int);
    return field && field->native_type == XR_NATIVE_ARRAY && cg_struct_native_heap_supported(sl)
               ? v
               : NULL;
}

static bool cg_fixed_array_ref_safe_uses(const XiFunc *f, const XiValue *target, int depth) {
    if (!f || !target || depth > 8)
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
                if ((v->op == XI_INDEX_GET || v->op == XI_INDEX_SET) && a == 0)
                    continue;
                if ((v->op == XI_COPY || v->op == XI_MOVE) && a == 0) {
                    if (!cg_fixed_array_ref_safe_uses(f, v, depth + 1))
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

static bool cg_value_is_elided_fixed_array_ref(const XiFunc *f, const XiValue *v) {
    const XiValue *target = cg_trace_fixed_array_field_ref(v);
    return target && cg_fixed_array_ref_safe_uses(f, target, 0);
}

static bool cg_fixed_array_const_index(const XiValue *v, int64_t *out) {
    v = cg_unwrap_identity_value(v);
    if (!v || v->op != XI_CONST || !v->type || v->type->kind != XR_KIND_INT)
        return false;
    if (out)
        *out = v->aux_int;
    return true;
}

static bool cg_fixed_array_control_proves_index_lt_count(const XiValue *control,
                                                         const XiValue *index,
                                                         uint16_t elem_count) {
    const XiValue *v = cg_unwrap_identity_value(control);
    int64_t bound = 0;
    return v && v->op == XI_LT && v->nargs >= 2 && cg_array_same_value(v->args[0], index) &&
           cg_fixed_array_const_index(v->args[1], &bound) && bound >= 0 &&
           bound <= (int64_t) elem_count;
}

static bool cg_fixed_array_index_bounds_proven(const XiValue *access, uint16_t elem_count) {
    if (!access || access->nargs < 2)
        return false;
    int64_t idx = 0;
    if (cg_fixed_array_const_index(access->args[1], &idx))
        return idx >= 0 && idx < (int64_t) elem_count;
    if (!access->block || !cg_array_value_known_nonnegative(access->args[1], NULL, 0) ||
        !cg_array_block_has_no_side_effect_before(access->block, access) ||
        access->block->npreds == 0)
        return false;
    for (uint16_t i = 0; i < access->block->npreds; i++) {
        const XiBlock *pred = access->block->preds[i];
        if (!pred || pred->kind != XI_BLOCK_IF || pred->succs[0] != access->block ||
            !cg_fixed_array_control_proves_index_lt_count(pred->control, access->args[1],
                                                          elem_count) ||
            !cg_array_block_has_no_side_effect_after(pred, pred->control))
            return false;
    }
    return true;
}

static bool emit_struct_fixed_array_index_get_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                   const XiValue *v, const char *prefix) {
    const XiValue *ref = (v && v->op == XI_INDEX_GET && v->nargs >= 2)
                             ? cg_trace_fixed_array_field_ref(v->args[0])
                             : NULL;
    if (!ref)
        return false;
    const XrStructLayout *sl = (const XrStructLayout *) ref->aux;
    const XrStructFieldLayout *field = cg_struct_field(sl, ref->aux_int);
    if (!field)
        return false;
    XrRep elem_rep = cg_struct_native_rep(field->elem_native_type);
    bool unchecked = cg_fixed_array_index_bounds_proven(v, field->elem_count);
    const char *conv_suffix = emit_conversion_prefix(out, v->type, elem_rep, cg_rep(v));
    if (!unchecked) {
        fprintf(out, "({ int64_t _idx = ");
        emit_value_as_rep(out, v->args[1], XR_REP_I64);
        fprintf(out,
                "; if (_idx < 0 || _idx >= %u) { fprintf(stderr, "
                "\"fixed array index out of range: %%lld (length %u)\\n\", "
                "(long long)_idx); abort(); } ",
                (unsigned) field->elem_count, (unsigned) field->elem_count);
    }
    fprintf(out, elem_rep == XR_REP_F64 ? "(double)" : "(int64_t)");
    emit_struct_heap_field_lvalue(ctx, out, f, sl, ref->aux_int, ref->args[0], prefix);
    fprintf(out, "[");
    if (unchecked)
        emit_value_as_rep(out, v->args[1], XR_REP_I64);
    else
        fprintf(out, "_idx");
    fprintf(out, "]");
    if (!unchecked)
        fprintf(out, "; })");
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool emit_struct_fixed_array_index_set_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                   const XiValue *v, const char *prefix) {
    const XiValue *ref = (v && v->op == XI_INDEX_SET && v->nargs >= 3)
                             ? cg_trace_fixed_array_field_ref(v->args[0])
                             : NULL;
    if (!ref)
        return false;
    const XrStructLayout *sl = (const XrStructLayout *) ref->aux;
    const XrStructFieldLayout *field = cg_struct_field(sl, ref->aux_int);
    if (!field)
        return false;
    bool unchecked = cg_fixed_array_index_bounds_proven(v, field->elem_count);
    if (unchecked) {
        fprintf(out, "(");
    } else {
        fprintf(out, "({ int64_t _idx = ");
        emit_value_as_rep(out, v->args[1], XR_REP_I64);
        fprintf(out,
                "; if (_idx < 0 || _idx >= %u) { fprintf(stderr, "
                "\"fixed array index out of range: %%lld (length %u)\\n\", "
                "(long long)_idx); abort(); } ",
                (unsigned) field->elem_count, (unsigned) field->elem_count);
    }
    emit_struct_heap_field_lvalue(ctx, out, f, sl, ref->aux_int, ref->args[0], prefix);
    fprintf(out, "[");
    if (unchecked)
        emit_value_as_rep(out, v->args[1], XR_REP_I64);
    else
        fprintf(out, "_idx");
    fprintf(out, "] = (%s)", cg_struct_native_c_type(field->elem_native_type));
    emit_value_as_rep(out, v->args[2], cg_struct_native_rep(field->elem_native_type));
    fprintf(out, unchecked ? ")" : "; })");
    return true;
}
