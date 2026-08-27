/*
 * test_typed_lifecycle_audit.c - Certificate-backed typed cleanup audit
 */

#include "base/xmalloc.h"
#include "ir/xi.h"
#include "ir/xi_coro_lower.h"
#include "ir/xi_module.h"
#include "ir/xi_stage.h"
#include "plan/ownership/xr_ownership_certificate_internal.h"
#include "plan/semantic/xr_semantic_builder.h"
#include "plan/semantic/xr_semantic_plan_internal.h"
#include "plan/target/xr_target_builder.h"
#include "plan/target/xr_target_plan_internal.h"
#include "plan/target_profile_test_fixture.h"
#include "runtime/abi/xr_runtime_target_authority.h"
#include "runtime/value/xtype.h"
#include "vm/audit/xr_typed_lifecycle_audit.h"
#include "vm/xr_typed_frame.h"
#include "vm/xr_typed_lifecycle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

typedef struct AuditFixture {
    XrSemanticPlan *semantic;
    XrTargetProfile *profile;
    XrTargetPlan *plan;
} AuditFixture;

typedef struct AuditCoordinates {
    uint32_t slot;
    uint32_t producer_operation;
    uint32_t normal_operation;
    uint32_t state_operation;
    uint32_t owner_index;
    uint32_t event_index;
} AuditCoordinates;

typedef struct AllocationProbe {
    XrString *object;
    uintptr_t address;
    uint32_t resolve_calls;
    uint32_t reclaim_calls;
    bool resolve_enabled;
} AllocationProbe;

typedef struct ObserverBridge {
    XrTypedLifecycleAuditContext *adapter;
    AllocationProbe *allocation;
    uint32_t calls;
    bool mutate_slot_identity;
} ObserverBridge;

typedef struct AuditBundle {
    XrOwnershipAudit *oracle;
    XrTypedLifecycleAuditContext *adapter;
} AuditBundle;

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_owned_string = {
    .kind = XR_KIND_STRING,
    .id = 22,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
};
static XrType stub_unit = {
    .kind = XR_KIND_UNIT,
    .id = 23,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
};

static XrSemanticPlan *build_lifecycle_semantic_plan(void) {
    XiFunc *function = xi_func_new("typed_lifecycle_audit_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    entry->sealed = true;
    XiValue *prefix = xi_param(function, entry, 0, &stub_int);
    REQUIRE(prefix != NULL);
    function->nparams = 1;
    function->params = (XiValue **) xr_malloc(sizeof(*function->params));
    REQUIRE(function->params != NULL);
    function->params[0] = prefix;
    XiValue *left = xi_const_str(function, entry, "typed", &stub_owned_string);
    XiValue *right = xi_const_str(function, entry, "-audit", &stub_owned_string);
    XiValue *text = xi_value_new(function, entry, XI_STR_CONCAT, &stub_owned_string, 2);
    XiValue *yield = xi_value_new(function, entry, XI_YIELD, &stub_unit, 0);
    XiValue *length = xi_value_new(function, entry, XI_LEN, &stub_int, 1);
    XiValue *release = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    REQUIRE(left && right && text && yield && length && release);
    text->args[0] = left;
    text->args[1] = right;
    length->args[0] = text;
    release->args[0] = text;
    for (int64_t value = 0; value < 8; value++)
        REQUIRE(xi_const_int(function, entry, value, &stub_int) != NULL);
    xi_block_set_return(entry, length);
    function->stage = XI_STAGE_SEMANTIC_LOWERED;
    function->invariant_mask = xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED);
    REQUIRE(xi_coro_lower(function, NULL));
    REQUIRE(function->coro_plan && function->coro_plan->nstates == 1 &&
            function->coro_plan->points[0].nroots == 1 &&
            function->coro_plan->points[0].ndrops == 1);
    function->stage = XI_STAGE_OPTIMIZED;
    /* The SemanticPlan builder requires a lowered graph to carry a typed
     * durable module identity and synthesizes none, so an in-memory probe
     * names its own memory-namespace identity. xi_func_free owns the module
     * it is attached to and releases it with the function. */
    XiModule *module =
        xi_module_new("typed_lifecycle_audit_probe.xr", "typed_lifecycle_audit_probe", function);
    REQUIRE(module != NULL);
    REQUIRE(xi_module_set_identity(module, "memory-module-v1:id=27:typed-lifecycle-audit-probe"));
    function->module = module;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(function, &semantic, error, sizeof(error)));
    xi_func_free(function);
    return semantic;
}

