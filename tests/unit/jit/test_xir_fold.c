/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_fold.c - Unit tests for XIR FOLD engine (on-the-fly peephole)
 */

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <math.h>
#include "../../../src/jit/xir.h"
#include "../../../src/jit/xir_fold.h"

/* ========== Helpers ========== */

/* Create a fresh func + block for each test */
static void setup(XirFunc **func, XirBlock **blk) {
    *func = xir_func_new("fold_test");
    *blk = xir_func_add_block(*func, "entry");
}

static void teardown(XirFunc *func) {
    xir_func_destroy(func);
}

/* Emit a CONST_I64 load and return the vreg */
static XirRef make_i64(XirFunc *func, XirBlock *blk, int64_t val) {
    XirRef cref = xir_const_i64(func, val);
    return xir_emit_unary(func, blk, XIR_CONST_I64, XR_REP_I64, cref);
}

/* Emit a CONST_F64 load and return the vreg */
static XirRef make_f64(XirFunc *func, XirBlock *blk, double val) {
    XirRef cref = xir_const_f64(func, val);
    return xir_emit_unary(func, blk, XIR_CONST_F64, XR_REP_F64, cref);
}

/* Check if two refs are the same vreg */
static bool same_ref(XirRef a, XirRef b) {
    return a == b;
}

/* Get the i64 constant value of a vreg (assumes CONST_I64 def) */
static int64_t get_i64_val(XirFunc *func, XirRef ref) {
    assert(xir_ref_is_vreg(ref));
    uint32_t idx = XIR_REF_INDEX(ref);
    assert(idx < func->nvreg);
    XirIns *def = func->vregs[idx].def;
    assert(def && def->op == XIR_CONST_I64);
    assert(xir_ref_is_const(def->args[0]));
    uint32_t cidx = XIR_REF_INDEX(def->args[0]);
    return func->consts[cidx].val.i64;
}

/* Get the f64 constant value of a vreg (assumes CONST_F64 def) */
static double get_f64_val(XirFunc *func, XirRef ref) {
    assert(xir_ref_is_vreg(ref));
    uint32_t idx = XIR_REF_INDEX(ref);
    assert(idx < func->nvreg);
    XirIns *def = func->vregs[idx].def;
    assert(def && def->op == XIR_CONST_F64);
    assert(xir_ref_is_const(def->args[0]));
    uint32_t cidx = XIR_REF_INDEX(def->args[0]);
    return func->consts[cidx].val.f64;
}

/* ========== Test: Identity Elimination ========== */

