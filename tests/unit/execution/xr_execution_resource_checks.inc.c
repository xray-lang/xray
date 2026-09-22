/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_execution_resource_checks.inc.c - Typed payload ownership and finalizer lifetime
 */

typedef struct ResourcePayloadProbe {
    XrInstance *instance;
    XrExecutionResource **owner;
    unsigned *destroyed;
    atomic_bool *entered;
    atomic_bool *may_return;
    int64_t value;
} ResourcePayloadProbe;

static void destroy_resource_payload(void *opaque) {
    ResourcePayloadProbe *payload = opaque;
    REQUIRE(payload->value == 42);
    REQUIRE(*payload->owner == NULL);
    xr_execution_resource_free(payload->owner);
    REQUIRE(xr_execution_instance_retire(payload->instance, NULL) == XR_EXECUTION_GENERATION_REJECTED);
    if (payload->entered) {
        atomic_store_explicit(payload->entered, true, memory_order_release);
        while (!atomic_load_explicit(payload->may_return, memory_order_acquire)) {
        }
    }
    ++*payload->destroyed;
    xr_free(payload);
}

static ResourcePayloadProbe *resource_payload(XrInstance *instance, XrExecutionResource **owner,
                                              unsigned *destroyed) {
    ResourcePayloadProbe *payload = xr_calloc(1u, sizeof(*payload));
    REQUIRE(payload != NULL);
    payload->instance = instance;
    payload->owner = owner;
    payload->destroyed = destroyed;
    payload->value = 42;
    return payload;
}

static void destroy_resource_module_state(void *opaque) {
    xr_execution_resource_free(opaque);
}

static void test_resource_payload_ownership(void) {
    XrValidatedProgram *program = build_module_program_with_resource(true);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestProviderBindings bindings = {0};
    XrInstance *instance = create_instance(program, profile, &bindings, 1u);
    XrInstance *other = create_instance(program, profile, &bindings, 1u);
    REQUIRE(xr_fingerprint_equal(xr_execution_instance_id(instance), xr_execution_instance_id(other)));
    XrExecutionLease lease = {0}, other_lease = {0};
    REQUIRE(xr_execution_instance_acquire(instance, &lease) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_acquire(other, &other_lease) == XR_EXECUTION_OK);
    XrStableId id = {{1u, 2u, 3u, 4u}}, wrong_id = {{2u, 2u, 3u, 4u}};
    XrExecutionResource *external = NULL, *state = NULL;
    unsigned destroyed = 0u;
    ResourcePayloadProbe *payload = resource_payload(instance, &external, &destroyed);
    REQUIRE(xr_execution_resource_adopt(&lease, XR_CORE_TYPE_I64, id, payload,
                                         destroy_resource_payload, &external) == XR_EXECUTION_INVALID_INPUT);
    REQUIRE(xr_execution_resource_adopt(&lease, UINT16_MAX, id, payload,
                                         destroy_resource_payload, &external) == XR_EXECUTION_INVALID_INPUT);
    REQUIRE(xr_execution_resource_adopt(&lease, 32u, wrong_id, payload,
                                         destroy_resource_payload, &external) == XR_EXECUTION_INVALID_INPUT);
    REQUIRE(xr_execution_resource_adopt(&lease, 32u, id, payload, NULL, &external) == XR_EXECUTION_INVALID_INPUT);
    REQUIRE(xr_execution_resource_adopt(&lease, 32u, id, NULL,
                                         destroy_resource_payload, &external) == XR_EXECUTION_INVALID_INPUT);
    REQUIRE(external == NULL && destroyed == 0u);
    REQUIRE(xr_execution_resource_adopt(&lease, 32u, id, payload,
                                         destroy_resource_payload, &external) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_resource_adopt(&lease, 32u, id, payload,
                                         destroy_resource_payload, &external) == XR_EXECUTION_INVALID_INPUT);
    void *borrowed = NULL;
    REQUIRE(!xr_execution_resource_borrow(&other_lease, external, id, &borrowed));
    REQUIRE(!xr_execution_resource_borrow(&lease, external, wrong_id, &borrowed));
    REQUIRE(borrowed == NULL);
    REQUIRE(xr_execution_resource_borrow(&lease, external, id, &borrowed));
    REQUIRE(borrowed == payload && ((ResourcePayloadProbe *) borrowed)->value == 42);
    REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
    ResourcePayloadProbe *state_payload = resource_payload(instance, &state, &destroyed);
    REQUIRE(xr_execution_resource_adopt(&lease, 32u, id, state_payload,
                                         destroy_resource_payload, &state) == XR_EXECUTION_OK);
    void *observed = NULL;
    REQUIRE(xr_execution_lease_bind_state(&lease, &runtime_state_layout, &state,
                                          destroy_resource_module_state, &observed) == XR_EXECUTION_STATE_ADOPTED);
    REQUIRE(observed == &state);
    XrExecutionLease stale = lease;
    REQUIRE(xr_execution_lease_release(&lease));
    REQUIRE(state == NULL && external != NULL && destroyed == 1u);
    borrowed = NULL;
    REQUIRE(!xr_execution_resource_borrow(&stale, external, id, &borrowed));
    REQUIRE(borrowed == NULL);
    REQUIRE(xr_execution_resource_adopt(&stale, 32u, id, payload,
                                         destroy_resource_payload, &state) == XR_EXECUTION_GENERATION_REJECTED);
    REQUIRE(state == NULL);
    REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_GENERATION_REJECTED);
    xr_execution_resource_free(&external);
    REQUIRE(external == NULL && destroyed == 2u);
    xr_execution_resource_free(&external);
    REQUIRE(destroyed == 2u);
    REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_lease_release(&other_lease));
    REQUIRE(xr_execution_instance_begin_drain(other, NULL) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_retire(other, NULL) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_free(&other, NULL) == XR_EXECUTION_OK);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void *free_resource_worker(void *opaque) {
    xr_execution_resource_free(opaque);
    return NULL;
}

static void test_resource_finalizer_retirement(void) {
    XrValidatedProgram *program = build_module_program_with_resource(true);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestProviderBindings bindings = {0};
    XrInstance *instance = create_instance(program, profile, &bindings, 1u);
    XrExecutionLease lease = {0};
    REQUIRE(xr_execution_instance_acquire(instance, &lease) == XR_EXECUTION_OK);
    XrExecutionResource *owner = NULL;
    unsigned destroyed = 0u;
    atomic_bool entered, may_return;
    atomic_init(&entered, false);
    atomic_init(&may_return, false);
    ResourcePayloadProbe *payload = resource_payload(instance, &owner, &destroyed);
    payload->entered = &entered;
    payload->may_return = &may_return;
    REQUIRE(xr_execution_resource_adopt(&lease, 32u, (XrStableId) {{1u, 2u, 3u, 4u}},
                                         payload, destroy_resource_payload, &owner) == XR_EXECUTION_OK);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
    REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_lease_release(&lease));
    xr_thread_t thread;
    REQUIRE(xr_thread_create(&thread, free_resource_worker, &owner));
    while (!atomic_load_explicit(&entered, memory_order_acquire)) {
    }
    REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_GENERATION_REJECTED);
    REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_GENERATION_REJECTED);
    atomic_store_explicit(&may_return, true, memory_order_release);
    REQUIRE(xr_thread_join(thread, NULL) == 0);
    REQUIRE(owner == NULL && destroyed == 1u);
    REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);
}
