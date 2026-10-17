/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_structural_source_reject.c - Invalid generic definitions never publish artifacts
 */
#include "xir/xxir_source.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
#include "../xir_source_fixture_owner.h"
int main(int argc,char **argv) {
    CHECK(argc==2);char path[1024];
    int length=snprintf(path,sizeof(path),"%s/structural_reject_%s.xr",XR_LANGUAGE_FIXTURES,argv[1]);
    CHECK(length>0 && (size_t)length<sizeof(path));
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK && session);
    const XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_LANGUAGE_FIXTURES};
    const XrXirSourceRequest request={session,path,&authority,&owner.context,XR_LANGUAGE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    printf("%s status=%u line=%d column=%d message=%s\n",argv[1],status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_BAD_TYPE && diagnostic.status==status && diagnostic.line>0 && diagnostic.message[0]);
    CHECK(!result.checked && !result.snapshot);
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
    source_fixture_owner_free(&owner);return 0;
}
