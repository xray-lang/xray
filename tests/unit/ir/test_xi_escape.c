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
#include "../../../src/ir/xi_arc.h"
#include "../../../src/base/xchecks.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/runtime/value/xtype.h"
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

/* ========== Shared Type Singletons ========== */

static XrType t_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType t_array = {.kind = XR_KIND_ARRAY, .id = 2, .frozen = true};
static XrType t_map = {.kind = XR_KIND_MAP, .id = 3, .frozen = true};
static XrType t_any = {.kind = XR_KIND_UNKNOWN, .id = 4, .frozen = true};
static XrType t_set = {.kind = XR_KIND_SET, .id = 5, .frozen = true};
static XrType t_func = {.kind = XR_KIND_FUNCTION, .id = 6, .frozen = true};

/* Helper: create function with sealed entry block */
static XiFunc *make_func(const char *name, XrType *ret) {
    XiFunc *f = xi_func_new(name, ret);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    return f;
}

static void set_single_param(XiFunc *f, XiValue *param) {
    f->nparams = 1;
    f->params = (XiValue **) xr_calloc(1, sizeof(XiValue *));
    XR_CHECK(f->params != NULL, "test_xi_escape: param allocation failed");
    f->params[0] = param;
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

    XiValue *cap = xi_const_int(f, b0, 4, &t_int);
    XiValue *arr = xi_value_new(f, b0, XI_ARRAY_NEW, &t_array, 1);
    arr->args[0] = cap;
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

    XiValue *cap = xi_const_int(f, b0, 4, &t_int);
    XiValue *arr = xi_value_new(f, b0, XI_ARRAY_NEW, &t_array, 1);
    arr->args[0] = cap;
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

    ASSERT_EQ(arr->escape, XI_ESC_HEAP, "array stored to field should be HEAP_ESCAPE");
    ASSERT_EQ(obj->escape, XI_ESC_HEAP, "object receiving field store should be HEAP_ESCAPE");

    xi_func_free(f);
}

/* ========== Test: channel send → global escape ========== */

/*
 * func send_array(ch):
 *   b0:
 *     v0 = PARAM 0                  ; channel
 *     v1 = ARRAY_NEW
 *     v2 = CHAN_SEND v0, move v1    ; move array through channel
 *     RETURN v0
 *
 * Only a MOVE payload transfers ownership out of the sender, so only a moved
 * channel-send payload globally escapes. copy/share sends keep the value owned
 * by the sender (task 131-S2 transfer-aware escape).
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
    xi_chan_send_set_transfer_mode(send, XR_TRANSFER_MOVE);

    xi_block_set_return(b0, ch);

    xi_escape_analyze(f);

    ASSERT_EQ(arr->escape, XI_ESC_GLOBAL, "array sent through channel should be GLOBAL_ESCAPE");

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

    ASSERT_EQ(arr->escape, XI_ESC_GLOBAL, "array stored to shared should be GLOBAL_ESCAPE");

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

    ASSERT_EQ(arr->escape, XI_ESC_HEAP, "array passed to unknown callee should be HEAP_ESCAPE");

    xi_func_free(f);
}

/* ========== Test: array filled via INDEX_SET stays NO_ESCAPE (per-arg) ===== */

/*
 * func fill_local_array():
 *   b0:
 *     v0 = ARRAY_NEW cap=4       ; the container (heap alloc)
 *     v1 = ARRAY_NEW cap=0       ; a heap value to store as an element
 *     v2 = CONST 0               ; index
 *     v3 = INDEX_SET v0, v2, v1  ; arr[0] = elem  (container mutated, elem stored)
 *     v4 = CONST 0
 *     v5 = INDEX_GET v0, v4      ; read back an element
 *     RETURN v5                  ; returns the element, NOT the array
 *
 * The container (arg 0 of INDEX_SET) must NOT be forced to HEAP_ESCAPE by the
 * store: it stays local -> NO_ESCAPE -> stack-allocatable. The stored element
 * (arg 2) does escape into the heap container -> HEAP_ESCAPE.
 */
