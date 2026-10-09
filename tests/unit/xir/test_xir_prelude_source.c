/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */
/* Canonical prelude from actual owned Source and complete immutable Catalog. */
#include "base/xmalloc.h"
#include "base/xsha256.h"
#include "xir/xxir_source.h"
#include "xir/xxir_prelude_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_library_catalog.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if (!(x)) {fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}} while(0)
#include "xir_library_compile_owner.h"
#include "xir/xxir_source_query.c"
#include "xir/xxir_type_inference.c"
#include "xir/xxir_source.c"
#include "xir_library_source_owner.h"
#include "xir_prelude_runtime.h"
#include "xir_prelude_source_cases.h"
static void prelude_query(const XrXirSourceResult *result,bool generated) {
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result->snapshot);
    CHECK(view && view->complete);uint32_t module=UINT32_MAX,crypto=0,ordering=0;
    for (uint32_t m=0;m<view->module_count;++m)
        if (!strcmp(view->modules[m].identity,XIR_PRELUDE_CANONICAL)) { CHECK(module==UINT32_MAX);module=m; }
    CHECK(module!=UINT32_MAX);
    if (generated) {
        CHECK(!view->modules[module].path);
        XrFingerprint expected={{0}};xr_module_source_fingerprint(xir_prelude_source.text,&expected);
        CHECK(!memcmp(&expected,&view->modules[module].fingerprint,sizeof(expected)));
    }
    for (uint32_t d=0;d<view->declaration_count;++d) {
        const XrXirSourceDeclaration *row=&view->declarations[d];
        if (row->range.module!=module || row->kind!=XR_XIR_SOURCE_TYPE) continue;
        if (!strcmp(row->name,"CryptoError")) {++crypto;CHECK(row->exported && row->native_identity==0);if(generated)CHECK(row->range.line==3 && row->range.column>0);}
        if (!strcmp(row->name,"Ordering")) {++ordering;CHECK(row->exported && row->native_identity==4);if(generated)CHECK(row->range.line==2 && row->range.column>0);}
    }
    CHECK(crypto==1 && ordering==1);
}
static XrXirSourceResult prelude_check(const XrXirCompileContext *context,const char *file,
    const XrXirLibraryCatalog *catalog) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    XrXirSourceRequest request={session,file,&authority,context,NULL,NULL,XR_XIR_PROGRAM,catalog};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"prelude source %s status%u %d:%d %s\n",file,status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && result.checked && result.snapshot);xr_compile_session_free(session);return result;
}
static void prelude_catch_dependencies(const XrXirCompileContext *context,const XrXirSourceResult *result) {
    const XrXirDeclarations *d=xr_xir_compile_artifact_module(result->checked)->declarations;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};char *canonical=NULL;
    CHECK(xr_compile_module_identity_from_logical(context->resources,&authority,"catch_only_child.xr",&canonical)==XR_MODULE_OK);
    uint32_t child=UINT32_MAX,prelude=UINT32_MAX;
    for (uint32_t m=0;m<d->module_count;++m) {
        const XrXirSourceModule *row=&d->modules[m];
        if (row->name_length==strlen(canonical) && !memcmp(row->name,canonical,row->name_length)) child=m;
        if (row->name_length==sizeof(XIR_PRELUDE_CANONICAL)-1 && !memcmp(row->name,XIR_PRELUDE_CANONICAL,row->name_length)) prelude=m;
    }
    xr_compile_resources_free(canonical);CHECK(child!=UINT32_MAX && prelude!=UINT32_MAX);
    CHECK(d->modules[child].dependency_count==1 && d->modules[child].dependencies[0]==prelude);
    CHECK(d->modules[d->root_module].dependency_count==1 && d->modules[d->root_module].dependencies[0]==child);
}
static void prelude_program(const XrXirCompileContext *context,const XrXirLibraryCatalog *catalog,const char *packet_path) {
    XrXirSourceResult source=prelude_check(context,XR_SOURCE_FIXTURES "/original_array_error_invoke_root.xr",catalog);
    prelude_query(&source,!catalog);XrXirArtifact *owned=NULL;
    CHECK(prelude_roundtrip(context,source.checked,&owned)==XR_XIR_OK);xr_xir_compile_source_result_free(&source);
    if (packet_path) {
        XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(owned,&packet,NULL)==XR_XIR_OK);
        FILE *file=fopen(packet_path,"wb");CHECK(file && fwrite(packet.bytes,1,packet.length,file)==packet.length && !fclose(file));
        xr_xir_compile_checked_packet_free(&packet);
    }
    xr_test_prelude_run(context,owned);
}
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==3);LibraryCompileOwner compiler={0};
    CHECK(library_compile_owner_new(&compiler,&library_compile_limits)==XR_XIR_OK);
    library_compile_operation_cases("Canonical generated prelude Source and two reads",prelude_factory_operation,NULL);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    LibrarySourceFixture direct={NULL,0,XR_SOURCE_FIXTURES "/original_array_error_invoke_root.xr",authority,XR_XIR_PROGRAM};
    library_compile_operation_cases("Original317B Crypto42 whole Source two reads",prelude_source_operation,&direct);
    prelude_program(&compiler.context,NULL,argc==3 ? argv[1] : NULL);
    XrXirSourceResult provider={0};CHECK(xr_xir_compile_prelude_library(&compiler.context,&provider,NULL)==XR_XIR_OK);
    prelude_query(&provider,true);XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(provider.checked,&packet,NULL)==XR_XIR_OK);
    char logical[64]="prelude/builtin_symbols.def",name[16]="prelude",physical[1024];
    int root=snprintf(physical,sizeof(physical),"%s",XR_SOURCE_STDLIB);CHECK(root>0 && (size_t)root<sizeof(physical));
    XrXirLibraryModuleInput binding={{XR_MODULE_IDENTITY_STDLIB,name,physical},logical};
    XrXirLibraryInput input={.packet=packet.bytes,.length=packet.length,.modules=&binding,.module_count=1};
    xr_sha256(input.packet,input.length,input.sha256);
    prelude_catalog_negatives(&compiler.context,provider.checked,&binding);
    LibrarySourceFixture linked={&input,1,direct.entry,authority,XR_XIR_PROGRAM};
    library_compile_operation_cases("Canonical complete prelude Catalog",library_catalog_operation,&linked);
    library_compile_operation_cases("Original317B Crypto42 Catalog Source two reads",prelude_source_operation,&linked);
    XrXirLibraryCatalog *catalog=NULL;CHECK(xr_xir_compile_library_catalog_new_v2(&compiler.context,&input,1,&catalog)==XR_XIR_OK);
    prelude_occupied(&compiler.context,&provider,&input,catalog);
    prelude_catalog_source_no_fallback(&compiler.context,catalog);
    xr_xir_compile_source_result_free(&provider);memset(packet.bytes,0,packet.length);xr_xir_compile_checked_packet_free(&packet);
    memset(logical,0,sizeof(logical));memset(name,0,sizeof(name));memset(physical,0,sizeof(physical));memset(&input,0,sizeof(input));memset(&binding,0,sizeof(binding));
    prelude_program(&compiler.context,catalog,argc==3 ? argv[2] : NULL);
    const char *positive[]={XR_SOURCE_FIXTURES "/catch_only_root.xr",XR_SOURCE_FIXTURES "/ordering.xr",XR_SOURCE_FIXTURES "/shadow.xr"};
    for (unsigned flow=0;flow<2;++flow) for (unsigned c=0;c<3;++c) {
        XrXirSourceResult result=prelude_check(&compiler.context,positive[c],flow ? catalog : NULL);
        if (c<2) prelude_query(&result,!flow);
        if (!c) prelude_catch_dependencies(&compiler.context,&result);
        XrXirArtifact *owned=NULL;CHECK(prelude_roundtrip(&compiler.context,result.checked,&owned)==XR_XIR_OK);
        xr_xir_compile_source_result_free(&result);xr_test_prelude_run(&compiler.context,owned);
    }
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(compiler.context.resources,&session)==XR_COMPILER_SESSION_OK);
    const char *negative[]={XR_SOURCE_FIXTURES "/foreign_variant.xr",XR_SOURCE_FIXTURES "/foreign_ordering.xr"};
    for (unsigned n=0;n<2;++n) {
        XrXirSourceRequest request={session,negative[n],&authority,&compiler.context,NULL,NULL,XR_XIR_PROGRAM,catalog};
        XrXirSourceResult result={0};XrXirStatus status=xr_xir_compile_source_check(&request,&result,NULL,NULL);
        CHECK(status==XR_XIR_BAD_TYPE && !result.checked && !result.snapshot);
    }
    xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);
    library_compile_owner_drop(&compiler);library_compile_observer_free();
    puts("Canonical Ordering4/Crypto0 Source/Catalog original317B=42 catch-only real dependency physicalzero PASS");return 0;
}
