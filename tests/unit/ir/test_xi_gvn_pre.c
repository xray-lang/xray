/*
 * Unit tests for Xi GVN-PRE (Global Value Numbering + Partial Redundancy
 * Elimination).
 *
 * Covers full-redundancy elimination via dominator-based VN, partial
 * redundancy insertion through join blocks, commutative VN matching,
 * critical-edge bail-out, multi-predecessor joins, and the conservative
 * load-speculation guard.
 */

#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_opt_gvn_pre.h"
#include "../../../src/ir/xi_tbaa.h"
#include "../../../src/ir/xi_verify.h"
#include "../../../src/runtime/value/xtype.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_bool = {.kind = XR_KIND_BOOL, .id = 2, .frozen = true};
static XrType stub_any = {.kind = XR_KIND_UNKNOWN, .id = 10, .frozen = true};

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

/* Allocate a function with a sealed entry block. */
static XiFunc *make_func(const char *name, XrType *ret_type) {
    XiFunc *f = xi_func_new(name, ret_type);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    return f;
}

/* ========== GVN-PRE Tests ========== */

TEST(gvn_pre_inserts_missing_edge_expression) {
    /* A join expression that is already available on one incoming edge
     * should be made fully available with one edge insertion and a phi. */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *cond = xi_param(f, entry, 2, &stub_bool);

    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);
    XiBlock *join = xi_block_new(f);
    then_blk->sealed = true;
    else_blk->sealed = true;
    join->sealed = true;

    xi_block_set_if(entry, cond, then_blk, else_blk);
    XiValue *then_add = xi_binary(f, then_blk, XI_ADD, &stub_int, x, y);
    xi_block_set_jump(then_blk, join);
    xi_block_set_jump(else_blk, join);

    XiValue *join_add = xi_binary(f, join, XI_ADD, &stub_int, x, y);
    xi_block_set_return(join, join_add);

    XiPassChange chg = xi_opt_gvn_pre(f);

    assert(chg.values_changed && "GVN-PRE should report inserted and replaced values");
    assert(chg.n_added >= 2 && "GVN-PRE should add an edge expression and a phi");
    assert(join_add->op == XI_COPY && "join expression should be replaced by a phi copy");
    assert(join->phis != NULL && "join should receive a phi for the partially available value");
    assert(join_add->args[0] == &join->phis->value && "copy should reference the inserted phi");
    assert(join->phis->value.nargs == join->npreds && "phi args should match predecessor count");
    assert(join->phis->value.args[0] == then_add && "then edge should reuse the available value");
    assert(join->phis->value.args[1]->op == XI_ADD && "else edge should get an inserted add");
    assert(join->phis->value.args[1]->block == else_blk && "inserted add should live on else edge");

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok)
        printf("  verify error: %s\n", errbuf);
    assert(ok && "GVN-PRE output should remain valid SSA");
    xi_func_free(f);
}

TEST(gvn_pre_phi_merges_when_all_predecessors_have_leaders) {
    /* Both branches independently compute x+y. Neither leader dominates
     * the join, so full-RE cannot replace the join expression. PRE must
     * synthesise a phi(then_add, else_add) and forward to it. */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *cond = xi_param(f, entry, 2, &stub_bool);

    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);
    XiBlock *join = xi_block_new(f);
    then_blk->sealed = true;
    else_blk->sealed = true;
    join->sealed = true;

    xi_block_set_if(entry, cond, then_blk, else_blk);
    XiValue *then_add = xi_binary(f, then_blk, XI_ADD, &stub_int, x, y);
    xi_block_set_jump(then_blk, join);
    XiValue *else_add = xi_binary(f, else_blk, XI_ADD, &stub_int, x, y);
    xi_block_set_jump(else_blk, join);

    XiValue *join_add = xi_binary(f, join, XI_ADD, &stub_int, x, y);
    xi_block_set_return(join, join_add);

    XiPassChange chg = xi_opt_gvn_pre(f);

    assert(chg.values_changed && "PRE should report a transformation");
    assert(join_add->op == XI_COPY && "join expression should forward to a phi");
    assert(join->phis != NULL && "phi should be created to merge the two leaders");
    assert(join_add->args[0] == &join->phis->value && "copy should reference the phi");
    assert(join->phis->value.args[0] == then_add && "phi arg[0] should be then leader");
    assert(join->phis->value.args[1] == else_add && "phi arg[1] should be else leader");

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok)
        printf("  verify error: %s\n", errbuf);
    assert(ok && "phi-merge output should remain valid SSA");
    xi_func_free(f);
}

TEST(gvn_pre_skips_critical_edge_predecessor) {
    /* If a join predecessor has more than one successor, inserting at
     * its tail would speculate the expression onto a sibling path. PRE
     * must bail without mutating the IR. */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *cond1 = xi_param(f, entry, 2, &stub_bool);
    XiValue *cond2 = xi_param(f, entry, 3, &stub_bool);

    XiBlock *then_blk = xi_block_new(f);
    XiBlock *crit = xi_block_new(f);
    XiBlock *side = xi_block_new(f);
    XiBlock *join = xi_block_new(f);
    then_blk->sealed = true;
    crit->sealed = true;
    side->sealed = true;
    join->sealed = true;

    xi_block_set_if(entry, cond1, then_blk, crit);
    XiValue *then_add = xi_binary(f, then_blk, XI_ADD, &stub_int, x, y);
    xi_block_set_jump(then_blk, join);

    /* 'crit' fans out to both 'join' and 'side' — critical edge into join. */
    xi_block_set_if(crit, cond2, join, side);
    xi_block_set_return(side, xi_const_int(f, side, 0, &stub_int));

    XiValue *join_add = xi_binary(f, join, XI_ADD, &stub_int, x, y);
    xi_block_set_return(join, join_add);

    XiPassChange chg = xi_opt_gvn_pre(f);

    assert(join_add->op == XI_ADD && "join expression must remain when PRE bails");
    assert(join->phis == NULL && "no phi should be created across a critical edge");
    assert(chg.n_added == 0 && "no insertion when PRE refuses to split");
    (void) then_add;
    xi_func_free(f);
}

TEST(gvn_pre_recognizes_commutative_argument_swap) {
    /* x+y on the then edge is the same VN as y+x on the join, because
     * VN normalization is commutativity-aware. PRE must reuse the then
     * leader and insert (y+x) into else as a clone. */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *cond = xi_param(f, entry, 2, &stub_bool);

    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);
    XiBlock *join = xi_block_new(f);
    then_blk->sealed = true;
    else_blk->sealed = true;
    join->sealed = true;

    xi_block_set_if(entry, cond, then_blk, else_blk);
    XiValue *then_add = xi_binary(f, then_blk, XI_ADD, &stub_int, x, y);
    xi_block_set_jump(then_blk, join);
    xi_block_set_jump(else_blk, join);

    /* Note swapped operand order — VN must still match. */
    XiValue *join_add = xi_binary(f, join, XI_ADD, &stub_int, y, x);
    xi_block_set_return(join, join_add);

    XiPassChange chg = xi_opt_gvn_pre(f);

    assert(chg.values_changed && "commutative match should still drive PRE");
    assert(join_add->op == XI_COPY && "join add should be replaced");
    assert(join->phis != NULL);
    assert(join->phis->value.args[0] == then_add &&
           "then leader should be reused via commutative VN match");
    xi_func_free(f);
}

