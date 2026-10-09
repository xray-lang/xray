/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_dispatch_cases.h - Source imports beside exact Checked bindings
 *
 * KEY CONCEPT:
 *   Unmatched Source imports retain their normal resolver, while matched failures never fall back.
 */
#ifndef XIR_LIBRARY_DISPATCH_CASES_H
#define XIR_LIBRARY_DISPATCH_CASES_H
#include "os/os_fs.h"
static void library_unmatched_source_imports(const XrModuleIdentityAuthority *authority,
    const XrXirLibraryCatalog *catalog) {
    XrOsIoPolicy io=xr_compile_io_policy(library_context->resources);CHECK(xr_os_io_mkdir(&io,XR_SOURCE_FIXTURES "/sub",0700)==XR_OS_IO_OK);
    conflict_text("sub/leaf.xr","export fn leaf()->i64 { return 0; }\n");
    conflict_text("nested.xr","import {answer} from \"./library\";\nimport {leaf} from \"./sub/leaf\";\nexport fn result()->i64 { return answer()+leaf(); }\n");
    size_t live=runtime_live,bytes=runtime_bytes,source_count=source_live,source_size=source_bytes;
    for(unsigned mode=0;mode<2;++mode){
        XrCompilerSession *session=library_session_new(library_context);CHECK(session);
        XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/nested.xr",authority,library_context,NULL,NULL,XR_XIR_PROGRAM,mode?catalog:NULL};
        XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
        if(status!=XR_XIR_OK)fprintf(stderr,"nested import mode=%u status=%u %s\n",mode,status,diagnostic.message);
        CHECK(status==XR_XIR_OK&&result.checked&&result.snapshot);
        CHECK(xr_xir_compile_artifact_module(result.checked)->declarations->module_count==3);
        xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
        CHECK(runtime_live==live&&runtime_bytes==bytes&&source_live==source_count&&source_bytes==source_size);
    }
    XrModuleResolverConfig config={0};
    config.catalog=catalog;size_t resource_count=0;CHECK(xr_xir_compile_library_catalog_resources_v2(catalog,&resource_count)&&resource_count==1);
    XrModuleResolver *resolver=library_resolver_new(library_context,&config);CHECK(resolver);
    XrModuleId identity={0};char *error=NULL;
    CHECK(xr_compile_module_resolver_resolve(resolver,"./dir/../library",XR_SOURCE_FIXTURES "/root.xr",authority,&identity,&error)==XR_MODULE_INVALID);
    /* Identity validation may reject before a diagnostic string is allocated. */
    xr_compile_resources_free(error);xr_compile_module_id_cleanup(&identity);xr_compile_module_resolver_free(resolver);
    CHECK(runtime_live==live&&runtime_bytes==bytes&&source_live==source_count&&source_bytes==source_size);
}
#endif // XIR_LIBRARY_DISPATCH_CASES_H
