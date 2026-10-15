/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_closed_error_control.c - Cancellation and exact error certificate rejection
 *
 * KEY CONCEPT:
 *   The original executable entries keep their real proof and frame ownership.
 */
#define main retained_closed_error_main
#include "test_xir_task_closed_error_baseline.c"
#undef main

typedef struct ClosedErrorControl {
    ClosedErrorFixture base;
    XrXirCallEntry selected[CLOSED_ERROR_FUNCTIONS];
    XrXirInstance *instances[2];
    unsigned worker_steps[2],forged;
    bool worker[CLOSED_ERROR_FUNCTIONS];
    XrXirValue replacement[2];
    XrXirTypeArena *foreign;
    unsigned replacements;
} ClosedErrorControl;
static ClosedErrorControl *closed_error_control;
static XrXirValue closed_error_wrong_value(ClosedErrorControl *control,XrXirCallView *view,unsigned instance) {
    XrXirValueAdmission admission=*xr_xir_call_admission(view);
    if (control->forged==3) admission.arena=control->foreign;
    const XrXirTypes *types=xr_xir_compile_type_arena_types(admission.arena);
    XrXirType array_type=XR_XIR_UNIT,enum_type=XR_XIR_UNIT;
    const char *name=control->forged==2?"Choice":"Problem";
    for (uint32_t t=0;t<types->count;++t) {
        const XrXirTypeNode *node=&types->nodes[t];
        if (node->kind==XR_XIR_TYPE_ARRAY && node->element==XR_XIR_I64)
            array_type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+t);
        if (node->kind!=XR_XIR_TYPE_NOMINAL) continue;
        const XrXirNominalIdentity *identity=&types->nominals->identities[node->nominal.declaration];
        if (identity->name.length==strlen(name) && !memcmp(identity->name.bytes,name,identity->name.length))
            enum_type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+t);
    }
    CHECK(array_type!=XR_XIR_UNIT && enum_type!=XR_XIR_UNIT && !control->replacement[instance].type);
    XrXirValue fields[]={{XR_XIR_I64,0,10},{XR_XIR_I64,0,20}},array={0};
    CHECK(xr_xir_array_new(array_type,fields,2,&admission,&array)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_enum_new(enum_type,control->forged==2?1:0,&array,1,&admission,
        &control->replacement[instance])==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&array);CHECK(xr_xir_value_valid(&control->replacement[instance]));
    ++control->replacements;return control->replacement[instance];
}
static XrXirAction closed_error_control_resume(XrXirCallView *view) {
    ClosedErrorControl *control=closed_error_control;CHECK(control && xr_xir_call_admission(view));
    uint32_t entry=xr_xir_call_current_entry(view->activation);CHECK(entry<CLOSED_ERROR_FUNCTIONS);
    unsigned instance=2;
    for (unsigned i=0;i<2;++i) if (control->instances[i]==view->instance) instance=i;
    CHECK(instance<2);bool child=false;
    CHECK(xr_xir_task_executor_view_member(control->instances[instance]->executor,view,&child));
    if (control->worker[entry]) { CHECK(child);++control->worker_steps[instance]; }
    XrXirAction action=control->selected[entry].resume(view);
    if (control->forged && control->worker[entry] && action.kind==XR_XIR_ACTION_THROW)
        action.value=closed_error_wrong_value(control,view,instance);
    return action;
}
static void closed_error_control_configure(ClosedErrorFixture *fixture,const XrXirModule *module) {
    ClosedErrorControl *control=closed_error_control;CHECK(fixture==&control->base);
    for (uint32_t f=0;f<module->function_count;++f) {
        control->worker[f]=module->functions[f].name_length>=7 && !memcmp(module->functions[f].name,"worker_",7);
        control->selected[f]=fixture->entries[f];fixture->entries[f].resume=closed_error_control_resume;
    }
}
static bool closed_error_control_frontier(ClosedErrorControl *control,unsigned instance,unsigned frontier) {
    if (!frontier) return true;
    XirTask *task=control->instances[instance]->executor->active;
    return task && (frontier==1?!control->worker_steps[instance]:control->worker_steps[instance]==1);
}
static void closed_error_control_pair(ClosedErrorControl *control,unsigned family,unsigned frontier,bool stop) {
    XrXirDomain *domains[2]={0};XrXirValue tasks[2]={{0}};
    memset(control->worker_steps,0,sizeof(control->worker_steps));
    for (unsigned i=0;i<2;++i) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_new(control->base.program,&config,&control->instances[i])==XR_XIR_CALL_READY);
        domains[i]=control->instances[i]->domain;CHECK(xr_xir_domain_retain(domains[i]));
        CHECK(xr_xir_instance_start(control->instances[i],control->base.entry[family][0],NULL,0)==XR_XIR_CALL_READY);
    }
    for (unsigned i=0;i<2;++i) {
        XrXirInstance *instance=control->instances[i];unsigned polls=0;
        while (!closed_error_control_frontier(control,i,frontier))
            CHECK(xr_xir_instance_poll_bounded(instance,1).outcome.status==XR_XIR_CALL_READY && ++polls<4096);
        if (frontier) {
            CHECK(instance->executor->active && !instance->executor->active->active_next);
            XrXirValue handle=task_core_task_value(instance->executor->active);
            CHECK(xr_xir_value_copy(&handle,&tasks[i])==XR_XIR_VALUE_OK);
            XrXirCallResult pending={0};size_t attempts=runtime_attempts;
            CHECK(xr_xir_task_copy_outcome(&tasks[i],&pending)==XR_XIR_CALL_BAD_STATE &&
                xr_xir_call_result_empty(&pending) && attempts==runtime_attempts);
        }
        CHECK((stop?xr_xir_instance_stop(instance):xr_xir_instance_cancel_current(instance))==
            (stop?XR_XIR_CALL_READY:XR_XIR_CALL_CANCEL_REQUESTED));
        XrXirInstanceResult result={0};
        do { result=xr_xir_instance_poll_bounded(instance,1);CHECK(++polls<4096); }
        while (result.outcome.status==XR_XIR_CALL_READY);
        CHECK(result.outcome.status==XR_XIR_CALL_CANCELLED);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);control->instances[i]=NULL;
        if (tasks[i].type) {
            const XirTask *task=(const XirTask *)(uintptr_t)tasks[i].payload;
            CHECK(task->state==XIR_TASK_TERMINAL && !task->executor && !task->call);
            for (unsigned copy=0;copy<2;++copy) {
                XrXirCallResult outcome={0};
                CHECK(xr_xir_task_copy_outcome(&tasks[i],&outcome)==(stop?XR_XIR_CALL_CANCELLED:XR_XIR_CALL_THROWN));
                if (!stop) CHECK(closed_error_value(&outcome.value,family));
                else CHECK(!outcome.value.type && xr_xir_panic_empty(&outcome.panic));
                xr_xir_call_result_drop(&outcome);
            }
            xr_xir_value_drop(&tasks[i]);
        }
        XrXirDomainBudgetStats spent=xr_xir_domain_budget_stats(domains[i]);
        CHECK(!spent.call_live && !spent.metadata_live && spent.call_allocations==spent.call_frees &&
            spent.metadata_allocations==spent.metadata_frees && xr_xir_domain_stats(domains[i]).live_bytes==sizeof(*domains[i]));
        xr_xir_domain_drop(domains[i]);
    }
    CHECK(!runtime_live && !runtime_bytes);
}
static void closed_error_control_reject(ClosedErrorControl *control,unsigned forged,unsigned family) {
    CHECK(family<CLOSED_ERROR_CASES);
    control->forged=forged;
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(control->base.program,&config,&control->instances[0])==XR_XIR_CALL_READY);
    XrXirInstance *instance=control->instances[0];
    CHECK(xr_xir_instance_start(instance,control->base.entry[family][0],NULL,0)==XR_XIR_CALL_READY);
    unsigned polls=0;XrXirValue task={0};XrXirInstanceResult result={0};
    do {
        result=xr_xir_instance_poll_bounded(instance,1);CHECK(++polls<4096);
        if (!task.type && instance->executor->active) {
            XrXirValue handle=task_core_task_value(instance->executor->active);
            CHECK(xr_xir_value_copy(&handle,&task)==XR_XIR_VALUE_OK);
        }
    } while (result.outcome.status==XR_XIR_CALL_READY);
    CHECK(task.type && result.outcome.status==XR_XIR_CALL_BAD_STATE && !result.outcome.value.type &&
        xr_xir_panic_empty(&result.outcome.panic));
    XrXirCallStatus freed=xr_xir_instance_free(instance);
    if (freed!=XR_XIR_CALL_BAD_STATE) fprintf(stderr,"Rejected error cleanup status=%u forged=%u\n",freed,forged);
    CHECK(freed==XR_XIR_CALL_BAD_STATE);control->instances[0]=NULL;
    XrXirCallResult sticky={0};CHECK(xr_xir_task_copy_outcome(&task,&sticky)==XR_XIR_CALL_BAD_STATE);
    CHECK(!sticky.value.type && xr_xir_panic_empty(&sticky.panic));
    xr_xir_call_result_drop(&sticky);xr_xir_value_drop(&task);xr_xir_value_drop(&control->replacement[0]);
    CHECK(!runtime_live && !runtime_bytes);
}
int main(int argc,char **argv) {
    CHECK(argc==3);ClosedErrorControl control={0};closed_error_control=&control;
    closed_error_build_configure(&control.base,argv[2],closed_error_control_configure);
    if (!strcmp(argv[1],"reject")) {
        CHECK(xr_xir_compile_type_arena_new(xr_xir_compile_artifact_context(control.base.lowered),
            control.base.program->types,&control.foreign)==XR_XIR_VALUE_OK);
        for (unsigned forged=1;forged<=3;++forged) closed_error_control_reject(&control,forged,1);
        CHECK(control.replacements==3);xr_xir_compile_type_arena_drop(control.foreign);
    } else {
        unsigned family=CLOSED_ERROR_CASES;
        for (unsigned n=0;n<CLOSED_ERROR_CASES;++n) if (!strcmp(argv[1],closed_error_names[n])) family=n;
        CHECK(family<CLOSED_ERROR_CASES);
        for (unsigned frontier=0;frontier<3;++frontier) for (unsigned stop=0;stop<2;++stop)
            closed_error_control_pair(&control,family,frontier,stop!=0);
    }
    xr_xir_compile_program_drop(control.base.program);
    CHECK(control.base.code_releases==1 && !control.base.lowered && !runtime_live && !runtime_bytes);
    closed_error_control=NULL;effects_source_owners_free();CHECK(!effects_compile_live && !effects_compile_bytes);
    printf("TASK_ERROR_CONTROL %s %s %s physical=0/0\n",argv[1],argv[2],
        control.forged?"exactVariantAndTypeAndArenaDenied":"rootCancel childThrown stop childCancelled frontiers3 twoInstances");return 0;
}
