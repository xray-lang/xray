/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_weak_handle.c - Weak field storage.
 *
 * The runtime half of `weak`, tested before any language surface exists for
 * it. What must hold:
 *
 *   W1  a read promotes to a strong reference, or returns null
 *   W5  the clearing point is exactly the target's last strong release
 *       — with no cycle collector there is no exception to this
 *
 * plus the two invariants that make it cheap and safe: all weak references to
 * one target share a single handle (so death is one store, not a walk), and a
 * handle that dies BEFORE its target leaves the table (or the target's later
 * death would write through freed memory).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../src/runtime/mem/xcoro_heap.h"
#include "../../../src/runtime/mem/xobj_header.h"
#include "../../../src/runtime/mem/xweak_handle.h"
#include "../../../src/runtime/mem/xruntime_object_heap.h"
#include "../../../src/runtime/value/xvalue.h"
#include "../../../src/runtime/xisolate_api.h"
#include "../../../src/runtime/xisolate_internal.h"
#include "../../../src/coro/xcoroutine.h"
#include "../test_win_compat.h"

static XrCoroutine dummy_coro;
static XrVMRuntime g_test_iso;
static XrRuntimeCore g_test_core;

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg));                                 \
            g_failed++;                                                                            \
            return;                                                                                \
        }                                                                                          \
        g_passed++;                                                                                \
    } while (0)

static void setup(void) {
    memset(&g_test_iso, 0, sizeof(g_test_iso));
    memset(&g_test_core, 0, sizeof(g_test_core));
    memset(&dummy_coro, 0, sizeof(dummy_coro));
    g_test_iso.core_rt = &g_test_core;
    g_test_core.vm_owner = &g_test_iso;
    dummy_coro.core = &g_test_core;
}

/* One handle per target: a target's death has to be a single store, not a walk
 * over every field that named it. */
static void test_handle_is_shared_per_target(void) {
    XrCoroHeap *heap = xr_coro_heap_create(dummy_coro.core);
    CHECK(heap != NULL, "heap create");

    XrObjHeader *a = xr_coro_heap_new_obj(heap, XR_TBLOB, 64);
    XrObjHeader *b = xr_coro_heap_new_obj(heap, XR_TBLOB, 64);
    CHECK(a && b, "allocate targets");

    XrWeakHandle *h1 = xr_weak_handle_acquire(heap, a);
    XrWeakHandle *h2 = xr_weak_handle_acquire(heap, a);
    XrWeakHandle *h3 = xr_weak_handle_acquire(heap, b);
    CHECK(h1 != NULL, "acquire first handle");
    CHECK(h1 == h2, "same target yields the same handle");
    CHECK(h3 != h1, "different target yields a different handle");
    CHECK(h1->target == a, "handle points at its target");

    /* The flag is what lets an object with no weak references pay only a bit
     * test on its way out. */
    CHECK((a->extra & XR_OBJ_HAS_WEAK) != 0, "target flagged HAS_WEAK");
    CHECK((b->extra & XR_OBJ_HAS_WEAK) != 0, "second target flagged HAS_WEAK");

    xr_coro_heap_destroy(heap);
}

/* W1: a read hands back a strong reference, never a borrowed pointer that
 * could die mid-expression. */
static void test_load_promotes_to_strong(void) {
    XrCoroHeap *heap = xr_coro_heap_create(dummy_coro.core);
    CHECK(heap != NULL, "heap create");

    XrObjHeader *target = xr_coro_heap_new_obj(heap, XR_TBLOB, 64);
    CHECK(target != NULL, "allocate target");
    XrWeakHandle *h = xr_weak_handle_acquire(heap, target);
    CHECK(h != NULL, "acquire handle");

    int32_t before = atomic_load_explicit(&target->refcount, memory_order_relaxed);
    XrObjHeader *loaded = xr_weak_handle_load(h);
    CHECK(loaded == target, "load returns the target");
    int32_t after = atomic_load_explicit(&target->refcount, memory_order_relaxed);
    /* 0-based RC: one more reference is one step further from zero. */
    CHECK(after != before, "load raised the refcount");

    xr_coro_heap_destroy(heap);
}

/* W5: the clearing point is the target's destruction, and it is immediate. */
static void test_target_death_clears_the_handle(void) {
    XrCoroHeap *heap = xr_coro_heap_create(dummy_coro.core);
    CHECK(heap != NULL, "heap create");

    XrObjHeader *target = xr_coro_heap_new_obj(heap, XR_TBLOB, 64);
    CHECK(target != NULL, "allocate target");
    XrWeakHandle *h = xr_weak_handle_acquire(heap, target);
    CHECK(h != NULL, "acquire handle");
    CHECK(xr_weak_handle_load(h) == target, "reads live before death");

    xr_coro_heap_destroy_obj(heap, target);

    CHECK(h->target == NULL, "handle cleared at target death");
    CHECK(xr_weak_handle_load(h) == NULL, "reads null after death");

    xr_coro_heap_destroy(heap);
}

