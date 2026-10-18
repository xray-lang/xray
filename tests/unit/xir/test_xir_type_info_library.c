/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_type_info_library.c - Future type facts through an owned Checked library
 *
 * KEY CONCEPT:
 *   The library and its private helper are compiled before the user type exists.
 *   Only authenticated Checked bodies survive into the actual Source consumer.
 */
#include "base/xmalloc.h"
#include "base/xsha256.h"
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_declarations.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1); } } while (0)
#include "xir_source_compile_owner.h"
static const char library_text[]=
    "type FieldInfo=(string,i64,i64,i64)\n"
    "type ParameterInfo=(i64,i64)\n"
    "type VariantInfo=(string,i64,i64)\n"
    "type NodeInfo=(i64,i64,string,string,i64,i64,i64,i64,i64,i64,i64,i64,i64,i64,i64,i64)\n"
    "type TypeInfo=(i64,Array<NodeInfo>,Array<FieldInfo>,Array<i64>,Array<ParameterInfo>,Array<VariantInfo>)\n"
    "export struct Hidden { private const concealed:string; const open:i64 }\n"
    "fn privateScore(info:TypeInfo)->i64 {\n"
    " const node=info.1[info.0]; var score=len(node.3)+node.0; var i:i64=0\n"
    " while(i<node.7){const field=info.2[node.6+i];const leaf=info.1[field.1];score=score*31+len(field.0)*7+len(leaf.3)*3+field.2+field.3;i=i+1}\n"
    " return score\n}\n"
    "export fn analyze(info:TypeInfo)->i64 { return privateScore(info) }\n";
static const char user_text[]=
    "import {analyze,Hidden} from \"./generator\"\n"
    "struct Future { private const secret:i64; const label:string }\n"
    "export fn run()->i64 {\n"
    " const own=comptime analyze(typeInfo<Future>())\n"
    " const other=comptime {const info=typeInfo<Hidden>();(len(info.1)==2 && len(info.2)==1 && info.2[0].3==1)?analyze(info):0}\n"
    " return own+other\n}\n";
static void fixture_path(char output[2048],const char *name) {
    int n=snprintf(output,2048,"%s/%s",XR_TYPE_INFO_WORK,name);CHECK(n>0 && n<2048);
}
static void fixture_write(const char *path,const char *text) {
    FILE *file=fopen(path,"wb");size_t length=strlen(text);
    CHECK(file && fwrite(text,1,length,file)==length && !fclose(file));
}
static void fixture_absent(const char *path) {
    FILE *file=fopen(path,"rb");CHECK(!file);
}
static void fixture_remove_previous(const char *path) {
    FILE *file=fopen(path,"rb");
    if (file) { CHECK(!fclose(file));CHECK(!remove(path)); }
    fixture_absent(path);
}
static void fixture_source(const XrXirCompileContext *context,const char *path,
    XrXirLinkageKind kind,const XrXirLibraryCatalog *catalog,XrXirSourceResult *output) {
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    char root[2048];int n=snprintf(root,sizeof(root),"%s",XR_TYPE_INFO_WORK);CHECK(n>0 && n<(int)sizeof(root));
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,kind,catalog,NULL};
    XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,output,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"ordinary type-info library Source status=%u %d:%d %s\n",(unsigned)status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && output->checked && output->snapshot);
    xr_compile_session_free(session);memset(root,0xa5,sizeof(root));
}
static uint32_t fixture_function(const XrXirModule *module,const char *name) {
    size_t length=strlen(name);uint32_t found=UINT32_MAX;
    for (uint32_t f=0;f<module->function_count;++f) {
        if (module->functions[f].name_length==length && !memcmp(module->functions[f].name,name,length)) {
            CHECK(found==UINT32_MAX);found=f;
        }
    }
    CHECK(found!=UINT32_MAX);return found;
}
static void fixture_result(const XrXirCompileContext *context,XrXirArtifact *checked) {
    const XrXirModule *module=xr_xir_compile_artifact_module(checked);
    const XrXirFunction *run=&module->functions[fixture_function(module,"run")];
    for (uint32_t i=0;i<run->instruction_count;++i)
        CHECK(run->instructions[i].op!=XR_XIR_CALL && run->instructions[i].op!=XR_XIR_CALL_INDIRECT);
    XrXirCheckedPacket packet={0};XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_artifact_verify(received,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);
    uint32_t f=fixture_function(xr_xir_compile_artifact_module(lowered),"run");
    XrXirRunContext execution={65536,65536,0,0,0,0};XrXirValue value={0};
    CHECK(xr_xir_compile_vm_run(lowered,f,&execution,NULL,0,&value)==XR_XIR_RUN_OK);
    /* Future: ((6+4)*31 + 6*7+3*3+1)*31 + 5*7+6*3+1 =11276.
     * Hidden: (6+4)*31 + 4*7+3*3+1 =348. Private ordinal0 is absent. */
    CHECK(value.type==XR_XIR_I64 && !value.reserved && value.payload==11624);
    CHECK(!execution.live_bytes && execution.allocations==execution.frees);
    xr_xir_compile_artifact_free(lowered);
}
int main(void) {
    XrCompileResourceLimits caps={67108864,8388608,128000000};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrCompileResourceStats baseline={0};
    CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    char library_path[2048],user_path[2048];fixture_path(library_path,"generator.xr");fixture_path(user_path,"future.xr");
    /* A prior successful run removes both inputs. A failed run is overwritten
     * only with these same owned fixture paths on the next explicit retry. */
    fixture_remove_previous(user_path);
    fixture_write(library_path,library_text);XrXirSourceResult producer={0};
    fixture_source(&context,library_path,XR_XIR_LIBRARY,NULL,&producer);
    const XrXirModule *library=xr_xir_compile_artifact_module(producer.checked);
    CHECK(library->linkage_kind==XR_XIR_LIBRARY && library->declarations->module_count==1);
    uint32_t helper=fixture_function(library,"privateScore");
    CHECK(!library->declarations->functions[helper].exported);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(producer.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&producer);
    CHECK(!remove(library_path));fixture_absent(library_path);
    char root[2048];int n=snprintf(root,sizeof(root),"%s",XR_TYPE_INFO_WORK);CHECK(n>0 && n<(int)sizeof(root));
    XrXirLibraryModuleInput binding={{XR_MODULE_IDENTITY_SCRIPT,NULL,root},"generator.xr"};
    XrXirLibraryInput input={packet.bytes,packet.length,{0},&binding,1};xr_sha256(packet.bytes,packet.length,input.sha256);
    XrXirLibraryCatalog *catalog=NULL;CHECK(xr_xir_compile_library_catalog_new_v2(&context,&input,1,&catalog)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    memset(root,0xa5,sizeof(root));memset(&binding,0xa5,sizeof(binding));memset(&input,0xa5,sizeof(input));
    fixture_write(user_path,user_text);XrXirSourceResult user={0};
    fixture_source(&context,user_path,XR_XIR_PROGRAM,catalog,&user);fixture_absent(library_path);
    xr_xir_compile_library_catalog_free(catalog);
    CHECK(!remove(user_path));fixture_absent(user_path);
    XrXirArtifact *checked=user.checked;user.checked=NULL;xr_xir_compile_source_result_free(&user);
    fixture_result(&context,checked);XrCompileResourceStats spent={0};
    CHECK(xr_compile_resources_stats(context.resources,&spent)==XR_COMPILE_RESOURCE_OK);
    CHECK(spent.live_bytes==baseline.live_bytes);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"ordinary no-source Checked library/private helper/future type/hidden field filter/packet death:11624 compiler physical0/0\n");
    return 0;
}
