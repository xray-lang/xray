/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * managed_frame_local_cases.h - Local Frame transactions keep the Root protocol
 */
static int32_t local_references(XrtManagedHeader *value) {
    return atomic_load_explicit(&value->references, memory_order_relaxed);
}
static void local_unchanged(const InlineHomes *homes, XrtManagedHeader *first, XrtManagedHeader *second,
    int32_t first_count, const Fixture *fixture, size_t events) {
    CHECK(homes->first == first && homes->second == second && fixture->audit.count == events);
    if (first) CHECK(local_references(first) == first_count);
}
/* Release, copy, move and store retire an old owner while the Root is busy and
 * leave reference counts as the Place transactions do. A busy Root, a pending
 * slot, an empty source, a failed retain and a slot without a payload owner
 * field are refused without changing any owner. */
static void managed_frame_local_transactions(void) {
    Fixture fixture; fixture_init(&fixture); XrtManagedRoot *root = NULL;
    OK(xrt_managed_root_new(fixture.billing, &root));
    XrtManagedFrameLocations view = forwarding_frame(root); InlineHomes *homes = view.payload;
    XrtManagedHeader *owned = make_value(&fixture, 1), *first = owned;
    OK(xrt_managed_slot_adopt(view.frame, 0, &owned));
    owned = make_value(&fixture, 2);
    OK(xrt_managed_slot_adopt(view.frame, 1, &owned));
    fixture.audit.locations_watch = &view; fixture.audit.locations_root = root; fixture.audit.locations_drop = true;

    OK(xrt_managed_frame_local_replace(&view, 1, 0, true));
    CHECK(homes->first == first && homes->second == first && local_references(first) == 2 &&
        fixture.audit.count == 1 && fixture.audit.events[0] == 2 && fixture.audit.locations_callbacks == 1 &&
        !*view.busy);
    OK(xrt_managed_frame_local_replace(&view, 0, 0, true));
    local_unchanged(homes, first, first, 2, &fixture, 1);
    OK(xrt_managed_frame_local_replace(&view, 0, 1, false));
    CHECK(homes->first == first && !homes->second && local_references(first) == 1 && fixture.audit.count == 1);

    BAD(xrt_managed_frame_local_replace(&view, 0, 1, true));
    BAD(xrt_managed_frame_local_replace(&view, 0, 1, false));
    local_unchanged(homes, first, NULL, 1, &fixture, 1);
    *view.busy = true;
    BAD(xrt_managed_frame_local_drop(&view, 0));
    BAD(xrt_managed_frame_local_replace(&view, 1, 0, true));
    BAD(xrt_managed_frame_local_store(&view, 1, first));
    *view.busy = false;
    local_unchanged(homes, first, NULL, 1, &fixture, 1);
    BAD(xrt_managed_frame_local_drop(&view, 2));
    BAD(xrt_managed_frame_local_drop(&view, view.slot_count));
    BAD(xrt_managed_frame_local_replace(&view, 2, 0, true));
    BAD(xrt_managed_frame_local_replace(&view, 0, view.slot_count, true));
    BAD(xrt_managed_frame_local_store(&view, 2, first));
    BAD(xrt_managed_frame_local_store(&view, 1, NULL));
    local_unchanged(homes, first, NULL, 1, &fixture, 1);

    (void)prepare_value(&fixture, view.frame, 1, 9);
    BAD(xrt_managed_frame_local_replace(&view, 1, 0, true));
    BAD(xrt_managed_frame_local_replace(&view, 0, 1, false));
    BAD(xrt_managed_frame_local_drop(&view, 1));
    BAD(xrt_managed_frame_local_store(&view, 1, first));
    OK(xrt_managed_slot_abort(view.frame, 1));
    // Aborting the pending allocation retires its partial value, not an owner.
    CHECK(fixture.audit.count == 2 && fixture.audit.events[1] == 9);
    local_unchanged(homes, first, NULL, 1, &fixture, 2);

    atomic_store_explicit(&first->references, INT32_MAX, memory_order_relaxed);
    CHECK(xrt_managed_frame_local_replace(&view, 1, 0, true) == XRT_MANAGED_REFCOUNT && !*view.busy);
    CHECK(xrt_managed_frame_local_store(&view, 1, first) == XRT_MANAGED_REFCOUNT && !*view.busy);
    atomic_store_explicit(&first->references, 1, memory_order_relaxed);
    local_unchanged(homes, first, NULL, 1, &fixture, 2);

    owned = make_value(&fixture, 3);
    OK(xrt_managed_slot_adopt(view.frame, 1, &owned));
    OK(xrt_managed_frame_local_store(&view, 1, first));
    CHECK(homes->second == first && local_references(first) == 2 && fixture.audit.count == 3 &&
        fixture.audit.events[2] == 3 && !*view.busy);
    OK(xrt_managed_frame_local_drop(&view, 1));
    CHECK(!homes->second && local_references(first) == 1 && fixture.audit.count == 3);
    size_t callbacks = fixture.audit.locations_callbacks;
    OK(xrt_managed_frame_local_drop(&view, 0));
    CHECK(!homes->first && fixture.audit.count == 4 && fixture.audit.events[3] == 1 &&
        fixture.audit.locations_callbacks == callbacks + 1 && !*view.busy);
    OK(xrt_managed_frame_local_drop(&view, 0));
    CHECK(fixture.audit.count == 4);
    fixture.audit.locations_watch = NULL;
    OK(xrt_managed_frame_leave(root, view.frame));
    OK(xrt_managed_root_destroy(&root)); fixture_finish(&fixture);
    puts("Frame local transactions: busy retirement, Place counts and unchanged refusals PASS");
}
