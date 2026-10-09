
#include "module/xlockfile.h"
static XrXirStatus overlay_package_once(const char *root,XrCompileResourceLimits limits,unsigned mode,XrCompileResourceStats *stats) {
    XrXirCompileContext context={0};XrCompilerSession *session=NULL;XrLockfile *lock=NULL;XrXirSourceResult result={0};
    XrCompileResourceStatus made=xr_compile_resources_new(&limits,&context.resources);
    if(made!=XR_COMPILE_RESOURCE_OK)return made==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    context.limits=xr_xir_compile_default_limits();
    XrCompilerSessionStatus opened=xr_compile_session_new(context.resources,&session);
    XrXirStatus status=opened==XR_COMPILER_SESSION_OK?XR_XIR_OK:
        opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrOsIoPolicy policy=xr_compile_io_policy(context.resources);
    if(status==XR_XIR_OK) {
        XrOsIoStatus st=xr_lockfile_new_owned(&policy,&lock);
        if(st==XR_OS_IO_OK)st=xr_lockfile_add_package_owned(lock,"owner/package","1.2.3","fixture.tar.gz",
            mode==1?"sha256:0000000000000000000000000000000000000000000000000000000000000000":
            "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        if(st!=XR_OS_IO_OK)status=st==XR_OS_IO_BUDGET?XR_XIR_BUDGET:st==XR_OS_IO_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
    }
    char path[XR_TEST_PATH_MAX],base[XR_TEST_PATH_MAX],entry[XR_TEST_PATH_MAX];
    CHECK(snprintf(base,sizeof(base),"%s/.xray/packages/owner/package/1.2.3/src",root)>0);
    CHECK(snprintf(path,sizeof(path),"%s/main.xr",base)>0&&snprintf(entry,sizeof(entry),"%s/root.xr",root)>0);
    const char *text="import { packageValue } from \"owner/package\"\nexport fn result()->i64 { return packageValue() }\n";
    const char *leaf="export fn packageValue()->i64 { return 43 }\n";
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrModuleOverlayInput inputs[2]={{authority,"root.xr",entry,text,strlen(text)},
        {{XR_MODULE_IDENTITY_SCRIPT,NULL,base},"main.xr",path,leaf,strlen(leaf)}};
    if(status==XR_XIR_OK) {
        XrXirSourceRequest request={session,entry,&authority,&context,NULL,mode==2?NULL:lock,XR_XIR_PROGRAM,NULL};
        status=xr_xir_compile_source_check_overlay(&request,inputs,2,&result,NULL,NULL);
    }
    xr_compile_session_free(session);xr_lockfile_free_owned(lock);
    if(status==XR_XIR_OK) {
        const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result.snapshot);CHECK(view&&view->complete);
        const XrXirSourceDeclaration *fn=overlay_function(view,"packageValue");
        CHECK(strstr(view->modules[fn->range.module].identity,"package")&&
            strstr(view->modules[fn->range.module].identity,"owner/package@1.2.3")&&
            strstr(view->modules[fn->range.module].identity,"src/main.xr"));
        status=xr_xir_compile_artifact_verify(result.checked,NULL);
    } else CHECK(!result.checked&&!result.snapshot);
    if(stats)CHECK(xr_compile_resources_stats(context.resources,stats)==XR_COMPILE_RESOURCE_OK);
    xr_compile_resources_release(context.resources);xr_xir_compile_source_result_free(&result);overlay_source_zero();return status;
}
static void overlay_package_cases(const char *root) {
    const char *relative[]={".xray",".xray/cache",".xray/packages",".xray/packages/owner",
        ".xray/packages/owner/package",".xray/packages/owner/package/1.2.3",".xray/packages/owner/package/1.2.3/src"};
    char paths[7][XR_TEST_PATH_MAX],archive[XR_TEST_PATH_MAX],fallback[XR_TEST_PATH_MAX];
    for(unsigned i=0;i<7;++i){CHECK(snprintf(paths[i],sizeof(paths[i]),"%s/%s",root,relative[i])>0);CHECK(xr_test_mkdir(paths[i])==0);}
    CHECK(snprintf(archive,sizeof(archive),"%s/owner-package-1.2.3.tar.gz",paths[1])>0);
    FILE *f=fopen(archive,"wb");CHECK(f&&fputs("abc",f)>=0&&fclose(f)==0);
    /* Existing fallback is deliberately invalid; src/main overlay has priority. */
    CHECK(snprintf(fallback,sizeof(fallback),"%s/main.xr",paths[5])>0);
    f=fopen(fallback,"wb");CHECK(f&&fputs("invalid disk source\n",f)>=0&&fclose(f)==0);
    const char *environment=getenv("HOME");char *saved=environment?xr_strdup(environment):NULL;CHECK(!environment||saved);
    CHECK(xr_test_setenv("HOME",root,1)==0);
    size_t sites=0;XrCompileResourceStats stats={0};
    for(size_t probe=0;probe<=sites;++probe) {
        source_fixture_compile_attempts=0;source_fixture_compile_fail_at=probe?probe-1:SIZE_MAX;source_fixture_compile_injected=false;
        XrXirStatus status=overlay_package_once(root,overlay_source_limits(),0,probe?NULL:&stats);
        if(!probe){CHECK(status==XR_XIR_OK);sites=source_fixture_compile_attempts;CHECK(sites);}
        else CHECK(status==XR_XIR_OUT_OF_MEMORY&&source_fixture_compile_injected);
    }
    source_fixture_compile_fail_at=SIZE_MAX;source_fixture_compile_injected=false;
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
        XrCompileResourceLimits exact={stats.allocated_bytes,stats.peak_bytes,stats.work};
        uint64_t *bound=axis==0?&exact.allocated_bytes:axis==1?&exact.live_bytes:&exact.work;
        CHECK(*bound&&*bound<INT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
        CHECK(overlay_package_once(root,exact,0,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));
    }
    CHECK(overlay_package_once(root,overlay_source_limits(),1,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(overlay_package_once(root,overlay_source_limits(),2,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(saved?xr_test_setenv("HOME",saved,1)==0:xr_test_unsetenv("HOME")==0);xr_free(saved);
    CHECK(xr_test_unlink(archive)==0&&xr_test_unlink(fallback)==0);
    for(unsigned i=7;i>0;--i)CHECK(xr_test_rmdir(paths[i-1])==0);
}
/* Copyright (c) 2026 Xinglei Xu. MIT License. */
/* root/sub/lib.xr is opened independently as a SCRIPT rooted in sub; the
 * importer binds those same immutable bytes under its PROJECT authority.
 * std/io/edited is opened as SCRIPT too; descriptor admission must bind STDLIB.
 * Neither leaf exists on disk. Text types and actual calls are fixed oracles. */
static XrXirStatus overlay_authority_once(const char *root,XrCompileResourceLimits limits,unsigned mode,
    XrCompileResourceStats *stats) {
    XrXirCompileContext context={0};XrCompilerSession *session=NULL;XrXirSourceResult result={0};
    XrCompileResourceStatus made=xr_compile_resources_new(&limits,&context.resources);
    if(made!=XR_COMPILE_RESOURCE_OK)return made==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    context.limits=xr_xir_compile_default_limits();
    XrCompilerSessionStatus opened=xr_compile_session_new(context.resources,&session);
    XrXirStatus status=opened==XR_COMPILER_SESSION_OK?XR_XIR_OK:
        opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    char sub[XR_TEST_PATH_MAX],io[XR_TEST_PATH_MAX],paths[3][XR_TEST_PATH_MAX];
    CHECK(snprintf(sub,sizeof(sub),"%s/sub",root)>0);CHECK(snprintf(io,sizeof(io),"%s/io",root)>0);
    CHECK(snprintf(paths[0],sizeof(paths[0]),"%s/main.xr",root)>0);
    CHECK(snprintf(paths[1],sizeof(paths[1]),"%s/lib.xr",sub)>0);
    CHECK(snprintf(paths[2],sizeof(paths[2]),"%s/edited.xr",io)>0);
    const char *texts[]={"import { local } from \"./sub/lib\"\nimport { standard } from \"std/io/edited\"\nexport fn result()->i64 { return local()+standard() }\n",
        "export fn local()->i64 { return 41 }\n","export fn standard()->i64 { return 42 }\n"};
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_PROJECT,"overlay-project",root};
    XrModuleOverlayInput inputs[3]={
        {authority,"main.xr",paths[0],texts[0],strlen(texts[0])},
        {{XR_MODULE_IDENTITY_SCRIPT,NULL,sub},"lib.xr",paths[1],texts[1],strlen(texts[1])},
        {{XR_MODULE_IDENTITY_SCRIPT,NULL,io},"edited.xr",paths[2],texts[2],strlen(texts[2])}};
    if(mode==1){inputs[1].text="fn local()->i64 { return 41 }\n";inputs[1].length=strlen(inputs[1].text);}
    if(mode==2){inputs[2].text="export fn standard()->i64 { return \"bad\" }\n";inputs[2].length=strlen(inputs[2].text);}
    if(status==XR_XIR_OK) {
        XrXirSourceRequest request={session,paths[0],&authority,&context,root,NULL,XR_XIR_PROGRAM,NULL};
        status=xr_xir_compile_source_check_overlay(&request,inputs,3,&result,NULL,NULL);
    }
    xr_compile_session_free(session);
    if(status==XR_XIR_OK) {
        const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result.snapshot);CHECK(view&&view->complete);
        const XrXirSourceDeclaration *local=overlay_function(view,"local"),*standard=overlay_function(view,"standard");
        CHECK(local->range.module!=standard->range.module);
        CHECK(strstr(view->modules[local->range.module].identity,"overlay-project"));
        CHECK(strstr(view->modules[standard->range.module].identity,"stdlib-module-v1:module=2:io:path=12:io/edited.xr"));
        unsigned calls=0;
        for(uint32_t i=0;i<view->reference_count;++i)if(view->references[i].access==XR_XIR_SOURCE_CALL&&
            (view->references[i].target==local->id||view->references[i].target==standard->id))++calls;
        CHECK(calls==2);
        status=xr_xir_compile_artifact_verify(result.checked,NULL);
    } else CHECK(!result.checked&&!result.snapshot);
    if(stats)CHECK(xr_compile_resources_stats(context.resources,stats)==XR_COMPILE_RESOURCE_OK);
    xr_compile_resources_release(context.resources);xr_xir_compile_source_result_free(&result);
    overlay_source_zero();return status;
}
static void overlay_authority_cases(const char *root) {
    char sub[XR_TEST_PATH_MAX],io[XR_TEST_PATH_MAX];
    CHECK(snprintf(sub,sizeof(sub),"%s/sub",root)>0&&xr_test_mkdir(sub)==0);
    CHECK(snprintf(io,sizeof(io),"%s/io",root)>0&&xr_test_mkdir(io)==0);
    size_t sites=0;XrCompileResourceStats stats={0};
    for(size_t probe=0;probe<=sites;++probe) {
        source_fixture_compile_attempts=0;source_fixture_compile_fail_at=probe?probe-1:SIZE_MAX;
        source_fixture_compile_injected=false;
        XrXirStatus status=overlay_authority_once(root,overlay_source_limits(),0,probe?NULL:&stats);
        if(!probe){CHECK(status==XR_XIR_OK);sites=source_fixture_compile_attempts;CHECK(sites);}
        else CHECK(status==XR_XIR_OUT_OF_MEMORY&&source_fixture_compile_injected);
    }
    source_fixture_compile_fail_at=SIZE_MAX;source_fixture_compile_injected=false;
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
        XrCompileResourceLimits exact={stats.allocated_bytes,stats.peak_bytes,stats.work};
        uint64_t *bound=axis==0?&exact.allocated_bytes:axis==1?&exact.live_bytes:&exact.work;
        CHECK(*bound&&*bound<INT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
        CHECK(overlay_authority_once(root,exact,0,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));
    }
    CHECK(overlay_authority_once(root,overlay_source_limits(),1,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(overlay_authority_once(root,overlay_source_limits(),2,NULL)==XR_XIR_BAD_TYPE);
    CHECK(xr_test_rmdir(sub)==0&&xr_test_rmdir(io)==0);
    overlay_package_cases(root);
}
