/*
 * test_xi_opt.c - Unit tests for Xi IR optimization passes
 *
 * Tests constant folding, copy propagation, dead code elimination,
 * phi simplification, and the combined pass runner.
 */

#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_opt.h"
#include "../../../src/ir/xi_verify.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

/* Minimal XrType stubs */
static XrType stub_int   = { .kind = XR_KIND_INT,    .id = 1, .frozen = true };
static XrType stub_float = { .kind = XR_KIND_FLOAT,  .id = 2, .frozen = true };
static XrType stub_bool  = { .kind = XR_KIND_BOOL,   .id = 3, .frozen = true };
static XrType stub_null  = { .kind = XR_KIND_NULL,   .id = 4, .frozen = true };
static XrType stub_str   = { .kind = XR_KIND_STRING, .id = 5, .frozen = true };
static XrType stub_void  = { .kind = XR_KIND_VOID,   .id = 6, .frozen = true };
static XrType stub_func  = { .kind = XR_KIND_FUNCTION, .id = 7, .frozen = true };

static int tests_passed = 0;
static int tests_failed = 0;

/* Create function with sealed entry block. */
static XiFunc *make_func(const char *name, XrType *ret_type) {
    XiFunc *f = xi_func_new(name, ret_type);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    return f;
}

#define TEST(name) \
    static void test_##name(void); \
    static void run_##name(void) { \
        printf("--- " #name " ---\n"); \
        test_##name(); \
        printf("  PASS\n"); \
        tests_passed++; \
    } \
    static void test_##name(void)

/* ========== Constant Folding Tests ========== */

TEST(const_fold_int_add) {
    /* 3 + 4 => 7 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c3 = xi_const_int(f, blk, 3, &stub_int);
    XiValue *c4 = xi_const_int(f, blk, 4, &stub_int);
    XiValue *add = xi_binary(f, blk, XI_ADD, &stub_int, c3, c4);
    xi_block_set_return(blk, add);

    xi_opt_const_fold(f);

    assert(add->op == XI_CONST && "add should be folded to CONST");
    assert(add->aux_int == 7 && "3 + 4 should be 7");
    assert(add->nargs == 0 && "folded value should have 0 args");
    xi_func_free(f);
}

TEST(const_fold_int_sub) {
    /* 10 - 3 => 7 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c10 = xi_const_int(f, blk, 10, &stub_int);
    XiValue *c3 = xi_const_int(f, blk, 3, &stub_int);
    XiValue *sub = xi_binary(f, blk, XI_SUB, &stub_int, c10, c3);

    xi_opt_const_fold(f);

    assert(sub->op == XI_CONST && sub->aux_int == 7);
    xi_func_free(f);
}

TEST(const_fold_int_mul) {
    /* 5 * 6 => 30 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c5 = xi_const_int(f, blk, 5, &stub_int);
    XiValue *c6 = xi_const_int(f, blk, 6, &stub_int);
    XiValue *mul = xi_binary(f, blk, XI_MUL, &stub_int, c5, c6);

    xi_opt_const_fold(f);

    assert(mul->op == XI_CONST && mul->aux_int == 30);
    xi_func_free(f);
}

TEST(const_fold_int_div) {
    /* 20 / 4 => 5 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c20 = xi_const_int(f, blk, 20, &stub_int);
    XiValue *c4 = xi_const_int(f, blk, 4, &stub_int);
    XiValue *div = xi_binary(f, blk, XI_DIV, &stub_int, c20, c4);

    xi_opt_const_fold(f);

    assert(div->op == XI_CONST && div->aux_int == 5);
    xi_func_free(f);
}

TEST(const_fold_div_by_zero) {
    /* 10 / 0 => NOT folded (undefined behavior) */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c10 = xi_const_int(f, blk, 10, &stub_int);
    XiValue *c0 = xi_const_int(f, blk, 0, &stub_int);
    XiValue *div = xi_binary(f, blk, XI_DIV, &stub_int, c10, c0);

    xi_opt_const_fold(f);

    assert(div->op == XI_DIV && "div by zero should NOT be folded");
    xi_func_free(f);
}

TEST(const_fold_int_compare) {
    /* 3 < 5 => true (1) */
    XiFunc *f = make_func("test", &stub_bool);
    XiBlock *blk = f->entry;

    XiValue *c3 = xi_const_int(f, blk, 3, &stub_int);
    XiValue *c5 = xi_const_int(f, blk, 5, &stub_int);
    XiValue *lt = xi_binary(f, blk, XI_LT, &stub_bool, c3, c5);

    xi_opt_const_fold(f);

    assert(lt->op == XI_CONST && lt->aux_int == 1 && "3 < 5 should be true");
    xi_func_free(f);
}

