/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * backing_set_cases.h - Actual I64 Set order, COW, failure refunds and leases
 */
#include "runtime/native/xnative_set_backing.h"
static void set_order(const XrtSetBacking *set, const int64_t *values, size_t count) {
    CHECK(set->length==count);
    for (size_t n=0;n<count;++n) {
        int64_t value=0; bool found=false;
        OK(xrt_set_backing_at(set,n,&value)); CHECK(value==values[n]);
        OK(xrt_set_backing_contains(set,value,&found)); CHECK(found);
    }
    int64_t unchanged=77; BAD(xrt_set_backing_at(set,count,&unchanged)); CHECK(unchanged==77);
}
static void set_values_and_transactions(void) {
    Fixture f; fixture_init(&f,SIZE_MAX);
    int64_t input[]={0,INT64_MIN,INT64_MAX,0}, order[]={0,INT64_MIN,INT64_MAX};
    OK(xrt_set_backing_create(f.frame,0,&f.types,input,4));
    XrtSetBacking *first=(XrtSetBacking *)borrow(&f,0); set_order(first,order,3);
    XrtPlace binding={.frame=f.frame,.slot=0,.nulls=XRT_PLACE_NONNULL}; bool removed=true;
    XrtSetUpdate edit={XRT_SET_UPDATE_REMOVE,INT64_MIN};
    XrtBillingStats baseline=xrt_billing_owner_stats(f.billing); xrt_billing_owner_fail_after(f.billing,0);
    OK(xrt_set_update_place(binding,f.frame,1,&f.types,&edit,&removed)); CHECK(removed && borrow(&f,0)==&first->managed);
    edit=(XrtSetUpdate){XRT_SET_UPDATE_ADD,INT64_MIN};
    OK(xrt_set_update_place(binding,f.frame,1,&f.types,&edit,&removed)); CHECK(!removed && borrow(&f,0)==&first->managed);
    int64_t reinserted[]={0,INT64_MAX,INT64_MIN}; set_order(first,reinserted,3); same_bill(f.billing,baseline);
    CHECK(xrt_billing_owner_stats(f.billing).allocations==baseline.allocations);
    OK(xrt_backing_slot_copy(f.frame,2,&first->managed));
    edit.key=0; OK(xrt_set_update_place(binding,f.frame,1,&f.types,&edit,&removed));
    CHECK(borrow(&f,0)==borrow(&f,2) && !removed);
    edit=(XrtSetUpdate){XRT_SET_UPDATE_REMOVE,17};
    OK(xrt_set_update_place(binding,f.frame,1,&f.types,&edit,&removed)); CHECK(!removed);
    baseline=xrt_billing_owner_stats(f.billing); edit=(XrtSetUpdate){XRT_SET_UPDATE_ADD,42}; removed=true;
    CHECK(xrt_set_update_place(binding,f.frame,1,&f.types,&edit,&removed)==XRT_MANAGED_OOM);
    CHECK(removed && borrow(&f,0)==&first->managed && borrow(&f,2)==&first->managed && !borrow(&f,1));
    set_order(first,reinserted,3); same_bill(f.billing,baseline);
    bool completed=false;
    for (size_t allowed=0;allowed<8;++allowed) {
        xrt_billing_owner_fail_after(f.billing,allowed); removed=true;
        XrtManagedStatus status=xrt_set_update_place(binding,f.frame,1,&f.types,&edit,&removed);
        if (status==XRT_MANAGED_OK) { completed=true; CHECK(!removed); break; }
        CHECK(status==XRT_MANAGED_OOM && removed && borrow(&f,0)==&first->managed && !borrow(&f,1));
        set_order(first,reinserted,3); same_bill(f.billing,baseline);
    }
    CHECK(completed); xrt_billing_owner_fail_after(f.billing,SIZE_MAX);
    XrtSetBacking *changed=(XrtSetBacking *)borrow(&f,0);
    int64_t expanded[]={0,INT64_MAX,INT64_MIN,42}; set_order(changed,expanded,4); set_order(first,reinserted,3);
    CHECK(changed!=first && borrow(&f,2)==&first->managed && !borrow(&f,1));
    edit=(XrtSetUpdate){XRT_SET_UPDATE_ADD,43}; baseline=xrt_billing_owner_stats(f.billing);
    xrt_billing_owner_fail_after(f.billing,0);
    CHECK(xrt_set_update_place(binding,f.frame,1,&f.types,&edit,&removed)==XRT_MANAGED_OOM);
    CHECK(borrow(&f,0)==&changed->managed && !borrow(&f,1)); set_order(changed,expanded,4); same_bill(f.billing,baseline);
    xrt_billing_owner_fail_after(f.billing,SIZE_MAX);
    OK(xrt_set_update_place(binding,f.frame,1,&f.types,&edit,&removed));
    changed=(XrtSetBacking *)borrow(&f,0); CHECK(changed->length==5 && changed->capacity==16);
    BAD(xrt_set_update_place(binding,f.frame,0,&f.types,&edit,&removed));
    edit=(XrtSetUpdate){XRT_SET_UPDATE_CLEAR,0}; OK(xrt_set_update_place(binding,f.frame,1,&f.types,&edit,&removed));
    CHECK(!removed && !((XrtSetBacking *)borrow(&f,0))->length);
    fixture_done(&f);
}
static void set_growth_and_lifetimes(void) {
    Fixture f; fixture_init(&f,SIZE_MAX);
    OK(xrt_set_backing_create(f.frame,0,&f.types,NULL,0));
    XrtPlace binding={.frame=f.frame,.slot=0,.nulls=XRT_PLACE_NONNULL}; bool removed=false;
    for (int64_t n=0;n<512;++n) {
        XrtSetUpdate edit={XRT_SET_UPDATE_ADD,n}; OK(xrt_set_update_place(binding,f.frame,1,&f.types,&edit,&removed));
    }
    for (int64_t n=0;n<256;++n) {
        XrtSetUpdate edit={XRT_SET_UPDATE_REMOVE,n}; OK(xrt_set_update_place(binding,f.frame,1,&f.types,&edit,&removed)); CHECK(removed);
    }
    for (int64_t n=0;n<256;++n) {
        XrtSetUpdate edit={XRT_SET_UPDATE_ADD,n}; OK(xrt_set_update_place(binding,f.frame,1,&f.types,&edit,&removed));
    }
    XrtSetBacking *set=(XrtSetBacking *)borrow(&f,0);
    for (size_t n=0;n<512;++n) {
        int64_t value=-1; OK(xrt_set_backing_at(set,n,&value)); CHECK(value==(int64_t)((n+256)%512));
    }
    XrtManagedHeader *kept=NULL; OK(xrt_managed_slot_take(f.frame,0,&kept));
    fixture_retire(&f); CHECK(!atomic_load(&f.audit.finalized));
    bool found=false; OK(xrt_set_backing_contains((XrtSetBacking *)kept,511,&found)); CHECK(found);
    int64_t value=-1; OK(xrt_set_backing_at((XrtSetBacking *)kept,511,&value)); CHECK(value==255);
    xrt_managed_drop(kept);
    CHECK(atomic_load(&f.audit.finalized)==1 && !atomic_load(&f.audit.bytes) && !atomic_load(&f.audit.blocks));
}
