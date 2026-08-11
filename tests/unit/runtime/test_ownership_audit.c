#include "runtime/ownership/xr_ownership_audit.h"
#include "base/xplatform.h"

#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#ifdef XR_OS_WINDOWS
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

static int failures;

#define CHECK(condition, message)                                                             \
    do {                                                                                      \
        if (!(condition)) {                                                                   \
            fprintf(stderr, "FAIL:%d: %s\n", __LINE__, (message));                          \
            failures++;                                                                       \
        }                                                                                     \
    } while (0)

enum {
    ID_OWNER = 10,
    ID_LAYOUT = 11,
    ID_DESCRIPTOR = 12,
    ID_OBJECT_KIND = 13,
    ID_EXTENT = 14,
    ID_ALLOCATION_SITE = 15,
    ID_FRAME = 16,
    ID_GENERATION = 17,
    ID_DESTRUCTOR = 18,
    ID_PREMISE = 19,
    ID_PIN = 20,
    ID_ALLOC = 21,
    ID_BORROW = 22,
    ID_END_BORROW = 23,
    ID_SUSPEND = 24,
    ID_RESUME = 25,
    ID_DETACH = 26,
    ID_RETAIN = 27,
    ID_RELEASE_ONE = 28,
    ID_RELEASE_ZERO = 29,
    ID_DESTROY = 30,
    ID_UNPIN = 31,
    ID_EXIT = 32,
    ID_LOCAL_DOMAIN = 33,
    ID_SHARED_DOMAIN = 34,
    ID_LOAN = 35,
    ID_BEGIN_TEARDOWN = 36,
    ID_BEGIN_FINALIZE = 37,
    ID_END_FINALIZE = 38,
    ID_RECLAIM = 39,
    ID_END_TEARDOWN = 40,
    ID_BEGIN_LOCAL_TEARDOWN = 41,
    ID_END_LOCAL_TEARDOWN = 42,
    ID_CANCEL = 43,
    ID_RELEASE_LOCAL_MODE = 44,
};

typedef struct Fixture {
    XrRuntimeExtentDescriptor extent;
    XrRuntimeLayoutDescriptor layout;
    XrOwnershipAuditOwnerManifest owner;
    XrRuntimeDomainIdentity local_domain;
    XrRuntimeDomainIdentity shared_domain;
} Fixture;

static XrStableId make_id(uint8_t seed) {
    XrStableId id = {{0}};
    for (size_t i = 0; i < sizeof(id.bytes); i++)
        id.bytes[i] = (uint8_t) (seed + i);
    return id;
}

static XrStableId make_wide_id(uint64_t value) {
    XrStableId id = {{0}};
    for (size_t i = 0; i < 8; i++) {
        id.bytes[i] = (uint8_t) (value >> (i * 8));
        id.bytes[i + 8] = (uint8_t) (~value >> (i * 8));
    }
    return id;
}

static XrFingerprint make_fingerprint(uint8_t seed) {
    XrFingerprint fingerprint = {{0}};
    for (size_t i = 0; i < sizeof(fingerprint.bytes); i++)
        fingerprint.bytes[i] = (uint8_t) (seed + i);
    return fingerprint;
}

static void seal_extent(XrRuntimeExtentDescriptor *extent) {
    CHECK(xr_runtime_extent_descriptor_fingerprint(extent, &extent->fingerprint) ==
              XR_RUNTIME_ABI_OK,
          "fixture extent seals");
}

static void seal_layout(XrRuntimeLayoutDescriptor *layout) {
    CHECK(xr_runtime_layout_descriptor_fingerprint(layout, &layout->fingerprint) ==
              XR_RUNTIME_ABI_OK,
          "fixture layout seals");
}

static Fixture make_fixture(void) {
    Fixture fixture;
    memset(&fixture, 0, sizeof(fixture));
    fixture.extent = (XrRuntimeExtentDescriptor) {
        .schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .id = make_id(ID_EXTENT),
        .layout_id = make_id(ID_LAYOUT),
        .operand_index = XR_RUNTIME_EXTENT_OPERAND_NONE,
        .part_count = 1,
        .kind = XR_RUNTIME_EXTENT_FIXED,
    };
    seal_extent(&fixture.extent);
    fixture.layout = (XrRuntimeLayoutDescriptor) {
        .schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .descriptor_id = make_id(ID_DESCRIPTOR),
        .layout_id = make_id(ID_LAYOUT),
        .object_kind_id = make_id(ID_OBJECT_KIND),
        .extent_id = make_id(ID_EXTENT),
        .destructor_id = make_id(ID_DESTRUCTOR),
        .extent_fingerprint = fixture.extent.fingerprint,
        .fixed_prefix_size = 24,
        .alignment = 8,
        .allowed_semantic_domains =
            XR_SEMANTIC_DOMAIN_MASK(XR_STORAGE_EXEC_LOCAL) |
            XR_SEMANTIC_DOMAIN_MASK(XR_STORAGE_SYNC_SHARED),
        .allowed_materializations =
            XR_MATERIALIZATION_MASK(XR_MATERIALIZE_EXEC_HEAP) |
            XR_MATERIALIZATION_MASK(XR_MATERIALIZE_SYSTEM_HEAP),
        .flags = XR_LAYOUT_HAS_DESTRUCTOR,
    };
    seal_layout(&fixture.layout);
    fixture.local_domain = (XrRuntimeDomainIdentity) {
        .contract_id = make_id(ID_LOCAL_DOMAIN),
        .instance_id = 700,
        .semantic_domain = XR_STORAGE_EXEC_LOCAL,
        .materialization = XR_MATERIALIZE_EXEC_HEAP,
    };
    fixture.shared_domain = (XrRuntimeDomainIdentity) {
        .contract_id = make_id(ID_SHARED_DOMAIN),
        .instance_id = 900,
        .semantic_domain = XR_STORAGE_SYNC_SHARED,
        .materialization = XR_MATERIALIZE_SYSTEM_HEAP,
    };
    fixture.owner = (XrOwnershipAuditOwnerManifest) {
        .owner_id = make_id(ID_OWNER),
        .descriptor = &fixture.layout,
        .extent = &fixture.extent,
        .allocation_site_id = make_id(ID_ALLOCATION_SITE),
        .frame_id = make_id(ID_FRAME),
        .generation_id = make_id(ID_GENERATION),
        .destructor_id = make_id(ID_DESTRUCTOR),
        .premise_fingerprint = make_fingerprint(ID_PREMISE),
        .initial_domain_contract_id = make_id(ID_LOCAL_DOMAIN),
        .initial_logical_balance = 0,
        .flags = XR_OWN_AUDIT_REQUIRE_GENERATION_PIN,
        .initial_state = XR_OWN_UNINITIALIZED,
        .initial_semantic_domain = XR_STORAGE_EXEC_LOCAL,
        .initial_materialization = XR_MATERIALIZE_EXEC_HEAP,
    };
    return fixture;
}

static Fixture make_lifecycle_fixture(void) {
    Fixture fixture = make_fixture();
    fixture.owner.flags |= XR_OWN_AUDIT_TRACK_ALLOCATION_LIFECYCLE;
    return fixture;
}

static XrOwnershipAuditConfig audit_config(size_t event_capacity) {
    return (XrOwnershipAuditConfig) {.max_owner_manifests = 2,
                                     .max_transition_manifests = 16,
                                     .max_dynamic_instances = 8,
                                     .max_events = event_capacity,
                                     .max_loans = 4,
                                     .max_generations = 2,
                                     .max_lifecycle_manifests = 8,
                                     .max_lifecycle_events = 8,
                                     .max_teardown_domains = 2};
}

static XrOwnershipAudit *new_audit(size_t event_capacity) {
    XrOwnershipAuditStatus status;
    XrOwnershipAudit *audit = xr_ownership_audit_create(audit_config(event_capacity), &status);
    CHECK(audit != NULL && status == XR_OWN_AUDIT_OK, "audit heap preallocates");
    return audit;
}

static XrOwnershipAuditTransitionManifest transition(
    uint8_t id, uint8_t kind, uint32_t flags, int16_t logical_delta,
    uint32_t state_before_mask, uint8_t state_after) {
    XrOwnershipAuditTransitionManifest manifest = {
        .transition_id = make_id(id),
        .owner_id = make_id(ID_OWNER),
        .operation_id = make_id((uint8_t) (id + 80)),
        .state_before_mask = state_before_mask,
        .flags = flags,
        .logical_delta = logical_delta,
        .kind = kind,
        .state_after = state_after,
        .program_point = XR_OWN_POINT_AFTER_OPERATION,
        .physical_rc_mode =
            kind == XR_OWN_EVENT_RETAIN || kind == XR_OWN_EVENT_RELEASE
                ? XR_OWN_AUDIT_RC_SHARED
                : XR_OWN_AUDIT_RC_NONE,
    };
    if ((flags & XR_OWN_AUDIT_TRANSITION_TERMINAL) != 0)
        manifest.exit_id = make_id(ID_EXIT);
    if ((flags & XR_OWN_AUDIT_TRANSITION_CHANGES_DOMAIN) != 0) {
        manifest.next_domain_contract_id = make_id(ID_SHARED_DOMAIN);
        manifest.next_semantic_domain = XR_STORAGE_SYNC_SHARED;
        manifest.next_materialization = XR_MATERIALIZE_SYSTEM_HEAP;
    }
    return manifest;
}

static void register_transition(XrOwnershipAudit *audit,
                                XrOwnershipAuditTransitionManifest manifest) {
    CHECK(xr_ownership_audit_register_transition(audit, &manifest) == XR_OWN_AUDIT_OK,
          "transition manifest registers");
}

