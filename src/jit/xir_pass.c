/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_pass.c - XIR optimization passes implementation
 *
 * KEY CONCEPT:
 *   Each pass operates on XirFunc in-place. Passes are composable
 *   and ordered by the pipeline runner based on optimization level.
 */

#include "xir_pass_internal.h"
#include "xir_pass_limits.h"
#include "xir_pass_sccp.h"
#include "xir_looptree.h"
#include "xir_domtree.h"
#include "xir_alias.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../base/xtime.h"

/* ========== Dead Code Elimination ========== */

/*
 * Worklist-driven DCE: build DefUse once, seed worklist with dead vregs,
 * cascade deletions via use_count decrements. Single pass, no re-scanning.
 */
static inline bool dce_is_removable(const XirIns *ins) {
    if (!xir_ref_is_vreg(ins->dst))
        return false;
    if (ins->flags & (XIR_FLAG_SIDE_EFFECT | XIR_FLAG_MAY_THROW))
        return false;
    if (xir_op_has_side_effect(ins->op))
        return false;
    return true;
}

/*
 * Helper: decrement use_count for a vreg ref.
 * If count drops to 0 and the defining instruction is removable, push to worklist.
 */
static inline void dce_dec_use(uint32_t *use_count, XirRef ref, uint32_t nvreg, uint32_t *wl,
                               uint32_t *wl_top, const uint8_t *removable) {
    if (!xir_ref_is_vreg(ref))
        return;
    uint32_t v = XIR_REF_INDEX(ref);
    if (v >= nvreg)
        return;
    if (use_count[v] > 0)
        use_count[v]--;
    if (use_count[v] == 0 && removable[v]) {
        wl[(*wl_top)++] = v;
    }
}

XirPassChange xir_pass_dce(XirFunc *func) {
    if (!func || func->nvreg == 0)
        return xir_pass_no_change();

    uint32_t nv = func->nvreg;

    // Step 1: Build DefUse once to get use counts
    XirDefUse du;
    xir_defuse_build(&du, func);
    if (du.nvreg == 0 && nv > 0)
        return xir_pass_no_change();

    // Step 2: Extract mutable use_count[] and compute removable[] flags
    uint32_t *use_count = (uint32_t *) xr_malloc(nv * sizeof(uint32_t));
    uint8_t *removable = (uint8_t *) xr_calloc(nv, sizeof(uint8_t));
    uint32_t *worklist = (uint32_t *) xr_malloc(nv * sizeof(uint32_t));
    if (!use_count || !removable || !worklist) {
        xr_free(use_count);
        xr_free(removable);
        xr_free(worklist);
        xir_defuse_free(&du);
        return xir_pass_no_change();
    }

    for (uint32_t v = 0; v < nv; v++) {
        use_count[v] = (v < du.nvreg) ? du.count[v] : 0;
    }
    xir_defuse_free(&du);

    // Mark which vregs have removable definitions (instruction or phi)
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        for (XirPhi *phi = blk->phis; phi; phi = phi->next) {
            if (xir_ref_is_vreg(phi->dst)) {
                removable[XIR_REF_INDEX(phi->dst)] = 1;
            }
        }
        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];
            if (dce_is_removable(ins)) {
                removable[XIR_REF_INDEX(ins->dst)] = 1;
            }
        }
    }

    // Step 3: Seed worklist with initially dead removable vregs
    uint32_t wl_top = 0;
    for (uint32_t v = 0; v < nv; v++) {
        if (use_count[v] == 0 && removable[v]) {
            worklist[wl_top++] = v;
        }
    }

    // Step 4: Process worklist — NOP dead instructions, cascade to operands
    uint32_t n_removed = 0;
    while (wl_top > 0) {
        uint32_t v = worklist[--wl_top];
        if (use_count[v] > 0)
            continue;  // re-check (may have been re-used)

        XirIns *def = func->vregs[v].def;
        if (def && dce_is_removable(def)) {
            // Decrement operand use counts before NOPing
            dce_dec_use(use_count, def->args[0], nv, worklist, &wl_top, removable);
            dce_dec_use(use_count, def->args[1], nv, worklist, &wl_top, removable);
            n_removed++;
            def->op = XIR_NOP;
            def->dst = XIR_NONE;
            def->args[0] = XIR_NONE;
            def->args[1] = XIR_NONE;
            def->flags = 0;
            func->vregs[v].def = NULL;
            continue;
        }

        // Check if it's a dead phi (def pointer is NULL for phis)
        for (uint32_t bi = 0; bi < func->nblk; bi++) {
            XirBlock *blk = func->blocks[bi];
            XirPhi **pp = &blk->phis;
            while (*pp) {
                XirPhi *phi = *pp;
                if (xir_ref_is_vreg(phi->dst) && XIR_REF_INDEX(phi->dst) == v) {
                    // Decrement use counts for phi args
                    for (uint32_t a = 0; a < phi->narg; a++) {
                        dce_dec_use(use_count, phi->args[a], nv, worklist, &wl_top, removable);
                    }
                    *pp = phi->next;
                    n_removed++;
                    goto phi_removed;
                }
                pp = &(*pp)->next;
            }
        }
    phi_removed:;
    }

    // Step 5: Compact — remove NOPs from all blocks
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        uint32_t write = 0;
        for (uint32_t read = 0; read < blk->nins; read++) {
            if (blk->ins[read].op != XIR_NOP) {
                if (write != read)
                    blk->ins[write] = blk->ins[read];
                write++;
            }
        }
        blk->nins = write;
    }

    xr_free(use_count);
    xr_free(removable);
    xr_free(worklist);
    return n_removed ? (XirPassChange){false, true, false, n_removed, 0, n_removed}
                     : xir_pass_no_change();
}

/* ========== Instruction Normalization (shared by CSE and GVN) ========== */

/* Normalize instruction args: for commutative ops, ensure
 * args[0] <= args[1] (by XirRef numerical value). This makes
 * ADD(v5,v2) hash the same as ADD(v2,v5). */
static void gvn_normalize(XirIns *ins) {
    if (xir_op_is_commutative(ins->op) && ins->args[0] > ins->args[1]) {
        XirRef tmp = ins->args[0];
        ins->args[0] = ins->args[1];
        ins->args[1] = tmp;
    }
    /* Canonicalize non-commutative comparisons: GT→LT, GE→LE (swap args).
     * This ensures GT(a,b) and LT(b,a) hash identically in GVN/CSE. */
    switch (ins->op) {
        case XIR_GT:
            ins->op = XIR_LT;
            goto swap_args;
        case XIR_GE:
            ins->op = XIR_LE;
            goto swap_args;
        default:
            break;
    }
    return;
swap_args: {
    XirRef tmp = ins->args[0];
    ins->args[0] = ins->args[1];
    ins->args[1] = tmp;
}
}

/* ========== Common Subexpression Elimination ========== */

/*
 * Local CSE within each basic block. When two instructions in the same block
 * have the same (op, args[0], args[1]) and neither is side-effecting, the
 * second instruction is replaced by a copy from the first's result.
 *
 * Uses a simple hash table keyed on (op, arg0, arg1) → dst vreg.
 * Only pure instructions (no side effects, no memory ops) are candidates.
 */

/* Table sizing bounds come from xir_pass_limits.h; aliased here for
 * readability of the local CSE code. */
#define CSE_MIN_TABLE XIR_CSE_MIN_TABLE
#define CSE_MAX_TABLE XIR_CSE_MAX_TABLE

typedef struct {
    uint32_t key;  // hash of (op, arg0, arg1); 0 = empty slot
    uint16_t op;
    XirRef arg0;
    XirRef arg1;
    XirRef result;  // dst vreg of the first occurrence
} CseEntry;

static uint32_t cse_hash(uint16_t op, XirRef a0, XirRef a1) {
    uint32_t h = (uint32_t) op * 2654435761u;
    h ^= (uint32_t) a0 * 2246822519u;
    h ^= (uint32_t) a1 * 3266489917u;
    return h | 1;  // ensure non-zero (0 = empty)
}

