/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_managed_root.c - Stable activation owners, pending rollback and nonlocal drain
 *
 * KEY CONCEPT:
 *   External ownership survives longjmp without consulting abandoned C activations.
 */
#include "runtime/native/xnative_managed_root.h"
#include "base/xmalloc.h"
#include <setjmp.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); abort(); } } while (0)
#define OK(c) CHECK((c) == XRT_MANAGED_OK)
#define BAD(c) CHECK((c) == XRT_MANAGED_ARGUMENT)

typedef struct Audit {
    size_t bytes, blocks, finalized, count, events[64];
    XrtManagedFrame *watch;
    XrtManagedHeader *replacement;
    size_t watch_id;
    bool published;
} Audit;
typedef struct Provider { _Atomic(int32_t) references; Audit *audit; } Provider;
typedef struct Payload {
    XrtManagedHeader header;
    XrtManagedHeader *child;
    size_t capacity, id;
} Payload;
typedef struct Fixture {
    Audit audit;
    Provider *provider;
    XrtBillingOwner *billing;
    XrtRuntimeTypePlan *plan;
    XrtManagedOps *ops;
} Fixture;

static bool provider_retain(void *context) {
    return xr_signed_rc_retain(&((Provider *)context)->references);
}
static void provider_drop(void *context) {
    Provider *provider = context;
    XrSignedRcRelease released = xr_signed_rc_release(&provider->references);
    CHECK(released != XR_SIGNED_RC_INVALID);
    if (released == XR_SIGNED_RC_LAST) {
        CHECK(!provider->audit->bytes && !provider->audit->blocks);
        ++provider->audit->finalized;
        xr_free(provider);
    }
}
static void *provider_allocate(void *context, size_t bytes) {
    Audit *audit = ((Provider *)context)->audit;
    void *memory = xr_malloc(bytes);
    if (memory) { audit->bytes += bytes; ++audit->blocks; }
    return memory;
}
static void provider_free(void *context, void *memory, size_t bytes) {
    Audit *audit = ((Provider *)context)->audit;
    CHECK(audit->bytes >= bytes && audit->blocks);
    audit->bytes -= bytes;
    --audit->blocks;
    xr_free(memory);
}
static void payload_drop(XrtManagedHeader *value, void *context) {
    Payload *payload = (Payload *)value;
    Audit *audit = ((Provider *)context)->audit;
    CHECK(!payload->child && audit->count < 64);
    audit->events[audit->count++] = payload->id;
    if (audit->watch && payload->id == audit->watch_id) {
        XrtManagedHeader *current = NULL;
        OK(xrt_managed_slot_borrow(audit->watch, 0, &current));
        CHECK(current == audit->replacement);
        BAD(xrt_managed_slot_drop(audit->watch, 0));
        audit->published = true;
    }
}
static void fixture_init(Fixture *fixture) {
    *fixture = (Fixture){0};
    Provider *provider = xr_malloc(sizeof(*provider));
    CHECK(provider);
    atomic_init(&provider->references, -1);
    provider->audit = &fixture->audit;
    fixture->provider = provider;
    XrtManagedAllocator allocator = {provider, provider_allocate, provider_free,
        {provider, provider_retain, provider_drop}};
    OK(xrt_billing_owner_new(&allocator, SIZE_MAX, &fixture->billing));
    size_t child = offsetof(Payload, child);
    XrtRuntimeTypeSpec spec = {.fixed_size=sizeof(Payload), .alignment=alignof(Payload),
        .capacity_offset=offsetof(Payload, capacity), .stride=1, .child_offsets=&child,
        .child_count=1, .drop_payload=payload_drop, .code={provider, provider_retain, provider_drop}};
    OK(xrt_runtime_type_plan_new(fixture->billing, &spec, &fixture->plan));
    OK(xrt_managed_ops_bind(fixture->billing, fixture->plan, &fixture->ops));
}
static void fixture_finish(Fixture *fixture) {
    xrt_managed_ops_drop(fixture->ops);
    xrt_runtime_type_plan_drop(fixture->plan);
    xrt_billing_owner_drop(fixture->billing);
    provider_drop(fixture->provider);
    CHECK(fixture->audit.finalized == 1);
}
static void same_storage(XrtBillingStats before, XrtBillingStats after) {
    CHECK(before.bytes == after.bytes && before.blocks == after.blocks);
}
static XrtManagedHeader *make_value(Fixture *fixture, size_t id) {
    XrtManagedPending pending = {0};
    OK(xrt_managed_prepare(fixture->ops, sizeof(Payload) + 7, &pending));
    ((Payload *)pending.value)->id = id;
    XrtManagedHeader *owned = NULL;
    OK(xrt_managed_commit(&pending, &owned));
    return owned;
}
static Payload *prepare_value(Fixture *fixture, XrtManagedFrame *frame, size_t slot, size_t id) {
    XrtManagedHeader *payload = NULL;
    OK(xrt_managed_slot_prepare(frame, slot, fixture->ops, sizeof(Payload) + 11, &payload));
    ((Payload *)payload)->id = id;
    return (Payload *)payload;
}
static size_t event_position(const Audit *audit, size_t id) {
    for (size_t i = 0; i < audit->count; ++i) if (audit->events[i] == id) return i;
    CHECK(false);
    return SIZE_MAX;
}

