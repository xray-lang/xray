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

static bool emit_structured_loop_condition_expr(FILE *out, const XiValue *control);

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

static bool emit_structured_counted_loop_stmt(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                              const XiBlock *preheader, const char *prefix) {
    CgStructuredLoop loop;
    if (!cg_structured_counted_loop_from_preheader(preheader, &loop))
        return false;
    if (!emit_structured_loop_condition_expr(NULL, loop.guard->control))
        return false;

    uint16_t pre_idx = find_pred_idx(loop.guard, loop.preheader);
    uint16_t body_idx = find_pred_idx(loop.guard, loop.body);
    emit_phi_copies(ctx, out, f, loop.guard, pre_idx);
    fprintf(out, "    while (");
    (void) emit_structured_loop_condition_expr(out, loop.guard->control);
    fprintf(out, ") {\n");
    for (uint32_t i = 0; i < loop.body->nvalues; i++) {
        XiValue *v = loop.body->values[i];
        if (v)
            emit_value_stmt(ctx, out, f, v, prefix);
    }
    emit_phi_copies(ctx, out, f, loop.guard, body_idx);
    fprintf(out, "    }\n");
    fprintf(out, "    goto L%u;\n", loop.exit->id);
    return true;
}
