/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_class_observation.inc.c - Same-domain bounded class lifetime records
 *
 * KEY CONCEPT:
 *   Construction prepays recording and one drain; retirement never allocates.
 */
enum { CLASS_OBSERVATION_PREPARED = 1, CLASS_OBSERVATION_COMPLETE, CLASS_OBSERVATION_FINAL };
enum { CLASS_OBSERVATION_VALIDATE_WORK = 32 };
struct XrXirClassObservation {
    XrXirDomain *domain;
    uint64_t next_serial;
    uint32_t references, capacity, count, drained;
    bool overflow, overflow_drained;
};
_Static_assert(sizeof(XrXirClassObservation) % _Alignof(XrXirClassEvent) == 0 &&
    _Alignof(XrXirClassObservation) >= _Alignof(XrXirClassEvent), "observation tail record alignment");
static XrXirClassEvent *class_observation_records(XrXirClassObservation *observation) {
    return (XrXirClassEvent *)(observation + 1);
}
static uint64_t class_observation_bytes(uint32_t capacity) {
    return sizeof(XrXirClassObservation) + (uint64_t)capacity * sizeof(XrXirClassEvent);
}
static void class_observation_dispose(XrXirDomain *domain) {
    XR_CHECK(xr_xir_domain_current(domain), "class observation retains its current domain");
    XrXirClassObservation *observation = domain->observation;
    if (!observation) return;
    XR_CHECK(!observation->references, "domain disposal has no external observation lease");
    domain->observation = NULL;
    xr_xir_domain_metadata_deallocate(domain, observation, class_observation_bytes(observation->capacity));
}
XR_FUNC XrXirValueStatus xr_xir_domain_class_observation_open(XrXirDomain *domain,
    uint32_t capacity, XrXirClassObservation **output) {
    if (!xr_xir_domain_current(domain)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!domain || !output || *output || !capacity || capacity > XR_XIR_CLASS_OBSERVATION_CAPACITY)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    graph_lock();
    bool available = !domain->closed && !domain->observation && !domain->objects;
    graph_unlock();
    if (!available) return xr_xir_domain_work(domain, CLASS_OBSERVATION_VALIDATE_WORK) ?
        XR_XIR_VALUE_BAD_ARGUMENT : XR_XIR_VALUE_LIMIT;
    uint64_t bytes = class_observation_bytes(capacity);
    if (!xr_xir_domain_work(domain, bytes + CLASS_OBSERVATION_VALIDATE_WORK)) return XR_XIR_VALUE_LIMIT;
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    XrXirClassObservation *observation = xr_xir_domain_metadata_allocate(domain, bytes, &status);
    if (!observation) return status;
    if (!xr_xir_domain_retain(domain)) {
        xr_xir_domain_metadata_deallocate(domain, observation, bytes); return XR_XIR_VALUE_REFCOUNT_LIMIT;
    }
    observation->domain = domain; observation->next_serial = 1;
    observation->references = 1; observation->capacity = capacity;
    graph_lock();
    available = !domain->closed && !domain->observation && !domain->objects;
    if (available) domain->observation = observation;
    graph_unlock();
    if (!available) {
        xr_xir_domain_metadata_deallocate(domain, observation, bytes);
        xr_xir_domain_drop(domain); return XR_XIR_VALUE_BAD_ARGUMENT;
    }
    *output = observation; return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_class_observation_retain(XrXirClassObservation *observation) {
    if (!observation || !xr_xir_domain_current(observation->domain)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!observation) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_domain_work(observation->domain, 1)) return XR_XIR_VALUE_LIMIT;
    graph_lock();
    bool observation_live = observation->references && observation->domain->observation == observation;
    bool retained = observation_live && observation->references != UINT32_MAX && xr_xir_domain_retain(observation->domain);
    if (retained) ++observation->references;
    graph_unlock();
    return retained ? XR_XIR_VALUE_OK : observation_live ? XR_XIR_VALUE_REFCOUNT_LIMIT : XR_XIR_VALUE_BAD_ARGUMENT;
}
XR_FUNC void xr_xir_class_observation_drop(XrXirClassObservation **pointer) {
    if (!pointer || !*pointer) return;
    XrXirClassObservation *observation = *pointer;
    XrXirDomain *domain = observation->domain;
    XR_CHECK(xr_xir_domain_current(domain), "observation release retains its current owner");
    graph_lock();
    XR_CHECK(domain->observation == observation && observation->references,
        "observation drop consumes its actual external domain lease");
    --observation->references; *pointer = NULL;
    graph_unlock();
    xr_xir_domain_drop(domain);
}
static bool class_observation_overlap(const void *left, uint64_t left_bytes,
    const void *right, uint64_t right_bytes) {
    uintptr_t a = (uintptr_t)left, b = (uintptr_t)right;
    if (left_bytes > UINTPTR_MAX - a || right_bytes > UINTPTR_MAX - b) return true;
    return a < b + right_bytes && b < a + left_bytes;
}
static bool class_observation_outputs_disjoint(const XrXirClassObservation *observation,
    const uint64_t *cursor, const XrXirClassEvent *events, uint32_t capacity,
    const uint32_t *count, const bool *overflow) {
    const void *outputs[] = {cursor, events, count, overflow};
    uint64_t bytes[] = {sizeof(*cursor), (uint64_t)capacity * sizeof(*events), sizeof(*count), sizeof(*overflow)};
    for (uint32_t i = 0; i < 4; ++i) {
        if (class_observation_overlap(outputs[i], bytes[i], observation,
            class_observation_bytes(observation->capacity))) return false;
        for (uint32_t j = 0; j < i; ++j)
            if (class_observation_overlap(outputs[i], bytes[i], outputs[j], bytes[j])) return false;
    }
    return true;
}
static bool class_observation_drain_valid(const XrXirClassObservation *observation,
    const uint64_t *cursor, const XrXirClassEvent *events, uint32_t capacity,
    const uint32_t *count, const bool *overflow) {
    return xr_xir_domain_current(observation->domain) && observation->references &&
        observation->domain->observation == observation &&
        cursor && *cursor == observation->drained && events && capacity &&
        capacity <= XR_XIR_CLASS_OBSERVATION_CAPACITY && count && !*count && overflow && !*overflow &&
        class_observation_outputs_disjoint(observation, cursor, events, capacity, count, overflow);
}
XR_FUNC XrXirValueStatus xr_xir_class_observation_drain(XrXirClassObservation *observation,
    uint64_t *cursor, XrXirClassEvent *events, uint32_t capacity, uint32_t *count, bool *overflow) {
    if (!observation || !xr_xir_domain_current(observation->domain)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!observation) return XR_XIR_VALUE_BAD_ARGUMENT;
    graph_lock();
    bool valid = class_observation_drain_valid(observation, cursor, events, capacity, count, overflow);
    bool prepaid = valid && (observation->count > observation->drained ||
        (observation->overflow && !observation->overflow_drained));
    if (!prepaid) {
        graph_unlock();
        if (!xr_xir_domain_work(observation->domain, CLASS_OBSERVATION_VALIDATE_WORK)) return XR_XIR_VALUE_LIMIT;
        graph_lock();
        valid = class_observation_drain_valid(observation, cursor, events, capacity, count, overflow);
    }
    if (!valid) { graph_unlock(); return XR_XIR_VALUE_BAD_ARGUMENT; }
    uint32_t amount = observation->count - observation->drained;
    if (amount > capacity) amount = capacity;
    if (amount) memcpy(events, class_observation_records(observation) + observation->drained, (size_t)amount * sizeof(*events));
    observation->drained += amount;
    *cursor = observation->drained; *count = amount; *overflow = observation->overflow;
    if (observation->overflow) observation->overflow_drained = true;
    graph_unlock(); return XR_XIR_VALUE_OK;
}
static XrXirValueStatus class_observation_prepare(XrXirDomain *domain, uint64_t *serial) {
    XR_CHECK(xr_xir_domain_current(domain), "class observation retains its current domain");
    *serial = 0;
    graph_lock();
    bool observed = domain->observation != NULL;
    graph_unlock();
    if (!observed) return XR_XIR_VALUE_OK;
    /* Each reservation pays three fixed writes and three one-time copies,
     * including one-call validation/publication per record and serial allocation.
     * Partial/no-storage failure keeps this charge; no cleanup spends new work. */
    uint64_t work = 3 * (2 * (uint64_t)sizeof(XrXirClassEvent) + CLASS_OBSERVATION_VALIDATE_WORK) + 1;
    if (!xr_xir_domain_work(domain, work)) return XR_XIR_VALUE_LIMIT;
    graph_lock();
    XrXirClassObservation *observation = domain->observation;
    bool available = observation->next_serial != UINT64_MAX;
    if (available) *serial = observation->next_serial++;
    graph_unlock();
    return available ? XR_XIR_VALUE_OK : XR_XIR_VALUE_LIMIT;
}
static void class_observation_record_locked(XrXirDomain *domain, XrXirClassEvent event) {
    XR_CHECK(xr_xir_domain_current(domain), "class observation retains its current domain");
    if (!event.serial) return;
    XrXirClassObservation *observation = domain->observation;
    XR_CHECK(observation && event.serial < observation->next_serial,
        "class record owns its actual prepaid observation serial");
    if (observation->count == observation->capacity) observation->overflow = true;
    else class_observation_records(observation)[observation->count++] = event;
}
static void class_observation_record(XrXirDomain *domain, XrXirClassEvent event) {
    if (!event.serial) return;
    graph_lock(); class_observation_record_locked(domain, event); graph_unlock();
}
static XrXirClassEvent class_observation_event(const XirClassObject *instance,
    uint32_t kind, uint32_t flags, uint32_t initialized, XrXirValueStatus status) {
    return (XrXirClassEvent){instance->observation_serial, (uint32_t)instance->object.type,
        instance->observation_declaration, kind, flags, initialized, (uint32_t)status};
}
static void class_observation_complete(XirClassObject *instance) {
    if (!instance->observation_serial) return;
    XR_CHECK(instance->observation_state == CLASS_OBSERVATION_PREPARED, "class publishes completion once");
    instance->observation_state = CLASS_OBSERVATION_COMPLETE;
    class_observation_record(instance->object.domain,
        class_observation_event(instance, XR_XIR_CLASS_CONSTRUCT, 0, 0, XR_XIR_VALUE_OK));
}
static void class_observation_finalize_locked(XirObject *object) {
    XirClassObject *instance = (XirClassObject *)object;
    if (!instance->observation_serial) return;
    XR_CHECK(instance->observation_state == CLASS_OBSERVATION_COMPLETE, "class finalization starts once");
    instance->observation_state = CLASS_OBSERVATION_FINAL;
    class_observation_record_locked(object->domain, class_observation_event(instance, XR_XIR_CLASS_FINALIZE,
        object->graph_dead ? XR_XIR_CLASS_EVENT_RESIDUAL : 0, 0, XR_XIR_VALUE_OK));
}
static void class_observation_finalize(XirObject *object) {
    graph_lock(); class_observation_finalize_locked(object); graph_unlock();
}
static void class_storage_deallocate(XirClassObject *instance,
    bool partial, uint32_t initialized, XrXirValueStatus status) {
    XrXirDomain *domain = instance->object.domain;
    XR_CHECK(!instance->observation_serial || instance->observation_state ==
        (uint32_t)(partial ? CLASS_OBSERVATION_PREPARED : CLASS_OBSERVATION_FINAL),
        "physical class reclaim follows its exact initialized lifetime");
    uint32_t flags = partial ? XR_XIR_CLASS_EVENT_PARTIAL :
        instance->object.graph_dead ? XR_XIR_CLASS_EVENT_RESIDUAL : 0;
    XrXirClassEvent event = class_observation_event(instance, XR_XIR_CLASS_RECLAIM, flags, initialized, status);
    size_t bytes = instance->allocation_bytes;
    xr_xir_domain_deallocate(domain, instance, bytes);
    class_observation_record(domain, event);
}
static void class_discard_partial(XirClassObject *instance, uint32_t initialized, XrXirValueStatus status) {
    XrXirDomain *domain = instance->object.domain;
    XrXirTypeArena *arena = instance->object.arena;
    XR_CHECK(!instance->object.graph_registered && !instance->object.graph_dead,
        "incomplete class fields have never been published to the graph");
    class_storage_deallocate(instance, true, initialized, status);
    xr_xir_compile_type_arena_drop(arena); xr_xir_domain_drop(domain);
}
