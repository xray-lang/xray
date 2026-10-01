/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_assert_condition_cases.h - Independent typed assertion runtime outcomes
 */
#ifndef XIR_ASSERT_CONDITION_CASES_H
#define XIR_ASSERT_CONDITION_CASES_H
enum {ASSERT_OMITTED,ASSERT_MESSAGE,ASSERT_CODE,ASSERT_SUCCESS,ASSERT_ORDERED,
    ASSERT_INFORMATION,ASSERT_RAW,ASSERT_MESSAGE_PANIC,ASSERT_SUSPENDED,ASSERT_CLEANUP,ASSERT_FUNCTIONS};
static void assert_initialization_trace(void *context,XrXirLifecycleEvent event,uint32_t module) {
    unsigned *count=context;
    if (event!=XR_XIR_MODULE_BEGIN && event!=XR_XIR_MODULE_READY) return;
    CHECK(*count<4);
    CHECK(module==(*count<2 ? 1u : 0u));
    CHECK(event==(*count%2 ? XR_XIR_MODULE_READY : XR_XIR_MODULE_BEGIN));++*count;
}
static XrXirCallStatus assert_poll(XrXirInstance *instance, unsigned *suspensions) {
    XrXirInstanceResult result=xr_xir_instance_poll(instance);
    while (result.outcome.status==XR_XIR_CALL_SUSPENDED) {
        CHECK(++*suspensions<=2);
        CHECK(xr_xir_instance_resume(instance,result.epoch,result.outcome.wake)==XR_XIR_CALL_READY);
        result=xr_xir_instance_poll(instance);
    }
    return result.outcome.status;
}
static XrXirValue assert_result(XrXirInstance *instance, uint32_t function, unsigned expected) {
    CHECK(xr_xir_instance_start(instance,function,NULL,0)==XR_XIR_CALL_READY);
    unsigned suspensions=0; XrXirCallStatus status=assert_poll(instance,&suspensions);
    if (status!=XR_XIR_CALL_RETURNED) fprintf(stderr,"assert entry=%u status=%u\n",function,status);
    CHECK(status==XR_XIR_CALL_RETURNED && suspensions==expected);
    XrXirValue value={0}; CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    return value;
}
static void assert_raw_messages(XrXirInstance *instance, uint32_t function) {
    const char small[]="a\0\xe4\xb8\xadz";
    char *long_bytes=xr_malloc(65537);CHECK(long_bytes);
    memset(long_bytes,'L',65537);long_bytes[3]=0;long_bytes[65536]='Z';
    for (uint32_t i=0;i<3;++i) {
        const char *expected=i==1 ? small : long_bytes;
        size_t expected_size=i==0 ? 0 : i==1 ? sizeof(small) : 65537;
        XrXirDomain *domain=NULL; CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
        XrXirValue arguments[2]={{XR_XIR_BOOL,0,0},{0}};
        CHECK(xr_xir_string_new(domain,expected,expected_size,&arguments[1])==XR_XIR_VALUE_OK);
        CHECK(xr_xir_instance_start(instance,function,arguments,2)==XR_XIR_CALL_READY);
        xr_xir_value_drop(&arguments[1]);xr_xir_domain_drop(domain);
        unsigned suspensions=0;CHECK(assert_poll(instance,&suspensions)==XR_XIR_CALL_RETURNED && !suspensions);
        XrXirValue info={0},message={0};CHECK(xr_xir_instance_take_result(instance,&info)==XR_XIR_CALL_RETURNED);
        XrXirFaultDetail detail={0}; CHECK(xr_xir_panic_info_detail(&info,&detail) && detail.code==445);
        CHECK(xr_xir_panic_info_message(&info,&message)==XR_XIR_VALUE_OK);xr_xir_value_drop(&info);
        const char *bytes=NULL;size_t length=0;
        CHECK(xr_xir_string_view(&message,&bytes,&length) && length==expected_size &&
            (!length || !memcmp(bytes,expected,length)));
        xr_xir_value_drop(&message);
    }
    xr_free(long_bytes);
}
static void assert_runtime_oom(XrXirProgram *program, uint32_t function) {
    size_t live=runtime_live,bytes=runtime_bytes,sites=0;
    for (size_t fault=0;fault<=sites;++fault) {
        runtime_attempts=0;runtime_fail_at=fault ? fault-1 : SIZE_MAX;
        XrXirInstance *instance=NULL;XrXirInstanceConfig config=xr_xir_instance_defaults();
        XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
        if (status==XR_XIR_CALL_READY) status=xr_xir_instance_start(instance,function,NULL,0);
        unsigned suspensions=0;
        if (status==XR_XIR_CALL_READY) status=assert_poll(instance,&suspensions);
        if (!fault) {CHECK(status==XR_XIR_CALL_RETURNED);sites=runtime_attempts;}
        else {
            if (status!=XR_XIR_CALL_OOM) fprintf(stderr,"assert OOM entry=%u site=%zu/%zu status=%u\n",function,fault,sites,status);
            CHECK(runtime_attempts>runtime_fail_at && status==XR_XIR_CALL_OOM);
        }
        runtime_fail_at=SIZE_MAX;
        if (instance) CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        CHECK(runtime_live==live && runtime_bytes==bytes);
    }
    printf("Assertion entry %u actual runtime OOM=%zu physical baseline restored\n",function,sites);
}
static void assert_cases(XrXirProgram *program, const uint32_t *functions) {
    assert_runtime_oom(program,functions[ASSERT_INFORMATION]);
    assert_runtime_oom(program,functions[ASSERT_SUSPENDED]);
    XrXirInstanceConfig config=xr_xir_instance_defaults();
    XrXirValue held[2]={{0}};
    for (uint32_t repeat=0;repeat<2;++repeat) {
        unsigned initialized=0;config.trace=assert_initialization_trace;config.trace_context=&initialized;
        XrXirInstance *instance=NULL;
        CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        const uint32_t entries[]={ASSERT_OMITTED,ASSERT_MESSAGE,ASSERT_CODE,ASSERT_SUCCESS,ASSERT_ORDERED,
            ASSERT_INFORMATION,ASSERT_MESSAGE_PANIC,ASSERT_SUSPENDED,ASSERT_CLEANUP};
        for (uint32_t n=0;n<sizeof(entries)/sizeof(entries[0]);++n) {
            uint32_t i=entries[n];
            XrXirValue value=assert_result(instance,functions[i],i==ASSERT_SUSPENDED ? 2 : 0);
            if (i==ASSERT_OMITTED || i==ASSERT_MESSAGE || i==ASSERT_SUSPENDED) {
                const char *bytes=NULL;size_t length=0;
                CHECK(xr_xir_string_view(&value,&bytes,&length));
                size_t expected=i==ASSERT_OMITTED ? 0 : i==ASSERT_MESSAGE ? 8 : 6;
                CHECK(length==expected);
                if (length) CHECK(!memcmp(bytes,i==ASSERT_MESSAGE ? "a\n\r\t\xe4\xb8\xadZ" : "paused",length));
            } else if (i==ASSERT_INFORMATION) {
                CHECK(value.type==XR_XIR_PANIC_INFO); held[repeat]=value;value=(XrXirValue){0};
            } else CHECK(value.type==XR_XIR_I64 && value.payload==(i==ASSERT_CODE ? 445 :
                i==ASSERT_SUCCESS ? 23 : i==ASSERT_ORDERED ? 12 : i==ASSERT_MESSAGE_PANIC ? 420 : 7));
            xr_xir_value_drop(&value);
        }
        assert_raw_messages(instance,functions[ASSERT_RAW]);
        CHECK(initialized==4);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    xr_xir_program_drop(program);
    for (uint32_t repeat=0;repeat<2;++repeat) {
        XrXirValue message={0};CHECK(xr_xir_panic_info_message(&held[repeat],&message)==XR_XIR_VALUE_OK);
        const char *bytes=NULL;size_t length=0;
        CHECK(xr_xir_string_view(&message,&bytes,&length) && length==4 && !memcmp(bytes,"held",4));
        xr_xir_value_drop(&message);xr_xir_value_drop(&held[repeat]);
    }
}
#endif // XIR_ASSERT_CONDITION_CASES_H