static AuditFixture make_fixture(bool native_profile) {
    AuditFixture fixture = {.semantic = build_lifecycle_semantic_plan()};
    char error[512] = {0};
    if (native_profile) {
        XrRuntimeTargetAuthority authority;
        REQUIRE(xr_runtime_target_authority_native_hosted(&authority) == XR_RUNTIME_ABI_OK);
        XrTargetProfileBuildInput input = {
            .machine = authority.machine,
            .runtime_abi = &authority.runtime_abi,
            .object_header_materialization = &authority.object_header_materialization,
            .string_contract = &authority.string_contract,
            .providers = authority.providers,
            .provider_count = authority.provider_count,
        };
        REQUIRE(xr_target_profile_build(&input, &fixture.profile, error, sizeof(error)));
    } else {
        fixture.profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
        REQUIRE(fixture.profile != NULL);
    }
    REQUIRE(xr_target_plan_build(fixture.semantic, fixture.profile, &fixture.plan, error,
                                 sizeof(error)));
    REQUIRE(xr_target_plan_is_verified(fixture.plan));
    return fixture;
}

static void dispose_fixture(AuditFixture *fixture) {
    xr_target_plan_free(fixture->plan);
    xr_target_profile_free(fixture->profile);
    xr_semantic_plan_free(fixture->semantic);
    memset(fixture, 0, sizeof(*fixture));
}

static AuditCoordinates find_coordinates(const AuditFixture *fixture) {
    REQUIRE(fixture && fixture->plan && fixture->semantic && fixture->plan->functions_count == 1 &&
            fixture->plan->root_maps_count == 1 && fixture->plan->root_slots_count == 1 &&
            fixture->plan->cleanups_count == 2 && fixture->plan->coroutines_count == 1);
    AuditCoordinates result = {
        .slot = fixture->plan->root_slots[0],
        .producer_operation = XR_SEMANTIC_INDEX_NONE,
        .normal_operation = XR_SEMANTIC_INDEX_NONE,
        .state_operation = fixture->plan->coroutines[0].semantic_operation,
        .owner_index = XR_SEMANTIC_INDEX_NONE,
        .event_index = XR_SEMANTIC_INDEX_NONE,
    };
    const XrTargetSlotRecord *slot = &fixture->plan->slots[result.slot];
    result.producer_operation = slot->semantic_operation;
    for (uint32_t i = 0; i < fixture->plan->cleanups_count; i++)
        if (fixture->plan->cleanups[i].flags == 0)
            result.normal_operation = fixture->plan->cleanups[i].semantic_operation;
    const XrOwnershipCertificate *certificate = xr_semantic_plan_ownership(fixture->semantic);
    REQUIRE(certificate && result.normal_operation != XR_SEMANTIC_INDEX_NONE);
    for (uint32_t i = 0; i < xr_ownership_certificate_owner_count(certificate); i++) {
        const XrOwnershipOwnerRecord *owner = xr_ownership_certificate_owner(certificate, i);
        if (owner && owner->origin_value == slot->semantic_value)
            result.owner_index = i;
    }
    for (uint32_t i = 0; i < xr_ownership_certificate_event_count(certificate); i++) {
        const XrOwnershipEventRecord *event = xr_ownership_certificate_event(certificate, i);
        if (event && event->owner == result.owner_index &&
            event->operation == result.normal_operation && event->kind == XR_OWN_EVENT_RELEASE)
            result.event_index = i;
    }
    REQUIRE(result.owner_index != XR_SEMANTIC_INDEX_NONE &&
            result.event_index != XR_SEMANTIC_INDEX_NONE);
    return result;
}

