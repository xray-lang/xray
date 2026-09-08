/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xi_cleanup_clone.c - Cleanup identity preservation through real Xi passes
 *
 * KEY CONCEPT:
 *   Each emitted clone owns its complete frontier. Optional transformations
 *   that would split a frontier leave the original graph unchanged.
 */

#include "../test_framework.h"
#include "ir/xi_analysis.h"
#include "ir/xi_cleanup.h"
#include "ir/xi_cfg_edit.h"
#include "ir/xi_opt_inline.h"
#include "ir/xi_opt_loop_inv_branch.h"
#include "ir/xi_opt_loop_unroll.h"
#include "ir/xi_verify.h"
#include "runtime/value/xtype.h"

static XrType clone_int = {.kind = XR_KIND_INT, .id = 1u, .frozen = true};
static XrType clone_bool = {.kind = XR_KIND_BOOL, .id = 2u, .frozen = true};
static XrType clone_unit = {.kind = XR_KIND_UNIT, .id = 3u, .frozen = true};
static XrType clone_function = {.kind = XR_KIND_FUNCTION, .id = 4u, .frozen = true};

typedef struct ClonePair {
    XiValue *enter;
    XiValue *leave;
} ClonePair;

typedef struct CloneLoop {
    XiFunc *function;
    XiBlock *entry;
    XiBlock *header;
    XiBlock *branch;
    XiBlock *then_block;
    XiBlock *else_block;
    XiBlock *latch;
    XiBlock *exit_block;
} CloneLoop;

static XiFunc *clone_test_function(const char *name) {
    XiFunc *function = xi_func_new(name, &clone_int);
    if (!function)
        return NULL;
    XiBlock *entry = xi_block_new(function);
    if (!entry) {
        xi_func_free(function);
        return NULL;
    }
    entry->sealed = true;
    return function;
}

static ClonePair clone_test_pair(XiFunc *function, XiBlock *block) {
    ClonePair pair = {0};
    pair.enter = xi_value_new(function, block, XI_CLEANUP_ENTER, &clone_unit, 0u);
    pair.leave = xi_value_new(function, block, XI_CLEANUP_LEAVE, &clone_unit, 0u);
    if (pair.enter)
        pair.enter->flags |= XI_FLAG_SIDE_EFFECT;
    if (pair.leave)
        pair.leave->flags |= XI_FLAG_SIDE_EFFECT;
    return pair;
}

static bool clone_test_frontier(XiFunc *function, const ClonePair *pairs, uint32_t count) {
    char error[192];
    for (uint32_t remaining = count; remaining > 0u; --remaining) {
        uint32_t index = remaining - 1u;
        XiCleanupBoundary boundary = {
            .enter = pairs[index].enter,
            .leave = pairs[index].leave,
            .remaining = index + 1u < count ? pairs[index + 1u].enter : NULL,
            .frontier = pairs[0].enter,
            .rank = count - index,
            .kind = XI_CLEANUP_BOUNDARY_CLOSED,
        };
        if (!xi_cleanup_boundary_attach(function, &boundary, error, sizeof(error)))
            return false;
    }
    return xi_cleanup_verify(function, error, sizeof(error));
}

static bool clone_test_verify(XiFunc *function) {
    char error[256] = {0};
    xi_cfg_invalidate(function);
    bool valid = xi_verify(function, error, sizeof(error)) &&
                 xi_cleanup_verify(function, error, sizeof(error));
    if (!valid)
        printf("  Xi verification: %s\n", error);
    return valid;
}

/* Check independently counted occurrences as well as the verifier result.
 * Old arena records can remain allocated after the original blocks retire;
 * no emitted ENTER may keep one of those records. */
static bool clone_test_inventory(XiFunc *function, uint32_t expected_heads, uint32_t expected_pairs,
                                 const XiCleanupBoundary *old_first,
                                 const XiCleanupBoundary *old_second) {
    char error[192];
    if (!xi_cleanup_verify(function, error, sizeof(error)))
        return false;
    uint32_t heads = 0u;
    uint32_t pairs = 0u;
    for (uint32_t bi = 0u; bi < function->nblocks; ++bi) {
        const XiBlock *block = function->blocks[bi];
        for (uint32_t vi = 0u; vi < block->nvalues; ++vi) {
            const XiValue *value = block->values[vi];
            if (!value || value->op != XI_CLEANUP_ENTER)
                continue;
            const XiCleanupBoundary *row = value->cleanup_boundary;
            if (!row || row == old_first || row == old_second || row->enter != value)
                return false;
            ++pairs;
            heads += row->frontier == value;
        }
    }
    return heads == expected_heads && pairs == expected_pairs;
}

