/*
 * test_xi_opt.c - Unit tests for Xi IR optimization passes
 *
 * Tests constant folding, copy propagation, dead code elimination,
 * phi simplification, and the combined pass runner.
 */

#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_opt.h"
#include "../../../src/ir/xi_opt_block_simplify.h"
#include "../../../src/ir/xi_opt_jump_thread.h"
#include "../../../src/ir/xi_tbaa.h"
#include "../../../src/ir/xi_verify.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

/* Minimal XrType stubs */
static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_float = {.kind = XR_KIND_FLOAT, .id = 2, .frozen = true};
static XrType stub_bool = {.kind = XR_KIND_BOOL, .id = 3, .frozen = true};
static XrType stub_null = {.kind = XR_KIND_NULL, .id = 4, .frozen = true};
static XrType stub_str = {.kind = XR_KIND_STRING, .id = 5, .frozen = true};
static XrType stub_void = {.kind = XR_KIND_UNIT, .id = 6, .frozen = true};
static XrType stub_func = {.kind = XR_KIND_FUNCTION, .id = 7, .frozen = true};
static XrType stub_u8 = {
    .kind = XR_KIND_INT, .id = 8, .frozen = true, .native_width = XR_NATIVE_U8};
static XrType stub_u8_array = {
    .kind = XR_KIND_ARRAY,
    .id = 9,
    .frozen = true,
    .container = {.element_type = &stub_u8},
};

static int tests_passed = 0;
static int tests_failed = 0;

/* Create function with sealed entry block. */
static XiFunc *make_func(const char *name, XrType *ret_type) {
    XiFunc *f = xi_func_new(name, ret_type);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    return f;
}

#define TEST(name)                                                                                 \
    static void test_##name(void);                                                                 \
    static void run_##name(void) {                                                                 \
        printf("--- " #name " ---\n");                                                             \
        test_##name();                                                                             \
        printf("  PASS\n");                                                                        \
        tests_passed++;                                                                            \
    }                                                                                              \
    static void test_##name(void)

/* ========== Constant Folding Tests ========== */

TEST(const_fold_int_add) {
    /* 3 + 4 -> 7 */
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
    /* 10 - 3 -> 7 */
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
    /* 5 * 6 -> 30 */
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
    /* 20 / 4 -> 5 */
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
    /* 10 / 0 -> NOT folded (undefined behavior) */
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
    /* 3 < 5 -> true (1) */
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
    /* -(42) -> -42 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c42 = xi_const_int(f, blk, 42, &stub_int);
    XiValue *neg = xi_unary(f, blk, XI_NEG, &stub_int, c42);

    xi_opt_const_fold(f);

    assert(neg->op == XI_CONST && neg->aux_int == -42);
    xi_func_free(f);
}

TEST(const_fold_not) {
    /* !true -> false */
    XiFunc *f = make_func("test", &stub_bool);
    XiBlock *blk = f->entry;

    XiValue *ctrue = xi_const_bool(f, blk, true, &stub_bool);
    XiValue *n = xi_unary(f, blk, XI_NOT, &stub_bool, ctrue);

    xi_opt_const_fold(f);

    assert(n->op == XI_CONST && n->aux_int == 0 && "!true should be false");
    xi_func_free(f);
}

TEST(const_fold_float_add) {
    /* 1.5 + 2.5 -> 4.0 */
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
    /* (2 + 3) * 4 -> 5 * 4 -> 20 after two passes */
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
    /* 17 % 5 -> 2 */
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
    /* 10 % 0 -> NOT folded */
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
    /* ~0 -> -1 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c0 = xi_const_int(f, blk, 0, &stub_int);
    XiValue *bn = xi_unary(f, blk, XI_BNOT, &stub_int, c0);

    xi_opt_const_fold(f);

    assert(bn->op == XI_CONST && bn->aux_int == ~(int64_t) 0);
    xi_func_free(f);
}

TEST(const_fold_float_sub) {
    /* 5.0 - 1.5 -> 3.5 */
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
    /* 2.0 < 3.0 -> true */
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
    /* 7 == 7 -> true */
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
    /* 3 != 5 -> true */
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
    /* 0xFF & 0x0F -> 0x0F; 0xA0 | 0x05 -> 0xA5; 6 ^ 3 -> 5 */
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
    /* 1 << 4 -> 16; 32 >> 2 -> 8 */
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
    /* x = COPY(a); y = x + 1 -> y = a + 1 */
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
    /* a -> COPY -> COPY -> use -> use(a) */
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

    xi_const_int(f, blk, 42, &stub_int);               /* dead */
    XiValue *c2 = xi_const_int(f, blk, 99, &stub_int); /* live */
    xi_block_set_return(blk, c2);

    uint32_t before = blk->nvalues;
    xi_opt_dce(f);

    assert(blk->nvalues < before && "dead value should be removed");
    /* c2 should remain (used by return) */
    bool found = false;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        if (blk->values[i] == c2)
            found = true;
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
        if (blk->values[i]->op == XI_PRINT)
            found = true;
    }
    assert(found && "side-effecting value should not be removed");
    xi_func_free(f);
}

