/*
 * Unit tests for Xi sparse conditional constant propagation (SCCP).
 *
 * Covers constant folding, branch simplification with known conditions,
 * and the no-change path on already-canonical IR.
 */

#include "../../../src/ir/xi_opt_sccp.h"
#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_verify.h"
#include "../../../src/runtime/value/xtype.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static XiFunc *make_func(void) {
    XiFunc *f = xi_func_new("test_sccp", &stub_int);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    return f;
}

static void wire(XiBlock *from, XiBlock *to, int slot) {
    from->succs[slot] = to;
    xi_block_add_pred(to, from);
}

/* ========== Test: 1 + 2 folds to a constant ========== */

TEST(folds_constant_add) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    entry->kind = XI_BLOCK_RETURN;

    XiValue *a = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    a->aux_int = 1;
    XiValue *b = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    b->aux_int = 2;

    XiValue *sum = xi_value_new(f, entry, XI_ADD, &stub_int, 2);
    sum->args[0] = a;
    sum->args[1] = b;
    xi_block_set_return(entry, sum);

    XiPassChange chg = xi_opt_sccp(f);
    ASSERT(chg.values_changed);
    /* The defining add should have been rewritten to a constant. */
    ASSERT(sum->op == XI_CONST);
    ASSERT(sum->aux_int == 3);

    xi_func_free(f);
}

/* ========== Test: branch on a true constant collapses the IF ========== */

TEST(simplifies_constant_branch) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    XiBlock *then_b = xi_block_new(f);
    XiBlock *else_b = xi_block_new(f);

    entry->kind = XI_BLOCK_IF;
    then_b->kind = XI_BLOCK_RETURN;
    else_b->kind = XI_BLOCK_RETURN;

    XiValue *t = xi_value_new(f, entry, XI_CONST, &stub_bool, 0);
    t->aux_int = 1; /* true */
    entry->control = t;
    wire(entry, then_b, 0);
    wire(entry, else_b, 1);

    XiValue *r1 = xi_value_new(f, then_b, XI_CONST, &stub_int, 0);
    r1->aux_int = 100;
    xi_block_set_return(then_b, r1);

    XiValue *r2 = xi_value_new(f, else_b, XI_CONST, &stub_int, 0);
    r2->aux_int = 200;
    xi_block_set_return(else_b, r2);

    then_b->sealed = true;
    else_b->sealed = true;

    XiPassChange chg = xi_opt_sccp(f);
    ASSERT(chg.cfg_changed || chg.values_changed);
    /* Entry must no longer be a two-way branch on a known-true condition. */
    ASSERT(entry->kind != XI_BLOCK_IF);
    ASSERT(f->next_block_id == f->nblocks);

    XiBlock *after_sccp = xi_block_new(f);
    ASSERT(after_sccp != NULL);
    ASSERT(after_sccp->id == f->nblocks - 1);
    ASSERT(f->blocks[f->nblocks - 1] == after_sccp);

    xi_func_free(f);
}

/* ========== Test: comparison with known constants folds ========== */

TEST(folds_constant_compare) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    entry->kind = XI_BLOCK_RETURN;

    XiValue *a = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    a->aux_int = 5;
    XiValue *b = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    b->aux_int = 5;

    XiValue *cmp = xi_value_new(f, entry, XI_EQ, &stub_bool, 2);
    cmp->args[0] = a;
    cmp->args[1] = b;
    xi_block_set_return(entry, cmp);

    XiPassChange chg = xi_opt_sccp(f);
    ASSERT(chg.values_changed);
    ASSERT(cmp->op == XI_CONST);
    ASSERT(cmp->aux_int == 1);

    xi_func_free(f);
}

/* ========== Test: a function with no constants reports no_change ========== */

TEST(no_const_no_change) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    entry->kind = XI_BLOCK_RETURN;

    /* A function parameter is opaque to the lattice — SCCP cannot fold it. */
    XiValue *p = xi_param(f, entry, 0, &stub_int);
    xi_block_set_return(entry, p);

    XiPassChange chg = xi_opt_sccp(f);
    ASSERT(!chg.values_changed);
    ASSERT(!chg.cfg_changed);
    ASSERT(p->op == XI_PARAM);

    xi_func_free(f);
}

/* ========== Main ========== */

int main(void) {
    printf("=== Xi SCCP tests ===\n\n");

    run_folds_constant_add();
    run_simplifies_constant_branch();
    run_folds_constant_compare();
    run_no_const_no_change();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
