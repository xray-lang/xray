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
#include "../../../src/ir/xi_verify.h"
#include "../../../src/runtime/value/xtype.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_bool = {.kind = XR_KIND_BOOL, .id = 2, .frozen = true};

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

/* ========== Main ========== */

int main(void) {
    printf("=== Xi GVN-PRE tests ===\n\n");

    run_gvn_pre_inserts_missing_edge_expression();
    run_gvn_pre_phi_merges_when_all_predecessors_have_leaders();
    run_gvn_pre_skips_critical_edge_predecessor();
    run_gvn_pre_recognizes_commutative_argument_swap();
    run_gvn_pre_handles_three_predecessor_join();
    run_gvn_pre_does_not_speculate_loads();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
