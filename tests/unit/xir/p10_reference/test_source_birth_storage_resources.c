/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_birth_storage_resources.c - Real receiving allocation boundaries
 */
#include "xir/xxir_source_generation.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_generic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "../xir_source_fixture_owner.h"
#include "../xir_instance_compile_observer.h"
#include "../xir_runtime_allocations.h"

typedef struct BirthCost { size_t sites; XrXirCallStatus status; } BirthCost;
static XrXirOutputStatus birth_output(void *context,const XrXirOutputGroup *group) {
    unsigned *calls=context;
    CHECK(group && group->count==1 && group->values[0].type==XR_XIR_I64 && group->values[0].payload==17 && !*calls);
    ++*calls;return XR_XIR_OUTPUT_OK;
}
static uint64_t birth_source(XrXirSourceGenerations *owner,const char *source,const char *name,
    XrXirProgram **output,uint32_t *entry) {
    XrXirSourceText text={NULL,source,strlen(source)};uint64_t serial=0;
    XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_generation_prepare(owner,&text,&serial,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"birth source %s status=%u line=%d %s\n",name,(unsigned)status,diagnostic.line,diagnostic.message);
    CHECK(status==XR_XIR_OK);
    XrXirSourceGenerationView view={0};
    CHECK(xr_xir_compile_source_generation_view(owner,serial,&view)==XR_XIR_OK);
    XrXirArtifact *closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_specialize(view.result->checked,&closed,NULL)==XR_XIR_OK);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);bool found=false;
    for (uint32_t f=0;f<module->function_count;++f) {
        if (module->declarations->functions[f].module!=module->declarations->root_module ||
            module->functions[f].name_length!=strlen(name) || memcmp(module->functions[f].name,name,strlen(name))) continue;
        CHECK(!found);found=true;*entry=f;
    }
    CHECK(found && xr_xir_compile_vm_program_take(&lowered,output)==XR_XIR_OK && !lowered);
    CHECK(xr_xir_compile_source_generation_bind_program(owner,serial,*output)==XR_XIR_OK);
    return serial;
}
static XrXirCallStatus birth_drive(XrXirInstance *instance,uint32_t entry) {
    XrXirCallStatus status=xr_xir_instance_start(instance,entry,NULL,0);
    if (status!=XR_XIR_CALL_READY) return status;
    status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
    if (status!=XR_XIR_CALL_RETURNED) return status;
    XrXirValue value={0};
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.type==XR_XIR_I64 && value.payload==42);xr_xir_value_drop(&value);
    return XR_XIR_CALL_READY;
}
static bool birth_trial(size_t ordinal,bool publish,bool text_value,unsigned storage_kind,BirthCost *cost) {
    CHECK(!runtime_live && !runtime_bytes && !instance_compile_live && !instance_compile_bytes);
    runtime_fail_at=SIZE_MAX;
    SourceFixtureOwner compiler={0};source_fixture_owner_new(&compiler);
    XrXirSourceGenerationConfig config={compiler.context,"birth-resource-test",NULL,NULL,NULL};
    XrXirSourceGenerations *owner=NULL;
    CHECK(xr_xir_compile_source_generations_new(&config,&owner)==XR_XIR_OK);
    XrXirProgram *first_program=NULL,*next_program=NULL;uint32_t first_entry=0,next_entry=0;
    bool cell_value=storage_kind==1 || storage_kind==2;
    const char *source=cell_value ?
        "var value:i64=41\nfn old()->i64{return value+1}\nprint(17)\nexport fn seed()->i64{return old()}" : text_value ?
        "const value=\"birth\"\nfn old()->i64{if (value==\"birth\"){return 42}else{return 0}}\nprint(17)\nexport fn seed()->i64{return old()}" :
        "const value=41\nfn old()->i64{return value+1}\nprint(17)\nexport fn seed()->i64{return old()}";
    if (storage_kind==2) source="var value:Array<i64> = [41]\nfn old()->i64{return value[0]+1}\nprint(17)\nexport fn seed()->i64{return old()}";
    if (storage_kind==3) source="const value:Array<i64> = [41]\nfn old()->i64{return value[0]+1}\nprint(17)\nexport fn seed()->i64{return old()}";
    if (storage_kind==4) source="var cell:Box=Box(41)\nfn old()->i64{return cell.value+1}\nclass Box{value:i64 init(first:i64){this.value=first}}\nprint(17)\nexport fn seed()->i64{return old()}";
    if (storage_kind==5) source="var cell:Box=Box(41)\nfn old()->i64{return cell.value[0]+1}\nclass Box{value:Array<i64> init(first:i64){this.value=[first]}}\nprint(17)\nexport fn seed()->i64{return old()}";
    if (storage_kind==6) source="var cell:Box=Box(\"birth\")\nfn old()->i64{if(cell.value==\"birth\"){return 42}else{return 0}}\nclass Box{value:string init(first:string){this.value=first}}\nfn fetch()->Box{return cell}\nprint(17)\nexport fn seed()->i64{return old()}";
    uint64_t first=birth_source(owner,source,
        "seed",&first_program,&first_entry);
    XrXirInstanceConfig runtime;CHECK(xr_xir_instance_config_init(&runtime,sizeof(runtime))==XR_XIR_CALL_READY);
    unsigned calls=0;runtime.output=(XrXirOutputProvider){.abi_version=XR_XIR_CALL_ABI_VERSION,
        .write=birth_output,.context=&calls};
    XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_generation_new(&runtime,&instance)==XR_XIR_CALL_READY);
    uint64_t generation=0;
    CHECK(xr_xir_source_generation_prepare_instance(owner,first,instance,&generation)==XR_XIR_CALL_READY);
    CHECK(birth_drive(instance,first_entry)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_generation_publish(instance,generation)==XR_XIR_CALL_READY);
    CHECK(xr_xir_compile_source_generation_publish(owner,first)==XR_XIR_OK);
    const char *next_source=text_value ? "const value=99\nexport fn answer()->i64{return old()}" :
        "const value=\"shadow\"\nexport fn answer()->i64{return old()}";
    if (storage_kind==4) next_source="const value=\"shadow\"\nexport fn answer()->i64{return cell.value+1}";
    if (storage_kind==5) next_source="const value=\"shadow\"\nexport fn answer()->i64{return cell.value[0]+1}";
    if (storage_kind==6) next_source="struct Padding{flag:bool}\nconst value=\"shadow\"\nfn read(slot:ref string)->i64{if(slot==\"birth\"){return 42}else{return 0}}\nexport fn answer()->i64{var object=fetch();return read(ref object.value)}";
    uint64_t next=birth_source(owner,next_source,"answer",&next_program,&next_entry);
    size_t before_blocks=runtime_live,before_bytes=runtime_bytes;
    runtime_attempts=0;runtime_fail_at=ordinal;generation=0;
    XrXirCallStatus status=xr_xir_source_generation_prepare_instance(owner,next,instance,&generation);
    if (status==XR_XIR_CALL_READY) status=birth_drive(instance,next_entry);
    bool complete=status==XR_XIR_CALL_READY;
    CHECK(complete || status==XR_XIR_CALL_OOM);
    cost->sites=runtime_attempts;cost->status=status;runtime_fail_at=SIZE_MAX;
    if (complete && publish) {
        CHECK(xr_xir_instance_generation_publish(instance,generation)==XR_XIR_CALL_READY);
        CHECK(xr_xir_compile_source_generation_publish(owner,next)==XR_XIR_OK);
    } else {
        if (generation) CHECK(xr_xir_instance_generation_abort(instance,generation)==XR_XIR_CALL_READY);
        CHECK(xr_xir_compile_source_generation_abort(owner,next)==XR_XIR_OK);
        CHECK(runtime_live==before_blocks && runtime_bytes==before_bytes);
        CHECK(birth_drive(instance,first_entry)==XR_XIR_CALL_READY);
    }
    CHECK(calls==1);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    xr_xir_compile_source_generations_free(owner);
    xr_xir_compile_program_drop(next_program);xr_xir_compile_program_drop(first_program);
    XrCompileResourceStats compiler_stats={0};
    CHECK(xr_compile_resources_stats(compiler.context.resources,&compiler_stats)==XR_COMPILE_RESOURCE_OK &&
        compiler_stats.live_bytes==compiler.baseline.live_bytes);
    xr_compile_resources_release(compiler.context.resources);
    CHECK(!runtime_live && !runtime_bytes);instance_compile_zero();
    return complete;
}
int main(int argc,char **argv) {
    bool baseline=argc==2;
    CHECK(argc==1 || (baseline && !strcmp(argv[1],"--baseline")));
    for (unsigned mode=0;mode<16;++mode) {
        bool publish=(mode&1)!=0,text_value=mode<4 && (mode&2)!=0;
        unsigned storage_kind=mode<4 ? 0 : mode<6 ? 1 : mode<8 ? 2 : mode<10 ? 3 : mode<12 ? 4 : mode<14 ? 5 : 6;
        bool cell_value=storage_kind==1 || storage_kind==2;
        BirthCost normal={0};CHECK(birth_trial(SIZE_MAX,publish,text_value,storage_kind,&normal));
        CHECK(normal.sites && normal.sites<20000);
        printf("BIRTH_STORAGE_BASELINE mode=%u publish=%u string=%u cell=%u sites=%zu compiler_and_runtime_physical=0/0\n",
            mode,(unsigned)publish,(unsigned)text_value,(unsigned)cell_value,normal.sites);
        if (baseline) continue;
        for (size_t ordinal=0;ordinal<normal.sites;++ordinal) {
            BirthCost failure={0};CHECK(!birth_trial(ordinal,publish,text_value,storage_kind,&failure));
            CHECK(failure.status==XR_XIR_CALL_OOM && failure.sites>ordinal);
        }
        BirthCost retry={0};CHECK(birth_trial(SIZE_MAX,publish,text_value,storage_kind,&retry) && retry.sites==normal.sites);
        printf("BIRTH_STORAGE_FI mode=%u sites=%zu all=OOM old42_preserved=1 retry42=1 initialization_once=1 physical=0/0\n",mode,normal.sites);
    }
    return 0;
}