static void test_index_set_container_no_escape(void) {
    XiFunc *f = make_func("fill_local_array", &t_any);
    XiBlock *b0 = f->entry;

    XiValue *cap = xi_const_int(f, b0, 4, &t_int);
    XiValue *arr = xi_value_new(f, b0, XI_ARRAY_NEW, &t_array, 1);
    arr->args[0] = cap;

    XiValue *elem = xi_value_new(f, b0, XI_ARRAY_NEW, &t_array, 0);

    XiValue *idx = xi_const_int(f, b0, 0, &t_int);
    XiValue *set = xi_value_new(f, b0, XI_INDEX_SET, &t_any, 3);
    set->args[0] = arr;
    set->args[1] = idx;
    set->args[2] = elem;
    set->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;

    XiValue *idx2 = xi_const_int(f, b0, 0, &t_int);
    XiValue *get = xi_value_new(f, b0, XI_INDEX_GET, &t_any, 2);
    get->args[0] = arr;
    get->args[1] = idx2;
    xi_block_set_return(b0, get);

    xi_escape_analyze(f);

    ASSERT_EQ(arr->escape, XI_ESC_NONE, "array filled via INDEX_SET should stay NO_ESCAPE");
    ASSERT_EQ(elem->escape, XI_ESC_HEAP, "element stored via INDEX_SET should be HEAP_ESCAPE");

    /* Array literals stay on the heap even when NO_ESCAPE: the stack
     * rewrite would lose the preset length + typed element storage the
     * literal INDEX_SET fills depend on (xi_arc.c stack eligibility). */
    xi_stack_alloc_rewrite(f);
    ASSERT_EQ(arr->op, XI_ARRAY_NEW, "filled local array stays on the heap path");

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
    ASSERT_EQ(xi_op_is_heap_alloc(XI_STRUCT_NEW), 0, "STRUCT_NEW is not escape heap alloc");
    ASSERT_EQ(xi_op_is_heap_alloc(XI_CHAN_NEW), 0, "CHAN_NEW is not escape heap alloc");
    ASSERT_EQ(xi_op_is_heap_alloc(XI_CLASS_CREATE), 0, "CLASS_CREATE is not escape heap alloc");
}

/* ========== Test: generated use-site escape policy ========== */

static void test_generated_use_escape_policy(void) {
    ASSERT_EQ(xi_op_use_escape_level(XI_SET_SHARED), XI_ESC_GLOBAL,
              "SET_SHARED use escape is GLOBAL");
    ASSERT_EQ(xi_op_use_escape_level(XI_CHAN_SEND), XI_ESC_GLOBAL,
              "CHAN_SEND use escape is GLOBAL");
    ASSERT_EQ(xi_op_use_escape_level(XI_GO), XI_ESC_GLOBAL, "GO use escape is GLOBAL");
    ASSERT_EQ(xi_op_use_escape_level(XI_STORE_FIELD), XI_ESC_HEAP,
              "STORE_FIELD use escape is HEAP");
    ASSERT_EQ(xi_op_use_escape_level(XI_TUPLE_NEW), XI_ESC_HEAP, "TUPLE_NEW use escape is HEAP");
    ASSERT_EQ(xi_op_use_escape_level(XI_CALL), XI_ESC_HEAP, "CALL use escape is HEAP");
    ASSERT_EQ(xi_op_use_escape_level(XI_THROW), XI_ESC_ARG, "THROW use escape is ARG");
    ASSERT_EQ(xi_op_use_escape_level(XI_ADD), XI_ESC_NONE, "ADD use escape is NONE");
    ASSERT_EQ(xi_op_use_escape_level(XI_INDEX_GET), XI_ESC_NONE, "INDEX_GET use escape is NONE");
}

/* ========== Test: lattice join ========== */

static void test_lattice_join(void) {
    ASSERT_EQ(xi_esc_join(XI_ESC_NONE, XI_ESC_NONE), XI_ESC_NONE, "join(NONE, NONE) = NONE");
    ASSERT_EQ(xi_esc_join(XI_ESC_NONE, XI_ESC_ARG), XI_ESC_ARG, "join(NONE, ARG) = ARG");
    ASSERT_EQ(xi_esc_join(XI_ESC_HEAP, XI_ESC_ARG), XI_ESC_HEAP, "join(HEAP, ARG) = HEAP");
    ASSERT_EQ(xi_esc_join(XI_ESC_GLOBAL, XI_ESC_HEAP), XI_ESC_GLOBAL,
              "join(GLOBAL, HEAP) = GLOBAL");
}

/* ========== Test: ARC insertion — no retain for NO_ESCAPE ========== */

/* Count occurrences of a given op in a function's IR. */
static int count_ops(const XiFunc *f, uint16_t op) {
    int count = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            if (blk->values[i] && blk->values[i]->op == op)
                count++;
        }
    }
    return count;
}

