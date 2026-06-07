/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_value_helpers.inc.c - AOT scalar value emission helpers
 */

static void emit_cell_ref(FILE *out, uint8_t var_id) {
    fprintf(out, "cell_%u", (unsigned) var_id);
}

static void emit_boxed_value_ref(FILE *out, const XiValue *v) {
    if (v && v->type && v->type->kind == XR_KIND_NULL) {
        fprintf(out, "XR_NULL_VAL");
        return;
    }

    XrRep rep = cg_rep(v);
    if (rep == XR_REP_TAGGED) {
        emit_vref(out, v);
    } else if (rep == XR_REP_F64) {
        fprintf(out, "XR_FROM_FLOAT(");
        emit_vref(out, v);
        fprintf(out, ")");
    } else if (cg_value_type_is_bool(v)) {
        fprintf(out, "XR_FROM_BOOL(");
        emit_vref(out, v);
        fprintf(out, ")");
    } else {
        fprintf(out, "XR_FROM_INT(");
        emit_vref(out, v);
        fprintf(out, ")");
    }
}

static void emit_cell_get_for_rep(FILE *out, const XiValue *v, const char *cell_expr) {
    XrRep rep = cg_rep(v);
    if (rep == XR_REP_TAGGED) {
        fprintf(out, "xrt_cell_get(%s)", cell_expr);
    } else if (rep == XR_REP_F64) {
        fprintf(out, "xrt_cell_get(%s).f", cell_expr);
    } else {
        fprintf(out, "xrt_cell_get(%s).i", cell_expr);
    }
}

static bool cg_value_has_cell(const XiCgenCtx *ctx, const XiValue *v) {
    return ctx && v && v->var_id != 0xFF && ctx->cell_vars[v->var_id];
}

static bool cg_value_is_cell_origin(const XiCgenCtx *ctx, const XiValue *v) {
    return cg_value_has_cell(ctx, v) && ctx->cell_origins[v->var_id] == v;
}

static bool cg_value_has_structural_use(const XiFunc *f, const XiValue *target) {
    if (!f || !target)
        return target && target->uses > 0;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return true;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == target)
                    return true;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] == target)
                    return true;
            }
        }
    }
    return false;
}

static bool cg_value_is_dead_aot_marker(const XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    if (!v || cg_value_has_cell(ctx, v) || cg_value_has_structural_use(f, v))
        return false;
    if (v->flags &
        (XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM | XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND))
        return false;
    switch ((XiOp) v->op) {
        case XI_CONST:
        case XI_BOX:
        case XI_UNBOX:
        case XI_COPY:
        case XI_MOVE:
            return true;
        default:
            return false;
    }
}

static void cg_prepare_cell_vars(XiCgenCtx *ctx, const XiFunc *f) {
    memset(ctx->cell_vars, 0, sizeof(ctx->cell_vars));
    memset(ctx->cell_origins, 0, sizeof(ctx->cell_origins));
    if (!f)
        return;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v || v->op != XI_CLOSURE_NEW || !v->aux)
                continue;
            const XiFunc *child = (const XiFunc *) v->aux;
            for (uint16_t ci = 0; ci < child->ncaptures; ci++) {
                const XiCapture *cap = &child->captures[ci];
                if (!cap->needs_cell || cap->source != XI_CAPTURE_SRC_REG)
                    continue;
                const XiValue *cap_val = (ci < v->nargs && v->args[ci]) ? v->args[ci] : cap->value;
                if (!cap_val || cap_val->var_id == 0xFF)
                    continue;
                uint8_t var_id = cap_val->var_id;
                ctx->cell_vars[var_id] = true;
                if (!ctx->cell_origins[var_id])
                    ctx->cell_origins[var_id] = cap_val;
            }
        }
    }
}

static void emit_c_string_literal(FILE *out, const char *s) {
    fputc('"', out);
    if (s) {
        for (const char *p = s; *p; p++) {
            if (*p == '"')
                fprintf(out, "\\\"");
            else if (*p == '\\')
                fprintf(out, "\\\\");
            else if (*p == '\n')
                fprintf(out, "\\n");
            else if (*p == '\t')
                fprintf(out, "\\t");
            else
                fputc(*p, out);
        }
    }
    fputc('"', out);
}

static void emit_enum_member_value_expr(FILE *out, const XiEnumMemberData *member) {
    if (!member) {
        fprintf(out, "XR_NULL_VAL");
        return;
    }
    switch (member->value_kind) {
        case XI_ENUM_LITERAL_INT:
            fprintf(out, "XR_FROM_INT(%" PRId64 ")", member->int_value);
            break;
        case XI_ENUM_LITERAL_FLOAT:
            fprintf(out, "XR_FROM_FLOAT(%a)", member->float_value);
            break;
        case XI_ENUM_LITERAL_BOOL:
            fprintf(out, "XR_FROM_BOOL(%d)", member->bool_value ? 1 : 0);
            break;
        case XI_ENUM_LITERAL_STRING:
            fprintf(out, "xr_box_str(");
            emit_c_string_literal(out, member->string_value);
            fprintf(out, ")");
            break;
        case XI_ENUM_LITERAL_NULL:
        default:
            fprintf(out, "XR_NULL_VAL");
            break;
    }
}

static void emit_enum_type_expr(FILE *out, const XiEnumData *ed) {
    if (!ed) {
        fprintf(out, "XR_NULL_VAL");
        return;
    }
    fprintf(out, "({ XrValue _e = xrt_map_new(%u); ", (unsigned) ed->member_count);
    for (uint32_t i = 0; i < ed->member_count; i++) {
        const XiEnumMemberData *member = &ed->members[i];
        const char *name = member->name ? member->name : "";
        fprintf(out, "xrt_map_set((xrt_map_t*)_e.ptr, xr_box_str(");
        emit_c_string_literal(out, name);
        fprintf(out, "), ");
        if (ed->is_adt)
            fprintf(out, "XR_FROM_INT(%u)", (unsigned) i);
        else
            emit_enum_member_value_expr(out, member);
        fprintf(out, "); ");
    }
    fprintf(out, "_e; })");
}

