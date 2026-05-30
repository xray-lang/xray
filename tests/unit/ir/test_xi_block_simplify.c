/*
 * Unit tests for Xi block simplification pass (xi_opt_block_simplify).
 * Covers empty block elimination, block merge, and edge cases.
 */

#include "../../../src/ir/xi_opt_block_simplify.h"
#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_op_name.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};

static int tests_passed = 0;
static int tests_failed = 0;

static XiFunc *make_func(void) {
    XiFunc *f = xi_func_new("test_blk_simp", &stub_int);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    return f;
}

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

/* ========== Empty Block Elimination Tests ========== */

TEST(eliminate_empty_plain_block) {
    /* entry -> empty -> ret  =>  entry -> ret */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    XiBlock *empty = xi_block_new(f);
    empty->sealed = true;
    XiBlock *ret = xi_block_new(f);
    ret->sealed = true;

    entry->kind = XI_BLOCK_PLAIN;
    entry->succs[0] = empty;
    xi_block_add_pred(empty, entry);

    empty->kind = XI_BLOCK_PLAIN;
    empty->succs[0] = ret;
    xi_block_add_pred(ret, empty);

    XiValue *c1 = xi_const_int(f, entry, 42, &stub_int);
    ret->kind = XI_BLOCK_RETURN;
    ret->control = c1;

    uint32_t orig = f->nblocks;
    XiPassChange chg = xi_opt_block_simplify(f);

    ASSERT(chg.cfg_changed);
    ASSERT(f->nblocks < orig);
    /* After eliminating empty + merging ret into entry:
     * entry becomes RETURN (absorbed ret). */
    ASSERT(entry->kind == XI_BLOCK_RETURN);

    xi_func_free(f);
}

TEST(dont_eliminate_entry) {
    /* Entry block should never be eliminated even if empty.
     * However, the successor may be merged into entry. */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    XiBlock *ret = xi_block_new(f);
    ret->sealed = true;

    entry->kind = XI_BLOCK_PLAIN;
    entry->succs[0] = ret;
    xi_block_add_pred(ret, entry);

    ret->kind = XI_BLOCK_RETURN;
    ret->control = NULL;

    xi_opt_block_simplify(f);

    /* Entry is preserved (not eliminated), but ret merged into it. */
    ASSERT(f->blocks[0] == entry);
    ASSERT(entry->kind == XI_BLOCK_RETURN);

    xi_func_free(f);
}

TEST(eliminate_chain_of_empty_blocks) {
    /* entry -> e1 -> e2 -> ret  =>  entry -> ret */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    XiBlock *e1 = xi_block_new(f);
    e1->sealed = true;
    XiBlock *e2 = xi_block_new(f);
    e2->sealed = true;
    XiBlock *ret = xi_block_new(f);
    ret->sealed = true;

    entry->kind = XI_BLOCK_PLAIN;
    entry->succs[0] = e1;
    xi_block_add_pred(e1, entry);

    e1->kind = XI_BLOCK_PLAIN;
    e1->succs[0] = e2;
    xi_block_add_pred(e2, e1);

    e2->kind = XI_BLOCK_PLAIN;
    e2->succs[0] = ret;
    xi_block_add_pred(ret, e2);

    ret->kind = XI_BLOCK_RETURN;
    ret->control = NULL;

    xi_opt_block_simplify(f);

    /* All empty blocks eliminated + ret merged into entry = 1 block. */
    ASSERT(f->nblocks == 1);
    ASSERT(entry->kind == XI_BLOCK_RETURN);

    xi_func_free(f);
}

/* ========== Block Merge Tests ========== */

TEST(merge_single_succ_single_pred) {
    /* entry (has values) -> blk (has values, RETURN)
     * => entry absorbs blk's values and becomes RETURN. */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    XiBlock *blk = xi_block_new(f);
    blk->sealed = true;

    XiValue *c1 = xi_const_int(f, entry, 10, &stub_int);
    entry->kind = XI_BLOCK_PLAIN;
    entry->succs[0] = blk;
    xi_block_add_pred(blk, entry);

    XiValue *c2 = xi_const_int(f, blk, 20, &stub_int);
    blk->kind = XI_BLOCK_RETURN;
    blk->control = c2;

    uint32_t orig = f->nblocks;
    xi_opt_block_simplify(f);

    /* blk merged into entry. */
    ASSERT(f->nblocks < orig);
    ASSERT(entry->kind == XI_BLOCK_RETURN);
    ASSERT(entry->control == c2);
    /* Entry should have both values. */
    bool found_c1 = false, found_c2 = false;
    for (uint32_t i = 0; i < entry->nvalues; i++) {
        if (entry->values[i] == c1)
            found_c1 = true;
        if (entry->values[i] == c2)
            found_c2 = true;
    }
    ASSERT(found_c1 && found_c2);

    xi_func_free(f);
}

