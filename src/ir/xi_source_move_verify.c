/*
 * xi_source_move_verify.c - N-version verifier for XI_SOURCE_MOVE.
 *
 * This pass does not trust an opaque proof ID. It independently checks the
 * canonical evidence axes, definition/order constraints, and every reachable
 * post-consume use. Allocation failure is an INTERNAL_ERROR, never success.
 */

#include "xi_source_move_verify.h"
#include "xi_analysis.h"
#include "../base/xmalloc.h"
#include "../frontend/analyzer/xa_ownership.h"
#include "../runtime/value/xtype.h"
#include <stdio.h>
#include <string.h>

static XiSourceMoveVerifyStatus move_report(XiSourceMoveVerifyReport *report,
                                            XiSourceMoveVerifyStatus status,
                                            XiSourceMoveContract contract, const XiFunc *func,
                                            const XiValue *move, const XiValue *use,
                                            const char *message) {
    if (report) {
        memset(report, 0, sizeof(*report));
        report->status = status;
        report->contract = contract;
        report->func = func;
        report->move = move;
        report->use = use;
        snprintf(report->message, sizeof(report->message), "%s", message ? message : "");
    }
    return status;
}

static int value_position(const XiBlock *block, const XiValue *value) {
    if (!block || !value)
        return -1;
    for (uint32_t i = 0; i < block->nvalues; i++) {
        if (block->values[i] == value)
            return (int) i;
    }
    return -1;
}

static XiSourceMoveVerifyStatus block_reachable_from(const XiFunc *func, const XiBlock *start,
                                                     const XiBlock *target, bool *out,
                                                     XiSourceMoveVerifyReport *report,
                                                     const XiValue *move) {
    *out = false;
    if (start == target) {
        *out = true;
        return XI_SOURCE_MOVE_PASS;
    }
    bool *seen = (bool *) xr_calloc(func->nblocks ? func->nblocks : 1, sizeof(bool));
    const XiBlock **queue =
        (const XiBlock **) xr_malloc((func->nblocks ? func->nblocks : 1) * sizeof(*queue));
    if (!seen || !queue) {
        xr_free(seen);
        xr_free(queue);
        return move_report(report, XI_SOURCE_MOVE_INTERNAL_ERROR, XI_SOURCE_MOVE_C5_RESOURCE, func,
                           move, NULL,
                           "source-move verifier allocation failed (AnalysisResourceFailure)");
    }
    uint32_t head = 0;
    uint32_t tail = 0;
    if (start->id >= func->nblocks) {
        xr_free(seen);
        xr_free(queue);
        return move_report(report, XI_SOURCE_MOVE_VIOLATION, XI_SOURCE_MOVE_C2_DOMINANCE, func,
                           move, NULL, "source-move block has an invalid CFG id");
    }
    queue[tail++] = start;
    seen[start->id] = true;
    while (head < tail) {
        const XiBlock *block = queue[head++];
        for (int si = 0; si < 2; si++) {
            const XiBlock *succ = block->succs[si];
            if (!succ)
                continue;
            if (succ == target) {
                *out = true;
                head = tail;
                break;
            }
            if (succ->id >= func->nblocks || seen[succ->id])
                continue;
            seen[succ->id] = true;
            queue[tail++] = succ;
        }
    }
    xr_free(seen);
    xr_free(queue);
    return XI_SOURCE_MOVE_PASS;
}

static bool value_uses(const XiValue *user, const XiValue *source) {
    if (!user || !source)
        return false;
    for (uint16_t i = 0; i < user->nargs; i++) {
        if (user->args && user->args[i] == source)
            return true;
    }
    return false;
}

/* ARC may preserve a coroutine-frame owner across a consuming move by
 * pre-paying one extra reference, then dropping the frame slot immediately
 * after the move:
 *
 *     RETAIN source
 *     moved = SOURCE_MOVE source
 *     RELEASE source
 *
 * RELEASE is physical ownership bookkeeping, not a semantic read of the
 * source binding.  Accept only a same-block cleanup backed by an unmatched
 * RETAIN before the move; an ordinary value use, an unbalanced release, or a
 * cleanup on a later CFG path remains a C3 violation.  The independent ARC
 * verifier still proves the complete path-wise reference-count balance. */
static bool is_balanced_arc_cleanup(const XiValue *move, const XiValue *release,
                                    const XiValue *source) {
    if (!move || !release || !source || release->op != XI_RELEASE || release->nargs != 1 ||
        !release->args || release->args[0] != source || !move->block ||
        release->block != move->block)
        return false;

    int move_pos = value_position(move->block, move);
    int release_pos = value_position(release->block, release);
    if (move_pos < 0 || release_pos <= move_pos)
        return false;

    int credits = 0;
    for (int i = 0; i < move_pos; i++) {
        const XiValue *value = move->block->values[i];
        if (!value || value->nargs != 1 || !value->args || value->args[0] != source)
            continue;
        if (value->op == XI_RETAIN)
            credits++;
        else if (value->op == XI_RELEASE && credits > 0)
            credits--;
    }
    for (int i = move_pos + 1; i < release_pos && credits > 0; i++) {
        const XiValue *value = move->block->values[i];
        if (value && value->op == XI_RELEASE && value->nargs == 1 && value->args &&
            value->args[0] == source)
            credits--;
    }
    return credits > 0;
}