static void test_arc_no_escape_skipped(void) {
    /* Local array (NO_ESCAPE) should get zero RETAIN/RELEASE */
    XiFunc *f = make_func("arc_local", &t_int);
    XiBlock *b0 = f->entry;

    XiValue *arr = xi_value_new(f, b0, XI_ARRAY_NEW, &t_array, 0);
    XiValue *idx = xi_const_int(f, b0, 0, &t_int);
    XiValue *get = xi_value_new(f, b0, XI_INDEX_GET, &t_int, 2);
    get->args[0] = arr;
    get->args[1] = idx;
    xi_block_set_return(b0, get);

    xi_escape_analyze(f);
    xi_arc_insert(f);

    ASSERT_EQ(count_ops(f, XI_RETAIN), 0, "NO_ESCAPE array should have 0 retains");
    ASSERT_EQ(count_ops(f, XI_RELEASE), 0, "NO_ESCAPE array should have 0 releases");
    xi_func_free(f);
}

/* ========== Test: ARC insertion — returned value is a move (no dup) ========== */

static void test_arc_return_gets_retain(void) {
    /* Returned array: the return is the single (last) consuming use, so it
     * is a MOVE — caller takes ownership. No dup, no drop (Perceus). */
    XiFunc *f = make_func("arc_return", &t_array);
    XiBlock *b0 = f->entry;

    XiValue *arr = xi_value_new(f, b0, XI_ARRAY_NEW, &t_array, 0);
    xi_block_set_return(b0, arr);

    xi_escape_analyze(f);
    xi_arc_insert(f);

    ASSERT_EQ(count_ops(f, XI_RETAIN), 0, "returned array is moved: 0 dup");
    ASSERT_EQ(count_ops(f, XI_RELEASE), 0, "returned array is moved: 0 drop (caller owns)");
    xi_func_free(f);
}

/* ========== Test: ARC insertion — move into field + borrowed receiver ====== */

static void test_arc_heap_gets_retain_release(void) {
    /* Array stored to a field: the store is the single consuming use, so
     * the array is MOVED into the object — 0 dup, 0 drop for it.
     * The receiver `obj` (param, arg 0 of STORE_FIELD) is only borrowed and
     * never consumed, so it is dropped once at function exit. */
    XiFunc *f = make_func("arc_heap", &t_any);
    XiBlock *b0 = f->entry;

    XiValue *obj = xi_param(f, b0, 0, &t_any);
    XiValue *arr = xi_value_new(f, b0, XI_ARRAY_NEW, &t_array, 0);
    XiValue *store = xi_value_new(f, b0, XI_STORE_FIELD, &t_any, 2);
    store->args[0] = obj;
    store->args[1] = arr;
    store->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;

    XiValue *zero = xi_const_int(f, b0, 0, &t_int);
    xi_block_set_return(b0, zero);

    xi_escape_analyze(f);
    xi_arc_insert(f);

    ASSERT_EQ(count_ops(f, XI_RETAIN), 0, "array moved into field: 0 dup");
    ASSERT_EQ(count_ops(f, XI_RELEASE), 0, "borrowed receiver is not dropped by callee");
    xi_func_free(f);
}

/* ========== Test: ARC elim keeps borrowed single-consumer dup ========== */

static void test_arc_elim_keeps_borrowed_single_consumer_retain(void) {
    XiFunc *f = make_func("arc_borrowed_single_consume", &t_int);
    f->receiver_borrowed = true;
    XiBlock *b0 = f->entry;

    XiValue *self = xi_param(f, b0, 0, &t_array);
    set_single_param(f, self);
    XiValue *set = xi_value_new(f, b0, XI_SET_SHARED, &t_any, 1);
    set->args[0] = self;
    set->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;

    XiValue *zero = xi_const_int(f, b0, 0, &t_int);
    xi_block_set_return(b0, zero);

    xi_escape_analyze(f);
    xi_arc_insert(f);

    ASSERT_EQ(count_ops(f, XI_RETAIN), 1, "borrowed stored receiver needs one retain");
    xi_arc_elim(f);
    ASSERT_EQ(count_ops(f, XI_RETAIN), 1, "ARC elim must keep borrowed transfer retain");
    ASSERT_EQ(count_ops(f, XI_RELEASE), 0, "borrowed receiver is not owned by callee");
    xi_func_free(f);
}

/* ========== Test: ARC handles more than the old fixed site cap ========== */