static uint16_t clone_test_pred_index(const XiBlock *block, const XiBlock *predecessor) {
    for (uint16_t index = 0u; index < block->npreds; ++index) {
        if (block->preds[index] == predecessor)
            return index;
    }
    return UINT16_MAX;
}

static bool clone_test_loop_edges(CloneLoop *loop, bool branched) {
    XiFunc *function = loop->function;
    XiValue *start = xi_const_int(function, loop->entry, 0, &clone_int);
    XiValue *limit = xi_const_int(function, loop->entry, 3, &clone_int);
    XiValue *step = xi_const_int(function, loop->entry, 1, &clone_int);
    XiValue *invariant = xi_const_bool(function, loop->entry, true, &clone_bool);
    if (!start || !limit || !step || !invariant)
        return false;
    xi_block_set_jump(loop->entry, loop->header);
    xi_block_set_jump(loop->latch, loop->header);
    XiPhi *iv = xi_phi_new(function, loop->header, &clone_int, loop->header->npreds);
    if (!iv)
        return false;
    XiValue *condition = xi_binary(function, loop->header, XI_LT, &clone_bool, &iv->value, limit);
    XiValue *next = xi_binary(function, loop->latch, XI_ADD, &clone_int, &iv->value, step);
    if (!condition || !next)
        return false;
    xi_block_set_if(loop->header, condition, branched ? loop->branch : loop->latch,
                    loop->exit_block);
    if (branched) {
        xi_block_set_if(loop->branch, invariant, loop->then_block, loop->else_block);
        xi_block_set_jump(loop->then_block, loop->latch);
        xi_block_set_jump(loop->else_block, loop->latch);
    }
    uint16_t initial = clone_test_pred_index(loop->header, loop->entry);
    uint16_t backedge = clone_test_pred_index(loop->header, loop->latch);
    if (initial >= iv->value.nargs || backedge >= iv->value.nargs)
        return false;
    iv->value.args[initial] = start;
    iv->value.args[backedge] = next;
    XiPhi *result = xi_phi_new(function, loop->exit_block, &clone_int, loop->exit_block->npreds);
    if (!result)
        return false;
    uint16_t exit_index = clone_test_pred_index(loop->exit_block, loop->header);
    if (exit_index >= result->value.nargs)
        return false;
    result->value.args[exit_index] = &iv->value;
    xi_block_set_return(loop->exit_block, &result->value);
    return true;
}

static CloneLoop clone_test_loop(bool branched) {
    CloneLoop loop = {0};
    loop.function = clone_test_function("cleanup_loop_clone");
    if (!loop.function)
        return loop;
    loop.entry = loop.function->entry;
    loop.header = xi_block_new(loop.function);
    if (branched) {
        loop.branch = xi_block_new(loop.function);
        loop.then_block = xi_block_new(loop.function);
        loop.else_block = xi_block_new(loop.function);
    }
    loop.latch = xi_block_new(loop.function);
    loop.exit_block = xi_block_new(loop.function);
    if (!loop.header || !loop.latch || !loop.exit_block ||
        (branched && (!loop.branch || !loop.then_block || !loop.else_block)) ||
        !clone_test_loop_edges(&loop, branched)) {
        xi_func_free(loop.function);
        return (CloneLoop) {0};
    }
    for (uint32_t bi = 0u; bi < loop.function->nblocks; ++bi)
        loop.function->blocks[bi]->sealed = true;
    return loop;
}

static bool clone_test_split_linear_body(CloneLoop *loop) {
    XiBlock *body = xi_block_new(loop->function);
    if (!body || !xi_cfg_redirect_edge(loop->header, loop->latch, body, NULL, 0u))
        return false;
    xi_block_set_jump(body, loop->latch);
    body->sealed = true;
    loop->branch = body;
    return true;
}