static XrStableId invocation_id(void) {
    XrStableId id = {{0}};
    XrFingerprint digest = {{0}};
    REQUIRE(xr_stable_id_from_key("typed-lifecycle-audit-invocation-v1", &id, &digest));
    return id;
}

static XrOwnershipAudit *make_oracle(size_t events) {
    XrOwnershipAuditConfig config = {
        .max_owner_manifests = 1,
        .max_transition_manifests = 1,
        .max_dynamic_instances = events,
        .max_events = events,
        .max_loans = 1,
        .max_generations = 1,
        .max_lifecycle_manifests = 1,
        .max_lifecycle_events = 1,
        .max_teardown_domains = 1,
    };
    XrOwnershipAuditStatus status = XR_OWN_AUDIT_INVALID_ARGUMENT;
    XrOwnershipAudit *audit = xr_ownership_audit_create(config, &status);
    REQUIRE(audit && status == XR_OWN_AUDIT_OK);
    return audit;
}

static AuditBundle make_adapter(const AuditFixture *fixture, size_t events, bool physical,
                                uint64_t first_epoch) {
    AuditBundle bundle = {.oracle = make_oracle(events)};
    XrFingerprint fingerprint = xr_target_plan_fingerprint(fixture->plan);
    XrTypedLifecycleAuditConfig config = {
        .verified_semantic_plan = fixture->semantic,
        .verified_target_plan = fixture->plan,
        .required_target_plan_fingerprint = &fingerprint,
        .oracle = bundle.oracle,
        .invocation_id = invocation_id(),
        .first_activation_epoch = first_epoch,
        .function = 0,
        .record_physical_rc = physical,
    };
    XrTypedLifecycleAuditStatus status = XR_TYPED_LIFECYCLE_AUDIT_INVALID_ARGUMENT;
    bundle.adapter = xr_typed_lifecycle_audit_create(&config, &status);
    REQUIRE(bundle.adapter && status == XR_TYPED_LIFECYCLE_AUDIT_OK);
    return bundle;
}

static void dispose_bundle(AuditBundle *bundle) {
    xr_typed_lifecycle_audit_destroy(bundle->adapter);
    xr_ownership_audit_destroy(bundle->oracle);
    memset(bundle, 0, sizeof(*bundle));
}

static void allocate_string(AllocationProbe *probe, bool retain_once) {
    static const char payload[] = "audit";
    uint64_t bytes = xr_runtime_string_object_allocation_bytes(sizeof(payload) - 1u);
    REQUIRE(probe && !probe->object && bytes <= SIZE_MAX);
    XrString *object = (XrString *) xr_calloc(1, (size_t) bytes);
    REQUIRE(object);
    REQUIRE(xr_runtime_string_object_init(object, XR_RUNTIME_STRING_DOMAIN_EXEC_LOCAL,
                                          sizeof(payload) - 1u, sizeof(payload) - 1u, 0,
                                          XR_RUNTIME_STRING_TRAIT_LOCAL) == XR_RUNTIME_ABI_OK);
    memcpy(object->data, payload, sizeof(payload));
    if (retain_once)
        REQUIRE(xr_runtime_object_header_retain(&object->header) == XR_RUNTIME_ABI_OK);
    probe->object = object;
    probe->address = (uintptr_t) object;
}

static XrRuntimeObjectHeader *resolve_object(void *opaque, uintptr_t address) {
    AllocationProbe *probe = (AllocationProbe *) opaque;
    REQUIRE(probe);
    probe->resolve_calls++;
    return probe->resolve_enabled && probe->object && address == probe->address
               ? &probe->object->header
               : NULL;
}

