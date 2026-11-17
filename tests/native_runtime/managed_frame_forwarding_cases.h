/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * managed_frame_forwarding_cases.h - Actual factory views and unchanged refusal paths
 */
typedef struct FrameForwardCall {
    XrtManagedFrame *frame;
    void *payload;
    const XrtManagedFrameLocations *locations;
    size_t source, source_offset, destination, destination_offset;
} FrameForwardCall;

static XrtManagedFrameLocations forwarding_frame(XrtManagedRoot *root) {
    size_t offsets[]={offsetof(InlineHomes,first),offsetof(InlineHomes,second),SIZE_MAX};
    XrtManagedFrameSpec spec={3,sizeof(InlineHomes),alignof(InlineHomes),offsets};
    XrtManagedFrameLocations view={0};
    OK(xrt_managed_frame_enter_locations(root,&spec,&view));
    return view;
}
static FrameForwardCall forwarding_call(const XrtManagedFrameLocations *view) {
    return (FrameForwardCall){view->frame,view->payload,view,0,offsetof(InlineHomes,first),
        1,offsetof(InlineHomes,second)};
}
static bool forwarding_try(FrameForwardCall call) {
    return xrt_managed_frame_try_forward(call.frame,call.payload,call.locations,
        (XrtManagedFrameForward){call.source,call.source_offset,call.destination,call.destination_offset});
}
static void forwarding_rejected(Fixture *fixture, const XrtManagedFrameLocations *real, FrameForwardCall call) {
    InlineHomes *homes=real->payload, before=*homes;
    XrtManagedHeader *owners[]={homes->first,homes->second,real->slots[2].committed};
    XrtManagedPending pending[3]; XrtManagedHeader *committed[3]; int32_t counts[3];
    for (size_t n=0;n<3;++n) {
        pending[n]=real->slots[n].pending; committed[n]=real->slots[n].committed;
        counts[n]=owners[n]?atomic_load_explicit(&owners[n]->references,memory_order_relaxed):0;
    }
    bool busy=*real->busy; size_t events=fixture->audit.count, callbacks=fixture->audit.locations_callbacks;
    XrtBillingStats storage=xrt_billing_owner_stats(fixture->billing);
    CHECK(!forwarding_try(call));
    CHECK(homes->first==before.first && homes->second==before.second &&
        homes->tag==before.tag && homes->scalar==before.scalar && *real->busy==busy);
    for (size_t n=0;n<3;++n) {
        CHECK(real->slots[n].committed==committed[n] && real->slots[n].pending.value==pending[n].value &&
            real->slots[n].pending.allocation_bytes==pending[n].allocation_bytes);
        CHECK(!owners[n] || atomic_load_explicit(&owners[n]->references,memory_order_relaxed)==counts[n]);
    }
    same_storage(storage,xrt_billing_owner_stats(fixture->billing));
    CHECK(xrt_billing_owner_stats(fixture->billing).allocations==storage.allocations &&
        fixture->audit.count==events && fixture->audit.locations_callbacks==callbacks);
}
static void managed_forwarding_success(void) {
    Fixture fixture; fixture_init(&fixture); XrtManagedRoot *root=NULL;
    OK(xrt_managed_root_new(fixture.billing,&root));
    XrtBillingStats root_only=xrt_billing_owner_stats(fixture.billing);
    XrtManagedFrameLocations view=forwarding_frame(root); InlineHomes *homes=view.payload;
    homes->tag=91; homes->scalar=2.5;
    XrtManagedHeader *owned=make_value(&fixture,90), *value=owned;
    OK(xrt_managed_slot_adopt(view.frame,0,&owned)); CHECK(!owned);
    xrt_billing_owner_fail_after(fixture.billing,0);
    XrtBillingStats before=xrt_billing_owner_stats(fixture.billing);
    CHECK(forwarding_try(forwarding_call(&view)) && !homes->first && homes->second==value &&
        atomic_load_explicit(&value->references,memory_order_relaxed)==1);
    FrameForwardCall reverse={view.frame,view.payload,&view,1,offsetof(InlineHomes,second),0,offsetof(InlineHomes,first)};
    CHECK(forwarding_try(reverse) && homes->first==value && !homes->second &&
        atomic_load_explicit(&value->references,memory_order_relaxed)==1);
    XrtManagedHeader *snapshot=value;
    CHECK(xrt_managed_retain(snapshot) && atomic_load_explicit(&value->references,memory_order_relaxed)==2);
    CHECK(forwarding_try(forwarding_call(&view)) && !homes->first && homes->second==snapshot &&
        atomic_load_explicit(&snapshot->references,memory_order_relaxed)==2);
    same_storage(before,xrt_billing_owner_stats(fixture.billing));
    CHECK(xrt_billing_owner_stats(fixture.billing).allocations==before.allocations && !fixture.audit.count &&
        !fixture.audit.locations_callbacks && homes->tag==91 && homes->scalar==2.5 && !*view.busy);
    xrt_managed_drop(snapshot); snapshot=NULL;
    CHECK(atomic_load_explicit(&value->references,memory_order_relaxed)==1 && !fixture.audit.count);
    OK(xrt_managed_frame_leave(root,view.frame));
    CHECK(fixture.audit.count==1 && fixture.audit.events[0]==90);
    same_storage(root_only,xrt_billing_owner_stats(fixture.billing));
    xrt_billing_owner_fail_after(fixture.billing,SIZE_MAX);
    OK(xrt_managed_root_destroy(&root)); fixture_finish(&fixture);
}
static void forwarding_geometry(Fixture *fixture, const XrtManagedFrameLocations *view) {
    FrameForwardCall valid=forwarding_call(view);
    for (unsigned n=0;n<13;++n) {
        FrameForwardCall call=valid;
        switch (n) {
        case 0: call.frame=NULL; break;
        case 1: call.payload=NULL; break;
        case 2: call.locations=NULL; break;
        case 3: call.source=view->slot_count; break;
        case 4: call.destination=view->slot_count; break;
        case 5: call.source=SIZE_MAX; break;
        case 6: call.destination=SIZE_MAX; break;
        case 7: call.destination=call.source; call.destination_offset=call.source_offset; break;
        case 8: call.source_offset=SIZE_MAX; break;
        case 9: call.destination_offset=SIZE_MAX; break;
        case 10: call.source_offset=call.destination_offset; break;
        case 11: ++call.source_offset; break;
        default: ++call.destination_offset; break;
        }
        forwarding_rejected(fixture,view,call);
    }
    XrtManagedFrameLocations foreign=forwarding_frame(xrt_managed_frame_root(view->frame));
    InlineHomes *other=foreign.payload;
    XrtManagedHeader *owned=make_value(fixture,91), *value=owned;
    OK(xrt_managed_slot_adopt(foreign.frame,0,&owned)); CHECK(!owned);
    FrameForwardCall call=valid; call.locations=&foreign; forwarding_rejected(fixture,view,call);
    call=valid; call.payload=foreign.payload; forwarding_rejected(fixture,view,call);
    call=valid; call.frame=foreign.frame; forwarding_rejected(fixture,view,call);
    CHECK(other->first==value && !other->second && !*foreign.busy &&
        atomic_load_explicit(&value->references,memory_order_relaxed)==1);
    OK(xrt_managed_frame_leave(xrt_managed_frame_root(view->frame),foreign.frame));
}
static void managed_forwarding_refusals(void) {
    Fixture fixture; fixture_init(&fixture); XrtManagedRoot *root=NULL;
    OK(xrt_managed_root_new(fixture.billing,&root));
    XrtBillingStats root_only=xrt_billing_owner_stats(fixture.billing);
    XrtManagedFrameLocations view=forwarding_frame(root); InlineHomes *homes=view.payload;
    homes->tag=92; homes->scalar=3.5;
    XrtManagedHeader *owned=make_value(&fixture,92), *source=owned;
    OK(xrt_managed_slot_adopt(view.frame,0,&owned)); CHECK(!owned);
    FrameForwardCall call=forwarding_call(&view);
    forwarding_geometry(&fixture,&view);
    /* Counter-only fault injection: real owned storage remains live, and its
     * actual count is restored before any ownership API consumes the value. */
    const int32_t faults[]={-1,-2,-INT32_MAX,INT32_MIN,0,INT32_MAX};
    for (size_t n=0;n<sizeof(faults)/sizeof(*faults);++n) {
        atomic_store_explicit(&source->references,faults[n],memory_order_relaxed);
        forwarding_rejected(&fixture,&view,call);
        CHECK(atomic_load_explicit(&source->references,memory_order_relaxed)==faults[n]);
        atomic_store_explicit(&source->references,1,memory_order_relaxed);
    }
    XrtManagedSlotLocation location={0}; OK(xrt_managed_slot_location(view.frame,0,&location));
    OK(xrt_managed_domain_enter(location)); CHECK(*view.busy);
    forwarding_rejected(&fixture,&view,call);
    CHECK(*view.busy); xrt_managed_domain_leave(location); CHECK(!*view.busy);
    for (size_t n=0;n<2;++n) {
        Payload *pending=prepare_value(&fixture,view.frame,n,93+n);
        CHECK(view.slots[n].pending.value==&pending->header && view.slots[n].pending.allocation_bytes);
        forwarding_rejected(&fixture,&view,call);
        OK(xrt_managed_slot_abort(view.frame,n));
    }
    owned=make_value(&fixture,95); OK(xrt_managed_slot_adopt(view.frame,1,&owned));
    CHECK(!owned && homes->second); forwarding_rejected(&fixture,&view,call);
    OK(xrt_managed_slot_drop(view.frame,1));
    owned=source; CHECK(xrt_managed_retain(owned)); OK(xrt_managed_slot_adopt(view.frame,1,&owned));
    CHECK(!owned && homes->second==source); forwarding_rejected(&fixture,&view,call);
    OK(xrt_managed_slot_drop(view.frame,1));
    owned=make_value(&fixture,96); OK(xrt_managed_slot_adopt(view.frame,2,&owned)); CHECK(!owned);
    FrameForwardCall ordinary=call; ordinary.source=2; ordinary.source_offset=SIZE_MAX;
    forwarding_rejected(&fixture,&view,ordinary);
    ordinary=call; ordinary.destination=2; ordinary.destination_offset=SIZE_MAX;
    forwarding_rejected(&fixture,&view,ordinary); OK(xrt_managed_slot_drop(view.frame,2));
    OK(xrt_managed_slot_take(view.frame,0,&owned)); CHECK(owned==source && !homes->first);
    forwarding_rejected(&fixture,&view,call);
    OK(xrt_managed_slot_adopt(view.frame,0,&owned)); CHECK(!owned && homes->first==source);
    OK(xrt_managed_frame_leave(root,view.frame));
    same_storage(root_only,xrt_billing_owner_stats(fixture.billing));
    OK(xrt_managed_root_destroy(&root)); fixture_finish(&fixture);
}
static void managed_frame_forwarding(void) {
    managed_forwarding_success();
    managed_forwarding_refusals();
    puts("Frame forwarding: real RC1/RC2 ownership, exact geometry and unchanged refusals PASS");
}