TEST(const_fold_neg) {
    /* -(42) => -42 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c42 = xi_const_int(f, blk, 42, &stub_int);
    XiValue *neg = xi_unary(f, blk, XI_NEG, &stub_int, c42);

    xi_opt_const_fold(f);

    assert(neg->op == XI_CONST && neg->aux_int == -42);
    xi_func_free(f);
}

TEST(const_fold_not) {
    /* !true => false */
    XiFunc *f = make_func("test", &stub_bool);
    XiBlock *blk = f->entry;

    XiValue *ctrue = xi_const_bool(f, blk, true, &stub_bool);
    XiValue *n = xi_unary(f, blk, XI_NOT, &stub_bool, ctrue);

    xi_opt_const_fold(f);

    assert(n->op == XI_CONST && n->aux_int == 0 && "!true should be false");
    xi_func_free(f);
}

TEST(const_fold_float_add) {
    /* 1.5 + 2.5 => 4.0 */
    XiFunc *f = make_func("test", &stub_float);
    XiBlock *blk = f->entry;

    XiValue *c1 = xi_const_float(f, blk, 1.5, &stub_float);
    XiValue *c2 = xi_const_float(f, blk, 2.5, &stub_float);
    XiValue *add = xi_binary(f, blk, XI_ADD, &stub_float, c1, c2);

    xi_opt_const_fold(f);

    assert(add->op == XI_CONST && "float add should be folded");
    double result;
    memcpy(&result, &add->aux_int, sizeof(double));
    assert(result == 4.0 && "1.5 + 2.5 should be 4.0");
    xi_func_free(f);
}

TEST(const_fold_chain) {
    /* (2 + 3) * 4 => 5 * 4 => 20 after two passes */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c2 = xi_const_int(f, blk, 2, &stub_int);
    XiValue *c3 = xi_const_int(f, blk, 3, &stub_int);
    XiValue *add = xi_binary(f, blk, XI_ADD, &stub_int, c2, c3);
    XiValue *c4 = xi_const_int(f, blk, 4, &stub_int);
    XiValue *mul = xi_binary(f, blk, XI_MUL, &stub_int, add, c4);

    /* First pass folds add to 5 */
    xi_opt_const_fold(f);
    assert(add->op == XI_CONST && add->aux_int == 5);

    /* Second pass folds mul to 20 (add is now const) */
    xi_opt_const_fold(f);
    assert(mul->op == XI_CONST && mul->aux_int == 20);
    xi_func_free(f);
}

TEST(const_fold_no_fold_variable) {
    /* x + 3 should NOT be folded */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *c3 = xi_const_int(f, blk, 3, &stub_int);
    XiValue *add = xi_binary(f, blk, XI_ADD, &stub_int, x, c3);

    xi_opt_const_fold(f);

    assert(add->op == XI_ADD && "x + 3 should NOT be folded");
    xi_func_free(f);
}

TEST(const_fold_int_mod) {
    /* 17 % 5 => 2 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c17 = xi_const_int(f, blk, 17, &stub_int);
    XiValue *c5 = xi_const_int(f, blk, 5, &stub_int);
    XiValue *mod = xi_binary(f, blk, XI_MOD, &stub_int, c17, c5);

    xi_opt_const_fold(f);

    assert(mod->op == XI_CONST && mod->aux_int == 2);
    xi_func_free(f);
}

TEST(const_fold_mod_by_zero) {
    /* 10 % 0 => NOT folded */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c10 = xi_const_int(f, blk, 10, &stub_int);
    XiValue *c0 = xi_const_int(f, blk, 0, &stub_int);
    XiValue *mod = xi_binary(f, blk, XI_MOD, &stub_int, c10, c0);

    xi_opt_const_fold(f);

    assert(mod->op == XI_MOD && "mod by zero should NOT be folded");
    xi_func_free(f);
}

