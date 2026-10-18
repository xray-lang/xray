/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_instance_generations.c - Candidate publication and exact birth owners
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xir/xxir_instance_generation.h"
#include "xir/xxir_program_packet_internal.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"
#define XIR_INSTANCE_IMPLEMENTATION_PRESENT
#include "xir_instance_retirement_probe.c"
#undef XIR_INSTANCE_IMPLEMENTATION_PRESENT
#undef CHECK
#define main original_instance_regressions
#include "test_xir_instances.c"
#undef main

static XrXirInstanceResult generation_drive(XrXirInstance *instance) {
    XrXirInstanceResult result;
    do { result = xr_xir_instance_poll_bounded(instance,256); }
    while (result.outcome.status == XR_XIR_CALL_READY);
    return result;
}
static void generation_normal(void) {
    unsigned old_releases=0,new_releases=0;
    XrXirProgram *old=NULL,*newer=NULL,*failed=NULL;
    CHECK(function_case_seal(&old_releases,&old)==XR_XIR_OK);
    CHECK(function_case_seal(&new_releases,&newer)==XR_XIR_OK);
    Fixture failure;fixture(&failure,2);
    CHECK(xr_xir_compile_program_seal(xr_xir_compile_artifact_context(failure.proof),&failure.spec,&failed)==XR_XIR_OK);
    xr_xir_compile_artifact_free(failure.proof);failure.proof=NULL;native_fixture_owner_free(&failure.compiler);
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_generation_new(&config,&instance)==XR_XIR_CALL_READY);
    XrXirInstance *before=instance;
    CHECK(xr_xir_instance_generation_new(&config,&instance)==XR_XIR_CALL_BAD_ARGUMENT && instance==before);
    CHECK(xr_xir_instance_new(old,&config,&instance)==XR_XIR_CALL_BAD_ARGUMENT && instance==before);
    CHECK(xr_xir_instance_start(instance,0,NULL,0)==XR_XIR_CALL_BAD_STATE);
    uint64_t generation=0,published=99;
    CHECK(xr_xir_instance_generation_published(instance,&published)==XR_XIR_CALL_READY && !published);
    CHECK(xr_xir_instance_generation_prepare(instance,old,&generation)==XR_XIR_CALL_READY && generation==1);
    CHECK(instance->prepared_generation->domain==instance->domain &&
        instance_budget(instance->prepared_generation)==&instance->budget);
    CHECK(xr_xir_instance_generation_publish(instance,generation)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_start(instance,2,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult first=generation_drive(instance);
    CHECK(first.outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue old_function={0};
    CHECK(xr_xir_instance_take_result(instance,&old_function)==XR_XIR_CALL_RETURNED);
    const XrXirFunctionBinding *binding=xr_xir_function_binding(&old_function);
    CHECK(binding && ((FunctionGate *)binding->owner)->program==old &&
        ((FunctionGate *)binding->owner)->instance==instance);
    CHECK(xr_xir_instance_start(instance,1,NULL,0)==XR_XIR_CALL_BAD_STATE);
    size_t attempts=runtime_attempts;
    CHECK(xr_xir_instance_generation_publish(instance,generation)==XR_XIR_CALL_READY && runtime_attempts==attempts);
    XrXirInstance *old_state=instance->published_generation;
    XrXirValue old_cell=old_state->slots[0];
    uint64_t second=0;
    CHECK(xr_xir_instance_generation_prepare(instance,newer,&second)==XR_XIR_CALL_READY && second==2);
    CHECK(xr_xir_instance_start(instance,1,NULL,0)==XR_XIR_CALL_READY);
    CHECK(generation_drive(instance).outcome.status==XR_XIR_CALL_RETURNED);
    attempts=runtime_attempts;
    CHECK(xr_xir_instance_generation_publish(instance,second)==XR_XIR_CALL_READY && attempts==runtime_attempts);
    CHECK(!old_state->call && old_state->slots[0].payload==old_cell.payload);
    CHECK(instance->published_generation->slots[0].payload!=old_cell.payload);
    XrXirDomainBudgetStats selection=xr_xir_domain_budget_stats(instance->domain);
    uint64_t work_limit=instance->domain->budget.work_limit;
    XrXirCall *published_call=instance->published_generation->call;
    instance->domain->budget.work_limit=selection.work+1;
    attempts=runtime_attempts;
    CHECK(xr_xir_instance_start_function(instance,&old_function,NULL,0)==XR_XIR_CALL_LIMIT);
    CHECK(runtime_attempts==attempts && instance->published_generation->call==published_call &&
        instance_current(instance)==instance->published_generation && !old_state->call);
    CHECK(xr_xir_domain_budget_stats(instance->domain).work==selection.work+1);
    instance->domain->budget.work_limit=work_limit;
    CHECK(xr_xir_instance_start_function(instance,&old_function,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult wait=generation_drive(instance);
    CHECK(wait.outcome.status==XR_XIR_CALL_SUSPENDED && wait.epoch>first.epoch);
    CHECK(xr_xir_instance_resume(instance,first.epoch,wait.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(instance,wait.epoch,wait.outcome.wake)==XR_XIR_CALL_READY);
    XrXirInstanceResult returned=generation_drive(instance);
    CHECK(returned.outcome.status==XR_XIR_CALL_RETURNED);
    strings_equal(&returned.outcome.value,"owned callback result",21);
    XrXirCall *previous_call=old_state->call;
    XrXirValue previous_result=returned.outcome.value;
    uint64_t third=0;
    XrXirDomainBudgetStats prefix=xr_xir_domain_budget_stats(instance->domain);
    CHECK(xr_xir_instance_generation_prepare(instance,failed,&third)==XR_XIR_CALL_READY && third==3);
    CHECK(xr_xir_instance_start(instance,3,NULL,0)==XR_XIR_CALL_READY);
    wait=generation_drive(instance);CHECK(wait.outcome.status==XR_XIR_CALL_SUSPENDED);
    CHECK(xr_xir_instance_resume(instance,wait.epoch,wait.outcome.wake)==XR_XIR_CALL_READY);
    CHECK(generation_drive(instance).outcome.status==XR_XIR_CALL_THROWN);
    CHECK(xr_xir_instance_generation_publish(instance,third)==XR_XIR_CALL_BAD_STATE);
    XrXirCallResult escaped={0};
    CHECK(xr_xir_instance_copy_failure(instance,&escaped)==XR_XIR_CALL_THROWN);
    CHECK(xr_xir_instance_generation_abort(instance,third)==XR_XIR_CALL_READY);
    CHECK(instance->published_generation->generation_id==second && instance_current(instance)==old_state);
    CHECK(old_state->call==previous_call && xr_xir_instance_poll_bounded(instance,1).outcome.value.payload==previous_result.payload);
    CHECK(xr_xir_instance_generation_abort(instance,third)==XR_XIR_CALL_BAD_STATE);
    XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(instance->domain);
    CHECK(after.work>prefix.work && after.requested_bytes>prefix.requested_bytes &&
        after.requested_call_bytes>prefix.requested_call_bytes);
    XrXirValue owned_result={0};
    CHECK(xr_xir_instance_take_result(instance,&owned_result)==XR_XIR_CALL_RETURNED);
    xr_xir_compile_program_drop(old);xr_xir_compile_program_drop(newer);xr_xir_compile_program_drop(failed);
    CHECK(failure.witness.releases==1 && !old_releases && !new_releases);
    CHECK(xr_xir_instance_stop(instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start_function(instance,&old_function,NULL,0)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && new_releases==1 && !old_releases);
    strings_equal(&owned_result,"owned callback result",21);
    XrXirDomain *reader=NULL;CHECK(xr_xir_domain_new(65536,&reader)==XR_XIR_VALUE_OK);
    CHECK(error_fixture_is_code(&escaped.value,reader,91));
    xr_xir_domain_drop(reader);xr_xir_value_drop(&owned_result);xr_xir_call_result_drop(&escaped);
    xr_xir_value_drop(&old_function);CHECK(old_releases==1);
    CHECK(!runtime_live && !runtime_bytes);instance_compile_zero();
    puts("Generation publish/abort: old closure, original result/storage, one ledger, no commit allocation and final physical0 PASS");
}
typedef struct GenerationCost {
    XrXirDomainBudgetStats budget;
    XrXirDomainStats values;
    size_t sites;
    XrXirCallStatus failure;
} GenerationCost;
static XrXirAction generation_throw(XrXirCallView *view) {
    Frame *frame=view->state;
    frame->value=error_fixture_code(xr_xir_call_admission(view),91);
    return action(XR_XIR_ACTION_THROW,frame->value);
}
static void generation_terminal_rejection(void) {
    Fixture source;fixture_spec(&source,1);source.entries[7].resume=generation_throw;
    CHECK(native_fixture_owner_new(&source.compiler)==XR_XIR_OK);
    CHECK(native_metadata_fixture(&source.compiler.context,&source.spec,&source.proof)==XR_XIR_OK);
    source.spec.types=xr_xir_compile_artifact_module(source.proof)->types;
    source.spec.packet=xir_program_packet_borrow(source.proof);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(xr_xir_compile_artifact_context(source.proof),&source.spec,&program)==XR_XIR_OK);
    xr_xir_compile_artifact_free(source.proof);native_fixture_owner_free(&source.compiler);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    Trace observer={0};config.trace=trace;config.trace_context=&observer;
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_generation_new(&config,&instance)==XR_XIR_CALL_READY);
    observer.instance=instance;
    uint64_t serial=9;size_t attempts=runtime_attempts;
    CHECK(xr_xir_instance_generation_prepare(instance,program,&serial)==XR_XIR_CALL_BAD_ARGUMENT &&
        serial==9 && runtime_attempts==attempts);
    serial=0;CHECK(xr_xir_instance_generation_prepare(instance,program,&serial)==XR_XIR_CALL_READY);
    uint64_t pending=0;CHECK(xr_xir_instance_generation_prepare(instance,program,&pending)==XR_XIR_CALL_BUSY && !pending);
    CHECK(xr_xir_instance_generation_abort(instance,serial+1)==XR_XIR_CALL_BAD_STATE);
    XrXirValue input={0};CHECK(xr_xir_string_new(instance->domain,"input",5,&input)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_instance_start(instance,7,&input,1)==XR_XIR_CALL_READY);
    XrXirInstanceResult wait=generation_drive(instance);CHECK(wait.outcome.status==XR_XIR_CALL_SUSPENDED);
    CHECK(xr_xir_instance_generation_publish(instance,serial)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(instance,wait.epoch,wait.outcome.wake)==XR_XIR_CALL_READY);
    CHECK(generation_drive(instance).outcome.status==XR_XIR_CALL_THROWN && xr_xir_instance_state(instance)==XR_XIR_INSTANCE_READY);
    XrXirValue error={0};CHECK(xr_xir_instance_take_result(instance,&error)==XR_XIR_CALL_THROWN);
    CHECK(xr_xir_call_state(instance_current(instance)->call)==XR_XIR_CALL_CONSUMED);
    CHECK(xr_xir_instance_generation_publish(instance,serial)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_start(instance,6,NULL,0)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_generation_abort(instance,serial)==XR_XIR_CALL_READY);
    uint64_t next=0;CHECK(xr_xir_instance_generation_prepare(instance,program,&next)==XR_XIR_CALL_READY && next>serial);
    CHECK(xr_xir_instance_start(instance,8,NULL,0)==XR_XIR_CALL_READY);
    wait=generation_drive(instance);CHECK(wait.outcome.status==XR_XIR_CALL_SUSPENDED);
    CHECK(xr_xir_instance_generation_abort(instance,next)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_resume(instance,wait.epoch,wait.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    uint64_t published=99;CHECK(xr_xir_instance_generation_published(instance,&published)==XR_XIR_CALL_READY && !published);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    xr_xir_value_drop(&input);xr_xir_value_drop(&error);xr_xir_compile_program_drop(program);
    CHECK(source.witness.releases==1 && !runtime_live && !runtime_bytes);instance_compile_zero();
    puts("Generation terminal rejection: consumed THROW cannot publish or rerun; suspended abort rejects stale wake; serial and final physical0 PASS");
}
static bool generation_fault_run(XrXirProgram *old,XrXirProgram *newer,size_t fail_at,
    const XrXirInstanceConfig *limits,GenerationCost *cost) {
    size_t prefix_live=runtime_live,prefix_bytes=runtime_bytes;
    runtime_attempts=0;runtime_fail_at=fail_at;
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    if(limits)config=*limits;
    XrXirInstance *instance=NULL;XrXirValue function={0},result={0};
    XrXirCallStatus status=xr_xir_instance_generation_new(&config,&instance);
    XrXirCallStatus accepted=XR_XIR_CALL_READY;
    uint64_t first=0,second=0;
    if(status==XR_XIR_CALL_READY)status=xr_xir_instance_generation_prepare(instance,old,&first);
    if(status==XR_XIR_CALL_READY)status=xr_xir_instance_start(instance,2,NULL,0);
    if(status==XR_XIR_CALL_READY) {
        status=generation_drive(instance).outcome.status;
        if(status==XR_XIR_CALL_RETURNED)
            status=xr_xir_instance_take_result(instance,&function)==XR_XIR_CALL_RETURNED?XR_XIR_CALL_READY:XR_XIR_CALL_BAD_STATE;
        else accepted=status;
    }
    if(status==XR_XIR_CALL_READY)status=xr_xir_instance_generation_publish(instance,first);
    XrXirInstance *prefix=instance?instance->published_generation:NULL;
    XrXirCall *prefix_call=prefix?prefix->call:NULL;
    XrXirValue prefix_cell=prefix?prefix->slots[0]:(XrXirValue){0};
    if(status==XR_XIR_CALL_READY)status=xr_xir_instance_generation_prepare(instance,newer,&second);
    if(status==XR_XIR_CALL_READY)status=xr_xir_instance_start(instance,1,NULL,0);
    if(status==XR_XIR_CALL_READY) {
        status=generation_drive(instance).outcome.status;
        if(status==XR_XIR_CALL_RETURNED)status=XR_XIR_CALL_READY;
        else accepted=status;
    }
    if(status==XR_XIR_CALL_READY)status=xr_xir_instance_generation_publish(instance,second);
    if(status==XR_XIR_CALL_READY)status=xr_xir_instance_start_function(instance,&function,NULL,0);
    if(status==XR_XIR_CALL_READY) {
        XrXirInstanceResult wait=generation_drive(instance);
        if(wait.outcome.status==XR_XIR_CALL_SUSPENDED)status=xr_xir_instance_resume(instance,wait.epoch,wait.outcome.wake);
        else {status=wait.outcome.status;accepted=status;}
    }
    if(status==XR_XIR_CALL_READY) {
        status=generation_drive(instance).outcome.status;
        if(status==XR_XIR_CALL_RETURNED) {
            CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
            strings_equal(&result,"owned callback result",21);status=XR_XIR_CALL_READY;
        } else accepted=status;
    }
    bool complete=status==XR_XIR_CALL_READY;
    CHECK(complete || status==XR_XIR_CALL_OOM || status==XR_XIR_CALL_LIMIT);
    cost->sites=runtime_attempts;cost->failure=status;runtime_fail_at=SIZE_MAX;
    if(instance) {
        cost->budget=xr_xir_domain_budget_stats(instance->domain);
        cost->values=xr_xir_domain_stats(instance->domain);
    }
    if(instance && instance->prepared_generation) {
        XrXirInstance *candidate=instance->prepared_generation;
        uint64_t before=instance->published_generation?instance->published_generation->generation_id:0;
        XrXirDomainBudgetStats charged=xr_xir_domain_budget_stats(instance->domain);
        XrXirCallStatus cancelled=xr_xir_instance_generation_abort(instance,candidate->generation_id);
        XrXirCallStatus expected=accepted==XR_XIR_CALL_READY && instance->budget.exhausted?
            XR_XIR_CALL_LIMIT:accepted;
        CHECK(cancelled==expected);
        accepted=XR_XIR_CALL_READY;
        uint64_t after=UINT64_MAX;
        CHECK(xr_xir_instance_generation_published(instance,&after)==XR_XIR_CALL_READY && before==after);
        XrXirDomainBudgetStats restored=xr_xir_domain_budget_stats(instance->domain);
        CHECK(restored.requested_bytes>=charged.requested_bytes && restored.work>=charged.work &&
            restored.requested_call_bytes>=charged.requested_call_bytes);
    }
    if(instance && prefix && instance->published_generation==prefix) {
        CHECK(instance_current(instance)==prefix && prefix->call==prefix_call &&
            prefix->slots[0].payload==prefix_cell.payload);
    }
    if(instance && accepted==XR_XIR_CALL_READY && instance->budget.exhausted)accepted=XR_XIR_CALL_LIMIT;
    CHECK(xr_xir_instance_free(instance)==accepted);
    xr_xir_value_drop(&function);xr_xir_value_drop(&result);
    CHECK(runtime_live==prefix_live && runtime_bytes==prefix_bytes);
    return complete;
}
static void generation_faults(void) {
    unsigned releases[2]={0};XrXirProgram *programs[2]={0};
    for(unsigned i=0;i<2;++i)CHECK(function_case_seal(&releases[i],&programs[i])==XR_XIR_OK);
    uint32_t refs[2]={atomic_load(&programs[0]->references),atomic_load(&programs[1]->references)};
    GenerationCost baseline={0};
    CHECK(generation_fault_run(programs[0],programs[1],SIZE_MAX,NULL,&baseline) && baseline.sites);
    for(size_t ordinal=0;ordinal<baseline.sites;++ordinal) {
        GenerationCost actual={0};
        CHECK(!generation_fault_run(programs[0],programs[1],ordinal,NULL,&actual) && actual.sites>ordinal);
        CHECK(atomic_load(&programs[0]->references)==refs[0] && atomic_load(&programs[1]->references)==refs[1]);
    }
    for(unsigned i=0;i<2;++i) {CHECK(!releases[i]);xr_xir_compile_program_drop(programs[i]);CHECK(releases[i]==1);}
    CHECK(!runtime_live && !runtime_bytes);instance_compile_zero();
    printf("Generation complete runtime OOM sites=%zu; exact snapshots, cumulative charges and physical baseline PASS\n",baseline.sites);
}
static void generation_axis_limit(XrXirInstanceConfig *config,unsigned axis,uint64_t cap) {
    switch(axis) {
    case 0: config->requested_value_limit=cap;break;
    case 1: config->requested_call_limit=cap;break;
    case 2: config->work_limit=cap;break;
    case 3: config->metadata_limit=cap;break;
    case 4: config->call_limit=cap;break;
    default: CHECK(axis==5);config->value_limit=cap;break;
    }
}
static void generation_axes(void) {
    unsigned releases[2]={0};XrXirProgram *programs[2]={0};
    for(unsigned i=0;i<2;++i)CHECK(function_case_seal(&releases[i],&programs[i])==XR_XIR_OK);
    GenerationCost baseline={0};
    CHECK(generation_fault_run(programs[0],programs[1],SIZE_MAX,NULL,&baseline));
    XrXirInstanceConfig original;CHECK(xr_xir_instance_config_init(&original,sizeof(original))==XR_XIR_CALL_READY);
    uint64_t caps[]={original.requested_value_limit,original.requested_call_limit,original.work_limit,
        original.metadata_limit,original.call_limit,original.value_limit};
    for(unsigned axis=0;axis<6;++axis) {
        uint64_t low=0,high=caps[axis];unsigned probes=0;
        while(high-low>1) {
            CHECK(++probes<=64);XrXirInstanceConfig config=original;
            uint64_t cap=low+(high-low)/2;generation_axis_limit(&config,axis,cap);
            GenerationCost actual={0};
            if(generation_fault_run(programs[0],programs[1],SIZE_MAX,&config,&actual)) {
                if(axis==2)CHECK(actual.budget.work==baseline.budget.work &&
                    actual.budget.requested_bytes==baseline.budget.requested_bytes &&
                    actual.budget.requested_call_bytes==baseline.budget.requested_call_bytes);
                high=cap;
            } else {CHECK(actual.failure==XR_XIR_CALL_LIMIT);low=cap;}
        }
        CHECK(high>1);
        for(unsigned minus=0;minus<2;++minus) {
            XrXirInstanceConfig config=original;generation_axis_limit(&config,axis,high-minus);
            GenerationCost actual={0};
            bool complete=generation_fault_run(programs[0],programs[1],SIZE_MAX,&config,&actual);
            CHECK(complete==!minus && actual.failure==(minus?XR_XIR_CALL_LIMIT:XR_XIR_CALL_READY));
        }
        printf("GENERATION_AXIS axis=%u exact=%llu minus1=LIMIT probes=%u original_cap=%llu physical_baseline=1\n",
            axis,(unsigned long long)high,probes,(unsigned long long)caps[axis]);
    }
    for(unsigned i=0;i<2;++i){xr_xir_compile_program_drop(programs[i]);CHECK(releases[i]==1);}
    CHECK(!runtime_live && !runtime_bytes);instance_compile_zero();
}
static void generation_same_quota(void) {
    unsigned releases[2]={0};XrXirProgram *programs[2]={0};
    for(unsigned i=0;i<2;++i)CHECK(function_case_seal(&releases[i],&programs[i])==XR_XIR_OK);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_generation_new(&config,&instance)==XR_XIR_CALL_READY);
    uint64_t first=0;CHECK(xr_xir_instance_generation_prepare(instance,programs[0],&first)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,1,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult returned=generation_drive(instance);
    CHECK(returned.outcome.status==XR_XIR_CALL_RETURNED && returned.outcome.value.payload==17);
    CHECK(xr_xir_instance_generation_publish(instance,first)==XR_XIR_CALL_READY);
    XrXirInstance *published=instance->published_generation;
    XrXirCall *call=published->call;XrXirValue cell=published->slots[0];
    XrXirDomainStats values=xr_xir_domain_stats(instance->domain);
    XrXirDomainBudgetStats prefix=xr_xir_domain_budget_stats(instance->domain);
    uint64_t original_limit=instance->domain->budget.requested_call_limit;
    CHECK(prefix.requested_call_bytes+sizeof(XrXirInstance)<original_limit);
    instance->domain->budget.requested_call_limit=prefix.requested_call_bytes+sizeof(XrXirInstance);
    uint64_t second=0;size_t before=runtime_attempts;
    runtime_fail_at=before;
    CHECK(xr_xir_instance_generation_prepare(instance,programs[1],&second)==XR_XIR_CALL_OOM && !second);
    CHECK(runtime_attempts==before+1);
    runtime_fail_at=SIZE_MAX;
    XrXirDomainBudgetStats failed=xr_xir_domain_budget_stats(instance->domain);
    CHECK(failed.requested_call_bytes==prefix.requested_call_bytes+sizeof(XrXirInstance) && failed.work>prefix.work);
    before=runtime_attempts;
    CHECK(xr_xir_instance_generation_prepare(instance,programs[1],&second)==XR_XIR_CALL_LIMIT && !second);
    CHECK(runtime_attempts==before && instance->published_generation==published &&
        instance_current(instance)==published && published->call==call && published->slots[0].payload==cell.payload);
    XrXirInstanceResult unchanged=xr_xir_instance_poll_bounded(instance,1);
    CHECK(unchanged.epoch==returned.epoch && unchanged.outcome.status==XR_XIR_CALL_RETURNED &&
        unchanged.outcome.value.payload==17);
    XrXirDomainStats after=xr_xir_domain_stats(instance->domain);
    CHECK(after.live_bytes==values.live_bytes && instance->domain->budget.requested_call_bytes==failed.requested_call_bytes);
    instance->domain->budget.requested_call_limit=original_limit;
    CHECK(xr_xir_instance_generation_prepare(instance,programs[1],&second)==XR_XIR_CALL_READY && second>first);
    CHECK(xr_xir_instance_generation_abort(instance,second)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    for(unsigned i=0;i<2;++i){xr_xir_compile_program_drop(programs[i]);CHECK(releases[i]==1);}
    CHECK(!runtime_live && !runtime_bytes);instance_compile_zero();
    puts("Generation same-owner OOM then same-quota LIMIT: no allocation retry, original Call/result/slot unchanged, charges not refunded PASS");
}
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--runtime-fi")) {
        generation_faults();instance_compile_report();native_fixture_owner_report();return 0;
    }
    if(argc==2 && !strcmp(argv[1],"--runtime-axes")) {
        generation_axes();instance_compile_report();native_fixture_owner_report();return 0;
    }
    if(argc==2 && !strcmp(argv[1],"--runtime-prepare")) {
        generation_same_quota();instance_compile_report();native_fixture_owner_report();return 0;
    }
    CHECK(argc==1);
    generation_normal();
    generation_terminal_rejection();
    instance_compile_report();native_fixture_owner_report();
    return 0;
}
