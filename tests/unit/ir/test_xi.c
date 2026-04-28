/*
 * test_xi.c - Unit test for the new typed SSA IR (Xi)
 *
 * Builds a simple IR for: fn add(a: int, b: int) -> int { return a + b }
 * and dumps it to verify structure.
 */

#include "../../../src/ir/xi.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

/* Minimal XrType stubs for testing without an isolate */
static XrType stub_int  = { .kind = XR_KIND_INT,   .id = 1, .frozen = true };
static XrType stub_float = { .kind = XR_KIND_FLOAT, .id = 2, .frozen = true };
static XrType stub_bool = { .kind = XR_KIND_BOOL,  .id = 3, .frozen = true };
static XrType stub_null = { .kind = XR_KIND_NULL,  .id = 4, .frozen = true };
static XrType stub_str  = { .kind = XR_KIND_STRING,.id = 5, .frozen = true };

/* Test 1: fn add(a: int, b: int) -> int { return a + b } */
static void test_simple_add(void) {
    printf("--- test_simple_add ---\n");

    XiFunc *f = xi_func_new("add", &stub_int);
    assert(f != NULL);

    XiBlock *entry = xi_block_new(f);
    assert(entry != NULL);
    assert(entry == f->entry);

    /* Parameters */
    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiValue *b = xi_param(f, entry, 1, &stub_int);
    assert(a != NULL && b != NULL);
    assert(a->id == 0 && b->id == 1);

    /* Register params */
    f->nparams = 2;
    f->params = (XiValue **) xr_malloc(2 * sizeof(XiValue *));
    assert(f->params != NULL);
    f->params[0] = a;
    f->params[1] = b;

    /* a + b */
    XiValue *sum = xi_binary(f, entry, XI_ADD, &stub_int, a, b);
    assert(sum != NULL);
    assert(sum->nargs == 2);
    assert(sum->args[0] == a);
    assert(sum->args[1] == b);

    /* return sum */
    xi_block_set_return(entry, sum);
    assert(entry->kind == XI_BLOCK_RETURN);
    assert(entry->control == sum);

    /* Dump */
    xi_func_dump(f, stdout);

    /* Verify structure */
    assert(f->nblocks == 1);
    assert(f->next_value_id == 3);

    xi_func_free(f);
    printf("  PASS\n\n");
}

/* Test 2: fn max(a: int, b: int) -> int { if a > b { return a } else { return b } } */
static void test_if_branch(void) {
    printf("--- test_if_branch ---\n");

    XiFunc *f = xi_func_new("max", &stub_int);
    assert(f != NULL);

    XiBlock *entry = xi_block_new(f);
    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);
    assert(entry && then_blk && else_blk);

    /* Parameters */
    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiValue *b = xi_param(f, entry, 1, &stub_int);
    f->nparams = 2;
    f->params = (XiValue **) xr_malloc(2 * sizeof(XiValue *));
    assert(f->params != NULL);
    f->params[0] = a;
    f->params[1] = b;

    /* a > b */
    XiValue *cmp = xi_binary(f, entry, XI_GT, &stub_bool, a, b);

    /* if cmp -> then_blk, else_blk */
    xi_block_set_if(entry, cmp, then_blk, else_blk);

    /* then: return a */
    xi_block_set_return(then_blk, a);

    /* else: return b */
    xi_block_set_return(else_blk, b);

    /* Dump */
    xi_func_dump(f, stdout);

    /* Verify */
    assert(f->nblocks == 3);
    assert(entry->kind == XI_BLOCK_IF);
    assert(entry->succs[0] == then_blk);
    assert(entry->succs[1] == else_blk);
    assert(then_blk->npreds == 1 && then_blk->preds[0] == entry);
    assert(else_blk->npreds == 1 && else_blk->preds[0] == entry);

    xi_func_free(f);
    printf("  PASS\n\n");
}

/* Test 3: phi node with a diamond CFG */
static void test_phi_node(void) {
    printf("--- test_phi_node ---\n");

    XiFunc *f = xi_func_new("phi_test", &stub_int);
    XiBlock *entry = xi_block_new(f);
    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);
    XiBlock *merge = xi_block_new(f);

    XiValue *cond = xi_const_bool(f, entry, true, &stub_bool);
    XiValue *val_t = xi_const_int(f, then_blk, 42, &stub_int);
    XiValue *val_f = xi_const_int(f, else_blk, 0, &stub_int);

    xi_block_set_if(entry, cond, then_blk, else_blk);
    xi_block_set_jump(then_blk, merge);
    xi_block_set_jump(else_blk, merge);

    /* Phi in merge block: merges val_t from then, val_f from else */
    XiPhi *phi = xi_phi_new(f, merge, &stub_int, 2);
    assert(phi != NULL);
    phi->value.args[0] = val_t;
    phi->value.args[1] = val_f;

    xi_block_set_return(merge, &phi->value);

    /* Dump */
    xi_func_dump(f, stdout);

    /* Verify */
    assert(f->nblocks == 4);
    assert(merge->npreds == 2);
    assert(merge->phis != NULL);

    xi_func_free(f);
    printf("  PASS\n\n");
}

/* Test 4: constant types */
static void test_constants(void) {
    printf("--- test_constants ---\n");

    XiFunc *f = xi_func_new("consts", &stub_null);
    XiBlock *blk = xi_block_new(f);

    XiValue *vi = xi_const_int(f, blk, 123, &stub_int);
    XiValue *vf = xi_const_float(f, blk, 3.14, &stub_float);
    XiValue *vb = xi_const_bool(f, blk, false, &stub_bool);
    XiValue *vn = xi_const_null(f, blk, &stub_null);
    XiValue *vs = xi_const_str(f, blk, "hello", &stub_str);

    assert(vi && vf && vb && vn && vs);
    assert(vi->aux_int == 123);
    assert(vb->aux_int == 0);

    f->nparams = 0;
    f->params = NULL;
    xi_block_set_return(blk, vn);
    xi_func_dump(f, stdout);

    xi_func_free(f);
    printf("  PASS\n\n");
}

int main(void) {
    printf("=== Xi IR Unit Tests ===\n\n");

    test_simple_add();
    test_if_branch();
    test_phi_node();
    test_constants();

    printf("=== All Xi IR tests passed ===\n");
    return 0;
}