static void register_manifests(XrOwnershipAudit *audit, const Fixture *fixture) {
    XrOwnershipAuditOwnerManifest owner = fixture->owner;
    owner.descriptor = &fixture->layout;
    owner.extent = &fixture->extent;
    CHECK(xr_ownership_audit_register_owner(audit, &owner) == XR_OWN_AUDIT_OK,
          "owner manifest registers");

    XrOwnershipAuditTransitionManifest pin = {
        .transition_id = make_id(ID_PIN),
        .operation_id = make_id((uint8_t) (ID_PIN + 80)),
        .generation_id = make_id(ID_GENERATION),
        .kind = XR_OWN_EVENT_PIN,
        .program_point = XR_OWN_POINT_AFTER_OPERATION,
    };
    register_transition(audit, pin);
    register_transition(audit, transition(ID_ALLOC, XR_OWN_EVENT_ALLOC,
                                          XR_OWN_AUDIT_TRANSITION_OPENS_INSTANCE, 1,
                                          XR_OWN_STATE_MASK(XR_OWN_UNINITIALIZED),
                                          XR_OWN_OWNED_LOCAL));
    register_transition(audit, transition(ID_BORROW, XR_OWN_EVENT_BORROW, 0, 0,
                                          XR_OWN_STATE_MASK(XR_OWN_OWNED_LOCAL),
                                          XR_OWN_BORROWED));
    register_transition(audit, transition(ID_END_BORROW, XR_OWN_EVENT_END_BORROW, 0, 0,
                                          XR_OWN_STATE_MASK(XR_OWN_BORROWED),
                                          XR_OWN_OWNED_LOCAL));
    register_transition(audit, transition(ID_SUSPEND, XR_OWN_EVENT_SUSPEND, 0, 0,
                                          XR_OWN_STATE_MASK(XR_OWN_OWNED_LOCAL),
                                          XR_OWN_FRAME_OWNED));
    register_transition(audit, transition(ID_RESUME, XR_OWN_EVENT_RESUME, 0, 0,
                                          XR_OWN_STATE_MASK(XR_OWN_FRAME_OWNED),
                                          XR_OWN_OWNED_LOCAL));
    register_transition(audit, transition(ID_DETACH, XR_OWN_EVENT_DETACH,
                                          XR_OWN_AUDIT_TRANSITION_CHANGES_DOMAIN, 0,
                                          XR_OWN_STATE_MASK(XR_OWN_OWNED_LOCAL),
                                          XR_OWN_OWNED_LOCAL));
    register_transition(audit, transition(ID_RETAIN, XR_OWN_EVENT_RETAIN, 0, 1,
                                          XR_OWN_STATE_MASK(XR_OWN_OWNED_LOCAL),
                                          XR_OWN_OWNED_LOCAL));
    register_transition(audit, transition(ID_RELEASE_ONE, XR_OWN_EVENT_RELEASE, 0, -1,
                                          XR_OWN_STATE_MASK(XR_OWN_OWNED_LOCAL),
                                          XR_OWN_OWNED_LOCAL));
    register_transition(audit, transition(ID_RELEASE_ZERO, XR_OWN_EVENT_RELEASE, 0, -1,
                                          XR_OWN_STATE_MASK(XR_OWN_OWNED_LOCAL),
                                          XR_OWN_RELEASED));
    register_transition(audit, transition(ID_DESTROY, XR_OWN_EVENT_DESTROY,
                                          XR_OWN_AUDIT_TRANSITION_TERMINAL, 0,
                                          XR_OWN_STATE_MASK(XR_OWN_RELEASED),
                                          XR_OWN_RELEASED));
    register_transition(audit, transition(ID_CANCEL, XR_OWN_EVENT_CANCEL,
                                          XR_OWN_AUDIT_TRANSITION_TERMINAL, -1,
                                          XR_OWN_STATE_MASK(XR_OWN_OWNED_LOCAL),
                                          XR_OWN_RELEASED));
    XrOwnershipAuditTransitionManifest unpin = pin;
    unpin.transition_id = make_id(ID_UNPIN);
    unpin.operation_id = make_id((uint8_t) (ID_UNPIN + 80));
    unpin.kind = XR_OWN_EVENT_UNPIN;
    register_transition(audit, unpin);
}

static XrOwnershipAuditLifecycleManifest domain_lifecycle_manifest(
    uint8_t id, uint8_t kind, XrStableId contract_id, uint8_t semantic_domain,
    uint8_t materialization) {
    return (XrOwnershipAuditLifecycleManifest) {
        .transition_id = make_id(id),
        .operation_id = make_id((uint8_t) (id + 80)),
        .domain_contract_id = contract_id,
        .kind = kind,
        .semantic_domain = semantic_domain,
        .materialization = materialization,
    };
}

static XrOwnershipAuditLifecycleManifest object_lifecycle_manifest(uint8_t id,
                                                                   uint8_t kind) {
    return (XrOwnershipAuditLifecycleManifest) {
        .transition_id = make_id(id),
        .owner_id = make_id(ID_OWNER),
        .operation_id = make_id((uint8_t) (id + 80)),
        .destructor_id = make_id(ID_DESTRUCTOR),
        .kind = kind,
    };
}

static void register_lifecycle_manifest(
    XrOwnershipAudit *audit, XrOwnershipAuditLifecycleManifest manifest) {
    CHECK(xr_ownership_audit_register_lifecycle(audit, &manifest) == XR_OWN_AUDIT_OK,
          "lifecycle manifest registers");
}

static void register_lifecycle_manifests(XrOwnershipAudit *audit,
                                         const Fixture *fixture) {
    register_lifecycle_manifest(
        audit, domain_lifecycle_manifest(ID_BEGIN_TEARDOWN,
                                         XR_OWN_AUDIT_LIFECYCLE_BEGIN_TEARDOWN,
                                         fixture->shared_domain.contract_id,
                                         fixture->shared_domain.semantic_domain,
                                         fixture->shared_domain.materialization));
    register_lifecycle_manifest(
        audit, object_lifecycle_manifest(ID_BEGIN_FINALIZE,
                                         XR_OWN_AUDIT_LIFECYCLE_BEGIN_FINALIZE));
    register_lifecycle_manifest(
        audit, object_lifecycle_manifest(ID_END_FINALIZE,
                                         XR_OWN_AUDIT_LIFECYCLE_END_FINALIZE));
    register_lifecycle_manifest(
        audit, object_lifecycle_manifest(ID_RECLAIM,
                                         XR_OWN_AUDIT_LIFECYCLE_RECLAIM));
    register_lifecycle_manifest(
        audit, domain_lifecycle_manifest(ID_END_TEARDOWN,
                                         XR_OWN_AUDIT_LIFECYCLE_END_TEARDOWN,
                                         fixture->shared_domain.contract_id,
                                         fixture->shared_domain.semantic_domain,
                                         fixture->shared_domain.materialization));
    register_lifecycle_manifest(
        audit, domain_lifecycle_manifest(ID_BEGIN_LOCAL_TEARDOWN,
                                         XR_OWN_AUDIT_LIFECYCLE_BEGIN_TEARDOWN,
                                         fixture->local_domain.contract_id,
                                         fixture->local_domain.semantic_domain,
                                         fixture->local_domain.materialization));
    register_lifecycle_manifest(
        audit, domain_lifecycle_manifest(ID_END_LOCAL_TEARDOWN,
                                         XR_OWN_AUDIT_LIFECYCLE_END_TEARDOWN,
                                         fixture->local_domain.contract_id,
                                         fixture->local_domain.semantic_domain,
                                         fixture->local_domain.materialization));
}

static XrOwnershipAuditEvent pin_event(uint8_t transition_id, uint8_t kind) {
    return (XrOwnershipAuditEvent) {
        .transition_id = make_id(transition_id),
        .operation_id = make_id((uint8_t) (transition_id + 80)),
        .generation_id = make_id(ID_GENERATION),
        .kind = kind,
        .program_point = XR_OWN_POINT_AFTER_OPERATION,
    };
}

static XrOwnershipAuditObjectKey object_key(uint8_t invocation, uint64_t epoch) {
    return (XrOwnershipAuditObjectKey) {.owner_id = make_id(ID_OWNER),
                                        .invocation_id = make_id(invocation),
                                        .activation_epoch = epoch};
}

static XrOwnershipAuditEvent owner_event(const Fixture *fixture,
                                         XrOwnershipAuditObjectKey object,
                                         uint8_t transition_id, uint8_t kind,
                                         XrRuntimeDomainIdentity domain) {
    XrOwnershipAuditEvent event = {
        .object = object,
        .transition_id = make_id(transition_id),
        .layout_id = make_id(ID_LAYOUT),
        .allocation_site_id = make_id(ID_ALLOCATION_SITE),
        .operation_id = make_id((uint8_t) (transition_id + 80)),
        .frame_id = make_id(ID_FRAME),
        .generation_id = make_id(ID_GENERATION),
        .premise_fingerprint = fixture->owner.premise_fingerprint,
        .domain = domain,
        .kind = kind,
        .program_point = XR_OWN_POINT_AFTER_OPERATION,
    };
    if (transition_id == ID_DETACH)
        event.next_domain = fixture->shared_domain;
    if (transition_id == ID_DESTROY) {
        event.exit_id = make_id(ID_EXIT);
        event.destructor_id = make_id(ID_DESTRUCTOR);
    }
    if (transition_id == ID_CANCEL)
        event.exit_id = make_id(ID_EXIT);
    if (transition_id == ID_BORROW || transition_id == ID_END_BORROW)
        event.loan_id = make_id(ID_LOAN);
    return event;
}

static XrOwnershipAuditLifecycleEvent lifecycle_event(
    XrOwnershipAuditObjectKey object, uint8_t transition_id, uint8_t kind,
    XrRuntimeDomainIdentity domain) {
    XrOwnershipAuditLifecycleEvent event = {
        .object = object,
        .transition_id = make_id(transition_id),
        .operation_id = make_id((uint8_t) (transition_id + 80)),
        .domain = domain,
        .kind = kind,
    };
    if (kind != XR_OWN_AUDIT_LIFECYCLE_BEGIN_TEARDOWN &&
        kind != XR_OWN_AUDIT_LIFECYCLE_END_TEARDOWN)
        event.destructor_id = make_id(ID_DESTRUCTOR);
    return event;
}

static XrOwnershipAuditStatus record(XrOwnershipAudit *audit,
                                     XrOwnershipAuditEvent event) {
    return xr_ownership_audit_record(audit, &event);
}

static XrOwnershipAuditStatus record_lifecycle(
    XrOwnershipAudit *audit, XrOwnershipAuditLifecycleEvent event) {
    return xr_ownership_audit_record_lifecycle(audit, &event);
}

static XrRuntimeDomainIdentity advance_to_destroy(XrOwnershipAudit *audit,
                                                  const Fixture *fixture,
                                                  XrOwnershipAuditObjectKey object) {
    XrRuntimeDomainIdentity domain = fixture->local_domain;
    CHECK(record(audit, owner_event(fixture, object, ID_ALLOC, XR_OWN_EVENT_ALLOC, domain)) ==
              XR_OWN_AUDIT_OK,
          "allocation opens a dynamic instance from the static manifest");
    CHECK(record(audit, owner_event(fixture, object, ID_BORROW, XR_OWN_EVENT_BORROW, domain)) ==
              XR_OWN_AUDIT_OK,
          "borrow transition verifies");
    CHECK(record(audit, owner_event(fixture, object, ID_END_BORROW,
                                    XR_OWN_EVENT_END_BORROW, domain)) == XR_OWN_AUDIT_OK,
          "borrow closure verifies");
    CHECK(record(audit, owner_event(fixture, object, ID_SUSPEND, XR_OWN_EVENT_SUSPEND,
                                    domain)) == XR_OWN_AUDIT_OK,
          "suspend transition verifies");
    CHECK(record(audit, owner_event(fixture, object, ID_RESUME, XR_OWN_EVENT_RESUME,
                                    domain)) == XR_OWN_AUDIT_OK,
          "resume transition verifies");
    CHECK(record(audit, owner_event(fixture, object, ID_DETACH, XR_OWN_EVENT_DETACH,
                                    domain)) == XR_OWN_AUDIT_OK,
          "non-terminal detach changes the exact domain identity");
    domain = fixture->shared_domain;

    XrOwnershipAuditEvent retain =
        owner_event(fixture, object, ID_RETAIN, XR_OWN_EVENT_RETAIN, domain);
    retain.flags = XR_OWN_AUDIT_EVENT_PHYSICAL_RC;
    retain.physical_rc_before = 1;
    retain.physical_rc_after = 2;
    retain.physical_rc_mode = XR_OWN_AUDIT_RC_SHARED;
    CHECK(record(audit, retain) == XR_OWN_AUDIT_OK,
          "logical retain accepts exact optional physical telemetry");

    XrOwnershipAuditEvent release_one =
        owner_event(fixture, object, ID_RELEASE_ONE, XR_OWN_EVENT_RELEASE, domain);
    release_one.flags = XR_OWN_AUDIT_EVENT_PHYSICAL_RC;
    release_one.physical_rc_before = 2;
    release_one.physical_rc_after = 1;
    release_one.physical_rc_mode = XR_OWN_AUDIT_RC_SHARED;
    CHECK(record(audit, release_one) == XR_OWN_AUDIT_OK,
          "physical telemetry chains observed before and after values");

    XrOwnershipAuditEvent release_zero =
        owner_event(fixture, object, ID_RELEASE_ZERO, XR_OWN_EVENT_RELEASE, domain);
    release_zero.flags = XR_OWN_AUDIT_EVENT_PHYSICAL_RC;
    release_zero.physical_rc_before = 1;
    release_zero.physical_rc_after = 0;
    release_zero.physical_rc_mode = XR_OWN_AUDIT_RC_SHARED;
    CHECK(record(audit, release_zero) == XR_OWN_AUDIT_OK,
          "last release reaches the observed zero count");
    return domain;
}

