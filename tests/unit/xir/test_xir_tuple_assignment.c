/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_tuple_assignment.c - Complete Tuple source, typed refusals and cold owner retirement
 */
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"Tuple assignment %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "tuple_assignment_fixture.h"
#include "tuple_assignment_runtime.h"
static XrXirStatus tuple_assignment_check(const XrXirCompileContext *context,const char *text,size_t count,XrXirSourceResult *result) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"tuple-assignment",NULL};
    XrXirSourceRequest request={.session=session,.authority=&authority,.context=context,.linkage_kind=XR_XIR_PROGRAM};
    XrXirSourceText input={.text=text,.length=count};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"Tuple Source%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    xr_compile_session_free(session);return status;
}
static void tuple_assignment_refusals(const XrXirCompileContext *context) {
    static const char *const inputs[]={
        "const a=1;var b=2;(a,b)=(2,1)\n",
        "var a=(1,_)\n",
        "var a=1;var b=2;(a,b)=(true,1)\n",
        "var a=1;var b=2;(a,b)=(1,2,3)\n",
        "var a=1;(a,missing)=(2,3)\n",
        "var a=1;(a,a)=(2,3)\n",
        "var a=1;var b=2;[a,b]=[2,1]\n",
        "var a=1;var b=2;var c=3;((a,b),c)=((2,1),3)\n",
        "var a=1;(a,...rest)=(2,3)\n",
        "var p={x:1};var a=2;(p.x,a)=(2,1)\n",
        "fn bad<T>(a:T,b:T){(a,b)=(b,a)}\n",
        "for((a,b) in [1]){}\n",
        "for((a,b) in [(1,2,3)]){}\n",
        "for((a,b) in [(1,2)]){a=3}\n",
        "for(((a,b),c) in [((1,2),3)]){}\n",
        "const none:i64?=null;fn bad()->i64{var x:i64?=1;if(x!=null){(x,)=(none,);return x+1};return 0}\n",
        "const none:i64?=null;fn bad(flag:bool)->i64{var x:i64?=1;if(x!=null){while(flag){(x,)=(none,);break};return x+1};return 0}\n",
        "const none:i64?=null;fn bad()->i64{var x:i64?=1;var set=fn(){(x,)=(none,)};if(x!=null){return x+1};return 0}\n"
    };
    static const XrXirStatus expected[]={
        XR_XIR_BAD_TYPE,XR_XIR_BAD_STRUCTURE,XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE,
        XR_XIR_BAD_STRUCTURE,XR_XIR_BAD_STRUCTURE,XR_XIR_BAD_STRUCTURE,XR_XIR_BAD_STRUCTURE,
        XR_XIR_BAD_STRUCTURE,XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE,
        XR_XIR_BAD_STRUCTURE,XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE};
    _Static_assert(sizeof(expected)/sizeof(*expected)==sizeof(inputs)/sizeof(*inputs),"each refusal has a typed outcome");
    XrCompileResourceStats before,after;CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    for(size_t i=0;i<sizeof(inputs)/sizeof(*inputs);++i) {
        XrXirSourceResult result={0};XrXirStatus status=tuple_assignment_check(context,inputs[i],strlen(inputs[i]),&result);
        fprintf(stderr,"Tuple refusal%zu status=%u\n",i,status);
        CHECK(status==expected[i] && !result.checked && !result.snapshot);xr_xir_compile_source_result_free(&result);
        CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);
    }
    fprintf(stderr,"Tuple budget begin\n");
    XrXirCompileContext small=*context;small.limits.instructions=1;XrXirSourceResult failed={0};
    CHECK(tuple_assignment_check(&small,tuple_assignment_source,sizeof(tuple_assignment_source)-1,&failed)==XR_XIR_BUDGET);
    CHECK(!failed.checked&&!failed.snapshot);xr_xir_compile_source_result_free(&failed);
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);
}
static void tuple_assignment_one(const XrXirCompileContext *context,const char *text,size_t count,bool original,const char *path) {
    XrXirSourceResult source={0};CHECK(tuple_assignment_check(context,text,count,&source)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t ids[2]={0};
    if(original)ids[0]=module->declarations->entry_function;
    else for(unsigned i=0;i<2;++i) {
        ids[i]=UINT32_MAX;
        for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==strlen(tuple_assignment_names[i])&&
            !memcmp(module->functions[f].name,tuple_assignment_names[i],strlen(tuple_assignment_names[i])))ids[i]=f;
        CHECK(ids[i]!=UINT32_MAX);
    }
    if(path) {
        const char *prefix=original?"tuple_demo":"tuple_assignment";XrXirCSource code={0};
        CHECK(xr_xir_compile_emit_c(lowered,prefix,8388608,&code)==XR_XIR_OK);FILE *file=fopen(path,"wb");CHECK(file);
        CHECK(fwrite(code.text,1,code.length,file)==code.length);
        CHECK(fprintf(file,"\nconst uint32_t %s_ids[2]={%uu,%uu};\n",prefix,ids[0],ids[1])>0&&!fclose(file));
        xr_xir_compile_c_source_free(&code);
    }
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK&&!lowered);
    tuple_run(program,ids,original);
}
int main(int argc,char **argv) {
    CHECK(argc==2||argc==4);XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    tuple_assignment_refusals(&context);
    fprintf(stderr,"Tuple positive begin\n");
    tuple_assignment_one(&context,tuple_assignment_source,sizeof(tuple_assignment_source)-1,false,argc==4?argv[2]:NULL);
    FILE *file=fopen(argv[1],"rb");CHECK(file&&!fseek(file,0,SEEK_END));long count=ftell(file);CHECK(count>0&&!fseek(file,0,SEEK_SET));
    char *text=xr_malloc((size_t)count);CHECK(text&&fread(text,1,(size_t)count,file)==(size_t)count&&!fclose(file));
    tuple_assignment_one(&context,text,(size_t)count,true,argc==4?argv[3]:NULL);memset(text,'!',(size_t)count);xr_free(text);
    xr_compile_resources_release(context.resources);CHECK(!runtime_live&&!runtime_bytes&&!source_fixture_compile_live&&!source_fixture_compile_bytes);
    puts("Exact Tuple assignment and complete original tuples VM/retirement PASS");return 0;
}
