/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xi_own.c - Unit tests for backward ownership inference (xi_own.h/c)
 *
 * Constructs small IR functions and verifies that xi_own_analyze
 * correctly classifies RC values (owned/borrow), finds last/consuming
 * uses, flags dead values, and infers a parameter borrow signature.
 */

#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_own.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"
#include <stdio.h>
#include <string.h>

static int g_passed = 0;
static int g_failed = 0;

#define ASSERT_EQ(actual, expected, msg)                                                           \
    do {                                                                                           \
        if ((actual) != (expected)) {                                                              \
            fprintf(stderr, "  FAIL: %s (got %d, expected %d)\n", msg, (int) (actual),             \
                    (int) (expected));                                                             \
            g_failed++;                                                                            \
        } else {                                                                                   \
            g_passed++;                                                                            \
        }                                                                                          \
    } while (0)

#define ASSERT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "  FAIL: %s\n", msg);                                                  \
            g_failed++;                                                                            \
        } else {                                                                                   \
            g_passed++;                                                                            \
        }                                                                                          \
    } while (0)

/* ========== Shared Type Singletons ========== */

static XrType t_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType t_array = {.kind = XR_KIND_ARRAY, .id = 2, .frozen = true};
static XrType t_str = {.kind = XR_KIND_STRING, .id = 3, .frozen = true};
static XrType t_any = {.kind = XR_KIND_UNKNOWN, .id = 4, .frozen = true};

static XiFunc *make_func(const char *name, XrType *ret) {
    XiFunc *f = xi_func_new(name, ret);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    return f;
}

/* ========== Test: type classification ========== */

static void test_type_is_rc(void) {
    ASSERT_EQ(xi_own_type_is_rc(&t_int), false, "int is not RC");
    ASSERT_EQ(xi_own_type_is_rc(&t_array), true, "array is RC");
    ASSERT_EQ(xi_own_type_is_rc(&t_str), true, "string is RC");
    ASSERT_EQ(xi_own_type_is_rc(NULL), true, "NULL type is conservatively RC");
}

/* ========== Test: generated use-site ownership policy ========== */

static void test_use_policy(void) {
    ASSERT_EQ(xi_own_use_is_consuming(XI_ADD, 0), false, "ADD borrows operands");
    ASSERT_EQ(xi_own_use_is_consuming(XI_INDEX_GET, 0), false, "INDEX_GET borrows base");
    ASSERT_EQ(xi_own_use_is_consuming(XI_STORE_FIELD, 0), false, "STORE_FIELD borrows receiver");
    ASSERT_EQ(xi_own_use_is_consuming(XI_STORE_FIELD, 1), true,
              "STORE_FIELD consumes stored value");
    ASSERT_EQ(xi_own_use_is_consuming(XI_CALL_METHOD, 0), false, "CALL_METHOD borrows receiver");
    ASSERT_EQ(xi_own_use_is_consuming(XI_CALL_METHOD, 1), true,
              "CALL_METHOD consumes non-receiver args");
    ASSERT_EQ(xi_own_use_is_consuming(XI_CALL, 0), true, "CALL consumes args");
    ASSERT_EQ(xi_own_use_is_consuming(XI_OP_COUNT, 0), true, "unknown op conservatively consumes");
}

/* ========== Test: dead value → drop at definition ========== */

/*
 * func dead_array():
 *   b0:
 *     v0 = ARRAY_NEW   ; never used
 *     RETURN const 0
 */
static void test_dead_value(void) {
    XiFunc *f = make_func("dead_array", &t_int);
    XiBlock *b0 = f->entry;

    XiValue *arr = xi_value_new(f, b0, XI_ARRAY_NEW, &t_array, 0);
    XiValue *zero = xi_const_int(f, b0, 0, &t_int);
    xi_block_set_return(b0, zero);

    XiOwnResult own;
    ASSERT_TRUE(xi_own_analyze(f, &own), "analyze dead_array");

    const XiOwnInfo *ai = &own.values[arr->id];
    ASSERT_EQ(ai->rc_managed, true, "array is RC managed");
    ASSERT_EQ(ai->is_dead, true, "unused array is dead (drop at def)");
    ASSERT_EQ(ai->consumed, false, "dead array is not consumed");

    xi_own_free(&own);
    xi_func_free(f);
}

