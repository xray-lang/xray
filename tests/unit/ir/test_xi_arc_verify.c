/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xi_arc_verify.c - Unit tests for the independent RC contract verifier
 * (task 219). Each historical ARC incident is fixed as a minimal violating IR
 * fixture fed directly to xi_arc_verify, asserting the corresponding contract
 * number fires. Three legal-form fixtures prove zero false positives.
 */

#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_arc_verify.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"
#include <stdio.h>
#include <string.h>

static int g_passed = 0;
static int g_failed = 0;

#define ASSERT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "  FAIL: %s\n", msg);                                                  \
            g_failed++;                                                                            \
        } else {                                                                                   \
            g_passed++;                                                                            \
        }                                                                                          \
    } while (0)

#define ASSERT_CONTRACT(f, expected, msg)                                                          \
    do {                                                                                           \
        XiArcVerifyReport _r;                                                                      \
        bool _ok = xi_arc_verify((f), &_r);                                                        \
        if (_ok || _r.contract != (expected)) {                                                    \
            fprintf(stderr, "  FAIL: %s (ok=%d contract=%s, expected %s)\n", msg, _ok,             \
                    xi_arc_contract_name(_r.contract), xi_arc_contract_name(expected));            \
            g_failed++;                                                                            \
        } else {                                                                                   \
            g_passed++;                                                                            \
        }                                                                                          \
    } while (0)

#define ASSERT_OK(f, msg)                                                                          \
    do {                                                                                           \
        XiArcVerifyReport _r;                                                                      \
        bool _ok = xi_arc_verify((f), &_r);                                                        \
        if (!_ok) {                                                                                \
            fprintf(stderr, "  FAIL: %s (false positive %s: %s)\n", msg,                           \
                    xi_arc_contract_name(_r.contract), _r.message);                                \
            g_failed++;                                                                            \
        } else {                                                                                   \
            g_passed++;                                                                            \
        }                                                                                          \
    } while (0)

/* ========== Shared type singletons ========== */

static XrType t_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType t_array = {.kind = XR_KIND_ARRAY, .id = 2, .frozen = true};
static XrType t_bool = {.kind = XR_KIND_BOOL, .id = 3, .frozen = true};
static XrType t_span = {.kind = XR_KIND_SPAN, .id = 4, .frozen = true};

static XiFunc *make_func(const char *name, XrType *ret) {
    XiFunc *f = xi_func_new(name, ret);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    return f;
}

static XiValue *rc_new(XiFunc *f, XiBlock *b) {
    return xi_value_new(f, b, XI_ARRAY_NEW, &t_array, 0);
}

static XiValue *release(XiFunc *f, XiBlock *b, XiValue *v) {
    XiValue *r = xi_value_new(f, b, XI_RELEASE, &t_int, 1);
    r->args[0] = v;
    return r;
}

static XiValue *retain(XiFunc *f, XiBlock *b, XiValue *v) {
    XiValue *r = xi_value_new(f, b, XI_RETAIN, &t_int, 1);
    r->args[0] = v;
    return r;
}

static XiValue *index_get(XiFunc *f, XiBlock *b, XiValue *base) {
    XiValue *idx = xi_const_int(f, b, 0, &t_int);
    XiValue *g = xi_value_new(f, b, XI_INDEX_GET, &t_int, 2);
    g->args[0] = base;
    g->args[1] = idx;
    return g;
}

/* A borrow view (Span) of `base`: result-ownership is BORROWED, so the verifier
 * records base as the view's owner (C3). */
static XiValue *span_view(XiFunc *f, XiBlock *b, XiValue *base) {
    XiValue *s = xi_const_int(f, b, 0, &t_int);
    XiValue *e = xi_const_int(f, b, 1, &t_int);
    XiValue *v = xi_value_new(f, b, XI_SPAN_WINDOW, &t_span, 3);
    v->args[0] = base;
    v->args[1] = s;
    v->args[2] = e;
    return v;
}

/* Remove v from blk->values[] to simulate a def that a pass eliminated while
 * leaving a stale user behind (incident 4). */
static void orphan_value(XiBlock *blk, XiValue *v) {
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        if (blk->values[i] == v) {
            for (uint32_t j = i; j + 1 < blk->nvalues; j++)
                blk->values[j] = blk->values[j + 1];
            blk->nvalues--;
            return;
        }
    }
}

/* ========== Incident 1 (C1): use after early release ========== */

/* Incident 1's real shape: conn released before a loop, merged by a header phi,
 * used inside the loop. The verifier must scan blk->phis and flag the released
 * input on its predecessor edge. (RC allows aliases/dups to keep an object
 * live, so C1 is enforced through this unconditional released-phi-input
 * signature rather than a bare release-then-read; see xi_arc_verify.c.) */
