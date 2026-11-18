/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * execution_assertions.inc.c - Independent assertion-message failure ownership
 */
typedef struct AssertionCall {
    XrtBillingOwner *billing;
    XrtManagedHeader *saturated;
    const char *bytes;
    size_t length, allocations;
    uint8_t condition;
    bool overflow, ready, after;
} AssertionCall;
static void assertion_root(void *context) {
    AssertionCall *call=context;
    XrtManagedRoot *root=xrt_native_managed_root(1);
    XrtManagedFrame *frame=NULL;
    managed_require(xrt_managed_frame_enter(root,1,&frame));
    xrt_native_string_create(frame,0,call->bytes,call->length,1);
    XrtManagedHeader *message=NULL;
    managed_require(xrt_managed_slot_borrow(frame,0,&message));
    call->ready=true;
    call->allocations=xrt_billing_owner_stats(call->billing).allocations;
    xrt_billing_owner_fail_after(call->billing,0);
    if (call->overflow) {
        call->saturated=message;
        atomic_store_explicit(&message->references,INT32_MAX,memory_order_relaxed);
    }
    xrt_assert_condition(call->condition,(const XrtStringBacking *)message,1);
    call->after=true;
}
static void managed_assertion_owner(void) {
    static const char binary[]="a\0\xe4\xb8\xad";
    const char *messages[]={"",binary,"ordinary assertion message"};
    size_t lengths[]={0,sizeof(binary)-1,sizeof("ordinary assertion message")-1};
    XrtLocationOwner *map=locations("assertion-owner.xr");
    for (uint32_t sample=0;sample<3;++sample) for (uint8_t condition=0;condition<=1;++condition) {
        bool completed=false;
        for (size_t allowance=0;allowance<64;++allowance) {
            XrtValueRoot owner={0}; XrtBillingOwner *billing=NULL; XrtExecutionFailure failure={0};
            CHECK(xrt_native_values_prepare(&owner,&billing)==XRT_MANAGED_OK && xrt_billing_owner_retain(billing));
#if defined(XR_NATIVE_VALUE_ALLOCATIONS)
            xrt_billing_owner_fail_after(billing,allowance);
#else
            (void)allowance;
#endif
            AssertionCall call={.billing=billing,.bytes=messages[sample],.length=lengths[sample],.condition=condition};
            XrtExecutionStatus status=xrt_run_embedded_owned(map,&failure,assertion_root,&call,&owner);
            CHECK(xrt_value_root_empty(&owner) && !xrt_value_root_active());
            completed=call.ready;
            if (completed) {
                CHECK(xrt_billing_owner_stats(billing).allocations==call.allocations);
                if (condition) CHECK(status==XRT_EXECUTION_COMPLETED && call.after);
                else {
                    CHECK(status==XRT_EXECUTION_FAILURE && !call.after && failure.kind==XRT_FAILURE_MESSAGE_PANIC);
                    const XrtStringBacking *message=(const XrtStringBacking *)failure.detail.message.message;
                    CHECK(failure.detail.message.code==445 && failure.detail.message.source_id==1 && message &&
                        message->length==lengths[sample] && !memcmp(message->bytes,messages[sample],lengths[sample]) &&
                        xr_signed_rc_read(&message->managed.references)==1);
                }
            } else CHECK(status==XRT_EXECUTION_FAILURE && !call.after && failure.kind==XRT_FAILURE_VALUE &&
                failure.detail.value.status==XR_XIR_VALUE_OOM);
            xrt_execution_failure_drop(&failure); xrt_execution_failure_drop(&failure);
            CHECK(xrt_billing_owner_stats(billing).blocks==1);
            xrt_billing_owner_drop(billing);
            if (completed) break;
        }
        CHECK(completed);
    }
    XrtValueRoot owner={0}; XrtBillingOwner *billing=NULL; XrtExecutionFailure failure={0};
    CHECK(xrt_native_values_prepare(&owner,&billing)==XRT_MANAGED_OK && xrt_billing_owner_retain(billing));
    AssertionCall overflow={.billing=billing,.bytes=binary,.length=sizeof(binary)-1,.overflow=true};
    CHECK(xrt_run_embedded_owned(map,&failure,assertion_root,&overflow,&owner)==XRT_EXECUTION_FAILURE);
    CHECK(overflow.ready && !overflow.after && failure.kind==XRT_FAILURE_VALUE &&
        failure.detail.value.status==XR_XIR_VALUE_REFCOUNT_LIMIT && failure.detail.value.source_id==1);
    CHECK(overflow.saturated && xr_signed_rc_read(&overflow.saturated->references)==INT32_MAX-1);
    /* Restore the injected counter after the real frame relinquishes its one reference. */
    atomic_store_explicit(&overflow.saturated->references,1,memory_order_relaxed);
    xrt_managed_drop(overflow.saturated);
    xrt_execution_failure_drop(&failure);
    CHECK(xrt_value_root_empty(&owner) && xrt_billing_owner_stats(billing).blocks==1);
    xrt_billing_owner_drop(billing);
    xrt_locations_release(map);
}
