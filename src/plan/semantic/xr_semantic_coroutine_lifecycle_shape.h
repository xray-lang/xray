/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_coroutine_lifecycle_shape.h - Exact first coroutine owner family
 */

#ifndef XR_SEMANTIC_COROUTINE_LIFECYCLE_SHAPE_H
#define XR_SEMANTIC_COROUTINE_LIFECYCLE_SHAPE_H

#include "xr_semantic_cleanup_shape.h"
#include "xr_semantic_graph.h"

#include <stdlib.h>
#include <string.h>

typedef struct XrSemanticCoroutineLifecycleShape {
    uint32_t function;
    uint32_t state_entity;
    uint32_t state_operation;
    uint32_t logical_state;
    uint32_t producer_operation;
    uint32_t producer_value;
    uint32_t release_operation;
    uint32_t owner;
} XrSemanticCoroutineLifecycleShape;

typedef enum XrSemanticCoroutineLifecycleProjectionStatus {
    XR_SEMANTIC_LIFECYCLE_PROJECTION_OK = 0,
    XR_SEMANTIC_LIFECYCLE_PROJECTION_INVALID,
    XR_SEMANTIC_LIFECYCLE_PROJECTION_BUDGET_EXHAUSTED,
    XR_SEMANTIC_LIFECYCLE_PROJECTION_ALLOCATION_FAILED,
} XrSemanticCoroutineLifecycleProjectionStatus;

typedef struct XrSemanticCoroutineLifecycleProjection {
    XrSemanticCoroutineLifecycleShape *rows;
    uint32_t count;
    uint32_t capacity;
    uint64_t indexed_work;
} XrSemanticCoroutineLifecycleProjection;

typedef struct XrSemanticLifecycleStatePoint {
    uint32_t function;
    uint32_t state_entity;
    uint32_t state_operation;
    uint32_t logical_state;
    uint32_t block;
} XrSemanticLifecycleStatePoint;

typedef struct XrSemanticLifecycleStateRangePoint {
    uint32_t function;
    uint32_t dominator_order;
    uint32_t postdominator_order;
    uint32_t state_index;
} XrSemanticLifecycleStateRangePoint;

typedef struct XrSemanticLifecycleStatePostOrder {
    uint32_t postdominator_order;
    uint32_t range_index;
} XrSemanticLifecycleStatePostOrder;

typedef struct XrSemanticLifecycleStateBlockOrder {
    uint32_t block;
    uint32_t state_operation;
    uint32_t state_index;
} XrSemanticLifecycleStateBlockOrder;

static inline void xr_semantic_coroutine_lifecycle_projection_dispose(
    XrSemanticCoroutineLifecycleProjection *projection) {
    if (!projection)
        return;
    xr_free(projection->rows);
    *projection = (XrSemanticCoroutineLifecycleProjection) {0};
}

static inline int xr_semantic_lifecycle_compare_state_function_operation(
    const void *left, const void *right) {
    const XrSemanticLifecycleStatePoint *a =
        (const XrSemanticLifecycleStatePoint *) left;
    const XrSemanticLifecycleStatePoint *b =
        (const XrSemanticLifecycleStatePoint *) right;
    if (a->function != b->function)
        return a->function < b->function ? -1 : 1;
    if (a->state_operation != b->state_operation)
        return a->state_operation < b->state_operation ? -1 : 1;
    if (a->state_entity != b->state_entity)
        return a->state_entity < b->state_entity ? -1 : 1;
    return 0;
}

static inline int xr_semantic_lifecycle_compare_state_range(
    const void *left, const void *right) {
    const XrSemanticLifecycleStateRangePoint *a =
        (const XrSemanticLifecycleStateRangePoint *) left;
    const XrSemanticLifecycleStateRangePoint *b =
        (const XrSemanticLifecycleStateRangePoint *) right;
    if (a->function != b->function)
        return a->function < b->function ? -1 : 1;
    if (a->dominator_order != b->dominator_order)
        return a->dominator_order < b->dominator_order ? -1 : 1;
    if (a->postdominator_order != b->postdominator_order)
        return a->postdominator_order < b->postdominator_order ? -1 : 1;
    if (a->state_index != b->state_index)
        return a->state_index < b->state_index ? -1 : 1;
    return 0;
}

