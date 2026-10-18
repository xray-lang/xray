/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_array_observe.c - Typed Array observations on the original finite owner
 *
 * KEY CONCEPT:
 *   Source supplies real type identities. Selected snapshots retain authentic
 *   identities without granting authority or admitting unrelated elements.
 */
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1); } } while (0)
#include "xir_source_compile_owner.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_array.h"
#include "xir/xxir_class.h"
#include "xir/xxir_nullable.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_types.h"
#include "xir/xxir_compile_domain.h"
#include "xir/xxir_value_internal.h"
static const XrCompileResourceLimits limits={UINT64_C(67108864),UINT64_C(8388608),UINT64_C(128000000)};
typedef struct ObserveJob {
    XrXirCompileContext context;
    XrXirDomain *domain;
    XrXirTypeArena *arena;
    XrXirArtifact *lowered;
    XrXirValue box,values[2],array;
} ObserveJob;
static XrCompileResourceStats stats(const ObserveJob *job) {
    XrCompileResourceStats out={0};CHECK(xr_compile_resources_stats(job->context.resources,&out)==XR_COMPILE_RESOURCE_OK);return out;
}
static XrXirValueAdmission admission(const ObserveJob *job) {
    return (XrXirValueAdmission){.arena=job->arena,.domain=job->domain,.work=UINT64_MAX,.scratch_bytes=65536};
}
static void job_new(ObserveJob *job) {
    *job=(ObserveJob){0};CHECK(xr_compile_resources_new(&limits,&job->context.resources)==XR_COMPILE_RESOURCE_OK);
    job->context.limits=xr_xir_compile_default_limits();
    static const char text[]="class Box{value:i64;constructor(value:i64){this.value=value}}\nexport fn run()->i64{const xs:Array<Box?>=[Box(40),null];return (xs[0]??Box(0)).value+2}\n";
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(job->context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"array-observe",NULL};
    XrXirSourceRequest request={session,NULL,&authority,&job->context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,text,sizeof(text)-1};XrXirSourceResult source={0};XrXirArtifact *closed=NULL;
    CHECK(xr_xir_compile_source_check_text(&request,&input,&source,NULL,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(source.checked,&closed,NULL)==XR_XIR_OK);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&job->lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);xr_xir_compile_source_result_free(&source);xr_compile_session_free(session);
    const XrXirTypes *types=job->lowered->module.types;XrXirType array=XR_XIR_UNIT,nullable=XR_XIR_UNIT,box=XR_XIR_UNIT;
    for (uint32_t i=0;i<types->count;++i) {
        XrXirType type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i);const XrXirTypeNode *node=&types->nodes[i];
        if (node->kind!=XR_XIR_TYPE_ARRAY) continue;
        const XrXirTypeNode *element=xr_xir_type_node(types,node->element);
        if (element && element->kind==XR_XIR_TYPE_NULLABLE && xr_xir_type_is_class(types,element->element)) {
            array=type;nullable=node->element;box=element->element;break;
        }
    }
    CHECK(array!=XR_XIR_UNIT && xr_xir_type_node(types,box)->nominal.field_count==1);
    CHECK(xr_xir_compile_type_arena_new(&job->context,types,&job->arena)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_domain_new(job->context.resources,UINT64_MAX,&job->domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission a=admission(job);XrXirValue number={XR_XIR_I64,0,40};
    CHECK(xr_xir_class_new(box,&number,1,&a,&job->box)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new(nullable,&job->box,&a,&job->values[0])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new(nullable,NULL,&a,&job->values[1])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new(array,job->values,2,&a,&job->array)==XR_XIR_VALUE_OK);
}
static void job_free(ObserveJob *job) {
    xr_xir_value_drop(&job->array);xr_xir_value_drop(&job->values[1]);xr_xir_value_drop(&job->values[0]);xr_xir_value_drop(&job->box);
    xr_xir_compile_artifact_free(job->lowered);xr_xir_compile_type_arena_drop(job->arena);xr_xir_domain_drop(job->domain);
    xr_compile_resources_release(job->context.resources);
}
static XrXirValueStatus observe(ObserveJob *job,int64_t index,XrXirValueAdmission *a,XrXirValue *out,XrXirFaultDetail *fault) {
    /* This fixture's selected Nullable<Class> has one inline field and a fixed
     * identity header. Its entire shallow snapshot span is below 512 visits. */
    CHECK(xr_xir_domain_work(job->domain,512));
    return xr_xir_array_observe(&job->array,index,a,out,fault);
}
static void check_value(const XrXirValue *value,const ObserveJob *job,bool expected) {
    bool some=false;const XrXirValue *payload=NULL;CHECK(xr_xir_nullable_view(value,&some,&payload) && some==expected);
    if (!some) return;
    CHECK(payload->type==job->box.type && payload->payload==job->box.payload);
    XrXirValueAdmission a=admission(job);XrXirValue number={0};CHECK(xr_xir_domain_work(job->domain,512));
    CHECK(xr_xir_class_get(payload,0,&a,&number)==XR_XIR_VALUE_OK && number.type==XR_XIR_I64 && number.payload==40);
    xr_xir_value_drop(&number);
}
static void controls(ObserveJob *job) {
    XrXirValueAdmission a=admission(job);XrXirValue out={0};XrXirFaultDetail fault={0};
    CHECK(observe(job,0,&a,&out,&fault)==XR_XIR_VALUE_OK);check_value(&out,job,true);xr_xir_value_drop(&out);
    CHECK(observe(job,1,&a,&out,&fault)==XR_XIR_VALUE_OK);check_value(&out,job,false);xr_xir_value_drop(&out);
    for (int64_t index=-1;index<=2;index+=3) {
        CHECK(observe(job,index,&a,&out,&fault)==XR_XIR_VALUE_BOUNDS && !out.type && !out.payload);
        CHECK(xr_xir_fault_bounds_valid(fault) && fault.index==index && fault.length==2);
    }
    XrXirValue occupied={XR_XIR_I64,0,7};XrCompileResourceStats before=stats(job);
    CHECK(xr_xir_array_observe(&job->array,0,&a,&occupied,&fault)==XR_XIR_VALUE_BAD_ARGUMENT && occupied.payload==7);
    CHECK(stats(job).work==before.work);
    XrXirTypeArena *foreign=NULL;CHECK(xr_xir_compile_type_arena_new(&job->context,job->lowered->module.types,&foreign)==XR_XIR_VALUE_OK);
    a.arena=foreign;CHECK(observe(job,0,&a,&out,&fault)==XR_XIR_VALUE_BAD_ARGUMENT && !out.type);xr_xir_compile_type_arena_drop(foreign);
    XrXirDomain *domain=NULL;CHECK(xr_xir_compile_domain_new(job->context.resources,UINT64_MAX,&domain)==XR_XIR_VALUE_OK);
    a=admission(job);a.domain=domain;CHECK(observe(job,0,&a,&out,&fault)==XR_XIR_VALUE_BAD_ARGUMENT && !out.type);xr_xir_domain_drop(domain);
    uint64_t exact_work=0,exact_scratch=0;
    for (uint64_t work=0;work<256;++work) {
        a=admission(job);a.work=work;XrXirValueStatus status=observe(job,0,&a,&out,&fault);
        if (status==XR_XIR_VALUE_OK) { exact_work=work;xr_xir_value_drop(&out);break; }
        CHECK(status==XR_XIR_VALUE_LIMIT && !out.type && !out.payload);
    }
    for (uint64_t scratch=0;scratch<1024;++scratch) {
        a=admission(job);a.scratch_bytes=scratch;XrXirValueStatus status=observe(job,0,&a,&out,&fault);
        if (status==XR_XIR_VALUE_OK) { exact_scratch=scratch;xr_xir_value_drop(&out);break; }
        CHECK(status==XR_XIR_VALUE_LIMIT && !out.type && !out.payload);
    }
    CHECK(exact_work && exact_scratch);fprintf(stderr,"Array observation exact localwork=%llu scratch=%llu, all prefixes refuse; real identity/None, bounds, occupied output and exact arena/Domain gates passed\n",(unsigned long long)exact_work,(unsigned long long)exact_scratch);
}
static void faults(ObserveJob *job) {
    XrXirValueAdmission a=admission(job);XrXirValue out={0};XrXirFaultDetail fault={0};
    size_t start=source_fixture_compile_attempts;CHECK(observe(job,0,&a,&out,&fault)==XR_XIR_VALUE_OK);
    size_t sites=source_fixture_compile_attempts-start;CHECK(sites);xr_xir_value_drop(&out);
    XrCompileResources *same=job->context.resources;XrCompileResourceStats baseline=stats(job);
    for (size_t site=0;site<sites;++site) {
        a=admission(job);start=source_fixture_compile_attempts;source_fixture_compile_fail_at=start+site;source_fixture_compile_injected=false;
        CHECK(observe(job,0,&a,&out,&fault)==XR_XIR_VALUE_OOM && !out.type && !out.payload);
        CHECK(source_fixture_compile_injected && source_fixture_compile_attempts==start+site+1 && stats(job).live_bytes==baseline.live_bytes);
        XrCompileResourceStats failed=stats(job);source_fixture_compile_fail_at=SIZE_MAX;a=admission(job);start=source_fixture_compile_attempts;
        CHECK(observe(job,0,&a,&out,&fault)==XR_XIR_VALUE_OK && source_fixture_compile_attempts==start+sites);check_value(&out,job,true);xr_xir_value_drop(&out);
        XrCompileResourceStats retry=stats(job);CHECK(job->context.resources==same && retry.work>=failed.work && retry.allocated_bytes>=failed.allocated_bytes && retry.live_bytes==baseline.live_bytes);
    }
    fprintf(stderr,"Array observation complete FI=%zu original-owner retry, empty output/live baseline, retained work/allocation\n",sites);
}
static void lifetime(ObserveJob *job) {
    XrXirValueAdmission a=admission(job);XrXirValue out={0};XrXirFaultDetail fault={0};
    CHECK(observe(job,0,&a,&out,&fault)==XR_XIR_VALUE_OK);int64_t identity=job->box.payload;
    job_free(job);bool some=false;const XrXirValue *payload=NULL;CHECK(xr_xir_nullable_view(&out,&some,&payload) && some && payload->payload==identity);
    XrXirValue number={0};CHECK(xr_xir_domain_work(a.domain,512));CHECK(xr_xir_class_get(payload,0,&a,&number)==XR_XIR_VALUE_OK && number.payload==40);xr_xir_value_drop(&number);
    size_t attempts=source_fixture_compile_attempts;source_fixture_compile_fail_at=attempts;xr_xir_value_drop(&out);source_fixture_compile_fail_at=SIZE_MAX;
    CHECK(source_fixture_compile_attempts==attempts && !source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Array observed snapshot survives Source/Artifact/arena/Domain/ledger caller death; last drop allocates nothing physical0\n");
}
int main(void) {
    ObserveJob job;job_new(&job);controls(&job);faults(&job);lifetime(&job);return 0;
}
