/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_instance_domain_fuel.c - Independent work and resume-count limits
 *
 * KEY CONCEPT:
 *   Real checked bodies supply Program permissions. Actual active callbacks
 *   spend the live Domain bank, while poll counts only bound resumes.
 */
#include "xir/xxir_program_packet_internal.h"
#include "base/xmalloc.h"
#include "xir/xxir_program.h"
#include "xir/xxir_value_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"DOMAIN_FUEL %d: %s\n",__LINE__,#c);exit(1); } } while (0)

typedef struct FuelAllocation { void *pointer; size_t bytes; } FuelAllocation;
typedef struct FuelPhysical { size_t blocks, bytes; } FuelPhysical;
static FuelAllocation fuel_blocks[4096];
static size_t fuel_live, fuel_bytes, fuel_attempts, fuel_fail_at=SIZE_MAX;
static bool fuel_injected;
static FuelPhysical fuel_mark(void) { return (FuelPhysical){fuel_live,fuel_bytes}; }
static void fuel_balanced(FuelPhysical before) {
    CHECK(fuel_live==before.blocks && fuel_bytes==before.bytes);
}
static void fuel_record(void *pointer, size_t bytes) {
    if (!pointer) return;
    CHECK(fuel_live<4096 && bytes<=SIZE_MAX-fuel_bytes);
    fuel_blocks[fuel_live++]=(FuelAllocation){pointer,bytes};fuel_bytes+=bytes;
}
static void fuel_forget(void *pointer) {
    if (!pointer) return;
    size_t at=0;while(at<fuel_live && fuel_blocks[at].pointer!=pointer) ++at;
    CHECK(at<fuel_live && fuel_blocks[at].bytes<=fuel_bytes);
    fuel_bytes-=fuel_blocks[at].bytes;fuel_blocks[at]=fuel_blocks[--fuel_live];
}
static bool fuel_fail(void) {
    if(fuel_attempts++!=fuel_fail_at) return false;
    fuel_injected=true;return true;
}
static void *fuel_malloc(size_t bytes) {
    if(fuel_fail()) return NULL;
    void *pointer=xr_malloc(bytes);fuel_record(pointer,bytes);return pointer;
}
static void *fuel_calloc(size_t count, size_t bytes) {
    CHECK(!bytes || count<=SIZE_MAX/bytes);
    if(fuel_fail()) return NULL;
    void *pointer=xr_calloc(count,bytes);fuel_record(pointer,count*bytes);return pointer;
}
static void *fuel_realloc(void *pointer, size_t bytes) {
    CHECK(bytes);
    if(fuel_fail()) return NULL;
    void *replacement=xr_realloc(pointer,bytes);
    if(replacement) { fuel_forget(pointer);fuel_record(replacement,bytes); }
    return replacement;
}
static void fuel_free(void *pointer) { fuel_forget(pointer);xr_free(pointer); }

/* All Instance and task storage uses the real Domain allocator; compiler
 * storage uses its real retained ledger. Neither observation changes a fee. */
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_calloc")
#pragma push_macro("xr_realloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_malloc(bytes) fuel_malloc(bytes)
#define xr_calloc(count,bytes) fuel_calloc(count,bytes)
#define xr_realloc(pointer,bytes) fuel_realloc(pointer,bytes)
#define xr_free(pointer) fuel_free(pointer)
#include "base/xcompile_resources.c"
#include "xir/xxir_value.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_realloc")
#pragma pop_macro("xr_calloc")
#pragma pop_macro("xr_malloc")
#include "xir_construction_fixture.h"

