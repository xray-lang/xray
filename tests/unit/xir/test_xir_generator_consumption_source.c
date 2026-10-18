/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_consumption_source.c - Actual consumption carries parked ROOT effects
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_source_fixture_owner.h"
typedef struct ConsumptionCase {const char *name,*body;bool cold;} ConsumptionCase;
static const ConsumptionCase consumption_cases[]={
    {"hasNext","const it=values()\nif (!it.hasNext()){return -1}\nreturn 17",false},
    {"next","const it=values()\nreturn it.next()",false},
    {"nth","const it=values()\nreturn it.nth(0)",false},
    {"loop","var sum=0\nfor(value in values()){sum=sum+value}\nreturn sum",false},
    {"returned","const it=make()\nreturn it.next()",false},
    {"parameter","return consume(values())",false},
    {"callable","const factory=fn()->Iterator<i64>{return values()}\nconst it=factory()\nreturn it.next()",false},
    {"phi","const left=values()\nconst right=values()\nconst it=true ? left : right\nreturn it.next()",false},
    {"parameter_phi","const left=values()\nconst right=values()\nconst it=true ? left : right\nreturn consume(it)",false},
    {"cold","const unused=values()\nreturn 17",true}
};
static uint32_t consumption_function(const XrXirModule *module,const char *name) {
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==strlen(name) &&
            !memcmp(module->functions[f].name,name,strlen(name))) return f;
    return UINT32_MAX;
}
static void consumption_facts(XrXirArtifact *artifact) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);XrXirEffects *effects=NULL;
    CHECK(xr_xir_compile_effects_analyze(artifact,&effects)==XR_XIR_OK);
    uint32_t worker=consumption_function(module,"worker");CHECK(worker!=UINT32_MAX);
    XrXirEffectFacts facts=xr_xir_effects_read(effects,worker);
    CHECK(facts.status==XR_XIR_OK && !facts.root.requires_root && !facts.root.unresolved);
    xr_xir_compile_effects_free(effects);
}
static void consumption_execute(XrXirArtifact **lowered) {
    const XrXirModule *module=xr_xir_compile_artifact_module(*lowered);
    uint32_t entry=consumption_function(module,"run");CHECK(entry!=UINT32_MAX);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(lowered,&program)==XR_XIR_OK && !*lowered);
    XrXirInstanceConfig config;XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirCallStatus status=xr_xir_instance_poll_bounded(instance,1000000).outcome.status;
    if(status!=XR_XIR_CALL_RETURNED) fprintf(stderr,"Iterator consumption execution status%u\n",status);
    CHECK(status==XR_XIR_CALL_RETURNED);XrXirValue value={0};
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.type==XR_XIR_I64 && value.payload==17);xr_xir_value_drop(&value);
    xr_xir_compile_program_drop(program);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
}
static void consumption_check(const ConsumptionCase *fixture,bool root,const char *c_path) {
    char text[4096];int length=snprintf(text,sizeof(text),
        "var state:i64=0\nfn values()->Iterator<i64>{yield %s}\n"
        "fn make()->Iterator<i64>{return values()}\n"
        "fn consume(it:Iterator<i64>)->i64{return it.next()}\n"
        "fn worker()->i64{try{%s} catch(error){return -99}}\nexport fn run()->i64{return await go worker()}\nfn main(){}\n",
        root?"state":"17",fixture->body);
    CHECK(length>0 && (size_t)length<sizeof(text));
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"generator-consumption",NULL};
    XrXirSourceRequest request={session,NULL,&authority,&owner.context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,
        &(XrXirSourceText){NULL,text,(size_t)length},&source,&diagnostic,NULL);
    xr_compile_session_free(session);bool reject=root && !fixture->cold;
    XrXirStatus expected=reject?XR_XIR_BAD_TYPE:XR_XIR_OK;
    if(status!=expected) fprintf(stderr,"%s root%u expected%u status%u line%d: %s\n",
        fixture->name,root,expected,status,diagnostic.line,diagnostic.message);
    CHECK(status==expected);
    if(reject) {
        CHECK(!source.checked && !source.snapshot);
        CHECK(strstr(diagnostic.message,"GO target requires the current instance root execution"));
    } else {
        CHECK(source.checked && source.snapshot);consumption_facts(source.checked);
        XrXirCheckedPacket packet={0};XrXirArtifact *decoded=NULL,*closed=NULL,*lowered=NULL;
        CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
        xr_xir_compile_source_result_free(&source);
        CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
        memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);consumption_facts(decoded);
        CHECK(xr_xir_compile_specialize(decoded,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(decoded);
        CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},&lowered,NULL)==XR_XIR_OK);
        xr_xir_compile_artifact_free(closed);CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);
        XrXirCSource c={0};CHECK(xr_xir_compile_emit_c(lowered,"generator_consumption",1048576,&c)==XR_XIR_OK);
        CHECK(c.text && c.length && !strstr(c.text,"({"));
        if(c_path) {
            FILE *file=fopen(c_path,"wb");CHECK(file);CHECK(fwrite(c.text,1,c.length,file)==c.length);
            CHECK(fprintf(file,"\nconst uint32_t generator_consumption_entry=%u;\n",
                consumption_function(xr_xir_compile_artifact_module(lowered),"run"))>0);CHECK(!fclose(file));
        }
        xr_xir_compile_c_source_free(&c);consumption_execute(&lowered);
    }
    xr_xir_compile_source_result_free(&source);source_fixture_owner_free(&owner);
    fprintf(stderr,"Consumption %s root%u %s PASS\n",fixture->name,root,reject?"rejected":"executed");
}
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);
    for(size_t i=0;i<sizeof(consumption_cases)/sizeof(*consumption_cases);++i) {
        consumption_check(&consumption_cases[i],true,NULL);
        consumption_check(&consumption_cases[i],false,argc==2 && i==8?argv[1]:NULL);
    }
    puts("Actual Iterator ROOT, pure and cold Source pairs, dead producers, Checked roundtrip, C11 and VM PASS");
    return 0;
}