static void execute_instance(XrOwnershipAudit *audit, const Fixture *fixture,
                             XrOwnershipAuditObjectKey object) {
    XrRuntimeDomainIdentity domain = advance_to_destroy(audit, fixture, object);
    CHECK(record(audit, owner_event(fixture, object, ID_DESTROY, XR_OWN_EVENT_DESTROY,
                                    domain)) == XR_OWN_AUDIT_OK,
          "terminal destructor and exit disposition verify");
}

static void test_dynamic_instances_and_event_reuse(void) {
    Fixture fixture = make_fixture();
    XrOwnershipAudit *audit = new_audit(32);
    register_manifests(audit, &fixture);
    CHECK(record(audit, pin_event(ID_PIN, XR_OWN_EVENT_PIN)) == XR_OWN_AUDIT_OK,
          "generation pin records before object activation");
    execute_instance(audit, &fixture, object_key(60, 1));
    execute_instance(audit, &fixture, object_key(61, 1));
    CHECK(record(audit, pin_event(ID_UNPIN, XR_OWN_EVENT_UNPIN)) == XR_OWN_AUDIT_OK,
          "generation pin closes after both dynamic instances");
    CHECK(xr_ownership_audit_finish(audit) == XR_OWN_AUDIT_OK,
          "all executed dynamic instances reach manifest terminals");
    CHECK(xr_ownership_audit_event_count(audit) == 22,
          "bounded history contains both repeated event paths");
    CHECK(strcmp(xr_ownership_audit_evidence_scope(),
                 "executed-path dynamic evidence, not formal proof") == 0,
          "reporting scope explicitly rejects formal-proof claims");
    xr_ownership_audit_destroy(audit);
}

typedef void (*MutationFn)(XrOwnershipAuditEvent *event, Fixture *fixture);

static void wrong_transition(XrOwnershipAuditEvent *event, Fixture *fixture) {
    (void) fixture;
    event->transition_id = make_id(240);
}

static void wrong_operation(XrOwnershipAuditEvent *event, Fixture *fixture) {
    (void) fixture;
    event->operation_id = make_id(241);
}

static void wrong_origin(XrOwnershipAuditEvent *event, Fixture *fixture) {
    (void) fixture;
    event->allocation_site_id = make_id(242);
}

static void wrong_layout(XrOwnershipAuditEvent *event, Fixture *fixture) {
    (void) fixture;
    event->layout_id = make_id(247);
}

static void wrong_frame(XrOwnershipAuditEvent *event, Fixture *fixture) {
    (void) fixture;
    event->frame_id = make_id(248);
}

static void wrong_generation(XrOwnershipAuditEvent *event, Fixture *fixture) {
    (void) fixture;
    event->generation_id = make_id(249);
}

static void wrong_premise(XrOwnershipAuditEvent *event, Fixture *fixture) {
    (void) fixture;
    event->premise_fingerprint.bytes[0] ^= 1;
}

static void wrong_domain_contract(XrOwnershipAuditEvent *event, Fixture *fixture) {
    (void) fixture;
    event->domain.contract_id = make_id(243);
}

static void run_alloc_mutation(MutationFn mutate, XrOwnershipAuditStatus expected,
                               const char *message) {
    Fixture fixture = make_fixture();
    XrOwnershipAudit *audit = new_audit(8);
    register_manifests(audit, &fixture);
    CHECK(record(audit, pin_event(ID_PIN, XR_OWN_EVENT_PIN)) == XR_OWN_AUDIT_OK,
          "negative fixture pins generation");
    XrOwnershipAuditEvent event = owner_event(&fixture, object_key(70, 1), ID_ALLOC,
                                              XR_OWN_EVENT_ALLOC, fixture.local_domain);
    mutate(&event, &fixture);
    CHECK(record(audit, event) == expected, message);
    CHECK(xr_ownership_audit_status(audit) == expected,
          "first audit error remains sticky");
    CHECK(xr_ownership_audit_failed_event(audit) != NULL,
          "first failed observation is retained");
    xr_ownership_audit_destroy(audit);
}

static void test_seeded_identity_failures(void) {
    run_alloc_mutation(wrong_transition, XR_OWN_AUDIT_UNKNOWN_TRANSITION,
                       "unknown certificate transition is rejected");
    run_alloc_mutation(wrong_operation, XR_OWN_AUDIT_IDENTITY_MISMATCH,
                       "wrong operation identity is rejected");
    run_alloc_mutation(wrong_origin, XR_OWN_AUDIT_ORIGIN_MISMATCH,
                       "wrong allocation origin is rejected");
    run_alloc_mutation(wrong_layout, XR_OWN_AUDIT_IDENTITY_MISMATCH,
                       "wrong runtime layout identity is rejected");
    run_alloc_mutation(wrong_frame, XR_OWN_AUDIT_IDENTITY_MISMATCH,
                       "wrong frame identity is rejected");
    run_alloc_mutation(wrong_generation, XR_OWN_AUDIT_IDENTITY_MISMATCH,
                       "wrong generation identity is rejected");
    run_alloc_mutation(wrong_premise, XR_OWN_AUDIT_IDENTITY_MISMATCH,
                       "wrong premise fingerprint is rejected");
    run_alloc_mutation(wrong_domain_contract, XR_OWN_AUDIT_DOMAIN_MISMATCH,
                       "wrong initial domain contract is rejected");
}

static XrOwnershipAudit *open_one(Fixture *fixture, XrOwnershipAuditObjectKey *object,
                                  size_t event_capacity) {
    XrOwnershipAudit *audit = new_audit(event_capacity);
    register_manifests(audit, fixture);
    CHECK(record(audit, pin_event(ID_PIN, XR_OWN_EVENT_PIN)) == XR_OWN_AUDIT_OK,
          "fixture pin succeeds");
    *object = object_key(80, 1);
    CHECK(record(audit, owner_event(fixture, *object, ID_ALLOC, XR_OWN_EVENT_ALLOC,
                                    fixture->local_domain)) == XR_OWN_AUDIT_OK,
          "fixture allocation succeeds");
    return audit;
}

