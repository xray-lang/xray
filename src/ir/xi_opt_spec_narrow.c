/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_spec_narrow.c - Speculative type narrowing pass
 *
 * For each call site (XI_CALL_METHOD, XI_LOAD_FIELD) that has monomorphic
 * IC metadata, inserts an XI_GUARD_TYPE instruction that:
 *   1. Checks the receiver's runtime type against the expected class.
 *   2. Deoptimizes to the interpreter on mismatch.
 *   3. Narrows the type of the receiver for downstream optimization.
 *
 * Guard merging: when multiple call sites in the same block guard the
 * same receiver value, only the first guard is inserted; subsequent
 * uses piggyback on the already-guarded value.
 */

#include "xi_opt_spec_narrow.h"
#include "xi.h"
#include "xi_ic.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xtype.h"
#include <string.h>

/* Maximum guards inserted per block (prevents guard explosion). */
#define MAX_GUARDS_PER_BLOCK 8

/* Track which values have already been guarded in the current block.
 * Used for guard merging: if value v was already guarded to type T,
 * reuse the guarded value instead of inserting a duplicate guard. */
typedef struct {
    uint32_t original_id; /* value ID before guard */
    XiValue *guarded;     /* XI_GUARD_TYPE result value */
} GuardEntry;

typedef struct {
    GuardEntry entries[MAX_GUARDS_PER_BLOCK];
    uint32_t count;
} GuardTable;

static XiValue *find_guarded(const GuardTable *tbl, uint32_t value_id) {
    for (uint32_t i = 0; i < tbl->count; i++) {
        if (tbl->entries[i].original_id == value_id)
            return tbl->entries[i].guarded;
    }
    return NULL;
}

static bool add_guarded(GuardTable *tbl, uint32_t original_id, XiValue *guarded) {
    if (tbl->count >= MAX_GUARDS_PER_BLOCK)
        return false;
    tbl->entries[tbl->count].original_id = original_id;
    tbl->entries[tbl->count].guarded = guarded;
    tbl->count++;
    return true;
}

/* Insert an XI_GUARD_TYPE before position `pos` in block `blk`.
 * xi_value_new appends at end; we then rotate the guard into place.
 * Returns the guard value (re-typed receiver). */
static XiValue *insert_guard(XiFunc *f, XiBlock *blk, uint32_t pos, XiValue *receiver,
                             const XiIcMeta *ic) {
    (void) ic;

    XiValue *guard = xi_value_new(f, blk, XI_GUARD_TYPE, receiver->type, 1);
    if (!guard)
        return NULL;

    guard->args[0] = receiver;
    guard->aux_int = (int64_t) ic->targets[0].type_id;
    guard->line = receiver->line;

    /* xi_value_new appended guard at nvalues-1.
     * Rotate it down to position `pos`: shift values[pos .. nvalues-2]
     * right by one and place guard at pos. */
    uint32_t end = blk->nvalues - 1;
    if (pos < end) {
        for (uint32_t j = end; j > pos; j--) {
            blk->values[j] = blk->values[j - 1];
        }
        blk->values[pos] = guard;
    }

    return guard;
}

XR_FUNC XiPassChange xi_opt_spec_narrow(XiFunc *f) {
    if (!f)
        return xi_pass_no_change();

    /* Static degradation: no IC table → skip (AOT or cold path). */
    if (!(f->invariant_mask & XI_INV_IC_ATTACHED) || !f->ic_table)
        return xi_pass_no_change();

    if (f->ic_table->nentries == 0)
        return xi_pass_no_change();

    uint32_t guards_inserted = 0;

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;

        GuardTable gtbl;
        memset(&gtbl, 0, sizeof(gtbl));

        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v)
                continue;

            /* Only narrow method calls and field accesses. */
            bool is_call = (v->op == XI_CALL_METHOD);
            bool is_field = (v->op == XI_LOAD_FIELD || v->op == XI_STORE_FIELD);
            if (!is_call && !is_field)
                continue;

            const XiIcMeta *ic = xi_ic_lookup(f, v->id);
            if (!ic || ic->kind != XI_IC_MONO)
                continue;

            /* Receiver is always args[0] for these ops. */
            if (v->nargs == 0 || !v->args[0])
                continue;
            XiValue *receiver = v->args[0];

            /* Guard merging: reuse existing guard for the same receiver. */
            XiValue *existing = find_guarded(&gtbl, receiver->id);
            if (existing) {
                v->args[0] = existing;
                guards_inserted++;
                continue;
            }

            /* Insert guard before this instruction. */
            XiValue *guarded = insert_guard(f, blk, vi, receiver, ic);
            if (!guarded)
                continue;

            /* Update current value to use guarded receiver. */
            v->args[0] = guarded;
            add_guarded(&gtbl, receiver->id, guarded);
            guards_inserted++;

            /* Skip past the newly inserted guard. */
            vi++;
        }
    }

    if (guards_inserted == 0)
        return xi_pass_no_change();

    return (XiPassChange) {
        .values_changed = true,
        .cfg_changed = false,
        .types_changed = true,
        .n_added = guards_inserted,
    };
}