static void reclaim_object(void *opaque, XrRuntimeObjectHeader *header) {
    AllocationProbe *probe = (AllocationProbe *) opaque;
    REQUIRE(probe && probe->object && header == &probe->object->header);
    XrString *object = probe->object;
    probe->object = NULL;
    probe->reclaim_calls++;
    xr_free(object);
}

static XrTypedLifecycleStatus bridge_observer(void *opaque, const XrTypedLifecycleEvent *event) {
    ObserverBridge *bridge = (ObserverBridge *) opaque;
    REQUIRE(bridge && bridge->adapter && bridge->allocation && event);
    if (event->physical_last_release)
        REQUIRE(!bridge->allocation->object && bridge->allocation->reclaim_calls == 1);
    else
        REQUIRE(bridge->allocation->object && bridge->allocation->reclaim_calls == 0);
    bridge->calls++;
    XrTypedLifecycleEvent observation = *event;
    if (bridge->mutate_slot_identity)
        observation.slot_identity.bytes[0] ^= UINT8_C(1);
    return xr_typed_lifecycle_audit_observe(bridge->adapter, &observation);
}

static XrTypedFrame *make_frame(const AuditFixture *fixture) {
    XrTypedFrameLimits limits;
    xr_typed_frame_limits_default(&limits);
    XrFingerprint fingerprint = xr_target_plan_fingerprint(fixture->plan);
    XrTypedFrame *frame = NULL;
    REQUIRE(xr_typed_frame_create(fixture->plan, &fingerprint, 0, &limits, &frame) ==
                XR_TYPED_FRAME_OK &&
            frame);
    return frame;
}

static void write_carrier_field(uint8_t *carrier, size_t capacity,
                                const XrRuntimePhysicalFieldAbi *field, uint64_t value) {
    REQUIRE(field && field->width && field->width <= sizeof(value) && field->offset <= capacity &&
            field->width <= capacity - field->offset);
    memcpy(carrier + field->offset, &value, field->width);
}

static void store_owner(XrTypedLifecycleContext *context, XrTypedFrame *frame, uint32_t slot,
                        const AllocationProbe *probe) {
    uint8_t carrier[32] = {0};
    REQUIRE(context->dynamic_value.size <= sizeof(carrier));
    const XrRuntimeDynamicTagAbiEntry *tag = NULL;
    for (uint32_t i = 0; i < context->dynamic_value.tag_count; i++)
        if (context->dynamic_value.tags[i].encoding == context->dynamic_value.object_reference_tag)
            tag = &context->dynamic_value.tags[i];
    REQUIRE(tag && tag->payload_kind == XR_RUNTIME_DYN_PAYLOAD_OBJECT_REFERENCE);
    write_carrier_field(carrier, sizeof(carrier), &context->dynamic_value.fields[0],
                        context->dynamic_value.object_reference_tag);
    write_carrier_field(carrier, sizeof(carrier), &context->dynamic_value.fields[1],
                        tag->required_flags);
    write_carrier_field(carrier, sizeof(carrier), &context->dynamic_value.fields[2],
                        probe->address);
    XrTypedSlotAccess access = {0};
    REQUIRE(xr_typed_frame_describe_slot(frame, slot, &access) == XR_TYPED_FRAME_OK &&
            access.size == context->dynamic_value.size &&
            xr_typed_frame_store(frame, &access, carrier, access.size) == XR_TYPED_FRAME_OK);
}

static void finish_frame(XrTypedFrame **frame) {
    REQUIRE(xr_typed_frame_cleanup(*frame) == XR_TYPED_FRAME_OK);
    REQUIRE(xr_typed_frame_free(frame) == XR_TYPED_FRAME_OK && !*frame);
}

