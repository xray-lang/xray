/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Cold declaration namespaces retain names independently of callable types. */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_compile_memory.h"
#include "xir/xxir_library_catalog.h"
#include "base/xsha256.h"
#include "toolchain/xcompiler_session.h"
#include "shared/xerror_codes.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
static size_t physical_live,physical_bytes,physical_attempts,physical_fail=SIZE_MAX;
typedef union NamespaceAllocation {size_t bytes;long double alignment;void *pointer;} NamespaceAllocation;
static void *namespace_allocate(size_t bytes) {
    if(physical_attempts++==physical_fail || bytes>SIZE_MAX-sizeof(NamespaceAllocation))return NULL;
    NamespaceAllocation *allocation=xr_malloc(sizeof(*allocation)+bytes);
    if(!allocation)return NULL;
    allocation->bytes=bytes;++physical_live;physical_bytes+=bytes;return allocation+1;
}
static void namespace_release(void *pointer) {
    if(!pointer)return;
    NamespaceAllocation *allocation=(NamespaceAllocation *)pointer-1;
    CHECK(physical_live && physical_bytes>=allocation->bytes);
    --physical_live;physical_bytes-=allocation->bytes;xr_free(allocation);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc namespace_allocate
#define xr_free namespace_release
#include "base/xcompile_resources.c"
#undef xr_malloc
#undef xr_free
typedef struct NamespaceCancel {size_t polls,after;} NamespaceCancel;
static bool namespace_cancel(const void *opaque) {
    NamespaceCancel *cancel=(NamespaceCancel *)opaque;return ++cancel->polls>=cancel->after;
}
static XrXirStatus namespace_source(const XrXirCompileContext *compile,const char *logical,
    const char *text,XrXirLinkageKind kind,const XrXirLibraryCatalog *catalog,
    XrXirSourceResult *result,XrXirSourceDiagnostic *diagnostic) {
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(compile->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,NAMESPACE_LIBRARY_ROOT};
    char path[1024];int size=snprintf(path,sizeof(path),"%s/%s",NAMESPACE_LIBRARY_ROOT,logical);
    CHECK(size>0 && (size_t)size<sizeof(path));
    XrXirSourceRequest request={session,path,&authority,compile,NULL,NULL,kind,catalog, XR_XIR_HOST_TARGET};
    XrXirSourceText source={logical,text,strlen(text)};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&source,result,diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"namespace source %s: %u %s\n",logical,(unsigned)status,diagnostic->message);
    xr_compile_session_free(session);return status;
}
static void namespace_names(const XrXirModule *module,const char *left) {
    const XrXirDeclarations *d=module->declarations;
    CHECK(d && d->parameter_name_count>=2);
    bool function=false,requirement=false;
    for(uint32_t n=0;n<d->parameter_name_count;++n) {
        const XrXirParameterNames *row=&d->parameter_names[n];
        if(row->count!=2)continue;
        CHECK(!row->parameter_offset && row->names[0].length==strlen(left) &&
            !memcmp(row->names[0].bytes,left,strlen(left)) && row->names[1].length==5 &&
            !memcmp(row->names[1].bytes,"right",5));
        if(row->kind==XR_XIR_PARAMETER_FUNCTION)function=true;else requirement=true;
    }
    CHECK(function && requirement);
}
static void namespace_shapes(const XrXirCompileContext *compile) {
    XrXirSourceModule module={.name="m",.name_length=1};
    XrXirFunctionIdentity functions[2]={{0},{0}};
    XrXirLiteral names[2]={{"left",4},{"right",5}};
    XrXirParameterNames row={XR_XIR_PARAMETER_FUNCTION,1,0,0,names,2};
    XrXirDeclarations d={.modules=&module,.module_count=1,.functions=functions,
        .entry_function=1,.parameter_names=&row,.parameter_name_count=1};
    CHECK(xr_xir_compile_declarations_verify(compile,&d,NULL,2,XR_XIR_PROGRAM,NULL)==XR_XIR_OK);
    for(unsigned bad=0;bad<8;++bad) {
        XrXirParameterNames original=row;XrXirLiteral first=names[0];
        if(bad==0)row.owner=2;
        else if(bad==1)row.member=1;
        else if(bad==2)row.parameter_offset=1;
        else if(bad==3)row.kind=XR_XIR_PARAMETER_REQUIREMENT;
        else if(bad==4)row.names=NULL;
        else if(bad==5)names[0]=(XrXirLiteral){"\xff",1};
        else if(bad==6)names[0]=(XrXirLiteral){"x\0",2};
        else names[0]=names[1];
        CHECK(xr_xir_compile_declarations_verify(compile,&d,NULL,2,XR_XIR_PROGRAM,NULL)==XR_XIR_BAD_STRUCTURE);
        row=original;names[0]=first;
    }
    char long_name[16385];memset(long_name,'x',sizeof(long_name));
    names[0]=(XrXirLiteral){long_name,sizeof(long_name)};row.count=1;
    NamespaceCancel cancellation={0,SIZE_MAX};
    XrCompileRequestOptions options={.cancel_poll=namespace_cancel,.cancel_context=&cancellation};
    XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_xir_compile_declarations_verify(compile,&d,NULL,2,XR_XIR_PROGRAM,&control)==XR_XIR_OK);
    size_t polls=cancellation.polls,live=physical_live,bytes=physical_bytes;CHECK(polls>8);
    for(size_t point=1;point<=polls;++point) {
        cancellation=(NamespaceCancel){0,point};control=(XrCompileRequestControl){0};
        CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_xir_compile_declarations_verify(compile,&d,NULL,2,XR_XIR_PROGRAM,&control)==XR_XIR_CANCELLED);
        CHECK(physical_live==live && physical_bytes==bytes);
    }
}
static XrXirStatus module_order_expect(const XrXirCompileContext *compile,
    const XrXirDeclarations *declarations, const uint32_t *expected) {
    uint32_t order[7];
    uint32_t sentinel[7];
    for (size_t i = 0; i < sizeof(order) / sizeof(*order); ++i) order[i] = sentinel[i] = UINT32_MAX;
    CHECK(declarations->module_count == sizeof(order) / sizeof(*order));
    XrXirStatus status = xr_xir_compile_declarations_order(compile, declarations, order);
    CHECK((status == XR_XIR_OK) == (expected != NULL));
    CHECK(!memcmp(order, expected ? expected : sentinel, sizeof(order)));
    return status;
}
static void declaration_module_order_cases(const XrXirCompileContext *compile) {
    uint32_t deps[] = {0, 0};
    XrXirSourceModule modules[] = {
        {.name = "z", .name_length = 1, .initializer = 0},
        {.name = "a", .name_length = 1, .initializer = 1, .dependencies = deps, .dependency_count = 1},
        {.name = "b", .name_length = 1, .initializer = 2},
        {.name = "a", .name_length = 1, .initializer = 3},
        {.name = "aa", .name_length = 2, .initializer = 4},
        {.name = "\xc3\xa9", .name_length = 2, .initializer = 5},
        {.name = "a", .name_length = 1, .initializer = 6},
    };
    XrXirFunctionIdentity functions[] = {
        {0}, {.module = 1}, {.module = 2}, {.module = 3}, {.module = 4}, {.module = 5}, {.module = 6}, {0},
    };
    XrXirDeclarations declarations = {
        .modules = modules, .module_count = 7, .functions = functions, .entry_function = 7,
    };
    const uint32_t duplicate_names[] = {3, 6, 4, 2, 0, 1, 5};
    CHECK(module_order_expect(compile, &declarations, duplicate_names) == XR_XIR_OK);
    CHECK(xr_xir_compile_declarations_verify(compile, &declarations, NULL, 8, XR_XIR_PROGRAM, NULL) == XR_XIR_BAD_STRUCTURE);
    modules[3].name = "ab";
    modules[3].name_length = 2;
    modules[6].name = "ac";
    modules[6].name_length = 2;
    const uint32_t ordered[] = {4, 3, 6, 2, 0, 1, 5};
    CHECK(module_order_expect(compile, &declarations, ordered) == XR_XIR_OK);
    CHECK(xr_xir_compile_declarations_verify(compile, &declarations, NULL, 8, XR_XIR_PROGRAM, NULL) == XR_XIR_OK);

    modules[0].name = NULL;
    CHECK(module_order_expect(compile, &declarations, NULL) == XR_XIR_BAD_STRUCTURE);
    modules[0].name_length = 0;
    const uint32_t empty_name[] = {0, 1, 4, 3, 6, 2, 5};
    CHECK(module_order_expect(compile, &declarations, empty_name) == XR_XIR_OK);
    CHECK(xr_xir_compile_declarations_verify(compile, &declarations, NULL, 8, XR_XIR_PROGRAM, NULL) == XR_XIR_BAD_STRUCTURE);
    modules[0].name = "z";
    modules[0].name_length = 1;

    modules[1].dependency_count = 2;
    CHECK(module_order_expect(compile, &declarations, ordered) == XR_XIR_OK);
    CHECK(xr_xir_compile_declarations_verify(compile, &declarations, NULL, 8, XR_XIR_PROGRAM, NULL) == XR_XIR_BAD_STRUCTURE);
    modules[1].dependency_count = 1;
    deps[0] = 7;
    CHECK(module_order_expect(compile, &declarations, NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(xr_xir_compile_declarations_verify(compile, &declarations, NULL, 8, XR_XIR_PROGRAM, NULL) == XR_XIR_BAD_STRUCTURE);
    deps[0] = 0;
    modules[1].dependencies = NULL;
    CHECK(module_order_expect(compile, &declarations, NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(xr_xir_compile_declarations_verify(compile, &declarations, NULL, 8, XR_XIR_PROGRAM, NULL) == XR_XIR_BAD_STRUCTURE);
    modules[1].dependencies = deps;
    uint32_t back = 1;
    modules[0].dependencies = &back;
    modules[0].dependency_count = 1;
    CHECK(module_order_expect(compile, &declarations, NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(xr_xir_compile_declarations_verify(compile, &declarations, NULL, 8, XR_XIR_PROGRAM, NULL) == XR_XIR_BAD_STRUCTURE);
    modules[0].dependencies = NULL;
    modules[0].dependency_count = 0;

    size_t live = physical_live, bytes = physical_bytes;
    size_t first = physical_attempts;
    CHECK(module_order_expect(compile, &declarations, ordered) == XR_XIR_OK);
    size_t order_sites = physical_attempts - first;
    first = physical_attempts;
    CHECK(xr_xir_compile_declarations_verify(compile, &declarations, NULL, 8, XR_XIR_PROGRAM, NULL) == XR_XIR_OK);
    size_t verify_sites = physical_attempts - first;
    CHECK(order_sites && verify_sites);
    for (size_t point = 0; point < order_sites; ++point) {
        physical_fail = physical_attempts + point;
        CHECK(module_order_expect(compile, &declarations, NULL) == XR_XIR_OUT_OF_MEMORY);
        physical_fail = SIZE_MAX;
        CHECK(physical_live == live && physical_bytes == bytes);
    }
    for (size_t point = 0; point < verify_sites; ++point) {
        physical_fail = physical_attempts + point;
        CHECK(xr_xir_compile_declarations_verify(compile, &declarations, NULL, 8, XR_XIR_PROGRAM, NULL) == XR_XIR_OUT_OF_MEMORY);
        physical_fail = SIZE_MAX;
        CHECK(physical_live == live && physical_bytes == bytes);
    }
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(compile->resources, &stats) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_FINITE, stats.held_bytes, false};
    XrCompileResources *restricted = NULL;
    CHECK(xr_compile_resources_new(&limits, &restricted) == XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext bounded = {restricted, compile->limits};
    CHECK(module_order_expect(&bounded, &declarations, NULL) == XR_XIR_MEMORY_LIMIT);
    CHECK(xr_xir_compile_declarations_verify(&bounded, &declarations, NULL, 8, XR_XIR_PROGRAM, NULL) == XR_XIR_MEMORY_LIMIT);
    xr_compile_resources_release(restricted);
    CHECK(physical_live == live && physical_bytes == bytes);
}
static XrXirStatus map_key_source_failure(const XrXirCompileContext *compile,const char *text,
    XrXirSourceFailure *failure) {
    XrCompilerSession *session=NULL;
    XrCompileResourceStatus admission=xr_compile_session_new(compile->resources,&session);
    if(admission!=XR_COMPILE_RESOURCE_OK)return xir_compile_resource_status(admission);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,NAMESPACE_LIBRARY_ROOT};
    char path[1024];int size=snprintf(path,sizeof(path),"%s/bare-key.xr",NAMESPACE_LIBRARY_ROOT);
    CHECK(size>0 && (size_t)size<sizeof(path));
    XrXirSourceRequest request={session,path,&authority,compile,NULL,NULL,XR_XIR_PROGRAM,NULL, XR_XIR_HOST_TARGET};
    XrXirSourceText source={"bare-key.xr",text,strlen(text)};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&source,&result,&diagnostic,failure);
    CHECK(!result.checked && !result.snapshot);
    xr_compile_session_free(session);return status;
}
static void map_key_diagnostic_case(const XrXirCompileContext *compile,bool heterogeneous) {
    char name[4097],text[8400];memset(name,'q',sizeof(name)-1);name[sizeof(name)-1]=0;
    int size=heterogeneous?snprintf(text,sizeof(text),"const %s=1\nconst value={\"a\":1,%s:2}\n",name,name):
        snprintf(text,sizeof(text),"const value={%s:1}\n",name);
    CHECK(size>0 && (size_t)size<sizeof(text));
    size_t live=physical_live,bytes=physical_bytes,first=physical_attempts;
    XrXirSourceFailure failure={0};
    CHECK(map_key_source_failure(compile,text,&failure)==(heterogeneous?XR_XIR_BAD_TYPE:XR_XIR_BAD_VALUE));
    size_t sites=physical_attempts-first;CHECK(sites>0 && failure.report);
    memset(text,0xa5,(size_t)size);
    XrFrontendReportInfo info={0};XrFrontendRoot root={0};XrFrontendLabel label={0};
    CHECK(xr_frontend_report_info(failure.report,&info) && info.roots==1);
    CHECK(xr_frontend_report_root(failure.report,0,&root) && root.labels==(heterogeneous?3u:1u));
    if(heterogeneous)CHECK(root.code==XR_ERR_COLLECTION_LITERAL_TYPES);
    else CHECK(!strcmp(root.message,"name is not an initialized value"));
    CHECK(xr_frontend_report_label(failure.report,0,root.labels-1,&label));
    const char prefix[]="if you intended a string key, write \"";
    CHECK(!strncmp(label.message,prefix,sizeof(prefix)-1) &&
        !memcmp(label.message+sizeof(prefix)-1,name,sizeof(name)-1) &&
        label.message[sizeof(prefix)-1+sizeof(name)-1]=='"' &&
        !label.message[sizeof(prefix)+sizeof(name)-1]);
    xr_xir_compile_source_failure_free(&failure);CHECK(physical_live==live && physical_bytes==bytes);
    CHECK((heterogeneous?snprintf(text,sizeof(text),"const %s=1\nconst value={\"a\":1,%s:2}\n",name,name):
        snprintf(text,sizeof(text),"const value={%s:1}\n",name))==size);
    for(size_t point=0;point<sites;++point) {
        physical_fail=physical_attempts+point;
        XrXirStatus status=map_key_source_failure(compile,text,&failure);
        physical_fail=SIZE_MAX;
        CHECK(status==XR_XIR_OUT_OF_MEMORY);
        xr_xir_compile_source_failure_free(&failure);
        CHECK(physical_live==live && physical_bytes==bytes);
    }
}
int main(void) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext compile={resources,xr_xir_compile_default_limits()};
    namespace_shapes(&compile);
    declaration_module_order_cases(&compile);
    map_key_diagnostic_case(&compile,false);
    map_key_diagnostic_case(&compile,true);
    const char producer[]="export interface Join { join(left:i64,right:i64)->i64 }\n"
        "export fn combine(left:i64,right:i64=9)->i64{return left*10+right}\n";
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(namespace_source(&compile,"library.xr",producer,XR_XIR_LIBRARY,NULL,&source,&diagnostic)==XR_XIR_OK);
    namespace_names(xr_xir_compile_artifact_module(source.checked),"left");
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    const char renamed[]="export interface Join { join(value:i64,right:i64)->i64 }\n"
        "export fn combine(value:i64,right:i64=9)->i64{return value*10+right}\n";
    XrXirSourceResult renamed_source={0};XrXirSourceDiagnostic renamed_diagnostic={0};
    CHECK(namespace_source(&compile,"library.xr",renamed,XR_XIR_LIBRARY,NULL,&renamed_source,&renamed_diagnostic)==XR_XIR_OK);
    const XrXirModule *before=xr_xir_compile_artifact_module(source.checked);
    const XrXirModule *after=xr_xir_compile_artifact_module(renamed_source.checked);
    CHECK(before->function_count==after->function_count);
    for(uint32_t f=0;f<before->function_count;++f) {
        CHECK(before->functions[f].parameter_count==after->functions[f].parameter_count &&
            before->functions[f].result==after->functions[f].result);
        for(uint32_t p=0;p<before->functions[f].parameter_count;++p)
            CHECK(before->functions[f].parameters[p]==after->functions[f].parameters[p]);
    }
    namespace_names(after,"value");
    XrXirCheckedPacket renamed_packet={0};
    CHECK(xr_xir_compile_checked_write(renamed_source.checked,&renamed_packet,NULL)==XR_XIR_OK);
    CHECK(renamed_packet.length!=packet.length || memcmp(renamed_packet.bytes,packet.bytes,packet.length));
    xr_xir_compile_checked_packet_free(&renamed_packet);xr_xir_compile_source_result_free(&renamed_source);
    xr_xir_compile_source_result_free(&source);
    XrXirArtifact *decoded=NULL;size_t first=physical_attempts;
    CHECK(xr_xir_compile_checked_read(&compile,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    size_t sites=physical_attempts-first;CHECK(sites>0);
    namespace_names(xr_xir_compile_artifact_module(decoded),"left");xr_xir_compile_artifact_free(decoded);decoded=NULL;
    size_t live=physical_live,bytes=physical_bytes;
    for(size_t point=0;point<sites;++point) {
        physical_fail=physical_attempts+point;
        XrXirStatus status=xr_xir_compile_checked_read(&compile,packet.bytes,packet.length,&decoded,NULL);
        physical_fail=SIZE_MAX;
        CHECK(status==XR_XIR_OUT_OF_MEMORY && !decoded && physical_live==live && physical_bytes==bytes);
    }
    NamespaceCancel cancellation={0,SIZE_MAX};
    XrCompileRequestOptions options={.cancel_poll=namespace_cancel,.cancel_context=&cancellation};
    XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xir_checked_read(&compile,packet.bytes,packet.length,&decoded,NULL,&control)==XR_XIR_OK);
    size_t polls=cancellation.polls;xr_xir_compile_artifact_free(decoded);decoded=NULL;
    for(size_t point=1;point<=polls;++point) {
        cancellation=(NamespaceCancel){0,point};control=(XrCompileRequestControl){0};
        CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        CHECK(xir_checked_read(&compile,packet.bytes,packet.length,&decoded,NULL,&control)==XR_XIR_CANCELLED);
        CHECK(!decoded && physical_live==live && physical_bytes==bytes);
    }
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,NAMESPACE_LIBRARY_ROOT};
    XrXirLibraryModuleInput binding={authority,"library.xr"};
    XrXirLibraryInput input={.packet=packet.bytes,.length=packet.length,.modules=&binding,.module_count=1};
    xr_sha256(packet.bytes,packet.length,input.sha256);XrXirLibraryCatalog *catalog=NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(&compile,&input,1,&catalog)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    memset(&binding,0xa5,sizeof(binding));memset(&input,0xa5,sizeof(input));
    const char consumer[]="import {combine,Join} from \"./library\"\n"
        "fn via<T:Join>(value:T)->i64{return value.join(right:2,left:1)}\n"
        "const answer=combine(right:2,left:1)\nconst defaulted=combine(left:3)\n";
    CHECK(namespace_source(&compile,"consumer.xr",consumer,XR_XIR_PROGRAM,catalog,&source,&diagnostic)==XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_verify(source.checked,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&source);
    const char alias[]="import {combine} from \"./library\"\nconst value=combine\nconst answer=value(left:1,right:2)\n";
    diagnostic=(XrXirSourceDiagnostic){0};
    CHECK(namespace_source(&compile,"consumer.xr",alias,XR_XIR_PROGRAM,catalog,&source,&diagnostic)==XR_XIR_BAD_TYPE);
    CHECK(!source.checked && !strncmp(diagnostic.message,"E0327:",6));xr_xir_compile_source_result_free(&source);
    xr_xir_compile_library_catalog_free(catalog);xr_compile_resources_release(resources);
    CHECK(!physical_live && !physical_bytes);
    puts("Cold declaration parameter names, fault cleanup, cancellation and alias refusal PASS");return 0;
}
