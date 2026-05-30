/*
 * Unit tests for xi_opt_guard_combine — redundant guard elimination.
 */

#include "../../../src/ir/xi_opt_guard_combine.h"
#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_analysis.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"

#include <stdio.h>
#include <string.h>

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

static XiFunc *make_func(void) {
    XiFunc *f = xi_func_new("test_guard_combine", &stub_int);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    return f;
}

static void wire(XiBlock *from, XiBlock *to, int slot) {
    from->succs[slot] = to;
    xi_block_add_pred(to, from);
}

static uint32_t count_guards(XiBlock *blk) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        if (blk->values[i] && blk->values[i]->op == XI_GUARD_TYPE)
            n++;
    }
    return n;
}

TEST(no_guards_is_noop) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    XiValue *v = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    v->aux_int = 42;
    xi_block_set_return(entry, v);

    XiPassChange c = xi_opt_guard_combine(f);
    ASSERT(!c.values_changed);

    xi_func_free(f);
}

TEST(single_guard_unchanged) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiValue *recv = xi_value_new(f, entry, XI_PARAM, &stub_int, 0);
    XiValue *guard = xi_value_new(f, entry, XI_GUARD_TYPE, &stub_int, 1);
    guard->args[0] = recv;
    guard->aux_int = 0xBEEF;

    xi_block_set_return(entry, guard);

    XiPassChange c = xi_opt_guard_combine(f);
    ASSERT(!c.values_changed);
    ASSERT(count_guards(entry) == 1);

    xi_func_free(f);
}

TEST(duplicate_guard_eliminated) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiValue *recv = xi_value_new(f, entry, XI_PARAM, &stub_int, 0);

    XiValue *g1 = xi_value_new(f, entry, XI_GUARD_TYPE, &stub_int, 1);
    g1->args[0] = recv;
    g1->aux_int = 0xBEEF;

    XiValue *g2 = xi_value_new(f, entry, XI_GUARD_TYPE, &stub_int, 1);
    g2->args[0] = recv;
    g2->aux_int = 0xBEEF;

    XiValue *use = xi_value_new(f, entry, XI_ADD, &stub_int, 2);
    use->args[0] = g1;
    use->args[1] = g2;

    xi_block_set_return(entry, use);

    XiPassChange c = xi_opt_guard_combine(f);
    ASSERT(c.values_changed);
    ASSERT(c.n_removed >= 1);

    /* Only 1 guard should remain in the block; use references g1 for both args */
    ASSERT(count_guards(entry) == 1);
    ASSERT(use->args[0] == g1);
    ASSERT(use->args[1] == g1);

    xi_func_free(f);
}

TEST(different_types_not_combined) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiValue *recv = xi_value_new(f, entry, XI_PARAM, &stub_int, 0);

    XiValue *g1 = xi_value_new(f, entry, XI_GUARD_TYPE, &stub_int, 1);
    g1->args[0] = recv;
    g1->aux_int = 0xBEEF;

    XiValue *g2 = xi_value_new(f, entry, XI_GUARD_TYPE, &stub_int, 1);
    g2->args[0] = recv;
    g2->aux_int = 0xCAFE;

    xi_block_set_return(entry, g2);

    XiPassChange c = xi_opt_guard_combine(f);
    ASSERT(!c.values_changed);
    ASSERT(g1->op == XI_GUARD_TYPE);
    ASSERT(g2->op == XI_GUARD_TYPE);

    xi_func_free(f);
}

TEST(different_receivers_not_combined) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiValue *r1 = xi_value_new(f, entry, XI_PARAM, &stub_int, 0);
    XiValue *r2 = xi_value_new(f, entry, XI_PARAM, &stub_int, 0);

    XiValue *g1 = xi_value_new(f, entry, XI_GUARD_TYPE, &stub_int, 1);
    g1->args[0] = r1;
    g1->aux_int = 0xBEEF;

    XiValue *g2 = xi_value_new(f, entry, XI_GUARD_TYPE, &stub_int, 1);
    g2->args[0] = r2;
    g2->aux_int = 0xBEEF;

    xi_block_set_return(entry, g2);

    XiPassChange c = xi_opt_guard_combine(f);
    ASSERT(!c.values_changed);

    xi_func_free(f);
}

TEST(triple_duplicate_two_eliminated) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiValue *recv = xi_value_new(f, entry, XI_PARAM, &stub_int, 0);

    XiValue *g1 = xi_value_new(f, entry, XI_GUARD_TYPE, &stub_int, 1);
    g1->args[0] = recv;
    g1->aux_int = 0xBEEF;

    XiValue *g2 = xi_value_new(f, entry, XI_GUARD_TYPE, &stub_int, 1);
    g2->args[0] = recv;
    g2->aux_int = 0xBEEF;

    XiValue *g3 = xi_value_new(f, entry, XI_GUARD_TYPE, &stub_int, 1);
    g3->args[0] = recv;
    g3->aux_int = 0xBEEF;

    xi_block_set_return(entry, g3);

    XiPassChange c = xi_opt_guard_combine(f);
    ASSERT(c.values_changed);
    ASSERT(c.n_removed >= 2);
    ASSERT(count_guards(entry) == 1);

    xi_func_free(f);
}

TEST(dominator_guard_propagated) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiBlock *blk2 = xi_block_new(f);
    blk2->sealed = true;

    entry->kind = XI_BLOCK_PLAIN;
    wire(entry, blk2, 0);
    blk2->kind = XI_BLOCK_RETURN;

    /* Compute dominators so blk2->idom = entry */
    xi_ensure_dominators(f);

    XiValue *recv = xi_value_new(f, entry, XI_PARAM, &stub_int, 0);

    XiValue *g1 = xi_value_new(f, entry, XI_GUARD_TYPE, &stub_int, 1);
    g1->args[0] = recv;
    g1->aux_int = 0xBEEF;

    XiValue *g2 = xi_value_new(f, blk2, XI_GUARD_TYPE, &stub_int, 1);
    g2->args[0] = recv;
    g2->aux_int = 0xBEEF;

    blk2->control = g2;

    XiPassChange c = xi_opt_guard_combine(f);
    ASSERT(c.values_changed);
    ASSERT(count_guards(blk2) == 0);

    xi_func_free(f);
}

TEST(null_func_safe) {
    XiPassChange c = xi_opt_guard_combine(NULL);
    ASSERT(!c.values_changed);
}

int main(void) {
    printf("=== Xi Guard Combine Tests ===\n\n");

    run_no_guards_is_noop();
    run_single_guard_unchanged();
    run_duplicate_guard_eliminated();
    run_different_types_not_combined();
    run_different_receivers_not_combined();
    run_triple_duplicate_two_eliminated();
    run_dominator_guard_propagated();
    run_null_func_safe();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
