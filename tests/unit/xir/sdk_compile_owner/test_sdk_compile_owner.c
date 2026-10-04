/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_sdk_compile_owner.c - One compiler ledger across admitted SDK and native program
 */
#include "execution/xr_xir_host_execution.h"
#include "toolchain/xr_xir_runtime_sdk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "../xir_sdk_resource_test.h"
extern const XrXirProgramSpec compile_owner_program;
extern const XrXirProgramSpec *old_program_data(void);
extern unsigned old_callback_count(void);
extern unsigned old_release_count(void);
typedef XrXirStatus (*SealPrototype)(const XrXirCompileContext *,const XrXirProgramSpec *,XrXirProgram **);
typedef void (*DropPrototype)(XrXirProgram *);
typedef XrXirCompileLimits (*LimitsPrototype)(void);
_Static_assert(_Generic(&xr_xir_compile_program_seal,SealPrototype:1,default:0),"Mandatory compile context");
_Static_assert(_Generic(&xr_xir_compile_program_drop,DropPrototype:1,default:0),"Owned program destructor");
_Static_assert(_Generic(&xr_xir_compile_default_limits,LimitsPrototype:1,default:0),"Pure structural defaults");
static DWORD handles(void) { DWORD count=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&count));return count; }
static char *input(const char *path,size_t *length) {
    FILE *file=fopen(path,"rb");CHECK(file && !fseek(file,0,SEEK_END));long bytes=ftell(file);
    CHECK(bytes>0 && bytes<2097152 && !fseek(file,0,SEEK_SET));
    char *result=xr_malloc((size_t)bytes);CHECK(result);
    CHECK(fread(result,1,(size_t)bytes,file)==(size_t)bytes && !fclose(file));*length=(size_t)bytes;return result;
}
static XrXirRuntimeSdk *admit(const XrXirRuntimeSdkRequest *request) {
    XrXirRuntimeSdk *sdk=NULL;CHECK(xr_xir_runtime_sdk_load(request,&sdk)==XR_XIR_SDK_OK);
    const XrXirRuntimeSdkFacts *facts=xr_xir_runtime_sdk_facts(sdk);
    CHECK(facts->value_abi==19 && facts->call_abi==25 && facts->program_abi==28 && facts->crt==2);
    const char *resource_library=NULL;
    CHECK(xr_xir_runtime_sdk_file(sdk,"lib/xray_compile_resources.lib",&resource_library)==XR_XIR_SDK_OK && resource_library);
    return sdk;
}
static XrXirCompileContext context(XrCompileResources *resources) {
    XrXirCompileContext result={resources,{0}};result.limits=xr_xir_compile_default_limits();return result;
}
typedef struct SdkAbiPhysicalSnapshot {
    size_t attempts,live,bytes;
    DWORD handle_count;
} SdkAbiPhysicalSnapshot;
static SdkAbiPhysicalSnapshot abi_physical(void) {
    return (SdkAbiPhysicalSnapshot){runtime_attempts,runtime_live,runtime_bytes,handles()};
}
static void abi_refusal_stats(XrCompileResources *resources,XrCompileResourceStats before,
    uint64_t expected_work,SdkAbiPhysicalSnapshot physical) {
    XrCompileResourceStats after=sdk_stats(resources);
    CHECK(after.allocation_count==before.allocation_count && after.allocated_bytes==before.allocated_bytes);
    CHECK(after.live_bytes==before.live_bytes && after.peak_bytes==before.peak_bytes);
    CHECK(before.work<=UINT64_MAX-expected_work && after.work==before.work+expected_work);
    CHECK(runtime_attempts==physical.attempts && runtime_live==physical.live &&
        runtime_bytes==physical.bytes && handles()==physical.handle_count);
}
static void old_versions(const XrXirRuntimeSdkRequest *original) {
    XrXirRuntimeSdkRequest request=*original;request.resources=sdk_ledger(&sdk_unlimited);
    XrXirRuntimeSdk *sdk=admit(&request);XrXirCompileContext compile=context(request.resources);
    CHECK(old_program_data()->abi_version==26 && old_program_data()->target.abi_version==17);
    CHECK(old_program_data()->entries[0].abi_version==21);
    for (unsigned version=0;version<3;++version) {
        XrXirProgramSpec spec=*old_program_data();
        if (version>0) spec.abi_version=28;
        if (version>1) spec.target.abi_version=19;
        XrXirProgram *program=NULL;XrCompileResourceStats before=sdk_stats(request.resources);
        SdkAbiPhysicalSnapshot physical=abi_physical();
        CHECK(xr_xir_compile_program_seal(&compile,&spec,&program)==XR_XIR_BAD_LAYOUT && !program);
        /* Program/Value prefixes charge no work; the first entry ABI read charges one. */
        abi_refusal_stats(request.resources,before,version==2 ? 1u : 0u,physical);
        CHECK(!old_callback_count() && !old_release_count());
    }
    xr_xir_runtime_sdk_free(sdk);xr_compile_resources_release(request.resources);
    CHECK(!runtime_live && !runtime_bytes);
    puts("Program26, Value17 and Call21 independently reject before poison metadata, allocation, lease and callback");
}
typedef struct SdkAbiObservation {
    const XrXirCallEntry *entry;
    unsigned resumes,releases,leases;
} SdkAbiObservation;
static SdkAbiObservation sdk_abi_observation;
static XrXirAction abi_resume(XrXirCallView *view) {
    CHECK(sdk_abi_observation.entry && sdk_abi_observation.entry->resume);
    ++sdk_abi_observation.resumes;return sdk_abi_observation.entry->resume(view);
}
static void abi_release(XrXirCallView *view,XrXirCallStatus reason) {
    CHECK(sdk_abi_observation.entry && sdk_abi_observation.entry->release);
    ++sdk_abi_observation.releases;sdk_abi_observation.entry->release(view,reason);
}
static void abi_code_release(void *owner) {
    CHECK(owner==&sdk_abi_observation);++sdk_abi_observation.leases;
}
static void current_entry_version(const XrXirRuntimeSdkRequest *original) {
    DWORD initial=handles();XrXirRuntimeSdkRequest request=*original;request.resources=sdk_ledger(&sdk_unlimited);
    XrXirRuntimeSdk *sdk=admit(&request);XrXirCompileContext compile=context(request.resources);
    CHECK(compile_owner_program.abi_version==28 && compile_owner_program.target.abi_version==19);
    CHECK(compile_owner_program.entry_count==3 && compile_owner_program.entries);
    CHECK(!compile_owner_program.code.owner && !compile_owner_program.code.release);
    CHECK(compile_owner_program.entries[1].result==XR_XIR_I64 && !compile_owner_program.entries[1].parameter_count);
    uint64_t copy_bytes=(uint64_t)compile_owner_program.entry_count*sizeof(XrXirCallEntry);
    CHECK(copy_bytes<=SIZE_MAX);void *memory=NULL;
    CHECK(xr_compile_resources_alloc(request.resources,(size_t)copy_bytes,&memory)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_work(request.resources,copy_bytes)==XR_COMPILE_RESOURCE_OK);
    memcpy(memory,compile_owner_program.entries,(size_t)copy_bytes);XrXirCallEntry *entries=memory;
    for(uint32_t i=0;i<compile_owner_program.entry_count;++i)CHECK(entries[i].abi_version==25);
    sdk_abi_observation=(SdkAbiObservation){compile_owner_program.entries+1,0,0,0};
    entries[1].resume=abi_resume;entries[1].release=abi_release;
    XrXirProgramSpec spec=compile_owner_program;spec.entries=entries;
    spec.code=(XrXirCodeLease){&sdk_abi_observation,abi_code_release};
    /* Only entry zero's Call ABI differs from the complete current positive control. */
    entries[0].abi_version=24;
    XrXirProgram *program=NULL;XrCompileResourceStats before=sdk_stats(request.resources);
    SdkAbiPhysicalSnapshot physical=abi_physical();
    CHECK(xr_xir_compile_program_seal(&compile,&spec,&program)==XR_XIR_BAD_LAYOUT && !program);
    abi_refusal_stats(request.resources,before,1,physical);
    CHECK(!sdk_abi_observation.resumes && !sdk_abi_observation.releases && !sdk_abi_observation.leases);
    CHECK(!old_callback_count() && !old_release_count());
    XrXirCallConfig standalone;CHECK(xr_xir_call_config_init(&standalone,sizeof(standalone))==XR_XIR_CALL_READY);
    CHECK(standalone.abi_version==25 && standalone.struct_size==sizeof(standalone));
    XrXirCallAccounting accounting={0};standalone.accounting=&accounting;standalone.entries=entries;
    standalone.entry_count=spec.entry_count;standalone.byte_limit=1048576;standalone.poll_limit=1000000;standalone.depth_limit=64;
    XrXirCall *call=(XrXirCall *)(uintptr_t)1;before=sdk_stats(request.resources);
    physical=abi_physical();
    CHECK(xr_xir_call_new(&standalone,1,NULL,0,&call)==XR_XIR_CALL_BAD_ABI && !call);
    abi_refusal_stats(request.resources,before,0,physical);
    CHECK(!accounting.allocations && !accounting.frees && !accounting.live_bytes && !accounting.peak_bytes);
    CHECK(!accounting.polls && !accounting.depth && !accounting.peak_depth);
    CHECK(!sdk_abi_observation.resumes && !sdk_abi_observation.releases && !sdk_abi_observation.leases);
    entries[0].abi_version=25;
    CHECK(xr_xir_compile_program_seal(&compile,&spec,&program)==XR_XIR_OK && program);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirHostExecutionRequest execution={program,&config,1,NULL,0};XrXirCallResult result={0};
    CHECK(xr_xir_host_execute(&execution,&result)==XR_XIR_CALL_RETURNED);
    CHECK(result.value.type==XR_XIR_I64 && !result.value.reserved && result.value.payload==42);
    xr_xir_call_result_drop(&result);
    CHECK(sdk_abi_observation.resumes && sdk_abi_observation.releases && !sdk_abi_observation.leases);
    entries[0].abi_version=24;XrXirProgram *occupied=program;
    unsigned resumes=sdk_abi_observation.resumes,releases=sdk_abi_observation.releases;
    before=sdk_stats(request.resources);physical=abi_physical();
    CHECK(xr_xir_compile_program_seal(&compile,&spec,&occupied)==XR_XIR_BAD_STRUCTURE && occupied==program);
    abi_refusal_stats(request.resources,before,0,physical);
    CHECK(sdk_abi_observation.resumes==resumes && sdk_abi_observation.releases==releases && !sdk_abi_observation.leases);
    xr_xir_compile_program_drop(program);CHECK(sdk_abi_observation.leases==1);
    CHECK(sdk_abi_observation.resumes==resumes && sdk_abi_observation.releases==releases);
    xr_compile_resources_free(entries);sdk_abi_observation=(SdkAbiObservation){0};
    xr_xir_runtime_sdk_free(sdk);xr_compile_resources_release(request.resources);
    CHECK(!runtime_live && !runtime_bytes && handles()==initial);
    puts("SDK current Program28/Value19 independently reject entry Call24; standalone Call25 rejects Call24; real42/lease controls PASS");
}
static void positive(const XrXirRuntimeSdkRequest *original) {
    DWORD initial=handles();XrXirRuntimeSdkRequest request=*original;request.resources=sdk_ledger(&sdk_unlimited);
    XrXirRuntimeSdk *sdk=admit(&request);XrXirCompileContext compile=context(request.resources);
    XrCompileResourceStats before=sdk_stats(request.resources);XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&compile,&compile_owner_program,&program)==XR_XIR_OK);
    XrCompileResourceStats after=sdk_stats(request.resources);
    CHECK(after.live_bytes>before.live_bytes && after.allocated_bytes>before.allocated_bytes && after.work>before.work);
    CHECK(after.live_bytes==runtime_bytes);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirHostExecutionRequest call={program,&config,1,NULL,0};XrXirCallResult number={0},text={0};
    CHECK(xr_xir_host_execute(&call,&number)==XR_XIR_CALL_RETURNED && number.value.type==XR_XIR_I64 && number.value.payload==42);
    call.entry=2;CHECK(xr_xir_host_execute(&call,&text)==XR_XIR_CALL_RETURNED);
    xr_compile_resources_release(request.resources);xr_xir_compile_program_drop(program);
    before=sdk_stats(request.resources);
    CHECK(!xr_xir_runtime_sdk_resources(NULL) && xr_xir_runtime_sdk_resources(sdk)==request.resources);
    after=sdk_stats(xr_xir_runtime_sdk_resources(sdk));
    CHECK(before.allocation_count==after.allocation_count && before.allocated_bytes==after.allocated_bytes &&
        before.live_bytes==after.live_bytes && before.peak_bytes==after.peak_bytes && before.work==after.work);
    xr_xir_runtime_sdk_free(sdk);
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(&text.value,&bytes,&length) && length==5 && !memcmp(bytes,"A\0\xe4\xb8\xad",5));
    xr_xir_call_result_drop(&number);xr_xir_call_result_drop(&text);
    CHECK(!runtime_live && !runtime_bytes && handles()==initial);
}
static void failure_matrix(const XrXirRuntimeSdkRequest *original) {
    DWORD initial=handles();XrXirRuntimeSdkRequest request=*original;request.resources=sdk_ledger(&sdk_unlimited);
    XrXirRuntimeSdk *sdk=admit(&request);XrXirCompileContext compile=context(request.resources);XrXirProgram *program=NULL;
    size_t first=runtime_attempts;CHECK(xr_xir_compile_program_seal(&compile,&compile_owner_program,&program)==XR_XIR_OK);
    size_t points=runtime_attempts-first;XrCompileResourceStats exact=sdk_stats(request.resources);
    xr_xir_compile_program_drop(program);xr_xir_runtime_sdk_free(sdk);xr_compile_resources_release(request.resources);
    CHECK(!runtime_live && !runtime_bytes && handles()==initial);
    for (unsigned pass=0;pass<4;++pass) {
        XrCompileResourceLimits limits={exact.allocated_bytes-(pass==1),exact.peak_bytes-(pass==2),exact.work-(pass==3)};
        request.resources=sdk_ledger(&limits);sdk=admit(&request);compile=context(request.resources);program=NULL;
        CHECK(xr_xir_compile_program_seal(&compile,&compile_owner_program,&program)==(pass ? XR_XIR_BUDGET : XR_XIR_OK));
        CHECK(pass ? !program : program!=NULL);xr_xir_compile_program_drop(program);xr_xir_runtime_sdk_free(sdk);
        xr_compile_resources_release(request.resources);CHECK(!runtime_live && !runtime_bytes && handles()==initial);
    }
    for (size_t at=0;at<points;++at) {
        request.resources=sdk_ledger(&sdk_unlimited);sdk=admit(&request);compile=context(request.resources);program=NULL;
        XrCompileResourceStats before=sdk_stats(request.resources);size_t physical=runtime_bytes;DWORD sdk_handles=handles();
        runtime_fail_at=runtime_attempts+at;
        CHECK(xr_xir_compile_program_seal(&compile,&compile_owner_program,&program)==XR_XIR_OUT_OF_MEMORY && !program);
        runtime_fail_at=SIZE_MAX;
        CHECK(sdk_stats(request.resources).live_bytes==before.live_bytes && runtime_bytes==physical && handles()==sdk_handles);
        CHECK(xr_xir_runtime_sdk_facts(sdk)->program_abi==28);
        xr_xir_runtime_sdk_free(sdk);xr_compile_resources_release(request.resources);
        CHECK(!runtime_live && !runtime_bytes && handles()==initial);
    }
    printf("Admitted SDK + Program same ledger: exact/minus1 and %zu actual malloc OOM points; physical zero\n",points);
}
int main(int argc,char **argv) {
    CHECK(argc==3);size_t length=0;char *manifest=input(argv[2],&length);
    XrXirRuntimeSdkRequest request={argv[1],manifest,length,NULL};old_versions(&request);current_entry_version(&request);positive(&request);failure_matrix(&request);
    xr_free(manifest);puts("SDK native 42 and owned embedded-NUL UTF-8 remain correct after producer destruction");return 0;
}
