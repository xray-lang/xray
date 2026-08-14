/*
 * Unit tests for Xi loop splitting.
 */

#include "../../../src/ir/xi_analysis.h"
#include "../../../src/ir/xi_loop.h"
#include "../../../src/ir/xi_opt_loop_split.h"
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

/* Build `for (i = 0; i < header_bound; i++) { if (i < exit_bound) body else
 * early_exit }`, or the mirrored `if (i >= exit_bound) early_exit else body`
 * when exit_on_true is set.  header_bound_value overrides the header's
 * constant bound with a caller-supplied value so the same skeleton can carry
 * a bound that is not known at compile time. */
static XiFunc *build_loop_with_body_exit(const char *name, int64_t header_bound,
                                         int64_t exit_bound, bool exit_on_true,
                                         XiValue *(*make_bound)(XiFunc *, XiBlock *),
                                         XiBlock **out_check, XiBlock **out_body,
                                         XiBlock **out_early_exit) {
    XiFunc *f = xi_func_new(name, &stub_int);
    XiBlock *entry = xi_block_new(f);
    XiBlock *header = xi_block_new(f);
    XiBlock *check_blk = xi_block_new(f);
    XiBlock *body = xi_block_new(f);
    XiBlock *early_exit = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);
    entry->sealed = header->sealed = check_blk->sealed = true;
    body->sealed = early_exit->sealed = exit_blk->sealed = true;

    XiValue *start = xi_const_int(f, entry, 0, &stub_int);
    XiValue *step = xi_const_int(f, entry, 1, &stub_int);
    XiValue *shared_bound = make_bound ? make_bound(f, entry) : NULL;
    XiValue *limit = shared_bound ? shared_bound : xi_const_int(f, entry, header_bound, &stub_int);
    XiValue *array_len =
        shared_bound ? shared_bound : xi_const_int(f, entry, exit_bound, &stub_int);

    xi_block_set_jump(entry, header);
    xi_block_set_jump(body, header);

    XiPhi *iv = xi_phi_new(f, header, &stub_int, header->npreds);
    XiValue *cond = xi_binary(f, header, XI_LT, &stub_bool, &iv->value, limit);
    xi_block_set_if(header, cond, check_blk, exit_blk);

    if (exit_on_true) {
        XiValue *check_cond = xi_binary(f, check_blk, XI_GE, &stub_bool, &iv->value, array_len);
        xi_block_set_if(check_blk, check_cond, early_exit, body);
    } else {
        XiValue *check_cond = xi_binary(f, check_blk, XI_LT, &stub_bool, &iv->value, array_len);
        xi_block_set_if(check_blk, check_cond, body, early_exit);
    }

    XiValue *next = xi_binary(f, body, XI_ADD, &stub_int, &iv->value, step);

    int pre_idx = pred_index(header, entry);
    int latch_idx = pred_index(header, body);
    iv->value.args[pre_idx] = start;
    iv->value.args[latch_idx] = next;

    xi_block_set_return(exit_blk, xi_const_int(f, exit_blk, 0, &stub_int));
    xi_block_set_return(early_exit, xi_const_int(f, early_exit, -1, &stub_int));

    if (out_check)
        *out_check = check_blk;
    if (out_body)
        *out_body = body;
    if (out_early_exit)
        *out_early_exit = early_exit;
    return f;
}

/* An opaque loop-invariant bound: not a constant, so only an operand-identity
 * match can relate the header's comparison to the body's. */
static XiValue *make_opaque_bound(XiFunc *f, XiBlock *entry) {
    XiValue *a = xi_const_int(f, entry, 10, &stub_int);
    XiValue *b = xi_const_int(f, entry, 1, &stub_int);
    return xi_binary(f, entry, XI_ADD, &stub_int, a, b);
}

/* `i < 10` in the header leaves the whole range [5, 10) able to take a
 * `i < 5` body exit, so the exit edge is live and must survive. */
TEST(keeps_exit_the_header_bound_does_not_rule_out) {
    XiBlock *check_blk = NULL;
    XiFunc *f = build_loop_with_body_exit("keeps_false_exit", 10, 5, false, NULL, &check_blk, NULL,
                                          NULL);
    ASSERT(f != NULL);
    ASSERT(verify_func(f));

    uint32_t orig_nblocks = f->nblocks;
    XiPassChange chg = xi_opt_loop_split(f);
    ASSERT(!chg.cfg_changed);
    ASSERT(!chg.values_changed);
    ASSERT(f->nblocks == orig_nblocks);
    ASSERT(check_blk->kind == XI_BLOCK_IF);
    ASSERT(verify_func(f));
    xi_func_free(f);
}

