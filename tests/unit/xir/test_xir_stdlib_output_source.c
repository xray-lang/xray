/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_stdlib_output_source.c - Real standard library Checked publication
 *
 * KEY CONCEPT:
 *   Published library bytes survive deletion of their original module source.
 */
#include "xir_construction_fixture.h"
#include "base/xmalloc.h"
#include "xir_stdlib_output_module_probe.h"
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) xr_test_stdlib_output_module_allocate(bytes)
#define xr_free(pointer) xr_test_stdlib_output_module_release(pointer)
#include "xir_effect_execution_owner.h"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")
#define source_attempts effects_compile_attempts
#define source_fail_at effects_compile_fail_at
#define source_live effects_compile_live
#define source_bytes effects_compile_bytes
#include "xir/xxir_source_query.c"
#include "xir/xxir_type_inference.c"
#include "xir/xxir_source.c"
/* Expose the actual catalog record only for malformed-binding fixture construction. */
#include "xir/xxir_library_catalog.c"
#define XR_STDLIB_OUTPUT_SOURCE
#include "xir_stdlib_output_runtime.h"
#include "xir/xxir_library_catalog.h"
#include "base/xsha256.h"
#include "base/xfileio.h"
#ifdef XR_OS_WINDOWS
#include "base/xwindows_utf8.h"
#endif