typedef struct FuelObservation {
    uint32_t mode, init_calls, main_calls, admissions, releases;
    uint64_t initial_bank, limit_bank;
    bool exhausted;
} FuelObservation;
static void fuel_code_release(void *owner) {
    FuelObservation *observation=owner;CHECK(observation && !observation->releases);
    ++observation->releases;
}
static void fuel_scalar(XrXirValueAdmission *admission, FuelObservation *observation) {
    CHECK(admission && admission->domain && admission->work);
    XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(admission->domain);
    CHECK(before.bound && before.work<before.work_limit);
    const uint64_t local=admission->work;
    const XrXirValue value={XR_XIR_I64,0,42};
    CHECK(xr_xir_value_admit(&value,XR_XIR_I64,admission)==XR_XIR_VALUE_OK);
    XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(admission->domain);
    CHECK(admission->work==local-1 && after.work==before.work+1 &&
        after.work_limit==before.work_limit && after.bound);
    ++observation->admissions;
}
static XrXirAction fuel_domain_boundary(XrXirValueAdmission *admission, FuelObservation *observation) {
    XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(admission->domain);
    CHECK(before.bound && before.work<before.work_limit && admission->work>1);
    const uint64_t remaining=before.work_limit-before.work;
    CHECK(xr_xir_domain_work(admission->domain,remaining-1));
    fuel_scalar(admission,observation);
    const uint64_t local=admission->work;
    const XrXirValue value={XR_XIR_I64,0,42};
    CHECK(local && xr_xir_value_admit(&value,XR_XIR_I64,admission)==XR_XIR_VALUE_LIMIT);
    XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(admission->domain);
    CHECK(after.work==after.work_limit && admission->work==local);
    CHECK(!xr_xir_domain_work(admission->domain,1));
    observation->limit_bank=local;observation->exhausted=true;
    return (XrXirAction){.kind=XR_XIR_ACTION_FAULT,.value={XR_XIR_I64,0,XR_XIR_CALL_LIMIT}};
}
static XrXirAction fuel_init_resume(XrXirCallView *view) {
    FuelObservation *observation=(FuelObservation *)view->environment;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    CHECK(observation && admission && view->phase==XR_XIR_CALL_NORMAL);
    ++observation->init_calls;observation->initial_bank=admission->work;
    CHECK(admission->work>8);
    for(uint32_t i=0;i<12;++i) fuel_scalar(admission,observation);
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN};
}
static XrXirAction fuel_main_resume(XrXirCallView *view) {
    FuelObservation *observation=(FuelObservation *)view->environment;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    CHECK(observation && admission && view->phase==XR_XIR_CALL_NORMAL);
    ++observation->main_calls;
    if(observation->mode==2) return fuel_domain_boundary(admission,observation);
    for(uint32_t i=0;i<4;++i) fuel_scalar(admission,observation);
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value={XR_XIR_I64,0,42}};
}