static XrTypedLifecycleContext make_executor(const AuditFixture *fixture, AllocationProbe *probe,
                                             ObserverBridge *bridge) {
    XrTypedLifecycleBindings bindings = {
        .resolve_object = resolve_object,
        .reclaim_object = reclaim_object,
        .allocation_context = probe,
        .observer = bridge_observer,
        .observer_context = bridge,
    };
    XrFingerprint fingerprint = xr_target_plan_fingerprint(fixture->plan);
    XrTypedLifecycleContext context = {0};
    REQUIRE(xr_typed_lifecycle_context_init(fixture->plan, &fingerprint, 0, &bindings, &context) ==
            XR_TYPED_LIFECYCLE_OK);
    return context;
}

static void execute_one(const AuditFixture *fixture, const AuditCoordinates *coordinates,
                        XrTypedLifecycleContext *executor, AllocationProbe *probe,
                        XrTypedLifecycleExit exit, bool retain_once,
                        XrTypedLifecycleStatus expected) {
    memset(probe, 0, sizeof(*probe));
    probe->resolve_enabled = true;
    allocate_string(probe, retain_once);
    XrTypedFrame *frame = make_frame(fixture);
    store_owner(executor, frame, coordinates->slot, probe);
    uint32_t operation = coordinates->normal_operation;
    if (exit != XR_TYPED_LIFECYCLE_EXIT_NORMAL) {
        operation = coordinates->state_operation;
        REQUIRE(xr_typed_frame_bind_coroutine_state(frame, 0) == XR_TYPED_FRAME_OK);
    }
    uint32_t executed = 0;
    REQUIRE(xr_typed_lifecycle_execute(executor, frame, operation, exit, &executed) == expected &&
            executed == 1 && probe->resolve_calls == 1);
    REQUIRE(xr_typed_lifecycle_execute(executor, frame, operation, exit, &executed) ==
                XR_TYPED_LIFECYCLE_ALREADY_EXECUTED &&
            executed == 0 && probe->resolve_calls == 1);
    finish_frame(&frame);
}

static void verify_oracle_events(const AuditFixture *fixture, const AuditCoordinates *coordinates,
                                 const AuditBundle *bundle, size_t count, uint64_t first_epoch,
                                 bool physical) {
    const XrOwnershipCertificate *certificate = xr_semantic_plan_ownership(fixture->semantic);
    const XrOwnershipOwnerRecord *owner =
        xr_ownership_certificate_owner(certificate, coordinates->owner_index);
    const XrOwnershipEventRecord *transition =
        xr_ownership_certificate_event(certificate, coordinates->event_index);
    const XrSemanticOperationRecord *producer =
        xr_semantic_plan_operation(fixture->semantic, coordinates->producer_operation);
    const XrSemanticOperationRecord *release =
        xr_semantic_plan_operation(fixture->semantic, coordinates->normal_operation);
    const XrSemanticFunctionRecord *function = xr_semantic_plan_function(fixture->semantic, 0);
    XrRuntimeStringObjectContract strings;
    REQUIRE(xr_runtime_string_object_contract_build(&strings) == XR_RUNTIME_ABI_OK);
    const XrRuntimeDomainIdentity expected_domain =
        strings.domains[XR_RUNTIME_STRING_DOMAIN_EXEC_LOCAL];
    REQUIRE(transition->program_point == XR_OWN_POINT_AFTER_OPERATION &&
            transition->successor == XR_SEMANTIC_INDEX_NONE);
    REQUIRE(xr_ownership_audit_event_count(bundle->oracle) == count);
    for (size_t i = 0; i < count; i++) {
        const XrOwnershipAuditEvent *event = xr_ownership_audit_event(bundle->oracle, i);
        REQUIRE(event && xr_stable_id_equal(event->object.owner_id, owner->id) &&
                xr_stable_id_equal(event->object.invocation_id, invocation_id()) &&
                event->object.activation_epoch == first_epoch + i &&
                xr_stable_id_equal(event->transition_id, transition->id) &&
                xr_stable_id_equal(event->operation_id, release->id) &&
                xr_stable_id_equal(event->exit_id, release->id) &&
                xr_stable_id_equal(event->allocation_site_id, producer->allocation_id) &&
                xr_stable_id_equal(event->frame_id, function->id) &&
                xr_fingerprint_equal(event->premise_fingerprint,
                                     xr_ownership_certificate_fingerprint(certificate)) &&
                xr_stable_id_equal(event->layout_id, strings.layout.layout_id) &&
                xr_stable_id_equal(event->domain.contract_id, expected_domain.contract_id) &&
                event->domain.instance_id == expected_domain.instance_id &&
                event->domain.semantic_domain == expected_domain.semantic_domain &&
                event->domain.materialization == expected_domain.materialization &&
                event->kind == transition->kind &&
                event->program_point == transition->program_point);
        if (physical) {
            REQUIRE(event->flags == XR_OWN_AUDIT_EVENT_PHYSICAL_RC &&
                    event->physical_rc_mode == XR_OWN_AUDIT_RC_LOCAL &&
                    event->physical_rc_before == (i == 4 ? 2 : 1) &&
                    event->physical_rc_after == (i == 4 ? 1 : 0));
        } else {
            REQUIRE(event->flags == 0 && event->physical_rc_before == 0 &&
                    event->physical_rc_after == 0 &&
                    event->physical_rc_mode == XR_OWN_AUDIT_RC_NONE);
        }
    }
}

