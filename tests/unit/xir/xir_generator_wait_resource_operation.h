/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generator_wait_resource_operation.h - Fallible independent domains and escaped values
 */
#ifndef XIR_GENERATOR_WAIT_RESOURCE_OPERATION_H
#define XIR_GENERATOR_WAIT_RESOURCE_OPERATION_H
typedef struct WaitResourceCost {
    XrXirCallStatus failure,primary[2],disposed[2];
    XrXirDomainBudgetStats budget[2];
    XrXirDomainStats values[2];
    uint32_t polls[2];
    size_t sites,request_before,request_after;
    bool requested,rejected,hit;
} WaitResourceCost;
typedef struct WaitResourceRequest {
    unsigned mode,cancel_pass;
    size_t fault;
    const XrXirInstanceConfig *limits;
    uint32_t cancel_prefix;
    bool retire;
} WaitResourceRequest;
typedef struct WaitResourceRun {
    XrXirInstance *instance;
    const uint32_t *ids;
    uint32_t factory,nth;
    XrXirDomain *domain;
    XrXirValue inner,outer,held;
    WaitResourceCost *cost;
    unsigned pass,mode,cleanup[2];
    uint32_t cancel_prefix;
} WaitResourceRun;
static XrXirOutputStatus resource_output(void *owner,const XrXirOutputGroup *group) {
    WaitResourceRun *run=owner;CHECK(group && group->line && group->stream==XR_XIR_STDOUT && group->count==1);
    CHECK(group->values[0].type==XR_XIR_I64);
    unsigned index=group->values[0].payload==401 ? 0u : 1u;
    CHECK(!index || group->values[0].payload==402);CHECK(!run->cleanup[index]++);return XR_XIR_OUTPUT_OK;
}
static void resource_string(const XrXirValue *value,const char *expected) {
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(value,&bytes,&length));
    CHECK(length==strlen(expected) && !memcmp(bytes,expected,length));
}
static bool resource_failure(WaitResourceRun *run,XrXirCallStatus status) {
    CHECK(status==XR_XIR_CALL_OOM || status==XR_XIR_CALL_LIMIT || status==XR_XIR_CALL_CANCELLED);
    run->cost->primary[run->pass]=status;
    if(run->cost->failure==XR_XIR_CALL_READY) run->cost->failure=status;
    return false;
}
static bool resource_root_arguments(WaitResourceRun *run,uint32_t entry,const XrXirValue *arguments,uint32_t count,XrXirValue *value) {
    CHECK(!value->type);
    source_native_phase=1;resource_observed_instance=run->instance;
    XrXirCallStatus status=xr_xir_instance_start(run->instance,entry,arguments,count);
    if(status!=XR_XIR_CALL_READY) return resource_failure(run,status);
    XrXirInstanceResult pending={0};
    for(;;) {
        uint32_t *polls=&run->cost->polls[run->pass];CHECK(*polls<32768);
        source_native_poll=*polls;
        if(*polls==run->cancel_prefix) {
            size_t before=runtime_attempts;
            XrXirCallStatus state=xr_xir_call_state(run->instance->call);
            bool active=state==XR_XIR_CALL_READY || state==XR_XIR_CALL_SUSPENDED;
            XrXirCallStatus cancelled=xr_xir_instance_cancel_current(run->instance);
            CHECK(cancelled==(active ? XR_XIR_CALL_CANCEL_REQUESTED : XR_XIR_CALL_BAD_STATE));
            CHECK(runtime_attempts==before);run->cost->requested=active;run->cost->rejected=!active;
            run->cost->request_before=before;run->cost->request_after=runtime_attempts;
        }
        ++*polls;
        if(pending.outcome.status==XR_XIR_CALL_SUSPENDED) {
            source_native_phase=3;
            if(run->cost->requested) {
                CHECK(xr_xir_instance_resume(run->instance,pending.epoch,pending.outcome.wake)==XR_XIR_CALL_BAD_STATE);
            } else {
                XrXirWaitRequest wait={0};
                CHECK(xr_xir_instance_wait_request(run->instance,pending.epoch,pending.outcome.wake,&wait)==XR_XIR_CALL_READY);
                CHECK(wait.kind==XR_XIR_WAIT_TIMER_MS && (wait.after_ms==5 || wait.after_ms==7 ||
                    (run->mode==16 && wait.after_ms==3)));
                CHECK(xr_xir_instance_resume(run->instance,pending.epoch,pending.outcome.wake)==XR_XIR_CALL_READY);
            }
            pending=(XrXirInstanceResult){0};continue;
        }
        source_native_phase=2;pending=xr_xir_instance_poll_bounded(run->instance,1);
        if(pending.outcome.status==XR_XIR_CALL_READY || pending.outcome.status==XR_XIR_CALL_SUSPENDED) continue;
        if(pending.outcome.status!=XR_XIR_CALL_RETURNED) return resource_failure(run,pending.outcome.status);
        run->cost->primary[run->pass]=XR_XIR_CALL_RETURNED;
        CHECK(xr_xir_instance_take_result(run->instance,value)==XR_XIR_CALL_RETURNED);return true;
    }
}
static bool resource_root(WaitResourceRun *run,uint32_t entry,const XrXirValue *argument,XrXirValue *value) {
    return resource_root_arguments(run,entry,argument,argument ? 1u : 0u,value);
}
static bool resource_nth_run(WaitResourceRun *run) {
    const uint32_t *ids=run->ids;
    if(run->mode==4) {
        XrXirValue more={0};if(!resource_root(run,ids[1],&run->inner,&more)) return false;
        CHECK(more.type==XR_XIR_BOOL && more.payload);xr_xir_value_drop(&more);
    }
    XrXirValue arguments[]={run->inner,{XR_XIR_I64,0,run->mode==5 ? 0 : 1}};
    if(!resource_root_arguments(run,run->nth,arguments,2,&run->held)) return false;
    resource_string(&run->held,run->mode==5 ? "one" : "two");
    if(run->mode!=5) {
        XrXirValue more={0};if(!resource_root(run,ids[1],&run->inner,&more)) return false;
        CHECK(more.type==XR_XIR_BOOL && !more.payload && run->cleanup[0]==1);xr_xir_value_drop(&more);
    }
    return true;
}
static bool resource_loop_task_run(WaitResourceRun *run) {
    XrXirValue result={0};
    if(!resource_root(run,run->ids[run->mode==9 ? 21 : run->mode==10 ? 22 : 23],NULL,&result)) return false;
    CHECK(result.type==XR_XIR_I64 && result.payload==(run->mode==9 ? 1 : run->mode==10 ? 17 : 11) && run->cleanup[0]==(run->mode==11 ? 0u : 1u));
    xr_xir_value_drop(&result);return true;
}
static bool resource_loop_run(WaitResourceRun *run) {
    uint32_t entry=run->ids[run->mode==6 ? 8 : run->mode==7 ? 9 : 10];
    XrXirValue result={0};
    if(!resource_root(run,entry,&run->inner,&result)) return false;
    CHECK(run->cleanup[0]==1);
    if(run->mode==8) {resource_string(&result,"one");run->held=result;}
    else {
        CHECK(result.type==XR_XIR_I64 && result.payload==(run->mode==7 ? 11001 : 1));
        xr_xir_value_drop(&result);
    }
    return true;
}
static bool resource_iterable_run(WaitResourceRun *run) {
    static const int64_t results[]={13,6,17,19,21,17,9,7,71},stats[]={111,111,1,11,11,0,1,11,11};
    unsigned mode=run->mode-12;CHECK(mode<9);
    XrXirValue value={0};uint32_t entry=run->ids[mode<2 ? mode : mode+1];
    if(!resource_root(run,entry,NULL,&value)) return false;
    CHECK(value.type==XR_XIR_I64 && value.payload==results[mode]);xr_xir_value_drop(&value);
    if(!resource_root(run,run->ids[2],NULL,&value)) return false;
    CHECK(value.type==XR_XIR_I64 && value.payload==stats[mode] &&
        run->cleanup[0]==(mode==5 ? 0u : 1u));xr_xir_value_drop(&value);return true;
}
static bool resource_method_factory_run(WaitResourceRun *run) {
    static const int64_t results[]={12,14,16,9,10,10,11,12};
    unsigned mode=run->mode-21;CHECK(mode<8);
    XrXirValue value={0};
    if(!resource_root(run,run->ids[mode],NULL,&value)) return false;
    CHECK(value.type==XR_XIR_I64 && value.payload==results[mode]);xr_xir_value_drop(&value);
    if(!resource_root(run,run->ids[9],NULL,&value)) return false;
    CHECK(value.type==XR_XIR_I64 && value.payload==(mode<6 ? 1 : 0) &&
        run->cleanup[0]==(mode==7 ? 0u : 1u));xr_xir_value_drop(&value);return true;
}
static bool resource_callable_run(WaitResourceRun *run) {
    unsigned mode=run->mode-29;CHECK(mode<4);XrXirValue value={0};
    if(!resource_root(run,run->ids[mode],NULL,&value)) return false;
    CHECK(value.type==XR_XIR_I64 && value.payload==7);xr_xir_value_drop(&value);
    if(!resource_root(run,run->ids[4],NULL,&value)) return false;
    CHECK(value.type==XR_XIR_I64 && value.payload==1 && run->cleanup[0]==1);xr_xir_value_drop(&value);return true;
}
static bool resource_receiver_run(WaitResourceRun *run) {
    static const int64_t expected[]={11,12,13,14,15,16,0};
    unsigned mode=run->mode-33;CHECK(mode<7);XrXirValue value={0};
    if(!resource_root(run,run->ids[mode],NULL,&value)) return false;
    CHECK(value.type==XR_XIR_I64 && value.payload==expected[mode] && run->cleanup[0]==(mode==6?0u:1u));
    xr_xir_value_drop(&value);return true;
}
static bool resource_instance_run(WaitResourceRun *run) {
    const uint32_t *ids=run->ids;
    if(run->mode>=33) return resource_receiver_run(run);
    if(run->mode>=29) return resource_callable_run(run);
    if(run->mode>=21) return resource_method_factory_run(run);
    if(run->mode>=12) return resource_iterable_run(run);
    if(run->mode>=9) return resource_loop_task_run(run);
    if(!resource_root(run,run->factory,NULL,&run->inner)) return false;
    if(run->mode>=6) return resource_loop_run(run);
    if(run->mode>=3) return resource_nth_run(run);
    if(run->mode==1 && !resource_root(run,ids[4],&run->inner,&run->outer)) return false;
    const XrXirValue *iterator=run->mode==1 ? &run->outer : &run->inner;
    for(unsigned n=0;n<(run->mode==2 ? 1u : 2u);++n) {
        for(unsigned probe=0;probe<2;++probe) {
            XrXirValue more={0};if(!resource_root(run,ids[1],iterator,&more)) return false;
            CHECK(more.type==XR_XIR_BOOL && more.payload==1);xr_xir_value_drop(&more);
        }
        if(run->mode==2) break;
        XrXirValue value={0};if(!resource_root(run,ids[2],iterator,&value)) return false;
        resource_string(&value,n ? "two" : "one");
        if(!n) run->held=value;else xr_xir_value_drop(&value);
    }
    if(run->mode!=2) {
        XrXirValue more={0};if(!resource_root(run,ids[1],iterator,&more)) return false;
        CHECK(more.type==XR_XIR_BOOL && !more.payload);xr_xir_value_drop(&more);
        if(run->mode==1) {
            if(!resource_root(run,ids[1],&run->inner,&more)) return false;
            CHECK(more.type==XR_XIR_BOOL && !more.payload);xr_xir_value_drop(&more);
        }
        CHECK(run->cleanup[0]==1 && run->cleanup[1]==(run->mode==1 ? 1u : 0u));
    }
    return true;
}
static void resource_dispose(WaitResourceRun *run,size_t fault,bool limited) {
    source_native_phase=4;source_native_instance=run->pass;resource_observed_instance=run->instance;
    if(run->instance) {
        XrXirCallStatus before=task_core_completion_status(run->instance->executor);
        size_t attempts=runtime_attempts;
        XrXirCallStatus disposed=xr_xir_instance_free(run->instance);run->instance=NULL;resource_observed_instance=NULL;
        bool faulted=attempts<=fault && runtime_attempts>fault;
        XrXirCallStatus expected=before;
        if(expected==XR_XIR_CALL_READY && faulted) expected=XR_XIR_CALL_OOM;
        if(expected==XR_XIR_CALL_READY && limited && disposed==XR_XIR_CALL_LIMIT) expected=XR_XIR_CALL_LIMIT;
        if(disposed!=expected) fprintf(stderr,"Dispose pass%u before%u actual%u expected%u faulted%u\n",run->pass,before,disposed,expected,faulted ? 1u : 0u);
        CHECK(disposed==expected);run->cost->disposed[run->pass]=disposed;
        /* Cancellation does not replace an allocation failure discovered during drain. */
        if(disposed!=XR_XIR_CALL_READY && (run->cost->failure==XR_XIR_CALL_READY || run->cost->failure==XR_XIR_CALL_CANCELLED))
            run->cost->failure=disposed;
    }
    if(run->inner.type) {CHECK(xr_xir_value_valid(&run->inner));xr_xir_value_drop(&run->inner);}
    if(run->outer.type) {CHECK(xr_xir_value_valid(&run->outer));xr_xir_value_drop(&run->outer);}
}
static WaitResourceCost resource_operation(WaitResourceFixture *fixture,const WaitResourceRequest *request) {
    unsigned mode=request->mode,cancel_pass=request->cancel_pass;size_t fault=request->fault;
    const XrXirInstanceConfig *limits=request->limits;uint32_t cancel_prefix=request->cancel_prefix;
    bool retire=request->retire;
    CHECK(mode<40 && !runtime_live && !runtime_bytes && runtime_fail_at==SIZE_MAX);
    WaitResourceCost cost={0};WaitResourceRun runs[2]={0};runtime_attempts=0;runtime_fail_at=fault;
    source_native_event_count=0;source_native_recording=true;resource_observed_instance=NULL;
    for(unsigned i=0;i<2;++i) {
        source_native_phase=0;source_native_instance=i;source_native_poll=UINT32_MAX;
        runs[i]=(WaitResourceRun){.ids=fixture->ids,.factory=fixture->factory,.nth=fixture->nth,.cost=&cost,.pass=i,.mode=mode,.cancel_prefix=i==cancel_pass ? cancel_prefix : UINT32_MAX};
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        if(limits) config=*limits;
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,resource_output,&runs[i]};
        XrXirCallStatus status=xr_xir_instance_new(fixture->program,&config,&runs[i].instance);
        if(status!=XR_XIR_CALL_READY) {resource_failure(&runs[i],status);goto done;}
        runs[i].domain=runs[i].instance->domain;CHECK(xr_xir_domain_retain(runs[i].domain));
    }
    CHECK(runs[0].domain!=runs[1].domain);
    if(retire) {xr_xir_compile_program_drop(fixture->program);fixture->program=NULL;}
    for(unsigned i=0;i<2;++i) {
        source_native_instance=i;if(!resource_instance_run(&runs[i])) goto done;
    }
 done:
    for(unsigned i=0;i<2;++i) resource_dispose(&runs[i],fault,limits!=NULL);
    if(retire) CHECK(fixture->code_releases==1);
    for(unsigned i=0;i<2;++i) {
        source_native_phase=5;source_native_instance=i;source_native_poll=UINT32_MAX;
        if(runs[i].domain) cost.values[i]=xr_xir_domain_stats(runs[i].domain);
        if(runs[i].held.type) {resource_string(&runs[i].held,mode==3 || mode==4 ? "two" : "one");xr_xir_value_drop(&runs[i].held);}
        if(runs[i].domain) {
            XrXirDomainBudgetStats stats=xr_xir_domain_budget_stats(runs[i].domain);
            CHECK(!stats.call_live && !stats.metadata_live && stats.call_allocations==stats.call_frees &&
                stats.metadata_allocations==stats.metadata_frees && xr_xir_domain_stats(runs[i].domain).live_bytes==sizeof(*runs[i].domain));
            cost.budget[i]=stats;xr_xir_domain_drop(runs[i].domain);
        }
    }
    cost.sites=runtime_attempts;cost.hit=fault!=SIZE_MAX && runtime_attempts>fault;runtime_fail_at=SIZE_MAX;
    source_native_recording=false;resource_observed_instance=NULL;
    CHECK(source_native_event_count==cost.sites && !runtime_live && !runtime_bytes);return cost;
}
#endif // XIR_GENERATOR_WAIT_RESOURCE_OPERATION_H
