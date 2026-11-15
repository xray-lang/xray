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
    XrtManagedHeader *expected, *expected_other;
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
        CHECK(!audit->observed_source || !*audit->observed_source);
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
    XrtExternalOwner external = {&outside, &busy, preflight, &guard};
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
    XrtPlaceEdge swap[] = {{slot(&fixture, 1), slot(&fixture, 0), slot(&fixture, 8)},
        {slot(&fixture, 0), slot(&fixture, 1), slot(&fixture, 9)}};
    size_t allocations = atomic_load(&fixture.audit.allocations);
    xrt_billing_owner_fail_after(fixture.billing, 0);
    OK(xrt_place_edge_move(swap, 2, NULL));
    const XrtManagedHeader *borrowed = NULL;
    OK(xrt_place_borrow(slot(&fixture, 0), &borrowed)); CHECK(borrowed == two);
    OK(xrt_place_borrow(slot(&fixture, 1), &borrowed)); CHECK(borrowed == one);
    CHECK(atomic_load(&fixture.audit.allocations) == allocations);
    XrtPlaceEdge duplicate[] = {{slot(&fixture, 2), slot(&fixture, 0), slot(&fixture, 8)},
        {slot(&fixture, 3), slot(&fixture, 0), slot(&fixture, 9)}};
    BAD(xrt_place_edge_move(duplicate, 2, NULL));
    duplicate[1].source = slot(&fixture, 1); duplicate[1].destination = slot(&fixture, 2);
    BAD(xrt_place_edge_move(duplicate, 2, NULL));
    BAD(xrt_place_edge_move(NULL, 1, NULL));
    OK(xrt_place_edge_move(NULL, 0, NULL));
    OK(xrt_place_move_replace(slot(&fixture, 3), slot(&peer, 0), NULL));
    OK(xrt_place_borrow(slot(&fixture, 3), &borrowed)); CHECK(borrowed == three);
    // An invalid later location must release only domains acquired by this call.
    XrtPlaceEdge invalid[] = {swap[0], {slot(&peer, 30), slot(&fixture, 1), slot(&fixture, 10)}};
    BAD(xrt_place_edge_move(invalid, 2, NULL));
    OK(xrt_place_edge_move(swap, 2, NULL));
    fixture_done(&fixture); fixture_done(&peer);
}
static void escaped_lifetime(void) {
    Fixture fixture;
    fixture_init(&fixture);
    make(&fixture, 0, 1);
    XrtManagedHeader *outside = NULL; bool busy = false;
    Guard guard = {.readable=true, .writable=true};
    XrtExternalOwner external = {&outside, &busy, preflight, &guard};
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
    XrtExternalOwner external = {&outside, &busy, preflight, &guard};
    XrtPlace field = {.external=&external, .nulls=XRT_PLACE_NONNULL};
    OK(xrt_place_move_replace(field, slot(&fixture, 3), NULL));
    XrtPlaceEdge edges[] = {{slot(&fixture, 2), slot(&fixture, 0), slot(&fixture, 8)},
        {field, slot(&fixture, 1), slot(&fixture, 9)}};
    busy = true;
    BAD(xrt_place_edge_move(edges, 2, NULL)); CHECK(busy);
    busy = false;
    guard.writable = false;
    CHECK(xrt_place_edge_move(edges, 2, NULL) == XRT_MANAGED_CLOSED);
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
    OK(xrt_place_edge_move(edges, 2, NULL));
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
    OK(xrt_place_edge_move(NULL, 0, &publication));
    CHECK(tag == 31 && bytes.calls == 4);
    XrtPlacePublication invalid = {NULL, &bytes};
    BAD(xrt_place_edge_move(NULL, 0, &invalid));
    BAD(xrt_place_move_replace(slot(&fixture, 0), slot(&fixture, 1), &invalid));
    CHECK(tag == 31 && bytes.calls == 4);
    fixture_done(&fixture);
}
static void edge_publication(void) {
    Fixture fixture;
    fixture_init(&fixture);
    XrtManagedHeader *one = make(&fixture, 0, 1), *two = make(&fixture, 1, 2);
    make(&fixture, 2, 3); make(&fixture, 3, 4);
    XrtPlaceEdge edges[] = {{slot(&fixture, 2), slot(&fixture, 0), slot(&fixture, 8)},
        {slot(&fixture, 3), slot(&fixture, 1), slot(&fixture, 9)}};
    uint8_t tags[] = {30, 40}, source_tags[] = {11, 22}, expected[] = {11, 22};
    PublicationBytes bytes = {tags, source_tags, 2, 0, {11, 22}};
    XrtPlacePublication publication = {publish_bytes, &bytes};
    XrtManagedHeader *pending = NULL;
    OK(xrt_managed_slot_prepare(fixture.frame, 3, fixture.ops, sizeof(Payload), &pending));
    ((Payload *)pending)->value = 5;
    BAD(xrt_place_edge_move(edges, 2, &publication));
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
    OK(xrt_place_edge_move(edges, 2, &publication));
    CHECK(bytes.calls == 1 && tags[0] == 11 && tags[1] == 22 && !source_tags[0] && !source_tags[1]);
    fixture.audit.watch = false;
    fixture_done(&fixture);
    puts("payload publication: tags and scalar bytes precede every replaced-owner callback PASS");
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
    XrtPlaceEdge swap[] = {{slot(&fixture, 0), slot(&fixture, 1), slot(&fixture, 2)},
        {slot(&fixture, 1), slot(&fixture, 0), slot(&fixture, 3)}};
    size_t allocations = atomic_load(&fixture.audit.allocations);
    OK(xrt_place_edge_move(swap, 2, NULL));
    CHECK(homes->left == two && homes->right == one);
    fixture.audit.watch = true;
    fixture.audit.observed_destination = &homes->right;
    fixture.audit.expected = two;
    fixture.audit.reenter = slot(&fixture, 1);
    OK(xrt_place_copy_replace(slot(&fixture, 1), slot(&fixture, 0), NULL));
    CHECK(homes->left == two && homes->right == two && xr_signed_rc_read(&two->references) == 2);
    CHECK(atomic_load(&fixture.audit.allocations) == allocations);
    fixture.audit.watch = false;
    OK(xrt_place_edge_move(swap, 2, NULL));
    CHECK(homes->left == two && homes->right == two && xr_signed_rc_read(&two->references) == 2);
    XrtManagedHeader *pending = NULL;
    OK(xrt_managed_slot_prepare(fixture.frame, 0, fixture.ops, sizeof(Payload), &pending));
    ((Payload *)pending)->value = 3;
    BAD(xrt_place_edge_move(swap, 2, NULL));
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
    inline_home_transfers();
    root_transfers(); external_transfers(); edges_and_domains(); edge_guards_and_retirement(); escaped_lifetime();
    puts("stable places: guarded replacement, whole-edge moves and complete owner/code retirement passed");
    return 0;
}
