/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_atomic_compiler_resources.c - Original-owner Atomic compiler boundaries
 */
#include "xir_mixed_atomic_fixture.h"

typedef struct AtomicCompilerResult {
    XrCompileResourceStats stats;
    XrXirStatus status;
    unsigned stage,releases;
    size_t sites;
    bool complete,injected;
} AtomicCompilerResult;

static XrCompileResourceLimits atomic_compiler_caps(void) {
    return (XrCompileResourceLimits){67108864,8388608,128000000};
}
static void atomic_compiler_product(const XrXirProgram *program) {
    CHECK(program && program->entry_count==37 && program->declarations->module_count==4 && program->declarations->slot_count==7);
    for(unsigned i=0;i<ATOMIC_RUNTIME_ENTRIES;++i) {
        CHECK(p20_atomic_runtime_entries[i]<program->entry_count);
        const XrXirCallEntry *entry=&program->entries[p20_atomic_runtime_entries[i]];
        CHECK(!entry->parameter_count);
        if(i<6)CHECK(entry->result==XR_XIR_I64);
        else if(i<8)CHECK(entry->result==XR_XIR_F64);
        else if(i<10)CHECK(entry->result==XR_XIR_BOOL);
        else if(i==14)CHECK(entry->result==XR_XIR_UNIT);
        else {
            const XrXirTypeNode *type=xr_xir_type_node(program->types,entry->result);
            CHECK(type && type->kind==XR_XIR_TYPE_ATOMIC);
        }
    }
}
static AtomicCompilerResult atomic_compiler_transaction(unsigned backend,XrCompileResourceLimits caps,size_t failure) {
    CHECK(backend<4 && !effects_compile_live && !effects_compile_bytes && !effects_source_owner_count && !runtime_live && !runtime_bytes);
    CHECK(caps.allocated_bytes<=67108864 && caps.live_bytes<=8388608 && caps.work<=128000000);
    effects_compile_attempts=0;effects_compile_fail_at=failure;effects_compile_injected=false;
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    AtomicSourcePrepare source={.context=&context};AtomicRuntimeFixture fixture={.backend=backend};
    AtomicCompilerResult result={0};XrXirProgram *program=NULL;
    XrCompileResourceStatus created=xr_compile_resources_new(&caps,&context.resources);
    if(atomic_source_stage(&source,1,atomic_runtime_resource_status(created))) {
        fixture.resources=context.resources;
        if(atomic_source_prepare(&source)) {
            uint8_t digest[32];xr_sha256((const uint8_t *)source.generated.text,source.generated.length,digest);
            CHECK(source.generated.length==p20_atomic_runtime_c_bytes && !memcmp(digest,p20_atomic_runtime_c_sha256,sizeof(digest)));
            fixture.c_body_matched=true;result.complete=atomic_runtime_images(&source,&fixture,&program);
        }
    }else CHECK(!context.resources);
    result.status=source.status;result.stage=source.stage;atomic_source_cleanup(&source);
    if(!program) {
        CHECK(!result.complete && result.status!=XR_XIR_OK && !fixture.releases);
        xr_xir_compile_program_drop(fixture.vm);fixture.vm=NULL;
        xr_compile_resources_free(fixture.entries);fixture.entries=NULL;
    }else {
        CHECK(result.complete && result.status==XR_XIR_OK && result.stage==11 && fixture.c_body_matched && fixture.vm_prepared);
        atomic_compiler_product(program);
    }
    if(context.resources)CHECK(xr_compile_resources_stats(context.resources,&result.stats)==XR_COMPILE_RESOURCE_OK);
    size_t attempts=effects_compile_attempts;
    xr_compile_resources_release(context.resources);context.resources=NULL;
    if(program) {
        atomic_compiler_product(program);xr_xir_compile_program_drop(program);
        CHECK(fixture.releases==1 && !fixture.vm && !fixture.entries);
    }
    CHECK(effects_compile_attempts==attempts && !effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
    result.sites=attempts;result.injected=effects_compile_injected;result.releases=fixture.releases;effects_compile_fail_at=SIZE_MAX;
    CHECK(result.stats.allocated_bytes<=caps.allocated_bytes && result.stats.peak_bytes<=caps.live_bytes && result.stats.work<=caps.work);
    return result;
}
static void atomic_compiler_axes(unsigned backend,const AtomicCompilerResult *normal) {
    const uint64_t measured[]={normal->stats.allocated_bytes,normal->stats.peak_bytes,normal->stats.work};
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus) {
        CHECK(measured[axis]>1);XrCompileResourceLimits caps=atomic_compiler_caps();
        if(!axis)caps.allocated_bytes=measured[axis]-minus;
        else if(axis==1)caps.live_bytes=measured[axis]-minus;
        else caps.work=measured[axis]-minus;
        AtomicCompilerResult r=atomic_compiler_transaction(backend,caps,SIZE_MAX);
        printf("Atomic compiler axis backend=%u axis=%u minus=%u cap=%llu complete=%u status=%u stage=%u sites=%zu allocated=%llu peak=%llu work=%llu leases=%u sameOriginalOwner callerDeadBeforeProgram physical0/0\n",
            backend,axis,minus,(unsigned long long)(measured[axis]-minus),(unsigned)r.complete,(unsigned)r.status,r.stage,r.sites,
            (unsigned long long)r.stats.allocated_bytes,(unsigned long long)r.stats.peak_bytes,(unsigned long long)r.stats.work,r.releases);fflush(stdout);
        CHECK(r.status==(minus?XR_XIR_BUDGET:XR_XIR_OK) && r.complete==!minus && r.releases==(unsigned)!minus);
        if(!minus)CHECK(r.stats.allocated_bytes==measured[0] && r.stats.peak_bytes==measured[1] && r.stats.work==measured[2] && r.sites==normal->sites);
    }
}
static size_t atomic_compiler_number(const char *text) {
    char *end=NULL;unsigned long n=strtoul(text,&end,10);CHECK(text[0] && end && !*end && n<20000);return (size_t)n;
}
int main(int argc,char **argv) {(void)atomic_runtime_cost;(void)atomic_runtime_prepare;
    CHECK(argc==3 || argc==6);const char *names[]={"0","1","vm","native"};unsigned backend=0;
    while(backend<4 && strcmp(argv[1],names[backend]))++backend;CHECK(backend<4);
    AtomicCompilerResult normal=atomic_compiler_transaction(backend,atomic_compiler_caps(),SIZE_MAX);
    printf("Atomic compiler normal backend=%u status=%u stage=%u sites=%zu allocated=%llu peak=%llu work=%llu leases=%u realCBodyMatched SourceSessionDead CheckedReadPoison sameOriginalOwner callerDeadBeforeProgram physical0/0\n",
        backend,(unsigned)normal.status,normal.stage,normal.sites,(unsigned long long)normal.stats.allocated_bytes,(unsigned long long)normal.stats.peak_bytes,(unsigned long long)normal.stats.work,normal.releases);fflush(stdout);
    CHECK(normal.complete && normal.status==XR_XIR_OK && normal.stage==11 && normal.releases==1 && normal.sites && normal.sites<20000);
    if(!strcmp(argv[2],"axes")){CHECK(argc==3);atomic_compiler_axes(backend,&normal);return 0;}
    CHECK(argc==6 && !strcmp(argv[2],"fi"));
    size_t begin=atomic_compiler_number(argv[3]),end=atomic_compiler_number(argv[4]),expected=atomic_compiler_number(argv[5]);
    CHECK(begin<end && end<=expected && normal.sites==expected);
    for(size_t site=begin;site<end;++site) {
        AtomicCompilerResult r=atomic_compiler_transaction(backend,atomic_compiler_caps(),site);
        printf("Atomic compiler FI backend=%u ordinal=%zu status=%u stage=%u sites=%zu injected=%u leases=%u physical0/0\n",backend,site,(unsigned)r.status,r.stage,r.sites,(unsigned)r.injected,r.releases);fflush(stdout);
        CHECK(!r.complete && r.status==XR_XIR_OUT_OF_MEMORY && r.injected && r.sites>site && !r.releases);
    }
    printf("Atomic compiler complete FI backend=%u range=%zu:%zu of%zu everyOriginalOrdinal typedOOM physical0/0\n",backend,begin,end,expected);return 0;
}