static void test_incident1_phi_released_input(void) {
    XiFunc *f = make_func("incident1_phi", &t_int);
    XiBlock *b0 = f->entry;
    XiBlock *b1 = xi_block_new(f);

    XiValue *conn = rc_new(f, b0);
    release(f, b0, conn); /* dropped too early (ARC missed the phi use) */
    xi_block_set_jump(b0, b1);

    XiPhi *p = xi_phi_new(f, b1, &t_array, b1->npreds);
    p->value.args[0] = conn;
    XiValue *u = index_get(f, b1, &p->value);
    xi_block_set_return(b1, u);

    ASSERT_CONTRACT(f, XI_ARC_C1_USE_AFTER_RELEASE, "incident1: phi merges released conn");
    xi_func_free(f);
}

/* ========== Incident 4 (C5): stale user after phi elimination ========== */

/* func: v = NEW; INDEX_GET(v) but v's def is removed from the block, leaving
 * INDEX_GET pointing at a value with no live definition. */
static void test_incident4_stale_use(void) {
    XiFunc *f = make_func("incident4_stale", &t_int);
    XiBlock *b0 = f->entry;
    XiValue *v = rc_new(f, b0);
    XiValue *u = index_get(f, b0, v);
    xi_block_set_return(b0, u);
    orphan_value(b0, v); /* pass eliminated v but left u referencing it */
    ASSERT_CONTRACT(f, XI_ARC_C5_STALE_USE, "incident4: stale user of removed def");
    xi_func_free(f);
}

/* ========== Incident 5 (C5): RC op inconsistent with ownership metadata ==== */

/* func: n = CONST 5 (int); RETAIN(n) — a refcount primitive on a non-RC value.
 * This is the incident-5 class: an op treating a borrowed / non-owned value as
 * owned. (The structural fix — fail-closed :own-use — is regression-tested in
 * xisagen's self-test; this asserts the verifier also rejects it at the IR.) */
static void test_incident5_rc_op_metadata(void) {
    XiFunc *f = make_func("incident5_meta", &t_int);
    XiBlock *b0 = f->entry;
    XiValue *n = xi_const_int(f, b0, 5, &t_int);
    retain(f, b0, n);
    xi_block_set_return(b0, n);
    ASSERT_CONTRACT(f, XI_ARC_C5_METADATA, "incident5: retain on non-RC value");
    xi_func_free(f);
}

/* ========== Incident 2 (C3): borrow view outlives released owner ========== */

/* base owner released before a loop; a Span borrow of base is merged by the
 * loop-header phi and read inside the loop → the owner is gone under the view. */
static void test_incident2_borrow_view_owner_released(void) {
    XiFunc *f = make_func("incident2_c3", &t_int);
    XiBlock *b0 = f->entry;
    XiBlock *b1 = xi_block_new(f);

    XiValue *base = rc_new(f, b0);
    XiValue *sp = span_view(f, b0, base);
    release(f, b0, base); /* owner dropped before the borrow view's loop use */
    xi_block_set_jump(b0, b1);

    XiPhi *p = xi_phi_new(f, b1, &t_span, b1->npreds);
    p->value.args[0] = sp;
    XiValue *u = index_get(f, b1, &p->value);
    xi_block_set_return(b1, u);

    ASSERT_CONTRACT(f, XI_ARC_C3_BORROW_ESCAPE, "incident2: phi borrow view over-released owner");
    xi_func_free(f);
}

/* ========== Incident 3 (C4): release on a non-dominating join path ========= */

/* larr is defined only in the left branch; releasing it in the join executes on
 * the right path where larr was never defined. */
static void test_incident3_nondominating_release(void) {
    XiFunc *f = make_func("incident3_c4", &t_int);
    XiBlock *b0 = f->entry;
    XiBlock *b1 = xi_block_new(f);
    XiBlock *b2 = xi_block_new(f);
    XiBlock *b3 = xi_block_new(f);

    XiValue *cond = xi_const_bool(f, b0, true, &t_bool);
    xi_block_set_if(b0, cond, b1, b2);

    XiValue *larr = rc_new(f, b1);
    xi_block_set_jump(b1, b3);
    xi_block_set_jump(b2, b3);

    release(f, b3, larr); /* larr's def (b1) does not dominate b3 */
    XiValue *ret = xi_const_int(f, b3, 0, &t_int);
    xi_block_set_return(b3, ret);

    ASSERT_CONTRACT(f, XI_ARC_C4_DOMINANCE, "incident3: release on non-dominating join path");
    xi_func_free(f);
}

/* ========== Legal C3: owner alive at every borrow-view use ================= */

