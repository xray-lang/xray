/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_class_helpers.inc.c - AOT class receiver field emission helpers
 */

static void cg_class_field_cache_reset(CgClassFieldCache *cache) {
    if (cache)
        memset(cache, 0, sizeof(*cache));
}

static int cg_class_field_cache_find(const CgClassFieldCache *cache, const char *name) {
    if (!cache || !name)
        return -1;
    for (uint16_t i = 0; i < cache->nfields; i++) {
        const char *field = cache->fields[i].name;
        if (field && strcmp(field, name) == 0)
            return (int) i;
    }
    return -1;
}

static int cg_class_layout_field_index(const XrStructLayout *layout, const char *name) {
    if (!layout || !layout->field_names || !name)
        return -1;
    for (uint16_t i = 0; i < layout->field_count; i++) {
        const char *field = layout->field_names[i];
        if (field && strcmp(field, name) == 0)
            return (int) i;
    }
    return -1;
}

static bool cg_class_field_cache_add_receiver_alias(CgClassFieldCache *cache, const XiValue *v) {
    if (!cache || !v)
        return false;
    for (uint16_t i = 0; i < cache->nreceiver_aliases; i++) {
        if (cache->receiver_aliases[i] == v)
            return false;
    }
    if (cache->nreceiver_aliases >= CG_MAX_CLASS_FIELD_CACHE_ALIASES)
        return false;
    cache->receiver_aliases[cache->nreceiver_aliases++] = v;
    return true;
}

static bool cg_class_field_cache_add(CgClassFieldCache *cache, const char *name, const XrType *type,
                                     XrRep rep, bool dirty) {
    if (!cache || !name || rep == XR_REP_TAGGED)
        return false;

    int layout_index = cg_class_layout_field_index(cache->layout, name);
    if (cache->layout && layout_index < 0)
        return false;

    int existing = cg_class_field_cache_find(cache, name);
    if (existing >= 0) {
        cache->fields[existing].dirty = cache->fields[existing].dirty || dirty;
        return true;
    }

    if (cache->nfields >= CG_MAX_CLASS_FIELD_CACHE)
        return false;

    uint16_t insert_at = cache->nfields;
    if (layout_index >= 0) {
        for (uint16_t i = 0; i < cache->nfields; i++) {
            if (cache->fields[i].layout_index > layout_index) {
                insert_at = i;
                break;
            }
        }
    }
    for (uint16_t i = cache->nfields; i > insert_at; i--)
        cache->fields[i] = cache->fields[i - 1];
    cache->nfields++;

    CgClassFieldCacheEntry *entry = &cache->fields[insert_at];
    entry->name = name;
    entry->type = type;
    entry->rep = rep;
    entry->dirty = dirty;
    entry->layout_index = (int16_t) layout_index;
    return true;
}

static bool cg_class_field_cache_func_is_constructor(const XiFunc *f) {
    if (!f || !f->name)
        return false;
    const char *name = strrchr(f->name, '.');
    name = name ? name + 1 : f->name;
    return strcmp(name, "constructor") == 0;
}

static const CgMethodEntry *cg_class_field_cache_method_entry(const XiCgenCtx *ctx,
                                                              const XiFunc *f) {
    if (!ctx || !f)
        return NULL;
    for (int i = 0; i < ctx->nmethod; i++) {
        if (ctx->methods[i].func == f)
            return &ctx->methods[i];
    }
    return NULL;
}

static bool cg_class_field_cache_is_receiver(const CgClassFieldCache *cache, const XiValue *v) {
    if (!cache || !v)
        return false;
    for (uint16_t i = 0; i < cache->nreceiver_aliases; i++) {
        if (cache->receiver_aliases[i] == v)
            return true;
    }
    return false;
}

static void emit_class_field_cache_receiver_expr(FILE *out, const CgClassFieldCache *cache) {
    const XiValue *receiver = cache ? cache->receiver : NULL;
    if (receiver && receiver->op == XI_PARAM && receiver->aux_int >= 0) {
        fprintf(out, "p%d", (int) receiver->aux_int);
        return;
    }
    emit_vref(out, receiver);
}