static inline int xr_semantic_lifecycle_compare_state_postorder(
    const void *left, const void *right) {
    const XrSemanticLifecycleStatePostOrder *a =
        (const XrSemanticLifecycleStatePostOrder *) left;
    const XrSemanticLifecycleStatePostOrder *b =
        (const XrSemanticLifecycleStatePostOrder *) right;
    if (a->postdominator_order != b->postdominator_order)
        return a->postdominator_order < b->postdominator_order ? -1 : 1;
    if (a->range_index != b->range_index)
        return a->range_index < b->range_index ? -1 : 1;
    return 0;
}

static inline int xr_semantic_lifecycle_compare_state_block_operation(
    const void *left, const void *right) {
    const XrSemanticLifecycleStateBlockOrder *a =
        (const XrSemanticLifecycleStateBlockOrder *) left;
    const XrSemanticLifecycleStateBlockOrder *b =
        (const XrSemanticLifecycleStateBlockOrder *) right;
    if (a->block != b->block)
        return a->block < b->block ? -1 : 1;
    if (a->state_operation != b->state_operation)
        return a->state_operation < b->state_operation ? -1 : 1;
    if (a->state_index != b->state_index)
        return a->state_index < b->state_index ? -1 : 1;
    return 0;
}

static inline int xr_semantic_lifecycle_compare_projection(
    const void *left, const void *right) {
    const XrSemanticCoroutineLifecycleShape *a =
        (const XrSemanticCoroutineLifecycleShape *) left;
    const XrSemanticCoroutineLifecycleShape *b =
        (const XrSemanticCoroutineLifecycleShape *) right;
    if (a->state_entity != b->state_entity)
        return a->state_entity < b->state_entity ? -1 : 1;
    if (a->producer_operation != b->producer_operation)
        return a->producer_operation < b->producer_operation ? -1 : 1;
    if (a->release_operation != b->release_operation)
        return a->release_operation < b->release_operation ? -1 : 1;
    return 0;
}

static inline bool xr_semantic_lifecycle_tree_intervals(
    const uint32_t *parents, uint32_t count, uint32_t *order,
    uint32_t *order_end) {
    if ((count && (!parents || !order || !order_end)))
        return false;
    uint32_t *child_begin = count
        ? (uint32_t *) xr_calloc((size_t) count + 1u,
                                 sizeof(*child_begin))
        : NULL;
    uint32_t *children = count
        ? (uint32_t *) xr_malloc((size_t) count * sizeof(*children))
        : NULL;
    uint32_t *cursor = count
        ? (uint32_t *) xr_malloc((size_t) count * sizeof(*cursor))
        : NULL;
    uint32_t *node_stack = count
        ? (uint32_t *) xr_malloc((size_t) count * sizeof(*node_stack))
        : NULL;
    uint32_t *child_cursor = count
        ? (uint32_t *) xr_malloc((size_t) count * sizeof(*child_cursor))
        : NULL;
    if (count && (!child_begin || !children || !cursor || !node_stack ||
                  !child_cursor)) {
        xr_free(child_begin);
        xr_free(children);
        xr_free(cursor);
        xr_free(node_stack);
        xr_free(child_cursor);
        return false;
    }
    for (uint32_t node = 0; node < count; node++) {
        order[node] = XR_SEMANTIC_INDEX_NONE;
        order_end[node] = XR_SEMANTIC_INDEX_NONE;
        uint32_t parent = parents[node];
        if (parent != XR_SEMANTIC_INDEX_NONE && parent != node) {
            if (parent >= count) {
                xr_free(child_begin);
                xr_free(children);
                xr_free(cursor);
                xr_free(node_stack);
                xr_free(child_cursor);
                return false;
            }
            child_begin[parent + 1u]++;
        }
    }
    for (uint32_t node = 1; node <= count; node++)
        child_begin[node] += child_begin[node - 1u];
    if (count)
        memcpy(cursor, child_begin, (size_t) count * sizeof(*cursor));
    for (uint32_t node = 0; node < count; node++) {
        uint32_t parent = parents[node];
        if (parent != XR_SEMANTIC_INDEX_NONE && parent != node)
            children[cursor[parent]++] = node;
    }

    uint32_t next_order = 0;
    for (uint32_t root = 0; root < count; root++) {
        if (parents[root] != XR_SEMANTIC_INDEX_NONE && parents[root] != root)
            continue;
        uint32_t depth = 1;
        node_stack[0] = root;
        child_cursor[0] = child_begin[root];
        order[root] = next_order++;
        while (depth) {
            uint32_t node = node_stack[depth - 1u];
            uint32_t next_child = child_cursor[depth - 1u];
            if (next_child < child_begin[node + 1u]) {
                uint32_t child = children[next_child];
                child_cursor[depth - 1u]++;
                if (order[child] != XR_SEMANTIC_INDEX_NONE) {
                    next_order = UINT32_MAX;
                    break;
                }
                order[child] = next_order++;
                node_stack[depth] = child;
                child_cursor[depth++] = child_begin[child];
                continue;
            }
            order_end[node] = next_order;
            depth--;
        }
        if (next_order == UINT32_MAX)
            break;
    }
    bool valid = next_order == count;
    xr_free(child_begin);
    xr_free(children);
    xr_free(cursor);
    xr_free(node_stack);
    xr_free(child_cursor);
    return valid;
}

