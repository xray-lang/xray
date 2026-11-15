/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * backing_place_cases.h - Real binding edits under registered Root transactions
 */
typedef struct ArrayPlaceWatch {
    Fixture *fixture;
    XrtPlace place;
    XrtPlaceReplacement *transaction;
    XrtArrayUpdate update;
    XrtManagedHeader *old;
    size_t allocations, retirements;
} ArrayPlaceWatch;
static void array_place_observe(void *context, void *memory, bool allocating) {
    ArrayPlaceWatch *watch=context;
    XrtManagedHeader *before=borrow(watch->fixture,0);
    BAD(xrt_array_update_place(watch->place,watch->update.item.kind,watch->fixture->frame,1,
        &watch->fixture->types,&watch->update));
    const XrtManagedHeader *unpublished=NULL;
    BAD(xrt_place_borrow(watch->place,&unpublished)); CHECK(!unpublished);
    CHECK(borrow(watch->fixture,0)==before);
    if (watch->transaction) {
        BAD(xrt_place_replacement_end(watch->transaction));
        BAD(xrt_place_replacement_publish(watch->transaction));
        XrtManagedHeader *payload=NULL;
        BAD(xrt_managed_slot_reserved_prepare(watch->transaction,watch->fixture->types.arrays[0],32,&payload));
        CHECK(!payload);
    }
    if (allocating) { CHECK(before==watch->old); ++watch->allocations; }
    if (memory==watch->old) {
        CHECK(before!=watch->old && !borrow(watch->fixture,1));
        ++watch->retirements;
    }
}
static void array_place_scalar(Fixture *fixture, XrtBackingArrayKind kind, size_t capacity) {
    int64_t i64=7; uint64_t u64=7; uint8_t u8=7;
    if (kind==XRT_BACKING_ARRAY_I64) OK(xrt_array_backing_i64_create(fixture->frame,0,&fixture->types,&i64,1,capacity));
    else if (kind==XRT_BACKING_ARRAY_U8) OK(xrt_array_backing_u8_create(fixture->frame,0,&fixture->types,&u8,1,capacity));
    else OK(xrt_array_backing_u64_create(fixture->frame,0,&fixture->types,&u64,1,capacity));
}
static void array_place_updates(void) {
    const XrtBackingArrayKind kinds[]={XRT_BACKING_ARRAY_I64,XRT_BACKING_ARRAY_U8,XRT_BACKING_ARRAY_U64};
    for (size_t k=0;k<3;++k) {
        Fixture f; fixture_init(&f,SIZE_MAX); XrtBackingArrayKind kind=kinds[k]; array_place_scalar(&f,kind,2);
        XrtManagedHeader *original=borrow(&f,0);
        XrtPlace binding={.frame=f.frame,.slot=0,.nulls=XRT_PLACE_NONNULL};
        XrtArrayUpdate edit={.operation=XRT_ARRAY_UPDATE_SET,.item={.kind=kind}};
        if (kind==XRT_BACKING_ARRAY_I64) edit.item.value.i64=42;
        else if (kind==XRT_BACKING_ARRAY_U8) edit.item.value.u8=42;
        else edit.item.value.u64=42;
        XrtBillingStats baseline=xrt_billing_owner_stats(f.billing); xrt_billing_owner_fail_after(f.billing,0);
        OK(xrt_array_update_place(binding,kind,f.frame,1,&f.types,&edit));
        CHECK(borrow(&f,0)==original && xr_signed_rc_read(&original->references)==1);
        CHECK(xrt_billing_owner_stats(f.billing).allocations==baseline.allocations); same_bill(f.billing,baseline);
        edit.operation=XRT_ARRAY_UPDATE_PUSH; OK(xrt_array_update_place(binding,kind,f.frame,1,&f.types,&edit));
        CHECK(borrow(&f,0)==original && ((XrtArrayBacking *)original)->length==2 && !borrow(&f,1));
        baseline=xrt_billing_owner_stats(f.billing);
        CHECK(xrt_array_update_place(binding,kind,f.frame,1,&f.types,&edit)==XRT_MANAGED_OOM);
        CHECK(borrow(&f,0)==original && ((XrtArrayBacking *)original)->length==2 && !borrow(&f,1)); same_bill(f.billing,baseline);
        xrt_billing_owner_fail_after(f.billing,SIZE_MAX);
        ArrayPlaceWatch watch={.fixture=&f,.place=binding,.update=edit,.old=original};
        f.audit.observe=array_place_observe; f.audit.observer=&watch;
        OK(xrt_array_update_place(binding,kind,f.frame,1,&f.types,&edit));
        f.audit.observe=NULL; CHECK(watch.allocations && watch.retirements==1 && borrow(&f,0)!=original);
        XrtArrayBacking *changed=(XrtArrayBacking *)borrow(&f,0); CHECK(changed->length==3);
        OK(xrt_backing_slot_copy(f.frame,2,&changed->managed));
        baseline=xrt_billing_owner_stats(f.billing); xrt_billing_owner_fail_after(f.billing,0);
        edit.operation=XRT_ARRAY_UPDATE_SET;
        CHECK(xrt_array_update_place(binding,kind,f.frame,1,&f.types,&edit)==XRT_MANAGED_OOM);
        CHECK(borrow(&f,0)==&changed->managed && borrow(&f,2)==&changed->managed && !borrow(&f,1)); same_bill(f.billing,baseline);
        xrt_billing_owner_fail_after(f.billing,SIZE_MAX);
        OK(xrt_array_update_place(binding,kind,f.frame,1,&f.types,&edit));
        CHECK(borrow(&f,0)!=&changed->managed && borrow(&f,2)==&changed->managed && !borrow(&f,1));
        edit.index=-1; CHECK(xrt_array_update_place(binding,kind,f.frame,1,&f.types,&edit)==XRT_MANAGED_BOUNDS);
        edit.index=0;
        BAD(xrt_array_update_place(binding,XRT_BACKING_ARRAY_STRING,f.frame,1,&f.types,&edit));
        BAD(xrt_array_update_place(binding,kind,f.frame,0,&f.types,&edit));
        fixture_done(&f);
    }
    Fixture strings; fixture_init(&strings,SIZE_MAX);
    OK(xrt_string_backing_create(strings.frame,3,&strings.types,"old",3,0));
    OK(xrt_string_backing_create(strings.frame,4,&strings.types,"new",3,0));
    XrtStringBacking *old=(XrtStringBacking *)borrow(&strings,3),*next=(XrtStringBacking *)borrow(&strings,4);
    OK(xrt_array_backing_string_create(strings.frame,0,&strings.types,&old,1,2));
    XrtArrayUpdate edit={XRT_ARRAY_UPDATE_SET,0,{.kind=XRT_BACKING_ARRAY_STRING,.value.string=next}};
    XrtPlace binding={.frame=strings.frame,.slot=0,.nulls=XRT_PLACE_NONNULL};
    XrtManagedHeader *array=borrow(&strings,0); xrt_billing_owner_fail_after(strings.billing,0);
    int32_t unique_refs=xr_signed_rc_read(&next->managed.references);
    atomic_store(&next->managed.references,INT32_MIN);
    CHECK(xrt_array_update_place(binding,XRT_BACKING_ARRAY_STRING,strings.frame,1,&strings.types,&edit)==XRT_MANAGED_REFCOUNT);
    CHECK(borrow(&strings,0)==array && tail((XrtArrayBacking *)array,0)==&old->managed && !borrow(&strings,1));
    atomic_store(&next->managed.references,unique_refs);
    OK(xrt_array_update_place(binding,XRT_BACKING_ARRAY_STRING,strings.frame,1,&strings.types,&edit));
    CHECK(borrow(&strings,0)==array && tail((XrtArrayBacking *)array,0)==&next->managed);
    CHECK(xr_signed_rc_read(&old->managed.references)==1 && xr_signed_rc_read(&next->managed.references)==2);
    OK(xrt_backing_slot_copy(strings.frame,2,array));
    XrtBillingStats baseline=xrt_billing_owner_stats(strings.billing);
    CHECK(xrt_array_update_place(binding,XRT_BACKING_ARRAY_STRING,strings.frame,1,&strings.types,&edit)==XRT_MANAGED_OOM);
    same_bill(strings.billing,baseline); CHECK(!borrow(&strings,1));
    xrt_billing_owner_fail_after(strings.billing,SIZE_MAX);
    int32_t refs=xr_signed_rc_read(&next->managed.references);
    atomic_store(&next->managed.references,INT32_MIN);
    CHECK(xrt_array_update_place(binding,XRT_BACKING_ARRAY_STRING,strings.frame,1,&strings.types,&edit)==XRT_MANAGED_REFCOUNT);
    CHECK(borrow(&strings,0)==array && !borrow(&strings,1)); same_bill(strings.billing,baseline);
    atomic_store(&next->managed.references,refs);
    OK(xrt_array_update_place(binding,XRT_BACKING_ARRAY_STRING,strings.frame,1,&strings.types,&edit));
    CHECK(borrow(&strings,0)!=array && tail((XrtArrayBacking *)borrow(&strings,0),0)==&next->managed);
    fixture_done(&strings);
    Fixture child; fixture_init(&child,SIZE_MAX);
    OK(xrt_string_backing_create(child.frame,3,&child.types,"retire",6,0));
    OK(xrt_string_backing_create(child.frame,4,&child.types,"live",4,0));
    old=(XrtStringBacking *)borrow(&child,3); next=(XrtStringBacking *)borrow(&child,4);
    OK(xrt_array_backing_string_create(child.frame,0,&child.types,&old,1,1));
    OK(xrt_managed_slot_drop(child.frame,3));
    edit=(XrtArrayUpdate){XRT_ARRAY_UPDATE_SET,0,{.kind=XRT_BACKING_ARRAY_STRING,.value.string=next}};
    binding=(XrtPlace){.frame=child.frame,.slot=0,.nulls=XRT_PLACE_NONNULL};
    ArrayPlaceWatch watch={.fixture=&child,.place=binding,.update=edit,.old=&old->managed};
    child.audit.observe=array_place_observe; child.audit.observer=&watch;
    xrt_billing_owner_fail_after(child.billing,0);
    OK(xrt_array_update_place(binding,XRT_BACKING_ARRAY_STRING,child.frame,1,&child.types,&edit));
    child.audit.observe=NULL; CHECK(watch.retirements==1 && !watch.allocations);
    CHECK(tail((XrtArrayBacking *)borrow(&child,0),0)==&next->managed); fixture_done(&child);
}
typedef struct ArrayFieldGuard { XrtManagedHeader *owner; bool busy,ready,mutable; size_t borrows,replaces; } ArrayFieldGuard;
static XrtManagedStatus array_field_guard(void *context, XrtPlaceAction action) {
    ArrayFieldGuard *field=context;
    CHECK(field->busy);
    if (action==XRT_PLACE_BORROW) ++field->borrows;
    if (action==XRT_PLACE_REPLACE) ++field->replaces;
    return field->ready && (action!=XRT_PLACE_REPLACE || field->mutable) && action!=XRT_PLACE_MOVE_FROM ?
        XRT_MANAGED_OK : XRT_MANAGED_ARGUMENT;
}
static void array_place_guards(void) {
    Fixture left,right; fixture_init(&left,SIZE_MAX); fixture_init(&right,SIZE_MAX);
    array_place_scalar(&left,XRT_BACKING_ARRAY_I64,1);
    XrtManagedHeader *prior=borrow(&left,0);
    XrtArrayUpdate grow={XRT_ARRAY_UPDATE_PUSH,0,{.kind=XRT_BACKING_ARRAY_I64,.value.i64=8}};
    OK(xrt_array_update_place((XrtPlace){.frame=left.frame,.slot=0,.nulls=XRT_PLACE_NONNULL},
        XRT_BACKING_ARRAY_I64,right.frame,1,&right.types,&grow));
    CHECK(borrow(&left,0)!=prior && !borrow(&right,1));
    fixture_retire(&right); CHECK(!atomic_load(&right.audit.finalized)); fixture_done(&left);
    CHECK(atomic_load(&right.audit.finalized)==1 && !atomic_load(&right.audit.bytes) && !atomic_load(&right.audit.blocks));
    Fixture source,destination; fixture_init(&source,SIZE_MAX); fixture_init(&destination,SIZE_MAX);
    array_place_scalar(&source,XRT_BACKING_ARRAY_I64,2);
    ArrayFieldGuard field={.ready=true}; OK(xrt_managed_slot_take(source.frame,0,&field.owner));
    XrtExternalOwner external={&field.owner,&field.busy,array_field_guard,&field};
    XrtPlace binding={.external=&external,.nulls=XRT_PLACE_NONNULL};
    const XrtManagedHeader *borrowed=NULL; OK(xrt_place_borrow(binding,&borrowed)); CHECK(borrowed==field.owner && field.borrows==1);
    XrtArrayUpdate edit={XRT_ARRAY_UPDATE_SET,0,{.kind=XRT_BACKING_ARRAY_I64,.value.i64=99}};
    BAD(xrt_array_update_place(binding,XRT_BACKING_ARRAY_I64,destination.frame,1,&destination.types,&edit));
    CHECK(field.replaces==1 && !field.busy && !borrow(&destination,1)); field.mutable=true; field.ready=false;
    BAD(xrt_array_update_place(binding,XRT_BACKING_ARRAY_I64,destination.frame,1,&destination.types,&edit)); field.ready=true;
    xrt_billing_owner_fail_after(destination.billing,0);
    OK(xrt_array_update_place(binding,XRT_BACKING_ARRAY_I64,destination.frame,1,&destination.types,&edit)); CHECK(field.owner==borrowed);
    edit.operation=XRT_ARRAY_UPDATE_PUSH; OK(xrt_array_update_place(binding,XRT_BACKING_ARRAY_I64,destination.frame,1,&destination.types,&edit));
    xrt_billing_owner_close(destination.billing);
    CHECK(xrt_array_update_place(binding,XRT_BACKING_ARRAY_I64,destination.frame,1,&destination.types,&edit)==XRT_MANAGED_CLOSED);
    CHECK(field.owner==borrowed && !field.busy && !borrow(&destination,1));
    edit.operation=XRT_ARRAY_UPDATE_SET;
    OK(xrt_array_update_place(binding,XRT_BACKING_ARRAY_I64,destination.frame,1,&destination.types,&edit));
    xrt_managed_drop(field.owner); field.owner=NULL; fixture_done(&source); fixture_done(&destination);
}
static XrtManagedStatus array_token_reentry(XrtManagedHeader *value, void *context) {
    XrtPlaceReplacement *token=context; CHECK(value);
    BAD(xrt_place_replacement_end(token)); BAD(xrt_place_replacement_publish(token)); return XRT_MANAGED_OK;
}
static void array_place_tokens(void) {
    Fixture f,other; fixture_init(&f,SIZE_MAX); fixture_init(&other,SIZE_MAX); array_place_scalar(&f,XRT_BACKING_ARRAY_I64,1);
    XrtPlace binding={.frame=f.frame,.slot=0,.nulls=XRT_PLACE_NONNULL};
    XrtManagedHeader *pending=NULL; OK(xrt_managed_slot_prepare(f.frame,1,f.types.arrays[0],32,&pending));
    XrtArrayUpdate edit={XRT_ARRAY_UPDATE_SET,0,{.kind=XRT_BACKING_ARRAY_I64,.value.i64=11}};
    XrtManagedHeader *before=borrow(&f,0);
    BAD(xrt_array_update_place(binding,XRT_BACKING_ARRAY_I64,f.frame,1,&f.types,&edit));
    XrtManagedSlotLocation location; OK(xrt_managed_slot_location(f.frame,1,&location));
    CHECK(location.pending->value==pending && borrow(&f,0)==before); OK(xrt_managed_slot_abort(f.frame,1));
    XrtPlaceReplacement token={0}; OK(xrt_place_replacement_begin(binding,f.frame,1,&token));
    XrtPlaceReplacement copied=token; BAD(xrt_place_replacement_end(&copied));
    BAD(xrt_place_replacement_publish(&copied)); copied.root=other.root; BAD(xrt_place_replacement_end(&copied));
    BAD(xrt_place_replacement_begin(binding,other.frame,1,&token));
    OK(xrt_managed_slot_reserved_apply(&token,array_token_reentry,&token));
    ArrayPlaceWatch watch={.fixture=&f,.place=binding,.transaction=&token,.old=borrow(&f,0),
        .update={XRT_ARRAY_UPDATE_PUSH,0,{.kind=XRT_BACKING_ARRAY_I64,.value.i64=3}}};
    f.audit.observe=array_place_observe; f.audit.observer=&watch;
    XrtManagedHeader *candidate=NULL; OK(xrt_managed_slot_reserved_prepare(&token,f.types.arrays[0],32+4*8,&candidate));
    CHECK(candidate && watch.allocations); OK(xrt_managed_slot_reserved_commit(&token));
    OK(xrt_place_replacement_publish(&token)); CHECK(watch.retirements==1);
    f.audit.observe=NULL; BAD(xrt_place_replacement_publish(&token));
    OK(xrt_place_replacement_end(&token)); BAD(xrt_place_replacement_end(&token));
    XrtManagedHeader *stable=borrow(&f,0); XrtBillingStats baseline=xrt_billing_owner_stats(f.billing);
    OK(xrt_place_replacement_begin(binding,f.frame,1,&token)); candidate=NULL;
    OK(xrt_managed_slot_reserved_prepare(&token,f.types.arrays[0],32+8,&candidate));
    ((XrtArrayBacking *)candidate)->capacity=SIZE_MAX;
    BAD(xrt_managed_slot_reserved_commit(&token));
    OK(xrt_place_replacement_end(&token));
    CHECK(borrow(&f,0)==stable && !borrow(&f,1)); same_bill(f.billing,baseline);
    fixture_done(&f); fixture_done(&other);
}
