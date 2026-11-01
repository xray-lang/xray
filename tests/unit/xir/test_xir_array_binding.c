/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"Array binding %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "array_binding_fixture.h"
#include "array_binding_runtime.h"
static XrXirStatus array_binding_check(const XrXirCompileContext *context,const char *text,size_t count,XrXirSourceResult *result) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"array-binding",NULL};
    XrXirSourceRequest request={.session=session,.authority=&authority,.context=context,.linkage_kind=XR_XIR_PROGRAM};
    XrXirSourceText input={.text=text,.length=count};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"Array Source%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    xr_compile_session_free(session);return status;
}
static void array_binding_refusals(const XrXirCompileContext *context) {
    static const char *const inputs[]={
        "var [a]=(1,)\n", "fn bad(xs:Slice<i64>){var [a]=xs}\n",
        "const [a]=[1]\na=2\n", "var [a,a]=[1,2]\n",
        "var [[a]]=[[1]]\n", "var [a,...rest]=[1,2]\n",
        "var p={first:1}\nvar {missing}=p\n", "var [a]=[1]\nprint(missing)\n"};
    XrCompileResourceStats before,after;CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    for(size_t i=0;i<sizeof(inputs)/sizeof(*inputs);++i) {
        XrXirSourceResult result={0};CHECK(array_binding_check(context,inputs[i],strlen(inputs[i]),&result)!=XR_XIR_OK);
        CHECK(!result.checked&&!result.snapshot);xr_xir_compile_source_result_free(&result);
        CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes);
    }
    XrXirCompileContext small=*context;small.limits.instructions=1;XrXirSourceResult failed={0};
    CHECK(array_binding_check(&small,array_binding_source,sizeof(array_binding_source)-1,&failed)==XR_XIR_BUDGET);
    CHECK(!failed.checked&&!failed.snapshot);xr_xir_compile_source_result_free(&failed);
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes);
}
static void array_binding_order(const XrXirModule *module) {
    unsigned checked=0;
    for(uint32_t f=0;f<module->function_count;++f)for(unsigned n=1;n<=4;++n) {
        const XrXirFunction *function=&module->functions[f];
        if(function->name_length!=strlen(array_binding_names[n])||memcmp(function->name,array_binding_names[n],function->name_length))continue;
        bool asserted=false;
        for(uint32_t i=0;i<function->instruction_count;++i) {
            XrXirOp op=function->instructions[i].op;
            if(op==XR_XIR_ASSERT_CONDITION)asserted=true;
            if(op==XR_XIR_ARRAY_GET||op==XR_XIR_CELL_NEW)CHECK(asserted);
        }
        CHECK(asserted);++checked;
    }
    CHECK(checked==4);
}
static void array_binding_one(const XrXirCompileContext *context,const char *text,size_t count,bool original,const char *path) {
    XrXirSourceResult source={0};CHECK(array_binding_check(context,text,count,&source)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    if(!original)array_binding_order(xr_xir_compile_artifact_module(source.checked));
    xr_xir_compile_source_result_free(&source);XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    const XrXirTarget target={XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t ids[6]={0};
    if(original)ids[0]=module->declarations->entry_function;
    else for(unsigned i=0;i<6;++i) {
        ids[i]=UINT32_MAX;
        for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==strlen(array_binding_names[i])&&
            !memcmp(module->functions[f].name,array_binding_names[i],strlen(array_binding_names[i])))ids[i]=f;
        CHECK(ids[i]!=UINT32_MAX);
    }
    if(path) {
        const char *prefix=original?"array_variables":"array_binding";XrXirCSource code={0};
        CHECK(xr_xir_compile_emit_c(lowered,prefix,8388608,&code)==XR_XIR_OK);FILE *file=fopen(path,"wb");CHECK(file);
        CHECK(fwrite(code.text,1,code.length,file)==code.length);
        CHECK(fprintf(file,"\nconst uint32_t %s_ids[6]={%uu,%uu,%uu,%uu,%uu,%uu};\n",prefix,ids[0],ids[1],ids[2],ids[3],ids[4],ids[5])>0&&!fclose(file));
        xr_xir_compile_c_source_free(&code);
    }
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK&&!lowered);
    array_binding_run(program,ids,original);
}
int main(int argc,char **argv) {
    CHECK(argc==2||argc==4);XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    array_binding_refusals(&context);
    array_binding_one(&context,array_binding_source,sizeof(array_binding_source)-1,false,argc==4?argv[2]:NULL);
    FILE *file=fopen(argv[1],"rb");CHECK(file&&!fseek(file,0,SEEK_END));long count=ftell(file);CHECK(count>0&&!fseek(file,0,SEEK_SET));
    char *text=malloc((size_t)count);CHECK(text&&fread(text,1,(size_t)count,file)==(size_t)count&&!fclose(file));
    array_binding_one(&context,text,(size_t)count,true,argc==4?argv[3]:NULL);memset(text,'!',(size_t)count);free(text);
    xr_compile_resources_release(context.resources);CHECK(!runtime_live&&!runtime_bytes&&!source_fixture_compile_live&&!source_fixture_compile_bytes);
    puts("Exact Array binding and complete original variables VM/retirement PASS");return 0;
}
