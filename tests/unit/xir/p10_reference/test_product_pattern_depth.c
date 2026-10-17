/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_product_pattern_depth.c - Bounded nesting rejects before publishing Source
 */
#include "xir/xxir_source.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
#include "../xir_source_fixture_owner.h"
int main(void) {
    char text[2048];size_t at=0;
    const char *prefix="export fn referenceAnswer()->i64 {\n const ";
    size_t prefix_length=strlen(prefix);memcpy(text,prefix,prefix_length);at=prefix_length;
    for (unsigned i=0;i<129;++i) text[at++]='(';
    text[at++]='n';
    for (unsigned i=0;i<129;++i) {text[at++]=',';text[at++]=')';}
    const char *suffix="=42; return 42\n}\n";
    memcpy(text+at,suffix,strlen(suffix)+1);
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK);
    const XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"pattern-depth",NULL};
    const XrXirSourceRequest request={session,NULL,&authority,&owner.context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    const XrXirSourceText input={NULL,text,strlen(text)};XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&result,&diagnostic,NULL);
    printf("nested product depth: status=%u line=%d message=%s\n",status,diagnostic.line,diagnostic.message);
    CHECK(status==XR_XIR_BUDGET && diagnostic.status==status && diagnostic.line==2);
    CHECK(strstr(diagnostic.message,"product binding nesting exhausted"));
    CHECK(!result.checked && !result.snapshot);
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);source_fixture_owner_free(&owner);
    return 0;
}
