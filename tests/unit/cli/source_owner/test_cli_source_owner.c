/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_cli_source_owner.c - Real CLI source, diagnostics and resource ownership
 */
#include "app/cli/xcli_canonical_source.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_output.h"
#include "base/xfileio.h"
#include "base/xsha256.h"
#include "base/xwindows_utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "../../xir/xir_sdk_resource_test.h"

typedef struct CliOutput { char bytes[128];size_t length;XrXirOutputSink sink; } CliOutput;
static XrXirOutputStatus output_bytes(void *context,XrXirOutputStream stream,const char *text,size_t length) {
    CliOutput *output=context;
    CHECK(stream==XR_XIR_STDOUT && length<=sizeof(output->bytes)-output->length);
    memcpy(output->bytes+output->length,text,length);output->length+=length;return XR_XIR_OUTPUT_OK;
}
static XrXirOutputStatus output_group(void *context,const XrXirOutputGroup *group) {
    return xr_xir_output_render(&((CliOutput *)context)->sink,group);
}
static DWORD handles(void) { DWORD count=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&count));return count; }
static void execute(XrXirSourceProduct *product,const char *expected) {
    const XrXirSourceView *view=xr_xir_compile_source_product_view(product);
    CHECK(view && view->complete && view->module_count && view->modules[0].path[0]);
    CHECK(xr_xir_compile_source_product_verify(product,16777216,NULL)==XR_XIR_OK);
    XrXirCSource source={0};CHECK(xr_xir_compile_source_product_emit(product,"cli_owner",16777216,&source)==XR_XIR_OK);
    CHECK(source.text && source.length);
    uint32_t entry=xr_xir_compile_source_product_facts(product)->entry;
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_source_product_vm_take(product,&program)==XR_XIR_OK);
    xr_xir_compile_source_product_free(product);
    CHECK(source.text[source.length]==0);xr_xir_compile_c_source_free(&source);
    for (unsigned pass=0;pass<2;++pass) {
        CliOutput output={0};output.sink=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,output_bytes,&output,sizeof(output.bytes)};
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,output_group,&output};
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
        CHECK(result.type==XR_XIR_I64 && !result.payload && !result.reserved);xr_xir_value_drop(&result);
        CHECK(output.length==strlen(expected) && !memcmp(output.bytes,expected,output.length));
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
}
static void input_rejections(const XrCliCompileSourceRequest *original) {
    XrCompileResourceStats before=sdk_stats(original->context->resources);
    XrXirSourceProduct *sentinel=(XrXirSourceProduct *)(uintptr_t)1;
    XrCliCompileSourceDiagnostic diagnostic={0};
    CHECK(xr_cli_compile_source_build(original,&sentinel,&diagnostic)==XR_CLI_COMPILE_SOURCE_BAD_ARGUMENT);
    CHECK(sentinel==(XrXirSourceProduct *)(uintptr_t)1);
    XrCliCompileSourceRequest wrong=*original;wrong.target.abi_version=17;
    XrXirSourceProduct *output=NULL;
    CHECK(xr_cli_compile_source_build(&wrong,&output,&diagnostic)==XR_CLI_COMPILE_SOURCE_UNSUPPORTED && !output);
    CHECK(runtime_attempts==0);
    XrCompileResourceStats after=sdk_stats(original->context->resources);
    CHECK(after.work==before.work && after.allocation_count==before.allocation_count);
    const char *relative[]={"main.xr","C:main.xr","\\main.xr","/main.xr",""};
    for (size_t i=0;i<sizeof(relative)/sizeof(*relative);++i) {
        wrong=*original;wrong.absolute_entry_path=relative[i];
        CHECK(xr_cli_compile_source_build(&wrong,&output,&diagnostic)==XR_CLI_COMPILE_SOURCE_BAD_ARGUMENT && !output);
        CHECK(runtime_attempts==0);
    }
    wrong=*original;wrong.absolute_stdlib_path="stdlib";
    CHECK(xr_cli_compile_source_build(&wrong,&output,&diagnostic)==XR_CLI_COMPILE_SOURCE_BAD_ARGUMENT && !output);
    CHECK(runtime_attempts==0);xr_cli_compile_source_diagnostic_free(&diagnostic);
}
static XrXirLibraryCatalog *catalog(const XrCliCompileSourceRequest *request) {
    XrOsIoPolicy policy=xr_compile_io_policy(request->context->resources);
    char *root=NULL,*path=NULL;
    CHECK(xr_path_dirname_owned(&policy,request->absolute_entry_path,&root)==XR_OS_IO_OK);
    CHECK(xr_path_join_owned(&policy,root,"library.xr",&path)==XR_OS_IO_OK);
    XrModuleIdentityAuthority authority={0};char *script_root=NULL;
    CHECK(xr_compile_module_identity_script_authority_from_source(request->context->resources,path,&authority,&script_root)==XR_MODULE_OK);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(request->context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrXirSourceRequest source={session,path,&authority,request->context,request->absolute_stdlib_path,NULL,XR_XIR_LIBRARY,NULL};
    XrXirSourceResult checked={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_source_check(&source,&checked,&diagnostic,NULL)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(checked.checked,&packet,NULL)==XR_XIR_OK);
    XrXirLibraryInput input={packet.bytes,packet.length,{0}, (XrXirLibraryModuleInput[]){{authority,"library.xr"}},1};
    xr_sha256(packet.bytes,packet.length,input.sha256);
    XrXirLibraryCatalog *result=NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(request->context,&input,1,&result)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_source_result_free(&checked);xr_compile_session_free(session);
    CHECK(DeleteFileA(path));
    xr_compile_resources_free(script_root);xr_compile_resources_free(path);xr_compile_resources_free(root);
    return result;
}
static void catalog_rejections(const XrCliCompileSourceRequest *request) {
    XrCompileResources *foreign=sdk_ledger(&sdk_unlimited);
    XrXirCompileContext context={foreign,request->context->limits};
    XrCliCompileSourceRequest wrong=*request;wrong.context=&context;
    XrXirSourceProduct *output=NULL;XrCliCompileSourceDiagnostic diagnostic={0};
    size_t attempts=runtime_attempts;XrCompileResourceStats before=sdk_stats(foreign);
    CHECK(xr_cli_compile_source_build(&wrong,&output,&diagnostic)==XR_CLI_COMPILE_SOURCE_BAD_ARGUMENT && !output);
    CHECK(runtime_attempts==attempts && sdk_stats(foreign).work==before.work);
    for (unsigned axis=0;axis<5;++axis) {
        context=*request->context;
        if(axis==0)--context.limits.functions;
        if(axis==1)--context.limits.parameters;
        if(axis==2)--context.limits.blocks;
        if(axis==3)--context.limits.instructions;
        if(axis==4)--context.limits.frame_bytes;
        XrCompileResourceStats own_before=sdk_stats(context.resources);
        CHECK(xr_cli_compile_source_build(&wrong,&output,&diagnostic)==XR_CLI_COMPILE_SOURCE_BAD_ARGUMENT && !output);
        CHECK(runtime_attempts==attempts && sdk_stats(context.resources).work==own_before.work);
    }
    xr_compile_resources_release(foreign);
}
static void fault_scan(const char *entry,const char *stdlib) {
    XrCompileResources *resources=sdk_ledger(&sdk_unlimited);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrCliCompileSourceRequest request={&context,entry,stdlib,NULL,{1048576,64},{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *product=NULL;XrCliCompileSourceDiagnostic diagnostic={0};
    runtime_attempts=0;CHECK(xr_cli_compile_source_build(&request,&product,&diagnostic)==XR_CLI_COMPILE_SOURCE_OK);
    size_t sites=runtime_attempts;XrCompileResourceStats total=sdk_stats(resources);
    xr_xir_compile_source_product_free(product);xr_cli_compile_source_diagnostic_free(&diagnostic);xr_compile_resources_release(resources);
    CHECK(!runtime_live && !runtime_bytes);DWORD baseline=handles();
    /* One ledger creation, then six charged prefix reads precede allocation. */
    for (uint64_t cap=1;cap<=6;++cap) {
        XrCompileResourceLimits limits=sdk_unlimited;limits.work=cap;
        resources=sdk_ledger(&limits);context.resources=resources;product=NULL;runtime_attempts=0;
        CHECK(xr_cli_compile_source_build(&request,&product,&diagnostic)==XR_CLI_COMPILE_SOURCE_BUDGET);
        CHECK(!product && !runtime_attempts && sdk_stats(resources).work==cap);
        CHECK(diagnostic.stage==XR_CLI_COMPILE_SOURCE_PATH);
        xr_cli_compile_source_diagnostic_free(&diagnostic);xr_compile_resources_release(resources);
        CHECK(!runtime_live && !runtime_bytes && handles()==baseline);
    }
    size_t snapshots=0;
    for (size_t i=0;i<sites;++i) {
        resources=sdk_ledger(&sdk_unlimited);context.resources=resources;product=NULL;
        runtime_attempts=0;runtime_fail_at=i;
        XrCliCompileSourceStatus status=xr_cli_compile_source_build(&request,&product,&diagnostic);
        runtime_fail_at=SIZE_MAX;
        CHECK(status==XR_CLI_COMPILE_SOURCE_OUT_OF_MEMORY && !product && runtime_attempts>i);
        CHECK(diagnostic.status==status);xr_compile_resources_release(resources);
        if(diagnostic.source.snapshot) {
            ++snapshots;
            const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(diagnostic.source.snapshot);
            CHECK(view && view->complete && view->module_count && view->modules[0].path[0]);
            XrCliCompileSourceDiagnostic saved=diagnostic;
            CHECK(xr_cli_compile_source_build(NULL,&product,&diagnostic)==XR_CLI_COMPILE_SOURCE_BAD_ARGUMENT);
            CHECK(!memcmp(&saved,&diagnostic,sizeof(saved)));
        }
        xr_cli_compile_source_diagnostic_free(&diagnostic);
        CHECK(!runtime_live && !runtime_bytes && handles()==baseline);
    }
    for (unsigned axis=0;axis<3;++axis) for (unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits limits=sdk_unlimited;
        if (axis==0) limits.allocated_bytes=total.allocated_bytes-minus;
        if (axis==1) limits.live_bytes=total.peak_bytes-minus;
        if (axis==2) limits.work=total.work-minus;
        resources=sdk_ledger(&limits);context.resources=resources;product=NULL;
        XrCliCompileSourceStatus status=xr_cli_compile_source_build(&request,&product,&diagnostic);
        CHECK(status==(minus?XR_CLI_COMPILE_SOURCE_BUDGET:XR_CLI_COMPILE_SOURCE_OK));
        CHECK(minus?!product:product!=NULL);
        xr_xir_compile_source_product_free(product);xr_cli_compile_source_diagnostic_free(&diagnostic);xr_compile_resources_release(resources);
        CHECK(!runtime_live && !runtime_bytes && handles()==baseline);
    }
    CHECK(snapshots);
    printf("CLI Source %zu actual malloc failures, %zu owned diagnostic snapshots; allocated/peak/work exact-minus1 and physical zero PASS\n",sites,snapshots);
}
typedef struct FailureObservation {
    XrCompileResourceStats stats;
    size_t attempts;
    XrXirStatus status;
    bool has_path;
} FailureObservation;
static FailureObservation source_failure(const char *entry,const char *stdlib,
    const XrCompileResourceLimits *limits,bool want_path,size_t fail_at) {
    DWORD baseline=handles();
    XrCompileResources *resources=sdk_ledger(limits);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrModuleIdentityAuthority authority={0};char *root=NULL;
    CHECK(xr_compile_module_identity_script_authority_from_source(resources,entry,&authority,&root)==XR_MODULE_OK);
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(resources,&session)==XR_COMPILER_SESSION_OK);
    XrXirSourceRequest request={session,entry,&authority,&context,stdlib,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult result={0},empty=result;
    XrXirSourceDiagnostic diagnostic={0};char *path=NULL;
    char *sentinel=(char *)(uintptr_t)1;
    XrCompileResourceStats before=sdk_stats(resources);
    size_t before_attempts=runtime_attempts;
    CHECK(xr_xir_compile_source_check(&request,&result,&diagnostic,&sentinel)==XR_XIR_BAD_STRUCTURE);
    CHECK(sentinel==(char *)(uintptr_t)1 && !memcmp(&result,&empty,sizeof(result)));
    CHECK(runtime_attempts==before_attempts && sdk_stats(resources).work==before.work);
    result.checked=(XrXirArtifact *)(uintptr_t)1;
    XrXirSourceResult saved=result;
    CHECK(xr_xir_compile_source_check(&request,&result,&diagnostic,&path)==XR_XIR_BAD_STRUCTURE);
    CHECK(!path && !memcmp(&result,&saved,sizeof(result)));
    result=empty;
    runtime_attempts=0;runtime_fail_at=fail_at;
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,want_path?&path:NULL);
    runtime_fail_at=SIZE_MAX;
    FailureObservation observation={sdk_stats(resources),runtime_attempts,status,path!=NULL};
    CHECK(status!=XR_XIR_OK && !memcmp(&result,&empty,sizeof(result)) && diagnostic.status==status);
    xr_compile_session_free(session);xr_compile_resources_free(root);
    xr_compile_resources_release(resources);
    memset(&context,0xcc,sizeof(context));memset(&request,0xcc,sizeof(request));
    memset(&authority,0xcc,sizeof(authority));
    if(path) {
        /* The first path read occurs after every producer and external ledger
         * reference has died. This fixture's independent source is line 1. */
        CHECK(!strcmp(path,entry) && diagnostic.line>0);
        if(status!=XR_XIR_OUT_OF_MEMORY) CHECK(diagnostic.line==1 && diagnostic.column==13);
    }
    xr_compile_resources_free(path);
    CHECK(!runtime_live && !runtime_bytes && handles()==baseline);
    return observation;
}
static void failure_path_scan(const char *entry,const char *stdlib) {
    XrCompileResourceLimits limits=xr_cli_compile_default_resource_limits();
    FailureObservation absent=source_failure(entry,stdlib,&limits,false,SIZE_MAX);
    FailureObservation owned=source_failure(entry,stdlib,&limits,true,SIZE_MAX);
    CHECK(owned.status==XR_XIR_BAD_VALUE && owned.has_path && !absent.has_path);
    CHECK(absent.attempts==owned.attempts && absent.stats.work==owned.stats.work &&
        absent.stats.allocated_bytes==owned.stats.allocated_bytes && absent.stats.peak_bytes==owned.stats.peak_bytes);
    limits.work=owned.stats.work-1;
    FailureObservation budget=source_failure(entry,stdlib,&limits,true,SIZE_MAX);
    CHECK(budget.status==XR_XIR_BUDGET && budget.has_path && budget.attempts==owned.attempts);
    CHECK(budget.stats.work<=limits.work);
    limits=xr_cli_compile_default_resource_limits();
    for(size_t point=0;point<owned.attempts;++point) {
        FailureObservation failed=source_failure(entry,stdlib,&limits,true,point);
        CHECK(failed.status==XR_XIR_OUT_OF_MEMORY && failed.attempts>point);
    }
    printf("SOURCE failure path: %zu real OOM points; identical optional-path allocations/work, exhausted-work transfer and physical zero PASS\n",owned.attempts);
}
static int source_owner_main(int argc,char **argv) {
    (void)sdk_fixture_malloc;(void)sdk_fixture_free;
    CHECK(argc==4 || argc==5);DWORD baseline=handles();
    XrCompileResources *resources=sdk_ledger(&sdk_unlimited);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrCliCompileSourceRequest request={&context,argv[1],argv[2],NULL,{1048576,64},{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    runtime_attempts=0;input_rejections(&request);
    bool library=!strcmp(argv[3],"catalog");XrXirLibraryCatalog *libraries=library?catalog(&request):NULL;
    request.libraries=libraries;if(library)catalog_rejections(&request);
    if(!strcmp(argv[3],"limit"))request.manifest_limits.input_bytes=1;
    XrXirSourceProduct *product=NULL;XrCliCompileSourceDiagnostic diagnostic={0};
    XrCliCompileSourceStatus status=xr_cli_compile_source_build(&request,&product,&diagnostic);
    printf("CLI %s status=%s stage=%u source=%u authority=%u message=%s %s\n",argv[3],
        xr_cli_compile_source_status_name(status),(unsigned)diagnostic.stage,(unsigned)diagnostic.source.status,
        (unsigned)diagnostic.authority_status,diagnostic.source.source.message,diagnostic.authority.message);
    XrCliCompileSourceStatus expected=XR_CLI_COMPILE_SOURCE_OK;
    bool semantic=!strcmp(argv[3],"reject") || !strcmp(argv[3],"failure-path");
    if(semantic || !strcmp(argv[3],"parse-reject"))expected=XR_CLI_COMPILE_SOURCE_REJECTED;
    if(!strcmp(argv[3],"graph-reject"))expected=XR_CLI_COMPILE_SOURCE_NOT_FOUND;
    if(!strcmp(argv[3],"not-found"))expected=XR_CLI_COMPILE_SOURCE_NOT_FOUND;
    if(!strcmp(argv[3],"invalid"))expected=XR_CLI_COMPILE_SOURCE_INVALID;
    if(!strcmp(argv[3],"limit"))expected=XR_CLI_COMPILE_SOURCE_LIMIT;
    CHECK(status==expected && diagnostic.status==status);
    xr_xir_compile_library_catalog_free(libraries);xr_compile_resources_release(resources);
    memset(&context,0xcc,sizeof(context));memset(&request,0xcc,sizeof(request));
    if(status==XR_CLI_COMPILE_SOURCE_OK)execute(product,library?"cli-owner 42\n":"cli-owner 41\n");
    else CHECK(!product);
    if(semantic) {
        CHECK(!diagnostic.source.snapshot);
        CHECK(diagnostic.source.source_path && !strcmp(diagnostic.source.source_path,argc==5?argv[4]:argv[1]));
        CHECK(diagnostic.source.source.line==1 && diagnostic.source.source.column==13);
        CHECK(!strcmp(diagnostic.source.source.message,"name is not an initialized value"));
        if(argc==5) CHECK(diagnostic.source.source.module!=0);
        XrCliCompileSourceDiagnostic saved=diagnostic;
        CHECK(xr_cli_compile_source_build(NULL,&product,&diagnostic)==XR_CLI_COMPILE_SOURCE_BAD_ARGUMENT);
        CHECK(!memcmp(&saved,&diagnostic,sizeof(saved)));
        CHECK(xr_xir_compile_source_product_build(NULL,&product,&diagnostic.source)==XR_XIR_BAD_STRUCTURE);
        CHECK(!memcmp(&saved,&diagnostic,sizeof(saved)));
    }
    if(!strcmp(argv[3],"parse-reject") || !strcmp(argv[3],"graph-reject"))
        CHECK(!diagnostic.source.source_path && !diagnostic.source.snapshot && !diagnostic.source.source.line);
    xr_cli_compile_source_diagnostic_free(&diagnostic);xr_cli_compile_source_diagnostic_free(&diagnostic);
    CHECK(!runtime_live && !runtime_bytes && handles()==baseline);
    if(!strcmp(argv[3],"scan"))fault_scan(argv[1],argv[2]);
    if(!strcmp(argv[3],"reject"))failure_path_scan(argv[1],argv[2]);
    puts("CLI Source producer/result/diagnostic lifetime and two-instance independent expectation PASS");return 0;
}
int wmain(int argc,wchar_t **wide_argv) {
    XrWinPathStatus status;
    char **argv=xr_win_utf16_arguments(argc,wide_argv,&status);CHECK(argv);
    int result=source_owner_main(argc,argv);
    xr_win_utf8_arguments_free(argc,argv);return result;
}