XirPassChange xir_pass_cse(XirFunc *func) {
    if (!func)
        return xir_pass_no_change();

    // Find max block size to determine table capacity
    uint32_t max_nins = 0;
    for (uint32_t bi = 0; bi < func->nblk; bi++)
        if (func->blocks[bi]->nins > max_nins)
            max_nins = func->blocks[bi]->nins;

    // Size table to ~50% load factor, clamped to [CSE_MIN_TABLE, CSE_MAX_TABLE]
    uint32_t tsize = CSE_MIN_TABLE;
    while (tsize < CSE_MAX_TABLE && tsize < max_nins * 2)
        tsize <<= 1;
    uint32_t tmask = tsize - 1;

    CseEntry *table = (CseEntry *) xr_malloc(tsize * sizeof(CseEntry));
    if (!table)
        return xir_pass_no_change();

    uint32_t n_replaced = 0;
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        memset(table, 0, tsize * sizeof(CseEntry));

        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];

            if (!xir_op_is_pure(ins->op))
                continue;
            if (ins->flags & XIR_FLAG_SIDE_EFFECT)
                continue;
            if (!xir_ref_is_vreg(ins->dst))
                continue;

            // Normalize commutative ops for better CSE hit rate
            gvn_normalize(ins);

            uint32_t h = cse_hash(ins->op, ins->args[0], ins->args[1]);
            uint32_t slot = h & tmask;

            // Linear probe to find match or empty slot
            bool found = false;
            for (uint32_t probe = 0; probe < tsize; probe++) {
                uint32_t idx = (slot + probe) & tmask;
                CseEntry *e = &table[idx];

                if (e->key == 0) {
                    // Empty: insert new entry
                    e->key = h;
                    e->op = ins->op;
                    e->arg0 = ins->args[0];
                    e->arg1 = ins->args[1];
                    e->result = ins->dst;
                    break;
                }

                if (e->key == h && e->op == ins->op && e->arg0 == ins->args[0] &&
                    e->arg1 == ins->args[1]) {
                    // Match: replace this instruction with MOV from first result
                    ins->op = XIR_MOV;
                    ins->args[0] = e->result;
                    ins->args[1] = XIR_NONE;
                    n_replaced++;
                    found = true;
                    break;
                }
            }
            (void) found;
        }
    }

    xr_free(table);
    return n_replaced ? (XirPassChange){false, false, true, 0, 0, 0} : xir_pass_no_change();
}

/* ========== Global Value Numbering (GVN) ========== */

/*
 * Dominator-based global CSE. Eliminates redundant pure computations
 * across basic blocks by leveraging dominance: if block A dominates
 * block B and both compute the same (op, arg0, arg1), the second
 * occurrence in B is replaced with a MOV from A's result.
 *
 * Algorithm:
 *   1. Compute immediate dominators (Cooper's algorithm)
 *   2. Traverse blocks in layout order with a global hash table
 *   3. For each pure instruction, look up (op, arg0, arg1) in table
 *   4. If found AND the defining block dominates current block → replace
 *   5. Otherwise insert into table
 */

/* Helper: if vreg is defined by XIR_CONST_I64, return its i64 value.
 * Returns false if not a constant. */
static bool gvn_get_const_i64(XirFunc *func, XirRef ref, int64_t *out) {
    if (!xir_ref_is_vreg(ref))
        return false;
    uint32_t idx = XIR_REF_INDEX(ref);
    if (idx >= func->nvreg)
        return false;
    XirIns *def = func->vregs[idx].def;
    if (!def || def->op != XIR_CONST_I64)
        return false;
    if (!xir_ref_is_const(def->args[0]))
        return false;
    uint32_t ci = XIR_REF_INDEX(def->args[0]);
    if (ci >= func->nconst)
        return false;
    *out = func->consts[ci].val.i64;
    return true;
}

/* Try to fold associative constant chains: (a OP K1) OP K2 → a OP (K1+K2).
 * Only applies to ADD/MUL/AND/OR/XOR where one arg is a constant and
 * the other is defined by the same op with one constant argument.
 *
 * Safe rewrite strategy: modifies only the current instruction's args
 * to bypass the intermediate (a OP K1), and updates the outer constant
 * vreg's definition to hold the combined value. The const vreg is only
 * modified if it has exactly one use (this instruction), checked via
 * def-use info passed as parameter. The inner (a OP K1) becomes dead
 * and will be cleaned up by a subsequent DCE pass. */
static bool gvn_assoc_const(XirFunc *func, XirIns *ins, const XirDefUse *du) {
    if (!xir_ref_is_vreg(ins->dst))
        return false;
    if (!du)
        return false;

    // Only integer associative ops
    uint16_t op = ins->op;
    if (op != XIR_ADD && op != XIR_MUL && op != XIR_AND && op != XIR_OR && op != XIR_XOR)
        return false;

    // Identify which arg is a constant vreg, which is a non-const vreg
    int const_slot = -1;
    int64_t k2 = 0;
    for (int a = 0; a < 2; a++) {
        int64_t val;
        if (gvn_get_const_i64(func, ins->args[a], &val)) {
            const_slot = a;
            k2 = val;
        }
    }
    if (const_slot < 0)
        return false;
    int var_slot = 1 - const_slot;
    if (!xir_ref_is_vreg(ins->args[var_slot]))
        return false;

    // Check if varg is defined by the same op with one constant arg
    uint32_t vidx = XIR_REF_INDEX(ins->args[var_slot]);
    if (vidx >= func->nvreg)
        return false;
    XirIns *vdef = func->vregs[vidx].def;
    if (!vdef || vdef->op != op)
        return false;

    int64_t k1 = 0;
    int inner_const_slot = -1;
    for (int a = 0; a < 2; a++) {
        if (gvn_get_const_i64(func, vdef->args[a], &k1)) {
            inner_const_slot = a;
            break;
        }
    }
    if (inner_const_slot < 0)
        return false;
    XirRef inner_var = vdef->args[1 - inner_const_slot];

    /* Safety check: only modify the outer const vreg if it has exactly
     * one use (this instruction). Otherwise we'd corrupt other users. */
    uint32_t const_vreg = XIR_REF_INDEX(ins->args[const_slot]);
    if (const_vreg >= func->nvreg)
        return false;
    if (xir_defuse_nuses(du, const_vreg) != 1)
        return false;

    // Combine constants: K1 OP K2
    int64_t combined;
    switch (op) {
        case XIR_ADD:
            combined = k1 + k2;
            break;
        case XIR_MUL:
            combined = k1 * k2;
            break;
        case XIR_AND:
            combined = k1 & k2;
            break;
        case XIR_OR:
            combined = k1 | k2;
            break;
        case XIR_XOR:
            combined = k1 ^ k2;
            break;
        default:
            return false;
    }

    // Update the const vreg's definition to hold the combined value
    XirRef new_cref = xir_const_i64(func, combined);
    XirIns *cdef = func->vregs[const_vreg].def;
    if (cdef && cdef->op == XIR_CONST_I64) {
        cdef->args[0] = new_cref;
    } else {
        return false;
    }

    // Rewrite the current instruction to use inner_var directly
    ins->args[var_slot] = inner_var;
    // args[const_slot] stays the same vreg, but its value is now combined

    gvn_normalize(ins);
    return true;
}

#define GVN_MIN_TABLE XIR_GVN_MIN_TABLE

typedef struct {
    uint32_t key;  // hash; 0 = empty
    uint16_t op;
    XirRef arg0, arg1;
    XirRef result;
    uint32_t def_blk;  // block id of first occurrence
} GvnEntry;

XirPassChange xir_pass_gvn(XirFunc *func) {
    if (!func || func->nblk < 2)
        return xir_pass_no_change();

    const XirDomTree *dt = xir_func_get_domtree(func);
    if (!dt)
        return xir_pass_no_change();

    /* Dynamic table size: one slot per ~half an instruction so the load
     * factor stays around 50%.  Floored at GVN_MIN_TABLE so tiny
     * functions still have probe room; XIR_MAX_FUNC_TOTAL_INS bounds
     * worst-case allocation upstream. */
    uint32_t total_ins = 0;
    for (uint32_t bi = 0; bi < func->nblk; bi++)
        total_ins += func->blocks[bi]->nins;
    uint32_t gvn_tsize = GVN_MIN_TABLE;
    while (gvn_tsize < total_ins * 2)
        gvn_tsize <<= 1;
    uint32_t gvn_tmask = gvn_tsize - 1;

    GvnEntry *table = (GvnEntry *) xr_calloc(gvn_tsize, sizeof(GvnEntry));
    if (!table)
        return xir_pass_no_change();

    uint32_t n_replaced = 0;

    /* Pull the def-use chain from the function's shared cache (Phase
     * 2.3) rather than building a local throwaway copy. */
    const XirDefUse *du = xir_func_get_defuse(func);
    bool has_du = (du && du->nvreg > 0);

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];

        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];
            if (!xir_op_is_pure(ins->op))
                continue;
            if (ins->flags & XIR_FLAG_SIDE_EFFECT)
                continue;
            if (!xir_ref_is_vreg(ins->dst))
                continue;

            // Normalize commutative ops so ADD(v5,v2) hashes same as ADD(v2,v5)
            gvn_normalize(ins);

            // Try associative constant folding: (a+K1)+K2 → a+(K1+K2)
            if (has_du)
                gvn_assoc_const(func, ins, du);

            uint32_t h = cse_hash(ins->op, ins->args[0], ins->args[1]);
            uint32_t slot = h & gvn_tmask;

            for (uint32_t probe = 0; probe < gvn_tsize; probe++) {
                uint32_t idx = (slot + probe) & gvn_tmask;
                GvnEntry *e = &table[idx];

                if (e->key == 0) {
                    // Empty: insert
                    e->key = h;
                    e->op = ins->op;
                    e->arg0 = ins->args[0];
                    e->arg1 = ins->args[1];
                    e->result = ins->dst;
                    e->def_blk = bi;
                    break;
                }

                if (e->key == h && e->op == ins->op && e->arg0 == ins->args[0] &&
                    e->arg1 == ins->args[1]) {
                    /* Match: replace with the existing result only
                     * when the earlier definition dominates us.  The
                     * cached dominator tree answers this in O(1). */
                    if (xir_dom_covers(dt, e->def_blk, bi)) {
                        ins->op = XIR_MOV;
                        ins->args[0] = e->result;
                        ins->args[1] = XIR_NONE;
                        n_replaced++;
                    }
                    break;
                }
            }
        }
    }

    xr_free(table);
    return n_replaced ? (XirPassChange){false, false, true, 0, 0, 0} : xir_pass_no_change();
}