static inline uint32_t xr_semantic_lifecycle_state_range_lower_bound(
    const XrSemanticLifecycleStateRangePoint *rows, uint32_t count,
    uint32_t function, uint32_t dominator_order) {
    uint32_t low = 0;
    uint32_t high = count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        const XrSemanticLifecycleStateRangePoint *candidate = &rows[middle];
        if (candidate->function < function ||
            (candidate->function == function &&
             candidate->dominator_order < dominator_order))
            low = middle + 1u;
        else
            high = middle;
    }
    return low;
}

static inline uint32_t xr_semantic_lifecycle_node_post_lower_bound(
    const uint32_t *node_states, uint32_t begin, uint32_t end,
    const XrSemanticLifecycleStateRangePoint *ranges,
    uint32_t postdominator_order) {
    uint32_t low = begin;
    uint32_t high = end;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        uint32_t range_index = node_states[middle];
        if (ranges[range_index].postdominator_order < postdominator_order)
            low = middle + 1u;
        else
            high = middle;
    }
    return low;
}

static inline uint32_t xr_semantic_lifecycle_state_block_lower_bound(
    const XrSemanticLifecycleStateBlockOrder *rows, uint32_t count,
    uint32_t block, uint32_t operation) {
    uint32_t low = 0;
    uint32_t high = count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        const XrSemanticLifecycleStateBlockOrder *candidate = &rows[middle];
        if (candidate->block < block ||
            (candidate->block == block &&
             candidate->state_operation < operation))
            low = middle + 1u;
        else
            high = middle;
    }
    return low;
}

static inline bool xr_semantic_lifecycle_projection_append_state(
    const XrSemanticStringConcatReleaseShape *release,
    const XrSemanticLifecycleStatePoint *state,
    XrSemanticCoroutineLifecycleProjection *projection) {
    if (state->function != release->function)
        return true;
    if (projection->count >= projection->capacity)
        return false;
    projection->rows[projection->count++] =
        (XrSemanticCoroutineLifecycleShape) {
            .function = state->function,
            .state_entity = state->state_entity,
            .state_operation = state->state_operation,
            .logical_state = state->logical_state,
            .producer_operation = release->producer_operation,
            .producer_value = release->released_value,
            .release_operation = release->operation,
            .owner = release->owner,
        };
    return true;
}