/* Same range, exit taken on the true edge instead. */
TEST(keeps_true_exit_the_header_bound_does_not_rule_out) {
    XiBlock *check_blk = NULL;
    XiFunc *f =
        build_loop_with_body_exit("keeps_true_exit", 10, 5, true, NULL, &check_blk, NULL, NULL);
    ASSERT(f != NULL);
    ASSERT(verify_func(f));

    uint32_t orig_nblocks = f->nblocks;
    XiPassChange chg = xi_opt_loop_split(f);
    ASSERT(!chg.cfg_changed);
    ASSERT(!chg.values_changed);
    ASSERT(f->nblocks == orig_nblocks);
    ASSERT(check_blk->kind == XI_BLOCK_IF);
    ASSERT(verify_func(f));
    xi_func_free(f);
}

/* `i < 5` in the header rules out the `i >= 10` exit for every iteration, so
 * the exit edge is dead.  It is dropped in place: the branch becomes a jump
 * onward into the loop, and no guard block appears in front of the header. */
TEST(drops_true_exit_the_header_bound_rules_out) {
    XiBlock *check_blk = NULL;
    XiBlock *body = NULL;
    XiBlock *early_exit = NULL;
    XiFunc *f = build_loop_with_body_exit("drops_true_exit", 5, 10, true, NULL, &check_blk, &body,
                                          &early_exit);
    ASSERT(f != NULL);
    ASSERT(verify_func(f));

    uint32_t orig_nblocks = f->nblocks;
    XiPassChange chg = xi_opt_loop_split(f);
    ASSERT(chg.cfg_changed);
    ASSERT(f->nblocks == orig_nblocks);
    ASSERT(check_blk->kind == XI_BLOCK_PLAIN);
    ASSERT(check_blk->control == NULL);
    ASSERT(check_blk->succs[0] == body);
    ASSERT(check_blk->succs[1] == NULL);
    ASSERT(pred_index(early_exit, check_blk) < 0);
    ASSERT(verify_func(f));
    xi_func_free(f);
}

/* The mirrored shape: `i < 5` in the header rules out exiting on `i < 10`
 * being false. */
TEST(drops_false_exit_the_header_bound_rules_out) {
    XiBlock *check_blk = NULL;
    XiBlock *body = NULL;
    XiBlock *early_exit = NULL;
    XiFunc *f = build_loop_with_body_exit("drops_false_exit", 5, 10, false, NULL, &check_blk, &body,
                                          &early_exit);
    ASSERT(f != NULL);
    ASSERT(verify_func(f));

    XiPassChange chg = xi_opt_loop_split(f);
    ASSERT(chg.cfg_changed);
    ASSERT(check_blk->kind == XI_BLOCK_PLAIN);
    ASSERT(check_blk->succs[0] == body);
    ASSERT(pred_index(early_exit, check_blk) < 0);
    ASSERT(verify_func(f));
    xi_func_free(f);
}

/* Neither bound is a constant, but both comparisons name the same value, so
 * `i < bound` in the header still rules out an `i >= bound` exit. */
TEST(drops_exit_against_the_same_opaque_bound) {
    XiBlock *check_blk = NULL;
    XiBlock *body = NULL;
    XiBlock *early_exit = NULL;
    XiFunc *f = build_loop_with_body_exit("drops_opaque_exit", 0, 0, true, make_opaque_bound,
                                          &check_blk, &body, &early_exit);
    ASSERT(f != NULL);
    ASSERT(verify_func(f));

    XiPassChange chg = xi_opt_loop_split(f);
    ASSERT(chg.cfg_changed);
    ASSERT(check_blk->kind == XI_BLOCK_PLAIN);
    ASSERT(check_blk->succs[0] == body);
    ASSERT(pred_index(early_exit, check_blk) < 0);
    ASSERT(verify_func(f));
    xi_func_free(f);
}

TEST(no_loop_no_change) {
    XiFunc *f = xi_func_new("no_loop", &stub_int);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    XiValue *ret = xi_const_int(f, entry, 0, &stub_int);
    xi_block_set_return(entry, ret);

    XiPassChange chg = xi_opt_loop_split(f);
    ASSERT(!chg.cfg_changed);
    ASSERT(!chg.values_changed);
    xi_func_free(f);
}

TEST(skips_loop_without_early_exit) {
    XiFunc *f = xi_func_new("no_exit", &stub_int);
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
    ASSERT(verify_func(f));

    XiPassChange chg = xi_opt_loop_split(f);
    ASSERT(!chg.cfg_changed);
    xi_func_free(f);
}

