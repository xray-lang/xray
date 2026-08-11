/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_coro_lower.c - Shared coroutine CFG lowering for Xi IR
 */

#include "xi_coro_lower.h"
#include "xi_edit.h"
#include <string.h>

#define CORO_FNV_OFFSET UINT64_C(1469598103934665603)
#define CORO_FNV_PRIME UINT64_C(1099511628211)

static void *coro_alloc(XiFunc *f, uint32_t count, uint32_t item_size) {
    if (count == 0)
        return NULL;
    if (item_size == 0 || count > UINT32_MAX / item_size)
        return NULL;
    uint32_t size = count * item_size;
    void *result = xi_func_arena_alloc(f, size);
    if (result)
        memset(result, 0, size);
    return result;
}

static bool coro_find_value(const XiValue *value, XiBlock **out_block, uint32_t *out_index) {
    if (!value || !value->block || !out_block || !out_index)
        return false;
    XiBlock *block = value->block;
    for (uint32_t i = 0; i < block->nvalues; i++) {
        if (block->values[i] == value) {
            *out_block = block;
            *out_index = i;
            return true;
        }
    }
    return false;
}

static void coro_replace_successor_preds(XiBlock *successor, XiBlock *old_pred,
                                         XiBlock *new_pred) {
    if (!successor)
        return;
    for (uint16_t i = 0; i < successor->npreds; i++) {
        if (successor->preds[i] == old_pred)
            successor->preds[i] = new_pred;
    }
}

/* Split one block into pre -> suspend -> resume while preserving successor PHI
 * operand positions.  Processing points in state order also handles multiple
 * suspension sites from the same original block deterministically. */
static bool coro_split_point(XiFunc *f, XiCoroSuspendPoint *point) {
    XiBlock *pre = NULL;
    uint32_t op_index = 0;
    if (!coro_find_value(point->op, &pre, &op_index))
        return false;

    XiBlock *suspend = xi_block_new(f);
    XiBlock *resume = xi_block_new(f);
    if (!suspend || !resume || !suspend->values || !suspend->preds || !resume->values ||
        !resume->preds)
        return false;
    uint32_t post_count = pre->nvalues - op_index - 1;
    if (!xi_block_ensure_value_capacity(resume, post_count))
        return false;

    uint16_t old_kind = pre->kind;
    uint32_t old_line = pre->line;
    XiValue *old_control = pre->control;
    XiBlock *old_succ0 = pre->succs[0];
    XiBlock *old_succ1 = pre->succs[1];

    for (uint32_t i = 0; i < post_count; i++) {
        XiValue *value = pre->values[op_index + 1 + i];
        resume->values[i] = value;
        value->block = resume;
    }
    resume->nvalues = post_count;
    resume->kind = old_kind;
    resume->line = old_line;
    resume->control = old_control;
    resume->succs[0] = old_succ0;
    resume->succs[1] = old_succ1;
    resume->sealed = true;
    coro_replace_successor_preds(old_succ0, pre, resume);
    if (old_succ1 != old_succ0)
        coro_replace_successor_preds(old_succ1, pre, resume);

    pre->nvalues = op_index;
    pre->control = NULL;
    pre->succs[0] = NULL;
    pre->succs[1] = NULL;
    pre->kind = XI_BLOCK_PLAIN;
    pre->line = point->op->line;

    suspend->values[0] = point->op;
    suspend->nvalues = 1;
    suspend->line = point->op->line;
    suspend->sealed = true;
    point->op->block = suspend;

    xi_block_set_jump(pre, suspend);
    xi_block_set_jump(suspend, resume);
    point->pre_block = pre;
    point->suspend_block = suspend;
    point->resume_block = resume;
    return true;
}

static bool coro_point_has_child(const XiCoroSuspendPoint *point) {
    return point && point->op && point->op->nargs > 0 &&
           (point->kind == XI_CORO_SUSP_CALL || point->kind == XI_CORO_SUSP_GO ||
            point->kind == XI_CORO_SUSP_AWAIT);
}