/* ========== Test: borrowed use only (read, no consume) ========== */

/*
 * func borrow_only():
 *   b0:
 *     v0 = ARRAY_NEW
 *     v1 = CONST 0
 *     v2 = INDEX_GET v0, v1   ; reads array (borrow)
 *     RETURN v2               ; returns int element, array not escaped
 */
static void test_borrow_only(void) {
    XiFunc *f = make_func("borrow_only", &t_int);
    XiBlock *b0 = f->entry;

    XiValue *arr = xi_value_new(f, b0, XI_ARRAY_NEW, &t_array, 0);
    XiValue *idx = xi_const_int(f, b0, 0, &t_int);
    XiValue *get = xi_value_new(f, b0, XI_INDEX_GET, &t_int, 2);
    get->args[0] = arr;
    get->args[1] = idx;
    xi_block_set_return(b0, get);

    XiOwnResult own;
    ASSERT_TRUE(xi_own_analyze(f, &own), "analyze borrow_only");

    const XiOwnInfo *ai = &own.values[arr->id];
    ASSERT_EQ(ai->rc_managed, true, "array is RC managed");
    ASSERT_EQ(ai->is_dead, false, "array is used (not dead)");
    /* INDEX_GET only borrows the array → not consumed; a drop is needed
     * after the last borrow. */
    ASSERT_EQ(ai->consumed, false, "array used only by INDEX_GET is borrowed, not consumed");

    /* The returned int element is RC=false → not tracked as owned. */
    const XiOwnInfo *gi = &own.values[get->id];
    ASSERT_EQ(gi->rc_managed, false, "int element is not RC managed");

    xi_own_free(&own);
    xi_func_free(f);
}

/* ========== Test: consuming use (returned array) ========== */

/*
 * func make_array():
 *   b0:
 *     v0 = ARRAY_NEW
 *     RETURN v0      ; returns the array → consumed at the return
 */
static void test_consumed_return(void) {
    XiFunc *f = make_func("make_array", &t_array);
    XiBlock *b0 = f->entry;

    XiValue *arr = xi_value_new(f, b0, XI_ARRAY_NEW, &t_array, 0);
    xi_block_set_return(b0, arr);

    XiOwnResult own;
    ASSERT_TRUE(xi_own_analyze(f, &own), "analyze make_array");

    const XiOwnInfo *ai = &own.values[arr->id];
    ASSERT_EQ(ai->rc_managed, true, "array is RC managed");
    ASSERT_EQ(ai->is_dead, false, "returned array is not dead");
    ASSERT_EQ(ai->consumed, true, "returned array is consumed at the return");
    ASSERT_EQ(ai->last_use_blk, b0->id, "last use is in the return block");

    xi_own_free(&own);
    xi_func_free(f);
}

/* ========== Test: consuming use (stored to field) ========== */

/*
 * func store_field(p0):
 *   b0:
 *     v0 = PARAM 0            ; receiver (borrowed)
 *     v1 = ARRAY_NEW
 *     v2 = STORE_FIELD v0, v1 ; array consumed into the object
 *     RETURN const 0
 */
static void test_consumed_store_field(void) {
    XiFunc *f = make_func("store_field", &t_int);
    XiBlock *b0 = f->entry;

    XiValue *obj = xi_param(f, b0, 0, &t_any);
    XiValue *arr = xi_value_new(f, b0, XI_ARRAY_NEW, &t_array, 0);
    XiValue *store = xi_value_new(f, b0, XI_STORE_FIELD, &t_any, 2);
    store->args[0] = obj;
    store->args[1] = arr;
    store->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
    XiValue *zero = xi_const_int(f, b0, 0, &t_int);
    xi_block_set_return(b0, zero);

    XiOwnResult own;
    ASSERT_TRUE(xi_own_analyze(f, &own), "analyze store_field");

    /* The stored array (arg 1) is consumed. */
    const XiOwnInfo *ai = &own.values[arr->id];
    ASSERT_EQ(ai->consumed, true, "array stored to field is consumed");
    ASSERT_EQ(ai->last_use_val, store->id, "last use is the STORE_FIELD");

    /* The receiver object (arg 0) is borrowed by the store. */
    const XiOwnInfo *oi = &own.values[obj->id];
    ASSERT_EQ(oi->rc_managed, true, "receiver object is RC managed");

    xi_own_free(&own);
    xi_func_free(f);
}

