/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * math_source_cold.h - Complete standard-module bytes through an owning catalog
 *
 * KEY CONCEPT:
 *   Math source and packet producers retire before a source-free receiver.
 */
#ifndef MATH_SOURCE_COLD_H
#define MATH_SOURCE_COLD_H
#include "xir/xxir_library_catalog.h"
#include "base/xsha256.h"
#include "base/xfileio.h"
#include "base/xio_policy.h"
static XrXirSourceResult math_cold_source(const XrXirCompileContext *context,const char *consumer) {
    char library[32768]={0};size_t length=append_file(library,0,sizeof(library),XR_MATH_ROOT "/stdlib/math/math.xr");
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_STDLIB,"math",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,XR_MATH_ROOT "/stdlib",NULL,XR_XIR_LIBRARY,NULL};
    XrXirSourceText text={"math/math.xr",library,length};XrXirSourceResult producer={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&text,&producer,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"math cold producer%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK);XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(producer.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&producer);xr_compile_session_free(session);session=NULL;memset(library,'!',sizeof(library));
    XrXirArtifact *received=NULL;CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(received);CHECK(module->declarations->module_count==1);
    XrOsIoPolicy policy=xr_compile_io_policy(context->resources);char *root=NULL;
    CHECK(xr_realpath_owned(&policy,XR_MATH_EMPTY_STDLIB,&root)==XR_OS_IO_OK);
    XrXirLibraryModuleInput binding={{XR_MODULE_IDENTITY_STDLIB,"math",root},"math/math.xr"};
    XrXirLibraryInput input={packet.bytes,packet.length,{0},&binding,1};xr_sha256(packet.bytes,packet.length,input.sha256);
    XrXirLibraryCatalog *catalog=NULL;status=xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog);
    if(status!=XR_XIR_OK)fprintf(stderr,"math catalog%u\n",status);CHECK(status==XR_XIR_OK&&catalog);
    xr_xir_compile_artifact_free(received);memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority project={XR_MODULE_IDENTITY_PROJECT,"math-source",XR_MATH_ROOT};
    request=(XrXirSourceRequest){session,XR_MATH_ROOT "/tests/fixtures/xir_math/driver.xr",&project,
        context,XR_MATH_EMPTY_STDLIB,NULL,XR_XIR_PROGRAM,catalog};
    XrModuleResolver *resolver=NULL;XrModuleResolverConfig config={XR_MATH_EMPTY_STDLIB,NULL,catalog};
    CHECK(xr_compile_module_resolver_new(context->resources,&config,&resolver)==XR_MODULE_OK);
    XrModuleId selected={0};char *error=NULL;
    XrModuleStatus resolved=xr_compile_module_resolver_resolve(resolver,"math",request.entry_path,&project,&selected,&error);
    fprintf(stderr,"math resolver%u root=%s error=%s\n",resolved,binding.authority.physical_root,error?error:"none");
    CHECK(resolved==XR_MODULE_OK&&selected.representation==XR_MODULE_CHECKED_LIBRARY);
    xr_compile_module_id_cleanup(&selected);xr_compile_resources_free(error);xr_compile_module_resolver_free(resolver);
    text=(XrXirSourceText){"tests/fixtures/xir_math/driver.xr",consumer,strlen(consumer)};XrXirSourceResult result={0};
    status=xr_xir_compile_source_check_text(&request,&text,&result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"math cold receiver%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK);xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);xr_compile_resources_free(root);
    return result;
}
#endif // MATH_SOURCE_COLD_H
