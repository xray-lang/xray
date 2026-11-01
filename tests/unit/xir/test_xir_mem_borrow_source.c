/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mem_borrow_source.c - Complete memory source and owned return facts
 *
 * KEY CONCEPT:
 *   A cold reader retains genuine receiver relationships after the Source
 *   producer retires. Removing their facts cannot turn a wrapper into a root.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_construction.h"
#include "xir/xxir_compile_memory.h"
#include "xir/xxir_internal.h"
#include "base/xresource_limits.h"
#include "xir/xxir_types.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"mem Source %d: %s\n",__LINE__,#c);exit(1);}} while (0)
static uint64_t held(XrCompileResources *resources) {
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(resources,&stats)==XR_COMPILE_RESOURCE_OK);
    return stats.held_bytes;
}
int main(int argc,char **argv) {
    CHECK(argc==2);char stdlib[2048];
    CHECK(snprintf(stdlib,sizeof(stdlib),"%s/stdlib",argv[1])>0);
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};uint64_t frame=held(resources);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"mem-borrow-source",NULL};
    XrXirSourceRequest request={session,NULL,&authority,&context,stdlib,NULL,XR_XIR_PROGRAM,NULL};
    static const char input[]="import mem\nprint(1)\n";
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,
        &(XrXirSourceText){NULL,input,sizeof(input)-1},&source,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"Source %u module%u %d:%d %s\n",status,
        diagnostic.module,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked);
    const XrXirConstruction *construction=xr_xir_compile_artifact_construction(source.checked);
    const XrXirBorrowReturns *returns=xr_xir_compile_construction_borrow_returns(construction);
    CHECK(returns && returns->count==2 && returns->rows);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);xr_compile_session_free(session);
    XrXirArtifact *cold=NULL;CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&cold,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    construction=xr_xir_compile_artifact_construction(cold);
    returns=xr_xir_compile_construction_borrow_returns(construction);CHECK(returns && returns->count==2);
    const XrXirModule *module=xr_xir_compile_artifact_module(cold);
    uint32_t count=xr_xir_compile_construction_count(construction);
    XrXirStatus allocation=XR_XIR_OK;
    XrXirConstructionRow *rows=count?xir_compile_calloc(&context,count,sizeof(*rows),&allocation):NULL;
    CHECK(allocation==XR_XIR_OK && (!count || rows));
    for (uint32_t n=0;n<count;++n) rows[n]=*xr_xir_compile_construction_row(construction,n);
    uint32_t bindings=xr_xir_compile_construction_materialization_count(construction);
    XrXirMaterializationBinding *materializations=bindings?xir_compile_calloc(&context,bindings,sizeof(*materializations),&allocation):NULL;
    CHECK(allocation==XR_XIR_OK && (!bindings || materializations));
    for (uint32_t n=0;n<bindings;++n) materializations[n]=*xr_xir_compile_construction_materialization(construction,n);
    XrXirConstructionSpec specification={rows,count,materializations,bindings,
        xr_xir_compile_construction_comptime(construction),NULL};
    XrXirConstruction *missing=NULL;
    CHECK(xr_xir_compile_construction_new_v5(&context,module->types,&specification,
        xr_xir_compile_construction_slots(construction),&missing,NULL)==XR_XIR_OK);
    XrXirArtifact *rejected=NULL;
    CHECK(xr_xir_compile_recheck_v2(&context,module,missing,&rejected,NULL,NULL)!=XR_XIR_OK && !rejected);
    xr_xir_compile_construction_free(missing);xr_compile_resources_free(rows);xr_compile_resources_free(materializations);
    xr_xir_compile_artifact_free(cold);CHECK(held(resources)==frame);xr_compile_resources_release(resources);
    puts("mem source return facts PASS");return 0;
}
