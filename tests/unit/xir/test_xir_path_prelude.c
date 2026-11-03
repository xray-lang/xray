/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_path_prelude.c - Source-owned Path type visibility without value imports
 *
 * KEY CONCEPT:
 *   Full library checking and actual nominal access retain independent authority.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_library_catalog.h"
#include "base/xsha256.h"
#include "xir/xxir_nominal.h"
#include "xir/xxir_types.h"
#include "xir/xxir_path_prelude.h"
#include "toolchain/xcompiler_session.h"
#include "base/xresource_limits.h"
#include "base/xmalloc.h"
#include "base/xfileio.h"
#include "base/xio_policy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"Path %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "path_source_memory.h"
#include "xir_path_prelude_runtime.h"
static XrXirStatus path_checked(const XrXirCompileContext *context,const char *text,
    const char *stdlib,const XrXirLibraryCatalog *catalog,XrXirSourceResult *result) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_PROJECT,"path-shadow",XR_PATH_FIXTURES};
    XrXirSourceRequest request={session,XR_PATH_FIXTURES "/client.xr",&authority,context,stdlib,NULL,XR_XIR_PROGRAM,catalog};
    XrXirSourceText input={"client.xr",text,strlen(text)};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"Path check%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    xr_compile_session_free(session);return status;
}
static XrXirStatus checked(const XrXirCompileContext *context,const char *text) {
    XrXirSourceResult result={0};XrXirStatus status=path_checked(context,text,XR_PATH_ROOT "/stdlib",NULL,&result);
    xr_xir_compile_source_result_free(&result);return status;
}
static bool path_cancelled(const void *unused){(void)unused;return true;}
static void path_finite_controls(const XrXirCompileContext *context) {
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    const char *source="fn keep(value:Path)->Path{return value}";
    for(unsigned mode=0;mode<2;++mode) {
        XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
        XrCompileRequestControl control={0};XrCompileRequestOptions options={.cancel_poll=path_cancelled};
        if(mode) {
            CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
            CHECK(xr_compile_session_bind_request(session,&control)==XR_COMPILE_RESOURCE_OK);
        } else path_physical_fail=true;
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_PROJECT,"path-finite",XR_PATH_FIXTURES};
        XrXirSourceRequest request={session,XR_PATH_FIXTURES "/client.xr",&authority,context,XR_PATH_FIXTURES "/stdlib",NULL,XR_XIR_PROGRAM,NULL};
        XrXirSourceText input={"client.xr",source,strlen(source)};XrXirSourceResult result={0};
        CHECK(xr_xir_compile_source_check_text(&request,&input,&result,NULL,NULL)==(mode?XR_XIR_CANCELLED:XR_XIR_OUT_OF_MEMORY));
        CHECK(!result.checked&&!result.snapshot&&!path_physical_fail&&path_physical_failures==1);
        xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
        CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK&&before.held_bytes==after.held_bytes);
    }
    XrXirSourceResult restored={0};CHECK(path_checked(context,source,XR_PATH_FIXTURES "/stdlib",NULL,&restored)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&restored);CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(before.held_bytes==after.held_bytes);
}
static void access_controls(const XrXirCompileContext *context) {
    char logical[64],canonical[160];CHECK(xir_path_prelude_coordinates(logical,canonical));
    XrXirNominalField field={{"payload",7},XR_XIR_STRING,XR_XIR_FIELD_MUTABLE};
    XrXirNominalDeclaration nominal={.module={canonical,(uint32_t)strlen(canonical)},.name={"Path",4},
        .exported=1,.fields=&field,.field_count=1,.kind=XR_XIR_NOMINAL_CLASS};
    XrXirNominalTable table={&nominal,1,NULL};XrXirType projection=XR_XIR_STRING;
    XrXirTypeNode node={.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,NULL,0,&projection,1}};
    XrXirTypes types={&node,1,&table,NULL};XrXirFunctionIdentity identity={0};
    XrXirSourceModule modules[2]={{.name="client",.name_length=6,.initializer=0},
        {.name=canonical,.name_length=(uint32_t)strlen(canonical),.initializer=1}};
    XrXirDeclarations declarations={.modules=modules,.module_count=2,.functions=&identity};
    XrXirModule module={.functions=NULL,.function_count=1,.declarations=&declarations,.types=&types};
    CHECK(xr_xir_compile_nominal_structure_verify(context,&table,&types)==XR_XIR_OK);
    XrXirType type=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    CHECK(xr_xir_compile_nominal_access(context,&module,0,type,0,XR_XIR_NOMINAL_TYPE)==XR_XIR_OK);
    for(XrXirNominalAccess access=XR_XIR_NOMINAL_CONSTRUCT;access<XR_XIR_NOMINAL_TYPE;++access)
        CHECK(xr_xir_compile_nominal_access(context,&module,0,type,0,access)==XR_XIR_BAD_TYPE);
    for(unsigned mode=0;mode<5;++mode) {
        XrXirNominalDeclaration original=nominal;XrXirSourceModule owner=modules[1];
        if(mode==0)nominal.exported=0;
        if(mode==1)nominal.name=(XrXirLiteral){"PathInfo",8};
        if(mode==2)nominal.kind=XR_XIR_NOMINAL_STRUCT;
        if(mode==3)nominal.module=(XrXirLiteral){"other",5};
        if(mode==4){modules[1].name="foreign";modules[1].name_length=7;}
        CHECK(xr_xir_compile_nominal_access(context,&module,0,type,0,XR_XIR_NOMINAL_TYPE)==
            (mode>=3?XR_XIR_BAD_STRUCTURE:XR_XIR_BAD_TYPE));
        nominal=original;modules[1]=owner;
    }
}
static uint32_t path_main(const XrXirModule *module) {
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==4&&!memcmp(module->functions[f].name,"main",4))return f;
    CHECK(false);return 0;
}
static const char path_container_main[]=
    "fn pathCount(values:Array<Path?>)->i64{return len(values)}\n"
    "export fn main()->i64{const paths:Array<Path?>=[null];assert(paths[0]==null);return pathCount(paths)+41}";
