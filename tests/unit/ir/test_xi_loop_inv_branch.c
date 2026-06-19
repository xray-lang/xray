/*
 * Unit tests for Xi loop-invariant branch hoisting.
 */

#include "../../../src/ir/xi_analysis.h"
#include "../../../src/ir/xi_loop.h"
#include "../../../src/ir/xi_opt_loop_inv_branch.h"
#include "../../../src/ir/xi_verify.h"
#include "../../../src/ir/xi.h"
#include "../../../src/runtime/value/xtype.h"

#include <stdio.h>

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_bool = {.kind = XR_KIND_BOOL, .id = 2, .frozen = true};

static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("  FAIL: %s (line %d)\n", #cond, __LINE__);                                     \
            tests_failed++;                                                                        \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define TEST(name)                                                                                 \
    static void test_##name(void);                                                                 \
    static void run_##name(void) {                                                                 \
        printf("--- " #name " ---\n");                                                             \
        test_##name();                                                                             \
        printf("  PASS\n");                                                                        \
        tests_passed++;                                                                            \
    }                                                                                              \
    static void test_##name(void)

static int pred_index(const XiBlock *blk, const XiBlock *pred) {
    if (!blk || !pred)
        return -1;
    for (uint16_t i = 0; i < blk->npreds; i++) {
        if (blk->preds[i] == pred)
            return (int) i;
    }
    return -1;
}

static bool verify_func(XiFunc *f) {
    char err[256] = {0};
    xi_cfg_invalidate(f);
    bool ok = xi_verify(f, err, sizeof(err));
    if (!ok)
        printf("  verify error: %s\n", err);
    return ok;
}

/* Build a loop with an invariant branch inside the body. */
static XiFunc *build_loop_with_invariant_branch(void) {
    XiFunc *f = xi_func_new("inv_branch_target", &stub_int);
    XiBlock *entry = xi_block_new(f);
    XiBlock *header = xi_block_new(f);
    XiBlock *branch_blk = xi_block_new(f);
    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);
    XiBlock *merge = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);
    entry->sealed = header->sealed = branch_blk->sealed = true;
    then_blk->sealed = else_blk->sealed = merge->sealed = exit_blk->sealed = true;

    XiValue *start = xi_const_int(f, entry, 0, &stub_int);
    XiValue *limit = xi_const_int(f, entry, 10, &stub_int);
    XiValue *step = xi_const_int(f, entry, 1, &stub_int);
    XiValue *param_a = xi_const_int(f, entry, 5, &stub_int);
    XiValue *param_b = xi_const_int(f, entry, 3, &stub_int);

    xi_block_set_jump(entry, header);
    xi_block_set_jump(merge, header);

    XiPhi *iv = xi_phi_new(f, header, &stub_int, header->npreds);
    XiValue *cond = xi_binary(f, header, XI_LT, &stub_bool, &iv->value, limit);
    xi_block_set_if(header, cond, branch_blk, exit_blk);

    /* Invariant branch: both operands from outside the loop. */
    XiValue *inv_cond = xi_binary(f, branch_blk, XI_GT, &stub_bool, param_a, param_b);
    xi_block_set_if(branch_blk, inv_cond, then_blk, else_blk);

    xi_binary(f, then_blk, XI_ADD, &stub_int, &iv->value, param_a);
    xi_block_set_jump(then_blk, merge);

    xi_binary(f, else_blk, XI_ADD, &stub_int, &iv->value, param_b);
    xi_block_set_jump(else_blk, merge);

    XiValue *next = xi_binary(f, merge, XI_ADD, &stub_int, &iv->value, step);

    int pre_idx = pred_index(header, entry);
    int latch_idx = pred_index(header, merge);
    iv->value.args[pre_idx] = start;
    iv->value.args[latch_idx] = next;

    xi_block_set_return(exit_blk, xi_const_int(f, exit_blk, 0, &stub_int));
    return f;
}

