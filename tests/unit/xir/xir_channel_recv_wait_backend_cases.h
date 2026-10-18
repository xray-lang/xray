/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_recv_wait_backend_cases.h - Actual Source sender into a parked receiver
 */
#ifndef XIR_CHANNEL_RECV_WAIT_BACKEND_CASES_H
#define XIR_CHANNEL_RECV_WAIT_BACKEND_CASES_H
static XrXirType recv_wait_backend_type(const XrXirValue *channel) {
    const XrXirTypes *types=xr_xir_compile_type_arena_types(xr_xir_value_arena(channel));
    for (uint32_t i=0;i<types->count;++i) {
        XrXirType type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i);
        if (xr_xir_nominal_native_recv(types,type,XR_XIR_I64)) return type;
    }
    CHECK(false);return XR_XIR_UNIT;
}
static size_t recv_wait_backend_once(XrXirProgram *program,const uint32_t entries[3],size_t ordinal) {
    size_t live=runtime_live,bytes=runtime_bytes,begin=runtime_attempts;
    runtime_fail_at=ordinal==SIZE_MAX ? SIZE_MAX : begin+ordinal;
    XrXirInstanceConfig config={0};CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *maker=NULL,*sender=NULL;XrXirDomain *host=NULL;XrXirValue channel={0},sent={0},received={0};
    XrXirChannelRecvWait *recv=NULL;XrXirCallStatus maker_release=XR_XIR_CALL_READY,sender_release=XR_XIR_CALL_READY;
    XrXirCallStatus status=xr_xir_instance_new(program,&config,&maker);
    if (status==XR_XIR_CALL_READY) status=rendezvous_backend_invoke(maker,entries[0],NULL,0,&channel,&maker_release);
    if (status==XR_XIR_CALL_RETURNED) {
        CHECK(xr_xir_value_valid(&channel));size_t attempts=runtime_attempts;
        CHECK(xr_xir_instance_free(maker)==XR_XIR_CALL_READY && runtime_attempts==attempts);maker=NULL;
        XrXirDomainBudgetControls controls={8388608,8388608,128000000,4194304,4194304};
        XrXirValueStatus created=xr_xir_domain_new_budgeted(8388608,&controls,&host);
        status=created==XR_XIR_VALUE_OOM ? XR_XIR_CALL_OOM : XR_XIR_CALL_READY;CHECK(created==XR_XIR_VALUE_OK || created==XR_XIR_VALUE_OOM);
        if (status==XR_XIR_CALL_READY) {
            XrXirValueAdmission admission={.arena=xr_xir_value_arena(&channel),.domain=host,.work=128000000,.scratch_bytes=4194304};
            XrXirChannelRecvWaitState state=XR_XIR_CHANNEL_RECV_WAIT_RETRY;
            XrXirValueStatus pending=xr_xir_channel_rendezvous_recv_begin(&channel,recv_wait_backend_type(&channel),&admission,&recv,&state);
            CHECK(pending==XR_XIR_VALUE_OK || pending==XR_XIR_VALUE_OOM);status=pending==XR_XIR_VALUE_OOM ? XR_XIR_CALL_OOM : XR_XIR_CALL_READY;
            if (status==XR_XIR_CALL_READY) {
                CHECK(recv && state==XR_XIR_CHANNEL_RECV_WAIT_PENDING && !((XirChannel *)object_pointer(&channel))->buffer.count);
                xr_xir_domain_close(host);xr_xir_domain_drop(host);host=NULL;admission=(XrXirValueAdmission){0};
                status=xr_xir_instance_new(program,&config,&sender);
                XrXirValue arguments[]={channel,{XR_XIR_I64,0,771}};
                if (status==XR_XIR_CALL_READY) status=rendezvous_backend_invoke(sender,entries[2],arguments,2,&sent,&sender_release);
                if (status==XR_XIR_CALL_RETURNED) {
                    CHECK(sent.type==XR_XIR_I64 && sent.payload==1 && xr_xir_channel_recv_wait_state(recv)==XR_XIR_CHANNEL_RECV_WAIT_VALUE &&
                        !((XirChannel *)object_pointer(&channel))->recv_count && xr_xir_channel_recv_wait_take(recv,&received));
                    XrXirEnumBorrow borrow={0};CHECK(xr_xir_enum_borrow(&received,&borrow)==XR_XIR_VALUE_OK &&
                        borrow.field_count==1 && borrow.fields[0].type==XR_XIR_I64 && borrow.fields[0].payload==771);
                }
            }
        }
    }
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (status!=(ordinal==SIZE_MAX ? XR_XIR_CALL_RETURNED : XR_XIR_CALL_OOM))
        fprintf(stderr,"Receive-first Source sender ordinal=%zu sites=%zu status=%u\n",ordinal,sites,status);
    CHECK(status==(ordinal==SIZE_MAX ? XR_XIR_CALL_RETURNED : XR_XIR_CALL_OOM));
    size_t attempts=runtime_attempts;CHECK(xr_xir_instance_free(sender)==sender_release && xr_xir_instance_free(maker)==maker_release);
    xr_xir_value_drop(&received);xr_xir_value_drop(&sent);xr_xir_channel_recv_wait_drop(recv);xr_xir_domain_drop(host);xr_xir_value_drop(&channel);
    CHECK(runtime_attempts==attempts && runtime_live==live && runtime_bytes==bytes);return sites;
}
static void recv_wait_backend_execute(XrXirProgram *program,const uint32_t entries[3]) {
    CHECK(recv_wait_backend_once(program,entries,SIZE_MAX));
    puts("Channel actual Source sender: creator Instance freed, owned receive reservation after host Domain close/admission end, native Recv771 and physical baseline");
}
static void recv_wait_backend_faults(XrXirProgram *program,const uint32_t entries[3]) {
    size_t sites=recv_wait_backend_once(program,entries,SIZE_MAX);CHECK(sites);
    for (size_t ordinal=0;ordinal<sites;++ordinal) {
        CHECK(recv_wait_backend_once(program,entries,ordinal)>ordinal);
        CHECK(recv_wait_backend_once(program,entries,SIZE_MAX)==sites);
    }
    printf("Channel actual backend receive-first/Source sender complete runtime allocator ordinals=%zu; same-process recovery physical baseline\n",sites);
}
#endif // XIR_CHANNEL_RECV_WAIT_BACKEND_CASES_H