static bool clone_test_exit_iv_is_three(const CloneLoop *loop) {
    const XiPhi *result = loop->exit_block->phis;
    if (loop->exit_block->npreds != 1u || !result || result->value.nargs != 1u ||
        loop->exit_block->control != &result->value)
        return false;
    const XiValue *latch_value = result->value.args[0];
    /* The three-iteration loop exits with its final latch value 2 + 1,
     * not the last body's entry value 2. No executor provides this oracle. */
    return latch_value && latch_value->op == XI_ADD && latch_value->nargs == 2u &&
           latch_value->args[0] && latch_value->args[0]->op == XI_CONST &&
           latch_value->args[0]->aux_int == 2 && latch_value->args[1] &&
           latch_value->args[1]->op == XI_CONST && latch_value->args[1]->aux_int == 1;
}

TEST(inline_remaps_forward_frontier_and_owns_callee_metadata) {
    XiFunc *callee = clone_test_function("cleanup_callee");
    XiFunc *caller = clone_test_function("cleanup_caller");
    ASSERT(callee && caller);
    ClonePair pairs[2];
    pairs[0] = clone_test_pair(callee, callee->entry);
    pairs[1] = clone_test_pair(callee, callee->entry);
    ASSERT(clone_test_frontier(callee, pairs, 2u));
    XiValue *result = xi_const_int(callee, callee->entry, 42, &clone_int);
    ASSERT(result);
    xi_block_set_return(callee->entry, result);
    XiValue *closure = xi_value_new(caller, caller->entry, XI_CLOSURE_NEW, &clone_function, 0u);
    XiValue *call = xi_value_new(caller, caller->entry, XI_CALL, &clone_int, 1u);
    ASSERT(closure && call);
    closure->aux = callee;
    call->args[0] = closure;
    xi_block_set_return(caller->entry, call);
    ASSERT(clone_test_verify(callee));
    ASSERT(clone_test_verify(caller));
    const XiCleanupBoundary *old_first = pairs[0].enter->cleanup_boundary;
    const XiCleanupBoundary *old_second = pairs[1].enter->cleanup_boundary;
    XiPassChange change = xi_opt_inline(caller);
    bool valid = change.cfg_changed && change.values_changed && clone_test_verify(caller) &&
                 clone_test_inventory(caller, 1u, 2u, old_first, old_second);
    xi_func_free(callee);
    /* Only cleanup records are inspected after the external callee is freed. */
    char error[192];
    valid = xi_cleanup_verify(caller, error, sizeof(error)) && valid;
    xi_func_free(caller);
    ASSERT(valid);
}

TEST(unroll_gives_every_iteration_an_independent_complete_frontier) {
    CloneLoop loop = clone_test_loop(false);
    ASSERT(loop.function);
    ClonePair pairs[2];
    pairs[0] = clone_test_pair(loop.function, loop.latch);
    pairs[1] = clone_test_pair(loop.function, loop.latch);
    ASSERT(clone_test_frontier(loop.function, pairs, 2u));
    ASSERT(clone_test_verify(loop.function));
    const XiCleanupBoundary *old_first = pairs[0].enter->cleanup_boundary;
    const XiCleanupBoundary *old_second = pairs[1].enter->cleanup_boundary;
    XiPassChange change = xi_opt_loop_unroll(loop.function);
    bool valid = change.cfg_changed && change.values_changed && clone_test_verify(loop.function) &&
                 clone_test_inventory(loop.function, 3u, 6u, old_first, old_second) &&
                 clone_test_exit_iv_is_three(&loop);
    xi_func_free(loop.function);
    ASSERT(valid);
}

TEST(unroll_without_cleanup_preserves_exit_phi_edge_and_final_latch_value) {
    CloneLoop loop = clone_test_loop(false);
    ASSERT(loop.function);
    ASSERT(clone_test_verify(loop.function));
    XiPassChange change = xi_opt_loop_unroll(loop.function);
    bool valid = change.cfg_changed && change.values_changed && clone_test_verify(loop.function) &&
                 clone_test_inventory(loop.function, 0u, 0u, NULL, NULL) &&
                 clone_test_exit_iv_is_three(&loop);
    xi_func_free(loop.function);
    ASSERT(valid);
}

