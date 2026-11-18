/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * backing_array_local_cases.h - Real private bindings and exact false-path preservation
 */
#include "xray_native_array_local.h"
typedef struct ArrayLocalHomes { XrtManagedHeader *binding, *workspace; int64_t marker; } ArrayLocalHomes;
typedef struct ArrayLocalFixture {
    Fixture base;
    XrtManagedFrameLocations view;
    XrtArrayI64LocalView prepared;
    size_t offsets[2];
    XrtManagedFrameSpec spec;
    XrtBillingStats baseline;
} ArrayLocalFixture;
typedef struct ArrayLocalCall {
    XrtManagedFrame *frame;
    void *payload;
    const XrtManagedFrameLocations *locations;
    const XrtManagedFrameSpec *spec;
    XrtArrayI64LocalGeometry geometry;
    XrtArrayI64LocalView prepared;
    XrtArrayI64LocalUpdate update;
} ArrayLocalCall;
static void array_local_init(ArrayLocalFixture *f, size_t count) {
    *f=(ArrayLocalFixture){0}; fixture_init(&f->base,SIZE_MAX);
    f->baseline=xrt_billing_owner_stats(f->base.billing);
    f->offsets[0]=offsetof(ArrayLocalHomes,binding); f->offsets[1]=offsetof(ArrayLocalHomes,workspace);
    f->spec=(XrtManagedFrameSpec){2,sizeof(ArrayLocalHomes),_Alignof(ArrayLocalHomes),f->offsets};
    OK(xrt_managed_frame_enter_locations(f->base.root,&f->spec,&f->view));
    /* The factory descriptors may expire. Preparation borrows only immutable
     * addresses inside the real Frame, even before its backing exists. */
    {
        size_t offsets[]={f->offsets[0],f->offsets[1]};
        XrtManagedFrameSpec spec=f->spec; spec.owned_offsets=offsets;
        CHECK(xrt_array_i64_local_prepare(f->view.frame,f->view.payload,&f->view,&spec,
            (XrtArrayI64LocalGeometry){0,offsets[0],1,offsets[1]},&f->prepared));
    }
    int64_t values[]={11,13,17,19};
    OK(xrt_array_backing_i64_create(f->view.frame,0,&f->base.types,values,count,4));
    ((ArrayLocalHomes *)f->view.payload)->marker=71;
}
static void array_local_done(ArrayLocalFixture *f) {
    OK(xrt_managed_frame_leave(f->base.root,f->view.frame));
    same_bill(f->base.billing,f->baseline); fixture_done(&f->base);
}
static ArrayLocalCall array_local_call(const ArrayLocalFixture *f) {
    return (ArrayLocalCall){f->view.frame,f->view.payload,&f->view,&f->spec,
        {0,f->offsets[0],1,f->offsets[1]},f->prepared,{XRT_ARRAY_UPDATE_SET,1,31}};
}
static bool array_local_try(ArrayLocalCall call) {
    return xrt_array_i64_try_update_local(&call.prepared,call.update);
}
static XrtArrayBacking *array_local_backing(const ArrayLocalFixture *f) {
    return (XrtArrayBacking *)((ArrayLocalHomes *)f->view.payload)->binding;
}
static int64_t array_local_element(const XrtArrayBacking *array, size_t index) {
    int64_t value=0; CHECK(index<array->length);
    memcpy(&value,array->elements+index*sizeof(value),sizeof(value)); return value;
}
static void array_local_check_refused(ArrayLocalFixture *f, ArrayLocalCall call, bool prepare) {
    ArrayLocalHomes *homes=f->view.payload, before=*homes;
    unsigned char data[sizeof(XrtArrayBacking)+4*sizeof(int64_t)];
    unsigned char workspace[sizeof(XrtArrayBacking)+sizeof(int64_t)];
    if (before.binding) memcpy(data,before.binding,sizeof(data));
    if (before.workspace) memcpy(workspace,before.workspace,sizeof(workspace));
    XrtManagedPending pending[]={f->view.slots[0].pending,f->view.slots[1].pending};
    XrtManagedHeader *committed[]={f->view.slots[0].committed,f->view.slots[1].committed};
    bool busy=*f->view.busy; XrtBillingStats bill=xrt_billing_owner_stats(f->base.billing);
    XrtArrayI64LocalView empty={0};
    if (prepare) {
        CHECK(!xrt_array_i64_local_prepare(call.frame,call.payload,call.locations,call.spec,call.geometry,&empty));
        CHECK(!empty.busy && !empty.binding_pending && !empty.workspace_pending && !empty.binding_owner && !empty.workspace_owner);
    } else CHECK(!array_local_try(call));
    CHECK(homes->binding==before.binding && homes->workspace==before.workspace && homes->marker==before.marker &&
        *f->view.busy==busy && (!before.binding || !memcmp(data,before.binding,sizeof(data))) &&
        (!before.workspace || !memcmp(workspace,before.workspace,sizeof(workspace))));
    for (size_t n=0;n<2;++n) CHECK(f->view.slots[n].committed==committed[n] &&
        f->view.slots[n].pending.value==pending[n].value && f->view.slots[n].pending.allocation_bytes==pending[n].allocation_bytes);
    same_bill(f->base.billing,bill);
    CHECK(xrt_billing_owner_stats(f->base.billing).allocations==bill.allocations);
}
static void array_local_refused(ArrayLocalFixture *f, ArrayLocalCall call) {
    array_local_check_refused(f,call,false);
}
static void array_local_prepare_refused(ArrayLocalFixture *f, ArrayLocalCall call) {
    array_local_check_refused(f,call,true);
}
static void array_local_observed(void *context, void *memory, bool allocate) {
    (void)memory; (void)allocate; ++*(size_t *)context;
}
static void array_local_success(void) {
    ArrayLocalFixture f; array_local_init(&f,2); ArrayLocalCall call=array_local_call(&f);
    XrtArrayBacking *array=array_local_backing(&f); const XrtManagedOps *ops=array->managed.ops;
    uint32_t flags=atomic_load_explicit(&array->managed.flags,memory_order_relaxed);
    XrtBillingStats before=xrt_billing_owner_stats(f.base.billing);
    size_t callbacks=0; f.base.audit.observe=array_local_observed; f.base.audit.observer=&callbacks;
    xrt_billing_owner_fail_after(f.base.billing,0);
    CHECK(array_local_try(call));
    CHECK(array_local_element(array,0)==11 && array_local_element(array,1)==31 && array->length==2);
    call.update.operation=XRT_ARRAY_UPDATE_PUSH; call.update.value=INT64_MIN;
    CHECK(array_local_try(call));
    CHECK(array_local_element(array,2)==INT64_MIN && array->length==3 && array->capacity==4 &&
        array==array_local_backing(&f) && array->managed.ops==ops &&
        atomic_load_explicit(&array->managed.references,memory_order_relaxed)==1 &&
        atomic_load_explicit(&array->managed.flags,memory_order_relaxed)==flags);
    CHECK(!callbacks && !*f.view.busy && !((ArrayLocalHomes *)f.view.payload)->workspace &&
        ((ArrayLocalHomes *)f.view.payload)->marker==71);
    same_bill(f.base.billing,before); CHECK(xrt_billing_owner_stats(f.base.billing).allocations==before.allocations);
    f.base.audit.observe=NULL; f.base.audit.observer=NULL;
    xrt_billing_owner_fail_after(f.base.billing,SIZE_MAX); array_local_done(&f);
}
static void array_local_geometry(ArrayLocalFixture *f) {
    ArrayLocalCall original=array_local_call(f);
    for (unsigned n=0;n<12;++n) {
        ArrayLocalCall call=original;
        switch (n) {
        case 0: call.frame=NULL; break;
        case 1: call.payload=NULL; break;
        case 2: call.locations=NULL; break;
        case 3: call.spec=NULL; break;
        case 4: call.geometry.binding_slot=SIZE_MAX; break;
        case 5: call.geometry.workspace_slot=f->view.slot_count; break;
        case 6: call.geometry.binding_offset=SIZE_MAX; break;
        case 7: call.geometry.workspace_offset=SIZE_MAX; break;
        case 8: call.geometry.workspace_slot=call.geometry.binding_slot; break;
        case 9: call.geometry.workspace_offset=call.geometry.binding_offset; break;
        case 10: ++call.geometry.binding_offset; break;
        default: ++call.geometry.workspace_offset; break;
        }
        array_local_prepare_refused(f,call);
    }
    for (unsigned n=0;n<4;++n) {
        XrtManagedFrameSpec spec=f->spec;
        if (!n) spec.slot_count=1;
        else if (n==1) spec.payload_size=f->offsets[1];
        else if (n==2) spec.payload_alignment=0;
        else spec.payload_alignment=3;
        ArrayLocalCall call=original; call.spec=&spec; array_local_prepare_refused(f,call);
    }
    XrtManagedFrameLocations foreign={0};
    OK(xrt_managed_frame_enter_locations(f->base.root,&f->spec,&foreign));
    ArrayLocalCall call=original; call.locations=&foreign; array_local_prepare_refused(f,call);
    call=original; call.payload=foreign.payload; array_local_prepare_refused(f,call);
    call=original; call.frame=foreign.frame; array_local_prepare_refused(f,call);
    CHECK(array_local_try(original)); /* A child Frame cannot move the parent. */
    call=original; call.update.value=13; CHECK(array_local_try(call));
    CHECK(!((ArrayLocalHomes *)foreign.payload)->binding && !((ArrayLocalHomes *)foreign.payload)->workspace);
    OK(xrt_managed_frame_leave(f->base.root,foreign.frame));
    XrtArrayI64LocalView nonempty=f->prepared, before=nonempty;
    CHECK(!xrt_array_i64_local_prepare(original.frame,original.payload,original.locations,original.spec,original.geometry,&nonempty));
    CHECK(nonempty.busy==before.busy && nonempty.binding_pending==before.binding_pending &&
        nonempty.workspace_pending==before.workspace_pending && nonempty.binding_owner==before.binding_owner &&
        nonempty.workspace_owner==before.workspace_owner);
    CHECK(!xrt_array_i64_local_prepare(original.frame,original.payload,original.locations,original.spec,original.geometry,NULL));
}
static void array_local_guards(void) {
    ArrayLocalFixture f; array_local_init(&f,2); ArrayLocalCall call=array_local_call(&f);
    XrtArrayBacking *array=array_local_backing(&f); array_local_geometry(&f);
    CHECK(!xrt_array_i64_try_update_local(NULL,call.update));
    ArrayLocalCall empty=call; empty.prepared=(XrtArrayI64LocalView){0}; array_local_refused(&f,empty);
    const int32_t counts[]={-1,-2,-INT32_MAX,0,INT32_MAX};
    /* Counter faults keep the actual allocation and owner. Restore each one
     * before any ownership API may consume it; no fake Header is created. */
    for (size_t n=0;n<sizeof(counts)/sizeof(*counts);++n) {
        atomic_store_explicit(&array->managed.references,counts[n],memory_order_relaxed);
        array_local_refused(&f,call);
        CHECK(atomic_load_explicit(&array->managed.references,memory_order_relaxed)==counts[n]);
        atomic_store_explicit(&array->managed.references,1,memory_order_relaxed);
    }
    XrtManagedSlotLocation domain={0}; OK(xrt_managed_slot_location(f.view.frame,0,&domain));
    OK(xrt_managed_domain_enter(domain));
    XrtArrayI64LocalView while_busy={0};
    CHECK(xrt_array_i64_local_prepare(call.frame,call.payload,call.locations,call.spec,call.geometry,&while_busy));
    call.prepared=while_busy; array_local_refused(&f,call); xrt_managed_domain_leave(domain);
    for (size_t n=0;n<2;++n) {
        XrtManagedHeader *pending=NULL;
        OK(xrt_managed_slot_prepare(f.view.frame,n,f.base.types.arrays[XRT_BACKING_ARRAY_I64],
            sizeof(XrtArrayBacking)+sizeof(int64_t),&pending));
        ((XrtArrayBacking *)pending)->length=1;
        XrtArrayI64LocalView while_pending={0};
        CHECK(xrt_array_i64_local_prepare(call.frame,call.payload,call.locations,call.spec,call.geometry,&while_pending));
        call.prepared=while_pending; array_local_refused(&f,call);
        OK(xrt_managed_slot_abort(f.view.frame,n));
    }
    int64_t old=23; OK(xrt_array_backing_i64_create(f.view.frame,1,&f.base.types,&old,1,1));
    array_local_refused(&f,call);
    BAD(xrt_array_update_place((XrtPlace){.frame=f.view.frame,.slot=0,.nulls=XRT_PLACE_NONNULL},
        XRT_BACKING_ARRAY_I64,f.view.frame,1,&f.base.types,&(XrtArrayUpdate){XRT_ARRAY_UPDATE_SET,1,{XRT_BACKING_ARRAY_I64,{.i64=31}}}));
    OK(xrt_managed_slot_drop(f.view.frame,1));
    XrtManagedHeader *saved=NULL; OK(xrt_managed_slot_take(f.view.frame,0,&saved));
    array_local_refused(&f,call); OK(xrt_managed_slot_adopt(f.view.frame,0,&saved));
    const int64_t indices[]={-1,2,INT64_MIN,INT64_MAX};
    for (size_t n=0;n<sizeof(indices)/sizeof(*indices);++n) {
        call.update.index=indices[n]; array_local_refused(&f,call);
    }
    call=array_local_call(&f); call.update.operation=(XrtArrayUpdateKind)UINT_MAX; array_local_refused(&f,call);
    call=array_local_call(&f);
    array->length=5; array_local_refused(&f,call); array->length=2;
    array->capacity=SIZE_MAX; array_local_refused(&f,call); array->capacity=4;
    array->length=SIZE_MAX; array->capacity=SIZE_MAX; call.update.operation=XRT_ARRAY_UPDATE_PUSH;
    array_local_refused(&f,call); array->length=2; array->capacity=4;
    array_local_done(&f);
}
static void array_local_cow_and_growth(void) {
    ArrayLocalFixture f; array_local_init(&f,2); ArrayLocalCall call=array_local_call(&f);
    XrtManagedHeader *snapshot=&array_local_backing(&f)->managed; CHECK(xrt_managed_retain(snapshot));
    XrtManagedHeader *owned=snapshot; OK(xrt_managed_slot_adopt(f.base.frame,0,&owned)); CHECK(!owned);
    array_local_refused(&f,call);
    XrtArrayUpdate edit={XRT_ARRAY_UPDATE_SET,1,{XRT_BACKING_ARRAY_I64,{.i64=31}}};
    XrtBillingStats before=xrt_billing_owner_stats(f.base.billing); xrt_billing_owner_fail_after(f.base.billing,0);
    CHECK(xrt_array_update_place((XrtPlace){.frame=f.view.frame,.slot=0,.nulls=XRT_PLACE_NONNULL},
        XRT_BACKING_ARRAY_I64,f.view.frame,1,&f.base.types,&edit)==XRT_MANAGED_OOM);
    CHECK(&array_local_backing(&f)->managed==snapshot && array_local_element(array_local_backing(&f),1)==13 &&
        atomic_load_explicit(&snapshot->references,memory_order_relaxed)==2 && !*f.view.busy &&
        !((ArrayLocalHomes *)f.view.payload)->workspace); same_bill(f.base.billing,before);
    xrt_billing_owner_fail_after(f.base.billing,SIZE_MAX);
    OK(xrt_array_update_place((XrtPlace){.frame=f.view.frame,.slot=0,.nulls=XRT_PLACE_NONNULL},
        XRT_BACKING_ARRAY_I64,f.view.frame,1,&f.base.types,&edit));
    CHECK(array_local_backing(&f)!=(XrtArrayBacking *)snapshot && array_local_element((XrtArrayBacking *)snapshot,1)==13 &&
        array_local_element(array_local_backing(&f),1)==31);
    call.update.value=37; CHECK(array_local_try(call));
    CHECK(array_local_element(array_local_backing(&f),1)==37 && array_local_element((XrtArrayBacking *)snapshot,1)==13);
    OK(xrt_managed_slot_drop(f.base.frame,0));
    array_local_done(&f);
    array_local_init(&f,4); call=array_local_call(&f); call.update.operation=XRT_ARRAY_UPDATE_PUSH; call.update.value=41;
    array_local_refused(&f,call);
    edit=(XrtArrayUpdate){XRT_ARRAY_UPDATE_PUSH,0,{XRT_BACKING_ARRAY_I64,{.i64=41}}};
    before=xrt_billing_owner_stats(f.base.billing); xrt_billing_owner_fail_after(f.base.billing,0);
    CHECK(xrt_array_update_place((XrtPlace){.frame=f.view.frame,.slot=0,.nulls=XRT_PLACE_NONNULL},
        XRT_BACKING_ARRAY_I64,f.view.frame,1,&f.base.types,&edit)==XRT_MANAGED_OOM);
    CHECK(array_local_backing(&f)->length==4 && array_local_backing(&f)->capacity==4 &&
        array_local_element(array_local_backing(&f),3)==19 && !*f.view.busy &&
        !((ArrayLocalHomes *)f.view.payload)->workspace); same_bill(f.base.billing,before);
    xrt_billing_owner_fail_after(f.base.billing,SIZE_MAX);
    OK(xrt_array_update_place((XrtPlace){.frame=f.view.frame,.slot=0,.nulls=XRT_PLACE_NONNULL},
        XRT_BACKING_ARRAY_I64,f.view.frame,1,&f.base.types,&edit));
    CHECK(array_local_backing(&f)->length==5 && array_local_backing(&f)->capacity>=5 &&
        array_local_element(array_local_backing(&f),0)==11 && array_local_element(array_local_backing(&f),4)==41);
    call.update.operation=XRT_ARRAY_UPDATE_SET; call.update.index=4; call.update.value=43;
    CHECK(array_local_try(call)); CHECK(array_local_element(array_local_backing(&f),4)==43);
    array_local_done(&f);
}
static void array_local_updates(void) {
    array_local_success(); array_local_guards(); array_local_cow_and_growth();
    puts("local I64 Array edits: real Frame guards, snapshot COW, growth fallback and refunds passed");
}
