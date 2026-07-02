/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_loop_helpers.inc.c - Structured loop emission helpers
 */

typedef struct CgStructuredLoop {
    const XiBlock *preheader;
    const XiBlock *guard;
    const XiBlock *body;
    const XiBlock *exit;
} CgStructuredLoop;

typedef struct CgStructuredArrayFillLoop {
    const XiBlock *entry;
    const XiBlock *body;
    const XiBlock *exit;
    CgArrayFillLoop fill;
} CgStructuredArrayFillLoop;

static bool emit_structured_loop_condition_expr(FILE *out, const XiValue *control);
static bool emit_structured_loop_condition_expr_ctx(XiCgenCtx *ctx, FILE *out,
                                                    const XiValue *control);

static bool cg_structured_value_used_outside_block(const XiFunc *f, const XiValue *target,
                                                   const XiBlock *owner) {
    if (!f || !target || !owner)
        return true;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk != owner && blk->control == target)
            return true;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t ai = 0; ai < phi->value.nargs; ai++) {
                if (phi->value.args[ai] == target && blk != owner)
                    return true;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *value = blk->values[vi];
            if (!value || value == target)
                continue;
            for (uint16_t ai = 0; ai < value->nargs; ai++) {
                if (value->args[ai] == target && blk != owner)
                    return true;
            }
        }
    }
    return false;
}

static bool cg_structured_block_phis_stay_local(const XiFunc *f, const XiBlock *blk) {
    if (!f || !blk)
        return false;
    for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
        if (cg_structured_value_used_outside_block(f, &phi->value, blk))
            return false;
    }
    return true;
}

static bool cg_structured_counted_loop_from_preheader(const XiBlock *preheader,
                                                      CgStructuredLoop *out) {
    if (!preheader || preheader->kind != XI_BLOCK_PLAIN || !preheader->succs[0])
        return false;
    if (preheader->id != 0)
        return false;
    const XiBlock *guard = preheader->succs[0];
    if (!guard || guard->kind != XI_BLOCK_IF || !guard->control || !guard->succs[0] ||
        !guard->succs[1])
        return false;
    const XiBlock *body = guard->succs[0];
    const XiBlock *exit = guard->succs[1];
    if (!body || body == guard || body->kind != XI_BLOCK_PLAIN || body->succs[0] != guard ||
        !exit || guard->npreds != 2 || body->npreds != 1 || body->preds[0] != guard ||
        guard->nvalues != 1 || guard->values[0] != guard->control)
        return false;
    if (cg_array_pred_index(guard, preheader) == UINT16_MAX ||
        cg_array_pred_index(guard, body) == UINT16_MAX)
        return false;
    for (uint16_t i = 0; i < guard->control->nargs; i++) {
        const XiValue *arg = cg_unwrap_identity_value(guard->control->args[i]);
        if (arg && arg->op != XI_PHI && arg->block == guard)
            return false;
    }
    for (uint32_t vi = 0; vi < body->nvalues; vi++) {
        const XiValue *value = body->values[vi];
        if (!value)
            continue;
        for (uint16_t ai = 0; ai < value->nargs; ai++) {
            const XiValue *arg = cg_unwrap_identity_value(value->args[ai]);
            if (arg && arg->op != XI_PHI && arg->block == guard)
                return false;
        }
    }
    if (out) {
        out->preheader = preheader;
        out->guard = guard;
        out->body = body;
        out->exit = exit;
    }
    return true;
}