TEST(dce_cascading) {
    /* a = 1; b = a + 2; (b unused) -> both removed */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c1 = xi_const_int(f, blk, 1, &stub_int);
    XiValue *c2 = xi_const_int(f, blk, 2, &stub_int);
    xi_binary(f, blk, XI_ADD, &stub_int, c1, c2);       /* dead */
    XiValue *c99 = xi_const_int(f, blk, 99, &stub_int); /* live */
    xi_block_set_return(blk, c99);

    xi_opt_dce(f);

    /* Only c99 should remain */
    assert(blk->nvalues == 1 && "only the live return value should remain");
    assert(blk->values[0] == c99);
    xi_func_free(f);
}

/* ========== Phi Simplification Tests ========== */

TEST(phi_simplify_trivial) {
    /* phi(a, a) -> a */
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
        if (blk->values[i] == cp)
            found_copy = true;
    }
    assert(!found_copy && "dead copy should be removed");
    xi_func_free(f);
}

/* Strength reduction tests live in test_xi_strength.c (dedicated). */

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

/* GVN-PRE tests live in test_xi_gvn_pre.c (dedicated). */

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
    v->type = NULL; /* intentionally break invariant */

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
    blk->control = NULL; /* broken! */

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
    if (!ok)
        printf("  verify error: %s\n", errbuf);
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
    call->args[0] = p0; /* callee */
    call->args[1] = p0; /* arg */
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

TEST(select_rep_keeps_narrow_store_for_shared_typed_array) {
    XiFunc *f = make_func("test", &stub_void);
    XiBlock *blk = f->entry;

    XiValue *arr = xi_value_new(f, blk, XI_GET_SHARED, &stub_u8_array, 0);
    arr->aux_int = 0;
    XiValue *idx = xi_const_int(f, blk, 0, &stub_int);
    XiValue *val = xi_const_int(f, blk, 300, &stub_int);
    XiValue *narrow = xi_value_new(f, blk, XI_NARROW_U8, &stub_int, 1);
    narrow->args[0] = val;

    XiValue *store = xi_value_new(f, blk, XI_INDEX_SET, &stub_void, 3);
    store->args[0] = arr;
    store->args[1] = idx;
    store->args[2] = narrow;
    xi_block_set_return(blk, NULL);

    XiRepPolicy policy = xi_rep_policy_aot_transition();
    xi_opt_select_rep_with_policy(f, &policy);

    assert(store->args[2] == narrow && "typed array store must consume NARROW_U8 directly");
    assert(narrow->rep == XR_REP_I64 && "NARROW_U8 stays in an integer register");

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok)
        printf("  verify error: %s\n", errbuf);
    assert(ok && "shared typed array store should verify after select_rep");
    xi_func_free(f);
}

TEST(select_rep_native_policy_keeps_return_unboxed) {
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c42 = xi_const_int(f, blk, 42, &stub_int);
    xi_block_set_return(blk, c42);

    XiRepPolicy policy = xi_rep_policy_native_boundary();
    xi_opt_select_rep_with_policy(f, &policy);

    assert(blk->control == c42 && "native return policy should not BOX scalar return");
    assert(c42->rep == XR_REP_I64 && "int return should stay I64");

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok)
        printf("  verify error: %s\n", errbuf);
    assert(ok && "native scalar return rep should verify");
    xi_func_free(f);
}