static bool cg_class_field_cache_op_is_globally_safe(const XiValue *v) {
    if (!v)
        return true;
    if (v->flags & (XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND))
        return false;
    switch ((XiOp) v->op) {
        case XI_PARAM:
        case XI_CONST:
        case XI_ADD:
        case XI_SUB:
        case XI_MUL:
        case XI_DIV:
        case XI_MOD:
        case XI_NEG:
        case XI_BAND:
        case XI_BOR:
        case XI_BXOR:
        case XI_BNOT:
        case XI_SHL:
        case XI_SHR:
        case XI_EQ:
        case XI_NE:
        case XI_LT:
        case XI_LE:
        case XI_GT:
        case XI_GE:
        case XI_NOT:
        case XI_CONVERT:
        case XI_BOX:
        case XI_UNBOX:
        case XI_NARROW_I8:
        case XI_NARROW_U8:
        case XI_NARROW_I16:
        case XI_NARROW_U16:
        case XI_NARROW_I32:
        case XI_NARROW_U32:
        case XI_NARROW_F32:
        case XI_WIDEN_I8:
        case XI_WIDEN_U8:
        case XI_WIDEN_I16:
        case XI_WIDEN_U16:
        case XI_WIDEN_I32:
        case XI_WIDEN_U32:
        case XI_WIDEN_F32:
        case XI_COPY:
        case XI_MOVE:
        case XI_LOAD_FIELD:
        case XI_STORE_FIELD:
        case XI_RETAIN:
        case XI_RELEASE:
            return true;
        default:
            return false;
    }
}

static bool cg_class_field_cache_receiver_use_is_safe(CgClassFieldCache *cache, const XiValue *v,
                                                      uint16_t arg_idx) {
    if (!cache || !v)
        return false;
    switch ((XiOp) v->op) {
        case XI_LOAD_FIELD: {
            if (arg_idx != 0)
                return false;
            const char *field = (const char *) v->aux;
            XrRep rep = cg_type_aot_storage_rep(v->type);
            if (field && rep != XR_REP_TAGGED)
                cg_class_field_cache_add(cache, field, v->type, rep, false);
            return true;
        }
        case XI_STORE_FIELD: {
            if (arg_idx != 0)
                return false;
            const char *field = (const char *) v->aux;
            const XrType *type = (v->nargs >= 2 && v->args[1]) ? v->args[1]->type : v->type;
            XrRep rep = cg_type_aot_storage_rep(type);
            if (field && rep != XR_REP_TAGGED)
                cg_class_field_cache_add(cache, field, type, rep, true);
            return true;
        }
        case XI_COPY:
        case XI_MOVE:
            return arg_idx == 0 && cg_class_field_cache_is_receiver(cache, v);
        case XI_RETAIN:
        case XI_RELEASE:
            return arg_idx == 0;
        default:
            return false;
    }
}

static bool cg_class_field_cache_can_collect(const XiCgenCtx *ctx, CgClassFieldCache *cache,
                                             const XiFunc *f) {
    if (!cache || !f || f->nparams == 0 || !f->params[0] || !f->params[0]->type)
        return false;
    if (cg_class_field_cache_func_is_constructor(f))
        return false;
    if (cg_type_is_task(f->params[0]->type))
        return false;
    const CgMethodEntry *method = cg_class_field_cache_method_entry(ctx, f);
    if (!f->receiver_borrowed && !method)
        return false;
    cache->receiver = f->params[0];
    cache->layout = method ? method->instance_layout : NULL;
    cache->class_data = method ? method->class_data : NULL;
    cache->native_receiver = cache->layout != NULL;
    cg_class_field_cache_add_receiver_alias(cache, cache->receiver);
    return true;
}

static bool cg_class_field_cache_phi_is_receiver_alias(CgClassFieldCache *cache, const XiPhi *phi) {
    if (!cache || !phi || phi->value.nargs == 0)
        return false;
    bool saw_known_alias = false;
    for (uint16_t ai = 0; ai < phi->value.nargs; ai++) {
        const XiValue *arg = phi->value.args[ai];
        if (arg == &phi->value)
            continue;
        if (!cg_class_field_cache_is_receiver(cache, arg))
            return false;
        saw_known_alias = true;
    }
    return saw_known_alias;
}

