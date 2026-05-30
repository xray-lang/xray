/*
 * Unit tests for xi_opt_guard_motion — guard hoisting out of loops.
 */

#include "../../../src/ir/xi_opt_guard_motion.h"
#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_analysis.h"
#include "../../../src/ir/xi_loop.h"
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

static void wire(XiBlock *from, XiBlock *to, int slot) {
    from->succs[slot] = to;
    xi_block_add_pred(to, from);
}

static uint32_t count_guards_in(XiBlock *blk) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        if (blk->values[i] && blk->values[i]->op == XI_GUARD_TYPE)
            n++;
    }
    return n;
}

/*
 * Build a simple loop:
 *   entry -> preheader -> header -> latch --back--> header
 *                                   header -> exit
 *
 * Returns preheader block via *out_preheader.
 * Returns header block via *out_header.
 * Returns latch block via *out_latch.
 */
static XiFunc *make_loop_func(XiBlock **out_preheader, XiBlock **out_header, XiBlock **out_latch,
                              XiBlock **out_exit) {
    XiFunc *f = xi_func_new("test_guard_motion", &stub_int);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;

    XiBlock *preheader = xi_block_new(f);
    preheader->sealed = true;

    XiBlock *header = xi_block_new(f);
    header->sealed = true;

    XiBlock *latch = xi_block_new(f);
    latch->sealed = true;

    XiBlock *exit = xi_block_new(f);
    exit->sealed = true;

    /* entry -> preheader */
    entry->kind = XI_BLOCK_PLAIN;
    wire(entry, preheader, 0);

    /* preheader -> header */
    preheader->kind = XI_BLOCK_PLAIN;
    wire(preheader, header, 0);

    /* header -> latch (fall-through) and header -> exit (branch) */
    header->kind = XI_BLOCK_IF;
    wire(header, latch, 0);
    wire(header, exit, 1);

    /* latch -> header (back edge) */
    latch->kind = XI_BLOCK_PLAIN;
    wire(latch, header, 0);

    /* exit returns */
    exit->kind = XI_BLOCK_RETURN;

    *out_preheader = preheader;
    *out_header = header;
    *out_latch = latch;
    *out_exit = exit;

    return f;
}

TEST(no_loops_is_noop) {
    XiFunc *f = xi_func_new("test_guard_no_loop", &stub_int);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    entry->kind = XI_BLOCK_RETURN;

    XiValue *recv = xi_value_new(f, entry, XI_PARAM, &stub_int, 0);
    XiValue *guard = xi_value_new(f, entry, XI_GUARD_TYPE, &stub_int, 1);
    guard->args[0] = recv;
    guard->aux_int = 0xBEEF;

    entry->control = guard;

    XiPassChange c = xi_opt_guard_motion(f);
    ASSERT(!c.values_changed);
    ASSERT(count_guards_in(entry) == 1);

    xi_func_free(f);
}

TEST(loop_invariant_guard_hoisted) {
    XiBlock *pre, *hdr, *latch, *exit;
    XiFunc *f = make_loop_func(&pre, &hdr, &latch, &exit);

    /* Receiver defined in entry (outside the loop). */
    XiValue *recv = xi_value_new(f, f->entry, XI_PARAM, &stub_int, 0);

    /* Guard inside the loop header. */
    XiValue *guard = xi_value_new(f, hdr, XI_GUARD_TYPE, &stub_int, 1);
    guard->args[0] = recv;
    guard->aux_int = 0xBEEF;

    /* Control: use a const as branch condition */
    XiValue *cond = xi_value_new(f, hdr, XI_CONST, &stub_int, 0);
    cond->aux_int = 1;
    hdr->control = cond;
    exit->control = cond;

    XiPassChange c = xi_opt_guard_motion(f);
    ASSERT(c.values_changed);

    /* Guard should now be in the preheader, not in header */
    ASSERT(count_guards_in(pre) == 1);
    ASSERT(count_guards_in(hdr) == 0);
    ASSERT(guard->block == pre);

    xi_func_free(f);
}

TEST(loop_variant_guard_stays) {
    XiBlock *pre, *hdr, *latch, *exit;
    XiFunc *f = make_loop_func(&pre, &hdr, &latch, &exit);

    /* Receiver defined inside the loop header (loop-variant). */
    XiValue *recv = xi_value_new(f, hdr, XI_PARAM, &stub_int, 0);

    XiValue *guard = xi_value_new(f, hdr, XI_GUARD_TYPE, &stub_int, 1);
    guard->args[0] = recv;
    guard->aux_int = 0xBEEF;

    XiValue *cond = xi_value_new(f, hdr, XI_CONST, &stub_int, 0);
    cond->aux_int = 1;
    hdr->control = cond;
    exit->control = cond;

    XiPassChange c = xi_opt_guard_motion(f);
    ASSERT(!c.values_changed);
    ASSERT(count_guards_in(hdr) == 1);
    ASSERT(count_guards_in(pre) == 0);

    xi_func_free(f);
}