static int cg_enum_member_index(const XiEnumData *ed, const char *member_name) {
    if (!ed || !member_name)
        return -1;
    for (uint32_t i = 0; i < ed->member_count; i++) {
        if (ed->members[i].name && strcmp(ed->members[i].name, member_name) == 0)
            return (int) i;
    }
    return -1;
}

static const XiEnumData *cg_enum_for_shared_value(const XiCgenCtx *ctx, const XiValue *v) {
    v = cg_unwrap_identity_value(v);
    if (!ctx || !v || v->op != XI_GET_SHARED)
        return NULL;
    int slot = (int) v->aux_int;
    if (slot < 0 || slot >= CG_MAX_SHARED)
        return NULL;
    return ctx->shared_enum[slot];
}

static const XiEnumData *cg_enum_for_runtime_type(const XiCgenCtx *ctx, const void *runtime_type) {
    if (!ctx || !runtime_type)
        return NULL;
    for (int i = 0; i < CG_MAX_SHARED; i++) {
        const XiEnumData *ed = ctx->shared_enum[i];
        if (ed && ed->runtime_type == runtime_type)
            return ed;
    }
    return NULL;
}

static void emit_adt_enum_construct_expr(FILE *out, int member_idx, const XiValue *v) {
    uint16_t payload_count = v->nargs > 0 ? (uint16_t) (v->nargs - 1) : 0;
    fprintf(out, "({ XrValue _adt = xrt_array_new(%u); ", (unsigned) payload_count + 1);
    fprintf(out, "xrt_array_push(_adt, XR_FROM_INT(%d)); ", member_idx);
    for (uint16_t a = 1; a < v->nargs; a++) {
        fprintf(out, "xrt_array_push(_adt, ");
        emit_value_as_rep(out, v->args[a], XR_REP_TAGGED);
        fprintf(out, "); ");
    }
    fprintf(out, "_adt; })");
}

static void emit_call_hidden_closure(FILE *out, const XiFunc *current, const XiFunc *target,
                                     const XiValue *callee) {
    if (!target || target->ncaptures == 0) {
        fprintf(out, "NULL");
        return;
    }
    if (current == target && callee && callee->op == XI_CONST && callee->type &&
        callee->type->kind == XR_KIND_NULL) {
        fprintf(out, "_cl");
        return;
    }
    fprintf(out, "(xrt_closure_t*)");
    emit_vref(out, callee);
    fprintf(out, ".ptr");
}

static void emit_str_concat_expr(FILE *out, const XiValue *v) {
    if (!v || v->nargs == 0) {
        fprintf(out, "xr_box_str(\"\")");
        return;
    }
    if (v->nargs == 1) {
        emit_vref(out, v->args[0]);
        return;
    }
    for (uint16_t i = 1; i < v->nargs; i++)
        fprintf(out, "xrt_add(");
    emit_vref(out, v->args[0]);
    for (uint16_t i = 1; i < v->nargs; i++) {
        fprintf(out, ", ");
        emit_vref(out, v->args[i]);
        fprintf(out, ")");
    }
}

static void emit_closure_new_expr(XiCgenCtx *ctx, FILE *out, const char *prefix, const XiValue *v) {
    if (v->aux) {
        XiFunc *child = (XiFunc *) v->aux;
        uint16_t ncap = child->ncaptures;
        fprintf(out, "({ xrt_closure_t *_c = (xrt_closure_t*)xrt_closure_new((void*)");
        if (cg_func_uses_typed_abi(ctx, child))
            emit_typed_abi_fname(ctx, out, prefix, child);
        else
            emit_fname(ctx, out, prefix, child);
        fprintf(out, ", %u).ptr; ", ncap);
        for (uint16_t ci = 0; ci < ncap; ci++) {
            XiCapture *cap = &child->captures[ci];
            if (cap->needs_cell && cap->source == XI_CAPTURE_SRC_REG) {
                const XiValue *cap_val = (ci < v->nargs && v->args[ci]) ? v->args[ci] : cap->value;
                fprintf(out, "_c->upvals[%u] = ", ci);
                if (cap_val && cg_value_has_cell(ctx, cap_val))
                    emit_cell_ref(out, cap_val->var_id);
                else if (cap_val)
                    emit_vref(out, cap_val);
                else
                    fprintf(out, "XR_NULL_VAL");
                fprintf(out, "; ");
            } else if (ci < v->nargs && v->args[ci]) {
                fprintf(out, "_c->upvals[%u] = ", ci);
                emit_vref(out, v->args[ci]);
                fprintf(out, "; ");
            } else if (cap->source == XI_CAPTURE_SRC_UPVAL) {
                fprintf(out, "_c->upvals[%u] = _cl ? _cl->upvals[%u] : XR_NULL_VAL; ", ci,
                        (unsigned) cap->index);
            } else {
                ctx->error = true;
                fprintf(stderr, "[xi_cgen] ERROR: missing AOT closure capture '%s'\n",
                        cap->name ? cap->name : "?");
                fprintf(out, "_c->upvals[%u] = XR_NULL_VAL; ", ci);
            }
        }
        fprintf(out, "xr_mkptr(_c, XR_TAG_CLOSURE); })");
    } else {
        fprintf(out, "XR_NULL_VAL /* closure: unknown */");
    }
}