static inline bool xr_semantic_lifecycle_projection_append_node(
    const XrSemanticPlan *plan,
    const XrSemanticStringConcatReleaseShape *release,
    const XrSemanticLifecycleStatePoint *states,
    const XrSemanticLifecycleStateRangePoint *ranges,
    const uint32_t *node_begin, const uint32_t *node_states, uint32_t node,
    uint32_t postdominator_begin, uint32_t postdominator_end,
    XrSemanticCoroutineLifecycleProjection *projection) {
    uint32_t begin = xr_semantic_lifecycle_node_post_lower_bound(
        node_states, node_begin[node], node_begin[node + 1u], ranges,
        postdominator_begin);
    uint32_t end = xr_semantic_lifecycle_node_post_lower_bound(
        node_states, begin, node_begin[node + 1u], ranges,
        postdominator_end);
    const XrSemanticOperationRecord *producer =
        xr_semantic_plan_operation(plan, release->producer_operation);
    const XrSemanticOperationRecord *release_operation =
        xr_semantic_plan_operation(plan, release->operation);
    for (uint32_t cursor = begin; cursor < end; cursor++) {
        uint32_t range_index = node_states[cursor];
        const XrSemanticLifecycleStatePoint *state =
            &states[ranges[range_index].state_index];
        if (!producer || !release_operation ||
            (producer->block == state->block &&
             release->producer_operation >= state->state_operation) ||
            (release_operation->block == state->block &&
             release->operation <= state->state_operation))
            continue;
        if (!xr_semantic_lifecycle_projection_append_state(
                release, state, projection))
            return false;
    }
    return true;
}

static inline bool xr_semantic_lifecycle_projection_append_block_range(
    const XrSemanticStringConcatReleaseShape *release,
    const XrSemanticLifecycleStatePoint *states,
    const XrSemanticLifecycleStateBlockOrder *block_order, uint32_t begin,
    uint32_t end, XrSemanticCoroutineLifecycleProjection *projection) {
    for (uint32_t cursor = begin; cursor < end; cursor++) {
        if (!xr_semantic_lifecycle_projection_append_state(
                release, &states[block_order[cursor].state_index],
                projection))
            return false;
    }
    return true;
}

/* Build the lifecycle join once from immutable rows.  State points are sorted
 * by function/state operation, indexed by dominator and post-dominator tree
 * intervals, then queried by every exact release.  The resulting rows are
 * sorted by state/producer/release, so consumers never rescan either table. */