static void test_all_exits_nonlast_retry_and_exact_once(void) {
    AuditFixture fixture = make_fixture(true);
    AuditCoordinates coordinates = find_coordinates(&fixture);
    AuditBundle bundle = make_adapter(&fixture, 8, true, 41);
    AllocationProbe probe = {0};
    ObserverBridge bridge = {.adapter = bundle.adapter, .allocation = &probe};
    XrTypedLifecycleContext executor = make_executor(&fixture, &probe, &bridge);
    for (uint32_t exit = XR_TYPED_LIFECYCLE_EXIT_NORMAL; exit < XR_TYPED_LIFECYCLE_EXIT_COUNT;
         exit++)
        execute_one(&fixture, &coordinates, &executor, &probe, (XrTypedLifecycleExit) exit, false,
                    XR_TYPED_LIFECYCLE_OK);
    execute_one(&fixture, &coordinates, &executor, &probe, XR_TYPED_LIFECYCLE_EXIT_NORMAL, true,
                XR_TYPED_LIFECYCLE_OK);
    REQUIRE(probe.object && probe.reclaim_calls == 0 && bridge.calls == 5 &&
            atomic_load_explicit(&probe.object->header.rc, memory_order_acquire) == 1);
    bool last = false;
    REQUIRE(xr_runtime_object_header_release(&probe.object->header, &last) == XR_RUNTIME_ABI_OK &&
            last);
    reclaim_object(&probe, &probe.object->header);

    memset(&probe, 0, sizeof(probe));
    allocate_string(&probe, false);
    XrTypedFrame *frame = make_frame(&fixture);
    store_owner(&executor, frame, coordinates.slot, &probe);
    uint32_t executed = UINT32_MAX;
    REQUIRE(xr_typed_lifecycle_execute(&executor, frame, coordinates.normal_operation,
                                       XR_TYPED_LIFECYCLE_EXIT_NORMAL,
                                       &executed) == XR_TYPED_LIFECYCLE_CARRIER_INVALID &&
            executed == 0 && bridge.calls == 5 && probe.object &&
            atomic_load_explicit(&probe.object->header.rc, memory_order_acquire) == 1);
    probe.resolve_enabled = true;
    REQUIRE(xr_typed_lifecycle_execute(&executor, frame, coordinates.normal_operation,
                                       XR_TYPED_LIFECYCLE_EXIT_NORMAL,
                                       &executed) == XR_TYPED_LIFECYCLE_OK &&
            executed == 1 && bridge.calls == 6 && !probe.object && probe.reclaim_calls == 1);
    REQUIRE(xr_typed_lifecycle_execute(&executor, frame, coordinates.normal_operation,
                                       XR_TYPED_LIFECYCLE_EXIT_NORMAL,
                                       &executed) == XR_TYPED_LIFECYCLE_ALREADY_EXECUTED &&
            executed == 0 && bridge.calls == 6 && probe.reclaim_calls == 1);
    finish_frame(&frame);
    verify_oracle_events(&fixture, &coordinates, &bundle, 6, 41, true);
    REQUIRE(xr_typed_lifecycle_audit_status(bundle.adapter) == XR_TYPED_LIFECYCLE_AUDIT_OK &&
            xr_ownership_audit_finish(bundle.oracle) == XR_OWN_AUDIT_OK);
    xr_typed_lifecycle_context_dispose(&executor);
    dispose_bundle(&bundle);
    dispose_fixture(&fixture);
}

