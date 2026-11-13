/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_signed_object_rc.c - Published counts and last-owner handoff ordering
 */
#include "xir/xxir_value_internal.h"
#include "os/os_thread.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%d: %s\n", __LINE__, #condition); abort(); \
} } while (0)

enum { THREAD_COUNT = 8, REFERENCE_ROUNDS = 100000, OWNER_HANDOFFS = 2000 };
static XirObject shared;
static _Atomic(bool) started;

static void *count_worker(void *unused) {
    (void)unused;
    while (!atomic_load_explicit(&started, memory_order_acquire)) xr_thread_yield();
    for (uint32_t round = 0; round < REFERENCE_ROUNDS; ++round) {
        if (!xir_object_retain(&shared)) return (void *)(uintptr_t)1;
        if (xir_object_release(&shared)) return (void *)(uintptr_t)2;
    }
    return NULL;
}

typedef struct ReferenceHandoff {
    XirObject object;
    uint32_t payload, observed;
} ReferenceHandoff;

static void *handoff_reader(void *opaque) {
    ReferenceHandoff *handoff = opaque;
    handoff->observed = handoff->payload;
    if (xir_object_release(&handoff->object)) return (void *)(uintptr_t)3;
    return NULL;
}

int main(void) {
    xr_thread_t threads[THREAD_COUNT];
    xir_object_initialize(&shared);
    atomic_store_explicit(&shared.references, -1, memory_order_release);
    atomic_init(&started, false);
    for (uint32_t thread = 0; thread < THREAD_COUNT; ++thread)
        CHECK(xr_thread_create(&threads[thread], count_worker, NULL));
    atomic_store_explicit(&started, true, memory_order_release);
    for (uint32_t thread = 0; thread < THREAD_COUNT; ++thread) {
        void *result = NULL;
        CHECK(xr_thread_join(threads[thread], &result) == 0 && !result);
    }
    CHECK(xir_object_references(&shared) == -1);
    CHECK(xir_object_unique(&shared) && xir_object_references(&shared) == 1);
    CHECK(xir_object_release(&shared));

    for (uint32_t round = 0; round < OWNER_HANDOFFS; ++round) {
        ReferenceHandoff handoff = {.payload=round};
        xir_object_initialize(&handoff.object);
        CHECK(xir_object_retain(&handoff.object));
        atomic_store_explicit(&handoff.object.references, -2, memory_order_release);
        xr_thread_t reader;
        CHECK(xr_thread_create(&reader, handoff_reader, &handoff));
        /* The reader's final release and this acquire are the only ordering
         * between its payload read and the following write, before join. */
        while (!xir_object_unique(&handoff.object)) xr_thread_yield();
        handoff.payload = UINT32_MAX;
        void *result = NULL;
        CHECK(xr_thread_join(reader, &result) == 0 && !result);
        CHECK(handoff.observed == round && xir_object_references(&handoff.object) == 1);
        CHECK(xir_object_release(&handoff.object));
    }
    puts("signed object RC: 8x100000 concurrent references and 2000 last-owner handoffs passed");
    return 0;
}
