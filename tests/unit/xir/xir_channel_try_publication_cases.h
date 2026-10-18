/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_try_publication_cases.h - Ordinary message argument ownership on every outcome
 */
#ifndef XIR_CHANNEL_TRY_PUBLICATION_CASES_H
#define XIR_CHANNEL_TRY_PUBLICATION_CASES_H
static void try_copy_prepare(ResultOwner *owner,XrXirValue *channel,XrXirValue *message) {
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_DEEP,1,&owner->admission,channel)==XR_XIR_VALUE_OK);
    result_deep_payload(owner,message);
}
static size_t try_copy_fault(size_t ordinal) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},message={0},output={0};
    try_copy_prepare(&owner,&channel,&message);int64_t payload=message.payload;
    XirChannel *storage=(XirChannel *)object_pointer(&channel);XrXirChannelState state=XR_XIR_CHANNEL_VALUE;
    size_t begin=runtime_memory.attempts,live=runtime_memory.live,bytes=runtime_memory.bytes;
    if (ordinal!=SIZE_MAX) runtime_memory.fail_at=begin+ordinal;
    XrXirValueStatus status=xr_xir_channel_try_send(&channel,&message,(XrXirType)RESULT_SEND,&owner.admission,&output,&state);
    size_t sites=runtime_memory.attempts-begin;runtime_memory.fail_at=SIZE_MAX;
    CHECK(message.type==RESULT_DEEP_ARRAY && message.payload==payload);
    if (ordinal!=SIZE_MAX) {
        CHECK(status==XR_XIR_VALUE_OOM && sites>ordinal && state==XR_XIR_CHANNEL_VALUE && result_empty(&output));
        CHECK(!storage->buffer.count && runtime_memory.live==live && runtime_memory.bytes==bytes);
        CHECK(xr_xir_channel_try_send(&channel,&message,(XrXirType)RESULT_SEND,&owner.admission,&output,&state)==XR_XIR_VALUE_OK);
    } else CHECK(status==XR_XIR_VALUE_OK);
    CHECK(state==XR_XIR_CHANNEL_SENT && storage->buffer.count==1 && message.payload==payload);result_variant(&output,0,0);
    xr_xir_value_drop(&output);
    CHECK(xr_xir_channel_try_send(&channel,&message,(XrXirType)RESULT_SEND,&owner.admission,&output,&state)==XR_XIR_VALUE_OK &&
        state==XR_XIR_CHANNEL_FULL && message.payload==payload);result_variant(&output,1,0);xr_xir_value_drop(&output);
    CHECK(xr_xir_channel_close(&channel,&owner.admission,&state)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_channel_try_send(&channel,&message,(XrXirType)RESULT_SEND,&owner.admission,&output,&state)==XR_XIR_VALUE_OK &&
        state==XR_XIR_CHANNEL_CLOSED && message.payload==payload);result_variant(&output,3,0);
    xr_xir_value_drop(&message);xr_xir_value_drop(&output);xr_xir_value_drop(&channel);result_owner_free(&owner);return sites;
}
static void try_copy_work(void) {
    uint64_t required=0;
    for (uint64_t cut=UINT64_MAX;;++cut) {
        ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},message=result_integer(67),output={0};
        CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
        uint64_t begin=owner.admission.work;XrXirChannelState state=XR_XIR_CHANNEL_VALUE;
        if (cut!=UINT64_MAX) owner.admission.work=cut;
        XrXirValueStatus status=xr_xir_channel_try_send(&channel,&message,(XrXirType)RESULT_SEND,&owner.admission,&output,&state);
        CHECK(message.type==XR_XIR_I64 && message.payload==67);
        if (cut==UINT64_MAX) { CHECK(status==XR_XIR_VALUE_OK);required=begin-owner.admission.work;CHECK(required && required<128); }
        else {
            CHECK(status==(cut==required ? XR_XIR_VALUE_OK : XR_XIR_VALUE_LIMIT));
            if (cut<required) CHECK(result_empty(&output) && state==XR_XIR_CHANNEL_VALUE && !((XirChannel *)object_pointer(&channel))->buffer.count);
            else CHECK(state==XR_XIR_CHANNEL_SENT);
        }
        xr_xir_value_drop(&output);xr_xir_value_drop(&message);xr_xir_value_drop(&channel);result_owner_free(&owner);
        if (cut==required) break;
    }
    printf("Channel ordinary trySend work exact=%" PRIu64 " prefixes=%" PRIu64 " source-preserved physical0\n",required,required+1);
}
static void try_copy_rejections(void) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},message=result_integer(71),output={0};
    XrXirChannelState state=XR_XIR_CHANNEL_VALUE;
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    owner.admission.work=0;
    CHECK(xr_xir_channel_try_send(&channel,&message,(XrXirType)RESULT_SEND,&owner.admission,NULL,&state)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_channel_try_send(&channel,&message,(XrXirType)RESULT_SEND,&owner.admission,&message,&state)==XR_XIR_VALUE_BAD_ARGUMENT);
    owner.admission.work=128000000;
    XirChannel *storage=(XirChannel *)object_pointer(&channel);atomic_store_explicit(&storage->locked,true,memory_order_relaxed);
    CHECK(xr_xir_channel_try_send(&channel,&message,(XrXirType)RESULT_SEND,&owner.admission,&output,&state)==XR_XIR_VALUE_OK &&
        state==XR_XIR_CHANNEL_RETRY && result_empty(&output) && message.payload==71 && !storage->buffer.count);
    atomic_store_explicit(&storage->locked,false,memory_order_relaxed);
    xr_xir_value_drop(&message);xr_xir_value_drop(&channel);result_owner_free(&owner);
}
#endif // XIR_CHANNEL_TRY_PUBLICATION_CASES_H
