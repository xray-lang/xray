/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generator_nth_cases.h - Owned skipping preserves exact waits and cleanup
 */
#ifndef XIR_GENERATOR_NTH_CASES_H
#define XIR_GENERATOR_NTH_CASES_H
static void nth_start(XrXirInstance *instance,uint32_t entry,XrXirValue iterator,int64_t index) {
    XrXirValue arguments[]={iterator,{XR_XIR_I64,0,index}};
    CHECK(xr_xir_instance_start(instance,entry,arguments,2)==XR_XIR_CALL_READY);
}
static void nth_exhausted(XrXirInstance *instance,uint32_t entry,XrXirValue iterator) {
    nth_start(instance,entry,iterator,0);
    XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1000000);
    CHECK(result.outcome.status==XR_XIR_CALL_RUNTIME_PANIC &&
        result.outcome.panic.detail.code==XR_XIR_PANIC_ITERATOR_EXHAUSTED && !result.outcome.wake);
    CHECK(xr_xir_call_result_valid(&result.outcome));
}
static void nth_value_case(XrXirProgram *program,const uint32_t ids[17],uint32_t factory,unsigned mode) {
    GeneratorWaitLog log={0};XrXirInstance *instance=wait_instance(program,&log);
    XrXirValue iterator=wait_make(instance,factory);XirGenerator *generator=(XirGenerator *)(uintptr_t)iterator.payload;
    bool cached=mode==2;int64_t index=mode==3 ? INT64_MAX : mode ? 1 : 0;
    if(cached) {
        CHECK(xr_xir_instance_start(instance,ids[1],&iterator,1)==XR_XIR_CALL_READY);
        XrXirInstanceResult pending=wait_timer(instance,5);wait_resume(instance,pending);
        XrXirValue more=wait_returned(instance);CHECK(more.type==XR_XIR_BOOL && more.payload);xr_xir_value_drop(&more);
    }
    nth_start(instance,ids[13],iterator,index);
    XrXirTaskWaitToken previous={0};XrXirCall *consumer=NULL;
    if(!cached) {
        XrXirInstanceResult pending=wait_timer(instance,5);wait_invalid_tokens(generator,false);
        consumer=generator->consumer;CHECK(xr_xir_call_iterator_wait_token(consumer,&previous));
        CHECK(previous.request.kind==XR_XIR_WAIT_ITERATOR_NTH && previous.request.after_ms==(uint64_t)index);
        uint64_t polls=generator->accounting.polls;XrXirInstanceResult same=wait_timer(instance,5);
        CHECK(same.epoch==pending.epoch && same.outcome.wake==pending.outcome.wake && generator->accounting.polls==polls);
        wait_resume(instance,pending);
    }
    if(mode) {
        XrXirInstanceResult pending=wait_timer(instance,7);wait_invalid_tokens(generator,false);
        XrXirTaskWaitToken current={0};CHECK(xr_xir_call_iterator_wait_token(generator->consumer,&current));
        CHECK(current.request.after_ms==(uint64_t)(index-1));
        if(!cached) {
            CHECK(generator->consumer==consumer && current.wake==previous.wake);
            CHECK(xr_xir_call_complete_iterator_wait(consumer,&previous)==XR_XIR_CALL_BAD_STATE);
            CHECK(xr_xir_call_fail_iterator_wait(consumer,&previous,XR_XIR_CALL_LIMIT)==XR_XIR_CALL_BAD_STATE);
        }
        wait_resume(instance,pending);
    }
    if(mode==3) {
        XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1000000);
        CHECK(result.outcome.status==XR_XIR_CALL_RUNTIME_PANIC &&
            result.outcome.panic.detail.code==XR_XIR_PANIC_ITERATOR_EXHAUSTED && log.count==1);
    } else {
        XrXirValue value=wait_returned(instance);wait_string(&value,mode ? "two" : "one");
        CHECK(!log.count && !generator->consumer);xr_xir_value_drop(&value);
        if(!mode) {
            nth_start(instance,ids[13],iterator,0);XrXirInstanceResult pending=wait_timer(instance,7);
            wait_resume(instance,pending);value=wait_returned(instance);wait_string(&value,"two");xr_xir_value_drop(&value);
        }
    }
    nth_exhausted(instance,ids[13],iterator);CHECK(log.count==1 && !generator->call && !generator->consumer);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && log.count==1);xr_xir_value_drop(&iterator);
}
static void nth_negative_case(XrXirProgram *program,const uint32_t ids[17]) {
    GeneratorWaitLog log={0};XrXirInstance *instance=wait_instance(program,&log);
    XrXirValue iterator=wait_make(instance,ids[12]);XirGenerator *generator=(XirGenerator *)(uintptr_t)iterator.payload;
    const int64_t indices[]={-1,INT64_MIN};
    for(unsigned i=0;i<2;++i) {
        nth_start(instance,ids[13],iterator,indices[i]);
        XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1000000);
        CHECK(result.outcome.status==XR_XIR_CALL_BOUNDS && result.outcome.panic.detail.code==XR_XIR_PANIC_BOUNDS &&
            result.outcome.panic.detail.index==indices[i] && !result.outcome.panic.detail.length);
        CHECK(xr_xir_call_result_valid(&result.outcome) && !generator->accounting.polls && !generator->consumer && !log.count);
    }
    nth_start(instance,ids[13],iterator,0);XrXirInstanceResult pending=wait_timer(instance,5);wait_resume(instance,pending);
    XrXirValue one=wait_returned(instance);wait_string(&one,"one");
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && log.count==1);
    wait_string(&one,"one");xr_xir_value_drop(&one);xr_xir_value_drop(&iterator);
}
static void nth_cancel_case(XrXirProgram *program,const uint32_t ids[17],unsigned mode,bool second) {
    GeneratorWaitLog log={0};XrXirInstance *instance=wait_instance(program,&log);
    XrXirValue iterator=wait_make(instance,ids[12]);XirGenerator *generator=(XirGenerator *)(uintptr_t)iterator.payload;
    nth_start(instance,ids[13],iterator,1);XrXirInstanceResult pending=wait_timer(instance,5);
    if(second) {wait_resume(instance,pending);pending=wait_timer(instance,7);}
    XrXirCall *consumer=generator->consumer;XrXirTaskWaitToken token={0};
    CHECK(xr_xir_call_iterator_wait_token(consumer,&token));
    if(mode==2) CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    else {
        CHECK((mode==1 ? xr_xir_instance_stop(instance) : xr_xir_instance_cancel_current(instance))==
            (mode==1 ? XR_XIR_CALL_READY : XR_XIR_CALL_CANCEL_REQUESTED));
        CHECK(!generator->consumer && xr_xir_instance_resume(instance,pending.epoch,pending.outcome.wake)==XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_call_complete_iterator_wait(consumer,&token)==XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_call_fail_iterator_wait(consumer,&token,XR_XIR_CALL_LIMIT)==XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_poll_bounded(instance,1000000).outcome.status==XR_XIR_CALL_CANCELLED);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    CHECK(log.count==1 && !generator->call && !generator->consumer && xr_xir_value_valid(&iterator));
    xr_xir_value_drop(&iterator);
}
static void nth_unit_case(XrXirProgram *program,const uint32_t ids[17]) {
    GeneratorWaitLog log={0};XrXirInstance *instance=wait_instance(program,&log);
    XrXirValue iterator=wait_make(instance,ids[14]);nth_start(instance,ids[15],iterator,1);
    XrXirInstanceResult pending=wait_timer(instance,5);wait_resume(instance,pending);
    pending=wait_timer(instance,7);wait_resume(instance,pending);XrXirValue unit=wait_returned(instance);
    CHECK(!unit.type && !unit.reserved && !unit.payload && !log.count);xr_xir_value_drop(&unit);
    nth_exhausted(instance,ids[15],iterator);CHECK(log.count==1);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_value_drop(&iterator);
}
static void nth_error_case(XrXirProgram *program,const uint32_t ids[17]) {
    GeneratorWaitLog log={0};XrXirInstance *instance=wait_instance(program,&log);
    XrXirValue iterator=wait_make(instance,ids[16]);nth_start(instance,ids[13],iterator,1);
    XrXirInstanceResult pending=wait_timer(instance,5);wait_resume(instance,pending);
    pending=wait_timer(instance,7);wait_resume(instance,pending);
    XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1000000);
    CHECK(result.outcome.status==XR_XIR_CALL_THROWN && xr_xir_call_result_valid(&result.outcome) && log.count==1);
    XrXirValue saved={0};CHECK(xr_xir_value_copy(&result.outcome.value,&saved)==XR_XIR_VALUE_OK);
    nth_start(instance,ids[13],iterator,0);result=xr_xir_instance_poll_bounded(instance,1000000);
    CHECK(result.outcome.status==XR_XIR_CALL_THROWN && result.outcome.value.payload==saved.payload && log.count==1);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_value_drop(&iterator);
    CHECK(xr_xir_value_valid(&saved));xr_xir_value_drop(&saved);
}
static void generator_nth_cases(XrXirProgram *program,const uint32_t ids[17],const uint32_t factories[6]) {
    for(unsigned f=0;f<6;++f) for(unsigned mode=0;mode<4;++mode) nth_value_case(program,ids,factories[f],mode);
    nth_negative_case(program,ids);
    for(unsigned mode=0;mode<3;++mode) for(unsigned second=0;second<2;++second) nth_cancel_case(program,ids,mode,second!=0);
    nth_unit_case(program,ids);nth_error_case(program,ids);
    puts("Iterator nth: 33 owned skip, cached, bounds, exhaustion, exact Timer, cancellation, Unit and sticky Error cases PASS");
}
#endif
