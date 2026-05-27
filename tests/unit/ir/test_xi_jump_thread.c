/*
 * Unit tests for Xi jump threading pass (xi_opt_jump_thread).
 * Covers same-condition threading, negated-condition threading,
 * and non-threadable cases.
 */

#include "../../../src/ir/xi_opt_jump_thread.h"
#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_op_name.h"
#include "../../../src/ir/xi_verify.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_bool = {.kind = XR_KIND_BOOL, .id = 2, .frozen = true};

static int tests_passed = 0;
static int tests_failed = 0;

static XiFunc *make_func(void) {
    XiFunc *f = xi_func_new("test_jt", &stub_int);
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

/* Helper: create a comparison value. */
static XiValue *make_cmp(XiFunc *f, XiBlock *blk, XiOp op, XiValue *lhs, XiValue *rhs) {
    XiValue *v = xi_value_new(f, blk, op, &stub_bool, 2);
    v->args[0] = lhs;
    v->args[1] = rhs;
    return v;
}

/* ========== Same-Condition Threading ========== */

TEST(thread_same_eq) {
    /*
     * A: if (x == 0) goto B else goto C
     * B: goto D
     * D: if (x == 0) goto E else goto F
     * => B's edge to D redirected to E (x==0 known true from A->B)
     */
    XiFunc *f = make_func();
    XiBlock *A = f->entry;
    XiBlock *B = xi_block_new(f);
    B->sealed = true;
    XiBlock *C = xi_block_new(f);
    C->sealed = true;
    XiBlock *D = xi_block_new(f);
    D->sealed = true;
    XiBlock *E = xi_block_new(f);
    E->sealed = true;
    XiBlock *F = xi_block_new(f);
    F->sealed = true;

    XiValue *x = xi_const_int(f, A, 42, &stub_int);
    XiValue *zero = xi_const_int(f, A, 0, &stub_int);
    XiValue *cmp_a = make_cmp(f, A, XI_EQ, x, zero);

    A->kind = XI_BLOCK_IF;
    A->control = cmp_a;
    A->succs[0] = B; /* then: x==0 is true */
    A->succs[1] = C; /* else: x==0 is false */
    xi_block_add_pred(B, A);
    xi_block_add_pred(C, A);

    B->kind = XI_BLOCK_PLAIN;
    B->succs[0] = D;
    xi_block_add_pred(D, B);

    C->kind = XI_BLOCK_RETURN;
    C->control = NULL;

    XiValue *cmp_d = make_cmp(f, D, XI_EQ, x, zero);
    D->kind = XI_BLOCK_IF;
    D->control = cmp_d;
    D->succs[0] = E; /* then */
    D->succs[1] = F; /* else */
    xi_block_add_pred(E, D);
    xi_block_add_pred(F, D);

    E->kind = XI_BLOCK_RETURN;
    E->control = NULL;
    F->kind = XI_BLOCK_RETURN;
    F->control = NULL;

    XiPassChange chg = xi_opt_jump_thread(f);

    /* B should now go directly to E (skipping D). */
    ASSERT(chg.cfg_changed);
    ASSERT(B->succs[0] == E);

    xi_func_free(f);
}

TEST(thread_else_side) {
    /*
     * A: if (x == 0) goto B else goto C
     * C: goto D
     * D: if (x == 0) goto E else goto F
     * => C's edge to D redirected to F (x==0 known false from A->C)
     */
    XiFunc *f = make_func();
    XiBlock *A = f->entry;
    XiBlock *B = xi_block_new(f);
    B->sealed = true;
    XiBlock *C = xi_block_new(f);
    C->sealed = true;
    XiBlock *D = xi_block_new(f);
    D->sealed = true;
    XiBlock *E = xi_block_new(f);
    E->sealed = true;
    XiBlock *F = xi_block_new(f);
    F->sealed = true;

    XiValue *x = xi_const_int(f, A, 42, &stub_int);
    XiValue *zero = xi_const_int(f, A, 0, &stub_int);
    XiValue *cmp_a = make_cmp(f, A, XI_EQ, x, zero);

    A->kind = XI_BLOCK_IF;
    A->control = cmp_a;
    A->succs[0] = B;
    A->succs[1] = C;
    xi_block_add_pred(B, A);
    xi_block_add_pred(C, A);

    B->kind = XI_BLOCK_RETURN;
    B->control = NULL;

    C->kind = XI_BLOCK_PLAIN;
    C->succs[0] = D;
    xi_block_add_pred(D, C);

    XiValue *cmp_d = make_cmp(f, D, XI_EQ, x, zero);
    D->kind = XI_BLOCK_IF;
    D->control = cmp_d;
    D->succs[0] = E;
    D->succs[1] = F;
    xi_block_add_pred(E, D);
    xi_block_add_pred(F, D);

    E->kind = XI_BLOCK_RETURN;
    E->control = NULL;
    F->kind = XI_BLOCK_RETURN;
    F->control = NULL;

    xi_opt_jump_thread(f);

    /* C should now go directly to F (x==0 was false along A->C). */
    ASSERT(C->succs[0] == F);

    xi_func_free(f);
}

/* ========== Negated-Condition Threading ========== */

TEST(thread_negated_lt_ge) {
    /*
     * A: if (x < y) goto B else goto C
     * B: goto D
     * D: if (x >= y) goto E else goto F
     *    (x>=y is negation of x<y)
     * => B's edge to D redirected to F (x<y is true from A->B,
     *    so x>=y is false → else side = F)
     */
    XiFunc *f = make_func();
    XiBlock *A = f->entry;
    XiBlock *B = xi_block_new(f);
    B->sealed = true;
    XiBlock *C = xi_block_new(f);
    C->sealed = true;
    XiBlock *D = xi_block_new(f);
    D->sealed = true;
    XiBlock *E = xi_block_new(f);
    E->sealed = true;
    XiBlock *F = xi_block_new(f);
    F->sealed = true;

    XiValue *x = xi_const_int(f, A, 10, &stub_int);
    XiValue *y = xi_const_int(f, A, 20, &stub_int);
    XiValue *cmp_lt = make_cmp(f, A, XI_LT, x, y);

    A->kind = XI_BLOCK_IF;
    A->control = cmp_lt;
    A->succs[0] = B;
    A->succs[1] = C;
    xi_block_add_pred(B, A);
    xi_block_add_pred(C, A);

    B->kind = XI_BLOCK_PLAIN;
    B->succs[0] = D;
    xi_block_add_pred(D, B);

    C->kind = XI_BLOCK_RETURN;
    C->control = NULL;

    XiValue *cmp_ge = make_cmp(f, D, XI_GE, x, y);
    D->kind = XI_BLOCK_IF;
    D->control = cmp_ge;
    D->succs[0] = E;
    D->succs[1] = F;
    xi_block_add_pred(E, D);
    xi_block_add_pred(F, D);

    E->kind = XI_BLOCK_RETURN;
    E->control = NULL;
    F->kind = XI_BLOCK_RETURN;
    F->control = NULL;

    xi_opt_jump_thread(f);

    /* B->D threaded: x<y true means x>=y false, so goes to F (else). */
    ASSERT(B->succs[0] == F);

    xi_func_free(f);
}

/* ========== Non-Threadable Cases ========== */

TEST(no_thread_different_operands) {
    /*
     * A: if (x == 0) goto B else goto C
     * B: goto D
     * D: if (y == 0) goto E else goto F   (different operand y)
     * => No threading (different comparison).
     */
    XiFunc *f = make_func();
    XiBlock *A = f->entry;
    XiBlock *B = xi_block_new(f);
    B->sealed = true;
    XiBlock *C = xi_block_new(f);
    C->sealed = true;
    XiBlock *D = xi_block_new(f);
    D->sealed = true;
    XiBlock *E = xi_block_new(f);
    E->sealed = true;
    XiBlock *F = xi_block_new(f);
    F->sealed = true;

    XiValue *x = xi_const_int(f, A, 1, &stub_int);
    XiValue *y = xi_const_int(f, A, 2, &stub_int);
    XiValue *zero = xi_const_int(f, A, 0, &stub_int);

    A->kind = XI_BLOCK_IF;
    A->control = make_cmp(f, A, XI_EQ, x, zero);
    A->succs[0] = B;
    A->succs[1] = C;
    xi_block_add_pred(B, A);
    xi_block_add_pred(C, A);

    B->kind = XI_BLOCK_PLAIN;
    B->succs[0] = D;
    xi_block_add_pred(D, B);

    C->kind = XI_BLOCK_RETURN;
    C->control = NULL;

    D->kind = XI_BLOCK_IF;
    D->control = make_cmp(f, D, XI_EQ, y, zero);
    D->succs[0] = E;
    D->succs[1] = F;
    xi_block_add_pred(E, D);
    xi_block_add_pred(F, D);

    E->kind = XI_BLOCK_RETURN;
    E->control = NULL;
    F->kind = XI_BLOCK_RETURN;
    F->control = NULL;

    XiPassChange chg = xi_opt_jump_thread(f);

    /* No threading: different operands. */
    ASSERT(!chg.cfg_changed);
    ASSERT(B->succs[0] == D);

    xi_func_free(f);
}

TEST(no_thread_plain_pred) {
    /*
     * A: goto D  (PLAIN, not IF)
     * D: if (x == 0) goto E else goto F
     * => No threading (pred is not IF).
     */
    XiFunc *f = make_func();
    XiBlock *A = f->entry;
    XiBlock *D = xi_block_new(f);
    D->sealed = true;
    XiBlock *E = xi_block_new(f);
    E->sealed = true;
    XiBlock *F = xi_block_new(f);
    F->sealed = true;

    XiValue *x = xi_const_int(f, A, 1, &stub_int);
    XiValue *zero = xi_const_int(f, A, 0, &stub_int);

    A->kind = XI_BLOCK_PLAIN;
    A->succs[0] = D;
    xi_block_add_pred(D, A);

    D->kind = XI_BLOCK_IF;
    D->control = make_cmp(f, D, XI_EQ, x, zero);
    D->succs[0] = E;
    D->succs[1] = F;
    xi_block_add_pred(E, D);
    xi_block_add_pred(F, D);

    E->kind = XI_BLOCK_RETURN;
    E->control = NULL;
    F->kind = XI_BLOCK_RETURN;
    F->control = NULL;

    XiPassChange chg = xi_opt_jump_thread(f);

    ASSERT(!chg.cfg_changed);

    xi_func_free(f);
}

TEST(single_block_no_crash) {
    XiFunc *f = make_func();
    f->entry->kind = XI_BLOCK_RETURN;
    f->entry->control = NULL;

    XiPassChange chg = xi_opt_jump_thread(f);
    ASSERT(!chg.cfg_changed);

    xi_func_free(f);
}

TEST(no_thread_loop_backedge_from_target_condition) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    XiBlock *header = xi_block_new(f);
    XiBlock *body = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);
    header->sealed = true;
    body->sealed = true;
    exit_blk->sealed = true;

    XiValue *zero = xi_const_int(f, entry, 0, &stub_int);
    XiValue *limit = xi_const_int(f, entry, 10, &stub_int);
    xi_block_set_jump(entry, header);

    XiPhi *iv = xi_phi_new(f, header, &stub_int, 2);
    iv->value.args[0] = zero;
    XiValue *cmp = make_cmp(f, header, XI_LT, &iv->value, limit);
    xi_block_set_if(header, cmp, body, exit_blk);

    XiValue *one = xi_const_int(f, body, 1, &stub_int);
    XiValue *next = xi_binary(f, body, XI_ADD, &stub_int, &iv->value, one);
    xi_block_set_jump(body, header);
    iv->value.args[1] = next;

    xi_block_set_return(exit_blk, &iv->value);

    XiPassChange chg = xi_opt_jump_thread(f);

    ASSERT(!chg.cfg_changed);
    ASSERT(body->succs[0] == header);
    ASSERT(header->npreds == 2);
    ASSERT(iv->value.nargs == 2);

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok)
        printf("  verify error: %s\n", errbuf);
    ASSERT(ok);

    xi_func_free(f);
}