TEST(unroll_flattens_a_cross_block_frontier_in_cfg_order) {
    CloneLoop loop = clone_test_loop(false);
    ASSERT(loop.function);
    ASSERT(clone_test_split_linear_body(&loop));
    ClonePair pairs[2];
    pairs[0] = clone_test_pair(loop.function, loop.branch);
    pairs[1] = clone_test_pair(loop.function, loop.latch);
    ASSERT(clone_test_frontier(loop.function, pairs, 2u));
    ASSERT(clone_test_verify(loop.function));
    const XiCleanupBoundary *old_first = pairs[0].enter->cleanup_boundary;
    const XiCleanupBoundary *old_second = pairs[1].enter->cleanup_boundary;
    XiPassChange change = xi_opt_loop_unroll(loop.function);
    bool valid = change.cfg_changed && change.values_changed && clone_test_verify(loop.function) &&
                 clone_test_inventory(loop.function, 3u, 6u, old_first, old_second) &&
                 clone_test_exit_iv_is_three(&loop);
    xi_func_free(loop.function);
    ASSERT(valid);
}

TEST(unroll_rejects_cross_block_frontier_order_before_any_clone) {
    CloneLoop loop = clone_test_loop(false);
    ASSERT(loop.function);
    ASSERT(clone_test_split_linear_body(&loop));
    ClonePair pairs[2];
    pairs[0] = clone_test_pair(loop.function, loop.latch);
    pairs[1] = clone_test_pair(loop.function, loop.branch);
    /* Identity verification does not infer order between different blocks.
     * Flattening must establish its stronger same-block order before editing. */
    ASSERT(clone_test_frontier(loop.function, pairs, 2u));
    ASSERT(clone_test_verify(loop.function));
    uint32_t block_count = loop.function->nblocks;
    uint32_t next_value = loop.function->next_value_id;
    const XiCleanupBoundary *old_record = pairs[0].enter->cleanup_boundary;
    XiPassChange change = xi_opt_loop_unroll(loop.function);
    bool unchanged = !change.cfg_changed && !change.values_changed &&
                     loop.function->nblocks == block_count &&
                     loop.function->next_value_id == next_value &&
                     pairs[0].enter->cleanup_boundary == old_record &&
                     loop.header->succs[0] == loop.branch && clone_test_verify(loop.function);
    xi_func_free(loop.function);
    ASSERT(unchanged);
}

TEST(unroll_rejects_a_frontier_outside_the_iteration_without_editing) {
    CloneLoop loop = clone_test_loop(false);
    ASSERT(loop.function);
    ClonePair pairs[2];
    pairs[0] = clone_test_pair(loop.function, loop.latch);
    pairs[1] = clone_test_pair(loop.function, loop.exit_block);
    ASSERT(clone_test_frontier(loop.function, pairs, 2u));
    ASSERT(clone_test_verify(loop.function));
    uint32_t block_count = loop.function->nblocks;
    uint32_t next_value = loop.function->next_value_id;
    const XiCleanupBoundary *old_record = pairs[0].enter->cleanup_boundary;
    XiPassChange change = xi_opt_loop_unroll(loop.function);
    bool unchanged =
        !change.cfg_changed && !change.values_changed && loop.function->nblocks == block_count &&
        loop.function->next_value_id == next_value && loop.entry->succs[0] == loop.header &&
        pairs[0].enter->cleanup_boundary == old_record && clone_test_verify(loop.function);
    xi_func_free(loop.function);
    ASSERT(unchanged);
}

TEST(unroll_keeps_header_cleanup_with_its_distinct_execution_count) {
    CloneLoop loop = clone_test_loop(false);
    ASSERT(loop.function);
    ClonePair pair = clone_test_pair(loop.function, loop.header);
    ASSERT(clone_test_frontier(loop.function, &pair, 1u));
    ASSERT(clone_test_verify(loop.function));
    uint32_t block_count = loop.function->nblocks;
    uint32_t next_value = loop.function->next_value_id;
    XiPassChange change = xi_opt_loop_unroll(loop.function);
    bool unchanged = !change.cfg_changed && !change.values_changed &&
                     loop.function->nblocks == block_count &&
                     loop.function->next_value_id == next_value &&
                     loop.entry->succs[0] == loop.header && clone_test_verify(loop.function);
    xi_func_free(loop.function);
    ASSERT(unchanged);
}

