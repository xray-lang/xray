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
#include "../base/xmalloc.h"
#include "../runtime/value/xtype.h"
#include <string.h>

#define CORO_FNV_OFFSET UINT64_C(1469598103934665603)
#define CORO_FNV_PRIME UINT64_C(1099511628211)

typedef struct CoroPreparedBlocks {
    XiBlock *suspend;
    XiBlock *resume;
} CoroPreparedBlocks;

static void *coro_alloc(XiFunc *f, XiCoroPlan *plan, uint32_t count, uint32_t item_size) {
    if (count == 0)
        return NULL;
    if (item_size == 0 || count > UINT32_MAX / item_size)
        return NULL;
    uint32_t size = count * item_size;
    if (!plan || size > XI_CORO_MAX_PLAN_BYTES ||
        plan->planned_bytes > XI_CORO_MAX_PLAN_BYTES - size)
        return NULL;
    void *result = xi_func_arena_alloc(f, size);
    if (result) {
        memset(result, 0, size);
        plan->planned_bytes += size;
    }
    return result;
}

static XiBlock *coro_alloc_detached_block(XiFunc *f, XiCoroPlan *plan, uint32_t id,
                                          uint32_t value_capacity) {
    XiBlock *block = (XiBlock *) coro_alloc(f, plan, 1, (uint32_t) sizeof(XiBlock));
    if (!block)
        return NULL;
    block->id = id;
    block->kind = XI_BLOCK_PLAIN;
    block->func = f;
    block->values_cap = value_capacity > 0 ? value_capacity : 1;
    block->values = (XiValue **) coro_alloc(
        f, plan, block->values_cap, (uint32_t) sizeof(XiValue *));
    block->preds_cap = 4;
    block->preds =
        (XiBlock **) coro_alloc(f, plan, block->preds_cap, (uint32_t) sizeof(XiBlock *));
    return block->values && block->preds ? block : NULL;
}

static bool coro_reserve_block_index(XiFunc *f, uint32_t required) {
    if (!f || required < f->nblocks || required > UINT32_MAX / sizeof(XiBlock *))
        return false;
    if (required <= f->blocks_cap)
        return true;
    uint32_t capacity = f->blocks_cap ? f->blocks_cap : 16;
    while (capacity < required) {
        if (capacity > UINT32_MAX / 2u) {
            capacity = required;
            break;
        }
        capacity *= 2u;
    }
    XiBlock **blocks =
        (XiBlock **) xr_realloc(f->blocks, (size_t) capacity * sizeof(XiBlock *));
    if (!blocks)
        return false;
    f->blocks = blocks;
    f->blocks_cap = capacity;
    return true;
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
static void coro_split_point(XiFunc *f, XiCoroSuspendPoint *point, XiBlock *suspend,
                             XiBlock *resume) {
    XiBlock *pre = NULL;
    uint32_t op_index = 0;
    if (!coro_find_value(point->op, &pre, &op_index))
        return;
    uint32_t post_count = pre->nvalues - op_index - 1;

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
    point->continuation = resume;
}

static XiValue *coro_point_child_value(const XiCoroSuspendPoint *point) {
    if (!point || !point->op || point->op->nargs == 0)
        return NULL;
    if (point->op->op == XI_GO || point->op->op == XI_AWAIT || point->op->op == XI_CALL)
        return point->op->args[0];
    return NULL;
}

static bool coro_point_has_child(const XiCoroSuspendPoint *point) {
    return coro_point_child_value(point) != NULL || (point && point->resolved_callee != NULL);
}

static void coro_fill_edge(XiCoroEdge *edge, XiCoroEdgeKind kind,
                           const XiCoroSuspendPoint *point) {
    memset(edge, 0, sizeof(*edge));
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
        edge->child = coro_point_child_value(point);
        edge->callee = point->resolved_callee;
        edge->indirect_child = point->op->op == XI_CALL && !point->resolved_callee;
        return;
    }
    if (kind == XI_CORO_EDGE_ERROR && point->error_continuation) {
        edge->target_state_id = point->state_id;
        edge->target_block = point->error_continuation;
        return;
    }
    if (kind == XI_CORO_EDGE_PANIC && point->active_handler_count > 0) {
        XiValue *handler =
            point->active_handlers[point->active_handler_count - 1u];
        edge->target_state_id = point->state_id;
        edge->target_block = handler ? (XiBlock *) handler->aux : NULL;
        return;
    }
    edge->terminal = true;
    edge->target_state_id = XI_CORO_STATE_TERMINAL;
    edge->drops = point->drops;
    edge->ndrops = point->ndrops;
}

static bool coro_materialize_edges(XiFunc *f, XiCoroPlan *plan) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < plan->nstates; i++) {
        XiCoroSuspendPoint *point = &plan->points[i];
        uint32_t point_count = XI_CORO_EDGE_CHILD + (coro_point_has_child(point) ? 1u : 0u);
        if (count > UINT32_MAX - point_count)
            return false;
        count += point_count;
    }
    plan->edges = (XiCoroEdge *) coro_alloc(f, plan, count, (uint32_t) sizeof(XiCoroEdge));
    if (count && !plan->edges)
        return false;
    uint32_t cursor = 0;
    for (uint32_t i = 0; i < plan->nstates; i++) {
        XiCoroSuspendPoint *point = &plan->points[i];
        point->nedges =
            (uint8_t) (XI_CORO_EDGE_CHILD + (coro_point_has_child(point) ? 1u : 0u));
        point->edges = &plan->edges[cursor];
        cursor += point->nedges;
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
        f, plan, plan->ndispatch, (uint32_t) sizeof(XiCoroDispatchEntry));
    if (!plan->dispatch)
        return false;
    plan->dispatch[0].state_id = XI_CORO_STATE_ENTRY;
    plan->dispatch[0].target = plan->entry_block;
    return true;
}

