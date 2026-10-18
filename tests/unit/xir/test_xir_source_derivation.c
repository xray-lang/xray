/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_derivation.c - Automatic ordinary library derivation
 *
 * KEY CONCEPT:
 *   The generator is Checked before the future type exists. Source binds its
 *   result, ordinary calls use it, and a second Source receives the same data.
 */
#include "base/xmalloc.h"
#include "base/xsha256.h"
#include "xir/xxir_source.h"
#include "xir/xxir_types.h"
#include "xir/xxir_derivation_internal.h"
#include "xir/xxir_implementation.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#define CHECK(c) do { if (!(c)) {fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);} } while(0)
#include "xir_source_compile_owner.h"
#include "xir_source_derivation_runtime.h"
#include "xir_generated_schema.h"
#include "xir_source_derivation_parser.h"
static const char generator_format[]=
    "%s"
    "type MethodInfo=(string,i64,i64,i64,i64)\n"
    "type CalleeInfo=(string,string,i64,i64,i64,i64)\n"
    "type DeriveContext=(Array<MethodInfo>,Array<CalleeInfo>,Array<i64>,i64,Array<string>)\n"
    "fn privateBody(info:TypeInfo,context:DeriveContext,field:FieldInfo)->Generated {\n"
    " const functions:Array<FunctionRow>=[(context.0[0].0,0,0,context.0[0].1,0,1,0,1,0,2,0,0,0,0)]\n"
    " const parameters:Array<i64>=[info.0];const blocks:Array<BlockRow>=[(0,2,0,0)]\n"
    " const code:Array<InstructionRow>=[(%u,field.1,0,0,0,0,field.3,0,0,field.3),(%u,-1,1,0,0,0,0,0,0,-1)]\n"
    " const operands:Array<i64>=[];const literals:Array<string>=[];const arguments:Array<i64>=[]\n"
    " return (1,functions,parameters,blocks,code,operands,literals,arguments)\n}\n"
    "export fn generate(info:TypeInfo,context:DeriveContext)->Generated {\n"
    " const node=info.1[info.0]\n"
    " if(node.7==1 && len(context.0)==1 && context.0[0].4==0 && context.0[0].3==0){\n"
    "  const field=info.2[node.6];if(field.0==\"secret\" && field.3==1){return privateBody(info,context,field)}\n }\n"
    " const functions:Array<FunctionRow>=[];const parameters:Array<i64>=[];const blocks:Array<BlockRow>=[]\n"
    " const code:Array<InstructionRow>=[];const operands:Array<i64>=[];const literals:Array<string>=[];const arguments:Array<i64>=[]\n"
    " return (0,functions,parameters,blocks,code,operands,literals,arguments)\n}\n";
static const char user_source[]=
    "import {generate} from \"./generator\"\n"
    "interface Measure { measure()->i64 }\n"
    "@derive(Measure, generate, describe: [\"secret\"], read: [\"secret\"])\n"
    "struct Future { private const hidden:string;private const secret:i64\n"
    " init(hidden:string,secret:i64){this.hidden=hidden;this.secret=secret} }\n"
    "export fn main()->i64 {\n"
    " const value=Future(\"unused\",42)\n"
    " const folded=comptime Future(\"folded\",42).measure()\n"
    " return (folded==42)?value.measure():0\n}\n";