TEST(skips_body_defined_exit_check) {
    XiFunc *f = xi_func_new("body_defined_exit_check", &stub_int);
    XiBlock *entry = xi_block_new(f);
    XiBlock *header = xi_block_new(f);
    XiBlock *check_blk = xi_block_new(f);
    XiBlock *body = xi_block_new(f);
    XiBlock *early_exit = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);
    entry->sealed = header->sealed = check_blk->sealed = true;
    body->sealed = early_exit->sealed = exit_blk->sealed = true;

    XiValue *start = xi_const_int(f, entry, 0, &stub_int);
    XiValue *limit = xi_const_int(f, entry, 10, &stub_int);
    XiValue *array_len = xi_const_int(f, entry, 5, &stub_int);
    XiValue *step = xi_const_int(f, entry, 1, &stub_int);

    xi_block_set_jump(entry, header);
    xi_block_set_jump(body, header);

    XiPhi *iv = xi_phi_new(f, header, &stub_int, header->npreds);
    XiValue *cond = xi_binary(f, header, XI_LT, &stub_bool, &iv->value, limit);
    xi_block_set_if(header, cond, check_blk, exit_blk);

    XiValue *body_value = xi_binary(f, check_blk, XI_ADD, &stub_int, &iv->value, step);
    XiValue *check_cond = xi_binary(f, check_blk, XI_LT, &stub_bool, body_value, array_len);
    xi_block_set_if(check_blk, check_cond, body, early_exit);

    XiValue *next = xi_binary(f, body, XI_ADD, &stub_int, &iv->value, step);

    int pre_idx = pred_index(header, entry);
    int latch_idx = pred_index(header, body);
    iv->value.args[pre_idx] = start;
    iv->value.args[latch_idx] = next;

    xi_block_set_return(exit_blk, xi_const_int(f, exit_blk, 0, &stub_int));
    xi_block_set_return(early_exit, xi_const_int(f, early_exit, -1, &stub_int));
    ASSERT(verify_func(f));

    XiPassChange chg = xi_opt_loop_split(f);
    ASSERT(!chg.cfg_changed);
    ASSERT(!chg.values_changed);
    ASSERT(verify_func(f));
    xi_func_free(f);
}

TEST(skips_non_basic_loop_carried_phi_exit_check) {
    XiFunc *f = xi_func_new("non_basic_loop_carried_exit_check", &stub_int);
    XiBlock *entry = xi_block_new(f);
    XiBlock *header = xi_block_new(f);
    XiBlock *check_blk = xi_block_new(f);
    XiBlock *body = xi_block_new(f);
    XiBlock *early_exit = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);
    entry->sealed = header->sealed = check_blk->sealed = true;
    body->sealed = early_exit->sealed = exit_blk->sealed = true;

    XiValue *start = xi_const_int(f, entry, 0, &stub_int);
    XiValue *limit = xi_const_int(f, entry, 10, &stub_int);
    XiValue *step = xi_const_int(f, entry, 1, &stub_int);
    XiValue *grow_start = xi_const_int(f, entry, 65536, &stub_int);
    XiValue *factor = xi_const_int(f, entry, 4, &stub_int);
    XiValue *size = xi_const_int(f, entry, 150000, &stub_int);

    xi_block_set_jump(entry, header);
    xi_block_set_jump(body, header);

    XiPhi *iv = xi_phi_new(f, header, &stub_int, header->npreds);
    XiPhi *grow = xi_phi_new(f, header, &stub_int, header->npreds);
    XiValue *cond = xi_binary(f, header, XI_LT, &stub_bool, &iv->value, limit);
    xi_block_set_if(header, cond, check_blk, exit_blk);

    XiValue *check_cond = xi_binary(f, check_blk, XI_LE, &stub_bool, size, &grow->value);
    xi_block_set_if(check_blk, check_cond, early_exit, body);

    XiValue *next_iv = xi_binary(f, body, XI_ADD, &stub_int, &iv->value, step);
    XiValue *next_grow = xi_binary(f, body, XI_MUL, &stub_int, &grow->value, factor);

    int pre_idx = pred_index(header, entry);
    int latch_idx = pred_index(header, body);
    iv->value.args[pre_idx] = start;
    iv->value.args[latch_idx] = next_iv;
    grow->value.args[pre_idx] = grow_start;
    grow->value.args[latch_idx] = next_grow;

    xi_block_set_return(exit_blk, xi_const_int(f, exit_blk, 0, &stub_int));
    xi_block_set_return(early_exit, xi_const_int(f, early_exit, -1, &stub_int));
    ASSERT(verify_func(f));

    XiPassChange chg = xi_opt_loop_split(f);
    ASSERT(!chg.cfg_changed);
    ASSERT(!chg.values_changed);
    ASSERT(verify_func(f));
    xi_func_free(f);
}

int main(void) {
    printf("=== Xi Loop Split Tests ===\n\n");

    run_keeps_exit_the_header_bound_does_not_rule_out();
    run_keeps_true_exit_the_header_bound_does_not_rule_out();
    run_drops_true_exit_the_header_bound_rules_out();
    run_drops_false_exit_the_header_bound_rules_out();
    run_drops_exit_against_the_same_opaque_bound();
    run_no_loop_no_change();
    run_skips_loop_without_early_exit();
    run_skips_body_defined_exit_check();
    run_skips_non_basic_loop_carried_phi_exit_check();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
