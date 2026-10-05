/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_module_state_class_cases.h - Independent four-slot isolation, cleanup and fault oracles
 *
 * KEY CONCEPT:
 *   Finite complete Programs preserve real ownership and physical cleanup.
 */

#ifndef XIR_MODULE_STATE_CLASS_CASES_H
#define XIR_MODULE_STATE_CLASS_CASES_H
static void state_class_trace(void *context,XrXirLifecycleEvent event,uint32_t index) {
    StateClassTrace *t=context;CHECK(t && t->instance && index<4);
    CHECK(xr_xir_instance_start(t->instance,4,NULL,0)==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_cancel_current(t->instance)==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_free(t->instance)==XR_XIR_CALL_BUSY);
    if(event==XR_XIR_SLOT_RELEASED) {
        CHECK(t->release_count<t->publication_count);
        CHECK(index==t->published[t->publication_count-t->release_count-1]);
        t->released[t->release_count++]=index;
    } else {
        CHECK(t->trace_count<20);t->trace[t->trace_count++]=(uint32_t)event*4+index;
        if(event==XR_XIR_SLOT_PUBLISHED) {
            const uint32_t expected[]={1,0,3,2};CHECK(t->publication_count<4);
            CHECK(index==expected[t->publication_count]);t->published[t->publication_count++]=index;
        }
    }
}
static XrXirInstanceConfig state_class_config(StateClassTrace *trace) {
    XrXirInstanceConfig c;CHECK(xr_xir_instance_config_init(&c,sizeof(c))==XR_XIR_CALL_READY);
    c.metadata_limit=1024*1024;c.value_limit=1024*1024;c.call_limit=1024*1024;
    c.poll_limit=1000000;c.depth_limit=64;c.trace=state_class_trace;c.trace_context=trace;return c;
}
static void state_class_fields(StateClassTrace *t) {
    CHECK(t->constructed==4 && t->replacements==4 && !t->free_count);
    CHECK(t->helper_calls==(t->scenario==8?4u:0u));t->inspecting=true;
    for(uint32_t n=0;n<4;++n) {
        CHECK(t->old_freed[n]==1);for(uint32_t j=0;j<n;++j)CHECK(t->classes[n]!=t->classes[j]);
        const uint32_t slots[]={1,0,3,2};XrXirValue value={0};
        CHECK((uintptr_t)t->instance->slots[slots[n]].payload==t->classes[n]);
        XrXirValueAdmission admission={t->instance->program->arena,t->instance->domain,NULL,NULL,100000,65536};
        CHECK(xr_xir_class_get(&t->instance->slots[slots[n]],0,&admission,&value)==XR_XIR_VALUE_OK);
        const char *text=NULL;size_t length=0;CHECK(xr_xir_string_view(&value,&text,&length));
        CHECK(length==1 && text[0]=='x');xr_xir_value_drop(&value);
    }
    t->inspecting=false;
}
static void state_class_terminal(StateClassTrace *trace,bool fault) {
    XrXirInstance *instance=trace->instance;XrXirInstanceResult result;
    do {result=xr_xir_instance_poll_bounded(instance,8);}while(result.outcome.status==XR_XIR_CALL_READY);
    if(!fault) {
        CHECK(result.outcome.status==XR_XIR_CALL_RETURNED && xr_xir_instance_state(instance)==XR_XIR_INSTANCE_READY);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        CHECK(value.type==XR_XIR_I64 && value.payload==41);xr_xir_value_drop(&value);return;
    }
    CHECK(result.outcome.status==XR_XIR_CALL_OOM);
    if(xr_xir_instance_state(instance)==XR_XIR_INSTANCE_FAILED) {
        XrXirCallResult retained={0};CHECK(xr_xir_instance_copy_failure(instance,&retained)==XR_XIR_CALL_OOM);
        CHECK(!retained.value.type && !retained.value.payload);xr_xir_call_result_drop(&retained);
        XrXirValue value={XR_XIR_I64,0,91},before=value;
        CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_BAD_STATE && !memcmp(&value,&before,sizeof(value)));
        size_t attempts=runtime_attempts;
        for(uint32_t i=0;i<24;++i) {
            CHECK(xr_xir_instance_start(instance,4,NULL,0)==XR_XIR_CALL_OOM);
            CHECK(xr_xir_instance_poll_bounded(instance,8).outcome.status==XR_XIR_CALL_OOM);
        }
        CHECK(runtime_attempts==attempts);
        CHECK(trace->trace_count<12);
    } else {
        CHECK(xr_xir_instance_state(instance)==XR_XIR_INSTANCE_READY);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_OOM && !value.payload);
    }
}
static void state_class_release(StateClassTrace *t,bool complete) {
    CHECK(xr_xir_instance_stop(t->instance)==XR_XIR_CALL_READY);
    if(complete)CHECK(!t->free_count && !t->release_count);
    state_class_active=t;CHECK(xr_xir_instance_free(t->instance)==XR_XIR_CALL_READY);t->instance=NULL;
    CHECK(t->release_count==t->publication_count && t->free_count==t->constructed);
    if(complete) {
        CHECK(t->constructed==4 && t->replacements==4 && t->helper_calls==(t->scenario==8?4u:0u));
        for(uint32_t i=0;i<4;++i)CHECK(t->old_freed[i]==1);
        const uint32_t slots[]={2,3,0,1},classes[]={4,3,2,1};
        CHECK(!memcmp(t->released,slots,sizeof(slots)) && !memcmp(t->freed,classes,sizeof(classes)));
    }
}
static void state_class_pair(XrXirProgram *program,uint32_t scenario,uint32_t order) {
    CHECK(!runtime_live && !runtime_bytes);StateClassTrace traces[2]={0};
    for(uint32_t i=0;i<2;++i) {
        traces[i].scenario=scenario;state_class_registry[i]=&traces[i];state_class_active=&traces[i];
        XrXirInstanceConfig config=state_class_config(&traces[i]);
        for(uint32_t abi=4;abi<=6;++abi) {
            XrXirInstanceConfig stale=config;stale.abi_version=abi;XrXirInstance *rejected=NULL;
            CHECK(xr_xir_instance_new(program,&stale,&rejected)==XR_XIR_CALL_BAD_ABI && !rejected);
        }
        CHECK(xr_xir_instance_new(program,&config,&traces[i].instance)==XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
    for(uint32_t repeat=0;repeat<24;++repeat)for(uint32_t i=0;i<2;++i) {
        StateClassTrace *t=&traces[i];state_class_active=t;
        CHECK(xr_xir_instance_start(t->instance,4,NULL,0)==XR_XIR_CALL_READY);state_class_terminal(t,false);
        const uint32_t expected[]={0,4,1,5,2,6,3,9,8,11,10,7};
        CHECK(t->trace_count==12 && !memcmp(t->trace,expected,sizeof(expected)));state_class_fields(t);
    }
    for(uint32_t n=0;n<4;++n)for(uint32_t j=0;j<4;++j)CHECK(traces[0].classes[n]!=traces[1].classes[j]);
    uint32_t first=order?1:0;state_class_release(&traces[first],true);
    state_class_active=&traces[1-first];state_class_fields(&traces[1-first]);
    state_class_release(&traces[1-first],true);
    CHECK(!runtime_live && !runtime_bytes);state_class_active=NULL;state_class_registry[0]=state_class_registry[1]=NULL;
    printf("class state %u order %u: 2x24, trace, fields, old-owner and actual class free4/3/2/1; physical0/0\n",scenario,order);
}
static size_t state_class_runtime_run(XrXirProgram *program,uint32_t scenario,size_t fail_at) {
    CHECK(!runtime_live && !runtime_bytes);StateClassTrace trace={0};trace.scenario=scenario;
    state_class_registry[0]=&trace;state_class_active=&trace;runtime_attempts=0;runtime_fail_at=fail_at;
    XrXirInstanceConfig config=state_class_config(&trace);
    XrXirCallStatus status=xr_xir_instance_new(program,&config,&trace.instance);
    bool injected=runtime_attempts>fail_at;
    if(status==XR_XIR_CALL_READY) {
        status=xr_xir_instance_start(trace.instance,4,NULL,0);injected=runtime_attempts>fail_at;
        if(status==XR_XIR_CALL_READY) {
            state_class_terminal(&trace,fail_at!=SIZE_MAX);
            injected=runtime_attempts>fail_at;
        } else CHECK(injected && status==XR_XIR_CALL_OOM);
        runtime_fail_at=SIZE_MAX;state_class_release(&trace,!injected);
    } else CHECK(injected && status==XR_XIR_CALL_OOM && !trace.instance);
    size_t attempts=runtime_attempts;runtime_fail_at=SIZE_MAX;
    CHECK(!runtime_live && !runtime_bytes);state_class_active=NULL;state_class_registry[0]=NULL;
    return attempts;
}
static void state_class_runtime_scan(XrXirProgram *program,uint32_t scenario) {
    size_t points=state_class_runtime_run(program,scenario,SIZE_MAX);
    for(size_t point=0;point<points;++point) {
        size_t attempts=state_class_runtime_run(program,scenario,point);CHECK(attempts>point);
    }
    printf("class state %u runtime: every %zu actual allocation ordinal; physical0/0\n",scenario,points);
    xr_xir_compile_program_drop(program);
}
#endif
