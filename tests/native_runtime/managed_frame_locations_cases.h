/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * managed_frame_locations_cases.h - Actual active metadata and atomic cold publication
 */
#include "xray_native_place.h"

static bool locations_empty(XrtManagedFrameLocations value) {
    return !value.frame && !value.payload && !value.slots && !value.busy && !value.slot_count;
}
static bool locations_same(XrtManagedFrameLocations left, XrtManagedFrameLocations right) {
    return left.frame==right.frame && left.payload==right.payload && left.slots==right.slots &&
        left.busy==right.busy && left.slot_count==right.slot_count;
}
static void locations_busy_callback(Audit *audit) {
    CHECK(audit->locations_watch && audit->locations_root && audit->locations_watch->busy &&
        *audit->locations_watch->busy);
    XrtManagedFrameLocations denied={0};
    XrtManagedFrameSpec spec={1,0,1,NULL};
    BAD(xrt_managed_frame_enter_locations(audit->locations_root,&spec,&denied));
    CHECK(locations_empty(denied) && *audit->locations_watch->busy);
    ++audit->locations_callbacks;
}
static void canonical_locations_live(void) {
    Fixture fixture; fixture_init(&fixture); XrtManagedRoot *root=NULL;
    OK(xrt_managed_root_new(fixture.billing,&root));
    XrtBillingStats baseline=xrt_billing_owner_stats(fixture.billing);
    XrtManagedFrameLocations view={0};
    {
        size_t offsets[]={offsetof(InlineHomes,first),offsetof(InlineHomes,second),SIZE_MAX};
        XrtManagedFrameSpec spec={3,sizeof(InlineHomes),alignof(XrMaxAlignment),offsets};
        OK(xrt_managed_frame_enter_locations(root,&spec,&view));
        memset(offsets,0xff,sizeof(offsets)); spec=(XrtManagedFrameSpec){0};
    }
    CHECK(view.frame && view.payload && view.slots && view.busy && !*view.busy && view.slot_count==3);
    CHECK(xrt_billing_owner_stats(fixture.billing).allocations==baseline.allocations+1);
    CHECK(view.slots[0].owner_offset==offsetof(InlineHomes,first) &&
        view.slots[1].owner_offset==offsetof(InlineHomes,second) && view.slots[2].owner_offset==SIZE_MAX);
    InlineHomes *homes=view.payload; homes->tag=73; homes->scalar=2.5;
    void *payload=NULL; size_t bytes=0; OK(xrt_managed_frame_payload(view.frame,&payload,&bytes));
    CHECK(payload==view.payload && bytes==sizeof(*homes));
    for (size_t n=0;n<view.slot_count;++n) {
        XrtManagedSlotLocation location;
        OK(xrt_managed_slot_location(view.frame,n,&location));
        CHECK(location.pending==&view.slots[n].pending && location.busy==view.busy &&
            !view.slots[n].pending.value && !view.slots[n].pending.allocation_bytes && !view.slots[n].committed);
        const void *owner=n==2 ? (const void *)&view.slots[n].committed :
            (const unsigned char *)view.payload+view.slots[n].owner_offset;
        CHECK((const void *)location.owner==owner && !*location.owner);
    }
    XrtManagedHeader *owned=make_value(&fixture,60),*original=owned;
    OK(xrt_managed_slot_adopt(view.frame,0,&owned));
    CHECK(!owned && homes->first==original && !view.slots[0].committed);
    fixture.audit.locations_watch=&view; fixture.audit.locations_root=root;
    XrtManagedFrameLocations saved=view,child={0};
    XrtManagedFrameSpec child_spec={1,0,1,NULL};
    fixture.audit.locations_allocate=true;
    OK(xrt_managed_frame_enter_locations(root,&child_spec,&child));
    fixture.audit.locations_allocate=false;
    CHECK(fixture.audit.locations_callbacks==1 && locations_same(view,saved) && !*view.busy);
    CHECK(child.frame && child.frame!=view.frame && child.slots && child.slots!=view.slots &&
        !child.payload && child.slot_count==1 && child.busy==view.busy);
    CHECK(homes->tag==73 && homes->scalar==2.5 && homes->first==original &&
        view.slots[0].owner_offset==offsetof(InlineHomes,first));
    BAD(xrt_managed_frame_leave(root,view.frame));
    XrtManagedSlotLocation parent_location;
    OK(xrt_managed_slot_location(view.frame,0,&parent_location));
    CHECK(parent_location.owner==&homes->first && parent_location.pending==&view.slots[0].pending &&
        parent_location.busy==view.busy);
    OK(xrt_managed_frame_leave(root,child.frame));
    XrtPlaceEdge edge={.destination={.frame=view.frame,.slot=1,.nulls=XRT_PLACE_NULLABLE},
        .source={.frame=view.frame,.slot=0,.nulls=XRT_PLACE_NONNULL},
        .temporary={.frame=view.frame,.slot=2,.nulls=XRT_PLACE_NULLABLE},.transfer=XRT_PLACE_TRANSFER_MOVE};
    for (size_t n=0;n<view.slot_count;++n) {
        size_t callbacks=fixture.audit.locations_callbacks;
        fixture.audit.locations_allocate=true;
        Payload *pending=prepare_value(&fixture,view.frame,n,61+n);
        fixture.audit.locations_allocate=false;
        CHECK(fixture.audit.locations_callbacks==callbacks+1 && !*view.busy && !view.slots[n].committed &&
            view.slots[n].pending.value==&pending->header && view.slots[n].pending.allocation_bytes);
        if (n==1) CHECK(!homes->second);
        XrtBillingStats with_pending=xrt_billing_owner_stats(fixture.billing);
        BAD(xrt_place_edge_transfer(&edge,1,NULL));
        CHECK(!*view.busy && homes->first==original && !homes->second && !view.slots[2].committed &&
            view.slots[n].pending.value==&pending->header);
        same_storage(with_pending,xrt_billing_owner_stats(fixture.billing));
        fixture.audit.locations_drop=true;
        OK(xrt_managed_slot_abort(view.frame,n));
        fixture.audit.locations_drop=false;
        CHECK(fixture.audit.locations_callbacks==callbacks+2 && !*view.busy &&
            !view.slots[n].pending.value && !view.slots[n].pending.allocation_bytes);
    }
    owned=make_value(&fixture,64); original=homes->first;
    OK(xrt_managed_slot_adopt(view.frame,2,&owned));
    CHECK(!owned && view.slots[2].committed && ((Payload *)view.slots[2].committed)->id==64);
    fixture.audit.locations_drop=true;
    OK(xrt_managed_slot_drop(view.frame,2));
    fixture.audit.locations_drop=false;
    CHECK(!view.slots[2].committed && !*view.busy);
    OK(xrt_place_edge_transfer(&edge,1,NULL));
    CHECK(!homes->first && homes->second==original && !view.slots[2].committed && !*view.busy);
    fixture.audit.locations_watch=NULL; fixture.audit.locations_root=NULL;
    OK(xrt_managed_frame_leave(root,view.frame));
    same_storage(baseline,xrt_billing_owner_stats(fixture.billing));
    OK(xrt_managed_root_destroy(&root)); fixture_finish(&fixture);
}
static void canonical_locations_rejections(void) {
    Fixture fixture; fixture_init(&fixture); XrtManagedRoot *root=NULL;
    OK(xrt_managed_root_new(fixture.billing,&root));
    size_t offset=offsetof(InlineHomes,first);
    XrtManagedFrameSpec spec={1,sizeof(InlineHomes),alignof(XrMaxAlignment),&offset};
    XrtManagedFrameLocations parent={0},output={0};
    OK(xrt_managed_frame_enter_locations(root,&spec,&parent));
    XrtBillingStats baseline=xrt_billing_owner_stats(fixture.billing);
    BAD(xrt_managed_frame_enter_locations(root,&spec,NULL));
    BAD(xrt_managed_frame_enter_locations(NULL,&spec,&output)); CHECK(locations_empty(output));
    BAD(xrt_managed_frame_enter_locations(root,NULL,&output)); CHECK(locations_empty(output));
    for (unsigned field=0;field<5;++field) {
        output=(XrtManagedFrameLocations){0};
        switch (field) {
        case 0: output.frame=parent.frame; break;
        case 1: output.payload=parent.payload; break;
        case 2: output.slots=parent.slots; break;
        case 3: output.busy=parent.busy; break;
        default: output.slot_count=parent.slot_count; break;
        }
        XrtManagedFrameLocations before=output;
        BAD(xrt_managed_frame_enter_locations(root,&spec,&output)); CHECK(locations_same(output,before));
    }
    output=(XrtManagedFrameLocations){0};
    XrtManagedFrameSpec invalid={1,1,1,&offset};
    BAD(xrt_managed_frame_enter_locations(root,&invalid,&output)); CHECK(locations_empty(output));
    invalid=(XrtManagedFrameSpec){SIZE_MAX,0,1,NULL};
    CHECK(xrt_managed_frame_enter_locations(root,&invalid,&output)==XRT_MANAGED_OVERFLOW && locations_empty(output));
    same_storage(baseline,xrt_billing_owner_stats(fixture.billing));
    CHECK(xrt_billing_owner_stats(fixture.billing).allocations==baseline.allocations);
    xrt_billing_owner_fail_after(fixture.billing,0);
    CHECK(xrt_managed_frame_enter_locations(root,&spec,&output)==XRT_MANAGED_OOM && locations_empty(output));
    same_storage(baseline,xrt_billing_owner_stats(fixture.billing));
    CHECK(!*parent.busy && parent.slots[0].owner_offset==offset);
    xrt_billing_owner_fail_after(fixture.billing,SIZE_MAX);
    invalid=(XrtManagedFrameSpec){0,sizeof(InlineHomes),alignof(InlineHomes),NULL};
    OK(xrt_managed_frame_enter_locations(root,&invalid,&output));
    CHECK(output.frame && output.payload && !output.slots && !output.slot_count && output.busy==parent.busy);
    OK(xrt_managed_frame_leave(root,output.frame)); output=(XrtManagedFrameLocations){0};
    invalid=(XrtManagedFrameSpec){1,0,1,NULL};
    OK(xrt_managed_frame_enter_locations(root,&invalid,&output));
    CHECK(output.frame && !output.payload && output.slots && output.slot_count==1 &&
        output.slots[0].owner_offset==SIZE_MAX && output.busy==parent.busy);
    OK(xrt_managed_frame_leave(root,output.frame));
    same_storage(baseline,xrt_billing_owner_stats(fixture.billing));
    OK(xrt_managed_frame_leave(root,parent.frame));
    OK(xrt_managed_root_destroy(&root)); fixture_finish(&fixture);
}
static void canonical_frame_locations(void) {
    canonical_locations_live();
    canonical_locations_rejections();
    puts("canonical Frame locations: actual bindings, live tokens, callback busy and atomic failure PASS");
}
