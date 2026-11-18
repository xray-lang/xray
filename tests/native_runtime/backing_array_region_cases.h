/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * backing_array_region_cases.h - A pure borrowed cache over real initialized slots
 */
static XrtArrayI64RegionContext array_region_context(const ArrayLocalFixture *fixture) {
    return (XrtArrayI64RegionContext){fixture->view.frame,fixture->view.payload,&fixture->view,&fixture->spec};
}
static void array_region_refused(ArrayLocalFixture *fixture, const XrtArrayI64RegionContext *context,
    const XrtArrayI64RegionSpec *spec, bool ready) {
    ArrayLocalHomes before = *(ArrayLocalHomes *)fixture->view.payload;
    XrtManagedPending pending[] = {fixture->view.slots[0].pending,fixture->view.slots[1].pending};
    XrtBillingStats bill = xrt_billing_owner_stats(fixture->base.billing);
    bool busy = *fixture->view.busy;
    XrtArrayBacking *array = array_local_backing(fixture);
    int32_t count = array ? atomic_load_explicit(&array->managed.references,memory_order_relaxed) : 0;
    int64_t elements[2] = {0};
    if (array) for (size_t n = 0; n < 2; ++n) elements[n] = array_local_element(array,n);
    XrtArrayI64RegionCache cache = {0};
    void (*observe)(void *,void *,bool) = fixture->base.audit.observe;
    void *observer = fixture->base.audit.observer; size_t callbacks = 0;
    fixture->base.audit.observe = array_local_observed; fixture->base.audit.observer = &callbacks;
    bool refused = !xrt_array_i64_region_try_begin(context,spec,ready,&cache);
    fixture->base.audit.observe = observe; fixture->base.audit.observer = observer;
    CHECK(refused && !callbacks);
    ArrayLocalHomes *homes = fixture->view.payload;
    CHECK(!cache.owner && !cache.elements && !cache.length && homes->binding == before.binding &&
        homes->workspace == before.workspace && homes->marker == before.marker && *fixture->view.busy == busy);
    for (size_t n = 0; n < 2; ++n) {
        CHECK(fixture->view.slots[n].pending.value == pending[n].value &&
            fixture->view.slots[n].pending.allocation_bytes == pending[n].allocation_bytes);
        if (array) CHECK(array_local_element(array,n) == elements[n]);
    }
    CHECK(!array || atomic_load_explicit(&array->managed.references,memory_order_relaxed) == count);
    same_bill(fixture->base.billing,bill);
    CHECK(xrt_billing_owner_stats(fixture->base.billing).allocations == bill.allocations);
}
static void array_region_guards(void) {
    ArrayLocalFixture fixture; array_local_init(&fixture,2);
    XrtArrayI64RegionContext context = array_region_context(&fixture);
    XrtArrayI64RegionSlot slots[] = {{0,fixture.offsets[0]},{1,fixture.offsets[1]}};
    XrtArrayI64RegionSpec spec = {slots,2,0};
    array_region_refused(&fixture,&context,&spec,false);
    XrtArrayUpdate edit = {XRT_ARRAY_UPDATE_SET,1,{XRT_BACKING_ARRAY_I64,{.i64=31}}};
    OK(xrt_array_update_place((XrtPlace){.frame=fixture.view.frame,.slot=0,.nulls=XRT_PLACE_NONNULL},
        XRT_BACKING_ARRAY_I64,fixture.view.frame,1,&fixture.base.types,&edit));
    for (unsigned n = 0; n < 9; ++n) {
        XrtArrayI64RegionContext invalid = context; XrtArrayI64RegionSpec shape = spec;
        if (!n) invalid.frame = NULL;
        else if (n == 1) invalid.payload = NULL;
        else if (n == 2) invalid.locations = NULL;
        else if (n == 3) invalid.spec = NULL;
        else if (n == 4) shape.slots = NULL;
        else if (n == 5) shape.slot_count = 0;
        else if (n == 6) shape.canonical = 2;
        else if (n == 7) slots[1].slot = SIZE_MAX;
        else ++slots[1].offset;
        array_region_refused(&fixture,&invalid,&shape,true);
        slots[1] = (XrtArrayI64RegionSlot){1,fixture.offsets[1]};
    }
    for (unsigned n = 0; n < 4; ++n) {
        XrtManagedFrameSpec shape = fixture.spec; XrtArrayI64RegionContext invalid = context;
        if (!n) shape.slot_count = 1;
        else if (n == 1) shape.payload_size = fixture.offsets[1];
        else if (n == 2) shape.payload_alignment = 0;
        else shape.payload_alignment = 3;
        invalid.spec = &shape; array_region_refused(&fixture,&invalid,&spec,true);
    }
    XrtManagedFrameLocations foreign = {0};
    OK(xrt_managed_frame_enter_locations(fixture.base.root,&fixture.spec,&foreign));
    XrtArrayI64RegionContext invalid = context; invalid.locations = &foreign;
    array_region_refused(&fixture,&invalid,&spec,true);
    OK(xrt_managed_frame_leave(fixture.base.root,foreign.frame));
    XrtArrayBacking *array = array_local_backing(&fixture);
    const int32_t counts[] = {-1,-2,INT32_MIN,0,2,INT32_MAX};
    for (size_t n = 0; n < sizeof(counts) / sizeof(*counts); ++n) {
        atomic_store_explicit(&array->managed.references,counts[n],memory_order_relaxed);
        array_region_refused(&fixture,&context,&spec,true);
        atomic_store_explicit(&array->managed.references,1,memory_order_relaxed);
    }
    XrtManagedSlotLocation location = {0}; OK(xrt_managed_slot_location(fixture.view.frame,0,&location));
    OK(xrt_managed_domain_enter(location)); array_region_refused(&fixture,&context,&spec,true);
    xrt_managed_domain_leave(location);
    for (size_t n = 0; n < 2; ++n) {
        XrtManagedHeader *pending = NULL;
        OK(xrt_managed_slot_prepare(fixture.view.frame,n,fixture.base.types.arrays[XRT_BACKING_ARRAY_I64],
            sizeof(XrtArrayBacking),&pending));
        array_region_refused(&fixture,&context,&spec,true); OK(xrt_managed_slot_abort(fixture.view.frame,n));
    }
    int64_t item = 17; OK(xrt_array_backing_i64_create(fixture.view.frame,1,&fixture.base.types,&item,1,1));
    array_region_refused(&fixture,&context,&spec,true); OK(xrt_managed_slot_drop(fixture.view.frame,1));
    XrtManagedHeader *saved = NULL; OK(xrt_managed_slot_take(fixture.view.frame,0,&saved));
    array_region_refused(&fixture,&context,&spec,true); OK(xrt_managed_slot_adopt(fixture.view.frame,0,&saved));
    array->capacity = SIZE_MAX; array_region_refused(&fixture,&context,&spec,true); array->capacity = 4;
    array->length = 3; array->capacity = 2; array_region_refused(&fixture,&context,&spec,true);
    array->length = 2; array->capacity = 4;
    XrtArrayI64RegionCache occupied = {&array->managed,array->elements,array->length};
    CHECK(!xrt_array_i64_region_try_begin(&context,&spec,true,&occupied) && occupied.owner == &array->managed &&
        occupied.elements == array->elements && occupied.length == 2);
    occupied = (XrtArrayI64RegionCache){0}; CHECK(!occupied.owner);
    CHECK(!xrt_array_i64_region_try_begin(&context,&spec,true,NULL));
    XrtArrayI64RegionCache cache = {0}; XrtBillingStats bill = xrt_billing_owner_stats(fixture.base.billing);
    xrt_billing_owner_fail_after(fixture.base.billing,0);
    CHECK(xrt_array_i64_region_try_begin(&context,&spec,true,&cache) && cache.owner == &array->managed &&
        cache.elements == array->elements && cache.length == 2 && array_local_backing(&fixture) == array &&
        atomic_load_explicit(&array->managed.references,memory_order_relaxed) == 1 && !*fixture.view.busy);
    same_bill(fixture.base.billing,bill);
    CHECK(xrt_billing_owner_stats(fixture.base.billing).allocations == bill.allocations);
    /* Frame retirement owns the Header even while this borrowed cache exists.
     * Discarding the cache afterward never reads an expired Frame or payload. */
    OK(xrt_managed_frame_leave(fixture.base.root,fixture.view.frame));
    cache = (XrtArrayI64RegionCache){0}; CHECK(!cache.owner && !cache.elements && !cache.length);
    xrt_billing_owner_fail_after(fixture.base.billing,SIZE_MAX);
    same_bill(fixture.base.billing,fixture.baseline); fixture_done(&fixture.base);
}
static void array_region_snapshot(void) {
    ArrayLocalFixture fixture; array_local_init(&fixture,2);
    XrtArrayI64RegionContext context = array_region_context(&fixture);
    XrtArrayI64RegionSlot slots[] = {{1,fixture.offsets[1]},{0,fixture.offsets[0]}};
    XrtArrayI64RegionSpec spec = {slots,2,1};
    XrtArrayUpdate setup = {XRT_ARRAY_UPDATE_SET,1,{XRT_BACKING_ARRAY_I64,{.i64=13}}};
    OK(xrt_array_update_place((XrtPlace){.frame=fixture.view.frame,.slot=0,.nulls=XRT_PLACE_NONNULL},
        XRT_BACKING_ARRAY_I64,fixture.view.frame,1,&fixture.base.types,&setup));
    XrtManagedHeader *snapshot = &array_local_backing(&fixture)->managed;
    CHECK(xrt_managed_retain(snapshot)); array_region_refused(&fixture,&context,&spec,true);
    XrtArrayUpdate edit = {XRT_ARRAY_UPDATE_SET,1,{XRT_BACKING_ARRAY_I64,{.i64=41}}};
    xrt_billing_owner_fail_after(fixture.base.billing,0);
    CHECK(xrt_array_update_place((XrtPlace){.frame=fixture.view.frame,.slot=0,.nulls=XRT_PLACE_NONNULL},
        XRT_BACKING_ARRAY_I64,fixture.view.frame,1,&fixture.base.types,&edit) == XRT_MANAGED_OOM);
    CHECK(&array_local_backing(&fixture)->managed == snapshot && array_local_element((XrtArrayBacking *)snapshot,1) == 13);
    xrt_billing_owner_fail_after(fixture.base.billing,SIZE_MAX);
    OK(xrt_array_update_place((XrtPlace){.frame=fixture.view.frame,.slot=0,.nulls=XRT_PLACE_NONNULL},
        XRT_BACKING_ARRAY_I64,fixture.view.frame,1,&fixture.base.types,&edit));
    XrtArrayI64RegionCache cache = {0};
    CHECK(xrt_array_i64_region_try_begin(&context,&spec,true,&cache) && cache.owner != snapshot &&
        cache.owner == &array_local_backing(&fixture)->managed && array_local_element((XrtArrayBacking *)snapshot,1) == 13);
    cache = (XrtArrayI64RegionCache){0}; xrt_managed_drop(snapshot);
    array_local_done(&fixture);
}
static void array_region_independent_span(void) {
    typedef struct RegionHomes { XrtManagedHeader *owners[4]; } RegionHomes;
    Fixture fixture; fixture_init(&fixture,SIZE_MAX);
    XrtBillingStats baseline = xrt_billing_owner_stats(fixture.billing);
    size_t offsets[4];
    for (size_t n = 0; n < 4; ++n) offsets[n] = n * sizeof(XrtManagedHeader *);
    XrtManagedFrameSpec frame = {4,sizeof(RegionHomes),_Alignof(RegionHomes),offsets};
    XrtManagedFrameLocations view = {0}; OK(xrt_managed_frame_enter_locations(fixture.root,&frame,&view));
    int64_t value = 19;
    OK(xrt_array_backing_i64_create(view.frame,0,&fixture.types,&value,1,1));
    OK(xrt_array_backing_i64_create(view.frame,2,&fixture.types,&value,1,1));
    XrtArrayUpdate setup = {XRT_ARRAY_UPDATE_SET,0,{XRT_BACKING_ARRAY_I64,{.i64=23}}};
    OK(xrt_array_update_place((XrtPlace){.frame=view.frame,.slot=2,.nulls=XRT_PLACE_NONNULL},
        XRT_BACKING_ARRAY_I64,view.frame,3,&fixture.types,&setup));
    XrtArrayI64RegionSlot all[] = {{0,offsets[0]},{1,offsets[1]},{3,offsets[3]},{2,offsets[2]}};
    size_t first = 2, canonical = 3;
    XrtArrayI64RegionSpec second = {all + first,2,canonical - first};
    XrtArrayI64RegionContext context = {view.frame,view.payload,&view,&frame};
    XrtManagedHeader *pending = NULL;
    OK(xrt_managed_slot_prepare(view.frame,0,fixture.types.arrays[XRT_BACKING_ARRAY_I64],sizeof(XrtArrayBacking),&pending));
    RegionHomes *homes = view.payload; XrtManagedHeader *other = homes->owners[0];
    XrtArrayI64RegionCache cache = {0}; XrtBillingStats before = xrt_billing_owner_stats(fixture.billing);
    CHECK(xrt_array_i64_region_try_begin(&context,&second,true,&cache) && cache.owner == homes->owners[2] &&
        homes->owners[0] == other && view.slots[0].pending.value == pending && !*view.busy &&
        atomic_load_explicit(&other->references,memory_order_relaxed) == 1);
    same_bill(fixture.billing,before);
    cache = (XrtArrayI64RegionCache){0}; CHECK(!cache.owner);
    OK(xrt_managed_slot_abort(view.frame,0)); OK(xrt_managed_frame_leave(fixture.root,view.frame));
    same_bill(fixture.billing,baseline); fixture_done(&fixture);
}
static void array_region_borrows(void) {
    array_region_guards(); array_region_snapshot(); array_region_independent_span();
    puts("Array region guard: unchanged refusals, real COW and pure borrowed retirement passed");
}