static XrTypedLifecycleEvent valid_observation(const AuditFixture *fixture,
                                               const AuditCoordinates *coordinates) {
    return (XrTypedLifecycleEvent) {
        .plan_fingerprint = xr_target_plan_fingerprint(fixture->plan),
        .slot_identity = fixture->plan->slots[coordinates->slot].identity,
        .function = 0,
        .semantic_operation = coordinates->normal_operation,
        .slot = coordinates->slot,
        .physical_rc_before = 1,
        .physical_rc_after = 0,
        .action = XR_TARGET_CLEANUP_RELEASE,
        .exit_kind = XR_TYPED_LIFECYCLE_EXIT_NORMAL,
        .physical_last_release = 1,
    };
}

static void test_mutation_is_sticky_and_release_is_not_retryable(void) {
    AuditFixture fixture = make_fixture(true);
    AuditCoordinates coordinates = find_coordinates(&fixture);
    AuditBundle bundle = make_adapter(&fixture, 2, true, 1);
    AllocationProbe probe = {0};
    ObserverBridge bridge = {
        .adapter = bundle.adapter,
        .allocation = &probe,
        .mutate_slot_identity = true,
    };
    XrTypedLifecycleContext executor = make_executor(&fixture, &probe, &bridge);
    execute_one(&fixture, &coordinates, &executor, &probe, XR_TYPED_LIFECYCLE_EXIT_NORMAL, false,
                XR_TYPED_LIFECYCLE_AUDIT_REJECTED);
    REQUIRE(!probe.object && probe.reclaim_calls == 1 && bridge.calls == 1 &&
            xr_ownership_audit_event_count(bundle.oracle) == 0 &&
            xr_typed_lifecycle_audit_status(bundle.adapter) ==
                XR_TYPED_LIFECYCLE_AUDIT_PLAN_MISMATCH &&
            xr_typed_lifecycle_audit_observe(bundle.adapter, &(XrTypedLifecycleEvent) {0}) ==
                XR_TYPED_LIFECYCLE_AUDIT_REJECTED);
    xr_typed_lifecycle_context_dispose(&executor);
    dispose_bundle(&bundle);

    bundle = make_adapter(&fixture, 1, true, 1);
    XrTypedLifecycleEvent observation = valid_observation(&fixture, &coordinates);
    uint32_t original_flags = fixture.plan->root_maps[0].flags;
    fixture.plan->root_maps[0].flags ^= XR_TARGET_ROOT_CANCEL;
    REQUIRE(xr_typed_lifecycle_audit_observe(bundle.adapter, &observation) ==
                XR_TYPED_LIFECYCLE_AUDIT_REJECTED &&
            xr_ownership_audit_event_count(bundle.oracle) == 0);
    fixture.plan->root_maps[0].flags = original_flags;
    dispose_bundle(&bundle);

    bundle = make_adapter(&fixture, 1, true, 1);
    XrOwnershipCertificate *certificate =
        (XrOwnershipCertificate *) xr_semantic_plan_ownership(fixture.semantic);
    XrStableId original_owner = certificate->owners[coordinates.owner_index].id;
    certificate->owners[coordinates.owner_index].id.bytes[0] ^= UINT8_C(1);
    REQUIRE(xr_typed_lifecycle_audit_observe(bundle.adapter, &observation) ==
                XR_TYPED_LIFECYCLE_AUDIT_REJECTED &&
            xr_ownership_audit_event_count(bundle.oracle) == 0);
    certificate->owners[coordinates.owner_index].id = original_owner;
    dispose_bundle(&bundle);

    bundle = make_adapter(&fixture, 1, true, 1);
    uint32_t original_block = certificate->events[coordinates.event_index].block;
    certificate->events[coordinates.event_index].block ^= UINT32_C(1);
    REQUIRE(xr_typed_lifecycle_audit_observe(bundle.adapter, &observation) ==
                XR_TYPED_LIFECYCLE_AUDIT_REJECTED &&
            xr_ownership_audit_event_count(bundle.oracle) == 0);
    certificate->events[coordinates.event_index].block = original_block;
    dispose_bundle(&bundle);

    bundle = make_adapter(&fixture, 1, true, 1);
    XrStableId original_producer = fixture.semantic->operations[coordinates.producer_operation].id;
    fixture.semantic->operations[coordinates.producer_operation].id.bytes[0] ^= UINT8_C(1);
    REQUIRE(xr_typed_lifecycle_audit_observe(bundle.adapter, &observation) ==
                XR_TYPED_LIFECYCLE_AUDIT_REJECTED &&
            xr_ownership_audit_event_count(bundle.oracle) == 0);
    fixture.semantic->operations[coordinates.producer_operation].id = original_producer;
    dispose_bundle(&bundle);

    bundle = make_adapter(&fixture, 1, true, 1);
    observation.slot++;
    REQUIRE(xr_typed_lifecycle_audit_observe(bundle.adapter, &observation) ==
                XR_TYPED_LIFECYCLE_AUDIT_REJECTED &&
            xr_ownership_audit_event_count(bundle.oracle) == 0);
    dispose_bundle(&bundle);
    dispose_fixture(&fixture);
}