TEST(select_rep_aot_policy_keeps_scalar_phi_unboxed) {
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;
    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);
    XiBlock *merge = xi_block_new(f);

    XiValue *cond = xi_const_bool(f, entry, true, &stub_bool);
    xi_block_set_if(entry, cond, then_blk, else_blk);

    XiValue *c1 = xi_const_int(f, then_blk, 1, &stub_int);
    xi_block_set_jump(then_blk, merge);

    XiValue *c2 = xi_const_int(f, else_blk, 2, &stub_int);
    xi_block_set_jump(else_blk, merge);

    XiPhi *phi = xi_phi_new(f, merge, &stub_int, 2);
    phi->value.args[0] = c1;
    phi->value.args[1] = c2;

    XiValue *c3 = xi_const_int(f, merge, 3, &stub_int);
    XiValue *add = xi_binary(f, merge, XI_ADD, &stub_int, &phi->value, c3);
    xi_block_set_return(merge, add);

    XiRepPolicy policy = xi_rep_policy_aot_transition();
    xi_opt_select_rep_with_policy(f, &policy);

    assert(phi->value.rep == XR_REP_I64 && "AOT transition policy should keep int phi I64");
    assert(phi->value.args[0] == c1 && "I64 phi arg should not be boxed");
    assert(phi->value.args[1] == c2 && "I64 phi arg should not be boxed");
    assert(add->args[0] == &phi->value && "arithmetic should consume native phi directly");
    assert(merge->nvalues == 3 && "only return BOX should be inserted in merge block");
    assert(merge->values[2]->op == XI_BOX && "AOT transition still boxes return for old cgen ABI");

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok)
        printf("  verify error: %s\n", errbuf);
    assert(ok && "native scalar phi rep should verify");
    xi_func_free(f);
}

/* ========== Tuple Projection Peephole Tests ========== */

/* TUPLE_GET(TUPLE_NEW(a, b), 0) collapses to COPY(a). */
TEST(tuple_get_of_tuple_new_first) {
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *e0 = xi_const_int(f, blk, 7, &stub_int);
    XiValue *e1 = xi_const_int(f, blk, 9, &stub_int);
    XiValue *tup = xi_value_new(f, blk, XI_TUPLE_NEW, &stub_int, 2);
    tup->args[0] = e0;
    tup->args[1] = e1;
    tup->aux_int = 2;

    XiValue *get = xi_value_new(f, blk, XI_TUPLE_GET, &stub_int, 1);
    get->args[0] = tup;
    get->aux_int = 0;
    xi_block_set_return(blk, get);

    xi_opt_const_fold(f);

    assert(get->op == XI_COPY && "TUPLE_GET(TUPLE_NEW, 0) should collapse to COPY");
    assert(get->args[0] == e0 && "COPY should forward to the original element");
    assert(get->aux_int == 0 && "COPY should clear the slot index");
    xi_func_free(f);
}

/* TUPLE_GET(TUPLE_NEW(a, b), 1) collapses to COPY(b). */
TEST(tuple_get_of_tuple_new_second) {
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *e0 = xi_const_int(f, blk, 11, &stub_int);
    XiValue *e1 = xi_const_int(f, blk, 13, &stub_int);
    XiValue *tup = xi_value_new(f, blk, XI_TUPLE_NEW, &stub_int, 2);
    tup->args[0] = e0;
    tup->args[1] = e1;
    tup->aux_int = 2;

    XiValue *get = xi_value_new(f, blk, XI_TUPLE_GET, &stub_int, 1);
    get->args[0] = tup;
    get->aux_int = 1;
    xi_block_set_return(blk, get);

    xi_opt_const_fold(f);

    assert(get->op == XI_COPY && "TUPLE_GET(TUPLE_NEW, 1) should collapse to COPY");
    assert(get->args[0] == e1 && "COPY should forward to element[1]");
    xi_func_free(f);
}

/* TUPLE_GET on a tuple that did NOT come from TUPLE_NEW must stay intact —
 * we cannot synthesise the source slot otherwise. */
TEST(tuple_get_unrelated_source_keeps_op) {
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *p0 = xi_param(f, blk, 0, &stub_int);
    XiValue *get = xi_value_new(f, blk, XI_TUPLE_GET, &stub_int, 1);
    get->args[0] = p0;
    get->aux_int = 0;
    xi_block_set_return(blk, get);

    xi_opt_const_fold(f);

    assert(get->op == XI_TUPLE_GET && "TUPLE_GET on non-NEW source must remain");
    assert(get->args[0] == p0 && "source should be untouched");
    xi_func_free(f);
}

