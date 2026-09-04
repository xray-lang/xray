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
#include "../../../src/ir/xi_analysis.h"
#include "../../../src/ir/xi_escape.h"
#include "../../../src/ir/xi_arc.h"
#include "../../../src/ir/xi_arc_verify.h"
#include "../../../src/ir/xi_builtin_map_entry_iterator_shape.h"
#include "../../../src/ir/xi_effect.h"
#include "../../../src/ir/xi_own.h"
#include "../../../src/ir/xi_module.h"
#include "../../../src/ir/xi_verify.h"
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
static XrType t_span = {.kind = XR_KIND_SLICE, .id = 7, .frozen = true};
static XrType t_bool = {.kind = XR_KIND_BOOL, .id = 8, .frozen = true};
static XrType t_unit = {.kind = XR_KIND_UNIT, .id = 10, .frozen = true};
static XrType t_stringbuilder = {
    .kind = XR_KIND_INSTANCE,
    .id = 9,
    .frozen = true,
    .instance = {.class_name = "StringBuilder"},
};
static XrType t_string = {.kind = XR_KIND_STRING, .id = 11, .frozen = true};
static XrType t_json = {.kind = XR_KIND_JSON, .id = 12, .frozen = true};
static XrType t_iterator = {
    .kind = XR_KIND_INSTANCE,
    .id = 13,
    .frozen = true,
    .instance = {.class_name = "Iterator"},
};
static XrType t_custom_iterable = {
    .kind = XR_KIND_INSTANCE,
    .id = 14,
    .frozen = true,
    .instance = {.class_name = "CustomIterable"},
};
static XrType t_value_struct = {
    .kind = XR_KIND_INSTANCE,
    .id = 15,
    .frozen = true,
    .is_value_type = true,
    .instance = {.class_name = "OwnedValue"},
};

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
    ASSERT_EQ(xi_op_is_heap_alloc(XI_OBJECT_NEW), 1, "OBJECT_NEW is heap alloc");
    ASSERT_EQ(xi_op_is_heap_alloc(XI_CLOSURE_NEW), 1, "CLOSURE_NEW is heap alloc");
    ASSERT_EQ(xi_op_is_heap_alloc(XI_STR_CONCAT), 1, "STR_CONCAT is heap alloc");
    /* Non-alloc ops */
    ASSERT_EQ(xi_op_is_heap_alloc(XI_ADD), 0, "ADD is not heap alloc");
    ASSERT_EQ(xi_op_is_heap_alloc(XI_CONST), 0, "CONST is not heap alloc");
    ASSERT_EQ(xi_op_is_heap_alloc(XI_CALL), 0, "CALL is not heap alloc");
    ASSERT_EQ(xi_op_is_heap_alloc(XI_AGG_NEW), 0, "AGG_NEW is not escape heap alloc");
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

static int count_target_ops(const XiFunc *f, uint16_t op, const XiValue *target) {
    int count = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *value = blk->values[i];
            if (value && value->op == op && value->nargs >= 1 && value->args[0] == target)
                count++;
        }
    }
    return count;
}

static int count_block_target_ops(const XiBlock *block, uint16_t op, const XiValue *target) {
    int count = 0;
    for (uint32_t i = 0; block && i < block->nvalues; i++) {
        XiValue *value = block->values[i];
        if (value && value->op == op && value->nargs >= 1 && value->args[0] == target)
            count++;
    }
    return count;
}

static void test_arc_no_escape_still_released_without_stack_rewrite(void) {
    /* A NO_ESCAPE heap allocation is still a HEAP allocation until something
     * actually rewrites it. escape + ARC alone (the VM's pipeline: see
     * xi_pipeline.c, where xi_stack_alloc_rewrite is gated on
     * run_backend_lower) leaves XI_ARRAY_NEW allocating on the heap, so it
     * needs exactly one release at its death point.
     *
     * ARC used to skip NO_ESCAPE heap allocations outright, on the assumption
     * that stack_alloc_rewrite would claim them. On the VM that pass never
     * runs — and it is also what promotes an allocation it CANNOT stack
     * allocate back to ESC_ARG — so those values were left on the heap with
     * nothing to release them. 2M non-escaping closures cost 143 MB max RSS;
     * 2M non-escaping array literals cost 301 MB. The regression pins this failure mode.
     *
     * The stack-allocated counterpart is asserted by
     * test_arc_stack_alloc_not_released below. */
    XiFunc *f = make_func("arc_local", &t_int);
    XiBlock *b0 = f->entry;

    XiValue *arr = xi_value_new(f, b0, XI_ARRAY_NEW, &t_array, 0);
    XiValue *idx = xi_const_int(f, b0, 0, &t_int);
    XiValue *get = xi_value_new(f, b0, XI_INDEX_GET, &t_int, 2);
    get->args[0] = arr;
    get->args[1] = idx;
    xi_block_set_return(b0, get);

    xi_escape_analyze(f);
    ASSERT_EQ(arr->escape, (uint8_t) XI_ESC_NONE, "local array does not escape");
    xi_arc_insert(f);

    ASSERT_EQ(count_ops(f, XI_RETAIN), 0, "single-use array is never dup'd");
    ASSERT_EQ(count_ops(f, XI_RELEASE), 1, "heap-allocated array is released once");
    xi_func_free(f);
}