TEST(multiple_invariant_guards_all_hoisted) {
    XiBlock *pre, *hdr, *latch, *exit;
    XiFunc *f = make_loop_func(&pre, &hdr, &latch, &exit);

    XiValue *r1 = xi_value_new(f, f->entry, XI_PARAM, &stub_int, 0);
    XiValue *r2 = xi_value_new(f, f->entry, XI_PARAM, &stub_int, 0);

    XiValue *g1 = xi_value_new(f, hdr, XI_GUARD_TYPE, &stub_int, 1);
    g1->args[0] = r1;
    g1->aux_int = 0xBEEF;

    XiValue *g2 = xi_value_new(f, hdr, XI_GUARD_TYPE, &stub_int, 1);
    g2->args[0] = r2;
    g2->aux_int = 0xCAFE;

    XiValue *cond = xi_value_new(f, hdr, XI_CONST, &stub_int, 0);
    cond->aux_int = 1;
    hdr->control = cond;
    exit->control = cond;

    XiPassChange c = xi_opt_guard_motion(f);
    ASSERT(c.values_changed);
    ASSERT(count_guards_in(pre) == 2);
    ASSERT(count_guards_in(hdr) == 0);

    xi_func_free(f);
}

TEST(mixed_invariant_variant_guards) {
    XiBlock *pre, *hdr, *latch, *exit;
    XiFunc *f = make_loop_func(&pre, &hdr, &latch, &exit);

    /* r_out: defined outside loop */
    XiValue *r_out = xi_value_new(f, f->entry, XI_PARAM, &stub_int, 0);
    /* r_in: defined inside loop */
    XiValue *r_in = xi_value_new(f, hdr, XI_PARAM, &stub_int, 0);

    XiValue *g_hoist = xi_value_new(f, hdr, XI_GUARD_TYPE, &stub_int, 1);
    g_hoist->args[0] = r_out;
    g_hoist->aux_int = 0xBEEF;

    XiValue *g_stay = xi_value_new(f, hdr, XI_GUARD_TYPE, &stub_int, 1);
    g_stay->args[0] = r_in;
    g_stay->aux_int = 0xCAFE;

    XiValue *cond = xi_value_new(f, hdr, XI_CONST, &stub_int, 0);
    cond->aux_int = 1;
    hdr->control = cond;
    exit->control = cond;

    XiPassChange c = xi_opt_guard_motion(f);
    ASSERT(c.values_changed);

    /* Only the invariant guard should be hoisted */
    ASSERT(count_guards_in(pre) == 1);
    ASSERT(count_guards_in(hdr) == 1);
    ASSERT(g_hoist->block == pre);
    ASSERT(g_stay->block == hdr);

    xi_func_free(f);
}

TEST(guard_in_latch_hoisted) {
    XiBlock *pre, *hdr, *latch, *exit;
    XiFunc *f = make_loop_func(&pre, &hdr, &latch, &exit);

    XiValue *recv = xi_value_new(f, f->entry, XI_PARAM, &stub_int, 0);

    /* Guard in the latch block (still in loop body). */
    XiValue *guard = xi_value_new(f, latch, XI_GUARD_TYPE, &stub_int, 1);
    guard->args[0] = recv;
    guard->aux_int = 0xBEEF;

    XiValue *cond = xi_value_new(f, hdr, XI_CONST, &stub_int, 0);
    cond->aux_int = 1;
    hdr->control = cond;
    exit->control = cond;

    XiPassChange c = xi_opt_guard_motion(f);
    ASSERT(c.values_changed);
    ASSERT(count_guards_in(pre) == 1);
    ASSERT(count_guards_in(latch) == 0);
    ASSERT(guard->block == pre);

    xi_func_free(f);
}

TEST(null_func_safe) {
    XiPassChange c = xi_opt_guard_motion(NULL);
    ASSERT(!c.values_changed);
}

int main(void) {
    printf("=== Xi Guard Motion Tests ===\n\n");

    run_no_loops_is_noop();
    run_loop_invariant_guard_hoisted();
    run_loop_variant_guard_stays();
    run_multiple_invariant_guards_all_hoisted();
    run_mixed_invariant_variant_guards();
    run_guard_in_latch_hoisted();
    run_null_func_safe();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
