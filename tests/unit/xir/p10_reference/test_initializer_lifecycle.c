/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_initializer_lifecycle.c - Owned constructor fields on failure and cancellation
 *
 * KEY CONCEPT:
 *   Unpublished constructor locals retire their real class and string owners.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_class_observation.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "../xir_source_fixture_owner.h"
#include "../xir_instance_compile_observer.h"
#include "../xir_runtime_allocations.h"

typedef struct InitializerEntries { uint32_t answer, failure, suspended, partial, payload, box; } InitializerEntries;
#if XR_INIT_NATIVE
XR_DATA const XrXirProgramSpec language_reference_program;
#endif

static uint32_t named_function(const XrXirModule *module,const char *name) {
    uint32_t index=UINT32_MAX;
    for (uint32_t f=0;f<module->function_count;++f)
        if (module->functions[f].name_length==strlen(name) && !memcmp(module->functions[f].name,name,strlen(name))) {
            CHECK(index==UINT32_MAX);index=f;
        }
    CHECK(index!=UINT32_MAX);return index;
}
static uint32_t named_class(const XrXirModule *module,const char *name) {
    const XrXirNominalTable *table=module->types->nominals;uint32_t index=UINT32_MAX;
    CHECK(table && (table->declarations || table->identities));
    for (uint32_t d=0;d<table->count;++d) {
        XrXirLiteral actual=table->declarations?table->declarations[d].name:table->identities[d].name;
        if (actual.length==strlen(name) && !memcmp(actual.bytes,name,strlen(name))) {
            CHECK(index==UINT32_MAX);index=d;
        }
    }
    CHECK(index!=UINT32_MAX);return index;
}
static InitializerEntries initializer_entries(const XrXirModule *module) {
    return (InitializerEntries){named_function(module,"referenceAnswer"),named_function(module,"referenceFailure"),
        named_function(module,"referenceSuspended"),named_function(module,"referencePartial"),
        named_class(module,"Payload"),named_class(module,"Box")};
}
#if !XR_INIT_NATIVE
static XrXirProgram *compile_fixture(SourceFixtureOwner *owner,InitializerEntries *entries) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(owner->context.resources,&session)==XR_COMPILER_SESSION_OK);
    const XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_LANGUAGE_FIXTURES};
    const XrXirSourceRequest request={session,XR_LANGUAGE_FIXTURES "/init_owned_fields.xr",&authority,&owner->context,
        XR_LANGUAGE_STDLIB,NULL,XR_XIR_PROGRAM,NULL,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&source,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"source=%u line=%d %s\n",(unsigned)status,diagnostic.line,diagnostic.message);
    CHECK(status==XR_XIR_OK);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);xr_compile_session_free(session);
    XrXirArtifact *decoded=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(&owner->context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(decoded,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(decoded);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    *entries=initializer_entries(module);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    return program;
}
#endif
static void lifecycle_events(XrXirClassObservation *observation,const InitializerEntries *entries,bool box,uint64_t *cursor,uint64_t *serial) {
    XrXirClassEvent events[12];uint32_t count=0;bool overflow=false;
    CHECK(xr_xir_class_observation_drain(observation,cursor,events,12,&count,&overflow)==XR_XIR_VALUE_OK);
    CHECK(!overflow && count==(box?6u:3u));
    uint32_t payload_kinds=0,box_kinds=0;uint64_t payload_serial=0,box_serial=0;
    for (uint32_t i=0;i<count;++i) {
        const XrXirClassEvent *event=&events[i];
        CHECK(event->kind>=XR_XIR_CLASS_CONSTRUCT && event->kind<=XR_XIR_CLASS_RECLAIM);
        CHECK(!event->flags && !event->initialized_fields && event->status==XR_XIR_VALUE_OK);
        uint32_t bit=1u<<event->kind;
        if (event->declaration==entries->payload) {
            CHECK(!(payload_kinds&bit));payload_kinds|=bit;
            if (!payload_serial) payload_serial=event->serial;
            CHECK(event->serial==payload_serial);
        } else {
            CHECK(box && event->declaration==entries->box && !(box_kinds&bit));box_kinds|=bit;
            if (!box_serial) box_serial=event->serial;
            CHECK(event->serial==box_serial && box_serial!=payload_serial);
        }
    }
    CHECK(payload_kinds==14 && box_kinds==(box?14u:0u));
    CHECK(payload_serial==++*serial);
    if (box) CHECK(box_serial==++*serial);
}
static void lifecycle_case(XrXirProgram *program,const InitializerEntries *entries,unsigned mode) {
    size_t blocks=runtime_live,bytes=runtime_bytes;
    size_t compiler_blocks=instance_compile_live,compiler_bytes=instance_compile_bytes;
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    XrXirClassObservation *observation=NULL;
    CHECK(xr_xir_instance_class_observation_open(instance,12,&observation)==XR_XIR_CALL_READY);
    bool failure=mode==1 || mode==4,cancel=mode==3 || mode==5;
    uint32_t entry=mode==0?entries->answer:mode==1?entries->failure:mode==4?entries->partial:entries->suspended;
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
    uint64_t cancelled_epoch=0,cancelled_wake=0;
    if (!failure && mode) {
        CHECK(result.outcome.status==XR_XIR_CALL_SUSPENDED);
        if (mode==2 || mode==5) {
            CHECK(xr_xir_instance_resume(instance,result.epoch,result.outcome.wake)==XR_XIR_CALL_READY);
            result=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
            CHECK(result.outcome.status==XR_XIR_CALL_SUSPENDED);
        }
        if (cancel) {
            cancelled_epoch=result.epoch;cancelled_wake=result.outcome.wake;
            CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
        }
        else CHECK(xr_xir_instance_resume(instance,result.epoch,result.outcome.wake)==XR_XIR_CALL_READY);
        result=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
    }
    XrXirCallStatus expected=failure?XR_XIR_CALL_ASSERTION:cancel?XR_XIR_CALL_CANCELLED:XR_XIR_CALL_RETURNED;
    if (result.outcome.status!=expected) fprintf(stderr,"mode=%u status=%u expected=%u\n",mode,(unsigned)result.outcome.status,(unsigned)expected);
    CHECK(result.outcome.status==expected);
    if (cancel) CHECK(xr_xir_instance_resume(instance,cancelled_epoch,cancelled_wake)==XR_XIR_CALL_BAD_STATE);
    const char *message=mode==4?"init-partial-failure":"init-owned-failure";
    if (failure) {
        const char *text=NULL;size_t length=0;
        CHECK(xr_xir_string_view(&result.outcome.panic.message,&text,&length) && length==strlen(message) && !memcmp(text,message,length));
    }
    XrXirCallResult escaped={0};
    if (failure) CHECK(xr_xir_call_result_copy(&result.outcome,&escaped)==XR_XIR_VALUE_OK);
    XrXirValue value={0};
    if (expected==XR_XIR_CALL_RETURNED) {
        CHECK(xr_xir_instance_take_result(instance,&value)==expected);
        CHECK(value.type==XR_XIR_I64 && value.payload==59);xr_xir_value_drop(&value);
    } else {
        CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_BAD_STATE);
        const XrXirValue empty={0};CHECK(!memcmp(&value,&empty,sizeof(value)));
    }
    CHECK(xr_xir_instance_state(instance)==XR_XIR_INSTANCE_READY);
    uint64_t cursor=0,serial=0;lifecycle_events(observation,entries,mode==0,&cursor,&serial);
    if (failure || cancel) {
        CHECK(xr_xir_instance_start(instance,entries->answer,NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        CHECK(value.type==XR_XIR_I64 && value.payload==59);xr_xir_value_drop(&value);
        lifecycle_events(observation,entries,true,&cursor,&serial);
    }
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    if (failure) {
        const char *text=NULL;size_t length=0;
        CHECK(xr_xir_string_view(&escaped.panic.message,&text,&length) && length==strlen(message) && !memcmp(text,message,length));
        xr_xir_call_result_drop(&escaped);
    }
    xr_xir_class_observation_drop(&observation);
    CHECK(runtime_live==blocks && runtime_bytes==bytes);
    CHECK(instance_compile_live==compiler_blocks && instance_compile_bytes==compiler_bytes);
    printf("constructor lifecycle mode=%u: exact class retirements, both physical baselines restored\n",mode);
}
#include "initializer_runtime_resources.h"
#if XR_INIT_NATIVE
static bool initializer_root(uint32_t function,const InitializerEntries *entries) {
    return function==entries->answer || function==entries->failure || function==entries->suspended || function==entries->partial;
}
static void native_profile(bool direction) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrXirProgramSpec spec=language_reference_program;XrXirArtifact *checked=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(&owner.context,spec.proof.bytes,spec.proof.length,&checked,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(checked);
    InitializerEntries entries=initializer_entries(module);
    XrXirCallEntry *calls=NULL;XrXirVmBinding *bindings=NULL;
    if (XR_INIT_MIXED) {
        CHECK(xr_xir_compile_lower(checked,&spec.target,&lowered,NULL)==XR_XIR_OK);
        CHECK(xr_compile_resources_calloc(owner.context.resources,spec.entry_count,sizeof(*calls),(void **)&calls)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_resources_calloc(owner.context.resources,spec.entry_count,sizeof(*bindings),(void **)&bindings)==XR_COMPILE_RESOURCE_OK);
        module=xr_xir_compile_artifact_module(lowered);uint32_t native=0,vm=0,crossing=0;
        CHECK(module->function_count==spec.entry_count);
        for (uint32_t f=0;f<spec.entry_count;++f) {
            CHECK(xr_xir_compile_vm_bind(lowered,f,&bindings[f],&calls[f])==XR_XIR_OK);
            bool use_vm=initializer_root(f,&entries)?direction:!direction;
            if (use_vm) {CHECK(calls[f].resume!=spec.entries[f].resume);++vm;}
            else {calls[f]=spec.entries[f];++native;}
            const XrXirFunction *function=&module->functions[f];
            for (uint32_t i=0;i<function->instruction_count;++i) {
                const XrXirInstruction *instruction=&function->instructions[i];
                if (instruction->op==XR_XIR_CALL) {
                    CHECK(instruction->immediate>=0 && (uint64_t)instruction->immediate<spec.entry_count);
                    bool callee_vm=initializer_root((uint32_t)instruction->immediate,&entries)?direction:!direction;
                    crossing+=use_vm!=callee_vm;
                }
            }
        }
        CHECK(native && vm && crossing);
        spec.entries=calls;spec.types=module->types;spec.declarations=module->declarations;
        XrXirProgramProof proof=xr_xir_compile_program_proof(lowered);
        CHECK(proof.identity && proof.length==spec.proof.length && !memcmp(proof.identity,spec.proof.identity,32) &&
            !memcmp(proof.bytes,spec.proof.bytes,proof.length));spec.proof=proof;
        printf("constructor mixed direction=%u vm=%u native=%u direct_cross_edges=%u\n",(unsigned)direction,vm,native,crossing);
    }
    xr_xir_compile_artifact_free(checked);XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&owner.context,&spec,&program)==XR_XIR_OK);
    initializer_profile_cases(program,&entries);
    xr_xir_compile_program_drop(program);xr_xir_compile_artifact_free(lowered);
    xr_compile_resources_free(bindings);xr_compile_resources_free(calls);source_fixture_owner_free(&owner);
    instance_compile_report();CHECK(!runtime_live && !runtime_bytes);
}
#endif
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);
    if (argc==2) {
        if (!strcmp(argv[1],"--resource-baseline")) initializer_resource_mode=1;
        else if (!strcmp(argv[1],"--runtime-fi")) initializer_resource_mode=2;
        else if (!strcmp(argv[1],"--runtime-axes")) initializer_resource_mode=3;
        else if (!strcmp(argv[1],"--focused-fault")) initializer_resource_mode=4;
        else {CHECK(!strcmp(argv[1],"--focused-value-limit"));initializer_resource_mode=5;}
    }
#if XR_INIT_NATIVE
    native_profile(false);if (XR_INIT_MIXED) native_profile(true);
#else
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);InitializerEntries entries={0};
    XrXirProgram *program=compile_fixture(&owner,&entries);
    initializer_profile_cases(program,&entries);
    xr_xir_compile_program_drop(program);source_fixture_owner_free(&owner);instance_compile_report();
    CHECK(!runtime_live && !runtime_bytes);
#endif
    puts("constructor failure/cancel/recovery ownership passed");return 0;
}
