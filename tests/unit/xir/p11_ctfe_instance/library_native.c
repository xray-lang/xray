/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * library_native.c - Real generated Library bodies cross providers
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"Library native/mixed %d: %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task_budget.c"
#include "xir/xxir_task.c"
XR_DATA const XrXirProgramSpec p11_library_native_program;
XR_DATA const uint32_t p11_library_callback_count;
XR_DATA const XrXirResumeEntry p11_library_resume_callbacks[];
XR_DATA const XrXirReleaseEntry p11_library_release_callbacks[];
typedef struct LibraryNativeOwner {
    XrXirArtifact *lowered;
    XrXirVmBinding *bindings;
    XrXirCallEntry *vm_entries,*entries;
    XrXirInstance *instances[2];
    uint32_t count,run;
    unsigned mode,releases;
    uint64_t steps[2][2],crossings[2];
} LibraryNativeOwner;
static LibraryNativeOwner *library_observed;
static bool library_native_function(const LibraryNativeOwner *owner,uint32_t function) {
    return !owner->mode || (function==owner->run)==(owner->mode==1);
}
static unsigned library_native_instance(const XrXirCallView *view) {
    CHECK(library_observed && view);
    for(unsigned i=0;i<2;++i) if(library_observed->instances[i]==view->instance) return i;
    CHECK(false);return 0;
}
static const XrXirCallEntry *library_native_delegate(const XrXirCallView *view,uint32_t function) {
    LibraryNativeOwner *owner=library_observed;
    CHECK(function<owner->count);
    const XrXirCallEntry *actual=library_native_function(owner,function)?
        &p11_library_native_program.entries[function]:&owner->vm_entries[function];
    CHECK(actual->resume && actual->release && view->environment==actual->environment);
    return actual;
}
XR_FUNC XrXirAction p11_library_observe_resume(XrXirCallView *view,uint32_t function) {
    unsigned instance=library_native_instance(view);
    CHECK(xr_xir_call_admission(view) && xr_xir_call_current_entry(view->activation)==function);
    const XrXirCallEntry *actual=library_native_delegate(view,function);
    bool native=library_native_function(library_observed,function);
    ++library_observed->steps[instance][native];
    XrXirAction result=actual->resume(view);
    if(result.kind==XR_XIR_ACTION_CALL) {
        CHECK(result.callee<library_observed->count);
        if(native!=library_native_function(library_observed,result.callee)) ++library_observed->crossings[instance];
    }
    return result;
}
XR_FUNC void p11_library_observe_release(XrXirCallView *view,XrXirCallStatus reason,uint32_t function) {
    (void)library_native_instance(view);CHECK(!xr_xir_call_admission(view));
    const XrXirCallEntry *actual=library_native_delegate(view,function);actual->release(view,reason);
}
static void library_native_code_release(void *opaque) {
    LibraryNativeOwner *owner=opaque;CHECK(owner==library_observed && !owner->releases);
    xr_compile_resources_free(owner->entries);owner->entries=NULL;
    xr_compile_resources_free(owner->bindings);owner->bindings=NULL;
    xr_compile_resources_free(owner->vm_entries);owner->vm_entries=NULL;
    xr_xir_compile_artifact_free(owner->lowered);owner->lowered=NULL;++owner->releases;
}
static XrXirProgram *library_native_prepare(LibraryNativeOwner *owner,const char *mode) {
    library_observed=owner;
    owner->mode=!strcmp(mode,"native")?0:!strcmp(mode,"native-vm")?1:2;
    CHECK(owner->mode!=2 || !strcmp(mode,"vm-native"));
    XrXirCompileContext context={0};XrCompileResourceLimits caps={UINT64_C(67108864),UINT64_C(8388608),UINT64_C(128000000)};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    const XrXirProgramSpec *native=&p11_library_native_program;
    XrXirArtifact *checked=NULL,*closed=NULL;
    CHECK(xr_xir_compile_checked_read(&context,native->proof.bytes,native->proof.length,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(closed,&packet,NULL)==XR_XIR_OK);
    CHECK(packet.length==native->proof.length && !memcmp(packet.bytes,native->proof.bytes,packet.length));
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_lower(closed,&native->target,&owner->lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    XrXirProgramProof proof=xr_xir_compile_program_proof(owner->lowered);
    CHECK(proof.length==native->proof.length && !memcmp(proof.identity,native->proof.identity,32) && !memcmp(proof.bytes,native->proof.bytes,proof.length));
    const XrXirModule *module=xr_xir_compile_artifact_module(owner->lowered);owner->count=module->function_count;owner->run=UINT32_MAX;
    CHECK(owner->count==native->entry_count && owner->count==p11_library_callback_count);
    for(uint32_t f=0;f<owner->count;++f) if(module->functions[f].name_length==3 && !memcmp(module->functions[f].name,"run",3)) {CHECK(owner->run==UINT32_MAX);owner->run=f;}
    CHECK(owner->run<owner->count && module->declarations->functions[owner->run].exported);
    CHECK(xr_xir_compile_vm_bind_table(owner->lowered,&owner->bindings,&owner->vm_entries)==XR_XIR_OK);
    CHECK(xr_compile_resources_calloc(context.resources,owner->count,sizeof(*owner->entries),(void **)&owner->entries)==XR_COMPILE_RESOURCE_OK);
    for(uint32_t f=0;f<owner->count;++f) {
        owner->entries[f]=library_native_function(owner,f)?native->entries[f]:owner->vm_entries[f];
        CHECK(owner->entries[f].resume && owner->entries[f].release);
        owner->entries[f].resume=p11_library_resume_callbacks[f];owner->entries[f].release=p11_library_release_callbacks[f];
    }
    XrXirProgramSpec spec=*native;spec.entries=owner->entries;spec.code=(XrXirCodeLease){owner,library_native_code_release};
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&spec,&program)==XR_XIR_OK);
    xr_compile_resources_release(context.resources);
    return program;
}
static void library_native_normal(LibraryNativeOwner *owner,XrXirProgram *program,const char *mode) {
    for(unsigned i=0;i<2;++i) {XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);CHECK(xr_xir_instance_new(program,&config,&owner->instances[i])==XR_XIR_CALL_READY);}
    xr_xir_compile_program_drop(program);
    for(unsigned i=0;i<2;++i) {
        CHECK(xr_xir_instance_start(owner->instances[i],owner->run,NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(owner->instances[i],UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(owner->instances[i],&value)==XR_XIR_CALL_RETURNED && value.type==XR_XIR_I64 && value.payload==42);xr_xir_value_drop(&value);
        CHECK(owner->steps[i][1] && (!owner->mode || (owner->steps[i][0] && owner->crossings[i]>=2)));
        CHECK(owner->mode || !owner->steps[i][0]);
        CHECK(xr_xir_instance_stop(owner->instances[i])==XR_XIR_CALL_READY && xr_xir_instance_free(owner->instances[i])==XR_XIR_CALL_READY);
    }
    if(runtime_live || runtime_bytes) runtime_report_residuals(stderr);
    instance_compile_report();
    CHECK(owner->releases==1 && !runtime_live && !runtime_bytes && !instance_compile_live && !instance_compile_bytes);
    fprintf(stderr,"Library %s authentic generated C and complete proof, two Instances42 real crossings%llu/%llu physical0\n",mode,(unsigned long long)owner->crossings[0],(unsigned long long)owner->crossings[1]);
}
#include "library_runtime.h"
int main(int argc,char **argv) {
    CHECK(argc==2 || argc==3);LibraryNativeOwner owner={0};
    XrXirProgram *program=library_native_prepare(&owner,argv[1]);
    if(argc==2) library_native_normal(&owner,program,argv[1]);
    else library_native_runtime_resources(&owner,program,argv[1],argv[2]);
    return 0;
}