static void coro_fill_edge(XiCoroEdge *edge, XiCoroEdgeKind kind,
                           const XiCoroSuspendPoint *point) {
    edge->kind = (uint8_t) kind;
    edge->source_state_id = point->state_id;
    edge->roots = point->roots;
    edge->nroots = point->nroots;
    if (kind == XI_CORO_EDGE_RESUME) {
        edge->target_state_id = point->state_id;
        edge->target_block = point->resume_block;
        return;
    }
    if (kind == XI_CORO_EDGE_CHILD) {
        edge->target_state_id = XI_CORO_STATE_ENTRY;
        edge->child = point->op->args[0];
        edge->indirect_child = point->kind == XI_CORO_SUSP_CALL &&
                               point->op->xg_callsite_id == 0;
        return;
    }
    edge->terminal = true;
    edge->target_state_id = XI_CORO_STATE_TERMINAL;
    edge->drops = point->drops;
    edge->ndrops = point->ndrops;
}

static bool coro_materialize_edges(XiFunc *f, XiCoroPlan *plan) {
    for (uint32_t i = 0; i < plan->nstates; i++) {
        XiCoroSuspendPoint *point = &plan->points[i];
        uint8_t count = (uint8_t) (XI_CORO_EDGE_CHILD + (coro_point_has_child(point) ? 1 : 0));
        point->edges =
            (XiCoroEdge *) coro_alloc(f, count, (uint32_t) sizeof(XiCoroEdge));
        if (!point->edges)
            return false;
        point->nedges = count;
    }
    return true;
}

static void coro_finalize_edges(XiCoroPlan *plan) {
    plan->edge_count = 0;
    for (uint32_t i = 0; i < plan->nstates; i++) {
        XiCoroSuspendPoint *point = &plan->points[i];
        for (uint8_t edge = 0; edge < point->nedges; edge++)
            coro_fill_edge(&point->edges[edge], (XiCoroEdgeKind) edge, point);
        plan->edge_count += point->nedges;
    }
}

static bool coro_allocate_dispatch(XiFunc *f, XiCoroPlan *plan) {
    plan->ndispatch = plan->nstates + 1;
    plan->dispatch = (XiCoroDispatchEntry *) coro_alloc(
        f, plan->ndispatch, (uint32_t) sizeof(XiCoroDispatchEntry));
    if (!plan->dispatch)
        return false;
    plan->dispatch[0].state_id = XI_CORO_STATE_ENTRY;
    plan->dispatch[0].target = plan->entry_block;
    return true;
}

static void coro_finalize_dispatch(XiCoroPlan *plan) {
    for (uint32_t i = 0; i < plan->nstates; i++) {
        plan->dispatch[i + 1].state_id = i + 1;
        plan->dispatch[i + 1].target = plan->points[i].resume_block;
    }
}

static bool coro_has_exception_region(const XiFunc *f) {
    for (uint32_t bi = 0; f && bi < f->nblocks; bi++) {
        const XiBlock *block = f->blocks[bi];
        for (uint32_t vi = 0; block && vi < block->nvalues; vi++) {
            const XiValue *value = block->values[vi];
            if (value && (value->op == XI_TRY || value->op == XI_END_TRY))
                return true;
        }
    }
    return false;
}

static uint64_t coro_hash_u32(uint64_t hash, uint32_t value) {
    for (uint32_t i = 0; i < 4; i++) {
        hash ^= (uint8_t) (value >> (i * 8u));
        hash *= CORO_FNV_PRIME;
    }
    return hash;
}

static uint32_t coro_value_id(const XiValue *value) {
    return value ? value->id : UINT32_MAX;
}

static uint32_t coro_block_id(const XiBlock *block) {
    return block ? block->id : UINT32_MAX;
}

