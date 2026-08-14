/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_coro_exception_verify.c - Independent coroutine continuation verifier
 */

#include "xi_coro_exception_verify.h"
#include "../base/xmalloc.h"

#include <stdarg.h>
#include <stdio.h>

static bool exception_error(char *error, size_t error_size, const char *format, ...) {
    if (error && error_size > 0) {
        va_list args;
        va_start(args, format);
        vsnprintf(error, error_size, format, args);
        va_end(args);
    }
    return false;
}

static uint32_t exception_block_index(const XiFunc *func, const XiBlock *target) {
    for (uint32_t i = 0; func && i < func->nblocks; i++) {
        if (func->blocks[i] == target)
            return i;
    }
    return UINT32_MAX;
}

static bool exception_handler_has_catch(const XiBlock *handler, const XiValue *registration) {
    uint32_t count = 0;
    for (uint32_t i = 0; handler && i < handler->nvalues; i++) {
        const XiValue *value = handler->values[i];
        if (value && value->op == XI_CATCH && value->aux == registration)
            count++;
    }
    return count == 1;
}

static bool exception_panic_registration_valid(const XiFunc *func, const XiValue *registration) {
    if (!registration || registration->op != XI_TRY || !registration->block || !registration->aux ||
        (registration->aux_int != -1 && registration->aux_int != XI_TRY_AUX_STATIC_CLEANUP))
        return false;
    const XiBlock *handler = (const XiBlock *) registration->aux;
    return exception_block_index(func, registration->block) != UINT32_MAX &&
           exception_block_index(func, handler) != UINT32_MAX &&
           exception_handler_has_catch(handler, registration);
}

static bool exception_panic_region_contains(const XiFunc *func, const XiValue *registration,
                                            const XiValue *point) {
    if (!func || !registration || !registration->block || !point || !point->block)
        return false;
    uint32_t registration_index = registration->block->nvalues;
    for (uint32_t i = 0; i < registration->block->nvalues; i++) {
        if (registration->block->values[i] == registration) {
            registration_index = i;
            break;
        }
    }
    if (registration_index == registration->block->nvalues)
        return false;
    for (uint32_t i = registration_index + 1; i < registration->block->nvalues; i++) {
        const XiValue *value = registration->block->values[i];
        if (value == point)
            return true;
        if (value && value->op == XI_END_TRY && value->aux == registration)
            return false;
    }

    uint8_t *seen = (uint8_t *) xr_calloc(func->nblocks, sizeof(uint8_t));
    XiBlock **work = (XiBlock **) xr_calloc(func->nblocks, sizeof(XiBlock *));
    if (!seen || !work) {
        xr_free(seen);
        xr_free(work);
        return false;
    }
    uint32_t head = 0, tail = 0;
    for (uint32_t s = 0; s < 2; s++) {
        XiBlock *successor = registration->block->succs[s];
        uint32_t index = exception_block_index(func, successor);
        if (successor && index != UINT32_MAX && !seen[index]) {
            seen[index] = 1;
            work[tail++] = successor;
        }
    }
    bool found = false;
    while (head < tail && !found) {
        XiBlock *block = work[head++];
        bool closed = false;
        for (uint32_t i = 0; i < block->nvalues; i++) {
            const XiValue *value = block->values[i];
            if (value == point) {
                found = true;
                break;
            }
            if (value && value->op == XI_END_TRY && value->aux == registration) {
                closed = true;
                break;
            }
        }
        if (found || closed)
            continue;
        for (uint32_t s = 0; s < 2; s++) {
            XiBlock *successor = block->succs[s];
            uint32_t index = exception_block_index(func, successor);
            if (successor && index != UINT32_MAX && !seen[index] && tail < func->nblocks) {
                seen[index] = 1;
                work[tail++] = successor;
            }
        }
    }
    xr_free(seen);
    xr_free(work);
    return found;
}

