/*
 * Unit tests for Xi strength reduction (algebraic identity rewrites).
 *
 * Each case constructs a single binary op, runs the pass, and asserts
 * the value collapses to either XI_COPY (identity passes through) or
 * XI_CONST (zero / absorbing element).
 */

#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_opt_strength.h"
#include "../../../src/runtime/value/xtype.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)                                                                                 \
    static void test_##name(void);                                                                 \
    static void run_##name(void) {                                                                 \
        printf("--- " #name " ---\n");                                                             \
        test_##name();                                                                             \
        printf("  PASS\n");                                                                        \
        tests_passed++;                                                                            \
    }                                                                                              \
    static void test_##name(void)

static XiFunc *make_func(const char *name, XrType *ret_type) {
    XiFunc *f = xi_func_new(name, ret_type);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    return f;
}

/* ========== ADD / SUB ========== */

TEST(add_zero_rhs) {
    /* x + 0 -> x */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;
    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *c0 = xi_const_int(f, blk, 0, &stub_int);
    XiValue *add = xi_binary(f, blk, XI_ADD, &stub_int, x, c0);
    xi_block_set_return(blk, add);

    xi_opt_strength_reduce(f);
    assert(add->op == XI_COPY && add->args[0] == x);
    xi_func_free(f);
}

TEST(add_zero_lhs) {
    /* 0 + x -> x */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;
    XiValue *c0 = xi_const_int(f, blk, 0, &stub_int);
    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *add = xi_binary(f, blk, XI_ADD, &stub_int, c0, x);

    xi_opt_strength_reduce(f);
    assert(add->op == XI_COPY && add->args[0] == x);
    xi_func_free(f);
}

TEST(sub_self_is_zero) {
    /* x - x -> 0 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;
    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *sub = xi_binary(f, blk, XI_SUB, &stub_int, x, x);

    xi_opt_strength_reduce(f);
    assert(sub->op == XI_CONST && sub->aux_int == 0);
    xi_func_free(f);
}

/* ========== MUL / DIV ========== */

TEST(mul_zero_rhs_is_zero) {
    /* x * 0 -> 0 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;
    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *c0 = xi_const_int(f, blk, 0, &stub_int);
    XiValue *mul = xi_binary(f, blk, XI_MUL, &stub_int, x, c0);

    xi_opt_strength_reduce(f);
    assert(mul->op == XI_CONST && mul->aux_int == 0);
    xi_func_free(f);
}

TEST(mul_zero_lhs_is_zero) {
    /* 0 * x -> 0 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;
    XiValue *c0 = xi_const_int(f, blk, 0, &stub_int);
    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *mul = xi_binary(f, blk, XI_MUL, &stub_int, c0, x);

    xi_opt_strength_reduce(f);
    assert(mul->op == XI_CONST && mul->aux_int == 0);
    xi_func_free(f);
}

TEST(mul_one_rhs) {
    /* x * 1 -> x */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;
    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *c1 = xi_const_int(f, blk, 1, &stub_int);
    XiValue *mul = xi_binary(f, blk, XI_MUL, &stub_int, x, c1);

    xi_opt_strength_reduce(f);
    assert(mul->op == XI_COPY && mul->args[0] == x);
    xi_func_free(f);
}

TEST(mul_one_lhs) {
    /* 1 * x -> x */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;
    XiValue *c1 = xi_const_int(f, blk, 1, &stub_int);
    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *mul = xi_binary(f, blk, XI_MUL, &stub_int, c1, x);

    xi_opt_strength_reduce(f);
    assert(mul->op == XI_COPY && mul->args[0] == x);
    xi_func_free(f);
}

TEST(div_one) {
    /* x / 1 -> x */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;
    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *c1 = xi_const_int(f, blk, 1, &stub_int);
    XiValue *div = xi_binary(f, blk, XI_DIV, &stub_int, x, c1);

    xi_opt_strength_reduce(f);
    assert(div->op == XI_COPY && div->args[0] == x);
    xi_func_free(f);
}

/* ========== Bitwise ========== */

TEST(and_zero) {
    /* x & 0 -> 0 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;
    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *c0 = xi_const_int(f, blk, 0, &stub_int);
    XiValue *band = xi_binary(f, blk, XI_BAND, &stub_int, x, c0);

    xi_opt_strength_reduce(f);
    assert(band->op == XI_CONST && band->aux_int == 0);
    xi_func_free(f);
}

TEST(and_self) {
    /* x & x -> x */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;
    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *band = xi_binary(f, blk, XI_BAND, &stub_int, x, x);

    xi_opt_strength_reduce(f);
    assert(band->op == XI_COPY && band->args[0] == x);
    xi_func_free(f);
}

TEST(or_zero_lhs) {
    /* 0 | x -> x */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;
    XiValue *c0 = xi_const_int(f, blk, 0, &stub_int);
    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *bor = xi_binary(f, blk, XI_BOR, &stub_int, c0, x);

    xi_opt_strength_reduce(f);
    assert(bor->op == XI_COPY && bor->args[0] == x);
    xi_func_free(f);
}

TEST(or_self) {
    /* x | x -> x */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;
    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *bor = xi_binary(f, blk, XI_BOR, &stub_int, x, x);

    xi_opt_strength_reduce(f);
    assert(bor->op == XI_COPY && bor->args[0] == x);
    xi_func_free(f);
}

TEST(xor_self_is_zero) {
    /* x ^ x -> 0 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;
    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *xv = xi_binary(f, blk, XI_BXOR, &stub_int, x, x);

    xi_opt_strength_reduce(f);
    assert(xv->op == XI_CONST && xv->aux_int == 0);
    xi_func_free(f);
}

/* ========== Shifts ========== */

TEST(shl_zero) {
    /* x << 0 -> x */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;
    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *c0 = xi_const_int(f, blk, 0, &stub_int);
    XiValue *shl = xi_binary(f, blk, XI_SHL, &stub_int, x, c0);

    xi_opt_strength_reduce(f);
    assert(shl->op == XI_COPY && shl->args[0] == x);
    xi_func_free(f);
}

TEST(shr_zero) {
    /* x >> 0 -> x */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;
    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *c0 = xi_const_int(f, blk, 0, &stub_int);
    XiValue *shr = xi_binary(f, blk, XI_SHR, &stub_int, x, c0);

    xi_opt_strength_reduce(f);
    assert(shr->op == XI_COPY && shr->args[0] == x);
    xi_func_free(f);
}

/* ========== Main ========== */

int main(void) {
    printf("=== Xi strength reduction tests ===\n\n");

    run_add_zero_rhs();
    run_add_zero_lhs();
    run_sub_self_is_zero();
    run_mul_zero_rhs_is_zero();
    run_mul_zero_lhs_is_zero();
    run_mul_one_rhs();
    run_mul_one_lhs();
    run_div_one();
    run_and_zero();
    run_and_self();
    run_or_zero_lhs();
    run_or_self();
    run_xor_self_is_zero();
    run_shl_zero();
    run_shr_zero();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
