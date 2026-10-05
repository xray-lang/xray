/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_resize_cases.h - Independent values and the single publication point
 *
 * KEY CONCEPT:
 *   Each cancellation prefix and allocation failure checks the old alias.
 */
#ifndef XIR_ARRAY_RESIZE_CASES_H
#define XIR_ARRAY_RESIZE_CASES_H
static XrXirInstance *resize_open(ResizeCompile *run) {
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.value_limit = 1048576;
    XrXirInstance *instance = NULL; CHECK(xr_xir_instance_new(run->program, &config, &instance) == XR_XIR_CALL_READY);
    XrXirValue entry = resize_run(instance, run->program->declarations->entry_function);
    CHECK(entry.type == XR_XIR_I64 && !entry.payload); xr_xir_value_drop(&entry); return instance;
}
static XrXirValueAdmission resize_admission(const XrXirValue *value, XrXirDomain **reader) {
    CHECK(xr_xir_domain_new(1048576, reader) == XR_XIR_VALUE_OK);
    return (XrXirValueAdmission){xr_xir_value_arena(value), *reader, NULL, NULL, 100000, 1048576};
}
static void resize_array(const XrXirValue *array, const int64_t *expected, int64_t count, bool nullable, int64_t none) {
    XrXirDomain *reader = NULL; XrXirValueAdmission admission = resize_admission(array, &reader);
    int64_t length = -1; CHECK(xr_xir_array_len(array, &admission, &length) == XR_XIR_VALUE_OK && length == count);
    for (int64_t i = 0; i < count; ++i) {
        XrXirValue element = {0}; XrXirFaultDetail fault = {0};
        CHECK(xr_xir_array_get(array, i, &admission, &element, &fault) == XR_XIR_VALUE_OK);
        const XrXirValue *payload = &element; bool some = true;
        if (nullable) { CHECK(xr_xir_nullable_view(&element, &some, &payload)); CHECK(some == (i != none && expected[i] != INT64_MIN)); }
        if (some) CHECK(payload && payload->type == XR_XIR_I64 && payload->payload == expected[i]);
        else CHECK(!payload);
        xr_xir_value_drop(&element);
    }
    xr_xir_domain_drop(reader);
}
static void resize_state(XrXirInstance *instance, ResizeCompile *run, bool shrink, bool committed) {
    static const int64_t old[] = {1, INT64_MIN, 3};
    static const int64_t grown[] = {1, INT64_MIN, 3, INT64_MIN, INT64_MIN};
    XrXirValue count = resize_run(instance, resize_find(run->module, "count"));
    CHECK(count.type == XR_XIR_I64 && count.payload == (committed ? (shrink ? 1 : 5) : 3)); xr_xir_value_drop(&count);
    XrXirValue root = resize_run(instance, resize_find(run->module, "saved"));
    resize_array(&root, committed && !shrink ? grown : old, committed ? (shrink ? 1 : 5) : 3, true, 1);
    xr_xir_value_drop(&root);
}
static void resize_goldens(ResizeCompile *run) {
    XrXirInstance *instance = resize_open(run);
    static const int64_t numbers[] = {3,3,5,1,3,9,9,2,2,2,2,0,0};
    static const int64_t independent[] = {99,2,1,42}, rebound[] = {81,83,9}, handle[] = {3,7}, vacant[] = {0,2,7,7};
    static const char *const names[] = {"numbers","independent","rebound","readHandle","vacant"};
    const int64_t *const values[] = {numbers, independent, rebound, handle, vacant};
    static const int64_t counts[] = {13,4,3,2,4};
    for (unsigned n=0;n<5;++n) {
        XrXirValue value=resize_run(instance,resize_find(run->module,names[n]));
        resize_array(&value,values[n],counts[n],false,-1);xr_xir_value_drop(&value);
    }
    XrXirValue trace=resize_run(instance,resize_find(run->module,"currentTrace"));
    CHECK(trace.payload==123);xr_xir_value_drop(&trace);
    XrXirValue alias=resize_run(instance,resize_find(run->module,"saved"));
    XrXirValue same=resize_run(instance,resize_find(run->module,"same"));
    static const int64_t old[]={1,INT64_MIN,3};resize_array(&same,old,3,true,1);xr_xir_value_drop(&same);
    XrXirValue zero=resize_run(instance,resize_find(run->module,"zero"));resize_array(&zero,old,0,true,-1);xr_xir_value_drop(&zero);
    resize_array(&alias,old,3,true,1);xr_xir_value_drop(&alias);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY&&!runtime_live&&!runtime_bytes);
    for(unsigned n=0;n<2;++n) {
        instance=resize_open(run);XrXirValue value=resize_run(instance,resize_find(run->module,n?"sideZero":"sideSame"));
        resize_array(&value,old,n?0:3,true,1);xr_xir_value_drop(&value);
        value=resize_run(instance,resize_find(run->module,"currentTrace"));CHECK(value.payload==12);xr_xir_value_drop(&value);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY&&!runtime_live&&!runtime_bytes);
    }
    puts("resize independent lengths/prefix/fill/COW99-42/selectors123/read-class physical0");
}
static void resize_extra_faults(ResizeCompile *run) {
    static const char *const names[]={"numbers","independent","rebound","readHandle","vacant","text","retainedCallable","same","zero","sideSame","sideZero"};
    for(unsigned n=0;n<sizeof(names)/sizeof(names[0]);++n) {
        uint32_t function=resize_find(run->module,names[n]);XrXirInstance *instance=resize_open(run);
        runtime_attempts=0;XrXirValue result=resize_run(instance,function);const size_t sites=runtime_attempts;
        xr_xir_value_drop(&result);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY&&!runtime_live&&!runtime_bytes);
        for(size_t failure=0;failure<sites;++failure) {
            instance=resize_open(run);runtime_attempts=0;runtime_fail_at=failure;
            XrXirCallStatus status=xr_xir_instance_start(instance,function,NULL,0);
            while(status==XR_XIR_CALL_READY)status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
            CHECK(runtime_attempts>failure&&status==XR_XIR_CALL_OOM);runtime_fail_at=SIZE_MAX;
            result=(XrXirValue){0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_BAD_STATE&&!result.type&&!result.payload);
            CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY&&!runtime_live&&!runtime_bytes);
        }
        printf("resize %s runtimeOOM%zu physical0\n",names[n],sites);
    }
}
typedef struct ResizeTimeline {size_t ticks,sites,commit,allocation_ticks[4096];} ResizeTimeline;
static ResizeTimeline resize_timeline(ResizeCompile *run,bool shrink) {
    ResizeTimeline line={0};line.commit=SIZE_MAX;
    const size_t live=runtime_live,bytes=runtime_bytes;
    const uint32_t function=resize_find(run->module,shrink?"shrink":"grow");
    XrXirInstance *instance=resize_open(run);
    runtime_attempts=0;runtime_fail_at=SIZE_MAX;
    CHECK(xr_xir_instance_start(instance,function,NULL,0)==XR_XIR_CALL_READY);
    for(size_t i=0;i<runtime_attempts;++i)line.allocation_ticks[i]=0;
    XrXirCallStatus status=XR_XIR_CALL_READY;
    while(status==XR_XIR_CALL_READY) {
        CHECK(line.ticks<2048);++line.ticks;size_t first=runtime_attempts;
        status=xr_xir_instance_poll_bounded(instance,1).outcome.status;CHECK(runtime_attempts<4096);
        for(size_t i=first;i<runtime_attempts;++i)line.allocation_ticks[i]=line.ticks;
    }
    CHECK(status==XR_XIR_CALL_RETURNED);line.sites=runtime_attempts;
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    static const int64_t grow[]={5,5},small[]={1,1};resize_array(&result,shrink?small:grow,2,false,-1);
    xr_xir_value_drop(&result);resize_state(instance,run,shrink,true);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY&&runtime_live==live&&runtime_bytes==bytes);
    for(size_t prefix=0;prefix<line.ticks;++prefix) {
        instance=resize_open(run);XrXirValue alias=resize_run(instance,resize_find(run->module,"saved"));
        CHECK(xr_xir_instance_start(instance,function,NULL,0)==XR_XIR_CALL_READY);
        for(size_t tick=0;tick<prefix;++tick)CHECK(xr_xir_instance_poll_bounded(instance,1).outcome.status==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_CANCELLED);
        XrXirValue count=resize_run(instance,resize_find(run->module,"count"));
        CHECK(count.payload==3||count.payload==(shrink?1:5));
        if(count.payload!=3&&line.commit==SIZE_MAX)line.commit=prefix;
        xr_xir_value_drop(&count);resize_state(instance,run,shrink,line.commit!=SIZE_MAX);
        static const int64_t old[]={1,INT64_MIN,3};resize_array(&alias,old,3,true,1);xr_xir_value_drop(&alias);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY&&runtime_live==live&&runtime_bytes==bytes);
    }
    CHECK(line.commit>0&&line.commit<line.ticks&&line.sites);return line;
}
static void resize_runtime_faults(ResizeCompile *run,bool shrink) {
    ResizeTimeline line=resize_timeline(run,shrink);
    const uint32_t function=resize_find(run->module,shrink?"shrink":"grow");
    const size_t live=runtime_live,bytes=runtime_bytes;size_t before=0,after=0;
    for(size_t failure=0;failure<line.sites;++failure) {
        XrXirInstance *instance=resize_open(run);XrXirValue alias=resize_run(instance,resize_find(run->module,"saved"));
        runtime_attempts=0;runtime_fail_at=failure;
        XrXirCallStatus status=xr_xir_instance_start(instance,function,NULL,0);
        size_t tick=0,failed=runtime_attempts>failure?0:SIZE_MAX;
        while(status==XR_XIR_CALL_READY) {
            CHECK(++tick<4096);size_t first=runtime_attempts;
            status=xr_xir_instance_poll_bounded(instance,1).outcome.status;
            if(first<=failure&&failure<runtime_attempts){CHECK(failed==SIZE_MAX);failed=tick;}
        }
        CHECK(runtime_attempts>failure&&status==XR_XIR_CALL_OOM&&failed==line.allocation_ticks[failure]);
        runtime_fail_at=SIZE_MAX;XrXirValue absent={0};
        CHECK(xr_xir_instance_take_result(instance,&absent)==XR_XIR_CALL_BAD_STATE&&!absent.type&&!absent.payload);
        bool committed=failed>line.commit;resize_state(instance,run,shrink,committed);
        if(committed)++after;else ++before;
        static const int64_t old[]={1,INT64_MIN,3};resize_array(&alias,old,3,true,1);xr_xir_value_drop(&alias);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY&&runtime_live==live&&runtime_bytes==bytes);
    }
    CHECK(before&&after&&before+after==line.sites);
    printf("resize %s runtimeOOM%zu pre%zu post%zu cancel-prefixes%zu commit%zu physical0\n",shrink?"shrink":"grow",line.sites,before,after,line.ticks,line.commit);
}
#endif