static void test_seeded_lifecycle_failures(void) {
    Fixture fixture = make_fixture();
    XrOwnershipAuditObjectKey object;
    XrOwnershipAudit *audit = open_one(&fixture, &object, 16);
    XrOwnershipAuditEvent end = owner_event(&fixture, object, ID_END_BORROW,
                                            XR_OWN_EVENT_END_BORROW,
                                            fixture.local_domain);
    CHECK(record(audit, end) == XR_OWN_AUDIT_INVALID_TRANSITION,
          "state-before mask catches an end-borrow without borrow");
    xr_ownership_audit_destroy(audit);

    audit = open_one(&fixture, &object, 16);
    CHECK(record(audit, owner_event(&fixture, object, ID_ALLOC, XR_OWN_EVENT_ALLOC,
                                    fixture.local_domain)) ==
              XR_OWN_AUDIT_DUPLICATE_INSTANCE,
          "duplicate dynamic activation key is rejected");
    xr_ownership_audit_destroy(audit);

    audit = open_one(&fixture, &object, 16);
    XrOwnershipAuditEvent detach = owner_event(&fixture, object, ID_DETACH,
                                               XR_OWN_EVENT_DETACH,
                                               fixture.local_domain);
    detach.next_domain.instance_id++;
    CHECK(record(audit, detach) == XR_OWN_AUDIT_OK,
          "manifest permits a concrete next-domain instance chosen at runtime");
    XrOwnershipAuditEvent retain = owner_event(&fixture, object, ID_RETAIN,
                                               XR_OWN_EVENT_RETAIN,
                                               fixture.shared_domain);
    CHECK(record(audit, retain) == XR_OWN_AUDIT_DOMAIN_MISMATCH,
          "later events must use the exact chosen domain instance");
    xr_ownership_audit_destroy(audit);

    audit = open_one(&fixture, &object, 16);
    detach = owner_event(&fixture, object, ID_DETACH, XR_OWN_EVENT_DETACH,
                         fixture.local_domain);
    detach.next_domain.contract_id = make_id(244);
    CHECK(record(audit, detach) == XR_OWN_AUDIT_DOMAIN_MISMATCH,
          "wrong next-domain contract is rejected");
    xr_ownership_audit_destroy(audit);

    audit = open_one(&fixture, &object, 16);
    XrOwnershipAuditEvent retain_bad = owner_event(&fixture, object, ID_RETAIN,
                                                   XR_OWN_EVENT_RETAIN,
                                                   fixture.local_domain);
    retain_bad.flags = XR_OWN_AUDIT_EVENT_PHYSICAL_RC;
    retain_bad.physical_rc_before = 1;
    retain_bad.physical_rc_after = 3;
    retain_bad.physical_rc_mode = XR_OWN_AUDIT_RC_LOCAL;
    CHECK(record(audit, retain_bad) == XR_OWN_AUDIT_PHYSICAL_RC_MISMATCH,
          "seeded physical RC transition mismatch is rejected");
    xr_ownership_audit_destroy(audit);

    audit = open_one(&fixture, &object, 16);
    XrOwnershipAuditEvent sticky_spoof = owner_event(
        &fixture, object, ID_RETAIN, XR_OWN_EVENT_RETAIN, fixture.local_domain);
    sticky_spoof.flags = XR_OWN_AUDIT_EVENT_PHYSICAL_RC;
    sticky_spoof.physical_rc_before = 7;
    sticky_spoof.physical_rc_after = 7;
    sticky_spoof.physical_rc_mode = XR_OWN_AUDIT_RC_STICKY;
    CHECK(record(audit, sticky_spoof) == XR_OWN_AUDIT_PHYSICAL_RC_MISMATCH,
          "event cannot self-report sticky mode against a shared-RC manifest");
    xr_ownership_audit_destroy(audit);

    audit = new_audit(16);
    register_manifests(audit, &fixture);
    XrOwnershipAuditTransitionManifest local_release = transition(
        ID_RELEASE_LOCAL_MODE, XR_OWN_EVENT_RELEASE, 0, -1,
        XR_OWN_STATE_MASK(XR_OWN_OWNED_LOCAL), XR_OWN_OWNED_LOCAL);
    local_release.physical_rc_mode = XR_OWN_AUDIT_RC_LOCAL;
    register_transition(audit, local_release);
    CHECK(record(audit, pin_event(ID_PIN, XR_OWN_EVENT_PIN)) == XR_OWN_AUDIT_OK,
          "physical-mode continuity fixture pins generation");
    object = object_key(82, 1);
    CHECK(record(audit, owner_event(&fixture, object, ID_ALLOC, XR_OWN_EVENT_ALLOC,
                                    fixture.local_domain)) == XR_OWN_AUDIT_OK,
          "physical-mode continuity fixture allocates");
    CHECK(record(audit, owner_event(&fixture, object, ID_DETACH, XR_OWN_EVENT_DETACH,
                                    fixture.local_domain)) == XR_OWN_AUDIT_OK,
          "physical-mode continuity fixture enters shared domain");
    XrOwnershipAuditEvent shared_retain = owner_event(
        &fixture, object, ID_RETAIN, XR_OWN_EVENT_RETAIN, fixture.shared_domain);
    shared_retain.flags = XR_OWN_AUDIT_EVENT_PHYSICAL_RC;
    shared_retain.physical_rc_before = 1;
    shared_retain.physical_rc_after = 2;
    shared_retain.physical_rc_mode = XR_OWN_AUDIT_RC_SHARED;
    CHECK(record(audit, shared_retain) == XR_OWN_AUDIT_OK,
          "first physical observation binds shared mode");
    XrOwnershipAuditEvent mode_change = owner_event(
        &fixture, object, ID_RELEASE_LOCAL_MODE, XR_OWN_EVENT_RELEASE,
        fixture.shared_domain);
    mode_change.flags = XR_OWN_AUDIT_EVENT_PHYSICAL_RC;
    mode_change.physical_rc_before = 2;
    mode_change.physical_rc_after = 1;
    mode_change.physical_rc_mode = XR_OWN_AUDIT_RC_LOCAL;
    CHECK(record(audit, mode_change) == XR_OWN_AUDIT_PHYSICAL_RC_MISMATCH,
          "matching manifests cannot change physical mode within one object");
    xr_ownership_audit_destroy(audit);

    audit = open_one(&fixture, &object, 8);
    XrOwnershipAuditEvent nonzero_release = owner_event(
        &fixture, object, ID_RELEASE_ZERO, XR_OWN_EVENT_RELEASE,
        fixture.local_domain);
    nonzero_release.flags = XR_OWN_AUDIT_EVENT_PHYSICAL_RC;
    nonzero_release.physical_rc_before = 2;
    nonzero_release.physical_rc_after = 1;
    nonzero_release.physical_rc_mode = XR_OWN_AUDIT_RC_SHARED;
    CHECK(record(audit, nonzero_release) == XR_OWN_AUDIT_OK,
          "logical terminal state may precede a nonzero observed physical count");
    CHECK(record(audit, owner_event(&fixture, object, ID_DESTROY,
                                    XR_OWN_EVENT_DESTROY, fixture.local_domain)) ==
              XR_OWN_AUDIT_PHYSICAL_RC_MISMATCH,
          "destroy rejects a nonsticky nonzero physical count");
    xr_ownership_audit_destroy(audit);

    audit = new_audit(8);
    register_manifests(audit, &fixture);
    CHECK(record(audit, owner_event(&fixture, object_key(81, 1), ID_ALLOC,
                                    XR_OWN_EVENT_ALLOC, fixture.local_domain)) ==
              XR_OWN_AUDIT_GENERATION_PIN_MISSING,
          "missing generation pin is rejected");
    xr_ownership_audit_destroy(audit);

    audit = new_audit(8);
    register_manifests(audit, &fixture);
    XrOwnershipAuditEvent invalid = pin_event(ID_PIN, XR_OWN_EVENT_PIN);
    memset(&invalid.operation_id, 0, sizeof(invalid.operation_id));
    CHECK(record(audit, invalid) == XR_OWN_AUDIT_INVALID_ARGUMENT,
          "malformed event poisons the audit");
    CHECK(xr_ownership_audit_finish(audit) == XR_OWN_AUDIT_INVALID_ARGUMENT,
          "malformed event status remains sticky through finish");
    xr_ownership_audit_destroy(audit);
}

static void test_exit_destructor_capacity_and_oom(void) {
    Fixture fixture = make_fixture();
    XrOwnershipAudit *audit = new_audit(16);
    register_manifests(audit, &fixture);
    CHECK(record(audit, pin_event(ID_PIN, XR_OWN_EVENT_PIN)) == XR_OWN_AUDIT_OK,
          "exit fixture pin succeeds");
    XrOwnershipAuditObjectKey object = object_key(89, 1);
    XrRuntimeDomainIdentity domain = advance_to_destroy(audit, &fixture, object);
    XrOwnershipAuditEvent destroy = owner_event(&fixture, object, ID_DESTROY,
                                                XR_OWN_EVENT_DESTROY, domain);
    destroy.exit_id = make_id(245);
    CHECK(record(audit, destroy) == XR_OWN_AUDIT_EXIT_MISMATCH,
          "wrong terminal exit identity is rejected");
    xr_ownership_audit_destroy(audit);

    audit = new_audit(16);
    register_manifests(audit, &fixture);
    CHECK(record(audit, pin_event(ID_PIN, XR_OWN_EVENT_PIN)) == XR_OWN_AUDIT_OK,
          "destructor fixture pin succeeds");
    object = object_key(89, 2);
    domain = advance_to_destroy(audit, &fixture, object);
    destroy = owner_event(&fixture, object, ID_DESTROY, XR_OWN_EVENT_DESTROY, domain);
    destroy.destructor_id = make_id(246);
    CHECK(record(audit, destroy) == XR_OWN_AUDIT_DESTRUCTOR_MISMATCH,
          "wrong destructor identity is rejected");
    xr_ownership_audit_destroy(audit);

    audit = new_audit(2);
    register_manifests(audit, &fixture);
    CHECK(record(audit, pin_event(ID_PIN, XR_OWN_EVENT_PIN)) == XR_OWN_AUDIT_OK,
          "capacity fixture pin succeeds");
    object = object_key(90, 1);
    CHECK(record(audit, owner_event(&fixture, object, ID_ALLOC, XR_OWN_EVENT_ALLOC,
                                    fixture.local_domain)) == XR_OWN_AUDIT_OK,
          "capacity fixture alloc succeeds");
    CHECK(record(audit, owner_event(&fixture, object, ID_BORROW, XR_OWN_EVENT_BORROW,
                                    fixture.local_domain)) ==
              XR_OWN_AUDIT_CAPACITY_EXCEEDED,
          "event capacity exhaustion poisons without growing during recording");
    xr_ownership_audit_destroy(audit);

    XrOwnershipAuditConfig overflow = audit_config(8);
    overflow.max_owner_manifests = SIZE_MAX;
    XrOwnershipAuditStatus status = XR_OWN_AUDIT_OK;
    audit = xr_ownership_audit_create(overflow, &status);
    CHECK(audit == NULL && status == XR_OWN_AUDIT_OUT_OF_MEMORY,
          "table-size overflow deterministically exercises create OOM cleanup");

    audit = open_one(&fixture, &object, 8);
    CHECK(xr_ownership_audit_finish(audit) == XR_OWN_AUDIT_INCOMPLETE,
          "finish rejects a live dynamic instance");
    xr_ownership_audit_destroy(audit);
}

static void setup_lifecycle_audit(XrOwnershipAudit *audit, const Fixture *fixture) {
    register_manifests(audit, fixture);
    register_lifecycle_manifests(audit, fixture);
}

static void test_finalize_reclaim_with_and_without_teardown(void) {
    Fixture fixture = make_lifecycle_fixture();
    XrOwnershipAudit *audit = new_audit(32);
    setup_lifecycle_audit(audit, &fixture);
    size_t allocation_count = xr_ownership_audit_allocation_count(audit);
    CHECK(record(audit, pin_event(ID_PIN, XR_OWN_EVENT_PIN)) == XR_OWN_AUDIT_OK,
          "tracked lifecycle pins its code generation");
    XrOwnershipAuditObjectKey object = object_key(100, 1);
    XrRuntimeDomainIdentity domain = advance_to_destroy(audit, &fixture, object);
    CHECK(xr_ownership_audit_allocation_state(audit, object) ==
              XR_OWN_AUDIT_ALLOCATION_LIVE,
          "tracked allocation starts live");
    CHECK(record_lifecycle(
              audit, lifecycle_event((XrOwnershipAuditObjectKey) {0}, ID_BEGIN_TEARDOWN,
                                     XR_OWN_AUDIT_LIFECYCLE_BEGIN_TEARDOWN, domain)) ==
              XR_OWN_AUDIT_OK,
          "exact shared domain begins draining");
    CHECK(record_lifecycle(audit, lifecycle_event(object, ID_BEGIN_FINALIZE,
                                                  XR_OWN_AUDIT_LIFECYCLE_BEGIN_FINALIZE,
                                                  domain)) == XR_OWN_AUDIT_OK &&
              xr_ownership_audit_allocation_state(audit, object) ==
                  XR_OWN_AUDIT_ALLOCATION_FINALIZING,
          "released object enters finalizing state");
    CHECK(record(audit, owner_event(&fixture, object, ID_DESTROY, XR_OWN_EVENT_DESTROY,
                                    domain)) == XR_OWN_AUDIT_OK,
          "manifest destructor executes only while finalizing");
    CHECK(record_lifecycle(audit, lifecycle_event(object, ID_END_FINALIZE,
                                                  XR_OWN_AUDIT_LIFECYCLE_END_FINALIZE,
                                                  domain)) == XR_OWN_AUDIT_OK &&
              xr_ownership_audit_allocation_state(audit, object) ==
                  XR_OWN_AUDIT_ALLOCATION_FINALIZED,
          "finalize end requires the observed destructor terminal");
    CHECK(record_lifecycle(audit, lifecycle_event(object, ID_RECLAIM,
                                                  XR_OWN_AUDIT_LIFECYCLE_RECLAIM,
                                                  domain)) == XR_OWN_AUDIT_OK &&
              xr_ownership_audit_allocation_state(audit, object) ==
                  XR_OWN_AUDIT_ALLOCATION_RECLAIMED,
          "physical reclaim follows finalization exactly once");
    CHECK(record_lifecycle(
              audit, lifecycle_event((XrOwnershipAuditObjectKey) {0}, ID_END_TEARDOWN,
                                     XR_OWN_AUDIT_LIFECYCLE_END_TEARDOWN, domain)) ==
              XR_OWN_AUDIT_OK,
          "domain teardown closes after every allocation is reclaimed");
    CHECK(record(audit, pin_event(ID_UNPIN, XR_OWN_EVENT_UNPIN)) == XR_OWN_AUDIT_OK,
          "generation drain remains independent from domain teardown");
    CHECK(xr_ownership_audit_finish(audit) == XR_OWN_AUDIT_OK,
          "teardown lifecycle reaches a complete audit");
    CHECK(xr_ownership_audit_allocation_count(audit) == allocation_count,
          "ownership and lifecycle record paths perform zero allocations");
    CHECK(xr_ownership_audit_lifecycle_event_count(audit) == 5 &&
              xr_ownership_audit_lifecycle_event(audit, 4)->kind ==
                  XR_OWN_AUDIT_LIFECYCLE_END_TEARDOWN,
          "bounded lifecycle history preserves the verified sequence");
    CHECK(xr_ownership_audit_allocation_state(audit, object_key(101, 99)) ==
              XR_OWN_AUDIT_ALLOCATION_UNKNOWN,
          "unknown allocation keys are distinct from untracked allocations");
    xr_ownership_audit_destroy(audit);

    audit = new_audit(32);
    setup_lifecycle_audit(audit, &fixture);
    CHECK(record(audit, pin_event(ID_PIN, XR_OWN_EVENT_PIN)) == XR_OWN_AUDIT_OK,
          "last-release fixture pins generation");
    object = object_key(101, 1);
    domain = advance_to_destroy(audit, &fixture, object);
    CHECK(record_lifecycle(audit, lifecycle_event(object, ID_BEGIN_FINALIZE,
                                                  XR_OWN_AUDIT_LIFECYCLE_BEGIN_FINALIZE,
                                                  domain)) == XR_OWN_AUDIT_OK,
          "last release can finalize without domain teardown");
    CHECK(record(audit, owner_event(&fixture, object, ID_DESTROY, XR_OWN_EVENT_DESTROY,
                                    domain)) == XR_OWN_AUDIT_OK,
          "last-release destructor verifies");
    CHECK(record_lifecycle(audit, lifecycle_event(object, ID_END_FINALIZE,
                                                  XR_OWN_AUDIT_LIFECYCLE_END_FINALIZE,
                                                  domain)) == XR_OWN_AUDIT_OK,
          "last-release finalization closes");
    CHECK(record_lifecycle(audit, lifecycle_event(object, ID_RECLAIM,
                                                  XR_OWN_AUDIT_LIFECYCLE_RECLAIM,
                                                  domain)) == XR_OWN_AUDIT_OK,
          "last-release allocation reclaims without a teardown record");
    CHECK(record(audit, pin_event(ID_UNPIN, XR_OWN_EVENT_UNPIN)) == XR_OWN_AUDIT_OK &&
              xr_ownership_audit_finish(audit) == XR_OWN_AUDIT_OK,
          "non-teardown lifecycle completes independently");
    xr_ownership_audit_destroy(audit);
}

