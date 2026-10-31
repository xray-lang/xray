/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_array_materialization.c - Complete Array source, typed refusals and cold owner retirement
 */
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"Array materialization %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "array_materialization_fixture.h"
#include "array_materialization_runtime.h"
static XrXirStatus array_materialization_check(const XrXirCompileContext *context,const char *text,size_t count,XrXirSourceResult *result) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"tuple-assignment",NULL};
    XrXirSourceRequest request={.session=session,.authority=&authority,.context=context,.linkage_kind=XR_XIR_PROGRAM};
    XrXirSourceText input={.text=text,.length=count};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"Array Source%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    xr_compile_session_free(session);return status;
}
static void array_materialization_refusals(const XrXirCompileContext *context) {
    static const char *const inputs[]={
        "fn fresh()->Array<i64>{return [1]};var bad=copy fresh()[0:1]\n",
        "var bad=copy [1,2][0:1]\n",
        "var values=[1.0];values.sort()\n",
        "var values=[\"b\",\"a\"];values.sort()\n",
        "var values:Array<u8>=[2,1];values.sort()\n",
        "var values=[2,1];values.sort(fn(a:i64,b:i64)->i64{return a-b})\n",
        "const values=[2,1];values.sort()\n",
        "var values=[1];var bad=copy values[false:1]\n",
        "var values=[\"x\"];var bad=copy values[:]\n",
        "var values=[1];var view:Slice<i64>=values[:];view[0]=2\n",
        "var values=[1];var view:Slice<i64>=values[:];var owned=copy values[:];values[0]=2;print(view[0])\n",
        "var bad:Array<Slice<u8>>=[]\n",
        "var stream=\"x\".runes();var values=[1];var owned=copy values[:]\n",
    };
    static const XrXirStatus expected[]={XR_XIR_BAD_TYPE,XR_XIR_BAD_STRUCTURE,XR_XIR_BAD_TYPE,
        XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE,
        XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE,XR_XIR_UNRESOLVED};
    _Static_assert(sizeof(inputs)/sizeof(*inputs)==sizeof(expected)/sizeof(*expected),"each input preserves its actual typed frontier");
    XrCompileResourceStats before,after;CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    for(size_t i=0;i<sizeof(inputs)/sizeof(*inputs);++i) {
        XrXirSourceResult result={0};XrXirStatus status=array_materialization_check(context,inputs[i],strlen(inputs[i]),&result);
        fprintf(stderr,"Array refusal%zu status=%u\n",i,status);
        CHECK(status==expected[i] && !result.checked && !result.snapshot);xr_xir_compile_source_result_free(&result);
        CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);
    }
    fprintf(stderr,"Array budget begin\n");
    XrXirCompileContext small=*context;small.limits.instructions=1;XrXirSourceResult failed={0};
    CHECK(array_materialization_check(&small,array_materialization_source,sizeof(array_materialization_source)-1,&failed)==XR_XIR_BUDGET);
    CHECK(!failed.checked&&!failed.snapshot);xr_xir_compile_source_result_free(&failed);
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);
}
static bool array_cancel(const void *data) {unsigned *calls=(unsigned *)data;++*calls;return true;}
static void array_plain_control(const XrXirCompileContext *context,const XrXirSourceResult *source) {
    const XrXirModule *module=xr_xir_compile_artifact_module(source->checked);
    const XrXirConstruction *construction=xr_xir_compile_artifact_construction(source->checked);
    XrCompileResourceStats before,after;
    CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    for(unsigned i=0;i<2;++i) {
        unsigned calls=0;XrCompileRequestControl control={0};
        XrCompileRequestOptions options={.cancel_poll=array_cancel,.cancel_context=&calls};
        if(i)options=(XrCompileRequestOptions){.deadline_mode=XR_COMPILE_DEADLINE_AFTER};
        CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        XrXirStatus status=xir_compile_view_cell_access_verified_input(context,(XirEffectInput){module,construction,&control},NULL);
        fprintf(stderr,"Array plain request%u status%u polls%u\n",i,status,calls);
        CHECK(status==(i?XR_XIR_DEADLINE:XR_XIR_CANCELLED) && calls==(i?0u:1u));
        CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK && before.held_bytes==after.held_bytes);
    }
    CHECK(xir_compile_view_cell_access_verified_input(context,(XirEffectInput){module,construction,NULL},NULL)==XR_XIR_OK);
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK && before.held_bytes==after.held_bytes);
}
static uint32_t array_named(const XrXirModule *module,const char *name) {
    for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==strlen(name) &&
        !memcmp(module->functions[f].name,name,strlen(name)))return f;
    CHECK(false);return UINT32_MAX;
}
static void array_error_order(const XrXirModule *module) {
    uint32_t fail=array_named(module,"failLower"),later=array_named(module,"laterUpper");
    const char *names[]={"borrowError","copyError"};
    for(unsigned k=0;k<2;++k) {
        const XrXirFunction *body=module->functions+array_named(module,names[k]);
        uint32_t lower=UINT32_MAX,upper=UINT32_MAX,loan=UINT32_MAX;
        for(uint32_t i=0;i<body->instruction_count;++i) {
            const XrXirInstruction *op=body->instructions+i;
            if((op->op==XR_XIR_INVOKE || op->op==XR_XIR_CALL) && op->immediate==fail){CHECK(lower==UINT32_MAX);lower=i;}
            if((op->op==XR_XIR_INVOKE || op->op==XR_XIR_CALL) && op->immediate==later){CHECK(upper==UINT32_MAX);upper=i;}
            if(op->op==XR_XIR_VIEW_NEW && loan==UINT32_MAX)loan=i;
        }
        if(lower==UINT32_MAX || upper==UINT32_MAX || loan==UINT32_MAX) {
            fprintf(stderr,"Array error order %s fail%u later%u lower%u upper%u loan%u\n",names[k],fail,later,lower,upper,loan);
            for(uint32_t i=0;i<body->instruction_count;++i)fprintf(stderr,"order %u op%u immediate%lld\n",i,(unsigned)body->instructions[i].op,(long long)body->instructions[i].immediate);
        }
        CHECK(lower!=UINT32_MAX && upper!=UINT32_MAX && loan!=UINT32_MAX && lower<upper && upper<loan);
    }
}
static void array_materialization_one(const XrXirCompileContext *context,const char *text,size_t count,bool original,const char *path) {
    XrXirSourceResult source={0};CHECK(array_materialization_check(context,text,count,&source)==XR_XIR_OK);
    if(original)array_plain_control(context,&source);
    else array_error_order(xr_xir_compile_artifact_module(source.checked));
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t ids[ARRAY_MATERIALIZATION_ENTRY_COUNT]={0};
    if(original)ids[0]=module->declarations->entry_function;
    else for(unsigned i=0;i<ARRAY_MATERIALIZATION_ENTRY_COUNT;++i) {
        ids[i]=UINT32_MAX;
        for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==strlen(array_materialization_names[i])&&
            !memcmp(module->functions[f].name,array_materialization_names[i],strlen(array_materialization_names[i])))ids[i]=f;
        CHECK(ids[i]!=UINT32_MAX);
    }
    if(path) {
        const char *prefix=original?"array_demo":"array_materialization";XrXirCSource code={0};
        CHECK(xr_xir_compile_emit_c(lowered,prefix,8388608,&code)==XR_XIR_OK);FILE *file=fopen(path,"wb");CHECK(file);
        CHECK(fwrite(code.text,1,code.length,file)==code.length);
        CHECK(fprintf(file,"\nconst uint32_t %s_ids[%u]={",prefix,ARRAY_MATERIALIZATION_ENTRY_COUNT)>0);
        for(unsigned i=0;i<ARRAY_MATERIALIZATION_ENTRY_COUNT;++i)CHECK(fprintf(file,"%s%uu",i?",":"",ids[i])>0);
        CHECK(fprintf(file,"};\n")>0&&!fclose(file));
        xr_xir_compile_c_source_free(&code);
    }
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK&&!lowered);
    array_run(program,ids,original,context);
}
int main(int argc,char **argv) {
    CHECK(argc==2||argc==4);XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    array_materialization_refusals(&context);
    fprintf(stderr,"Array positive begin\n");
    array_materialization_one(&context,array_materialization_source,sizeof(array_materialization_source)-1,false,argc==4?argv[2]:NULL);
    FILE *file=fopen(argv[1],"rb");CHECK(file&&!fseek(file,0,SEEK_END));long count=ftell(file);CHECK(count>0&&!fseek(file,0,SEEK_SET));
    char *text=xr_malloc((size_t)count);CHECK(text&&fread(text,1,(size_t)count,file)==(size_t)count&&!fclose(file));
    array_materialization_one(&context,text,(size_t)count,true,argc==4?argv[3]:NULL);memset(text,'!',(size_t)count);xr_free(text);
    xr_compile_resources_release(context.resources);CHECK(!runtime_live&&!runtime_bytes&&!source_fixture_compile_live&&!source_fixture_compile_bytes&&array_raw_view_checks==5);
    puts("Exact Array materialization and complete arrays VM/retirement PASS");return 0;
}
