/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xm_fold.c - Unit tests for Xm FOLD engine (on-the-fly peephole)
 */

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <math.h>
#include "../../../src/jit/xm.h"
#include "../../../src/jit/xm_fold.h"
#include "../test_win_compat.h"

/* ========== Helpers ========== */

/* Create a fresh func + block for each test */
static void setup(XmFunc **func, XmBlock **blk) {
    *func = xm_func_new("fold_test");
    *blk = xm_func_add_block(*func, "entry");
}

static void teardown(XmFunc *func) {
    xm_func_destroy(func);
}

/* Emit a CONST_I64 load and return the vreg */
static XmRef make_i64(XmFunc *func, XmBlock *blk, int64_t val) {
    XmRef cref = xm_const_i64(func, val);
    return xm_emit_unary(func, blk, XM_CONST_I64, XR_REP_I64, cref);
}

/* Emit a CONST_F64 load and return the vreg */
static XmRef make_f64(XmFunc *func, XmBlock *blk, double val) {
    XmRef cref = xm_const_f64(func, val);
    return xm_emit_unary(func, blk, XM_CONST_F64, XR_REP_F64, cref);
}

/* Check if two refs are the same vreg */
static bool same_ref(XmRef a, XmRef b) {
    return a == b;
}

/* Get the i64 constant value of a vreg (assumes CONST_I64 def) */
static int64_t get_i64_val(XmFunc *func, XmRef ref) {
    assert(xm_ref_is_vreg(ref));
    uint32_t idx = XM_REF_INDEX(ref);
    assert(idx < func->nvreg);
    XmIns *def = func->vregs[idx].def;
    assert(def && def->op == XM_CONST_I64);
    assert(xm_ref_is_const(def->args[0]));
    uint32_t cidx = XM_REF_INDEX(def->args[0]);
    return func->consts[cidx].val.i64;
}

/* Get the f64 constant value of a vreg (assumes CONST_F64 def) */
static double get_f64_val(XmFunc *func, XmRef ref) {
    assert(xm_ref_is_vreg(ref));
    uint32_t idx = XM_REF_INDEX(ref);
    assert(idx < func->nvreg);
    XmIns *def = func->vregs[idx].def;
    assert(def && def->op == XM_CONST_F64);
    assert(xm_ref_is_const(def->args[0]));
    uint32_t cidx = XM_REF_INDEX(def->args[0]);
    return func->consts[cidx].val.f64;
}

/* ========== Test: Identity Elimination ========== */

