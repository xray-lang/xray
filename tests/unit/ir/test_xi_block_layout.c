/*
 * Unit tests for xi_opt_block_layout — block reordering.
 */

#include "../../../src/ir/xi_block_layout.h"
#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_analysis.h"
#include "../../../src/runtime/value/xtype.h"

#include <stdio.h>

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};

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

TEST(null_safe) {
    XiPassChange c = xi_opt_block_layout(NULL);
    ASSERT(!c.cfg_changed);
}

TEST(single_block_noop) {
    XiFunc *f = xi_func_new("test_layout", &stub_int);
    XiBlock *entry = xi_block_new(f);
    XiValue *c = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    c->aux_int = 42;
    xi_block_set_return(entry, c);

    XiPassChange chg = xi_opt_block_layout(f);
    ASSERT(!chg.cfg_changed);

    xi_func_free(f);
}

TEST(two_blocks_no_profile_noop) {
    XiFunc *f = xi_func_new("test_layout_2", &stub_int);
    XiBlock *entry = xi_block_new(f);
    XiBlock *b1 = xi_block_new(f);

    XiValue *c = xi_value_new(f, b1, XI_CONST, &stub_int, 0);
    c->aux_int = 42;
    xi_block_set_return(b1, c);

    entry->kind = XI_BLOCK_PLAIN;
    entry->succs[0] = b1;
    xi_block_add_pred(b1, entry);
    entry->sealed = true;
    b1->sealed = true;

    XiPassChange chg = xi_opt_block_layout(f);
    ASSERT(!chg.cfg_changed);

    xi_func_free(f);
}

TEST(profile_reorders_hot_blocks) {
    XiFunc *f = xi_func_new("test_layout_profile", &stub_int);
    XiBlock *entry = xi_block_new(f); /* id=0, freq=10 */
    XiBlock *cold = xi_block_new(f);  /* id=1, freq=1 */
    XiBlock *hot = xi_block_new(f);   /* id=2, freq=100 */

    entry->frequency = 10;
    cold->frequency = 1;
    hot->frequency = 100;

    XiValue *c1 = xi_value_new(f, cold, XI_CONST, &stub_int, 0);
    c1->aux_int = 0;
    xi_block_set_return(cold, c1);

    XiValue *c2 = xi_value_new(f, hot, XI_CONST, &stub_int, 0);
    c2->aux_int = 1;
    xi_block_set_return(hot, c2);

    XiValue *cond = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    cond->aux_int = 1;
    entry->kind = XI_BLOCK_IF;
    entry->control = cond;
    entry->succs[0] = hot;
    entry->succs[1] = cold;

    xi_block_add_pred(hot, entry);
    xi_block_add_pred(cold, entry);
    entry->sealed = true;
    cold->sealed = true;
    hot->sealed = true;

    XiPassChange chg = xi_opt_block_layout(f);
    /* Entry must remain first. */
    ASSERT(f->blocks[0] == entry);
    /* Hot block should come before cold block. */
    uint32_t hot_pos = UINT32_MAX, cold_pos = UINT32_MAX;
    for (uint32_t i = 0; i < f->nblocks; i++) {
        if (f->blocks[i] == hot)
            hot_pos = i;
        if (f->blocks[i] == cold)
            cold_pos = i;
    }
    ASSERT(hot_pos < cold_pos);

    xi_func_free(f);
}

TEST(entry_always_first) {
    XiFunc *f = xi_func_new("test_entry_first", &stub_int);
    XiBlock *entry = xi_block_new(f);
    XiBlock *b1 = xi_block_new(f);

    entry->frequency = 1;
    b1->frequency = 1000;

    XiValue *c = xi_value_new(f, b1, XI_CONST, &stub_int, 0);
    c->aux_int = 42;
    xi_block_set_return(b1, c);

    entry->kind = XI_BLOCK_PLAIN;
    entry->succs[0] = b1;
    xi_block_add_pred(b1, entry);
    entry->sealed = true;
    b1->sealed = true;

    xi_opt_block_layout(f);
    ASSERT(f->blocks[0] == entry);

    xi_func_free(f);
}

int main(void) {
    printf("=== Xi Block Layout Tests ===\n\n");

    run_null_safe();
    run_single_block_noop();
    run_two_blocks_no_profile_noop();
    run_profile_reorders_hot_blocks();
    run_entry_always_first();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