static void coro_finalize_dispatch(XiCoroPlan *plan) {
    for (uint32_t i = 0; i < plan->nstates; i++) {
        plan->dispatch[i + 1].state_id = i + 1;
        plan->dispatch[i + 1].target = plan->points[i].suspend_block;
    }
}

static uint32_t coro_block_index(const XiFunc *f, const XiBlock *block) {
    for (uint32_t i = 0; f && i < f->nblocks; i++) {
        if (f->blocks[i] == block)
            return i;
    }
    return UINT32_MAX;
}

static bool coro_try_region_reaches_point(const XiFunc *f, const XiValue *try_op,
                                          const XiValue *point) {
    if (!f || !try_op || !try_op->block || !point || !point->block)
        return true;
    uint32_t try_index = try_op->block->nvalues;
    for (uint32_t i = 0; i < try_op->block->nvalues; i++) {
        if (try_op->block->values[i] == try_op) {
            try_index = i;
            break;
        }
    }
    if (try_index == try_op->block->nvalues)
        return true;

    uint8_t *visited = (uint8_t *) xr_calloc(f->nblocks, sizeof(uint8_t));
    XiBlock **queue = (XiBlock **) xr_calloc(f->nblocks, sizeof(XiBlock *));
    if (!visited || !queue) {
        xr_free(visited);
        xr_free(queue);
        return true;
    }
    uint32_t head = 0, tail = 0;
    uint32_t try_bi = coro_block_index(f, try_op->block);
    if (try_bi == UINT32_MAX) {
        xr_free(visited);
        xr_free(queue);
        return true;
    }
    visited[try_bi] = 1;

    bool stopped = false;
    for (uint32_t i = try_index + 1; i < try_op->block->nvalues; i++) {
        XiValue *value = try_op->block->values[i];
        if (value == point) {
            xr_free(visited);
            xr_free(queue);
            return true;
        }
        if (value && value->op == XI_END_TRY && value->aux == try_op) {
            stopped = true;
            break;
        }
    }
    if (!stopped) {
        for (uint32_t s = 0; s < 2; s++) {
            XiBlock *succ = try_op->block->succs[s];
            uint32_t succ_bi = coro_block_index(f, succ);
            if (succ && succ_bi != UINT32_MAX && !visited[succ_bi] && tail < f->nblocks) {
                visited[succ_bi] = 1;
                queue[tail++] = succ;
            }
        }
    }
    bool found = false;
    while (head < tail && !found) {
        XiBlock *block = queue[head++];
        bool ends_region = false;
        for (uint32_t i = 0; i < block->nvalues; i++) {
            XiValue *value = block->values[i];
            if (value == point) {
                found = true;
                break;
            }
            if (value && value->op == XI_END_TRY && value->aux == try_op) {
                ends_region = true;
                break;
            }
        }
        if (found || ends_region)
            continue;
        for (uint32_t s = 0; s < 2; s++) {
            XiBlock *succ = block->succs[s];
            uint32_t succ_bi = coro_block_index(f, succ);
            if (succ && succ_bi != UINT32_MAX && !visited[succ_bi] && tail < f->nblocks) {
                visited[succ_bi] = 1;
                queue[tail++] = succ;
            }
        }
    }
    xr_free(visited);
    xr_free(queue);
    return found;
}

static bool coro_block_has_catch_for_try(const XiBlock *handler,
                                         const XiValue *try_op) {
    if (!handler || !try_op)
        return false;
    uint32_t matches = 0;
    for (uint32_t i = 0; i < handler->nvalues; i++) {
        const XiValue *value = handler->values[i];
        if (value && value->op == XI_CATCH && value->aux == try_op)
            matches++;
    }
    return matches == 1;
}

static bool coro_panic_handler_is_supported(const XiFunc *f,
                                            const XiValue *try_op) {
    if (!f || !try_op || try_op->op != XI_TRY || !try_op->block ||
        !try_op->aux ||
        (try_op->aux_int != -1 &&
         try_op->aux_int != XI_TRY_AUX_STATIC_CLEANUP))
        return false;
    XiBlock *handler = (XiBlock *) try_op->aux;
    return coro_block_index(f, try_op->block) != UINT32_MAX &&
           coro_block_index(f, handler) != UINT32_MAX &&
           coro_block_has_catch_for_try(handler, try_op);
}

