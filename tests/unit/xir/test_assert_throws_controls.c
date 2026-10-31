/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_assert_throws_controls.c - Exact typed Error expectations and owner exits
 *
 * KEY CONCEPT:
 *   Only a real action Error satisfies the expectation. Other terminal channels
 *   and argument failures retain their actual owners and independent status.
 */
#include "program/xr_xir_source_product.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_output.h"
#include "base/xmalloc.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"Throws control %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static const char source_text[]=
    "enum Problem{Bad{message:string}}\n"
    "fn fail() throws(Problem)->string{throw Problem.Bad{message:\"owned\"}}\n"
    "export fn typed()->i64{assertThrows(fail);return 42}\n"
    "export fn unit()->i64{assertThrows(fn(){throw Problem.Bad{message:\"unit\"}});return 42}\n"
    "export fn normal_int(){assertThrows(fn(){return 7},\"missing\")}\n"
    "export fn normal_string(){assertThrows(fn(){return \"owned\"},\"missing\")}\n"
    "export fn normal_array(){assertThrows(fn(){return [\"owned\",\"values\"]},\"missing\")}\n"
    "export fn panic_action(){assertThrows(fn(){assert(false,\"inner\")},\"outer\")}\n"
    "export fn ordered()->i64{var order=0\n"
    "fn action()->fn() throws(Error)->string{order=order*10+1;return fail}\n"
    "fn message()->string{order=order*10+2;return \"ignored\"}\n"
    "assertThrows(action(),message());return order}\n"
    "export fn shadow()->i64{const assertThrows=fn(){return 1}\n"
    "return assertThrows()}\n"
    "fn message_error() throws(Problem)->string{throw Problem.Bad{message:\"argument\"}}\n"
    "export fn argument_error() throws(Problem){assertThrows(fail,try message_error())}\n"
    "export fn endless(){assertThrows(fn(){print(17);while(true){}})}\n"
    "export fn normal_unit(){assertThrows(fn(){},\"missing\")}\n";
