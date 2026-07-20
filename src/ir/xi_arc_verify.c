/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_arc_verify.c - Independent RC / ownership contract verifier (task 219)
 *
 * See xi_arc_verify.h for the contract summary. This is an N-version checker:
 * it re-derives ownership facts from the IR + the ops.def ownership table and
 * shares no analysis code with xi_arc.c.
 *
 * GRANULARITY — net reference delta per owning reference.
 *   Reference counting lets one heap object have several live references at once
 *   and lets ARC pre-pay for a later consume with a RETAIN. A bare "release(v)
 *   then read v" is therefore NOT necessarily a bug: a preceding RETAIN (the
 *   Perceus dup `RETAIN(x); y = MOVE(x)`), a surviving alias, or a shared
 *   constant reused on many paths can all keep the object live. Flagging those
 *   would false-positive on correct ARC output.
 *
 *   The verifier instead tracks, per SSA owning reference and per program
 *   point, the NET delta = (#RETAIN - #RELEASE) since the value's definition.
 *   delta < 0 means releases have outrun retains, i.e. the value's own
 *   reference has been dropped past balance. That is the sound, baseline-free
 *   signal:
 *     C1  a PHI merges an input whose delta < 0 on that predecessor edge
 *         (ARC dropped a value it still flows forward — the loop-header conn
 *          phi it never scanned, incident 1).
 *     C2  a RELEASE executes while delta is already < 0 (double release).
 *   Both are unconditionally wrong regardless of how many references exist,
 *   because they are measured relative to each reference's own definition.
 *   The C3 borrow-closure and C4 dominance checks (P2) cover the released-owner
 *   / live-view and non-dominating-join cases.
 *
 * Plus two structural C5 checks that need no dataflow:
 *   C5  an operand pointing at a value with no live definition (stale user,
 *       incident 4) and a RETAIN/RELEASE on a non-RC value (metadata, the
 *       incident-5 class).
 */

#include "xi_arc_verify.h"
#include "xi_own.h"
#include "xi_analysis.h"
#include "xi_ops_gen.h"
#include "xi_op_name.h"
#include "../runtime/value/xtype.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"

#include <stdio.h>
#include <string.h>

/* ========== Verifier state ========== */

typedef struct ArcVerify {
    XiFunc *f;
    uint32_t n;         /* value-id count (f->next_value_id) */
    XiValue **def;      /* def[id] = defining value pointer (NULL = none) */
    bool *is_rc;        /* is_rc[id] = RC-managed */
    int32_t *track;     /* track[id] = compact delta index, or -1 (not retained/released) */
    uint32_t ntracked;  /* number of distinct retained/released references */
    int16_t *delta_in;  /* [nblocks*ntracked] net (retain-release) on entry (min over preds) */
    int16_t *delta_out; /* [nblocks*ntracked] net delta on exit */
    XiArcVerifyReport *rep;
} ArcVerify;

/* ========== Reporting ========== */

static const struct {
    XiArcContract c;
    const char *name;
} k_contract_names[] = {
    {XI_ARC_CONTRACT_NONE, "OK"},
    {XI_ARC_C1_USE_AFTER_RELEASE, "C1 (use-after-release)"},
    {XI_ARC_C2_DOUBLE_RELEASE, "C2 (double-release)"},
    {XI_ARC_C2_BALANCE, "C2 (path-balance)"},
    {XI_ARC_C3_BORROW_ESCAPE, "C3 (borrow-closure)"},
    {XI_ARC_C4_DOMINANCE, "C4 (dominance-boundary)"},
    {XI_ARC_C5_STALE_USE, "C5 (stale-use / SSA-completeness)"},
    {XI_ARC_C5_METADATA, "C5 (metadata-consistency)"},
};

XR_FUNC const char *xi_arc_contract_name(XiArcContract c) {
    for (size_t i = 0; i < sizeof(k_contract_names) / sizeof(k_contract_names[0]); i++) {
        if (k_contract_names[i].c == c)
            return k_contract_names[i].name;
    }
    return "?";
}

/* BFS shortest CFG path from block `from_id` to `to_id`, written to buf as
 * "b3->b5->b8". Safe no-op if either id is out of range. */
static void write_shortest_path(XiFunc *f, uint32_t from_id, uint32_t to_id, char *buf,
                                size_t buflen) {
    buf[0] = '\0';
    if (from_id >= f->nblocks || to_id >= f->nblocks)
        return;
    uint32_t *parent = (uint32_t *) xr_malloc(f->nblocks * sizeof(uint32_t));
    uint32_t *queue = (uint32_t *) xr_malloc(f->nblocks * sizeof(uint32_t));
    if (!parent || !queue) {
        xr_free(parent);
        xr_free(queue);
        return;
    }
    for (uint32_t i = 0; i < f->nblocks; i++)
        parent[i] = UINT32_MAX;
    uint32_t head = 0, tail = 0;
    queue[tail++] = from_id;
    parent[from_id] = from_id;
    bool found = (from_id == to_id);
    while (head < tail && !found) {
        uint32_t cur = queue[head++];
        XiBlock *blk = f->blocks[cur];
        if (!blk)
            continue;
        for (int s = 0; s < 2; s++) {
            XiBlock *sc = blk->succs[s];
            if (!sc || sc->id >= f->nblocks || parent[sc->id] != UINT32_MAX)
                continue;
            parent[sc->id] = cur;
            if (sc->id == to_id) {
                found = true;
                break;
            }
            queue[tail++] = sc->id;
        }
    }
    if (found) {
        uint32_t chain[64];
        uint32_t len = 0;
        uint32_t cur = to_id;
        while (len < 64) {
            chain[len++] = cur;
            if (cur == from_id)
                break;
            cur = parent[cur];
            if (cur == UINT32_MAX)
                break;
        }
        size_t off = 0;
        for (uint32_t i = len; i > 0 && off + 12 < buflen; i--) {
            off += (size_t) snprintf(buf + off, buflen - off, "%sb%u", (i == len) ? "" : "->",
                                     chain[i - 1]);
        }
    }
    xr_free(parent);
    xr_free(queue);
}

static bool report_violation(ArcVerify *av, XiArcContract contract, uint32_t value_id,
                             uint32_t release_blk, uint32_t use_blk, const char *detail) {
    if (!av->rep)
        return false;
    XiArcVerifyReport *r = av->rep;
    r->ok = false;
    r->contract = contract;
    r->func = av->f;
    r->func_name = av->f->name ? av->f->name : "<anon>";
    r->value_id = value_id;
    r->release_blk = release_blk;
    r->use_blk = use_blk;
    snprintf(r->message, sizeof(r->message), "%s: v%u in '%s'%s%s", xi_arc_contract_name(contract),
             value_id, r->func_name, detail && detail[0] ? " — " : "", detail ? detail : "");
    if (release_blk != UINT32_MAX && use_blk != UINT32_MAX)
        write_shortest_path(av->f, release_blk, use_blk, r->path, sizeof(r->path));
    else
        r->path[0] = '\0';
    return false;
}

/* Find any block that releases owning reference `vid`, for counterexample paths. */
static uint32_t find_release_block(ArcVerify *av, uint32_t vid) {
    for (uint32_t b = 0; b < av->f->nblocks; b++) {
        XiBlock *blk = av->f->blocks[b];
        if (!blk || blk->rpo == 0)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (v && v->op == XI_RELEASE && v->nargs >= 1 && v->args[0] && v->args[0]->id == vid)
                return blk->id;
        }
    }
    return UINT32_MAX;
}