static inline XrSemanticCoroutineLifecycleProjectionStatus
xr_semantic_coroutine_lifecycle_projection_build(
    const XrSemanticPlan *plan, const XrSemanticGraph *graph,
    uint32_t output_limit,
    XrSemanticCoroutineLifecycleProjection *projection) {
    if (!projection)
        return XR_SEMANTIC_LIFECYCLE_PROJECTION_INVALID;
    *projection = (XrSemanticCoroutineLifecycleProjection) {0};
    uint32_t block_count =
        plan ? (uint32_t) xr_semantic_plan_block_count(plan) : 0;
    uint32_t entity_count =
        plan ? (uint32_t) xr_semantic_plan_entity_count(plan) : 0;
    uint32_t operation_count =
        plan ? (uint32_t) xr_semantic_plan_operation_count(plan) : 0;
    if (!plan || !graph || graph->block_count != block_count)
        return XR_SEMANTIC_LIFECYCLE_PROJECTION_INVALID;

    uint64_t work = 0;
    if (!xr_semantic_lifecycle_work_charge(&work, entity_count) ||
        !xr_semantic_lifecycle_work_charge_product(
            &work, block_count, 12u) ||
        !xr_semantic_lifecycle_work_charge(&work, output_limit))
        return XR_SEMANTIC_LIFECYCLE_PROJECTION_BUDGET_EXHAUSTED;
    uint32_t state_count = 0;
    for (uint32_t entity = 0; entity < entity_count; entity++) {
        const XrSemanticEntityRecord *record =
            xr_semantic_plan_entity(plan, entity);
        if (!record)
            return XR_SEMANTIC_LIFECYCLE_PROJECTION_INVALID;
        if (record->kind != XR_SEM_ENTITY_COROUTINE_STATE)
            continue;
        if (record->subject_kind != XR_SEM_ENTITY_SUBJECT_OPERATION ||
            record->subject >= operation_count || record->ordinal == 0)
            return XR_SEMANTIC_LIFECYCLE_PROJECTION_INVALID;
        if (state_count == UINT32_MAX)
            return XR_SEMANTIC_LIFECYCLE_PROJECTION_BUDGET_EXHAUSTED;
        state_count++;
    }
    if (state_count == 0) {
        projection->indexed_work = work;
        return XR_SEMANTIC_LIFECYCLE_PROJECTION_OK;
    }

    uint32_t sort_height = xr_semantic_lifecycle_sort_height(state_count);
    uint32_t tree_height = sort_height + 1u;
    uint64_t node_entry_count = (uint64_t) state_count * tree_height;
    uint32_t tree_base = 1u;
    while (tree_base < state_count) {
        if (tree_base > (UINT32_MAX - 1u) / 2u)
            return XR_SEMANTIC_LIFECYCLE_PROJECTION_BUDGET_EXHAUSTED;
        tree_base *= 2u;
    }
    uint32_t node_count = tree_base * 2u;
    if (node_entry_count > UINT32_MAX ||
        !xr_semantic_lifecycle_work_charge_product(
            &work, state_count, (uint64_t) sort_height * 3u + 5u) ||
        !xr_semantic_lifecycle_work_charge(&work, node_entry_count * 2u) ||
        !xr_semantic_lifecycle_work_charge_product(
            &work, output_limit,
            xr_semantic_lifecycle_sort_height(output_limit) + 2u))
        return XR_SEMANTIC_LIFECYCLE_PROJECTION_BUDGET_EXHAUSTED;

    XrSemanticStringConcatReleaseIndex releases = {0};
    XrSemanticStringConcatReleaseIndexStatus release_status =
        xr_semantic_string_concat_release_index_build(plan, &releases);
    if (release_status != XR_SEMANTIC_RELEASE_INDEX_OK)
        return release_status == XR_SEMANTIC_RELEASE_INDEX_INVALID
                   ? XR_SEMANTIC_LIFECYCLE_PROJECTION_INVALID
                   : release_status ==
                             XR_SEMANTIC_RELEASE_INDEX_BUDGET_EXHAUSTED
                         ? XR_SEMANTIC_LIFECYCLE_PROJECTION_BUDGET_EXHAUSTED
                         : XR_SEMANTIC_LIFECYCLE_PROJECTION_ALLOCATION_FAILED;
    uint64_t query_height = (uint64_t) tree_height * tree_height;
    if (!xr_semantic_lifecycle_work_charge(&work, releases.linear_work) ||
        !xr_semantic_lifecycle_work_charge_product(
            &work, releases.count, query_height)) {
        xr_semantic_string_concat_release_index_dispose(&releases);
        return XR_SEMANTIC_LIFECYCLE_PROJECTION_BUDGET_EXHAUSTED;
    }
    if (releases.count == 0) {
        projection->indexed_work = work;
        xr_semantic_string_concat_release_index_dispose(&releases);
        return XR_SEMANTIC_LIFECYCLE_PROJECTION_OK;
    }

    XrSemanticLifecycleStatePoint *states =
        (XrSemanticLifecycleStatePoint *) xr_malloc(
            (size_t) state_count * sizeof(*states));
    XrSemanticLifecycleStateRangePoint *ranges =
        (XrSemanticLifecycleStateRangePoint *) xr_malloc(
            (size_t) state_count * sizeof(*ranges));
    XrSemanticLifecycleStatePostOrder *postorder =
        (XrSemanticLifecycleStatePostOrder *) xr_malloc(
            (size_t) state_count * sizeof(*postorder));
    XrSemanticLifecycleStateBlockOrder *block_order =
        (XrSemanticLifecycleStateBlockOrder *) xr_malloc(
            (size_t) state_count * sizeof(*block_order));
    uint32_t *dominator_order = (uint32_t *) xr_malloc(
        (size_t) block_count * sizeof(*dominator_order));
    uint32_t *dominator_end = (uint32_t *) xr_malloc(
        (size_t) block_count * sizeof(*dominator_end));
    uint32_t *postdominator_order = (uint32_t *) xr_malloc(
        (size_t) block_count * sizeof(*postdominator_order));
    uint32_t *postdominator_end = (uint32_t *) xr_malloc(
        (size_t) block_count * sizeof(*postdominator_end));
    uint32_t *node_begin = (uint32_t *) xr_calloc(
        (size_t) node_count + 1u, sizeof(*node_begin));
    uint32_t *node_cursor = (uint32_t *) xr_malloc(
        (size_t) node_count * sizeof(*node_cursor));
    uint32_t *node_states = (uint32_t *) xr_malloc(
        (size_t) node_entry_count * sizeof(*node_states));
    projection->rows = output_limit
        ? (XrSemanticCoroutineLifecycleShape *) xr_malloc(
              (size_t) output_limit * sizeof(*projection->rows))
        : NULL;
    projection->capacity = output_limit;
    bool allocated = states && ranges && postorder && block_order &&
                     dominator_order &&
                     dominator_end && postdominator_order &&
                     postdominator_end && node_begin && node_cursor &&
                     node_states && (output_limit == 0 || projection->rows);
    if (!allocated) {
        xr_free(states);
        xr_free(ranges);
        xr_free(postorder);
        xr_free(block_order);
        xr_free(dominator_order);
        xr_free(dominator_end);
        xr_free(postdominator_order);
        xr_free(postdominator_end);
        xr_free(node_begin);
        xr_free(node_cursor);
        xr_free(node_states);
        xr_semantic_string_concat_release_index_dispose(&releases);
        xr_semantic_coroutine_lifecycle_projection_dispose(projection);
        return XR_SEMANTIC_LIFECYCLE_PROJECTION_ALLOCATION_FAILED;
    }
    bool valid = xr_semantic_lifecycle_tree_intervals(
                     graph->immediate_dominator, block_count,
                     dominator_order, dominator_end) &&
                 xr_semantic_lifecycle_tree_intervals(
                     graph->immediate_postdominator, block_count,
                     postdominator_order, postdominator_end);
    uint32_t next_state = 0;
    for (uint32_t entity = 0; valid && entity < entity_count; entity++) {
        const XrSemanticEntityRecord *record =
            xr_semantic_plan_entity(plan, entity);
        if (!record) {
            valid = false;
            break;
        }
        if (record->kind != XR_SEM_ENTITY_COROUTINE_STATE)
            continue;
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(plan, record->subject);
        if (!operation || operation->block >= block_count) {
            valid = false;
            break;
        }
        states[next_state++] = (XrSemanticLifecycleStatePoint) {
            .function = operation->function,
            .state_entity = entity,
            .state_operation = record->subject,
            .logical_state = record->ordinal,
            .block = operation->block,
        };
    }
    qsort(states, state_count, sizeof(*states),
          xr_semantic_lifecycle_compare_state_function_operation);
    for (uint32_t state = 0; valid && state < state_count; state++) {
        uint32_t block = states[state].block;
        if (dominator_order[block] == XR_SEMANTIC_INDEX_NONE ||
            postdominator_order[block] == XR_SEMANTIC_INDEX_NONE) {
            valid = false;
            break;
        }
        ranges[state] = (XrSemanticLifecycleStateRangePoint) {
            .function = states[state].function,
            .dominator_order = dominator_order[block],
            .postdominator_order = postdominator_order[block],
            .state_index = state,
        };
        block_order[state] = (XrSemanticLifecycleStateBlockOrder) {
            .block = states[state].block,
            .state_operation = states[state].state_operation,
            .state_index = state,
        };
    }
    qsort(block_order, state_count, sizeof(*block_order),
          xr_semantic_lifecycle_compare_state_block_operation);
    qsort(ranges, state_count, sizeof(*ranges),
          xr_semantic_lifecycle_compare_state_range);
    for (uint32_t range = 0; range < state_count; range++) {
        postorder[range] = (XrSemanticLifecycleStatePostOrder) {
            .postdominator_order = ranges[range].postdominator_order,
            .range_index = range,
        };
        for (uint32_t node = tree_base + range; node; node /= 2u)
            node_begin[node + 1u]++;
    }
    for (uint32_t node = 1; node <= node_count; node++)
        node_begin[node] += node_begin[node - 1u];
    memcpy(node_cursor, node_begin,
           (size_t) node_count * sizeof(*node_cursor));
    qsort(postorder, state_count, sizeof(*postorder),
          xr_semantic_lifecycle_compare_state_postorder);
    for (uint32_t i = 0; i < state_count; i++) {
        uint32_t range = postorder[i].range_index;
        for (uint32_t node = tree_base + range; node; node /= 2u)
            node_states[node_cursor[node]++] = range;
    }

    for (uint32_t release_index = 0;
         valid && release_index < releases.count; release_index++) {
        const XrSemanticStringConcatReleaseShape *release =
            &releases.rows[release_index];
        const XrSemanticOperationRecord *producer =
            xr_semantic_plan_operation(plan, release->producer_operation);
        const XrSemanticOperationRecord *release_operation =
            xr_semantic_plan_operation(plan, release->operation);
        if (!producer || !release_operation ||
            producer->block >= block_count ||
            release_operation->block >= block_count) {
            valid = false;
            break;
        }
        uint32_t producer_block_begin =
            xr_semantic_lifecycle_state_block_lower_bound(
                block_order, state_count, producer->block,
                release->producer_operation + 1u);
        uint32_t producer_block_end =
            xr_semantic_lifecycle_state_block_lower_bound(
                block_order, state_count, producer->block,
                producer->block == release_operation->block
                    ? release->operation
                    : UINT32_MAX);
        bool producer_block_postdominated =
            postdominator_order[release_operation->block] <=
                postdominator_order[producer->block] &&
            postdominator_order[producer->block] <
                postdominator_end[release_operation->block];
        if (producer_block_postdominated) {
            valid = xr_semantic_lifecycle_projection_append_block_range(
                release, states, block_order, producer_block_begin,
                producer_block_end, projection);
        }
        if (valid && producer->block != release_operation->block) {
            bool release_block_dominated =
                dominator_order[producer->block] <=
                    dominator_order[release_operation->block] &&
                dominator_order[release_operation->block] <
                    dominator_end[producer->block];
            if (release_block_dominated) {
                uint32_t release_block_begin =
                    xr_semantic_lifecycle_state_block_lower_bound(
                        block_order, state_count, release_operation->block, 0);
                uint32_t release_block_end =
                    xr_semantic_lifecycle_state_block_lower_bound(
                        block_order, state_count, release_operation->block,
                        release->operation);
                valid = xr_semantic_lifecycle_projection_append_block_range(
                    release, states, block_order, release_block_begin,
                    release_block_end, projection);
            }
        }
        uint32_t low = xr_semantic_lifecycle_state_range_lower_bound(
            ranges, state_count, release->function,
            dominator_order[producer->block] + 1u);
        uint32_t high = xr_semantic_lifecycle_state_range_lower_bound(
            ranges, state_count, release->function,
            dominator_end[producer->block]);
        uint32_t left = tree_base + low;
        uint32_t right = tree_base + high;
        while (valid && left < right) {
            if (left & 1u) {
                valid = xr_semantic_lifecycle_projection_append_node(
                    plan, release, states, ranges, node_begin, node_states,
                    left++,
                    postdominator_order[release_operation->block] + 1u,
                    postdominator_end[release_operation->block], projection);
            }
            if (valid && (right & 1u)) {
                right--;
                valid = xr_semantic_lifecycle_projection_append_node(
                    plan, release, states, ranges, node_begin, node_states,
                    right,
                    postdominator_order[release_operation->block] + 1u,
                    postdominator_end[release_operation->block], projection);
            }
            left /= 2u;
            right /= 2u;
        }
    }
    if (valid) {
        qsort(projection->rows, projection->count, sizeof(*projection->rows),
              xr_semantic_lifecycle_compare_projection);
        for (uint32_t i = 1; valid && i < projection->count; i++) {
            const XrSemanticCoroutineLifecycleShape *previous =
                &projection->rows[i - 1u];
            const XrSemanticCoroutineLifecycleShape *current =
                &projection->rows[i];
            if (previous->state_entity == current->state_entity &&
                previous->producer_operation ==
                    current->producer_operation)
                valid = false;
        }
    }
    projection->indexed_work = work;
    xr_free(states);
    xr_free(ranges);
    xr_free(postorder);
    xr_free(block_order);
    xr_free(dominator_order);
    xr_free(dominator_end);
    xr_free(postdominator_order);
    xr_free(postdominator_end);
    xr_free(node_begin);
    xr_free(node_cursor);
    xr_free(node_states);
    xr_semantic_string_concat_release_index_dispose(&releases);
    if (!valid) {
        xr_semantic_coroutine_lifecycle_projection_dispose(projection);
        return XR_SEMANTIC_LIFECYCLE_PROJECTION_INVALID;
    }
    return XR_SEMANTIC_LIFECYCLE_PROJECTION_OK;
}