static bool cg_class_field_cache_value_defines_receiver_alias(CgClassFieldCache *cache,
                                                              const XiValue *v) {
    if (!cache || !v || v->nargs != 1)
        return false;
    switch ((XiOp) v->op) {
        case XI_COPY:
        case XI_MOVE:
            return cg_class_field_cache_is_receiver(cache, v->args[0]);
        default:
            return false;
    }
}

static void cg_class_field_cache_collect_receiver_aliases(CgClassFieldCache *cache,
                                                          const XiFunc *f) {
    if (!cache || !f)
        return;
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t bi = 0; bi < f->nblocks; bi++) {
            const XiBlock *blk = f->blocks[bi];
            if (!blk)
                continue;
            for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
                if (cg_class_field_cache_phi_is_receiver_alias(cache, phi))
                    changed |= cg_class_field_cache_add_receiver_alias(cache, &phi->value);
            }
            for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
                const XiValue *v = blk->values[vi];
                if (cg_class_field_cache_value_defines_receiver_alias(cache, v))
                    changed |= cg_class_field_cache_add_receiver_alias(cache, v);
            }
        }
    }
}

static void cg_class_field_cache_collect(XiCgenCtx *ctx, const XiFunc *f) {
    CgClassFieldCache draft;
    cg_class_field_cache_reset(&draft);
    if (!ctx || !cg_class_field_cache_can_collect(ctx, &draft, f))
        return;
    cg_class_field_cache_collect_receiver_aliases(&draft, f);

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (cg_class_field_cache_is_receiver(&draft, blk->control))
            return;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (cg_class_field_cache_is_receiver(&draft, &phi->value))
                continue;
            for (uint16_t ai = 0; ai < phi->value.nargs; ai++) {
                if (cg_class_field_cache_is_receiver(&draft, phi->value.args[ai]))
                    return;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!cg_class_field_cache_op_is_globally_safe(v))
                return;
            if (!v)
                continue;
            if ((v->op == XI_LOAD_FIELD || v->op == XI_STORE_FIELD) &&
                (v->nargs == 0 || !cg_class_field_cache_is_receiver(&draft, v->args[0])))
                return;
            for (uint16_t ai = 0; ai < v->nargs; ai++) {
                if (!cg_class_field_cache_is_receiver(&draft, v->args[ai]))
                    continue;
                if (!cg_class_field_cache_receiver_use_is_safe(&draft, v, ai))
                    return;
            }
        }
    }

    if (draft.nfields == 0)
        return;
    draft.active = true;
    ctx->class_field_cache = draft;
}

static void emit_class_field_cache_var(FILE *out, uint16_t index) {
    fprintf(out, "_cf%u", (unsigned) index);
}

static void emit_class_field_cache_native_path(const XiCgenCtx *ctx, FILE *out,
                                               const XiClassData *cd, int16_t layout_index) {
    if (!cd || layout_index < 0 || (uint16_t) layout_index >= cd->inherited_field_count) {
        fprintf(out, "f%d", (int) layout_index);
        return;
    }
    const XiClassData *super = cg_class_native_data_by_name(ctx, cd->super_name);
    fprintf(out, "base.");
    emit_class_field_cache_native_path(ctx, out, super, layout_index);
}

static void emit_class_field_cache_native_ref(XiCgenCtx *ctx, FILE *out,
                                              const CgClassFieldCache *cache,
                                              int16_t layout_index) {
    fprintf(out, "p0->");
    emit_class_field_cache_native_path(ctx, out, cache ? cache->class_data : NULL, layout_index);
}

static void emit_class_field_cache_value_box(FILE *out, const CgClassFieldCacheEntry *entry,
                                             uint16_t index) {
    if (entry->rep == XR_REP_F64) {
        fprintf(out, "XR_FROM_FLOAT(");
        emit_class_field_cache_var(out, index);
        fprintf(out, ")");
    } else if (entry->type && entry->type->kind == XR_KIND_BOOL) {
        fprintf(out, "XR_FROM_BOOL(");
        emit_class_field_cache_var(out, index);
        fprintf(out, ")");
    } else {
        fprintf(out, "XR_FROM_INT(");
        emit_class_field_cache_var(out, index);
        fprintf(out, ")");
    }
}

