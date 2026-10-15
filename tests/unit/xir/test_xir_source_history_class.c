/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_history_class.c - Birth Class storage across real Source generations
 */
#include "xir/xxir_source_generation.h"
#include "xir/xxir_instance_generation.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"
#include "xir/xxir_vm.c"
#include "xir_source_fixture_owner.h"
static XrXirOutputStatus generation_output(void *context,const XrXirOutputGroup *group) {
    unsigned *calls=context;
    CHECK(group && group->count==1 && group->values[0].type==XR_XIR_I64 && group->values[0].payload==17);
    CHECK(!*calls);++*calls;return XR_XIR_OUTPUT_OK;
}
static XrXirProgram *program(const XrXirSourceGenerationView *view,const char *name,uint32_t *entry) {
    XrXirArtifact *closed=NULL,*lowered=NULL;XrXirProgram *output=NULL;
    CHECK(xr_xir_compile_specialize(view->result->checked,&closed,NULL)==XR_XIR_OK);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);bool found=false;
    for (uint32_t i=0;i<module->function_count;++i)
        if (module->declarations->functions[i].module==module->declarations->root_module &&
            module->functions[i].name_length==strlen(name) && !memcmp(module->functions[i].name,name,strlen(name))) {
            CHECK(!found);*entry=i;found=true;
        }
    CHECK(found);

    CHECK(xr_xir_compile_vm_program_take(&lowered,&output)==XR_XIR_OK && !lowered);return output;
}
static XrXirCallStatus generation_poll(XrXirInstance *instance) {
    for (unsigned i=0;i<1024;++i) {
        XrXirCallStatus status=xr_xir_instance_poll_bounded(instance,256).outcome.status;
        if (status!=XR_XIR_CALL_READY) return status;
    }
    CHECK(false);return XR_XIR_CALL_LIMIT;
}
static void source_publish(SourceFixtureOwner *compiler,XrXirSourceGenerations *owner,uint64_t serial) {
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(compiler->context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_xir_compile_source_generation_publish(owner,serial)==XR_XIR_OK);
    CHECK(xr_compile_resources_stats(compiler->context.resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(before.work==after.work && before.allocated_bytes==after.allocated_bytes && after.live_bytes<=before.live_bytes);
}
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--shift")));
    const char *inputs[]={
        "final class Counter{n:i64 constructor(n:i64){this.n=n} add()->i64{this.n+=1;return this.n}}\nvar value=Counter(41)\nfn old()->i64{return value.add()}\nprint(17)\nexport fn seed()->i64{return 0}",
        "const value=\"shadow\"\nexport fn answer()->i64{return old()}",
        "const value=true\nexport fn answer()->i64{return old()}",
        "const value=99\nexport fn fail()->i64{const seen=old();assert(false);return seen}"
    };
    if(argc==2) {
        inputs[1]="final class Padding{x:bool constructor(x:bool){this.x=x}}\nconst padding=Padding(true)\nconst value=\"shadow\"\nexport fn answer()->i64{return old()}";
        inputs[2]="final class Other{x:string constructor(x:string){this.x=x}}\nconst padding=Other(\"gap\")\nconst value=true\nexport fn answer()->i64{return old()}";
    }
    SourceFixtureOwner compiler={0};source_fixture_owner_new(&compiler);
    XrXirSourceGenerationConfig source_config={compiler.context,"class-history",NULL,NULL,NULL};
    XrXirSourceGenerations *owner=NULL;
    CHECK(xr_xir_compile_source_generations_new(&source_config,&owner)==XR_XIR_OK);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    unsigned output_calls=0;config.output=(XrXirOutputProvider){.abi_version=XR_XIR_CALL_ABI_VERSION,
        .write=generation_output,.context=&output_calls};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_generation_new(&config,&instance)==XR_XIR_CALL_READY);
    XrXirProgram *programs[4]={0};uint32_t entries[4]={0};
    for(unsigned i=0;i<4;++i){
        XrXirSourceText input={NULL,inputs[i],strlen(inputs[i])};uint64_t serial=0;
        XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus checked=xr_xir_compile_source_generation_prepare(owner,&input,&serial,&diagnostic);
        fprintf(stderr,"Class history generation%u Source=%u serial=%llu diagnostic=%s\n",i+1,checked,(unsigned long long)serial,diagnostic.message);
        CHECK(checked==XR_XIR_OK && serial==i+1);
        XrXirSourceGenerationView view={0};CHECK(xr_xir_compile_source_generation_view(owner,serial,&view)==XR_XIR_OK);
        programs[i]=program(&view,i==0 ? "seed" : i==3 ? "fail" : "answer",&entries[i]);
        XrXirStatus bound=xr_xir_compile_source_generation_bind_program(owner,serial,programs[i]);
        fprintf(stderr,"Class history generation%u bind=%u\n",i+1,bound);CHECK(bound==XR_XIR_OK);
        uint64_t candidate=0;CHECK(xr_xir_source_generation_prepare_instance(owner,serial,instance,&candidate)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,entries[i],NULL,0)==XR_XIR_CALL_READY);
        XrXirCallStatus outcome=generation_poll(instance);
        fprintf(stderr,"Class history generation%u runtime=%u output_calls=%u\n",i+1,outcome,output_calls);
        if(i==3){
            CHECK(outcome==XR_XIR_CALL_ASSERTION);
            CHECK(xr_xir_instance_generation_abort(instance,candidate)==XR_XIR_CALL_READY);
            CHECK(xr_xir_compile_source_generation_abort(owner,serial)==XR_XIR_OK);
        }else{
            CHECK(outcome==XR_XIR_CALL_RETURNED);XrXirValue result={0};
            CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
            CHECK(result.type==XR_XIR_I64 && result.payload==(i==0 ? 0 : i==1 ? 42 : 43));
            xr_xir_value_drop(&result);
            CHECK(xr_xir_instance_generation_publish(instance,candidate)==XR_XIR_CALL_READY);
            source_publish(&compiler,owner,serial);
        }
    }
    CHECK(xr_xir_instance_start(instance,entries[2],NULL,0)==XR_XIR_CALL_READY && generation_poll(instance)==XR_XIR_CALL_RETURNED);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(result.type==XR_XIR_I64 && result.payload==45);xr_xir_value_drop(&result);
    CHECK(output_calls==1 && xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    xr_xir_compile_source_generations_free(owner);
    for(unsigned i=0;i<4;++i)xr_xir_compile_program_drop(programs[i]);
    source_fixture_owner_free(&compiler);puts("Class birth identity and committed side effects survived three generations and a failed candidate");
    instance_compile_zero();
    CHECK(!runtime_live && !runtime_bytes);
    printf("Class history compiler/runtime physical0; runtime sites=%zu shifted=%u\n",runtime_attempts,argc==2);
    return 0;
}