static void path_container_identity(const XrXirModule *module) {
    bool found=false;
    for(uint32_t i=0;i<module->types->count;++i) {
        const XrXirTypeNode *array=&module->types->nodes[i];if(array->kind!=XR_XIR_TYPE_ARRAY)continue;
        const XrXirTypeNode *nullable=xr_xir_type_node(module->types,array->element);
        if(!nullable||nullable->kind!=XR_XIR_TYPE_NULLABLE)continue;
        const XrXirTypeNode *nominal=xr_xir_type_node(module->types,nullable->element);
        if(nominal&&nominal->kind==XR_XIR_TYPE_NOMINAL&&
            xir_path_prelude_type(module->declarations,module->types->nominals,nominal->nominal.declaration))found=true;
    }
    CHECK(found);
}
static XrXirLibraryCatalog *path_catalog(const XrXirCompileContext *context) {
    XrOsIoPolicy policy=xr_compile_io_policy(context->resources);char *root=NULL;
    CHECK(xr_realpath_owned(&policy,XR_PATH_FIXTURES "/cold-stdlib",&root)==XR_OS_IO_OK);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_STDLIB,"path",root};
    XrXirSourceRequest request={session,XR_PATH_FIXTURES "/cold-stdlib/path/path.xr",&authority,context,root,NULL,XR_XIR_LIBRARY,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic issue={0};
    CHECK(xr_xir_compile_source_check(&request,&source,&issue,NULL)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);xr_compile_session_free(session);
    XrXirLibraryModuleInput binding={authority,"path/path.xr"};XrXirLibraryInput input={packet.bytes,packet.length,{0},&binding,1};
    xr_sha256(packet.bytes,packet.length,input.sha256);XrXirLibraryCatalog *catalog=NULL;
    XrXirStatus status=xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog);
    if(status)fprintf(stderr,"Path Catalog%u\n",status);CHECK(status==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);xr_compile_resources_free(root);return catalog;
}
static XrXirArtifact *path_source_controls(const XrXirCompileContext *context) {
    static const char *const refused[]={
        "fn make()->Path{return Path(\"bad\")}",
        "fn read(value:Path)->string{return value.raw}",
        "fn write(value:Path){value.raw=\"bad\"}",
        "fn secret(value:Path)->PathInfo{return null}",
        "import path;const secret=path.privatePathSlot",
        "fn invalid(){var Path=3;var value:Path?=null}",
        "type P=Path\nfn make()->Path{return P(\"bad\")}"};
    for(unsigned i=0;i<sizeof(refused)/sizeof(*refused);++i) {
        XrXirSourceResult result={0};XrXirStatus status=path_checked(context,refused[i],XR_PATH_FIXTURES "/stdlib",NULL,&result);
        fprintf(stderr,"Path refusal%u status%u\n",i,status);
        CHECK(status==(i==0||i==6?XR_XIR_BAD_VALUE:i==4?XR_XIR_BAD_STRUCTURE:XR_XIR_BAD_TYPE)&&!result.checked&&!result.snapshot);
        xr_xir_compile_source_result_free(&result);
    }
    XrXirSourceResult before={0};CHECK(path_checked(context,
        "fn before(){var value:Path?=null;var Path=3;assert(Path==3)}",XR_PATH_FIXTURES "/stdlib",NULL,&before)==XR_XIR_OK);
    const XrXirDeclarations *observed=xr_xir_compile_artifact_module(before.checked)->declarations;
    char logical[64],canonical[160];CHECK(xir_path_prelude_coordinates(logical,canonical));uint32_t matches=0;
    for(uint32_t m=0;m<observed->module_count;++m)
        if(observed->modules[m].name_length==strlen(canonical)&&!memcmp(observed->modules[m].name,canonical,strlen(canonical))) {
            ++matches;CHECK(!xr_xir_module_imports(observed,observed->root_module,m));
        }
    CHECK(matches==1);xr_xir_compile_source_result_free(&before);
    XrXirLibraryCatalog *catalog=path_catalog(context);XrXirSourceResult cold={0};
    CHECK(path_checked(context,path_container_main,
        XR_PATH_FIXTURES "/cold-stdlib",catalog,&cold)==XR_XIR_OK);
    xr_xir_compile_library_catalog_free(catalog);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(cold.checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&cold);
    XrXirArtifact *received=NULL;CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);CHECK(xr_xir_compile_artifact_verify(received,NULL)==XR_XIR_OK);
    XrXirArtifact *closed=NULL,*code=NULL;CHECK(xr_xir_compile_specialize(received,&closed,NULL,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(received);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},NULL,&code,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);return code;
}
static void path_built_controls(const XrXirCompileContext *context,const XrXirArtifact *checked) {
    const XrXirModule *original=xr_xir_compile_artifact_module(checked);const XrXirNominalTable *table=original->types->nominals;
    uint32_t target=UINT32_MAX;for(uint32_t n=0;n<table->count;++n)
        if(xir_path_prelude_type(original->declarations,table,n)){CHECK(target==UINT32_MAX);target=n;}
    CHECK(target!=UINT32_MAX);XrXirNominalDeclaration *records=xr_malloc((size_t)table->count*sizeof(*records));CHECK(records);
    memcpy(records,table->declarations,(size_t)table->count*sizeof(*records));XrXirNominalTable changed=*table;changed.declarations=records;
    XrXirTypes types=*original->types;types.nominals=&changed;XrXirModule built=*original;built.types=&types;built.stage=XR_XIR_BUILT;built.provenance=NULL;
    for(unsigned mode=0;mode<4;++mode) {
        records[target]=table->declarations[target];
        if(mode==0)records[target].exported=0;
        if(mode==1)records[target].name=(XrXirLiteral){"ForeignPath",11};
        if(mode==2)records[target].kind=XR_XIR_NOMINAL_STRUCT;
        if(mode==3)records[target].native.source_fingerprint[0]=1;
        XrXirArtifact *output=NULL;XrXirDiagnostic issue={0};
        XrXirStatus status=xr_xir_compile_check_v2(context,&built,xr_xir_compile_artifact_construction(checked),&output,&issue);
        fprintf(stderr,"Path BUILT refusal%u status%u f%u i%u\n",mode,status,issue.function,issue.instruction);
        CHECK(status==(mode>=2?XR_XIR_BAD_STRUCTURE:XR_XIR_BAD_TYPE)&&!output);
    }
    xr_free(records);
}
static XrXirArtifact *path_product(const XrXirCompileContext *context,bool explicit_import) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_PROJECT,"path-client",XR_PATH_FIXTURES};
    XrXirSourceRequest request={session,XR_PATH_FIXTURES "/client.xr",&authority,context,XR_PATH_FIXTURES "/stdlib",NULL,XR_XIR_PROGRAM,NULL};
    char text[768];snprintf(text,sizeof(text),"%s\n%s",explicit_import?
        "import {Path} from path\nconst value=Path(\"actual\")\nassert(value.raw==\"actual\")":
        "fn keep(value:Path)->Path{return value}",path_container_main);
    XrXirSourceText input={"client.xr",text,strlen(text)};XrXirSourceResult result={0};XrXirSourceDiagnostic issue={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&result,&issue,NULL);
    if(status)fprintf(stderr,"Path product%u %d:%d %s\n",status,issue.line,issue.column,issue.message);
    CHECK(status==XR_XIR_OK);if(!explicit_import)path_built_controls(context,result.checked);xr_compile_session_free(session);memset(text,'!',sizeof(text));
    const XrXirModule *module=xr_xir_compile_artifact_module(result.checked);uint32_t path=UINT32_MAX;
    char logical[64],canonical[160];CHECK(xir_path_prelude_coordinates(logical,canonical));
    for(uint32_t m=0;m<module->declarations->module_count;++m)
        if(module->declarations->modules[m].name_length==strlen(canonical)&&!memcmp(module->declarations->modules[m].name,canonical,strlen(canonical)))path=m;
    CHECK(path!=UINT32_MAX);CHECK(xr_xir_module_imports(module->declarations,module->declarations->root_module,path)==explicit_import);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(result.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&result);XrXirArtifact *cold=NULL,*closed=NULL,*code=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&cold,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(cold,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(cold);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},NULL,&code,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);return code;
}
int main(int argc,char **argv) {
    XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    if(argc==3&&!strcmp(argv[1],"--previous123")) {
        unsigned char packet[8192];FILE *file=fopen(argv[2],"rb");CHECK(file);
        size_t length=fread(packet,1,sizeof(packet),file);CHECK(length&&feof(file)&&!ferror(file)&&!fclose(file));
        CHECK(packet[8]==41&&packet[12]==123&&XR_XIR_CHECKED_CONTRACT==129);XrXirArtifact *rejected=NULL;
        CHECK(xr_xir_compile_checked_read(&context,packet,length,&rejected,NULL)==XR_XIR_BAD_STRUCTURE&&!rejected);
        CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK&&before.held_bytes==after.held_bytes);
        xr_compile_resources_release(context.resources);return 0;
    }
    if(argc==3&&!strcmp(argv[1],"--full")) {
        XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILE_RESOURCE_OK);
        XrOsIoPolicy policy=xr_compile_io_policy(context.resources);char *root=NULL;
        CHECK(xr_realpath_owned(&policy,XR_PATH_ROOT "/stdlib",&root)==XR_OS_IO_OK);
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_STDLIB,argv[2],root};
        char path[512];CHECK(snprintf(path,sizeof(path),XR_PATH_ROOT "/stdlib/%s/%s.xr",argv[2],argv[2])>0);
        XrXirSourceRequest request={session,path,&authority,&context,XR_PATH_ROOT "/stdlib",NULL,XR_XIR_LIBRARY,NULL};
        XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
        fprintf(stderr,"full %s status%u module%u %d:%d %s\n",argv[2],status,diagnostic.module,diagnostic.line,diagnostic.column,diagnostic.message);
        xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);xr_compile_resources_free(root);xr_compile_resources_release(context.resources);return status?1:0;
    }
    CHECK(argc==1||argc==4);access_controls(&context);XrXirArtifact *catalog_code=path_source_controls(&context);path_finite_controls(&context);
    for(unsigned mode=0;mode<3;++mode) {
        XrXirArtifact *code=mode==2?catalog_code:path_product(&context,mode!=0);
        path_container_identity(xr_xir_compile_artifact_module(code));uint32_t entry=path_main(xr_xir_compile_artifact_module(code));
        if(argc==4) {
            char prefix[32];snprintf(prefix,sizeof(prefix),"path_%u",mode);XrXirCSource generated={0};
            CHECK(xr_xir_compile_emit_c(code,prefix,16777216,&generated)==XR_XIR_OK);
            char destination[1024];CHECK(snprintf(destination,sizeof(destination),"%s%s",argv[mode==1?3:2],mode==2?".catalog.c":"")>0);
            FILE *out=fopen(destination,"wb");CHECK(out&&fwrite(generated.text,1,generated.length,out)==generated.length);
            CHECK(fprintf(out,"\nconst uint32_t path_%u_main=%u;\n",mode,entry)>0&&!fclose(out));xr_xir_compile_c_source_free(&generated);
            xr_xir_compile_artifact_free(code);
        } else {
            XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&code,&program)==XR_XIR_OK&&!code);
            path_execute(program,entry,mode==1?1:0);xr_xir_compile_program_drop(program);
        }
    }
    CHECK(checked(&context,"class Path{value:i64 init(value:i64){this.value=value}}\nfn keep(value:Path)->Path{return value}")==XR_XIR_OK);
    CHECK(checked(&context,"fn keep<Path>(value:Path)->Path{return value}")==XR_XIR_OK);
    CHECK(checked(&context,"import {Path} from \"./custom\"\nfn keep(value:Path)->Path{return value}")==XR_XIR_OK);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);CHECK(before.held_bytes==after.held_bytes);
    xr_compile_resources_release(context.resources);puts("Path declaration-only access and lexical shadow controls passed");return 0;
}
