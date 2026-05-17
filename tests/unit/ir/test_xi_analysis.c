/*
 * test_xi_analysis.c - Unit tests for Xi IR analysis passes
 *
 * Tests RPO computation, dominator tree, and liveness analysis.
 */

#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_analysis.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

/* Minimal XrType stubs */
static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_bool = {.kind = XR_KIND_BOOL, .id = 3, .frozen = true};
static XrType stub_void = {.kind = XR_KIND_VOID, .id = 6, .frozen = true};

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)                                                                                 \
    static void test_##name(void);                                                                 \
    static void run_##name(void) {                                                                 \
        printf("--- " #name " ---\n");                                                             \
        test_##name();                                                                             \
        printf("  PASS\n");                                                                        \
        tests_passed++;                                                                            \
    }                                                                                              \
    static void test_##name(void)

/* Helper: create function with sealed entry block */
static XiFunc *make_func(const char *name, XrType *ret) {
    XiFunc *f = xi_func_new(name, ret);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    return f;
}

/* ========== RPO Tests ========== */

TEST(rpo_single_block) {
    /* Single block function: entry gets rpo=1 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *c = xi_const_int(f, entry, 42, &stub_int);
    xi_block_set_return(entry, c);

    uint32_t reachable = xi_compute_rpo(f);
    assert(reachable == 1 && "single block -> 1 reachable");
    assert(entry->rpo == 1 && "entry should be rpo=1");
    xi_func_free(f);
}

TEST(rpo_linear_chain) {
    /*  entry -> b1 -> b2 (return)
     *  RPO: entry=1, b1=2, b2=3 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiBlock *b1 = xi_block_new(f);
    b1->sealed = true;
    XiBlock *b2 = xi_block_new(f);
    b2->sealed = true;

    xi_block_set_jump(entry, b1);
    xi_block_set_jump(b1, b2);
    XiValue *c = xi_const_int(f, b2, 1, &stub_int);
    xi_block_set_return(b2, c);

    uint32_t reachable = xi_compute_rpo(f);
    assert(reachable == 3);
    assert(entry->rpo == 1);
    assert(b1->rpo == 2);
    assert(b2->rpo == 3);
    xi_func_free(f);
}

TEST(rpo_diamond) {
    /*  entry -> then, else -> merge
     *  RPO: entry=1, then=2 or 3, else=2 or 3, merge=4 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *cond = xi_const_bool(f, entry, true, &stub_bool);
    XiBlock *then_b = xi_block_new(f);
    then_b->sealed = true;
    XiBlock *else_b = xi_block_new(f);
    else_b->sealed = true;
    XiBlock *merge = xi_block_new(f);

    xi_block_set_if(entry, cond, then_b, else_b);
    xi_block_set_jump(then_b, merge);
    xi_block_set_jump(else_b, merge);
    merge->sealed = true;

    XiValue *c = xi_const_int(f, merge, 0, &stub_int);
    xi_block_set_return(merge, c);

    uint32_t reachable = xi_compute_rpo(f);
    assert(reachable == 4);
    assert(entry->rpo == 1);
    /* then and else can be rpo 2 or 3 depending on DFS order */
    assert(then_b->rpo >= 2 && then_b->rpo <= 3);
    assert(else_b->rpo >= 2 && else_b->rpo <= 3);
    assert(merge->rpo == 4);
    xi_func_free(f);
}