static bool cg_structured_counted_loop_block_is_elided(const XiFunc *f, const XiBlock *blk) {
    if (!f || !blk)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        CgStructuredLoop loop;
        if (!cg_structured_counted_loop_from_preheader(f->blocks[bi], &loop) ||
            !emit_structured_loop_condition_expr(NULL, loop.guard->control) ||
            (blk != loop.guard && blk != loop.body))
            continue;
        bool preheader_is_elided = false;
        for (uint32_t pi = 0; pi < f->nblocks; pi++) {
            CgStructuredLoop parent;
            if (f->blocks[pi] == loop.preheader)
                continue;
            if (cg_structured_counted_loop_from_preheader(f->blocks[pi], &parent) &&
                emit_structured_loop_condition_expr(NULL, parent.guard->control) &&
                (loop.preheader == parent.guard || loop.preheader == parent.body)) {
                preheader_is_elided = true;
                break;
            }
        }
        if (!preheader_is_elided)
            return true;
    }
    return false;
}

static bool cg_structured_array_fill_loop_from_entry(XiCgenCtx *ctx, const XiFunc *f,
                                                     const XiBlock *entry,
                                                     CgStructuredArrayFillLoop *out) {
    if (!ctx || !f || !entry || entry->id != 0 || entry->kind != XI_BLOCK_IF || !entry->control ||
        !entry->succs[0] || !entry->succs[1])
        return false;
    const XiBlock *body = entry->succs[0];
    const XiBlock *exit = entry->succs[1];
    if (!body || body == entry || body->kind != XI_BLOCK_IF || body->succs[0] != body ||
        body->succs[1] != exit || exit->phis)
        return false;
    if (cg_array_pred_index(body, entry) == UINT16_MAX ||
        cg_array_pred_index(body, body) == UINT16_MAX)
        return false;
    if (!cg_structured_block_phis_stay_local(f, body))
        return false;

    CgArrayFillLoop fill;
    bool have = false;
    for (uint32_t vi = 0; vi < body->nvalues; vi++) {
        const XiValue *value = body->values[vi];
        if (!cg_array_fill_loop_match(ctx, f, value, &fill) || fill.exit_block != exit)
            continue;
        if (have)
            return false;
        have = true;
    }
    if (!have || !fill.index_value || !fill.cap_value)
        return false;
    if (out)
        *out = (CgStructuredArrayFillLoop) {entry, body, exit, fill};
    return true;
}

static bool cg_structured_array_fill_loop_block_is_elided(XiCgenCtx *ctx, const XiFunc *f,
                                                          const XiBlock *blk) {
    if (!ctx || !f || !blk)
        return false;
    CgStructuredArrayFillLoop loop;
    return cg_structured_array_fill_loop_from_entry(ctx, f, f->nblocks > 0 ? f->blocks[0] : NULL,
                                                    &loop) &&
           blk == loop.body;
}

static bool emit_structured_loop_condition_expr(FILE *out, const XiValue *control) {
    if (!control || control->nargs < 2)
        return false;
    const char *op = NULL;
    switch ((XiOp) control->op) {
        case XI_EQ:
            op = "==";
            break;
        case XI_NE:
            op = "!=";
            break;
        case XI_LT:
            op = "<";
            break;
        case XI_LE:
            op = "<=";
            break;
        case XI_GT:
            op = ">";
            break;
        case XI_GE:
            op = ">=";
            break;
        default:
            return false;
    }
    XrRep lhs_rep = cg_rep(control->args[0]);
    XrRep rhs_rep = cg_rep(control->args[1]);
    if (lhs_rep == XR_REP_TAGGED || rhs_rep == XR_REP_TAGGED)
        return false;
    XrRep rep = (lhs_rep == XR_REP_F64 || rhs_rep == XR_REP_F64) ? XR_REP_F64 : XR_REP_I64;
    if (out) {
        fprintf(out, "(");
        emit_value_as_rep(out, control->args[0], rep);
        fprintf(out, " %s ", op);
        emit_value_as_rep(out, control->args[1], rep);
        fprintf(out, ")");
    }
    return true;
}

