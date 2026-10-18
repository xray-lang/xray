/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_send_result_cases.h - Prepared send outcomes and queue publication controls
 */
#ifndef XIR_CHANNEL_SEND_RESULT_CASES_H
#define XIR_CHANNEL_SEND_RESULT_CASES_H
static void result_send_outcomes(void) {
    ResultOwner owner={0}; result_owner_ready(&owner);
    XrXirValue channel={0},message=result_integer(19),output={0},received={0};
    XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_channel_send_result(&channel,&message,(XrXirType)RESULT_SEND,&owner.admission,&output,&state)==XR_XIR_VALUE_OK &&
        state==XR_XIR_CHANNEL_SENT && result_empty(&message)); result_variant(&output,0,0); xr_xir_value_drop(&output);
    message=result_integer(23);
    CHECK(xr_xir_channel_send_result(&channel,&message,(XrXirType)RESULT_SEND,&owner.admission,&output,&state)==XR_XIR_VALUE_OK &&
        state==XR_XIR_CHANNEL_FULL && message.type==XR_XIR_I64 && message.payload==23);
    result_variant(&output,1,0); xr_xir_value_drop(&output);
    CHECK(xr_xir_channel_close(&channel,&owner.admission,&state)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_channel_send_result(&channel,&message,(XrXirType)RESULT_SEND,&owner.admission,&output,&state)==XR_XIR_VALUE_OK &&
        state==XR_XIR_CHANNEL_CLOSED && message.type==XR_XIR_I64 && message.payload==23);
    result_variant(&output,3,0); xr_xir_value_drop(&output);
    CHECK(xr_xir_channel_receive(&channel,&owner.admission,&received,&state)==XR_XIR_VALUE_OK &&
        state==XR_XIR_CHANNEL_VALUE && received.type==XR_XIR_I64 && received.payload==19);
    xr_xir_value_drop(&received); xr_xir_value_drop(&message); xr_xir_value_drop(&channel); result_owner_free(&owner);
}
static void result_send_rejections(void) {
    ResultOwner owner={0}; result_owner_ready(&owner);
    XrXirValue channel={0},message=result_integer(31),output={0}; XrXirChannelState state=XR_XIR_CHANNEL_VALUE;
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_channel_send_result(&channel,&message,(XrXirType)RESULT_I64,&owner.admission,&output,&state)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(result_empty(&output) && message.payload==31 && state==XR_XIR_CHANNEL_VALUE);
    output=result_integer(71);
    CHECK(xr_xir_channel_send_result(&channel,&message,(XrXirType)RESULT_SEND,&owner.admission,&output,&state)==XR_XIR_VALUE_BAD_ARGUMENT && output.payload==71);
    CHECK(xr_xir_channel_send_result(&channel,&message,(XrXirType)RESULT_SEND,&owner.admission,&message,&state)==XR_XIR_VALUE_BAD_ARGUMENT && message.payload==31);
    xr_xir_value_drop(&output);
    XirChannel *storage=(XirChannel *)object_pointer(&channel); CHECK(!storage->buffer.count);
    atomic_store_explicit(&storage->locked,true,memory_order_relaxed);
    CHECK(xr_xir_channel_send_result(&channel,&message,(XrXirType)RESULT_SEND,&owner.admission,&output,&state)==XR_XIR_VALUE_OK &&
        state==XR_XIR_CHANNEL_RETRY && result_empty(&output) && message.payload==31 && !storage->buffer.count);
    atomic_store_explicit(&storage->locked,false,memory_order_relaxed);
    xr_xir_value_drop(&message); xr_xir_value_drop(&channel); result_owner_free(&owner);
}
static void result_send_unit(void) {
    ResultOwner owner={0}; result_owner_ready(&owner);
    XrXirValue channel={0},unit={0},output={0}; XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_UNIT,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_channel_send_result(&channel,&unit,(XrXirType)RESULT_SEND,&owner.admission,&output,&state)==XR_XIR_VALUE_OK &&
        state==XR_XIR_CHANNEL_SENT && result_empty(&unit)); result_variant(&output,0,0); xr_xir_value_drop(&output);
    CHECK(xr_xir_channel_receive_result(&channel,(XrXirType)RESULT_UNIT,&owner.admission,&output,&state)==XR_XIR_VALUE_OK &&
        state==XR_XIR_CHANNEL_VALUE); result_variant(&output,0,1);
    xr_xir_value_drop(&output); xr_xir_value_drop(&channel); result_owner_free(&owner);
}
static void result_send_work(void) {
    uint64_t required=0;
    for (uint64_t cut=UINT64_MAX;;++cut) {
        ResultOwner owner={0}; result_owner_ready(&owner);
        XrXirValue channel={0},message=result_integer(37),output={0}; XrXirChannelState state=XR_XIR_CHANNEL_VALUE;
        CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
        uint64_t begin=owner.admission.work;
        if (cut!=UINT64_MAX) owner.admission.work=cut;
        XrXirValueStatus status=xr_xir_channel_send_result(&channel,&message,(XrXirType)RESULT_SEND,&owner.admission,&output,&state);
        XirChannel *storage=(XirChannel *)object_pointer(&channel);
        if (cut==UINT64_MAX) { CHECK(status==XR_XIR_VALUE_OK); required=begin-owner.admission.work; CHECK(required && required<128); }
        else {
            CHECK(status==(cut==required ? XR_XIR_VALUE_OK : XR_XIR_VALUE_LIMIT));
            if (cut<required) CHECK(result_empty(&output) && message.type==XR_XIR_I64 && message.payload==37 &&
                state==XR_XIR_CHANNEL_VALUE && !storage->buffer.count);
            else CHECK(state==XR_XIR_CHANNEL_SENT && result_empty(&message) && storage->buffer.count==1);
        }
        xr_xir_value_drop(&output); xr_xir_value_drop(&message); xr_xir_value_drop(&channel); result_owner_free(&owner);
        if (cut==required) break;
    }
    printf("Channel SendResult publication work exact=%" PRIu64 " prefixes=%" PRIu64 " physical=0\n",required,required+1);
}
static size_t result_send_publication_fault(size_t ordinal) {
    ResultOwner owner={0}; result_owner_ready(&owner);
    XrXirValue channel={0},message={0},output={0}; XrXirChannelState state=XR_XIR_CHANNEL_VALUE;
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_DEEP,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    result_deep_payload(&owner,&message); int64_t payload=message.payload;
    XirChannel *storage=(XirChannel *)object_pointer(&channel);
    size_t begin=runtime_memory.attempts,live=runtime_memory.live,bytes=runtime_memory.bytes;
    XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(owner.domain);
    if (ordinal!=SIZE_MAX) runtime_memory.fail_at=begin+ordinal;
    XrXirValueStatus status=xr_xir_channel_send_result(&channel,&message,(XrXirType)RESULT_SEND,&owner.admission,&output,&state);
    size_t sites=runtime_memory.attempts-begin; runtime_memory.fail_at=SIZE_MAX;
    if (ordinal!=SIZE_MAX) {
        CHECK(sites>ordinal && status==XR_XIR_VALUE_OOM && state==XR_XIR_CHANNEL_VALUE && result_empty(&output));
        CHECK(!storage->buffer.count && message.type==RESULT_DEEP_ARRAY && message.payload==payload);
        CHECK(runtime_memory.live==live && runtime_memory.bytes==bytes);
        XrXirDomainBudgetStats failed=xr_xir_domain_budget_stats(owner.domain);
        CHECK(failed.requested_bytes>before.requested_bytes && failed.work>before.work);
        CHECK(xr_xir_channel_send_result(&channel,&message,(XrXirType)RESULT_SEND,&owner.admission,&output,&state)==XR_XIR_VALUE_OK);
        XrXirDomainBudgetStats retry=xr_xir_domain_budget_stats(owner.domain);
        CHECK(retry.requested_bytes>failed.requested_bytes && retry.work>failed.work);
    } else CHECK(status==XR_XIR_VALUE_OK);
    CHECK(state==XR_XIR_CHANNEL_SENT && result_empty(&message) && storage->buffer.count==1); result_variant(&output,0,0);
    xr_xir_value_drop(&output); xr_xir_value_drop(&channel); result_owner_free(&owner); return sites;
}
#endif // XIR_CHANNEL_SEND_RESULT_CASES_H