/* ========== Registration + tracked-set ========== */

static void register_def(ArcVerify *av, XiValue *v) {
    if (!v || v->id >= av->n)
        return;
    av->def[v->id] = v;
    av->is_rc[v->id] = xi_own_type_is_rc(v->type);
}

static void build_defs(ArcVerify *av) {
    XiFunc *f = av->f;
    for (uint16_t p = 0; p < f->nparams; p++)
        register_def(av, f->params[p]);
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (XiPhi *phi = blk->phis; phi; phi = phi->next)
            register_def(av, &phi->value);
        for (uint32_t i = 0; i < blk->nvalues; i++)
            register_def(av, blk->values[i]);
    }
}

/* Assign a compact delta index to every value that is a RETAIN/RELEASE target;
 * only those can ever reach delta < 0, so the delta dataflow tracks just them. */
static void build_tracked(ArcVerify *av) {
    XiFunc *f = av->f;
    av->ntracked = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v || (v->op != XI_RETAIN && v->op != XI_RELEASE) || v->nargs < 1)
                continue;
            XiValue *x = v->args[0];
            if (x && x->id < av->n && av->is_rc[x->id] && av->track[x->id] < 0)
                av->track[x->id] = (int32_t) av->ntracked++;
        }
    }
}

/* ========== C5 (structural) ========== */