static void test_arc_stack_alloc_not_released(void) {
    /* Once stack_alloc_rewrite HAS claimed the allocation (the AOT pipeline),
     * it has frame lifetime and must not be released. This is the assertion
     * that "NO_ESCAPE means no release" was standing in for; keyed on what the
     * value IS, not on what a later pass might do to it.
     *
     * A map with a constant capacity is used because XI_ARRAY_NEW is one of
     * the ops stack_alloc_rewrite explicitly declines (a stack array would
     * start at length 0 and trap on the literal's index-sets), and a declined
     * allocation is promoted to ESC_ARG — which is exactly the released case
     * above. */
    XiFunc *f = make_func("arc_stack", &t_int);
    XiBlock *b0 = f->entry;

    XiValue *cap = xi_const_int(f, b0, 0, &t_int);
    XiValue *map = xi_value_new(f, b0, XI_MAP_NEW, &t_map, 1);
    map->args[0] = cap;
    XiValue *ret = xi_const_int(f, b0, 0, &t_int);
    xi_block_set_return(b0, ret);

    xi_escape_analyze(f);
    xi_stack_alloc_rewrite(f);
    ASSERT_EQ(map->op, (uint16_t) XI_STACK_ALLOC, "const-capacity map is stack-allocated");
    xi_arc_insert(f);

    ASSERT_EQ(count_ops(f, XI_RETAIN), 0, "stack-allocated map has 0 retains");
    ASSERT_EQ(count_ops(f, XI_RELEASE), 0, "stack-allocated map has 0 releases");
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

/* ========== Test: ARC elim keeps retain that creates an owned alias ====== */

static void test_arc_elim_keeps_retain_before_sole_borrowing_alias(void) {
    XiFunc *f = make_func("arc_owned_alias_from_borrow", &t_int);
    XiBlock *b0 = f->entry;

    XiValue *source = xi_value_new(f, b0, XI_ARRAY_NEW, &t_array, 0);
    source->escape = XI_ESC_ARG;

    XiValue *retain = xi_value_new(f, b0, XI_RETAIN, &t_any, 1);
    retain->args[0] = source;

    XiValue *alias = xi_value_new(f, b0, XI_AS, &t_array, 1);
    alias->args[0] = source;
    alias->aux_int = ((int64_t) (uint32_t) -1 << 1);

    XiValue *len = xi_value_new(f, b0, XI_LEN, &t_int, 1);
    len->args[0] = alias;
    xi_block_set_return(b0, len);

    xi_arc_insert(f);
    ASSERT_EQ(count_ops(f, XI_RETAIN), 1,
              "owned alias requires an explicit retain before borrowing cast");
    xi_arc_elim(f);
    ASSERT_EQ(count_ops(f, XI_RETAIN), 1,
              "ARC elim must not remove retain whose sole real use borrows");
    xi_func_free(f);
}

/* ========== Test: ARC elim keeps a repeated loop transfer ============== */

static void test_arc_elim_keeps_single_consumer_retain_inside_loop(void) {
    XiFunc *f = make_func("arc_loop_single_consume", &t_int);
    XiBlock *entry = f->entry;
    XiBlock *header = xi_block_new(f);
    XiBlock *body = xi_block_new(f);
    XiBlock *exit = xi_block_new(f);
    header->sealed = true;
    body->sealed = true;
    exit->sealed = true;

    XiValue *value = xi_param(f, entry, 0, &t_array);
    set_single_param(f, value);
    xi_block_set_jump(entry, header);

    XiValue *condition = xi_const_bool(f, header, true, &t_bool);
    xi_block_set_if(header, condition, body, exit);

    XiValue *consume = xi_value_new(f, body, XI_SET_SHARED, &t_any, 1);
    consume->args[0] = value;
    consume->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
    xi_block_set_jump(body, header);

    XiValue *zero = xi_const_int(f, exit, 0, &t_int);
    xi_block_set_return(exit, zero);

    xi_escape_analyze(f);
    xi_arc_insert(f);
    ASSERT_EQ(count_ops(f, XI_RETAIN), 1, "loop consume needs one retain executed per iteration");
    xi_arc_elim(f);
    ASSERT_EQ(count_ops(f, XI_RETAIN), 1,
              "ARC elim must keep a single-consumer retain inside a loop");
    xi_func_free(f);
}

static void test_arc_elim_removes_retain_for_loop_local_owner(void) {
    XiFunc *f = make_func("arc_loop_local_single_consume", &t_int);
    XiBlock *entry = f->entry;
    XiBlock *header = xi_block_new(f);
    XiBlock *body = xi_block_new(f);
    XiBlock *exit = xi_block_new(f);
    header->sealed = true;
    body->sealed = true;
    exit->sealed = true;

    xi_block_set_jump(entry, header);
    XiValue *condition = xi_const_bool(f, header, true, &t_bool);
    xi_block_set_if(header, condition, body, exit);

    XiValue *fresh = xi_value_new(f, body, XI_ARRAY_NEW, &t_array, 0);
    XiValue *retain = xi_value_new(f, body, XI_RETAIN, &t_unit, 1);
    retain->args[0] = fresh;
    retain->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
    XiValue *consume = xi_value_new(f, body, XI_SET_SHARED, &t_any, 1);
    consume->args[0] = fresh;
    consume->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
    xi_block_set_jump(body, header);

    XiValue *zero = xi_const_int(f, exit, 0, &t_int);
    xi_block_set_return(exit, zero);

    xi_escape_analyze(f);
    ASSERT_EQ(count_ops(f, XI_RETAIN), 1, "loop-local owner starts with one retain");
    xi_arc_elim(f);
    ASSERT_EQ(count_ops(f, XI_RETAIN), 0,
              "loop-local owner forwards without repeated retain overhead");
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

/* ========== Test: owner-forward remains an ARC-tracked owner ============ */

static void test_arc_owner_forward_tracks_repeated_consumes(void) {
    XiFunc *f = make_func("arc_owner_forward_repeated_consumes", &t_int);
    XiBlock *b0 = f->entry;

    XiValue *source = xi_value_new(f, b0, XI_ARRAY_NEW, &t_array, 0);
    source->escape = XI_ESC_ARG;
    XiValue *forward = xi_value_new(f, b0, XI_COPY, &t_array, 1);
    forward->args[0] = source;

    XiValue *first = xi_value_new(f, b0, XI_SET_SHARED, &t_any, 1);
    first->args[0] = forward;
    first->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
    XiValue *second = xi_value_new(f, b0, XI_SET_SHARED, &t_any, 1);
    second->args[0] = forward;
    second->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;

    XiValue *zero = xi_const_int(f, b0, 0, &t_int);
    xi_block_set_return(b0, zero);

    xi_arc_insert(f);

    ASSERT_EQ(forward->op, XI_OWNER_FORWARD, "escaping reference copy should become owner-forward");
    ASSERT_EQ(xi_op_result_ownership(XI_OWNER_FORWARD), XI_GEN_RESULT_OWNERSHIP_OWNED,
              "owner-forward result must remain an ARC-tracked owner");
    ASSERT_EQ(xi_own_value_arg_is_consuming(forward, 0), true,
              "owner-forward must consume its source owner");
    ASSERT_EQ(count_ops(f, XI_RETAIN), 1,
              "two sequential consumes of a forwarded owner need one retain");

    XiArcVerifyReport rep;
    ASSERT_EQ(xi_arc_verify(f, &rep), true,
              "tracked owner-forward output must satisfy the ARC verifier");
    xi_func_free(f);
}

static void test_arc_tracks_owner_forward_through_phi(void) {
    XiFunc *f = make_func("arc_owner_forward_phi_repeated_consumes", &t_int);
    XiBlock *entry = f->entry;
    XiBlock *join = xi_block_new(f);
    join->sealed = true;

    XiValue *source = xi_value_new(f, entry, XI_ARRAY_NEW, &t_array, 0);
    source->escape = XI_ESC_ARG;
    XiValue *forward = xi_value_new(f, entry, XI_COPY, &t_array, 1);
    forward->args[0] = source;
    xi_block_set_jump(entry, join);

    XiPhi *merged = xi_phi_new(f, join, &t_array, join->npreds);
    merged->value.args[0] = forward;
    XiValue *first = xi_value_new(f, join, XI_SET_SHARED, &t_any, 1);
    first->args[0] = &merged->value;
    first->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
    XiValue *second = xi_value_new(f, join, XI_SET_SHARED, &t_any, 1);
    second->args[0] = &merged->value;
    second->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
    XiValue *zero = xi_const_int(f, join, 0, &t_int);
    xi_block_set_return(join, zero);

    xi_arc_insert(f);

    ASSERT_EQ(forward->op, XI_OWNER_FORWARD, "phi input copy should become owner-forward");
    ASSERT_EQ(count_ops(f, XI_RETAIN), 1, "two consumes after an owning phi need one retain");
    XiArcVerifyReport rep;
    ASSERT_EQ(xi_arc_verify(f, &rep), true, "owner-forward must stay live across its phi transfer");
    xi_func_free(f);
}

static void test_arc_phi_move_drops_owner_on_sibling_edge(void) {
    XiFunc *f = make_func("arc_phi_move_sibling_edge_drop", &t_int);
    XiBlock *entry = f->entry;
    XiBlock *join = xi_block_new(f);
    XiBlock *dead = xi_block_new(f);
    join->sealed = true;
    dead->sealed = true;

    XiValue *source = xi_value_new(f, entry, XI_ARRAY_NEW, &t_array, 0);
    source->escape = XI_ESC_ARG;
    XiValue *cond = xi_const_bool(f, entry, true, &t_bool);
    xi_block_set_if(entry, cond, join, dead);

    XiPhi *merged = xi_phi_new(f, join, &t_array, join->npreds);
    merged->value.args[0] = source;
    XiValue *consume = xi_value_new(f, join, XI_SET_SHARED, &t_any, 1);
    consume->args[0] = &merged->value;
    consume->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
    xi_block_set_return(join, xi_const_int(f, join, 1, &t_int));
    xi_block_set_return(dead, xi_const_int(f, dead, 0, &t_int));

    xi_arc_insert(f);

    ASSERT_EQ(count_ops(f, XI_RETAIN), 0,
              "single phi consume should move the incoming owner without retain");
    ASSERT_EQ(count_ops(f, XI_RELEASE), 1,
              "the sibling edge which does not execute the phi must drop the owner");
    ASSERT_EQ(dead->values[0]->op, XI_RELEASE,
              "the sibling-edge drop must execute only in the non-phi successor");
    XiArcVerifyReport rep;
    ASSERT_EQ(xi_arc_verify(f, &rep), true,
              "edge-specific phi transfer and sibling drop must verify");
    xi_func_free(f);
}

static void test_arc_frame_pinned_distinct_phi_releases_old_owner(void) {
    XiFunc *f = make_func("arc_frame_pinned_distinct_phi", &t_int);
    XiBlock *entry = f->entry;
    XiBlock *selected = xi_block_new(f);
    XiBlock *fallback = xi_block_new(f);
    XiBlock *join = xi_block_new(f);

    XiValue *source = xi_value_new(f, entry, XI_ARRAY_NEW, &t_array, 0);
    source->escape = XI_ESC_ARG;
    (void) xi_value_new(f, entry, XI_YIELD, &t_unit, 0);
    XiValue *condition = xi_const_bool(f, entry, true, &t_bool);
    xi_block_set_if(entry, condition, selected, fallback);

    xi_block_set_jump(selected, join);
    XiValue *alternate = xi_value_new(f, fallback, XI_ARRAY_NEW, &t_array, 0);
    alternate->escape = XI_ESC_ARG;
    xi_block_set_jump(fallback, join);

    XiPhi *merged = xi_phi_new(f, join, &t_array, join->npreds);
    merged->value.args[0] = source;
    merged->value.args[1] = alternate;
    XiValue *consume = xi_value_new(f, join, XI_SET_SHARED, &t_any, 1);
    consume->args[0] = &merged->value;
    consume->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
    xi_block_set_return(join, xi_const_int(f, join, 0, &t_int));
    selected->sealed = true;
    fallback->sealed = true;
    join->sealed = true;

    xi_arc_insert(f);
    xi_arc_elim(f);

    ASSERT_EQ(count_target_ops(f, XI_RETAIN, source), 1,
              "distinct phi retain must survive ARC elimination for the new phi owner");
    ASSERT_EQ(count_target_ops(f, XI_RELEASE, source), 2,
              "old frame owner must drop on both the distinct-phi and sibling edges");
    XiArcVerifyReport rep;
    ASSERT_EQ(xi_arc_verify(f, &rep), true,
              "frame-pinned distinct-phi transfer must satisfy ARC verification");
    xi_func_free(f);
}

static void test_arc_call_result_forward_retains_across_sibling_borrow(void) {
    XiFunc *f = make_func("arc_call_result_forward_sibling_borrow", &t_int);
    XiBlock *entry = f->entry;
    XiBlock *join = xi_block_new(f);
    XiBlock *borrow = xi_block_new(f);
    join->sealed = true;
    borrow->sealed = true;

    XiValue *source = xi_value_new(f, entry, XI_CALL_BUILTIN, &t_array, 0);
    source->escape = XI_ESC_ARG;
    XiValue *forward = xi_value_new(f, entry, XI_COPY, &t_array, 1);
    forward->args[0] = source;
    XiValue *cond = xi_const_bool(f, entry, true, &t_bool);
    xi_block_set_if(entry, cond, join, borrow);

    XiPhi *merged = xi_phi_new(f, join, &t_array, join->npreds);
    merged->value.args[0] = forward;
    XiValue *consume = xi_value_new(f, join, XI_SET_SHARED, &t_any, 1);
    consume->args[0] = &merged->value;
    consume->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
    xi_block_set_return(join, xi_const_int(f, join, 1, &t_int));

    XiValue *length = xi_value_new(f, borrow, XI_LEN, &t_int, 1);
    length->args[0] = source;
    xi_block_set_return(borrow, xi_const_int(f, borrow, 0, &t_int));

    xi_arc_insert(f);

    ASSERT_EQ(forward->op, XI_OWNER_FORWARD,
              "phi-bound call-result copy should become owner-forward");
    ASSERT_EQ(count_ops(f, XI_RETAIN), 1,
              "call result must retain before a forward when a sibling still borrows source");
    ASSERT_EQ(borrow->values[0]->op, XI_RELEASE,
              "sibling path must release only the forwarded reference before borrowing source");
    XiArcVerifyReport rep;
    ASSERT_EQ(xi_arc_verify(f, &rep), true,
              "call-result forward with a sibling borrow must satisfy ARC verification");
    xi_func_free(f);
}

static void test_arc_call_result_retain_before_same_block_phi_consume(void) {
    XiFunc *f = make_func("arc_call_result_same_block_phi_consume", &t_int);
    XiBlock *entry = f->entry;
    XiBlock *join = xi_block_new(f);
    join->sealed = true;

    XiValue *source = xi_value_new(f, entry, XI_CALL_BUILTIN, &t_array, 0);
    source->escape = XI_ESC_ARG;
    XiValue *store = xi_value_new(f, entry, XI_SET_SHARED, &t_any, 1);
    store->args[0] = source;
    store->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
    xi_block_set_jump(entry, join);

    XiPhi *merged = xi_phi_new(f, join, &t_array, join->npreds);
    merged->value.args[0] = source;
    XiValue *consume = xi_value_new(f, join, XI_SET_SHARED, &t_any, 1);
    consume->args[0] = &merged->value;
    consume->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
    xi_block_set_return(join, xi_const_int(f, join, 0, &t_int));

    xi_arc_insert(f);

    ASSERT_EQ(count_ops(f, XI_RETAIN), 2,
              "each consume of an alias-uncertain call result needs its own retain");
    ASSERT_EQ(entry->values[1]->op, XI_RETAIN,
              "retain must execute immediately before the first consuming store");
    XiArcVerifyReport rep;
    ASSERT_EQ(xi_arc_verify(f, &rep), true,
              "same-block consume followed by phi-edge consume must verify");
    xi_func_free(f);
}

static void test_arc_frame_pinned_phi_edge_drops_with_live_sibling(void) {
    XiFunc *f = make_func("arc_frame_pinned_phi_live_sibling", &t_int);
    XiBlock *entry = f->entry;
    XiBlock *choice = xi_block_new(f);
    XiBlock *alternate = xi_block_new(f);
    XiBlock *join = xi_block_new(f);
    XiBlock *keep = xi_block_new(f);

    XiValue *source = xi_value_new(f, entry, XI_ARRAY_NEW, &t_array, 0);
    source->escape = XI_ESC_ARG;
    (void) xi_value_new(f, entry, XI_YIELD, &t_unit, 0);
    XiValue *outer = xi_const_bool(f, entry, true, &t_bool);
    xi_block_set_if(entry, outer, choice, alternate);

    XiValue *inner = xi_const_bool(f, choice, true, &t_bool);
    xi_block_set_if(choice, inner, join, keep);
    XiValue *other = xi_value_new(f, alternate, XI_ARRAY_NEW, &t_array, 0);
    other->escape = XI_ESC_ARG;
    xi_block_set_jump(alternate, join);

    XiPhi *merged = xi_phi_new(f, join, &t_array, join->npreds);
    for (uint16_t i = 0; i < join->npreds; i++)
        merged->value.args[i] = join->preds[i] == choice ? source : other;
    XiValue *consume = xi_value_new(f, join, XI_SET_SHARED, &t_any, 1);
    consume->args[0] = &merged->value;
    consume->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
    xi_block_set_return(join, xi_const_int(f, join, 0, &t_int));

    XiValue *length = xi_value_new(f, keep, XI_LEN, &t_int, 1);
    length->args[0] = source;
    xi_block_set_return(keep, length);
    choice->sealed = true;
    alternate->sealed = true;
    join->sealed = true;
    keep->sealed = true;

    xi_arc_insert(f);
    xi_arc_elim(f);

    XiBlock *selected_edge = NULL;
    for (uint16_t i = 0; i < join->npreds; i++) {
        if (merged->value.args[i] == source) {
            selected_edge = join->preds[i];
            break;
        }
    }
    ASSERT_EQ(selected_edge != NULL && selected_edge != choice, true,
              "frame-pinned distinct PHI must use a dedicated selected edge");
    ASSERT_EQ(count_target_ops(f, XI_RETAIN, source), 1,
              "selected PHI edge must retain the new owner while a sibling uses the old slot");
    bool edge_retain = false;
    bool edge_release = false;
    for (uint32_t i = 0; selected_edge && i < selected_edge->nvalues; i++) {
        XiValue *value = selected_edge->values[i];
        edge_retain |= value && value->op == XI_RETAIN && value->nargs == 1 &&
                       value->args[0] == source;
        edge_release |= value && value->op == XI_RELEASE && value->nargs == 1 &&
                        value->args[0] == source;
    }
    ASSERT_EQ(edge_retain && edge_release, true,
              "selected PHI edge must retain the new owner and release the old frame owner");
    XiArcVerifyReport report;
    ASSERT_EQ(xi_arc_verify(f, &report), true,
              "path-specific frame PHI transfer must satisfy ARC verification");
    xi_func_free(f);
}

static void test_arc_reuses_split_edge_for_multiple_phi_consumes(void) {
    XiFunc *f = make_func("arc_multiple_phi_consumes_one_edge", &t_int);
    XiBlock *entry = f->entry;
    XiBlock *other = xi_block_new(f);
    XiBlock *join = xi_block_new(f);
    other->sealed = true;
    join->sealed = true;

    XiValue *source = xi_value_new(f, entry, XI_ARRAY_NEW, &t_array, 0);
    source->escape = XI_ESC_ARG;
    (void) xi_value_new(f, entry, XI_YIELD, &t_unit, 0);
    XiValue *condition = xi_const_bool(f, entry, true, &t_bool);
    xi_block_set_if(entry, condition, other, join);

    XiValue *alternate1 = xi_value_new(f, other, XI_ARRAY_NEW, &t_array, 0);
    XiValue *alternate2 = xi_value_new(f, other, XI_ARRAY_NEW, &t_array, 0);
    alternate1->escape = XI_ESC_ARG;
    alternate2->escape = XI_ESC_ARG;
    xi_block_set_jump(other, join);

    XiPhi *first = xi_phi_new(f, join, &t_array, join->npreds);
    XiPhi *second = xi_phi_new(f, join, &t_array, join->npreds);
    for (uint16_t i = 0; i < join->npreds; i++) {
        bool direct = join->preds[i] == entry;
        first->value.args[i] = direct ? source : alternate1;
        second->value.args[i] = direct ? source : alternate2;
    }
    XiValue *store1 = xi_value_new(f, join, XI_SET_SHARED, &t_any, 1);
    store1->args[0] = &first->value;
    store1->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
    XiValue *store2 = xi_value_new(f, join, XI_SET_SHARED, &t_any, 1);
    store2->args[0] = &second->value;
    store2->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
    xi_block_set_return(join, xi_const_int(f, join, 0, &t_int));

    xi_arc_insert(f);

    XiBlock *direct_edge = NULL;
    for (uint16_t i = 0; i < join->npreds; i++) {
        if (first->value.args[i] == source) {
            direct_edge = join->preds[i];
            break;
        }
    }
    ASSERT_EQ(direct_edge != NULL && direct_edge != entry, true,
              "multiple PHI consumes must share a dedicated direct-edge block");
    ASSERT_EQ(direct_edge && direct_edge->npreds == 1 && direct_edge->preds[0] == entry, true,
              "the shared PHI edge block must preserve the original predecessor");
    ASSERT_EQ(count_target_ops(f, XI_RETAIN, source), 2,
              "each frame-pinned PHI consume needs a retain on the shared edge block");
    uint32_t edge_retains = 0;
    for (uint32_t i = 0; direct_edge && i < direct_edge->nvalues; i++) {
        XiValue *value = direct_edge->values[i];
        if (value && value->op == XI_RETAIN && value->nargs == 1 && value->args[0] == source)
            edge_retains++;
    }
    ASSERT_EQ(edge_retains, 2,
              "both PHI retains must remain executable on the direct edge");
    char error[512] = {0};
    ASSERT_EQ(xi_verify(f, error, sizeof(error)), true,
              "shared PHI edge split must preserve CFG and SSA verification");
    XiArcVerifyReport report;
    ASSERT_EQ(xi_arc_verify(f, &report), true,
              "shared PHI edge retains must satisfy the ARC verifier");
    xi_func_free(f);
}

static void test_arc_orders_adjacent_retain_before_release(void) {
    XiFunc *f = make_func("arc_adjacent_rc_order", &t_int);
    XiBlock *entry = f->entry;

    /* Model two SSA names that may coalesce to one VM register and hold the
     * same runtime object.  RC insertion walks are independent, so this is the
     * minimal post-insertion ordering shape that must be normalized. */
    XiValue *old_owner = xi_value_new(f, entry, XI_GET_SHARED, &t_array, 0);
    old_owner->escape = XI_ESC_GLOBAL;
    XiValue *incoming = xi_value_new(f, entry, XI_GET_SHARED, &t_array, 0);
    incoming->escape = XI_ESC_GLOBAL;
    XiValue *release = xi_value_new(f, entry, XI_RELEASE, &t_array, 1);
    release->args[0] = old_owner;
    release->flags = XI_FLAG_SIDE_EFFECT;
    XiValue *retain = xi_value_new(f, entry, XI_RETAIN, &t_array, 1);
    retain->args[0] = incoming;
    retain->flags = XI_FLAG_SIDE_EFFECT;
    xi_block_set_return(entry, xi_const_int(f, entry, 0, &t_int));

    xi_arc_insert(f);

    uint32_t retain_index = UINT32_MAX;
    uint32_t release_index = UINT32_MAX;
    for (uint32_t i = 0; i < entry->nvalues; i++) {
        if (entry->values[i] == retain)
            retain_index = i;
        if (entry->values[i] == release)
            release_index = i;
    }
    ASSERT_EQ(retain_index < release_index, true,
              "adjacent RC run must acquire replacement ownership before releasing old owner");
    xi_func_free(f);
}

static void test_arc_unknown_call_result_retains_before_single_consume(void) {
    XiFunc *f = make_func("arc_unknown_call_result_single_consume", &t_int);
    XiBlock *entry = f->entry;

    XiValue *source = xi_value_new(f, entry, XI_CALL_BUILTIN, &t_array, 0);
    source->escape = XI_ESC_ARG;
    XiValue *store = xi_value_new(f, entry, XI_SET_SHARED, &t_any, 1);
    store->args[0] = source;
    store->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
    xi_block_set_return(entry, xi_const_int(f, entry, 0, &t_int));

    xi_arc_insert(f);

    ASSERT_EQ(count_ops(f, XI_RETAIN), 1,
              "an alias-uncertain call result must retain before its sole consume");
    ASSERT_EQ(entry->values[1]->op, XI_RETAIN,
              "the ownership transfer retain must immediately precede the consume");
    ASSERT_EQ(count_ops(f, XI_RELEASE), 0,
              "an alias-uncertain call result must never receive a death release");
    XiArcVerifyReport rep;
    ASSERT_EQ(xi_arc_verify(f, &rep), true,
              "single-consume unknown call result must satisfy ARC verification");
    xi_func_free(f);
}

static void test_arc_resolved_callee_contract_overrides_callsite_borrow(void) {
    XiFunc *f = make_func("arc_resolved_callee_contract", &t_int);
    XiBlock *entry = f->entry;
    XiFunc *callee = make_func("owned_identity", &t_array);
    callee->parent_func = f;
    XiValue *parameter = xi_param(callee, callee->entry, 0, &t_array);
    set_single_param(callee, parameter);
    xi_block_set_return(callee->entry, parameter);

    f->children = (XiFunc **) xr_calloc(1, sizeof(*f->children));
    XR_CHECK(f->children != NULL, "test_xi_escape: child allocation failed");
    f->children[0] = callee;
    f->children_cap = 1;
    f->nchildren = 1;

    XiValue *closure = xi_value_new(f, entry, XI_CLOSURE_NEW, &t_func, 0);
    closure->aux = callee;
    XiValue *argument = xi_value_new(f, entry, XI_ARRAY_NEW, &t_array, 0);
    XiValue *call = xi_value_new(f, entry, XI_CALL, &t_array, 2);
    call->args[0] = closure;
    call->args[1] = argument;
    call->flags = XI_FLAG_SIDE_EFFECT;
    call->call_return_ownership.kind = XI_RETURN_OWNERSHIP_BORROWED_PARAM;
    call->call_return_ownership.param_index = 0;
    call->call_return_ownership.complete = true;
    XiValue *store = xi_value_new(f, entry, XI_SET_SHARED, &t_unit, 1);
    store->args[0] = call;
    store->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
    xi_block_set_return(entry, xi_const_int(f, entry, 0, &t_int));

    xi_arc_insert(f);

    ASSERT_EQ(callee->arc_borrow_sig->param_own[0], XI_OWN_OWNED,
              "resolved callee parameter mode must reach its fixed point");
    ASSERT_EQ(callee->arc_return_ownership.kind, XI_RETURN_OWNERSHIP_OWNED,
              "resolved callee must publish its owned return ABI");
    ASSERT_EQ(count_target_ops(f, XI_RELEASE, argument), 0,
              "caller must move the argument required by the resolved callee");
    ASSERT_EQ(count_target_ops(f, XI_RETAIN, call), 0,
              "owned resolved-call result moves directly into the shared slot");
    xi_func_free(f);
}

/* A local instance method is just as statically resolvable as a closure call.
 * ARC used to resolve only namespace methods while coroutine analysis carried a
 * separate local-method resolver.  The caller then treated this borrowed Array
 * parameter as an unresolved consuming argument, contradicting the SemanticPlan
 * call contract later in the pipeline.  Keep the declaration table and the
 * caller/callee ownership result in one fixture so either half drifting fails. */
static void test_arc_local_method_uses_resolved_borrow_contract(void) {
    static int class_identity;
    XrType receiver_type = {
        .kind = XR_KIND_INSTANCE,
        .id = 16,
        .frozen = true,
        .instance =
            {
                .class_name = "BorrowingReader",
                .class_ref = (struct XrClassInfo *) &class_identity,
            },
    };
    XiFunc *root = make_func("arc_local_method_module", &t_int);
    XiFunc *caller = make_func("invokeBorrow", &t_int);
    XiFunc *callee = make_func("borrowArray", &t_int);
    caller->parent_func = root;
    callee->parent_func = root;
    root->children = (XiFunc **) xr_calloc(2, sizeof(*root->children));
    XR_CHECK(root->children != NULL, "test_xi_escape: method child allocation failed");
    root->children[0] = caller;
    root->children[1] = callee;
    root->children_cap = root->nchildren = 2;

    XiValue *caller_receiver = xi_param(caller, caller->entry, 0, &receiver_type);
    set_single_param(caller, caller_receiver);
    callee->nparams = 2;
    callee->has_receiver = true;
    callee->receiver_mode = XR_PARAM_READ;
    callee->receiver_borrowed = true;
    callee->params = (XiValue **) xr_calloc(2, sizeof(*callee->params));
    XR_CHECK(callee->params != NULL, "test_xi_escape: method param allocation failed");
    callee->params[0] = xi_param(callee, callee->entry, 0, &receiver_type);
    callee->params[1] = xi_param(callee, callee->entry, 1, &t_array);
    xi_block_set_return(callee->entry, xi_const_int(callee, callee->entry, 0, &t_int));

    XiValue *argument = xi_value_new(caller, caller->entry, XI_ARRAY_NEW, &t_array, 0);
    XiValue *call = xi_value_new(caller, caller->entry, XI_CALL_METHOD, &t_int, 2);
    call->args[0] = caller_receiver;
    call->args[1] = argument;
    call->aux = (void *) "borrowArray";
    call->flags = XI_FLAG_SIDE_EFFECT;
    xi_block_set_return(caller->entry, call);
    xi_block_set_return(root->entry, xi_const_int(root, root->entry, 0, &t_int));

    XiClassMethod methods[2] = {
        {.name = "invokeBorrow", .is_static = false},
        {.name = "borrowArray", .is_static = false},
    };
    uint16_t child_indices[2] = {0, 1};
    XiClassData class_data = {
        .class_info = (struct XrClassInfo *) &class_identity,
        .class_name = "BorrowingReader",
        .methods = methods,
        .nmethod = 2,
        .child_idx = child_indices,
        .ninst = 2,
    };
    XiClassData *classes[1] = {&class_data};
    XiModule module = {
        .init = root,
        .classes = classes,
        .nclasses = 1,
    };
    root->module = &module;

    xi_arc_insert(root);

    ASSERT_EQ(callee->arc_borrow_sig && callee->arc_borrow_sig->valid &&
                  callee->arc_borrow_sig->nparams == 2,
              true, "local method must publish a complete ownership signature");
    ASSERT_EQ(callee->arc_borrow_sig->param_own[1], XI_OWN_BORROWED,
              "local method Array parameter must remain borrowed");
    ASSERT_EQ(count_target_ops(caller, XI_RETAIN, argument), 0,
              "borrowed local-method argument must not be retained for a consume");
    ASSERT_EQ(count_target_ops(caller, XI_RELEASE, argument), 1,
              "caller must retain ownership and release the borrowed method argument at death");
    root->module = NULL;
    xi_func_free(root);
}

/* Instance call operands and callee parameters both put the receiver at slot
 * zero. An explicit parameter must therefore keep its index instead of using
 * the +1 offset reserved for plain and namespace calls. If ARC reads the
 * receiver's borrowed contract for payload slot one, it releases the payload
 * after the call even though the method transferred it into shared storage. */
static void test_arc_local_method_maps_owned_parameter_after_receiver(void) {
    static int class_identity;
    XrType receiver_type = {
        .kind = XR_KIND_INSTANCE,
        .id = 17,
        .frozen = true,
        .instance =
            {
                .class_name = "OwningWriter",
                .class_ref = (struct XrClassInfo *) &class_identity,
            },
    };
    XiFunc *root = make_func("arc_owned_method_module", &t_int);
    XiFunc *caller = make_func("invokeStore", &t_int);
    XiFunc *callee = make_func("storeArray", &t_int);
    caller->parent_func = root;
    callee->parent_func = root;
    root->children = (XiFunc **) xr_calloc(2, sizeof(*root->children));
    XR_CHECK(root->children != NULL, "test_xi_escape: owned method child allocation failed");
    root->children[0] = caller;
    root->children[1] = callee;
    root->children_cap = root->nchildren = 2;

    XiValue *caller_receiver = xi_param(caller, caller->entry, 0, &receiver_type);
    set_single_param(caller, caller_receiver);
    caller->has_receiver = true;
    caller->receiver_mode = XR_PARAM_READ;
    caller->receiver_borrowed = true;
    callee->nparams = 2;
    callee->has_receiver = true;
    callee->receiver_mode = XR_PARAM_READ;
    callee->receiver_borrowed = true;
    callee->params = (XiValue **) xr_calloc(2, sizeof(*callee->params));
    XR_CHECK(callee->params != NULL, "test_xi_escape: owned method param allocation failed");
    callee->params[0] = xi_param(callee, callee->entry, 0, &receiver_type);
    callee->params[1] = xi_param(callee, callee->entry, 1, &t_array);
    XiValue *store = xi_value_new(callee, callee->entry, XI_SET_SHARED, &t_unit, 1);
    store->args[0] = callee->params[1];
    store->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
    xi_block_set_return(callee->entry, xi_const_int(callee, callee->entry, 0, &t_int));

    XiValue *argument = xi_value_new(caller, caller->entry, XI_ARRAY_NEW, &t_array, 0);
    XiValue *call = xi_value_new(caller, caller->entry, XI_CALL_METHOD, &t_int, 2);
    call->args[0] = caller_receiver;
    call->args[1] = argument;
    call->aux = (void *) "storeArray";
    call->flags = XI_FLAG_SIDE_EFFECT;
    xi_block_set_return(caller->entry, call);
    xi_block_set_return(root->entry, xi_const_int(root, root->entry, 0, &t_int));

    XiClassMethod methods[2] = {
        {.name = "invokeStore", .is_static = false},
        {.name = "storeArray", .is_static = false},
    };
    uint16_t child_indices[2] = {0, 1};
    XiClassData class_data = {
        .class_info = (struct XrClassInfo *) &class_identity,
        .class_name = "OwningWriter",
        .methods = methods,
        .nmethod = 2,
        .child_idx = child_indices,
        .ninst = 2,
    };
    XiClassData *classes[1] = {&class_data};
    XiModule module = {
        .init = root,
        .classes = classes,
        .nclasses = 1,
    };
    root->module = &module;

    xi_arc_insert(root);

    ASSERT_EQ(callee->arc_borrow_sig && callee->arc_borrow_sig->valid &&
                  callee->arc_borrow_sig->param_own[0] == XI_OWN_BORROWED,
              true, "instance receiver must remain borrowed");
    ASSERT_EQ(callee->arc_borrow_sig->param_own[1], XI_OWN_OWNED,
              "stored explicit method parameter must be owned");
    ASSERT_EQ(count_target_ops(caller, XI_RELEASE, argument), 0,
              "caller must move an explicit parameter consumed by an instance method");
    root->module = NULL;
    xi_func_free(root);
}

static void test_arc_stringbuilder_builtin_result_is_fresh(void) {
    XiFunc *f = make_func("arc_fresh_stringbuilder", &t_int);
    XiBlock *entry = f->entry;

    XiValue *builder = xi_value_new(f, entry, XI_CALL_BUILTIN, &t_stringbuilder, 0);
    builder->aux = (void *) "StringBuilder";
    builder->flags = XI_FLAG_SIDE_EFFECT;
    xi_block_set_return(entry, xi_const_int(f, entry, 0, &t_int));

    xi_arc_insert(f);

    XiValue *release = NULL;
    for (uint32_t i = 0; i < entry->nvalues; i++) {
        if (entry->values[i]->op == XI_RELEASE) {
            release = entry->values[i];
            break;
        }
    }
    ASSERT_EQ(count_ops(f, XI_RELEASE), 1,
              "discarded StringBuilder() result must be released as a fresh owner");
    ASSERT_EQ(release != NULL && release->args[0] == builder, true,
              "StringBuilder death-point release must target the constructor result");
    xi_func_free(f);
}

static XiValue *find_release_for_value(const XiFunc *f, const XiValue *target) {
    for (uint32_t b = 0; b < f->nblocks; b++) {
        const XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *value = blk->values[i];
            if (value && value->op == XI_RELEASE && value->nargs == 1 && value->args[0] == target)
                return value;
        }
    }
    return NULL;
}

static void test_arc_value_clone_is_fresh_owner(void) {
    XiFunc *f = make_func("arc_value_clone_owner", &t_int);
    XiBlock *entry = f->entry;

    XiValue *source = xi_value_new(f, entry, XI_AGG_NEW, &t_value_struct, 0);
    XiValue *clone = xi_value_new(f, entry, XI_COPY, &t_value_struct, 1);
    clone->args[0] = source;
    clone->aux_int = XI_COPY_KIND_VALUE_CLONE;
    xi_block_set_return(entry, xi_const_int(f, entry, 0, &t_int));

    xi_arc_insert(f);

    ASSERT_EQ(find_release_for_value(f, source) != NULL, true,
              "discarded value-struct source must be released");
    ASSERT_EQ(find_release_for_value(f, clone) != NULL, true,
              "VALUE_CLONE must be released as an independent fresh owner");
    XiArcVerifyReport report;
    ASSERT_EQ(xi_arc_verify(f, &report), true,
              "independent ARC verifier must accept VALUE_CLONE ownership");
    xi_func_free(f);
}

static void assert_arc_iterator_method_result_is_fresh(XrType *receiver_type, uint16_t op,
                                                       const char *method,
                                                       XiMethodSymbolId method_symbol,
                                                       const char *message) {
    XiFunc *f = make_func("arc_fresh_iterator_method", &t_int);
    XiBlock *entry = f->entry;
    XiValue *receiver = xi_value_new(f, entry, XI_PARAM, receiver_type, 0);
    set_single_param(f, receiver);
    XiValue *iterator = xi_value_new(f, entry, op, &t_iterator, 1);
    iterator->args[0] = receiver;
    iterator->aux = (void *) method;
    iterator->aux_int = (int64_t) method_symbol << 1;
    iterator->flags = XI_FLAG_SIDE_EFFECT;
    xi_block_set_return(entry, xi_const_int(f, entry, 0, &t_int));

    xi_arc_insert(f);

    ASSERT_EQ(find_release_for_value(f, iterator) != NULL, true, message);
    xi_func_free(f);
}

static void test_arc_builtin_iterator_results_are_fresh(void) {
    assert_arc_iterator_method_result_is_fresh(&t_string, XI_CALL_METHOD, "runes",
                                               XI_METHOD_SYMBOL_RUNES,
                                               "discarded string.runes iterator must be released");
    assert_arc_iterator_method_result_is_fresh(
        &t_array, XI_CALL_METHOD_DIRECT, "entriesIterator", XI_METHOD_SYMBOL_ENTRIES_ITERATOR,
        "discarded array entries iterator must be released after direct lowering");
    assert_arc_iterator_method_result_is_fresh(&t_map, XI_CALL_METHOD, "iterator",
                                               XI_METHOD_SYMBOL_ITERATOR,
                                               "discarded map iterator must be released");
    assert_arc_iterator_method_result_is_fresh(&t_set, XI_CALL_METHOD, "iterator",
                                               XI_METHOD_SYMBOL_ITERATOR,
                                               "discarded set iterator must be released");
    assert_arc_iterator_method_result_is_fresh(&t_json, XI_CALL_METHOD, "entriesIterator",
                                               XI_METHOD_SYMBOL_ENTRIES_ITERATOR,
                                               "discarded Json entries iterator must be released");
}

static void test_map_entry_iterator_typed_shape(void) {
    XrType *entry_elements[2] = {&t_string, &t_int};
    XrType entry_type = {
        .kind = XR_KIND_TUPLE,
        .id = 101,
        .frozen = true,
        .tuple = {.element_types = entry_elements, .element_count = 2},
    };
    XrType *iterator_args[1] = {&entry_type};
    XrType iterator_type = {
        .kind = XR_KIND_INSTANCE,
        .id = 102,
        .frozen = true,
        .instance = {.class_name = "Iterator", .type_args = iterator_args, .type_arg_count = 1},
    };
    XrType map_type = {
        .kind = XR_KIND_MAP,
        .id = 103,
        .frozen = true,
        .map = {.key_type = &t_string, .value_type = &t_int},
    };
    XiFunc *f = make_func("map_entry_iterator_typed_shape", &t_int);
    XiBlock *entry = f->entry;
    XiValue *map = xi_value_new(f, entry, XI_PARAM, &map_type, 0);
    XiValue *iterator = xi_value_new(f, entry, XI_CALL_METHOD, &iterator_type, 1);
    XiValue *has_next = xi_value_new(f, entry, XI_CALL_METHOD, &t_bool, 1);
    XiValue *next = xi_value_new(f, entry, XI_CALL_METHOD, &entry_type, 1);
    iterator->args[0] = map;
    iterator->aux = (void *) "diagnostic-only";
    iterator->aux_int = (int64_t) XI_METHOD_SYMBOL_ENTRIES_ITERATOR << 1;
    has_next->args[0] = iterator;
    has_next->aux = (void *) "not-hasNext";
    has_next->aux_int = (int64_t) XI_METHOD_SYMBOL_HAS_NEXT << 1;
    next->args[0] = iterator;
    next->aux = (void *) "not-next";
    next->aux_int = (int64_t) XI_METHOD_SYMBOL_NEXT << 1;

    ASSERT_EQ(xi_map_entries_iterator_is_exact(iterator), true,
              "Map entriesIterator shape must come from stable symbol and exact K/V types");
    ASSERT_EQ(xi_map_entry_iterator_has_next_is_exact(has_next), true,
              "hasNext must consume the exact Map entry iterator family");
    ASSERT_EQ(xi_map_entry_iterator_next_is_exact(next), true,
              "next must return the exact (K,V) entry type");

    iterator->aux_int = (int64_t) XI_METHOD_SYMBOL_ITERATOR << 1;
    ASSERT_EQ(xi_map_entries_iterator_is_exact(iterator), false,
              "an ordinary iterator symbol must not acquire Map entry authority");
    iterator->aux_int = (int64_t) XI_METHOD_SYMBOL_ENTRIES_ITERATOR << 1;
    iterator_args[0] = &t_any;
    ASSERT_EQ(xi_map_entries_iterator_is_exact(iterator), false,
              "Iterator<unknown> must fail the exact Map entry shape");
    iterator_args[0] = &entry_type;
    next->type = &t_any;
    ASSERT_EQ(xi_map_entry_iterator_next_is_exact(next), false,
              "next returning unknown must fail the exact entry family");
    next->type = &entry_type;
    has_next->aux_int = (int64_t) XI_METHOD_SYMBOL_NEXT << 1;
    ASSERT_EQ(xi_map_entry_iterator_has_next_is_exact(has_next), false,
              "next's stable symbol must not acquire hasNext authority");
    has_next->aux_int = (int64_t) XI_METHOD_SYMBOL_HAS_NEXT << 1;
    ASSERT_EQ(xi_map_entry_iterator_next_is_exact(next), true,
              "restored exact Map entry shape must remain accepted");
    xi_func_free(f);
}

static void test_arc_generator_iterator_result_is_fresh(void) {
    XiFunc *f = make_func("arc_fresh_generator_iterator", &t_int);
    XiBlock *entry = f->entry;
    XiValue *callee = xi_value_new(f, entry, XI_PARAM, &t_func, 0);
    set_single_param(f, callee);
    XiValue *iterator = xi_value_new(f, entry, XI_GEN_CALL, &t_iterator, 1);
    iterator->args[0] = callee;
    iterator->flags = XI_FLAG_SIDE_EFFECT;
    xi_block_set_return(entry, xi_const_int(f, entry, 0, &t_int));

    xi_arc_insert(f);

    ASSERT_EQ(find_release_for_value(f, iterator) != NULL, true,
              "discarded generator iterator must be released as a fresh owner");
    xi_func_free(f);
}

static void test_arc_custom_iterator_method_stays_alias_uncertain(void) {
    XiFunc *f = make_func("arc_custom_iterator_alias", &t_int);
    XiBlock *entry = f->entry;
    XiValue *receiver = xi_value_new(f, entry, XI_PARAM, &t_custom_iterable, 0);
    set_single_param(f, receiver);
    XiValue *iterator = xi_value_new(f, entry, XI_CALL_METHOD, &t_iterator, 1);
    iterator->args[0] = receiver;
    iterator->aux = (void *) "iterator";
    iterator->aux_int = (int64_t) XI_METHOD_SYMBOL_ITERATOR << 1;
    iterator->flags = XI_FLAG_SIDE_EFFECT;
    xi_block_set_return(entry, xi_const_int(f, entry, 0, &t_int));

    xi_arc_insert(f);

    ASSERT_EQ(find_release_for_value(f, iterator) == NULL, true,
              "user-defined iterator method must remain alias-uncertain");
    xi_func_free(f);
}

static void test_arc_err_check_carries_cold_edge_cleanup(void) {
    XiFunc *f = make_func("arc_err_check_cleanup", &t_int);
    XiBlock *entry = f->entry;

    XiValue *builder = xi_value_new(f, entry, XI_CALL_BUILTIN, &t_stringbuilder, 0);
    builder->aux = (void *) "StringBuilder";
    builder->flags = XI_FLAG_SIDE_EFFECT;
    XiValue *fallible = xi_value_new(f, entry, XI_CALL_BUILTIN, &t_int, 0);
    fallible->aux = (void *) "fallible";
    fallible->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    XiValue *check = xi_value_new(f, entry, XI_ERR_CHECK, &t_unit, 0);
    check->flags = XI_FLAG_SIDE_EFFECT;
    XiValue *use = xi_value_new(f, entry, XI_LEN, &t_int, 1);
    use->args[0] = builder;
    xi_block_set_return(entry, use);

    xi_arc_insert(f);
    xi_arc_elim(f);
    int producer_uses_before = fallible->uses;
    xi_arc_attach_error_cleanups(f);

    ASSERT_EQ(check->nargs, 1, "unit ERR_CHECK should carry one cold-edge owner");
    ASSERT_EQ(fallible->uses, producer_uses_before,
              "ERR_CHECK must not force an otherwise-unused producer result to materialize");
    ASSERT_EQ(check->args[XI_ERR_CHECK_CLEANUP_ARG_BASE] == builder, true,
              "ERR_CHECK cold edge must drop the StringBuilder live on success");
    XiArcVerifyReport rep;
    ASSERT_EQ(xi_arc_verify(f, &rep), true,
              "error-edge cleanup operands must preserve the ARC contract");
    xi_func_free(f);
}

static void test_arc_err_check_without_throwing_source_stays_operand_free(void) {
    XiFunc *f = make_func("arc_dead_err_check", &t_int);
    XiBlock *entry = f->entry;

    XiValue *builder = xi_value_new(f, entry, XI_CALL_BUILTIN, &t_stringbuilder, 0);
    builder->aux = (void *) "StringBuilder";
    builder->flags = XI_FLAG_SIDE_EFFECT;
    XiValue *check = xi_value_new(f, entry, XI_ERR_CHECK, &t_unit, 0);
    check->flags = XI_FLAG_SIDE_EFFECT;
    XiValue *use = xi_value_new(f, entry, XI_LEN, &t_int, 1);
    use->args[0] = builder;
    xi_block_set_return(entry, use);

    xi_arc_insert(f);
    xi_arc_elim(f);
    xi_arc_attach_error_cleanups(f);

    ASSERT_EQ(check->nargs, 0,
              "ERR_CHECK left after a folded nothrow producer has no error-edge owners");
    xi_func_free(f);
}

static void test_arc_err_check_cleanup_requires_dominating_owner(void) {
    XiFunc *f = make_func("arc_err_check_cleanup_dominance", &t_int);
    XiBlock *entry = f->entry;
    XiBlock *left = xi_block_new(f);
    XiBlock *right = xi_block_new(f);
    XiBlock *join = xi_block_new(f);
    left->sealed = true;
    right->sealed = true;
    join->sealed = true;

    XiValue *cond = xi_const_bool(f, entry, true, &t_bool);
    xi_block_set_if(entry, cond, left, right);

    XiValue *left_builder = xi_value_new(f, left, XI_CALL_BUILTIN, &t_stringbuilder, 0);
    left_builder->aux = (void *) "StringBuilder";
    left_builder->flags = XI_FLAG_SIDE_EFFECT;
    xi_block_set_jump(left, join);

    XiValue *right_builder = xi_value_new(f, right, XI_CALL_BUILTIN, &t_stringbuilder, 0);
    right_builder->aux = (void *) "StringBuilder";
    right_builder->flags = XI_FLAG_SIDE_EFFECT;
    XiValue *fallible = xi_value_new(f, right, XI_CALL_BUILTIN, &t_int, 0);
    fallible->aux = (void *) "fallible";
    fallible->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    XiValue *check = xi_value_new(f, right, XI_ERR_CHECK, &t_unit, 0);
    check->flags = XI_FLAG_SIDE_EFFECT;
    xi_block_set_jump(right, join);

    XiPhi *merged = xi_phi_new(f, join, &t_stringbuilder, join->npreds);
    for (uint16_t i = 0; i < join->npreds; i++)
        merged->value.args[i] = join->preds[i] == left ? left_builder : right_builder;
    XiValue *length = xi_value_new(f, join, XI_LEN, &t_int, 1);
    length->args[0] = &merged->value;
    xi_block_set_return(join, length);

    xi_arc_insert(f);
    xi_arc_elim(f);
    xi_arc_attach_error_cleanups(f);

    ASSERT_EQ(check->nargs, 1,
              "error edge should clean only the branch-local owner available on its path");
    ASSERT_EQ(check->args[0] == right_builder, true,
              "non-dominating sibling owner must not enter ERR_CHECK cleanup metadata");
    XiArcVerifyReport rep;
    ASSERT_EQ(xi_arc_verify(f, &rep), true,
              "dominance-filtered error cleanup must preserve the ARC contract");
    xi_func_free(f);
}

static void test_arc_err_check_skips_boxed_ref_load_borrow(void) {
    XiFunc *f = make_func("arc_err_check_boxed_ref_borrow", &t_int);
    XiBlock *entry = f->entry;

    XiValue *place = xi_value_new(f, entry, XI_PARAM, &t_array, 0);
    set_single_param(f, place);
    XiValue *borrowed = xi_value_new(f, entry, XI_PLACE_LOAD, &t_array, 1);
    borrowed->args[0] = place;
    XiValue *boxed = xi_value_new(f, entry, XI_BOX, &t_array, 1);
    boxed->args[0] = borrowed;
    XiValue *fallible = xi_value_new(f, entry, XI_CALL_BUILTIN, &t_int, 0);
    fallible->aux = (void *) "fallible";
    fallible->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    XiValue *check = xi_value_new(f, entry, XI_ERR_CHECK, &t_unit, 0);
    check->flags = XI_FLAG_SIDE_EFFECT;
    XiValue *use = xi_value_new(f, entry, XI_LEN, &t_int, 1);
    use->args[0] = boxed;
    xi_block_set_return(entry, use);

    xi_arc_attach_error_cleanups(f);

    ASSERT_EQ(check->nargs, 0,
              "representation adapter over ref-loaded Array must stay borrowed on error edge");
    xi_func_free(f);
}

static void test_arc_err_check_keeps_boxed_fresh_owner(void) {
    XiFunc *f = make_func("arc_err_check_boxed_fresh_owner", &t_int);
    XiBlock *entry = f->entry;

    XiValue *builder = xi_value_new(f, entry, XI_CALL_BUILTIN, &t_stringbuilder, 0);
    builder->aux = (void *) "StringBuilder";
    builder->flags = XI_FLAG_SIDE_EFFECT;
    XiValue *boxed = xi_value_new(f, entry, XI_BOX, &t_stringbuilder, 1);
    boxed->args[0] = builder;
    XiValue *fallible = xi_value_new(f, entry, XI_CALL_BUILTIN, &t_int, 0);
    fallible->aux = (void *) "fallible";
    fallible->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    XiValue *check = xi_value_new(f, entry, XI_ERR_CHECK, &t_unit, 0);
    check->flags = XI_FLAG_SIDE_EFFECT;
    XiValue *use = xi_value_new(f, entry, XI_LEN, &t_int, 1);
    use->args[0] = boxed;
    xi_block_set_return(entry, use);

    xi_arc_attach_error_cleanups(f);

    ASSERT_EQ(check->nargs, 1,
              "representation adapter over a fresh owner must retain cold-edge cleanup");
    ASSERT_EQ(check->args[0] == boxed, true,
              "cold edge should release the live boxed representation of the fresh owner");
    xi_func_free(f);
}

/* ========== Test: borrowed Slice lifetime flows through a phi ========== */

static void test_arc_span_borrow_flows_through_phi(void) {
    XiFunc *f = make_func("arc_span_phi_borrow", &t_int);
    XiBlock *entry = f->entry;
    XiBlock *body = xi_block_new(f);
    body->sealed = true;

    XiValue *arr = xi_value_new(f, entry, XI_ARRAY_NEW, &t_array, 0);
    arr->escape = XI_ESC_ARG;
    XiValue *start = xi_const_int(f, entry, 0, &t_int);
    XiValue *end = xi_const_int(f, entry, 1, &t_int);
    XiValue *span = xi_value_new(f, entry, XI_SLICE, &t_span, 3);
    span->args[0] = arr;
    span->args[1] = start;
    span->args[2] = end;
    xi_block_set_jump(entry, body);

    XiPhi *span_phi = xi_phi_new(f, body, &t_span, body->npreds);
    span_phi->value.args[0] = span;
    XiValue *idx = xi_const_int(f, body, 0, &t_int);
    XiValue *get = xi_value_new(f, body, XI_INDEX_GET, &t_int, 2);
    get->args[0] = &span_phi->value;
    get->args[1] = idx;
    xi_block_set_return(body, get);

    xi_arc_insert(f);

    /* The general RC verifier (task 219, C3 borrow-closure) now enforces that a
     * borrow view's owner stays live through every use, including across a phi.
     * Assert the ARC output is accepted rather than re-deriving the exact drop
     * placement by hand (that manual regression is subsumed by the verifier). */
    (void) get;
    XiArcVerifyReport rep;
    ASSERT_EQ(xi_arc_verify(f, &rep), true,
              "Slice phi keeps its array owner alive into the consuming block (C3)");
    xi_func_free(f);
}

/* A view-returning method may root its Slice in an ordinary RC receiver rather
 * than in another Slice.  The call's ViewEvidence must therefore extend the
 * receiver lifetime through the returned view's final use. */
static void test_arc_span_call_view_keeps_non_span_owner_alive(void) {
    XiFunc *f = make_func("arc_span_call_owner", &t_int);
    XiBlock *entry = f->entry;

    XiValue *owner = xi_value_new(f, entry, XI_ARRAY_NEW, &t_array, 0);
    owner->escape = XI_ESC_ARG;
    XiValue *view = xi_value_new(f, entry, XI_CALL_METHOD, &t_span, 1);
    view->args[0] = owner;
    view->aux = (void *) "view";
    XiViewSourceEvidence source = {
        .source_operand = 0,
        .source_param = -1,
        .origin = XI_VIEW_ORIGIN_RECEIVER,
        .lifetime = 1,
    };
    ASSERT_EQ(xi_value_set_view_evidence(f, view, &source, 1, 0, 0, 1), true,
              "method Slice result accepts receiver-rooted ViewEvidence");
    XiValue *idx = xi_const_int(f, entry, 0, &t_int);
    XiValue *get = xi_value_new(f, entry, XI_INDEX_GET, &t_int, 2);
    get->args[0] = view;
    get->args[1] = idx;
    xi_block_set_return(entry, get);

    xi_arc_insert(f);

    int release_pos = -1;
    int use_pos = -1;
    for (uint32_t i = 0; i < entry->nvalues; i++) {
        XiValue *value = entry->values[i];
        if (value == get)
            use_pos = (int) i;
        if (value && value->op == XI_RELEASE && value->nargs == 1 &&
            value->args[0] == owner)
            release_pos = (int) i;
    }
    ASSERT_EQ(count_target_ops(f, XI_RELEASE, owner), 1,
              "view owner is released exactly once");
    ASSERT_EQ(release_pos > use_pos, true,
              "non-Slice owner outlives its receiver-rooted Slice result");
    XiArcVerifyReport rep;
    ASSERT_EQ(xi_arc_verify(f, &rep), true,
              "receiver-rooted Slice call satisfies the borrow-closure contract");
    xi_func_free(f);
}

/* A branch-local Slice can be one incoming owner of a join phi. The phi is an
 * ownership-transfer boundary: uses after the join cannot extend that one
 * branch owner's liveness into a block the owner definition does not
 * dominate. ARC must keep the incoming transfer on the branch edge rather
 * than emit RELEASE(branch_span) in the join. */
static void test_arc_branch_local_span_phi_stays_in_dominance_region(void) {
    XiFunc *f = make_func("arc_branch_span_phi", &t_int);
    XiBlock *entry = f->entry;
    XiBlock *left = xi_block_new(f);
    XiBlock *right = xi_block_new(f);
    XiBlock *join = xi_block_new(f);
    left->sealed = true;
    right->sealed = true;
    join->sealed = true;

    XiValue *cond = xi_const_bool(f, entry, true, &t_bool);
    xi_block_set_if(entry, cond, left, right);

    XiValue *left_arr = xi_value_new(f, left, XI_ARRAY_NEW, &t_array, 0);
    left_arr->escape = XI_ESC_ARG;
    XiValue *left_start = xi_const_int(f, left, 0, &t_int);
    XiValue *left_end = xi_const_int(f, left, 1, &t_int);
    XiValue *left_span = xi_value_new(f, left, XI_SLICE, &t_span, 3);
    left_span->args[0] = left_arr;
    left_span->args[1] = left_start;
    left_span->args[2] = left_end;
    xi_block_set_jump(left, join);

    XiValue *right_arr = xi_value_new(f, right, XI_ARRAY_NEW, &t_array, 0);
    right_arr->escape = XI_ESC_ARG;
    XiValue *right_start = xi_const_int(f, right, 0, &t_int);
    XiValue *right_end = xi_const_int(f, right, 1, &t_int);
    XiValue *right_span = xi_value_new(f, right, XI_SLICE, &t_span, 3);
    right_span->args[0] = right_arr;
    right_span->args[1] = right_start;
    right_span->args[2] = right_end;
    XiValue *right_move = xi_value_new(f, right, XI_OWNER_FORWARD, &t_span, 1);
    right_move->args[0] = right_span;
    xi_block_set_jump(right, join);

    XiPhi *merged = xi_phi_new(f, join, &t_span, join->npreds);
    for (uint16_t i = 0; i < join->npreds; i++)
        merged->value.args[i] = join->preds[i] == right ? right_move : left_span;
    XiValue *idx = xi_const_int(f, join, 0, &t_int);
    XiValue *get = xi_value_new(f, join, XI_INDEX_GET, &t_int, 2);
    get->args[0] = &merged->value;
    get->args[1] = idx;
    xi_block_set_return(join, get);

    xi_arc_insert(f);

    char err[512] = {0};
    ASSERT_EQ(xi_verify(f, err, sizeof(err)), true,
              "ARC releases for branch-local Slice phi remain SSA-dominated");
    /* C4 (dominance boundary) generalizes the hand-checked invariant that a
     * branch-local Slice owner is released only within its dominance region; the
     * per-release dominance scan below is subsumed by the general verifier. */
    (void) right;
    (void) right_span;
    XiArcVerifyReport rep;
    ASSERT_EQ(xi_arc_verify(f, &rep), true,
              "branch-local Slice phi releases stay in the owner's dominance region (C4)");
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

static void test_stack_alloc_closure_through_read_copy(void) {
    XiFunc *f = make_func("stack_closure_read_copy", &t_int);
    XiBlock *b0 = f->entry;

    XiFunc *child = make_func("child", &t_int);
    child->parent_func = f;
    XiValue *one = xi_const_int(child, child->entry, 1, &t_int);
    xi_block_set_return(child->entry, one);

    f->children = (XiFunc **) xr_calloc(1, sizeof(XiFunc *));
    XR_CHECK(f->children != NULL,
             "test_stack_alloc_closure_through_read_copy: child allocation failed");
    f->children[0] = child;
    f->children_cap = 1;
    f->nchildren = 1;

    XiValue *closure = xi_value_new(f, b0, XI_CLOSURE_NEW, &t_func, 0);
    closure->aux = child;
    XiValue *read_copy = xi_value_new(f, b0, XI_COPY, &t_func, 1);
    read_copy->args[0] = closure;
    XiValue *call = xi_value_new(f, b0, XI_CALL, &t_int, 1);
    call->args[0] = read_copy;
    xi_block_set_return(b0, call);

    xi_escape_analyze(f);
    ASSERT_EQ(closure->escape, XI_ESC_NONE, "closure reached through read COPY should not escape");
    xi_stack_alloc_rewrite(f);
    ASSERT_EQ(closure->op, XI_STACK_ALLOC,
              "closure reached through read COPY should become STACK_ALLOC");
    xi_func_free(f);
}

static void test_stack_alloc_direct_closure_in_resumable_function_stays_heap(void) {
    XiFunc *f = make_func("resumable_stack_closure", &t_int);
    XiBlock *b0 = f->entry;

    XiFunc *child = make_func("child", &t_int);
    child->parent_func = f;
    XiValue *one = xi_const_int(child, child->entry, 1, &t_int);
    xi_block_set_return(child->entry, one);

    f->children = (XiFunc **) xr_calloc(1, sizeof(XiFunc *));
    XR_CHECK(f->children != NULL, "test_stack_alloc_direct_closure_in_resumable_function_stays_"
                                  "heap: child allocation failed");
    f->children[0] = child;
    f->children_cap = 1;
    f->nchildren = 1;

    XiValue *closure = xi_value_new(f, b0, XI_CLOSURE_NEW, &t_func, 0);
    closure->aux = child;
    (void) xi_value_new(f, b0, XI_YIELD, &t_any, 0);
    XiValue *call = xi_value_new(f, b0, XI_CALL, &t_int, 1);
    call->args[0] = closure;
    xi_block_set_return(b0, call);

    xi_func_compute_effects(f);
    xi_escape_analyze(f);
    ASSERT_EQ(closure->escape, XI_ESC_NONE,
              "direct-call closure remains non-escaping in a resumable function");
    xi_stack_alloc_rewrite(f);
    ASSERT_EQ(closure->op, XI_CLOSURE_NEW,
              "resumable function closure must keep heap lifetime across suspension");
    ASSERT_EQ(closure->escape, XI_ESC_ARG,
              "closure rejected from stack allocation must re-enter ARC");

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

static void test_arc_rethrow_moves_caught_owner_once(void) {
    XiFunc *f = make_func("rethrow_moves_caught_owner", &t_int);
    XiBlock *b0 = f->entry;
    XiValue *caught = xi_value_new(f, b0, XI_CATCH, &t_any, 0);
    XiValue *rethrow = xi_value_new(f, b0, XI_THROW, &t_unit, 1);
    rethrow->args[0] = caught;
    rethrow->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    b0->kind = XI_BLOCK_UNREACHABLE;
    b0->control = caught;

    xi_arc_insert(f);

    int retain_count = 0;
    int release_count = 0;
    for (uint32_t i = 0; i < b0->nvalues; i++) {
        XiValue *value = b0->values[i];
        if (value && value->nargs == 1 && value->args[0] == caught) {
            retain_count += value->op == XI_RETAIN;
            release_count += value->op == XI_RELEASE;
        }
    }
    ASSERT_EQ(retain_count, 0, "rethrow must move the caught owner without a retain");
    ASSERT_EQ(release_count, 0, "rethrow consumer owns the caught value's final disposition");
    xi_func_free(f);
}

static XiValue *make_user_try(XiFunc *f, XiBlock *registration, XiBlock *body,
                              XiBlock *handler) {
    XiValue *try_op = xi_value_new(f, registration, XI_TRY, &t_unit, 0);
    try_op->aux = handler;
    try_op->aux_int = -1;
    try_op->flags = XI_FLAG_SIDE_EFFECT;
    xi_block_set_jump(registration, body);
    xi_block_add_pred(handler, registration);
    return try_op;
}

static void append_end_try(XiFunc *f, XiBlock *block, XiValue *try_op) {
    XiValue *end_try = xi_value_new(f, block, XI_END_TRY, &t_unit, 0);
    end_try->aux = try_op;
    end_try->flags = XI_FLAG_SIDE_EFFECT;
}

static void test_arc_user_try_materializes_panic_cleanup(void) {
    XiFunc *f = make_func("arc_user_try_cleanup", &t_int);
    XiBlock *entry = f->entry;
    XiBlock *body = xi_block_new(f);
    XiBlock *handler = xi_block_new(f);
    XiBlock *merge = xi_block_new(f);

    XiValue *arr = xi_value_new(f, entry, XI_ARRAY_NEW, &t_array, 0);
    XiValue *try_op = make_user_try(f, entry, body, handler);
    XiValue *index = xi_const_int(f, body, 0, &t_int);
    XiValue *get = xi_value_new(f, body, XI_INDEX_GET, &t_int, 2);
    get->args[0] = arr;
    get->args[1] = index;
    append_end_try(f, body, try_op);
    xi_block_set_jump(body, merge);
    (void) xi_value_new(f, handler, XI_CATCH, &t_any, 0);
    append_end_try(f, handler, try_op);
    xi_block_set_jump(handler, merge);
    xi_block_set_return(merge, xi_const_int(f, merge, 0, &t_int));
    body->sealed = true;
    handler->sealed = true;
    merge->sealed = true;

    xi_escape_analyze(f);
    xi_arc_insert(f);

    ASSERT_EQ(count_target_ops(f, XI_RELEASE, arr), 2,
              "try owner must have one normal and one panic-path release");
    ASSERT_EQ(count_block_target_ops(body, XI_RELEASE, arr), 1,
              "normal try exit must execute its owner release");
    ASSERT_EQ(count_block_target_ops(handler, XI_RELEASE, arr), 1,
              "panic handler must execute its owner release");
    XiArcVerifyReport rep;
    ASSERT_EQ(xi_arc_verify(f, &rep), true,
              "normal and panic try dispositions must remain balanced");
    xi_func_free(f);
}

static void test_arc_user_try_does_not_release_unavailable_owner(void) {
    XiFunc *f = make_func("arc_user_try_unavailable_owner", &t_int);
    XiBlock *entry = f->entry;
    XiBlock *body = xi_block_new(f);
    XiBlock *handler = xi_block_new(f);
    XiBlock *merge = xi_block_new(f);

    XiValue *try_op = make_user_try(f, entry, body, handler);
    XiValue *arr = xi_value_new(f, body, XI_ARRAY_NEW, &t_array, 0);
    XiValue *index = xi_const_int(f, body, 0, &t_int);
    XiValue *get = xi_value_new(f, body, XI_INDEX_GET, &t_int, 2);
    get->args[0] = arr;
    get->args[1] = index;
    append_end_try(f, body, try_op);
    xi_block_set_jump(body, merge);
    (void) xi_value_new(f, handler, XI_CATCH, &t_any, 0);
    append_end_try(f, handler, try_op);
    xi_block_set_jump(handler, merge);
    xi_block_set_return(merge, xi_const_int(f, merge, 0, &t_int));
    body->sealed = true;
    handler->sealed = true;
    merge->sealed = true;

    xi_escape_analyze(f);
    xi_arc_insert(f);

    ASSERT_EQ(count_target_ops(f, XI_RELEASE, arr), 1,
              "owner created after TRY must keep only its executable normal release");
    ASSERT_EQ(count_block_target_ops(handler, XI_RELEASE, arr), 0,
              "panic handler must not release an owner unavailable at registration");
    XiArcVerifyReport rep;
    ASSERT_EQ(xi_arc_verify(f, &rep), true,
              "unavailable owner must stay outside the handler balance");
    xi_func_free(f);
}

static void test_arc_user_try_handler_use_releases_once(void) {
    XiFunc *f = make_func("arc_user_try_handler_use", &t_int);
    XiBlock *entry = f->entry;
    XiBlock *body = xi_block_new(f);
    XiBlock *handler = xi_block_new(f);
    XiBlock *merge = xi_block_new(f);

    XiValue *arr = xi_value_new(f, entry, XI_ARRAY_NEW, &t_array, 0);
    XiValue *try_op = make_user_try(f, entry, body, handler);
    XiValue *normal_index = xi_const_int(f, body, 0, &t_int);
    XiValue *normal_get = xi_value_new(f, body, XI_INDEX_GET, &t_int, 2);
    normal_get->args[0] = arr;
    normal_get->args[1] = normal_index;
    append_end_try(f, body, try_op);
    xi_block_set_jump(body, merge);
    (void) xi_value_new(f, handler, XI_CATCH, &t_any, 0);
    XiValue *handler_index = xi_const_int(f, handler, 1, &t_int);
    XiValue *handler_get = xi_value_new(f, handler, XI_INDEX_GET, &t_int, 2);
    handler_get->args[0] = arr;
    handler_get->args[1] = handler_index;
    append_end_try(f, handler, try_op);
    xi_block_set_jump(handler, merge);
    xi_block_set_return(merge, xi_const_int(f, merge, 0, &t_int));
    body->sealed = true;
    handler->sealed = true;
    merge->sealed = true;

    xi_escape_analyze(f);
    xi_arc_insert(f);

    ASSERT_EQ(count_target_ops(f, XI_RELEASE, arr), 2,
              "handler borrow must not duplicate the panic-path owner release");
    ASSERT_EQ(count_block_target_ops(handler, XI_RELEASE, arr), 1,
              "handler must release the borrowed owner exactly once");
    XiArcVerifyReport rep;
    ASSERT_EQ(xi_arc_verify(f, &rep), true,
              "handler borrow and both try dispositions must balance");
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
    test_arc_no_escape_still_released_without_stack_rewrite();
    test_arc_stack_alloc_not_released();
    test_arc_return_gets_retain();
    test_arc_heap_gets_retain_release();
    test_arc_elim_keeps_borrowed_single_consumer_retain();
    test_arc_elim_keeps_retain_before_sole_borrowing_alias();
    test_arc_elim_keeps_single_consumer_retain_inside_loop();
    test_arc_elim_removes_retain_for_loop_local_owner();
    test_arc_many_consume_sites();
    test_arc_owner_forward_tracks_repeated_consumes();
    test_arc_tracks_owner_forward_through_phi();
    test_arc_phi_move_drops_owner_on_sibling_edge();
    test_arc_frame_pinned_distinct_phi_releases_old_owner();
    test_arc_frame_pinned_phi_edge_drops_with_live_sibling();
    test_arc_call_result_forward_retains_across_sibling_borrow();
    test_arc_call_result_retain_before_same_block_phi_consume();
    test_arc_reuses_split_edge_for_multiple_phi_consumes();
    test_arc_orders_adjacent_retain_before_release();
    test_arc_unknown_call_result_retains_before_single_consume();
    test_arc_resolved_callee_contract_overrides_callsite_borrow();
    test_arc_local_method_uses_resolved_borrow_contract();
    test_arc_local_method_maps_owned_parameter_after_receiver();
    test_arc_stringbuilder_builtin_result_is_fresh();
    test_arc_value_clone_is_fresh_owner();
    test_arc_builtin_iterator_results_are_fresh();
    test_map_entry_iterator_typed_shape();
    test_arc_generator_iterator_result_is_fresh();
    test_arc_custom_iterator_method_stays_alias_uncertain();
    test_arc_err_check_carries_cold_edge_cleanup();
    test_arc_err_check_without_throwing_source_stays_operand_free();
    test_arc_err_check_cleanup_requires_dominating_owner();
    test_arc_err_check_skips_boxed_ref_load_borrow();
    test_arc_err_check_keeps_boxed_fresh_owner();
    test_arc_span_borrow_flows_through_phi();
    test_arc_span_call_view_keeps_non_span_owner_alive();
    test_arc_branch_local_span_phi_stays_in_dominance_region();
    test_stack_alloc_local_array();
    test_stack_alloc_local_plain_map_set();
    test_stack_alloc_skips_metadata_or_dynamic_capacity();
    test_stack_alloc_direct_closure();
    test_stack_alloc_closure_through_read_copy();
    test_stack_alloc_direct_closure_in_resumable_function_stays_heap();
    test_stack_alloc_escaping_stays();
    test_arc_rethrow_moves_caught_owner_once();
    test_arc_user_try_materializes_panic_cleanup();
    test_arc_user_try_does_not_release_unavailable_owner();
    test_arc_user_try_handler_use_releases_once();

    printf("\n=== test_xi_escape: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
