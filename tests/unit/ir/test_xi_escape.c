/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xi_escape.c - Unit tests for escape analysis (xi_escape.h/c)
 *
 * Constructs small IR functions and verifies that xi_escape_analyze
 * correctly computes escape levels for heap-allocating values.
 */

#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_escape.h"
#include "../../../src/runtime/value/xtype.h"
#include <stdio.h>
#include <string.h>

static int g_passed = 0;
static int g_failed = 0;

#define ASSERT_EQ(actual, expected, msg)                             \
    do {                                                             \
        if ((actual) != (expected)) {                                \
            fprintf(stderr, "  FAIL: %s (got %d, expected %d)\n",   \
                    msg, (int)(actual), (int)(expected));             \
            g_failed++;                                              \
        } else {                                                     \
            g_passed++;                                              \
        }                                                            \
    } while (0)

/* ========== Shared Type Singletons ========== */

static XrType t_int   = { .kind = XR_KIND_INT,     .id = 1, .frozen = true };
static XrType t_array = { .kind = XR_KIND_ARRAY,   .id = 2, .frozen = true };
static XrType t_str   = { .kind = XR_KIND_STRING,  .id = 3, .frozen = true };
static XrType t_any   = { .kind = XR_KIND_UNKNOWN, .id = 4, .frozen = true };

/* Helper: create function with sealed entry block */
static XiFunc *make_func(const char *name, XrType *ret) {
    XiFunc *f = xi_func_new(name, ret);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    return f;
}

/* ========== Test: local array does not escape ========== */

/*
 * func local_array():
 *   b0:
 *     v0 = ARRAY_NEW          ; heap alloc
 *     v1 = CONST 42
 *     v2 = INDEX_GET v0, v1   ; read from array
 *     RETURN v2               ; returns element, not array
 */
static void test_local_no_escape(void) {
    XiFunc *f = make_func("local_array", &t_int);
    XiBlock *b0 = f->entry;

    XiValue *arr = xi_value_new(f, b0, XI_ARRAY_NEW, &t_array, 0);
    XiValue *idx = xi_const_int(f, b0, 0, &t_int);
    XiValue *get = xi_value_new(f, b0, XI_INDEX_GET, &t_int, 2);
    get->args[0] = arr;
    get->args[1] = idx;

    xi_block_set_return(b0, get);

    xi_escape_analyze(f);

    ASSERT_EQ(arr->escape, XI_ESC_NONE, "local array should be NO_ESCAPE");
    ASSERT_EQ(idx->escape, XI_ESC_NONE, "const index should be NO_ESCAPE");
    ASSERT_EQ(get->escape, XI_ESC_ARG, "returned value should be ARG_ESCAPE");

    xi_func_free(f);
}

/* ========== Test: returned array escapes via arg ========== */

/*
 * func make_array():
 *   b0:
 *     v0 = ARRAY_NEW
 *     RETURN v0               ; array escapes to caller
 */
static void test_return_escape(void) {
    XiFunc *f = make_func("make_array", &t_array);
    XiBlock *b0 = f->entry;

    XiValue *arr = xi_value_new(f, b0, XI_ARRAY_NEW, &t_array, 0);
    xi_block_set_return(b0, arr);

    xi_escape_analyze(f);

    ASSERT_EQ(arr->escape, XI_ESC_ARG, "returned array should be ARG_ESCAPE");

    xi_func_free(f);
}

/* ========== Test: stored to field → heap escape ========== */

/*
 * func store_to_field(p0):
 *   b0:
 *     v0 = PARAM 0             ; existing object
 *     v1 = ARRAY_NEW           ; new array
 *     v2 = STORE_FIELD v0, v1  ; store array into object
 *     RETURN v0
 */
static void test_store_field_escape(void) {
    XiFunc *f = make_func("store_field", &t_any);
    XiBlock *b0 = f->entry;

    XiValue *obj = xi_param(f, b0, 0, &t_any);
    XiValue *arr = xi_value_new(f, b0, XI_ARRAY_NEW, &t_array, 0);
    XiValue *store = xi_value_new(f, b0, XI_STORE_FIELD, &t_any, 2);
    store->args[0] = obj;
    store->args[1] = arr;
    store->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;

    xi_block_set_return(b0, obj);

    xi_escape_analyze(f);

    ASSERT_EQ(arr->escape, XI_ESC_HEAP,
              "array stored to field should be HEAP_ESCAPE");
    ASSERT_EQ(obj->escape, XI_ESC_HEAP,
              "object receiving field store should be HEAP_ESCAPE");

    xi_func_free(f);
}

/* ========== Test: channel send → global escape ========== */

/*
 * func send_array(ch):
 *   b0:
 *     v0 = PARAM 0             ; channel
 *     v1 = ARRAY_NEW
 *     v2 = CHAN_SEND v0, v1    ; send array through channel
 *     RETURN v0
 */