/* ========== Store-to-Load Forwarding ========== */

/*
 * Per-block forward scan: track recent stores and loads to object fields.
 * When a LOAD_FIELD matches a prior STORE_FIELD's (obj, offset), replace
 * the load with MOV from the stored value. Also handles load-load forwarding.
 *
 * Alias rules (within a single block, no cross-block tracking):
 *   - Same obj vreg + same const offset → Must Alias (forward value)
 *   - Same obj vreg + different offset → No Alias (no interference)
 *   - Different obj vregs → May Alias (conservatively kill all entries)
 *   - CALL / side-effecting instructions → kill all entries
 *
 * The "different obj → kill all" rule is conservative but safe. A future
 * enhancement could use ALLOC provenance to prove No Alias between
 * different allocation sites.
 */

typedef struct {
    XirRef obj;      // object pointer vreg
    int64_t offset;  // field byte offset
    XirRef value;    // last stored/loaded value vreg
    bool valid;
    bool is_store;       // true if value came from a store (for DSE)
    uint32_t store_blk;  // block index of the store (for DSE)
    uint32_t store_idx;  // instruction index of the store (for DSE)
} S2lEntry;

typedef struct {
    S2lEntry *entries;
    uint32_t size;  // power-of-two capacity
    uint32_t mask;  // size - 1
} S2lTable;

// Helper: extract constant i64 offset from a LOAD_FIELD/STORE_FIELD ref
static bool s2l_get_offset(XirFunc *func, XirRef ref, int64_t *out) {
    if (!xir_ref_is_const(ref))
        return false;
    uint32_t ci = XIR_REF_INDEX(ref);
    if (ci >= func->nconst)
        return false;
    *out = func->consts[ci].val.i64;
    return true;
}

// Invalidate all entries (after calls, unknown stores, etc.)
static void s2l_kill_all(S2lTable *t) {
    memset(t->entries, 0, t->size * sizeof(S2lEntry));
}

// Look up (obj, offset) in the S2L table. Returns value vreg or XIR_NONE.
static XirRef s2l_lookup(const S2lTable *t, XirRef obj, int64_t offset) {
    uint32_t h = ((uint32_t) obj * 31 + (uint32_t) offset) & t->mask;
    for (uint32_t probe = 0; probe < t->size; probe++) {
        uint32_t idx = (h + probe) & t->mask;
        S2lEntry *e = &t->entries[idx];
        if (!e->valid)
            return XIR_NONE;
        if (e->obj == obj && e->offset == offset)
            return e->value;
    }
    return XIR_NONE;
}

// Look up full entry (for DSE: need to check is_store). Returns NULL if not found.
static S2lEntry *s2l_find(S2lTable *t, XirRef obj, int64_t offset) {
    uint32_t h = ((uint32_t) obj * 31 + (uint32_t) offset) & t->mask;
    for (uint32_t probe = 0; probe < t->size; probe++) {
        uint32_t idx = (h + probe) & t->mask;
        S2lEntry *e = &t->entries[idx];
        if (!e->valid)
            return NULL;
        if (e->obj == obj && e->offset == offset)
            return e;
    }
    return NULL;
}

// Insert or update (obj, offset) → value in the S2L table.
static void s2l_insert(S2lTable *t, XirRef obj, int64_t offset, XirRef value, bool is_store,
                       uint32_t blk, uint32_t idx) {
    uint32_t h = ((uint32_t) obj * 31 + (uint32_t) offset) & t->mask;
    for (uint32_t probe = 0; probe < t->size; probe++) {
        uint32_t slot = (h + probe) & t->mask;
        S2lEntry *e = &t->entries[slot];
        if (!e->valid || (e->obj == obj && e->offset == offset)) {
            e->obj = obj;
            e->offset = offset;
            e->value = value;
            e->valid = true;
            e->is_store = is_store;
            e->store_blk = blk;
            e->store_idx = idx;
            return;
        }
    }
    /* Table full — rarely hit because size is provisioned for the
     * function's actual store/load count plus padding.  Conservative
     * drop is safe: worst case we miss a forwarding opportunity. */
}

/* Decide how large the hash table should be for this function.
 * Pick the next power of two above (nstore+nload)*2 to keep the load
 * factor around 50%; floor at 32 so tiny functions still have room
 * for probing.  Max capped at the central XIR_MAX_FUNC_TOTAL_INS to
 * bound worst-case allocation. */
static uint32_t s2l_table_size(XirFunc *func) {
    uint32_t fields = 0;
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nins; i++) {
            uint16_t op = blk->ins[i].op;
            if (op == XIR_LOAD_FIELD || op == XIR_STORE_FIELD)
                fields++;
        }
    }
    uint32_t target = fields * 2u;
    uint32_t size = 32;
    while (size < target)
        size <<= 1;
    if (size > XIR_MAX_FUNC_TOTAL_INS)
        size = XIR_MAX_FUNC_TOTAL_INS;
    return size;
}

XirPassChange xir_pass_store_to_load(XirFunc *func) {
    if (!func || func->nblk == 0)
        return xir_pass_no_change();

    S2lTable t;
    t.size = s2l_table_size(func);
    t.mask = t.size - 1;
    t.entries = (S2lEntry *) xr_calloc(t.size, sizeof(S2lEntry));
    if (!t.entries)
        return xir_pass_no_change();

    uint32_t n_fwd = 0;

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];

        /* Cross-block S2L: carry forward the table when this block has
         * exactly one predecessor and it's the immediately preceding block.
         * This enables store-to-load forwarding across extended basic blocks
         * (straight-line code spanning multiple blocks). At merge points or
         * discontinuities, reset the table conservatively. */
        bool carry_forward = false;
        if (bi > 0 && blk->npred == 1 && blk->preds) {
            XirBlock *prev = func->blocks[bi - 1];
            if (blk->preds[0] == prev)
                carry_forward = true;
        }
        if (!carry_forward)
            s2l_kill_all(&t);

        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];

            if (ins->op == XIR_STORE_FIELD) {
                // STORE_FIELD: dst=const(offset), args[0]=obj, args[1]=value
                XirRef obj = ins->args[0];
                int64_t offset;
                if (s2l_get_offset(func, ins->dst, &offset)) {
                    /* DSE: if there's a prior store to same (obj,offset)
                     * with no intervening load, the prior store is dead.
                     * Works both intra-block and cross-block (when the
                     * S2L table was carried forward from a predecessor). */
                    S2lEntry *prev = s2l_find(&t, obj, offset);
                    if (prev && prev->is_store) {
                        XirBlock *dead_blk = NULL;
                        if (prev->store_blk == bi) {
                            dead_blk = blk;
                        } else if (prev->store_blk < func->nblk) {
                            dead_blk = func->blocks[prev->store_blk];
                        }
                        if (dead_blk && prev->store_idx < dead_blk->nins) {
                            XirIns *dead = &dead_blk->ins[prev->store_idx];
                            n_fwd++;
                            dead->op = XIR_MOV;
                            dead->dst = XIR_NONE;
                            dead->args[0] = XIR_NONE;
                            dead->args[1] = XIR_NONE;
                        }
                    }
                    s2l_insert(&t, obj, offset, ins->args[1], true, bi, i);
                }
                continue;
            }

            if (ins->op == XIR_LOAD_FIELD) {
                // LOAD_FIELD: dst=result, args[0]=obj, args[1]=const(offset)
                XirRef obj = ins->args[0];
                int64_t offset;
                if (!xir_ref_is_vreg(ins->dst))
                    continue;
                if (!s2l_get_offset(func, ins->args[1], &offset))
                    continue;

                XirRef fwd = s2l_lookup(&t, obj, offset);
                if (!xir_ref_is_none(fwd)) {
                    // Forward: replace LOAD_FIELD with MOV from stored/loaded value
                    n_fwd++;
                    ins->op = XIR_MOV;
                    ins->args[0] = fwd;
                    ins->args[1] = XIR_NONE;
                }
                /* Record this load result for load-load forwarding.
                 * Mark as non-store so DSE won't kill the prior store. */
                s2l_insert(&t, obj, offset, xir_ref_is_none(fwd) ? ins->dst : fwd, false, bi, i);
                continue;
            }

            /* Any side-effecting instruction kills all tracked state.
             * This includes CALL, STORE, ALLOC (may trigger GC which moves objects),
             * and anything with SIDE_EFFECT flag. */
            if (ins->op == XIR_CALL || ins->op == XIR_CALL_C || ins->op == XIR_CALL_C_LEAF ||
                ins->op == XIR_CALL_SELF_DIRECT || ins->op == XIR_CALL_KNOWN_REG ||
                ins->op == XIR_STORE || ins->op == XIR_ALLOC || ins->op == XIR_STORE_CORO ||
                (ins->flags & XIR_FLAG_SIDE_EFFECT)) {
                s2l_kill_all(&t);
            }
        }
    }

    xr_free(t.entries);
    return n_fwd ? (XirPassChange){false, true, true, n_fwd, 0, 0} : xir_pass_no_change();
}

