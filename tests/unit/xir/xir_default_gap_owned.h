/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_default_gap_owned.h - Current input recovery and independent owner lifetime
 */
#ifndef XIR_DEFAULT_GAP_OWNED_H
#define XIR_DEFAULT_GAP_OWNED_H
#if DEFAULT_MODE==3
#include "xir_mixed_binding_projection.h"
static bool default_owned_recovery;
static unsigned default_binding_attack,default_binding_callbacks;
static XrXirVmBinding *default_attack_binding;
static void default_gap_before_allocation(size_t bytes) {
    (void)bytes;
    if(!default_attack_binding)return;
    ++default_binding_callbacks;
    if(default_binding_attack==1)default_attack_binding->artifact=(const XrXirArtifact *)(uintptr_t)1;
    else default_attack_binding->function=1;
}

static XrXirStatus default_owned_retry(const XrXirCompileContext *context,
    XrXirProgramSpec *spec,DefaultMixedOwner *owner,XrXirProgramInput **input,XrXirProgram **output) {
    unsigned char saved_owner[sizeof(*owner)];
    unsigned char saved_entries[64*sizeof(*owner->entries)];
    unsigned char saved_bindings[64*sizeof(*owner->bindings)];
    CHECK(spec->entry_count<=64 && *input && !*output);
    size_t entries=spec->entry_count*sizeof(*owner->entries),bindings=spec->entry_count*sizeof(*owner->bindings);
    memcpy(saved_owner,owner,sizeof(*owner));memcpy(saved_entries,owner->entries,entries);
    memcpy(saved_bindings,owner->bindings,bindings);
    XrXirProgramInput *retained=*input;XrXirArtifact *artifact=retained->artifact;
    XrCompileResourceStats before=default_stats(context);unsigned releases=default_mixed_releases;
    uint32_t abi=spec->abi_version;CHECK(abi==XR_XIR_PROGRAM_ABI_VERSION && artifact);
    spec->abi_version=0;
    CHECK(xir_program_receive(context,spec,input,NULL,output)==XR_XIR_BAD_LAYOUT);
    CHECK(*input==retained && retained->artifact==artifact && !*output && releases==default_mixed_releases);
    CHECK(!memcmp(saved_owner,owner,sizeof(*owner)) && !memcmp(saved_entries,owner->entries,entries));
    CHECK(!memcmp(saved_bindings,owner->bindings,bindings));
    XrCompileResourceStats after=default_stats(context);
    CHECK(after.live_bytes==before.live_bytes && after.work>=before.work);
    spec->abi_version=abi;
    return xir_program_receive(context,spec,input,NULL,output);
}

static void gap_owned_lifetime(XrXirCompileContext *context,XrXirProgram **program,
    const uint32_t ids[3],XrXirValue held[2]) {
    XrXirInstance *instances[2]={NULL,NULL};
    unsigned releases=default_mixed_releases;
    for(unsigned n=0;n<2;++n){
        XrXirInstanceConfig config;
        CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_new(*program,&config,&instances[n])==XR_XIR_CALL_READY);
    }
    XrCompileResources *producer=context->resources;context->resources=NULL;
    xr_compile_resources_release(producer);
    xr_xir_compile_program_drop(*program);*program=NULL;
    CHECK(default_mixed_releases==releases);
    for(unsigned n=0;n<2;++n){
        for(uint32_t e=0;e<3;++e){
            CHECK(xr_xir_instance_start(instances[n],ids[e],NULL,0)==XR_XIR_CALL_READY);
            XrXirInstanceResult run=xr_xir_instance_poll_bounded(instances[n],UINT64_MAX);
            uint32_t yields=0;
            while(run.outcome.status==XR_XIR_CALL_SUSPENDED){
                CHECK(e==2 && ++yields==1);
                CHECK(xr_xir_instance_resume(instances[n],run.epoch,run.outcome.wake)==XR_XIR_CALL_READY);
                run=xr_xir_instance_poll_bounded(instances[n],UINT64_MAX);
            }
            CHECK(run.outcome.status==XR_XIR_CALL_RETURNED && yields==(e==2?1u:0u));
            XrXirValue value={0};
            CHECK(xr_xir_instance_take_result(instances[n],&value)==XR_XIR_CALL_RETURNED);
            if(e==1)held[n]=value;
            else{CHECK(value.type==XR_XIR_I64 && value.payload==(e==0?1234u:41u));xr_xir_value_drop(&value);}
        }
        CHECK(xr_xir_instance_free(instances[n])==XR_XIR_CALL_READY);
        CHECK(default_mixed_releases==releases+(n==1));
    }
    CHECK(!context->resources && !*program);
}
#endif
#endif // XIR_DEFAULT_GAP_OWNED_H
