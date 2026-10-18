/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_input_raw_resources.h - Exact Source receiving outside the text overlay
 */
#include "xir/xxir_source.h"
#include "xir/xxir_library_catalog.h"
typedef struct RawInputOwners {
    XrSourceCompiler *compiler;
    XrSourceAuthority *authority;
    XrXirSourceResult result;
    XrXirSourceDiagnostic diagnostic;
    XrXirSourceFailure failure;
} RawInputOwners;
static XrSourceStatus raw_input_prepare(RawInputOwners *owner,const XrCompileResourceLimits *limits) {
    XrSourceCompilerOptions options;CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.stdlib_path=XR_PUBLIC_STDLIB;options.allocated_bytes=limits->allocated_bytes;
    options.live_bytes=limits->live_bytes;options.work=limits->work;
    XrSourceStatus status=xray_source_compiler_new(&options,&owner->compiler);
    if(status!=XR_SOURCE_OK)return status;
    const XrSourceEntryInput entry={XR_SOURCE_ENTRY_AST,XR_INPUT_DIAGNOSTIC_FIXTURE "/坏 库.xr",NULL,0};
    XrSourceCompileDiagnostic detail={0};
    XrSourceCompileStatus opened=xr_source_compiler_open_input(owner->compiler,&entry,&owner->authority,&detail);
    xr_source_compile_diagnostic_free(&detail);return api_compile(opened);
}
static XrXirStatus raw_input_reject(RawInputOwners *owner,bool rooted) {
    const char bad[]="// 中\n\0print(42)\n";
    const XrModuleIdentityAuthority memory={XR_MODULE_IDENTITY_MEMORY,"raw-input-diagnostics",NULL};
    const XrXirSourceRequest request={xr_source_compiler_session(owner->compiler),
        rooted?xr_source_authority_source_path(owner->authority):NULL,
        rooted?xr_source_authority_entry(owner->authority):&memory,
        xr_source_authority_context(owner->authority),XR_PUBLIC_STDLIB,xr_source_authority_lockfile(owner->authority),
        XR_XIR_PROGRAM,xr_source_authority_catalog(owner->authority)};
    const XrXirSourceText text={rooted?xr_source_authority_logical_path(owner->authority):NULL,bad,sizeof(bad)-1};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&text,&owner->result,&owner->diagnostic,&owner->failure);
    CHECK(!owner->result.checked && !owner->result.snapshot);
    return status;
}
static void raw_input_close(RawInputOwners *owner,bool normal,bool rooted) {
    xr_source_authority_close(owner->authority);xray_source_compiler_free(owner->compiler);
    if(normal) {
        CHECK(owner->diagnostic.status==XR_XIR_BAD_STRUCTURE && owner->failure.origin==XR_XIR_SOURCE_DIAGNOSTIC_INPUT);
        CHECK(owner->diagnostic.line==2 && owner->diagnostic.column==1 &&
            !strcmp(owner->diagnostic.message,"source text contains an embedded NUL"));
        CHECK(rooted?owner->failure.path && strstr(owner->failure.path,"坏 库.xr"):owner->failure.path==NULL);
    }
    xr_xir_compile_source_result_free(&owner->result);xr_xir_compile_source_failure_free(&owner->failure);
    *owner=(RawInputOwners){0};CHECK(!runtime_live && !runtime_bytes);
}
static void raw_input_resources(bool rooted) {
    RawInputOwners owner={0};XrCompileResourceStats required={0};size_t sites=0;
    for(size_t point=0;!point || point<=sites;++point) {
        runtime_fail_at=SIZE_MAX;CHECK(raw_input_prepare(&owner,&generous)==XR_SOURCE_OK);
        runtime_attempts=0;runtime_fail_at=point?point-1:SIZE_MAX;
        XrXirStatus status=raw_input_reject(&owner,rooted);
        if(!point) {
            CHECK(status==XR_XIR_BAD_STRUCTURE);sites=runtime_attempts;CHECK(sites);
            CHECK(xr_compile_resources_stats(xr_source_compiler_resources(owner.compiler),&required)==XR_COMPILE_RESOURCE_OK);
        } else CHECK(status==XR_XIR_OUT_OF_MEMORY && runtime_attempts>runtime_fail_at);
        runtime_fail_at=SIZE_MAX;raw_input_close(&owner,!point,rooted);
    }
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
        XrCompileResourceLimits limits={required.allocated_bytes,required.peak_bytes,required.work};
        uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        CHECK(*bound>1);*bound=delta<0?*bound-1:*bound+(unsigned)delta;
        XrSourceStatus prepared=raw_input_prepare(&owner,&limits);
        if(prepared==XR_SOURCE_OK)CHECK(raw_input_reject(&owner,rooted)==(delta<0?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE));
        else CHECK(delta<0 && prepared==XR_SOURCE_BUDGET);
        raw_input_close(&owner,delta>=0,rooted);
    }
    printf("direct Source NUL rooted=%u failure sites=%zu swept=%zu exact axes=%llu/%llu/%llu physical=0/0\n",
        (unsigned)rooted,sites,sites,(unsigned long long)required.allocated_bytes,
        (unsigned long long)required.peak_bytes,(unsigned long long)required.work);
}
static void authority_input_receivers(void) {
    RawInputOwners owner={0};CHECK(raw_input_prepare(&owner,&generous)==XR_SOURCE_OK);
    const char bad[]="// 中\n\0print(42)\n";
    const XrSourceEntryInput input={XR_SOURCE_ENTRY_TEXT,XR_INPUT_DIAGNOSTIC_FIXTURE "/坏 库.xr",bad,sizeof(bad)-1};
    const XrXirCompileContext *context=xr_source_authority_context(owner.authority);
    const XrXirLibraryCatalog *catalog=xr_source_authority_catalog(owner.authority);
    const XrTomlParseLimits limits={1024*1024,64};
    XrSourceAuthority *output=NULL;XrXirSourceDiagnostic detail={.status=XR_XIR_BAD_STAGE};
    char occupied[]="occupied";XrXirSourceFailure failure={.path=occupied};
    XrSourceAuthorityDiagnostics receivers={NULL,&detail,&failure};
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    size_t attempts=runtime_attempts;
    CHECK(xr_source_authority_open_input(context,&input,catalog,&limits,&output,&receivers)==XR_MANIFEST_BAD_ARGUMENT);
    CHECK(!output && failure.path==occupied && detail.status==XR_XIR_BAD_STAGE && runtime_attempts==attempts);
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK && !memcmp(&before,&after,sizeof(before)));
    XrSourceEntryInput oversized=input;oversized.length=SIZE_MAX;failure=(XrXirSourceFailure){0};
    CHECK(xr_source_authority_open_input(context,&oversized,catalog,&limits,&output,&receivers)==XR_MANIFEST_BUDGET);
    CHECK(!output && xr_xir_compile_source_failure_empty(&failure) && detail.status==XR_XIR_BAD_STAGE);
    CHECK(xr_source_authority_open_input(context,&input,catalog,&limits,&output,NULL)==XR_MANIFEST_INVALID && !output);
    failure=(XrXirSourceFailure){0};receivers.source=NULL;
    CHECK(xr_source_authority_open_input(context,&input,catalog,&limits,&output,&receivers)==XR_MANIFEST_INVALID && !output && failure.path);
    xr_xir_compile_source_failure_free(&failure);failure=(XrXirSourceFailure){0};receivers.source=&detail;
    CHECK(xr_source_authority_open_input(context,&input,catalog,&limits,&output,&receivers)==XR_MANIFEST_INVALID && !output && failure.path);
    CHECK(detail.status==XR_XIR_BAD_STRUCTURE && failure.origin==XR_XIR_SOURCE_DIAGNOSTIC_INPUT &&
        detail.line==2 && detail.column==1 && !strcmp(detail.message,"source text contains an embedded NUL"));
    xr_source_authority_close(owner.authority);owner.authority=NULL;
    xray_source_compiler_free(owner.compiler);owner.compiler=NULL;
    CHECK(strstr(failure.path,"坏 库.xr"));xr_xir_compile_source_failure_free(&failure);raw_input_close(&owner,false,false);
    puts("Source authority occupied receiver preserves outputs/ledger; optional diagnostics and owned canonical path PASS");
}
