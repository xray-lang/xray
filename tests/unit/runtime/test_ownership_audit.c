#include "runtime/ownership/xr_ownership_audit.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

static XrOwnershipAuditConfig audit_config(size_t event_capacity) {
    return (XrOwnershipAuditConfig) {.max_owner_manifests = 2,
                                     .max_transition_manifests = 16,
                                     .max_dynamic_instances = 8,
                                     .max_events = event_capacity,
                                     .max_loans = 4,
                                     .max_generations = 2};
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
    XrOwnershipAuditTransitionManifest unpin = pin;
    unpin.transition_id = make_id(ID_UNPIN);
    unpin.operation_id = make_id((uint8_t) (ID_UNPIN + 80));
    unpin.kind = XR_OWN_EVENT_UNPIN;
    register_transition(audit, unpin);
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
    if (transition_id == ID_BORROW || transition_id == ID_END_BORROW)
        event.loan_id = make_id(ID_LOAN);
    return event;
}

static XrOwnershipAuditStatus record(XrOwnershipAudit *audit,
                                     XrOwnershipAuditEvent event) {
    return xr_ownership_audit_record(audit, &event);
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

int main(void) {
    test_dynamic_instances_and_event_reuse();
    test_seeded_identity_failures();
    test_seeded_lifecycle_failures();
    test_exit_destructor_capacity_and_oom();
    if (failures != 0) {
        fprintf(stderr, "%d ownership audit test(s) failed\n", failures);
        return 1;
    }
    puts("ownership audit tests passed");
    return 0;
}