/* C5 (SSA completeness): an operand that points at a value with no live def of
 * the same pointer is a stale user (incident 4: trivial-phi elim leaving stale
 * users). */
static bool check_operand_defined(ArcVerify *av, XiBlock *blk, XiValue *user, XiValue *arg,
                                  const char *kind) {
    if (!arg)
        return true;
    if (arg->id >= av->n || av->def[arg->id] != arg) {
        char detail[160];
        snprintf(detail, sizeof(detail), "%s v%u of %s references a value with no live def", kind,
                 arg->id, user ? xi_op_name(user->op) : "?");
        return report_violation(av, XI_ARC_C5_STALE_USE, arg->id, UINT32_MAX, blk->id, detail);
    }
    return true;
}

static bool check_stale_uses(ArcVerify *av) {
    XiFunc *f = av->f;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk || blk->rpo == 0)
            continue; /* dead code: skip */
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (!check_operand_defined(av, blk, &phi->value, phi->value.args[a], "phi input"))
                    return false;
            }
        }
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (!check_operand_defined(av, blk, v, v->args[a], "operand"))
                    return false;
            }
        }
        if (blk->control && !check_operand_defined(av, blk, NULL, blk->control, "control"))
            return false;
    }
    return true;
}

/* C5 (metadata consistency): a RETAIN / RELEASE must operate on an RC-managed
 * value. Emitting a refcount primitive on a non-RC value is the incident-5
 * class of defect (an op treated as owning something it does not own). Correct
 * ARC gates every retain/release on xi_own_type_is_rc, so this is
 * zero-false-positive. */
static bool check_rc_op_metadata(ArcVerify *av) {
    XiFunc *f = av->f;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk || blk->rpo == 0)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v || (v->op != XI_RETAIN && v->op != XI_RELEASE))
                continue;
            XiValue *x = (v->nargs >= 1) ? v->args[0] : NULL;
            if (!x)
                continue;
            if (x->id >= av->n || !av->is_rc[x->id]) {
                char detail[128];
                snprintf(detail, sizeof(detail),
                         "%s applied to non-RC value v%u (ownership metadata inconsistency)",
                         xi_op_name(v->op), x->id);
                return report_violation(av, XI_ARC_C5_METADATA, x->id, UINT32_MAX, blk->id, detail);
            }
        }
    }
    return true;
}

/* ========== Net-delta dataflow (C1 / C2) ========== */

/* Apply block `blk`'s transfer to `work` (net delta per tracked reference).
 * On `report`, record the first C1/C2 violation and return false. */