static const XrXirCompileContext *publication_context;
static XrXirLibraryInput publication_input;
/* Receiver-owned blocks and borrowed fixture bytes have distinct lifetimes. */
typedef struct PublicationScope {XrXirCompileContext context;XrCompileResourceStats receiver;XrCompilerSession *session;XrXirLibraryCatalog *catalog;size_t blocks,bytes;} PublicationScope;
static XrXirStatus publication_scope_begin(PublicationScope *scope,XrCompileResourceLimits caps){
 memset(scope,0,sizeof(*scope));scope->blocks=effects_compile_live;scope->bytes=effects_compile_bytes;
 XrCompileResourceStatus status=xr_compile_resources_new(&caps,&scope->context.resources);
 if(status!=XR_COMPILE_RESOURCE_OK)return status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET;
 scope->context.limits=xr_xir_compile_default_limits();CHECK(xr_compile_resources_stats(scope->context.resources,&scope->receiver)==XR_COMPILE_RESOURCE_OK);return XR_XIR_OK;
}
static void publication_scope_end(PublicationScope *scope){
 xr_compile_session_free(scope->session);xr_xir_compile_library_catalog_free(scope->catalog);
 if(scope->context.resources){XrCompileResourceStats final={0};CHECK(xr_compile_resources_stats(scope->context.resources,&final)==XR_COMPILE_RESOURCE_OK);CHECK(final.live_bytes==scope->receiver.live_bytes);xr_compile_resources_release(scope->context.resources);}
 CHECK(effects_compile_live==scope->blocks&&effects_compile_bytes==scope->bytes);
}
static XrXirStatus publication_scope_source(PublicationScope *scope,const XrXirSourceRequest *original,XrXirSourceRequest *copy){
 XrXirStatus status=xr_xir_compile_library_catalog_new_v2(&scope->context,&publication_input,1,&scope->catalog);if(status!=XR_XIR_OK)return status;
 XrCompilerSessionStatus created=xr_compile_session_new(scope->context.resources,&scope->session);
 if(created!=XR_COMPILER_SESSION_OK)return created==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET;
 *copy=*original;copy->session=scope->session;copy->context=&scope->context;copy->libraries=scope->catalog;return XR_XIR_OK;
}
static const XrCompileResourceLimits publication_caps={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
static XrModuleResolver *publication_resolver(const XrModuleResolverConfig *config){
 XrCompileResources *resources=NULL;bool owned=config->catalog==NULL;
 if(owned)CHECK(xr_compile_resources_new(&publication_caps,&resources)==XR_COMPILE_RESOURCE_OK);
 else resources=xr_xir_compile_library_catalog_context(config->catalog)->resources;
 XrModuleResolver *resolver=NULL;CHECK(xr_compile_module_resolver_new(resources,config,&resolver)==XR_MODULE_OK&&resolver);
 if(owned)xr_compile_resources_release(resources);return resolver;
}
static XrXirStatus publication_source_probe(const XrXirSourceRequest *request,XrCompileResourceLimits caps,size_t failure,bool module_failure,size_t *sites){
 PublicationScope scope;XrXirStatus status=publication_scope_begin(&scope,caps);XrXirSourceResult result={0};XrXirSourceRequest copy={0};
 if(status==XR_XIR_OK)status=publication_scope_source(&scope,request,&copy);
 if(status==XR_XIR_OK){
  source_attempts=0;source_fail_at=module_failure?SIZE_MAX:failure;effects_compile_injected=false;
  module_attempts=0;module_fail_at=module_failure?failure:SIZE_MAX;module_injected=false;module_injecting=module_failure;
  status=xr_xir_compile_source_check(&copy,&result,NULL,NULL);
  if(sites)*sites=module_failure?module_attempts:source_attempts;
  source_fail_at=SIZE_MAX;module_fail_at=SIZE_MAX;module_injecting=false;
  if(failure!=SIZE_MAX)CHECK(module_failure?module_injected:effects_compile_injected);
 }
 CHECK(status==XR_XIR_OK?(result.checked&&result.snapshot):(!result.checked&&!result.snapshot));
 xr_xir_compile_source_result_free(&result);publication_scope_end(&scope);CHECK(!module_live&&!module_bytes);return status;
}
static XrXirStatus publication_catalog_probe(const XrXirLibraryInput *input,XrCompileResourceLimits caps,size_t failure,size_t *sites){
 PublicationScope scope;XrXirStatus status=publication_scope_begin(&scope,caps);
 if(status==XR_XIR_OK){source_attempts=0;source_fail_at=failure;effects_compile_injected=false;
  status=xr_xir_compile_library_catalog_new_v2(&scope.context,input,1,&scope.catalog);if(sites)*sites=source_attempts;
  source_fail_at=SIZE_MAX;if(failure!=SIZE_MAX)CHECK(effects_compile_injected);
 }
 CHECK(status==XR_XIR_OK?scope.catalog!=NULL:scope.catalog==NULL);publication_scope_end(&scope);return status;
}
static void publication_exact(const XrXirLibraryInput *input,const XrXirSourceRequest *request){
 for(unsigned dimension=0;dimension<3;++dimension){uint64_t low=0,high=dimension==0?publication_caps.work:dimension==1?publication_caps.allocated_bytes:publication_caps.live_bytes;
  while(low<high){uint64_t middle=low+(high-low)/2;XrCompileResourceLimits caps=publication_caps;
   if(dimension==0)caps.work=middle;else if(dimension==1)caps.allocated_bytes=middle;else caps.live_bytes=middle;
   XrXirStatus status=request?publication_source_probe(request,caps,SIZE_MAX,false,NULL):publication_catalog_probe(input,caps,SIZE_MAX,NULL);
   CHECK(status==XR_XIR_OK||status==XR_XIR_BUDGET);if(status==XR_XIR_OK)high=middle;else low=middle+1;
  }CHECK(low);
  for(unsigned below=0;below<2;++below){XrCompileResourceLimits caps=publication_caps;
   if(dimension==0)caps.work=low-below;else if(dimension==1)caps.allocated_bytes=low-below;else caps.live_bytes=low-below;
   XrXirStatus status=request?publication_source_probe(request,caps,SIZE_MAX,false,NULL):publication_catalog_probe(input,caps,SIZE_MAX,NULL);
   CHECK(status==(below?XR_XIR_BUDGET:XR_XIR_OK));}
  printf("stdlib output %s dimension=%u exact=%llu/minus1 physicalbaseline\n",request?"Source":"Catalog",dimension,(unsigned long long)low);
 }
}
static XrXirCheckedPacket publication_packet(const char *root, const char *path) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(publication_context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_STDLIB,"io",root};
    XrXirSourceRequest request = {session,path,&authority,publication_context,root,NULL,XR_XIR_LIBRARY,NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if (status != XR_XIR_OK) fprintf(stderr,"producer %u %s\n",status,diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked);
    const XrXirModule *module = xr_xir_compile_artifact_module(result.checked);
    CHECK(module->linkage_kind == XR_XIR_LIBRARY && module->declarations->module_count == 1);
    CHECK(module->function_count == 3 && !module->types && !module->provenance);
    const char *identity = "stdlib-module-v1:module=2:io:path=12:io/output.xr";
    CHECK(module->declarations->modules[0].name_length == strlen(identity) &&
        !memcmp(module->declarations->modules[0].name,identity,strlen(identity)));
    unsigned writes = 0;
    for (uint32_t f = 0; f < module->function_count; ++f)
        for (uint32_t i = 0; i < module->functions[f].instruction_count; ++i)
            if (module->functions[f].instructions[i].op == XR_XIR_WRITE_STREAM) ++writes;
    CHECK(writes == 2);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(result.checked,&packet,NULL) == XR_XIR_OK);
    xr_xir_compile_source_result_free(&result); xr_compile_session_free(session);
    /* Live compiler metadata belongs to the explicit parent ledger. */
    session=NULL;CHECK(xr_compile_session_new(publication_context->resources,&session)==XR_COMPILER_SESSION_OK);
    authority.kind = XR_MODULE_IDENTITY_SCRIPT; authority.namespace_id = NULL;
    request.session = session; result = (XrXirSourceResult){0};
    CHECK(xr_xir_compile_source_check(&request,&result,&diagnostic,NULL) != XR_XIR_OK && !result.checked && !result.snapshot);
    xr_xir_compile_source_result_free(&result); xr_compile_session_free(session);
    /* Live compiler metadata belongs to the explicit parent ledger. */
    return packet;
}
static void publication_reject(const XrXirLibraryInput *input, size_t count) {
    size_t live=source_live,bytes=source_bytes;
    XrXirLibraryCatalog *catalog = NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(publication_context,input,count,&catalog) != XR_XIR_OK && !catalog);
    CHECK(source_live==live&&source_bytes==bytes);
}
static void publication_catalog_cases(const XrXirLibraryInput *input) {
    XrXirLibraryModuleInput bad_binding = *input->modules;
    XrXirLibraryInput bad = *input; bad.modules = &bad_binding;
    bad.sha256[0] ^= 1; publication_reject(&bad,1);
    bad = *input; bad_binding = *input->modules; bad.modules = &bad_binding; bad.modules[0].authority.namespace_id = "fs"; publication_reject(&bad,1);
    bad = *input; bad_binding = *input->modules; bad.modules = &bad_binding; bad.modules[0].logical_path = "io/../output.xr"; publication_reject(&bad,1);
    bad = *input; bad_binding = *input->modules; bad.modules = &bad_binding; bad.modules[0].logical_path = "io/output/../output.xr"; publication_reject(&bad,1);
    bad = *input; bad_binding = *input->modules; bad.modules = &bad_binding; bad.modules[0].authority.physical_root = "relative"; publication_reject(&bad,1);
    bad = *input; bad_binding = *input->modules; bad.modules = &bad_binding; bad.modules[0].authority.kind = XR_MODULE_IDENTITY_SCRIPT;
    bad.modules[0].authority.namespace_id = NULL; bad.modules[0].logical_path = "output.xr"; publication_reject(&bad,1);
    XrXirLibraryInput duplicate[] = {*input,*input}; publication_reject(duplicate,2);
    unsigned char *altered = xr_malloc(input->length); CHECK(altered);
    memcpy(altered,input->packet,input->length);
    const char *identity = "stdlib-module-v1:module=2:io:path=12:io/output.xr";
    size_t changed = 0;
    for (size_t i = 64; i + strlen(identity) <= input->length; ++i)
        if (!memcmp(altered+i,identity,strlen(identity))) {
            altered[i+strlen(identity)-9] = 'p'; ++changed;
        }
    CHECK(changed == 1);
    XrSHA256Context hash; xr_sha256_init(&hash); xr_sha256_update(&hash,altered,32);
    xr_sha256_update(&hash,altered+64,input->length-64); xr_sha256_final(&hash,altered+32);
    bad = *input; bad_binding = *input->modules; bad.modules = &bad_binding; bad.packet = altered; xr_sha256(altered,input->length,bad.sha256);
    XrXirArtifact *retagged = NULL;
    CHECK(xr_xir_compile_checked_read(publication_context,altered,input->length,&retagged,NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(retagged); publication_reject(&bad,1); xr_free(altered);
    altered = xr_malloc(input->length); CHECK(altered); memcpy(altered,input->packet,input->length);
    unsigned char write_prefix[24] = {0}; write_prefix[0] = (unsigned char)XR_XIR_WRITE_STREAM;
    write_prefix[4] = (unsigned char)XR_XIR_BOOL; changed = 0;
    for (size_t i = 64; i + 40 <= input->length; ++i)
        if (!memcmp(altered+i,write_prefix,sizeof(write_prefix))) {
            altered[i+24] = 3; ++changed;
        }
    CHECK(changed == 2);
    xr_sha256_init(&hash); xr_sha256_update(&hash,altered,32);
    xr_sha256_update(&hash,altered+64,input->length-64); xr_sha256_final(&hash,altered+32);
    bad = *input; bad_binding = *input->modules; bad.modules = &bad_binding; bad.packet = altered; xr_sha256(altered,input->length,bad.sha256);
    publication_reject(&bad,1); xr_free(altered);
    size_t sites=0;CHECK(publication_catalog_probe(input,publication_caps,SIZE_MAX,&sites)==XR_XIR_OK);CHECK(sites);
    for(size_t failure=0;failure<sites;++failure)CHECK(publication_catalog_probe(input,publication_caps,failure,NULL)==XR_XIR_OUT_OF_MEMORY);
    printf("stdlib output Catalog actual compiler OOM=%zu physicalbaseline\n",sites);publication_exact(input,NULL);
}
static void publication_private_case(const XrXirLibraryInput *input, const char *root, const char *entry) {
    XrXirArtifact *original = NULL, *checked = NULL;
    CHECK(xr_xir_compile_checked_read(publication_context,input->packet,input->length,&original,NULL) == XR_XIR_OK);
    XrXirModule built = *xr_xir_compile_artifact_module(original); built.stage = XR_XIR_BUILT;
    XrXirDeclarations declarations = *built.declarations;
    XrXirFunctionIdentity functions[3]; CHECK(built.function_count == 3);
    memcpy(functions,declarations.functions,sizeof(functions));
    for (unsigned f = 0; f < 3; ++f) functions[f].exported = 0;
    declarations.functions = functions; built.declarations = &declarations;
    CHECK(xr_xir_compile_check_v2(publication_context,&built,xr_xir_compile_artifact_construction(original),&checked,NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(checked,&packet,NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked); xr_xir_compile_artifact_free(original);
    XrXirLibraryInput private_input = *input;
    private_input.packet = packet.bytes; private_input.length = packet.length;
    xr_sha256(packet.bytes,packet.length,private_input.sha256);
    XrXirLibraryCatalog *catalog = NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(publication_context,&private_input,1,&catalog) == XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(publication_context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirSourceRequest request = {session,entry,&authority,publication_context,root,NULL,XR_XIR_PROGRAM,catalog};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    CHECK(xr_xir_compile_source_check(&request,&result,&diagnostic,NULL) != XR_XIR_OK && !result.checked);
    CHECK(!strcmp(diagnostic.message,"import requires an exported declaration"));
    xr_xir_compile_source_result_free(&result); xr_compile_session_free(session);
    xr_xir_compile_library_catalog_free(catalog);
    /* Live compiler metadata belongs to the explicit parent ledger. */
}
static void publication_resolver_cases(XrXirLibraryCatalog *catalog,const char *root,const char *entry){
 size_t count=0;const XrModuleResourceBinding *resources=xr_xir_compile_library_catalog_resources_v2(catalog,&count);CHECK(count==1&&resources);
 XrModuleIdentityAuthority script={XR_MODULE_IDENTITY_SCRIPT,NULL,root};
 for(unsigned attack=0;attack<10;++attack){XrModuleResourceBinding binding=resources[0];
  if(attack==1)binding.authority.kind=XR_MODULE_IDENTITY_SCRIPT;if(attack==2)binding.authority.namespace_id="fs";
  if(attack==3)binding.authority.physical_root="E:/foreign";if(attack==4)binding.logical_path="io/elsewhere.xr";
  if(attack==5)binding.checked=NULL;if(attack==6)binding.source_locator=NULL;
  XrXirLibraryCatalog hostile=*catalog;hostile.resources=&binding;hostile.count=1;
  XrModuleResolverConfig config={root,NULL,&hostile};XrModuleResolver *resolver=publication_resolver(&config);
  const char *specifier=attack==7?"std/io/../output":attack==8?"std/io/output.xr":attack==9?"std//io/output":"std/io/output";
  XrModuleId id={0};char *error=NULL;XrModuleStatus status=xr_compile_module_resolver_resolve(resolver,specifier,entry,&script,&id,&error);
  CHECK(attack?status!=XR_MODULE_OK:status==XR_MODULE_OK);
  if(attack)CHECK(!id.canonical&&!id.source_path&&!id.logical_path&&!id.authority.namespace_id&&!id.authority.physical_root&&!id.resource);
  if(!attack)CHECK(id.kind==XR_MOD_STDLIB&&id.representation==XR_MODULE_CHECKED_LIBRARY&&id.resource==&binding&&!strcmp(id.canonical,resources[0].canonical));
  xr_compile_module_id_cleanup(&id);xr_compile_resources_free(error);xr_compile_module_resolver_free(resolver);
 }
}

static void publication_source_resolver_faults(const char *root){
 for(unsigned cached=0;cached<2;++cached){size_t sites=0;
  for(size_t pass=0;pass<=sites;++pass){XrModuleResolverConfig config={root,NULL,NULL};XrModuleResolver *resolver=publication_resolver(&config);
   XrModuleId id={0};char *error=NULL;
   if(cached){CHECK(xr_compile_module_resolver_resolve(resolver,"std/io/output",NULL,NULL,&id,&error)==XR_MODULE_OK);xr_compile_module_id_cleanup(&id);CHECK(!error);}
   module_attempts=0;module_fail_at=pass?pass-1:SIZE_MAX;module_injected=false;module_injecting=true;
   XrModuleStatus status=xr_compile_module_resolver_resolve(resolver,"std/io/output",NULL,NULL,&id,&error);module_injecting=false;module_fail_at=SIZE_MAX;
   if(!pass){CHECK(status==XR_MODULE_OK);sites=module_attempts;CHECK(sites);}else CHECK(module_injected&&status==XR_MODULE_OUT_OF_MEMORY&&!id.canonical&&!id.logical_path&&!id.source_path&&!id.authority.namespace_id&&!id.authority.physical_root);
   xr_compile_module_id_cleanup(&id);xr_compile_resources_free(error);xr_compile_module_resolver_free(resolver);CHECK(!module_live&&!module_bytes);
  }printf("stdlib output source resolver cached=%u real compiler allocations=%zu exact OOM/physical zero\n",cached,sites);
 }
}


#include "xir_stdlib_output_file_probe.inc.c"

static void publication_module_faults(const XrXirSourceRequest *request){
 for(unsigned cached=0;cached<2;++cached){size_t sites=0;
  for(size_t pass=0;pass<=sites;++pass){PublicationScope scope;CHECK(publication_scope_begin(&scope,publication_caps)==XR_XIR_OK);
   CHECK(xr_xir_compile_library_catalog_new_v2(&scope.context,&publication_input,1,&scope.catalog)==XR_XIR_OK);
   XrModuleResolverConfig config={request->stdlib_path,NULL,scope.catalog};XrModuleResolver *resolver=publication_resolver(&config);
   size_t count=0;const XrModuleResourceBinding *resources=xr_xir_compile_library_catalog_resources_v2(scope.catalog,&count);CHECK(count==1);
   XrModuleId id={0};char *error=NULL;
   if(cached){CHECK(xr_compile_module_resolver_resolve(resolver,"std/io/output",request->entry_path,request->authority,&id,&error)==XR_MODULE_OK);xr_compile_module_id_cleanup(&id);CHECK(!error);}
   module_attempts=0;module_fail_at=pass?pass-1:SIZE_MAX;module_injected=false;module_injecting=true;
   XrModuleStatus status=xr_compile_module_resolver_resolve(resolver,"std/io/output",request->entry_path,request->authority,&id,&error);module_injecting=false;module_fail_at=SIZE_MAX;
   if(!pass){CHECK(status==XR_MODULE_OK&&id.resource==resources);sites=module_attempts;CHECK(sites);}else CHECK(module_injected&&status==XR_MODULE_OUT_OF_MEMORY&&!id.canonical&&!id.logical_path&&!id.source_path&&!id.authority.namespace_id&&!id.authority.physical_root&&!id.resource);
   xr_compile_module_id_cleanup(&id);xr_compile_resources_free(error);xr_compile_module_resolver_free(resolver);publication_scope_end(&scope);CHECK(!module_live&&!module_bytes);
  }printf("stdlib output Checked resolver cached=%u actual compiler allocations=%zu OOM physicalbaseline\n",cached,sites);
 }
 size_t sites=0;CHECK(publication_source_probe(request,publication_caps,SIZE_MAX,true,&sites)==XR_XIR_OK);CHECK(sites);
 for(size_t failure=0;failure<sites;++failure)CHECK(publication_source_probe(request,publication_caps,failure,true,NULL)==XR_XIR_OUT_OF_MEMORY);
 printf("stdlib output real module pipeline whole-compiler malloc OOM=%zu physicalbaseline\n",sites);publication_source_path_faults(request);
}


static void publication_core_module_faults(const XrXirSourceRequest *request) {
    char path[2048];
    CHECK(snprintf(path, sizeof(path), "%s/core-probe.xr", request->authority->physical_root) > 0);
    FILE *file = fopen(path, "wb"); CHECK(file);
    const char source[] = "import \"std/io/output\" as output\n"
        "export fn emit(value:string)->bool {assert(true);return output.writeStdout(value)}\n";
    CHECK(fwrite(source, 1, sizeof(source) - 1, file) == sizeof(source) - 1 && !fclose(file));
    XrXirSourceRequest core = *request; core.entry_path = path;
    publication_module_faults(&core);
    CHECK(!remove(path));
}

static void publication_module_statuses(const XrXirSourceRequest *request) {
    XrXirSourceRequest invalid = *request;
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    char missing[2048];
    CHECK(snprintf(missing, sizeof(missing), "%s/not-published.xr", request->authority->physical_root) > 0);
    invalid.entry_path = missing;
    CHECK(xr_xir_compile_source_check(&invalid, &result, &diagnostic,NULL) == XR_XIR_UNRESOLVED);
    CHECK(diagnostic.status == XR_XIR_UNRESOLVED && !result.checked && !result.snapshot);
    char long_root[33000]; memset(long_root, 'a', sizeof(long_root) - 1); long_root[sizeof(long_root) - 1] = 0;
    invalid = *request; invalid.stdlib_path = long_root;
    CHECK(xr_xir_compile_source_check(&invalid, &result, &diagnostic,NULL) == XR_XIR_BUDGET);
    CHECK(!result.checked && !result.snapshot);
    XrModuleIdentityAuthority authority = *request->authority; authority.namespace_id = "forged";
    invalid = *request; invalid.authority = &authority;
    CHECK(xr_xir_compile_source_check(&invalid, &result, &diagnostic,NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(!result.checked && !result.snapshot);
#ifdef XR_OS_WINDOWS
    XrOsIoPolicy host_policy=xr_compile_io_policy(publication_context->resources);
    wchar_t *wide=NULL;CHECK(xr_win_utf8_path_owned(&host_policy,request->entry_path,&wide)==XR_OS_IO_OK&&wide);
    HANDLE held = CreateFileW(wide, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL); xr_compile_resources_free(wide);
    CHECK(held != INVALID_HANDLE_VALUE);
    CHECK(xr_xir_compile_source_check(request, &result, &diagnostic,NULL) == XR_XIR_IO);
    CHECK(diagnostic.status == XR_XIR_IO && !result.checked && !result.snapshot);
    CHECK(CloseHandle(held));
#endif
    size_t attempts=0;
    for(unsigned pass=0;pass<2;++pass){
     PublicationScope scope;CHECK(publication_scope_begin(&scope,publication_caps)==XR_XIR_OK);
     CHECK(xr_xir_compile_library_catalog_new_v2(&scope.context,&publication_input,1,&scope.catalog)==XR_XIR_OK);
     size_t count=0;const XrModuleResourceBinding *resources=xr_xir_compile_library_catalog_resources_v2(scope.catalog,&count);CHECK(count==1);
     XrModuleResourceBinding binding=resources[0];binding.checked=NULL;XrXirLibraryCatalog hostile=*scope.catalog;hostile.resources=&binding;
     XrModuleResolverConfig config={request->stdlib_path,NULL,&hostile};XrModuleResolver *resolver=publication_resolver(&config);
     module_injecting=true;module_attempts=0;module_fail_at=pass?attempts-1:SIZE_MAX;module_injected=false;
     XrModuleId id={0};char *error=NULL;CHECK(xr_compile_module_resolver_resolve(resolver,"std/io/output",request->entry_path,request->authority,&id,&error)==XR_MODULE_INVALID);
     module_injecting=false;module_fail_at=SIZE_MAX;attempts=module_attempts;CHECK(attempts&&!id.canonical&&!id.source_path);CHECK(pass?!error:!!error);
     if(pass)CHECK(module_injected);
     xr_compile_resources_free(error);xr_compile_module_resolver_free(resolver);publication_scope_end(&scope);CHECK(!module_live&&!module_bytes);
    }puts("module INVALID/UNRESOLVED/BUDGET/IO retain causes; diagnostic OOM keeps INVALID");
}
static void publication_source_budgets(const XrXirSourceRequest *request){publication_exact(NULL,request);}

int main(int argc, char **argv) {
    CHECK(argc==3);publication_context=effects_source_owner(UINT64_C(64)*1024*1024,UINT64_C(128000000));
    XrOsIoPolicy root_policy=xr_compile_io_policy(publication_context->resources);char *root=NULL;CHECK(xr_realpath_owned(&root_policy,argv[1],&root)==XR_OS_IO_OK&&root);
    char source[1024], entry[1024], path[1024];
    CHECK(snprintf(source,sizeof(source),"%s/io/output.xr",root) > 0);
    CHECK(snprintf(entry,sizeof(entry),"%s/root.xr",root) > 0);
    CHECK(snprintf(path,sizeof(path),"%s/output.xrc",root) > 0);
    XrXirCheckedPacket packet = publication_packet(root,source);
    FILE *file = fopen(path,"wb"); CHECK(file);
    CHECK(fwrite(packet.bytes,1,packet.length,file) == packet.length && !fclose(file));
    size_t length = packet.length; xr_xir_compile_checked_packet_free(&packet);
    CHECK(!runtime_live && !runtime_bytes);
    void *bytes = xr_malloc(length); CHECK(bytes); file = fopen(path,"rb"); CHECK(file);
    CHECK(fread(bytes,1,length,file) == length && !fclose(file));
    XrXirLibraryInput input = {bytes,length,{0}, (XrXirLibraryModuleInput[]){{{XR_MODULE_IDENTITY_STDLIB,"io",root},"io/output.xr"}},1};
    xr_sha256(bytes,length,input.sha256);publication_input=input;void *frozen=malloc(length);CHECK(frozen);memcpy(frozen,bytes,length);publication_input.packet=frozen;
    publication_catalog_cases(&input);
    publication_private_case(&input,root,entry);
    XrXirLibraryCatalog *catalog = NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(publication_context,&input,1,&catalog) == XR_XIR_OK);
    memset(bytes,0,length); xr_free(bytes);
    publication_resolver_cases(catalog,root,entry);
    publication_source_resolver_faults(root);
    publication_file_probe_cases(root, entry, source);
    CHECK(!remove(source)); publication_resolver_cases(catalog,root,entry);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(publication_context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirSourceRequest request = {session,entry,&authority,publication_context,root,NULL,XR_XIR_PROGRAM,catalog};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    source_attempts = 0; runtime_attempts = 0;
    XrXirStatus status = xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if (status != XR_XIR_OK) fprintf(stderr,"consumer %u %s\n",status,diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked && result.snapshot);
    const size_t live=source_live,physical=source_bytes,core_live=runtime_live,core_bytes=runtime_bytes;
    size_t sites=0;CHECK(publication_source_probe(&request,publication_caps,SIZE_MAX,false,&sites)==XR_XIR_OK);CHECK(sites);
    for(size_t failure=0;failure<sites;++failure){CHECK(publication_source_probe(&request,publication_caps,failure,false,NULL)==XR_XIR_OUT_OF_MEMORY);
      CHECK(source_live==live&&source_bytes==physical&&runtime_live==core_live&&runtime_bytes==core_bytes);}
    printf("stdlib output Source plus Checked metadata actual compiler OOM=%zu physicalbaseline; former runtime metadata domain retired\n",sites);
    publication_source_budgets(&request);
    publication_module_faults(&request);
    publication_module_statuses(&request);
    publication_core_module_faults(&request);
    XrXirArtifact *owned = result.checked; result.checked = NULL;
    xr_xir_compile_source_result_free(&result); xr_compile_session_free(session);
    xr_xir_compile_library_catalog_free(catalog); xr_compile_resources_free(root);
    /* Live compiler metadata belongs to the explicit parent ledger. */
    CHECK(xr_xir_compile_artifact_verify(owned,NULL) == XR_XIR_OK);
    xr_test_stdlib_output_execute(owned,argv[2]);
    free(frozen);publication_input=(XrXirLibraryInput){0};CHECK(!runtime_live&&!runtime_bytes);effects_source_owners_free();
    puts("real io/output source removed; first owned Checked execution PASS"); return 0;
}
