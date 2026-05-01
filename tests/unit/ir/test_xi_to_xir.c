/*
 * test_xi_to_xir.c - Unit tests for Xi IR to XIR lowering
 *
 * Validates that xi_to_xir_lower correctly translates Xi SSA
 * to XIR SSA for arithmetic, comparison, branching, and phi nodes.
 */

#include "../../../src/ir/xi.h"
#include "../../../src/jit/xi_to_xir.h"
#include "../../../src/jit/xir.h"
#include "../../../src/jit/xir_ops.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"

#include <stdio.h>
#include <assert.h>
#include <string.h>

/* Minimal XrType stubs */
static XrType stub_int   = { .kind = XR_KIND_INT,   .id = 1, .frozen = true };
static XrType stub_float = { .kind = XR_KIND_FLOAT, .id = 2, .frozen = true };
static XrType stub_bool  = { .kind = XR_KIND_BOOL,  .id = 3, .frozen = true };
static XrType stub_void  = { .kind = XR_KIND_VOID,  .id = 6, .frozen = true };

static int tests_passed = 0;

#define TEST(name) \
    static void test_##name(void); \
    static void run_##name(void) { \
        printf("--- " #name " ---\n"); \
        test_##name(); \
        printf("  PASS\n"); \
        tests_passed++; \
    } \
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

    XirFunc *xir = xi_to_xir_lower(f, NULL, NULL, NULL);
    assert(xir != NULL && "lowering should succeed");
    assert(xir->nblk >= 1 && "should have at least 1 block");

    xir_func_destroy(xir);
    xi_func_free(f);
}

TEST(lower_const_float) {
    XiFunc *f = make_func("test", &stub_float);
    XiBlock *entry = f->entry;

    XiValue *c = xi_const_float(f, entry, 3.14, &stub_float);
    xi_block_set_return(entry, c);

    XirFunc *xir = xi_to_xir_lower(f, NULL, NULL, NULL);
    assert(xir != NULL && "lowering should succeed");

    xir_func_destroy(xir);
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

    XirFunc *xir = xi_to_xir_lower(f, NULL, NULL, NULL);
    assert(xir != NULL && "lowering should succeed");

    /* Verify: should have at least an ADD instruction */
    XirBlock *blk0 = xir->blocks[0];
    bool found_add = false;
    for (uint32_t i = 0; i < blk0->nins; i++) {
        if (blk0->ins[i].op == XIR_ADD)
            found_add = true;
    }
    assert(found_add && "should contain XIR_ADD instruction");

    xir_func_destroy(xir);
    xi_func_free(f);
}

TEST(lower_add_float) {
    XiFunc *f = make_func("fadd", &stub_float);
    XiBlock *entry = f->entry;

    XiValue *a = xi_param(f, entry, 0, &stub_float);
    XiValue *b = xi_param(f, entry, 1, &stub_float);
    XiValue *sum = xi_binary(f, entry, XI_ADD, &stub_float, a, b);
    xi_block_set_return(entry, sum);

    XirFunc *xir = xi_to_xir_lower(f, NULL, NULL, NULL);
    assert(xir != NULL);

    XirBlock *blk0 = xir->blocks[0];
    bool found_fadd = false;
    for (uint32_t i = 0; i < blk0->nins; i++) {
        if (blk0->ins[i].op == XIR_FADD)
            found_fadd = true;
    }
    assert(found_fadd && "should contain XIR_FADD instruction");

    xir_func_destroy(xir);
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

    XirFunc *xir = xi_to_xir_lower(f, NULL, NULL, NULL);
    assert(xir != NULL);

    XirBlock *blk0 = xir->blocks[0];
    bool found_lt = false;
    for (uint32_t i = 0; i < blk0->nins; i++) {
        if (blk0->ins[i].op == XIR_LT)
            found_lt = true;
    }
    assert(found_lt && "should contain XIR_LT instruction");

    xir_func_destroy(xir);
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

    XirFunc *xir = xi_to_xir_lower(f, NULL, NULL, NULL);
    assert(xir != NULL);
    assert(xir->nblk == 3 && "should have entry + then + else blocks");

    /* Entry block should have a branch terminator */
    XirBlock *xir_entry = xir->blocks[0];
    assert(xir_entry->jmp.type == XIR_JMP_BR && "entry should be conditional branch");
    assert(xir_entry->s1 != NULL && "should have then successor");
    assert(xir_entry->s2 != NULL && "should have else successor");

    xir_func_destroy(xir);
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

    XirFunc *xir = xi_to_xir_lower(f, NULL, NULL, NULL);
    assert(xir != NULL);
    assert(xir->nblk == 4 && "should have 4 blocks");

    /* Merge block should have a phi node */
    XirBlock *xir_merge = xir->blocks[3];
    (void)xir_merge;
    assert(xir_merge->phis != NULL && "merge block should have phi");

    xir_func_destroy(xir);
    xi_func_free(f);
}

TEST(lower_neg_unary) {
    XiFunc *f = make_func("neg", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiValue *neg = xi_unary(f, entry, XI_NEG, &stub_int, a);
    xi_block_set_return(entry, neg);

    XirFunc *xir = xi_to_xir_lower(f, NULL, NULL, NULL);
    assert(xir != NULL);

    XirBlock *blk0 = xir->blocks[0];
    bool found_neg = false;
    for (uint32_t i = 0; i < blk0->nins; i++) {
        if (blk0->ins[i].op == XIR_NEG)
            found_neg = true;
    }
    assert(found_neg && "should contain XIR_NEG instruction");

    xir_func_destroy(xir);
    xi_func_free(f);
}

TEST(lower_void_return) {
    XiFunc *f = make_func("void_fn", &stub_void);
    XiBlock *entry = f->entry;
    xi_block_set_return(entry, NULL);

    XirFunc *xir = xi_to_xir_lower(f, NULL, NULL, NULL);
    assert(xir != NULL);

    XirBlock *blk0 = xir->blocks[0];
    assert(blk0->jmp.type == XIR_JMP_RET && "should have return terminator");

    xir_func_destroy(xir);
    xi_func_free(f);
}

/* ========== Main ========== */

int main(void) {
    printf("=== Xi-to-XIR Lowering Unit Tests ===\n\n");

    run_lower_const_int();
    run_lower_const_float();
    run_lower_add_int();
    run_lower_add_float();
    run_lower_comparison();
    run_lower_if_branch();
    run_lower_phi();
    run_lower_neg_unary();
    run_lower_void_return();

    printf("\n=== %d/%d tests passed ===\n", tests_passed, tests_passed);
    return 0;
}
