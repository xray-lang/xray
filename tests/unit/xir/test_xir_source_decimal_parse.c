/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_decimal_parse.c - Exact decimal text through ordinary Source
 */
#include "base/xmalloc.h"
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_nullable.h"
#include "xir/xxir_emit_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_decimal_parse_runtime.h"
static uint32_t decimal_function(const XrXirModule *module,const char *name) {
    uint32_t found=UINT32_MAX;
    for(uint32_t i=0;i<module->function_count;++i)if(module->functions[i].name_length==strlen(name) &&
        !memcmp(module->functions[i].name,name,strlen(name))){CHECK(found==UINT32_MAX);found=i;}
    CHECK(found!=UINT32_MAX);return found;
}
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==3);
    XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_DECIMAL_PARSE_WORK};
    XrXirSourceRequest request={.session=session,.entry_path=XR_DECIMAL_PARSE_WORK "/decimal-parse.xr",
        .authority=&authority,.context=&context,.linkage_kind=XR_XIR_PROGRAM};
    char text[]=
        "export fn inspect(text:string)->f64? {return f64.tryParse(text)}\n"
        "export fn main()->i64 {const folded=comptime inspect(\"1.7976931348623157e308\");"
        "const ordinary=inspect(\"1.7976931348623157e308\");"
        "return (folded!=null && ordinary!=null && folded! == 1.7976931348623157e308 && ordinary! == folded! && inspect(\"bad\")==null)?42:0}\n"
        "export fn observed()->f64? {return inspect(\"1.7976931348623157e308\")}\n";
    XrXirSourceText input={"decimal-parse.xr",text,strlen(text)};XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&result,&diagnostic,NULL);
    XrCompileResourceStats spent={0};CHECK(xr_compile_resources_stats(context.resources,&spent)==XR_COMPILE_RESOURCE_OK);
    fprintf(stderr,"Decimal ordinary Source%u %d:%d %s spent=%llu/%llu/%llu\n",(unsigned)status,diagnostic.line,
        diagnostic.column,diagnostic.message,(unsigned long long)spent.allocated_bytes,
        (unsigned long long)spent.peak_bytes,(unsigned long long)spent.work);
    memset(text,0xa5,sizeof(text));xr_compile_session_free(session);
    if(status!=XR_XIR_OK) {
        CHECK(!result.checked && !result.snapshot);xr_xir_compile_source_result_free(&result);xr_compile_resources_release(context.resources);
        CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes && !runtime_live && !runtime_bytes);
        fprintf(stderr,"Decimal Source failure released all compiler/runtime owners before strict positive assertion\n");
    }
    CHECK(status==XR_XIR_OK && result.checked && result.snapshot);XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(result.checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&result);
    if(argc==3){FILE *file=fopen(argv[2],"wb");CHECK(file);CHECK(fwrite(packet.bytes,1,packet.length,file)==packet.length && fclose(file)==0);}
    XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_artifact_verify(received,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target, NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t main_entry=decimal_function(module,"main");
    uint32_t observed=decimal_function(module,"observed");const uint32_t functions[3]={main_entry,observed,decimal_function(module,"inspect")};
    if(argc==3) {
        XrXirCSource source={0};CHECK(xr_xir_compile_emit_c(lowered,"decimal_source",8388608,&source)==XR_XIR_OK);
        FILE *file=fopen(argv[1],"wb");CHECK(file);CHECK(fwrite(source.text,1,source.length,file)==source.length);
        CHECK(fprintf(file,"\nconst uint32_t decimal_source_functions[3]={%uu,%uu,%uu};\n",functions[0],functions[1],functions[2])>0 && fclose(file)==0);
        xr_xir_compile_c_source_free(&source);
    }
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    XrXirValue kept=decimal_program_cases(program,functions);xr_compile_resources_release(context.resources);decimal_retained(&kept);
    puts("Exact decimal ordinary Source/comptime/Checked/VM42 and final Nullable Float after all owners/physical0");return 0;
}