static void test_arc_many_consume_sites(void) {
    enum {
        NCONSUMES = 300
    };
    XiFunc *f = make_func("arc_many_consume_sites", &t_int);
    XiBlock *b0 = f->entry;

    XiValue *value = xi_param(f, b0, 0, &t_array);
    set_single_param(f, value);
    for (int i = 0; i < NCONSUMES; i++) {
        XiValue *set = xi_value_new(f, b0, XI_SET_SHARED, &t_any, 1);
        set->args[0] = value;
        set->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
    }

    XiValue *zero = xi_const_int(f, b0, 0, &t_int);
    xi_block_set_return(b0, zero);

    xi_escape_analyze(f);
    xi_arc_insert(f);

    ASSERT_EQ(count_ops(f, XI_RETAIN), NCONSUMES - 1,
              "all non-last consumes need retains beyond 256 sites");
    xi_func_free(f);
}

/* ========== Test: stack alloc rewrite — NO_ESCAPE becomes STACK_ALLOC ========== */

static void test_stack_alloc_local_array(void) {
    /* Local arrays stay on the heap even when NO_ESCAPE: XI_ARRAY_NEW is
     * excluded from the stack rewrite because literals depend on the heap
     * path's preset length + typed element storage. */
    XiFunc *f = make_func("stack_local", &t_int);
    XiBlock *b0 = f->entry;

    XiValue *cap = xi_const_int(f, b0, 4, &t_int);
    XiValue *arr = xi_value_new(f, b0, XI_ARRAY_NEW, &t_array, 1);
    arr->args[0] = cap;
    XiValue *idx = xi_const_int(f, b0, 0, &t_int);
    XiValue *get = xi_value_new(f, b0, XI_INDEX_GET, &t_int, 2);
    get->args[0] = arr;
    get->args[1] = idx;
    xi_block_set_return(b0, get);

    xi_escape_analyze(f);
    xi_stack_alloc_rewrite(f);

    ASSERT_EQ(arr->op, XI_ARRAY_NEW, "NO_ESCAPE array stays on the heap path");
    xi_func_free(f);
}

static void test_stack_alloc_local_plain_map_set(void) {
    XiFunc *f = make_func("stack_map_set", &t_int);
    XiBlock *b0 = f->entry;

    XiValue *map_cap = xi_const_int(f, b0, 4, &t_int);
    XiValue *map = xi_value_new(f, b0, XI_MAP_NEW, &t_map, 1);
    map->args[0] = map_cap;

    XiValue *set_cap = xi_const_int(f, b0, 4, &t_int);
    XiValue *set = xi_value_new(f, b0, XI_SET_NEW, &t_set, 1);
    set->args[0] = set_cap;

    XiValue *idx = xi_const_int(f, b0, 0, &t_int);
    XiValue *map_get = xi_value_new(f, b0, XI_INDEX_GET, &t_int, 2);
    map_get->args[0] = map;
    map_get->args[1] = idx;
    xi_block_set_return(b0, map_get);

    xi_escape_analyze(f);
    xi_stack_alloc_rewrite(f);

    ASSERT_EQ(map->op, XI_STACK_ALLOC, "plain local map should become STACK_ALLOC");
    ASSERT_EQ(map->aux_int, XI_MAP_NEW, "map STACK_ALLOC should preserve original op");
    ASSERT_EQ(set->op, XI_STACK_ALLOC, "plain local set should become STACK_ALLOC");
    ASSERT_EQ(set->aux_int, XI_SET_NEW, "set STACK_ALLOC should preserve original op");
    xi_func_free(f);
}

