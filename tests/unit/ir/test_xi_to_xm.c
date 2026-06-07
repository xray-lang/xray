/*
 * test_xi_to_xm.c - Unit tests for Xi IR to Xm lowering
 *
 * Validates that xi_to_xm_lower correctly translates Xi SSA
 * to Xm SSA for arithmetic, comparison, branching, and phi nodes.
 */

#include "../../../src/ir/xi.h"
#include "../../../src/jit/xi_to_xm.h"
#include "../../../src/jit/xm.h"
#include "../../../src/jit/xm_ops.h"
#include "../../../src/jit/xm_jit_runtime.h"
#include "../../../src/jit/xm_helper_table.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"

#include <stdio.h>
#include <assert.h>
#include <string.h>

/* Minimal XrType stubs */
static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_float = {.kind = XR_KIND_FLOAT, .id = 2, .frozen = true};
static XrType stub_bool = {.kind = XR_KIND_BOOL, .id = 3, .frozen = true};
static XrType stub_void = {.kind = XR_KIND_UNIT, .id = 6, .frozen = true};

static int tests_passed = 0;

#define TEST(name)                                                                                 \
    static void test_##name(void);                                                                 \
    static void run_##name(void) {                                                                 \
        printf("--- " #name " ---\n");                                                             \
        test_##name();                                                                             \
        printf("  PASS\n");                                                                        \
        tests_passed++;                                                                            \
    }                                                                                              \
    static void test_##name(void)

/* Helper: create function with sealed entry block */
static XiFunc *make_func(const char *name, XrType *ret) {
    XiFunc *f = xi_func_new(name, ret);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    return f;
}

static void register_func_params(XiFunc *f, XiValue **params, uint16_t nparams) {
    f->params = xr_calloc(nparams, sizeof(XiValue *));
    assert(f->params != NULL);
    f->nparams = nparams;
    for (uint16_t i = 0; i < nparams; i++)
        f->params[i] = params[i];
}

/* ========== Tests ========== */

TEST(lower_const_int) {
    /* fn() { return 42 } */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *c = xi_const_int(f, entry, 42, &stub_int);
    xi_block_set_return(entry, c);

    XmFunc *xm = xi_to_xm_lower(f, NULL, NULL, NULL, NULL);
    assert(xm != NULL && "lowering should succeed");
    assert(xm->nblk >= 1 && "should have at least 1 block");

    xm_func_destroy(xm);
    xi_func_free(f);
}

TEST(lower_const_float) {
    XiFunc *f = make_func("test", &stub_float);
    XiBlock *entry = f->entry;

    XiValue *c = xi_const_float(f, entry, 3.14, &stub_float);
    xi_block_set_return(entry, c);

    XmFunc *xm = xi_to_xm_lower(f, NULL, NULL, NULL, NULL);
    assert(xm != NULL && "lowering should succeed");

    xm_func_destroy(xm);
    xi_func_free(f);
}