static void test_identity_add_zero(void) {
    fprintf(stderr, "  test_identity_add_zero...");
    XmFunc *func;
    XmBlock *blk;
    setup(&func, &blk);

    XmRef x = make_i64(func, blk, 42);
    XmRef zero = make_i64(func, blk, 0);

    /* x + 0 â†?x */
    XmRef r = xm_fold_emit(func, blk, XM_ADD, XR_REP_I64, x, zero);
    assert(same_ref(r, x));

    /* 0 + x â†?x */
    r = xm_fold_emit(func, blk, XM_ADD, XR_REP_I64, zero, x);
    assert(same_ref(r, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_identity_sub_zero(void) {
    fprintf(stderr, "  test_identity_sub_zero...");
    XmFunc *func;
    XmBlock *blk;
    setup(&func, &blk);

    XmRef x = make_i64(func, blk, 42);
    XmRef zero = make_i64(func, blk, 0);

    /* x - 0 â†?x */
    XmRef r = xm_fold_emit(func, blk, XM_SUB, XR_REP_I64, x, zero);
    assert(same_ref(r, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_identity_mul_one(void) {
    fprintf(stderr, "  test_identity_mul_one...");
    XmFunc *func;
    XmBlock *blk;
    setup(&func, &blk);

    XmRef x = make_i64(func, blk, 42);
    XmRef one = make_i64(func, blk, 1);

    /* x * 1 â†?x */
    XmRef r = xm_fold_emit(func, blk, XM_MUL, XR_REP_I64, x, one);
    assert(same_ref(r, x));

    /* 1 * x â†?x */
    r = xm_fold_emit(func, blk, XM_MUL, XR_REP_I64, one, x);
    assert(same_ref(r, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_identity_div_one(void) {
    fprintf(stderr, "  test_identity_div_one...");
    XmFunc *func;
    XmBlock *blk;
    setup(&func, &blk);

    XmRef x = make_i64(func, blk, 42);
    XmRef one = make_i64(func, blk, 1);

    /* x / 1 â†?x */
    XmRef r = xm_fold_emit(func, blk, XM_DIV, XR_REP_I64, x, one);
    assert(same_ref(r, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_identity_or_zero(void) {
    fprintf(stderr, "  test_identity_or_zero...");
    XmFunc *func;
    XmBlock *blk;
    setup(&func, &blk);

    XmRef x = make_i64(func, blk, 42);
    XmRef zero = make_i64(func, blk, 0);

    /* x | 0 â†?x */
    XmRef r = xm_fold_emit(func, blk, XM_OR, XR_REP_I64, x, zero);
    assert(same_ref(r, x));

    /* 0 | x â†?x */
    r = xm_fold_emit(func, blk, XM_OR, XR_REP_I64, zero, x);
    assert(same_ref(r, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_identity_and_allones(void) {
    fprintf(stderr, "  test_identity_and_allones...");
    XmFunc *func;
    XmBlock *blk;
    setup(&func, &blk);

    XmRef x = make_i64(func, blk, 42);
    XmRef allones = make_i64(func, blk, -1);

    /* x & -1 â†?x */
    XmRef r = xm_fold_emit(func, blk, XM_AND, XR_REP_I64, x, allones);
    assert(same_ref(r, x));

    /* -1 & x â†?x */
    r = xm_fold_emit(func, blk, XM_AND, XR_REP_I64, allones, x);
    assert(same_ref(r, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_identity_xor_zero(void) {
    fprintf(stderr, "  test_identity_xor_zero...");
    XmFunc *func;
    XmBlock *blk;
    setup(&func, &blk);

    XmRef x = make_i64(func, blk, 42);
    XmRef zero = make_i64(func, blk, 0);

    /* x ^ 0 â†?x */
    XmRef r = xm_fold_emit(func, blk, XM_XOR, XR_REP_I64, x, zero);
    assert(same_ref(r, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_identity_shift_zero(void) {
    fprintf(stderr, "  test_identity_shift_zero...");
    XmFunc *func;
    XmBlock *blk;
    setup(&func, &blk);

    XmRef x = make_i64(func, blk, 42);
    XmRef zero = make_i64(func, blk, 0);

    /* x << 0 â†?x */
    XmRef r = xm_fold_emit(func, blk, XM_SHL, XR_REP_I64, x, zero);
    assert(same_ref(r, x));

    /* x >> 0 â†?x */
    r = xm_fold_emit(func, blk, XM_SHR, XR_REP_I64, x, zero);
    assert(same_ref(r, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Test: Annihilation ========== */

static void test_annihilation_mul_zero(void) {
    fprintf(stderr, "  test_annihilation_mul_zero...");
    XmFunc *func;
    XmBlock *blk;
    setup(&func, &blk);

    XmRef x = make_i64(func, blk, 42);
    XmRef zero = make_i64(func, blk, 0);

    /* x * 0 â†?0 */
    XmRef r = xm_fold_emit(func, blk, XM_MUL, XR_REP_I64, x, zero);
    assert(get_i64_val(func, r) == 0);

    /* 0 * x â†?0 */
    r = xm_fold_emit(func, blk, XM_MUL, XR_REP_I64, zero, x);
    assert(get_i64_val(func, r) == 0);

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_annihilation_and_zero(void) {
    fprintf(stderr, "  test_annihilation_and_zero...");
    XmFunc *func;
    XmBlock *blk;
    setup(&func, &blk);

    XmRef x = make_i64(func, blk, 42);
    XmRef zero = make_i64(func, blk, 0);

    /* x & 0 â†?0 */
    XmRef r = xm_fold_emit(func, blk, XM_AND, XR_REP_I64, x, zero);
    assert(get_i64_val(func, r) == 0);

    /* 0 & x â†?0 */
    r = xm_fold_emit(func, blk, XM_AND, XR_REP_I64, zero, x);
    assert(get_i64_val(func, r) == 0);

    teardown(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Test: Self-Operation ========== */

static void test_self_sub(void) {
    fprintf(stderr, "  test_self_sub...");
    XmFunc *func;
    XmBlock *blk;
    setup(&func, &blk);

    XmRef x = make_i64(func, blk, 42);

    /* x - x â†?0 */
    XmRef r = xm_fold_emit(func, blk, XM_SUB, XR_REP_I64, x, x);
    assert(get_i64_val(func, r) == 0);

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_self_xor(void) {
    fprintf(stderr, "  test_self_xor...");
    XmFunc *func;
    XmBlock *blk;
    setup(&func, &blk);

    XmRef x = make_i64(func, blk, 42);

    /* x ^ x â†?0 */
    XmRef r = xm_fold_emit(func, blk, XM_XOR, XR_REP_I64, x, x);
    assert(get_i64_val(func, r) == 0);

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_self_and(void) {
    fprintf(stderr, "  test_self_and...");
    XmFunc *func;
    XmBlock *blk;
    setup(&func, &blk);

    XmRef x = make_i64(func, blk, 42);

    /* x & x â†?x */
    XmRef r = xm_fold_emit(func, blk, XM_AND, XR_REP_I64, x, x);
    assert(same_ref(r, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_self_or(void) {
    fprintf(stderr, "  test_self_or...");
    XmFunc *func;
    XmBlock *blk;
    setup(&func, &blk);

    XmRef x = make_i64(func, blk, 42);

    /* x | x â†?x */
    XmRef r = xm_fold_emit(func, blk, XM_OR, XR_REP_I64, x, x);
    assert(same_ref(r, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Test: Double Negation ========== */

static void test_double_neg(void) {
    fprintf(stderr, "  test_double_neg...");
    XmFunc *func;
    XmBlock *blk;
    setup(&func, &blk);

    XmRef x = make_i64(func, blk, 42);

    /* NEG(x) â†?intermediate */
    XmRef neg1 = xm_fold_emit(func, blk, XM_NEG, XR_REP_I64, x, XM_NONE);
    assert(!same_ref(neg1, x));

    /* NEG(NEG(x)) â†?x */
    XmRef neg2 = xm_fold_emit(func, blk, XM_NEG, XR_REP_I64, neg1, XM_NONE);
    assert(same_ref(neg2, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_double_not(void) {
    fprintf(stderr, "  test_double_not...");
    XmFunc *func;
    XmBlock *blk;
    setup(&func, &blk);

    XmRef x = make_i64(func, blk, 42);

    /* NOT(x) â†?intermediate */
    XmRef not1 = xm_fold_emit(func, blk, XM_NOT, XR_REP_I64, x, XM_NONE);
    assert(!same_ref(not1, x));

    /* NOT(NOT(x)) â†?x */
    XmRef not2 = xm_fold_emit(func, blk, XM_NOT, XR_REP_I64, not1, XM_NONE);
    assert(same_ref(not2, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_double_fneg(void) {
    fprintf(stderr, "  test_double_fneg...");
    XmFunc *func;
    XmBlock *blk;
    setup(&func, &blk);

    XmRef x = make_f64(func, blk, 3.14);

    /* FNEG(x) â†?intermediate */
    XmRef neg1 = xm_fold_emit(func, blk, XM_FNEG, XR_REP_F64, x, XM_NONE);
    assert(!same_ref(neg1, x));

    /* FNEG(FNEG(x)) â†?x */
    XmRef neg2 = xm_fold_emit(func, blk, XM_FNEG, XR_REP_F64, neg1, XM_NONE);
    assert(same_ref(neg2, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Test: Conversion Roundtrip ========== */

static void test_f2i_i2f_roundtrip(void) {
    fprintf(stderr, "  test_f2i_i2f_roundtrip...");
    XmFunc *func;
    XmBlock *blk;
    setup(&func, &blk);

    XmRef x = make_i64(func, blk, 42);

    /* I2F(x) â†?intermediate */
    XmRef i2f = xm_fold_emit(func, blk, XM_I2F, XR_REP_F64, x, XM_NONE);
    assert(!same_ref(i2f, x));

    /* F2I(I2F(x)) â†?x */
    XmRef f2i = xm_fold_emit(func, blk, XM_F2I, XR_REP_I64, i2f, XM_NONE);
    assert(same_ref(f2i, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_i2f_f2i_roundtrip(void) {
    fprintf(stderr, "  test_i2f_f2i_roundtrip...");
    XmFunc *func;
    XmBlock *blk;
    setup(&func, &blk);

    XmRef x = make_f64(func, blk, 3.14);

    /* F2I(x) â†?intermediate */
    XmRef f2i = xm_fold_emit(func, blk, XM_F2I, XR_REP_I64, x, XM_NONE);
    assert(!same_ref(f2i, x));

    /* I2F(F2I(x)) â†?x */
    XmRef i2f = xm_fold_emit(func, blk, XM_I2F, XR_REP_F64, f2i, XM_NONE);
    assert(same_ref(i2f, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Test: Constant Folding (i64) ========== */

static void test_const_fold_i64(void) {
    fprintf(stderr, "  test_const_fold_i64...");
    XmFunc *func;
    XmBlock *blk;
    setup(&func, &blk);

    XmRef a = make_i64(func, blk, 10);
    XmRef b = make_i64(func, blk, 3);

    /* 10 + 3 â†?13 */
    XmRef r = xm_fold_emit(func, blk, XM_ADD, XR_REP_I64, a, b);
    assert(get_i64_val(func, r) == 13);

    /* 10 - 3 â†?7 */
    r = xm_fold_emit(func, blk, XM_SUB, XR_REP_I64, a, b);
    assert(get_i64_val(func, r) == 7);

    /* 10 * 3 â†?30 */
    r = xm_fold_emit(func, blk, XM_MUL, XR_REP_I64, a, b);
    assert(get_i64_val(func, r) == 30);

    /* 10 / 3 â†?3 */
    r = xm_fold_emit(func, blk, XM_DIV, XR_REP_I64, a, b);
    assert(get_i64_val(func, r) == 3);

    /* 10 % 3 â†?1 */
    r = xm_fold_emit(func, blk, XM_MOD, XR_REP_I64, a, b);
    assert(get_i64_val(func, r) == 1);

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_const_fold_i64_bitwise(void) {
    fprintf(stderr, "  test_const_fold_i64_bitwise...");
    XmFunc *func;
    XmBlock *blk;
    setup(&func, &blk);

    XmRef a = make_i64(func, blk, 0xFF);
    XmRef b = make_i64(func, blk, 0x0F);

    /* 0xFF & 0x0F â†?0x0F */
    XmRef r = xm_fold_emit(func, blk, XM_AND, XR_REP_I64, a, b);
    assert(get_i64_val(func, r) == 0x0F);

    /* 0xFF | 0x0F â†?0xFF */
    r = xm_fold_emit(func, blk, XM_OR, XR_REP_I64, a, b);
    assert(get_i64_val(func, r) == 0xFF);

    /* 0xFF ^ 0x0F â†?0xF0 */
    r = xm_fold_emit(func, blk, XM_XOR, XR_REP_I64, a, b);
    assert(get_i64_val(func, r) == 0xF0);

    /* 1 << 4 â†?16 */
    XmRef one = make_i64(func, blk, 1);
    XmRef four = make_i64(func, blk, 4);
    r = xm_fold_emit(func, blk, XM_SHL, XR_REP_I64, one, four);
    assert(get_i64_val(func, r) == 16);

    /* 16 >> 2 â†?4 */
    XmRef sixteen = make_i64(func, blk, 16);
    XmRef two = make_i64(func, blk, 2);
    r = xm_fold_emit(func, blk, XM_SHR, XR_REP_I64, sixteen, two);
    assert(get_i64_val(func, r) == 4);

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_const_fold_i64_cmp(void) {
    fprintf(stderr, "  test_const_fold_i64_cmp...");
    XmFunc *func;
    XmBlock *blk;
    setup(&func, &blk);

    XmRef a = make_i64(func, blk, 10);
    XmRef b = make_i64(func, blk, 20);

    /* 10 < 20 â†?1 */
    XmRef r = xm_fold_emit(func, blk, XM_LT, XR_REP_I64, a, b);
    assert(get_i64_val(func, r) == 1);

    /* 20 < 10 â†?0 */
    r = xm_fold_emit(func, blk, XM_LT, XR_REP_I64, b, a);
    assert(get_i64_val(func, r) == 0);

    /* 10 == 10 â†?1 */
    r = xm_fold_emit(func, blk, XM_EQ, XR_REP_I64, a, a);
    assert(get_i64_val(func, r) == 1);

    /* 10 != 20 â†?1 */
    r = xm_fold_emit(func, blk, XM_NE, XR_REP_I64, a, b);
    assert(get_i64_val(func, r) == 1);

    /* 10 <= 10 â†?1 */
    r = xm_fold_emit(func, blk, XM_LE, XR_REP_I64, a, a);
    assert(get_i64_val(func, r) == 1);

    /* 10 >= 20 â†?0 */
    r = xm_fold_emit(func, blk, XM_GE, XR_REP_I64, a, b);
    assert(get_i64_val(func, r) == 0);

    teardown(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Test: Constant Folding (f64) ========== */

static void test_const_fold_f64(void) {
    fprintf(stderr, "  test_const_fold_f64...");
    XmFunc *func;
    XmBlock *blk;
    setup(&func, &blk);

    XmRef a = make_f64(func, blk, 2.5);
    XmRef b = make_f64(func, blk, 1.5);

    /* 2.5 + 1.5 â†?4.0 */
    XmRef r = xm_fold_emit(func, blk, XM_FADD, XR_REP_F64, a, b);
    assert(get_f64_val(func, r) == 4.0);

    /* 2.5 - 1.5 â†?1.0 */
    r = xm_fold_emit(func, blk, XM_FSUB, XR_REP_F64, a, b);
    assert(get_f64_val(func, r) == 1.0);

    /* 2.5 * 1.5 â†?3.75 */
    r = xm_fold_emit(func, blk, XM_FMUL, XR_REP_F64, a, b);
    assert(get_f64_val(func, r) == 3.75);

    /* 3.0 / 1.5 â†?2.0 */
    XmRef three = make_f64(func, blk, 3.0);
    r = xm_fold_emit(func, blk, XM_FDIV, XR_REP_F64, three, b);
    assert(get_f64_val(func, r) == 2.0);

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_const_fold_f64_cmp(void) {
    fprintf(stderr, "  test_const_fold_f64_cmp...");
    XmFunc *func;
    XmBlock *blk;
    setup(&func, &blk);

    XmRef a = make_f64(func, blk, 1.0);
    XmRef b = make_f64(func, blk, 2.0);

    /* 1.0 < 2.0 â†?1 */
    XmRef r = xm_fold_emit(func, blk, XM_FLT, XR_REP_I64, a, b);
    assert(get_i64_val(func, r) == 1);

    /* 1.0 == 1.0 â†?1 */
    r = xm_fold_emit(func, blk, XM_FEQ, XR_REP_I64, a, a);
    assert(get_i64_val(func, r) == 1);

    /* 1.0 != 2.0 â†?1 */
    r = xm_fold_emit(func, blk, XM_FNE, XR_REP_I64, a, b);
    assert(get_i64_val(func, r) == 1);

    /* 2.0 <= 1.0 â†?0 */
    r = xm_fold_emit(func, blk, XM_FLE, XR_REP_I64, b, a);
    assert(get_i64_val(func, r) == 0);

    teardown(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Test: Float Identity ========== */

static void test_float_identity(void) {
    fprintf(stderr, "  test_float_identity...");
    XmFunc *func;
    XmBlock *blk;
    setup(&func, &blk);

    XmRef x = make_f64(func, blk, 3.14);
    XmRef zero = make_f64(func, blk, 0.0);
    XmRef one = make_f64(func, blk, 1.0);

    /* x + 0.0 â†?x */
    XmRef r = xm_fold_emit(func, blk, XM_FADD, XR_REP_F64, x, zero);
    assert(same_ref(r, x));

    /* 0.0 + x â†?x */
    r = xm_fold_emit(func, blk, XM_FADD, XR_REP_F64, zero, x);
    assert(same_ref(r, x));

    /* x - 0.0 â†?x */
    r = xm_fold_emit(func, blk, XM_FSUB, XR_REP_F64, x, zero);
    assert(same_ref(r, x));

    /* x * 1.0 â†?x */
    r = xm_fold_emit(func, blk, XM_FMUL, XR_REP_F64, x, one);
    assert(same_ref(r, x));

    /* 1.0 * x â†?x */
    r = xm_fold_emit(func, blk, XM_FMUL, XR_REP_F64, one, x);
    assert(same_ref(r, x));

    /* x / 1.0 â†?x */
    r = xm_fold_emit(func, blk, XM_FDIV, XR_REP_F64, x, one);
    assert(same_ref(r, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Test: No-fold (non-const operand) ========== */

static void test_no_fold_dynamic(void) {
    fprintf(stderr, "  test_no_fold_dynamic...");
    XmFunc *func;
    XmBlock *blk;
    setup(&func, &blk);

    /* Create a vreg that is NOT a CONST_I64 (simulate a param) */
    XmRef param = xm_new_vreg(func, XR_REP_I64);
    XmRef zero = make_i64(func, blk, 0);

    /* param + 0 â†?param (identity still works with right-const) */
    XmRef r = xm_fold_emit(func, blk, XM_ADD, XR_REP_I64, param, zero);
    assert(same_ref(r, param));

    /* param + param â†?should NOT fold (not self-zero pattern, just new instruction) */
    /* Actually param + param with same vreg has no special rule for ADD */
    uint32_t nins_before = blk->nins;
    r = xm_fold_emit(func, blk, XM_ADD, XR_REP_I64, param, param);
    assert(blk->nins == nins_before + 1);

    teardown(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Test: Div by zero not folded ========== */

static void test_div_by_zero_not_folded(void) {
    fprintf(stderr, "  test_div_by_zero_not_folded...");
    XmFunc *func;
    XmBlock *blk;
    setup(&func, &blk);

    XmRef a = make_i64(func, blk, 10);
    XmRef zero = make_i64(func, blk, 0);

    /* 10 / 0 â†?NOT folded (emits normal instruction) */
    uint32_t nins_before = blk->nins;
    XmRef r = xm_fold_emit(func, blk, XM_DIV, XR_REP_I64, a, zero);
    assert(blk->nins == nins_before + 1);
    (void) r;

    /* 10 % 0 â†?NOT folded */
    nins_before = blk->nins;
    r = xm_fold_emit(func, blk, XM_MOD, XR_REP_I64, a, zero);
    assert(blk->nins == nins_before + 1);

    teardown(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Main ========== */

int main(void) {
    xr_test_suppress_dialogs();
    fprintf(stderr, "=== test_xm_fold ===\n");

    /* Identity elimination */
    test_identity_add_zero();
    test_identity_sub_zero();
    test_identity_mul_one();
    test_identity_div_one();
    test_identity_or_zero();
    test_identity_and_allones();
    test_identity_xor_zero();
    test_identity_shift_zero();

    /* Annihilation */
    test_annihilation_mul_zero();
    test_annihilation_and_zero();

    /* Self-operation */
    test_self_sub();
    test_self_xor();
    test_self_and();
    test_self_or();

    /* Double negation */
    test_double_neg();
    test_double_not();
    test_double_fneg();

    /* Conversion roundtrip */
    test_f2i_i2f_roundtrip();
    test_i2f_f2i_roundtrip();

    /* Constant folding (i64) */
    test_const_fold_i64();
    test_const_fold_i64_bitwise();
    test_const_fold_i64_cmp();

    /* Constant folding (f64) */
    test_const_fold_f64();
    test_const_fold_f64_cmp();

    /* Float identity */
    test_float_identity();

    /* Edge cases */
    test_no_fold_dynamic();
    test_div_by_zero_not_folded();

    fprintf(stderr, "All %d tests passed!\n", 28);
    return 0;
}