TEST(unswitch_remaps_cross_block_frontiers_in_both_versions) {
    CloneLoop loop = clone_test_loop(true);
    ASSERT(loop.function);
    ClonePair pairs[2];
    pairs[0] = clone_test_pair(loop.function, loop.branch);
    pairs[1] = clone_test_pair(loop.function, loop.latch);
    ASSERT(clone_test_frontier(loop.function, pairs, 2u));
    ASSERT(clone_test_verify(loop.function));
    const XiCleanupBoundary *old_first = pairs[0].enter->cleanup_boundary;
    const XiCleanupBoundary *old_second = pairs[1].enter->cleanup_boundary;
    XiPassChange change = xi_opt_loop_inv_branch(loop.function);
    bool valid = change.cfg_changed && change.values_changed && clone_test_verify(loop.function) &&
                 clone_test_inventory(loop.function, 2u, 4u, old_first, old_second);
    xi_func_free(loop.function);
    ASSERT(valid);
}

TEST(unswitch_can_omit_a_whole_untaken_frontier) {
    CloneLoop loop = clone_test_loop(true);
    ASSERT(loop.function);
    ClonePair first = clone_test_pair(loop.function, loop.then_block);
    ASSERT(clone_test_frontier(loop.function, &first, 1u));
    ClonePair second = clone_test_pair(loop.function, loop.else_block);
    ASSERT(clone_test_frontier(loop.function, &second, 1u));
    ASSERT(clone_test_verify(loop.function));
    const XiCleanupBoundary *old_first = first.enter->cleanup_boundary;
    const XiCleanupBoundary *old_second = second.enter->cleanup_boundary;
    XiPassChange change = xi_opt_loop_inv_branch(loop.function);
    bool valid = change.cfg_changed && change.values_changed && clone_test_verify(loop.function) &&
                 clone_test_inventory(loop.function, 2u, 2u, old_first, old_second);
    xi_func_free(loop.function);
    ASSERT(valid);
}

TEST(unswitch_rejects_a_partial_specialized_frontier_without_editing) {
    CloneLoop loop = clone_test_loop(true);
    ASSERT(loop.function);
    ClonePair pairs[2];
    pairs[0] = clone_test_pair(loop.function, loop.then_block);
    pairs[1] = clone_test_pair(loop.function, loop.latch);
    ASSERT(clone_test_frontier(loop.function, pairs, 2u));
    ASSERT(clone_test_verify(loop.function));
    uint32_t block_count = loop.function->nblocks;
    uint32_t next_value = loop.function->next_value_id;
    const XiCleanupBoundary *old_record = pairs[0].enter->cleanup_boundary;
    XiPassChange change = xi_opt_loop_inv_branch(loop.function);
    bool unchanged =
        !change.cfg_changed && !change.values_changed && loop.function->nblocks == block_count &&
        loop.function->next_value_id == next_value && loop.entry->succs[0] == loop.header &&
        pairs[0].enter->cleanup_boundary == old_record && clone_test_verify(loop.function);
    xi_func_free(loop.function);
    ASSERT(unchanged);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Xi cleanup graph cloning");
RUN_TEST(inline_remaps_forward_frontier_and_owns_callee_metadata);
RUN_TEST(unroll_gives_every_iteration_an_independent_complete_frontier);
RUN_TEST(unroll_without_cleanup_preserves_exit_phi_edge_and_final_latch_value);
RUN_TEST(unroll_flattens_a_cross_block_frontier_in_cfg_order);
RUN_TEST(unroll_rejects_cross_block_frontier_order_before_any_clone);
RUN_TEST(unroll_rejects_a_frontier_outside_the_iteration_without_editing);
RUN_TEST(unroll_keeps_header_cleanup_with_its_distinct_execution_count);
RUN_TEST(unswitch_remaps_cross_block_frontiers_in_both_versions);
RUN_TEST(unswitch_can_omit_a_whole_untaken_frontier);
RUN_TEST(unswitch_rejects_a_partial_specialized_frontier_without_editing);
TEST_MAIN_END()