static XrOwnershipAudit *prepare_released_lifecycle(
    Fixture *fixture, XrOwnershipAuditObjectKey *object, XrRuntimeDomainIdentity *domain,
    bool begin_domain_teardown) {
    *fixture = make_lifecycle_fixture();
    XrOwnershipAudit *audit = new_audit(32);
    setup_lifecycle_audit(audit, fixture);
    CHECK(record(audit, pin_event(ID_PIN, XR_OWN_EVENT_PIN)) == XR_OWN_AUDIT_OK,
          "adversarial lifecycle fixture pins generation");
    *object = object_key(110, 1);
    *domain = advance_to_destroy(audit, fixture, *object);
    if (begin_domain_teardown)
        CHECK(record_lifecycle(
                  audit, lifecycle_event((XrOwnershipAuditObjectKey) {0},
                                         ID_BEGIN_TEARDOWN,
                                         XR_OWN_AUDIT_LIFECYCLE_BEGIN_TEARDOWN,
                                         *domain)) == XR_OWN_AUDIT_OK,
              "adversarial lifecycle fixture begins teardown");
    return audit;
}

static void test_lifecycle_adversarial_ordering(void) {
    Fixture fixture;
    XrOwnershipAuditObjectKey object;
    XrRuntimeDomainIdentity domain;
    XrOwnershipAudit *audit =
        prepare_released_lifecycle(&fixture, &object, &domain, true);
    CHECK(record_lifecycle(audit, lifecycle_event(object, ID_RECLAIM,
                                                  XR_OWN_AUDIT_LIFECYCLE_RECLAIM,
                                                  domain)) ==
              XR_OWN_AUDIT_RECLAIM_MISMATCH,
          "reclaim before finalization is rejected");
    CHECK(xr_ownership_audit_failed_lifecycle_event(audit) != NULL,
          "first failed lifecycle observation is retained");
    xr_ownership_audit_destroy(audit);

    audit = prepare_released_lifecycle(&fixture, &object, &domain, true);
    CHECK(record_lifecycle(audit, lifecycle_event(object, ID_BEGIN_FINALIZE,
                                                  XR_OWN_AUDIT_LIFECYCLE_BEGIN_FINALIZE,
                                                  domain)) == XR_OWN_AUDIT_OK,
          "ordering fixture enters finalization");
    CHECK(record_lifecycle(audit, lifecycle_event(object, ID_END_FINALIZE,
                                                  XR_OWN_AUDIT_LIFECYCLE_END_FINALIZE,
                                                  domain)) ==
              XR_OWN_AUDIT_FINALIZE_MISMATCH,
          "finalize cannot end before destructor terminal");
    xr_ownership_audit_destroy(audit);

    audit = prepare_released_lifecycle(&fixture, &object, &domain, false);
    XrOwnershipAuditLifecycleEvent bad = lifecycle_event(
        object, ID_BEGIN_FINALIZE, XR_OWN_AUDIT_LIFECYCLE_BEGIN_FINALIZE, domain);
    bad.destructor_id = make_id(230);
    CHECK(record_lifecycle(audit, bad) == XR_OWN_AUDIT_DESTRUCTOR_MISMATCH,
          "wrong lifecycle destructor identity is rejected");
    xr_ownership_audit_destroy(audit);

    audit = prepare_released_lifecycle(&fixture, &object, &domain, true);
    CHECK(record_lifecycle(
              audit, lifecycle_event((XrOwnershipAuditObjectKey) {0}, ID_BEGIN_TEARDOWN,
                                     XR_OWN_AUDIT_LIFECYCLE_BEGIN_TEARDOWN, domain)) ==
              XR_OWN_AUDIT_TEARDOWN_MISMATCH,
          "duplicate exact-domain teardown is rejected");
    xr_ownership_audit_destroy(audit);

    audit = prepare_released_lifecycle(&fixture, &object, &domain, false);
    bad = lifecycle_event(object, ID_BEGIN_FINALIZE,
                          XR_OWN_AUDIT_LIFECYCLE_BEGIN_FINALIZE, domain);
    bad.operation_id = make_id(231);
    CHECK(record_lifecycle(audit, bad) == XR_OWN_AUDIT_IDENTITY_MISMATCH,
          "wrong lifecycle operation identity is rejected");
    xr_ownership_audit_destroy(audit);

    audit = prepare_released_lifecycle(&fixture, &object, &domain, false);
    bad = lifecycle_event(object, 232, XR_OWN_AUDIT_LIFECYCLE_BEGIN_FINALIZE, domain);
    CHECK(record_lifecycle(audit, bad) == XR_OWN_AUDIT_UNKNOWN_TRANSITION,
          "unknown lifecycle transition is rejected");
    xr_ownership_audit_destroy(audit);

    audit = prepare_released_lifecycle(&fixture, &object, &domain, false);
    CHECK(record(audit, owner_event(&fixture, object, ID_DESTROY, XR_OWN_EVENT_DESTROY,
                                    domain)) == XR_OWN_AUDIT_FINALIZE_MISMATCH,
          "tracked terminal cannot bypass begin-finalize");
    xr_ownership_audit_destroy(audit);

    audit = prepare_released_lifecycle(&fixture, &object, &domain, false);
    CHECK(record_lifecycle(audit, lifecycle_event(object, ID_BEGIN_FINALIZE,
                                                  XR_OWN_AUDIT_LIFECYCLE_BEGIN_FINALIZE,
                                                  domain)) == XR_OWN_AUDIT_OK,
          "unpin fixture enters finalization");
    CHECK(record(audit, pin_event(ID_UNPIN, XR_OWN_EVENT_UNPIN)) ==
              XR_OWN_AUDIT_GENERATION_PIN_MISMATCH,
          "required generation cannot unpin during finalization");
    xr_ownership_audit_destroy(audit);
}

