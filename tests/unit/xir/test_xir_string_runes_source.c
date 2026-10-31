/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_string_runes_source.c - Authenticated owned Rune generator lifetimes
 *
 * KEY CONCEPT:
 *   Real Source, packet and Program owners retire before captured strings and
 *   their live Iterator values leave the ordinary executor protocol.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_program.h"
#include "xir/xxir_types.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"Rune owner %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static const char source_text[]=
 "export fn make(value:string)->Iterator<rune>{return value.runes()}\n"
 "export fn take(it:Iterator<rune>)->i64{return it.next().toUInt32() as i64}\n"
 "export fn more(it:Iterator<rune>)->bool{return it.hasNext()}\n";
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
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"rune-owned",NULL};
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
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t helpers=0;
    for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length>=13&&!memcmp(module->functions[f].name,"$string_runes",13)) {
        const XrXirFunctionIdentity *identity=&module->declarations->functions[f];const XrXirFunction *body=&module->functions[f];
        CHECK(identity->promises==XR_XIR_FUNCTION_GENERATOR&&!identity->exported&&!identity->nominal_owner&&!identity->member_access&&!identity->cleanup_owner&&!identity->native_callable&&!identity->test_role);
        CHECK(identity->method_kind==XR_XIR_NON_MEMBER&&(!module->generics||!module->generics[f].parameter_count)&&body->parameter_count==1&&body->parameters[0]==XR_XIR_STRING);
        const XrXirTypeNode *type=xr_xir_type_node(module->types,body->result);CHECK(type&&type->kind==XR_XIR_TYPE_ITERATOR&&type->element==XR_XIR_RUNE&&type->result==XR_XIR_NEVER);++helpers;
    }
    CHECK(helpers==1);ids[0]=named(module,"make");ids[1]=named(module,"take");ids[2]=named(module,"more");XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK&&!lowered);return program;
}
static void rejections(XrXirCompileContext *context) {
    const char *cases[]={"const bad=\"x\".runes(1)\n","const bad=\"x\".runes<i64>()\n"};
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        uint64_t frame=stats(context->resources).held_bytes;XrCompilerSession *session=NULL;
        CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"rune-rejected",NULL};
        XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
        XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_source_check_text(&request,&(XrXirSourceText){NULL,cases[i],strlen(cases[i])},&output,&diagnostic,NULL);
        CHECK(status==XR_XIR_BAD_TYPE&&!output.checked&&diagnostic.line==1&&diagnostic.column==15);
        CHECK(!strcmp(diagnostic.message,"Rune producer requires its exact String receiver declaration"));
        xr_xir_compile_source_result_free(&output);xr_compile_session_free(session);
        CHECK(stats(context->resources).held_bytes==frame);
    }
}
int main(void) {
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();const XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);uint64_t frame=stats(context.resources).held_bytes;
    rejections(&context);uint32_t ids[3]={0};XrXirProgram *program=compile(&context,ids);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *first=NULL,*second=NULL;CHECK(xr_xir_instance_new(program,&config,&first)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&second)==XR_XIR_CALL_READY);
    uint32_t make=ids[0],take=ids[1],more=ids[2];xr_xir_compile_program_drop(program);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);uint64_t runtime_frame=xr_xir_domain_stats(domain).live_bytes;
    const char text[]="A\0\xe4\xb8\xad\xf0\x9f\x98\x80Z";XrXirValue input={0};CHECK(xr_xir_string_new(domain,text,sizeof(text)-1,&input)==XR_XIR_VALUE_OK);
    XrXirValue iterator=invoke(first,make,&input,1);xr_xir_value_drop(&input);CHECK(xr_xir_domain_stats(domain).live_bytes>runtime_frame);
    CHECK(xr_xir_instance_start(second,take,&iterator,1)==XR_XIR_CALL_BAD_ARGUMENT);
    const int64_t scalars[]={65,0,20013,128512,90};
    for(unsigned i=0;i<sizeof(scalars)/sizeof(*scalars);++i){for(unsigned repeat=0;repeat<2;++repeat){XrXirValue yes=invoke(first,more,&iterator,1);CHECK(yes.type==XR_XIR_BOOL&&yes.payload==1);xr_xir_value_drop(&yes);}XrXirValue scalar=invoke(first,take,&iterator,1);CHECK(scalar.type==XR_XIR_I64&&scalar.payload==scalars[i]);xr_xir_value_drop(&scalar);}
    XrXirValue no=invoke(first,more,&iterator,1);CHECK(no.type==XR_XIR_BOOL&&!no.payload);xr_xir_value_drop(&no);xr_xir_value_drop(&iterator);
    CHECK(xr_xir_string_new(domain,"owned",5,&input)==XR_XIR_VALUE_OK);iterator=invoke(first,make,&input,1);xr_xir_value_drop(&input);
    CHECK(xr_xir_instance_free(first)==XR_XIR_CALL_READY);CHECK(xr_xir_instance_free(second)==XR_XIR_CALL_READY);
    xr_xir_value_drop(&iterator);CHECK(xr_xir_domain_stats(domain).live_bytes==runtime_frame);xr_xir_domain_drop(domain);
    CHECK(stats(context.resources).held_bytes==frame);xr_compile_resources_release(context.resources);
    puts("Real Rune helper: packet, fresh role, capture, foreign rejection and retirement PASS");return 0;
}