static bool transfer_block(ArcVerify *av, XiBlock *blk, int16_t *work, bool report) {
    uint32_t T = av->ntracked;

    /* Phi inputs: an input whose delta is already < 0 on its predecessor edge
     * has been released past balance yet is still merged forward — C1. The phi
     * result then (re)starts a fresh reference. */
    for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
        XiValue *pv = &phi->value;
        if (report) {
            for (uint16_t a = 0; a < pv->nargs && a < blk->npreds; a++) {
                XiValue *in = pv->args[a];
                XiBlock *pred = blk->preds[a];
                if (!in || in->id >= av->n || av->track[in->id] < 0 || !pred ||
                    pred->id >= av->f->nblocks)
                    continue;
                int16_t d = av->delta_out[(size_t) pred->id * T + (uint32_t) av->track[in->id]];
                if (d < 0) {
                    char detail[128];
                    snprintf(detail, sizeof(detail),
                             "phi merges over-released value v%u from pred b%u", in->id, pred->id);
                    return report_violation(av, XI_ARC_C1_USE_AFTER_RELEASE, in->id, pred->id,
                                            blk->id, detail);
                }
            }
        }
        if (pv->id < av->n && av->track[pv->id] >= 0)
            work[av->track[pv->id]] = 0; /* reset at definition */
    }

    for (uint32_t i = 0; i < blk->nvalues; i++) {
        XiValue *v = blk->values[i];
        if (!v)
            continue;
        if (v->op == XI_RETAIN) {
            XiValue *x = (v->nargs >= 1) ? v->args[0] : NULL;
            if (x && x->id < av->n && av->track[x->id] >= 0)
                work[av->track[x->id]]++;
            continue;
        }
        if (v->op == XI_RELEASE) {
            XiValue *x = (v->nargs >= 1) ? v->args[0] : NULL;
            if (x && x->id < av->n && av->track[x->id] >= 0) {
                int32_t t = av->track[x->id];
                if (report && work[t] < 0) {
                    char detail[96];
                    snprintf(detail, sizeof(detail), "release of already-released v%u", x->id);
                    return report_violation(av, XI_ARC_C2_DOUBLE_RELEASE, x->id,
                                            find_release_block(av, x->id), blk->id, detail);
                }
                work[t]--;
            }
            continue;
        }
        /* A fresh SSA definition (re)starts the reference at delta 0; this also
         * clears any stale delta carried around a loop back-edge. */
        if (v->id < av->n && av->track[v->id] >= 0)
            work[av->track[v->id]] = 0;
    }
    return true;
}

/* delta_in[b] = min over predecessors of delta_out[pred] (worst-case path). */
static void seed_delta_in(ArcVerify *av, XiBlock *blk, int16_t *out) {
    uint32_t T = av->ntracked;
    bool any = false;
    for (uint16_t p = 0; p < blk->npreds; p++) {
        XiBlock *pred = blk->preds[p];
        if (!pred || pred->id >= av->f->nblocks || pred->rpo == 0)
            continue;
        const int16_t *po = av->delta_out + (size_t) pred->id * T;
        if (!any) {
            memcpy(out, po, T * sizeof(int16_t));
            any = true;
        } else {
            for (uint32_t t = 0; t < T; t++)
                if (po[t] < out[t])
                    out[t] = po[t];
        }
    }
    if (!any)
        memset(out, 0, T * sizeof(int16_t));
}

static bool run_delta_dataflow(ArcVerify *av) {
    XiFunc *f = av->f;
    uint32_t T = av->ntracked;
    if (T == 0)
        return true; /* no retained/released references: C1/C2 vacuously hold */

    int16_t *work = (int16_t *) xr_malloc(T * sizeof(int16_t));
    if (!work)
        return true; /* allocation failure: skip (do not falsely fail) */

    bool changed = true;
    uint32_t guard = 0;
    uint32_t max_rounds = f->nblocks + 4;
    while (changed && guard++ <= max_rounds) {
        changed = false;
        for (uint32_t b = 0; b < f->nblocks; b++) {
            XiBlock *blk = f->blocks[b];
            if (!blk || blk->rpo == 0)
                continue;
            int16_t *in = av->delta_in + (size_t) blk->id * T;
            int16_t *out = av->delta_out + (size_t) blk->id * T;
            seed_delta_in(av, blk, in);
            memcpy(work, in, T * sizeof(int16_t));
            transfer_block(av, blk, work, /*report=*/false);
            if (memcmp(out, work, T * sizeof(int16_t)) != 0) {
                memcpy(out, work, T * sizeof(int16_t));
                changed = true;
            }
        }
    }

    bool ok = true;
    for (uint32_t b = 0; b < f->nblocks && ok; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk || blk->rpo == 0)
            continue;
        int16_t *in = av->delta_in + (size_t) blk->id * T;
        seed_delta_in(av, blk, in);
        memcpy(work, in, T * sizeof(int16_t));
        ok = transfer_block(av, blk, work, /*report=*/true);
    }
    xr_free(work);
    return ok;
}

