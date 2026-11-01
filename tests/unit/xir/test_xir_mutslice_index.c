/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Local writable indexing retains full source and independent cold checks. */
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"MutSlice %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "mutslice_index_fixture.h"
#include "mutslice_index_runtime.h"
static XrXirStatus mutslice_check(const XrXirCompileContext *context,const char *text,
    XrCompileRequestControl *control,XrXirSourceResult *out) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    if(control)CHECK(xr_compile_session_bind_request(session,control)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"mutslice-index",NULL};
    XrXirSourceRequest request={.session=session,.authority=&authority,.context=context,.linkage_kind=XR_XIR_PROGRAM};
    XrXirSourceText input={.text=text,.length=strlen(text)};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,out,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"MutSlice Source%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    xr_compile_session_free(session);return status;
}
static bool mutslice_cancel(const void *data){unsigned *calls=(unsigned *)data;++*calls;return true;}
static void mutslice_refusals(const XrXirCompileContext *context) {
    const char *const inputs[]={
        "fn f(){var a=[1];var v:Slice<i64>=a[:];v[0]=2}\n",
        "fn f(){var a:Array<u8>=[1];var v:Slice<u8>=a[:];v[0]=2}\n",
        "fn f(){const a=[1];var v:MutSlice<i64>=a[:];v[0]=2}\n",
        "fn f(){var v:MutSlice<i64>=[1,2][:];v[0]=2}\n",
        "fn f(){var a=[1];var v:MutSlice<i64>=a[:];print(a[0]);print(v[0])}\n",
        "fn f(){var a=[1];var v:MutSlice<i64>=a[:];a[0]=2;print(v[0])}\n",
        "fn f(){var a=[1];var v:MutSlice<i64>=a[:];var stored=a;print(v[0]);print(stored[0])}\n",
        "fn f(){var a=[1];var v:MutSlice<i64>=a[:];a=[2];print(v[0])}\n",
        "fn f(){var a=[1];var v:MutSlice<i64>=a[:];var w:MutSlice<i64>=a[:];print(v[0]);print(w[0])}\n",
        "fn f(){var a=[1];var v:MutSlice<i64>=a[:];var r:Slice<i64>=v[:];v[0]=2;print(r[0])}\n",
        "fn f(){var a=[1];var v:MutSlice<i64>=a[:];v[false]=2}\n",
        "fn f(){var a=[1];var v:MutSlice<i64>=a[:];v[0]=true}\n",
        "fn f(v:MutSlice<i64>){v[0]=2}\n",
        "fn f(v:MutSlice<u8>){v[0]=2}\n",
        "fn f()->MutSlice<i64>{var a=[1];return a[:]}\n",
        "fn f(){var a=[1];var v:MutSlice<i64>=a[:];var saved:Array<MutSlice<i64>>=[v]}\n"
    };
    const XrXirStatus expected[]={XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE,
        XR_XIR_BAD_VALUE,XR_XIR_BAD_VALUE,XR_XIR_BAD_VALUE,XR_XIR_BAD_VALUE,XR_XIR_BAD_VALUE,XR_XIR_BAD_VALUE,
        XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE};
    _Static_assert(sizeof(inputs)/sizeof(*inputs)==sizeof(expected)/sizeof(*expected),"each refusal keeps its actual independent boundary");
    XrCompileResourceStats before,after;CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    for(unsigned i=0;i<sizeof(inputs)/sizeof(*inputs);++i) {
        XrXirSourceResult out={0};XrXirStatus status=mutslice_check(context,inputs[i],NULL,&out);
        fprintf(stderr,"MutSlice refusal%u status%u\n",i,status);
        CHECK(status==expected[i]&&!out.checked&&!out.snapshot);xr_xir_compile_source_result_free(&out);
        CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK&&before.held_bytes==after.held_bytes);
    }
    for(unsigned i=0;i<2;++i) {
        unsigned calls=0;XrCompileRequestOptions options={.cancel_poll=mutslice_cancel,.cancel_context=&calls};
        if(i)options=(XrCompileRequestOptions){.deadline_mode=XR_COMPILE_DEADLINE_AFTER};
        XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        XrXirSourceResult out={0};CHECK(mutslice_check(context,mutslice_index_source,&control,&out)==(i?XR_XIR_DEADLINE:XR_XIR_CANCELLED));
        CHECK(!out.checked&&!out.snapshot&&calls==(i?0u:1u));xr_xir_compile_source_result_free(&out);
        CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK&&before.held_bytes==after.held_bytes);
    }
}
int main(int argc,char **argv) {
    CHECK(argc==1||argc==2);XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    mutslice_refusals(&context);XrXirSourceResult source={0};CHECK(mutslice_check(&context,mutslice_index_source,NULL,&source)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t ids[MUTSLICE_INDEX_ENTRIES];
    for(unsigned n=0;n<MUTSLICE_INDEX_ENTRIES;++n) {
        ids[n]=UINT32_MAX;
        for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==strlen(mutslice_index_names[n])&&
            !memcmp(module->functions[f].name,mutslice_index_names[n],strlen(mutslice_index_names[n])))ids[n]=f;
        CHECK(ids[n]!=UINT32_MAX);
    }
    if(argc==2) {
        XrXirCSource code={0};CHECK(xr_xir_compile_emit_c(lowered,"mutslice_index",8388608,&code)==XR_XIR_OK);
        FILE *file=fopen(argv[1],"wb");CHECK(file&&fwrite(code.text,1,code.length,file)==code.length);
        CHECK(fprintf(file,"\nconst uint32_t mutslice_index_ids[4]={%uu,%uu,%uu,%uu};\n",ids[0],ids[1],ids[2],ids[3])>0&&!fclose(file));
        xr_xir_compile_c_source_free(&code);
    }
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK&&!lowered);
    mutslice_run(program,ids);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live&&!source_fixture_compile_bytes);puts("MutSlice source/cold/VM/retirement PASS");return 0;
}
