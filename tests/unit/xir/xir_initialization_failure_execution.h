/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_initialization_failure_execution.h - Independent sticky-failure expectations
 *
 * KEY CONCEPT:
 *   Each consumer proves its own trace and owning results; agreement alone is insufficient.
 */
#include "xir/xxir_error.h"
#include "xir/xxir_enum.h"
typedef struct InitTrace {uint32_t output,begins,ready,published,released,stack[16];bool reject;} InitTrace;
static bool init_output(void *context,const XrXirOutputGroup *group){
    InitTrace *trace=context;const char *text=NULL;size_t size=0;
    CHECK(group->stream==XR_XIR_STDOUT&&group->line&&group->count==1&&trace->output<2);
    CHECK(xr_xir_string_view(&group->values[0],&text,&size)&&size==1&&text[0]==(trace->output?'B':'A'));
    ++trace->output;return !trace->reject;
}
static void init_trace(void *context,XrXirLifecycleEvent event,uint32_t index){
    InitTrace *trace=context;
    if(event==XR_XIR_MODULE_BEGIN)++trace->begins;
    else if(event==XR_XIR_MODULE_READY)++trace->ready;
    else if(event==XR_XIR_SLOT_PUBLISHED){CHECK(trace->published<16);trace->stack[trace->published++]=index;}
    else {CHECK(event==XR_XIR_SLOT_RELEASED&&trace->released<trace->published);
        CHECK(index==trace->stack[trace->published-1-trace->released++]);}
}
static void init_failure_value(const XrXirValue *error){
    XrXirValue concrete=*error;
    CHECK(xr_xir_type_is_enum(xr_xir_type_arena_types(xr_xir_value_arena(error)),(XrXirType)error->type));
    XrXirDomain *reader=NULL;CHECK(xr_xir_domain_new(65536,&reader)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={xr_xir_value_arena(error),reader,NULL,NULL,10000,65536};
    XrXirValue text={0},count={0};
    CHECK(xr_xir_enum_get(&concrete,0,0,&admission,&text)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_enum_get(&concrete,0,1,&admission,&count)==XR_XIR_VALUE_OK);
    const char *bytes=NULL;size_t length=0;const char expected[]="initialization-owned-long-payload-after-owner-destruction";
    CHECK(xr_xir_string_view(&text,&bytes,&length)&&length==sizeof(expected)-1&&!memcmp(bytes,expected,length));
    CHECK(count.type==XR_XIR_I64&&count.payload==41);xr_xir_value_drop(&text);xr_xir_value_drop(&count);xr_xir_domain_drop(reader);
}
static void init_pair(XrXirProgram *program,uint32_t entry,XrXirValue held[3]){
    XrXirInstance *instances[2]={0};InitTrace traces[2]={{0}};XrXirInstanceResult paused[2];
    for(unsigned i=0;i<2;++i){XrXirInstanceConfig c=xr_xir_instance_defaults();c.output=(XrXirOutputProvider){init_output,&traces[i]};c.trace=init_trace;c.trace_context=&traces[i];
        CHECK(xr_xir_instance_new(program,&c,&instances[i])==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instances[i],entry,NULL,0)==XR_XIR_CALL_READY);
        paused[i]=xr_xir_instance_poll(instances[i]);CHECK(paused[i].outcome.status==XR_XIR_CALL_SUSPENDED);
        CHECK(traces[i].output==2&&traces[i].begins==2&&traces[i].ready==1&&!traces[i].released);
        CHECK(xr_xir_instance_start(instances[i],entry,NULL,0)==XR_XIR_CALL_BUSY);
    }
    for(unsigned i=0;i<2;++i){CHECK(xr_xir_instance_resume(instances[i],paused[i].epoch,paused[i].outcome.wake)==XR_XIR_CALL_READY);
        XrXirInstanceResult failed=xr_xir_instance_poll(instances[i]);CHECK(failed.outcome.status==XR_XIR_CALL_THROWN);init_failure_value(&failed.outcome.value);
        CHECK(traces[i].released==traces[i].published&&traces[i].published==3&&traces[i].output==2&&traces[i].ready==1);
        if(!i)CHECK(xr_xir_instance_state(instances[1])==XR_XIR_INSTANCE_INITIALIZING&&!traces[1].released);
        for(unsigned n=0;n<3;++n){CHECK(xr_xir_instance_start(instances[i],entry,NULL,0)==XR_XIR_CALL_THROWN);
            CHECK(xr_xir_instance_poll(instances[i]).outcome.status==XR_XIR_CALL_THROWN&&traces[i].output==2);}
        XrXirValue no_take={XR_XIR_I64,0,73};CHECK(xr_xir_instance_take_result(instances[i],&no_take)==XR_XIR_CALL_BAD_STATE&&no_take.type==XR_XIR_I64&&no_take.payload==73);
        XrXirCallResult copy={0};CHECK(xr_xir_instance_copy_failure(instances[i],&copy)==XR_XIR_CALL_THROWN);held[i]=copy.value;
        CHECK(xr_xir_instance_free(instances[i])==XR_XIR_CALL_READY);
    }
    /* A new instance retries; none of the two failed instances is revived. */
    InitTrace trace={0};XrXirInstanceConfig c=xr_xir_instance_defaults();c.output=(XrXirOutputProvider){init_output,&trace};c.trace=init_trace;c.trace_context=&trace;
    XrXirInstance *retry=NULL;CHECK(xr_xir_instance_new(program,&c,&retry)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(retry,entry,NULL,0)==XR_XIR_CALL_READY);XrXirInstanceResult r=xr_xir_instance_poll(retry);
    CHECK(r.outcome.status==XR_XIR_CALL_SUSPENDED);CHECK(xr_xir_instance_resume(retry,r.epoch,r.outcome.wake)==XR_XIR_CALL_READY);
    r=xr_xir_instance_poll(retry);CHECK(r.outcome.status==XR_XIR_CALL_THROWN);init_failure_value(&r.outcome.value);
    XrXirCallResult copy={0};CHECK(xr_xir_instance_copy_failure(retry,&copy)==XR_XIR_CALL_THROWN);held[2]=copy.value;
    CHECK(trace.output==2&&trace.released==trace.published&&xr_xir_instance_free(retry)==XR_XIR_CALL_READY);
}
static void init_retained(XrXirValue held[3]){for(unsigned i=0;i<3;++i){init_failure_value(&held[i]);xr_xir_value_drop(&held[i]);}}
static void init_protocol_failures(XrXirProgram *program,uint32_t entry){
    for(unsigned mode=0;mode<2;++mode){InitTrace trace={0};trace.reject=mode!=0;
        XrXirInstanceConfig c=xr_xir_instance_defaults();c.output=(XrXirOutputProvider){init_output,&trace};c.trace=init_trace;c.trace_context=&trace;
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&c,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);XrXirInstanceResult r=xr_xir_instance_poll(instance);
        if(!mode){CHECK(r.outcome.status==XR_XIR_CALL_SUSPENDED);CHECK(xr_xir_instance_stop(instance)==XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_resume(instance,r.epoch,r.outcome.wake)==XR_XIR_CALL_BAD_STATE);r=xr_xir_instance_poll(instance);}
        XrXirCallStatus expected=mode?XR_XIR_CALL_OUTPUT_ERROR:XR_XIR_CALL_CANCELLED;
        CHECK(r.outcome.status==expected&&trace.output==(mode?1u:2u)&&trace.released==trace.published);
        CHECK(xr_xir_instance_start(instance,entry,NULL,0)==expected);XrXirCallResult copy={0};
        XrXirValue no_take={XR_XIR_I64,0,73};CHECK(xr_xir_instance_take_result(instance,&no_take)==XR_XIR_CALL_BAD_STATE&&no_take.type==XR_XIR_I64&&no_take.payload==73);
        CHECK(xr_xir_instance_copy_failure(instance,&copy)==expected&&!copy.value.type&&xr_xir_fault_empty(copy.panic.detail));
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
}
static void init_runtime_faults(XrXirProgram *program,uint32_t entry){
    size_t live_before=runtime_live,bytes_before=runtime_bytes,sites=0;
    for(size_t pass=0;pass<=sites;++pass){runtime_attempts=0;runtime_fail_at=pass?pass-1:SIZE_MAX;
        InitTrace trace={0};XrXirInstanceConfig c=xr_xir_instance_defaults();c.output=(XrXirOutputProvider){init_output,&trace};c.trace=init_trace;c.trace_context=&trace;
        XrXirInstance *instance=NULL;XrXirCallResult held={0};XrXirCallStatus status=xr_xir_instance_new(program,&c,&instance);
        if(status==XR_XIR_CALL_READY)status=xr_xir_instance_start(instance,entry,NULL,0);
        if(status==XR_XIR_CALL_READY){XrXirInstanceResult r=xr_xir_instance_poll(instance);status=r.outcome.status;
            if(status==XR_XIR_CALL_SUSPENDED){CHECK(xr_xir_instance_resume(instance,r.epoch,r.outcome.wake)==XR_XIR_CALL_READY);status=xr_xir_instance_poll(instance).outcome.status;}}
        if(status==XR_XIR_CALL_THROWN)status=xr_xir_instance_copy_failure(instance,&held);
        if(!pass){CHECK(status==XR_XIR_CALL_THROWN);sites=runtime_attempts;CHECK(sites);}
        else {if(status!=XR_XIR_CALL_OOM)fprintf(stderr,"init runtime failure %zu/%zu status %u\n",pass-1,sites,status);CHECK(status==XR_XIR_CALL_OOM);}
        if(instance)CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_value_drop(&held.value);
        CHECK(runtime_live==live_before&&runtime_bytes==bytes_before);
    }
    runtime_fail_at=SIZE_MAX;printf("initialization runtime %zu OOM sites, physical baseline restored\n",sites);
}