/* DCE should reap the TUPLE_NEW once every TUPLE_GET has been collapsed
 * to copies that no longer reference it. */
TEST(tuple_new_eliminated_after_full_projection) {
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *e0 = xi_const_int(f, blk, 100, &stub_int);
    XiValue *e1 = xi_const_int(f, blk, 200, &stub_int);
    XiValue *tup = xi_value_new(f, blk, XI_TUPLE_NEW, &stub_int, 2);
    tup->args[0] = e0;
    tup->args[1] = e1;
    tup->aux_int = 2;

    XiValue *get0 = xi_value_new(f, blk, XI_TUPLE_GET, &stub_int, 1);
    get0->args[0] = tup;
    get0->aux_int = 0;
    XiValue *get1 = xi_value_new(f, blk, XI_TUPLE_GET, &stub_int, 1);
    get1->args[0] = tup;
    get1->aux_int = 1;
    XiValue *sum = xi_binary(f, blk, XI_ADD, &stub_int, get0, get1);
    xi_block_set_return(blk, sum);

    /* Run the standard pipeline so peephole + copy_prop + dce all apply. */
    xi_opt_run(f);

    bool tuple_new_alive = false;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        if (blk->values[i] && blk->values[i]->op == XI_TUPLE_NEW) {
            tuple_new_alive = true;
            break;
        }
    }
    assert(!tuple_new_alive && "TUPLE_NEW should be DCE'd once all GETs project away");
    xi_func_free(f);
}

TEST(tuple_get_fold_clears_tbaa_metadata) {
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *e0 = xi_const_int(f, blk, 100, &stub_int);
    XiValue *tup = xi_value_new(f, blk, XI_TUPLE_NEW, &stub_int, 1);
    tup->args[0] = e0;
    tup->aux_int = 1;

    XiValue *get = xi_value_new(f, blk, XI_TUPLE_GET, &stub_int, 1);
    get->args[0] = tup;
    get->aux_int = 0;
    xi_block_set_return(blk, get);

    xi_tbaa_annotate(f);
    assert(get->mem_group == XI_MEM_TUPLE && "TUPLE_GET should be annotated before folding");

    xi_opt_const_fold(f);

    assert(get->op == XI_COPY && "TUPLE_GET(TUPLE_NEW, 0) should collapse to COPY");
    assert(get->mem_group == XI_MEM_NONE && "COPY must not keep TUPLE_GET memory group");

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok)
        printf("  verify error: %s\n", errbuf);
    assert(ok && "folded COPY should satisfy TBAA verifier");
    xi_func_free(f);
}

/* ========== BOX/UNBOX Peephole Tests ========== */

TEST(box_elim_unbox_of_box) {
    /* UNBOX(BOX(x)) -> COPY(x) */
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
    /* BOX(UNBOX(x)) -> COPY(x) */
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

/* ========== Block Simplify Tests ========== */

TEST(block_simplify_single_pred_empty_block) {
    /* entry --> empty --> exit
     * 'empty' is PLAIN, has no values and one pred, one succ → it can
     * be eliminated and the function can collapse into a single block. */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiBlock *empty = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);
    empty->sealed = true;
    exit_blk->sealed = true;

    xi_block_set_jump(entry, empty);
    xi_block_set_jump(empty, exit_blk);
    xi_block_set_return(exit_blk, xi_const_int(f, exit_blk, 7, &stub_int));

    XiPassChange chg = xi_opt_block_simplify(f);

    assert(chg.cfg_changed && "block_simplify should report a CFG change");
    assert(f->nblocks <= 2 && "single-pred chain should collapse");

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok)
        printf("  verify error: %s\n", errbuf);
    assert(ok && "IR should remain valid after block_simplify");
    xi_func_free(f);
}