/* ========== If-Conversion ========== */

/*
 * Convert simple diamond CFG patterns into conditional select instructions.
 *
 * Pattern detected:
 *   ifblk: BR(cond) → thenblk, elseblk
 *   thenblk: [0-2 pure instructions]; JMP joinblk
 *   elseblk: [0-2 pure instructions]; JMP joinblk
 *   joinblk: PHI(then_val, else_val) [1-2 phis, no FP phis]
 *
 * Conversion:
 *   ifblk: [original ins] + [then ins] + [else ins]
 *          + SELECT_COND(cond) + SELECT(true_val, false_val) per PHI
 *          JMP joinblk
 *   joinblk: PHI removed
 *
 * Constraints (conservative):
 *   - joinblk must have exactly 2 predecessors
 *   - then/else blocks must each have ≤ 2 non-NOP pure instructions
 *   - then/else blocks must JMP to the same joinblk
 *   - no side-effecting or pinned instructions in then/else
 */

#define IFCONV_MAX_INS XIR_IFCONV_MAX_INS
#define IFCONV_MAX_PHIS XIR_IFCONV_MAX_PHIS

static bool ifconv_ok_branch(XirBlock *blk) {
    int n = 0;
    for (uint32_t i = 0; i < blk->nins; i++) {
        XirIns *ins = &blk->ins[i];
        if (ins->op == XIR_NOP || xir_op_is_copy(ins->op))
            continue;
        if (ins->flags & (XIR_FLAG_SIDE_EFFECT | XIR_FLAG_MAY_THROW))
            return false;
        if (!xir_op_is_pure(ins->op) && ins->op != XIR_CONST_I64 && ins->op != XIR_CONST_F64 &&
            ins->op != XIR_CONST_PTR)
            return false;
        n++;
    }
    return n <= IFCONV_MAX_INS;
}

static bool ifconv_ok_join(XirBlock *blk) {
    int n = 0;
    for (XirPhi *p = blk->phis; p; p = p->next) {
        n++;
    }
    return n >= 1 && n <= IFCONV_MAX_PHIS;
}

// Find the PHI arg corresponding to a given predecessor block
static XirRef ifconv_phi_arg(XirPhi *phi, XirBlock *pred, XirBlock *joinblk) {
    for (uint32_t i = 0; i < joinblk->npred; i++) {
        if (joinblk->preds[i] == pred && i < phi->narg)
            return phi->args[i];
    }
    return XIR_NONE;
}

/* Maximum number of inner iterations.  Each round collapses one level
 * of diamond nesting into the parent block, so three rounds suffice
 * for the two-deep nested diamond shapes we care about in practice
 * without risking unbounded growth on pathological inputs. */
#define IFCONV_MAX_ROUNDS 3

XirPassChange xir_pass_ifconvert(XirFunc *func) {
    if (!func || func->nblk < 3)
        return xir_pass_no_change();

    bool ever_converted = false;

    /* Iterate to a local fixed-point so a successful outer conversion
     * exposes the next level of nesting inside the now-flattened
     * parent block.  The previous single-pass version required the
     * whole pipeline to re-run ifconvert for every nesting level. */
    for (int round = 0; round < IFCONV_MAX_ROUNDS; round++) {
        bool converted_any = false;

        for (uint32_t bi = 0; bi < func->nblk; bi++) {
            XirBlock *ifblk = func->blocks[bi];

            // Must be a conditional branch
            if (ifblk->jmp.type != XIR_JMP_BR)
                continue;
            if (!ifblk->s1 || !ifblk->s2)
                continue;
            if (ifblk->s1 == ifblk->s2)
                continue;

            XirBlock *thenblk = ifblk->s1;
            XirBlock *elseblk = ifblk->s2;

            // Both branches must JMP to the same join block
            if (thenblk->jmp.type != XIR_JMP_JMP)
                continue;
            if (elseblk->jmp.type != XIR_JMP_JMP)
                continue;
            if (thenblk->s1 != elseblk->s1)
                continue;

            XirBlock *joinblk = thenblk->s1;

            // Join block must have exactly 2 predecessors
            if (joinblk->npred != 2)
                continue;

            // Validate branch blocks and join block
            if (!ifconv_ok_branch(thenblk))
                continue;
            if (!ifconv_ok_branch(elseblk))
                continue;
            if (!ifconv_ok_join(joinblk))
                continue;

            // --- Conversion ---
            XirRef cond = ifblk->jmp.arg;

            // Move then-block instructions to ifblk
            for (uint32_t i = 0; i < thenblk->nins; i++)
                xir_emit_raw(func, ifblk, thenblk->ins[i].op, thenblk->ins[i].rep,
                             thenblk->ins[i].dst, thenblk->ins[i].args[0], thenblk->ins[i].args[1]);

            // Move else-block instructions to ifblk
            for (uint32_t i = 0; i < elseblk->nins; i++)
                xir_emit_raw(func, ifblk, elseblk->ins[i].op, elseblk->ins[i].rep,
                             elseblk->ins[i].dst, elseblk->ins[i].args[0], elseblk->ins[i].args[1]);

            // Emit SELECT_COND + SELECT for each PHI
            xir_emit_raw(func, ifblk, XIR_SELECT_COND, XR_REP_VOID, XIR_NONE, cond, XIR_NONE);

            for (XirPhi *p = joinblk->phis; p; p = p->next) {
                XirRef true_val = ifconv_phi_arg(p, thenblk, joinblk);
                XirRef false_val = ifconv_phi_arg(p, elseblk, joinblk);
                if (xir_ref_is_none(true_val) || xir_ref_is_none(false_val))
                    continue;

                // Determine type from phi dst
                uint8_t sel_type = XR_REP_I64;
                if (xir_ref_is_vreg(p->dst)) {
                    uint32_t idx = XIR_REF_INDEX(p->dst);
                    if (idx < func->nvreg)
                        sel_type = func->vregs[idx].rep;
                }

                xir_emit_raw(func, ifblk, XIR_SELECT, sel_type, p->dst, true_val, false_val);
            }

            // Rewire: ifblk JMP → joinblk
            ifblk->jmp.type = XIR_JMP_JMP;
            ifblk->jmp.arg = XIR_NONE;
            ifblk->s1 = joinblk;
            ifblk->s2 = NULL;

            // Clear then/else blocks (will be cleaned by remove_unreachable)
            thenblk->nins = 0;
            thenblk->jmp.type = XIR_JMP_NONE;
            thenblk->s1 = NULL;
            elseblk->nins = 0;
            elseblk->jmp.type = XIR_JMP_NONE;
            elseblk->s1 = NULL;

            // Update join block predecessors
            joinblk->npred = 1;
            joinblk->preds[0] = ifblk;
            joinblk->phis = NULL;
            converted_any = true;
            ever_converted = true;
        }

        if (!converted_any)
            break;
    }
    return ever_converted ? (XirPassChange){true, true, false, 0, 0, 0} : xir_pass_no_change();
}

/* ========== Loop Invariant Code Motion (LICM) ========== */

/*
 * Enhanced LICM: nested loop support + iterative chain-invariant hoisting.
 *
 * Drives off the cached XirLoopInfo (xir_looptree.h), so it no longer
 * assumes any particular block-array layout, no longer maintains its
 * own fixed-size LicmLoop[] array, and no longer writes to the (now
 * deleted) XirBlock::loop_depth field.  The central pass budget
 * XIR_LICM_MAX_ITERATIONS bounds chain-invariant propagation; every
 * other storage structure grows dynamically.
 */

typedef struct {
    XirRef obj;
    int64_t offset;
    bool has_offset;
} LicmStoreInfo;

static void licm_build_def_block(XirFunc *func, uint32_t *db, uint32_t nv) {
    memset(db, 0xFF, nv * sizeof(uint32_t));
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        for (XirPhi *phi = blk->phis; phi; phi = phi->next)
            if (xir_ref_is_vreg(phi->dst)) {
                uint32_t idx = XIR_REF_INDEX(phi->dst);
                if (idx < nv)
                    db[idx] = bi;
            }
        for (uint32_t i = 0; i < blk->nins; i++)
            if (xir_ref_is_vreg(blk->ins[i].dst)) {
                uint32_t idx = XIR_REF_INDEX(blk->ins[i].dst);
                if (idx < nv)
                    db[idx] = bi;
            }
    }
}

/*
 * Returns true if |ref| is defined outside the currently processed
 * loop (or is not a vreg at all).  |in_loop| is a per-block bitmap
 * marking body membership; the def-block of the vreg is looked up in
 * |db|.
 */
