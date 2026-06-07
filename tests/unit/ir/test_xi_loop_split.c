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

/* Build a loop with an early-exit condition based on an invariant check. */
static XiFunc *build_loop_with_early_exit(void) {
    XiFunc *f = xi_func_new("split_target", &stub_int);
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

    XiValue *check_cond = xi_binary(f, check_blk, XI_LT, &stub_bool, &iv->value, array_len);
    xi_block_set_if(check_blk, check_cond, body, early_exit);

    XiValue *next = xi_binary(f, body, XI_ADD, &stub_int, &iv->value, step);

    int pre_idx = pred_index(header, entry);
    int latch_idx = pred_index(header, body);
    iv->value.args[pre_idx] = start;
    iv->value.args[latch_idx] = next;

    xi_block_set_return(exit_blk, xi_const_int(f, exit_blk, 0, &stub_int));
    xi_block_set_return(early_exit, xi_const_int(f, early_exit, -1, &stub_int));
    return f;
}

static XiFunc *build_loop_with_true_early_exit(XiBlock **out_entry, XiBlock **out_header,
                                               XiBlock **out_early_exit) {
    XiFunc *f = xi_func_new("split_true_exit", &stub_int);
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

    XiValue *check_cond = xi_binary(f, check_blk, XI_GE, &stub_bool, &iv->value, array_len);
    xi_block_set_if(check_blk, check_cond, early_exit, body);

    XiValue *next = xi_binary(f, body, XI_ADD, &stub_int, &iv->value, step);

    int pre_idx = pred_index(header, entry);
    int latch_idx = pred_index(header, body);
    iv->value.args[pre_idx] = start;
    iv->value.args[latch_idx] = next;

    xi_block_set_return(exit_blk, xi_const_int(f, exit_blk, 0, &stub_int));
    xi_block_set_return(early_exit, xi_const_int(f, early_exit, -1, &stub_int));

    if (out_entry)
        *out_entry = entry;
    if (out_header)
        *out_header = header;
    if (out_early_exit)
        *out_early_exit = early_exit;
    return f;
}

TEST(splits_loop_with_invariant_exit) {
    XiFunc *f = build_loop_with_early_exit();
    ASSERT(f != NULL);
    ASSERT(verify_func(f));

    uint32_t orig_nblocks = f->nblocks;
    XiPassChange chg = xi_opt_loop_split(f);
    ASSERT(chg.cfg_changed);
    ASSERT(f->nblocks > orig_nblocks);
    ASSERT(verify_func(f));
    xi_func_free(f);
}

TEST(splits_loop_with_true_early_exit) {
    XiBlock *entry = NULL;
    XiBlock *header = NULL;
    XiBlock *early_exit = NULL;
    XiFunc *f = build_loop_with_true_early_exit(&entry, &header, &early_exit);
    ASSERT(f != NULL);
    ASSERT(verify_func(f));

    XiPassChange chg = xi_opt_loop_split(f);
    ASSERT(chg.cfg_changed);
    ASSERT(verify_func(f));

    XiBlock *guard = entry->succs[0];
    ASSERT(guard != NULL);
    ASSERT(guard->kind == XI_BLOCK_IF);
    ASSERT(guard->succs[0] == early_exit);
    ASSERT(guard->succs[1] == header);

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

int main(void) {
    printf("=== Xi Loop Split Tests ===\n\n");

    run_splits_loop_with_invariant_exit();
    run_splits_loop_with_true_early_exit();
    run_no_loop_no_change();
    run_skips_loop_without_early_exit();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
