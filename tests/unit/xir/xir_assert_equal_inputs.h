/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_assert_equal_inputs.h - Borrowed exact bytes and messages outliving producers
 */
#ifndef XIR_ASSERT_EQUAL_INPUTS_H
#define XIR_ASSERT_EQUAL_INPUTS_H
static XrXirCallStatus equal_instance_bounded(XrXirProgram *program,uint32_t function,uint64_t limit) {
    size_t live=runtime_live,bytes=runtime_bytes;
    XrXirInstance *instance=NULL;XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);config.poll_limit=limit;
    XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
    if (status==XR_XIR_CALL_READY) status=xr_xir_instance_start(instance,function,NULL,0);
    if (status==XR_XIR_CALL_READY) status=xr_xir_instance_poll(instance).outcome.status;
    if (status==XR_XIR_CALL_RETURNED) {
        XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
        CHECK(result.type==XR_XIR_I64 && result.payload==3);xr_xir_value_drop(&result);
    } else CHECK(status==XR_XIR_CALL_LIMIT);
    if (instance) CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(runtime_live==live && runtime_bytes==bytes);return status;
}
static void equal_instance_limits(XrXirProgram *program,uint32_t function) {
    uint64_t low=1,high=1000000;
    CHECK(equal_instance_bounded(program,function,high)==XR_XIR_CALL_RETURNED);
    while (low<high) {
        uint64_t middle=low+(high-low)/2;
        if (equal_instance_bounded(program,function,middle)==XR_XIR_CALL_RETURNED) high=middle;
        else low=middle+1;
    }
    CHECK(low>1 && equal_instance_bounded(program,function,low)==XR_XIR_CALL_RETURNED);
    CHECK(equal_instance_bounded(program,function,low-1)==XR_XIR_CALL_LIMIT);
    printf("Equal caught-assertion runtime poll/admission exact=%llu/minus1 remains LIMIT with physical refund\n",(unsigned long long)low);
}
static void equal_instance_oom(XrXirProgram *program,uint32_t function) {
    size_t live=runtime_live,bytes=runtime_bytes,sites=0;
    for (size_t point=0;point<=sites;++point) {
        runtime_attempts=0;runtime_fail_at=point ? point-1 : SIZE_MAX;
        XrXirInstance *instance=NULL;XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
        if (status==XR_XIR_CALL_READY) status=xr_xir_instance_start(instance,function,NULL,0);
        if (status==XR_XIR_CALL_READY) status=xr_xir_instance_poll(instance).outcome.status;
        if (!point) {CHECK(status==XR_XIR_CALL_RETURNED);sites=runtime_attempts;}
        else CHECK(status==XR_XIR_CALL_OOM && runtime_attempts>runtime_fail_at);
        runtime_fail_at=SIZE_MAX;
        if (instance) CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        CHECK(runtime_live==live && runtime_bytes==bytes);
    }
    printf("Equal Array/caught-assertion runtime actualOOM=%zu, resource channel and physical refund PASS\n",sites);
    equal_instance_limits(program,function);
}
static void equal_owned_inputs(XrXirInstance *instance,uint32_t relation,uint32_t assertion,XrXirValue held[2]) {
    const char small[]="a\0\xe4\xb8\xadz";
    char *long_bytes=xr_malloc(65537);CHECK(long_bytes);
    memset(long_bytes,'L',65537);long_bytes[3]=0;long_bytes[65536]='Z';
    for (uint32_t i=0;i<3;++i) {
        const char *expected=i==1 ? small : long_bytes;
        size_t length=i==0 ? 0 : i==1 ? sizeof(small) : 65537;
        for (uint32_t different=0;different<2;++different) {
            XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
            XrXirValue inputs[2]={{0}};
            CHECK(xr_xir_string_new(domain,expected,length,&inputs[0])==XR_XIR_VALUE_OK);
            if (different && length) {
                char *copy=xr_malloc(length);CHECK(copy);memcpy(copy,expected,length);copy[length-1]='Q';
                CHECK(xr_xir_string_new(domain,copy,length,&inputs[1])==XR_XIR_VALUE_OK);xr_free(copy);
            } else CHECK(xr_xir_value_copy(&inputs[0],&inputs[1])==XR_XIR_VALUE_OK);
            CHECK(xr_xir_instance_start(instance,relation,inputs,2)==XR_XIR_CALL_READY);
            xr_xir_value_drop(&inputs[0]);xr_xir_value_drop(&inputs[1]);xr_xir_domain_drop(domain);
            CHECK(xr_xir_instance_poll(instance).outcome.status==XR_XIR_CALL_RETURNED);
            XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
            CHECK(value.type==XR_XIR_BOOL && value.payload==(int64_t)(!different || !length));xr_xir_value_drop(&value);
        }
        XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
        XrXirValue input={0};CHECK(xr_xir_string_new(domain,expected,length,&input)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_instance_start(instance,assertion,&input,1)==XR_XIR_CALL_READY);
        xr_xir_value_drop(&input);xr_xir_domain_drop(domain);
        CHECK(xr_xir_instance_poll(instance).outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue info={0};CHECK(xr_xir_instance_take_result(instance,&info)==XR_XIR_CALL_RETURNED);
        XrXirFaultDetail detail={0};CHECK(xr_xir_panic_info_detail(&info,&detail) && detail.code==445);
        if (i) held[i-1]=info;
        else {XrXirValue message={0};const char *bytes=NULL;size_t size=1;
            CHECK(xr_xir_panic_info_message(&info,&message)==XR_XIR_VALUE_OK);
            CHECK(xr_xir_string_view(&message,&bytes,&size) && !size);
            xr_xir_value_drop(&message);xr_xir_value_drop(&info);}
    }
    xr_free(long_bytes);
}
static void equal_held_messages(XrXirValue held[2]) {
    const char small[]="a\0\xe4\xb8\xadz";
    for (uint32_t i=0;i<2;++i) {
        XrXirValue message={0};CHECK(xr_xir_panic_info_message(&held[i],&message)==XR_XIR_VALUE_OK);
        xr_xir_value_drop(&held[i]);const char *bytes=NULL;size_t length=0;
        CHECK(xr_xir_string_view(&message,&bytes,&length));
        if (!i) CHECK(length==sizeof(small) && !memcmp(bytes,small,length));
        else {
            CHECK(length==65537);
            for (size_t n=0;n<length;++n) CHECK(bytes[n]==(n==3 ? 0 : n==65536 ? 'Z' : 'L'));
        }
        xr_xir_value_drop(&message);
    }
}
#endif // XIR_ASSERT_EQUAL_INPUTS_H
