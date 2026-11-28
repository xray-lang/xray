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
#include "runtime/native/xnative_execution.h"
#include "runtime/native/xnative_string_backing.h"
#include "runtime/native/xnative_values.h"
#include "xray_native_values.h"
#include "base/xmalloc.h"
#include "base/xmax_align.h"
#include <setjmp.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); abort(); } } while (0)
#define OK(c) CHECK((c) == XRT_MANAGED_OK)
#define BAD(c) CHECK((c) == XRT_MANAGED_ARGUMENT)

typedef struct Audit {
    size_t bytes, blocks, finalized, count, events[64];
    XrtManagedFrame *watch;
    XrtManagedHeader *replacement;
    XrtManagedHeader **input_owner;
    size_t watch_id;
    bool published;
    const XrtManagedFrameLocations *locations_watch;
    XrtManagedRoot *locations_root;
    size_t locations_callbacks;
    bool locations_allocate, locations_drop;
} Audit;

static void locations_busy_callback(Audit *);
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
    if (audit->locations_watch && audit->locations_allocate) locations_busy_callback(audit);
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
    if (audit->locations_watch && audit->locations_drop) locations_busy_callback(audit);
    CHECK(!payload->child && audit->count < 64);
    audit->events[audit->count++] = payload->id;
    if (audit->watch && payload->id == audit->watch_id) {
        XrtManagedHeader *current = NULL;
        OK(xrt_managed_slot_borrow(audit->watch, 0, &current));
        CHECK(current == audit->replacement);
        if (audit->input_owner) CHECK(!*audit->input_owner);
        BAD(xrt_managed_slot_drop(audit->watch, 0));
        XrtManagedHeader *taken = NULL;
        BAD(xrt_managed_slot_take(audit->watch, 0, &taken));
        CHECK(!taken);
        XrtManagedHeader *candidate = current;
        CHECK(xrt_managed_retain(candidate));
        BAD(xrt_managed_slot_replace(audit->watch, 0, &candidate));
        CHECK(candidate == current);
        xrt_managed_drop(candidate);
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
static void independent_taken_owner(void) {
    Fixture fixture;
    fixture_init(&fixture);
    XrtManagedRoot *root = NULL;
    XrtManagedFrame *frame = NULL;
    OK(xrt_managed_root_new(fixture.billing, &root));
    OK(xrt_managed_frame_enter(root, 3, &frame));
    XrtManagedHeader *value = make_value(&fixture, 41), *alias = value, *output = NULL;
    CHECK(xrt_managed_retain(alias) && xr_signed_rc_read(&value->references) == 2);
    OK(xrt_managed_slot_adopt(frame, 0, &value));
    OK(xrt_managed_slot_adopt(frame, 1, &alias));
    BAD(xrt_managed_slot_take(frame, 3, &output));
    BAD(xrt_managed_slot_take(frame, 2, &output));
    BAD(xrt_managed_slot_take(frame, 0, NULL));
    XrtBillingStats before = xrt_billing_owner_stats(fixture.billing);
    xrt_billing_owner_fail_after(fixture.billing, 0);
    OK(xrt_managed_slot_take(frame, 0, &output));
    CHECK(output && ((Payload *)output)->id == 41 && xr_signed_rc_read(&output->references) == 2);
    same_storage(before, xrt_billing_owner_stats(fixture.billing));
    CHECK(before.allocations == xrt_billing_owner_stats(fixture.billing).allocations);
    XrtManagedHeader *borrowed = NULL;
    OK(xrt_managed_slot_borrow(frame, 0, &borrowed));
    CHECK(!borrowed);
    OK(xrt_managed_slot_borrow(frame, 1, &borrowed));
    CHECK(borrowed == output);
    BAD(xrt_managed_slot_take(frame, 1, &output));
    CHECK(xr_signed_rc_read(&output->references) == 2);
    xrt_billing_owner_fail_after(fixture.billing, SIZE_MAX);
    Payload *pending = prepare_value(&fixture, frame, 1, 42);
    pending->capacity = SIZE_MAX;
    BAD(xrt_managed_slot_commit(frame, 1));
    XrtManagedHeader *other = NULL;
    BAD(xrt_managed_slot_take(frame, 1, &other));
    CHECK(!other);
    borrowed = NULL;
    OK(xrt_managed_slot_borrow(frame, 1, &borrowed));
    CHECK(borrowed == output && xr_signed_rc_read(&borrowed->references) == 2);
    OK(xrt_managed_slot_abort(frame, 1));
    CHECK(fixture.audit.count == 1 && fixture.audit.events[0] == 42);
    same_storage(before, xrt_billing_owner_stats(fixture.billing));
    xrt_billing_owner_close(fixture.billing);
    xrt_billing_owner_fail_after(fixture.billing, 0);
    before = xrt_billing_owner_stats(fixture.billing);
    OK(xrt_managed_slot_take(frame, 1, &other));
    CHECK(other == output && xr_signed_rc_read(&output->references) == 2);
    same_storage(before, xrt_billing_owner_stats(fixture.billing));
    CHECK(before.allocations == xrt_billing_owner_stats(fixture.billing).allocations);
    xrt_managed_drop(other);
    CHECK(xr_signed_rc_read(&output->references) == 1 && fixture.audit.count == 1);
    OK(xrt_managed_root_destroy(&root));
    xrt_managed_ops_drop(fixture.ops);
    xrt_runtime_type_plan_drop(fixture.plan);
    xrt_billing_owner_drop(fixture.billing);
    provider_drop(fixture.provider);
    fixture.ops = NULL; fixture.plan = NULL; fixture.billing = NULL; fixture.provider = NULL;
    CHECK(!fixture.audit.finalized && fixture.audit.bytes && fixture.audit.blocks);
    CHECK(((Payload *)output)->id == 41 && !((Payload *)output)->child);
    xrt_managed_drop(output);
    CHECK(fixture.audit.finalized == 1 && fixture.audit.count == 2 && fixture.audit.events[1] == 41);
    CHECK(!fixture.audit.bytes && !fixture.audit.blocks);
    puts("Taken real owner survives closed billing, Root and creator retirement PASS");
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
static void committed_owner_replacement(void) {
    Fixture fixture;
    fixture_init(&fixture);
    XrtManagedRoot *root = NULL;
    XrtManagedFrame *frame = NULL;
    OK(xrt_managed_root_new(fixture.billing, &root));
    OK(xrt_managed_frame_enter(root, 2, &frame));
    XrtManagedHeader *old = make_value(&fixture, 51), *owned = make_value(&fixture, 52);
    OK(xrt_managed_slot_adopt(frame, 0, &old));
    BAD(xrt_managed_slot_replace(frame, 2, &owned));
    BAD(xrt_managed_slot_replace(frame, 0, NULL));
    BAD(xrt_managed_slot_replace(frame, 0, &old));
    prepare_value(&fixture, frame, 0, 53);
    BAD(xrt_managed_slot_replace(frame, 0, &owned));
    CHECK(owned && ((Payload *)owned)->id == 52 && !fixture.audit.count);
    OK(xrt_managed_slot_abort(frame, 0));
    CHECK(fixture.audit.count == 1 && fixture.audit.events[0] == 53);
    fixture.audit.watch = frame;
    fixture.audit.watch_id = 51;
    fixture.audit.replacement = owned;
    fixture.audit.input_owner = &owned;
    XrtBillingStats before = xrt_billing_owner_stats(fixture.billing);
    xrt_billing_owner_close(fixture.billing);
    xrt_billing_owner_fail_after(fixture.billing, 0);
    OK(xrt_managed_slot_replace(frame, 0, &owned));
    CHECK(!owned && fixture.audit.published && fixture.audit.count == 2 && fixture.audit.events[1] == 51);
    XrtBillingStats after = xrt_billing_owner_stats(fixture.billing);
    CHECK(after.allocations == before.allocations && after.blocks + 1 == before.blocks &&
        after.bytes + sizeof(Payload) + 7 == before.bytes);
    fixture.audit.watch = NULL;
    fixture.audit.input_owner = NULL;
    OK(xrt_managed_slot_borrow(frame, 0, &owned));
    CHECK(xrt_managed_retain(owned) && xr_signed_rc_read(&owned->references) == 2);
    OK(xrt_managed_slot_replace(frame, 0, &owned));
    CHECK(!owned && fixture.audit.count == 2);
    same_storage(after, xrt_billing_owner_stats(fixture.billing));
    OK(xrt_managed_slot_take(frame, 0, &owned));
    CHECK(((Payload *)owned)->id == 52 && xr_signed_rc_read(&owned->references) == 1);
    OK(xrt_managed_slot_replace(frame, 1, &owned));
    CHECK(!owned && after.allocations == xrt_billing_owner_stats(fixture.billing).allocations);
    OK(xrt_managed_root_destroy(&root));
    CHECK(fixture.audit.count == 3 && fixture.audit.events[2] == 52);
    fixture_finish(&fixture);
    puts("Committed owner replacement publishes before retirement without allocation PASS");
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
typedef struct InlineHomes {
    int64_t tag;
    XrtManagedHeader *first;
    double scalar;
    XrtManagedHeader *second;
} InlineHomes;
static void inline_payload_homes(void) {
    Fixture fixture;
    fixture_init(&fixture);
    XrtManagedRoot *root = NULL;
    XrtManagedFrame *frame = NULL, *child = NULL;
    OK(xrt_managed_root_new(fixture.billing, &root));
    XrtBillingStats baseline = xrt_billing_owner_stats(fixture.billing);
    size_t offsets[] = {offsetof(InlineHomes, first), offsetof(InlineHomes, second), SIZE_MAX, SIZE_MAX};
    XrtManagedFrameSpec spec = {4, sizeof(InlineHomes), alignof(XrMaxAlignment), offsets};
    void *memory = NULL, *observed = NULL;
    OK(xrt_managed_frame_enter_payload(root, &spec, &frame, &memory));
    CHECK(xrt_billing_owner_stats(fixture.billing).allocations == baseline.allocations + 1);
    size_t bytes = 0;
    OK(xrt_managed_frame_payload(frame, &observed, &bytes));
    CHECK(memory == observed && memory && bytes == sizeof(InlineHomes) &&
        (uintptr_t)memory % alignof(XrMaxAlignment) == 0);
    InlineHomes *homes = memory;
    CHECK(!homes->first && !homes->second);
    homes->tag = 17;
    homes->scalar = 1.25;
    /* Neither the spec nor its borrowed vector remains in the stable frame. */
    offsets[0] = offsets[1] = SIZE_MAX;
    spec = (XrtManagedFrameSpec){0};
    XrtManagedSlotLocation first, second;
    OK(xrt_managed_slot_location(frame, 0, &first));
    OK(xrt_managed_slot_location(frame, 1, &second));
    CHECK(first.owner == &homes->first && second.owner == &homes->second);
    Payload *old = prepare_value(&fixture, frame, 0, 40);
    CHECK(!homes->first);
    OK(xrt_managed_slot_commit(frame, 0));
    CHECK(homes->first == &old->header);
    Payload *replacement = prepare_value(&fixture, frame, 0, 41);
    fixture.audit.watch = frame;
    fixture.audit.watch_id = 40;
    fixture.audit.replacement = &replacement->header;
    OK(xrt_managed_slot_commit(frame, 0));
    CHECK(fixture.audit.published && homes->first == &replacement->header);
    fixture.audit.watch = NULL;
    OK(xrt_managed_slot_move(frame, 1, frame, 0));
    CHECK(!homes->first && homes->second == &replacement->header);
    XrtBillingStats with_frame = xrt_billing_owner_stats(fixture.billing);
    xrt_billing_owner_fail_after(fixture.billing, 0);
    size_t child_offsets[] = {offsetof(InlineHomes, first)};
    XrtManagedFrameSpec child_spec = {1, sizeof(InlineHomes), alignof(InlineHomes), child_offsets};
    CHECK(xrt_managed_frame_enter_layout(root, &child_spec, &child) == XRT_MANAGED_OOM && !child);
    same_storage(with_frame, xrt_billing_owner_stats(fixture.billing));
    CHECK(homes->tag == 17 && homes->scalar == 1.25 && homes->second == &replacement->header);
    xrt_billing_owner_fail_after(fixture.billing, SIZE_MAX);
    OK(xrt_managed_frame_enter_layout(root, &child_spec, &child));
    OK(xrt_managed_slot_move(child, 0, frame, 1));
    CHECK(!homes->second);
    OK(xrt_managed_frame_leave(root, child));
    CHECK(fixture.audit.count == 2);
    OK(xrt_managed_frame_leave(root, frame));
    same_storage(baseline, xrt_billing_owner_stats(fixture.billing));
    OK(xrt_managed_root_destroy(&root));
    fixture_finish(&fixture);
    puts("inline frame homes: real Header fields, one allocation, atomic publication and source retirement PASS");
}
static void combined_frame_boundary(void) {
    Fixture fixture;
    fixture_init(&fixture);
    XrtManagedRoot *root = NULL;
    XrtManagedFrame *frame = NULL;
    void *payload = NULL;
    OK(xrt_managed_root_new(fixture.billing, &root));
    XrtBillingStats baseline = xrt_billing_owner_stats(fixture.billing);
    XrtManagedFrameSpec spec = {1, 0, 1, NULL};
    BAD(xrt_managed_frame_enter_payload(root, &spec, NULL, &payload));
    BAD(xrt_managed_frame_enter_payload(root, &spec, &frame, NULL));
    BAD(xrt_managed_frame_enter_payload(NULL, &spec, &frame, &payload));
    payload = &fixture;
    BAD(xrt_managed_frame_enter_payload(root, &spec, &frame, &payload));
    CHECK(!frame && payload == &fixture);
    payload = NULL;
    spec.payload_alignment = 3;
    BAD(xrt_managed_frame_enter_payload(root, &spec, &frame, &payload));
    spec.payload_alignment = 1;
    same_storage(baseline, xrt_billing_owner_stats(fixture.billing));
    xrt_billing_owner_fail_after(fixture.billing, 0);
    CHECK(xrt_managed_frame_enter_payload(root, &spec, &frame, &payload) == XRT_MANAGED_OOM);
    CHECK(!frame && !payload);
    same_storage(baseline, xrt_billing_owner_stats(fixture.billing));
    xrt_billing_owner_fail_after(fixture.billing, SIZE_MAX);
    OK(xrt_managed_frame_enter_payload(root, &spec, &frame, &payload));
    CHECK(frame && !payload);
    XrtManagedFrame *existing = frame;
    BAD(xrt_managed_frame_enter_payload(root, &spec, &existing, &payload));
    CHECK(existing == frame && !payload);
    OK(xrt_managed_frame_leave(root, frame));
    same_storage(baseline, xrt_billing_owner_stats(fixture.billing));
    OK(xrt_managed_root_destroy(&root));
    fixture_finish(&fixture);
}
static void inline_payload_rejection(void) {
    Fixture fixture;
    fixture_init(&fixture);
    XrtManagedRoot *root = NULL;
    XrtManagedFrame *frame = NULL;
    OK(xrt_managed_root_new(fixture.billing, &root));
    XrtBillingStats baseline = xrt_billing_owner_stats(fixture.billing);
    size_t offsets[] = {8, 8};
    XrtManagedFrameSpec spec = {2, 32, alignof(XrtManagedHeader *), offsets};
    BAD(xrt_managed_frame_enter_layout(root, &spec, &frame));
    offsets[0] = 24; offsets[1] = 8;
    BAD(xrt_managed_frame_enter_layout(root, &spec, &frame));
    offsets[0] = 1; offsets[1] = 16;
    BAD(xrt_managed_frame_enter_layout(root, &spec, &frame));
    offsets[0] = 0; offsets[1] = 32;
    BAD(xrt_managed_frame_enter_layout(root, &spec, &frame));
    offsets[1] = 24; spec.payload_size = 31;
    BAD(xrt_managed_frame_enter_layout(root, &spec, &frame));
    spec.payload_size = 32; spec.payload_alignment = 1;
    BAD(xrt_managed_frame_enter_layout(root, &spec, &frame));
    spec.payload_alignment = 3;
    BAD(xrt_managed_frame_enter_layout(root, &spec, &frame));
    /* An explicit payload alignment above the fundamental one is admitted up to
     * the shared limit; only a request past that limit is refused here. */
    spec.payload_alignment = XR_ALIGNMENT_LIMIT * 2;
    BAD(xrt_managed_frame_enter_layout(root, &spec, &frame));
    spec = (XrtManagedFrameSpec){SIZE_MAX, 0, 1, NULL};
    CHECK(xrt_managed_frame_enter_layout(root, &spec, &frame) == XRT_MANAGED_OVERFLOW);
    spec = (XrtManagedFrameSpec){1, SIZE_MAX, 1, NULL};
    CHECK(xrt_managed_frame_enter_layout(root, &spec, &frame) == XRT_MANAGED_OVERFLOW);
    CHECK(!frame);
    same_storage(baseline, xrt_billing_owner_stats(fixture.billing));
    OK(xrt_managed_root_destroy(&root));
    fixture_finish(&fixture);
}
static void run_inline_jump(Fixture *fixture, XrtManagedRoot *root) {
    jmp_buf target;
    if (setjmp(target) == 0) {
        size_t offsets[] = {offsetof(InlineHomes, first), offsetof(InlineHomes, second)};
        XrtManagedFrameSpec spec = {2, sizeof(InlineHomes), alignof(InlineHomes), offsets};
        XrtManagedFrame *frame = NULL;
        OK(xrt_managed_frame_enter_layout(root, &spec, &frame));
        Payload *parent = prepare_value(fixture, frame, 0, 50);
        OK(xrt_managed_slot_commit(frame, 0));
        XrtManagedFrame *callee = NULL;
        OK(xrt_managed_frame_enter_layout(root, &spec, &callee));
        Payload *pending = prepare_value(fixture, callee, 0, 51);
        CHECK(xrt_managed_retain(&parent->header));
        pending->child = &parent->header;
        longjmp(target, 1);
    }
    OK(xrt_managed_root_drain(root));
}
static void inline_payload_jump(void) {
    Fixture fixture;
    fixture_init(&fixture);
    XrtManagedRoot *root = NULL;
    OK(xrt_managed_root_new(fixture.billing, &root));
    XrtBillingStats baseline = xrt_billing_owner_stats(fixture.billing);
    run_inline_jump(&fixture, root);
    CHECK(fixture.audit.count == 2 && fixture.audit.events[0] == 51 && fixture.audit.events[1] == 50);
    same_storage(baseline, xrt_billing_owner_stats(fixture.billing));
    OK(xrt_managed_root_destroy(&root));
    fixture_finish(&fixture);
    puts("inline frame longjmp: stable pending aborted before real aggregate owners PASS");
}

typedef struct ExecutionValues {
    Fixture *fixture;
    XrtLocationOwner *locations;
    XrtValueRoot *owner;
    XrtManagedRoot *outer_root;
    bool fail, nested;
    size_t after;
} ExecutionValues;

static void fixed_require(XrtManagedStatus status) {
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, 1);
}

static void fixed_execution_root(void *context) {
    ExecutionValues *call = context;
    XrtManagedRoot *root = xrt_native_managed_root(1);
    CHECK(root && root == xrt_native_managed_root(1) && call->owner->managed);
    CHECK(root != call->outer_root);
    XrtManagedFrame *parent = NULL, *child = NULL;
    fixed_require(xrt_managed_frame_enter(root, 3, &parent));
    XrtManagedHeader *value = make_value(call->fixture, 70);
    OK(xrt_managed_slot_adopt(parent, 0, &value)); CHECK(!value);
    xrt_native_string_create(parent, 1, "fixed\0text", 10, 1);
    XrtManagedHeader *header = NULL;
    OK(xrt_managed_slot_borrow(parent, 1, &header));
    const XrtStringBacking *text = (const XrtStringBacking *)header;
    CHECK(text->length == 10 && text->runes == 10 && !memcmp(text->bytes, "fixed\0text", 10));
    xrt_native_string_concat_create(parent, 2, header, header, 1);
    XrtManagedHeader *concatenated = NULL;
    OK(xrt_managed_slot_borrow(parent, 2, &concatenated));
    const XrtStringBacking *joined = (const XrtStringBacking *)concatenated;
    CHECK(joined->length == 20 && joined->runes == 20 && !memcmp(joined->bytes, text->bytes, 10));
    CHECK(!memcmp(joined->bytes + 10, text->bytes, 10) && text->length == 10);
    if (call->nested) {
        XrtValueRoot inner_owner = {0}; XrtExecutionFailure inner_failure = {0};
        ExecutionValues inner = {.fixture=call->fixture, .locations=call->locations,
            .owner=&inner_owner, .outer_root=root};
        CHECK(xrt_run_embedded_owned(call->locations, &inner_failure, fixed_execution_root,
            &inner, &inner_owner) == XRT_EXECUTION_COMPLETED);
        CHECK(inner.after == 1 && xrt_value_root_empty(&inner_owner));
        CHECK(xrt_native_managed_root(1) == root);
    }
    if (call->fail) {
        fixed_require(xrt_managed_frame_enter(root, 1, &child));
        Payload *pending = prepare_value(call->fixture, child, 0, 71);
        XrtManagedHeader *parent_owner = NULL;
        OK(xrt_managed_slot_borrow(parent, 0, &parent_owner));
        CHECK(xrt_managed_retain(parent_owner)); pending->child = parent_owner;
        static const unsigned char invalid[] = {0xff};
        xrt_native_string_create(parent, 1, (const char *)invalid, sizeof(invalid), 1);
        CHECK(false);
    }
    ++call->after;
}

static void explicit_managed_failure(void *context) {
    XrtManagedStatus *status = context;
    xrt_native_managed_failure(*status, 1);
}

static void fixed_scalar_root(void *context) {
    ExecutionValues *call = context;
    CHECK(!call->owner->managed && !call->owner->draining);
    ++call->after;
}

static void cold_owner_reentry(void *context) {
    ExecutionValues *call = context;
    XrtExecutionFailure failure, before;
    memset(&failure, 0xa5, sizeof(failure)); before = failure;
    CHECK(xrt_native_values_prepared(call->owner));
    CHECK(xrt_run_embedded_owned(call->locations, &failure, fixed_scalar_root,
        call, call->owner) == XRT_EXECUTION_INVALID);
    CHECK(!memcmp(&failure, &before, sizeof(failure)) && !call->after);
    ++call->after;
}

static void fixed_cold_setup(void) {
    XrtValueRoot owner = {0}; XrtBillingOwner *billing = NULL, *again = NULL;
    BAD(xrt_native_values_prepare(NULL, &billing));
    OK(xrt_native_values_prepare(&owner, &billing));
    CHECK(billing && owner.managed && !xrt_value_root_empty(&owner) && xrt_native_values_prepared(&owner));
    XrtBillingStats setup = xrt_billing_owner_stats(billing);
    OK(xrt_native_values_prepare(&owner, &again)); CHECK(again == billing);
    same_storage(setup, xrt_billing_owner_stats(billing));
    BAD(xrt_native_values_prepare(&owner, &again)); CHECK(again == billing);
    CHECK(xrt_billing_owner_retain(billing));
    XrtSourceLocation location = {"cold-state.xr", 13, 1, 1, 0, 0};
    XrtLocationOwner *locations = NULL;
    CHECK(xrt_locations_create(&location, 1, &locations) == XRT_LOCATION_OK);
    XrtExecutionFailure failure = {0};
    ExecutionValues call = {.locations=locations, .owner=&owner};
    CHECK(xrt_run_embedded_owned(locations, &failure, cold_owner_reentry, &call, &owner) == XRT_EXECUTION_COMPLETED);
    CHECK(call.after == 1 && xrt_value_root_empty(&owner));
    CHECK(xrt_billing_owner_stats(billing).blocks == 1);
    xrt_billing_owner_drop(billing); billing = NULL;
    OK(xrt_native_values_prepare(&owner, &billing));
    CHECK(xrt_billing_owner_retain(billing));
    xrt_billing_owner_close(billing);
    Fixture fixture; fixture_init(&fixture); call.fixture = &fixture; call.after = 0;
    CHECK(xrt_run_embedded_owned(locations, &failure, fixed_execution_root, &call, &owner) == XRT_EXECUTION_FAILURE);
    CHECK(!call.after && xrt_value_root_empty(&owner) && failure.detail.value.status == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xrt_billing_owner_stats(billing).blocks == 1);
    xrt_billing_owner_drop(billing); fixture_finish(&fixture);
    billing = NULL;
    OK(xrt_native_values_prepare(&owner, &billing)); CHECK(xrt_billing_owner_retain(billing));
    const XrtBackingTypes *types = NULL, *ready = NULL;
    OK(xrt_native_values_backings(&owner, &types));
    XrtBillingStats initialized = xrt_billing_owner_stats(billing);
    OK(xrt_native_values_backings(&owner, &ready)); CHECK(ready == types);
    same_storage(initialized, xrt_billing_owner_stats(billing));
    BAD(xrt_native_values_backings(&owner, &ready)); CHECK(ready == types);
    owner.draining = true;
    ready = NULL;
    BAD(xrt_native_values_backings(&owner, &ready)); CHECK(!ready);
    owner.draining = false;
    xrt_value_root_drain(&owner);
    CHECK(xrt_value_root_empty(&owner) && xrt_billing_owner_stats(billing).blocks == 1);
    xrt_billing_owner_drop(billing); xrt_locations_release(locations);
    puts("cold execution billing: real setup owner, failed setup drain and active ancestor rejection PASS");
}

static void fixed_execution_boundary(void) {
    Fixture fixture; fixture_init(&fixture);
    XrtSourceLocation location = {"fixed-values.xr", 15, 7, 4, 7, 5};
    XrtLocationOwner *locations = NULL;
    CHECK(xrt_locations_create(&location, 1, &locations) == XRT_LOCATION_OK);
    XrtValueRoot owner = {0}; XrtExecutionFailure failure = {0};
    ExecutionValues call = {.fixture=&fixture, .locations=locations, .owner=&owner};
    XrtBillingStats baseline = xrt_billing_owner_stats(fixture.billing);
    CHECK(xrt_run_embedded_owned(locations, &failure, fixed_execution_root, &call, &owner) == XRT_EXECUTION_COMPLETED);
    CHECK(call.after == 1 && xrt_value_root_empty(&owner));
    CHECK(fixture.audit.count == 1 && fixture.audit.events[0] == 70);
    same_storage(baseline, xrt_billing_owner_stats(fixture.billing));
    call.after = 0; call.fail = true;
    CHECK(xrt_run_embedded_owned(locations, &failure, fixed_execution_root, &call, &owner) == XRT_EXECUTION_FAILURE);
    CHECK(!call.after && xrt_value_root_empty(&owner));
    CHECK(failure.kind == XRT_FAILURE_VALUE && failure.detail.value.status == XR_XIR_VALUE_BAD_UTF8);
    CHECK(failure.detail.value.source_id == 1 && fixture.audit.count == 3);
    CHECK(fixture.audit.events[1] == 71 && fixture.audit.events[2] == 70);
    same_storage(baseline, xrt_billing_owner_stats(fixture.billing));
    call.fail = false; call.nested = true;
    CHECK(xrt_run_embedded_owned(locations, &failure, fixed_execution_root, &call, &owner) == XRT_EXECUTION_COMPLETED);
    CHECK(call.after == 1 && xrt_value_root_empty(&owner));
    CHECK(fixture.audit.count == 5);
    same_storage(baseline, xrt_billing_owner_stats(fixture.billing));
    XrtManagedStatus status = XRT_MANAGED_OOM;
    CHECK(xrt_run_embedded_owned(locations, &failure, explicit_managed_failure, &status, &owner) == XRT_EXECUTION_FAILURE);
    CHECK(failure.detail.value.status == XR_XIR_VALUE_OOM && failure.detail.value.source_id == 1);
    CHECK(xrt_value_root_empty(&owner));
    owner.byte_limit = 1;
    call.after = 0;
    CHECK(xrt_run_embedded_owned(locations, &failure, fixed_execution_root, &call, &owner) == XRT_EXECUTION_FAILURE);
    CHECK(!call.after && xrt_value_root_empty(&owner) && owner.byte_limit == 1);
    CHECK(failure.detail.value.status == XR_XIR_VALUE_MEMORY_LIMIT);
    call.nested = false;
    for (uint64_t limit = 64; limit <= 4096; limit *= 2) {
        owner.byte_limit = limit; call.after = 0;
        XrtExecutionStatus result = xrt_run_embedded_owned(locations, &failure, fixed_execution_root, &call, &owner);
        CHECK(xrt_value_root_empty(&owner) && owner.byte_limit == limit);
        if (result == XRT_EXECUTION_FAILURE) {
            CHECK(!call.after && failure.kind == XRT_FAILURE_VALUE);
            CHECK(failure.detail.value.status == XR_XIR_VALUE_MEMORY_LIMIT);
        } else CHECK(result == XRT_EXECUTION_COMPLETED && call.after == 1);
        same_storage(baseline, xrt_billing_owner_stats(fixture.billing));
    }
    owner.byte_limit = 1; call.after = 0;
    CHECK(xrt_run_embedded_owned(locations, &failure, fixed_scalar_root, &call, &owner) == XRT_EXECUTION_COMPLETED);
    CHECK(call.after == 1 && xrt_value_root_empty(&owner));
    xrt_locations_release(locations); fixture_finish(&fixture);
    puts("fixed execution: normal, nested and failing roots drain actual stable owners PASS");
}
#include "managed_frame_locations_cases.h"
#include "managed_frame_forwarding_cases.h"
#include "managed_frame_local_cases.h"
/* A payload above the fundamental alignment is placed from the actual frame
 * address rather than a static offset, while fundamental alignments keep their
 * exact static placement. The shared limit refuses anything larger. */
static void explicit_payload_alignment(void) {
    Fixture fixture;
    fixture_init(&fixture);
    XrtBillingStats baseline = xrt_billing_owner_stats(fixture.billing);
    XrtManagedRoot *root = NULL;
    OK(xrt_managed_root_new(fixture.billing, &root));
    XrtManagedFrameSpec spec = {1, 128, 128, NULL};
    XrtManagedFrame *frame = NULL;
    void *payload = NULL;
    OK(xrt_managed_frame_enter_payload(root, &spec, &frame, &payload));
    CHECK(payload && ((uintptr_t)payload % 128) == 0);
    memset(payload, 0xA5, 128);
    OK(xrt_managed_frame_leave(root, frame));
    frame = NULL;
    spec = (XrtManagedFrameSpec){1, 16, 16, NULL};
    spec = (XrtManagedFrameSpec){1, XR_ALIGNMENT_LIMIT * 2, XR_ALIGNMENT_LIMIT * 2, NULL};
    CHECK(xrt_managed_frame_enter_layout(root, &spec, &frame) == XRT_MANAGED_ARGUMENT && !frame);
    spec = (XrtManagedFrameSpec){1, 48, 96, NULL};
    CHECK(xrt_managed_frame_enter_layout(root, &spec, &frame) == XRT_MANAGED_ARGUMENT && !frame);
    OK(xrt_managed_root_destroy(&root));
    same_storage(baseline, xrt_billing_owner_stats(fixture.billing));
    fixture_finish(&fixture);
    puts("explicit frame payload alignment PASS");
}

int main(void) {
    aliases_and_return();
    independent_taken_owner();
    replacement_and_extent();
    committed_owner_replacement();
    partial_children();
    nonlocal_cleanup();
    metadata_failures();
    scalar_and_closed();
    inline_payload_homes();
    combined_frame_boundary();
    canonical_frame_locations();
    managed_frame_forwarding();
    managed_frame_local_transactions();
    inline_payload_rejection();
    inline_payload_jump();
    explicit_payload_alignment();
    fixed_execution_boundary();
    fixed_cold_setup();
    puts("Stable managed Root/Frame: real aliases, publication, extent rollback and longjmp drain PASS");
    return 0;
}