TEST(gvn_pre_handles_three_predecessor_join) {
    /* Diamond-with-extra-edge: only block A holds a leader; PRE must
     * insert clones into B and C and synthesise a 3-arg phi. */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *cond = xi_param(f, entry, 2, &stub_bool);

    /* entry --IF cond--> mid_a, mid_bc
     * mid_bc --IF cond--> b_blk, c_blk
     * a_blk, b_blk, c_blk --jump--> join
     */
    XiBlock *mid_a = xi_block_new(f);
    XiBlock *mid_bc = xi_block_new(f);
    XiBlock *b_blk = xi_block_new(f);
    XiBlock *c_blk = xi_block_new(f);
    XiBlock *join = xi_block_new(f);
    mid_a->sealed = true;
    mid_bc->sealed = true;
    b_blk->sealed = true;
    c_blk->sealed = true;
    join->sealed = true;

    xi_block_set_if(entry, cond, mid_a, mid_bc);
    XiValue *a_add = xi_binary(f, mid_a, XI_ADD, &stub_int, x, y);
    xi_block_set_jump(mid_a, join);

    xi_block_set_if(mid_bc, cond, b_blk, c_blk);
    xi_block_set_jump(b_blk, join);
    xi_block_set_jump(c_blk, join);

    XiValue *join_add = xi_binary(f, join, XI_ADD, &stub_int, x, y);
    xi_block_set_return(join, join_add);

    XiPassChange chg = xi_opt_gvn_pre(f);

    assert(chg.values_changed);
    assert(join_add->op == XI_COPY && "join add should forward to phi");
    assert(join->phis != NULL && join->phis->value.nargs == 3 && "3-input phi expected");
    assert(join->phis->value.args[0] == a_add && "first edge must reuse the existing leader");
    assert(join->phis->value.args[1]->op == XI_ADD && join->phis->value.args[1]->block == b_blk &&
           "b edge should receive an inserted clone");
    assert(join->phis->value.args[2]->op == XI_ADD && join->phis->value.args[2]->block == c_blk &&
           "c edge should receive an inserted clone");
    assert(chg.n_added >= 3 && "two clones plus a phi");

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok)
        printf("  verify error: %s\n", errbuf);
    assert(ok);
    xi_func_free(f);
}

TEST(gvn_pre_does_not_speculate_loads) {
    /* A LOAD_FIELD is partially available on the 'then' edge but PRE
     * must not speculate it onto the 'else' edge: an unrelated store
     * could clobber the field, and TBAA/MemSSA reasoning is the
     * full-RE pass's job. */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *obj = xi_param(f, entry, 0, &stub_int);
    XiValue *cond = xi_param(f, entry, 1, &stub_bool);

    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);
    XiBlock *join = xi_block_new(f);
    then_blk->sealed = true;
    else_blk->sealed = true;
    join->sealed = true;

    xi_block_set_if(entry, cond, then_blk, else_blk);

    XiValue *then_load = xi_value_new(f, then_blk, XI_LOAD_FIELD, &stub_int, 1);
    then_load->args[0] = obj;
    then_load->aux_int = 42;
    xi_block_set_jump(then_blk, join);
    xi_block_set_jump(else_blk, join);

    XiValue *join_load = xi_value_new(f, join, XI_LOAD_FIELD, &stub_int, 1);
    join_load->args[0] = obj;
    join_load->aux_int = 42;
    xi_block_set_return(join, join_load);

    uint32_t else_n_before = else_blk->nvalues;

    XiPassChange chg = xi_opt_gvn_pre(f);

    assert(join_load->op == XI_LOAD_FIELD && "loads must not be hoisted by PRE");
    assert(join->phis == NULL && "no phi should be inserted for a speculative load");
    assert(chg.n_added == 0 && "PRE must not insert load clones");
    assert(else_blk->nvalues == else_n_before && "no clone should appear on the else edge");
    (void) then_load;
    xi_func_free(f);
}

TEST(gvn_eliminates_cross_block_load_via_memssa) {
    /* entry: load shared[0] → blk2: load shared[0] again.
     * No intervening store → second load should be eliminated. */
    XiFunc *f = make_func("xblock_load", &stub_int);
    XiBlock *entry = f->entry;
    XiBlock *blk2 = xi_block_new(f);
    blk2->sealed = true;

    XiValue *load1 = xi_value_new(f, entry, XI_GET_SHARED, &stub_int, 0);
    load1->aux_int = 0;
    xi_block_set_jump(entry, blk2);

    XiValue *load2 = xi_value_new(f, blk2, XI_GET_SHARED, &stub_int, 0);
    load2->aux_int = 0;
    xi_block_set_return(blk2, load2);

    xi_tbaa_annotate(f);
    XiPassChange chg = xi_opt_gvn_pre(f);

    assert(chg.values_changed);
    assert(load2->op == XI_COPY);
    assert(load2->args[0] == load1);

    xi_func_free(f);
}

TEST(gvn_no_cross_block_load_elim_after_call) {
    /* entry: load shared[0], call, → blk2: load shared[0].
     * Call clobbers memory → second load must NOT be eliminated. */
    XiFunc *f = make_func("call_clobber", &stub_int);
    XiBlock *entry = f->entry;
    XiBlock *blk2 = xi_block_new(f);
    blk2->sealed = true;

    XiValue *callee = xi_param(f, entry, 0, &stub_any);

    XiValue *load1 = xi_value_new(f, entry, XI_GET_SHARED, &stub_int, 0);
    load1->aux_int = 0;

    XiValue *call = xi_value_new(f, entry, XI_CALL, &stub_any, 1);
    call->args[0] = callee;

    xi_block_set_jump(entry, blk2);

    XiValue *load2 = xi_value_new(f, blk2, XI_GET_SHARED, &stub_int, 0);
    load2->aux_int = 0;
    xi_block_set_return(blk2, load2);

    xi_tbaa_annotate(f);
    XiPassChange chg = xi_opt_gvn_pre(f);
    (void) chg;

    assert(load2->op == XI_GET_SHARED && "load after call must not be eliminated");

    xi_func_free(f);
}

/* ========== 1. Full Redundancy Elimination (same block) ========== */

TEST(same_block_add_elimination) {
    XiFunc *f = make_func("sb_add", &stub_int);
    XiBlock *entry = f->entry;
    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *a1 = xi_binary(f, entry, XI_ADD, &stub_int, x, y);
    XiValue *a2 = xi_binary(f, entry, XI_ADD, &stub_int, x, y);
    xi_block_set_return(entry, a2);

    xi_opt_gvn_pre(f);
    assert(a2->op == XI_COPY && "second x+y should become COPY");
    assert(a2->args[0] == a1 && "COPY should reference the first add");
    xi_func_free(f);
}

TEST(same_block_sub_elimination) {
    XiFunc *f = make_func("sb_sub", &stub_int);
    XiBlock *entry = f->entry;
    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *s1 = xi_binary(f, entry, XI_SUB, &stub_int, x, y);
    XiValue *s2 = xi_binary(f, entry, XI_SUB, &stub_int, x, y);
    xi_block_set_return(entry, s2);

    xi_opt_gvn_pre(f);
    assert(s2->op == XI_COPY && "second x-y should become COPY");
    assert(s2->args[0] == s1);
    xi_func_free(f);
}

TEST(same_block_mul_elimination) {
    XiFunc *f = make_func("sb_mul", &stub_int);
    XiBlock *entry = f->entry;
    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *m1 = xi_binary(f, entry, XI_MUL, &stub_int, x, y);
    XiValue *m2 = xi_binary(f, entry, XI_MUL, &stub_int, x, y);
    xi_block_set_return(entry, m2);

    xi_opt_gvn_pre(f);
    assert(m2->op == XI_COPY && "second x*y should become COPY");
    assert(m2->args[0] == m1);
    xi_func_free(f);
}

TEST(same_block_band_elimination) {
    XiFunc *f = make_func("sb_band", &stub_int);
    XiBlock *entry = f->entry;
    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *b1 = xi_binary(f, entry, XI_BAND, &stub_int, x, y);
    XiValue *b2 = xi_binary(f, entry, XI_BAND, &stub_int, x, y);
    xi_block_set_return(entry, b2);

    xi_opt_gvn_pre(f);
    assert(b2->op == XI_COPY && "second x&y should become COPY");
    assert(b2->args[0] == b1);
    xi_func_free(f);
}

TEST(same_block_bor_elimination) {
    XiFunc *f = make_func("sb_bor", &stub_int);
    XiBlock *entry = f->entry;
    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *o1 = xi_binary(f, entry, XI_BOR, &stub_int, x, y);
    XiValue *o2 = xi_binary(f, entry, XI_BOR, &stub_int, x, y);
    xi_block_set_return(entry, o2);

    xi_opt_gvn_pre(f);
    assert(o2->op == XI_COPY && "second x|y should become COPY");
    assert(o2->args[0] == o1);
    xi_func_free(f);
}