/* ========== Test: borrow signature inference ========== */

/*
 * func use_params(p0: array, p1: array):
 *   b0:
 *     v0 = PARAM 0          ; only read (borrowed)
 *     v1 = PARAM 1          ; stored to p0 (consumed/owned)
 *     v2 = CONST 0
 *     v3 = INDEX_GET v0, v2 ; borrow p0
 *     v4 = STORE_FIELD v0, v1 ; consume p1 into p0
 *     RETURN const 0
 */
static void test_borrow_signature(void) {
    XiFunc *f = make_func("use_params", &t_int);
    XiBlock *b0 = f->entry;

    XiValue *p0 = xi_param(f, b0, 0, &t_array);
    XiValue *p1 = xi_param(f, b0, 1, &t_array);
    /* Wire up the function parameter table as the lowerer does
     * (xi_param creates the SSA value; params[] is populated by the
     * caller — see xi_lower.c). */
    f->nparams = 2;
    f->params = (XiValue **) xr_calloc(2, sizeof(XiValue *));
    f->params[0] = p0;
    f->params[1] = p1;

    XiValue *idx = xi_const_int(f, b0, 0, &t_int);
    XiValue *get = xi_value_new(f, b0, XI_INDEX_GET, &t_int, 2);
    get->args[0] = p0;
    get->args[1] = idx;
    XiValue *store = xi_value_new(f, b0, XI_STORE_FIELD, &t_any, 2);
    store->args[0] = p0;
    store->args[1] = p1;
    store->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
    XiValue *zero = xi_const_int(f, b0, 0, &t_int);
    xi_block_set_return(b0, zero);

    XiOwnResult own;
    ASSERT_TRUE(xi_own_analyze(f, &own), "analyze use_params");

    ASSERT_TRUE(own.sig.valid, "borrow signature computed");
    ASSERT_EQ(own.sig.nparams, 2, "two params tracked");
    /* p0 only read → borrowed; p1 stored → owned. */
    ASSERT_EQ(own.sig.param_own[0], XI_OWN_BORROWED, "p0 borrowed (only read)");
    ASSERT_EQ(own.sig.param_own[1], XI_OWN_OWNED, "p1 owned (stored to field)");

    xi_own_free(&own);
    xi_func_free(f);
}

/* ========== Test: scalar values are not RC tracked ========== */

static void test_scalar_not_tracked(void) {
    XiFunc *f = make_func("scalar_add", &t_int);
    XiBlock *b0 = f->entry;

    XiValue *a = xi_const_int(f, b0, 1, &t_int);
    XiValue *b = xi_const_int(f, b0, 2, &t_int);
    XiValue *sum = xi_value_new(f, b0, XI_ADD, &t_int, 2);
    sum->args[0] = a;
    sum->args[1] = b;
    xi_block_set_return(b0, sum);

    XiOwnResult own;
    ASSERT_TRUE(xi_own_analyze(f, &own), "analyze scalar_add");

    ASSERT_EQ(own.values[a->id].rc_managed, false, "int const not RC");
    ASSERT_EQ(own.values[sum->id].rc_managed, false, "int sum not RC");
    ASSERT_EQ(own.n_owned, 0u, "no owned RC values in scalar function");

    xi_own_free(&own);
    xi_func_free(f);
}

/* ========== Main ========== */

int main(void) {
    test_type_is_rc();
    test_use_policy();
    test_dead_value();
    test_borrow_only();
    test_consumed_return();
    test_consumed_store_field();
    test_borrow_signature();
    test_scalar_not_tracked();

    printf("\n=== test_xi_own: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