static XiSourceMoveVerifyStatus verify_one_move(XiFunc *func, XiValue *move,
                                                XiSourceMoveVerifyReport *report) {
    const uint32_t required = XA_OWNERSHIP_EV_BINDING_LIVE | XA_OWNERSHIP_EV_ROOT_UNIQUE |
                              XA_OWNERSHIP_EV_LOAN_FREE | XA_OWNERSHIP_EV_ALIAS_FREE |
                              XA_OWNERSHIP_EV_ESCAPE_FREE | XA_OWNERSHIP_EV_CAPABILITY |
                              XA_OWNERSHIP_EV_CFG_CONSISTENT | XA_OWNERSHIP_EV_STORAGE;
    if (!move || move->nargs != 1 || !move->args || !move->args[0])
        return move_report(report, XI_SOURCE_MOVE_VIOLATION, XI_SOURCE_MOVE_C1_EVIDENCE, func, move,
                           NULL, "XI_SOURCE_MOVE has no source operand");
    if (move->move_evidence_id == 0 || move->move_source_root_id == 0 ||
        move->move_source_symbol_id == 0 || move->move_storage_plan_id == 0 ||
        (move->move_evidence_bits & required) != required ||
        move->move_source_capability == XA_CAP_UNKNOWN ||
        move->move_source_domain == XR_STORAGE_DOMAIN_UNKNOWN)
        return move_report(report, XI_SOURCE_MOVE_VIOLATION, XI_SOURCE_MOVE_C1_EVIDENCE, func, move,
                           NULL, "XI_SOURCE_MOVE lacks complete canonical move evidence");

    XiValue *source = move->args[0];
    if (!move->type || !source->type ||
        (move->type->kind != XR_KIND_UNKNOWN && source->type->kind != XR_KIND_UNKNOWN &&
         move->type->kind != source->type->kind && !xr_type_assignable(move->type, source->type)))
        return move_report(report, XI_SOURCE_MOVE_VIOLATION, XI_SOURCE_MOVE_C4_TYPE, func, move,
                           source, "XI_SOURCE_MOVE changes the source value type");

    xi_ensure_dominators(func);
    if (!source->block || !move->block || !xi_dominates(source->block, move->block))
        return move_report(report, XI_SOURCE_MOVE_VIOLATION, XI_SOURCE_MOVE_C2_DOMINANCE, func,
                           move, source, "source definition does not dominate XI_SOURCE_MOVE");
    if (source->block == move->block &&
        value_position(move->block, source) >= value_position(move->block, move))
        return move_report(report, XI_SOURCE_MOVE_VIOLATION, XI_SOURCE_MOVE_C2_DOMINANCE, func,
                           move, source, "source definition is not ordered before XI_SOURCE_MOVE");

    int move_pos = value_position(move->block, move);
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        XiBlock *block = func->blocks[bi];
        if (!block)
            continue;
        bool reachable = false;
        XiSourceMoveVerifyStatus reach_status =
            block_reachable_from(func, move->block, block, &reachable, report, move);
        if (reach_status != XI_SOURCE_MOVE_PASS)
            return reach_status;
        if (!reachable)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            XiValue *user = block->values[vi];
            if (!user || user == move || !value_uses(user, source))
                continue;
            if (block != move->block || (int) vi > move_pos) {
                if (is_balanced_arc_cleanup(move, user, source))
                    continue;
                return move_report(report, XI_SOURCE_MOVE_VIOLATION,
                                   XI_SOURCE_MOVE_C3_USE_AFTER_CONSUME, func, move, user,
                                   "source value is used on a path after XI_SOURCE_MOVE");
            }
        }
        if (block->control == source && (block != move->block || move_pos < (int) block->nvalues))
            return move_report(report, XI_SOURCE_MOVE_VIOLATION,
                               XI_SOURCE_MOVE_C3_USE_AFTER_CONSUME, func, move, block->control,
                               "source value reaches a terminator after XI_SOURCE_MOVE");
    }
    return XI_SOURCE_MOVE_PASS;
}

XiSourceMoveVerifyStatus xi_source_move_verify(XiFunc *func, XiSourceMoveVerifyReport *report) {
    if (report)
        memset(report, 0, sizeof(*report));
    if (!func)
        return move_report(report, XI_SOURCE_MOVE_INTERNAL_ERROR, XI_SOURCE_MOVE_C5_RESOURCE, NULL,
                           NULL, NULL, "source-move verifier received NULL function");
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        XiBlock *block = func->blocks[bi];
        for (uint32_t vi = 0; block && vi < block->nvalues; vi++) {
            XiValue *value = block->values[vi];
            if (!value || value->op != XI_SOURCE_MOVE)
                continue;
            XiSourceMoveVerifyStatus status = verify_one_move(func, value, report);
            if (status != XI_SOURCE_MOVE_PASS)
                return status;
        }
    }
    if (report)
        report->status = XI_SOURCE_MOVE_PASS;
    return XI_SOURCE_MOVE_PASS;
}

XiSourceMoveVerifyStatus xi_source_move_verify_tree(XiFunc *func,
                                                    XiSourceMoveVerifyReport *report) {
    if (!func)
        return XI_SOURCE_MOVE_PASS;
    XiSourceMoveVerifyStatus status = xi_source_move_verify(func, report);
    if (status != XI_SOURCE_MOVE_PASS)
        return status;
    for (uint16_t i = 0; i < func->nchildren; i++) {
        status = xi_source_move_verify_tree(func->children[i], report);
        if (status != XI_SOURCE_MOVE_PASS)
            return status;
    }
    return XI_SOURCE_MOVE_PASS;
}