static bool licm_ref_outside(XirRef ref, const uint32_t *db, uint32_t nv, const uint8_t *in_loop,
                             uint32_t nblk) {
    if (!xir_ref_is_vreg(ref))
        return true;
    uint32_t idx = XIR_REF_INDEX(ref);
    if (idx >= nv)
        return false;
    uint32_t d = db[idx];
    if (d == UINT32_MAX || d >= nblk)
        return true;
    return !in_loop[d];
}

/* Look up a block's index in func->blocks[] (linear but only used
 * during body-bitmap setup, so per-loop cost is bounded). */
static uint32_t licm_block_index(XirFunc *func, const XirBlock *blk) {
    for (uint32_t i = 0; i < func->nblk; i++)
        if (func->blocks[i] == blk)
            return i;
    return UINT32_MAX;
}

/*
 * Return true when |a| and |b| can be *proved* to refer to distinct
 * objects based on provenance alone.
 *
 * We are conservative on purpose: absence of proof never implies
 * aliasing in the caller, only that the caller must fall back to the
 * usual pessimistic assumption.  Cases we currently handle:
 *
 *   1. Two FRESH_ALLOC vregs pointing at *different* ALLOC sites:
 *      the GC allocator produces a new object per site, so their
 *      identities cannot coincide.
 *   2. A FRESH_ALLOC against a PARAM: an object born in this
 *      function's scope cannot be an incoming argument.
 */
static bool licm_definitely_no_alias(XirFunc *func, XirRef a, XirRef b) {
    if (a == b)
        return false;
    const XirAliasInfo *ia = xir_func_get_alias(func, a);
    const XirAliasInfo *ib = xir_func_get_alias(func, b);
    if (!ia || !ib)
        return false;

    if (ia->source == XIR_ALIAS_FRESH_ALLOC && ib->source == XIR_ALIAS_FRESH_ALLOC)
        return ia->origin != ib->origin;

    if ((ia->source == XIR_ALIAS_FRESH_ALLOC && ib->source == XIR_ALIAS_PARAM) ||
        (ib->source == XIR_ALIAS_FRESH_ALLOC && ia->source == XIR_ALIAS_PARAM))
        return true;

    return false;
}