static bool coro_error_region_is_structural(const XiFunc *f,
                                            const XiErrorRegion *region,
                                            const XiValue *marker) {
    if (!f || !region || !marker || marker->op != XI_ERR_CATCH ||
        marker->error_region != region || region->catch_value != marker ||
        marker->block != region->catch_block ||
        !region->registration_block || !region->body_block ||
        !region->catch_block || !region->merge_block ||
        region->registration_block->succs[0] != region->body_block)
        return false;
    if (coro_block_index(f, region->registration_block) == UINT32_MAX ||
        coro_block_index(f, region->body_block) == UINT32_MAX ||
        coro_block_index(f, region->catch_block) == UINT32_MAX ||
        coro_block_index(f, region->merge_block) == UINT32_MAX)
        return false;
    const XiErrorRegion *ancestor = region;
    for (uint32_t depth = 0; ancestor; depth++, ancestor = ancestor->parent) {
        if (depth >= XI_CORO_MAX_EXCEPTION_DEPTH ||
            ancestor->parent == ancestor || !ancestor->catch_value ||
            ancestor->catch_value->op != XI_ERR_CATCH ||
            ancestor->catch_value->error_region != ancestor ||
            ancestor->catch_value->block != ancestor->catch_block ||
            !ancestor->registration_block || !ancestor->body_block ||
            !ancestor->catch_block || !ancestor->merge_block ||
            ancestor->registration_block->succs[0] != ancestor->body_block ||
            coro_block_index(f, ancestor->registration_block) == UINT32_MAX ||
            coro_block_index(f, ancestor->body_block) == UINT32_MAX ||
            coro_block_index(f, ancestor->catch_block) == UINT32_MAX ||
            coro_block_index(f, ancestor->merge_block) == UINT32_MAX)
            return false;
    }
    return true;
}

static bool coro_error_region_reaches_point(const XiFunc *f,
                                            const XiErrorRegion *region,
                                            const XiValue *point) {
    if (!f || !region || !point || !point->block)
        return false;
    uint8_t *visited = (uint8_t *) xr_calloc(f->nblocks, sizeof(uint8_t));
    XiBlock **queue = (XiBlock **) xr_calloc(f->nblocks, sizeof(XiBlock *));
    if (!visited || !queue) {
        xr_free(visited);
        xr_free(queue);
        return false;
    }
    uint32_t body_index = coro_block_index(f, region->body_block);
    if (body_index == UINT32_MAX) {
        xr_free(visited);
        xr_free(queue);
        return false;
    }
    uint32_t head = 0, tail = 0;
    visited[body_index] = 1;
    queue[tail++] = region->body_block;
    bool found = false;
    while (head < tail && !found) {
        XiBlock *block = queue[head++];
        if (block == region->catch_block || block == region->merge_block)
            continue;
        for (uint32_t i = 0; i < block->nvalues; i++) {
            if (block->values[i] == point) {
                found = true;
                break;
            }
        }
        for (uint32_t s = 0; s < 2 && !found; s++) {
            XiBlock *successor = block->succs[s];
            uint32_t index = coro_block_index(f, successor);
            if (successor && index != UINT32_MAX && !visited[index] &&
                tail < f->nblocks) {
                visited[index] = 1;
                queue[tail++] = successor;
            }
        }
    }
    xr_free(visited);
    xr_free(queue);
    return found;
}

static bool coro_error_region_is_ancestor(const XiErrorRegion *ancestor,
                                          const XiErrorRegion *region) {
    for (uint32_t depth = 0;
         region && depth < XI_CORO_MAX_EXCEPTION_DEPTH;
         depth++, region = region->parent) {
        if (region == ancestor)
            return true;
    }
    return false;
}

static bool coro_point_error_region(const XiFunc *f, const XiValue *point,
                                    XiErrorRegion **out_region) {
    XiErrorRegion *selected = NULL;
    for (uint32_t bi = 0; f && bi < f->nblocks; bi++) {
        const XiBlock *block = f->blocks[bi];
        for (uint32_t vi = 0; block && vi < block->nvalues; vi++) {
            XiValue *marker = block->values[vi];
            if (!marker || marker->op != XI_ERR_CATCH || !marker->error_region)
                continue;
            XiErrorRegion *candidate = marker->error_region;
            if (!coro_error_region_is_structural(f, candidate, marker))
                return false;
            if (!coro_error_region_reaches_point(f, candidate, point))
                continue;
            if (!selected || coro_error_region_is_ancestor(selected, candidate)) {
                selected = candidate;
            } else if (!coro_error_region_is_ancestor(candidate, selected)) {
                return false;
            }
        }
    }
    *out_region = selected;
    return true;
}

static bool coro_point_active_handler_count(const XiFunc *f,
                                            const XiValue *point,
                                            uint16_t *out_count) {
    uint32_t count = 0;
    for (uint32_t bi = 0; f && bi < f->nblocks; bi++) {
        const XiBlock *block = f->blocks[bi];
        for (uint32_t vi = 0; block && vi < block->nvalues; vi++) {
            const XiValue *try_op = block->values[vi];
            if (!try_op || try_op->op != XI_TRY ||
                !coro_try_region_reaches_point(f, try_op, point))
                continue;
            if (!coro_panic_handler_is_supported(f, try_op) ||
                count == UINT16_MAX)
                return false;
            count++;
        }
    }
    *out_count = (uint16_t) count;
    return true;
}

static bool coro_fill_point_active_handlers(const XiFunc *f,
                                            XiCoroSuspendPoint *point) {
    uint16_t count = 0;
    for (uint32_t bi = 0; f && bi < f->nblocks; bi++) {
        const XiBlock *block = f->blocks[bi];
        for (uint32_t vi = 0; block && vi < block->nvalues; vi++) {
            XiValue *try_op = block->values[vi];
            if (!try_op || try_op->op != XI_TRY ||
                !coro_try_region_reaches_point(f, try_op, point->op))
                continue;
            uint16_t insert = count;
            while (insert > 0 &&
                   point->active_handlers[insert - 1]->id > try_op->id) {
                point->active_handlers[insert] =
                    point->active_handlers[insert - 1];
                insert--;
            }
            point->active_handlers[insert] = try_op;
            count++;
        }
    }
    for (uint16_t i = 1; i < point->active_handler_count; i++) {
        if (!coro_try_region_reaches_point(
                f, point->active_handlers[i - 1], point->active_handlers[i]))
            return false;
    }
    return count == point->active_handler_count;
}

