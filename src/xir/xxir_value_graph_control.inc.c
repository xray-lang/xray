/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_value_graph_control.inc.c - Quiescent ownership graph transactions
 *
 * KEY CONCEPT:
 *   Operation leases exclude snapshots without holding locks across callbacks.
 */
static atomic_flag value_graph_lock = ATOMIC_FLAG_INIT;
static uint64_t value_graph_operations;
static bool value_graph_draining;
static XrXirDomain *value_graph_closed;
static void graph_drain(void);
static void graph_domain_dispose(XrXirDomain *domain);
static void graph_lock(void) {
    while (atomic_flag_test_and_set_explicit(&value_graph_lock, memory_order_acquire)) { }
}
static void graph_unlock(void) {
    atomic_flag_clear_explicit(&value_graph_lock, memory_order_release);
}
static void graph_object_initialize(XirObject *object) {
    object->graph_previous = object->graph_next = object->graph_work = NULL;
    object->graph_trial = 0;
    object->graph_registered = object->graph_live = object->graph_dead = false;
}
static void graph_dirty(XrXirDomain *domain) {
    if (domain && domain->closed) domain->close_pending = true;
}
static void graph_object_unlink(XirObject *object) {
    if (!object->graph_registered) return;
    XrXirDomain *domain = object->domain;
    if (object->graph_previous) object->graph_previous->graph_next = object->graph_next;
    else domain->objects = object->graph_next;
    if (object->graph_next) object->graph_next->graph_previous = object->graph_previous;
    object->graph_registered = false;
    object->graph_previous = object->graph_next = NULL;
}
static void graph_domain_unlink(XrXirDomain *domain) {
    if (!domain->closed) return;
    if (domain->closed_previous) domain->closed_previous->closed_next = domain->closed_next;
    else value_graph_closed = domain->closed_next;
    if (domain->closed_next) domain->closed_next->closed_previous = domain->closed_previous;
    domain->closed_previous = domain->closed_next = NULL;
}
XR_FUNC void xr_xir_value_graph_begin(void) {
    graph_lock();
    XR_CHECK(value_graph_operations != UINT64_MAX, "value graph operation count overflow");
    ++value_graph_operations;
    graph_unlock();
}
XR_FUNC void xr_xir_value_graph_end(void) {
    graph_lock();
    XR_CHECK(value_graph_operations, "value graph operation count underflow");
    --value_graph_operations;
    bool drain = !value_graph_operations && !value_graph_draining;
    if (drain) value_graph_draining = true;
    graph_unlock();
    if (drain) graph_drain();
}
XR_FUNC void xr_xir_value_object_publish(XirObject *object) {
    graph_lock();
    XR_CHECK(value_graph_operations && object && object->domain &&
        !object->graph_registered && !object->graph_dead &&
        atomic_load_explicit(&object->references, memory_order_relaxed),
        "complete value publication requires an operation and its original owner");
    XrXirDomain *domain = object->domain;
    object->graph_previous = NULL; object->graph_next = domain->objects;
    if (domain->objects) domain->objects->graph_previous = object;
    domain->objects = object; object->graph_registered = true;
    graph_dirty(domain);
    graph_unlock();
}
static bool graph_object_retain(XirObject *object) {
    graph_lock();
    bool retained = !object->graph_dead && xr_xir_reference_retain(&object->references);
    graph_unlock();
    return retained;
}
static void graph_object_release_locked(XirObject *object, XirObject **pending) {
    XR_CHECK(!object->graph_dead, "detached graph ownership cannot be released twice");
    graph_dirty(object->domain);
    if (xr_xir_reference_release(&object->references)) {
        graph_object_unlink(object);
        object->release_next = *pending; *pending = object;
    }
}
XR_FUNC void xr_xir_domain_close(XrXirDomain *domain) {
    if (!domain) return;
    graph_lock();
    if (!domain->closed) {
        domain->closed = true;
        domain->closed_next = value_graph_closed;
        if (value_graph_closed) value_graph_closed->closed_previous = domain;
        value_graph_closed = domain;
        domain->close_pending = true;
    }
    bool drain = !value_graph_operations && !value_graph_draining;
    if (drain) value_graph_draining = true;
    graph_unlock();
    if (drain) graph_drain();
}