static void test_identity_add_zero(void) {
    fprintf(stderr, "  test_identity_add_zero...");
    XirFunc *func; XirBlock *blk;
    setup(&func, &blk);

    XirRef x = make_i64(func, blk, 42);
    XirRef zero = make_i64(func, blk, 0);

    /* x + 0 → x */
    XirRef r = xir_fold_emit(func, blk, XIR_ADD, XR_REP_I64, x, zero);
    assert(same_ref(r, x));

    /* 0 + x → x */
    r = xir_fold_emit(func, blk, XIR_ADD, XR_REP_I64, zero, x);
    assert(same_ref(r, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_identity_sub_zero(void) {
    fprintf(stderr, "  test_identity_sub_zero...");
    XirFunc *func; XirBlock *blk;
    setup(&func, &blk);

    XirRef x = make_i64(func, blk, 42);
    XirRef zero = make_i64(func, blk, 0);

    /* x - 0 → x */
    XirRef r = xir_fold_emit(func, blk, XIR_SUB, XR_REP_I64, x, zero);
    assert(same_ref(r, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_identity_mul_one(void) {
    fprintf(stderr, "  test_identity_mul_one...");
    XirFunc *func; XirBlock *blk;
    setup(&func, &blk);

    XirRef x = make_i64(func, blk, 42);
    XirRef one = make_i64(func, blk, 1);

    /* x * 1 → x */
    XirRef r = xir_fold_emit(func, blk, XIR_MUL, XR_REP_I64, x, one);
    assert(same_ref(r, x));

    /* 1 * x → x */
    r = xir_fold_emit(func, blk, XIR_MUL, XR_REP_I64, one, x);
    assert(same_ref(r, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_identity_div_one(void) {
    fprintf(stderr, "  test_identity_div_one...");
    XirFunc *func; XirBlock *blk;
    setup(&func, &blk);

    XirRef x = make_i64(func, blk, 42);
    XirRef one = make_i64(func, blk, 1);

    /* x / 1 → x */
    XirRef r = xir_fold_emit(func, blk, XIR_DIV, XR_REP_I64, x, one);
    assert(same_ref(r, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_identity_or_zero(void) {
    fprintf(stderr, "  test_identity_or_zero...");
    XirFunc *func; XirBlock *blk;
    setup(&func, &blk);

    XirRef x = make_i64(func, blk, 42);
    XirRef zero = make_i64(func, blk, 0);

    /* x | 0 → x */
    XirRef r = xir_fold_emit(func, blk, XIR_OR, XR_REP_I64, x, zero);
    assert(same_ref(r, x));

    /* 0 | x → x */
    r = xir_fold_emit(func, blk, XIR_OR, XR_REP_I64, zero, x);
    assert(same_ref(r, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_identity_and_allones(void) {
    fprintf(stderr, "  test_identity_and_allones...");
    XirFunc *func; XirBlock *blk;
    setup(&func, &blk);

    XirRef x = make_i64(func, blk, 42);
    XirRef allones = make_i64(func, blk, -1);

    /* x & -1 → x */
    XirRef r = xir_fold_emit(func, blk, XIR_AND, XR_REP_I64, x, allones);
    assert(same_ref(r, x));

    /* -1 & x → x */
    r = xir_fold_emit(func, blk, XIR_AND, XR_REP_I64, allones, x);
    assert(same_ref(r, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_identity_xor_zero(void) {
    fprintf(stderr, "  test_identity_xor_zero...");
    XirFunc *func; XirBlock *blk;
    setup(&func, &blk);

    XirRef x = make_i64(func, blk, 42);
    XirRef zero = make_i64(func, blk, 0);

    /* x ^ 0 → x */
    XirRef r = xir_fold_emit(func, blk, XIR_XOR, XR_REP_I64, x, zero);
    assert(same_ref(r, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_identity_shift_zero(void) {
    fprintf(stderr, "  test_identity_shift_zero...");
    XirFunc *func; XirBlock *blk;
    setup(&func, &blk);

    XirRef x = make_i64(func, blk, 42);
    XirRef zero = make_i64(func, blk, 0);

    /* x << 0 → x */
    XirRef r = xir_fold_emit(func, blk, XIR_SHL, XR_REP_I64, x, zero);
    assert(same_ref(r, x));

    /* x >> 0 → x */
    r = xir_fold_emit(func, blk, XIR_SHR, XR_REP_I64, x, zero);
    assert(same_ref(r, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Test: Annihilation ========== */

static void test_annihilation_mul_zero(void) {
    fprintf(stderr, "  test_annihilation_mul_zero...");
    XirFunc *func; XirBlock *blk;
    setup(&func, &blk);

    XirRef x = make_i64(func, blk, 42);
    XirRef zero = make_i64(func, blk, 0);

    /* x * 0 → 0 */
    XirRef r = xir_fold_emit(func, blk, XIR_MUL, XR_REP_I64, x, zero);
    assert(get_i64_val(func, r) == 0);

    /* 0 * x → 0 */
    r = xir_fold_emit(func, blk, XIR_MUL, XR_REP_I64, zero, x);
    assert(get_i64_val(func, r) == 0);

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_annihilation_and_zero(void) {
    fprintf(stderr, "  test_annihilation_and_zero...");
    XirFunc *func; XirBlock *blk;
    setup(&func, &blk);

    XirRef x = make_i64(func, blk, 42);
    XirRef zero = make_i64(func, blk, 0);

    /* x & 0 → 0 */
    XirRef r = xir_fold_emit(func, blk, XIR_AND, XR_REP_I64, x, zero);
    assert(get_i64_val(func, r) == 0);

    /* 0 & x → 0 */
    r = xir_fold_emit(func, blk, XIR_AND, XR_REP_I64, zero, x);
    assert(get_i64_val(func, r) == 0);

    teardown(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Test: Self-Operation ========== */

static void test_self_sub(void) {
    fprintf(stderr, "  test_self_sub...");
    XirFunc *func; XirBlock *blk;
    setup(&func, &blk);

    XirRef x = make_i64(func, blk, 42);

    /* x - x → 0 */
    XirRef r = xir_fold_emit(func, blk, XIR_SUB, XR_REP_I64, x, x);
    assert(get_i64_val(func, r) == 0);

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_self_xor(void) {
    fprintf(stderr, "  test_self_xor...");
    XirFunc *func; XirBlock *blk;
    setup(&func, &blk);

    XirRef x = make_i64(func, blk, 42);

    /* x ^ x → 0 */
    XirRef r = xir_fold_emit(func, blk, XIR_XOR, XR_REP_I64, x, x);
    assert(get_i64_val(func, r) == 0);

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_self_and(void) {
    fprintf(stderr, "  test_self_and...");
    XirFunc *func; XirBlock *blk;
    setup(&func, &blk);

    XirRef x = make_i64(func, blk, 42);

    /* x & x → x */
    XirRef r = xir_fold_emit(func, blk, XIR_AND, XR_REP_I64, x, x);
    assert(same_ref(r, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_self_or(void) {
    fprintf(stderr, "  test_self_or...");
    XirFunc *func; XirBlock *blk;
    setup(&func, &blk);

    XirRef x = make_i64(func, blk, 42);

    /* x | x → x */
    XirRef r = xir_fold_emit(func, blk, XIR_OR, XR_REP_I64, x, x);
    assert(same_ref(r, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Test: Double Negation ========== */

static void test_double_neg(void) {
    fprintf(stderr, "  test_double_neg...");
    XirFunc *func; XirBlock *blk;
    setup(&func, &blk);

    XirRef x = make_i64(func, blk, 42);

    /* NEG(x) → intermediate */
    XirRef neg1 = xir_fold_emit(func, blk, XIR_NEG, XR_REP_I64, x, XIR_NONE);
    assert(!same_ref(neg1, x));

    /* NEG(NEG(x)) → x */
    XirRef neg2 = xir_fold_emit(func, blk, XIR_NEG, XR_REP_I64, neg1, XIR_NONE);
    assert(same_ref(neg2, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_double_not(void) {
    fprintf(stderr, "  test_double_not...");
    XirFunc *func; XirBlock *blk;
    setup(&func, &blk);

    XirRef x = make_i64(func, blk, 42);

    /* NOT(x) → intermediate */
    XirRef not1 = xir_fold_emit(func, blk, XIR_NOT, XR_REP_I64, x, XIR_NONE);
    assert(!same_ref(not1, x));

    /* NOT(NOT(x)) → x */
    XirRef not2 = xir_fold_emit(func, blk, XIR_NOT, XR_REP_I64, not1, XIR_NONE);
    assert(same_ref(not2, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_double_fneg(void) {
    fprintf(stderr, "  test_double_fneg...");
    XirFunc *func; XirBlock *blk;
    setup(&func, &blk);

    XirRef x = make_f64(func, blk, 3.14);

    /* FNEG(x) → intermediate */
    XirRef neg1 = xir_fold_emit(func, blk, XIR_FNEG, XR_REP_F64, x, XIR_NONE);
    assert(!same_ref(neg1, x));

    /* FNEG(FNEG(x)) → x */
    XirRef neg2 = xir_fold_emit(func, blk, XIR_FNEG, XR_REP_F64, neg1, XIR_NONE);
    assert(same_ref(neg2, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Test: Conversion Roundtrip ========== */

static void test_f2i_i2f_roundtrip(void) {
    fprintf(stderr, "  test_f2i_i2f_roundtrip...");
    XirFunc *func; XirBlock *blk;
    setup(&func, &blk);

    XirRef x = make_i64(func, blk, 42);

    /* I2F(x) → intermediate */
    XirRef i2f = xir_fold_emit(func, blk, XIR_I2F, XR_REP_F64, x, XIR_NONE);
    assert(!same_ref(i2f, x));

    /* F2I(I2F(x)) → x */
    XirRef f2i = xir_fold_emit(func, blk, XIR_F2I, XR_REP_I64, i2f, XIR_NONE);
    assert(same_ref(f2i, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_i2f_f2i_roundtrip(void) {
    fprintf(stderr, "  test_i2f_f2i_roundtrip...");
    XirFunc *func; XirBlock *blk;
    setup(&func, &blk);

    XirRef x = make_f64(func, blk, 3.14);

    /* F2I(x) → intermediate */
    XirRef f2i = xir_fold_emit(func, blk, XIR_F2I, XR_REP_I64, x, XIR_NONE);
    assert(!same_ref(f2i, x));

    /* I2F(F2I(x)) → x */
    XirRef i2f = xir_fold_emit(func, blk, XIR_I2F, XR_REP_F64, f2i, XIR_NONE);
    assert(same_ref(i2f, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Test: Constant Folding (i64) ========== */

static void test_const_fold_i64(void) {
    fprintf(stderr, "  test_const_fold_i64...");
    XirFunc *func; XirBlock *blk;
    setup(&func, &blk);

    XirRef a = make_i64(func, blk, 10);
    XirRef b = make_i64(func, blk, 3);

    /* 10 + 3 → 13 */
    XirRef r = xir_fold_emit(func, blk, XIR_ADD, XR_REP_I64, a, b);
    assert(get_i64_val(func, r) == 13);

    /* 10 - 3 → 7 */
    r = xir_fold_emit(func, blk, XIR_SUB, XR_REP_I64, a, b);
    assert(get_i64_val(func, r) == 7);

    /* 10 * 3 → 30 */
    r = xir_fold_emit(func, blk, XIR_MUL, XR_REP_I64, a, b);
    assert(get_i64_val(func, r) == 30);

    /* 10 / 3 → 3 */
    r = xir_fold_emit(func, blk, XIR_DIV, XR_REP_I64, a, b);
    assert(get_i64_val(func, r) == 3);

    /* 10 % 3 → 1 */
    r = xir_fold_emit(func, blk, XIR_MOD, XR_REP_I64, a, b);
    assert(get_i64_val(func, r) == 1);

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_const_fold_i64_bitwise(void) {
    fprintf(stderr, "  test_const_fold_i64_bitwise...");
    XirFunc *func; XirBlock *blk;
    setup(&func, &blk);

    XirRef a = make_i64(func, blk, 0xFF);
    XirRef b = make_i64(func, blk, 0x0F);

    /* 0xFF & 0x0F → 0x0F */
    XirRef r = xir_fold_emit(func, blk, XIR_AND, XR_REP_I64, a, b);
    assert(get_i64_val(func, r) == 0x0F);

    /* 0xFF | 0x0F → 0xFF */
    r = xir_fold_emit(func, blk, XIR_OR, XR_REP_I64, a, b);
    assert(get_i64_val(func, r) == 0xFF);

    /* 0xFF ^ 0x0F → 0xF0 */
    r = xir_fold_emit(func, blk, XIR_XOR, XR_REP_I64, a, b);
    assert(get_i64_val(func, r) == 0xF0);

    /* 1 << 4 → 16 */
    XirRef one = make_i64(func, blk, 1);
    XirRef four = make_i64(func, blk, 4);
    r = xir_fold_emit(func, blk, XIR_SHL, XR_REP_I64, one, four);
    assert(get_i64_val(func, r) == 16);

    /* 16 >> 2 → 4 */
    XirRef sixteen = make_i64(func, blk, 16);
    XirRef two = make_i64(func, blk, 2);
    r = xir_fold_emit(func, blk, XIR_SHR, XR_REP_I64, sixteen, two);
    assert(get_i64_val(func, r) == 4);

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_const_fold_i64_cmp(void) {
    fprintf(stderr, "  test_const_fold_i64_cmp...");
    XirFunc *func; XirBlock *blk;
    setup(&func, &blk);

    XirRef a = make_i64(func, blk, 10);
    XirRef b = make_i64(func, blk, 20);

    /* 10 < 20 → 1 */
    XirRef r = xir_fold_emit(func, blk, XIR_LT, XR_REP_I64, a, b);
    assert(get_i64_val(func, r) == 1);

    /* 20 < 10 → 0 */
    r = xir_fold_emit(func, blk, XIR_LT, XR_REP_I64, b, a);
    assert(get_i64_val(func, r) == 0);

    /* 10 == 10 → 1 */
    r = xir_fold_emit(func, blk, XIR_EQ, XR_REP_I64, a, a);
    assert(get_i64_val(func, r) == 1);

    /* 10 != 20 → 1 */
    r = xir_fold_emit(func, blk, XIR_NE, XR_REP_I64, a, b);
    assert(get_i64_val(func, r) == 1);

    /* 10 <= 10 → 1 */
    r = xir_fold_emit(func, blk, XIR_LE, XR_REP_I64, a, a);
    assert(get_i64_val(func, r) == 1);

    /* 10 >= 20 → 0 */
    r = xir_fold_emit(func, blk, XIR_GE, XR_REP_I64, a, b);
    assert(get_i64_val(func, r) == 0);

    teardown(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Test: Constant Folding (f64) ========== */

static void test_const_fold_f64(void) {
    fprintf(stderr, "  test_const_fold_f64...");
    XirFunc *func; XirBlock *blk;
    setup(&func, &blk);

    XirRef a = make_f64(func, blk, 2.5);
    XirRef b = make_f64(func, blk, 1.5);

    /* 2.5 + 1.5 → 4.0 */
    XirRef r = xir_fold_emit(func, blk, XIR_FADD, XR_REP_F64, a, b);
    assert(get_f64_val(func, r) == 4.0);

    /* 2.5 - 1.5 → 1.0 */
    r = xir_fold_emit(func, blk, XIR_FSUB, XR_REP_F64, a, b);
    assert(get_f64_val(func, r) == 1.0);

    /* 2.5 * 1.5 → 3.75 */
    r = xir_fold_emit(func, blk, XIR_FMUL, XR_REP_F64, a, b);
    assert(get_f64_val(func, r) == 3.75);

    /* 3.0 / 1.5 → 2.0 */
    XirRef three = make_f64(func, blk, 3.0);
    r = xir_fold_emit(func, blk, XIR_FDIV, XR_REP_F64, three, b);
    assert(get_f64_val(func, r) == 2.0);

    teardown(func);
    fprintf(stderr, " PASS\n");
}

static void test_const_fold_f64_cmp(void) {
    fprintf(stderr, "  test_const_fold_f64_cmp...");
    XirFunc *func; XirBlock *blk;
    setup(&func, &blk);

    XirRef a = make_f64(func, blk, 1.0);
    XirRef b = make_f64(func, blk, 2.0);

    /* 1.0 < 2.0 → 1 */
    XirRef r = xir_fold_emit(func, blk, XIR_FLT, XR_REP_I64, a, b);
    assert(get_i64_val(func, r) == 1);

    /* 1.0 == 1.0 → 1 */
    r = xir_fold_emit(func, blk, XIR_FEQ, XR_REP_I64, a, a);
    assert(get_i64_val(func, r) == 1);

    /* 1.0 != 2.0 → 1 */
    r = xir_fold_emit(func, blk, XIR_FNE, XR_REP_I64, a, b);
    assert(get_i64_val(func, r) == 1);

    /* 2.0 <= 1.0 → 0 */
    r = xir_fold_emit(func, blk, XIR_FLE, XR_REP_I64, b, a);
    assert(get_i64_val(func, r) == 0);

    teardown(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Test: Float Identity ========== */

static void test_float_identity(void) {
    fprintf(stderr, "  test_float_identity...");
    XirFunc *func; XirBlock *blk;
    setup(&func, &blk);

    XirRef x = make_f64(func, blk, 3.14);
    XirRef zero = make_f64(func, blk, 0.0);
    XirRef one = make_f64(func, blk, 1.0);

    /* x + 0.0 → x */
    XirRef r = xir_fold_emit(func, blk, XIR_FADD, XR_REP_F64, x, zero);
    assert(same_ref(r, x));

    /* 0.0 + x → x */
    r = xir_fold_emit(func, blk, XIR_FADD, XR_REP_F64, zero, x);
    assert(same_ref(r, x));

    /* x - 0.0 → x */
    r = xir_fold_emit(func, blk, XIR_FSUB, XR_REP_F64, x, zero);
    assert(same_ref(r, x));

    /* x * 1.0 → x */
    r = xir_fold_emit(func, blk, XIR_FMUL, XR_REP_F64, x, one);
    assert(same_ref(r, x));

    /* 1.0 * x → x */
    r = xir_fold_emit(func, blk, XIR_FMUL, XR_REP_F64, one, x);
    assert(same_ref(r, x));

    /* x / 1.0 → x */
    r = xir_fold_emit(func, blk, XIR_FDIV, XR_REP_F64, x, one);
    assert(same_ref(r, x));

    teardown(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Test: No-fold (non-const operand) ========== */

static void test_no_fold_dynamic(void) {
    fprintf(stderr, "  test_no_fold_dynamic...");
    XirFunc *func; XirBlock *blk;
    setup(&func, &blk);

    /* Create a vreg that is NOT a CONST_I64 (simulate a param) */
    XirRef param = xir_new_vreg(func, XR_REP_I64);
    XirRef zero = make_i64(func, blk, 0);

    /* param + 0 → param (identity still works with right-const) */
    XirRef r = xir_fold_emit(func, blk, XIR_ADD, XR_REP_I64, param, zero);
    assert(same_ref(r, param));

    /* param + param → should NOT fold (not self-zero pattern, just new instruction) */
    /* Actually param + param with same vreg has no special rule for ADD */
    uint32_t nins_before = blk->nins;
    r = xir_fold_emit(func, blk, XIR_ADD, XR_REP_I64, param, param);
    assert(blk->nins == nins_before + 1);

    teardown(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Test: Div by zero not folded ========== */

static void test_div_by_zero_not_folded(void) {
    fprintf(stderr, "  test_div_by_zero_not_folded...");
    XirFunc *func; XirBlock *blk;
    setup(&func, &blk);

    XirRef a = make_i64(func, blk, 10);
    XirRef zero = make_i64(func, blk, 0);

    /* 10 / 0 → NOT folded (emits normal instruction) */
    uint32_t nins_before = blk->nins;
    XirRef r = xir_fold_emit(func, blk, XIR_DIV, XR_REP_I64, a, zero);
    assert(blk->nins == nins_before + 1);
    (void)r;

    /* 10 % 0 → NOT folded */
    nins_before = blk->nins;
    r = xir_fold_emit(func, blk, XIR_MOD, XR_REP_I64, a, zero);
    assert(blk->nins == nins_before + 1);

    teardown(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Main ========== */

int main(void) {
    fprintf(stderr, "=== test_xir_fold ===\n");

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
