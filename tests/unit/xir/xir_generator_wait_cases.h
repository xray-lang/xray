/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generator_wait_cases.h - Exact external wakes and cancellation ownership
 */
#ifndef XIR_GENERATOR_WAIT_CASES_H
#define XIR_GENERATOR_WAIT_CASES_H
#include "xir/xxir_generator_internal.h"
#include <string.h>
typedef struct GeneratorWaitLog {unsigned count,relay,cycle;} GeneratorWaitLog;
static XrXirOutputStatus wait_output(void *opaque,const XrXirOutputGroup *group) {
    GeneratorWaitLog *log=opaque;CHECK(group && group->line && group->count==1 && group->stream==XR_XIR_STDOUT);
    CHECK(group->values[0].type==XR_XIR_I64);
    if(group->values[0].payload==401) ++log->count;
    else if(group->values[0].payload==402) ++log->relay;
    else {CHECK(group->values[0].payload==403);++log->cycle;}
    return XR_XIR_OUTPUT_OK;
}
static XrXirInstance *wait_instance(XrXirProgram *program,GeneratorWaitLog *log) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,wait_output,log};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);return instance;
}
static XrXirValue wait_returned(XrXirInstance *instance) {
    XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1000000);
    if(result.outcome.status!=XR_XIR_CALL_RETURNED) fprintf(stderr,"Expected return, observed%u\n",result.outcome.status);
    CHECK(result.outcome.status==XR_XIR_CALL_RETURNED);XrXirValue value={0};
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);return value;
}
static XrXirInstanceResult wait_timer(XrXirInstance *instance,uint64_t ms) {
    XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1000000);
    if(result.outcome.status!=XR_XIR_CALL_SUSPENDED) fprintf(stderr,"Expected Timer, observed%u\n",result.outcome.status);
    CHECK(result.outcome.status==XR_XIR_CALL_SUSPENDED && result.epoch && result.outcome.wake);
    XrXirWaitRequest request={0};CHECK(xr_xir_instance_wait_request(instance,result.epoch,result.outcome.wake,&request)==XR_XIR_CALL_READY);
    CHECK(request.kind==XR_XIR_WAIT_TIMER_MS && request.after_ms==ms && !request.reserved &&
        !request.subject && !request.generation && !request.ticket);return result;
}
static XrXirValue wait_make(XrXirInstance *instance,uint32_t entry) {
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);return wait_returned(instance);
}
static void wait_string(const XrXirValue *value,const char *expected) {
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(value,&bytes,&length));
    CHECK(length==strlen(expected) && !memcmp(bytes,expected,length));
}
static void wait_resume(XrXirInstance *instance,XrXirInstanceResult pending) {
    CHECK(xr_xir_instance_resume(instance,pending.epoch+1,pending.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(instance,pending.epoch,pending.outcome.wake+1)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(instance,pending.epoch,pending.outcome.wake)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_resume(instance,pending.epoch,pending.outcome.wake)==XR_XIR_CALL_BAD_STATE);
}
static void wait_invalid_tokens(XirGenerator *generator,bool nested) {
    XrXirCall *consumer=generator->consumer;XrXirTaskWaitToken exact={0};
    CHECK(consumer && xr_xir_call_iterator_wait_token(consumer,&exact));
    CHECK(xr_xir_call_resume(consumer,exact.wake)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_call_link_iterator_wait(consumer,&exact)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_call_complete_iterator_wait(consumer,&exact)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_call_fail_iterator_wait(consumer,&exact,XR_XIR_CALL_RETURNED)==XR_XIR_CALL_BAD_STATE);
    for(unsigned mode=0;mode<11;++mode) {
        XrXirTaskWaitToken forged=exact;
        switch(mode) {
        case 0:forged.binding.owner=NULL;break;
        case 1:forged.binding.activation=NULL;break;
        case 2:++forged.binding.generation;break;
        case 3:++forged.binding.ticket;break;
        case 4:++forged.wake;break;
        case 5:forged.request.kind=forged.request.kind==XR_XIR_WAIT_ITERATOR_NEXT ?
            XR_XIR_WAIT_ITERATOR_HAS_NEXT : XR_XIR_WAIT_ITERATOR_NEXT;break;
        case 6:forged.request.reserved=1;break;
        case 7:forged.request.after_ms=1;break;
        case 8:forged.request.subject=NULL;break;
        case 9:++forged.request.generation;break;
        default:++forged.request.ticket;break;
        }
        CHECK(xr_xir_call_link_iterator_wait(consumer,&forged)==XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_call_complete_iterator_wait(consumer,&forged)==XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_call_fail_iterator_wait(consumer,&forged,XR_XIR_CALL_LIMIT)==XR_XIR_CALL_BAD_STATE);
        XrXirTaskWaitToken current={0};CHECK(xr_xir_call_iterator_wait_token(consumer,&current));
        CHECK(!memcmp(&current,&exact,sizeof(exact)) && generator->consumer==consumer);
    }
    XrXirTaskWaitToken preserved=exact;
    bool own_wait=xr_xir_call_iterator_wait_token(generator->call,&preserved);
    if(nested) CHECK(own_wait && preserved.binding.activation==&generator->activation && preserved.request.subject!=generator);
    else CHECK(!own_wait && !memcmp(&preserved,&exact,sizeof(exact)));
}
static void wait_normal_case(XrXirProgram *program,const uint32_t ids[8],bool probe) {
    GeneratorWaitLog log={0};XrXirInstance *instance=wait_instance(program,&log);
    XrXirValue iterator=wait_make(instance,ids[0]);XirGenerator *generator=(XirGenerator *)(uintptr_t)iterator.payload;
    CHECK(!generator->accounting.polls && !log.count);
    for(unsigned n=0;n<2;++n) {
        CHECK(xr_xir_instance_start(instance,ids[probe ? 1 : 2],&iterator,1)==XR_XIR_CALL_READY);
        XrXirInstanceResult pending=wait_timer(instance,n ? 7 : 5);wait_invalid_tokens(generator,false);
        uint64_t polls=generator->accounting.polls;XrXirInstanceResult same=wait_timer(instance,n ? 7 : 5);
        CHECK(same.epoch==pending.epoch && same.outcome.wake==pending.outcome.wake && generator->accounting.polls==polls);
        wait_resume(instance,pending);XrXirValue value=wait_returned(instance);
        if(probe) {
            CHECK(value.type==XR_XIR_BOOL && value.payload==1);xr_xir_value_drop(&value);
            CHECK(xr_xir_instance_start(instance,ids[1],&iterator,1)==XR_XIR_CALL_READY);
            polls=generator->accounting.polls;value=wait_returned(instance);
            CHECK(value.type==XR_XIR_BOOL && value.payload==1 && generator->accounting.polls==polls);xr_xir_value_drop(&value);
            CHECK(xr_xir_instance_start(instance,ids[2],&iterator,1)==XR_XIR_CALL_READY);value=wait_returned(instance);
        }
        wait_string(&value,n ? "two" : "one");xr_xir_value_drop(&value);CHECK(!log.count);
        CHECK(xr_xir_instance_resume(instance,pending.epoch,pending.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    }
    CHECK(xr_xir_instance_start(instance,ids[1],&iterator,1)==XR_XIR_CALL_READY);XrXirValue more=wait_returned(instance);
    CHECK(more.type==XR_XIR_BOOL && !more.payload && log.count==1 && !generator->call);xr_xir_value_drop(&more);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && log.count==1);xr_xir_value_drop(&iterator);
}
static void wait_cancel_case(XrXirProgram *program,const uint32_t ids[8],unsigned mode) {
    GeneratorWaitLog log={0};XrXirInstance *instance=wait_instance(program,&log);XrXirValue iterator=wait_make(instance,ids[0]);
    XirGenerator *generator=(XirGenerator *)(uintptr_t)iterator.payload;
    CHECK(xr_xir_instance_start(instance,ids[2],&iterator,1)==XR_XIR_CALL_READY);XrXirInstanceResult pending=wait_timer(instance,5);
    if(mode==1) {
        wait_resume(instance,pending);XrXirValue one=wait_returned(instance);wait_string(&one,"one");xr_xir_value_drop(&one);
        CHECK(xr_xir_instance_start(instance,ids[2],&iterator,1)==XR_XIR_CALL_READY);pending=wait_timer(instance,7);
    }
    XrXirCall *consumer=generator->consumer;XrXirTaskWaitToken token={0};CHECK(xr_xir_call_iterator_wait_token(consumer,&token));
    if(mode==3) {
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && log.count==1);
    } else {
        CHECK((mode==2 ? xr_xir_instance_stop(instance) : xr_xir_instance_cancel_current(instance))==
            (mode==2 ? XR_XIR_CALL_READY : XR_XIR_CALL_CANCEL_REQUESTED));
        CHECK(!generator->consumer);
        CHECK(xr_xir_instance_resume(instance,pending.epoch,pending.outcome.wake)==XR_XIR_CALL_BAD_STATE);
        XrXirWaitRequest preserved={.kind=99,.after_ms=42};XrXirWaitRequest saved=preserved;
        CHECK(xr_xir_instance_wait_request(instance,pending.epoch,pending.outcome.wake,&preserved)==XR_XIR_CALL_BAD_STATE &&
            !memcmp(&preserved,&saved,sizeof(saved)));
        CHECK(xr_xir_call_complete_iterator_wait(consumer,&token)==XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_call_fail_iterator_wait(consumer,&token,XR_XIR_CALL_LIMIT)==XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_poll_bounded(instance,1000000).outcome.status==XR_XIR_CALL_CANCELLED);
        CHECK(log.count==1 && !generator->call && !generator->consumer);
        if(mode<2) {
            XrXirValue value=wait_make(instance,ids[3]);CHECK(value.type==XR_XIR_I64 && value.payload==9);xr_xir_value_drop(&value);
            CHECK(xr_xir_instance_resume(instance,pending.epoch,pending.outcome.wake)==XR_XIR_CALL_BAD_STATE);
            CHECK(xr_xir_instance_start(instance,ids[1],&iterator,1)==XR_XIR_CALL_READY);value=wait_returned(instance);
            CHECK(value.type==XR_XIR_BOOL && !value.payload);xr_xir_value_drop(&value);
        }
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && log.count==1);
    }
    CHECK(xr_xir_value_valid(&iterator) && !generator->call && !generator->consumer &&
        generator->outcome.status==XR_XIR_CALL_CANCELLED);xr_xir_value_drop(&iterator);
}
static void wait_drop_case(XrXirProgram *program,const uint32_t ids[8],bool cached) {
    GeneratorWaitLog log={0};XrXirInstance *instance=wait_instance(program,&log);XrXirValue iterator=wait_make(instance,ids[0]);
    CHECK(xr_xir_instance_start(instance,ids[cached ? 1 : 2],&iterator,1)==XR_XIR_CALL_READY);
    XrXirInstanceResult pending=wait_timer(instance,5);
    if(cached) {
        wait_resume(instance,pending);XrXirValue more=wait_returned(instance);CHECK(more.type==XR_XIR_BOOL && more.payload==1);
        xr_xir_value_drop(&more);xr_xir_value_drop(&iterator);CHECK(!log.count);
        XrXirValue value=wait_make(instance,ids[3]);CHECK(value.type==XR_XIR_I64 && value.payload==9);xr_xir_value_drop(&value);
    } else {
        xr_xir_value_drop(&iterator);CHECK(!log.count);wait_resume(instance,pending);
        XrXirValue value=wait_returned(instance);wait_string(&value,"one");xr_xir_value_drop(&value);
    }
    CHECK(log.count==1);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && log.count==1);
}
static void wait_relay_case(XrXirProgram *program,const uint32_t ids[8],bool cancel) {
    GeneratorWaitLog log={0};XrXirInstance *instance=wait_instance(program,&log);XrXirValue inner=wait_make(instance,ids[0]);
    CHECK(xr_xir_instance_start(instance,ids[4],&inner,1)==XR_XIR_CALL_READY);XrXirValue outer=wait_returned(instance);
    XirGenerator *producer=(XirGenerator *)(uintptr_t)inner.payload,*relay=(XirGenerator *)(uintptr_t)outer.payload;
    CHECK(producer!=relay && producer->call!=relay->call && !producer->accounting.polls && !relay->accounting.polls);
    CHECK(xr_xir_instance_start(instance,ids[2],&outer,1)==XR_XIR_CALL_READY);XrXirInstanceResult pending=wait_timer(instance,5);
    CHECK(producer->consumer==relay->call && relay->consumer && producer->consumer!=relay->consumer);
    wait_invalid_tokens(producer,false);wait_invalid_tokens(relay,true);
    if(cancel) {
        CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
        CHECK(xr_xir_instance_resume(instance,pending.epoch,pending.outcome.wake)==XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_poll_bounded(instance,1000000).outcome.status==XR_XIR_CALL_CANCELLED);
        CHECK(producer->outcome.status==XR_XIR_CALL_CANCELLED && relay->outcome.status==XR_XIR_CALL_CANCELLED);
    } else {
        wait_resume(instance,pending);XrXirValue value=wait_returned(instance);wait_string(&value,"one");xr_xir_value_drop(&value);
        CHECK(!log.count && !log.relay);
        CHECK(xr_xir_instance_start(instance,ids[2],&outer,1)==XR_XIR_CALL_READY);pending=wait_timer(instance,7);
        wait_resume(instance,pending);value=wait_returned(instance);wait_string(&value,"two");xr_xir_value_drop(&value);
        CHECK(!log.count && !log.relay);
        CHECK(xr_xir_instance_start(instance,ids[1],&outer,1)==XR_XIR_CALL_READY);value=wait_returned(instance);
        CHECK(value.type==XR_XIR_BOOL && !value.payload && !log.count && log.relay==1);xr_xir_value_drop(&value);
        CHECK(xr_xir_instance_start(instance,ids[1],&inner,1)==XR_XIR_CALL_READY);value=wait_returned(instance);
        CHECK(value.type==XR_XIR_BOOL && !value.payload);xr_xir_value_drop(&value);
    }
    CHECK(log.count==1 && log.relay==1 && !producer->call && !relay->call);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && log.count==1 && log.relay==1);
    xr_xir_value_drop(&inner);xr_xir_value_drop(&outer);
}
static void wait_detached_task_case(XrXirProgram *program,const uint32_t ids[8]) {
    GeneratorWaitLog log={0};XrXirInstance *instance=wait_instance(program,&log);
    CHECK(xr_xir_instance_start(instance,ids[5],NULL,0)==XR_XIR_CALL_READY);XrXirInstanceResult first=wait_timer(instance,11);
    CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
    CHECK(xr_xir_instance_resume(instance,first.epoch,first.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    XrXirInstanceResult selected=wait_timer(instance,11);
    CHECK(selected.epoch==first.epoch && selected.outcome.wake>first.outcome.wake);
    CHECK(xr_xir_instance_resume(instance,first.epoch,first.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    wait_resume(instance,selected);CHECK(xr_xir_instance_poll_bounded(instance,1000000).outcome.status==XR_XIR_CALL_CANCELLED);
    CHECK(!log.count && !log.relay);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
}
static void wait_reentry_case(XrXirProgram *program,const uint32_t ids[8],bool cycle) {
    GeneratorWaitLog log={0};XrXirInstance *instance=wait_instance(program,&log);XrXirValue iterator=wait_make(instance,ids[cycle ? 7 : 6]);
    XirGenerator *generator=(XirGenerator *)(uintptr_t)iterator.payload;
    CHECK(xr_xir_instance_start(instance,ids[2],&iterator,1)==XR_XIR_CALL_READY);
    XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1000000);
    CHECK(result.outcome.status==XR_XIR_CALL_BAD_STATE && !result.outcome.wake && xr_xir_call_result_valid(&result.outcome));
    if(log.relay!=1 || log.cycle!=(cycle ? 1u : 0u))
        fprintf(stderr,"Reentry cycle%u cleanup outer%u inner%u\n",cycle ? 1u : 0u,log.relay,log.cycle);
    CHECK(!log.count && log.relay==1 && log.cycle==(cycle ? 1u : 0u));
    CHECK(!generator->call && !generator->consumer && generator->outcome.status==XR_XIR_CALL_BAD_STATE);
    XrXirValue value=wait_make(instance,ids[3]);CHECK(value.type==XR_XIR_I64 && value.payload==9);xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && log.relay==1 && log.cycle==(cycle ? 1u : 0u));
    CHECK(xr_xir_value_valid(&iterator));xr_xir_value_drop(&iterator);
}
static void generator_wait_cases(XrXirProgram *program,const uint32_t ids[8]) {
    for(unsigned probe=0;probe<2;++probe) wait_normal_case(program,ids,probe!=0);
    for(unsigned mode=0;mode<4;++mode) wait_cancel_case(program,ids,mode);
    for(unsigned cached=0;cached<2;++cached) wait_drop_case(program,ids,cached!=0);
    for(unsigned cancel=0;cancel<2;++cancel) wait_relay_case(program,ids,cancel!=0);
    wait_detached_task_case(program,ids);
    for(unsigned cycle=0;cycle<2;++cycle) wait_reentry_case(program,ids,cycle!=0);
    xr_xir_compile_program_drop(program);
}
#endif