TEST(no_merge_if_block) {
    /* IF block with two succs should not be merged with either. */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    XiBlock *then_blk = xi_block_new(f);
    then_blk->sealed = true;
    XiBlock *else_blk = xi_block_new(f);
    else_blk->sealed = true;

    XiValue *cond = xi_const_int(f, entry, 1, &stub_int);
    entry->kind = XI_BLOCK_IF;
    entry->control = cond;
    entry->succs[0] = then_blk;
    entry->succs[1] = else_blk;
    xi_block_add_pred(then_blk, entry);
    xi_block_add_pred(else_blk, entry);

    then_blk->kind = XI_BLOCK_RETURN;
    then_blk->control = cond;
    else_blk->kind = XI_BLOCK_RETURN;
    else_blk->control = cond;

    uint32_t orig = f->nblocks;
    xi_opt_block_simplify(f);

    /* No change: then/else each have one pred but pred has 2 succs. */
    ASSERT(f->nblocks == orig);

    xi_func_free(f);
}

TEST(no_merge_multiple_preds) {
    /* Block with 2 preds should not be merged. */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    XiBlock *a = xi_block_new(f);
    a->sealed = true;
    XiBlock *b = xi_block_new(f);
    b->sealed = true;
    XiBlock *merge = xi_block_new(f);
    merge->sealed = true;

    XiValue *cond = xi_const_int(f, entry, 1, &stub_int);
    entry->kind = XI_BLOCK_IF;
    entry->control = cond;
    entry->succs[0] = a;
    entry->succs[1] = b;
    xi_block_add_pred(a, entry);
    xi_block_add_pred(b, entry);

    a->kind = XI_BLOCK_PLAIN;
    a->succs[0] = merge;
    xi_block_add_pred(merge, a);

    b->kind = XI_BLOCK_PLAIN;
    b->succs[0] = merge;
    xi_block_add_pred(merge, b);

    merge->kind = XI_BLOCK_RETURN;
    merge->control = NULL;

    uint32_t orig = f->nblocks;
    xi_opt_block_simplify(f);

    /* a and b are empty and can be eliminated, but merge has 2 preds. */
    /* After empty-elim of a and b: entry IF -> merge directly. */
    ASSERT(f->nblocks < orig);

    xi_func_free(f);
}

/* ========== No-Op Cases ========== */

TEST(single_block_no_change) {
    XiFunc *f = make_func();
    f->entry->kind = XI_BLOCK_RETURN;
    f->entry->control = NULL;

    XiPassChange chg = xi_opt_block_simplify(f);
    ASSERT(!chg.cfg_changed);

    xi_func_free(f);
}

TEST(non_empty_block_not_eliminated) {
    /* Block with values should not be empty-eliminated. */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    XiBlock *mid = xi_block_new(f);
    mid->sealed = true;
    XiBlock *ret = xi_block_new(f);
    ret->sealed = true;

    entry->kind = XI_BLOCK_PLAIN;
    entry->succs[0] = mid;
    xi_block_add_pred(mid, entry);

    /* mid has a value -> not empty */
    XiValue *c = xi_const_int(f, mid, 99, &stub_int);
    mid->kind = XI_BLOCK_PLAIN;
    mid->succs[0] = ret;
    xi_block_add_pred(ret, mid);

    ret->kind = XI_BLOCK_RETURN;
    ret->control = c;

    xi_opt_block_simplify(f);

    /* mid should be merged into entry (single pred/succ),
     * not eliminated as empty. Entry should now have PLAIN -> ret. */
    ASSERT(entry->nvalues > 0);

    xi_func_free(f);
}

/* ========== Runner ========== */

int main(void) {
    printf("=== Xi Block Simplify Tests ===\n\n");

    /* Empty block elimination */
    run_eliminate_empty_plain_block();
    run_dont_eliminate_entry();
    run_eliminate_chain_of_empty_blocks();

    /* Block merge */
    run_merge_single_succ_single_pred();
    run_no_merge_if_block();
    run_no_merge_multiple_preds();

    /* No-op */
    run_single_block_no_change();
    run_non_empty_block_not_eliminated();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
