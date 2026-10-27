/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * intrinsic_string_slice.h - Scalar ranges across real owned consumers
 *
 * Source and packet owners have retired before these consumers execute.
 * Arguments retire before VM polling; returned strings outlive the instance.
 */
#ifndef INTRINSIC_STRING_SLICE_H
#define INTRINSIC_STRING_SLICE_H
typedef struct SliceCase {
    const char *text,*expected;
    size_t text_size,expected_size;
    int64_t start,end;
    bool invalid,tail;
} SliceCase;
static void slice_bytes(const XrXirValue *value,const SliceCase *test) {
    const char *bytes=NULL;size_t length=0;
    CHECK(value->type==XR_XIR_STRING && xr_xir_string_view(value,&bytes,&length));
    CHECK(length==test->expected_size && (!length || !memcmp(bytes,test->expected,length)));
}
static void slice_consumers(Fixture *fixture) {
    static const char text[]="A\xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80";
    static const char combining[]="e\xcc\x81Z";
#define SLICE(text,start,end,result) {text,result,sizeof(text)-1,sizeof(result)-1,start,end,false,false}
#define BAD(start,end) {"abc",NULL,3,0,start,end,true,false}
    static const SliceCase cases[]={
        SLICE(text,1,3,"\xc3\xa9\xe4\xb8\xad"),SLICE(text,0,4,text),SLICE(text,4,4,""),
        SLICE("",0,0,""),SLICE("a\0b",1,2,"\0"),SLICE(combining,1,2,"\xcc\x81"),
        {text,"\xe4\xb8\xad\xf0\x9f\x98\x80",sizeof(text)-1,7,2,4,false,true},
        BAD(-1,0),BAD(2,1),BAD(0,4),BAD(0,INT64_MAX)
    };
#undef BAD
#undef SLICE
    for(size_t i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        const SliceCase *test=cases+i;
        XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(UINT64_MAX,&domain)==XR_XIR_VALUE_OK);
        XrXirValue arguments[3]={{0},{XR_XIR_I64,0,test->start},{XR_XIR_I64,0,test->end}};
        CHECK(xr_xir_string_new(domain,test->text,test->text_size,arguments)==XR_XIR_VALUE_OK);
        uint32_t function=test->tail?fixture->tail:fixture->cut,count=test->tail?2u:3u;
        XrXirArtifact *code=lowered(fixture);
        XrXirCtfeRequest request={.artifact=code,.function=function,
            .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},.arguments=arguments,.argument_count=count,
            .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES}};
        memcpy(request.checked_identity,fixture->identity,sizeof(fixture->identity));
        XrXirValue folded={0};XrXirCtfeStatus status=xr_xir_compile_ctfe_leaf(&request,&folded);
        CHECK(status==(test->invalid?XR_XIR_CTFE_BOUNDS:XR_XIR_CTFE_OK));
        if(!test->invalid) slice_bytes(&folded,test);else CHECK(!folded.type && !folded.payload);
        xr_xir_value_drop(&folded);
        XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&code,&program)==XR_XIR_OK && !code);
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        Output output={0};output.sink=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,output_bytes,&output,sizeof(output.bytes)};
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,output_group,&output};
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        xr_xir_compile_program_drop(program);
        CHECK(xr_xir_instance_start(instance,function,arguments,count)==XR_XIR_CALL_READY);
        xr_xir_value_drop(arguments);xr_xir_domain_drop(domain);
        XrXirInstanceResult polled=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
        if(polled.outcome.status!=(test->invalid?XR_XIR_CALL_RUNTIME_PANIC:XR_XIR_CALL_RETURNED))
            fprintf(stderr,"slice VM case %zu range %lld:%lld status %u panic %u\n",i,
                (long long)test->start,(long long)test->end,(unsigned)polled.outcome.status,polled.outcome.panic.detail.code);
        CHECK(polled.outcome.status==(test->invalid?XR_XIR_CALL_RUNTIME_PANIC:XR_XIR_CALL_RETURNED));
        if(test->invalid) CHECK(polled.outcome.panic.detail.code==XR_XIR_PANIC_STRING_SLICE_RANGE &&
            !polled.outcome.panic.detail.reserved && !polled.outcome.panic.detail.index && !polled.outcome.panic.detail.length);
        if(test->invalid) {
            char message[96];const char expected[]="string slice range is invalid";
            CHECK(xr_xir_panic_message_format(polled.outcome.panic.detail,message,sizeof(message))==sizeof(expected)-1 &&
                !strcmp(message,expected));
        }
        XrXirValue result={0};
        if(!test->invalid) CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        if(!test->invalid) slice_bytes(&result,test);else CHECK(!result.type && !result.payload);
        xr_xir_value_drop(&result);
    }
    puts("String slice scalar/NUL/bounds/CTFE/VM and retired-owner consumers passed");
}
#endif
