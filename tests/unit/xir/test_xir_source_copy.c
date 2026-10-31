/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_copy.c - Contextual copy positions through owned Source, packets, VM and C.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"source copy %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "source_copy_fixture.h"
#include "source_copy_runtime.h"
static XrXirStatus source_copy_check(const XrXirCompileContext *context,const char *text,XrXirSourceResult *result) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"source-copy",NULL};
    XrXirSourceRequest request={.session=session,.authority=&authority,.context=context,.linkage_kind=XR_XIR_PROGRAM};
    XrXirSourceText input={.text=text,.length=strlen(text)};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,result,&diagnostic,NULL);
    fprintf(stderr,"copy Source%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    xr_compile_session_free(session);return status;
}
static uint32_t source_copy_entry(const XrXirModule *module,const char *name) {
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==strlen(name) && !memcmp(module->functions[f].name,name,strlen(name)))return f;
    CHECK(false);return UINT32_MAX;
}
static void source_copy_refusals(const XrXirCompileContext *context) {
    static const char *cases[]={
        "fn value()->i64{return 1}\nexport fn bad()->i64{return copy value()}\n",
        "export fn bad()->i64{return copy 1}\n",
        "export fn bad()->i64{var x=\"text\";return copy x}\n",
        "fn bad(view:Slice<i64>)->i64{const owned=copy view;return 0}\n",
        "class Box{value:i64;init(){this.value=1}}\nfn worker(x:Box)->i64{return x.value}\nvar x=Box();var task=go worker(copy x)\n",
        "var privateState=1\nfn worker(xs:Array<i64>)->i64{return privateState+xs[0]}\nvar xs=[41];var task=go worker(copy xs)\n"};
    XrCompileResourceStats baseline={0},after={0};CHECK(xr_compile_resources_stats(context->resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        XrXirSourceResult result={0};CHECK(source_copy_check(context,cases[i],&result)!=XR_XIR_OK && !result.checked && !result.snapshot);
        xr_xir_compile_source_result_free(&result);CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==baseline.held_bytes);
    }
    XrXirCompileContext small=*context;small.limits.instructions=1;XrXirSourceResult failed={0};
    CHECK(source_copy_check(&small,source_copy_fixture,&failed)==XR_XIR_BUDGET && !failed.checked && !failed.snapshot);
    xr_xir_compile_source_result_free(&failed);CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==baseline.held_bytes);
}
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    source_copy_refusals(&context);char text[sizeof(source_copy_fixture)];memcpy(text,source_copy_fixture,sizeof(text));
    XrXirSourceResult source={0};CHECK(source_copy_check(&context,text,&source)==XR_XIR_OK && source.checked && source.snapshot);memset(text,'!',sizeof(text));
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&source);
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};CHECK(xr_xir_compile_lower(closed,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t entries[2]={source_copy_entry(module,"answer"),source_copy_entry(module,"kept")};
    if(argc==2) {
        XrXirCSource c={0};CHECK(xr_xir_compile_emit_c(lowered,"source_copy",8388608,&c)==XR_XIR_OK);
        FILE *file=fopen(argv[1],"wb");CHECK(file && fwrite(c.text,1,c.length,file)==c.length);
        CHECK(fprintf(file,"\nconst uint32_t source_copy_selection[2]={%uu,%uu};\n",entries[0],entries[1])>0 && !fclose(file));xr_xir_compile_c_source_free(&c);
    }
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    xr_compile_resources_release(context.resources);XrXirValue kept=source_copy_run(program,entries);source_copy_retired(&kept);
    puts("Explicit copy: typed positions, lexical call, nested COW, identity, finite refusal and retired String PASS");return 0;
}