static void test_teardown_adversarial_boundaries(void) {
    Fixture fixture = make_lifecycle_fixture();
    XrOwnershipAudit *audit = new_audit(16);
    setup_lifecycle_audit(audit, &fixture);
    CHECK(record_lifecycle(
              audit, lifecycle_event((XrOwnershipAuditObjectKey) {0},
                                     ID_BEGIN_LOCAL_TEARDOWN,
                                     XR_OWN_AUDIT_LIFECYCLE_BEGIN_TEARDOWN,
                                     fixture.local_domain)) == XR_OWN_AUDIT_OK,
          "local domain starts draining before allocation");
    CHECK(record(audit, pin_event(ID_PIN, XR_OWN_EVENT_PIN)) == XR_OWN_AUDIT_OK,
          "unrelated generation pin may coexist with domain drain");
    CHECK(record(audit, owner_event(&fixture, object_key(120, 1), ID_ALLOC,
                                    XR_OWN_EVENT_ALLOC, fixture.local_domain)) ==
              XR_OWN_AUDIT_TEARDOWN_MISMATCH,
          "allocation into a draining domain is rejected");
    xr_ownership_audit_destroy(audit);

    fixture = make_lifecycle_fixture();
    audit = new_audit(16);
    setup_lifecycle_audit(audit, &fixture);
    CHECK(record(audit, pin_event(ID_PIN, XR_OWN_EVENT_PIN)) == XR_OWN_AUDIT_OK,
          "borrow boundary fixture pins generation");
    XrOwnershipAuditObjectKey object = object_key(121, 1);
    CHECK(record(audit, owner_event(&fixture, object, ID_ALLOC, XR_OWN_EVENT_ALLOC,
                                    fixture.local_domain)) == XR_OWN_AUDIT_OK,
          "borrow boundary fixture allocates before teardown");
    CHECK(record_lifecycle(
              audit, lifecycle_event((XrOwnershipAuditObjectKey) {0},
                                     ID_BEGIN_LOCAL_TEARDOWN,
                                     XR_OWN_AUDIT_LIFECYCLE_BEGIN_TEARDOWN,
                                     fixture.local_domain)) == XR_OWN_AUDIT_OK,
          "allocation domain begins draining");
    CHECK(record(audit, owner_event(&fixture, object, ID_BORROW, XR_OWN_EVENT_BORROW,
                                    fixture.local_domain)) ==
              XR_OWN_AUDIT_TEARDOWN_MISMATCH,
          "new borrow in a draining domain is rejected");
    xr_ownership_audit_destroy(audit);

    fixture = make_fixture();
    audit = new_audit(16);
    register_manifests(audit, &fixture);
    register_lifecycle_manifest(
        audit, domain_lifecycle_manifest(ID_BEGIN_LOCAL_TEARDOWN,
                                         XR_OWN_AUDIT_LIFECYCLE_BEGIN_TEARDOWN,
                                         fixture.local_domain.contract_id,
                                         fixture.local_domain.semantic_domain,
                                         fixture.local_domain.materialization));
    register_lifecycle_manifest(
        audit, domain_lifecycle_manifest(ID_END_LOCAL_TEARDOWN,
                                         XR_OWN_AUDIT_LIFECYCLE_END_TEARDOWN,
                                         fixture.local_domain.contract_id,
                                         fixture.local_domain.semantic_domain,
                                         fixture.local_domain.materialization));
    CHECK(record(audit, pin_event(ID_PIN, XR_OWN_EVENT_PIN)) == XR_OWN_AUDIT_OK,
          "untracked teardown fixture pins generation");
    object = object_key(122, 1);
    CHECK(record(audit, owner_event(&fixture, object, ID_ALLOC, XR_OWN_EVENT_ALLOC,
                                    fixture.local_domain)) == XR_OWN_AUDIT_OK,
          "untracked object allocates before teardown");
    CHECK(record_lifecycle(
              audit, lifecycle_event((XrOwnershipAuditObjectKey) {0},
                                     ID_BEGIN_LOCAL_TEARDOWN,
                                     XR_OWN_AUDIT_LIFECYCLE_BEGIN_TEARDOWN,
                                     fixture.local_domain)) == XR_OWN_AUDIT_OK,
          "untracked object domain begins draining");
    CHECK(record_lifecycle(
              audit, lifecycle_event((XrOwnershipAuditObjectKey) {0},
                                     ID_END_LOCAL_TEARDOWN,
                                     XR_OWN_AUDIT_LIFECYCLE_END_TEARDOWN,
                                     fixture.local_domain)) ==
              XR_OWN_AUDIT_TEARDOWN_MISMATCH,
          "domain teardown rejects an untracked live object");
    xr_ownership_audit_destroy(audit);

    fixture = make_fixture();
    audit = new_audit(16);
    register_manifests(audit, &fixture);
    register_lifecycle_manifest(
        audit, domain_lifecycle_manifest(ID_BEGIN_LOCAL_TEARDOWN,
                                         XR_OWN_AUDIT_LIFECYCLE_BEGIN_TEARDOWN,
                                         fixture.local_domain.contract_id,
                                         fixture.local_domain.semantic_domain,
                                         fixture.local_domain.materialization));
    register_lifecycle_manifest(
        audit, domain_lifecycle_manifest(ID_END_LOCAL_TEARDOWN,
                                         XR_OWN_AUDIT_LIFECYCLE_END_TEARDOWN,
                                         fixture.local_domain.contract_id,
                                         fixture.local_domain.semantic_domain,
                                         fixture.local_domain.materialization));
    CHECK(record(audit, pin_event(ID_PIN, XR_OWN_EVENT_PIN)) == XR_OWN_AUDIT_OK,
          "cancel cleanup fixture pins generation");
    object = object_key(124, 1);
    CHECK(record(audit, owner_event(&fixture, object, ID_ALLOC, XR_OWN_EVENT_ALLOC,
                                    fixture.local_domain)) == XR_OWN_AUDIT_OK,
          "cancel cleanup fixture allocates");
    CHECK(record_lifecycle(
              audit, lifecycle_event((XrOwnershipAuditObjectKey) {0},
                                     ID_BEGIN_LOCAL_TEARDOWN,
                                     XR_OWN_AUDIT_LIFECYCLE_BEGIN_TEARDOWN,
                                     fixture.local_domain)) == XR_OWN_AUDIT_OK,
          "cancel cleanup begins domain drain");
    CHECK(record(audit, owner_event(&fixture, object, ID_CANCEL, XR_OWN_EVENT_CANCEL,
                                    fixture.local_domain)) == XR_OWN_AUDIT_OK,
          "nonpositive cancel disposition can close during domain drain");
    CHECK(record_lifecycle(
              audit, lifecycle_event((XrOwnershipAuditObjectKey) {0},
                                     ID_END_LOCAL_TEARDOWN,
                                     XR_OWN_AUDIT_LIFECYCLE_END_TEARDOWN,
                                     fixture.local_domain)) == XR_OWN_AUDIT_OK,
          "cancelled untracked allocation permits teardown completion");
    CHECK(record(audit, pin_event(ID_UNPIN, XR_OWN_EVENT_UNPIN)) == XR_OWN_AUDIT_OK &&
              xr_ownership_audit_finish(audit) == XR_OWN_AUDIT_OK,
          "cancel cleanup leaves complete audit evidence");
    xr_ownership_audit_destroy(audit);

    fixture = make_lifecycle_fixture();
    audit = new_audit(16);
    setup_lifecycle_audit(audit, &fixture);
    CHECK(record_lifecycle(
              audit, lifecycle_event((XrOwnershipAuditObjectKey) {0},
                                     ID_BEGIN_LOCAL_TEARDOWN,
                                     XR_OWN_AUDIT_LIFECYCLE_BEGIN_TEARDOWN,
                                     fixture.local_domain)) == XR_OWN_AUDIT_OK &&
              record_lifecycle(
                  audit, lifecycle_event((XrOwnershipAuditObjectKey) {0},
                                         ID_END_LOCAL_TEARDOWN,
                                         XR_OWN_AUDIT_LIFECYCLE_END_TEARDOWN,
                                         fixture.local_domain)) == XR_OWN_AUDIT_OK,
          "empty exact domain can complete teardown");
    CHECK(record(audit, pin_event(ID_PIN, XR_OWN_EVENT_PIN)) == XR_OWN_AUDIT_OK,
          "generation pin after domain teardown remains independent");
    CHECK(record(audit, owner_event(&fixture, object_key(123, 1), ID_ALLOC,
                                    XR_OWN_EVENT_ALLOC, fixture.local_domain)) ==
              XR_OWN_AUDIT_TEARDOWN_MISMATCH,
          "ended domain cannot resurrect an allocation");
    xr_ownership_audit_destroy(audit);
}

static void test_finalize_requires_reclaim_at_finish(void) {
    Fixture fixture;
    XrOwnershipAuditObjectKey object;
    XrRuntimeDomainIdentity domain;
    XrOwnershipAudit *audit =
        prepare_released_lifecycle(&fixture, &object, &domain, false);
    CHECK(record_lifecycle(audit, lifecycle_event(object, ID_BEGIN_FINALIZE,
                                                  XR_OWN_AUDIT_LIFECYCLE_BEGIN_FINALIZE,
                                                  domain)) == XR_OWN_AUDIT_OK &&
              record(audit, owner_event(&fixture, object, ID_DESTROY,
                                        XR_OWN_EVENT_DESTROY, domain)) == XR_OWN_AUDIT_OK &&
              record_lifecycle(audit, lifecycle_event(
                                          object, ID_END_FINALIZE,
                                          XR_OWN_AUDIT_LIFECYCLE_END_FINALIZE,
                                          domain)) == XR_OWN_AUDIT_OK,
          "finish fixture reaches finalized but not reclaimed");
    CHECK(record(audit, pin_event(ID_UNPIN, XR_OWN_EVENT_UNPIN)) == XR_OWN_AUDIT_OK,
          "finish fixture closes generation pin");
    CHECK(xr_ownership_audit_finish(audit) == XR_OWN_AUDIT_INCOMPLETE,
          "finalized allocation remains incomplete until reclaim");
    xr_ownership_audit_destroy(audit);
}

static void register_local_teardown_manifests(XrOwnershipAudit *audit,
                                              const Fixture *fixture) {
    register_lifecycle_manifest(
        audit, domain_lifecycle_manifest(ID_BEGIN_LOCAL_TEARDOWN,
                                         XR_OWN_AUDIT_LIFECYCLE_BEGIN_TEARDOWN,
                                         fixture->local_domain.contract_id,
                                         fixture->local_domain.semantic_domain,
                                         fixture->local_domain.materialization));
    register_lifecycle_manifest(
        audit, domain_lifecycle_manifest(ID_END_LOCAL_TEARDOWN,
                                         XR_OWN_AUDIT_LIFECYCLE_END_TEARDOWN,
                                         fixture->local_domain.contract_id,
                                         fixture->local_domain.semantic_domain,
                                         fixture->local_domain.materialization));
}