TEST(block_simplify_keeps_ir_valid_for_multi_pred_empty) {
    /* Two branches funnel through a shared empty PLAIN block before
     * reaching the join. block_simplify must not corrupt the CFG —
     * either it leaves the empty block in place or it correctly
     * stitches every predecessor through. The hard contract is: the
     * IR must remain a valid SSA program. */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *cond = xi_param(f, entry, 0, &stub_bool);

    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);
    XiBlock *funnel = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);
    then_blk->sealed = true;
    else_blk->sealed = true;
    funnel->sealed = true;
    exit_blk->sealed = true;

    xi_block_set_if(entry, cond, then_blk, else_blk);
    xi_block_set_jump(then_blk, funnel);
    xi_block_set_jump(else_blk, funnel);
    xi_block_set_jump(funnel, exit_blk);
    xi_block_set_return(exit_blk, xi_const_int(f, exit_blk, 9, &stub_int));

    (void) xi_opt_block_simplify(f);

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok)
        printf("  verify error: %s\n", errbuf);
    assert(ok && "block_simplify must preserve SSA validity on multi-pred empty");
    xi_func_free(f);
}

TEST(block_simplify_preserves_phi_when_merging) {
    /* exit block has a phi over its two preds. block_simplify must not
     * desync npreds and phi.nargs while operating on the shape. */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *cond = xi_param(f, entry, 0, &stub_bool);

    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);
    then_blk->sealed = true;
    else_blk->sealed = true;
    exit_blk->sealed = true;

    xi_block_set_if(entry, cond, then_blk, else_blk);
    XiValue *t = xi_const_int(f, then_blk, 1, &stub_int);
    XiValue *e = xi_const_int(f, else_blk, 2, &stub_int);
    xi_block_set_jump(then_blk, exit_blk);
    xi_block_set_jump(else_blk, exit_blk);

    XiPhi *phi = xi_phi_new(f, exit_blk, &stub_int, 2);
    phi->value.args[0] = t;
    phi->value.args[1] = e;
    xi_block_set_return(exit_blk, &phi->value);

    (void) xi_opt_block_simplify(f);

    /* Whichever shape survives, all phis in surviving blocks must agree
     * with their block's npreds. */
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (XiPhi *p = blk->phis; p; p = p->next) {
            assert(p->value.nargs == blk->npreds &&
                   "phi.nargs must match block.npreds after block_simplify");
        }
    }

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok)
        printf("  verify error: %s\n", errbuf);
    assert(ok);
    xi_func_free(f);
}

TEST(block_simplify_merges_past_arena_value_capacity) {
    /* Block value arrays belong to the Xi arena. Merging a successor into a
     * full predecessor must grow by arena-copying, not by heap realloc. */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;
    XiBlock *exit_blk = xi_block_new(f);
    exit_blk->sealed = true;

    uint32_t initial_cap = entry->values_cap;
    for (uint32_t i = 0; i < initial_cap; i++)
        (void) xi_const_int(f, entry, (int64_t) i, &stub_int);

    XiValue *ret = xi_const_int(f, exit_blk, 123, &stub_int);
    xi_block_set_jump(entry, exit_blk);
    xi_block_set_return(exit_blk, ret);

    XiPassChange chg = xi_opt_block_simplify(f);

    assert(chg.cfg_changed && "merge should report a CFG change");
    assert(f->nblocks == 1 && "linear return chain should merge into entry");
    assert(entry->nvalues == initial_cap + 1 && "merged value should be appended");
    assert(entry->values[initial_cap] == ret && "merged value should retain order");
    assert(ret->block == entry && "merged value should be rebound to predecessor");

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok)
        printf("  verify error: %s\n", errbuf);
    assert(ok && "IR should remain valid after growing during block merge");
    xi_func_free(f);
}

/* ========== Jump Thread Tests ========== */

TEST(jump_thread_basic_redirect) {
    /* entry tests cmp; both branches funnel into 'merge' which tests
     * the same cmp again. Both incoming edges should be threaded to
     * the matching successor of merge, leaving merge unreachable. */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *zero = xi_const_int(f, entry, 0, &stub_int);
    XiValue *cmp = xi_binary(f, entry, XI_EQ, &stub_bool, x, zero);

    XiBlock *a_blk = xi_block_new(f);
    XiBlock *b_blk = xi_block_new(f);
    XiBlock *merge = xi_block_new(f);
    XiBlock *t_blk = xi_block_new(f);
    XiBlock *f_blk = xi_block_new(f);
    a_blk->sealed = true;
    b_blk->sealed = true;
    merge->sealed = true;
    t_blk->sealed = true;
    f_blk->sealed = true;

    xi_block_set_if(entry, cmp, a_blk, b_blk);
    xi_block_set_jump(a_blk, merge);
    xi_block_set_jump(b_blk, merge);
    xi_block_set_if(merge, cmp, t_blk, f_blk);
    xi_block_set_return(t_blk, xi_const_int(f, t_blk, 100, &stub_int));
    xi_block_set_return(f_blk, xi_const_int(f, f_blk, 200, &stub_int));

    XiPassChange chg = xi_opt_jump_thread(f);

    assert(chg.cfg_changed && "jump_thread should redirect both edges");
    assert(a_blk->succs[0] == t_blk && "a_blk should now jump straight to t_blk");
    assert(b_blk->succs[0] == f_blk && "b_blk should now jump straight to f_blk");

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok)
        printf("  verify error: %s\n", errbuf);
    assert(ok && "IR must remain valid after jump_thread");
    xi_func_free(f);
}

