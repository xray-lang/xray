/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * initializer_runtime_resources.h - Real constructor allocation and budget traversal
 *
 * KEY CONCEPT:
 *   Every failed admission restores physical owners while retaining its debit.
 */
#ifndef INITIALIZER_RUNTIME_RESOURCES_H
#define INITIALIZER_RUNTIME_RESOURCES_H
typedef struct InitializerRuntimeCost {
    XrXirDomainBudgetStats budget;
    XrXirDomainStats values;
    size_t sites, after_start;
    XrXirCallStatus status;
} InitializerRuntimeCost;
static unsigned initializer_resource_mode;

static XrXirCallStatus resource_drive(XrXirInstance *instance,unsigned mode) {
    XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
    unsigned suspended=0;
    while (result.outcome.status==XR_XIR_CALL_SUSPENDED) {
        CHECK(++suspended<=2 && (mode==2 || mode==3 || mode==5));
        bool cancel=mode==3 || (mode==5 && suspended==2);
        XrXirCallStatus next=cancel?xr_xir_instance_cancel_current(instance):
            xr_xir_instance_resume(instance,result.epoch,result.outcome.wake);
        CHECK(next==(cancel?XR_XIR_CALL_CANCEL_REQUESTED:XR_XIR_CALL_READY));
        result=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
    }
    XrXirCallStatus expected=mode==1 || mode==4?XR_XIR_CALL_ASSERTION:
        mode==3 || mode==5?XR_XIR_CALL_CANCELLED:XR_XIR_CALL_RETURNED;
    if (result.outcome.status!=expected) return result.outcome.status;
    if (mode==2) CHECK(suspended==2);
    if (mode==3) CHECK(suspended==1);
    if (mode==5) CHECK(suspended==2);
    if (expected==XR_XIR_CALL_RETURNED) {
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==expected);
        CHECK(value.type==XR_XIR_I64 && value.payload==59);xr_xir_value_drop(&value);
    } else {
        XrXirValue value={0},empty={0};
        CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_BAD_STATE && !memcmp(&value,&empty,sizeof(value)));
        if (expected==XR_XIR_CALL_ASSERTION) {
            const char *text=NULL;size_t length=0;
            const char *message=mode==4?"init-partial-failure":"init-owned-failure";
            CHECK(xr_xir_string_view(&result.outcome.panic.message,&text,&length) && length==strlen(message) && !memcmp(text,message,length));
        }
    }
    return XR_XIR_CALL_READY;
}
static bool resource_case(XrXirProgram *program,const InitializerEntries *entries,unsigned mode,
    size_t fail_at,const XrXirInstanceConfig *limits,InitializerRuntimeCost *cost) {
    CHECK(mode<6);
    size_t blocks=runtime_live,bytes=runtime_bytes;
    size_t compiler_blocks=instance_compile_live,compiler_bytes=instance_compile_bytes;
    runtime_attempts=0;runtime_fail_at=fail_at;
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    if (limits) config=*limits;
    XrXirInstance *instance=NULL;XrXirClassObservation *observation=NULL;
    XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
    if (status==XR_XIR_CALL_READY) status=xr_xir_instance_class_observation_open(instance,12,&observation);
    uint32_t entry=mode==0?entries->answer:mode==1?entries->failure:mode==4?entries->partial:entries->suspended;
    if (status==XR_XIR_CALL_READY) status=xr_xir_instance_start(instance,entry,NULL,0);
    cost->after_start=runtime_attempts;
    if (status==XR_XIR_CALL_READY) status=resource_drive(instance,mode);
    if (status==XR_XIR_CALL_READY && (mode==1 || mode==3 || mode==4 || mode==5)) {
        status=xr_xir_instance_start(instance,entries->answer,NULL,0);
        if (status==XR_XIR_CALL_READY) status=resource_drive(instance,0);
    }
    bool complete=status==XR_XIR_CALL_READY;
    if (!complete && status!=XR_XIR_CALL_OOM && status!=XR_XIR_CALL_LIMIT)
        fprintf(stderr,"resource mode=%u fail_at=%zu unexpected=%u\n",mode,fail_at,(unsigned)status);
    CHECK(complete || status==XR_XIR_CALL_OOM || status==XR_XIR_CALL_LIMIT);
    cost->sites=runtime_attempts;cost->status=status;
    if (instance) {
        cost->budget=xr_xir_domain_budget_stats(instance->domain);
        cost->values=xr_xir_domain_stats(instance->domain);
    }
    runtime_fail_at=SIZE_MAX;
    bool exhausted=instance && instance->budget.exhausted;
    XrXirCallStatus completion=instance?xr_xir_task_executor_completion_status(instance->executor):XR_XIR_CALL_READY;
    XrXirCallStatus expected_disposal=completion!=XR_XIR_CALL_READY?completion:
        exhausted?XR_XIR_CALL_LIMIT:XR_XIR_CALL_READY;
    XrXirCallStatus disposed=xr_xir_instance_free(instance);
    if (disposed!=expected_disposal)
        fprintf(stderr,"resource cleanup mode=%u fail_at=%zu status=%u disposed=%u exhausted=%u\n",
            mode,fail_at,(unsigned)status,(unsigned)disposed,(unsigned)exhausted);
    CHECK(disposed==expected_disposal);
    xr_xir_class_observation_drop(&observation);
    CHECK(runtime_attempts==cost->sites);
    CHECK(runtime_live==blocks && runtime_bytes==bytes);
    CHECK(instance_compile_live==compiler_blocks && instance_compile_bytes==compiler_bytes);
    return complete;
}
static void resource_axis_limit(XrXirInstanceConfig *config,unsigned axis,uint64_t cap) {
    switch(axis) {
    case 0: config->requested_value_limit=cap;break;
    case 1: config->requested_call_limit=cap;break;
    case 2: config->work_limit=cap;break;
    case 3: config->metadata_limit=cap;break;
    case 4: config->call_limit=cap;break;
    default: CHECK(axis==5);config->value_limit=cap;break;
    }
}
static void resource_axes(XrXirProgram *program,const InitializerEntries *entries,unsigned mode) {
    XrXirInstanceConfig original;CHECK(xr_xir_instance_config_init(&original,sizeof(original))==XR_XIR_CALL_READY);
    uint64_t caps[]={original.requested_value_limit,original.requested_call_limit,original.work_limit,
        original.metadata_limit,original.call_limit,original.value_limit};
    unsigned axes=initializer_resource_mode==5?1u:6u;
    for (unsigned axis=0;axis<axes;++axis) {
        uint64_t low=0,high=caps[axis];unsigned probes=0;
        while (high-low>1) {
            CHECK(++probes<=64);XrXirInstanceConfig config=original;uint64_t cap=low+(high-low)/2;
            resource_axis_limit(&config,axis,cap);InitializerRuntimeCost actual={0};
            if (resource_case(program,entries,mode,SIZE_MAX,&config,&actual)) high=cap;
            else {CHECK(actual.status==XR_XIR_CALL_LIMIT);low=cap;}
        }
        CHECK(high>1);
        for (unsigned minus=0;minus<2;++minus) {
            XrXirInstanceConfig config=original;resource_axis_limit(&config,axis,high-minus);
            InitializerRuntimeCost actual={0};bool complete=resource_case(program,entries,mode,SIZE_MAX,&config,&actual);
            CHECK(complete==!minus && actual.status==(minus?XR_XIR_CALL_LIMIT:XR_XIR_CALL_READY));
        }
        printf("INIT_AXIS mode=%u axis=%u exact=%llu minus1=LIMIT probes=%u original_cap=%llu physical_baselines=1\n",
            mode,axis,(unsigned long long)high,probes,(unsigned long long)caps[axis]);
    }
}
static void resource_suite(XrXirProgram *program,const InitializerEntries *entries) {
    CHECK(initializer_resource_mode>=1 && initializer_resource_mode<=5);
    for (unsigned mode=0;mode<6;++mode) {
        InitializerRuntimeCost baseline={0};CHECK(resource_case(program,entries,mode,SIZE_MAX,NULL,&baseline));
        CHECK(baseline.sites && baseline.after_start<baseline.sites);
        printf("INIT_BASELINE mode=%u sites=%zu after_start=%zu requested_value=%llu requested_call=%llu work=%llu metadata_peak=%llu call_peak=%llu value_peak=%llu physical_baselines=1\n",
            mode,baseline.sites,baseline.after_start,(unsigned long long)baseline.budget.requested_bytes,
            (unsigned long long)baseline.budget.requested_call_bytes,(unsigned long long)baseline.budget.work,
            (unsigned long long)baseline.budget.metadata_peak,(unsigned long long)baseline.budget.call_peak,
            (unsigned long long)baseline.values.peak_bytes);
        if (initializer_resource_mode==2) {
            for (size_t ordinal=0;ordinal<baseline.sites;++ordinal) {
                InitializerRuntimeCost actual={0};
                CHECK(!resource_case(program,entries,mode,ordinal,NULL,&actual));
                CHECK(actual.status==XR_XIR_CALL_OOM && actual.sites>ordinal);
            }
            printf("INIT_FI mode=%u sites=%zu all=OOM physical_baselines=1\n",mode,baseline.sites);
        } else if (initializer_resource_mode==3 || initializer_resource_mode==5) resource_axes(program,entries,mode);
        else if (initializer_resource_mode==4) {
            InitializerRuntimeCost actual={0};
            CHECK(!resource_case(program,entries,mode,baseline.after_start,NULL,&actual));
            CHECK(actual.status==XR_XIR_CALL_OOM && actual.sites>baseline.after_start);
            printf("INIT_FOCUSED_FAULT mode=%u ordinal=%zu OOM physical_baselines=1\n",mode,baseline.after_start);
        }
    }
}
static void initializer_profile_cases(XrXirProgram *program,const InitializerEntries *entries) {
    if (initializer_resource_mode) resource_suite(program,entries);
    else for (unsigned mode=0;mode<6;++mode) lifecycle_case(program,entries,mode);
}
#endif // INITIALIZER_RUNTIME_RESOURCES_H
