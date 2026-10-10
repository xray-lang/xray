/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_native_inbox_boundaries.inc.c - Actual generated entry ownership gates
 */
XR_DATA const XrXirProgramSpec fixture_inbox0_program,fixture_inbox1_program,
    fixture_inbox2_program,fixture_inbox3_program,fixture_inbox4_program,fixture_inbox5_program;
typedef struct NativeInboxWitness {
    XrXirResumeEntry resume;
    XrXirDomain *domain;
    XrXirValue child_value,foreign;
    XrXirDomainStats input_baseline;
    uint32_t mode,attack,child_steps,child_releases,inboxes,outputs,panic_leaves,panic_landings;
} NativeInboxWitness;
static const unsigned char native_inbox_input[]={ 'a',0,0xC2,0xA2 };
static const char native_inbox_result[]={ 'r',0,'z' };
static XrXirAction native_inbox_child(XrXirCallView *view) {
    NativeInboxWitness *w=(NativeInboxWitness *)view->environment;CHECK(view->instance && w);
    if (!w->child_steps++) return (XrXirAction){XR_XIR_ACTION_SUSPEND,0,NULL,0,{0},{0},0};
    CHECK(w->child_steps==2);
    if (w->mode==2) return (XrXirAction){XR_XIR_ACTION_RETURN,0,NULL,0,{0},{0},0};
    XrXirValueStatus status=xr_xir_string_new(w->domain,native_inbox_result,sizeof(native_inbox_result),&w->child_value);
    if (status!=XR_XIR_VALUE_OK) {
        CHECK(status==XR_XIR_VALUE_OOM);
        return (XrXirAction){XR_XIR_ACTION_FAULT,0,NULL,0,{XR_XIR_I64,0,XR_XIR_CALL_OOM},{0},0};
    }
    return (XrXirAction){XR_XIR_ACTION_RETURN,0,NULL,0,w->child_value,{0},0};
}
static void native_inbox_child_release(XrXirCallView *view,XrXirCallStatus reason) {
    (void)reason; NativeInboxWitness *w=(NativeInboxWitness *)view->environment;CHECK(view->instance && w);
    xr_xir_value_drop(&w->child_value); ++w->child_releases;
}
static XrXirAction native_inbox_observe(XrXirCallView *view) {
    NativeInboxWitness *w=(NativeInboxWitness *)view->environment;CHECK(view->instance && w);
    bool incoming=view->phase==XR_XIR_CALL_NORMAL && view->inbox.status==XR_XIR_CALL_RETURNED &&
        !w->inboxes && w->mode<3;
    bool same_frontier_panic=view->phase==XR_XIR_CALL_NORMAL && w->mode==5 && !w->panic_landings &&
        view->inbox.status==XR_XIR_CALL_DIVIDE_BY_ZERO;
    XrXirCallResult original=view->inbox;
    XrXirDomainStats before={0}; XirObject *carrier=NULL; uint32_t references=0;
    size_t attempts=runtime_attempts;
    if (incoming) {
        ++w->inboxes; CHECK(w->child_steps==2 && w->child_releases==1);
        before=xr_xir_domain_stats(w->domain);
        if (w->mode<2) {
            const char *bytes=NULL;size_t length=0;
            CHECK(xr_xir_string_view(&view->inbox.value,&bytes,&length) &&
                length==sizeof(native_inbox_result) && !memcmp(bytes,native_inbox_result,length));
            carrier=object_pointer(&view->inbox.value);
            references=atomic_load_explicit(&carrier->references,memory_order_relaxed);
            CHECK(references==1 && before.live_bytes>w->input_baseline.live_bytes);
            if (w->attack==1) {
                CHECK(w->mode==1);
                atomic_store_explicit(&carrier->references,UINT32_MAX,memory_order_relaxed);
            }
        } else {
            CHECK(xr_xir_call_result_valid(&view->inbox) && !view->inbox.value.type);
            if (w->attack==1) view->inbox.value.payload=1;
            if (w->attack==2) view->inbox.value.reserved=1;
            if (w->attack==3) view->inbox.value=(XrXirValue){XR_XIR_I64,0,0};
            if (w->attack==4) view->inbox.status=XR_XIR_CALL_READY;
            if (w->attack==5) view->inbox.value=(XrXirValue){XR_XIR_UNIT,0,view->arguments[0].payload};
            if (w->attack==6) view->inbox.value=(XrXirValue){XR_XIR_UNIT,0,w->foreign.payload};
            if (w->attack==7) view->inbox.value=view->arguments[0];
            if (w->attack==8) view->inbox.value=w->foreign;
            if (w->attack==9) {
                view->inbox.status=XR_XIR_CALL_DIVIDE_BY_ZERO;
                view->inbox.panic.detail.code=XR_XIR_PANIC_DIVIDE;
                CHECK(xr_xir_call_result_valid(&view->inbox));
            }
            if (w->attack==10) view->inbox.status=XR_XIR_CALL_CANCELLED;
        }
    }
    XrXirAction action=w->resume(view);
    if (same_frontier_panic) {
        CHECK(!w->panic_landings++ && !w->panic_leaves && !w->outputs);
        CHECK(action.kind==XR_XIR_ACTION_CONTINUE && !action.callee && !action.arguments &&
            !action.argument_count && !action.flags && !action.value.type &&
            !action.value.reserved && !action.value.payload && xr_xir_panic_empty(&action.panic));
        CHECK(runtime_attempts==attempts);
    }
    if (incoming) {
        CHECK(runtime_attempts==attempts);
        if (w->mode==1 && w->attack==1) {
            CHECK(action.kind==XR_XIR_ACTION_FAULT && !action.callee && !action.arguments &&
                !action.argument_count && !action.flags && xr_xir_panic_empty(&action.panic) && action.value.type==XR_XIR_I64 &&
                !action.value.reserved && action.value.payload==XR_XIR_CALL_LIMIT);
            CHECK(atomic_load_explicit(&carrier->references,memory_order_relaxed)==UINT32_MAX);
            atomic_store_explicit(&carrier->references,references,memory_order_relaxed);
        } else if (w->mode==2 && w->attack) {
            CHECK(action.kind==XR_XIR_ACTION_FAULT && !action.callee && !action.arguments &&
                !action.argument_count && !action.flags && xr_xir_panic_empty(&action.panic) && !action.value.type &&
                !action.value.reserved && !action.value.payload);
            view->inbox=original;
        } else {
            CHECK(action.kind==XR_XIR_ACTION_CONTINUE);
            if (!w->mode) {
                XrXirDomainStats after=xr_xir_domain_stats(w->domain);
                CHECK(xr_xir_call_result_empty(&view->inbox));
                CHECK(after.live_bytes==w->input_baseline.live_bytes &&
                    after.allocations==before.allocations && after.frees==before.frees+2);
                CHECK(xr_xir_call_discard_inbox(view,XR_XIR_STRING)==XR_XIR_CALL_BAD_STATE);
            } else if (w->mode==1) {
                CHECK(atomic_load_explicit(&carrier->references,memory_order_relaxed)==references+1);
                CHECK(xr_xir_domain_stats(w->domain).live_bytes==before.live_bytes);
            }
        }
    }
    if (action.kind==XR_XIR_ACTION_LEAVE && action.flags==XR_XIR_ACTION_LEAVE_PANIC) {
        CHECK(w->mode==4 && !w->outputs && action.value.type==XR_XIR_I64 &&
            action.value.payload==XR_XIR_CALL_DIVIDE_BY_ZERO && action.panic.detail.code==XR_XIR_PANIC_DIVIDE);
        ++w->panic_leaves;
    }
    return action;
}
static XrXirOutputStatus native_inbox_output(void *context,const XrXirOutputGroup *group) {
    NativeInboxWitness *w=context;const char *bytes=NULL;size_t length=0;
    CHECK(w->mode==4 && group->stream==XR_XIR_STDOUT && !group->line && group->count==1);
    CHECK(xr_xir_string_view(group->values,&bytes,&length) && length==sizeof(native_inbox_input) &&
        !memcmp(bytes,native_inbox_input,length));
    CHECK(!w->outputs++);
    return XR_XIR_OUTPUT_OK;
}
static bool native_inbox_run(const XrXirCompileContext *context,
    const XrXirProgramSpec *original,uint32_t mode,uint32_t attack,bool cancel) {
    XrCompileResourceStats compiler_baseline={0};
    CHECK(xr_compile_resources_stats(context->resources,&compiler_baseline)==XR_COMPILE_RESOURCE_OK);
    NativeInboxWitness witness={0};witness.mode=mode;witness.attack=attack;
    XrXirCallEntry entries[5];memcpy(entries,original->entries,sizeof(entries));
    witness.resume=entries[0].resume;entries[0].resume=native_inbox_observe;entries[0].environment=&witness;
    if (mode<3) {entries[1].resume=native_inbox_child;entries[1].release=native_inbox_child_release;entries[1].environment=&witness;}
    XrXirDomain *domain=NULL,*foreign_domain=NULL;XrXirProgram *program=NULL;XrXirInstance *instance=NULL;
    XrXirValue input={0};bool accepted=false;
    XrXirDomainStats domain_baseline={0},foreign_baseline={0};
    bool completed=false; XrXirCallStatus status=XR_XIR_CALL_OOM;
    XrXirValueStatus vs=xr_xir_domain_new(65536,&domain);
    if (vs!=XR_XIR_VALUE_OK) {CHECK(vs==XR_XIR_VALUE_OOM);goto done;}
    domain_baseline=xr_xir_domain_stats(domain);
    witness.domain=domain;
    vs=xr_xir_string_new(domain,(const char *)native_inbox_input,sizeof(native_inbox_input),&input);
    if (vs!=XR_XIR_VALUE_OK) {CHECK(vs==XR_XIR_VALUE_OOM);goto done;}
    witness.input_baseline=xr_xir_domain_stats(domain);
    if (mode==2 && attack>=6 && attack<=8) {
        CHECK(xr_xir_domain_new(65536,&foreign_domain)==XR_XIR_VALUE_OK);
        foreign_baseline=xr_xir_domain_stats(foreign_domain);
        CHECK(xr_xir_string_new(foreign_domain,"foreign",7,&witness.foreign)==XR_XIR_VALUE_OK);
    }
    XrXirProgramSpec spec=*original;spec.entries=entries;
    if (!native_program_new(context,&spec,&program)) goto done;
    XrXirOutputProvider output={XR_XIR_CALL_ABI_VERSION,0,native_inbox_output,&witness};
    status=native_instance_new(program,&output,&instance);
    if (status!=XR_XIR_CALL_READY) {CHECK(status==XR_XIR_CALL_OOM);goto done;}
    xr_xir_compile_program_drop(program);program=NULL;
    status=xr_xir_instance_start(instance,0,&input,1);
    if (status!=XR_XIR_CALL_READY) {CHECK(status==XR_XIR_CALL_OOM);goto done;}
    accepted=true;
    XrXirInstanceResult polled=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
    if (polled.outcome.status==XR_XIR_CALL_SUSPENDED) {
        CHECK(!witness.inboxes && !witness.outputs);
        if (cancel) CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
        else CHECK(xr_xir_instance_resume(instance,polled.epoch,polled.outcome.wake)==XR_XIR_CALL_READY);
        polled=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
    }
    XrXirCallResult result=polled.outcome;
    status=result.status;
    if (status==XR_XIR_CALL_OOM) goto done;
    if (cancel) {
        CHECK(status==XR_XIR_CALL_CANCELLED && !witness.inboxes && !witness.panic_leaves && !witness.panic_landings);
        CHECK(witness.outputs==(uint32_t)(mode==4));
    } else if (mode==1 && attack) CHECK(status==XR_XIR_CALL_LIMIT && witness.inboxes==1);
    else if (mode==2 && attack) CHECK(status==XR_XIR_CALL_BAD_STATE && witness.inboxes==1);
    else if (mode==3) CHECK(status==XR_XIR_CALL_DIVIDE_BY_ZERO && !witness.outputs);
    else {
        CHECK(status==XR_XIR_CALL_RETURNED && result.value.type==XR_XIR_I64 &&
            result.value.payload==(mode>=4 ? 91 : 41));
        CHECK(witness.inboxes==(uint32_t)(mode<3));
        CHECK(witness.outputs==(uint32_t)(mode==4) && witness.panic_leaves==(uint32_t)(mode==4));
        CHECK(witness.panic_landings==(uint32_t)(mode==5));
    }
    completed=true;
 done:
    native_instance_retire(instance,accepted,status);xr_xir_compile_program_drop(program);
    xr_xir_value_drop(&witness.child_value);xr_xir_value_drop(&input);xr_xir_value_drop(&witness.foreign);
    if (domain) {
        XrXirDomainStats ds=xr_xir_domain_stats(domain);
        bool restored=ds.live_bytes==domain_baseline.live_bytes &&
            ds.allocations>=domain_baseline.allocations && ds.frees>=domain_baseline.frees &&
            ds.allocations-domain_baseline.allocations==ds.frees-domain_baseline.frees;
        if (!restored)
            fprintf(stderr,"Inbox release mode=%u cancel=%u attack=%u fail_at=%zu attempts=%zu live=%llu alloc=%llu free=%llu\n",
                mode,(unsigned)cancel,attack,runtime_fail_at,runtime_attempts,
                (unsigned long long)ds.live_bytes,(unsigned long long)ds.allocations,(unsigned long long)ds.frees);
        CHECK(restored);
    }
    if (foreign_domain) {
        XrXirDomainStats ds=xr_xir_domain_stats(foreign_domain);
        CHECK(ds.live_bytes==foreign_baseline.live_bytes &&
            ds.allocations>=foreign_baseline.allocations && ds.frees>=foreign_baseline.frees &&
            ds.allocations-foreign_baseline.allocations==ds.frees-foreign_baseline.frees);
    }
    xr_xir_domain_drop(domain);xr_xir_domain_drop(foreign_domain);
    CHECK(!runtime_live && !runtime_bytes);
    XrCompileResourceStats released={0};
    CHECK(xr_compile_resources_stats(context->resources,&released)==XR_COMPILE_RESOURCE_OK &&
        released.live_bytes==compiler_baseline.live_bytes);
    return completed;
}
static void native_inbox_boundaries(const XrXirCompileContext *context) {
    XrCompileResourceStats compiler_baseline={0};
    CHECK(xr_compile_resources_stats(context->resources,&compiler_baseline)==XR_COMPILE_RESOURCE_OK);
    const XrXirProgramSpec *tables[]={&fixture_inbox0_program,&fixture_inbox1_program,
        &fixture_inbox2_program,&fixture_inbox3_program,&fixture_inbox4_program,&fixture_inbox5_program};
    for (uint32_t mode=0;mode<6;++mode) for (uint32_t cancel=0;cancel<2;++cancel) {
        runtime_fail_at=SIZE_MAX;runtime_attempts=0;
        CHECK(native_inbox_run(context,tables[mode],mode,0,cancel!=0));
        size_t sites=runtime_attempts;CHECK(sites);
        for (size_t i=0;i<sites;++i) {
            runtime_fail_at=i;runtime_attempts=0;
            CHECK(!native_inbox_run(context,tables[mode],mode,0,cancel!=0));
            CHECK(runtime_attempts>i && !runtime_live && !runtime_bytes);
        }
        runtime_fail_at=SIZE_MAX;
        fprintf(stderr,"generated Inbox mode=%u cancel=%u real OOM sites=%zu physical=0/0\n",mode,cancel,sites);
    }
    runtime_fail_at=SIZE_MAX;runtime_attempts=0;
    CHECK(native_inbox_run(context,tables[1],1,1,false));
    for (uint32_t attack=1;attack<=8;++attack) CHECK(native_inbox_run(context,tables[2],2,attack,false));
    /* Borrowed-view faults: no handler for a valid panic, then invalid status. */
    CHECK(native_inbox_run(context,tables[2],2,9,false));
    CHECK(native_inbox_run(context,tables[2],2,10,false));
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK &&
        stats.live_bytes==compiler_baseline.live_bytes);
    puts("Generated native Inbox: discard refund, Unit admission, retain limit, panic cleanup, same-frontier landing, missing handler and actual OOM");
}