/* Several targets dying in turn: each clears only its own handle. A table bug
 * that cleared the wrong entry would show up as a live object reading null. */
static void test_many_targets_clear_independently(void) {
    XrCoroHeap *heap = xr_coro_heap_create(dummy_coro.core);
    CHECK(heap != NULL, "heap create");

    enum {
        N = 64
    };
    XrObjHeader *targets[N];
    XrWeakHandle *handles[N];
    for (int i = 0; i < N; i++) {
        targets[i] = xr_coro_heap_new_obj(heap, XR_TBLOB, 48);
        CHECK(targets[i] != NULL, "allocate target");
        handles[i] = xr_weak_handle_acquire(heap, targets[i]);
        CHECK(handles[i] != NULL, "acquire handle");
    }
    /* Enough entries to force at least one rehash, so the probe chains and
     * tombstone accounting are actually exercised. */
    for (int i = 0; i < N; i += 2)
        xr_coro_heap_destroy_obj(heap, targets[i]);

    for (int i = 0; i < N; i++) {
        if (i % 2 == 0) {
            if (handles[i]->target != NULL) {
                printf("FAIL: handle %d not cleared\n", i);
                g_failed++;
                return;
            }
        } else {
            if (handles[i]->target != targets[i]) {
                printf("FAIL: handle %d wrongly cleared\n", i);
                g_failed++;
                return;
            }
        }
    }
    g_passed++;

    xr_coro_heap_destroy(heap);
}

/* A handle dying first must leave the table. Otherwise the target's later
 * death would look up a freed handle and write through it. */
static void test_handle_dying_first_leaves_the_table(void) {
    XrCoroHeap *heap = xr_coro_heap_create(dummy_coro.core);
    CHECK(heap != NULL, "heap create");

    XrObjHeader *target = xr_coro_heap_new_obj(heap, XR_TBLOB, 64);
    CHECK(target != NULL, "allocate target");
    XrWeakHandle *h = xr_weak_handle_acquire(heap, target);
    CHECK(h != NULL, "acquire handle");

    xr_coro_heap_destroy_obj(heap, (XrObjHeader *) h);
    /* The target still carries HAS_WEAK, so its destroy path will consult the
     * table — and must find nothing there. */
    xr_coro_heap_destroy_obj(heap, target);
    g_passed++; /* reaching here without a use-after-free is the assertion */

    xr_coro_heap_destroy(heap);
}

static void test_canonical_string_weak_promotion_and_clear(void) {
    XrCoroHeap *heap = xr_coro_heap_create(dummy_coro.core);
    CHECK(heap != NULL, "heap create");

    static const char text[] = "weak-string";
    size_t bytes = (size_t) xr_runtime_string_object_allocation_bytes(
        (uint32_t) (sizeof(text) - 1));
    XrString *string = (XrString *) xr_runtime_object_allocate(
        bytes, XR_RUNTIME_OBJECT_KIND_STRING,
        XR_RUNTIME_STRING_LAYOUT_INDEX,
        XR_RUNTIME_STRING_DOMAIN_EXEC_LOCAL,
        XR_RUNTIME_OBJECT_OWNER_EXECUTION, heap, NULL, NULL);
    CHECK(string != NULL, "allocate canonical string target");
    CHECK(xr_runtime_string_object_init(
              string, XR_RUNTIME_STRING_DOMAIN_EXEC_LOCAL,
              (uint32_t) (sizeof(text) - 1),
              (uint32_t) (sizeof(text) - 1), 1,
              XR_RUNTIME_STRING_TRAIT_LOCAL) == XR_RUNTIME_ABI_OK,
          "materialize canonical string target");
    memcpy(string->data, text, sizeof(text));

    XrValue slot = xr_weak_field_store(heap, XR_FROM_STR(string));
    CHECK(XR_IS_PTR(slot), "weak string store creates a handle");
    XrValue loaded = xr_weak_field_load(slot);
    CHECK(XR_IS_STRING(loaded) && XR_TO_STRING(loaded) == string,
          "weak string load promotes the canonical target");
    CHECK(atomic_load_explicit(&string->header.rc,
                               memory_order_relaxed) == 2,
          "weak string promotion raises canonical RC");
    xr_rc_release_value(heap, loaded);
    CHECK(atomic_load_explicit(&string->header.rc,
                               memory_order_relaxed) == 1,
          "promoted weak string owner releases independently");
    xr_rc_release_value(heap, XR_FROM_STR(string));
    CHECK(XR_IS_NULL(xr_weak_field_load(slot)),
          "canonical string last release clears weak handles");

    xr_coro_heap_destroy(heap);
}

int main(void) {
    xr_test_suppress_dialogs();
    printf("test_weak_handle:\n");
    setup();
    test_handle_is_shared_per_target();
    test_load_promotes_to_strong();
    test_target_death_clears_the_handle();
    test_many_targets_clear_independently();
    test_handle_dying_first_leaves_the_table();
    test_canonical_string_weak_promotion_and_clear();
    printf("  %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