/* ========== Public API ========== */

XR_FUNC bool xi_arc_verify(XiFunc *f, XiArcVerifyReport *report) {
    XiArcVerifyReport local;
    XiArcVerifyReport *rep = report ? report : &local;
    memset(rep, 0, sizeof(*rep));
    rep->ok = true;
    rep->value_id = UINT32_MAX;
    rep->release_blk = UINT32_MAX;
    rep->use_blk = UINT32_MAX;

    XR_DCHECK(f != NULL, "xi_arc_verify: NULL func");
    if (!f || f->next_value_id == 0)
        return true;

    xi_ensure_dominators(f); /* refresh RPO + dominator caches (needed for C4) */

    ArcVerify av;
    memset(&av, 0, sizeof(av));
    av.f = f;
    av.n = f->next_value_id;
    av.rep = rep;
    av.def = (XiValue **) xr_calloc(av.n, sizeof(XiValue *));
    av.is_rc = (bool *) xr_calloc(av.n, sizeof(bool));
    av.track = (int32_t *) xr_malloc(av.n * sizeof(int32_t));
    if (!av.def || !av.is_rc || !av.track) {
        xr_free(av.def);
        xr_free(av.is_rc);
        xr_free(av.track);
        return true; /* allocation failure: do not falsely fail the compile */
    }
    for (uint32_t i = 0; i < av.n; i++)
        av.track[i] = -1;

    build_defs(&av);

    bool ok = check_stale_uses(&av); /* C5 (SSA completeness) */
    if (ok)
        ok = check_rc_op_metadata(&av); /* C5 (RC-op metadata consistency) */

    if (ok) {
        build_tracked(&av);
        av.delta_in = (int16_t *) xr_calloc((size_t) f->nblocks * (av.ntracked ? av.ntracked : 1),
                                            sizeof(int16_t));
        av.delta_out = (int16_t *) xr_calloc((size_t) f->nblocks * (av.ntracked ? av.ntracked : 1),
                                             sizeof(int16_t));
        if (av.delta_in && av.delta_out)
            ok = run_delta_dataflow(&av); /* C1 / C2 */
        xr_free(av.delta_in);
        xr_free(av.delta_out);
    }

    xr_free(av.def);
    xr_free(av.is_rc);
    xr_free(av.track);
    return ok;
}

XR_FUNC bool xi_arc_verify_tree(XiFunc *f, XiArcVerifyReport *report) {
    if (!f)
        return true;
    if (!xi_arc_verify(f, report))
        return false;
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i] && !xi_arc_verify_tree(f->children[i], report))
            return false;
    }
    return true;
}

XR_FUNC void xi_arc_verify_or_ice(XiFunc *f, const char *stage_label) {
    XiArcVerifyReport rep;
    if (xi_arc_verify_tree(f, &rep))
        return;

    fprintf(stderr,
            "\n========================================================\n"
            "[ICE] xi_arc_verify: RC contract violated after %s\n"
            "  contract : %s\n"
            "  function : %s\n"
            "  value    : v%u\n"
            "  detail   : %s\n",
            stage_label ? stage_label : "(unknown stage)", xi_arc_contract_name(rep.contract),
            rep.func_name ? rep.func_name : "<anon>", rep.value_id, rep.message);
    if (rep.release_blk != UINT32_MAX)
        fprintf(stderr, "  release  : b%u\n", rep.release_blk);
    if (rep.use_blk != UINT32_MAX)
        fprintf(stderr, "  use      : b%u\n", rep.use_blk);
    if (rep.path[0])
        fprintf(stderr, "  shortest counterexample path: %s\n", rep.path);
    fprintf(stderr, "========================================================\n");
    if (rep.func) {
        fprintf(stderr, "=== Xi IR dump: %s ===\n", rep.func_name ? rep.func_name : "<anon>");
        xi_func_dump((const XiFunc *) rep.func, stderr);
        fprintf(stderr, "======================================================\n");
    }
    fflush(stderr);
    abort();
}