TEST(lower_add_int) {
    /* fn(a: int, b: int) { return a + b } */
    XiFunc *f = make_func("add", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiValue *b = xi_param(f, entry, 1, &stub_int);
    XiValue *sum = xi_binary(f, entry, XI_ADD, &stub_int, a, b);
    xi_block_set_return(entry, sum);

    XmFunc *xm = xi_to_xm_lower(f, NULL, NULL, NULL, NULL);
    assert(xm != NULL && "lowering should succeed");

    /* Verify: should have at least an ADD instruction */
    XmBlock *blk0 = xm->blocks[0];
    bool found_add = false;
    for (uint32_t i = 0; i < blk0->nins; i++) {
        if (blk0->ins[i].op == XM_ADD)
            found_add = true;
    }
    assert(found_add && "should contain XM_ADD instruction");

    xm_func_destroy(xm);
    xi_func_free(f);
}

TEST(lower_add_float) {
    XiFunc *f = make_func("fadd", &stub_float);
    XiBlock *entry = f->entry;

    XiValue *a = xi_param(f, entry, 0, &stub_float);
    XiValue *b = xi_param(f, entry, 1, &stub_float);
    XiValue *sum = xi_binary(f, entry, XI_ADD, &stub_float, a, b);
    xi_block_set_return(entry, sum);

    XmFunc *xm = xi_to_xm_lower(f, NULL, NULL, NULL, NULL);
    assert(xm != NULL);

    XmBlock *blk0 = xm->blocks[0];
    bool found_fadd = false;
    for (uint32_t i = 0; i < blk0->nins; i++) {
        if (blk0->ins[i].op == XM_FADD)
            found_fadd = true;
    }
    assert(found_fadd && "should contain XM_FADD instruction");

    xm_func_destroy(xm);
    xi_func_free(f);
}

TEST(lower_comparison) {
    /* fn(a: int, b: int) -> bool { return a < b } */
    XiFunc *f = make_func("lt", &stub_bool);
    XiBlock *entry = f->entry;

    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiValue *b = xi_param(f, entry, 1, &stub_int);
    XiValue *cmp = xi_binary(f, entry, XI_LT, &stub_bool, a, b);
    xi_block_set_return(entry, cmp);

    XmFunc *xm = xi_to_xm_lower(f, NULL, NULL, NULL, NULL);
    assert(xm != NULL);

    XmBlock *blk0 = xm->blocks[0];
    bool found_lt = false;
    for (uint32_t i = 0; i < blk0->nins; i++) {
        if (blk0->ins[i].op == XM_LT)
            found_lt = true;
    }
    assert(found_lt && "should contain XM_LT instruction");

    xm_func_destroy(xm);
    xi_func_free(f);
}

static void check_lower_comparison_variant(XiOp xi_op, XmOp expected_xm_op) {
    XiFunc *f = make_func("cmp_variant", &stub_bool);
    XiBlock *entry = f->entry;

    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiValue *b = xi_param(f, entry, 1, &stub_int);
    XiValue *cmp = xi_binary(f, entry, xi_op, &stub_bool, a, b);
    xi_block_set_return(entry, cmp);

    XmFunc *xm = xi_to_xm_lower(f, NULL, NULL, NULL, NULL);
    assert(xm != NULL);

    XmBlock *blk0 = xm->blocks[0];
    bool found = false;
    for (uint32_t i = 0; i < blk0->nins; i++) {
        if (blk0->ins[i].op == expected_xm_op)
            found = true;
    }
    assert(found && "comparison variant should lower through generated dispatch");

    xm_func_destroy(xm);
    xi_func_free(f);
}

TEST(lower_comparison_variants) {
    check_lower_comparison_variant(XI_NE, XM_NE);
    check_lower_comparison_variant(XI_EQ_STRICT, XM_EQ);
    check_lower_comparison_variant(XI_NE_STRICT, XM_NE);
    check_lower_comparison_variant(XI_GT, XM_LT);
    check_lower_comparison_variant(XI_GE, XM_LE);
}

static void check_lower_binary_variant(XiOp xi_op, XmOp expected_xm_op) {
    XiFunc *f = make_func("binary_variant", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiValue *b = xi_param(f, entry, 1, &stub_int);
    XiValue *params[2] = {a, b};
    register_func_params(f, params, 2);
    XiValue *bin = xi_binary(f, entry, xi_op, &stub_int, a, b);
    xi_block_set_return(entry, bin);

    XmFunc *xm = xi_to_xm_lower(f, NULL, NULL, NULL, NULL);
    assert(xm != NULL);

    XmBlock *blk0 = xm->blocks[0];
    bool found = false;
    for (uint32_t i = 0; i < blk0->nins; i++) {
        if (blk0->ins[i].op == expected_xm_op)
            found = true;
    }
    assert(found && "binary variant should lower through generated dispatch");

    xm_func_destroy(xm);
    xi_func_free(f);
}

TEST(lower_bitwise_variants) {
    check_lower_binary_variant(XI_BAND, XM_AND);
    check_lower_binary_variant(XI_BOR, XM_OR);
    check_lower_binary_variant(XI_BXOR, XM_XOR);
    check_lower_binary_variant(XI_SHL, XM_SHL);
    check_lower_binary_variant(XI_SHR, XM_SHR);
}

TEST(lower_div_mod_variants) {
    check_lower_binary_variant(XI_DIV, XM_DIV);
    check_lower_binary_variant(XI_MOD, XM_MOD);
}

TEST(lower_if_branch) {
    /* fn(c: bool) -> int { if c { return 1 } else { return 2 } } */
    XiFunc *f = make_func("branch", &stub_int);
    XiBlock *entry = f->entry;
    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);
    then_blk->sealed = true;
    else_blk->sealed = true;

    XiValue *cond = xi_param(f, entry, 0, &stub_bool);

    /* Entry branches on cond */
    xi_block_set_if(entry, cond, then_blk, else_blk);

    /* Then: return 1 */
    XiValue *c1 = xi_const_int(f, then_blk, 1, &stub_int);
    xi_block_set_return(then_blk, c1);

    /* Else: return 2 */
    XiValue *c2 = xi_const_int(f, else_blk, 2, &stub_int);
    xi_block_set_return(else_blk, c2);

    XmFunc *xm = xi_to_xm_lower(f, NULL, NULL, NULL, NULL);
    assert(xm != NULL);
    assert(xm->nblk == 3 && "should have entry + then + else blocks");

    /* Entry block should have a branch terminator */
    XmBlock *xm_entry = xm->blocks[0];
    assert(xm_entry->jmp.type == XM_JMP_BR && "entry should be conditional branch");
    assert(xm_entry->s1 != NULL && "should have then successor");
    assert(xm_entry->s2 != NULL && "should have else successor");

    xm_func_destroy(xm);
    xi_func_free(f);
}

TEST(lower_phi) {
    /* fn(c: bool) -> int {
     *   if c { x = 10 } else { x = 20 }
     *   return x  // phi(10, 20)
     * } */
    XiFunc *f = make_func("phi_test", &stub_int);
    XiBlock *entry = f->entry;
    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);
    XiBlock *merge = xi_block_new(f);
    then_blk->sealed = true;
    else_blk->sealed = true;

    XiValue *cond = xi_param(f, entry, 0, &stub_bool);

    /* Entry → if(cond) then else */
    xi_block_set_if(entry, cond, then_blk, else_blk);

    /* Then: x = 10, goto merge */
    XiValue *c10 = xi_const_int(f, then_blk, 10, &stub_int);
    xi_block_set_jump(then_blk, merge);

    /* Else: x = 20, goto merge */
    XiValue *c20 = xi_const_int(f, else_blk, 20, &stub_int);
    xi_block_set_jump(else_blk, merge);

    /* Merge: phi(c10, c20), return phi */
    merge->sealed = true;
    XiPhi *phi = xi_phi_new(f, merge, &stub_int, merge->npreds);
    phi->value.args[0] = c10;
    phi->value.args[1] = c20;
    xi_block_set_return(merge, &phi->value);

    XmFunc *xm = xi_to_xm_lower(f, NULL, NULL, NULL, NULL);
    assert(xm != NULL);
    assert(xm->nblk == 4 && "should have 4 blocks");

    /* Merge block should have a phi node */
    XmBlock *xm_merge = xm->blocks[3];
    (void) xm_merge;
    assert(xm_merge->phis != NULL && "merge block should have phi");

    xm_func_destroy(xm);
    xi_func_free(f);
}

