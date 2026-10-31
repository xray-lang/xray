/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_map_iteration_source.c - Lexical Map snapshots and owned results
 *
 * KEY CONCEPT:
 *   A lexical loop holds one Map value while root mutations use COW. Its
 *   ordered key/value copies and escaping values retain their real owners.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_program.h"
#include "xir/xxir_types.h"
#include "xir/xxir_array.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"Map iteration %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static const char source_text[]=
    "fn count<K:Hashable,V>(m:Map<K,V>)->i64 {var n=0\n"
    "for(k,v in m){n+=1}\n"
    "return n}\n"
    "export fn entry()->i64 {\n"
    "var m=#{1:10,2:20,3:30}\n"
    "const old=m\n"
    "var calls=0\n"
    "fn receiver()->Map<i64,i64>{calls+=1\n"
    "return m}\n"
    "var trace=0\n"
    "var sum=0\n"
    "for(k,v in receiver()){\n"
    "trace=trace*10+k\n"
    "sum+=v\n"
    "if(k==1){m.clear()\n"
    "m.set(9,99)\n"
    "continue}\n"
    "if(k==2){break}\n"
    "}\n"
    "if(trace!=12 || sum!=30 || calls!=1 || len(old)!=3 || m[9]!=99){return -1}\n"
    "var touched=0\n"
    "for(k,v in Map<i64,string>()){touched+=1}\n"
    "if(touched!=0){return -2}\n"
    "for(_,v in #{1:2,2:3}){touched+=v}\n"
    "for(k,_ in #{4:9}){touched+=k}\n"
    "if(touched!=9 || count(old)!=3 || count(#{\"x\":[1,2]})!=1){return -3}\n"
    "return 42\n"
    "}\n"
    "export fn early()->i64 {for(k,v in #{4:5}){return k+v}\n"
    "return -1}\n"
    "export fn retained()->Array<string> {\n"
    "var m=#{1:\"中\",2:\"😀\"}\n"
    "var out:Array<string>=[]\n"
    "for(k,v in m){out.push(v)\n"
    "m.clear()}\n"
    "return out}\n"
;
static XrCompileResourceStats stats(XrCompileResources *resources) {
    XrCompileResourceStats out={0};CHECK(xr_compile_resources_stats(resources,&out)==XR_COMPILE_RESOURCE_OK);return out;
}
static XrXirValue invoke(XrXirInstance *instance,uint32_t id,const XrXirValue *args,uint32_t count) {
    CHECK(xr_xir_instance_start(instance,id,args,count)==XR_XIR_CALL_READY);
    XrXirCallResult result={0};
    for(unsigned p=0;;++p){CHECK(p<10000);result=xr_xir_instance_poll_bounded(instance,1024).outcome;if(result.status!=XR_XIR_CALL_READY)break;}
    CHECK(result.status==XR_XIR_CALL_RETURNED);XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);return value;
}
static uint32_t named(const XrXirModule *module,const char *name) {
    for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==strlen(name)&&!memcmp(module->functions[f].name,name,strlen(name)))return f;
    CHECK(false);return 0;
}
static XrXirProgram *compile(XrXirCompileContext *context,uint32_t ids[3]) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"map-iteration",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&(XrXirSourceText){NULL,source_text,sizeof(source_text)-1},&source,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"Source %u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK&&source.checked);xr_compile_session_free(session);
    XrXirCheckedPacket packet={0};XrXirArtifact *decoded=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_source_result_free(&source);
    CHECK(xr_xir_compile_specialize(decoded,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(decoded);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    ids[0]=named(module,"entry");ids[1]=named(module,"early");ids[2]=named(module,"retained");
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK&&!lowered);return program;
}
static void rejections(XrXirCompileContext *context) {
    const char *cases[]={"for(k in #{1:2}){}\n","for(k,v in #[1,2]){}\n","for(k,v in #{1:2}){k=3}\n"};
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        uint64_t frame=stats(context->resources).held_bytes;XrCompilerSession *session=NULL;
        CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"map-rejected",NULL};
        XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
        XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_source_check_text(&request,&(XrXirSourceText){NULL,cases[i],strlen(cases[i])},&output,&diagnostic,NULL);
        fprintf(stderr,"Map negative %u: %u %d:%d %s\n",i,status,diagnostic.line,diagnostic.column,diagnostic.message);
        CHECK(status==XR_XIR_BAD_TYPE&&!output.checked&&diagnostic.status==status);
        CHECK(!strcmp(diagnostic.message,i<2 ? "for-in requires an iterator method returning Iterator" :
            "assignment requires a mutable binding"));
        xr_xir_compile_source_result_free(&output);xr_compile_session_free(session);
        CHECK(stats(context->resources).held_bytes==frame);
    }
}
int main(void) {
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();const XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);uint64_t frame=stats(context.resources).held_bytes;
    rejections(&context);uint32_t ids[3]={0};XrXirProgram *program=compile(&context,ids);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    XrXirValue result=invoke(instance,ids[0],NULL,0);CHECK(result.type==XR_XIR_I64&&result.payload==42);xr_xir_value_drop(&result);
    result=invoke(instance,ids[1],NULL,0);CHECK(result.type==XR_XIR_I64&&result.payload==9);xr_xir_value_drop(&result);
    XrXirValue held=invoke(instance,ids[2],NULL,0);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={xr_xir_value_arena(&held),domain,NULL,NULL,100000,65536};
    int64_t length=0;CHECK(xr_xir_array_len(&held,&admission,&length)==XR_XIR_VALUE_OK&&length==2);
    const char *expected[]={"\xe4\xb8\xad","\xf0\x9f\x98\x80"};
    for(int64_t i=0;i<2;++i){XrXirValue item={0};XrXirFaultDetail fault={0};const char *bytes=NULL;size_t count=0;
        CHECK(xr_xir_array_get(&held,i,&admission,&item,&fault)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_string_view(&item,&bytes,&count)&&count==strlen(expected[i])&&!memcmp(bytes,expected[i],count));xr_xir_value_drop(&item);}
    xr_xir_value_drop(&held);xr_xir_domain_drop(domain);
    CHECK(stats(context.resources).held_bytes==frame);xr_compile_resources_release(context.resources);
    puts("Map lexical snapshot, exits, outer writes and retired values PASS");return 0;
}