static bool exception_error_region_valid(const XiFunc *func, const XiErrorRegion *region,
                                         const XiValue *marker) {
    if (!region || !marker || marker->op != XI_ERR_CATCH || marker->error_region != region ||
        region->catch_value != marker || marker->block != region->catch_block ||
        !region->registration_block || !region->body_block || !region->catch_block ||
        !region->merge_block || region->registration_block->succs[0] != region->body_block)
        return false;
    if (exception_block_index(func, region->registration_block) == UINT32_MAX ||
        exception_block_index(func, region->body_block) == UINT32_MAX ||
        exception_block_index(func, region->catch_block) == UINT32_MAX ||
        exception_block_index(func, region->merge_block) == UINT32_MAX)
        return false;
    const XiErrorRegion *cursor = region;
    for (uint32_t depth = 0; cursor; depth++, cursor = cursor->parent) {
        if (depth >= XI_CORO_MAX_EXCEPTION_DEPTH || cursor->parent == cursor ||
            !cursor->catch_value || cursor->catch_value->op != XI_ERR_CATCH ||
            cursor->catch_value->error_region != cursor ||
            cursor->catch_value->block != cursor->catch_block || !cursor->registration_block ||
            !cursor->body_block || !cursor->catch_block || !cursor->merge_block ||
            cursor->registration_block->succs[0] != cursor->body_block ||
            exception_block_index(func, cursor->registration_block) == UINT32_MAX ||
            exception_block_index(func, cursor->body_block) == UINT32_MAX ||
            exception_block_index(func, cursor->catch_block) == UINT32_MAX ||
            exception_block_index(func, cursor->merge_block) == UINT32_MAX)
            return false;
    }
    return true;
}

static bool exception_error_region_contains(const XiFunc *func, const XiErrorRegion *region,
                                            const XiValue *point) {
    uint8_t *seen = (uint8_t *) xr_calloc(func->nblocks, sizeof(uint8_t));
    XiBlock **work = (XiBlock **) xr_calloc(func->nblocks, sizeof(XiBlock *));
    if (!seen || !work) {
        xr_free(seen);
        xr_free(work);
        return false;
    }
    uint32_t body_index = exception_block_index(func, region->body_block);
    if (body_index == UINT32_MAX) {
        xr_free(seen);
        xr_free(work);
        return false;
    }
    uint32_t head = 0, tail = 0;
    seen[body_index] = 1;
    work[tail++] = region->body_block;
    bool found = false;
    while (head < tail && !found) {
        XiBlock *block = work[head++];
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
            uint32_t index = exception_block_index(func, successor);
            if (successor && index != UINT32_MAX && !seen[index] && tail < func->nblocks) {
                seen[index] = 1;
                work[tail++] = successor;
            }
        }
    }
    xr_free(seen);
    xr_free(work);
    return found;
}

static bool exception_region_is_ancestor(const XiErrorRegion *ancestor,
                                         const XiErrorRegion *region) {
    for (uint32_t depth = 0; region && depth < XI_CORO_MAX_EXCEPTION_DEPTH;
         depth++, region = region->parent) {
        if (region == ancestor)
            return true;
    }
    return false;
}

static bool exception_expected_error_region(const XiFunc *func, const XiValue *point,
                                            XiErrorRegion **out_region) {
    XiErrorRegion *selected = NULL;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *block = func->blocks[bi];
        for (uint32_t vi = 0; block && vi < block->nvalues; vi++) {
            XiValue *marker = block->values[vi];
            if (!marker || marker->op != XI_ERR_CATCH || !marker->error_region)
                continue;
            XiErrorRegion *candidate = marker->error_region;
            if (!exception_error_region_valid(func, candidate, marker))
                return false;
            if (!exception_error_region_contains(func, candidate, point))
                continue;
            if (!selected || exception_region_is_ancestor(selected, candidate))
                selected = candidate;
            else if (!exception_region_is_ancestor(candidate, selected))
                return false;
        }
    }
    *out_region = selected;
    return true;
}

static bool exception_list_contains(XiValue *const *values, uint16_t count, const XiValue *target) {
    for (uint16_t i = 0; values && i < count; i++) {
        if (values[i] == target)
            return true;
    }
    return false;
}

static bool exception_verify_handler_set(const XiFunc *func, const XiCoroSuspendPoint *point,
                                         char *error, size_t error_size) {
    uint32_t expected_count = 0;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *block = func->blocks[bi];
        for (uint32_t vi = 0; block && vi < block->nvalues; vi++) {
            const XiValue *registration = block->values[vi];
            if (!registration || registration->op != XI_TRY ||
                !exception_panic_region_contains(func, registration, point->op))
                continue;
            expected_count++;
            if (!exception_panic_registration_valid(func, registration) ||
                !exception_list_contains(point->active_handlers, point->active_handler_count,
                                         registration))
                return exception_error(
                    error, error_size,
                    "XR_CORO_4003 func '%s': state %u has an invalid active panic handler",
                    func->name, point->state_id);
        }
    }
    if (expected_count != point->active_handler_count ||
        (expected_count > 0 && !point->active_handlers))
        return exception_error(error, error_size,
                               "XR_CORO_4003 func '%s': state %u panic handler set is incomplete",
                               func->name, point->state_id);
    for (uint16_t i = 0; i < point->active_handler_count; i++) {
        const XiValue *registration = point->active_handlers[i];
        if (!registration || (i > 0 && point->active_handlers[i - 1]->id >= registration->id))
            return exception_error(
                error, error_size,
                "XR_CORO_4003 func '%s': state %u panic handler order is invalid", func->name,
                point->state_id);
        if (i > 0 &&
            !exception_panic_region_contains(func, point->active_handlers[i - 1], registration))
            return exception_error(error, error_size,
                                   "XR_CORO_4003 func '%s': state %u panic handlers are not nested",
                                   func->name, point->state_id);
    }
    return true;
}