TEST(thread_ne_eq_negation) {
    /*
     * A: if (x != y) goto B else goto C
     * B: goto D
     * D: if (x == y) goto E else goto F
     *    (x==y is negation of x!=y)
     * => B's edge to D redirected to F
     *    (x!=y true from A->B means x==y false → else = F)
     */
    XiFunc *f = make_func();
    XiBlock *A = f->entry;
    XiBlock *B = xi_block_new(f);
    B->sealed = true;
    XiBlock *C = xi_block_new(f);
    C->sealed = true;
    XiBlock *D = xi_block_new(f);
    D->sealed = true;
    XiBlock *E = xi_block_new(f);
    E->sealed = true;
    XiBlock *F = xi_block_new(f);
    F->sealed = true;

    XiValue *x = xi_const_int(f, A, 1, &stub_int);
    XiValue *y = xi_const_int(f, A, 2, &stub_int);

    A->kind = XI_BLOCK_IF;
    A->control = make_cmp(f, A, XI_NE, x, y);
    A->succs[0] = B;
    A->succs[1] = C;
    xi_block_add_pred(B, A);
    xi_block_add_pred(C, A);

    B->kind = XI_BLOCK_PLAIN;
    B->succs[0] = D;
    xi_block_add_pred(D, B);

    C->kind = XI_BLOCK_RETURN;
    C->control = NULL;

    D->kind = XI_BLOCK_IF;
    D->control = make_cmp(f, D, XI_EQ, x, y);
    D->succs[0] = E;
    D->succs[1] = F;
    xi_block_add_pred(E, D);
    xi_block_add_pred(F, D);

    E->kind = XI_BLOCK_RETURN;
    E->control = NULL;
    F->kind = XI_BLOCK_RETURN;
    F->control = NULL;

    xi_opt_jump_thread(f);

    ASSERT(B->succs[0] == F);

    xi_func_free(f);
}

/* ========== Runner ========== */

int main(void) {
    printf("=== Xi Jump Thread Tests ===\n\n");

    /* Same condition */
    run_thread_same_eq();
    run_thread_else_side();

    /* Negated condition */
    run_thread_negated_lt_ge();
    run_thread_ne_eq_negation();

    /* Non-threadable */
    run_no_thread_different_operands();
    run_no_thread_plain_pred();
    run_single_block_no_crash();
    run_no_thread_loop_backedge_from_target_condition();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