TEST(const_fold_bnot) {
    /* ~0 => -1 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c0 = xi_const_int(f, blk, 0, &stub_int);
    XiValue *bn = xi_unary(f, blk, XI_BNOT, &stub_int, c0);

    xi_opt_const_fold(f);

    assert(bn->op == XI_CONST && bn->aux_int == ~(int64_t)0);
    xi_func_free(f);
}

TEST(const_fold_float_sub) {
    /* 5.0 - 1.5 => 3.5 */
    XiFunc *f = make_func("test", &stub_float);
    XiBlock *blk = f->entry;

    XiValue *c5 = xi_const_float(f, blk, 5.0, &stub_float);
    XiValue *c1 = xi_const_float(f, blk, 1.5, &stub_float);
    XiValue *sub = xi_binary(f, blk, XI_SUB, &stub_float, c5, c1);

    xi_opt_const_fold(f);

    assert(sub->op == XI_CONST && "float sub should be folded");
    double result;
    memcpy(&result, &sub->aux_int, sizeof(double));
    assert(result == 3.5 && "5.0 - 1.5 should be 3.5");
    xi_func_free(f);
}

TEST(const_fold_float_compare) {
    /* 2.0 < 3.0 => true */
    XiFunc *f = make_func("test", &stub_bool);
    XiBlock *blk = f->entry;

    XiValue *c2 = xi_const_float(f, blk, 2.0, &stub_float);
    XiValue *c3 = xi_const_float(f, blk, 3.0, &stub_float);
    XiValue *lt = xi_binary(f, blk, XI_LT, &stub_bool, c2, c3);

    xi_opt_const_fold(f);

    assert(lt->op == XI_CONST && lt->aux_int == 1 && "2.0 < 3.0 should be true");
    xi_func_free(f);
}

TEST(const_fold_int_eq) {
    /* 7 == 7 => true */
    XiFunc *f = make_func("test", &stub_bool);
    XiBlock *blk = f->entry;

    XiValue *c7a = xi_const_int(f, blk, 7, &stub_int);
    XiValue *c7b = xi_const_int(f, blk, 7, &stub_int);
    XiValue *eq = xi_binary(f, blk, XI_EQ, &stub_bool, c7a, c7b);

    xi_opt_const_fold(f);

    assert(eq->op == XI_CONST && eq->aux_int == 1 && "7 == 7 should be true");
    xi_func_free(f);
}

TEST(const_fold_int_ne) {
    /* 3 != 5 => true */
    XiFunc *f = make_func("test", &stub_bool);
    XiBlock *blk = f->entry;

    XiValue *c3 = xi_const_int(f, blk, 3, &stub_int);
    XiValue *c5 = xi_const_int(f, blk, 5, &stub_int);
    XiValue *ne = xi_binary(f, blk, XI_NE, &stub_bool, c3, c5);

    xi_opt_const_fold(f);

    assert(ne->op == XI_CONST && ne->aux_int == 1 && "3 != 5 should be true");
    xi_func_free(f);
}