static bool exception_edge_is_terminal(const XiCoroEdge *edge, const XiCoroSuspendPoint *point) {
    return edge && edge->terminal && edge->target_state_id == XI_CORO_STATE_TERMINAL &&
           edge->target_block == NULL && edge->drops == point->drops &&
           edge->ndrops == point->ndrops;
}

static bool exception_verify_edges(const XiFunc *func, const XiCoroSuspendPoint *point, char *error,
                                   size_t error_size) {
    const XiCoroEdge *error_edge = xi_coro_point_find_edge(point, XI_CORO_EDGE_ERROR);
    const XiCoroEdge *panic_edge = xi_coro_point_find_edge(point, XI_CORO_EDGE_PANIC);
    const XiCoroEdge *cancel_edge = xi_coro_point_find_edge(point, XI_CORO_EDGE_CANCEL);
    const XiCoroEdge *drop_edge = xi_coro_point_find_edge(point, XI_CORO_EDGE_DROP);
    bool error_ok = point->error_continuation
                        ? error_edge && !error_edge->terminal &&
                              error_edge->target_state_id == point->state_id &&
                              error_edge->target_block == point->error_continuation &&
                              error_edge->ndrops == 0
                        : exception_edge_is_terminal(error_edge, point);
    XiBlock *panic_target =
        point->active_handler_count > 0
            ? (XiBlock *) point->active_handlers[point->active_handler_count - 1u]->aux
            : NULL;
    bool panic_ok = panic_target
                        ? panic_edge && !panic_edge->terminal &&
                              panic_edge->target_state_id == point->state_id &&
                              panic_edge->target_block == panic_target && panic_edge->ndrops == 0
                        : exception_edge_is_terminal(panic_edge, point);
    if (!error_ok || !panic_ok || !exception_edge_is_terminal(cancel_edge, point) ||
        !exception_edge_is_terminal(drop_edge, point))
        return exception_error(error, error_size,
                               "XR_CORO_4003 func '%s': state %u exception/cancel edge is invalid",
                               func->name, point->state_id);
    return true;
}

XR_FUNC bool xi_coro_exception_verify(const XiFunc *func, const XiCoroPlan *plan, char *error,
                                      size_t error_size) {
    if (!func || !plan)
        return exception_error(error, error_size, "XR_CORO_4000 coroutine exception plan is NULL");
    if ((plan->nstates > 0 && !plan->points) ||
        (plan->active_handler_count > 0 && !plan->active_handlers))
        return exception_error(error, error_size,
                               "XR_CORO_4003 func '%s': coroutine exception tables are incomplete",
                               func->name);
    uint32_t handler_cursor = 0;
    for (uint32_t i = 0; i < plan->nstates; i++) {
        const XiCoroSuspendPoint *point = &plan->points[i];
        XiErrorRegion *expected_error = NULL;
        if (!exception_expected_error_region(func, point->op, &expected_error) ||
            point->error_region != expected_error ||
            point->error_continuation != (expected_error ? expected_error->catch_block : NULL))
            return exception_error(
                error, error_size,
                "XR_CORO_4003 func '%s': state %u error continuation is not derivable", func->name,
                point->state_id);
        if (handler_cursor > plan->active_handler_count ||
            point->active_handler_count > plan->active_handler_count - handler_cursor ||
            (point->active_handler_count > 0 &&
             (!plan->active_handlers ||
              point->active_handlers != &plan->active_handlers[handler_cursor])))
            return exception_error(error, error_size,
                                   "XR_CORO_4003 func '%s': state %u handler slice is invalid",
                                   func->name, point->state_id);
        if (!exception_verify_handler_set(func, point, error, error_size) ||
            !exception_verify_edges(func, point, error, error_size))
            return false;
        handler_cursor += point->active_handler_count;
    }
    if (handler_cursor != plan->active_handler_count ||
        (handler_cursor > 0 && !plan->active_handlers))
        return exception_error(error, error_size,
                               "XR_CORO_4003 func '%s': active handler table is incomplete",
                               func->name);
    if (error && error_size > 0)
        error[0] = '\0';
    return true;
}