XirPassChange xir_pass_licm(XirFunc *func) {
    if (!func || func->nblk < 2 || func->nvreg == 0)
        return xir_pass_no_change();

    const XirLoopInfo *loops = xir_func_get_loops(func);
    if (!loops || loops->nloop == 0)
        return xir_pass_no_change();

    uint32_t nv = func->nvreg;
    uint32_t nblk = func->nblk;

    uint32_t *def_block = (uint32_t *) xr_malloc(nv * sizeof(uint32_t));
    uint8_t *in_loop = (uint8_t *) xr_malloc(nblk * sizeof(uint8_t));
    if (!def_block || !in_loop) {
        xr_free(def_block);
        xr_free(in_loop);
        return xir_pass_no_change();
    }
    bool any_hoisted = false;
    licm_build_def_block(func, def_block, nv);

    /* all_loops is already sorted innermost-first by xir_looptree, so
     * naive iteration processes the deepest loops before their parents
     * — a prerequisite for chain-invariant propagation. */
    for (uint32_t li = 0; li < loops->nloop; li++) {
        XirLoop *L = loops->all_loops[li];
        if (!L->preheader)
            continue;  // no unique preheader → skip

        /* Rebuild the body bitmap from XirLoop::body[]. */
        memset(in_loop, 0, nblk);
        for (uint32_t i = 0; i < L->nbody; i++) {
            uint32_t bi = licm_block_index(func, L->body[i]);
            if (bi != UINT32_MAX)
                in_loop[bi] = 1;
        }

        uint32_t preh_bi = licm_block_index(func, L->preheader);
        if (preh_bi == UINT32_MAX)
            continue;
        XirBlock *preheader = L->preheader;

        /* Collect (obj, offset) pairs stored in the loop body so we
         * can decide whether a LOAD_FIELD is aliased.  Heap-allocated
         * with growth to avoid the old LICM_MAX_STORE_OBJS cap. */
        LicmStoreInfo *stores = NULL;
        uint32_t nstores = 0, store_cap = 0;

        for (uint32_t i = 0; i < L->nbody; i++) {
            XirBlock *lb = L->body[i];
            for (uint32_t k = 0; k < lb->nins; k++) {
                XirIns *ins = &lb->ins[k];
                if (ins->op != XIR_STORE_FIELD || !xir_ref_is_vreg(ins->args[0]))
                    continue;
                XirRef obj = ins->args[0];
                int64_t off = 0;
                bool has_off = false;
                if (!xir_ref_is_none(ins->dst) && xir_ref_is_const(ins->dst)) {
                    uint32_t ci = XIR_REF_INDEX(ins->dst);
                    if (ci < func->nconst) {
                        off = func->consts[ci].val.i64;
                        has_off = true;
                    }
                }
                bool duplicate = false;
                for (uint32_t s = 0; s < nstores; s++) {
                    if (stores[s].obj == obj && stores[s].has_offset == has_off &&
                        (!has_off || stores[s].offset == off)) {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate)
                    continue;
                if (nstores >= store_cap) {
                    store_cap = store_cap ? store_cap * 2 : 8;
                    XR_REALLOC_OR_ABORT(stores, store_cap * sizeof(LicmStoreInfo), "licm stores");
                }
                stores[nstores++] = (LicmStoreInfo){obj, off, has_off};
            }
        }

        /* Iterative hoisting: when a chain-invariant is promoted, its
         * dependents become hoistable on the next round. */
        for (int iter = 0; iter < XIR_LICM_MAX_ITERATIONS; iter++) {
            bool hoisted_any = false;

            for (uint32_t i = 0; i < L->nbody; i++) {
                XirBlock *loop_blk = L->body[i];
                if (loop_blk == preheader)
                    continue;
                uint32_t write = 0;

                for (uint32_t k = 0; k < loop_blk->nins; k++) {
                    XirIns *ins = &loop_blk->ins[k];
                    bool can_hoist = false;

                    if (xir_op_is_pure(ins->op) && xir_ref_is_vreg(ins->dst)) {
                        can_hoist = true;
                        for (int a = 0; a < 2; a++) {
                            if (!licm_ref_outside(ins->args[a], def_block, nv, in_loop, nblk)) {
                                can_hoist = false;
                                break;
                            }
                        }
                    } else if (ins->op == XIR_LOAD_FIELD && xir_ref_is_vreg(ins->dst)) {
                        XirRef obj = ins->args[0];
                        if (licm_ref_outside(obj, def_block, nv, in_loop, nblk)) {
                            bool has_alias = false;
                            int64_t load_off = 0;
                            bool load_has_off = false;
                            if (xir_ref_is_const(ins->args[1])) {
                                uint32_t ci = XIR_REF_INDEX(ins->args[1]);
                                if (ci < func->nconst) {
                                    load_off = func->consts[ci].val.i64;
                                    load_has_off = true;
                                }
                            }
                            for (uint32_t s = 0; s < nstores; s++) {
                                if (stores[s].obj != obj) {
                                    /* Different vreg: only safe to
                                     * ignore when alias analysis
                                     * proves the two origins must be
                                     * distinct.  Otherwise it could
                                     * still be the same object
                                     * reached through a different
                                     * vreg. */
                                    if (licm_definitely_no_alias(func, stores[s].obj, obj))
                                        continue;
                                    has_alias = true;
                                    break;
                                }
                                if (!stores[s].has_offset || !load_has_off) {
                                    has_alias = true;
                                    break;
                                }
                                if (stores[s].offset == load_off) {
                                    has_alias = true;
                                    break;
                                }
                            }
                            can_hoist = !has_alias;
                        }
                    } else if (ins->op == XIR_GUARD_TAG || ins->op == XIR_GUARD_SHAPE ||
                               ins->op == XIR_GUARD_NONNULL || ins->op == XIR_GUARD_CLASS ||
                               ins->op == XIR_GUARD_KLASS) {
                        can_hoist = true;
                        for (int a = 0; a < 2; a++) {
                            if (!licm_ref_outside(ins->args[a], def_block, nv, in_loop, nblk)) {
                                can_hoist = false;
                                break;
                            }
                        }
                    }

                    if (can_hoist) {
                        xir_emit_raw(func, preheader, ins->op, ins->rep, ins->dst, ins->args[0],
                                     ins->args[1]);
                        preheader->ins[preheader->nins - 1].flags = ins->flags;
                        if (xir_ref_is_vreg(ins->dst)) {
                            uint32_t di = XIR_REF_INDEX(ins->dst);
                            if (di < nv)
                                def_block[di] = preh_bi;
                        }
                        hoisted_any = true;
                        any_hoisted = true;
                    } else {
                        if (write != k)
                            loop_blk->ins[write] = loop_blk->ins[k];
                        write++;
                    }
                }
                loop_blk->nins = write;
            }

            if (!hoisted_any)
                break;
        }

        xr_free(stores);
    }

    xr_free(def_block);
    xr_free(in_loop);
    return any_hoisted ? (XirPassChange){false, true, false, 0, 0, 0} : xir_pass_no_change();
}

/* ========== Canonicalize ========== */

/*
 * Normalize IR instructions for better optimization opportunities.
 *
 * Dart runs Canonicalize 8 times throughout its pipeline. This pass
 * complements strength_reduce (which handles identity/absorbing) with:
 *
 * 1. Commutative normalization: constant on right (for CSE/GVN matching)
 * 2. Double negation: NEG(NEG(x)) → x, NOT(NOT(x)) → x
 * 3. Redundant BOX/UNBOX: UNBOX(BOX(x)) → x, BOX(UNBOX(x)) → x
 * 4. Redundant MOV chains: MOV(MOV(x)) → MOV(x)
 * 5. Comparison with self: EQ(x,x) → 1, NE(x,x) → 0
 * 6. Float identity: FADD(x,0.0)→x, FMUL(x,1.0)→x, FSUB(x,0.0)→x
 */
XirPassChange xir_pass_canonicalize(XirFunc *func) {
    if (!func || func->nvreg == 0)
        return xir_pass_no_change();

    bool changed = false;
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        if (!blk)
            continue;

        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];
            if (!xir_ref_is_vreg(ins->dst))
                continue;

            uint16_t prev_op = ins->op;
            XirRef prev_a0 = ins->args[0], prev_a1 = ins->args[1];

            switch (ins->op) {
                // Commutative normalization: constant vreg on right side
                case XIR_ADD:
                case XIR_MUL:
                case XIR_AND:
                case XIR_OR:
                case XIR_XOR:
                case XIR_FADD:
                case XIR_FMUL:
                    if (xir_ref_is_vreg(ins->args[0]) && xir_ref_is_vreg(ins->args[1])) {
                        if (ins->args[0] > ins->args[1]) {
                            XirRef tmp = ins->args[0];
                            ins->args[0] = ins->args[1];
                            ins->args[1] = tmp;
                        }
                    }
                    break;

                // Double negation: NEG(NEG(x)) → MOV x
                case XIR_NEG: {
                    if (!xir_ref_is_vreg(ins->args[0]))
                        break;
                    uint32_t ai = XIR_REF_INDEX(ins->args[0]);
                    if (ai >= func->nvreg)
                        break;
                    XirIns *adef = func->vregs[ai].def;
                    if (adef && adef->op == XIR_NEG && xir_ref_is_vreg(adef->args[0])) {
                        ins->op = XIR_MOV;
                        ins->args[0] = adef->args[0];
                    }
                    break;
                }

                // NOT(NOT(x)) → MOV x
                case XIR_NOT: {
                    if (!xir_ref_is_vreg(ins->args[0]))
                        break;
                    uint32_t ai = XIR_REF_INDEX(ins->args[0]);
                    if (ai >= func->nvreg)
                        break;
                    XirIns *adef = func->vregs[ai].def;
                    if (adef && adef->op == XIR_NOT && xir_ref_is_vreg(adef->args[0])) {
                        ins->op = XIR_MOV;
                        ins->args[0] = adef->args[0];
                    }
                    break;
                }

                // FNEG(FNEG(x)) → MOV x
                case XIR_FNEG: {
                    if (!xir_ref_is_vreg(ins->args[0]))
                        break;
                    uint32_t ai = XIR_REF_INDEX(ins->args[0]);
                    if (ai >= func->nvreg)
                        break;
                    XirIns *adef = func->vregs[ai].def;
                    if (adef && adef->op == XIR_FNEG && xir_ref_is_vreg(adef->args[0])) {
                        ins->op = XIR_MOV;
                        ins->rep = XR_REP_F64;
                        ins->args[0] = adef->args[0];
                    }
                    break;
                }

                // UNBOX_I64(BOX_I64(x)) → MOV x
                case XIR_UNBOX_I64: {
                    if (!xir_ref_is_vreg(ins->args[0]))
                        break;
                    uint32_t ai = XIR_REF_INDEX(ins->args[0]);
                    if (ai >= func->nvreg)
                        break;
                    XirIns *adef = func->vregs[ai].def;
                    if (adef && adef->op == XIR_BOX_I64 && xir_ref_is_vreg(adef->args[0])) {
                        ins->op = XIR_MOV;
                        ins->args[0] = adef->args[0];
                    }
                    break;
                }

                // UNBOX_F64(BOX_F64(x)) → MOV x
                case XIR_UNBOX_F64: {
                    if (!xir_ref_is_vreg(ins->args[0]))
                        break;
                    uint32_t ai = XIR_REF_INDEX(ins->args[0]);
                    if (ai >= func->nvreg)
                        break;
                    XirIns *adef = func->vregs[ai].def;
                    if (adef && adef->op == XIR_BOX_F64 && xir_ref_is_vreg(adef->args[0])) {
                        ins->op = XIR_MOV;
                        ins->rep = XR_REP_F64;
                        ins->args[0] = adef->args[0];
                    }
                    break;
                }

                // BOX_I64(UNBOX_I64(x)) → MOV x
                case XIR_BOX_I64: {
                    if (!xir_ref_is_vreg(ins->args[0]))
                        break;
                    uint32_t ai = XIR_REF_INDEX(ins->args[0]);
                    if (ai >= func->nvreg)
                        break;
                    XirIns *adef = func->vregs[ai].def;
                    if (adef && adef->op == XIR_UNBOX_I64 && xir_ref_is_vreg(adef->args[0])) {
                        ins->op = XIR_MOV;
                        ins->rep = XR_REP_TAGGED;
                        ins->args[0] = adef->args[0];
                    }
                    break;
                }

                // BOX_F64(UNBOX_F64(x)) → MOV x
                case XIR_BOX_F64: {
                    if (!xir_ref_is_vreg(ins->args[0]))
                        break;
                    uint32_t ai = XIR_REF_INDEX(ins->args[0]);
                    if (ai >= func->nvreg)
                        break;
                    XirIns *adef = func->vregs[ai].def;
                    if (adef && adef->op == XIR_UNBOX_F64 && xir_ref_is_vreg(adef->args[0])) {
                        ins->op = XIR_MOV;
                        ins->rep = XR_REP_TAGGED;
                        ins->args[0] = adef->args[0];
                    }
                    break;
                }

                // Comparison with self: EQ(x,x) → 1, NE(x,x) → 0, LE(x,x) → 1, etc.
                case XIR_EQ:
                case XIR_NE:
                case XIR_LT:
                case XIR_LE:
                case XIR_GT:
                case XIR_GE:
                case XIR_FEQ:
                case XIR_FNE:
                case XIR_FLT:
                case XIR_FLE: {
                    // Commutative normalization for EQ/NE/FEQ/FNE
                    if (xir_op_is_commutative(ins->op) && xir_ref_is_vreg(ins->args[0]) &&
                        xir_ref_is_vreg(ins->args[1]) && ins->args[0] > ins->args[1]) {
                        XirRef tmp = ins->args[0];
                        ins->args[0] = ins->args[1];
                        ins->args[1] = tmp;
                    }
                    // Self-comparison folding
                    if (ins->args[0] == ins->args[1]) {
                        bool is_true =
                            (ins->op == XIR_EQ || ins->op == XIR_LE || ins->op == XIR_GE ||
                             ins->op == XIR_FEQ || ins->op == XIR_FLE);
                        XirRef cval = xir_const_i64(func, is_true ? 1 : 0);
                        ins->op = XIR_CONST_I64;
                        ins->rep = XR_REP_I64;
                        ins->args[0] = cval;
                        ins->args[1] = XIR_NONE;
                    }
                    break;
                }

                // Copy chain: copy(copy(x)) → copy(x) for MOV/REDEFINE
                case XIR_MOV:
                case XIR_REDEFINE: {
                    if (!xir_ref_is_vreg(ins->args[0]))
                        break;
                    uint32_t ai = XIR_REF_INDEX(ins->args[0]);
                    if (ai >= func->nvreg)
                        break;
                    XirIns *adef = func->vregs[ai].def;
                    if (adef && xir_op_is_copy(adef->op) && xir_ref_is_vreg(adef->args[0])) {
                        ins->args[0] = adef->args[0];
                    }
                    break;
                }

                // I2F(F2I(x)) → MOV x (roundtrip conversion, lossy but common)
                case XIR_I2F: {
                    if (!xir_ref_is_vreg(ins->args[0]))
                        break;
                    uint32_t ai = XIR_REF_INDEX(ins->args[0]);
                    if (ai >= func->nvreg)
                        break;
                    XirIns *adef = func->vregs[ai].def;
                    if (adef && adef->op == XIR_F2I && xir_ref_is_vreg(adef->args[0])) {
                        ins->op = XIR_MOV;
                        ins->rep = XR_REP_F64;
                        ins->args[0] = adef->args[0];
                    }
                    break;
                }

                // FSUB(x, x) → CONST_F64(0.0)
                case XIR_FSUB: {
                    if (ins->args[0] == ins->args[1]) {
                        XirRef zero = xir_const_f64(func, 0.0);
                        ins->op = XIR_CONST_F64;
                        ins->rep = XR_REP_F64;
                        ins->args[0] = zero;
                        ins->args[1] = XIR_NONE;
                    }
                    break;
                }

                // FDIV(x, x) → CONST_F64(1.0) — NaN-safe: skip if x could be 0/NaN
                case XIR_FDIV: {
                    if (ins->args[0] == ins->args[1]) {
                        /* Conservative: only fold if operand is known non-zero.
                         * For now, skip this optimization (NaN safety). */
                    }
                    break;
                }

                default:
                    break;
            }

            if (ins->op != prev_op || ins->args[0] != prev_a0 || ins->args[1] != prev_a1)
                changed = true;
        }
    }
    return changed ? (XirPassChange){false, false, true, 0, 0, 0} : xir_pass_no_change();
}