TEST(const_fold_bitwise_ops) {
    /* 0xFF & 0x0F => 0x0F; 0xA0 | 0x05 => 0xA5; 6 ^ 3 => 5 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *cFF = xi_const_int(f, blk, 0xFF, &stub_int);
    XiValue *c0F = xi_const_int(f, blk, 0x0F, &stub_int);
    XiValue *band = xi_binary(f, blk, XI_BAND, &stub_int, cFF, c0F);

    XiValue *cA0 = xi_const_int(f, blk, 0xA0, &stub_int);
    XiValue *c05 = xi_const_int(f, blk, 0x05, &stub_int);
    XiValue *bor = xi_binary(f, blk, XI_BOR, &stub_int, cA0, c05);

    XiValue *c6 = xi_const_int(f, blk, 6, &stub_int);
    XiValue *c3 = xi_const_int(f, blk, 3, &stub_int);
    XiValue *bxor = xi_binary(f, blk, XI_BXOR, &stub_int, c6, c3);

    xi_opt_const_fold(f);

    assert(band->op == XI_CONST && band->aux_int == 0x0F);
    assert(bor->op == XI_CONST && bor->aux_int == 0xA5);
    assert(bxor->op == XI_CONST && bxor->aux_int == 5);
    xi_func_free(f);
}

TEST(const_fold_shift) {
    /* 1 << 4 => 16; 32 >> 2 => 8 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c1 = xi_const_int(f, blk, 1, &stub_int);
    XiValue *c4 = xi_const_int(f, blk, 4, &stub_int);
    XiValue *shl = xi_binary(f, blk, XI_SHL, &stub_int, c1, c4);

    XiValue *c32 = xi_const_int(f, blk, 32, &stub_int);
    XiValue *c2 = xi_const_int(f, blk, 2, &stub_int);
    XiValue *shr = xi_binary(f, blk, XI_SHR, &stub_int, c32, c2);

    xi_opt_const_fold(f);

    assert(shl->op == XI_CONST && shl->aux_int == 16);
    assert(shr->op == XI_CONST && shr->aux_int == 8);
    xi_func_free(f);
}

/* ========== Copy Propagation Tests ========== */

TEST(copy_prop_basic) {
    /* x = COPY(a); y = x + 1 => y = a + 1 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *a = xi_param(f, blk, 0, &stub_int);
    XiValue *copy = xi_value_new(f, blk, XI_COPY, &stub_int, 1);
    copy->args[0] = a;
    XiValue *c1 = xi_const_int(f, blk, 1, &stub_int);
    XiValue *add = xi_binary(f, blk, XI_ADD, &stub_int, copy, c1);

    xi_opt_copy_prop(f);

    assert(add->args[0] == a && "copy should be propagated");
    xi_func_free(f);
}

TEST(copy_prop_chain) {
    /* a -> COPY -> COPY -> use => use(a) */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *a = xi_param(f, blk, 0, &stub_int);
    XiValue *cp1 = xi_value_new(f, blk, XI_COPY, &stub_int, 1);
    cp1->args[0] = a;
    XiValue *cp2 = xi_value_new(f, blk, XI_COPY, &stub_int, 1);
    cp2->args[0] = cp1;
    XiValue *c1 = xi_const_int(f, blk, 1, &stub_int);
    XiValue *add = xi_binary(f, blk, XI_ADD, &stub_int, cp2, c1);

    xi_opt_copy_prop(f);

    assert(add->args[0] == a && "chained copies should resolve to original");
    xi_func_free(f);
}

/* ========== DCE Tests ========== */

TEST(dce_removes_unused) {
    /* dead: c1 = 42 (unused); live: return c2 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    xi_const_int(f, blk, 42, &stub_int);    /* dead */
    XiValue *c2 = xi_const_int(f, blk, 99, &stub_int);  /* live */
    xi_block_set_return(blk, c2);

    uint32_t before = blk->nvalues;
    xi_opt_dce(f);

    assert(blk->nvalues < before && "dead value should be removed");
    /* c2 should remain (used by return) */
    bool found = false;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        if (blk->values[i] == c2) found = true;
    }
    assert(found && "live value should remain");
    xi_func_free(f);
}

TEST(dce_keeps_side_effects) {
    /* PRINT is side-effecting, should not be removed even with 0 uses */
    XiFunc *f = make_func("test", &stub_void);
    XiBlock *blk = f->entry;

    XiValue *c1 = xi_const_int(f, blk, 42, &stub_int);
    XiValue *pr = xi_value_new(f, blk, XI_PRINT, &stub_void, 1);
    pr->args[0] = c1;
    pr->flags |= XI_FLAG_SIDE_EFFECT;
    xi_block_set_return(blk, NULL);

    xi_opt_dce(f);

    /* Print should survive */
    bool found = false;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        if (blk->values[i]->op == XI_PRINT) found = true;
    }
    assert(found && "side-effecting value should not be removed");
    xi_func_free(f);
}

TEST(dce_cascading) {
    /* a = 1; b = a + 2; (b unused) => both removed */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c1 = xi_const_int(f, blk, 1, &stub_int);
    XiValue *c2 = xi_const_int(f, blk, 2, &stub_int);
    xi_binary(f, blk, XI_ADD, &stub_int, c1, c2);  /* dead */
    XiValue *c99 = xi_const_int(f, blk, 99, &stub_int);  /* live */
    xi_block_set_return(blk, c99);

    xi_opt_dce(f);

    /* Only c99 should remain */
    assert(blk->nvalues == 1 && "only the live return value should remain");
    assert(blk->values[0] == c99);
    xi_func_free(f);
}

/* ========== Phi Simplification Tests ========== */

TEST(phi_simplify_trivial) {
    /* phi(a, a) => a */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *a = xi_const_int(f, entry, 42, &stub_int);

    /* Create merge block with 2 preds */
    XiBlock *merge = xi_block_new(f);
    xi_block_add_pred(merge, entry);
    xi_block_add_pred(merge, entry);
    merge->sealed = true;

    XiPhi *phi = xi_phi_new(f, merge, &stub_int, 2);
    phi->value.args[0] = a;
    phi->value.args[1] = a;

    /* Create a use of phi in merge */
    XiValue *use = xi_value_new(f, merge, XI_PRINT, &stub_void, 1);
    use->args[0] = &phi->value;
    use->flags |= XI_FLAG_SIDE_EFFECT;
    xi_block_set_return(merge, NULL);

    xi_opt_phi_simplify(f);

    /* The phi should be removed and the use should reference 'a' directly */
    assert(merge->phis == NULL && "trivial phi should be removed");
    assert(use->args[0] == a && "use should reference original value");
    xi_func_free(f);
}