static void test_logical_only_and_wrong_profile(void) {
    AuditFixture fixture = make_fixture(true);
    AuditCoordinates coordinates = find_coordinates(&fixture);
    AuditBundle bundle = make_adapter(&fixture, 1, false, 7);
    XrTypedLifecycleEvent observation = valid_observation(&fixture, &coordinates);
    REQUIRE(xr_typed_lifecycle_audit_observe(bundle.adapter, &observation) ==
            XR_TYPED_LIFECYCLE_OK);
    verify_oracle_events(&fixture, &coordinates, &bundle, 1, 7, false);
    REQUIRE(xr_ownership_audit_finish(bundle.oracle) == XR_OWN_AUDIT_OK);
    dispose_bundle(&bundle);
    dispose_fixture(&fixture);

    fixture = make_fixture(false);
    XrOwnershipAudit *oracle = make_oracle(1);
    XrFingerprint fingerprint = xr_target_plan_fingerprint(fixture.plan);
    XrTypedLifecycleAuditConfig config = {
        .verified_semantic_plan = fixture.semantic,
        .verified_target_plan = fixture.plan,
        .required_target_plan_fingerprint = &fingerprint,
        .oracle = oracle,
        .invocation_id = invocation_id(),
        .first_activation_epoch = 1,
        .function = 0,
    };
    XrTypedLifecycleAuditStatus status = XR_TYPED_LIFECYCLE_AUDIT_OK;
    REQUIRE(!xr_typed_lifecycle_audit_create(&config, &status) &&
            status == XR_TYPED_LIFECYCLE_AUDIT_PLAN_MISMATCH &&
            xr_ownership_audit_event_count(oracle) == 0);
    xr_ownership_audit_destroy(oracle);
    dispose_fixture(&fixture);
}

int main(void) {
    test_all_exits_nonlast_retry_and_exact_once();
    test_mutation_is_sticky_and_release_is_not_retryable();
    test_logical_only_and_wrong_profile();
    puts("typed lifecycle ownership audit tests passed");
    return 0;
}