static XrXirCompileContext fuel_owner(void) {
    XrXirCompileContext context={0};
    const XrCompileResourceLimits caps={UINT64_C(2097152),UINT64_C(2097152),UINT64_C(16000000)};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();context.limits.functions=8;
    context.limits.parameters=8;context.limits.instructions=32;return context;
}
static XrXirArtifact *fuel_lowered(const XrXirCompileContext *context) {
    const XrXirInstruction init[]={{XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    const XrXirInstruction main_rows[]={
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},42,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    const XrXirBlock blocks[]={{0,1,0,0},{0,2,0,0}};
    const XrXirFunction functions[]={
        {"init",4,NULL,0,XR_XIR_UNIT,&blocks[0],1,init,1,NULL,0},
        {"main",4,NULL,0,XR_XIR_I64,&blocks[1],1,main_rows,2,NULL,0}};
    const XrXirSourceModule module={"fuel",4,NULL,0,0};
    const XrXirFunctionIdentity identities[]={{0},{.exported=1}};
    const XrXirDeclarations declarations={.modules=&module,.module_count=1,
        .functions=identities,.root_module=0,.entry_function=1};
    const XrXirModule built={XR_XIR_BUILT,functions,2,&declarations,NULL,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirArtifact *checked=NULL,*lowered=NULL;
    CHECK(xir_fixture_check(context,&built,&checked,NULL)==XR_XIR_OK && checked);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(checked,&target,&lowered,NULL)==XR_XIR_OK && lowered);
    xr_xir_compile_artifact_free(checked);return lowered;
}
static XrXirProgram *fuel_program(const XrXirCompileContext *context, FuelObservation *observation) {
    XrXirArtifact *lowered=fuel_lowered(context);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);CHECK(module);
    const XrXirCallEntry entries[]={
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.result=XR_XIR_UNIT,
            .resume=fuel_init_resume,.environment=observation},
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.result=XR_XIR_I64,
            .resume=fuel_main_resume,.environment=observation}};
    XrXirProgramSpec spec={.abi_version=XR_XIR_PROGRAM_ABI_VERSION,
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},.entries=entries,.entry_count=2,
        .declarations=module->declarations,.code={observation,fuel_code_release},
        .types=module->types,.packet=xir_program_packet_borrow(lowered)};
    CHECK(spec.packet.bytes && spec.packet.length && spec.packet.identity && xr_xir_compile_artifact_layout(lowered, 0));
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(context,&spec,&program)==XR_XIR_OK && program);
    xr_xir_compile_artifact_free(lowered);CHECK(!observation->releases);return program;
}
static XrXirInstanceConfig fuel_config(uint64_t polls) {
    XrXirInstanceConfig config={0};
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.poll_limit=polls;config.work_limit=4096;
    config.metadata_limit=65536;config.value_limit=65536;config.call_limit=65536;
    config.requested_value_limit=65536;config.requested_call_limit=65536;config.depth_limit=16;
    return config;
}
static void fuel_case(uint32_t mode) {
    CHECK(!fuel_live && !fuel_bytes);
    FuelObservation observation={.mode=mode};
    XrXirCompileContext context=fuel_owner();
    XrXirProgram *program=fuel_program(&context,&observation);
    XrXirInstanceConfig config=fuel_config(mode==1?2:8);
    XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY && instance);
    xr_xir_compile_program_drop(program);CHECK(!observation.releases);
    /* Producers and their host context expire before the first entry runs. */
    xr_compile_resources_release(context.resources);context=(XrXirCompileContext){0};
    CHECK(xr_xir_instance_start(instance,1,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
    CHECK(observation.init_calls==1 && observation.initial_bank>config.poll_limit);
    if(!mode) {
        CHECK(result.outcome.status==XR_XIR_CALL_RETURNED && result.outcome.value.type==XR_XIR_I64 &&
            result.outcome.value.payload==42 && observation.main_calls==1 && observation.admissions==16);
        XrXirValue owned={0};
        CHECK(xr_xir_instance_take_result(instance,&owned)==XR_XIR_CALL_RETURNED);
        CHECK(owned.type==XR_XIR_I64 && owned.payload==42);xr_xir_value_drop(&owned);
    } else if(mode==1) {
        CHECK(result.outcome.status==XR_XIR_CALL_LIMIT && !observation.main_calls &&
            observation.admissions==12 && !observation.exhausted);
    } else {
        CHECK(result.outcome.status==XR_XIR_CALL_LIMIT && observation.main_calls==1 &&
            observation.admissions==13 && observation.exhausted && observation.limit_bank);
    }
    CHECK(xr_xir_instance_free(instance)==(mode?XR_XIR_CALL_LIMIT:XR_XIR_CALL_READY));
    CHECK(observation.releases==1 && !fuel_live && !fuel_bytes);
}
static size_t fuel_runtime_attempt(XrXirProgram *program, const XrXirInstanceConfig *config) {
    FuelPhysical before=fuel_mark();const size_t start=fuel_attempts;
    XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(program,config,&instance)==XR_XIR_CALL_READY && instance);
    CHECK(xr_xir_instance_start(instance,1,NULL,0)==XR_XIR_CALL_READY);
    const size_t attempts=fuel_attempts-start;
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);fuel_balanced(before);return attempts;
}
static void fuel_runtime_faults(void) {
    FuelObservation observation={0};XrXirCompileContext context=fuel_owner();
    XrXirProgram *program=fuel_program(&context,&observation);
    XrXirInstanceConfig config=fuel_config(8);FuelPhysical before=fuel_mark();
    const size_t attempts=fuel_runtime_attempt(program,&config);CHECK(attempts);
    for(size_t i=0;i<attempts;++i) {
        fuel_fail_at=fuel_attempts+i;fuel_injected=false;XrXirInstance *instance=NULL;
        XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
        if(status==XR_XIR_CALL_READY) {
            CHECK(instance);status=xr_xir_instance_start(instance,1,NULL,0);
            CHECK(status==XR_XIR_CALL_OOM);
            fuel_fail_at=SIZE_MAX;CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        } else CHECK(status==XR_XIR_CALL_OOM && !instance);
        fuel_fail_at=SIZE_MAX;CHECK(fuel_injected && !observation.releases);fuel_balanced(before);
    }
    xr_xir_compile_program_drop(program);CHECK(observation.releases==1);
    xr_compile_resources_release(context.resources);CHECK(!fuel_live && !fuel_bytes);
    printf("Domain fuel runtime allocation faults=%zu; physical=0/0\n",attempts);
}
int main(void) {
    fuel_case(0);fuel_case(1);fuel_case(2);fuel_runtime_faults();
    puts("Domain fuel: one scalar debits both live banks once; poll and work limits are independent; code retires once");
    return 0;
}