static bool emit_structured_loop_condition_expr_ctx(XiCgenCtx *ctx, FILE *out,
                                                    const XiValue *control) {
    if (!control || control->nargs < 2)
        return false;
    const char *op = NULL;
    switch ((XiOp) control->op) {
        case XI_EQ:
            op = "==";
            break;
        case XI_NE:
            op = "!=";
            break;
        case XI_LT:
            op = "<";
            break;
        case XI_LE:
            op = "<=";
            break;
        case XI_GT:
            op = ">";
            break;
        case XI_GE:
            op = ">=";
            break;
        default:
            return false;
    }
    XrRep lhs_rep = cg_value_plan_storage_rep(ctx, control->args[0]);
    XrRep rhs_rep = cg_value_plan_storage_rep(ctx, control->args[1]);
    if (lhs_rep == XR_REP_TAGGED || rhs_rep == XR_REP_TAGGED)
        return false;
    XrRep rep = (lhs_rep == XR_REP_F64 || rhs_rep == XR_REP_F64) ? XR_REP_F64 : XR_REP_I64;
    if (out) {
        fprintf(out, "(");
        emit_value_as_rep_ctx(ctx, out, control->args[0], rep);
        fprintf(out, " %s ", op);
        emit_value_as_rep_ctx(ctx, out, control->args[1], rep);
        fprintf(out, ")");
    }
    return true;
}

static bool emit_structured_counted_loop_stmt(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                              const XiBlock *preheader, const char *prefix) {
    CgStructuredLoop loop;
    if (!cg_structured_counted_loop_from_preheader(preheader, &loop))
        return false;
    if (!emit_structured_loop_condition_expr_ctx(ctx, NULL, loop.guard->control))
        return false;

    uint16_t pre_idx = find_pred_idx(loop.guard, loop.preheader);
    uint16_t body_idx = find_pred_idx(loop.guard, loop.body);
    emit_phi_copies(ctx, out, f, loop.guard, pre_idx);
    emit_value_source_line(ctx, out, loop.guard->control);
    fprintf(out, "    while (");
    (void) emit_structured_loop_condition_expr_ctx(ctx, out, loop.guard->control);
    fprintf(out, ") {\n");
    for (uint32_t i = 0; i < loop.body->nvalues; i++) {
        XiValue *v = loop.body->values[i];
        if (v)
            emit_value_stmt(ctx, out, f, v, prefix);
    }
    emit_phi_copies(ctx, out, f, loop.guard, body_idx);
    emit_sync_backedge_heartbeat_if_edge(ctx, out, f, loop.body, loop.guard, "        ");
    fprintf(out, "    }\n");
    fprintf(out, "    goto L%u;\n", loop.exit->id);
    return true;
}

static bool emit_structured_array_fill_loop_stmt(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                 const XiBlock *entry, const char *prefix) {
    CgStructuredArrayFillLoop loop;
    if (!cg_structured_array_fill_loop_from_entry(ctx, f, entry, &loop))
        return false;

    uint16_t entry_idx = find_pred_idx(loop.body, loop.entry);
    uint16_t body_idx = find_pred_idx(loop.body, loop.body);
    emit_phi_copies(ctx, out, f, loop.body, entry_idx);
    emit_value_source_line(ctx, out, loop.body->control);
    fprintf(out, "    while (");
    emit_value_as_rep_ctx(ctx, out, loop.fill.index_value, XR_REP_I64);
    fprintf(out, " < ");
    emit_value_as_rep_ctx(ctx, out, loop.fill.cap_value, XR_REP_I64);
    fprintf(out, ") {\n");
    for (uint32_t i = 0; i < loop.body->nvalues; i++) {
        XiValue *v = loop.body->values[i];
        if (v && v != loop.body->control)
            emit_value_stmt(ctx, out, f, v, prefix);
    }
    emit_phi_copies(ctx, out, f, loop.body, body_idx);
    emit_sync_backedge_heartbeat_if_edge(ctx, out, f, loop.body, loop.body, "        ");
    fprintf(out, "    }\n");
    fprintf(out, "    goto L%u;\n", loop.exit->id);
    return true;
}