static uint64_t coro_plan_fingerprint(const XiCoroPlan *plan) {
    uint64_t hash = CORO_FNV_OFFSET;
    hash = coro_hash_u32(hash, plan->nstates);
    hash = coro_hash_u32(hash, plan->nslots);
    hash = coro_hash_u32(hash, coro_block_id(plan->entry_block));
    for (uint32_t i = 0; i < plan->nstates; i++) {
        const XiCoroSuspendPoint *point = &plan->points[i];
        hash = coro_hash_u32(hash, point->state_id);
        hash = coro_hash_u32(hash, (uint32_t) point->kind);
        hash = coro_hash_u32(hash, coro_value_id(point->op));
        hash = coro_hash_u32(hash, coro_block_id(point->pre_block));
        hash = coro_hash_u32(hash, coro_block_id(point->suspend_block));
        hash = coro_hash_u32(hash, coro_block_id(point->resume_block));
        hash = coro_hash_u32(hash, point->nlive);
        for (uint32_t j = 0; j < point->nlive; j++)
            hash = coro_hash_u32(hash, coro_value_id(point->live[j]));
        hash = coro_hash_u32(hash, point->nroots);
        for (uint32_t j = 0; j < point->nroots; j++)
            hash = coro_hash_u32(hash, coro_value_id(point->roots[j]));
        hash = coro_hash_u32(hash, point->ndrops);
        for (uint32_t j = 0; j < point->ndrops; j++)
            hash = coro_hash_u32(hash, coro_value_id(point->drops[j]));
        hash = coro_hash_u32(hash, point->nedges);
        for (uint8_t j = 0; j < point->nedges; j++) {
            const XiCoroEdge *edge = &point->edges[j];
            hash = coro_hash_u32(hash, edge->kind);
            hash = coro_hash_u32(hash, edge->target_state_id);
            hash = coro_hash_u32(hash, coro_block_id(edge->target_block));
            hash = coro_hash_u32(hash, coro_value_id(edge->child));
        }
    }
    return hash;
}

static bool coro_rewrite_func(XiFunc *f, XiCoroPlan *plan) {
    if (!plan->is_coroutine)
        return true;
    if (plan->cfg_rewritten)
        return plan->lowered_ir_revision == f->ir_revision &&
               plan->lowered_cfg_revision == f->cfg_version;
    if (plan->analyzed_ir_revision != f->ir_revision ||
        plan->analyzed_cfg_revision != f->cfg_version)
        return false;
    /* XI_TRY uses implicit handler predecessor edges.  Splitting a suspend in
     * such a region is not semantics-preserving until the logical plan carries
     * the active handler/defer stack, so reject the whole function before any
     * CFG mutation. */
    if (coro_has_exception_region(f))
        return false;
    if (!coro_materialize_edges(f, plan))
        return false;
    if (!coro_allocate_dispatch(f, plan))
        return false;

    XiEditSession edit;
    if (!xi_edit_begin(&edit, f))
        return false;
    for (uint32_t i = 0; i < plan->nstates; i++) {
        if (!coro_split_point(f, &plan->points[i]))
            return false;
    }
    coro_finalize_edges(plan);
    coro_finalize_dispatch(plan);
    plan->fingerprint = coro_plan_fingerprint(plan);

    XiPassOutcome outcome;
    char error[160] = {0};
    XiPassChange change = {.cfg_changed = true, .values_changed = true};
    if (!xi_edit_finish(&edit, change, 0, 0, &outcome, error, sizeof(error)))
        return false;
    plan->lowered_ir_revision = f->ir_revision;
    plan->lowered_cfg_revision = f->cfg_version;
    plan->cfg_rewritten = true;
    return true;
}

/* Partition a function into the target-neutral logical state machine.  Children
 * are processed first so a parent's direct-suspend-call analysis observes
 * their complete logical plans. */
static bool coro_lower_func(XiFunc *f, const XiCoroResolver *resolver) {
    if (!f)
        return false;
    if (f->stage != XI_STAGE_SEMANTIC_LOWERED ||
        f->invariant_mask != xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED))
        return false;

    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (!coro_lower_func(f->children[i], resolver))
            return false;
    }

    XiCoroPlan *plan = xi_coro_analyze(f, resolver);
    return plan && coro_rewrite_func(f, plan);
}

XR_FUNC bool xi_coro_lower(XiFunc *f, const XiCoroResolver *resolver) {
    return f && coro_lower_func(f, resolver);
}