TEST(same_block_neg_elimination) {
    XiFunc *f = make_func("sb_neg", &stub_int);
    XiBlock *entry = f->entry;
    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *n1 = xi_value_new(f, entry, XI_NEG, &stub_int, 1);
    n1->args[0] = x;
    XiValue *n2 = xi_value_new(f, entry, XI_NEG, &stub_int, 1);
    n2->args[0] = x;
    xi_block_set_return(entry, n2);

    xi_opt_gvn_pre(f);
    assert(n2->op == XI_COPY && "second -x should become COPY");
    assert(n2->args[0] == n1);
    xi_func_free(f);
}

TEST(same_block_not_elimination) {
    XiFunc *f = make_func("sb_not", &stub_bool);
    XiBlock *entry = f->entry;
    XiValue *x = xi_param(f, entry, 0, &stub_bool);
    XiValue *n1 = xi_value_new(f, entry, XI_NOT, &stub_bool, 1);
    n1->args[0] = x;
    XiValue *n2 = xi_value_new(f, entry, XI_NOT, &stub_bool, 1);
    n2->args[0] = x;
    xi_block_set_return(entry, n2);

    xi_opt_gvn_pre(f);
    assert(n2->op == XI_COPY && "second !x should become COPY");
    assert(n2->args[0] == n1);
    xi_func_free(f);
}

TEST(same_block_triple_redundancy) {
    XiFunc *f = make_func("sb_triple", &stub_int);
    XiBlock *entry = f->entry;
    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *a1 = xi_binary(f, entry, XI_ADD, &stub_int, x, y);
    XiValue *a2 = xi_binary(f, entry, XI_ADD, &stub_int, x, y);
    XiValue *a3 = xi_binary(f, entry, XI_ADD, &stub_int, x, y);
    xi_block_set_return(entry, a3);

    xi_opt_gvn_pre(f);
    assert(a2->op == XI_COPY && "second add should become COPY");
    assert(a3->op == XI_COPY && "third add should become COPY");
    assert(a2->args[0] == a1 && "second COPY targets first");
    assert(a3->args[0] == a1 && "third COPY also targets first");
    xi_func_free(f);
}

/* ========== 2. Full Redundancy Elimination (cross-block via dominator) ========== */

TEST(dom_chain_eliminates) {
    XiFunc *f = make_func("dom_chain", &stub_int);
    XiBlock *entry = f->entry;
    XiBlock *blk2 = xi_block_new(f);
    blk2->sealed = true;

    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *a1 = xi_binary(f, entry, XI_ADD, &stub_int, x, y);
    xi_block_set_jump(entry, blk2);

    XiValue *a2 = xi_binary(f, blk2, XI_ADD, &stub_int, x, y);
    xi_block_set_return(blk2, a2);

    xi_opt_gvn_pre(f);
    assert(a2->op == XI_COPY && "dominated x+y should become COPY");
    assert(a2->args[0] == a1);
    xi_func_free(f);
}

TEST(dom_three_blocks_chain) {
    XiFunc *f = make_func("dom_3chain", &stub_int);
    XiBlock *entry = f->entry;
    XiBlock *b2 = xi_block_new(f);
    XiBlock *b3 = xi_block_new(f);
    b2->sealed = true;
    b3->sealed = true;

    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *a1 = xi_binary(f, entry, XI_ADD, &stub_int, x, y);
    xi_block_set_jump(entry, b2);
    xi_block_set_jump(b2, b3);

    XiValue *a2 = xi_binary(f, b3, XI_ADD, &stub_int, x, y);
    xi_block_set_return(b3, a2);

    xi_opt_gvn_pre(f);
    assert(a2->op == XI_COPY && "deep dominated x+y should become COPY");
    assert(a2->args[0] == a1);
    xi_func_free(f);
}

TEST(dom_diamond_both_branches) {
    XiFunc *f = make_func("dom_diamond", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *cond = xi_param(f, entry, 2, &stub_bool);
    XiValue *entry_add = xi_binary(f, entry, XI_ADD, &stub_int, x, y);

    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);
    XiBlock *join = xi_block_new(f);
    then_blk->sealed = true;
    else_blk->sealed = true;
    join->sealed = true;

    xi_block_set_if(entry, cond, then_blk, else_blk);

    XiValue *then_dup = xi_binary(f, then_blk, XI_ADD, &stub_int, x, y);
    xi_block_set_jump(then_blk, join);

    XiValue *else_dup = xi_binary(f, else_blk, XI_ADD, &stub_int, x, y);
    xi_block_set_jump(else_blk, join);

    xi_block_set_return(join, xi_const_int(f, join, 0, &stub_int));

    xi_opt_gvn_pre(f);
    assert(then_dup->op == XI_COPY && "then x+y dominated by entry x+y");
    assert(then_dup->args[0] == entry_add);
    assert(else_dup->op == XI_COPY && "else x+y dominated by entry x+y");
    assert(else_dup->args[0] == entry_add);
    xi_func_free(f);
}

TEST(dom_no_elimination_when_not_dominating) {
    XiFunc *f = make_func("dom_no_elim", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *cond = xi_param(f, entry, 2, &stub_bool);

    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);
    then_blk->sealed = true;
    else_blk->sealed = true;

    xi_block_set_if(entry, cond, then_blk, else_blk);

    XiValue *then_add = xi_binary(f, then_blk, XI_ADD, &stub_int, x, y);
    xi_block_set_return(then_blk, then_add);

    XiValue *else_add = xi_binary(f, else_blk, XI_ADD, &stub_int, x, y);
    xi_block_set_return(else_blk, else_add);

    xi_opt_gvn_pre(f);
    assert(then_add->op == XI_ADD && "then_add not dominated by else_add");
    assert(else_add->op == XI_ADD && "else_add not dominated by then_add");
    xi_func_free(f);
}

