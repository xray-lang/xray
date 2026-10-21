/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * literal_receiving_resources.h - Allocation failure preserves the receiving owner
 *
 * KEY CONCEPT:
 *   Reuse an immutable evaluated batch to enumerate receiving allocations alone.
 *   Failed receiving leaves both the batch and the original ledger intact.
 */
#include "base/xresource_limits.h"
static void literal_receiving_resources(void) {
    XrXirCompileContext context={0};
    const XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    XrCompileResources *same=context.resources;
    XirCtfeInstanceBatch *batch=literal_batch(&context,"text");
    XirLiteralRequest request={&context,batch,2};XirLiteralDefinitions result={0};
    size_t first=source_fixture_compile_attempts;
    CHECK(xr_xir_compile_instance_literals(&request,&result,NULL)==XR_XIR_OK);
    size_t sites=source_fixture_compile_attempts-first;CHECK(sites && result.count==2);
    xr_xir_compile_instance_literals_free(&result);
    XrCompileResourceStats before=stats(&context);
    size_t live=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
    for (size_t fault=0;fault<sites;++fault) {
        first=source_fixture_compile_attempts;source_fixture_compile_injected=false;
        source_fixture_compile_fail_at=first+fault;
        XrXirStatus status=xr_xir_compile_instance_literals(&request,&result,NULL);
        if (status!=XR_XIR_OUT_OF_MEMORY)
            fprintf(stderr,"Literal receiving FI point=%zu/%zu status=%u\n",fault+1,sites,(unsigned)status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected && literal_empty(&result));
        CHECK(source_fixture_compile_attempts==first+fault+1);
        CHECK(context.resources==same && stats(&context).live_bytes==before.live_bytes);
        CHECK(source_fixture_compile_live==live && source_fixture_compile_bytes==bytes);
        source_fixture_compile_fail_at=SIZE_MAX;
        CHECK(xr_xir_compile_instance_literals(&request,&result,NULL)==XR_XIR_OK && result.count==2);
        CHECK(result.definition->context.resources==same);
        xr_xir_compile_instance_literals_free(&result);
        CHECK(stats(&context).live_bytes==before.live_bytes);
        CHECK(source_fixture_compile_live==live && source_fixture_compile_bytes==bytes);
        if (!(fault%256) || fault+1==sites)
            fprintf(stderr,"Literal receiving FI actual=%zu/%zu same-owner/retry/live\n",fault+1,sites);
    }
    XirCtfeInstanceView views[2]={0};
    CHECK(xr_xir_compile_ctfe_instance_batch_views(batch,views,2)==XR_XIR_CTFE_OK);
    literal_value("text",views[0].value);literal_value("text",views[1].value);
    xr_xir_compile_ctfe_instance_batch_free(batch);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Literal receiving COMPLETE_ALL_ORDINALS sites=%zu physical0\n",sites);
}