/* ========== Dead Store Elimination ========== */

/*
 * Per-block elimination of redundant STORE_FIELD instructions.
 *
 * A store is dead if a later store to the same (obj, offset) pair
 * exists in the same block with no intervening:
 *   - LOAD_FIELD from the same (obj, offset)
 *   - GC-triggering instruction (CALL_C, CALL_KNOWN, ALLOC, SAFEPOINT)
 *   - Any instruction that may alias (conservative for now)
 *
 * Algorithm: backward scan per block. Track last-seen stores per
 * (obj, offset). When a store is encountered and a later store to
 * the same location is tracked, the current store is dead.
 */

typedef struct {
    XirRef obj;
    XirRef offset;     // const ref for byte offset
    uint32_t ins_idx;  // instruction index in block
} DseEntry;

XirPassChange xir_pass_dse(XirFunc *func) {
    if (!func)
        return xir_pass_no_change();

    /* Reusable heap buffer: grows once and is recycled across blocks,
     * so we pay the allocation cost at most O(log) times per function
     * regardless of how many blocks contain STORE_FIELD. */
    DseEntry *tracked = NULL;
    uint32_t tracked_cap = 0;
    uint32_t n_killed = 0;

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        if (!blk || blk->nins == 0)
            continue;

        /* Worst case: every instruction in this block is a store to a
         * distinct (obj, offset), so the tracker never exceeds
         * blk->nins entries. */
        if (blk->nins > tracked_cap) {
            uint32_t new_cap = tracked_cap ? tracked_cap : 32;
            while (new_cap < blk->nins)
                new_cap <<= 1;
            XR_REALLOC_OR_ABORT(tracked, new_cap * sizeof(DseEntry), "DSE tracked stores");
            tracked_cap = new_cap;
        }
        uint32_t ntracked = 0;

        // Backward scan: track stores from the end
        for (int32_t i = (int32_t) blk->nins - 1; i >= 0; i--) {
            XirIns *ins = &blk->ins[i];

            /* GC trigger or call invalidates all tracked stores
             * (may observe stored values through aliases) */
            if (ins->op == XIR_CALL_C || ins->op == XIR_CALL_KNOWN ||
                ins->op == XIR_CALL_KNOWN_REG || ins->op == XIR_CALL_DIRECT ||
                ins->op == XIR_CALL_SELF_DIRECT || ins->op == XIR_ALLOC ||
                ins->op == XIR_SAFEPOINT || ins->op == XIR_CALL_C_LEAF) {
                ntracked = 0;
                continue;
            }

            // LOAD_FIELD: invalidate tracked stores to the same (obj, offset)
            if (ins->op == XIR_LOAD_FIELD) {
                XirRef lobj = ins->args[0];
                XirRef loff = ins->dst;  // LOAD_FIELD dst encodes offset
                for (uint32_t t = 0; t < ntracked;) {
                    if (tracked[t].obj == lobj && tracked[t].offset == loff) {
                        tracked[t] = tracked[--ntracked];
                    } else {
                        t++;
                    }
                }
                continue;
            }

            // STORE_FIELD: check if a later store to same (obj, offset) exists
            if (ins->op == XIR_STORE_FIELD) {
                XirRef sobj = ins->args[0];
                XirRef soff = ins->dst;  // packed (tag, offset)

                // Check if this store is dead (overwritten by a later store)
                bool is_dead = false;
                for (uint32_t t = 0; t < ntracked; t++) {
                    if (tracked[t].obj == sobj && tracked[t].offset == soff) {
                        is_dead = true;
                        break;
                    }
                }

                if (is_dead) {
                    // Kill this store — replace with NOP
                    n_killed++;
                    ins->op = XIR_NOP;
                    ins->dst = XIR_NONE;
                    ins->args[0] = XIR_NONE;
                    ins->args[1] = XIR_NONE;
                    ins->flags = 0;
                } else {
                    // Track this store as the latest to (obj, offset).
                    // Capacity is guaranteed >= blk->nins so no overflow.
                    tracked[ntracked].obj = sobj;
                    tracked[ntracked].offset = soff;
                    tracked[ntracked].ins_idx = (uint32_t) i;
                    ntracked++;
                }
            }
        }
    }

    xr_free(tracked);
    return n_killed ? (XirPassChange){false, true, false, n_killed, 0, 0} : xir_pass_no_change();
}

/* ========== Pipeline Runner ========== */

/* After every pass we rebuild vreg.def pointers and drop the cached
 * dominator / loop / def-use analyses so the next consumer sees a
 * coherent snapshot. Blanket invalidation is correct; a fine-grained
 * change tracker could be added if profiling shows it matters. */
#define XIR_RESET_ANALYSIS(fn)                                                                     \
    do {                                                                                           \
        xir_rebuild_vreg_defs(fn);                                                                 \
        xir_func_invalidate_loops(fn); /* transitively dom */                                      \
        xir_func_invalidate_defuse(fn);                                                            \
        xir_func_invalidate_alias(fn);                                                             \
    } while (0)

/* ========== FixedPoint pipeline driver ========== */

#ifndef NDEBUG
/* Count instructions with XIR_FLAG_SIDE_EFFECT.  Used by the
 * XIR_PASS_VERIFY_SE check: passes like DCE/CSE/GVN must never
 * reduce this count (doing so means a side-effectful instruction
 * was incorrectly removed or merged). */
static uint32_t xir_count_se_(XirFunc *fn) {
    uint32_t n = 0;
    for (uint32_t bi = 0; bi < fn->nblk; bi++)
        for (uint32_t ii = 0; ii < fn->blocks[bi]->nins; ii++)
            if (fn->blocks[bi]->ins[ii].flags & XIR_FLAG_SIDE_EFFECT)
                n++;
    return n;
}
#endif

/* Invoke a single pass descriptor, honouring its flags. */
static XirPassChange fixedpoint_invoke(XirFunc *func, XrProto *proto, const XirPassDesc *d) {
#ifndef NDEBUG
    uint32_t se_pre = (d->flags & XIR_PASS_VERIFY_SE) ? xir_count_se_(func) : 0;
#endif
    XirPassChange ch;
    if (d->flags & XIR_PASS_NEEDS_PROTO) {
        ch = d->fn.p ? d->fn.p(func, proto) : xir_pass_no_change();
    } else {
        ch = d->fn.v ? d->fn.v(func) : xir_pass_no_change();
    }
    if (!(d->flags & XIR_PASS_NO_RESET) &&
        (ch.cfg_changed || ch.vreg_defs_changed || ch.ins_changed))
        XIR_RESET_ANALYSIS(func);
#ifndef NDEBUG
    if (d->flags & XIR_PASS_VERIFY_SE) {
        uint32_t se_post = xir_count_se_(func);
        if (se_post < se_pre) {
            fprintf(stderr, "[SE VERIFY] %s: %s removed %u SE instructions (%u -> %u)\n",
                    func->name ? func->name : "?", d->name, se_pre - se_post, se_pre, se_post);
            XR_DCHECK(false, "pass removed side-effect instructions");
        }
    }
    if (!(d->flags & XIR_PASS_SKIP_CFG_VERIFY)) {
        if (ch.cfg_changed)
            xir_rebuild_preds(func);
        xir_verify_cfg(func);
    }
    xir_verify_types(func);
#endif
    return ch;
}

XirPipelineStats xir_run_fixedpoint(XirFunc *func, XrProto *proto, const XirPassDesc *passes,
                                    uint32_t npass, uint32_t max_rounds, XirCompileBudget *budget) {
    XirPipelineStats st = {0, 0, 0, 0};
    if (!func || !passes || npass == 0 || max_rounds == 0)
        return st;

    for (uint32_t round = 0; round < max_rounds; round++) {
        // Check compile-time budget at each round boundary
        if (budget && !budget->timed_out) {
            uint64_t now_ms = xr_time_monotonic_ms();
            if (now_ms * 1000000ULL > budget->deadline_ns) {
                budget->timed_out = true;
                st.timed_out = 1;
                break;
            }
        }
        bool any_change = false;
        for (uint32_t i = 0; i < npass; i++) {
            XirPassChange ch = fixedpoint_invoke(func, proto, &passes[i]);
            if (ch.cfg_changed || ch.vreg_defs_changed || ch.ins_changed)
                any_change = true;
            st.invocations++;
        }
        st.rounds_run = round + 1;
        if (!any_change) {
            st.converged = 1;
            break;
        }
    }
    return st;
}

