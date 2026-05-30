/*
 * Unit tests for Xi loop rotation.
 */

#include "../../../src/ir/xi_analysis.h"
#include "../../../src/ir/xi_loop.h"
#include "../../../src/ir/xi_opt_loop_rotate.h"
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

typedef struct LoopFixture {
    XiFunc *f;
    XiBlock *entry;
    XiBlock *header;
    XiBlock *body;
    XiBlock *exit_blk;
    XiPhi *iv;
    XiValue *start;
    XiValue *limit;
    XiValue *step;
    XiValue *next;
    XiValue *cond;
    XiValue *ret_val;
} LoopFixture;

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

static bool make_counted_loop(LoopFixture *fx) {
    fx->f = xi_func_new("loop_rotate", &stub_int);
    if (!fx->f)
        return false;
    fx->entry = xi_block_new(fx->f);
    fx->header = xi_block_new(fx->f);
    fx->body = xi_block_new(fx->f);
    fx->exit_blk = xi_block_new(fx->f);
    if (!fx->entry || !fx->header || !fx->body || !fx->exit_blk)
        return false;

    fx->start = xi_const_int(fx->f, fx->entry, 0, &stub_int);
    fx->limit = xi_const_int(fx->f, fx->entry, 4, &stub_int);
    fx->step = xi_const_int(fx->f, fx->entry, 1, &stub_int);
    fx->ret_val = xi_const_int(fx->f, fx->entry, 99, &stub_int);
    if (!fx->start || !fx->limit || !fx->step || !fx->ret_val)
        return false;

    xi_block_set_jump(fx->entry, fx->header);
    xi_block_set_jump(fx->body, fx->header);

    fx->iv = xi_phi_new(fx->f, fx->header, &stub_int, fx->header->npreds);
    if (!fx->iv)
        return false;
    fx->next = xi_binary(fx->f, fx->body, XI_ADD, &stub_int, &fx->iv->value, fx->step);
    fx->cond = xi_binary(fx->f, fx->header, XI_LT, &stub_bool, &fx->iv->value, fx->limit);
    if (!fx->next || !fx->cond)
        return false;

    int pre_idx = pred_index(fx->header, fx->entry);
    int latch_idx = pred_index(fx->header, fx->body);
    if (pre_idx < 0 || latch_idx < 0)
        return false;
    fx->iv->value.args[pre_idx] = fx->start;
    fx->iv->value.args[latch_idx] = fx->next;

    xi_block_set_if(fx->header, fx->cond, fx->body, fx->exit_blk);
    xi_block_set_return(fx->exit_blk, fx->ret_val);
    fx->entry->sealed = true;
    fx->header->sealed = true;
    fx->body->sealed = true;
    fx->exit_blk->sealed = true;
    return true;
}

static XiBlock *find_new_guard(const LoopFixture *fx) {
    for (uint32_t i = 0; i < fx->f->nblocks; i++) {
        XiBlock *blk = fx->f->blocks[i];
        if (blk != fx->header && blk != fx->body && blk->kind == XI_BLOCK_IF &&
            blk->succs[0] == fx->body && blk->succs[1] == fx->exit_blk)
            return blk;
    }
    return NULL;
}

TEST(rotates_counted_while_shape) {
    LoopFixture fx = {0};
    ASSERT(make_counted_loop(&fx));
    ASSERT(verify_func(fx.f));

    XiPassChange chg = xi_opt_loop_rotate(fx.f);
    ASSERT(chg.cfg_changed);
    ASSERT(chg.values_changed);
    ASSERT(verify_func(fx.f));

    XiBlock *guard = find_new_guard(&fx);
    ASSERT(guard != NULL);
    ASSERT(fx.entry->succs[0] == guard);
    ASSERT(guard->control != fx.cond);
    ASSERT(guard->control->op == XI_LT);
    ASSERT(guard->control->args[0] == fx.start);
    ASSERT(guard->control->args[1] == fx.limit);

    ASSERT(fx.header->npreds == 1);
    ASSERT(fx.header->preds[0] == fx.body);
    ASSERT(fx.iv->value.nargs == 1);
    ASSERT(fx.iv->value.args[0] == fx.next);
    ASSERT(fx.body->npreds == 2);
    ASSERT(fx.body->phis != NULL);
    ASSERT(fx.next->args[0] == &fx.body->phis->value);

    xi_func_free(fx.f);
}

TEST(rotated_loop_is_idempotent) {
    LoopFixture fx = {0};
    ASSERT(make_counted_loop(&fx));
    XiPassChange first = xi_opt_loop_rotate(fx.f);
    ASSERT(first.cfg_changed);
    ASSERT(verify_func(fx.f));

    XiPassChange second = xi_opt_loop_rotate(fx.f);
    ASSERT(!second.cfg_changed);
    ASSERT(verify_func(fx.f));

    xi_func_free(fx.f);
}

TEST(preserves_exit_phi_for_zero_iteration_path) {
    LoopFixture fx = {0};
    ASSERT(make_counted_loop(&fx));
    XiPhi *exit_phi = xi_phi_new(fx.f, fx.exit_blk, &stub_int, fx.exit_blk->npreds);
    ASSERT(exit_phi != NULL);
    int exit_header_idx = pred_index(fx.exit_blk, fx.header);
    ASSERT(exit_header_idx >= 0);
    exit_phi->value.args[exit_header_idx] = &fx.iv->value;
    xi_block_set_return(fx.exit_blk, &exit_phi->value);
    ASSERT(verify_func(fx.f));

    XiPassChange chg = xi_opt_loop_rotate(fx.f);
    ASSERT(chg.cfg_changed);
    ASSERT(verify_func(fx.f));

    XiBlock *guard = find_new_guard(&fx);
    ASSERT(guard != NULL);
    int header_idx = pred_index(fx.exit_blk, fx.header);
    int guard_idx = pred_index(fx.exit_blk, guard);
    ASSERT(header_idx >= 0);
    ASSERT(guard_idx >= 0);
    ASSERT(exit_phi->value.nargs == fx.exit_blk->npreds);
    ASSERT(exit_phi->value.args[header_idx] == &fx.iv->value);
    ASSERT(exit_phi->value.args[guard_idx] == fx.start);

    xi_func_free(fx.f);
}

TEST(rejects_header_value_used_directly_after_loop) {
    LoopFixture fx = {0};
    ASSERT(make_counted_loop(&fx));
    fx.exit_blk->control = &fx.iv->value;
    ASSERT(verify_func(fx.f));

    XiPassChange chg = xi_opt_loop_rotate(fx.f);
    ASSERT(!chg.cfg_changed);
    ASSERT(fx.entry->succs[0] == fx.header);
    ASSERT(verify_func(fx.f));

    xi_func_free(fx.f);
}

TEST(rejects_side_effect_header_condition) {
    LoopFixture fx = {0};
    ASSERT(make_counted_loop(&fx));
    fx.cond->flags |= XI_FLAG_SIDE_EFFECT;
    ASSERT(verify_func(fx.f));

    XiPassChange chg = xi_opt_loop_rotate(fx.f);
    ASSERT(!chg.cfg_changed);
    ASSERT(fx.entry->succs[0] == fx.header);
    ASSERT(verify_func(fx.f));

    xi_func_free(fx.f);
}

int main(void) {
    printf("=== Xi Loop Rotate Tests ===\n\n");

    run_rotates_counted_while_shape();
    run_rotated_loop_is_idempotent();
    run_preserves_exit_phi_for_zero_iteration_path();
    run_rejects_header_value_used_directly_after_loop();
    run_rejects_side_effect_header_condition();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
