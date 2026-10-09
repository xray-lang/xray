/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_closed_thread_cases.h - Separate owned handles across concurrent close
 */
#ifndef XIR_CLOSED_THREAD_CASES_H
#define XIR_CLOSED_THREAD_CASES_H
#include "os/os_thread.h"
typedef struct ClosedThreadGate {
    xr_mutex_t mutex;
    xr_cond_t condition;
    unsigned ready, completed;
    bool start, release;
} ClosedThreadGate;
typedef struct ClosedThreadCase {
    ClosedThreadGate *gate;
    XrXirValue text;
    uint32_t observations;
} ClosedThreadCase;
static void *closed_thread_worker(void *context) {
    ClosedThreadCase *worker = context; ClosedThreadGate *gate = worker->gate;
    xr_mutex_lock(&gate->mutex); ++gate->ready; xr_cond_broadcast(&gate->condition);
    while (!gate->start) xr_cond_wait(&gate->condition,&gate->mutex);
    xr_mutex_unlock(&gate->mutex);
    for (uint32_t i=0;i<10000;++i) {
        XrXirValue copy = {0};
        CHECK(xr_xir_value_copy(&worker->text,&copy) == XR_XIR_VALUE_OK);
        closed_string(&copy); xr_xir_value_drop(&copy); ++worker->observations;
    }
    xr_mutex_lock(&gate->mutex); ++gate->completed; xr_cond_broadcast(&gate->condition);
    while (!gate->release) xr_cond_wait(&gate->condition,&gate->mutex);
    xr_mutex_unlock(&gate->mutex);
    xr_xir_value_drop(&worker->text);
    return worker;
}
typedef struct ClosedPendingCase { ClosedThreadGate gate; XrXirDomain *domain; } ClosedPendingCase;
static void *closed_pending_worker(void *context) {
    ClosedPendingCase *worker = context; ClosedThreadGate *gate = &worker->gate;
    xr_mutex_lock(&gate->mutex); ++gate->ready; xr_cond_broadcast(&gate->condition);
    while (!gate->start) xr_cond_wait(&gate->condition,&gate->mutex);
    xr_mutex_unlock(&gate->mutex);
    xr_xir_domain_close(worker->domain);
    xr_xir_domain_drop(worker->domain); worker->domain = NULL;
    return worker;
}
static void closed_pending_first_close(void) {
    ClosedRing ring; closed_ring_new(&ring,1,false);
    xr_xir_value_drop(&ring.cell); xr_xir_value_drop(&ring.function); CHECK(!ring.releases);
    ClosedPendingCase worker = {0};
    CHECK(xr_xir_domain_retain(ring.domain)); worker.domain = ring.domain;
    xr_mutex_init(&worker.gate.mutex); xr_cond_init(&worker.gate.condition);
    /* This lease is an operation, not a mutex: another thread must return from
     * close while it is held. Both callers retain real domain ownership. */
    xr_xir_value_graph_begin();
    xr_thread_t thread = {0}; CHECK(xr_thread_create(&thread,closed_pending_worker,&worker));
    xr_mutex_lock(&worker.gate.mutex);
    while (worker.gate.ready != 1) xr_cond_wait(&worker.gate.condition,&worker.gate.mutex);
    size_t before; closed_no_alloc_begin(&before);
    worker.gate.start = true; xr_cond_broadcast(&worker.gate.condition); xr_mutex_unlock(&worker.gate.mutex);
    void *result = NULL; CHECK(xr_thread_join(thread,&result) == 0 && result == &worker);
    CHECK(!worker.domain && !ring.releases);
    /* No ordinary domain or producer-arena lease survives graph_end. Pending
     * close and the ring's physical owners must keep everything alive. */
    closed_ring_owners_drop(&ring); CHECK(!ring.releases && live);
    xr_xir_value_graph_end(); CHECK(ring.releases == 1 && !live);
    closed_no_alloc_end(before);
    xr_cond_destroy(&worker.gate.condition); xr_mutex_destroy(&worker.gate.mutex);
}
static void closed_thread_cases(void) {
    CHECK(!live && !closed_fail_all); fail_at = SIZE_MAX;
    closed_pending_first_close();
    ClosedRing ring; closed_ring_new(&ring,1,false);
    XrXirValue text = {0}; CHECK(xr_xir_string_new(ring.domain,"kept",4,&text) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&ring.function);
    ClosedThreadGate gate = {0}; xr_mutex_init(&gate.mutex); xr_cond_init(&gate.condition);
    ClosedThreadCase workers[2] = {{.gate=&gate},{.gate=&gate}};
    xr_thread_t threads[2] = {0};
    for (unsigned i=0;i<2;++i) {
        CHECK(xr_xir_value_copy(&text,&workers[i].text) == XR_XIR_VALUE_OK);
        CHECK(xr_thread_create(&threads[i],closed_thread_worker,&workers[i]));
    }
    xr_mutex_lock(&gate.mutex);
    while (gate.ready != 2) xr_cond_wait(&gate.condition,&gate.mutex);
    xr_mutex_unlock(&gate.mutex);
    /* Workers are quiescent at the first boundary. Later repeated closes run
     * concurrently with their immutable copies; each worker owns its handle.
     * The main String root prevents physical frees and allocator-counter races. */
    size_t before; closed_no_alloc_begin(&before);
    xr_xir_domain_close(ring.domain); CHECK(!ring.releases);
    xr_xir_value_drop(&ring.cell); CHECK(!ring.releases);
    size_t physical = live;
    xr_mutex_lock(&gate.mutex); gate.start = true; xr_cond_broadcast(&gate.condition); xr_mutex_unlock(&gate.mutex);
    for (unsigned i=0;i<10000;++i) xr_xir_domain_close(ring.domain);
    xr_mutex_lock(&gate.mutex);
    while (gate.completed != 2) xr_cond_wait(&gate.condition,&gate.mutex);
    CHECK(!ring.releases && live == physical);
    gate.release = true; xr_cond_broadcast(&gate.condition); xr_mutex_unlock(&gate.mutex);
    for (unsigned i=0;i<2;++i) {
        void *result = NULL; CHECK(xr_thread_join(threads[i],&result) == 0 && result == &workers[i]);
        CHECK(workers[i].observations == 10000 && !workers[i].text.type);
    }
    CHECK(!ring.releases && live == physical); closed_string(&text);
    xr_xir_value_drop(&text); CHECK(ring.releases == 1);
    closed_ring_owners_drop(&ring); CHECK(!live); closed_no_alloc_end(before);
    xr_cond_destroy(&gate.condition); xr_mutex_destroy(&gate.mutex);
    puts("Closed domain threads: deferred first close owns lifetime, 20000 copies, 10000 repeated closes, last-root residual retirement, physical0");
}
#endif // XIR_CLOSED_THREAD_CASES_H
