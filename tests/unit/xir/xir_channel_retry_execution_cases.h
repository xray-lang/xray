/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_retry_execution_cases.h - Real backend retry, single evaluation and cancellation
 */
#ifndef XIR_CHANNEL_RETRY_EXECUTION_CASES_H
#define XIR_CHANNEL_RETRY_EXECUTION_CASES_H
static XrXirValue channel_retry_return(XrXirInstance *instance) {
    XrXirInstanceResult result={0};unsigned polls=0;
    do { CHECK(++polls<4096);result=xr_xir_instance_poll_bounded(instance,1); } while(result.outcome.status==XR_XIR_CALL_READY);
    CHECK(result.outcome.status==XR_XIR_CALL_RETURNED && xr_xir_call_result_valid(&result.outcome));
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);return value;
}
static void channel_retry_case(XrXirProgram *program,const uint32_t entries[4],unsigned mode,bool cancel) {
    size_t live=runtime_live,bytes=runtime_bytes;
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entries[0],NULL,0)==XR_XIR_CALL_READY);
    XrXirValue channel=channel_retry_return(instance);CHECK(xr_xir_value_valid(&channel));
    CHECK(xr_xir_type_is_channel(xr_xir_compile_type_arena_types(xr_xir_value_arena(&channel)),(XrXirType)channel.type));
    if (mode==1) {
        CHECK(xr_xir_instance_start(instance,entries[1],&channel,1)==XR_XIR_CALL_READY);
        XrXirValue value=channel_retry_return(instance);CHECK(value.type==XR_XIR_I64 && value.payload==101);xr_xir_value_drop(&value);
    }
    XirChannel *storage=(XirChannel *)object_pointer(&channel);uint64_t sequence=storage->sequence;
    uint32_t count=storage->buffer.count;CHECK(count==(mode==1 ? 1u : 0u));
    atomic_store_explicit(&storage->locked,true,memory_order_relaxed);
    CHECK(xr_xir_instance_start(instance,entries[mode+1],&channel,1)==XR_XIR_CALL_READY);
    for (unsigned step=0;step<256;++step) {
        XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1);
        CHECK(result.outcome.status==XR_XIR_CALL_READY && xr_xir_call_result_valid(&result.outcome));
        CHECK(storage->buffer.count==count && storage->sequence==sequence && !atomic_load_explicit(&storage->closed,memory_order_relaxed));
    }
    if (cancel) {
        CHECK(xr_xir_instance_stop(instance)==XR_XIR_CALL_READY);
        XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1);
        CHECK(result.outcome.status==XR_XIR_CALL_CANCELLED && xr_xir_call_result_valid(&result.outcome));
        CHECK(storage->buffer.count==count && storage->sequence==sequence);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        CHECK(atomic_load_explicit(&storage->locked,memory_order_relaxed));
        atomic_store_explicit(&storage->locked,false,memory_order_release);
    } else {
        atomic_store_explicit(&storage->locked,false,memory_order_release);
        XrXirValue value=channel_retry_return(instance);const int64_t expected[]={101,41,7};
        CHECK(value.type==XR_XIR_I64 && value.payload==expected[mode]);xr_xir_value_drop(&value);
        CHECK(storage->buffer.count==(mode==0 ? 1u : 0u));
        CHECK(atomic_load_explicit(&storage->closed,memory_order_relaxed)==(mode==2));
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    xr_xir_value_drop(&channel);CHECK(runtime_live==live && runtime_bytes==bytes);
}
static void channel_retry_execute(XrXirProgram *program,const uint32_t entries[4]) {
    for (unsigned mode=0;mode<3;++mode) for (unsigned cancel=0;cancel<2;++cancel)
        channel_retry_case(program,entries,mode,cancel!=0);
    puts("Channel actual backend send/receive/close: 256 bounded lock retries, expression once, cancel while locked and escaped lifetime physical0");
}
#endif // XIR_CHANNEL_RETRY_EXECUTION_CASES_H