/* ========== Combined Pass Test ========== */

TEST(opt_run_combined) {
    /* 3 + 4 (folded to 7); dead COPY; return 7 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c3 = xi_const_int(f, blk, 3, &stub_int);
    XiValue *c4 = xi_const_int(f, blk, 4, &stub_int);
    XiValue *add = xi_binary(f, blk, XI_ADD, &stub_int, c3, c4);
    /* Dead copy */
    XiValue *cp = xi_value_new(f, blk, XI_COPY, &stub_int, 1);
    cp->args[0] = add;
    /* Return add directly */
    xi_block_set_return(blk, add);

    xi_opt_run(f);

    /* add should be folded to 7 */
    assert(add->op == XI_CONST && add->aux_int == 7);
    /* copy should be removed by DCE */
    bool found_copy = false;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        if (blk->values[i] == cp) found_copy = true;
    }
    assert(!found_copy && "dead copy should be removed");
    xi_func_free(f);
}

/* ========== Strength Reduction Tests ========== */

TEST(strength_add_zero) {
    /* x + 0 => x (copy) */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *c0 = xi_const_int(f, blk, 0, &stub_int);
    XiValue *add = xi_binary(f, blk, XI_ADD, &stub_int, x, c0);
    xi_block_set_return(blk, add);

    xi_opt_strength_reduce(f);

    assert(add->op == XI_COPY && "x + 0 should become COPY");
    assert(add->args[0] == x && "should copy x");
    xi_func_free(f);
}

TEST(strength_zero_add) {
    /* 0 + x => x */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c0 = xi_const_int(f, blk, 0, &stub_int);
    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *add = xi_binary(f, blk, XI_ADD, &stub_int, c0, x);

    xi_opt_strength_reduce(f);

    assert(add->op == XI_COPY && add->args[0] == x);
    xi_func_free(f);
}

TEST(strength_mul_zero) {
    /* x * 0 => 0 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *c0 = xi_const_int(f, blk, 0, &stub_int);
    XiValue *mul = xi_binary(f, blk, XI_MUL, &stub_int, x, c0);

    xi_opt_strength_reduce(f);

    assert(mul->op == XI_CONST && mul->aux_int == 0 && "x * 0 should be 0");
    xi_func_free(f);
}

TEST(strength_mul_one) {
    /* x * 1 => x */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *c1 = xi_const_int(f, blk, 1, &stub_int);
    XiValue *mul = xi_binary(f, blk, XI_MUL, &stub_int, x, c1);

    xi_opt_strength_reduce(f);

    assert(mul->op == XI_COPY && mul->args[0] == x && "x * 1 should become COPY");
    xi_func_free(f);
}

TEST(strength_sub_self) {
    /* x - x => 0 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *sub = xi_binary(f, blk, XI_SUB, &stub_int, x, x);

    xi_opt_strength_reduce(f);

    assert(sub->op == XI_CONST && sub->aux_int == 0 && "x - x should be 0");
    xi_func_free(f);
}

TEST(strength_xor_self) {
    /* x ^ x => 0 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *xr = xi_binary(f, blk, XI_BXOR, &stub_int, x, x);

    xi_opt_strength_reduce(f);

    assert(xr->op == XI_CONST && xr->aux_int == 0 && "x ^ x should be 0");
    xi_func_free(f);
}

TEST(strength_and_zero) {
    /* x & 0 => 0 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *c0 = xi_const_int(f, blk, 0, &stub_int);
    XiValue *band = xi_binary(f, blk, XI_BAND, &stub_int, x, c0);

    xi_opt_strength_reduce(f);

    assert(band->op == XI_CONST && band->aux_int == 0);
    xi_func_free(f);
}

TEST(strength_div_one) {
    /* x / 1 => x */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *c1 = xi_const_int(f, blk, 1, &stub_int);
    XiValue *div = xi_binary(f, blk, XI_DIV, &stub_int, x, c1);

    xi_opt_strength_reduce(f);

    assert(div->op == XI_COPY && div->args[0] == x && "x / 1 should copy x");
    xi_func_free(f);
}

TEST(strength_shl_zero) {
    /* x << 0 => x */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *c0 = xi_const_int(f, blk, 0, &stub_int);
    XiValue *shl = xi_binary(f, blk, XI_SHL, &stub_int, x, c0);

    xi_opt_strength_reduce(f);

    assert(shl->op == XI_COPY && shl->args[0] == x);
    xi_func_free(f);
}