static bool coro_exception_continuations_supported(const XiFunc *f,
                                                   const XiCoroPlan *plan) {
    for (uint32_t i = 0; f && plan && i < plan->nstates; i++) {
        uint16_t handler_count = 0;
        XiErrorRegion *error_region = NULL;
        if (!coro_point_active_handler_count(f, plan->points[i].op,
                                             &handler_count) ||
            !coro_point_error_region(f, plan->points[i].op, &error_region))
            return false;
    }
    return f && plan;
}

static bool coro_materialize_exception_continuations(XiFunc *f,
                                                     XiCoroPlan *plan) {
    uint64_t total_handlers = 0;
    for (uint32_t i = 0; i < plan->nstates; i++) {
        XiCoroSuspendPoint *point = &plan->points[i];
        if (!coro_point_active_handler_count(f, point->op,
                                             &point->active_handler_count) ||
            !coro_point_error_region(f, point->op, &point->error_region))
            return false;
        point->error_continuation = point->error_region
                                        ? point->error_region->catch_block
                                        : NULL;
        total_handlers += point->active_handler_count;
        if (total_handlers > XI_CORO_MAX_FRAME_ACTIONS)
            return false;
    }
    plan->active_handlers = (XiValue **) coro_alloc(
        f, plan, (uint32_t) total_handlers, (uint32_t) sizeof(XiValue *));
    if (total_handlers > 0 && !plan->active_handlers)
        return false;
    plan->active_handler_count = (uint32_t) total_handlers;
    uint32_t cursor = 0;
    for (uint32_t i = 0; i < plan->nstates; i++) {
        XiCoroSuspendPoint *point = &plan->points[i];
        point->active_handlers = point->active_handler_count > 0
                                     ? &plan->active_handlers[cursor]
                                     : NULL;
        if (!coro_fill_point_active_handlers(f, point))
            return false;
        cursor += point->active_handler_count;
    }
    return cursor == plan->active_handler_count;
}

static uint32_t coro_point_capabilities(const XiCoroSuspendPoint *point) {
    uint32_t result = XI_CORO_CAP_SCHEDULER | XI_CORO_CAP_CANCEL_CLEANUP;
    if (!point || !point->op)
        return result;
    if (point->op->op == XI_CHAN_SEND || point->op->op == XI_CHAN_RECV ||
        point->kind == XI_CORO_SUSP_CHAN_SEND || point->kind == XI_CORO_SUSP_CHAN_RECV ||
        point->kind == XI_CORO_SUSP_SELECT)
        result |= XI_CORO_CAP_CHANNEL;
    if (point->op->op == XI_GO || point->op->op == XI_AWAIT ||
        point->kind == XI_CORO_SUSP_AWAIT)
        result |= XI_CORO_CAP_TASK;
    if (coro_point_has_child(point))
        result |= XI_CORO_CAP_CHILD_FRAME;
    return result;
}

static const XiFunc *coro_resolve_local_callee(const XiFunc *caller, const XiValue *callee) {
    if (!callee)
        return NULL;
    if ((callee->op == XI_CLOSURE_NEW ||
         (callee->op == XI_STACK_ALLOC && callee->aux_int == XI_CLOSURE_NEW)) &&
        callee->aux)
        return (const XiFunc *) callee->aux;
    if (callee->op == XI_GET_SHARED && callee->aux_int >= 0) {
        for (const XiFunc *owner = caller; owner; owner = owner->parent_func) {
            if (owner->shared_slot_funcs &&
                callee->aux_int < (int64_t) owner->shared_slot_func_count &&
                owner->shared_slot_funcs[callee->aux_int])
                return owner->shared_slot_funcs[callee->aux_int];
        }
    }
    if (xi_copy_is_identity_alias(callee) && callee->nargs > 0)
        return coro_resolve_local_callee(caller, callee->args[0]);
    const XiImportRef *ref = xi_value_import_ref(caller, callee);
    return ref ? ref->resolved_func : NULL;
}

static void coro_resolve_point_contract(XiCoroSuspendPoint *point,
                                        const XiCoroResolver *resolver, const XiFunc *f) {
    if (!point || !point->op)
        return;
    if (point->op->op == XI_CALL && point->op->nargs > 0 && resolver &&
        resolver->resolve_callee)
        point->resolved_callee =
            resolver->resolve_callee(resolver->ud, f, point->op->args[0]);
    else if ((point->op->op == XI_CALL_METHOD ||
              point->op->op == XI_CALL_METHOD_DIRECT) &&
             resolver && resolver->resolve_method)
        point->resolved_callee = resolver->resolve_method(resolver->ud, f, point->op);
    if (!point->resolved_callee && point->op->op == XI_CALL && point->op->nargs > 0)
        point->resolved_callee = coro_resolve_local_callee(f, point->op->args[0]);
    point->result_slot = point->op;
    point->error_slot = NULL;
    point->generation = point->state_id;
    point->store_state_id = point->state_id;
    point->returns_to_scheduler = true;
    point->capability_mask = coro_point_capabilities(point);
}