TEST(lower_neg_unary) {
    XiFunc *f = make_func("neg", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiValue *neg = xi_unary(f, entry, XI_NEG, &stub_int, a);
    xi_block_set_return(entry, neg);

    XmFunc *xm = xi_to_xm_lower(f, NULL, NULL, NULL, NULL);
    assert(xm != NULL);

    XmBlock *blk0 = xm->blocks[0];
    bool found_neg = false;
    for (uint32_t i = 0; i < blk0->nins; i++) {
        if (blk0->ins[i].op == XM_NEG)
            found_neg = true;
    }
    assert(found_neg && "should contain XM_NEG instruction");

    xm_func_destroy(xm);
    xi_func_free(f);
}

static void check_lower_unary_variant(XiOp xi_op, XmOp expected_xm_op) {
    XiFunc *f = make_func("unary_variant", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiValue *params[1] = {a};
    register_func_params(f, params, 1);
    XiValue *un = xi_unary(f, entry, xi_op, &stub_int, a);
    xi_block_set_return(entry, un);

    XmFunc *xm = xi_to_xm_lower(f, NULL, NULL, NULL, NULL);
    assert(xm != NULL);

    XmBlock *blk0 = xm->blocks[0];
    bool found = false;
    for (uint32_t i = 0; i < blk0->nins; i++) {
        if (blk0->ins[i].op == expected_xm_op)
            found = true;
    }
    assert(found && "unary variant should lower through generated dispatch");

    xm_func_destroy(xm);
    xi_func_free(f);
}

TEST(lower_unary_variants) {
    check_lower_unary_variant(XI_NEG, XM_NEG);
    check_lower_unary_variant(XI_BNOT, XM_NOT);
}

TEST(lower_logical_not) {
    XiFunc *f = make_func("logical_not", &stub_bool);
    XiBlock *entry = f->entry;

    XiValue *a = xi_param(f, entry, 0, &stub_bool);
    XiValue *params[1] = {a};
    register_func_params(f, params, 1);
    XiValue *not_a = xi_unary(f, entry, XI_NOT, &stub_bool, a);
    xi_block_set_return(entry, not_a);

    XmFunc *xm = xi_to_xm_lower(f, NULL, NULL, NULL, NULL);
    assert(xm != NULL);

    XmBlock *blk0 = xm->blocks[0];
    bool found_eq = false;
    bool found_bitwise_not = false;
    for (uint32_t i = 0; i < blk0->nins; i++) {
        if (blk0->ins[i].op == XM_EQ)
            found_eq = true;
        if (blk0->ins[i].op == XM_NOT)
            found_bitwise_not = true;
    }
    assert(found_eq && "logical not should compare against false");
    assert(!found_bitwise_not && "logical not must not lower to bitwise NOT");

    xm_func_destroy(xm);
    xi_func_free(f);
}

TEST(lower_isnull) {
    XiFunc *f = make_func("isnull", &stub_bool);
    XiBlock *entry = f->entry;

    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiValue *params[1] = {a};
    register_func_params(f, params, 1);
    XiValue *isnull = xi_value_new(f, entry, XI_ISNULL, &stub_bool, 1);
    isnull->args[0] = a;
    xi_block_set_return(entry, isnull);

    XmFunc *xm = xi_to_xm_lower(f, NULL, NULL, NULL, NULL);
    assert(xm != NULL);

    XmBlock *blk0 = xm->blocks[0];
    bool found = false;
    for (uint32_t i = 0; i < blk0->nins; i++) {
        if (blk0->ins[i].op == XM_RT_ISNULL)
            found = true;
    }
    assert(found && "isnull should lower through generated dispatch");

    xm_func_destroy(xm);
    xi_func_free(f);
}

static void check_width_variant(XiOp xi_op, XrType *result_type, XmOp expected_op_a,
                                XmOp expected_op_b, bool expect_identity) {
    XiFunc *f = make_func("width_variant", result_type);
    XiBlock *entry = f->entry;
    XrType *param_type = result_type->kind == XR_KIND_FLOAT ? &stub_float : &stub_int;

    XiValue *a = xi_param(f, entry, 0, param_type);
    XiValue *params[1] = {a};
    register_func_params(f, params, 1);
    XiValue *cast = xi_unary(f, entry, xi_op, result_type, a);
    xi_block_set_return(entry, cast);

    XmFunc *xm = xi_to_xm_lower(f, NULL, NULL, NULL, NULL);
    assert(xm != NULL);

    XmBlock *blk0 = xm->blocks[0];
    bool found_a = false;
    bool found_b = false;
    for (uint32_t i = 0; i < blk0->nins; i++) {
        if (blk0->ins[i].op == expected_op_a)
            found_a = true;
        if (blk0->ins[i].op == expected_op_b)
            found_b = true;
    }
    if (expect_identity) {
        assert(!found_a && !found_b && "f32 width cast is a JIT identity");
    } else {
        assert(found_a && "width variant should emit expected first op");
        assert(found_b && "width variant should emit expected second op");
    }

    xm_func_destroy(xm);
    xi_func_free(f);
}

TEST(lower_width_variants) {
    check_width_variant(XI_NARROW_I8, &stub_int, XM_SHL, XM_SHR, false);
    check_width_variant(XI_NARROW_U8, &stub_int, XM_AND, XM_AND, false);
    check_width_variant(XI_NARROW_I16, &stub_int, XM_SHL, XM_SHR, false);
    check_width_variant(XI_NARROW_U16, &stub_int, XM_AND, XM_AND, false);
    check_width_variant(XI_NARROW_I32, &stub_int, XM_SHL, XM_SHR, false);
    check_width_variant(XI_NARROW_U32, &stub_int, XM_AND, XM_AND, false);
    check_width_variant(XI_NARROW_F32, &stub_float, XM_F2I, XM_I2F, true);
    check_width_variant(XI_WIDEN_I8, &stub_int, XM_SHL, XM_SHR, false);
    check_width_variant(XI_WIDEN_U8, &stub_int, XM_AND, XM_AND, false);
    check_width_variant(XI_WIDEN_I16, &stub_int, XM_SHL, XM_SHR, false);
    check_width_variant(XI_WIDEN_U16, &stub_int, XM_AND, XM_AND, false);
    check_width_variant(XI_WIDEN_I32, &stub_int, XM_SHL, XM_SHR, false);
    check_width_variant(XI_WIDEN_U32, &stub_int, XM_AND, XM_AND, false);
    check_width_variant(XI_WIDEN_F32, &stub_float, XM_F2I, XM_I2F, true);
}

TEST(lower_conversion_variants) {
    XiFunc *convert_f = make_func("convert_f2i", &stub_int);
    convert_f->stage = XI_STAGE_REPPED;
    XiBlock *convert_entry = convert_f->entry;
    XiValue *convert_arg = xi_param(convert_f, convert_entry, 0, &stub_float);
    convert_arg->rep = XR_REP_F64;
    XiValue *convert_params[1] = {convert_arg};
    register_func_params(convert_f, convert_params, 1);
    XiValue *converted = xi_unary(convert_f, convert_entry, XI_CONVERT, &stub_int, convert_arg);
    converted->rep = XR_REP_I64;
    xi_block_set_return(convert_entry, converted);

    XmFunc *convert_xm = xi_to_xm_lower(convert_f, NULL, NULL, NULL, NULL);
    assert(convert_xm != NULL);
    bool found_f2i = false;
    for (uint32_t i = 0; i < convert_xm->blocks[0]->nins; i++) {
        if (convert_xm->blocks[0]->ins[i].op == XM_F2I)
            found_f2i = true;
    }
    assert(found_f2i && "convert float->int should lower through generated dispatch");
    xm_func_destroy(convert_xm);
    xi_func_free(convert_f);

    XiFunc *box_f = make_func("box_f64", &stub_float);
    box_f->stage = XI_STAGE_REPPED;
    XiBlock *box_entry = box_f->entry;
    XiValue *box_arg = xi_param(box_f, box_entry, 0, &stub_float);
    box_arg->rep = XR_REP_F64;
    XiValue *box_params[1] = {box_arg};
    register_func_params(box_f, box_params, 1);
    XiValue *boxed = xi_unary(box_f, box_entry, XI_BOX, &stub_float, box_arg);
    boxed->rep = XR_REP_TAGGED;
    xi_block_set_return(box_entry, boxed);

    XmFunc *box_xm = xi_to_xm_lower(box_f, NULL, NULL, NULL, NULL);
    assert(box_xm != NULL);
    bool found_box = false;
    for (uint32_t i = 0; i < box_xm->blocks[0]->nins; i++) {
        if (box_xm->blocks[0]->ins[i].op == XM_BOX_F64)
            found_box = true;
    }
    assert(found_box && "box should lower through generated dispatch");
    xm_func_destroy(box_xm);
    xi_func_free(box_f);

    XiFunc *unbox_f = make_func("unbox_i64", &stub_int);
    unbox_f->stage = XI_STAGE_REPPED;
    XiBlock *unbox_entry = unbox_f->entry;
    XiValue *unbox_arg = xi_param(unbox_f, unbox_entry, 0, &stub_int);
    unbox_arg->rep = XR_REP_TAGGED;
    XiValue *unbox_params[1] = {unbox_arg};
    register_func_params(unbox_f, unbox_params, 1);
    XiValue *unboxed = xi_unary(unbox_f, unbox_entry, XI_UNBOX, &stub_int, unbox_arg);
    unboxed->rep = XR_REP_I64;
    xi_block_set_return(unbox_entry, unboxed);

    XmFunc *unbox_xm = xi_to_xm_lower(unbox_f, NULL, NULL, NULL, NULL);
    assert(unbox_xm != NULL);
    bool found_unbox = false;
    for (uint32_t i = 0; i < unbox_xm->blocks[0]->nins; i++) {
        if (unbox_xm->blocks[0]->ins[i].op == XM_UNBOX_I64)
            found_unbox = true;
    }
    assert(found_unbox && "unbox should lower through generated dispatch");
    xm_func_destroy(unbox_xm);
    xi_func_free(unbox_f);
}

TEST(lower_void_return) {
    XiFunc *f = make_func("void_fn", &stub_void);
    XiBlock *entry = f->entry;
    xi_block_set_return(entry, NULL);

    XmFunc *xm = xi_to_xm_lower(f, NULL, NULL, NULL, NULL);
    assert(xm != NULL);

    XmBlock *blk0 = xm->blocks[0];
    assert(blk0->jmp.type == XM_JMP_RET && "should have return terminator");

    xm_func_destroy(xm);
    xi_func_free(f);
}

TEST(lower_call) {
    /* fn(callee: fn, a: int) { return callee(a) } */
    XiFunc *f = make_func("call_test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *callee = xi_param(f, entry, 0, &stub_int);
    XiValue *arg = xi_param(f, entry, 1, &stub_int);

    /* XI_CALL: args[0]=callee, args[1]=param */
    XiValue *call = xi_value_new(f, entry, XI_CALL, &stub_int, 2);
    call->args[0] = callee;
    call->args[1] = arg;
    xi_block_set_return(entry, call);

    XmFunc *xm = xi_to_xm_lower(f, NULL, NULL, NULL, NULL);
    assert(xm != NULL && "call lowering should succeed");

    /* Verify: should have a CALL_DIRECT instruction */
    XmBlock *blk0 = xm->blocks[0];
    bool found_call = false;
    for (uint32_t i = 0; i < blk0->nins; i++) {
        if (blk0->ins[i].op == XM_CALL_DIRECT)
            found_call = true;
    }
    assert(found_call && "should contain XM_CALL_DIRECT");

    xm_func_destroy(xm);
    xi_func_free(f);
}

TEST(lower_print) {
    /* fn(x: int) { print(x) } */
    XiFunc *f = make_func("print_test", &stub_void);
    XiBlock *entry = f->entry;

    XiValue *x = xi_param(f, entry, 0, &stub_int);

    XiValue *pr = xi_value_new(f, entry, XI_PRINT, &stub_void, 1);
    pr->args[0] = x;
    pr->aux_int = 2; /* newline flag */
    xi_block_set_return(entry, NULL);

    XmFunc *xm = xi_to_xm_lower(f, NULL, NULL, NULL, NULL);
    assert(xm != NULL && "print lowering should succeed");

    XmBlock *blk0 = xm->blocks[0];
    bool found_print = false;
    for (uint32_t i = 0; i < blk0->nins; i++) {
        assert(blk0->ins[i].op != XM_RT_PRINT);
        if (blk0->ins[i].op == XM_CALL_C && blk0->ins[i].args[0] != XM_NONE) {
            assert(xm_ref_is_const(blk0->ins[i].args[0]));
            assert(xm_ref_is_const(blk0->ins[i].args[1]));
            uint32_t fn_ci = XM_REF_INDEX(blk0->ins[i].args[0]);
            uint32_t extra_ci = XM_REF_INDEX(blk0->ins[i].args[1]);
            assert(fn_ci < xm->nconst);
            assert(extra_ci < xm->nconst);
            assert(xm->consts[fn_ci].val.ptr == (void *) xr_jit_print);
            assert(xm->consts[extra_ci].val.i64 == 1);
            assert(blk0->ins[i].rep == XR_REP_I64);
            assert(blk0->ins[i].ctype.kind == xm_helper_type_kind(XM_HELPER_print));
            uint32_t vi = XM_REF_INDEX(blk0->ins[i].dst);
            assert(vi < xm->nvreg);
            assert(xm->vregs[vi].call_nargs == 1);
            found_print = true;
        }
    }
    assert(found_print && "should contain CALL_C to xr_jit_print");

    xm_func_destroy(xm);
    xi_func_free(f);
}

TEST(lower_throw) {
    XiFunc *f = make_func("throw_test", &stub_void);
    XiBlock *entry = f->entry;

    XiValue *x = xi_param(f, entry, 0, &stub_int);

    XiValue *thr = xi_value_new(f, entry, XI_THROW, &stub_void, 1);
    thr->args[0] = x;
    xi_block_set_return(entry, NULL);

    XmFunc *xm = xi_to_xm_lower(f, NULL, NULL, NULL, NULL);
    assert(xm != NULL && "throw lowering should succeed");

    XmBlock *blk0 = xm->blocks[0];
    bool found_throw = false;
    for (uint32_t i = 0; i < blk0->nins; i++) {
        if (blk0->ins[i].op == XM_CALL_C && blk0->ins[i].args[0] != XM_NONE) {
            assert(xm_ref_is_const(blk0->ins[i].args[0]));
            assert(xm_ref_is_const(blk0->ins[i].args[1]));
            uint32_t fn_ci = XM_REF_INDEX(blk0->ins[i].args[0]);
            uint32_t extra_ci = XM_REF_INDEX(blk0->ins[i].args[1]);
            assert(fn_ci < xm->nconst);
            assert(extra_ci < xm->nconst);
            assert(xm->consts[fn_ci].val.ptr == (void *) xr_jit_throw);
            assert(xm->consts[extra_ci].val.i64 == 0);
            assert((blk0->ins[i].flags & XM_FLAG_MAY_THROW) != 0);
            assert(blk0->ins[i].ctype.kind == xm_helper_type_kind(XM_HELPER_throw));
            uint32_t vi = XM_REF_INDEX(blk0->ins[i].dst);
            assert(vi < xm->nvreg);
            assert(xm->vregs[vi].call_nargs == 1);
            found_throw = true;
        }
    }
    assert(found_throw && "should contain CALL_C to xr_jit_throw");

    xm_func_destroy(xm);
    xi_func_free(f);
}

TEST(lower_shared_var) {
    /* fn() { var x = 42; return x } (shared) */
    XiFunc *f = make_func("shared_test", &stub_int);
    XiBlock *entry = f->entry;

    /* SET_SHARED slot 0 = 42 */
    XiValue *c42 = xi_const_int(f, entry, 42, &stub_int);
    XiValue *set = xi_value_new(f, entry, XI_SET_SHARED, &stub_int, 1);
    set->args[0] = c42;
    set->aux_int = 0;

    /* GET_SHARED slot 0 */
    XiValue *get = xi_value_new(f, entry, XI_GET_SHARED, &stub_int, 0);
    get->aux_int = 0;
    xi_block_set_return(entry, get);

    XmFunc *xm = xi_to_xm_lower(f, NULL, NULL, NULL, NULL);
    assert(xm != NULL && "shared var lowering should succeed");

    /* Both GET_SHARED and SET_SHARED lower through CALL_C bridges
     * (xr_jit_get_shared / xr_jit_set_shared). XM_STORE with a const
     * base would land at A64_XZR which encodes as SP in ARM64
     * memory ops, corrupting the saved frame pointer. */
    XmBlock *blk0 = xm->blocks[0];
    int call_c_count = 0;
    for (uint32_t i = 0; i < blk0->nins; i++) {
        if (blk0->ins[i].op == XM_CALL_C)
            call_c_count++;
        assert(blk0->ins[i].op != XM_STORE &&
               "SET_SHARED must not lower to XM_STORE (writes via CALL_C)");
    }
    assert(call_c_count >= 2 && "should contain XM_CALL_C for both GET_SHARED and SET_SHARED");

    xm_func_destroy(xm);
    xi_func_free(f);
}

TEST(lower_load_field) {
    /* fn(obj: any) -> int { return obj.field_0 } */
    XiFunc *f = make_func("load_field", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *obj = xi_param(f, entry, 0, &stub_int);
    XiValue *load = xi_value_new(f, entry, XI_LOAD_FIELD, &stub_int, 1);
    load->args[0] = obj;
    load->aux_int = 0; /* field index */
    xi_block_set_return(entry, load);

    XmFunc *xm = xi_to_xm_lower(f, NULL, NULL, NULL, NULL);
    assert(xm != NULL);

    XmBlock *blk0 = xm->blocks[0];
    bool found = false;
    for (uint32_t i = 0; i < blk0->nins; i++) {
        if (blk0->ins[i].op == XM_LOAD_FIELD)
            found = true;
    }
    assert(found && "should contain XM_LOAD_FIELD");

    xm_func_destroy(xm);
    xi_func_free(f);
}

TEST(lower_index_get) {
    /* fn(arr: any, idx: int) -> int { return arr[idx] } */
    XiFunc *f = make_func("idx_get", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *arr = xi_param(f, entry, 0, &stub_int);
    XiValue *idx = xi_param(f, entry, 1, &stub_int);
    XiValue *get = xi_value_new(f, entry, XI_INDEX_GET, &stub_int, 2);
    get->args[0] = arr;
    get->args[1] = idx;
    xi_block_set_return(entry, get);

    XmFunc *xm = xi_to_xm_lower(f, NULL, NULL, NULL, NULL);
    assert(xm != NULL);

    XmBlock *blk0 = xm->blocks[0];
    bool found = false;
    for (uint32_t i = 0; i < blk0->nins; i++) {
        if (blk0->ins[i].op == XM_CALL_C && blk0->ins[i].args[0] != XM_NONE) {
            uint32_t ci = XM_REF_INDEX(blk0->ins[i].args[0]);
            assert(ci < xm->nconst);
            assert(xm->consts[ci].val.ptr == (void *) xr_jit_index_get);
            assert((blk0->ins[i].flags & XM_FLAG_MAY_GC) != 0);
            assert((blk0->ins[i].flags & XM_FLAG_SAFEPOINT) != 0);
            assert(blk0->ins[i].ctype.kind == xm_helper_type_kind(XM_HELPER_index_get));
            uint32_t vi = XM_REF_INDEX(blk0->ins[i].dst);
            assert(vi < xm->nvreg);
            assert(xm->vregs[vi].call_nargs == 2);
            found = true;
        }
    }
    assert(found && "should contain CALL_C to xr_jit_index_get");

    xm_func_destroy(xm);
    xi_func_free(f);
}

TEST(lower_array_new) {
    /* fn() -> any { return [] } */
    XiFunc *f = make_func("arr_new", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *arr = xi_value_new(f, entry, XI_ARRAY_NEW, &stub_int, 0);
    xi_block_set_return(entry, arr);

    XmFunc *xm = xi_to_xm_lower(f, NULL, NULL, NULL, NULL);
    assert(xm != NULL);

    XmBlock *blk0 = xm->blocks[0];
    bool found = false;
    for (uint32_t i = 0; i < blk0->nins; i++) {
        if (blk0->ins[i].op == XM_RT_ARRAY_NEW)
            found = true;
    }
    assert(found && "should contain XM_RT_ARRAY_NEW");

    xm_func_destroy(xm);
    xi_func_free(f);
}

TEST(reject_multi_ret_with_extra_results) {
    XiFunc *f = make_func("multi_ret_extra", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *a = xi_const_int(f, entry, 1, &stub_int);
    XiValue *b = xi_const_int(f, entry, 2, &stub_int);
    XiValue *ret = xi_value_new(f, entry, XI_MULTI_RET, &stub_int, 2);
    ret->args[0] = a;
    ret->args[1] = b;
    xi_block_set_return(entry, ret);

    XmFunc *xm = xi_to_xm_lower(f, NULL, NULL, NULL, NULL);
    assert(xm == NULL && "multi-return with extra values should not lower to Xm yet");

    xi_func_free(f);
}

TEST(reject_extract_extra_result) {
    XiFunc *f = make_func("extract_extra", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *callee = xi_param(f, entry, 0, &stub_int);
    XiValue *arg = xi_param(f, entry, 1, &stub_int);
    XiValue *call = xi_value_new(f, entry, XI_CALL, &stub_int, 2);
    call->args[0] = callee;
    call->args[1] = arg;

    XiValue *extract = xi_value_new(f, entry, XI_EXTRACT, &stub_int, 1);
    extract->args[0] = call;
    extract->aux_int = 1;
    xi_block_set_return(entry, extract);

    XmFunc *xm = xi_to_xm_lower(f, NULL, NULL, NULL, NULL);
    assert(xm == NULL && "extracting secondary call results should not lower to Xm yet");

    xi_func_free(f);
}

TEST(reject_bytes_memory_ops_until_jit_driver_exists) {
    XiFunc *f = make_func("bytes_mem", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *bytes = xi_param(f, entry, 0, &stub_int);
    XiValue *offset = xi_const_int(f, entry, 0, &stub_int);
    XiValue *load = xi_value_new(f, entry, XI_BYTES_LOAD_U32_LE, &stub_int, 2);
    load->args[0] = bytes;
    load->args[1] = offset;
    xi_block_set_return(entry, load);

    XmFunc *xm = xi_to_xm_lower(f, NULL, NULL, NULL, NULL);
    assert(xm == NULL && "Bytes memory ops must explicitly reject JIT until a driver exists");

    xi_func_free(f);
}

/* ========== Main ========== */

int main(void) {
    printf("=== Xi-to-Xm Lowering Unit Tests ===\n\n");

    run_lower_const_int();
    run_lower_const_float();
    run_lower_add_int();
    run_lower_add_float();
    run_lower_comparison();
    run_lower_comparison_variants();
    run_lower_bitwise_variants();
    run_lower_div_mod_variants();
    run_lower_if_branch();
    run_lower_phi();
    run_lower_neg_unary();
    run_lower_unary_variants();
    run_lower_logical_not();
    run_lower_isnull();
    run_lower_width_variants();
    run_lower_conversion_variants();
    run_lower_void_return();
    run_lower_call();
    run_lower_print();
    run_lower_throw();
    run_lower_shared_var();
    run_lower_load_field();
    run_lower_index_get();
    run_lower_array_new();
    run_reject_multi_ret_with_extra_results();
    run_reject_extract_extra_result();
    run_reject_bytes_memory_ops_until_jit_driver_exists();

    printf("\n=== %d/%d tests passed ===\n", tests_passed, tests_passed);
    return 0;
}