TEST(strength_one_mul) {
    /* 1 * x => x */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c1 = xi_const_int(f, blk, 1, &stub_int);
    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *mul = xi_binary(f, blk, XI_MUL, &stub_int, c1, x);

    xi_opt_strength_reduce(f);

    assert(mul->op == XI_COPY && mul->args[0] == x && "1 * x should become COPY");
    xi_func_free(f);
}

TEST(strength_zero_mul_lhs) {
    /* 0 * x => 0 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c0 = xi_const_int(f, blk, 0, &stub_int);
    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *mul = xi_binary(f, blk, XI_MUL, &stub_int, c0, x);

    xi_opt_strength_reduce(f);

    assert(mul->op == XI_CONST && mul->aux_int == 0 && "0 * x should be 0");
    xi_func_free(f);
}

TEST(strength_and_self) {
    /* x & x => x */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *band = xi_binary(f, blk, XI_BAND, &stub_int, x, x);

    xi_opt_strength_reduce(f);

    assert(band->op == XI_COPY && band->args[0] == x && "x & x should be COPY x");
    xi_func_free(f);
}

TEST(strength_or_self) {
    /* x | x => x */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *bor = xi_binary(f, blk, XI_BOR, &stub_int, x, x);

    xi_opt_strength_reduce(f);

    assert(bor->op == XI_COPY && bor->args[0] == x && "x | x should be COPY x");
    xi_func_free(f);
}

TEST(strength_or_zero_lhs) {
    /* 0 | x => x */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c0 = xi_const_int(f, blk, 0, &stub_int);
    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *bor = xi_binary(f, blk, XI_BOR, &stub_int, c0, x);

    xi_opt_strength_reduce(f);

    assert(bor->op == XI_COPY && bor->args[0] == x);
    xi_func_free(f);
}

TEST(strength_shr_zero) {
    /* x >> 0 => x */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *c0 = xi_const_int(f, blk, 0, &stub_int);
    XiValue *shr = xi_binary(f, blk, XI_SHR, &stub_int, x, c0);

    xi_opt_strength_reduce(f);

    assert(shr->op == XI_COPY && shr->args[0] == x);
    xi_func_free(f);
}

TEST(phi_simplify_non_trivial) {
    /* phi(a, b) with a != b should NOT be simplified */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *a = xi_const_int(f, entry, 1, &stub_int);
    XiValue *b = xi_const_int(f, entry, 2, &stub_int);

    XiBlock *merge = xi_block_new(f);
    xi_block_add_pred(merge, entry);
    xi_block_add_pred(merge, entry);
    merge->sealed = true;

    XiPhi *phi = xi_phi_new(f, merge, &stub_int, 2);
    phi->value.args[0] = a;
    phi->value.args[1] = b;

    XiValue *use = xi_value_new(f, merge, XI_PRINT, &stub_void, 1);
    use->args[0] = &phi->value;
    use->flags |= XI_FLAG_SIDE_EFFECT;

    xi_opt_phi_simplify(f);

    assert(merge->phis != NULL && "non-trivial phi should NOT be removed");
    assert(use->args[0] == &phi->value && "use should still reference phi");
    xi_func_free(f);
}

/* ========== Verification Tests ========== */

TEST(verify_valid_func) {
    /* A well-formed function should pass verification */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c = xi_const_int(f, blk, 42, &stub_int);
    xi_block_set_return(blk, c);

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    assert(ok && "well-formed func should pass verification");
    assert(errbuf[0] == '\0');
    xi_func_free(f);
}

TEST(verify_null_type) {
    /* A value with NULL type should fail verification */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *v = xi_value_new(f, blk, XI_CONST, &stub_int, 0);
    v->type = NULL;  /* intentionally break invariant */

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    assert(!ok && "NULL type should fail verification");
    assert(errbuf[0] != '\0');
    xi_func_free(f);
}

TEST(verify_phi_arg_mismatch) {
    /* Phi with wrong arg count should fail */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *a = xi_const_int(f, entry, 1, &stub_int);

    XiBlock *merge = xi_block_new(f);
    xi_block_add_pred(merge, entry);
    xi_block_add_pred(merge, entry);
    merge->sealed = true;

    /* Create phi with 1 arg but block has 2 preds */
    XiPhi *phi = xi_phi_new(f, merge, &stub_int, 1);
    phi->value.args[0] = a;
    /* Manually set nargs to 1 (should be 2) */
    phi->value.nargs = 1;

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    assert(!ok && "phi arg mismatch should fail verification");
    xi_func_free(f);
}