static void aliases_and_return(void) {
    Fixture fixture;
    fixture_init(&fixture);
    XrtBillingStats baseline = xrt_billing_owner_stats(fixture.billing);
    XrtManagedRoot *root = NULL;
    OK(xrt_managed_root_new(fixture.billing, &root));
    XrtBillingStats root_only = xrt_billing_owner_stats(fixture.billing);
    XrtManagedFrame *caller = NULL, *callee = NULL;
    OK(xrt_managed_frame_enter(root, 3, &caller));
    XrtManagedHeader *owned = make_value(&fixture, 1), *alias = owned;
    CHECK(xrt_managed_retain(alias));
    OK(xrt_managed_slot_adopt(caller, 0, &owned));
    CHECK(!owned);
    OK(xrt_managed_slot_adopt(caller, 1, &alias));
    OK(xrt_managed_slot_drop(caller, 0));
    CHECK(!fixture.audit.count);
    BAD(xrt_managed_slot_move(caller, 1, caller, 1));
    OK(xrt_managed_slot_drop(caller, 1));
    CHECK(fixture.audit.count == 1 && fixture.audit.events[0] == 1);
    OK(xrt_managed_frame_enter(root, 2, &callee));
    BAD(xrt_managed_frame_leave(root, caller));
    owned = make_value(&fixture, 2);
    OK(xrt_managed_slot_adopt(callee, 0, &owned));
    OK(xrt_managed_slot_move(caller, 0, callee, 0));
    XrtManagedHeader *borrowed = NULL;
    OK(xrt_managed_slot_borrow(callee, 0, &borrowed));
    CHECK(!borrowed);
    BAD(xrt_managed_slot_borrow(callee, 2, &borrowed));
    OK(xrt_managed_frame_leave(root, callee));
    callee = NULL; /* A retired frame handle is never an admissible argument. */
    CHECK(fixture.audit.count == 1);
    OK(xrt_managed_slot_borrow(caller, 0, &borrowed));
    CHECK(borrowed && ((Payload *)borrowed)->id == 2);
    OK(xrt_managed_frame_leave(root, caller));
    caller = NULL;
    CHECK(fixture.audit.count == 2 && fixture.audit.events[1] == 2);
    same_storage(root_only, xrt_billing_owner_stats(fixture.billing));
    OK(xrt_managed_root_drain(root));
    OK(xrt_managed_root_drain(root));
    OK(xrt_managed_root_destroy(&root));
    OK(xrt_managed_root_destroy(&root));
    same_storage(baseline, xrt_billing_owner_stats(fixture.billing));
    fixture_finish(&fixture);
}
static void replacement_and_extent(void) {
    Fixture fixture;
    fixture_init(&fixture);
    XrtManagedRoot *root = NULL;
    XrtManagedFrame *frame = NULL;
    OK(xrt_managed_root_new(fixture.billing, &root));
    OK(xrt_managed_frame_enter(root, 2, &frame));
    XrtManagedHeader *old = make_value(&fixture, 3);
    OK(xrt_managed_slot_adopt(frame, 0, &old));
    XrtBillingStats old_storage = xrt_billing_owner_stats(fixture.billing);
    Payload *pending = prepare_value(&fixture, frame, 0, 4);
    pending->capacity = SIZE_MAX;
    BAD(xrt_managed_slot_commit(frame, 0));
    BAD(xrt_managed_slot_drop(frame, 0));
    XrtManagedHeader *borrowed = NULL;
    OK(xrt_managed_slot_borrow(frame, 0, &borrowed));
    CHECK(((Payload *)borrowed)->id == 3 && !fixture.audit.count);
    OK(xrt_managed_slot_abort(frame, 0));
    same_storage(old_storage, xrt_billing_owner_stats(fixture.billing));
    CHECK(fixture.audit.count == 1 && fixture.audit.events[0] == 4);
    pending = prepare_value(&fixture, frame, 0, 5);
    fixture.audit.watch = frame;
    fixture.audit.watch_id = 3;
    fixture.audit.replacement = &pending->header;
    OK(xrt_managed_slot_commit(frame, 0));
    CHECK(fixture.audit.published && fixture.audit.events[1] == 3);
    fixture.audit.watch = NULL;
    xrt_billing_owner_fail_after(fixture.billing, 0);
    borrowed = NULL;
    CHECK(xrt_managed_slot_prepare(frame, 0, fixture.ops, sizeof(Payload), &borrowed) == XRT_MANAGED_OOM);
    CHECK(!borrowed);
    xrt_billing_owner_fail_after(fixture.billing, SIZE_MAX);
    prepare_value(&fixture, frame, 0, 6);
    OK(xrt_managed_slot_abort(frame, 0));
    borrowed = NULL;
    OK(xrt_managed_slot_borrow(frame, 0, &borrowed));
    CHECK(((Payload *)borrowed)->id == 5);
    OK(xrt_managed_frame_leave(root, frame));
    CHECK(fixture.audit.count == 4 && fixture.audit.events[3] == 5);
    OK(xrt_managed_root_destroy(&root));
    fixture_finish(&fixture);
}
static void partial_children(void) {
    Fixture fixture;
    fixture_init(&fixture);
    XrtManagedRoot *root = NULL;
    XrtManagedFrame *frame = NULL;
    OK(xrt_managed_root_new(fixture.billing, &root));
    OK(xrt_managed_frame_enter(root, 1, &frame));
    XrtManagedHeader *owned = make_value(&fixture, 31);
    OK(xrt_managed_slot_adopt(frame, 0, &owned));
    owned = make_value(&fixture, 34);
    BAD(xrt_managed_slot_adopt(frame, 0, &owned));
    CHECK(owned && ((Payload *)owned)->id == 34);
    xrt_managed_drop(owned);
    XrtBillingStats old_storage = xrt_billing_owner_stats(fixture.billing);
    Payload *pending = prepare_value(&fixture, frame, 0, 32);
    pending->child = make_value(&fixture, 33);
    XrtManagedHeader *output = NULL;
    BAD(xrt_managed_slot_prepare(frame, 0, fixture.ops, sizeof(Payload), &output));
    CHECK(!output);
    pending->capacity = SIZE_MAX;
    BAD(xrt_managed_slot_commit(frame, 0));
    OK(xrt_managed_slot_abort(frame, 0));
    same_storage(old_storage, xrt_billing_owner_stats(fixture.billing));
    OK(xrt_managed_root_destroy(&root));
    CHECK(fixture.audit.count == 4);
    CHECK(event_position(&fixture.audit, 33) < event_position(&fixture.audit, 32));
    CHECK(event_position(&fixture.audit, 32) < event_position(&fixture.audit, 31));
    fixture_finish(&fixture);
}