TEST(jump_thread_keeps_ir_valid_when_dest_has_phi) {
    /* Same threading shape, but t_blk holds a phi anchored to merge.
     * Naive redirect would attach a fresh predecessor to t_blk without
     * extending the phi argument list, breaking npreds == phi.nargs.
     * The pass must either correctly extend the phi or refuse to
     * thread; either way the verifier must pass. */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *zero = xi_const_int(f, entry, 0, &stub_int);
    XiValue *cmp = xi_binary(f, entry, XI_EQ, &stub_bool, x, zero);

    XiBlock *a_blk = xi_block_new(f);
    XiBlock *b_blk = xi_block_new(f);
    XiBlock *merge = xi_block_new(f);
    XiBlock *t_blk = xi_block_new(f);
    XiBlock *f_blk = xi_block_new(f);
    a_blk->sealed = true;
    b_blk->sealed = true;
    merge->sealed = true;
    t_blk->sealed = true;
    f_blk->sealed = true;

    xi_block_set_if(entry, cmp, a_blk, b_blk);
    xi_block_set_jump(a_blk, merge);
    xi_block_set_jump(b_blk, merge);
    XiValue *m_val = xi_const_int(f, merge, 99, &stub_int);
    xi_block_set_if(merge, cmp, t_blk, f_blk);

    XiPhi *phi = xi_phi_new(f, t_blk, &stub_int, 1);
    phi->value.args[0] = m_val;
    xi_block_set_return(t_blk, &phi->value);
    xi_block_set_return(f_blk, xi_const_int(f, f_blk, 200, &stub_int));

    (void) xi_opt_jump_thread(f);

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (XiPhi *p = blk->phis; p; p = p->next) {
            assert(p->value.nargs == blk->npreds &&
                   "phi.nargs must match block.npreds after jump_thread");
        }
    }

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok)
        printf("  verify error: %s\n", errbuf);
    assert(ok && "IR must remain valid after jump_thread on a phi-bearing dest");
    xi_func_free(f);
}

/* ========== Main ========== */

int main(void) {
    printf("=== Xi Opt Unit Tests ===\n\n");

    (void) stub_null;
    (void) stub_str;

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

    /* GVN-PRE: covered by test_xi_gvn_pre.c (dedicated). */
    /* Strength reduction: covered by test_xi_strength.c (dedicated). */

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
    run_select_rep_keeps_narrow_store_for_shared_typed_array();
    run_select_rep_native_policy_keeps_return_unboxed();
    run_select_rep_aot_policy_keeps_scalar_phi_unboxed();

    /* Tuple projection peephole */
    run_tuple_get_of_tuple_new_first();
    run_tuple_get_of_tuple_new_second();
    run_tuple_get_unrelated_source_keeps_op();
    run_tuple_new_eliminated_after_full_projection();
    run_tuple_get_fold_clears_tbaa_metadata();

    /* BOX/UNBOX peephole */
    run_box_elim_unbox_of_box();
    run_box_elim_box_of_unbox();
    run_box_elim_no_false_positive();

    /* Block simplify */
    run_block_simplify_single_pred_empty_block();
    run_block_simplify_keeps_ir_valid_for_multi_pred_empty();
    run_block_simplify_preserves_phi_when_merging();
    run_block_simplify_merges_past_arena_value_capacity();

    /* Jump thread */
    run_jump_thread_basic_redirect();
    run_jump_thread_keeps_ir_valid_when_dest_has_phi();

    printf("\n=== %d/%d Xi Opt tests passed ===\n", tests_passed, tests_passed + tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
