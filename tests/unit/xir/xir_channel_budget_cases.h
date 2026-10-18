#ifndef XIR_CHANNEL_BUDGET_CASES_H
#define XIR_CHANNEL_BUDGET_CASES_H
/* This fixture applies every local work prefix to real queue operations. */
static XrXirValueStatus budget_operation(unsigned mode, ChannelOwner *owner,
    XrXirValue *channel, XrXirValue *input, XrXirValue *output, XrXirChannelState *state) {
    if (!mode) return xr_xir_channel_new((XrXirType)CH_I64,2,&owner->admission,output);
    if (mode==1) return xr_xir_channel_send_owned(channel,input,&owner->admission,state);
    if (mode==2) return xr_xir_channel_receive(channel,&owner->admission,output,state);
    return xr_xir_channel_close(channel,&owner->admission,state);
}
static void budget_prepare(unsigned mode, ChannelOwner *owner, XrXirValue *channel) {
    if (mode) CHECK(xr_xir_channel_new((XrXirType)CH_I64,2,&owner->admission,channel)==XR_XIR_VALUE_OK);
    if (mode==2) send_number(owner,channel,17,XR_XIR_CHANNEL_SENT);
}
static void local_work_cases(void) {
    for (unsigned mode=0;mode<4;++mode) {
        ChannelOwner owner={0}; owner_ready(&owner);
        XrXirValue channel={0},output={0},input=integer(17); XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
        budget_prepare(mode,&owner,&channel); uint64_t begin=owner.admission.work;
        CHECK(budget_operation(mode,&owner,&channel,&input,&output,&state)==XR_XIR_VALUE_OK);
        uint64_t required=begin-owner.admission.work; CHECK(required && required<128);
        xr_xir_value_drop(&channel); xr_xir_value_drop(&output); owner_free(&owner);
        for (uint64_t cut=0;cut<=required;++cut) {
            owner_ready(&owner); channel=(XrXirValue){0}; output=(XrXirValue){0}; input=integer(17);
            state=XR_XIR_CHANNEL_RETRY; budget_prepare(mode,&owner,&channel);
            owner.admission.work=cut;
            XrXirValueStatus status=budget_operation(mode,&owner,&channel,&input,&output,&state);
            CHECK(status==(cut==required ? XR_XIR_VALUE_OK : XR_XIR_VALUE_LIMIT));
            if (cut<required) {
                CHECK(empty(&output) && input.type==XR_XIR_I64 && input.payload==17 && state==XR_XIR_CHANNEL_RETRY);
                if (mode) {
                    XirChannel *storage=(XirChannel *)object_pointer(&channel);
                    CHECK(!atomic_load_explicit(&storage->closed,memory_order_acquire));
                    CHECK(storage->buffer.count==(mode==2 ? 1u : 0u));
                }
            }
            owner.admission.work=128000000;
            if (mode==2 && cut<required) receive_number(&owner,&channel,17,XR_XIR_CHANNEL_VALUE);
            xr_xir_value_drop(&channel); xr_xir_value_drop(&output); owner_free(&owner);
        }
        printf("Channel local work mode=%u exact=%" PRIu64 " all_prefixes=%" PRIu64 " physical=0\n",mode,required,required+1);
    }
}
#endif