typedef struct JumpContext { Fixture *fixture; XrtManagedRoot *root; } JumpContext;
static void abandoned_frames(JumpContext *context, jmp_buf target) {
    XrtManagedFrame *parent = NULL, *child = NULL;
    OK(xrt_managed_frame_enter(context->root, 2, &parent));
    XrtManagedHeader *owned = make_value(context->fixture, 10);
    OK(xrt_managed_slot_adopt(parent, 0, &owned));
    prepare_value(context->fixture, parent, 0, 11);
    OK(xrt_managed_frame_enter(context->root, 2, &child));
    owned = make_value(context->fixture, 20);
    OK(xrt_managed_slot_adopt(child, 0, &owned));
    Payload *pending = prepare_value(context->fixture, child, 0, 21);
    pending->capacity = SIZE_MAX;
    longjmp(target, 1);
}
static void run_jump(JumpContext *context) {
    jmp_buf target;
    /* The owned Root and every changed owner reside outside this function. */
    if (setjmp(target) == 0) abandoned_frames(context, target);
    OK(xrt_managed_root_drain(context->root));
}
static void nonlocal_cleanup(void) {
    Fixture fixture;
    fixture_init(&fixture);
    JumpContext *context = xr_malloc(sizeof(*context));
    CHECK(context);
    *context = (JumpContext){&fixture, NULL};
    OK(xrt_managed_root_new(fixture.billing, &context->root));
    XrtBillingStats before = xrt_billing_owner_stats(fixture.billing);
    run_jump(context);
    CHECK(fixture.audit.count == 4);
    CHECK(event_position(&fixture.audit, 11) < event_position(&fixture.audit, 10));
    CHECK(event_position(&fixture.audit, 11) < event_position(&fixture.audit, 20));
    CHECK(event_position(&fixture.audit, 21) < event_position(&fixture.audit, 10));
    CHECK(event_position(&fixture.audit, 21) < event_position(&fixture.audit, 20));
    same_storage(before, xrt_billing_owner_stats(fixture.billing));
    XrtManagedFrame *recovered = NULL;
    OK(xrt_managed_frame_enter(context->root, 1, &recovered));
    prepare_value(&fixture, recovered, 0, 22);
    OK(xrt_managed_slot_commit(recovered, 0));
    OK(xrt_managed_frame_leave(context->root, recovered));
    OK(xrt_managed_root_destroy(&context->root));
    xr_free(context);
    fixture_finish(&fixture);
}
static void metadata_failures(void) {
    Fixture fixture;
    fixture_init(&fixture);
    XrtBillingStats baseline = xrt_billing_owner_stats(fixture.billing);
    size_t failures = 0;
    for (size_t site = 0; site < 12; ++site) {
        XrtManagedRoot *root = NULL;
        xrt_billing_owner_fail_after(fixture.billing, site);
        XrtManagedStatus status = xrt_managed_root_new(fixture.billing, &root);
        xrt_billing_owner_fail_after(fixture.billing, SIZE_MAX);
        if (status == XRT_MANAGED_OK) {
            OK(xrt_managed_root_destroy(&root));
            same_storage(baseline, xrt_billing_owner_stats(fixture.billing));
            break;
        }
        CHECK(status == XRT_MANAGED_OOM && !root);
        ++failures;
        same_storage(baseline, xrt_billing_owner_stats(fixture.billing));
        OK(xrt_managed_root_new(fixture.billing, &root));
        OK(xrt_managed_root_destroy(&root));
        same_storage(baseline, xrt_billing_owner_stats(fixture.billing));
    }
    CHECK(failures == 5);
    XrtManagedRoot *root = NULL;
    XrtManagedFrame *parent = NULL, *child = NULL;
    OK(xrt_managed_root_new(fixture.billing, &root));
    OK(xrt_managed_frame_enter(root, 1, &parent));
    XrtBillingStats parent_storage = xrt_billing_owner_stats(fixture.billing);
    xrt_billing_owner_fail_after(fixture.billing, 0);
    CHECK(xrt_managed_frame_enter(root, 9, &child) == XRT_MANAGED_OOM && !child);
    BAD(xrt_managed_frame_enter(root, SIZE_MAX, &child));
    BAD(xrt_managed_frame_enter(root, 0, &child));
    same_storage(parent_storage, xrt_billing_owner_stats(fixture.billing));
    xrt_billing_owner_fail_after(fixture.billing, SIZE_MAX);
    OK(xrt_managed_frame_enter(root, 9, &child));
    CHECK(xrt_billing_owner_stats(fixture.billing).allocations == parent_storage.allocations + 1);
    OK(xrt_managed_frame_leave(root, child));
    same_storage(parent_storage, xrt_billing_owner_stats(fixture.billing));
    OK(xrt_managed_frame_leave(root, parent));
    OK(xrt_managed_root_destroy(&root));
    same_storage(baseline, xrt_billing_owner_stats(fixture.billing));
    fixture_finish(&fixture);
    printf("Root metadata OOM sites: %zu; frame OOM and recovery: PASS\n", failures);
}
static size_t scalar_call(size_t input) { return input * 3 + 1; }
static void scalar_and_closed(void) {
    Fixture fixture;
    fixture_init(&fixture);
    XrtBillingStats before = xrt_billing_owner_stats(fixture.billing);
    CHECK(scalar_call(7) == 22);
    CHECK(xrt_billing_owner_stats(fixture.billing).allocations == before.allocations);
    XrtManagedRoot *root = NULL;
    XrtManagedFrame *frame = NULL;
    OK(xrt_managed_root_new(fixture.billing, &root));
    OK(xrt_managed_frame_enter(root, 2, &frame));
    prepare_value(&fixture, frame, 0, 30);
    OK(xrt_managed_slot_commit(frame, 0));
    prepare_value(&fixture, frame, 0, 31);
    xrt_billing_owner_close(fixture.billing);
    XrtManagedHeader *payload = NULL;
    CHECK(xrt_managed_slot_prepare(frame, 1, fixture.ops, sizeof(Payload), &payload) == XRT_MANAGED_CLOSED);
    OK(xrt_managed_root_destroy(&root));
    CHECK(fixture.audit.count == 2 && fixture.audit.events[0] == 31 && fixture.audit.events[1] == 30);
    fixture_finish(&fixture);
}
int main(void) {
    aliases_and_return();
    replacement_and_extent();
    partial_children();
    nonlocal_cleanup();
    metadata_failures();
    scalar_and_closed();
    puts("Stable managed Root/Frame: real aliases, publication, extent rollback and longjmp drain PASS");
    return 0;
}
