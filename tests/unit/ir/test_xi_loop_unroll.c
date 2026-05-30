/*
 * Unit tests for Xi loop unrolling.
 */

#include "../../../src/ir/xi_analysis.h"
#include "../../../src/ir/xi_loop.h"
#include "../../../src/ir/xi_opt_loop_unroll.h"
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

/* Build a small counted loop: for i = 0; i < limit_val; i++ { sum += i }
 * Uses exit phis so unroll can rewrite them properly. */
static XiFunc *build_small_counted_loop(int64_t limit_val) {
    XiFunc *f = xi_func_new("unroll_target", &stub_int);
    XiBlock *entry = xi_block_new(f);
    XiBlock *header = xi_block_new(f);
    XiBlock *body = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);
    entry->sealed = header->sealed = body->sealed = exit_blk->sealed = true;

    XiValue *start = xi_const_int(f, entry, 0, &stub_int);
    XiValue *limit = xi_const_int(f, entry, limit_val, &stub_int);
    XiValue *step = xi_const_int(f, entry, 1, &stub_int);

    xi_block_set_jump(entry, header);
    xi_block_set_jump(body, header);

    XiPhi *iv = xi_phi_new(f, header, &stub_int, header->npreds);
    XiPhi *sum_phi = xi_phi_new(f, header, &stub_int, header->npreds);
    XiValue *cond = xi_binary(f, header, XI_LT, &stub_bool, &iv->value, limit);
    xi_block_set_if(header, cond, body, exit_blk);

    XiValue *sum_add = xi_binary(f, body, XI_ADD, &stub_int, &sum_phi->value, &iv->value);
    XiValue *next = xi_binary(f, body, XI_ADD, &stub_int, &iv->value, step);

    int pre_idx = pred_index(header, entry);
    int latch_idx = pred_index(header, body);
    iv->value.args[pre_idx] = start;
    iv->value.args[latch_idx] = next;
    sum_phi->value.args[pre_idx] = start;
    sum_phi->value.args[latch_idx] = sum_add;

    /* Exit returns sum via exit phi. */
    XiPhi *exit_phi = xi_phi_new(f, exit_blk, &stub_int, exit_blk->npreds);
    int hdr_exit_idx = pred_index(exit_blk, header);
    exit_phi->value.args[hdr_exit_idx] = &sum_phi->value;
    xi_block_set_return(exit_blk, &exit_phi->value);
    return f;
}

TEST(fully_unrolls_small_counted_loop) {
    XiFunc *f = build_small_counted_loop(4);
    ASSERT(f != NULL);
    ASSERT(verify_func(f));

    XiPassChange chg = xi_opt_loop_unroll(f);
    ASSERT(chg.cfg_changed);
    ASSERT(chg.values_changed);
    xi_func_free(f);
}

TEST(skips_large_trip_count) {
    XiFunc *f = build_small_counted_loop(100);
    ASSERT(f != NULL);
    ASSERT(verify_func(f));

    XiPassChange chg = xi_opt_loop_unroll(f);
    ASSERT(!chg.cfg_changed);
    xi_func_free(f);
}

TEST(skips_loop_with_throw) {
    XiFunc *f = xi_func_new("throw_loop", &stub_int);
    XiBlock *entry = xi_block_new(f);
    XiBlock *header = xi_block_new(f);
    XiBlock *body = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);
    entry->sealed = header->sealed = body->sealed = exit_blk->sealed = true;

    XiValue *start = xi_const_int(f, entry, 0, &stub_int);
    XiValue *limit = xi_const_int(f, entry, 4, &stub_int);
    XiValue *step = xi_const_int(f, entry, 1, &stub_int);

    xi_block_set_jump(entry, header);
    xi_block_set_jump(body, header);

    XiPhi *iv = xi_phi_new(f, header, &stub_int, header->npreds);
    XiValue *cond = xi_binary(f, header, XI_LT, &stub_bool, &iv->value, limit);
    xi_block_set_if(header, cond, body, exit_blk);

    XiValue *bc = xi_value_new(f, body, XI_BOUNDS_CHECK, &stub_int, 2);
    bc->args[0] = &iv->value;
    bc->args[1] = limit;
    bc->flags |= XI_FLAG_MAY_THROW;
    XiValue *next = xi_binary(f, body, XI_ADD, &stub_int, &iv->value, step);

    int pre_idx = pred_index(header, entry);
    int latch_idx = pred_index(header, body);
    iv->value.args[pre_idx] = start;
    iv->value.args[latch_idx] = next;

    xi_block_set_return(exit_blk, xi_const_int(f, exit_blk, 0, &stub_int));
    ASSERT(verify_func(f));

    XiPassChange chg = xi_opt_loop_unroll(f);
    ASSERT(!chg.cfg_changed);
    xi_func_free(f);
}

TEST(no_loop_no_change) {
    XiFunc *f = xi_func_new("no_loop", &stub_int);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    XiValue *ret = xi_const_int(f, entry, 0, &stub_int);
    xi_block_set_return(entry, ret);

    XiPassChange chg = xi_opt_loop_unroll(f);
    ASSERT(!chg.cfg_changed);
    ASSERT(!chg.values_changed);
    xi_func_free(f);
}

int main(void) {
    printf("=== Xi Loop Unroll Tests ===\n\n");

    run_fully_unrolls_small_counted_loop();
    run_skips_large_trip_count();
    run_skips_loop_with_throw();
    run_no_loop_no_change();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
