/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_typed_display_source.c - Owned text through Source and cold Checked
 *
 * KEY CONCEPT:
 *   Real rendering runs after Source and its syntax owner have retired.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_program.h"
#include "xir/xxir_output.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"display %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "typed_display_cases.h"
static uint64_t held(XrCompileResources *resources) {
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(resources,&stats)==XR_COMPILE_RESOURCE_OK);return stats.held_bytes;
}
static XrXirStatus source_check(XrXirCompileContext *context,const char *text,XrXirSourceResult *out,XrXirSourceDiagnostic *diagnostic) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"typed-display",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&(XrXirSourceText){NULL,text,strlen(text)},out,diagnostic,NULL);
    xr_compile_session_free(session);return status;
}
static void rejected(XrXirCompileContext *context) {
    const char *cases[]={"print(())\n","print(fn(x:i64)->i64{return x})\n",
        "struct Secret {value:i64=1}\nprint(Secret())\n",
        "class Private {value:i64=1}\nprint(Private())\n",
        "class User {toString()->string{return \"hidden\"}}\nprint(User())\n"};
    for(size_t i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        uint64_t frame=held(context->resources);XrXirSourceResult out={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=source_check(context,cases[i],&out,&diagnostic);
        fprintf(stderr,"display rejection %zu: %u %d:%d %s\n",i,status,diagnostic.line,diagnostic.column,diagnostic.message);
        CHECK(status==XR_XIR_BAD_TYPE && diagnostic.status==status && !out.checked);
        CHECK(!strcmp(diagnostic.message,i?"value has no admitted builtin display":"unit argument is not admitted"));
        xr_xir_compile_source_result_free(&out);CHECK(held(context->resources)==frame);
    }
}
static XrXirProgram *program(XrXirCompileContext *context,const char *text,uint32_t ids[2],bool named) {
    XrXirSourceResult out={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=source_check(context,text,&out,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"display Source %u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && out.checked);
    XrXirCheckedPacket packet={0};XrXirArtifact *decoded=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(out.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&out);
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(decoded,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(decoded);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},NULL,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    const char *names[]={"render","cow"};
    if(!named)ids[0]=module->declarations->entry_function;
    else for(unsigned n=0;n<2;++n){ids[n]=UINT32_MAX;for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==strlen(names[n])&&!memcmp(module->functions[f].name,names[n],strlen(names[n])))ids[n]=f;
        CHECK(ids[n]!=UINT32_MAX);}
    XrXirProgram *output=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&output)==XR_XIR_OK && !lowered);return output;
}
static XrXirValue invoke(XrXirInstance *instance,uint32_t function) {
    CHECK(xr_xir_instance_start(instance,function,NULL,0)==XR_XIR_CALL_READY);XrXirCallResult result={0};
    for(unsigned i=0;;++i){CHECK(i<10000);result=xr_xir_instance_poll_bounded(instance,1024).outcome;if(result.status!=XR_XIR_CALL_READY)break;}
    CHECK(result.status==XR_XIR_CALL_RETURNED);XrXirValue output={0};CHECK(xr_xir_instance_take_result(instance,&output)==XR_XIR_CALL_RETURNED);return output;
}
typedef struct DisplayOutput {char bytes[2048];size_t count;unsigned groups;} DisplayOutput;
static XrXirOutputStatus output_bytes(void *context,XrXirOutputStream stream,const char *bytes,size_t size) {
    DisplayOutput *out=context;CHECK(stream==XR_XIR_STDOUT && size<=sizeof(out->bytes)-out->count);
    memcpy(out->bytes+out->count,bytes,size);out->count+=size;++out->groups;return XR_XIR_OUTPUT_OK;
}
static size_t input(const char *root,const char *relative,char *bytes,size_t capacity) {
    char path[2048];int size=snprintf(path,sizeof(path),"%s/%s",root,relative);CHECK(size>0 && (size_t)size<sizeof(path));
    FILE *file=fopen(path,"rb");CHECK(file);size_t count=fread(bytes,1,capacity-1,file);CHECK(!ferror(file) && feof(file) && fclose(file)==0);
    bytes[count]=0;return count;
}
static void original_map(XrXirCompileContext *context,const char *root) {
    char text[8192],expected[2048];(void)input(root,"demos/02-collections/maps_and_sets.xr",text,sizeof(text));
    size_t length=input(root,"tests/smoke/expected/maps_and_sets.txt",expected,sizeof(expected));
    uint64_t frame=held(context->resources);uint32_t ids[2]={0};XrXirProgram *owner=program(context,text,ids,false);
    DisplayOutput output={0};XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,output_bytes,&output,sizeof(output.bytes)};
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sink};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(owner,&config,&instance)==XR_XIR_CALL_READY);
    XrXirValue answer=invoke(instance,ids[0]);xr_xir_value_drop(&answer);
    CHECK(output.groups==16 && output.count==length && !memcmp(output.bytes,expected,length));
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(owner);CHECK(held(context->resources)==frame);
}
int main(int argc,char **argv) {
    CHECK(argc==2);
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&(XrCompileResourceLimits){XR_COMPILE_MEMORY_ORDINARY,0,true},&context.resources)==XR_COMPILE_RESOURCE_OK);
    uint64_t frame=held(context.resources);rejected(&context);uint32_t ids[2]={0};XrXirProgram *owner=program(&context,display_source,ids,true);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(owner,&config,&instance)==XR_XIR_CALL_READY);
    XrXirValue text=invoke(instance,ids[0]),answer=invoke(instance,ids[1]);CHECK(answer.type==XR_XIR_I64 && answer.payload==42);
    xr_xir_value_drop(&answer);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(owner);
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&text,&bytes,&length));
    CHECK(length==sizeof(display_expected)-1 && !memcmp(bytes,display_expected,length));xr_xir_value_drop(&text);
    CHECK(held(context.resources)==frame);original_map(&context,argv[1]);xr_compile_resources_release(context.resources);
    puts("Typed display: exact text, COW, ordered effects, cold packet and retired result PASS");return 0;
}