static bool coro_prepare_blocks(XiFunc *f, XiCoroPlan *plan, CoroPreparedBlocks **out) {
    if (!f || !plan || !out || plan->nstates > (UINT32_MAX - f->nblocks) / 2u)
        return false;
    uint32_t required = f->nblocks + plan->nstates * 2u;
    if (!coro_reserve_block_index(f, required))
        return false;
    CoroPreparedBlocks *blocks = (CoroPreparedBlocks *) coro_alloc(
        f, plan, plan->nstates, (uint32_t) sizeof(CoroPreparedBlocks));
    if (plan->nstates && !blocks)
        return false;
    for (uint32_t i = 0; i < plan->nstates; i++) {
        XiBlock *source = NULL;
        uint32_t op_index = 0;
        if (!coro_find_value(plan->points[i].op, &source, &op_index))
            return false;
        uint32_t capacity = source->nvalues > 0 ? source->nvalues : 1;
        uint32_t suspend_id = f->next_block_id + i * 2u;
        blocks[i].suspend = coro_alloc_detached_block(f, plan, suspend_id, 1);
        blocks[i].resume = coro_alloc_detached_block(f, plan, suspend_id + 1u, capacity);
        if (!blocks[i].suspend || !blocks[i].resume)
            return false;
    }
    *out = blocks;
    return true;
}

static void coro_attach_blocks(XiFunc *f, const XiCoroPlan *plan,
                               const CoroPreparedBlocks *blocks) {
    for (uint32_t i = 0; i < plan->nstates; i++) {
        f->blocks[f->nblocks++] = blocks[i].suspend;
        f->blocks[f->nblocks++] = blocks[i].resume;
    }
    f->next_block_id += plan->nstates * 2u;
}

static uint32_t coro_slot_index(const XiCoroPlan *plan, const XiValue *value) {
    const XiCoroSlot *slot = xi_coro_plan_find_slot(plan, value);
    return slot ? (uint32_t) (slot - plan->slots) : UINT32_MAX;
}

static void coro_add_action(XiCoroPlan *plan, uint32_t *cursor, XiCoroFrameActionKind kind,
                            const XiCoroSuspendPoint *point, const XiValue *value,
                            XiCoroEdgeKind edge_kind) {
    XiCoroFrameAction *action = &plan->frame_actions[(*cursor)++];
    action->kind = (uint8_t) kind;
    action->edge_kind = (uint8_t) edge_kind;
    action->state_id = point->state_id;
    bool continuation_identity =
        kind == XI_CORO_FRAME_STORE_ERROR_CONTINUATION ||
        kind == XI_CORO_FRAME_STORE_PANIC_HANDLER;
    action->slot_index = value && !continuation_identity
                             ? coro_slot_index(plan, value)
                             : UINT32_MAX;
    action->value = (XiValue *) value;
    action->target = kind == XI_CORO_FRAME_STORE_ERROR_CONTINUATION
                         ? point->error_continuation
                         : kind == XI_CORO_FRAME_STORE_PANIC_HANDLER
                               ? (value ? (XiBlock *) value->aux : NULL)
                               : point->continuation;
}

static uint64_t coro_action_capacity(const XiCoroPlan *plan) {
    return (uint64_t) plan->nstates *
               ((uint64_t) plan->slot_capacity * 8u + 3u) +
           plan->active_handler_count;
}

static bool coro_allocate_actions(XiFunc *f, XiCoroPlan *plan) {
    uint64_t capacity = coro_action_capacity(plan);
    if (capacity > XI_CORO_MAX_FRAME_ACTIONS)
        return false;
    plan->frame_actions = (XiCoroFrameAction *) coro_alloc(
        f, plan, (uint32_t) capacity, (uint32_t) sizeof(XiCoroFrameAction));
    if (capacity && !plan->frame_actions)
        return false;
    plan->frame_action_capacity = (uint32_t) capacity;
    return true;
}

static bool coro_ensure_action_capacity(XiFunc *f, XiCoroPlan *plan) {
    uint64_t capacity = coro_action_capacity(plan);
    if (capacity > XI_CORO_MAX_FRAME_ACTIONS)
        return false;
    if (capacity <= plan->frame_action_capacity)
        return true;
    XiCoroFrameAction *actions = (XiCoroFrameAction *) coro_alloc(
        f, plan, (uint32_t) capacity, (uint32_t) sizeof(XiCoroFrameAction));
    if (!actions)
        return false;
    plan->frame_actions = actions;
    plan->frame_action_capacity = (uint32_t) capacity;
    return true;
}