TEST(dom_entry_dominates_all) {
    XiFunc *f = make_func("dom_all", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *cond = xi_param(f, entry, 2, &stub_bool);
    XiValue *entry_add = xi_binary(f, entry, XI_ADD, &stub_int, x, y);

    XiBlock *b1 = xi_block_new(f);
    XiBlock *b2 = xi_block_new(f);
    XiBlock *b3 = xi_block_new(f);
    b1->sealed = true;
    b2->sealed = true;
    b3->sealed = true;

    xi_block_set_if(entry, cond, b1, b2);

    XiValue *dup1 = xi_binary(f, b1, XI_ADD, &stub_int, x, y);
    xi_block_set_jump(b1, b3);

    XiValue *dup2 = xi_binary(f, b2, XI_ADD, &stub_int, x, y);
    xi_block_set_jump(b2, b3);

    XiValue *dup3 = xi_binary(f, b3, XI_ADD, &stub_int, x, y);
    xi_block_set_return(b3, dup3);

    xi_opt_gvn_pre(f);
    assert(dup1->op == XI_COPY && dup1->args[0] == entry_add);
    assert(dup2->op == XI_COPY && dup2->args[0] == entry_add);
    assert(dup3->op == XI_COPY && "b3 dup should also be eliminated");
    xi_func_free(f);
}

TEST(dom_sub_not_commutative) {
    XiFunc *f = make_func("dom_sub_comm", &stub_int);
    XiBlock *entry = f->entry;
    XiBlock *blk2 = xi_block_new(f);
    blk2->sealed = true;

    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *s1 = xi_binary(f, entry, XI_SUB, &stub_int, x, y);
    xi_block_set_jump(entry, blk2);

    XiValue *s2 = xi_binary(f, blk2, XI_SUB, &stub_int, y, x);
    xi_block_set_return(blk2, s2);

    xi_opt_gvn_pre(f);
    assert(s2->op == XI_SUB && "y-x should NOT match x-y for SUB (non-commutative)");
    (void) s1;
    xi_func_free(f);
}

/* ========== 3. Partial Redundancy Elimination variations ========== */

TEST(pre_sub_insertion) {
    XiFunc *f = make_func("pre_sub", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *cond = xi_param(f, entry, 2, &stub_bool);

    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);
    XiBlock *join = xi_block_new(f);
    then_blk->sealed = true;
    else_blk->sealed = true;
    join->sealed = true;

    xi_block_set_if(entry, cond, then_blk, else_blk);
    XiValue *then_sub = xi_binary(f, then_blk, XI_SUB, &stub_int, x, y);
    xi_block_set_jump(then_blk, join);
    xi_block_set_jump(else_blk, join);

    XiValue *join_sub = xi_binary(f, join, XI_SUB, &stub_int, x, y);
    xi_block_set_return(join, join_sub);

    XiPassChange chg = xi_opt_gvn_pre(f);
    assert(chg.values_changed);
    assert(join_sub->op == XI_COPY && "join SUB should be replaced by phi COPY");
    assert(join->phis != NULL);
    assert(join->phis->value.args[0] == then_sub);
    xi_func_free(f);
}

TEST(pre_mul_insertion) {
    XiFunc *f = make_func("pre_mul", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *cond = xi_param(f, entry, 2, &stub_bool);

    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);
    XiBlock *join = xi_block_new(f);
    then_blk->sealed = true;
    else_blk->sealed = true;
    join->sealed = true;

    xi_block_set_if(entry, cond, then_blk, else_blk);
    XiValue *then_mul = xi_binary(f, then_blk, XI_MUL, &stub_int, x, y);
    xi_block_set_jump(then_blk, join);
    xi_block_set_jump(else_blk, join);

    XiValue *join_mul = xi_binary(f, join, XI_MUL, &stub_int, x, y);
    xi_block_set_return(join, join_mul);

    XiPassChange chg = xi_opt_gvn_pre(f);
    assert(chg.values_changed);
    assert(join_mul->op == XI_COPY);
    assert(join->phis != NULL);
    assert(join->phis->value.args[0] == then_mul);
    xi_func_free(f);
}

TEST(pre_nested_diamond) {
    XiFunc *f = make_func("pre_nest", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *c1 = xi_param(f, entry, 2, &stub_bool);
    XiValue *c2 = xi_param(f, entry, 3, &stub_bool);

    XiBlock *outer_then = xi_block_new(f);
    XiBlock *outer_else = xi_block_new(f);
    XiBlock *inner_then = xi_block_new(f);
    XiBlock *inner_else = xi_block_new(f);
    XiBlock *inner_join = xi_block_new(f);
    XiBlock *outer_join = xi_block_new(f);
    outer_then->sealed = true;
    outer_else->sealed = true;
    inner_then->sealed = true;
    inner_else->sealed = true;
    inner_join->sealed = true;
    outer_join->sealed = true;

    xi_block_set_if(entry, c1, outer_then, outer_else);

    /* outer_then has x+y */
    XiValue *ot_add = xi_binary(f, outer_then, XI_ADD, &stub_int, x, y);
    xi_block_set_jump(outer_then, outer_join);

    /* outer_else → inner diamond */
    xi_block_set_if(outer_else, c2, inner_then, inner_else);
    xi_block_set_jump(inner_then, inner_join);
    xi_block_set_jump(inner_else, inner_join);
    xi_block_set_jump(inner_join, outer_join);

    XiValue *oj_add = xi_binary(f, outer_join, XI_ADD, &stub_int, x, y);
    xi_block_set_return(outer_join, oj_add);

    XiPassChange chg = xi_opt_gvn_pre(f);
    assert(chg.values_changed);
    assert(oj_add->op == XI_COPY && "outer join add partially redundant");
    (void) ot_add;
    xi_func_free(f);
}

TEST(pre_four_predecessors) {
    XiFunc *f = make_func("pre_4pred", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *c1 = xi_param(f, entry, 2, &stub_bool);
    XiValue *c2 = xi_param(f, entry, 3, &stub_bool);

    XiBlock *a = xi_block_new(f);
    XiBlock *bcd = xi_block_new(f);
    XiBlock *b = xi_block_new(f);
    XiBlock *cd = xi_block_new(f);
    XiBlock *c = xi_block_new(f);
    XiBlock *d = xi_block_new(f);
    XiBlock *join = xi_block_new(f);
    a->sealed = true;
    bcd->sealed = true;
    b->sealed = true;
    cd->sealed = true;
    c->sealed = true;
    d->sealed = true;
    join->sealed = true;

    xi_block_set_if(entry, c1, a, bcd);

    XiValue *a_add = xi_binary(f, a, XI_ADD, &stub_int, x, y);
    xi_block_set_jump(a, join);

    xi_block_set_if(bcd, c2, b, cd);
    xi_block_set_jump(b, join);
    xi_block_set_if(cd, c1, c, d);
    xi_block_set_jump(c, join);
    xi_block_set_jump(d, join);

    XiValue *join_add = xi_binary(f, join, XI_ADD, &stub_int, x, y);
    xi_block_set_return(join, join_add);

    XiPassChange chg = xi_opt_gvn_pre(f);
    assert(chg.values_changed);
    assert(join_add->op == XI_COPY && "join add should be replaced");
    assert(join->phis != NULL && join->phis->value.nargs == 4);
    assert(join->phis->value.args[0] == a_add && "a edge reuses existing leader");
    xi_func_free(f);
}

TEST(pre_expression_chain) {
    XiFunc *f = make_func("pre_chain", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *cond = xi_param(f, entry, 2, &stub_bool);

    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);
    XiBlock *join = xi_block_new(f);
    then_blk->sealed = true;
    else_blk->sealed = true;
    join->sealed = true;

    xi_block_set_if(entry, cond, then_blk, else_blk);
    XiValue *then_add = xi_binary(f, then_blk, XI_ADD, &stub_int, x, y);
    (void) then_add;
    xi_block_set_jump(then_blk, join);
    xi_block_set_jump(else_blk, join);

    XiValue *join_add = xi_binary(f, join, XI_ADD, &stub_int, x, y);
    XiValue *z = xi_param(f, entry, 3, &stub_int);
    XiValue *join_mul = xi_binary(f, join, XI_MUL, &stub_int, join_add, z);
    xi_block_set_return(join, join_mul);

    XiPassChange chg = xi_opt_gvn_pre(f);
    assert(chg.values_changed);
    assert(join_add->op == XI_COPY && "x+y at join should be eliminated");
    xi_func_free(f);
}

TEST(pre_constant_folding_independent) {
    XiFunc *f = make_func("pre_no_fold", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *c1 = xi_const_int(f, entry, 10, &stub_int);
    XiValue *c2 = xi_const_int(f, entry, 20, &stub_int);
    XiValue *add = xi_binary(f, entry, XI_ADD, &stub_int, c1, c2);
    xi_block_set_return(entry, add);

    xi_opt_gvn_pre(f);
    assert(add->op == XI_ADD && "GVN should not constant-fold, only number values");
    xi_func_free(f);
}

TEST(pre_no_insert_if_all_have_it) {
    XiFunc *f = make_func("pre_all_avail", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *cond = xi_param(f, entry, 2, &stub_bool);

    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);
    XiBlock *join = xi_block_new(f);
    then_blk->sealed = true;
    else_blk->sealed = true;
    join->sealed = true;

    xi_block_set_if(entry, cond, then_blk, else_blk);

    XiValue *then_add = xi_binary(f, then_blk, XI_ADD, &stub_int, x, y);
    xi_block_set_jump(then_blk, join);
    XiValue *else_add = xi_binary(f, else_blk, XI_ADD, &stub_int, x, y);
    xi_block_set_jump(else_blk, join);

    XiValue *join_add = xi_binary(f, join, XI_ADD, &stub_int, x, y);
    xi_block_set_return(join, join_add);

    uint32_t then_n = then_blk->nvalues;
    uint32_t else_n = else_blk->nvalues;

    XiPassChange chg = xi_opt_gvn_pre(f);
    assert(chg.values_changed);
    assert(join_add->op == XI_COPY);
    assert(then_blk->nvalues == then_n && "no new insertion in then (already has it)");
    assert(else_blk->nvalues == else_n && "no new insertion in else (already has it)");
    assert(join->phis != NULL);
    assert(join->phis->value.args[0] == then_add);
    assert(join->phis->value.args[1] == else_add);
    xi_func_free(f);
}

TEST(pre_different_ops_same_args) {
    XiFunc *f = make_func("pre_diff_ops", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);

    XiValue *add = xi_binary(f, entry, XI_ADD, &stub_int, x, y);
    XiValue *sub = xi_binary(f, entry, XI_SUB, &stub_int, x, y);
    xi_block_set_return(entry, sub);

    xi_opt_gvn_pre(f);
    assert(add->op == XI_ADD && "add should remain as-is");
    assert(sub->op == XI_SUB && "sub should remain as-is (different op, not redundant)");
    xi_func_free(f);
}

/* ========== 4. Commutative VN tests ========== */

TEST(commutative_mul) {
    XiFunc *f = make_func("comm_mul", &stub_int);
    XiBlock *entry = f->entry;
    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *m1 = xi_binary(f, entry, XI_MUL, &stub_int, x, y);
    XiValue *m2 = xi_binary(f, entry, XI_MUL, &stub_int, y, x);
    xi_block_set_return(entry, m2);

    xi_opt_gvn_pre(f);
    assert(m2->op == XI_COPY && "y*x should match x*y (commutative)");
    assert(m2->args[0] == m1);
    xi_func_free(f);
}

TEST(commutative_band) {
    XiFunc *f = make_func("comm_band", &stub_int);
    XiBlock *entry = f->entry;
    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *b1 = xi_binary(f, entry, XI_BAND, &stub_int, x, y);
    XiValue *b2 = xi_binary(f, entry, XI_BAND, &stub_int, y, x);
    xi_block_set_return(entry, b2);

    xi_opt_gvn_pre(f);
    assert(b2->op == XI_COPY && "y&x should match x&y (commutative)");
    assert(b2->args[0] == b1);
    xi_func_free(f);
}

TEST(commutative_bor) {
    XiFunc *f = make_func("comm_bor", &stub_int);
    XiBlock *entry = f->entry;
    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *o1 = xi_binary(f, entry, XI_BOR, &stub_int, x, y);
    XiValue *o2 = xi_binary(f, entry, XI_BOR, &stub_int, y, x);
    xi_block_set_return(entry, o2);

    xi_opt_gvn_pre(f);
    assert(o2->op == XI_COPY && "y|x should match x|y (commutative)");
    assert(o2->args[0] == o1);
    xi_func_free(f);
}

TEST(commutative_eq) {
    XiFunc *f = make_func("comm_eq", &stub_bool);
    XiBlock *entry = f->entry;
    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *e1 = xi_binary(f, entry, XI_EQ, &stub_bool, x, y);
    XiValue *e2 = xi_binary(f, entry, XI_EQ, &stub_bool, y, x);
    xi_block_set_return(entry, e2);

    xi_opt_gvn_pre(f);
    assert(e2->op == XI_COPY && "y==x should match x==y (commutative)");
    assert(e2->args[0] == e1);
    xi_func_free(f);
}

/* ========== 5. Non-commutative op tests ========== */

TEST(non_commutative_sub) {
    XiFunc *f = make_func("ncomm_sub", &stub_int);
    XiBlock *entry = f->entry;
    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *s1 = xi_binary(f, entry, XI_SUB, &stub_int, x, y);
    XiValue *s2 = xi_binary(f, entry, XI_SUB, &stub_int, y, x);
    xi_block_set_return(entry, s2);

    xi_opt_gvn_pre(f);
    assert(s1->op == XI_SUB && "x-y stays");
    assert(s2->op == XI_SUB && "y-x stays (not commutative with x-y)");
    xi_func_free(f);
}

TEST(non_commutative_lt) {
    XiFunc *f = make_func("ncomm_lt", &stub_bool);
    XiBlock *entry = f->entry;
    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *c1 = xi_binary(f, entry, XI_LT, &stub_bool, x, y);
    XiValue *c2 = xi_binary(f, entry, XI_LT, &stub_bool, y, x);
    xi_block_set_return(entry, c2);

    xi_opt_gvn_pre(f);
    assert(c1->op == XI_LT && "x<y stays");
    assert(c2->op == XI_LT && "y<x stays (LT is not commutative)");
    xi_func_free(f);
}

TEST(non_commutative_shl) {
    XiFunc *f = make_func("ncomm_shl", &stub_int);
    XiBlock *entry = f->entry;
    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *sh1 = xi_binary(f, entry, XI_SHL, &stub_int, x, y);
    XiValue *sh2 = xi_binary(f, entry, XI_SHL, &stub_int, y, x);
    xi_block_set_return(entry, sh2);

    xi_opt_gvn_pre(f);
    assert(sh1->op == XI_SHL && "x<<y stays");
    assert(sh2->op == XI_SHL && "y<<x stays (not commutative)");
    xi_func_free(f);
}

/* ========== 6. Memory / Load GVN ========== */

TEST(same_block_load_elimination) {
    XiFunc *f = make_func("sb_load", &stub_int);
    XiBlock *entry = f->entry;
    XiValue *obj = xi_param(f, entry, 0, &stub_any);

    XiValue *ld1 = xi_value_new(f, entry, XI_LOAD_FIELD, &stub_int, 1);
    ld1->args[0] = obj;
    ld1->aux_int = 5;

    XiValue *ld2 = xi_value_new(f, entry, XI_LOAD_FIELD, &stub_int, 1);
    ld2->args[0] = obj;
    ld2->aux_int = 5;
    xi_block_set_return(entry, ld2);

    xi_tbaa_annotate(f);
    xi_opt_gvn_pre(f);
    assert(ld2->op == XI_COPY && "second identical LOAD_FIELD should be eliminated");
    assert(ld2->args[0] == ld1);
    xi_func_free(f);
}

TEST(load_not_eliminated_after_store) {
    XiFunc *f = make_func("ld_after_store", &stub_int);
    XiBlock *entry = f->entry;
    XiValue *obj = xi_param(f, entry, 0, &stub_any);
    XiValue *val = xi_param(f, entry, 1, &stub_int);

    XiValue *ld1 = xi_value_new(f, entry, XI_LOAD_FIELD, &stub_int, 1);
    ld1->args[0] = obj;
    ld1->aux_int = 5;

    XiValue *st = xi_value_new(f, entry, XI_STORE_FIELD, &stub_int, 2);
    st->args[0] = obj;
    st->args[1] = val;
    st->aux_int = 5;

    XiValue *ld2 = xi_value_new(f, entry, XI_LOAD_FIELD, &stub_int, 1);
    ld2->args[0] = obj;
    ld2->aux_int = 5;
    xi_block_set_return(entry, ld2);

    xi_tbaa_annotate(f);
    xi_opt_gvn_pre(f);
    assert(ld2->op == XI_LOAD_FIELD && "load after same-field store must not be eliminated");
    xi_func_free(f);
}

TEST(load_eliminated_after_store_different_field) {
    XiFunc *f = make_func("ld_diff_field", &stub_int);
    XiBlock *entry = f->entry;
    XiValue *obj = xi_param(f, entry, 0, &stub_any);
    XiValue *val = xi_param(f, entry, 1, &stub_int);

    XiValue *ld1 = xi_value_new(f, entry, XI_LOAD_FIELD, &stub_int, 1);
    ld1->args[0] = obj;
    ld1->aux_int = 0;

    XiValue *st = xi_value_new(f, entry, XI_STORE_FIELD, &stub_int, 2);
    st->args[0] = obj;
    st->args[1] = val;
    st->aux_int = 1;

    XiValue *ld2 = xi_value_new(f, entry, XI_LOAD_FIELD, &stub_int, 1);
    ld2->args[0] = obj;
    ld2->aux_int = 0;
    xi_block_set_return(entry, ld2);

    xi_tbaa_annotate(f);
    xi_opt_gvn_pre(f);
    assert(ld2->op == XI_COPY &&
           "load field 0 after store field 1 should be eliminated (different field)");
    assert(ld2->args[0] == ld1);
    xi_func_free(f);
}

TEST(disjoint_group_load_after_store) {
    XiFunc *f = make_func("disjoint_grp", &stub_int);
    XiBlock *entry = f->entry;
    XiValue *obj = xi_param(f, entry, 0, &stub_any);
    XiValue *key = xi_param(f, entry, 1, &stub_int);
    XiValue *val = xi_param(f, entry, 2, &stub_int);

    XiValue *ld1 = xi_value_new(f, entry, XI_LOAD_FIELD, &stub_int, 1);
    ld1->args[0] = obj;
    ld1->aux_int = 3;

    XiValue *idx_set = xi_value_new(f, entry, XI_INDEX_SET, &stub_int, 3);
    idx_set->args[0] = obj;
    idx_set->args[1] = key;
    idx_set->args[2] = val;

    XiValue *ld2 = xi_value_new(f, entry, XI_LOAD_FIELD, &stub_int, 1);
    ld2->args[0] = obj;
    ld2->aux_int = 3;
    xi_block_set_return(entry, ld2);

    xi_tbaa_annotate(f);
    xi_opt_gvn_pre(f);
    assert(ld2->op == XI_COPY && "LOAD_FIELD after INDEX_SET: disjoint TBAA groups");
    assert(ld2->args[0] == ld1);
    xi_func_free(f);
}

TEST(load_not_eliminated_after_same_group_store) {
    XiFunc *f = make_func("same_grp_st", &stub_int);
    XiBlock *entry = f->entry;
    XiValue *obj = xi_param(f, entry, 0, &stub_any);
    XiValue *val = xi_param(f, entry, 1, &stub_int);

    XiValue *ld1 = xi_value_new(f, entry, XI_LOAD_FIELD, &stub_int, 1);
    ld1->args[0] = obj;
    ld1->aux_int = 3;

    XiValue *st = xi_value_new(f, entry, XI_STORE_FIELD, &stub_int, 2);
    st->args[0] = obj;
    st->args[1] = val;
    st->aux_int = 3;

    XiValue *ld2 = xi_value_new(f, entry, XI_LOAD_FIELD, &stub_int, 1);
    ld2->args[0] = obj;
    ld2->aux_int = 3;
    xi_block_set_return(entry, ld2);

    xi_tbaa_annotate(f);
    xi_opt_gvn_pre(f);
    assert(ld2->op == XI_LOAD_FIELD && "load after same-group store must not be eliminated");
    xi_func_free(f);
}

TEST(two_loads_different_slots_both_survive) {
    XiFunc *f = make_func("diff_slots", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *ld1 = xi_value_new(f, entry, XI_GET_SHARED, &stub_int, 0);
    ld1->aux_int = 0;
    XiValue *ld2 = xi_value_new(f, entry, XI_GET_SHARED, &stub_int, 0);
    ld2->aux_int = 1;
    xi_block_set_return(entry, ld2);

    xi_tbaa_annotate(f);
    xi_opt_gvn_pre(f);
    assert(ld1->op == XI_GET_SHARED && "shared[0] survives");
    assert(ld2->op == XI_GET_SHARED && "shared[1] survives (different slot)");
    xi_func_free(f);
}

/* ========== 7. Edge cases / safety ========== */

TEST(minimal_return_only_noop) {
    XiFunc *f = make_func("min_ret", &stub_int);
    XiBlock *entry = f->entry;
    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *add = xi_binary(f, entry, XI_ADD, &stub_int, x, y);
    xi_block_set_return(entry, add);

    XiPassChange chg = xi_opt_gvn_pre(f);
    assert(!chg.values_changed && "single add has nothing to eliminate");
    assert(add->op == XI_ADD);
    xi_func_free(f);
}

TEST(empty_func_noop) {
    XiFunc *f = make_func("empty", &stub_int);
    XiBlock *entry = f->entry;
    xi_block_set_return(entry, xi_const_int(f, entry, 0, &stub_int));

    XiPassChange chg = xi_opt_gvn_pre(f);
    assert(!chg.values_changed && "no redundancy in trivial func");
    xi_func_free(f);
}

TEST(single_value_noop) {
    XiFunc *f = make_func("single", &stub_int);
    XiBlock *entry = f->entry;
    XiValue *x = xi_param(f, entry, 0, &stub_int);
    xi_block_set_return(entry, x);

    XiPassChange chg = xi_opt_gvn_pre(f);
    assert(!chg.values_changed && "one value has no redundancy");
    xi_func_free(f);
}

TEST(phi_not_eliminated) {
    XiFunc *f = make_func("phi_kept", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *cond = xi_param(f, entry, 2, &stub_bool);

    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);
    XiBlock *join = xi_block_new(f);
    then_blk->sealed = true;
    else_blk->sealed = true;
    join->sealed = true;

    xi_block_set_if(entry, cond, then_blk, else_blk);
    xi_block_set_jump(then_blk, join);
    xi_block_set_jump(else_blk, join);

    XiPhi *phi = xi_phi_new(f, join, &stub_int, join->npreds);
    phi->value.args[0] = x;
    phi->value.args[1] = y;
    xi_block_set_return(join, &phi->value);

    xi_opt_gvn_pre(f);
    assert(phi->value.op == XI_PHI && "phi nodes must not be GVN'd away");
    xi_func_free(f);
}

TEST(const_same_value_not_gvnd) {
    XiFunc *f = make_func("const_gvn", &stub_int);
    XiBlock *entry = f->entry;
    XiBlock *blk2 = xi_block_new(f);
    blk2->sealed = true;

    XiValue *c1 = xi_const_int(f, entry, 42, &stub_int);
    (void) c1;
    xi_block_set_jump(entry, blk2);

    XiValue *c2 = xi_const_int(f, blk2, 42, &stub_int);
    xi_block_set_return(blk2, c2);

    xi_opt_gvn_pre(f);
    assert(c2->op == XI_CONST &&
           "constants are not eliminated by GVN-PRE (cheap to rematerialize)");
    xi_func_free(f);
}

TEST(zero_effect_op_without_vn_policy_not_eliminated) {
    XiFunc *f = make_func("is_not_vn", &stub_bool);
    XiBlock *entry = f->entry;
    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *e1 = xi_binary(f, entry, XI_IS, &stub_bool, x, y);
    XiValue *e2 = xi_binary(f, entry, XI_IS, &stub_bool, x, y);
    xi_block_set_return(entry, e2);

    xi_opt_gvn_pre(f);
    assert(e1->op == XI_IS && "first type check should remain");
    assert(e2->op == XI_IS && "type check needs explicit VN policy");
    xi_func_free(f);
}

/* ========== 8. Verifier integration ========== */

TEST(verify_after_full_re) {
    XiFunc *f = make_func("v_full_re", &stub_int);
    XiBlock *entry = f->entry;
    XiBlock *blk2 = xi_block_new(f);
    blk2->sealed = true;

    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    xi_binary(f, entry, XI_ADD, &stub_int, x, y);
    xi_block_set_jump(entry, blk2);

    XiValue *dup = xi_binary(f, blk2, XI_ADD, &stub_int, x, y);
    xi_block_set_return(blk2, dup);

    xi_opt_gvn_pre(f);
    assert(dup->op == XI_COPY);

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok)
        printf("  verify error: %s\n", errbuf);
    assert(ok && "IR must remain valid after full redundancy elimination");
    xi_func_free(f);
}

TEST(verify_after_pre_insertion) {
    XiFunc *f = make_func("v_pre", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *cond = xi_param(f, entry, 2, &stub_bool);

    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);
    XiBlock *join = xi_block_new(f);
    then_blk->sealed = true;
    else_blk->sealed = true;
    join->sealed = true;

    xi_block_set_if(entry, cond, then_blk, else_blk);
    xi_binary(f, then_blk, XI_ADD, &stub_int, x, y);
    xi_block_set_jump(then_blk, join);
    xi_block_set_jump(else_blk, join);

    XiValue *join_add = xi_binary(f, join, XI_ADD, &stub_int, x, y);
    xi_block_set_return(join, join_add);

    xi_opt_gvn_pre(f);
    assert(join_add->op == XI_COPY);

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok)
        printf("  verify error: %s\n", errbuf);
    assert(ok && "IR must remain valid after PRE insertion");
    xi_func_free(f);
}

TEST(verify_after_load_elimination) {
    XiFunc *f = make_func("v_load_elim", &stub_int);
    XiBlock *entry = f->entry;
    XiBlock *blk2 = xi_block_new(f);
    blk2->sealed = true;

    XiValue *ld1 = xi_value_new(f, entry, XI_GET_SHARED, &stub_int, 0);
    ld1->aux_int = 0;
    xi_block_set_jump(entry, blk2);

    XiValue *ld2 = xi_value_new(f, blk2, XI_GET_SHARED, &stub_int, 0);
    ld2->aux_int = 0;
    xi_block_set_return(blk2, ld2);

    xi_tbaa_annotate(f);
    xi_opt_gvn_pre(f);
    assert(ld2->op == XI_COPY);

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok)
        printf("  verify error: %s\n", errbuf);
    assert(ok && "IR must remain valid after load elimination");
    xi_func_free(f);
}

/* ========== 9. Additional coverage ========== */

TEST(div_not_eliminated_may_throw) {
    XiFunc *f = make_func("sb_div", &stub_int);
    XiBlock *entry = f->entry;
    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *d1 = xi_binary(f, entry, XI_DIV, &stub_int, x, y);
    XiValue *d2 = xi_binary(f, entry, XI_DIV, &stub_int, x, y);
    xi_block_set_return(entry, d2);

    xi_opt_gvn_pre(f);
    assert(d1->op == XI_DIV && "DIV has MAY_THROW, GVN must not eliminate");
    assert(d2->op == XI_DIV && "second DIV also survives");
    xi_func_free(f);
}

TEST(mod_not_eliminated_may_throw) {
    XiFunc *f = make_func("sb_mod", &stub_int);
    XiBlock *entry = f->entry;
    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *m1 = xi_binary(f, entry, XI_MOD, &stub_int, x, y);
    XiValue *m2 = xi_binary(f, entry, XI_MOD, &stub_int, x, y);
    xi_block_set_return(entry, m2);

    xi_opt_gvn_pre(f);
    assert(m1->op == XI_MOD && "MOD has MAY_THROW, GVN must not eliminate");
    assert(m2->op == XI_MOD && "second MOD also survives");
    xi_func_free(f);
}

TEST(same_block_shl_elimination) {
    XiFunc *f = make_func("sb_shl", &stub_int);
    XiBlock *entry = f->entry;
    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *s1 = xi_binary(f, entry, XI_SHL, &stub_int, x, y);
    XiValue *s2 = xi_binary(f, entry, XI_SHL, &stub_int, x, y);
    xi_block_set_return(entry, s2);

    xi_opt_gvn_pre(f);
    assert(s2->op == XI_COPY && "second x<<y should become COPY");
    assert(s2->args[0] == s1);
    xi_func_free(f);
}

TEST(same_block_shr_elimination) {
    XiFunc *f = make_func("sb_shr", &stub_int);
    XiBlock *entry = f->entry;
    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *s1 = xi_binary(f, entry, XI_SHR, &stub_int, x, y);
    XiValue *s2 = xi_binary(f, entry, XI_SHR, &stub_int, x, y);
    xi_block_set_return(entry, s2);

    xi_opt_gvn_pre(f);
    assert(s2->op == XI_COPY && "second x>>y should become COPY");
    assert(s2->args[0] == s1);
    xi_func_free(f);
}

TEST(same_block_bxor_elimination) {
    XiFunc *f = make_func("sb_bxor", &stub_int);
    XiBlock *entry = f->entry;
    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *x1 = xi_binary(f, entry, XI_BXOR, &stub_int, x, y);
    XiValue *x2 = xi_binary(f, entry, XI_BXOR, &stub_int, x, y);
    xi_block_set_return(entry, x2);

    xi_opt_gvn_pre(f);
    assert(x2->op == XI_COPY && "second x^y should become COPY");
    assert(x2->args[0] == x1);
    xi_func_free(f);
}

TEST(same_block_bnot_elimination) {
    XiFunc *f = make_func("sb_bnot", &stub_int);
    XiBlock *entry = f->entry;
    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *n1 = xi_value_new(f, entry, XI_BNOT, &stub_int, 1);
    n1->args[0] = x;
    XiValue *n2 = xi_value_new(f, entry, XI_BNOT, &stub_int, 1);
    n2->args[0] = x;
    xi_block_set_return(entry, n2);

    xi_opt_gvn_pre(f);
    assert(n2->op == XI_COPY && "second ~x should become COPY");
    assert(n2->args[0] == n1);
    xi_func_free(f);
}

TEST(same_block_eq_elimination) {
    XiFunc *f = make_func("sb_eq", &stub_bool);
    XiBlock *entry = f->entry;
    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *e1 = xi_binary(f, entry, XI_EQ, &stub_bool, x, y);
    XiValue *e2 = xi_binary(f, entry, XI_EQ, &stub_bool, x, y);
    xi_block_set_return(entry, e2);

    xi_opt_gvn_pre(f);
    assert(e2->op == XI_COPY && "second x==y should become COPY");
    assert(e2->args[0] == e1);
    xi_func_free(f);
}

TEST(same_block_lt_elimination) {
    XiFunc *f = make_func("sb_lt", &stub_bool);
    XiBlock *entry = f->entry;
    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *c1 = xi_binary(f, entry, XI_LT, &stub_bool, x, y);
    XiValue *c2 = xi_binary(f, entry, XI_LT, &stub_bool, x, y);
    xi_block_set_return(entry, c2);

    xi_opt_gvn_pre(f);
    assert(c2->op == XI_COPY && "second x<y should become COPY");
    assert(c2->args[0] == c1);
    xi_func_free(f);
}

TEST(commutative_bxor) {
    XiFunc *f = make_func("comm_bxor", &stub_int);
    XiBlock *entry = f->entry;
    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *x1 = xi_binary(f, entry, XI_BXOR, &stub_int, x, y);
    XiValue *x2 = xi_binary(f, entry, XI_BXOR, &stub_int, y, x);
    xi_block_set_return(entry, x2);

    xi_opt_gvn_pre(f);
    assert(x2->op == XI_COPY && "y^x should match x^y (commutative)");
    assert(x2->args[0] == x1);
    xi_func_free(f);
}

TEST(commutative_ne) {
    XiFunc *f = make_func("comm_ne", &stub_bool);
    XiBlock *entry = f->entry;
    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *n1 = xi_binary(f, entry, XI_NE, &stub_bool, x, y);
    XiValue *n2 = xi_binary(f, entry, XI_NE, &stub_bool, y, x);
    xi_block_set_return(entry, n2);

    xi_opt_gvn_pre(f);
    assert(n2->op == XI_COPY && "y!=x should match x!=y (commutative)");
    assert(n2->args[0] == n1);
    xi_func_free(f);
}

TEST(non_commutative_shr) {
    XiFunc *f = make_func("ncomm_shr", &stub_int);
    XiBlock *entry = f->entry;
    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *s1 = xi_binary(f, entry, XI_SHR, &stub_int, x, y);
    XiValue *s2 = xi_binary(f, entry, XI_SHR, &stub_int, y, x);
    xi_block_set_return(entry, s2);

    xi_opt_gvn_pre(f);
    assert(s1->op == XI_SHR && "x>>y stays");
    assert(s2->op == XI_SHR && "y>>x stays (not commutative)");
    xi_func_free(f);
}

TEST(non_commutative_gt) {
    XiFunc *f = make_func("ncomm_gt", &stub_bool);
    XiBlock *entry = f->entry;
    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *c1 = xi_binary(f, entry, XI_GT, &stub_bool, x, y);
    XiValue *c2 = xi_binary(f, entry, XI_GT, &stub_bool, y, x);
    xi_block_set_return(entry, c2);

    xi_opt_gvn_pre(f);
    assert(c1->op == XI_GT && "x>y stays");
    assert(c2->op == XI_GT && "y>x stays (GT is not commutative)");
    xi_func_free(f);
}

TEST(cross_block_load_same_shared_slot_eliminated) {
    XiFunc *f = make_func("xblk_shared", &stub_int);
    XiBlock *entry = f->entry;
    XiBlock *b2 = xi_block_new(f);
    XiBlock *b3 = xi_block_new(f);
    b2->sealed = true;
    b3->sealed = true;

    XiValue *ld1 = xi_value_new(f, entry, XI_GET_SHARED, &stub_int, 0);
    ld1->aux_int = 2;
    xi_block_set_jump(entry, b2);
    xi_block_set_jump(b2, b3);

    XiValue *ld2 = xi_value_new(f, b3, XI_GET_SHARED, &stub_int, 0);
    ld2->aux_int = 2;
    xi_block_set_return(b3, ld2);

    xi_tbaa_annotate(f);
    xi_opt_gvn_pre(f);
    assert(ld2->op == XI_COPY && "shared[2] in b3 dominated by entry's load");
    assert(ld2->args[0] == ld1);
    xi_func_free(f);
}

TEST(load_dependent_expr_not_reused_after_shared_store) {
    XiFunc *f = make_func("shared_store_dep", &stub_bool);
    XiBlock *entry = f->entry;

    XiValue *limit = xi_const_int(f, entry, 10, &stub_int);
    XiValue *one = xi_const_int(f, entry, 1, &stub_int);

    XiValue *ld1 = xi_value_new(f, entry, XI_GET_SHARED, &stub_int, 0);
    ld1->aux_int = 0;
    XiValue *cmp1 = xi_binary(f, entry, XI_LT, &stub_bool, ld1, limit);

    XiValue *next = xi_binary(f, entry, XI_ADD, &stub_int, ld1, one);
    XiValue *store = xi_value_new(f, entry, XI_SET_SHARED, &stub_int, 1);
    store->args[0] = next;
    store->aux_int = 0;

    XiValue *ld2 = xi_value_new(f, entry, XI_GET_SHARED, &stub_int, 0);
    ld2->aux_int = 0;
    XiValue *cmp2 = xi_binary(f, entry, XI_LT, &stub_bool, ld2, limit);
    xi_block_set_return(entry, cmp2);

    xi_tbaa_annotate(f);
    xi_opt_gvn_pre(f);

    assert(ld2->op == XI_GET_SHARED && "load after shared store must survive");
    assert(cmp2->op == XI_LT && "dependent compare must not reuse the stale load");
    assert(cmp2->args[0] == ld2);
    (void) cmp1;
    xi_func_free(f);
}

TEST(pre_bor_insertion) {
    XiFunc *f = make_func("pre_bor", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *cond = xi_param(f, entry, 2, &stub_bool);

    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);
    XiBlock *join = xi_block_new(f);
    then_blk->sealed = true;
    else_blk->sealed = true;
    join->sealed = true;

    xi_block_set_if(entry, cond, then_blk, else_blk);
    XiValue *then_or = xi_binary(f, then_blk, XI_BOR, &stub_int, x, y);
    xi_block_set_jump(then_blk, join);
    xi_block_set_jump(else_blk, join);

    XiValue *join_or = xi_binary(f, join, XI_BOR, &stub_int, x, y);
    xi_block_set_return(join, join_or);

    XiPassChange chg = xi_opt_gvn_pre(f);
    assert(chg.values_changed);
    assert(join_or->op == XI_COPY);
    assert(join->phis != NULL);
    assert(join->phis->value.args[0] == then_or);
    xi_func_free(f);
}

TEST(dom_mul_cross_block) {
    XiFunc *f = make_func("dom_mul", &stub_int);
    XiBlock *entry = f->entry;
    XiBlock *blk2 = xi_block_new(f);
    blk2->sealed = true;

    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *m1 = xi_binary(f, entry, XI_MUL, &stub_int, x, y);
    xi_block_set_jump(entry, blk2);

    XiValue *m2 = xi_binary(f, blk2, XI_MUL, &stub_int, x, y);
    xi_block_set_return(blk2, m2);

    xi_opt_gvn_pre(f);
    assert(m2->op == XI_COPY && "dominated MUL should become COPY");
    assert(m2->args[0] == m1);
    xi_func_free(f);
}

TEST(verify_full_pipeline_diamond) {
    XiFunc *f = make_func("v_pipe", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *y = xi_param(f, entry, 1, &stub_int);
    XiValue *cond = xi_param(f, entry, 2, &stub_bool);

    XiBlock *t = xi_block_new(f);
    XiBlock *e = xi_block_new(f);
    XiBlock *j = xi_block_new(f);
    t->sealed = true;
    e->sealed = true;
    j->sealed = true;

    xi_block_set_if(entry, cond, t, e);
    XiValue *t_add = xi_binary(f, t, XI_ADD, &stub_int, x, y);
    XiValue *t_mul = xi_binary(f, t, XI_MUL, &stub_int, t_add, x);
    (void) t_mul;
    xi_block_set_jump(t, j);

    XiValue *e_add = xi_binary(f, e, XI_ADD, &stub_int, x, y);
    (void) e_add;
    xi_block_set_jump(e, j);

    XiValue *j_add = xi_binary(f, j, XI_ADD, &stub_int, x, y);
    xi_block_set_return(j, j_add);

    XiPassChange chg = xi_opt_gvn_pre(f);
    assert(chg.values_changed);
    assert(j_add->op == XI_COPY);

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok)
        printf("  verify error: %s\n", errbuf);
    assert(ok && "complex diamond pipeline should verify clean");
    xi_func_free(f);
}

/* ========== Main ========== */

int main(void) {
    printf("=== Xi GVN-PRE tests ===\n\n");

    /* Original 8 tests */
    run_gvn_pre_inserts_missing_edge_expression();
    run_gvn_pre_phi_merges_when_all_predecessors_have_leaders();
    run_gvn_pre_skips_critical_edge_predecessor();
    run_gvn_pre_recognizes_commutative_argument_swap();
    run_gvn_pre_handles_three_predecessor_join();
    run_gvn_pre_does_not_speculate_loads();
    run_gvn_eliminates_cross_block_load_via_memssa();
    run_gvn_no_cross_block_load_elim_after_call();

    /* 1. Full Redundancy Elimination (same block) */
    run_same_block_add_elimination();
    run_same_block_sub_elimination();
    run_same_block_mul_elimination();
    run_same_block_band_elimination();
    run_same_block_bor_elimination();
    run_same_block_neg_elimination();
    run_same_block_not_elimination();
    run_same_block_triple_redundancy();

    /* 2. Full Redundancy Elimination (cross-block via dominator) */
    run_dom_chain_eliminates();
    run_dom_three_blocks_chain();
    run_dom_diamond_both_branches();
    run_dom_no_elimination_when_not_dominating();
    run_dom_entry_dominates_all();
    run_dom_sub_not_commutative();

    /* 3. Partial Redundancy Elimination variations */
    run_pre_sub_insertion();
    run_pre_mul_insertion();
    run_pre_nested_diamond();
    run_pre_four_predecessors();
    run_pre_expression_chain();
    run_pre_constant_folding_independent();
    run_pre_no_insert_if_all_have_it();
    run_pre_different_ops_same_args();

    /* 4. Commutative VN tests */
    run_commutative_mul();
    run_commutative_band();
    run_commutative_bor();
    run_commutative_eq();

    /* 5. Non-commutative op tests */
    run_non_commutative_sub();
    run_non_commutative_lt();
    run_non_commutative_shl();

    /* 6. Memory / Load GVN */
    run_same_block_load_elimination();
    run_load_not_eliminated_after_store();
    run_load_eliminated_after_store_different_field();
    run_disjoint_group_load_after_store();
    run_load_not_eliminated_after_same_group_store();
    run_two_loads_different_slots_both_survive();

    /* 7. Edge cases / safety */
    run_minimal_return_only_noop();
    run_empty_func_noop();
    run_single_value_noop();
    run_phi_not_eliminated();
    run_const_same_value_not_gvnd();
    run_zero_effect_op_without_vn_policy_not_eliminated();

    /* 8. Verifier integration */
    run_verify_after_full_re();
    run_verify_after_pre_insertion();
    run_verify_after_load_elimination();

    /* 9. Additional coverage */
    run_div_not_eliminated_may_throw();
    run_mod_not_eliminated_may_throw();
    run_same_block_shl_elimination();
    run_same_block_shr_elimination();
    run_same_block_bxor_elimination();
    run_same_block_bnot_elimination();
    run_same_block_eq_elimination();
    run_same_block_lt_elimination();
    run_commutative_bxor();
    run_commutative_ne();
    run_non_commutative_shr();
    run_non_commutative_gt();
    run_cross_block_load_same_shared_slot_eliminated();
    run_load_dependent_expr_not_reused_after_shared_store();
    run_pre_bor_insertion();
    run_dom_mul_cross_block();
    run_verify_full_pipeline_diamond();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