static XiFunc *build_loop_with_phi_branch_condition(void) {
    XiFunc *f = xi_func_new("phi_branch_condition", &stub_int);
    XiBlock *entry = xi_block_new(f);
    XiBlock *header = xi_block_new(f);
    XiBlock *split = xi_block_new(f);
    XiBlock *true_edge = xi_block_new(f);
    XiBlock *false_edge = xi_block_new(f);
    XiBlock *phi_branch = xi_block_new(f);
    XiBlock *then_body = xi_block_new(f);
    XiBlock *else_body = xi_block_new(f);
    XiBlock *merge = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);
    entry->sealed = header->sealed = split->sealed = true;
    true_edge->sealed = false_edge->sealed = phi_branch->sealed = true;
    then_body->sealed = else_body->sealed = merge->sealed = exit_blk->sealed = true;

    XiValue *start = xi_const_int(f, entry, 0, &stub_int);
    XiValue *limit = xi_const_int(f, entry, 10, &stub_int);
    XiValue *step = xi_const_int(f, entry, 1, &stub_int);
    XiValue *true_val = xi_const_bool(f, entry, true, &stub_bool);
    XiValue *false_val = xi_const_bool(f, entry, false, &stub_bool);

    xi_block_set_jump(entry, header);
    xi_block_set_jump(merge, header);

    XiPhi *iv = xi_phi_new(f, header, &stub_int, header->npreds);
    XiValue *loop_cond = xi_binary(f, header, XI_LT, &stub_bool, &iv->value, limit);
    xi_block_set_if(header, loop_cond, split, exit_blk);

    XiValue *edge_cond = xi_binary(f, split, XI_GT, &stub_bool, &iv->value, start);
    xi_block_set_if(split, edge_cond, true_edge, false_edge);

    xi_block_set_jump(true_edge, phi_branch);
    xi_block_set_jump(false_edge, phi_branch);

    XiPhi *branch_cond = xi_phi_new(f, phi_branch, &stub_bool, phi_branch->npreds);
    int true_idx = pred_index(phi_branch, true_edge);
    int false_idx = pred_index(phi_branch, false_edge);
    branch_cond->value.args[true_idx] = true_val;
    branch_cond->value.args[false_idx] = false_val;
    xi_block_set_if(phi_branch, &branch_cond->value, then_body, else_body);

    xi_binary(f, then_body, XI_ADD, &stub_int, &iv->value, step);
    xi_block_set_jump(then_body, merge);

    xi_binary(f, else_body, XI_ADD, &stub_int, &iv->value, start);
    xi_block_set_jump(else_body, merge);

    XiValue *next = xi_binary(f, merge, XI_ADD, &stub_int, &iv->value, step);

    int pre_idx = pred_index(header, entry);
    int latch_idx = pred_index(header, merge);
    iv->value.args[pre_idx] = start;
    iv->value.args[latch_idx] = next;

    xi_block_set_return(exit_blk, xi_const_int(f, exit_blk, 0, &stub_int));
    return f;
}

TEST(hoists_invariant_condition_to_preheader) {
    XiFunc *f = build_loop_with_invariant_branch();
    ASSERT(f != NULL);
    ASSERT(verify_func(f));

    XiPassChange chg = xi_opt_loop_inv_branch(f);
    ASSERT(chg.values_changed);
    ASSERT(chg.n_added >= 1);
    ASSERT(verify_func(f));
    xi_func_free(f);
}

TEST(skips_phi_branch_condition) {
    XiFunc *f = build_loop_with_phi_branch_condition();
    ASSERT(f != NULL);
    ASSERT(verify_func(f));

    XiPassChange chg = xi_opt_loop_inv_branch(f);
    ASSERT(!chg.values_changed);
    ASSERT(chg.n_added == 0);
    ASSERT(verify_func(f));
    xi_func_free(f);
}

TEST(no_loop_no_change) {
    XiFunc *f = xi_func_new("no_loop", &stub_int);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    XiValue *ret = xi_const_int(f, entry, 0, &stub_int);
    xi_block_set_return(entry, ret);

    XiPassChange chg = xi_opt_loop_inv_branch(f);
    ASSERT(!chg.cfg_changed);
    ASSERT(!chg.values_changed);
    xi_func_free(f);
}

TEST(skips_loop_variant_branch) {
    XiFunc *f = xi_func_new("variant_branch", &stub_int);
    XiBlock *entry = xi_block_new(f);
    XiBlock *header = xi_block_new(f);
    XiBlock *body = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);
    entry->sealed = header->sealed = body->sealed = exit_blk->sealed = true;

    XiValue *start = xi_const_int(f, entry, 0, &stub_int);
    XiValue *limit = xi_const_int(f, entry, 10, &stub_int);
    XiValue *step = xi_const_int(f, entry, 1, &stub_int);

    xi_block_set_jump(entry, header);
    xi_block_set_jump(body, header);

    XiPhi *iv = xi_phi_new(f, header, &stub_int, header->npreds);
    XiValue *cond = xi_binary(f, header, XI_LT, &stub_bool, &iv->value, limit);
    xi_block_set_if(header, cond, body, exit_blk);

    XiValue *next = xi_binary(f, body, XI_ADD, &stub_int, &iv->value, step);

    int pre_idx = pred_index(header, entry);
    int latch_idx = pred_index(header, body);
    iv->value.args[pre_idx] = start;
    iv->value.args[latch_idx] = next;

    xi_block_set_return(exit_blk, xi_const_int(f, exit_blk, 0, &stub_int));

    XiPassChange chg = xi_opt_loop_inv_branch(f);
    ASSERT(!chg.values_changed);
    xi_func_free(f);
}

int main(void) {
    printf("=== Xi Loop Invariant Branch Tests ===\n\n");

    run_hoists_invariant_condition_to_preheader();
    run_skips_phi_branch_condition();
    run_no_loop_no_change();
    run_skips_loop_variant_branch();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