static void test_legal_borrow_view_owner_alive(void) {
    XiFunc *f = make_func("legal_c3", &t_int);
    XiBlock *b0 = f->entry;
    XiBlock *b1 = xi_block_new(f);

    XiValue *base = rc_new(f, b0);
    XiValue *sp = span_view(f, b0, base);
    xi_block_set_jump(b0, b1);

    XiPhi *p = xi_phi_new(f, b1, &t_span, b1->npreds);
    p->value.args[0] = sp;
    index_get(f, b1, &p->value); /* read the view */
    release(f, b1, base);        /* owner released only AFTER the view's last use */
    XiValue *ret = xi_const_int(f, b1, 0, &t_int);
    xi_block_set_return(b1, ret);

    ASSERT_OK(f, "legal borrow view: owner outlives view");
    xi_func_free(f);
}

static void test_legal_retained_borrow_view_outlives_owner(void) {
    XiFunc *f = make_func("legal_retained_c3", &t_int);
    XiBlock *b0 = f->entry;
    XiBlock *b1 = xi_block_new(f);

    XiValue *base = rc_new(f, b0);
    XiValue *sp = span_view(f, b0, base);
    retain(f, b0, sp); /* promote the borrowed result to an owning reference */
    release(f, b0, base);
    xi_block_set_jump(b0, b1);

    XiPhi *p = xi_phi_new(f, b1, &t_span, b1->npreds);
    p->value.args[0] = sp;
    index_get(f, b1, &p->value);
    release(f, b1, &p->value);
    XiValue *ret = xi_const_int(f, b1, 0, &t_int);
    xi_block_set_return(b1, ret);

    ASSERT_OK(f, "legal retained borrow view outlives aggregate owner");
    xi_func_free(f);
}

static void test_released_retain_does_not_promote_borrow_view(void) {
    XiFunc *f = make_func("released_retain_c3", &t_int);
    XiBlock *b0 = f->entry;
    XiBlock *b1 = xi_block_new(f);

    XiValue *base = rc_new(f, b0);
    XiValue *sp = span_view(f, b0, base);
    retain(f, b0, sp);
    release(f, b0, sp); /* compensation is gone before the phi edge */
    release(f, b0, base);
    xi_block_set_jump(b0, b1);

    XiPhi *p = xi_phi_new(f, b1, &t_span, b1->npreds);
    p->value.args[0] = sp;
    index_get(f, b1, &p->value);
    XiValue *ret = xi_const_int(f, b1, 0, &t_int);
    xi_block_set_return(b1, ret);

    ASSERT_CONTRACT(f, XI_ARC_C3_BORROW_ESCAPE, "C3: balanced-away retain cannot outlive owner");
    xi_func_free(f);
}

/* ========== Legal C4: branch-local release stays in dominance region ======= */

static void test_legal_branch_local_release(void) {
    XiFunc *f = make_func("legal_c4", &t_int);
    XiBlock *b0 = f->entry;
    XiBlock *b1 = xi_block_new(f);
    XiBlock *b2 = xi_block_new(f);
    XiBlock *b3 = xi_block_new(f);

    XiValue *cond = xi_const_bool(f, b0, true, &t_bool);
    xi_block_set_if(b0, cond, b1, b2);

    XiValue *larr = rc_new(f, b1);
    release(f, b1, larr); /* released in its own defining, dominating block */
    xi_block_set_jump(b1, b3);
    xi_block_set_jump(b2, b3);

    XiValue *ret = xi_const_int(f, b3, 0, &t_int);
    xi_block_set_return(b3, ret);

    ASSERT_OK(f, "legal branch-local release in dominance region");
    xi_func_free(f);
}

/* ========== C2: double release ========== */

static void test_double_release(void) {
    XiFunc *f = make_func("double_release", &t_int);
    XiBlock *b0 = f->entry;
    XiValue *v = rc_new(f, b0);
    release(f, b0, v);
    release(f, b0, v);
    XiValue *z = xi_const_int(f, b0, 0, &t_int);
    xi_block_set_return(b0, z);
    ASSERT_CONTRACT(f, XI_ARC_C2_DOUBLE_RELEASE, "C2: double release on a path");
    xi_func_free(f);
}

/* ========== Legal form 1: loop-header borrow phi (zero false positive) ===== */

/* owner lives across a counted loop, borrowed inside it, released once at exit.
 * The header phi (int induction var) is legal; owner is never released early. */
