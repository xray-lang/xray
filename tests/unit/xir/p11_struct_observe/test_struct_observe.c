/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_struct_observe.c - Authentic bounded nominal field observations
 *
 * KEY CONCEPT:
 *   Complete ordinary Source and real VM execution precede observations.
 *   A copied field outlives its receiver, producer, Domain and ledger caller.
 */
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1); } } while (0)
#include "xir_source_compile_owner.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_struct.h"
#include "xir/xxir_class.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_program.h"
#include "xir/xxir_types.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_compile_domain.h"
#include "xir/xxir_value_internal.h"
static const XrCompileResourceLimits caps={67108864,8388608,128000000};
typedef struct ObserveJob {
    XrXirCompileContext context;
    XrXirTypeArena *arena;
    XrXirDomain *domain;
    XrXirValue record,box;
} ObserveJob;
static XrXirValueAdmission admission(ObserveJob *job) {
    return (XrXirValueAdmission){.arena=job->arena,.domain=job->domain,.work=UINT64_MAX,.scratch_bytes=65536};
}
static void execute(XrXirArtifact **lowered) {
    const XrXirModule *module=xr_xir_compile_artifact_module(*lowered);uint32_t run=UINT32_MAX;
    for (uint32_t f=0;f<module->function_count;++f)
        if (module->functions[f].name_length==3 && !memcmp(module->functions[f].name,"run",3)) { CHECK(run==UINT32_MAX);run=f; }
    CHECK(run!=UINT32_MAX);XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(lowered,&program)==XR_XIR_OK && !*lowered);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.metadata_limit=1048576;config.value_limit=1048576;config.call_limit=1048576;config.work_limit=1000000;
    XrXirInstance *instances[2]={NULL,NULL};
    for (uint32_t i=0;i<2;++i) CHECK(xr_xir_instance_new(program,&config,&instances[i])==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    for (uint32_t i=0;i<2;++i) {
        CHECK(xr_xir_instance_start(instances[i],run,NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instances[i],UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue result={0};CHECK(xr_xir_instance_take_result(instances[i],&result)==XR_XIR_CALL_RETURNED);
        CHECK(result.type==XR_XIR_I64 && result.payload==42);xr_xir_value_drop(&result);
        CHECK(xr_xir_instance_free(instances[i])==XR_XIR_CALL_READY);
    }
}
static void job_new(ObserveJob *job) {
    *job=(ObserveJob){0};CHECK(xr_compile_resources_new(&caps,&job->context.resources)==XR_COMPILE_RESOURCE_OK);
    job->context.limits=xr_xir_compile_default_limits();
    static const char text[]="class Box{value:i64;init(value:i64){this.value=value}}\n"
        "struct Wrap{item:Box;label:string}\nexport fn run()->i64{const w=Wrap{item:Box(42),label:\"ab\"};return w.item.value}\n";
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(job->context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"struct-observe",NULL};
    XrXirSourceRequest request={session,NULL,&authority,&job->context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,text,sizeof(text)-1};XrXirSourceResult source={0};XrXirArtifact *closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_source_check_text(&request,&input,&source,NULL,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(source.checked,&closed,NULL)==XR_XIR_OK);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target, NULL,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);xr_xir_compile_source_result_free(&source);xr_compile_session_free(session);
    const XrXirTypes *types=xr_xir_compile_artifact_module(lowered)->types;XrXirType wrap=XR_XIR_UNIT,box=XR_XIR_UNIT;
    for (uint32_t i=0;i<types->count;++i) {
        XrXirType type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i);const XrXirTypeNode *node=&types->nodes[i];
        if (xr_xir_type_is_struct(types,type) && node->nominal.field_count==2 &&
            xr_xir_type_is_class(types,node->nominal.fields[0]) && node->nominal.fields[1]==XR_XIR_STRING) {
            CHECK(wrap==XR_XIR_UNIT);wrap=type;box=node->nominal.fields[0];
        }
    }
    CHECK(wrap!=XR_XIR_UNIT);CHECK(xr_xir_compile_type_arena_new(&job->context,types,&job->arena)==XR_XIR_VALUE_OK);
    execute(&lowered);
    CHECK(xr_xir_compile_domain_new(job->context.resources,UINT64_MAX,&job->domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission a=admission(job);XrXirValue number={XR_XIR_I64,0,42},label={0};
    CHECK(xr_xir_class_new(box,&number,1,&a,&job->box)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(job->domain,"ab",2,&label)==XR_XIR_VALUE_OK);
    XrXirValue fields[]={job->box,label};CHECK(xr_xir_struct_new(wrap,fields,2,&a,&job->record)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&label);
}
static XrXirValueStatus observe(ObserveJob *job,uint32_t field,XrXirValueAdmission *a,XrXirValue *out) {
    /* This real two-field receiver and selected Class/String headers have a
     * complete fixed shallow validation span below 2048 visits. */
    CHECK(xr_xir_domain_work(job->domain,2048));
    return xr_xir_struct_observe(&job->record,field,a,out);
}
static void controls(ObserveJob *job) {
    XrXirValueAdmission a=admission(job);XrXirValue out={0};
    CHECK(observe(job,0,&a,&out)==XR_XIR_VALUE_OK && out.payload==job->box.payload);xr_xir_value_drop(&out);
    for (uint64_t work=0;work<=65;++work) {
        a=admission(job);a.work=work;a.scratch_bytes=0;
        CHECK(observe(job,1,&a,&out)==(work==65?XR_XIR_VALUE_OK:XR_XIR_VALUE_LIMIT));
        if (work==65) { const char *s=NULL;size_t n=0;CHECK(!a.work && xr_xir_string_view(&out,&s,&n) && n==2 && !memcmp(s,"ab",2)); }
        else CHECK(!out.type && !out.payload);
        xr_xir_value_drop(&out);
    }
    a=admission(job);CHECK(observe(job,2,&a,&out)==XR_XIR_VALUE_BAD_ARGUMENT && !out.type);
    XrXirValue occupied={XR_XIR_I64,0,7};XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(job->context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_xir_struct_observe(&job->record,0,&a,&occupied)==XR_XIR_VALUE_BAD_ARGUMENT && occupied.payload==7);
    CHECK(xr_compile_resources_stats(job->context.resources,&after)==XR_COMPILE_RESOURCE_OK && after.work==before.work);
    XrXirTypeArena *foreign=NULL;CHECK(xr_xir_compile_type_arena_new(&job->context,xr_xir_compile_type_arena_types(job->arena),&foreign)==XR_XIR_VALUE_OK);
    a=admission(job);a.arena=foreign;CHECK(observe(job,0,&a,&out)==XR_XIR_VALUE_BAD_ARGUMENT && !out.type);xr_xir_compile_type_arena_drop(foreign);
    XrXirDomain *other=NULL;CHECK(xr_xir_compile_domain_new(job->context.resources,UINT64_MAX,&other)==XR_XIR_VALUE_OK);
    a=admission(job);a.domain=other;CHECK(observe(job,0,&a,&out)==XR_XIR_VALUE_BAD_ARGUMENT && !out.type);xr_xir_domain_drop(other);
    a=admission(job);size_t start=source_fixture_compile_attempts;source_fixture_compile_fail_at=start;source_fixture_compile_injected=false;
    CHECK(observe(job,1,&a,&out)==XR_XIR_VALUE_OK && source_fixture_compile_attempts==start && !source_fixture_compile_injected);
    source_fixture_compile_fail_at=SIZE_MAX;xr_xir_value_drop(&out);
}
int main(void) {
    ObserveJob job;job_new(&job);controls(&job);
    XrXirValueAdmission a=admission(&job);XrXirValue saved={0};CHECK(observe(&job,1,&a,&saved)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&job.record);xr_xir_value_drop(&job.box);xr_xir_compile_type_arena_drop(job.arena);
    xr_xir_domain_drop(job.domain);xr_compile_resources_release(job.context.resources);
    const char *text=NULL;size_t length=0;CHECK(xr_xir_string_view(&saved,&text,&length) && length==2 && !memcmp(text,"ab",2));
    CHECK(source_fixture_compile_live);size_t calls=source_fixture_compile_attempts;xr_xir_value_drop(&saved);
    CHECK(calls==source_fixture_compile_attempts && !source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Struct observer actual allocation census0,localwork65/all smaller prefixes,scratch0,exact arena/Domain,bounds/occupied-output,ordinary two-Instance VM42 and last-owner physical0\n");return 0;
}