TEST(verify_if_block_missing_control) {
    /* IF block with NULL control should fail */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);

    /* Manually set up an IF block with NULL control */
    blk->kind = XI_BLOCK_IF;
    blk->succs[0] = then_blk;
    blk->succs[1] = else_blk;
    blk->control = NULL;  /* broken! */

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    assert(!ok && "IF with NULL control should fail");
    xi_func_free(f);
}

TEST(verify_after_optimization) {
    /* Function should be valid after running all optimizations */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c3 = xi_const_int(f, blk, 3, &stub_int);
    XiValue *c4 = xi_const_int(f, blk, 4, &stub_int);
    XiValue *add = xi_binary(f, blk, XI_ADD, &stub_int, c3, c4);
    XiValue *c0 = xi_const_int(f, blk, 0, &stub_int);
    xi_binary(f, blk, XI_MUL, &stub_int, add, c0); /* dead: x * 0 = 0, unused */
    xi_block_set_return(blk, add);

    xi_opt_run(f);

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok) printf("  verify error: %s\n", errbuf);
    assert(ok && "function should be valid after optimization");
    xi_func_free(f);
}

/* ========== SelectRepresentations Tests ========== */

TEST(select_rep_box_const_for_return) {
    /* int constant returned: const(I64) -> return(TAGGED) needs BOX */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c42 = xi_const_int(f, blk, 42, &stub_int);
    xi_block_set_return(blk, c42);

    xi_opt_select_rep(f);

    /* Return control should now be a BOX wrapping c42 */
    assert(blk->control != c42 && "return should wrap const in BOX");
    assert(blk->control->op == XI_BOX && "wrapper should be XI_BOX");
    assert(blk->control->args[0] == c42 && "BOX arg should be the constant");
    xi_func_free(f);
}

TEST(select_rep_unbox_param_for_arith) {
    /* Typed int param gets I64 rep directly (no UNBOX needed for ADD).
     * Return value still needs BOX (I64 → TAGGED for caller). */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *p0 = xi_param(f, blk, 0, &stub_int);
    XiValue *c1 = xi_const_int(f, blk, 1, &stub_int);
    XiValue *add = xi_binary(f, blk, XI_ADD, &stub_int, p0, c1);
    xi_block_set_return(blk, add);

    xi_opt_select_rep(f);

    /* Typed int param is already I64: ADD uses it directly */
    assert(add->args[0] == p0 && "typed param used directly by ADD");
    assert(p0->rep == XR_REP_I64 && "int param should have I64 rep");
    /* ADD result is I64, return needs TAGGED: should have BOX */
    assert(blk->control->op == XI_BOX && "return should BOX the ADD result");
    xi_func_free(f);
}

TEST(select_rep_no_change_for_call) {
    /* CALL with TAGGED-rep params: no BOX/UNBOX needed */
    XiFunc *f = make_func("test", &stub_func);
    XiBlock *blk = f->entry;

    XiValue *p0 = xi_param(f, blk, 0, &stub_func);
    XiValue *call = xi_value_new(f, blk, XI_CALL, &stub_func, 2);
    call->args[0] = p0;  /* callee */
    call->args[1] = p0;  /* arg */
    call->flags |= XI_FLAG_SIDE_EFFECT;
    xi_block_set_return(blk, call);

    uint32_t nv_before = blk->nvalues;
    xi_opt_select_rep(f);

    /* No conversions should be inserted (all TAGGED -> TAGGED) */
    assert(blk->nvalues == nv_before && "no BOX/UNBOX for all-tagged path");
    xi_func_free(f);
}

TEST(select_rep_arith_chain_stays_unboxed) {
    /* a + b + c: all int arithmetic stays I64, only final return needs BOX */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c1 = xi_const_int(f, blk, 1, &stub_int);
    XiValue *c2 = xi_const_int(f, blk, 2, &stub_int);
    XiValue *c3 = xi_const_int(f, blk, 3, &stub_int);
    XiValue *add1 = xi_binary(f, blk, XI_ADD, &stub_int, c1, c2);
    XiValue *add2 = xi_binary(f, blk, XI_ADD, &stub_int, add1, c3);
    xi_block_set_return(blk, add2);

    xi_opt_select_rep(f);

    /* Intermediate arithmetic: no conversions (I64 -> I64) */
    assert(add1->args[0] == c1 && "const->add should stay direct");
    assert(add1->args[1] == c2 && "const->add should stay direct");
    assert(add2->args[0] == add1 && "add->add chain should stay unboxed");
    /* Only return needs BOX */
    assert(blk->control->op == XI_BOX && "return needs BOX");
    assert(blk->control->args[0] == add2 && "BOX wraps final add");
    xi_func_free(f);
}