static void test_legal_loop_header_borrow_phi(void) {
    XiFunc *f = make_func("legal_loop", &t_int);
    XiBlock *b0 = f->entry;
    XiBlock *b1 = xi_block_new(f); /* header */
    XiBlock *b2 = xi_block_new(f); /* body / latch */
    XiBlock *b3 = xi_block_new(f); /* exit */

    XiValue *owner = rc_new(f, b0);
    XiValue *i0 = xi_const_int(f, b0, 0, &t_int);
    xi_block_set_jump(b0, b1); /* b1.preds = [b0] */
    xi_block_add_pred(b1, b2); /* back edge pred, before phi */

    XiPhi *iv = xi_phi_new(f, b1, &t_int, b1->npreds);
    iv->value.args[0] = i0;
    index_get(f, b1, owner); /* borrow use of owner inside the loop */
    XiValue *cond = xi_const_bool(f, b1, true, &t_bool);
    xi_block_set_if(b1, cond, b2, b3);

    XiValue *one = xi_const_int(f, b2, 1, &t_int);
    XiValue *iv2 = xi_value_new(f, b2, XI_ADD, &t_int, 2);
    iv2->args[0] = &iv->value;
    iv2->args[1] = one;
    iv->value.args[1] = iv2;
    xi_block_set_jump(b2, b1);

    release(f, b3, owner);
    XiValue *ret = xi_const_int(f, b3, 0, &t_int);
    xi_block_set_return(b3, ret);

    ASSERT_OK(f, "legal loop-header borrow phi");
    xi_func_free(f);
}

/* ========== Legal form 2: move-out (returned, never released) ============= */

static void test_legal_move_out(void) {
    XiFunc *f = make_func("legal_move_out", &t_array);
    XiBlock *b0 = f->entry;
    XiValue *v = rc_new(f, b0);
    XiValue *m = xi_value_new(f, b0, XI_OWNER_FORWARD, &t_array, 1);
    m->args[0] = v; /* ownership transferred through the move */
    xi_block_set_return(b0, m);
    ASSERT_OK(f, "legal move-out via return");
    xi_func_free(f);
}

/* ========== Legal form 3: early-return release on one branch ============== */

/* v released on both mutually exclusive paths at their deaths; never used after
 * release; joined block does not use v. */
static void test_legal_early_return_release(void) {
    XiFunc *f = make_func("legal_early_return", &t_int);
    XiBlock *b0 = f->entry;
    XiBlock *b1 = xi_block_new(f);
    XiBlock *b2 = xi_block_new(f);
    XiBlock *b3 = xi_block_new(f);

    XiValue *v = rc_new(f, b0);
    XiValue *cond = xi_const_bool(f, b0, true, &t_bool);
    xi_block_set_if(b0, cond, b1, b2);

    release(f, b1, v); /* dead on this path, dropped at branch entry */
    xi_block_set_jump(b1, b3);

    index_get(f, b2, v); /* used, then released at its death */
    release(f, b2, v);
    xi_block_set_jump(b2, b3);

    XiValue *ret = xi_const_int(f, b3, 0, &t_int);
    xi_block_set_return(b3, ret);

    ASSERT_OK(f, "legal early-return release");
    xi_func_free(f);
}

/* A trivially-correct function must pass. */
static void test_legal_trivial(void) {
    XiFunc *f = make_func("legal_trivial", &t_int);
    XiBlock *b0 = f->entry;
    XiValue *v = rc_new(f, b0);
    index_get(f, b0, v);
    release(f, b0, v);
    XiValue *z = xi_const_int(f, b0, 0, &t_int);
    xi_block_set_return(b0, z);
    ASSERT_OK(f, "legal trivial use-then-release");
    xi_func_free(f);
}

static void test_verifier_resource_failure_is_not_success(void) {
    XiFunc *f = make_func("resource_failure", &t_array);
    XiValue *v = rc_new(f, f->entry);
    xi_block_set_return(f->entry, v);
    XiArcVerifyOptions options = {.scratch_allocation_budget = 0};
    XiArcVerifyReport report;
    bool ok = xi_arc_verify_with_options(f, &report, &options);
    ASSERT_TRUE(!ok && report.contract == XI_ARC_INTERNAL_RESOURCE,
                "verifier OOM must fail closed as AnalysisResourceFailure");
    xi_func_free(f);
}

int main(void) {
    fprintf(stderr, "test_xi_arc_verify:\n");

    test_incident1_phi_released_input();
    test_incident2_borrow_view_owner_released();
    test_incident3_nondominating_release();
    test_incident4_stale_use();
    test_incident5_rc_op_metadata();
    test_double_release();

    test_legal_loop_header_borrow_phi();
    test_legal_move_out();
    test_legal_early_return_release();
    test_legal_borrow_view_owner_alive();
    test_legal_retained_borrow_view_outlives_owner();
    test_released_retain_does_not_promote_borrow_view();
    test_legal_branch_local_release();
    test_legal_trivial();
    test_verifier_resource_failure_is_not_success();

    fprintf(stderr, "\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