static void coro_finalize_actions(XiCoroPlan *plan) {
    uint32_t cursor = 0;
    for (uint32_t i = 0; i < plan->nstates; i++) {
        XiCoroSuspendPoint *point = &plan->points[i];
        point->action_begin = cursor;
        for (uint32_t j = 0; j < point->nlive; j++)
            coro_add_action(plan, &cursor, XI_CORO_FRAME_SPILL, point, point->live[j],
                            XI_CORO_EDGE_RESUME);
        if (point->error_region)
            coro_add_action(plan, &cursor,
                            XI_CORO_FRAME_STORE_ERROR_CONTINUATION, point,
                            point->error_region->catch_value, XI_CORO_EDGE_ERROR);
        for (uint16_t j = 0; j < point->active_handler_count; j++)
            coro_add_action(plan, &cursor, XI_CORO_FRAME_STORE_PANIC_HANDLER,
                            point, point->active_handlers[j],
                            XI_CORO_EDGE_PANIC);
        coro_add_action(plan, &cursor, XI_CORO_FRAME_STORE_STATE, point, NULL,
                        XI_CORO_EDGE_RESUME);
        coro_add_action(plan, &cursor, XI_CORO_FRAME_SCHED_EXIT, point, NULL,
                        XI_CORO_EDGE_RESUME);
        for (uint32_t j = 0; j < point->nlive; j++) {
            const XiCoroSlot *slot = xi_coro_plan_find_slot(plan, point->live[j]);
            coro_add_action(plan, &cursor, XI_CORO_FRAME_RELOAD, point, point->live[j],
                            XI_CORO_EDGE_RESUME);
            if (slot && slot->kind == XI_CORO_SLOT_PHI) {
                coro_add_action(plan, &cursor, XI_CORO_FRAME_PHI_CAPTURE, point,
                                point->live[j], XI_CORO_EDGE_RESUME);
                coro_add_action(plan, &cursor, XI_CORO_FRAME_PHI_COMMIT, point,
                                point->live[j], XI_CORO_EDGE_RESUME);
            }
        }
        for (uint8_t edge = XI_CORO_EDGE_ERROR; edge <= XI_CORO_EDGE_DROP; edge++) {
            for (uint32_t j = 0; j < point->ndrops; j++)
                coro_add_action(plan, &cursor, XI_CORO_FRAME_DROP, point, point->drops[j],
                                (XiCoroEdgeKind) edge);
        }
        point->action_count = cursor - point->action_begin;
    }
    plan->frame_action_count = cursor;
    plan->actions_materialized = cursor <= plan->frame_action_capacity;
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

static uint32_t coro_func_id(const XiFunc *func) {
    return func ? func->xg_body_func_id : UINT32_MAX;
}

static uint64_t coro_actions_fingerprint(const XiCoroPlan *plan) {
    uint64_t hash = CORO_FNV_OFFSET;
    hash = coro_hash_u32(hash, plan->frame_action_count);
    for (uint32_t i = 0; i < plan->frame_action_count; i++) {
        const XiCoroFrameAction *action = &plan->frame_actions[i];
        hash = coro_hash_u32(hash, action->kind);
        hash = coro_hash_u32(hash, action->edge_kind);
        hash = coro_hash_u32(hash, action->state_id);
        hash = coro_hash_u32(hash, action->slot_index);
        hash = coro_hash_u32(hash, coro_value_id(action->value));
        hash = coro_hash_u32(hash, coro_block_id(action->target));
    }
    return hash;
}

static uint64_t coro_plan_fingerprint(const XiCoroPlan *plan) {
    uint64_t hash = CORO_FNV_OFFSET;
    hash = coro_hash_u32(hash, plan->nstates);
    hash = coro_hash_u32(hash, plan->nslots);
    hash = coro_hash_u32(hash, plan->active_handler_count);
    hash = coro_hash_u32(hash, coro_block_id(plan->entry_block));
    for (uint32_t i = 0; i < plan->nslots; i++) {
        const XiCoroSlot *slot = &plan->slots[i];
        hash = coro_hash_u32(hash, coro_value_id(slot->value));
        hash = coro_hash_u32(hash, slot->type ? slot->type->id : UINT32_MAX);
        hash = coro_hash_u32(hash, slot->owner_value_id);
        hash = coro_hash_u32(hash, slot->logical_rep);
        hash = coro_hash_u32(hash, slot->kind);
        hash = coro_hash_u32(hash, slot->is_root);
        hash = coro_hash_u32(hash, slot->needs_release);
        hash = coro_hash_u32(hash, slot->needs_runtime_slot);
        hash = coro_hash_u32(hash, slot->needs_boundary_clone);
        hash = coro_hash_u32(hash, slot->live_across);
        hash = coro_hash_u32(hash, slot->frame_root);
        hash = coro_hash_u32(hash, slot->frame_release);
    }
    hash = coro_hash_u32(hash, plan->ndispatch);
    for (uint32_t i = 0; i < plan->ndispatch; i++) {
        hash = coro_hash_u32(hash, plan->dispatch[i].state_id);
        hash = coro_hash_u32(hash, coro_block_id(plan->dispatch[i].target));
    }
    for (uint32_t i = 0; i < plan->nstates; i++) {
        const XiCoroSuspendPoint *point = &plan->points[i];
        hash = coro_hash_u32(hash, point->state_id);
        hash = coro_hash_u32(hash, (uint32_t) point->kind);
        hash = coro_hash_u32(hash, coro_value_id(point->op));
        hash = coro_hash_u32(hash, coro_block_id(point->pre_block));
        hash = coro_hash_u32(hash, coro_block_id(point->suspend_block));
        hash = coro_hash_u32(hash, coro_block_id(point->resume_block));
        hash = coro_hash_u32(hash, coro_block_id(point->continuation));
        hash = coro_hash_u32(hash, coro_func_id(point->resolved_callee));
        hash = coro_hash_u32(hash, coro_value_id(point->result_slot));
        hash = coro_hash_u32(hash, coro_value_id(point->error_slot));
        hash = coro_hash_u32(
            hash, coro_value_id(point->error_region
                                    ? point->error_region->catch_value
                                    : NULL));
        hash = coro_hash_u32(hash,
                             coro_block_id(point->error_continuation));
        hash = coro_hash_u32(hash, point->active_handler_count);
        for (uint16_t j = 0; j < point->active_handler_count; j++)
            hash = coro_hash_u32(
                hash, coro_value_id(point->active_handlers[j]));
        hash = coro_hash_u32(hash, point->generation);
        hash = coro_hash_u32(hash, point->capability_mask);
        hash = coro_hash_u32(hash, point->store_state_id);
        hash = coro_hash_u32(hash, point->returns_to_scheduler);
        hash = coro_hash_u32(hash, point->action_begin);
        hash = coro_hash_u32(hash, point->action_count);
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
            hash = coro_hash_u32(hash, coro_func_id(edge->callee));
            hash = coro_hash_u32(hash, edge->indirect_child);
        }
    }
    hash = coro_hash_u32(hash, plan->actions_materialized);
    hash = coro_hash_u32(hash, (uint32_t) plan->action_fingerprint);
    hash = coro_hash_u32(hash, (uint32_t) (plan->action_fingerprint >> 32u));
    return hash;
}