TEST(rpo_unreachable_block) {
    /* entry -> return; unreachable block has rpo=0 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *c = xi_const_int(f, entry, 1, &stub_int);
    xi_block_set_return(entry, c);

    XiBlock *dead = xi_block_new(f);
    dead->sealed = true;

    uint32_t reachable = xi_compute_rpo(f);
    assert(reachable == 1);
    assert(dead->rpo == 0 && "unreachable block should have rpo=0");
    xi_func_free(f);
}

/* ========== Dominator Tests ========== */

TEST(dom_single_block) {
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *c = xi_const_int(f, entry, 42, &stub_int);
    xi_block_set_return(entry, c);

    xi_compute_rpo(f);
    xi_compute_dominators(f);

    assert(entry->idom == NULL && "entry idom should be NULL");
    assert(entry->dom_depth == 0);
    xi_func_free(f);
}

TEST(dom_linear_chain) {
    /* entry -> b1 -> b2: idom(b1)=entry, idom(b2)=b1 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiBlock *b1 = xi_block_new(f);
    b1->sealed = true;
    XiBlock *b2 = xi_block_new(f);
    b2->sealed = true;

    xi_block_set_jump(entry, b1);
    xi_block_set_jump(b1, b2);
    XiValue *c = xi_const_int(f, b2, 1, &stub_int);
    xi_block_set_return(b2, c);

    xi_compute_rpo(f);
    xi_compute_dominators(f);

    assert(entry->idom == NULL);
    assert(b1->idom == entry && "b1 dominated by entry");
    assert(b2->idom == b1 && "b2 dominated by b1");
    assert(b1->dom_depth == 1);
    assert(b2->dom_depth == 2);
    xi_func_free(f);
}

TEST(dom_diamond) {
    /* entry -> then, else -> merge: idom(merge) = entry */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *cond = xi_const_bool(f, entry, true, &stub_bool);
    XiBlock *then_b = xi_block_new(f);
    then_b->sealed = true;
    XiBlock *else_b = xi_block_new(f);
    else_b->sealed = true;
    XiBlock *merge = xi_block_new(f);

    xi_block_set_if(entry, cond, then_b, else_b);
    xi_block_set_jump(then_b, merge);
    xi_block_set_jump(else_b, merge);
    merge->sealed = true;

    XiValue *c = xi_const_int(f, merge, 0, &stub_int);
    xi_block_set_return(merge, c);

    xi_compute_rpo(f);
    xi_compute_dominators(f);

    assert(entry->idom == NULL);
    assert(then_b->idom == entry);
    assert(else_b->idom == entry);
    assert(merge->idom == entry && "merge dominated by entry (common dom)");
    assert(merge->dom_depth == 1);
    xi_func_free(f);
}

TEST(dom_dominates_query) {
    /* Verify xi_dominates() */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiBlock *b1 = xi_block_new(f);
    b1->sealed = true;
    XiBlock *b2 = xi_block_new(f);
    b2->sealed = true;

    xi_block_set_jump(entry, b1);
    xi_block_set_jump(b1, b2);
    XiValue *c = xi_const_int(f, b2, 1, &stub_int);
    xi_block_set_return(b2, c);

    xi_compute_rpo(f);
    xi_compute_dominators(f);

    assert(xi_dominates(entry, entry) && "block dominates itself");
    assert(xi_dominates(entry, b1) && "entry dominates b1");
    assert(xi_dominates(entry, b2) && "entry dominates b2");
    assert(xi_dominates(b1, b2) && "b1 dominates b2");
    assert(!xi_dominates(b2, b1) && "b2 does NOT dominate b1");
    assert(!xi_dominates(b1, entry) && "b1 does NOT dominate entry");
    xi_func_free(f);
}

/* ========== Liveness Tests ========== */

TEST(liveness_single_block) {
    /* fn(a) { return a + 1 }
     * a is live at block entry, 'add' is live at block exit */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiValue *c1 = xi_const_int(f, entry, 1, &stub_int);
    XiValue *add = xi_binary(f, entry, XI_ADD, &stub_int, a, c1);
    xi_block_set_return(entry, add);

    xi_compute_rpo(f);
    XiLiveness *l = xi_compute_liveness(f);
    assert(l != NULL);

    /* 'a' and 'c1' are defined in this block and used in same block,
     * so they don't appear in live_in (they're defined before use).
     * But 'add' is used by the block terminator (return). */
    /* The return value is live_out from the block. */
    assert(xi_is_live_out(l, entry, add) == false &&
           "return value is consumed by terminator, not live-out");

    xi_liveness_free(l);
    xi_func_free(f);
}

TEST(liveness_cross_block) {
    /* entry: a = param(0); jump b1
     * b1:    return a
     * 'a' should be live_out of entry and live_in of b1 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiBlock *b1 = xi_block_new(f);
    b1->sealed = true;
    xi_block_set_jump(entry, b1);
    xi_block_set_return(b1, a);

    xi_compute_rpo(f);
    XiLiveness *l = xi_compute_liveness(f);
    assert(l != NULL);

    assert(xi_is_live_out(l, entry, a) && "a should be live-out of entry");
    assert(xi_is_live_in(l, b1, a) && "a should be live-in of b1");

    xi_liveness_free(l);
    xi_func_free(f);
}

TEST(liveness_diamond) {
    /* entry: a=param(0), cond=true; if cond -> then, else
     * then:  b = a + 1; jump merge
     * else:  c = a + 2; jump merge
     * merge: phi(b, c); return phi
     * 'a' should be live across both branches */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiValue *cond = xi_const_bool(f, entry, true, &stub_bool);

    XiBlock *then_b = xi_block_new(f);
    then_b->sealed = true;
    XiBlock *else_b = xi_block_new(f);
    else_b->sealed = true;
    XiBlock *merge = xi_block_new(f);

    xi_block_set_if(entry, cond, then_b, else_b);

    XiValue *c1 = xi_const_int(f, then_b, 1, &stub_int);
    XiValue *b = xi_binary(f, then_b, XI_ADD, &stub_int, a, c1);
    xi_block_set_jump(then_b, merge);

    XiValue *c2 = xi_const_int(f, else_b, 2, &stub_int);
    XiValue *c = xi_binary(f, else_b, XI_ADD, &stub_int, a, c2);
    xi_block_set_jump(else_b, merge);

    merge->sealed = true;
    XiPhi *phi = xi_phi_new(f, merge, &stub_int, 2);
    phi->value.args[0] = b;
    phi->value.args[1] = c;

    xi_block_set_return(merge, &phi->value);

    xi_compute_rpo(f);
    XiLiveness *l = xi_compute_liveness(f);
    assert(l != NULL);

    /* 'a' is live out of entry (used in both branches) */
    assert(xi_is_live_out(l, entry, a) && "a live-out of entry");
    /* 'a' is live-in of both branches */
    assert(xi_is_live_in(l, then_b, a) && "a live-in of then");
    assert(xi_is_live_in(l, else_b, a) && "a live-in of else");

    xi_liveness_free(l);
    xi_func_free(f);
}

TEST(liveness_dead_value) {
    /* entry: a=param(0), dead=42; return a
     * 'dead' should NOT be live anywhere */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiValue *dead = xi_const_int(f, entry, 42, &stub_int);
    xi_block_set_return(entry, a);

    xi_compute_rpo(f);
    XiLiveness *l = xi_compute_liveness(f);
    assert(l != NULL);

    assert(!xi_is_live_out(l, entry, dead) && "dead not live-out");
    (void) dead;

    xi_liveness_free(l);
    xi_func_free(f);
}

/* ========== Main ========== */

int main(void) {
    printf("=== Xi Analysis Unit Tests ===\n\n");

    (void) stub_void;

    /* RPO */
    run_rpo_single_block();
    run_rpo_linear_chain();
    run_rpo_diamond();
    run_rpo_unreachable_block();

    /* Dominators */
    run_dom_single_block();
    run_dom_linear_chain();
    run_dom_diamond();
    run_dom_dominates_query();

    /* Liveness */
    run_liveness_single_block();
    run_liveness_cross_block();
    run_liveness_diamond();
    run_liveness_dead_value();

    printf("\n=== %d/%d Xi Analysis tests passed ===\n", tests_passed, tests_passed + tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