/* ========== Declarative pipeline groups ==========
 *
 * Each group corresponds to a logical phase of the historical
 * pipeline.  Groups are driven by xir_run_fixedpoint with a modest
 * round cap so cross-pass interactions (e.g. SCCP exposing a
 * canonicalize opportunity that exposes more CSE) reach a stable
 * point before we move on.  The max_rounds values are engineering
 * compromises: higher values buy more optimisation at measurable
 * compile-time cost; the current settings were picked to keep
 * compile time on par with the legacy linear pipeline while giving
 * the new algorithms room to converge.
 */

/* Canonicalising group: runs at every optimisation level to fold
 * constants, propagate types, drop dead code. */
static const XirPassDesc PG_CANON[] = {
    {"select_rep", {.v = xir_pass_select_rep}, XIR_PASS_SKIP_CFG_VERIFY},
    {"sccp", {.v = xir_pass_sccp}, 0},
    {"type_prop", {.v = xir_pass_type_prop}, XIR_PASS_SKIP_CFG_VERIFY},
    {"specialize", {.v = xir_pass_specialize}, 0},
    {"canonicalize", {.v = xir_pass_canonicalize}, 0},
    {"dce", {.v = xir_pass_dce}, XIR_PASS_VERIFY_SE},
};

/* Basic-level local cleanups: per-block CSE, simple copy folding,
 * escape-analysis (block-local), store-to-load forwarding. */
static const XirPassDesc PG_BASIC[] = {
    {"cse", {.v = xir_pass_cse}, XIR_PASS_VERIFY_SE},
    {"store_to_load", {.v = xir_pass_store_to_load}, 0},
    {"escape_analysis", {.v = xir_pass_escape_analysis}, 0},
    {"phi_simp", {.v = xir_pass_phi_simp}, 0},
    {"copy_prop", {.v = xir_pass_copy_prop}, XIR_PASS_VERIFY_SE},
    {"dce", {.v = xir_pass_dce}, XIR_PASS_VERIFY_SE},
};

/* Loop and cross-block optimisations.  Ordered so SCCP sees the
 * results of GVN/LICM, and canon + type_prop fire again before
 * ifconvert / phi_simp. */
static const XirPassDesc PG_LOOP[] = {
    {"licm", {.v = xir_pass_licm}, 0},
    {"elim_guards", {.v = xir_pass_elim_guards}, 0},
    {"gvn", {.v = xir_pass_gvn}, XIR_PASS_VERIFY_SE},
    {"gcm", {.v = xir_pass_gcm}, 0},
    {"propjnz", {.v = xir_pass_propjnz}, 0},
    {"store_to_load", {.v = xir_pass_store_to_load}, 0},
    {"dse", {.v = xir_pass_dse}, 0},
    {"sccp", {.v = xir_pass_sccp}, 0},
    {"type_prop", {.v = xir_pass_type_prop}, 0},
    {"specialize", {.v = xir_pass_specialize}, 0},
    {"elim_guards", {.v = xir_pass_elim_guards}, 0},
    {"ifconvert", {.v = xir_pass_ifconvert}, 0},
    {"phi_simp", {.v = xir_pass_phi_simp}, 0},
    {"copy_prop", {.v = xir_pass_copy_prop}, XIR_PASS_VERIFY_SE},
};

/* Final cleanup after all heavy optimisation: canon + alloc sink +
 * escape (second go) + DCE. */
static const XirPassDesc PG_CLEANUP[] = {
    {"canonicalize", {.v = xir_pass_canonicalize}, 0},
    {"alloc_sink", {.v = xir_pass_alloc_sink}, 0},
    {"escape_analysis", {.v = xir_pass_escape_analysis}, 0},
    {"dce", {.v = xir_pass_dce}, XIR_PASS_VERIFY_SE},
};

static const XirPassDesc PG_RANGE[] = {
    {"insert_redefines", {.v = xir_pass_insert_redefines}, 0},
    {"range_analysis", {.v = xir_pass_range_analysis}, 0},
    {"dce", {.v = xir_pass_dce}, XIR_PASS_VERIFY_SE},
    {"split_critical_edges", {.v = xir_pass_split_critical_edges}, 0},
};

#define XIR_PIPELINE_SIZE(arr) (uint32_t)(sizeof(arr) / sizeof((arr)[0]))

/* Cached pipeline-verbose toggle (XRAY_JIT_PIPELINE_VERBOSE).  Read
 * once on first use; suitable for debugging / tuning runs where
 * users opt in via environment variable. */
static int pipeline_verbose_cached = -1;
static bool pipeline_verbose(void) {
    if (pipeline_verbose_cached < 0)
        pipeline_verbose_cached = getenv("XRAY_JIT_PIPELINE_VERBOSE") ? 1 : 0;
    return pipeline_verbose_cached != 0;
}

/* Thin wrapper around xir_run_fixedpoint that logs stats when the
 * pipeline-verbose toggle is on.  Keeps xir_run_pipeline_ex free of
 * instrumentation clutter. */
static void run_group(const char *group_name, XirFunc *func, XrProto *proto,
                      const XirPassDesc *passes, uint32_t npass, uint32_t max_rounds,
                      XirCompileBudget *budget) {
    // Skip group entirely if budget already exhausted
    if (budget && budget->timed_out)
        return;
    XirPipelineStats s = xir_run_fixedpoint(func, proto, passes, npass, max_rounds, budget);
    if (pipeline_verbose()) {
        fprintf(stderr, "[jit-pipe] %-7s func=%s passes=%u rounds=%u invocations=%u%s\n",
                group_name, func->name ? func->name : "?", npass, s.rounds_run, s.invocations,
                s.converged   ? " converged"
                : s.timed_out ? " budget-exceeded"
                              : " max-rounds");
    }
}

void xir_run_pipeline_ex(XirFunc *func, XirOptLevel opt, XrProto *proto) {
    if (!func)
        return;

    // Compile-time budget: 50ms default (skip remaining groups on timeout)
    XirCompileBudget budget;
    budget.start_ns = xr_time_monotonic_ns();
    budget.deadline_ns = budget.start_ns + 50ULL * 1000000ULL;  // 50ms
    budget.timed_out = false;

    /* Canon group: initial type/rep analysis + DCE. Runs even at -O0
     * because the rest of the compiler expects vreg representations to
     * be resolved. */
    run_group("canon", func, proto, PG_CANON, XIR_PIPELINE_SIZE(PG_CANON), 3, &budget);

    if (opt >= XIR_OPT_BASIC && !budget.timed_out) {
        xir_rebuild_preds(func);
        xir_verify_cfg(func);
        xir_verify_types(func);
        run_group("basic", func, proto, PG_BASIC, XIR_PIPELINE_SIZE(PG_BASIC), 3, &budget);
    }

    if (opt >= XIR_OPT_FULL && !budget.timed_out) {
        if (proto) {
            (void) xir_pass_auto_inline(func, proto);
            XIR_RESET_ANALYSIS(func);
            /* Re-canon once post-inline so the loop group below sees
             * the callee's constants / types. */
            run_group("post-inline", func, proto, PG_CANON, XIR_PIPELINE_SIZE(PG_CANON), 2,
                      &budget);
        }

        run_group("loop", func, proto, PG_LOOP, XIR_PIPELINE_SIZE(PG_LOOP), 3, &budget);

        if (!budget.timed_out) {
            /* CFG cleanup between the heavy and the range-analysis
             * phases keeps subsequent checks on a well-formed CFG. */
            xir_pass_merge_blocks(func);
            xir_rebuild_preds(func);
            xir_verify_cfg(func);
            xir_verify_types(func);

            run_group("cleanup", func, proto, PG_CLEANUP, XIR_PIPELINE_SIZE(PG_CLEANUP), 2,
                      &budget);
            run_group("range", func, proto, PG_RANGE, XIR_PIPELINE_SIZE(PG_RANGE), 2, &budget);
        }
    }

    if (budget.timed_out && pipeline_verbose()) {
        uint64_t elapsed_ms = xr_time_monotonic_ms() - budget.start_ns / 1000000ULL;
        fprintf(stderr, "[jit-pipe] TIMEOUT func=%s after %llums\n", func->name ? func->name : "?",
                (unsigned long long) elapsed_ms);
    }

    /* Trailing housekeeping: block reordering + write barriers.
     * These are deterministic and run exactly once (always run). */
    (void) xir_pass_reorder_blocks(func);
    XIR_RESET_ANALYSIS(func);
    (void) xir_insert_write_barriers(func);
    XIR_RESET_ANALYSIS(func);
    (void) xir_pass_elim_write_barriers(func);
    XIR_RESET_ANALYSIS(func);
}

void xir_run_pipeline(XirFunc *func, XirOptLevel opt) {
    xir_run_pipeline_ex(func, opt, NULL);
}
