/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generator_loop_cases.h - Lexical close drains before loop continuation
 */
#ifndef XIR_GENERATOR_LOOP_CASES_H
#define XIR_GENERATOR_LOOP_CASES_H
static void loop_query_closed(XrXirInstance *instance,const uint32_t ids[23],int64_t expected) {
    XrXirValue count=wait_make(instance,ids[12]);CHECK(count.type==XR_XIR_I64 && count.payload==expected);xr_xir_value_drop(&count);
}
static void loop_value_case(XrXirProgram *program,const uint32_t ids[23],unsigned mode) {
    GeneratorWaitLog log={0};XrXirInstance *instance=wait_instance(program,&log);
    uint32_t entries[]={ids[8],ids[9],ids[10],ids[11],ids[14],ids[16],ids[18],ids[19]};
    XrXirValue iterator=wait_make(instance,ids[mode==4 ? 13 : 15]);
    CHECK(xr_xir_instance_start(instance,entries[mode],&iterator,1)==XR_XIR_CALL_READY);
    XrXirInstanceResult pending=wait_timer(instance,5);wait_resume(instance,pending);
    if(mode==1 || mode==4 || mode==6 || mode==7) {pending=wait_timer(instance,7);wait_resume(instance,pending);}
    XrXirValue value={0};
    if(mode==5) {
        XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1000000);
        CHECK(result.outcome.status==XR_XIR_CALL_BOUNDS && result.outcome.panic.detail.code==XR_XIR_PANIC_BOUNDS);
    } else {
        value=wait_returned(instance);
        if(mode==2 || mode==6) wait_string(&value,mode==2 ? "one" : "onetwo");
        else CHECK(value.type==XR_XIR_I64 && value.payload==(mode==1 ? 11001 : mode==4 || mode==7 ? 21001 : 1));
    }
    CHECK(log.count==1);
    XirGenerator *generator=(XirGenerator *)(uintptr_t)iterator.payload;
    CHECK(!generator->call && !generator->consumer);
    loop_query_closed(instance,ids,mode==1 || mode==4 || mode==6 || mode==7 ? 1001 : 1);
    CHECK(xr_xir_instance_resume(instance,pending.epoch,pending.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && log.count==1);xr_xir_value_drop(&iterator);
    if(mode==2 || mode==6) wait_string(&value,mode==2 ? "one" : "onetwo");xr_xir_value_drop(&value);
}
static void loop_cancel_case(XrXirProgram *program,const uint32_t ids[23],unsigned mode,bool second) {
    GeneratorWaitLog log={0};XrXirInstance *instance=wait_instance(program,&log);
    XrXirValue iterator=wait_make(instance,ids[15]);
    CHECK(xr_xir_instance_start(instance,ids[9],&iterator,1)==XR_XIR_CALL_READY);
    XrXirInstanceResult pending=wait_timer(instance,5);
    if(second) {wait_resume(instance,pending);pending=wait_timer(instance,7);}
    if(mode==2) CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    else {
        CHECK((mode==1 ? xr_xir_instance_stop(instance) : xr_xir_instance_cancel_current(instance))==
            (mode==1 ? XR_XIR_CALL_READY : XR_XIR_CALL_CANCEL_REQUESTED));
        CHECK(xr_xir_instance_resume(instance,pending.epoch,pending.outcome.wake)==XR_XIR_CALL_BAD_STATE);
        XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1000000);
        if(result.outcome.status!=XR_XIR_CALL_CANCELLED || log.count!=1)
            fprintf(stderr,"Loop cancellation mode%u second%u status%u cleanup%u\n",mode,second ? 1u : 0u,result.outcome.status,log.count);
        CHECK(result.outcome.status==XR_XIR_CALL_CANCELLED && log.count==1);
        if(!mode) loop_query_closed(instance,ids,1);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    XirGenerator *generator=(XirGenerator *)(uintptr_t)iterator.payload;
    CHECK(log.count==1 && !generator->call && !generator->consumer);xr_xir_value_drop(&iterator);
}
static void loop_nested_case(XrXirProgram *program,const uint32_t ids[23],bool alias) {
    GeneratorWaitLog log={0};XrXirInstance *instance=wait_instance(program,&log);
    XrXirValue values[]={wait_make(instance,ids[15]),{0}};
    if(alias) CHECK(xr_xir_value_copy(&values[0],&values[1])==XR_XIR_VALUE_OK);else values[1]=wait_make(instance,ids[15]);
    CHECK(xr_xir_instance_start(instance,ids[17],values,2)==XR_XIR_CALL_READY);
    for(unsigned i=0;i<2;++i) {XrXirInstanceResult pending=wait_timer(instance,i && alias ? 7 : 5);wait_resume(instance,pending);}
    XrXirValue result=wait_returned(instance);CHECK(result.type==XR_XIR_I64 && result.payload==1 && log.count==(alias ? 1u : 2u));
    xr_xir_value_drop(&result);loop_query_closed(instance,ids,alias ? 1 : 2);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && log.count==(alias ? 1u : 2u));
    xr_xir_value_drop(&values[0]);xr_xir_value_drop(&values[1]);
}
static void loop_close_cancel_case(XrXirProgram *program,const uint32_t ids[23]) {
    GeneratorWaitLog log={0};XrXirInstance *instance=wait_instance(program,&log);
    XrXirValue iterator=wait_make(instance,ids[15]);XirGenerator *generator=(XirGenerator *)(uintptr_t)iterator.payload;
    CHECK(xr_xir_instance_start(instance,ids[8],&iterator,1)==XR_XIR_CALL_READY);
    XrXirInstanceResult pending=wait_timer(instance,5);wait_resume(instance,pending);bool closing=false;
    for(unsigned i=0;i<10000;++i) {
        XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1);CHECK(result.outcome.status==XR_XIR_CALL_READY);
        XrXirTaskWaitToken token={0};
        if(generator->consumer && xr_xir_call_iterator_wait_token(generator->consumer,&token) && token.request.kind==XR_XIR_WAIT_ITERATOR_CLOSE) {
            CHECK(xr_xir_call_complete_iterator_wait(generator->consumer,&token)==XR_XIR_CALL_BAD_STATE);
            XrXirCall *consumer=generator->consumer;
            CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
            CHECK(generator->consumer==consumer && xr_xir_call_iterator_wait_token(consumer,&token));
            CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
            CHECK(generator->consumer==consumer);closing=true;break;
        }
    }
    XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1000000);
    if(!closing || result.outcome.status!=XR_XIR_CALL_CANCELLED || log.count!=1)
        fprintf(stderr,"Closing cancellation observed closing%u status%u cleanup%u producer%u consumer%u\n",
            closing ? 1u : 0u,result.outcome.status,log.count,generator->outcome.status,generator->consumer ? 1u : 0u);
    CHECK(closing && result.outcome.status==XR_XIR_CALL_CANCELLED && log.count==1);
    loop_query_closed(instance,ids,1);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_value_drop(&iterator);
}
static void loop_builtin_case(XrXirProgram *program,const uint32_t ids[23]) {
    GeneratorWaitLog log={0};XrXirInstance *instance=wait_instance(program,&log);
    XrXirValue value=wait_make(instance,ids[20]);CHECK(value.type==XR_XIR_I64 && value.payload==13 && !log.count);
    xr_xir_value_drop(&value);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
}
static void loop_task_case(XrXirProgram *program,const uint32_t ids[23],unsigned mode) {
    GeneratorWaitLog log={0};XrXirInstance *instance=wait_instance(program,&log);
    CHECK(xr_xir_instance_start(instance,ids[mode==4 ? 22 : 21],NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult pending=wait_timer(instance,5);
    if(mode==2) {CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && log.count==1);return;}
    if(mode==1) {
        CHECK(xr_xir_instance_stop(instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_resume(instance,pending.epoch,pending.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    } else {
        if(mode==3) {
            CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
            CHECK(xr_xir_instance_resume(instance,pending.epoch,pending.outcome.wake)==XR_XIR_CALL_BAD_STATE);
            XrXirInstanceResult selected=wait_timer(instance,5);
            CHECK(selected.outcome.wake>pending.outcome.wake);pending=selected;
        }
        wait_resume(instance,pending);
    }
    if(mode && mode<4) CHECK(xr_xir_instance_poll_bounded(instance,1000000).outcome.status==XR_XIR_CALL_CANCELLED);
    else {XrXirValue value=wait_returned(instance);CHECK(value.type==XR_XIR_I64 && value.payload==(mode==4 ? 17 : 1));xr_xir_value_drop(&value);}
    CHECK(log.count==1 && xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
}
static void generator_loop_cases(XrXirProgram *program,const uint32_t ids[23]) {
    for(unsigned mode=0;mode<8;++mode) loop_value_case(program,ids,mode);
    for(unsigned mode=0;mode<3;++mode) for(unsigned second=0;second<2;++second) loop_cancel_case(program,ids,mode,second!=0);
    loop_nested_case(program,ids,false);loop_nested_case(program,ids,true);loop_close_cancel_case(program,ids);
    loop_builtin_case(program,ids);for(unsigned mode=0;mode<5;++mode) loop_task_case(program,ids,mode);
    puts("Iterator for-in: 23 real lexical close, control, capture, Unit, Error, panic and cancellation cases PASS");
}
#endif
