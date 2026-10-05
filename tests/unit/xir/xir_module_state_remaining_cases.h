/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_module_state_remaining_cases.h - Independent four-slot isolation, cleanup and fault oracles
 *
 * KEY CONCEPT:
 *   Finite complete Programs preserve real ownership and physical cleanup.
 */

#ifndef XIR_MODULE_STATE_REMAINING_CASES_H
#define XIR_MODULE_STATE_REMAINING_CASES_H
#include "xir/xxir_error.h"
#include "xir/xxir_enum.h"
static bool remaining_yields_case(uint32_t scenario) {return scenario==7 || scenario==11 || scenario==12;}
static bool remaining_has_error(uint32_t scenario) {return scenario==9 || scenario==11;}
static void remaining_trace(void *context,XrXirLifecycleEvent event,uint32_t index) {
    RemainingTrace *t=context;CHECK(t && t->instance && index<4);
    CHECK(xr_xir_instance_start(t->instance,4,NULL,0)==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_cancel_current(t->instance)==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_free(t->instance)==XR_XIR_CALL_BUSY);
    if(event==XR_XIR_SLOT_RELEASED) {
        CHECK(t->release_count<t->publication_count);
        CHECK(index==t->published[t->publication_count-t->release_count-1]);t->released[t->release_count++]=index;
    } else {
        const uint32_t expected[]={0,4,1,5,2,6,3,9,8,11};
        CHECK(t->trace_count<10 && (uint32_t)event*4+index==expected[t->trace_count]);
        t->trace[t->trace_count++]=(uint32_t)event*4+index;
        if(event==XR_XIR_SLOT_PUBLISHED) {
            const uint32_t slots[]={1,0,3};CHECK(t->publication_count<3 && index==slots[t->publication_count]);
            t->published[t->publication_count++]=index;
        }
    }
}
static XrXirOutputStatus remaining_output(void *context,const XrXirOutputGroup *group) {
    RemainingTrace *trace=context;CHECK(trace && group && group->stream==XR_XIR_STDOUT);++trace->output;return XR_XIR_OUTPUT_OK;
}
static XrXirInstanceConfig remaining_config(RemainingTrace *trace) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.metadata_limit=1024*1024;config.value_limit=1024*1024;config.call_limit=1024*1024;
    config.poll_limit=1000000;config.depth_limit=64;config.trace=remaining_trace;config.trace_context=trace;
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,remaining_output,trace};return config;
}
static XrXirCallStatus remaining_expected(uint32_t scenario,uint32_t mode) {
    return mode?XR_XIR_CALL_CANCELLED:scenario==7?XR_XIR_CALL_BAD_STATE:
        remaining_has_error(scenario)?XR_XIR_CALL_THROWN:XR_XIR_CALL_ASSERTION;
}
static void remaining_pause(RemainingTrace *trace,const XrXirInstanceResult *result) {
    CHECK(result->outcome.status==XR_XIR_CALL_SUSPENDED && result->outcome.wake && result->epoch);
    CHECK(!result->outcome.value.type && !result->outcome.value.payload);
    CHECK(trace->trace_count==(trace->scenario==7?10u:7u) && !trace->output);
    if(trace->scenario==7)CHECK(trace->constructed==3 && trace->publication_count==3 && !trace->free_count);
    else {
        XrXirDomainStats domain=xr_xir_domain_stats(trace->instance->domain);
        CHECK(!trace->constructed && !trace->publication_count && !trace->replacements && !trace->free_count);
        CHECK(domain.allocations==1 && !domain.frees && domain.live_bytes==sizeof(XrXirDomain));
    }
    CHECK(xr_xir_instance_resume(trace->instance,result->epoch+1,result->outcome.wake)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(trace->instance,result->epoch,result->outcome.wake+1)==XR_XIR_CALL_BAD_STATE);
}
static XrXirInstanceResult remaining_drive(RemainingTrace *trace,uint32_t mode,bool strict_pause) {
    XrXirInstanceResult result;
    do {result=xr_xir_instance_poll_bounded(trace->instance,8);}while(result.outcome.status==XR_XIR_CALL_READY);
    if(result.outcome.status==XR_XIR_CALL_SUSPENDED) {
        if(strict_pause)remaining_pause(trace,&result);
        if(mode==0)CHECK(xr_xir_instance_resume(trace->instance,result.epoch,result.outcome.wake)==XR_XIR_CALL_READY);
        else if(mode==1)CHECK(xr_xir_instance_cancel_current(trace->instance)==XR_XIR_CALL_CANCEL_REQUESTED);
        else {
            CHECK(mode==2 && xr_xir_instance_stop(trace->instance)==XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_state(trace->instance)==XR_XIR_INSTANCE_DRAINING && trace->instance->state==XR_XIR_INSTANCE_INITIALIZING);
            CHECK(!trace->release_count && !trace->free_count);
        }
        do {result=xr_xir_instance_poll_bounded(trace->instance,8);}while(result.outcome.status==XR_XIR_CALL_READY);
    }
    return result;
}
static void remaining_error_view(RemainingTrace *trace,const XrXirValue *value) {
    if(value->type!=XR_XIR_CONSTRUCTED_TYPE_BASE+1 || !value->payload || !xr_xir_value_valid(value)) fprintf(stderr,"error view scenario=%u type=%u payload=%lld freed=%u releases=%u instance=%p refs=%u kind=%u object_type=%u\n",trace->scenario,value->type,(long long)value->payload,trace->free_count,trace->release_count,(void *)trace->instance,value->payload?atomic_load(&object_pointer(value)->references):0,value->payload?object_pointer(value)->kind:0,value->payload?object_pointer(value)->type:0);
    CHECK(value->type==XR_XIR_CONSTRUCTED_TYPE_BASE+1 && value->payload && xr_xir_value_valid(value));
    XrXirValueAdmission admission={xr_xir_value_arena(value),object_pointer(value)->domain,NULL,NULL,100000,65536};
    const XrXirTypes *types=xr_xir_compile_type_arena_types(admission.arena);
    CHECK(types && types->count==2 && types->nominals && types->nominals->identities[1].exported);
    CHECK(!(types->nominals->identities[0].fields[0].flags & XR_XIR_FIELD_PRIVATE));
    XrXirValue existential={0},borrowed={0},narrowed={0},erased={0},state={0},text={0};
    CHECK(xr_xir_error_erase(value,&admission,&existential)==XR_XIR_VALUE_OK && existential.type==XR_XIR_ERROR && existential.payload==value->payload);
    CHECK(xr_xir_error_borrow(&existential,&borrowed) && borrowed.type==XR_XIR_CONSTRUCTED_TYPE_BASE+1 && borrowed.payload==value->payload);
    CHECK(xr_xir_error_narrow(&existential,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1),&admission,&narrowed)==XR_XIR_VALUE_OK);
    CHECK(narrowed.payload==value->payload && xr_xir_error_erase(&narrowed,&admission,&erased)==XR_XIR_VALUE_OK && erased.payload==value->payload);
    XrXirEnumBorrow view={0};CHECK(xr_xir_enum_borrow(&narrowed,&view)==XR_XIR_VALUE_OK);
    CHECK(view.name.length==11 && !memcmp(view.name.bytes,"InitFailure",11) && view.member.length==5 && !memcmp(view.member.bytes,"Cause",5));
    CHECK(view.field_count==1 && view.fields[0].type==XR_XIR_CONSTRUCTED_TYPE_BASE && (uintptr_t)view.fields[0].payload==trace->classes[2]);
    CHECK(xr_xir_enum_get(&narrowed,0,0,&admission,&state)==XR_XIR_VALUE_OK && (uintptr_t)state.payload==trace->classes[2]);
    trace->inspecting=true;CHECK(xr_xir_class_get(&state,0,&admission,&text)==XR_XIR_VALUE_OK);trace->inspecting=false;
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&text,&bytes,&length) && length==1 && bytes[0]=='x');
    xr_xir_value_drop(&text);xr_xir_value_drop(&state);xr_xir_value_drop(&erased);xr_xir_value_drop(&narrowed);xr_xir_value_drop(&existential);
}
static void remaining_oracle(RemainingTrace *trace,const XrXirCallResult *outcome,uint32_t mode) {
    CHECK(outcome->status==remaining_expected(trace->scenario,mode) && !outcome->wake && !trace->output);
    if(mode || trace->scenario==7) {
        CHECK(!outcome->value.type && !outcome->value.payload && xr_xir_panic_empty(&outcome->panic));
        CHECK(trace->constructed==(trace->scenario==7?3u:0u) && trace->free_count==trace->constructed && trace->release_count==trace->publication_count);
        if(trace->scenario==7) {const uint32_t freed[]={3,2,1};CHECK(!memcmp(trace->freed,freed,sizeof(freed)));}return;
    }
    CHECK(trace->constructed==3 && trace->publication_count==3 && trace->replacements==3 && trace->release_count==3);
    const uint32_t released[]={3,0,1};CHECK(!memcmp(trace->released,released,sizeof(released)));
    for(uint32_t n=0;n<3;++n)CHECK(trace->old_freed[n]==1);
    if(remaining_has_error(trace->scenario)) {
        const uint32_t freed[]={2,1};CHECK(trace->free_count==2 && !memcmp(trace->freed,freed,sizeof(freed)));
        CHECK(xr_xir_panic_empty(&outcome->panic));remaining_error_view(trace,&outcome->value);
    } else {
        const uint32_t freed[]={3,2,1};CHECK(trace->free_count==3 && !memcmp(trace->freed,freed,sizeof(freed)));
        CHECK(!outcome->value.type && !outcome->value.payload && outcome->panic.detail.code==XR_XIR_PANIC_ASSERTION);
        CHECK(!outcome->panic.detail.index && !outcome->panic.detail.length && !outcome->panic.detail.reserved);
        const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&outcome->panic.message,&bytes,&length));
        const char *expected=trace->scenario==6?"module initializer assertion":trace->scenario==13?"x":"";
        CHECK(length==strlen(expected) && (!length || !memcmp(bytes,expected,length)));
    }
}
static void remaining_copy_limit(RemainingTrace *trace) {
    XrXirCallResult *failure=&trace->instance->failure;XrXirCallResult cached=*failure;
    const XrXirValue *value=remaining_has_error(trace->scenario)?&failure->value:&failure->panic.message;
    XirObject *object=object_pointer(value);uint32_t references=atomic_load(&object->references);
    size_t live=runtime_live,bytes=runtime_bytes,attempts=runtime_attempts;
    XrXirCallResult empty={0},before=empty;atomic_store(&object->references,UINT32_MAX);
    XrXirCallStatus status=xr_xir_instance_copy_failure(trace->instance,&empty);
    atomic_store(&object->references,references);
    CHECK(status==XR_XIR_CALL_LIMIT && !memcmp(&empty,&before,sizeof(empty)));
    CHECK(!memcmp(failure,&cached,sizeof(cached)) && failure->status==remaining_expected(trace->scenario,0) && runtime_live==live && runtime_bytes==bytes && runtime_attempts==attempts);
}
static void remaining_sticky(RemainingTrace *trace,const XrXirInstanceResult *terminal,uint32_t mode) {
    for(uint32_t i=0;i<24;++i) {
        size_t attempts=runtime_attempts;
        CHECK(xr_xir_instance_start(trace->instance,4,NULL,0)==terminal->outcome.status);
        XrXirInstanceResult repeat=xr_xir_instance_poll_bounded(trace->instance,8);
        CHECK(repeat.epoch==terminal->epoch && repeat.outcome.status==terminal->outcome.status);
        CHECK(repeat.outcome.value.payload==terminal->outcome.value.payload && repeat.outcome.panic.message.payload==terminal->outcome.panic.message.payload);
        CHECK(runtime_attempts==attempts);remaining_oracle(trace,&repeat.outcome,mode);
    }
    CHECK(trace->instance->state==XR_XIR_INSTANCE_FAILED && xr_xir_instance_state(trace->instance)==(trace->instance->stopping?XR_XIR_INSTANCE_DRAINING:XR_XIR_INSTANCE_FAILED));
    XrXirValue sentinel={XR_XIR_I64,0,91},before=sentinel;
    CHECK(xr_xir_instance_take_result(trace->instance,&sentinel)==XR_XIR_CALL_BAD_STATE && !memcmp(&sentinel,&before,sizeof(before)));
    XrXirCallResult occupied={.status=XR_XIR_CALL_RETURNED,.value={XR_XIR_I64,0,91}},saved=occupied;
    CHECK(xr_xir_instance_copy_failure(trace->instance,&occupied)==XR_XIR_CALL_BAD_ARGUMENT && !memcmp(&occupied,&saved,sizeof(saved)));
}
static void remaining_pair(XrXirProgram *program,uint32_t scenario,uint32_t mode,uint32_t order) {
    CHECK(!runtime_live && !runtime_bytes);RemainingTrace traces[2]={0};XrXirCallResult escaped[2]={{0},{0}};
    for(uint32_t i=0;i<2;++i) {
        traces[i].scenario=scenario;remaining_registry[i]=&traces[i];remaining_active=&traces[i];
        XrXirInstanceConfig config=remaining_config(&traces[i]);
        for(uint32_t abi=4;abi<=6;++abi) {
            XrXirInstanceConfig stale=config;stale.abi_version=abi;XrXirInstance *rejected=NULL;
            CHECK(xr_xir_instance_new(program,&stale,&rejected)==XR_XIR_CALL_BAD_ABI && !rejected);
        }
        CHECK(xr_xir_instance_new(program,&config,&traces[i].instance)==XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
    for(uint32_t i=0;i<2;++i) {
        RemainingTrace *trace=&traces[i];remaining_active=trace;
        CHECK(xr_xir_instance_start(trace->instance,4,NULL,0)==XR_XIR_CALL_READY);
        XrXirInstanceResult terminal=remaining_drive(trace,mode,true);remaining_oracle(trace,&terminal.outcome,mode);
        remaining_sticky(trace,&terminal,mode);
        if(!mode && scenario!=7)remaining_copy_limit(trace);
        CHECK(xr_xir_instance_copy_failure(trace->instance,&escaped[i])==terminal.outcome.status);
        remaining_oracle(trace,&escaped[i],mode);
    }
    if(!mode && remaining_has_error(scenario))CHECK(traces[0].classes[2]!=traces[1].classes[2] && traces[0].instance->domain!=traces[1].instance->domain);
    for(uint32_t n=0;n<2;++n) {
        uint32_t i=order?1-n:n;RemainingTrace *trace=&traces[i];remaining_active=trace;
        CHECK(xr_xir_instance_stop(trace->instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_free(trace->instance)==XR_XIR_CALL_READY);trace->instance=NULL;
        remaining_oracle(trace,&escaped[i],mode);
    }
    for(uint32_t i=0;i<2;++i) {
        remaining_active=&traces[i];remaining_oracle(&traces[i],&escaped[i],mode);xr_xir_call_result_drop(&escaped[i]);
        CHECK(traces[i].free_count==traces[i].constructed);
        if(!mode || scenario==7) {
            const uint32_t error_frees[]={2,1,3},ordinary[]={3,2,1};
            CHECK(traces[i].free_count==3 && !memcmp(traces[i].freed,remaining_has_error(scenario)&&!mode?error_frees:ordinary,sizeof(ordinary)));
        }
    }
    CHECK(!runtime_live && !runtime_bytes);remaining_active=NULL;remaining_registry[0]=remaining_registry[1]=NULL;
    printf("remaining %u mode %u order %u: two x24 sticky, exact slots/class frees, owned escaped failure, physical0/0\n",scenario,mode,order);
}

static void remaining_empty_sticky(RemainingTrace *trace,XrXirCallStatus reason,uint64_t epoch) {
    size_t attempts=runtime_attempts;
    for(uint32_t i=0;i<24;++i) {
        CHECK(xr_xir_instance_start(trace->instance,4,NULL,0)==reason);
        XrXirInstanceResult result=xr_xir_instance_poll_bounded(trace->instance,8);
        CHECK(result.outcome.status==reason && result.epoch==epoch && !result.outcome.value.type &&
            !result.outcome.value.payload && xr_xir_panic_empty(&result.outcome.panic));
    }
    CHECK(attempts==runtime_attempts && !trace->output);
    XrXirCallResult copy={0};CHECK(xr_xir_instance_copy_failure(trace->instance,&copy)==reason);
    CHECK(!copy.value.type && xr_xir_panic_empty(&copy.panic));xr_xir_call_result_drop(&copy);
    XrXirValue sentinel={XR_XIR_I64,0,91},before=sentinel;
    CHECK(xr_xir_instance_take_result(trace->instance,&sentinel)==XR_XIR_CALL_BAD_STATE && !memcmp(&sentinel,&before,sizeof(before)));
}
static size_t remaining_runtime_run(XrXirProgram *program,uint32_t scenario,uint32_t mode,size_t fail_at) {
    CHECK(!runtime_live && !runtime_bytes);RemainingTrace trace={0};trace.scenario=scenario;
    remaining_registry[0]=&trace;remaining_active=&trace;runtime_attempts=0;runtime_fail_at=fail_at;
    XrXirInstanceConfig config=remaining_config(&trace);
    XrXirCallStatus status=xr_xir_instance_new(program,&config,&trace.instance);
    if(status==XR_XIR_CALL_READY) {
        status=xr_xir_instance_start(trace.instance,4,NULL,0);
        if(status==XR_XIR_CALL_READY) {
            XrXirInstanceResult result=remaining_drive(&trace,mode,false);
            if(fail_at==SIZE_MAX)CHECK(result.outcome.status==remaining_expected(scenario,mode));
            else CHECK(runtime_attempts>fail_at && result.outcome.status==XR_XIR_CALL_OOM);
            runtime_fail_at=SIZE_MAX;
            if(fail_at!=SIZE_MAX)remaining_empty_sticky(&trace,XR_XIR_CALL_OOM,result.epoch);
        } else CHECK(runtime_attempts>fail_at && status==XR_XIR_CALL_OOM);
        runtime_fail_at=SIZE_MAX;CHECK(xr_xir_instance_free(trace.instance)==XR_XIR_CALL_READY);
        CHECK(trace.release_count==trace.publication_count && trace.free_count==trace.constructed);
    } else CHECK(runtime_attempts>fail_at && status==XR_XIR_CALL_OOM && !trace.instance);
    size_t attempts=runtime_attempts;runtime_fail_at=SIZE_MAX;
    CHECK(!runtime_live && !runtime_bytes);remaining_active=NULL;remaining_registry[0]=NULL;return attempts;
}
static void remaining_publication_limit(XrXirProgram *program,uint32_t scenario) {
    RemainingTrace trace={0};trace.scenario=scenario;remaining_registry[0]=&trace;remaining_active=&trace;
    XrXirInstanceConfig config=remaining_config(&trace);
    CHECK(xr_xir_instance_new(program,&config,&trace.instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(trace.instance,4,NULL,0)==XR_XIR_CALL_READY);
    XrXirCallResult result;
    for(;;) {
        trace.instance->driving=true;result=xr_xir_call_poll_bounded(trace.instance->call,8);trace.instance->driving=false;
        if(result.status==XR_XIR_CALL_READY)continue;
        if(result.status!=XR_XIR_CALL_SUSPENDED)break;
        XrXirInstanceResult pause={result,trace.instance->epoch};remaining_pause(&trace,&pause);
        CHECK(xr_xir_instance_resume(trace.instance,pause.epoch,result.wake)==XR_XIR_CALL_READY);
    }
    CHECK(result.status==remaining_expected(scenario,0) && trace.constructed==3 && !trace.release_count && !trace.free_count);
    const XrXirValue *value=remaining_has_error(scenario)?&result.value:&result.panic.message;
    XirObject *object=object_pointer(value);CHECK(object);uint32_t references=atomic_load(&object->references);
    size_t attempts=runtime_attempts;atomic_store(&object->references,UINT32_MAX);
    XrXirInstanceResult terminal=xr_xir_instance_poll_bounded(trace.instance,8);
    atomic_store(&object->references,references);
    CHECK(terminal.outcome.status==XR_XIR_CALL_LIMIT && attempts==runtime_attempts);
    CHECK(trace.release_count==3 && !terminal.outcome.value.type && xr_xir_panic_empty(&terminal.outcome.panic));
    remaining_empty_sticky(&trace,XR_XIR_CALL_LIMIT,terminal.epoch);
    CHECK(xr_xir_instance_free(trace.instance)==XR_XIR_CALL_READY && trace.free_count==3);
    const uint32_t ordinary[]={3,2,1},error[]={2,1,3};
    CHECK(!memcmp(trace.freed,remaining_has_error(scenario)?error:ordinary,sizeof(ordinary)));
    CHECK(!runtime_live && !runtime_bytes);remaining_active=NULL;remaining_registry[0]=NULL;
}
static void remaining_direct_free_pair(XrXirProgram *program,uint32_t order) {
    RemainingTrace traces[2]={0};
    for(uint32_t i=0;i<2;++i) {
        traces[i].scenario=7;remaining_registry[i]=&traces[i];remaining_active=&traces[i];
        XrXirInstanceConfig config=remaining_config(&traces[i]);
        CHECK(xr_xir_instance_new(program,&config,&traces[i].instance)==XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
    for(uint32_t i=0;i<2;++i) {
        remaining_active=&traces[i];
        CHECK(xr_xir_instance_start(traces[i].instance,4,NULL,0)==XR_XIR_CALL_READY);
        XrXirInstanceResult result;
        do {result=xr_xir_instance_poll_bounded(traces[i].instance,8);}while(result.outcome.status==XR_XIR_CALL_READY);
        remaining_pause(&traces[i],&result);
    }
    for(uint32_t n=0;n<3;++n)for(uint32_t j=0;j<3;++j)CHECK(traces[0].classes[n]!=traces[1].classes[j]);
    for(uint32_t n=0;n<2;++n) {
        uint32_t i=order?1-n:n;remaining_active=&traces[i];
        CHECK(xr_xir_instance_free(traces[i].instance)==XR_XIR_CALL_READY);traces[i].instance=NULL;
        const uint32_t slots[]={3,0,1},freed[]={3,2,1};
        CHECK(traces[i].release_count==3 && traces[i].free_count==3 && !traces[i].output);
        CHECK(!memcmp(traces[i].released,slots,sizeof(slots)) && !memcmp(traces[i].freed,freed,sizeof(freed)));
    }
    CHECK(!runtime_live && !runtime_bytes);remaining_active=NULL;remaining_registry[0]=remaining_registry[1]=NULL;
}
static size_t remaining_cancel_prefix(XrXirProgram *program,uint32_t scenario,size_t prefix,uint32_t mode) {
    RemainingTrace trace={0};trace.scenario=scenario;remaining_registry[0]=&trace;remaining_active=&trace;
    XrXirInstanceConfig config=remaining_config(&trace);CHECK(xr_xir_instance_new(program,&config,&trace.instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(trace.instance,4,NULL,0)==XR_XIR_CALL_READY);size_t polls=0;
    XrXirInstanceResult result={0};
    for(;;) {
        if(polls==prefix) {
            bool latched=trace.instance->call->aborting;
            if(latched)CHECK(scenario==7 && trace.instance->call->abort_reason==XR_XIR_CALL_BAD_STATE);
            if(mode==1)CHECK(xr_xir_instance_cancel_current(trace.instance)==(latched?XR_XIR_CALL_BAD_STATE:XR_XIR_CALL_CANCEL_REQUESTED));
            else CHECK(mode==2 && xr_xir_instance_stop(trace.instance)==(latched?XR_XIR_CALL_BAD_STATE:XR_XIR_CALL_READY));
            do {result=xr_xir_instance_poll_bounded(trace.instance,1);}while(result.outcome.status==XR_XIR_CALL_READY);
            XrXirCallStatus reason=latched?XR_XIR_CALL_BAD_STATE:XR_XIR_CALL_CANCELLED;
            CHECK(result.outcome.status==reason);remaining_empty_sticky(&trace,reason,result.epoch);break;
        }
        result=xr_xir_instance_poll_bounded(trace.instance,1);++polls;
        if(result.outcome.status==XR_XIR_CALL_SUSPENDED) {
            CHECK(xr_xir_instance_resume(trace.instance,result.epoch,result.outcome.wake)==XR_XIR_CALL_READY);continue;
        }
        if(result.outcome.status!=XR_XIR_CALL_READY) {CHECK(result.outcome.status==remaining_expected(scenario,0));break;}
    }
    CHECK(xr_xir_instance_free(trace.instance)==XR_XIR_CALL_READY);
    CHECK(trace.release_count==trace.publication_count && trace.free_count==trace.constructed && !trace.output);
    CHECK(!runtime_live && !runtime_bytes);remaining_active=NULL;remaining_registry[0]=NULL;return polls;
}
static void remaining_runtime_scan(XrXirProgram *program,uint32_t scenario) {
    uint32_t modes=remaining_yields_case(scenario)?3u:1u;
    for(uint32_t mode=0;mode<modes;++mode) {
        size_t points=remaining_runtime_run(program,scenario,mode,SIZE_MAX);
        for(size_t point=0;point<points;++point)CHECK(remaining_runtime_run(program,scenario,mode,point)>point);
        printf("remaining %u runtime mode %u: every %zu actual allocation ordinal; physical0/0\n",scenario,mode,points);
    }
    if(scenario!=7) {
        remaining_publication_limit(program,scenario);
        printf("remaining %u: real carrier failure-publication retain saturation normalized LIMIT/cache empty, physical0/0\n",scenario);
    }
    size_t polls=remaining_cancel_prefix(program,scenario,SIZE_MAX,1);
    for(uint32_t mode=1;mode<=2;++mode)for(size_t prefix=0;prefix<polls;++prefix)
        (void)remaining_cancel_prefix(program,scenario,prefix,mode);
    printf("remaining %u: cancel+stop all %zu quantum1 prefixes; physical0/0\n",scenario,polls);
    xr_xir_compile_program_drop(program);
}
#endif // XIR_MODULE_STATE_REMAINING_CASES_H