static void emit_class_field_cache_decls(XiCgenCtx *ctx, FILE *out) {
    CgClassFieldCache *cache = ctx ? &ctx->class_field_cache : NULL;
    if (!cache || !cache->active)
        return;
    for (uint16_t i = 0; i < cache->nfields; i++) {
        CgClassFieldCacheEntry *entry = &cache->fields[i];
        fprintf(out, "    %s ", ctype_str(entry->rep));
        emit_class_field_cache_var(out, i);
        fprintf(out, " = ");
        if (cache->native_receiver && entry->layout_index >= 0) {
            emit_class_field_cache_native_ref(ctx, out, cache, entry->layout_index);
        } else {
            bool wrapped = emit_conversion_prefix(out, entry->type, XR_REP_TAGGED, entry->rep);
            fprintf(out, "xrt_map_get((xrt_map_t*)");
            emit_class_field_cache_receiver_expr(out, cache);
            fprintf(out, ".ptr, ");
            cg_emit_str_value(ctx, out, entry->name);
            fprintf(out, ")");
            emit_conversion_suffix(out, wrapped);
        }
        fprintf(out, ";\n");
    }
}

static void emit_class_field_cache_flush(XiCgenCtx *ctx, FILE *out) {
    CgClassFieldCache *cache = ctx ? &ctx->class_field_cache : NULL;
    if (!cache || !cache->active)
        return;
    for (uint16_t i = 0; i < cache->nfields; i++) {
        CgClassFieldCacheEntry *entry = &cache->fields[i];
        if (!entry->dirty)
            continue;
        if (cache->native_receiver && entry->layout_index >= 0) {
            fprintf(out, "    ");
            emit_class_field_cache_native_ref(ctx, out, cache, entry->layout_index);
            fprintf(out, " = (%s)", cg_struct_field_c_type(cache->layout, entry->layout_index));
            emit_class_field_cache_var(out, i);
            fprintf(out, ";\n");
        } else {
            fprintf(out, "    xrt_map_set((xrt_map_t*)");
            emit_class_field_cache_receiver_expr(out, cache);
            fprintf(out, ".ptr, ");
            cg_emit_str_value(ctx, out, entry->name);
            fprintf(out, ", ");
            emit_class_field_cache_value_box(out, entry, i);
            fprintf(out, ");\n");
        }
    }
}

static bool emit_class_cached_field_load_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    CgClassFieldCache *cache = ctx ? &ctx->class_field_cache : NULL;
    if (!cache || !cache->active || !v || v->op != XI_LOAD_FIELD || v->nargs < 1 ||
        !cg_class_field_cache_is_receiver(cache, v->args[0]))
        return false;
    int index = cg_class_field_cache_find(cache, (const char *) v->aux);
    if (index < 0)
        return false;
    CgClassFieldCacheEntry *entry = &cache->fields[index];
    bool wrapped = emit_conversion_prefix(out, v->type, entry->rep, cg_rep(v));
    emit_class_field_cache_var(out, (uint16_t) index);
    emit_conversion_suffix(out, wrapped);
    return true;
}

static bool emit_class_cached_field_store_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    CgClassFieldCache *cache = ctx ? &ctx->class_field_cache : NULL;
    if (!cache || !cache->active || !v || v->op != XI_STORE_FIELD || v->nargs < 2 ||
        !cg_class_field_cache_is_receiver(cache, v->args[0]))
        return false;
    int index = cg_class_field_cache_find(cache, (const char *) v->aux);
    if (index < 0)
        return false;
    CgClassFieldCacheEntry *entry = &cache->fields[index];
    fprintf(out, "(");
    emit_class_field_cache_var(out, (uint16_t) index);
    fprintf(out, " = ");
    emit_value_as_rep(out, v->args[1], entry->rep);
    fprintf(out, ")");
    return true;
}
