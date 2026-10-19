/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_initializer_places_runtime.c - Real construction storage failure boundaries
 *
 * KEY CONCEPT:
 *   Private field Cells and projected Array writes keep their actual owners
 *   through allocation failure, every cancellation prefix and bounded retries.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef XR_LANGUAGE_RUNTIME_EXPECTED
#define XR_LANGUAGE_RUNTIME_EXPECTED 131256
#endif
#ifndef XR_LANGUAGE_RUNTIME_CROSSINGS
#define XR_LANGUAGE_RUNTIME_CROSSINGS 4
#endif
#ifndef XR_LANGUAGE_RUNTIME_SUSPENSIONS
#define XR_LANGUAGE_RUNTIME_SUSPENSIONS 0
#endif
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "../xir_source_fixture_owner.h"
#include "../xir_instance_compile_observer.h"
#include "../xir_runtime_allocations.h"
XR_DATA const XrXirProgramSpec language_reference_program;
typedef struct PlaceRuntimeCost { size_t sites;XrXirCallStatus status; } PlaceRuntimeCost;

static XrXirCallStatus place_answer(XrXirInstance *instance,uint32_t answer) {
    XrXirCallStatus status=xr_xir_instance_start(instance,answer,NULL,0);
    unsigned resumed=0;
    while (status==XR_XIR_CALL_READY) {
        XrXirInstanceResult observed=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
        status=observed.outcome.status;
        if (status==XR_XIR_CALL_SUSPENDED) {
            CHECK(resumed++<XR_LANGUAGE_RUNTIME_SUSPENSIONS);
            status=xr_xir_instance_resume(instance,observed.epoch,observed.outcome.wake);
            CHECK(status==XR_XIR_CALL_READY);
        }
    }
    if (status==XR_XIR_CALL_RETURNED) {
        CHECK(resumed==XR_LANGUAGE_RUNTIME_SUSPENSIONS);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==status);
        CHECK(value.type==XR_XIR_I64 && value.payload==XR_LANGUAGE_RUNTIME_EXPECTED);xr_xir_value_drop(&value);
    }
    return status;
}
static bool place_case(XrXirProgram *program,uint32_t answer,size_t fault,
    const XrXirInstanceConfig *limits,PlaceRuntimeCost *cost) {
    size_t blocks=runtime_live,bytes=runtime_bytes,compiler_blocks=instance_compile_live,compiler_bytes=instance_compile_bytes;
    runtime_attempts=0;runtime_fail_at=fault;
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    if (limits) config=*limits;
    XrXirInstance *instance=NULL;XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
    if (status==XR_XIR_CALL_READY) status=place_answer(instance,answer);
    CHECK(status==XR_XIR_CALL_RETURNED || status==XR_XIR_CALL_OOM || status==XR_XIR_CALL_LIMIT);
    cost->sites=runtime_attempts;cost->status=status;runtime_fail_at=SIZE_MAX;
    XrXirCallStatus completion=instance?xr_xir_task_executor_completion_status(instance->executor):XR_XIR_CALL_READY;
    XrXirCallStatus disposal=completion!=XR_XIR_CALL_READY?completion:
        instance && instance->budget.exhausted?XR_XIR_CALL_LIMIT:XR_XIR_CALL_READY;
    CHECK(xr_xir_instance_free(instance)==disposal);
    CHECK(runtime_attempts==cost->sites && runtime_live==blocks && runtime_bytes==bytes);
    CHECK(instance_compile_live==compiler_blocks && instance_compile_bytes==compiler_bytes);
    return status==XR_XIR_CALL_RETURNED;
}
static void place_faults(XrXirProgram *program,uint32_t answer,unsigned mode,size_t sites) {
    for (size_t ordinal=0;ordinal<sites;++ordinal) {
        PlaceRuntimeCost actual={0};CHECK(!place_case(program,answer,ordinal,NULL,&actual));
        CHECK(actual.status==XR_XIR_CALL_OOM && actual.sites>ordinal);
    }
    PlaceRuntimeCost retry={0};CHECK(place_case(program,answer,SIZE_MAX,NULL,&retry));
    CHECK(retry.sites==sites);
    printf("INIT_PLACES_FI mode=%u sites=%zu all=OOM retry=%lld dual_physical_baselines=1\n",
        mode,sites,(long long)XR_LANGUAGE_RUNTIME_EXPECTED);
}
static XrXirInstance *place_open(XrXirProgram *program) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);return instance;
}
static void place_cancel_step(XrXirInstance *instance,XrXirInstanceResult *observed,unsigned *resumed) {
    if (observed->outcome.status==XR_XIR_CALL_SUSPENDED) {
        CHECK((*resumed)++<XR_LANGUAGE_RUNTIME_SUSPENSIONS);
        CHECK(xr_xir_instance_resume(instance,observed->epoch,observed->outcome.wake)==XR_XIR_CALL_READY);
    } else CHECK(observed->outcome.status==XR_XIR_CALL_READY);
    *observed=xr_xir_instance_poll_bounded(instance,1);
}
static void place_cancel(XrXirProgram *program,uint32_t answer,unsigned mode) {
    size_t blocks=runtime_live,bytes=runtime_bytes,compiler_blocks=instance_compile_live,compiler_bytes=instance_compile_bytes;
    XrXirInstance *first=place_open(program);CHECK(xr_xir_instance_start(first,answer,NULL,0)==XR_XIR_CALL_READY);
    size_t ticks=0;unsigned resumed=0;
    XrXirInstanceResult observed={0};observed.outcome.status=XR_XIR_CALL_READY;
    while (observed.outcome.status==XR_XIR_CALL_READY || observed.outcome.status==XR_XIR_CALL_SUSPENDED) {
        CHECK(++ticks<4096);place_cancel_step(first,&observed,&resumed);
    }
    CHECK(observed.outcome.status==XR_XIR_CALL_RETURNED && resumed==XR_LANGUAGE_RUNTIME_SUSPENSIONS);
    XrXirValue result={0};
    CHECK(xr_xir_instance_take_result(first,&result)==XR_XIR_CALL_RETURNED && result.type==XR_XIR_I64 && result.payload==XR_LANGUAGE_RUNTIME_EXPECTED);
    xr_xir_value_drop(&result);CHECK(xr_xir_instance_free(first)==XR_XIR_CALL_READY);
    size_t initialization=0,retries=0,suspended=0;
    for (size_t prefix=0;prefix<ticks;++prefix) {
        first=place_open(program);XrXirInstance *second=place_open(program);
        CHECK(xr_xir_instance_start(first,answer,NULL,0)==XR_XIR_CALL_READY);
        observed=(XrXirInstanceResult){0};observed.outcome.status=XR_XIR_CALL_READY;resumed=0;
        for (size_t i=0;i<prefix;++i) {
            place_cancel_step(first,&observed,&resumed);
            CHECK(observed.outcome.status==XR_XIR_CALL_READY || observed.outcome.status==XR_XIR_CALL_SUSPENDED);
        }
        bool initializing=xr_xir_instance_state(first)==XR_XIR_INSTANCE_INITIALIZING;
        CHECK(initializing || xr_xir_instance_state(first)==XR_XIR_INSTANCE_READY);
        suspended+=observed.outcome.status==XR_XIR_CALL_SUSPENDED;
        runtime_attempts=0;CHECK(xr_xir_instance_cancel_current(first)==XR_XIR_CALL_CANCEL_REQUESTED);
        CHECK(xr_xir_instance_poll_bounded(first,UINT64_MAX).outcome.status==XR_XIR_CALL_CANCELLED && !runtime_attempts);
        XrXirValue empty={0};result=empty;
        CHECK(xr_xir_instance_take_result(first,&result)==XR_XIR_CALL_BAD_STATE && !memcmp(&result,&empty,sizeof(result)));
        CHECK(place_answer(second,answer)==XR_XIR_CALL_RETURNED);
        if (initializing) {
            ++initialization;CHECK(xr_xir_instance_state(first)==XR_XIR_INSTANCE_FAILED);
            XrXirCallResult failure={0};runtime_attempts=0;
            CHECK(xr_xir_instance_copy_failure(first,&failure)==XR_XIR_CALL_CANCELLED);
            CHECK(failure.status==XR_XIR_CALL_CANCELLED);xr_xir_call_result_drop(&failure);
            CHECK(place_answer(first,answer)==XR_XIR_CALL_CANCELLED && !runtime_attempts);
        } else {
            ++retries;CHECK(xr_xir_instance_state(first)==XR_XIR_INSTANCE_READY);
            CHECK(place_answer(first,answer)==XR_XIR_CALL_RETURNED);
        }
        CHECK(xr_xir_instance_free(first)==XR_XIR_CALL_READY && xr_xir_instance_free(second)==XR_XIR_CALL_READY);
        CHECK(runtime_live==blocks && runtime_bytes==bytes);
        CHECK(instance_compile_live==compiler_blocks && instance_compile_bytes==compiler_bytes);
    }
    CHECK(initialization && retries && initialization+retries==ticks && suspended==XR_LANGUAGE_RUNTIME_SUSPENSIONS);
    printf("INIT_PLACES_CANCEL mode=%u prefixes=%zu init_sticky=%zu ready_retry=%zu suspended=%zu cancel_allocations=0 same_instance_retry=%lld dual_physical_baselines=1\n",
        mode,ticks,initialization,retries,suspended,(long long)XR_LANGUAGE_RUNTIME_EXPECTED);
}
static void place_axis_limit(XrXirInstanceConfig *config,unsigned axis,uint64_t cap) {
    switch(axis) {
    case 0:config->requested_value_limit=cap;break;
    case 1:config->requested_call_limit=cap;break;
    case 2:config->work_limit=cap;break;
    case 3:config->metadata_limit=cap;break;
    case 4:config->call_limit=cap;break;
    default:CHECK(axis==5);config->value_limit=cap;break;
    }
}
static void place_axes(XrXirProgram *program,uint32_t answer,unsigned mode) {
    XrXirInstanceConfig original;CHECK(xr_xir_instance_config_init(&original,sizeof(original))==XR_XIR_CALL_READY);
    uint64_t caps[]={original.requested_value_limit,original.requested_call_limit,original.work_limit,
        original.metadata_limit,original.call_limit,original.value_limit};
    for (unsigned axis=0;axis<6;++axis) {
        uint64_t low=0,high=caps[axis];unsigned probes=0;
        while (high-low>1) {
            CHECK(++probes<=64);XrXirInstanceConfig config=original;uint64_t cap=low+(high-low)/2;
            place_axis_limit(&config,axis,cap);PlaceRuntimeCost actual={0};
            if (place_case(program,answer,SIZE_MAX,&config,&actual)) high=cap;
            else { CHECK(actual.status==XR_XIR_CALL_LIMIT);low=cap; }
        }
        CHECK(high>1);
        for (unsigned minus=0;minus<2;++minus) {
            XrXirInstanceConfig config=original;place_axis_limit(&config,axis,high-minus);PlaceRuntimeCost actual={0};
            CHECK(place_case(program,answer,SIZE_MAX,&config,&actual)==!minus);
            CHECK(actual.status==(minus?XR_XIR_CALL_LIMIT:XR_XIR_CALL_RETURNED));
        }
        printf("INIT_PLACES_AXIS mode=%u axis=%u exact=%llu minus1=LIMIT probes=%u original_cap=%llu dual_physical_baselines=1\n",
            mode,axis,(unsigned long long)high,probes,(unsigned long long)caps[axis]);
    }
}
static bool place_vm_function(uint32_t function,uint32_t answer,unsigned mode) {
    return !mode || (mode>=2 && ((function==answer)==(mode==3)));
}
static void place_profile(unsigned mode,const char *operation) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrXirProgramSpec spec=language_reference_program;XrXirArtifact *checked=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(&owner.context,spec.proof.bytes,spec.proof.length,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_lower(checked,&spec.target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t answer=UINT32_MAX;
    for (uint32_t f=0;f<module->function_count;++f)
        if (module->functions[f].name_length==15 && !memcmp(module->functions[f].name,"referenceAnswer",15)) {
            CHECK(answer==UINT32_MAX);answer=f;
        }
    CHECK(answer!=UINT32_MAX && module->function_count==spec.entry_count);
    XrXirCallEntry *calls=NULL;XrXirVmBinding *bindings=NULL;XrXirProgram *program=NULL;
    if (!mode) CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    else {
        if (mode>=2) {
            CHECK(xr_compile_resources_calloc(owner.context.resources,spec.entry_count,sizeof(*calls),(void **)&calls)==XR_COMPILE_RESOURCE_OK);
            CHECK(xr_compile_resources_calloc(owner.context.resources,spec.entry_count,sizeof(*bindings),(void **)&bindings)==XR_COMPILE_RESOURCE_OK);
            uint32_t native=0,vm=0,crossing=0;
            for (uint32_t f=0;f<spec.entry_count;++f) {
                CHECK(xr_xir_compile_vm_bind(lowered,f,&bindings[f],&calls[f])==XR_XIR_OK);
                bool use_vm=place_vm_function(f,answer,mode);
                if (use_vm) { CHECK(calls[f].resume!=spec.entries[f].resume);++vm; }
                else { calls[f]=spec.entries[f];++native; }
                const XrXirFunction *body=&module->functions[f];
                for (uint32_t i=0;i<body->instruction_count;++i)
                    if (body->instructions[i].op==XR_XIR_CALL) {
                        CHECK((uint64_t)body->instructions[i].immediate<spec.entry_count);
                        crossing+=use_vm!=place_vm_function((uint32_t)body->instructions[i].immediate,answer,mode);
                    }
            }
            CHECK(native && vm && crossing==XR_LANGUAGE_RUNTIME_CROSSINGS);spec.entries=calls;
            spec.types=module->types;spec.declarations=module->declarations;
            printf("INIT_PLACES_MIXED mode=%u vm=%u native=%u direct_cross_edges=%u\n",mode,vm,native,crossing);
        }
        XrXirProgramProof proof=xr_xir_compile_program_proof(lowered);
        CHECK(proof.length==spec.proof.length && !memcmp(proof.identity,spec.proof.identity,32) && !memcmp(proof.bytes,spec.proof.bytes,proof.length));
        spec.proof=proof;CHECK(xr_xir_compile_program_seal(&owner.context,&spec,&program)==XR_XIR_OK);
    }
    PlaceRuntimeCost baseline={0};CHECK(place_case(program,answer,SIZE_MAX,NULL,&baseline));
    CHECK(baseline.sites && baseline.sites<20000);
    printf("INIT_PLACES_BASELINE mode=%u sites=%zu answer=%lld dual_physical_baselines=1\n",
        mode,baseline.sites,(long long)XR_LANGUAGE_RUNTIME_EXPECTED);fflush(stdout);
    if (!strcmp(operation,"--fi")) place_faults(program,answer,mode,baseline.sites);
    else if (!strcmp(operation,"--cancel")) place_cancel(program,answer,mode);
    else if (!strcmp(operation,"--axes")) place_axes(program,answer,mode);
    else CHECK(!strcmp(operation,"--normal"));
    xr_xir_compile_program_drop(program);xr_xir_compile_artifact_free(lowered);
    xr_compile_resources_free(calls);xr_compile_resources_free(bindings);
    source_fixture_owner_free(&owner);instance_compile_report();CHECK(!runtime_live && !runtime_bytes);
}
int main(int argc,char **argv) {
    CHECK(argc==3);char *end=NULL;unsigned long mode=strtoul(argv[1],&end,10);
    CHECK(end!=argv[1] && !*end && mode<4);place_profile((unsigned)mode,argv[2]);return 0;
}