static void test_exact_domain_drain_cleanup_matrix(void) {
    Fixture fixture = make_fixture();
    XrOwnershipAudit *audit = new_audit(16);
    register_manifests(audit, &fixture);
    register_local_teardown_manifests(audit, &fixture);
    CHECK(record_lifecycle(
              audit, lifecycle_event((XrOwnershipAuditObjectKey) {0},
                                     ID_BEGIN_LOCAL_TEARDOWN,
                                     XR_OWN_AUDIT_LIFECYCLE_BEGIN_TEARDOWN,
                                     fixture.local_domain)) == XR_OWN_AUDIT_OK,
          "one exact local-domain instance begins draining");
    CHECK(record(audit, pin_event(ID_PIN, XR_OWN_EVENT_PIN)) == XR_OWN_AUDIT_OK,
          "neighbor-domain fixture pins generation");
    XrRuntimeDomainIdentity neighbor = fixture.local_domain;
    neighbor.instance_id++;
    XrOwnershipAuditObjectKey object = object_key(125, 1);
    CHECK(record(audit, owner_event(&fixture, object, ID_ALLOC, XR_OWN_EVENT_ALLOC,
                                    neighbor)) == XR_OWN_AUDIT_OK,
          "same contract with a different instance remains active");
    CHECK(record(audit, owner_event(&fixture, object, ID_CANCEL, XR_OWN_EVENT_CANCEL,
                                    neighbor)) == XR_OWN_AUDIT_OK,
          "neighbor instance closes independently");
    CHECK(record_lifecycle(
              audit, lifecycle_event((XrOwnershipAuditObjectKey) {0},
                                     ID_END_LOCAL_TEARDOWN,
                                     XR_OWN_AUDIT_LIFECYCLE_END_TEARDOWN,
                                     fixture.local_domain)) == XR_OWN_AUDIT_OK,
          "drained exact instance closes without capturing its neighbor");
    CHECK(record(audit, pin_event(ID_UNPIN, XR_OWN_EVENT_UNPIN)) == XR_OWN_AUDIT_OK &&
              xr_ownership_audit_finish(audit) == XR_OWN_AUDIT_OK,
          "exact-instance teardown evidence completes");
    xr_ownership_audit_destroy(audit);

    fixture = make_fixture();
    audit = new_audit(16);
    register_manifests(audit, &fixture);
    register_local_teardown_manifests(audit, &fixture);
    CHECK(record(audit, pin_event(ID_PIN, XR_OWN_EVENT_PIN)) == XR_OWN_AUDIT_OK,
          "borrow-drain fixture pins generation");
    object = object_key(126, 1);
    CHECK(record(audit, owner_event(&fixture, object, ID_ALLOC, XR_OWN_EVENT_ALLOC,
                                    fixture.local_domain)) == XR_OWN_AUDIT_OK &&
              record(audit, owner_event(&fixture, object, ID_BORROW,
                                        XR_OWN_EVENT_BORROW,
                                        fixture.local_domain)) == XR_OWN_AUDIT_OK,
          "borrow is active before drain begins");
    CHECK(record_lifecycle(
              audit, lifecycle_event((XrOwnershipAuditObjectKey) {0},
                                     ID_BEGIN_LOCAL_TEARDOWN,
                                     XR_OWN_AUDIT_LIFECYCLE_BEGIN_TEARDOWN,
                                     fixture.local_domain)) == XR_OWN_AUDIT_OK,
          "borrowed-object domain starts draining");
    CHECK(record(audit, owner_event(&fixture, object, ID_END_BORROW,
                                    XR_OWN_EVENT_END_BORROW,
                                    fixture.local_domain)) == XR_OWN_AUDIT_OK,
          "active borrow may close during drain");
    CHECK(record(audit, owner_event(&fixture, object, ID_CANCEL, XR_OWN_EVENT_CANCEL,
                                    fixture.local_domain)) == XR_OWN_AUDIT_OK,
          "borrowed object cancels after loan closure");
    CHECK(record_lifecycle(
              audit, lifecycle_event((XrOwnershipAuditObjectKey) {0},
                                     ID_END_LOCAL_TEARDOWN,
                                     XR_OWN_AUDIT_LIFECYCLE_END_TEARDOWN,
                                     fixture.local_domain)) == XR_OWN_AUDIT_OK,
          "loan-free cancelled object permits teardown end");
    CHECK(record(audit, pin_event(ID_UNPIN, XR_OWN_EVENT_UNPIN)) == XR_OWN_AUDIT_OK &&
              xr_ownership_audit_finish(audit) == XR_OWN_AUDIT_OK,
          "borrow drain cleanup completes");
    xr_ownership_audit_destroy(audit);

    fixture = make_fixture();
    audit = new_audit(16);
    register_manifests(audit, &fixture);
    register_local_teardown_manifests(audit, &fixture);
    CHECK(record(audit, pin_event(ID_PIN, XR_OWN_EVENT_PIN)) == XR_OWN_AUDIT_OK,
          "positive-delta drain fixture pins generation");
    object = object_key(127, 1);
    CHECK(record(audit, owner_event(&fixture, object, ID_ALLOC, XR_OWN_EVENT_ALLOC,
                                    fixture.local_domain)) == XR_OWN_AUDIT_OK,
          "positive-delta fixture allocates before drain");
    CHECK(record_lifecycle(
              audit, lifecycle_event((XrOwnershipAuditObjectKey) {0},
                                     ID_BEGIN_LOCAL_TEARDOWN,
                                     XR_OWN_AUDIT_LIFECYCLE_BEGIN_TEARDOWN,
                                     fixture.local_domain)) == XR_OWN_AUDIT_OK,
          "positive-delta fixture begins drain");
    XrOwnershipAuditEvent retain = owner_event(
        &fixture, object, ID_RETAIN, XR_OWN_EVENT_RETAIN, fixture.local_domain);
    retain.flags = XR_OWN_AUDIT_EVENT_PHYSICAL_RC;
    retain.physical_rc_before = 1;
    retain.physical_rc_after = 2;
    retain.physical_rc_mode = XR_OWN_AUDIT_RC_SHARED;
    CHECK(record(audit, retain) == XR_OWN_AUDIT_TEARDOWN_MISMATCH,
          "positive logical delta is rejected during drain");
    xr_ownership_audit_destroy(audit);

    fixture = make_fixture();
    audit = new_audit(16);
    register_manifests(audit, &fixture);
    register_local_teardown_manifests(audit, &fixture);
    CHECK(record(audit, pin_event(ID_PIN, XR_OWN_EVENT_PIN)) == XR_OWN_AUDIT_OK,
          "domain-change drain fixture pins generation");
    object = object_key(128, 1);
    CHECK(record(audit, owner_event(&fixture, object, ID_ALLOC, XR_OWN_EVENT_ALLOC,
                                    fixture.local_domain)) == XR_OWN_AUDIT_OK,
          "domain-change fixture allocates before drain");
    CHECK(record_lifecycle(
              audit, lifecycle_event((XrOwnershipAuditObjectKey) {0},
                                     ID_BEGIN_LOCAL_TEARDOWN,
                                     XR_OWN_AUDIT_LIFECYCLE_BEGIN_TEARDOWN,
                                     fixture.local_domain)) == XR_OWN_AUDIT_OK,
          "domain-change fixture begins drain");
    CHECK(record(audit, owner_event(&fixture, object, ID_DETACH,
                                    XR_OWN_EVENT_DETACH,
                                    fixture.local_domain)) ==
              XR_OWN_AUDIT_TEARDOWN_MISMATCH,
          "domain transfer is rejected during drain");
    xr_ownership_audit_destroy(audit);
}

static void test_lifecycle_additional_mutations_and_capacity(void) {
    Fixture fixture;
    XrOwnershipAuditObjectKey object;
    XrRuntimeDomainIdentity domain;
    XrOwnershipAudit *audit =
        prepare_released_lifecycle(&fixture, &object, &domain, false);
    XrOwnershipAuditLifecycleEvent mutated = lifecycle_event(
        object, ID_BEGIN_FINALIZE, XR_OWN_AUDIT_LIFECYCLE_RECLAIM, domain);
    CHECK(record_lifecycle(audit, mutated) == XR_OWN_AUDIT_IDENTITY_MISMATCH,
          "lifecycle kind must match its static manifest");
    xr_ownership_audit_destroy(audit);

    audit = prepare_released_lifecycle(&fixture, &object, &domain, false);
    mutated = lifecycle_event(object_key(111, 9), ID_BEGIN_FINALIZE,
                              XR_OWN_AUDIT_LIFECYCLE_BEGIN_FINALIZE, domain);
    CHECK(record_lifecycle(audit, mutated) == XR_OWN_AUDIT_UNKNOWN_OWNER,
          "unknown dynamic lifecycle object is rejected");
    xr_ownership_audit_destroy(audit);

    audit = prepare_released_lifecycle(&fixture, &object, &domain, false);
    mutated = lifecycle_event(object, ID_BEGIN_FINALIZE,
                              XR_OWN_AUDIT_LIFECYCLE_BEGIN_FINALIZE, domain);
    mutated.domain.instance_id++;
    CHECK(record_lifecycle(audit, mutated) == XR_OWN_AUDIT_DOMAIN_MISMATCH,
          "lifecycle event binds exact domain instance");
    xr_ownership_audit_destroy(audit);

    audit = prepare_released_lifecycle(&fixture, &object, &domain, false);
    CHECK(record_lifecycle(audit, lifecycle_event(object, ID_BEGIN_FINALIZE,
                                                  XR_OWN_AUDIT_LIFECYCLE_BEGIN_FINALIZE,
                                                  domain)) == XR_OWN_AUDIT_OK,
          "duplicate-finalize fixture enters finalizing");
    CHECK(record_lifecycle(audit, lifecycle_event(object, ID_BEGIN_FINALIZE,
                                                  XR_OWN_AUDIT_LIFECYCLE_BEGIN_FINALIZE,
                                                  domain)) ==
              XR_OWN_AUDIT_FINALIZE_MISMATCH,
          "duplicate begin-finalize is rejected");
    xr_ownership_audit_destroy(audit);

    fixture = make_lifecycle_fixture();
    XrOwnershipAuditConfig config = audit_config(32);
    config.max_lifecycle_events = 1;
    XrOwnershipAuditStatus create_status;
    audit = xr_ownership_audit_create(config, &create_status);
    CHECK(audit != NULL && create_status == XR_OWN_AUDIT_OK,
          "lifecycle-capacity audit preallocates");
    setup_lifecycle_audit(audit, &fixture);
    CHECK(record(audit, pin_event(ID_PIN, XR_OWN_EVENT_PIN)) == XR_OWN_AUDIT_OK,
          "lifecycle-capacity fixture pins generation");
    object = object_key(112, 1);
    domain = advance_to_destroy(audit, &fixture, object);
    CHECK(record_lifecycle(audit, lifecycle_event(object, ID_BEGIN_FINALIZE,
                                                  XR_OWN_AUDIT_LIFECYCLE_BEGIN_FINALIZE,
                                                  domain)) == XR_OWN_AUDIT_OK,
          "first lifecycle event fills bounded history");
    CHECK(record(audit, owner_event(&fixture, object, ID_DESTROY, XR_OWN_EVENT_DESTROY,
                                    domain)) == XR_OWN_AUDIT_OK,
          "destructor can run after lifecycle history fills");
    CHECK(record_lifecycle(audit, lifecycle_event(object, ID_END_FINALIZE,
                                                  XR_OWN_AUDIT_LIFECYCLE_END_FINALIZE,
                                                  domain)) ==
              XR_OWN_AUDIT_CAPACITY_EXCEEDED,
          "lifecycle history capacity fails closed without growth");
    xr_ownership_audit_destroy(audit);

    config = audit_config(8);
    config.max_teardown_domains = 1;
    audit = xr_ownership_audit_create(config, &create_status);
    CHECK(audit != NULL && create_status == XR_OWN_AUDIT_OK,
          "teardown-capacity audit preallocates");
    setup_lifecycle_audit(audit, &fixture);
    CHECK(record_lifecycle(
              audit, lifecycle_event((XrOwnershipAuditObjectKey) {0},
                                     ID_BEGIN_LOCAL_TEARDOWN,
                                     XR_OWN_AUDIT_LIFECYCLE_BEGIN_TEARDOWN,
                                     fixture.local_domain)) == XR_OWN_AUDIT_OK,
          "first exact domain fills teardown table");
    CHECK(record_lifecycle(
              audit, lifecycle_event((XrOwnershipAuditObjectKey) {0}, ID_BEGIN_TEARDOWN,
                                     XR_OWN_AUDIT_LIFECYCLE_BEGIN_TEARDOWN,
                                     fixture.shared_domain)) ==
              XR_OWN_AUDIT_CAPACITY_EXCEEDED,
          "teardown-domain capacity fails closed");
    xr_ownership_audit_destroy(audit);

    config = audit_config(8);
    config.max_lifecycle_manifests = 1;
    audit = xr_ownership_audit_create(config, &create_status);
    CHECK(audit != NULL && create_status == XR_OWN_AUDIT_OK,
          "manifest-capacity audit preallocates");
    register_manifests(audit, &fixture);
    register_lifecycle_manifest(
        audit, domain_lifecycle_manifest(ID_BEGIN_TEARDOWN,
                                         XR_OWN_AUDIT_LIFECYCLE_BEGIN_TEARDOWN,
                                         fixture.shared_domain.contract_id,
                                         fixture.shared_domain.semantic_domain,
                                         fixture.shared_domain.materialization));
    XrOwnershipAuditLifecycleManifest overflow = domain_lifecycle_manifest(
        ID_END_TEARDOWN, XR_OWN_AUDIT_LIFECYCLE_END_TEARDOWN,
        fixture.shared_domain.contract_id, fixture.shared_domain.semantic_domain,
        fixture.shared_domain.materialization);
    CHECK(xr_ownership_audit_register_lifecycle(audit, &overflow) ==
              XR_OWN_AUDIT_CAPACITY_EXCEEDED,
          "lifecycle manifest capacity fails closed");
    xr_ownership_audit_destroy(audit);
}

