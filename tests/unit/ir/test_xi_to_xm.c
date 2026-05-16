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
    pr->aux_int = 1; /* newline flag */
    xi_block_set_return(entry, NULL);

    XmFunc *xm = xi_to_xm_lower(f, NULL, NULL, NULL, NULL);
    assert(xm != NULL && "print lowering should succeed");

    XmBlock *blk0 = xm->blocks[0];
    bool found_print = false;
    for (uint32_t i = 0; i < blk0->nins; i++) {
        if (blk0->ins[i].op == XM_RT_PRINT)
            found_print = true;
    }
    assert(found_print && "should contain XM_RT_PRINT");

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

/* ========== Main ========== */

int main(void) {
    printf("=== Xi-to-Xm Lowering Unit Tests ===\n\n");

    run_lower_const_int();
    run_lower_const_float();
    run_lower_add_int();
    run_lower_add_float();
    run_lower_comparison();
    run_lower_if_branch();
    run_lower_phi();
    run_lower_neg_unary();
    run_lower_void_return();
    run_lower_call();
    run_lower_print();
    run_lower_shared_var();
    run_lower_load_field();
    run_lower_index_get();
    run_lower_array_new();

    printf("\n=== %d/%d tests passed ===\n", tests_passed, tests_passed);
    return 0;
}