static bool coro_rewrite_func(XiFunc *f, XiCoroPlan *plan,
                              const XiCoroResolver *resolver) {
    if (plan->cfg_rewritten)
        return xi_coro_plan_is_current(f, plan);
    if (plan->analyzed_ir_revision != f->ir_revision ||
        plan->analyzed_cfg_revision != f->cfg_version || !plan->analysis_complete)
        return false;
    if (!coro_exception_continuations_supported(f, plan))
        return false;
    for (uint32_t i = 0; i < plan->nstates; i++)
        coro_resolve_point_contract(&plan->points[i], resolver, f);
    if (!coro_materialize_exception_continuations(f, plan))
        return false;
    if (!coro_materialize_edges(f, plan))
        return false;
    if (!coro_allocate_dispatch(f, plan))
        return false;
    if (!coro_allocate_actions(f, plan))
        return false;

    CoroPreparedBlocks *blocks = NULL;
    if (!coro_prepare_blocks(f, plan, &blocks))
        return false;

    if (!plan->is_coroutine) {
        coro_finalize_edges(plan);
        coro_finalize_dispatch(plan);
        coro_finalize_actions(plan);
        plan->action_fingerprint = coro_actions_fingerprint(plan);
        plan->fingerprint = coro_plan_fingerprint(plan);
        plan->lowered_ir_revision = f->ir_revision;
        plan->lowered_cfg_revision = f->cfg_version;
        plan->cfg_rewritten = true;
        return plan->actions_materialized;
    }

    XiEditSession edit;
    if (!xi_edit_begin(&edit, f))
        return false;
    coro_attach_blocks(f, plan, blocks);
    for (uint32_t i = 0; i < plan->nstates; i++) {
        coro_split_point(f, &plan->points[i], blocks[i].suspend, blocks[i].resume);
    }

    XiPassOutcome outcome;
    char error[160] = {0};
    XiPassChange change = {.cfg_changed = true, .values_changed = true};
    if (!xi_edit_finish(&edit, change, 0, 0, &outcome, error, sizeof(error)))
        return false;
    if (!xi_coro_plan_refresh_point_sets(f, plan))
        return false;
    if (!coro_ensure_action_capacity(f, plan))
        return false;
    coro_finalize_edges(plan);
    coro_finalize_dispatch(plan);
    coro_finalize_actions(plan);
    if (!plan->actions_materialized)
        return false;
    plan->action_fingerprint = coro_actions_fingerprint(plan);
    plan->fingerprint = coro_plan_fingerprint(plan);
    plan->lowered_ir_revision = f->ir_revision;
    plan->lowered_cfg_revision = f->cfg_version;
    plan->cfg_rewritten = true;
    return true;
}

static bool coro_suspend_value_index(const XiBlock *block, const XiValue *value,
                                     uint32_t *index) {
    if (!block || !value || !index)
        return false;
    for (uint32_t i = 0; i < block->nvalues; i++) {
        if (block->values[i] == value) {
            *index = i;
            return true;
        }
    }
    return false;
}