static void test_trivial_noop_finalizer_lifecycle(void) {
    Fixture fixture = make_lifecycle_fixture();
    memset(&fixture.layout.destructor_id, 0, sizeof(fixture.layout.destructor_id));
    fixture.layout.flags &= ~XR_LAYOUT_HAS_DESTRUCTOR;
    seal_layout(&fixture.layout);
    memset(&fixture.owner.destructor_id, 0, sizeof(fixture.owner.destructor_id));

    XrOwnershipAudit *audit = new_audit(32);
    register_manifests(audit, &fixture);
    const uint8_t ids[] = {ID_BEGIN_FINALIZE, ID_END_FINALIZE, ID_RECLAIM};
    const uint8_t kinds[] = {XR_OWN_AUDIT_LIFECYCLE_BEGIN_FINALIZE,
                             XR_OWN_AUDIT_LIFECYCLE_END_FINALIZE,
                             XR_OWN_AUDIT_LIFECYCLE_RECLAIM};
    for (size_t i = 0; i < sizeof(ids); i++) {
        XrOwnershipAuditLifecycleManifest manifest =
            object_lifecycle_manifest(ids[i], kinds[i]);
        memset(&manifest.destructor_id, 0, sizeof(manifest.destructor_id));
        register_lifecycle_manifest(audit, manifest);
    }
    CHECK(record(audit, pin_event(ID_PIN, XR_OWN_EVENT_PIN)) == XR_OWN_AUDIT_OK,
          "trivial finalizer fixture pins generation");
    XrOwnershipAuditObjectKey object = object_key(113, 1);
    XrRuntimeDomainIdentity domain = advance_to_destroy(audit, &fixture, object);
    XrOwnershipAuditLifecycleEvent begin = lifecycle_event(
        object, ID_BEGIN_FINALIZE, XR_OWN_AUDIT_LIFECYCLE_BEGIN_FINALIZE, domain);
    memset(&begin.destructor_id, 0, sizeof(begin.destructor_id));
    CHECK(record_lifecycle(audit, begin) == XR_OWN_AUDIT_OK,
          "trivial allocation begins explicit no-op finalization");
    XrOwnershipAuditEvent destroy = owner_event(
        &fixture, object, ID_DESTROY, XR_OWN_EVENT_DESTROY, domain);
    memset(&destroy.destructor_id, 0, sizeof(destroy.destructor_id));
    CHECK(record(audit, destroy) == XR_OWN_AUDIT_OK,
          "zero destructor identity still records the terminal boundary");
    XrOwnershipAuditLifecycleEvent end = lifecycle_event(
        object, ID_END_FINALIZE, XR_OWN_AUDIT_LIFECYCLE_END_FINALIZE, domain);
    memset(&end.destructor_id, 0, sizeof(end.destructor_id));
    CHECK(record_lifecycle(audit, end) == XR_OWN_AUDIT_OK,
          "trivial no-op finalization ends");
    XrOwnershipAuditLifecycleEvent reclaim = lifecycle_event(
        object, ID_RECLAIM, XR_OWN_AUDIT_LIFECYCLE_RECLAIM, domain);
    memset(&reclaim.destructor_id, 0, sizeof(reclaim.destructor_id));
    CHECK(record_lifecycle(audit, reclaim) == XR_OWN_AUDIT_OK,
          "trivial allocation reclaims after no-op finalizer");
    CHECK(record(audit, pin_event(ID_UNPIN, XR_OWN_EVENT_UNPIN)) == XR_OWN_AUDIT_OK &&
              xr_ownership_audit_finish(audit) == XR_OWN_AUDIT_OK,
          "trivial finalizer lifecycle completes");
    xr_ownership_audit_destroy(audit);
}

enum {
    CONCURRENCY_THREADS = 2,
    CONCURRENCY_TRANSITIONS = 1,
};

typedef struct ConcurrentRecordContext {
    XrOwnershipAudit *audit;
    XrOwnershipAuditEvent event;
    atomic_int *ready;
    atomic_bool *start;
    XrOwnershipAuditStatus result;
} ConcurrentRecordContext;

static void concurrent_yield(void) {
#ifdef XR_OS_WINDOWS
    SwitchToThread();
#else
    sched_yield();
#endif
}

static atomic_bool contention_hook_enabled;
static atomic_bool contention_hook_acquired;
static atomic_bool contention_hook_observed;

void xr_ownership_audit_test_after_enter(XrOwnershipAudit *audit) {
    (void) audit;
    if (!atomic_load_explicit(&contention_hook_enabled, memory_order_acquire))
        return;
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &contention_hook_acquired, &expected, true,
            memory_order_acq_rel, memory_order_acquire))
        return;
    while (!atomic_load_explicit(&contention_hook_observed, memory_order_acquire))
        concurrent_yield();
}

void xr_ownership_audit_test_on_contention(XrOwnershipAudit *audit) {
    (void) audit;
    if (atomic_load_explicit(&contention_hook_enabled, memory_order_acquire))
        atomic_store_explicit(&contention_hook_observed, true, memory_order_release);
}

static void concurrent_record_body(ConcurrentRecordContext *context) {
    atomic_fetch_add_explicit(context->ready, 1, memory_order_release);
    while (!atomic_load_explicit(context->start, memory_order_acquire))
        concurrent_yield();
    context->result = xr_ownership_audit_record(context->audit, &context->event);
}

#ifdef XR_OS_WINDOWS
static unsigned __stdcall concurrent_record_worker(void *raw) {
    concurrent_record_body((ConcurrentRecordContext *) raw);
    return 0;
}
#else
static void *concurrent_record_worker(void *raw) {
    concurrent_record_body((ConcurrentRecordContext *) raw);
    return NULL;
}
#endif

static void test_real_concurrent_entry_poisoning(void) {
    XrOwnershipAuditConfig config = audit_config(CONCURRENCY_THREADS + 4);
    config.max_transition_manifests = CONCURRENCY_TRANSITIONS;
    XrOwnershipAuditStatus create_status;
    XrOwnershipAudit *audit = xr_ownership_audit_create(config, &create_status);
    CHECK(audit != NULL && create_status == XR_OWN_AUDIT_OK,
          "concurrency audit preallocates");
    if (!audit)
        return;

    XrStableId generation = make_id(ID_GENERATION);
    for (uint64_t i = 0; i < CONCURRENCY_TRANSITIONS; i++) {
        XrOwnershipAuditTransitionManifest manifest = {
            .transition_id = make_wide_id(i + 1),
            .operation_id = make_wide_id(i + CONCURRENCY_TRANSITIONS + 1),
            .generation_id = generation,
            .kind = XR_OWN_EVENT_PIN,
            .program_point = XR_OWN_POINT_AFTER_OPERATION,
        };
        if (xr_ownership_audit_register_transition(audit, &manifest) != XR_OWN_AUDIT_OK) {
            CHECK(false, "concurrency transition table registers");
            xr_ownership_audit_destroy(audit);
            return;
        }
    }

    atomic_int ready;
    atomic_bool start;
    atomic_init(&ready, 0);
    atomic_init(&start, false);
    ConcurrentRecordContext contexts[CONCURRENCY_THREADS];
#ifdef XR_OS_WINDOWS
    HANDLE threads[CONCURRENCY_THREADS];
#else
    pthread_t threads[CONCURRENCY_THREADS];
#endif
    size_t started = 0;
    for (size_t i = 0; i < CONCURRENCY_THREADS; i++) {
        contexts[i] = (ConcurrentRecordContext) {
            .audit = audit,
            .event = {
                .transition_id = make_wide_id(CONCURRENCY_TRANSITIONS),
                .operation_id = make_wide_id(CONCURRENCY_TRANSITIONS * 2),
                .generation_id = generation,
                .kind = XR_OWN_EVENT_PIN,
                .program_point = XR_OWN_POINT_AFTER_OPERATION,
            },
            .ready = &ready,
            .start = &start,
            .result = XR_OWN_AUDIT_OK,
        };
#ifdef XR_OS_WINDOWS
        uintptr_t handle = _beginthreadex(NULL, 0, concurrent_record_worker, &contexts[i],
                                          0, NULL);
        if (handle == 0) {
            CHECK(false, "concurrency worker starts");
            break;
        }
        threads[started++] = (HANDLE) handle;
#else
        if (pthread_create(&threads[started], NULL, concurrent_record_worker,
                           &contexts[i]) != 0) {
            CHECK(false, "concurrency worker starts");
            break;
        }
        started++;
#endif
    }
    while ((size_t) atomic_load_explicit(&ready, memory_order_acquire) != started)
        concurrent_yield();
    CHECK(started >= 2, "concurrency gate starts at least two workers");
    size_t allocation_count = xr_ownership_audit_allocation_count(audit);
    atomic_store_explicit(&contention_hook_acquired, false, memory_order_release);
    atomic_store_explicit(&contention_hook_observed, false, memory_order_release);
    atomic_store_explicit(&contention_hook_enabled, started >= 2, memory_order_release);
    atomic_store_explicit(&start, true, memory_order_release);
    for (size_t i = 0; i < started; i++) {
#ifdef XR_OS_WINDOWS
        CHECK(WaitForSingleObject(threads[i], INFINITE) == WAIT_OBJECT_0,
              "concurrency worker joins");
        CloseHandle(threads[i]);
#else
        CHECK(pthread_join(threads[i], NULL) == 0, "concurrency worker joins");
#endif
    }
    atomic_store_explicit(&contention_hook_enabled, false, memory_order_release);
    CHECK(atomic_load_explicit(&contention_hook_acquired, memory_order_acquire) &&
              atomic_load_explicit(&contention_hook_observed, memory_order_acquire),
          "test hook deterministically proves overlapping gate contention");
    CHECK(xr_ownership_audit_status(audit) == XR_OWN_AUDIT_REENTRANT,
          "real concurrent recorder contention poisons the audit");
    CHECK(xr_ownership_audit_allocation_count(audit) == allocation_count,
          "contended record path remains allocation-free");
    CHECK(xr_ownership_audit_finish(audit) == XR_OWN_AUDIT_REENTRANT,
          "concurrent-entry poison remains sticky through finish");
    xr_ownership_audit_destroy(audit);
}

int main(void) {
    test_dynamic_instances_and_event_reuse();
    test_seeded_identity_failures();
    test_seeded_lifecycle_failures();
    test_exit_destructor_capacity_and_oom();
    test_finalize_reclaim_with_and_without_teardown();
    test_lifecycle_adversarial_ordering();
    test_teardown_adversarial_boundaries();
    test_finalize_requires_reclaim_at_finish();
    test_exact_domain_drain_cleanup_matrix();
    test_lifecycle_additional_mutations_and_capacity();
    test_trivial_noop_finalizer_lifecycle();
    test_real_concurrent_entry_poisoning();
    if (failures != 0) {
        fprintf(stderr, "%d ownership audit test(s) failed\n", failures);
        return 1;
    }
    puts("ownership audit tests passed");
    return 0;
}
