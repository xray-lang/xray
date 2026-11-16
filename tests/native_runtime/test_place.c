/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_place.c - Stable ownership publication, guarded fields and parallel edges
 */
#include "runtime/native/xnative_place.h"
#include "base/xmalloc.h"
#include <limits.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); abort(); } } while (0)
#define OK(c) CHECK((c) == XRT_MANAGED_OK)
#define BAD(c) CHECK((c) == XRT_MANAGED_ARGUMENT)
typedef struct Payload { XrtManagedHeader header; uint64_t value; } Payload;
typedef struct Audit {
    _Atomic(size_t) bytes, blocks, allocations, owner_refs, code_refs, owner_final, code_final, retired;
    bool watch;
    XrtManagedHeader **observed_destination, **observed_source, **observed_other;
    XrtManagedHeader *expected, *expected_other, *expected_source;
    XrtPlace reenter;
    const uint8_t *observed_bytes, *expected_bytes;
    const size_t *observed_publications;
    size_t byte_count;
} Audit;
typedef struct Fixture {
    Audit audit;
    XrtBillingOwner *billing;
    XrtRuntimeTypePlan *plan;
    XrtManagedOps *ops;
    XrtManagedRoot *root;
    XrtManagedFrame *frame;
} Fixture;
static bool owner_retain(void *context) { atomic_fetch_add(&((Audit *)context)->owner_refs, 1); return true; }
static void owner_drop(void *context) {
    Audit *audit = context;
    CHECK(atomic_load(&audit->owner_refs));
    if (atomic_fetch_sub(&audit->owner_refs, 1) == 1) {
        CHECK(!atomic_load(&audit->bytes) && !atomic_load(&audit->blocks));
        atomic_fetch_add(&audit->owner_final, 1);
    }
}
static bool code_retain(void *context) { atomic_fetch_add(&((Audit *)context)->code_refs, 1); return true; }
static void code_drop(void *context) {
    Audit *audit = context;
    CHECK(atomic_load(&audit->code_refs));
    if (atomic_fetch_sub(&audit->code_refs, 1) == 1) atomic_fetch_add(&audit->code_final, 1);
}
static void *allocate(void *context, size_t bytes) {
    Audit *audit = context;
    atomic_fetch_add(&audit->allocations, 1);
    void *memory = xr_malloc(bytes);
    if (memory) { atomic_fetch_add(&audit->bytes, bytes); atomic_fetch_add(&audit->blocks, 1); }
    return memory;
}
static void deallocate(void *context, void *memory, size_t bytes) {
    Audit *audit = context;
    CHECK(atomic_fetch_sub(&audit->bytes, bytes) >= bytes && atomic_fetch_sub(&audit->blocks, 1));
    xr_free(memory);
}
static void payload_drop(XrtManagedHeader *value, void *context) {
    Audit *audit = context;
    CHECK(((Payload *)value)->value);
    atomic_fetch_add(&audit->retired, 1);
    if (audit->watch) {
        CHECK(*audit->observed_destination == audit->expected);
        CHECK(!audit->observed_other || *audit->observed_other == audit->expected_other);
        CHECK(!audit->observed_source || *audit->observed_source == audit->expected_source);
        for (size_t i = 0; i < audit->byte_count; ++i)
            CHECK(audit->observed_bytes[i] == audit->expected_bytes[i]);
        if (audit->observed_publications) CHECK(*audit->observed_publications == 1);
        const XrtManagedHeader *borrowed = NULL;
        BAD(xrt_place_borrow(audit->reenter, &borrowed));
        CHECK(!borrowed);
    }
}
static void fixture_init(Fixture *fixture) {
    *fixture = (Fixture){0};
    atomic_init(&fixture->audit.owner_refs, 1); atomic_init(&fixture->audit.code_refs, 1);
    XrtManagedAllocator provider = {&fixture->audit, allocate, deallocate,
        {&fixture->audit, owner_retain, owner_drop}};
    OK(xrt_billing_owner_new(&provider, SIZE_MAX, &fixture->billing));
    XrtRuntimeTypeSpec spec = {.fixed_size=sizeof(Payload), .alignment=alignof(Payload),
        .capacity_offset=SIZE_MAX, .drop_payload=payload_drop, .code={&fixture->audit, code_retain, code_drop}};
    OK(xrt_runtime_type_plan_new(fixture->billing, &spec, &fixture->plan));
    OK(xrt_managed_ops_bind(fixture->billing, fixture->plan, &fixture->ops));
    OK(xrt_managed_root_new(fixture->billing, &fixture->root));
    OK(xrt_managed_frame_enter(fixture->root, 16, &fixture->frame));
}
static XrtPlace slot(Fixture *fixture, size_t index) { return (XrtPlace){fixture->frame, index, NULL, XRT_PLACE_NONNULL}; }
static XrtManagedHeader **owner_field(Fixture *fixture, size_t index) {
    XrtManagedSlotLocation location;
    OK(xrt_managed_slot_location(fixture->frame, index, &location));
    return location.owner;
}
static XrtManagedHeader *make(Fixture *fixture, size_t index, uint64_t value) {
    XrtManagedHeader *payload = NULL;
    OK(xrt_managed_slot_prepare(fixture->frame, index, fixture->ops, sizeof(Payload), &payload));
    ((Payload *)payload)->value = value;
    OK(xrt_managed_slot_commit(fixture->frame, index));
    return payload;
}
static void fixture_retire(Fixture *fixture) {
    OK(xrt_managed_root_destroy(&fixture->root));
    xrt_managed_ops_drop(fixture->ops); xrt_runtime_type_plan_drop(fixture->plan);
    xrt_billing_owner_drop(fixture->billing); owner_drop(&fixture->audit); code_drop(&fixture->audit);
}
static void fixture_done(Fixture *fixture) {
    fixture_retire(fixture);
    CHECK(atomic_load(&fixture->audit.owner_final) == 1 && atomic_load(&fixture->audit.code_final) == 1);
}
typedef struct Guard {
    bool readable, writable, reentry;
    size_t calls;
    XrtPlace from, to;
} Guard;
static XrtManagedStatus preflight(void *context, XrtPlaceAction action) {
    Guard *guard = context;
    ++guard->calls;
    if (guard->reentry) BAD(xrt_place_move_replace(guard->to, guard->from, NULL));
    bool write = action == XRT_PLACE_REPLACE || action == XRT_PLACE_MOVE_FROM;
    return (write ? guard->writable : guard->readable) ? XRT_MANAGED_OK : XRT_MANAGED_CLOSED;
}
static void root_transfers(void) {
    Fixture fixture;
    fixture_init(&fixture);
    XrtManagedHeader *one = make(&fixture, 0, 1), *two = make(&fixture, 1, 2);
    const XrtManagedHeader *borrowed = NULL;
    OK(xrt_place_borrow(slot(&fixture, 0), &borrowed)); CHECK(borrowed == one);
    OK(xrt_place_copy_replace(slot(&fixture, 2), slot(&fixture, 0), NULL));
    CHECK(xr_signed_rc_read(&one->references) == 2);
    OK(xrt_place_move_replace(slot(&fixture, 2), slot(&fixture, 0), NULL));
    CHECK(xr_signed_rc_read(&one->references) == 1);
    BAD(xrt_place_borrow(slot(&fixture, 0), &borrowed)); CHECK(borrowed == one);
    atomic_store(&one->references, INT32_MAX);
    OK(xrt_place_copy_replace(slot(&fixture, 2), slot(&fixture, 2), NULL));
    OK(xrt_place_move_replace(slot(&fixture, 2), slot(&fixture, 2), NULL));
    CHECK(xr_signed_rc_read(&one->references) == INT32_MAX);
    CHECK(xrt_place_copy_replace(slot(&fixture, 1), slot(&fixture, 2), NULL) == XRT_MANAGED_REFCOUNT);
    OK(xrt_place_borrow(slot(&fixture, 1), &borrowed)); CHECK(borrowed == two);
    atomic_store(&one->references, 1);
    XrtManagedHeader *pending = NULL;
    OK(xrt_managed_slot_prepare(fixture.frame, 1, fixture.ops, sizeof(Payload), &pending));
    ((Payload *)pending)->value = 3;
    BAD(xrt_place_move_replace(slot(&fixture, 1), slot(&fixture, 2), NULL));
    BAD(xrt_place_move_replace(slot(&fixture, 2), slot(&fixture, 1), NULL));
    BAD(xrt_place_borrow(slot(&fixture, 1), &borrowed));
    OK(xrt_managed_slot_abort(fixture.frame, 1));
    XrtPlace nullable = slot(&fixture, 0); nullable.nulls = XRT_PLACE_NULLABLE;
    BAD(xrt_place_copy_replace(slot(&fixture, 1), nullable, NULL));
    XrtPlace nullable_to = slot(&fixture, 1); nullable_to.nulls = XRT_PLACE_NULLABLE;
    OK(xrt_place_copy_replace(nullable_to, nullable, NULL));
    borrowed = one; OK(xrt_place_borrow(nullable_to, &borrowed)); CHECK(!borrowed);
    BAD(xrt_place_move_replace(slot(&fixture, 20), slot(&fixture, 2), NULL));
    size_t allocations = atomic_load(&fixture.audit.allocations);
    xrt_billing_owner_fail_after(fixture.billing, 0); xrt_billing_owner_close(fixture.billing);
    OK(xrt_place_copy_replace(slot(&fixture, 3), slot(&fixture, 2), NULL));
    OK(xrt_place_move_replace(slot(&fixture, 4), slot(&fixture, 3), NULL));
    CHECK(atomic_load(&fixture.audit.allocations) == allocations);
    fixture_done(&fixture);
}
static void external_transfers(void) {
    Fixture fixture;
    fixture_init(&fixture);
    XrtManagedHeader *one = make(&fixture, 0, 1), *two = make(&fixture, 1, 2);
    XrtManagedHeader *outside = NULL; bool busy = false;
    Guard guard = {.readable=true, .writable=true};
    XrtExternalOwner external = {&outside, &busy, preflight, &guard, NULL};
    XrtPlace field = {.external=&external, .nulls=XRT_PLACE_NONNULL};
    guard.from = slot(&fixture, 0); guard.to = field; guard.reentry = true;
    OK(xrt_place_move_replace(field, slot(&fixture, 0), NULL)); CHECK(outside == one && !busy);
    guard.writable = false;
    CHECK(xrt_place_move_replace(field, slot(&fixture, 1), NULL) == XRT_MANAGED_CLOSED);
    CHECK(outside == one && xr_signed_rc_read(&two->references) == 1 && !busy);
    guard.writable = true; guard.readable = false;
    const XrtManagedHeader *borrowed = two;
    CHECK(xrt_place_borrow(field, &borrowed) == XRT_MANAGED_CLOSED && borrowed == two && !busy);
    guard.readable = true;
    fixture.audit.observed_destination = &outside; fixture.audit.expected = two;
    XrtManagedSlotLocation source;
    OK(xrt_managed_slot_location(fixture.frame, 1, &source));
    fixture.audit.observed_source = source.owner; fixture.audit.reenter = field; fixture.audit.watch = true;
    OK(xrt_place_move_replace(field, slot(&fixture, 1), NULL));
    fixture.audit.watch = false;
    CHECK(outside == two && !*source.owner && !busy);
    OK(xrt_place_copy_replace(slot(&fixture, 2), field, NULL));
    OK(xrt_place_move_replace(slot(&fixture, 2), field, NULL)); CHECK(!outside && xr_signed_rc_read(&two->references) == 1);
    BAD(xrt_place_move_replace(field, field, NULL));
    field.nulls = XRT_PLACE_NULLABLE;
    OK(xrt_place_move_replace(field, field, NULL));
    guard.readable = false;
    CHECK(xrt_place_borrow(field, &borrowed) == XRT_MANAGED_CLOSED);
    guard.readable = true;
    OK(xrt_place_borrow(field, &borrowed)); CHECK(!borrowed);
    bool other_busy = false;
    XrtExternalOwner alias = external; alias.busy = &other_busy;
    XrtPlace inconsistent = field; inconsistent.external = &alias;
    BAD(xrt_place_move_replace(inconsistent, field, NULL)); CHECK(!busy && !other_busy);
    busy = true; size_t calls = guard.calls;
    BAD(xrt_place_copy_replace(field, slot(&fixture, 2), NULL)); CHECK(guard.calls == calls && busy);
    busy = false;
    fixture_done(&fixture);
}
static void edges_and_domains(void) {
    Fixture fixture, peer;
    fixture_init(&fixture); fixture_init(&peer);
    XrtManagedHeader *one = make(&fixture, 0, 1), *two = make(&fixture, 1, 2), *three = make(&peer, 0, 3);
    XrtPlaceEdge swap[] = {{slot(&fixture, 1), slot(&fixture, 0), slot(&fixture, 8), XRT_PLACE_TRANSFER_MOVE},
        {slot(&fixture, 0), slot(&fixture, 1), slot(&fixture, 9), XRT_PLACE_TRANSFER_MOVE}};
    size_t allocations = atomic_load(&fixture.audit.allocations);
    xrt_billing_owner_fail_after(fixture.billing, 0);
    OK(xrt_place_edge_transfer(swap, 2, NULL));
    const XrtManagedHeader *borrowed = NULL;
    OK(xrt_place_borrow(slot(&fixture, 0), &borrowed)); CHECK(borrowed == two);
    OK(xrt_place_borrow(slot(&fixture, 1), &borrowed)); CHECK(borrowed == one);
    CHECK(atomic_load(&fixture.audit.allocations) == allocations);
    XrtPlaceEdge duplicate[] = {{slot(&fixture, 2), slot(&fixture, 0), slot(&fixture, 8), XRT_PLACE_TRANSFER_MOVE},
        {slot(&fixture, 3), slot(&fixture, 0), slot(&fixture, 9), XRT_PLACE_TRANSFER_MOVE}};
    BAD(xrt_place_edge_transfer(duplicate, 2, NULL));
    duplicate[1].source = slot(&fixture, 1); duplicate[1].destination = slot(&fixture, 2);
    BAD(xrt_place_edge_transfer(duplicate, 2, NULL));
    BAD(xrt_place_edge_transfer(NULL, 1, NULL));
    OK(xrt_place_edge_transfer(NULL, 0, NULL));
    OK(xrt_place_move_replace(slot(&fixture, 3), slot(&peer, 0), NULL));
    OK(xrt_place_borrow(slot(&fixture, 3), &borrowed)); CHECK(borrowed == three);
    // An invalid later location must release only domains acquired by this call.
    XrtPlaceEdge invalid[] = {swap[0], {slot(&peer, 30), slot(&fixture, 1), slot(&fixture, 10), XRT_PLACE_TRANSFER_MOVE}};
    BAD(xrt_place_edge_transfer(invalid, 2, NULL));
    OK(xrt_place_edge_transfer(swap, 2, NULL));
    fixture_done(&fixture); fixture_done(&peer);
}
static void escaped_lifetime(void) {
    Fixture fixture;
    fixture_init(&fixture);
    make(&fixture, 0, 1);
    XrtManagedHeader *outside = NULL; bool busy = false;
    Guard guard = {.readable=true, .writable=true};
    XrtExternalOwner external = {&outside, &busy, preflight, &guard, NULL};
    XrtPlace field = {.external=&external, .nulls=XRT_PLACE_NONNULL};
    OK(xrt_place_move_replace(field, slot(&fixture, 0), NULL));
    fixture_retire(&fixture);
    CHECK(outside && !atomic_load(&fixture.audit.owner_final) && !atomic_load(&fixture.audit.code_final));
    CHECK(((Payload *)outside)->value == 1);
    xrt_managed_drop(outside); outside = NULL;
    CHECK(atomic_load(&fixture.audit.owner_final) == 1 && atomic_load(&fixture.audit.code_final) == 1);
}
static void edge_guards_and_retirement(void) {
    Fixture fixture;
    fixture_init(&fixture);
    XrtManagedHeader *one = make(&fixture, 0, 1), *two = make(&fixture, 1, 2);
    XrtManagedHeader *three = make(&fixture, 2, 3);
    make(&fixture, 3, 4);
    XrtManagedHeader *outside = NULL; bool busy = false;
    Guard guard = {.readable=true, .writable=true};
    XrtExternalOwner external = {&outside, &busy, preflight, &guard, NULL};
    XrtPlace field = {.external=&external, .nulls=XRT_PLACE_NONNULL};
    OK(xrt_place_move_replace(field, slot(&fixture, 3), NULL));
    XrtPlaceEdge edges[] = {{slot(&fixture, 2), slot(&fixture, 0), slot(&fixture, 8), XRT_PLACE_TRANSFER_MOVE},
        {field, slot(&fixture, 1), slot(&fixture, 9), XRT_PLACE_TRANSFER_MOVE}};
    busy = true;
    BAD(xrt_place_edge_transfer(edges, 2, NULL)); CHECK(busy);
    busy = false;
    guard.writable = false;
    CHECK(xrt_place_edge_transfer(edges, 2, NULL) == XRT_MANAGED_CLOSED);
    const XrtManagedHeader *borrowed = NULL;
    OK(xrt_place_borrow(slot(&fixture, 0), &borrowed)); CHECK(borrowed == one);
    OK(xrt_place_borrow(slot(&fixture, 1), &borrowed)); CHECK(borrowed == two);
    OK(xrt_place_borrow(slot(&fixture, 2), &borrowed)); CHECK(borrowed == three);
    guard.writable = true;
    XrtManagedSlotLocation destination, source;
    OK(xrt_managed_slot_location(fixture.frame, 2, &destination));
    OK(xrt_managed_slot_location(fixture.frame, 0, &source));
    fixture.audit.observed_destination = destination.owner; fixture.audit.expected = one;
    fixture.audit.observed_source = source.owner;
    fixture.audit.observed_other = &outside; fixture.audit.expected_other = two;
    fixture.audit.reenter = field; fixture.audit.watch = true;
    size_t allocations = atomic_load(&fixture.audit.allocations);
    xrt_billing_owner_fail_after(fixture.billing, 0); xrt_billing_owner_close(fixture.billing);
    OK(xrt_place_edge_transfer(edges, 2, NULL));
    fixture.audit.watch = false;
    CHECK(atomic_load(&fixture.audit.retired) == 2 && atomic_load(&fixture.audit.allocations) == allocations);
    CHECK(!busy && outside == two);
    OK(xrt_place_move_replace(slot(&fixture, 4), field, NULL));
    fixture_done(&fixture);
}
typedef struct PublicationBytes {
    uint8_t *destination, *source;
    size_t count, calls;
    uint8_t snapshot[2];
} PublicationBytes;
static void publish_bytes(void *context) {
    PublicationBytes *bytes = context;
    if (bytes->source) for (size_t i = 0; i < bytes->count; ++i) bytes->source[i] = 0;
    for (size_t i = 0; i < bytes->count; ++i) bytes->destination[i] = bytes->snapshot[i];
    ++bytes->calls;
}
static void publication_boundaries(void) {
    Fixture fixture;
    fixture_init(&fixture);
    XrtManagedHeader *one = make(&fixture, 0, 1);
    make(&fixture, 1, 2);
    uint8_t tag = 3, source_tag = 11, expected = 17;
    PublicationBytes bytes = {&tag, NULL, 1, 0, {17, 0}};
    XrtPlacePublication publication = {publish_bytes, &bytes};
    XrtManagedSlotLocation location;
    OK(xrt_managed_slot_location(fixture.frame, 1, &location));
    fixture.audit.watch = true;
    fixture.audit.observed_destination = location.owner;
    fixture.audit.expected = one;
    fixture.audit.observed_bytes = &tag;
    fixture.audit.expected_bytes = &expected;
    fixture.audit.byte_count = 1;
    fixture.audit.observed_publications = &bytes.calls;
    fixture.audit.reenter = slot(&fixture, 1);
    OK(xrt_place_copy_replace(slot(&fixture, 1), slot(&fixture, 0), &publication));
    CHECK(tag == 17 && bytes.calls == 1 && xr_signed_rc_read(&one->references) == 2);
    fixture.audit.watch = false;
    bytes.snapshot[0] = 23;
    atomic_store(&one->references, INT32_MAX);
    CHECK(xrt_place_copy_replace(slot(&fixture, 1), slot(&fixture, 0), &publication) == XRT_MANAGED_REFCOUNT);
    CHECK(tag == 17 && bytes.calls == 1);
    atomic_store(&one->references, 2);
    bytes.source = &source_tag;
    OK(xrt_place_move_replace(slot(&fixture, 1), slot(&fixture, 0), &publication));
    CHECK(tag == 23 && !source_tag && bytes.calls == 2 && xr_signed_rc_read(&one->references) == 1);
    bytes.source = NULL;
    bytes.snapshot[0] = 29;
    OK(xrt_place_copy_replace(slot(&fixture, 1), slot(&fixture, 1), &publication));
    CHECK(tag == 29 && bytes.calls == 3);
    bytes.snapshot[0] = 31;
    OK(xrt_place_edge_transfer(NULL, 0, &publication));
    CHECK(tag == 31 && bytes.calls == 4);
    XrtPlacePublication invalid = {NULL, &bytes};
    BAD(xrt_place_edge_transfer(NULL, 0, &invalid));
    BAD(xrt_place_move_replace(slot(&fixture, 0), slot(&fixture, 1), &invalid));
    CHECK(tag == 31 && bytes.calls == 4);
    fixture_done(&fixture);
}
static void edge_publication(void) {
    Fixture fixture;
    fixture_init(&fixture);
    XrtManagedHeader *one = make(&fixture, 0, 1), *two = make(&fixture, 1, 2);
    make(&fixture, 2, 3); make(&fixture, 3, 4);
    XrtPlaceEdge edges[] = {{slot(&fixture, 2), slot(&fixture, 0), slot(&fixture, 8), XRT_PLACE_TRANSFER_MOVE},
        {slot(&fixture, 3), slot(&fixture, 1), slot(&fixture, 9), XRT_PLACE_TRANSFER_MOVE}};
    uint8_t tags[] = {30, 40}, source_tags[] = {11, 22}, expected[] = {11, 22};
    PublicationBytes bytes = {tags, source_tags, 2, 0, {11, 22}};
    XrtPlacePublication publication = {publish_bytes, &bytes};
    XrtManagedHeader *pending = NULL;
    OK(xrt_managed_slot_prepare(fixture.frame, 3, fixture.ops, sizeof(Payload), &pending));
    ((Payload *)pending)->value = 5;
    BAD(xrt_place_edge_transfer(edges, 2, &publication));
    CHECK(tags[0] == 30 && tags[1] == 40 && !bytes.calls && source_tags[0] == 11 && source_tags[1] == 22);
    OK(xrt_managed_slot_abort(fixture.frame, 3));
    XrtManagedSlotLocation destination, other, source;
    OK(xrt_managed_slot_location(fixture.frame, 2, &destination));
    OK(xrt_managed_slot_location(fixture.frame, 3, &other));
    OK(xrt_managed_slot_location(fixture.frame, 0, &source));
    fixture.audit.watch = true;
    fixture.audit.observed_destination = destination.owner;
    fixture.audit.observed_other = other.owner;
    fixture.audit.observed_source = source.owner;
    fixture.audit.expected = one; fixture.audit.expected_other = two;
    fixture.audit.observed_bytes = tags; fixture.audit.expected_bytes = expected;
    fixture.audit.byte_count = 2; fixture.audit.observed_publications = &bytes.calls;
    fixture.audit.reenter = slot(&fixture, 2);
    OK(xrt_place_edge_transfer(edges, 2, &publication));
    CHECK(bytes.calls == 1 && tags[0] == 11 && tags[1] == 22 && !source_tags[0] && !source_tags[1]);
    fixture.audit.watch = false;
    fixture_done(&fixture);
    puts("payload publication: tags and scalar bytes precede every replaced-owner callback PASS");
}
static void duplicate_copy_publication(void) {
    Fixture fixture;
    fixture_init(&fixture);
    XrtManagedHeader *one = make(&fixture, 0, 1);
    make(&fixture, 1, 2); make(&fixture, 2, 3);
    XrtPlaceEdge edges[] = {
        {slot(&fixture, 1), slot(&fixture, 0), slot(&fixture, 8), XRT_PLACE_TRANSFER_COPY},
        {slot(&fixture, 2), slot(&fixture, 0), slot(&fixture, 9), XRT_PLACE_TRANSFER_COPY}};
    uint8_t tags[] = {20, 30}, expected[] = {11, 11};
    PublicationBytes bytes = {tags, NULL, 2, 0, {11, 11}};
    XrtPlacePublication publication = {publish_bytes, &bytes};
    fixture.audit.observed_destination = owner_field(&fixture, 1); fixture.audit.expected = one;
    fixture.audit.observed_other = owner_field(&fixture, 2); fixture.audit.expected_other = one;
    fixture.audit.observed_source = owner_field(&fixture, 0); fixture.audit.expected_source = one;
    fixture.audit.observed_bytes = tags; fixture.audit.expected_bytes = expected;
    fixture.audit.byte_count = 2; fixture.audit.observed_publications = &bytes.calls;
    fixture.audit.reenter = slot(&fixture, 0); fixture.audit.watch = true;
    size_t allocations = atomic_load(&fixture.audit.allocations);
    xrt_billing_owner_fail_after(fixture.billing, 0); xrt_billing_owner_close(fixture.billing);
    OK(xrt_place_edge_transfer(edges, 2, &publication));
    fixture.audit.watch = false;
    CHECK(*owner_field(&fixture, 0) == one && *owner_field(&fixture, 1) == one && *owner_field(&fixture, 2) == one);
    CHECK(!*owner_field(&fixture, 8) && !*owner_field(&fixture, 9));
    CHECK(xr_signed_rc_read(&one->references) == 3 && bytes.calls == 1);
    CHECK(atomic_load(&fixture.audit.retired) == 2 && atomic_load(&fixture.audit.allocations) == allocations);
    XrtPlaceEdge self = {slot(&fixture, 0), slot(&fixture, 0), slot(&fixture, 8), XRT_PLACE_TRANSFER_COPY};
    OK(xrt_place_edge_transfer(&self, 1, NULL));
    CHECK(*owner_field(&fixture, 0) == one && xr_signed_rc_read(&one->references) == 3 && !*owner_field(&fixture, 8));
    edges[0].transfer = (XrtPlaceTransfer)0;
    BAD(xrt_place_edge_transfer(edges, 2, &publication));
    CHECK(bytes.calls == 1 && xr_signed_rc_read(&one->references) == 3);
    edges[0].transfer = XRT_PLACE_TRANSFER_COPY; edges[1].destination = edges[0].destination;
    BAD(xrt_place_edge_transfer(edges, 2, &publication));
    CHECK(bytes.calls == 1 && !*owner_field(&fixture, 8) && !*owner_field(&fixture, 9));
    fixture_done(&fixture);
    puts("duplicate COPY preserves the live source and publishes all fields before retirement");
}
static void copy_overflow_rollback(void) {
    Fixture fixture;
    fixture_init(&fixture);
    XrtManagedHeader *one = make(&fixture, 0, 1), *two = make(&fixture, 1, 2);
    XrtManagedHeader *three = make(&fixture, 2, 3), *four = make(&fixture, 3, 4);
    XrtManagedHeader *five = make(&fixture, 4, 5);
    /* A preceding MOVE must remain untouched when a later COPY retain fails. */
    XrtPlaceEdge edges[] = {
        {slot(&fixture, 4), slot(&fixture, 3), slot(&fixture, 10), XRT_PLACE_TRANSFER_MOVE},
        {slot(&fixture, 1), slot(&fixture, 0), slot(&fixture, 8), XRT_PLACE_TRANSFER_COPY},
        {slot(&fixture, 2), slot(&fixture, 0), slot(&fixture, 9), XRT_PLACE_TRANSFER_COPY}};
    uint8_t tag = 7;
    PublicationBytes bytes = {&tag, NULL, 1, 0, {19, 0}};
    XrtPlacePublication publication = {publish_bytes, &bytes};
    size_t allocations = atomic_load(&fixture.audit.allocations);
    XrtBillingStats before = xrt_billing_owner_stats(fixture.billing);
    xrt_billing_owner_fail_after(fixture.billing, 0); xrt_billing_owner_close(fixture.billing);
    const int32_t limits[] = {INT32_MAX, INT32_MAX - 1, -INT32_MAX, -INT32_MAX + 1};
    for (size_t i = 0; i < sizeof(limits) / sizeof(*limits); ++i) {
        atomic_store(&one->references, limits[i]);
        CHECK(xrt_place_edge_transfer(edges, 3, &publication) == XRT_MANAGED_REFCOUNT);
        CHECK(xr_signed_rc_read(&one->references) == limits[i]);
        CHECK(*owner_field(&fixture, 0) == one && *owner_field(&fixture, 1) == two && *owner_field(&fixture, 2) == three);
        CHECK(*owner_field(&fixture, 3) == four && *owner_field(&fixture, 4) == five);
        CHECK(!*owner_field(&fixture, 8) && !*owner_field(&fixture, 9) && !*owner_field(&fixture, 10));
        CHECK(tag == 7 && !bytes.calls && !atomic_load(&fixture.audit.retired));
        XrtBillingStats after = xrt_billing_owner_stats(fixture.billing);
        CHECK(after.bytes == before.bytes && after.blocks == before.blocks);
    }
    atomic_store(&one->references, 1);
    OK(xrt_place_edge_transfer(edges, 3, &publication));
    CHECK(!*owner_field(&fixture, 3) && *owner_field(&fixture, 4) == four);
    CHECK(*owner_field(&fixture, 0) == one && *owner_field(&fixture, 1) == one && *owner_field(&fixture, 2) == one);
    CHECK(xr_signed_rc_read(&one->references) == 3 && tag == 19 && bytes.calls == 1);
    CHECK(atomic_load(&fixture.audit.retired) == 3 && atomic_load(&fixture.audit.allocations) == allocations);
    fixture_done(&fixture);
    puts("COPY overflow in either counter band rolls back every staged retain before any MOVE");
}
static void mixed_edge_overlap(void) {
    Fixture fixture;
    fixture_init(&fixture);
    XrtManagedHeader *one = make(&fixture, 0, 1), *two = make(&fixture, 1, 2);
    make(&fixture, 2, 3);
    XrtPlaceEdge edges[] = {
        {slot(&fixture, 1), slot(&fixture, 0), slot(&fixture, 8), XRT_PLACE_TRANSFER_MOVE},
        {slot(&fixture, 0), slot(&fixture, 1), slot(&fixture, 9), XRT_PLACE_TRANSFER_COPY},
        {slot(&fixture, 2), slot(&fixture, 0), slot(&fixture, 10), XRT_PLACE_TRANSFER_COPY}};
    uint8_t tags[] = {10, 20}, expected[] = {20, 10};
    PublicationBytes bytes = {tags, NULL, 2, 0, {20, 10}};
    XrtPlacePublication publication = {publish_bytes, &bytes};
    fixture.audit.observed_destination = owner_field(&fixture, 0); fixture.audit.expected = two;
    fixture.audit.observed_other = owner_field(&fixture, 2); fixture.audit.expected_other = one;
    fixture.audit.observed_source = owner_field(&fixture, 1); fixture.audit.expected_source = one;
    fixture.audit.observed_bytes = tags; fixture.audit.expected_bytes = expected;
    fixture.audit.byte_count = 2; fixture.audit.observed_publications = &bytes.calls;
    fixture.audit.reenter = slot(&fixture, 2); fixture.audit.watch = true;
    size_t allocations = atomic_load(&fixture.audit.allocations);
    xrt_billing_owner_fail_after(fixture.billing, 0);
    OK(xrt_place_edge_transfer(edges, 3, &publication));
    fixture.audit.watch = false;
    CHECK(*owner_field(&fixture, 0) == two && *owner_field(&fixture, 1) == one && *owner_field(&fixture, 2) == one);
    CHECK(xr_signed_rc_read(&one->references) == 2 && xr_signed_rc_read(&two->references) == 1);
    CHECK(!*owner_field(&fixture, 8) && !*owner_field(&fixture, 9) && !*owner_field(&fixture, 10));
    CHECK(bytes.calls == 1 && atomic_load(&fixture.audit.retired) == 1 && atomic_load(&fixture.audit.allocations) == allocations);
    fixture_done(&fixture);
    puts("mixed COPY/MOVE snapshots overlapping sources before publishing a swap");
}
static void external_edge_copies(void) {
    Fixture fixture;
    fixture_init(&fixture);
    XrtManagedHeader *one = make(&fixture, 0, 1);
    make(&fixture, 1, 2); make(&fixture, 2, 3);
    XrtManagedHeader *outside = NULL; bool busy = false;
    Guard guard = {.readable=true, .writable=true};
    XrtExternalOwner external = {&outside, &busy, preflight, &guard, NULL};
    XrtPlace field = {.external=&external, .nulls=XRT_PLACE_NONNULL};
    OK(xrt_place_move_replace(field, slot(&fixture, 0), NULL));
    guard.from = field; guard.to = slot(&fixture, 3); guard.reentry = true; guard.writable = false;
    XrtPlaceEdge edges[] = {
        {slot(&fixture, 1), field, slot(&fixture, 8), XRT_PLACE_TRANSFER_COPY},
        {slot(&fixture, 2), field, slot(&fixture, 9), XRT_PLACE_TRANSFER_COPY}};
    edges[0].transfer = XRT_PLACE_TRANSFER_MOVE;
    CHECK(xrt_place_edge_transfer(edges, 2, NULL) == XRT_MANAGED_CLOSED);
    CHECK(outside == one && xr_signed_rc_read(&one->references) == 1 && !busy);
    edges[0].transfer = XRT_PLACE_TRANSFER_COPY;
    guard.readable = false;
    CHECK(xrt_place_edge_transfer(edges, 2, NULL) == XRT_MANAGED_CLOSED);
    guard.readable = true;
    bool other_busy = false;
    XrtExternalOwner alias = external; alias.busy = &other_busy;
    edges[1].source.external = &alias;
    BAD(xrt_place_edge_transfer(edges, 2, NULL));
    CHECK(!busy && !other_busy && outside == one);
    edges[1].source = field;
    XrtManagedHeader *pending = NULL;
    OK(xrt_managed_slot_prepare(fixture.frame, 8, fixture.ops, sizeof(Payload), &pending));
    ((Payload *)pending)->value = 4;
    BAD(xrt_place_edge_transfer(edges, 2, NULL));
    CHECK(outside == one && !*owner_field(&fixture, 8));
    OK(xrt_managed_slot_abort(fixture.frame, 8));
    size_t calls = guard.calls, allocations = atomic_load(&fixture.audit.allocations);
    xrt_billing_owner_fail_after(fixture.billing, 0); xrt_billing_owner_close(fixture.billing);
    OK(xrt_place_edge_transfer(edges, 2, NULL));
    CHECK(guard.calls == calls + 2 && outside == one && !busy && xr_signed_rc_read(&one->references) == 3);
    CHECK(*owner_field(&fixture, 1) == one && *owner_field(&fixture, 2) == one);
    CHECK(atomic_load(&fixture.audit.allocations) == allocations);
    guard.writable = true;
    OK(xrt_place_move_replace(slot(&fixture, 3), field, NULL));
    CHECK(!outside);
    fixture_done(&fixture);
    puts("external COPY needs readiness and read authority while MOVE still needs write authority");
}
static void nullable_edge_copies(void) {
    Fixture fixture;
    fixture_init(&fixture);
    make(&fixture, 1, 1); make(&fixture, 2, 2);
    XrtPlace source = slot(&fixture, 0), first = slot(&fixture, 1), second = slot(&fixture, 2);
    XrtPlace temporary = slot(&fixture, 8), other_temporary = slot(&fixture, 9);
    source.nulls = first.nulls = second.nulls = temporary.nulls = other_temporary.nulls = XRT_PLACE_NULLABLE;
    XrtPlaceEdge edges[] = {{first, source, temporary, XRT_PLACE_TRANSFER_COPY},
        {second, source, other_temporary, XRT_PLACE_TRANSFER_COPY}};
    uint8_t tag = 1, expected = 0;
    PublicationBytes bytes = {&tag, NULL, 1, 0, {0, 0}};
    XrtPlacePublication publication = {publish_bytes, &bytes};
    fixture.audit.observed_destination = owner_field(&fixture, 1);
    fixture.audit.observed_other = owner_field(&fixture, 2);
    fixture.audit.observed_source = owner_field(&fixture, 0);
    fixture.audit.observed_bytes = &tag; fixture.audit.expected_bytes = &expected;
    fixture.audit.byte_count = 1; fixture.audit.observed_publications = &bytes.calls;
    fixture.audit.reenter = first; fixture.audit.watch = true;
    OK(xrt_place_edge_transfer(edges, 2, &publication));
    fixture.audit.watch = false;
    CHECK(!*owner_field(&fixture, 0) && !*owner_field(&fixture, 1) && !*owner_field(&fixture, 2));
    CHECK(!*owner_field(&fixture, 8) && !*owner_field(&fixture, 9) && bytes.calls == 1 && !tag);
    fixture_done(&fixture);
}
static void single_edge_boundaries(void) {
    /* The second row is an empty nullable MOVE, so both transaction sizes
     * exercise the same first-row rejection and publication obligations. */
    for (size_t count = 1; count <= 2; ++count) {
        Fixture fixture;
        fixture_init(&fixture);
        XrtManagedHeader *one = make(&fixture, 0, 1), *two = make(&fixture, 1, 2);
        XrtManagedHeader *outside = NULL;
        bool busy = false, other_busy = false;
        Guard guard = {.readable=true, .writable=true};
        XrtExternalOwner external = {&outside, &busy, preflight, &guard, NULL};
        XrtPlace field = {.external=&external, .nulls=XRT_PLACE_NONNULL};
        OK(xrt_place_move_replace(field, slot(&fixture, 1), NULL));
        XrtPlace empty = slot(&fixture, 12), target = slot(&fixture, 13), temporary = slot(&fixture, 14);
        empty.nulls = target.nulls = temporary.nulls = XRT_PLACE_NULLABLE;
        XrtPlaceEdge edges[] = {{field, slot(&fixture, 0), slot(&fixture, 8), XRT_PLACE_TRANSFER_COPY},
            {target, empty, temporary, XRT_PLACE_TRANSFER_MOVE}};
        uint8_t tag = 3, expected = 17;
        PublicationBytes bytes = {&tag, NULL, 1, 0, {17, 0}};
        XrtPlacePublication publication = {publish_bytes, &bytes};
        for (size_t role = 0; role < 3; ++role) {
            XrtPlaceEdge invalid[] = {edges[0], edges[1]};
            if (role == 0) invalid[0].source = slot(&fixture, 30);
            else if (role == 1) invalid[0].destination = slot(&fixture, 30);
            else invalid[0].temporary = slot(&fixture, 30);
            BAD(xrt_place_edge_transfer(invalid, count, &publication));
            CHECK(!busy && outside == two && *owner_field(&fixture, 0) == one && !bytes.calls);
        }
        edges[0].transfer = (XrtPlaceTransfer)0;
        BAD(xrt_place_edge_transfer(edges, count, &publication));
        edges[0].transfer = XRT_PLACE_TRANSFER_COPY;
        XrtExternalOwner alias = external;
        alias.busy = &other_busy;
        edges[0].source = (XrtPlace){.external=&alias, .nulls=XRT_PLACE_NONNULL};
        BAD(xrt_place_edge_transfer(edges, count, &publication));
        CHECK(!busy && !other_busy);
        edges[0].source = slot(&fixture, 0);
        busy = true;
        size_t calls = guard.calls;
        BAD(xrt_place_edge_transfer(edges, count, &publication));
        CHECK(busy && guard.calls == calls);
        busy = false; guard.writable = false;
        CHECK(xrt_place_edge_transfer(edges, count, &publication) == XRT_MANAGED_CLOSED);
        guard.writable = true;
        const size_t pending_slots[] = {0, 1, 8};
        for (size_t i = 0; i < sizeof(pending_slots) / sizeof(*pending_slots); ++i) {
            XrtManagedHeader *pending = NULL;
            if (pending_slots[i] == 1) edges[0].destination = slot(&fixture, 1);
            OK(xrt_managed_slot_prepare(fixture.frame, pending_slots[i], fixture.ops, sizeof(Payload), &pending));
            ((Payload *)pending)->value = 3;
            BAD(xrt_place_edge_transfer(edges, count, &publication));
            CHECK(outside == two && *owner_field(&fixture, 0) == one && !busy);
            OK(xrt_managed_slot_abort(fixture.frame, pending_slots[i]));
            edges[0].destination = field;
        }
        make(&fixture, 8, 4);
        BAD(xrt_place_edge_transfer(edges, count, &publication));
        OK(xrt_managed_slot_drop(fixture.frame, 8));
        edges[0].temporary = slot(&fixture, 0);
        BAD(xrt_place_edge_transfer(edges, count, &publication));
        edges[0].temporary = slot(&fixture, 8);
        const int32_t limits[] = {INT32_MAX, -INT32_MAX};
        for (size_t i = 0; i < sizeof(limits) / sizeof(*limits); ++i) {
            atomic_store(&one->references, limits[i]);
            CHECK(xrt_place_edge_transfer(edges, count, &publication) == XRT_MANAGED_REFCOUNT);
            edges[0].destination = slot(&fixture, 0);
            CHECK(xrt_place_edge_transfer(edges, count, &publication) == XRT_MANAGED_REFCOUNT);
            edges[0].transfer = XRT_PLACE_TRANSFER_MOVE;
            OK(xrt_place_edge_transfer(edges, count, NULL));
            CHECK(xr_signed_rc_read(&one->references) == limits[i] && !*owner_field(&fixture, 8));
            edges[0].transfer = XRT_PLACE_TRANSFER_COPY; edges[0].destination = field;
        }
        atomic_store(&one->references, 1);
        CHECK(outside == two && tag == 3 && !bytes.calls && !busy);
        guard.from = slot(&fixture, 0); guard.to = field; guard.reentry = true;
        fixture.audit.observed_destination = &outside; fixture.audit.expected = one;
        fixture.audit.observed_source = owner_field(&fixture, 0); fixture.audit.expected_source = one;
        fixture.audit.observed_bytes = &tag; fixture.audit.expected_bytes = &expected;
        fixture.audit.byte_count = 1; fixture.audit.observed_publications = &bytes.calls;
        fixture.audit.reenter = field; fixture.audit.watch = true;
        size_t allocations = atomic_load(&fixture.audit.allocations);
        xrt_billing_owner_fail_after(fixture.billing, 0);
        OK(xrt_place_edge_transfer(edges, count, &publication));
        fixture.audit.watch = false;
        CHECK(outside == one && xr_signed_rc_read(&one->references) == 2 && tag == 17 && bytes.calls == 1 && !busy);
        CHECK(!*owner_field(&fixture, 8) && atomic_load(&fixture.audit.allocations) == allocations);
        edges[0].source = field; edges[0].destination = slot(&fixture, 2);
        guard.readable = false; guard.writable = false;
        CHECK(xrt_place_edge_transfer(edges, count, NULL) == XRT_MANAGED_CLOSED);
        guard.readable = true;
        OK(xrt_place_edge_transfer(edges, count, NULL));
        CHECK(outside == one && xr_signed_rc_read(&one->references) == 3 && !busy);
        edges[0].transfer = XRT_PLACE_TRANSFER_MOVE; edges[0].destination = slot(&fixture, 3);
        CHECK(xrt_place_edge_transfer(edges, count, NULL) == XRT_MANAGED_CLOSED);
        guard.writable = true;
        OK(xrt_place_edge_transfer(edges, count, NULL));
        CHECK(!outside && !busy && xr_signed_rc_read(&one->references) == 3);
        fixture_done(&fixture);
    }
    puts("single and multi-row edges retain identical rejection, alias, guard and publication boundaries");
}
static void inline_home_transfers(void) {
    typedef struct Homes { XrtManagedHeader *left, *right; } Homes;
    Fixture fixture;
    fixture_init(&fixture);
    XrtManagedFrame *outer = fixture.frame;
    fixture.frame = NULL;
    size_t offsets[] = {offsetof(Homes, left), offsetof(Homes, right), SIZE_MAX, SIZE_MAX};
    XrtManagedFrameSpec spec = {4, sizeof(Homes), alignof(Homes), offsets};
    OK(xrt_managed_frame_enter_layout(fixture.root, &spec, &fixture.frame));
    void *payload = NULL;
    size_t bytes = 0;
    OK(xrt_managed_frame_payload(fixture.frame, &payload, &bytes));
    CHECK(bytes == sizeof(Homes));
    Homes *homes = payload;
    XrtManagedHeader *one = make(&fixture, 0, 1), *two = make(&fixture, 1, 2);
    CHECK(homes->left == one && homes->right == two);
    XrtPlaceEdge swap[] = {{slot(&fixture, 0), slot(&fixture, 1), slot(&fixture, 2), XRT_PLACE_TRANSFER_MOVE},
        {slot(&fixture, 1), slot(&fixture, 0), slot(&fixture, 3), XRT_PLACE_TRANSFER_MOVE}};
    size_t allocations = atomic_load(&fixture.audit.allocations);
    OK(xrt_place_edge_transfer(swap, 2, NULL));
    CHECK(homes->left == two && homes->right == one);
    fixture.audit.watch = true;
    fixture.audit.observed_destination = &homes->right;
    fixture.audit.expected = two;
    fixture.audit.reenter = slot(&fixture, 1);
    OK(xrt_place_copy_replace(slot(&fixture, 1), slot(&fixture, 0), NULL));
    CHECK(homes->left == two && homes->right == two && xr_signed_rc_read(&two->references) == 2);
    CHECK(atomic_load(&fixture.audit.allocations) == allocations);
    fixture.audit.watch = false;
    OK(xrt_place_edge_transfer(swap, 2, NULL));
    CHECK(homes->left == two && homes->right == two && xr_signed_rc_read(&two->references) == 2);
    XrtManagedHeader *pending = NULL;
    OK(xrt_managed_slot_prepare(fixture.frame, 0, fixture.ops, sizeof(Payload), &pending));
    ((Payload *)pending)->value = 3;
    BAD(xrt_place_edge_transfer(swap, 2, NULL));
    CHECK(homes->left == two && homes->right == two);
    OK(xrt_managed_slot_abort(fixture.frame, 0));
    OK(xrt_managed_frame_leave(fixture.root, fixture.frame));
    fixture.frame = outer;
    CHECK(atomic_load(&fixture.audit.retired) == 3);
    fixture_done(&fixture);
    puts("inline owner places: parallel swaps, real aliases and publish-before-drop PASS");
}
int main(void) {
    publication_boundaries();
    edge_publication();
    duplicate_copy_publication(); copy_overflow_rollback(); mixed_edge_overlap();
    external_edge_copies(); nullable_edge_copies(); single_edge_boundaries();
    inline_home_transfers();
    root_transfers(); external_transfers(); edges_and_domains(); edge_guards_and_retirement(); escaped_lifetime();
    puts("stable places: guarded replacement, whole-edge transfers and complete owner/code retirement passed");
    return 0;
}