/* ========== BOX/UNBOX Peephole Tests ========== */

TEST(box_elim_unbox_of_box) {
    /* UNBOX(BOX(x)) => COPY(x) */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *box = xi_value_new(f, blk, XI_BOX, &stub_int, 1);
    box->args[0] = x;
    XiValue *unbox = xi_value_new(f, blk, XI_UNBOX, &stub_int, 1);
    unbox->args[0] = box;
    xi_block_set_return(blk, unbox);

    xi_opt_box_elim(f);

    assert(unbox->op == XI_COPY && "UNBOX(BOX(x)) should become COPY");
    assert(unbox->args[0] == x && "COPY should reference original x");
    xi_func_free(f);
}

TEST(box_elim_box_of_unbox) {
    /* BOX(UNBOX(x)) => COPY(x) */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *unbox = xi_value_new(f, blk, XI_UNBOX, &stub_int, 1);
    unbox->args[0] = x;
    XiValue *box = xi_value_new(f, blk, XI_BOX, &stub_int, 1);
    box->args[0] = unbox;
    xi_block_set_return(blk, box);

    xi_opt_box_elim(f);

    assert(box->op == XI_COPY && "BOX(UNBOX(x)) should become COPY");
    assert(box->args[0] == x && "COPY should reference original x");
    xi_func_free(f);
}

TEST(box_elim_no_false_positive) {
    /* BOX(x) where x is not UNBOX: should NOT be eliminated */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *box = xi_value_new(f, blk, XI_BOX, &stub_int, 1);
    box->args[0] = x;
    xi_block_set_return(blk, box);

    xi_opt_box_elim(f);

    assert(box->op == XI_BOX && "BOX(param) should NOT be eliminated");
    xi_func_free(f);
}

/* ========== Main ========== */

int main(void) {
    printf("=== Xi Opt Unit Tests ===\n\n");

    (void)stub_null;
    (void)stub_str;

    /* Constant folding */
    run_const_fold_int_add();
    run_const_fold_int_sub();
    run_const_fold_int_mul();
    run_const_fold_int_div();
    run_const_fold_div_by_zero();
    run_const_fold_int_compare();
    run_const_fold_neg();
    run_const_fold_not();
    run_const_fold_float_add();
    run_const_fold_chain();
    run_const_fold_no_fold_variable();
    run_const_fold_int_mod();
    run_const_fold_mod_by_zero();
    run_const_fold_bnot();
    run_const_fold_float_sub();
    run_const_fold_float_compare();
    run_const_fold_int_eq();
    run_const_fold_int_ne();
    run_const_fold_bitwise_ops();
    run_const_fold_shift();

    /* Copy propagation */
    run_copy_prop_basic();
    run_copy_prop_chain();

    /* Dead code elimination */
    run_dce_removes_unused();
    run_dce_keeps_side_effects();
    run_dce_cascading();

    /* Phi simplification */
    run_phi_simplify_trivial();
    run_phi_simplify_non_trivial();

    /* Strength reduction */
    run_strength_add_zero();
    run_strength_zero_add();
    run_strength_mul_zero();
    run_strength_mul_one();
    run_strength_sub_self();
    run_strength_xor_self();
    run_strength_and_zero();
    run_strength_div_one();
    run_strength_shl_zero();
    run_strength_one_mul();
    run_strength_zero_mul_lhs();
    run_strength_and_self();
    run_strength_or_self();
    run_strength_or_zero_lhs();
    run_strength_shr_zero();

    /* Verification */
    run_verify_valid_func();
    run_verify_null_type();
    run_verify_phi_arg_mismatch();
    run_verify_if_block_missing_control();
    run_verify_after_optimization();

    /* Combined */
    run_opt_run_combined();

    /* SelectRepresentations */
    run_select_rep_box_const_for_return();
    run_select_rep_unbox_param_for_arith();
    run_select_rep_no_change_for_call();
    run_select_rep_arith_chain_stays_unboxed();

    /* BOX/UNBOX peephole */
    run_box_elim_unbox_of_box();
    run_box_elim_box_of_unbox();
    run_box_elim_no_false_positive();

    printf("\n=== %d/%d Xi Opt tests passed ===\n",
           tests_passed, tests_passed + tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