#include "xir_source_derivation_resources.h"
static void derive_write(const char *path,const char *source) {
    FILE *file=fopen(path,"wb");size_t count=strlen(source);
    CHECK(file && fwrite(source,1,count,file)==count && !fclose(file));
}
static void derive_absent(const char *path) {FILE *file=fopen(path,"rb");CHECK(!file);}
#include "xir_source_derivation_negative.h"
#include "xir_source_derivation_lsp.h"
static void derive_source(const XrXirCompileContext *context,const char *path,
    XrXirLinkageKind kind,const XrXirLibraryCatalog *catalog,XrXirSourceResult *output) {
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_DERIVATION_WORK};
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,kind,catalog,NULL};
    XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,output,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"automatic derivation status=%u %d:%d %s\n",
        (unsigned)status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && output->checked && output->snapshot);xr_compile_session_free(session);
}
static void derive_library_memory(const XrXirCompileContext *context,const char *path,
    const char *text,XrXirSourceResult *output) {
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_DERIVATION_WORK};
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,XR_XIR_LIBRARY,NULL,NULL};
    XrXirSourceText source={"generator.xr",text,strlen(text)};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&source,output,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"memory generator status%u %d:%d %s\n",
        (unsigned)status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && output->checked && output->snapshot);xr_compile_session_free(session);
}
static uint32_t derive_function(const XrXirModule *module,const char *name) {
    size_t length=strlen(name);uint32_t found=UINT32_MAX;
    for (uint32_t f=0;f<module->function_count;++f)
        if (module->functions[f].name_length==length && !memcmp(module->functions[f].name,name,length)) {
            CHECK(found==UINT32_MAX);found=f;
        }
    CHECK(found!=UINT32_MAX);return found;
}
static void derive_run(const XrXirCompileContext *context,XrXirArtifact *checked,const char *emit,const char *packet_path,bool faults) {
    const XrXirModule *module=xr_xir_compile_artifact_module(checked);
    const XrXirDerivationTable *table=module->declarations->derivations;
    CHECK(table && table->policy_count==1 && table->count==1);
    CHECK(table->policies[0].member_count==1 && table->policies[0].members[0].field==1 &&
        table->policies[0].members[0].operations==(XR_XIR_DERIVE_DESCRIBE|XR_XIR_DERIVE_READ));
    CHECK(module->declarations->implementations && module->declarations->implementations->count==1 &&
        module->declarations->implementations->records[0].derivation_policy==1);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    if (packet_path) {
        FILE *file=fopen(packet_path,"wb");CHECK(file && fwrite(packet.bytes,1,packet.length,file)==packet.length && !fclose(file));
    }
    xr_xir_compile_artifact_free(checked);XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    uint32_t main=derive_function(xr_xir_compile_artifact_module(lowered),"main");
    if (emit) {
        uint32_t method=derive_function(xr_xir_compile_artifact_module(lowered),"measure");
        XrXirCSource output={0};CHECK(xr_xir_compile_emit_c(lowered,"fixture_derive",8388608,&output)==XR_XIR_OK);
        for (size_t i=0;i<output.length;++i) if (output.text[i]=='(') {
            size_t next=i+1;while(next<output.length && isspace((unsigned char)output.text[next]))++next;
            CHECK(next==output.length || output.text[next]!='{');
        }
        FILE *file=fopen(emit,"wb");CHECK(file && fwrite(output.text,1,output.length,file)==output.length);
        CHECK(fprintf(file,"\nconst uint32_t fixture_derive_selection[2]={%uu,%uu};\n",main,method)>0 && !fclose(file));
        xr_xir_compile_c_source_free(&output);
    }
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    size_t sites=derive_runtime_failures(program,main,faults);xr_xir_compile_program_drop(program);
    printf("ordinary derived method VM42; runtime sites=%zu full FI=%u; physical0/0\n",sites,faults);
}
int main(int argc,char **argv) {
    if (argc==7 && !strcmp(argv[1],"--compiler-fi")) {derive_compiler_partition(argv);return 0;}
    bool generator_packet=argc==3 && !strcmp(argv[1],"--emit-generator");
    bool emit=argc==4 && !strcmp(argv[1],"--emit-native");
    bool faults=argc==2 && !strcmp(argv[1],"--runtime-resources");
    bool compiler=argc==2 && (!strcmp(argv[1],"--compiler-resources") || !strcmp(argv[1],"--compiler-controls"));
    CHECK(argc==1 || emit || faults || compiler || generator_packet);
    if (!emit && !faults && !compiler && !generator_packet) derive_parser_cases();
    XrCompileResourceLimits caps={67108864,8388608,128000000};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrCompileResourceStats baseline={0};
    CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    char library_path[2048],user_path[2048],text[14000];
    int n=snprintf(library_path,sizeof(library_path),"%s/generator.xr",XR_SOURCE_DERIVATION_WORK);CHECK(n>0 && n<(int)sizeof(library_path));
    n=snprintf(user_path,sizeof(user_path),"%s/future.xr",XR_SOURCE_DERIVATION_WORK);CHECK(n>0 && n<(int)sizeof(user_path));
    if (!generator_packet && !compiler) {
        FILE *previous=fopen(user_path,"rb");if (previous) {CHECK(!fclose(previous));CHECK(!remove(user_path));}
        derive_absent(user_path);
    }
    n=snprintf(text,sizeof(text),generator_format,generated_schema_source,XR_XIR_STRUCT_GET,XR_XIR_RETURN);
    CHECK(n>0 && n<(int)sizeof(text));XrXirSourceResult producer={0};
    if (generator_packet || compiler) derive_library_memory(&context,library_path,text,&producer);
    else {derive_write(library_path,text);derive_source(&context,library_path,XR_XIR_LIBRARY,NULL,&producer);}
    memset(text,0xa5,sizeof(text));
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(producer.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&producer);
    if (!generator_packet && !compiler) {CHECK(!remove(library_path));derive_absent(library_path);}
    if (generator_packet) {
        FILE *file=fopen(argv[2],"wb");CHECK(file && fwrite(packet.bytes,1,packet.length,file)==packet.length && !fclose(file));
        xr_xir_compile_checked_packet_free(&packet);xr_compile_resources_release(context.resources);
        CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);return 0;
    }
    if (compiler) {
        derive_compiler_resources(&packet,user_path,!strcmp(argv[1],"--compiler-resources"));
        xr_xir_compile_checked_packet_free(&packet);xr_compile_resources_release(context.resources);
        CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes && !runtime_live && !runtime_bytes);return 0;
    }
    if (!emit && !faults) derive_negative_cases(&packet,user_path);derive_absent(library_path);
    XrXirLibraryModuleInput binding={{XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_DERIVATION_WORK},"generator.xr"};
    XrXirLibraryInput input={packet.bytes,packet.length,{0},&binding,1};xr_sha256(packet.bytes,packet.length,input.sha256);
    XrXirLibraryCatalog *catalog=NULL;CHECK(xr_xir_compile_library_catalog_new_v2(&context,&input,1,&catalog)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    memset(&binding,0xa5,sizeof(binding));memset(&input,0xa5,sizeof(input));
    derive_write(user_path,user_source);XrXirSourceResult user={0};
    derive_source(&context,user_path,XR_XIR_PROGRAM,catalog,&user);derive_absent(library_path);
    CHECK(!remove(user_path));derive_absent(user_path);if (!emit && !faults) derive_lsp(&context,user_path,catalog);
    xr_xir_compile_library_catalog_free(catalog);derive_absent(user_path);
    XrXirArtifact *checked=user.checked;user.checked=NULL;xr_xir_compile_source_result_free(&user);
    derive_run(&context,checked,emit?argv[2]:NULL,emit?argv[3]:NULL,faults);XrCompileResourceStats spent={0};
    CHECK(xr_compile_resources_stats(context.resources,&spent)==XR_COMPILE_RESOURCE_OK && spent.live_bytes==baseline.live_bytes);
    fprintf(stderr,"automatic ordinary derivation/explicit policy/private helper/second Source/packet death/VM=42; allocated=%llu peak=%llu work=%llu\n",
        (unsigned long long)spent.allocated_bytes,(unsigned long long)spent.peak_bytes,(unsigned long long)spent.work);
    xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes && !runtime_live && !runtime_bytes);return 0;
}