static bool coro_reisolate_suspend_blocks(XiFunc *f, XiCoroPlan *plan) {
    bool changed = false;
    for (uint32_t i = 0; i < plan->nstates; i++) {
        XiCoroSuspendPoint *point = &plan->points[i];
        uint32_t op_index = 0;
        if (!point->op || point->op->block != point->suspend_block ||
            !coro_suspend_value_index(point->suspend_block, point->op, &op_index))
            return false;
        if (point->suspend_block->nvalues == 1 && op_index == 0)
            continue;
        uint32_t before = op_index;
        uint32_t after = point->suspend_block->nvalues - op_index - 1u;
        if (before > UINT32_MAX - point->pre_block->nvalues ||
            after > UINT32_MAX - point->resume_block->nvalues ||
            !xi_block_ensure_value_capacity(point->pre_block,
                                            point->pre_block->nvalues + before) ||
            !xi_block_ensure_value_capacity(point->resume_block,
                                            point->resume_block->nvalues + after))
            return false;
        changed = true;
    }
    if (!changed)
        return true;

    XiEditSession edit;
    if (!xi_edit_begin(&edit, f))
        return false;
    for (uint32_t i = 0; i < plan->nstates; i++) {
        XiCoroSuspendPoint *point = &plan->points[i];
        XiBlock *suspend = point->suspend_block;
        uint32_t op_index = 0;
        if (!coro_suspend_value_index(suspend, point->op, &op_index))
            return false;
        uint32_t before = op_index;
        uint32_t after = suspend->nvalues - op_index - 1u;
        for (uint32_t j = 0; j < before; j++) {
            XiValue *value = suspend->values[j];
            point->pre_block->values[point->pre_block->nvalues++] = value;
            value->block = point->pre_block;
        }
        if (after > 0) {
            memmove(point->resume_block->values + after, point->resume_block->values,
                    (size_t) point->resume_block->nvalues * sizeof(XiValue *));
            for (uint32_t j = 0; j < after; j++) {
                XiValue *value = suspend->values[op_index + 1u + j];
                point->resume_block->values[j] = value;
                value->block = point->resume_block;
            }
            point->resume_block->nvalues += after;
        }
        suspend->values[0] = point->op;
        suspend->nvalues = 1;
        point->op->block = suspend;
    }
    XiPassOutcome outcome;
    char error[160] = {0};
    XiPassChange change = {.values_changed = true};
    return xi_edit_finish(&edit, change, 0, 0, &outcome, error, sizeof(error));
}

XR_FUNC bool xi_coro_plan_rebase(XiFunc *f) {
    if (!f || !f->coro_plan)
        return f != NULL;
    XiCoroPlan *plan = f->coro_plan;
    /* Semantic snapshot detachment replaces analyzer-owned value types with
     * stable clones.  Slots name their value's current semantic type; keeping
     * the old pointer would make the frozen witness depend on freed analyzer
     * storage even though the value graph itself is fully detached. */
    for (uint32_t i = 0; i < plan->nslots; i++) {
        if (!plan->slots[i].value || !plan->slots[i].value->type)
            return false;
        plan->slots[i].type = plan->slots[i].value->type;
    }
    if (!plan->is_coroutine) {
        plan->action_fingerprint = coro_actions_fingerprint(plan);
        plan->fingerprint = coro_plan_fingerprint(plan);
        plan->lowered_ir_revision = f->ir_revision;
        plan->lowered_cfg_revision = f->cfg_version;
        return plan->analysis_complete && plan->cfg_rewritten &&
               plan->actions_materialized;
    }
    if (!plan->analysis_complete || !plan->cfg_rewritten || !plan->points ||
        !plan->dispatch || !plan->frame_actions)
        return false;
    if (!coro_reisolate_suspend_blocks(f, plan))
        return false;
    for (uint32_t i = 0; i < plan->nstates; i++) {
        XiCoroSuspendPoint *point = &plan->points[i];
        if (!point->op || !point->pre_block || !point->suspend_block ||
            !point->resume_block || point->suspend_block->nvalues != 1 ||
            point->suspend_block->values[0] != point->op)
            return false;
        coro_resolve_point_contract(point, NULL, f);
    }
    if (!xi_coro_plan_refresh_point_sets(f, plan))
        return false;
    if (!coro_ensure_action_capacity(f, plan))
        return false;
    coro_finalize_edges(plan);
    coro_finalize_dispatch(plan);
    coro_finalize_actions(plan);
    if (!plan->actions_materialized)
        return false;
    plan->action_fingerprint = coro_actions_fingerprint(plan);
    plan->fingerprint = coro_plan_fingerprint(plan);
    plan->lowered_ir_revision = f->ir_revision;
    plan->lowered_cfg_revision = f->cfg_version;
    return true;
}

/* Partition a function tree into target-neutral logical state machines.  The
 * current function's exceptional continuations are validated before children
 * are committed, so an ordinary semantic rejection cannot leave a rewritten
 * child under an unlowered parent. */
static bool coro_lower_func(XiFunc *f, const XiCoroResolver *resolver) {
    if (!f)
        return false;
    if (f->stage != XI_STAGE_SEMANTIC_LOWERED ||
        f->invariant_mask != xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED))
        return false;

    XiCoroPlan *prior_plan = f->coro_plan;
    XiCoroPlan *plan = xi_coro_analyze(f, resolver);
    if (!plan)
        return false;
    if (!coro_exception_continuations_supported(f, plan)) {
        if (!prior_plan)
            f->coro_plan = NULL;
        return false;
    }

    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (!coro_lower_func(f->children[i], resolver)) {
            if (!prior_plan)
                f->coro_plan = NULL;
            return false;
        }
    }

    if (!coro_rewrite_func(f, plan, resolver)) {
        if (!prior_plan)
            f->coro_plan = NULL;
        return false;
    }
    return true;
}

XR_FUNC bool xi_coro_lower(XiFunc *f, const XiCoroResolver *resolver) {
    return f && coro_lower_func(f, resolver);
}

XR_FUNC bool xi_coro_plan_is_current(const XiFunc *f, const XiCoroPlan *plan) {
    if (!f || !plan || !plan->analysis_complete || !plan->cfg_rewritten ||
        plan->lowered_cfg_revision != f->cfg_version || !plan->actions_materialized)
        return false;
    if (plan->action_fingerprint != coro_actions_fingerprint(plan))
        return false;
    return plan->fingerprint == coro_plan_fingerprint(plan);
}