static void test_chan_send_escape(void) {
    XiFunc *f = make_func("send_array", &t_any);
    XiBlock *b0 = f->entry;

    XiValue *ch = xi_param(f, b0, 0, &t_any);
    XiValue *arr = xi_value_new(f, b0, XI_ARRAY_NEW, &t_array, 0);
    XiValue *send = xi_value_new(f, b0, XI_CHAN_SEND, &t_any, 2);
    send->args[0] = ch;
    send->args[1] = arr;
    send->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_SUSPEND;

    xi_block_set_return(b0, ch);

    xi_escape_analyze(f);

    ASSERT_EQ(arr->escape, XI_ESC_GLOBAL,
              "array sent through channel should be GLOBAL_ESCAPE");

    xi_func_free(f);
}

/* ========== Test: SET_SHARED → global escape ========== */

/*
 * func set_shared():
 *   b0:
 *     v0 = ARRAY_NEW
 *     v1 = SET_SHARED v0       ; store to module-level shared
 *     RETURN const 0
 */
static void test_set_shared_escape(void) {
    XiFunc *f = make_func("set_shared", &t_int);
    XiBlock *b0 = f->entry;

    XiValue *arr = xi_value_new(f, b0, XI_ARRAY_NEW, &t_array, 0);
    XiValue *set = xi_value_new(f, b0, XI_SET_SHARED, &t_any, 1);
    set->args[0] = arr;
    set->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;

    XiValue *zero = xi_const_int(f, b0, 0, &t_int);
    xi_block_set_return(b0, zero);

    xi_escape_analyze(f);

    ASSERT_EQ(arr->escape, XI_ESC_GLOBAL,
              "array stored to shared should be GLOBAL_ESCAPE");

    xi_func_free(f);
}

/* ========== Test: call arg → heap escape (conservative) ========== */

/*
 * func pass_to_call(callee):
 *   b0:
 *     v0 = PARAM 0
 *     v1 = ARRAY_NEW
 *     v2 = CALL v0, v1         ; pass array to unknown callee
 *     RETURN v2
 */
static void test_call_arg_escape(void) {
    XiFunc *f = make_func("pass_to_call", &t_any);
    XiBlock *b0 = f->entry;

    XiValue *callee = xi_param(f, b0, 0, &t_any);
    XiValue *arr = xi_value_new(f, b0, XI_ARRAY_NEW, &t_array, 0);
    XiValue *call = xi_value_new(f, b0, XI_CALL, &t_any, 2);
    call->args[0] = callee;
    call->args[1] = arr;
    call->flags = XI_FLAG_CALL_EFFECTS;

    xi_block_set_return(b0, call);

    xi_escape_analyze(f);

    ASSERT_EQ(arr->escape, XI_ESC_HEAP,
              "array passed to unknown callee should be HEAP_ESCAPE");

    xi_func_free(f);
}

/* ========== Test: xi_op_is_heap_alloc helper ========== */

static void test_heap_alloc_check(void) {
    ASSERT_EQ(xi_op_is_heap_alloc(XI_ARRAY_NEW), 1, "ARRAY_NEW is heap alloc");
    ASSERT_EQ(xi_op_is_heap_alloc(XI_MAP_NEW), 1, "MAP_NEW is heap alloc");
    ASSERT_EQ(xi_op_is_heap_alloc(XI_SET_NEW), 1, "SET_NEW is heap alloc");
    ASSERT_EQ(xi_op_is_heap_alloc(XI_JSON_NEW), 1, "JSON_NEW is heap alloc");
    ASSERT_EQ(xi_op_is_heap_alloc(XI_CLOSURE_NEW), 1, "CLOSURE_NEW is heap alloc");
    ASSERT_EQ(xi_op_is_heap_alloc(XI_STR_CONCAT), 1, "STR_CONCAT is heap alloc");
    ASSERT_EQ(xi_op_is_heap_alloc(XI_REGEX_COMPILE), 1, "REGEX_COMPILE is heap alloc");
    /* Non-alloc ops */
    ASSERT_EQ(xi_op_is_heap_alloc(XI_ADD), 0, "ADD is not heap alloc");
    ASSERT_EQ(xi_op_is_heap_alloc(XI_CONST), 0, "CONST is not heap alloc");
    ASSERT_EQ(xi_op_is_heap_alloc(XI_CALL), 0, "CALL is not heap alloc");
}

/* ========== Test: lattice join ========== */

static void test_lattice_join(void) {
    ASSERT_EQ(xi_esc_join(XI_ESC_NONE, XI_ESC_NONE), XI_ESC_NONE,
              "join(NONE, NONE) = NONE");
    ASSERT_EQ(xi_esc_join(XI_ESC_NONE, XI_ESC_ARG), XI_ESC_ARG,
              "join(NONE, ARG) = ARG");
    ASSERT_EQ(xi_esc_join(XI_ESC_HEAP, XI_ESC_ARG), XI_ESC_HEAP,
              "join(HEAP, ARG) = HEAP");
    ASSERT_EQ(xi_esc_join(XI_ESC_GLOBAL, XI_ESC_HEAP), XI_ESC_GLOBAL,
              "join(GLOBAL, HEAP) = GLOBAL");
}

/* ========== Main ========== */

int main(void) {
    test_heap_alloc_check();
    test_lattice_join();
    test_local_no_escape();
    test_return_escape();
    test_store_field_escape();
    test_chan_send_escape();
    test_set_shared_escape();
    test_call_arg_escape();

    printf("\n=== test_xi_escape: %d passed, %d failed ===\n",
           g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