typedef struct Output{char bytes[32];size_t count;}Output;
static XrXirOutputStatus output_bytes(void *context,XrXirOutputStream stream,const char *bytes,size_t length){
    Output *out=context;CHECK(stream==XR_XIR_STDOUT&&length<=sizeof(out->bytes)-out->count);
    memcpy(out->bytes+out->count,bytes,length);out->count+=length;return XR_XIR_OUTPUT_OK;
}
static uint64_t held(XrCompileResources *resources){XrCompileResourceStats s={0};CHECK(xr_compile_resources_stats(resources,&s)==XR_COMPILE_RESOURCE_OK);return s.held_bytes;}
static uint32_t named(const XrXirModule *module,const char *name){
    for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==strlen(name)&&!memcmp(module->functions[f].name,name,strlen(name)))return f;
    CHECK(false);return 0;
}
static void forged_recipe(const XrXirCompileContext *context,const XrXirSourceProduct *product){
    XrXirSourceProductPacketView packet={0};XrXirArtifact *decoded=NULL;
    CHECK(xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_SOURCE,&packet)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(decoded);uint32_t f=named(module,"assertThrows");
    CHECK(xr_xir_compile_result_binders_verify(context,module)==XR_XIR_OK);
    size_t size=(size_t)module->function_count*sizeof(*module->functions);XrXirFunction *functions=xr_malloc(size);CHECK(functions);memcpy(functions,module->functions,size);
    XrXirInstruction instructions[8];CHECK(functions[f].instruction_count==8);memcpy(instructions,functions[f].instructions,sizeof(instructions));functions[f].instructions=instructions;
    XrXirModule altered=*module;altered.functions=functions;uint64_t before=held(context->resources);
    for(unsigned mode=0;mode<3;++mode){
        memcpy(instructions,module->functions[f].instructions,sizeof(instructions));
        if(mode==0)instructions[3].immediate=1;
        else if(mode==1)instructions[6].type=XR_XIR_I64;
        else instructions[2].op=XR_XIR_RETURN;
        XrXirArtifact *rejected=NULL;XrXirDiagnostic diagnostic={0};
        CHECK(xr_xir_compile_result_binders_verify(context,&altered)==XR_XIR_BAD_TYPE);
        XrXirStatus status=xr_xir_compile_recheck_v2(context,&altered,xr_xir_compile_artifact_construction(decoded),&rejected,&diagnostic,NULL);
        fprintf(stderr,"forged recipe %u status%u\n",mode,status);CHECK(status==XR_XIR_BAD_TYPE&&!rejected&&held(context->resources)==before);
    }
    xr_free(functions);xr_xir_compile_artifact_free(decoded);
}
static XrXirProgram *build(XrXirCompileContext *context,uint32_t ids[11]){
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"throws-controls",NULL};
    XrXirSourceProductRequest request={{session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL},{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *product=NULL;XrXirSourceProductDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_product_build_text(&request,&(XrXirSourceText){NULL,source_text,sizeof(source_text)-1},&product,&diagnostic);
    if(status!=XR_XIR_OK){XrFrontendRoot root={0};if(diagnostic.failure.report)xr_frontend_report_root(diagnostic.failure.report,0,&root);fprintf(stderr,"Source %u stage%u %d:%d %s\n",status,diagnostic.stage,root.primary.line,root.primary.column,root.message?root.message:"");}
    CHECK(status==XR_XIR_OK&&product);xr_compile_session_free(session);
    forged_recipe(context,product);
    const char *names[]={"typed","unit","normal_int","normal_string","normal_array","panic_action","ordered","shadow","argument_error","endless","normal_unit"};
    const XrXirModule *module=xr_xir_compile_source_product_module(product);for(unsigned i=0;i<11;++i)ids[i]=named(module,names[i]);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_source_product_vm_take(product,&program)==XR_XIR_OK&&program);xr_xir_compile_source_product_free(product);return program;
}
static void outcomes(XrXirProgram *program,const uint32_t ids[11]){
    for(unsigned i=0;i<12;++i){
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        Output output={0};XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,output_bytes,&output,sizeof(output.bytes)};config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sink};
        if(i==9)config.poll_limit=1000;
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,ids[i<11?i:9],NULL,0)==XR_XIR_CALL_READY);
        XrXirCallResult result={0};
        if(i==11){result=xr_xir_instance_poll_bounded(instance,10000).outcome;CHECK(result.status==XR_XIR_CALL_READY&&output.count==3&&!memcmp(output.bytes,"17\n",3));CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);}
        for(unsigned p=0;;++p){CHECK(p<10000);result=xr_xir_instance_poll_bounded(instance,1024).outcome;if(result.status!=XR_XIR_CALL_READY)break;}
        XrXirCallStatus expected=i<2||i==6||i==7?XR_XIR_CALL_RETURNED:i<6||i==10?XR_XIR_CALL_ASSERTION:i==8?XR_XIR_CALL_THROWN:i==9?XR_XIR_CALL_LIMIT:XR_XIR_CALL_CANCELLED;
        fprintf(stderr,"outcome %u status%u expected%u\n",i,result.status,expected);CHECK(result.status==expected);
        if((i>=2&&i<6)||i==10){const char *bytes=NULL;size_t length=0;const char *text=i==5?"inner":"missing";CHECK(result.panic.detail.code==445&&xr_xir_string_view(&result.panic.message,&bytes,&length)&&length==strlen(text)&&!memcmp(bytes,text,length));}
        if(i==9)CHECK(output.count==3&&!memcmp(output.bytes,"17\n",3));
        if(expected==XR_XIR_CALL_RETURNED||expected==XR_XIR_CALL_THROWN){XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==expected);if(expected==XR_XIR_CALL_RETURNED)CHECK(value.type==XR_XIR_I64&&value.payload==(i==6?12:i==7?1:42));xr_xir_value_drop(&value);}
        CHECK(xr_xir_instance_free(instance)==(i==9?XR_XIR_CALL_LIMIT:XR_XIR_CALL_READY));
    }
}
static void source_rejections(XrXirCompileContext *context){
    const char *cases[]={"assertThrows(fn(a:i64){})\n","assertThrows(fn(){},3)\n","fn invented(action:fn()->R){}\n"};
    for(unsigned i=0;i<3;++i){uint64_t before=held(context->resources);XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"throws-rejected",NULL};XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};XrXirStatus status=xr_xir_compile_source_check_text(&request,&(XrXirSourceText){NULL,cases[i],strlen(cases[i])},&output,&diagnostic,NULL);fprintf(stderr,"Source rejection %u status%u %s\n",i,status,diagnostic.message);CHECK(status==XR_XIR_BAD_TYPE&&!output.checked);xr_xir_compile_source_result_free(&output);xr_compile_session_free(session);CHECK(held(context->resources)==before);}
}
int main(void){
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();const XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);uint64_t before=held(context.resources);
    source_rejections(&context);uint32_t ids[11];XrXirProgram *program=build(&context,ids);outcomes(program,ids);xr_xir_compile_program_drop(program);CHECK(held(context.resources)==before);xr_compile_resources_release(context.resources);
    puts("Throws exact recipe, result disposal, channels, order, shadow and owners PASS");return 0;
}