static void test_stack_alloc_skips_metadata_or_dynamic_capacity(void) {
    XiFunc *f = make_func("stack_guard", &t_int);
    XiBlock *b0 = f->entry;

    XiValue *cap = xi_const_int(f, b0, 4, &t_int);
    XiValue *typed_arr = xi_value_new(f, b0, XI_ARRAY_NEW, &t_array, 1);
    typed_arr->args[0] = cap;
    typed_arr->aux_int = 4;

    XiValue *typed_map = xi_value_new(f, b0, XI_MAP_NEW, &t_map, 1);
    typed_map->args[0] = cap;
    typed_map->aux_int = 4;

    XiValue *weak_set = xi_value_new(f, b0, XI_SET_NEW, &t_set, 1);
    weak_set->args[0] = cap;
    weak_set->aux_int = 0x02;

    XiValue *dyn_cap = xi_param(f, b0, 0, &t_int);
    XiValue *dyn_map = xi_value_new(f, b0, XI_MAP_NEW, &t_map, 1);
    dyn_map->args[0] = dyn_cap;

    XiValue *idx = xi_const_int(f, b0, 0, &t_int);
    XiValue *get = xi_value_new(f, b0, XI_INDEX_GET, &t_int, 2);
    get->args[0] = dyn_map;
    get->args[1] = idx;
    xi_block_set_return(b0, get);

    xi_escape_analyze(f);
    xi_stack_alloc_rewrite(f);

    ASSERT_EQ(typed_arr->op, XI_ARRAY_NEW, "typed array should keep metadata and stay heap");
    ASSERT_EQ(typed_arr->escape, XI_ESC_ARG, "skipped typed array should re-enter ARC");
    ASSERT_EQ(typed_map->op, XI_MAP_NEW, "typed map should keep metadata and stay heap");
    ASSERT_EQ(typed_map->escape, XI_ESC_ARG, "skipped typed map should re-enter ARC");
    ASSERT_EQ(weak_set->op, XI_SET_NEW, "weak set should keep weak flag and stay heap");
    ASSERT_EQ(weak_set->escape, XI_ESC_ARG, "skipped weak set should re-enter ARC");
    ASSERT_EQ(dyn_map->op, XI_MAP_NEW, "dynamic-capacity map should stay heap");
    ASSERT_EQ(dyn_map->escape, XI_ESC_ARG, "skipped dynamic map should re-enter ARC");

    xi_arc_insert(f);
    ASSERT_EQ(count_ops(f, XI_RELEASE) >= 4, 1,
              "skipped NO_ESCAPE heap allocations should receive death drops");
    xi_func_free(f);
}

static void test_stack_alloc_direct_closure(void) {
    XiFunc *f = make_func("stack_closure", &t_int);
    XiBlock *b0 = f->entry;

    XiFunc *child = make_func("child", &t_int);
    child->parent_func = f;
    XiValue *one = xi_const_int(child, child->entry, 1, &t_int);
    xi_block_set_return(child->entry, one);

    f->children = (XiFunc **) xr_calloc(1, sizeof(XiFunc *));
    XR_CHECK(f->children != NULL, "test_stack_alloc_direct_closure: child allocation failed");
    f->children[0] = child;
    f->children_cap = 1;
    f->nchildren = 1;

    XiValue *closure = xi_value_new(f, b0, XI_CLOSURE_NEW, &t_func, 0);
    closure->aux = child;
    XiValue *call = xi_value_new(f, b0, XI_CALL, &t_int, 1);
    call->args[0] = closure;
    xi_block_set_return(b0, call);

    xi_escape_analyze(f);
    ASSERT_EQ(closure->escape, XI_ESC_NONE, "direct-call callee closure should not escape");
    xi_stack_alloc_rewrite(f);
    ASSERT_EQ(closure->op, XI_STACK_ALLOC, "direct-call closure should become STACK_ALLOC");
    ASSERT_EQ(closure->aux_int, XI_CLOSURE_NEW, "closure STACK_ALLOC should preserve original op");
    xi_arc_insert(f);
    ASSERT_EQ(count_ops(f, XI_RELEASE) >= 1, 1,
              "stack closure should be destructed at its death point");
    xi_func_free(f);
}

static void test_stack_alloc_escaping_stays(void) {
    /* Returned array (ARG_ESCAPE) should NOT be rewritten */
    XiFunc *f = make_func("stack_esc", &t_array);
    XiBlock *b0 = f->entry;

    XiValue *arr = xi_value_new(f, b0, XI_ARRAY_NEW, &t_array, 0);
    xi_block_set_return(b0, arr);

    xi_escape_analyze(f);
    xi_stack_alloc_rewrite(f);

    ASSERT_EQ(arr->op, XI_ARRAY_NEW, "ARG_ESCAPE array should stay as ARRAY_NEW");
    xi_func_free(f);
}

/* ========== Main ========== */

int main(void) {
    test_heap_alloc_check();
    test_generated_use_escape_policy();
    test_lattice_join();
    test_local_no_escape();
    test_return_escape();
    test_store_field_escape();
    test_chan_send_escape();
    test_set_shared_escape();
    test_call_arg_escape();
    test_index_set_container_no_escape();
    test_arc_no_escape_skipped();
    test_arc_return_gets_retain();
    test_arc_heap_gets_retain_release();
    test_arc_elim_keeps_borrowed_single_consumer_retain();
    test_arc_many_consume_sites();
    test_stack_alloc_local_array();
    test_stack_alloc_local_plain_map_set();
    test_stack_alloc_skips_metadata_or_dynamic_capacity();
    test_stack_alloc_direct_closure();
    test_stack_alloc_escaping_stays();

    printf("\n=== test_xi_escape: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