/* Which functions own a coroutine state.
 *
 * The plan records one COROUTINE_STATE entity per suspending operation, so a
 * function is a coroutine exactly when such an entity names an operation that
 * function holds.  The target builder and the target verifier both need this
 * seed before they assign storage, and each carried its own copy of the walk:
 * the same loop over the same entities with the same two range checks,
 * differing only in whether a malformed identity leaves as a diagnostic or as
 * a bare false.  A fixpoint whose seed is written twice is how two layers come
 * to disagree about which function is a coroutine, so the walk lives here once
 * and each layer keeps only its own vocabulary for reporting a broken plan. */
typedef enum XrSemanticCoroutineStateMarkStatus {
    XR_SEMANTIC_COROUTINE_STATE_MARK_OK = 0,
    XR_SEMANTIC_COROUTINE_STATE_MARK_OPERATION_OUT_OF_RANGE,
    XR_SEMANTIC_COROUTINE_STATE_MARK_FUNCTION_OUT_OF_RANGE,
} XrSemanticCoroutineStateMarkStatus;

static inline XrSemanticCoroutineStateMarkStatus
xr_semantic_mark_coroutine_state_functions(const XrSemanticPlan *plan, uint8_t *deferred,
                                           uint32_t function_count) {
    size_t entity_count = xr_semantic_plan_entity_count(plan);
    size_t operation_count = xr_semantic_plan_operation_count(plan);
    for (size_t i = 0; i < entity_count; i++) {
        const XrSemanticEntityRecord *entity = xr_semantic_plan_entity(plan, i);
        if (!entity || entity->kind != XR_SEM_ENTITY_COROUTINE_STATE ||
            entity->subject_kind != XR_SEM_ENTITY_SUBJECT_OPERATION)
            continue;
        if (entity->subject >= operation_count)
            return XR_SEMANTIC_COROUTINE_STATE_MARK_OPERATION_OUT_OF_RANGE;
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(plan, entity->subject);
        if (!operation || operation->function >= function_count)
            return XR_SEMANTIC_COROUTINE_STATE_MARK_FUNCTION_OUT_OF_RANGE;
        deferred[operation->function] = 1;
    }
    return XR_SEMANTIC_COROUTINE_STATE_MARK_OK;
}

#endif  // XR_SEMANTIC_COROUTINE_LIFECYCLE_SHAPE_H
