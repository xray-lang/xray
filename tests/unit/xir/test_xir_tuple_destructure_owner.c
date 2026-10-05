/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_tuple_destructure_owner.c - One finite Source/Checked/native/Program ledger and all compiler faults
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_source_program_compile_owner.h"
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_source_query_internal.h"
static void query_bindings(const XrXirSourceView *view) {
    CHECK(view && view->complete);uint32_t make=0;
    for(uint32_t i=0;i<view->declaration_count;++i) {
        const XrXirSourceDeclaration *d=&view->declarations[i];
        if(d->kind==XR_XIR_SOURCE_FUNCTION && !strcmp(d->name,"make")){CHECK(!make);make=d->id;}
        CHECK(d->kind!=XR_XIR_SOURCE_BINDING || strcmp(d->name,"_"));
    }
    CHECK(make);
    const char *names[]={"text","items","unit","nested","atomic","single","generic","copy","shared","value"};
    const bool mutable[]={true,true,true,true,true,false,false,true,false,false};
    for(uint32_t n=0;n<10;++n) {
        unsigned found=0;
        for(uint32_t i=0;i<view->declaration_count;++i) {
            const XrXirSourceDeclaration *d=&view->declarations[i];
            if(d->kind==XR_XIR_SOURCE_BINDING && d->parent==make && !strcmp(d->name,names[n])) {
                ++found;CHECK(d->type.known && d->mutable==mutable[n] && !d->exported && !d->native_identity);
                CHECK(d->range.line>0 && d->range.column>0 && d->range.end_column>d->range.column);
                if(n==2)CHECK(d->type.type==XR_XIR_UNIT);
            }
        }
        CHECK(found==1);
    }
}
static XrXirStatus pipeline(XrCompileResourceLimits caps,XrCompileResourceStats *measured) {
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    XrCompileResourceStatus resource=xr_compile_resources_new(&caps,&context.resources);
    if(resource!=XR_COMPILE_RESOURCE_OK)return resource==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    XrCompileResourceStats baseline={0};CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session=NULL;XrXirSourceProduct *product=NULL;XrXirArtifact *checked=NULL;
    XrXirProgram *program=NULL;XrXirSourceSnapshot *snapshot=NULL;XrXirCSource source={0};
    XrXirSourceProductDiagnostic diagnostic={0};
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_TUPLE_DESTRUCTURE_FIXTURES};
    XrXirSourceProductRequest request={{NULL,XR_TUPLE_DESTRUCTURE_FIXTURES "/root.xr",&authority,&context,
        XR_TUPLE_DESTRUCTURE_STDLIB,NULL,XR_XIR_PROGRAM,NULL},{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    XrCompilerSessionStatus created=xr_compile_session_new(context.resources,&session);
    XrXirStatus status=created==XR_COMPILER_SESSION_OK ? XR_XIR_OK :
        created==XR_COMPILER_SESSION_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    request.source.session=session;
    if(status==XR_XIR_OK)status=xr_xir_compile_source_product_build(&request,&product,&diagnostic);
    CHECK(status==XR_XIR_OK || !product);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);xr_compile_session_free(session);
    memset(&request,0xcc,sizeof(request));memset(&authority,0xcc,sizeof(authority));
    if(status==XR_XIR_OK){query_bindings(xr_xir_compile_source_product_view(product));
        status=xr_xir_compile_source_snapshot_copy(&context,xr_xir_compile_source_product_view(product),&snapshot);}
    if(status==XR_XIR_OK) {
        XrXirSourceProductPacketView packet={0};
        status=xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet);
        if(status==XR_XIR_OK)status=xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&checked,NULL);
    }
    xr_xir_compile_artifact_free(checked);
    if(status==XR_XIR_OK)status=xr_xir_compile_source_product_emit(product,"tuple_destructure_owner",1048576,&source);
    if(status==XR_XIR_OK)CHECK(!strstr(source.text,"({"));
    xr_xir_compile_c_source_free(&source);
    if(status==XR_XIR_OK)status=xr_xir_compile_source_product_vm_take(product,&program);
    xr_xir_compile_source_product_free(product);xr_xir_compile_program_drop(program);
    if(status==XR_XIR_OK)query_bindings(xr_xir_compile_source_snapshot_view(snapshot));
    xr_xir_compile_source_snapshot_free(snapshot);
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
    CHECK(stats.live_bytes==baseline.live_bytes);if(measured)*measured=stats;
    xr_compile_resources_release(context.resources);
    CHECK(!source_program_compile_live && !source_program_compile_bytes);return status;
}
int main(void) {
    const XrCompileResourceLimits caps={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,128000000};
    XrCompileResourceStats stats={0};size_t sites=0;
    for(size_t pass=0;pass<=sites;++pass) {
        source_program_compile_attempts=0;source_program_compile_injected=false;
        source_program_compile_fail_at=pass ? pass-1 : SIZE_MAX;
        XrXirStatus status=pipeline(caps,NULL);size_t attempts=source_program_compile_attempts;
        source_program_compile_fail_at=SIZE_MAX;
        if(!pass){CHECK(status==XR_XIR_OK);sites=attempts;CHECK(sites);}
        else {
            if(status!=XR_XIR_OUT_OF_MEMORY)fprintf(stderr,"compiler fault pass=%zu status=%u attempts=%zu injected=%u\n",pass,status,attempts,source_program_compile_injected);
            CHECK(source_program_compile_injected && status==XR_XIR_OUT_OF_MEMORY && attempts>=pass);
        }
    }
    CHECK(pipeline(caps,&stats)==XR_XIR_OK);
    XrCompileResourceLimits exact={stats.allocated_bytes,stats.peak_bytes,stats.work};XrCompileResourceStats got={0};
    CHECK(pipeline(exact,&got)==XR_XIR_OK);
    CHECK(got.allocated_bytes==stats.allocated_bytes && got.peak_bytes==stats.peak_bytes && got.work==stats.work);
    XrCompileResourceLimits less=exact;--less.allocated_bytes;CHECK(pipeline(less,NULL)==XR_XIR_BUDGET);
    less=exact;--less.live_bytes;CHECK(pipeline(less,NULL)==XR_XIR_BUDGET);
    less=exact;--less.work;CHECK(pipeline(less,NULL)==XR_XIR_BUDGET);
    printf("Tuple local whole Source/packet/copy-query/emit/Program compiler OOM=%zu; exact allocated=%llu live=%llu work=%llu; three -1 failclosed; every owner physical0 PASS\n",
        sites,(unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work);
    source_program_owners_free();return 0;
}
