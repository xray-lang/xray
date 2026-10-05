/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_semantic65_vectors.c - Independent current roles and complete prior packets
 */
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_source_program_compile_owner.h"
#include "semantic65_packet_cases.h"
_Static_assert(XR_XIR_CHECKED_SCHEMA==25 && XR_XIR_CHECKED_CONTRACT==65 && XR_XIR_OP_COUNT==146,"single current identity");
static void packet_pair(const Semantic65Vector *v) {
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    const XrCompileResourceLimits caps={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline={0},after={0};
    CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *artifact=NULL;XrXirCheckedPacket packet={0};
    uint64_t initial_live=baseline.live_bytes;
    if(v->reader_positive) {
        XrXirDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_checked_read(&context,v->current,v->current_size,&artifact,&diagnostic);
        if(status!=XR_XIR_OK)fprintf(stderr,"vector bytes=%zu status=%u function=%u block=%u instruction=%u reason=%u\n",v->current_size,status,diagnostic.function,diagnostic.block,diagnostic.instruction,diagnostic.reason);
        CHECK(status==XR_XIR_OK && artifact);
        CHECK(xr_xir_compile_checked_write(artifact,&packet,NULL)==XR_XIR_OK);
        CHECK(packet.length==v->current_size && !memcmp(packet.bytes,v->current,v->current_size));
        xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(artifact);artifact=NULL;
    }
    CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    CHECK(baseline.live_bytes==initial_live);
    size_t attempts=source_program_compile_attempts;
    CHECK(xr_xir_compile_checked_read(&context,v->previous,v->previous_size,&artifact,NULL)==XR_XIR_BAD_STRUCTURE && !artifact);
    artifact=(XrXirArtifact *)(uintptr_t)1;
    CHECK(xr_xir_compile_checked_read(&context,v->previous,v->previous_size,&artifact,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(artifact==(XrXirArtifact *)(uintptr_t)1 && attempts==source_program_compile_attempts);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.allocated_bytes==baseline.allocated_bytes && after.allocation_count==baseline.allocation_count &&
        after.live_bytes==baseline.live_bytes && after.peak_bytes==baseline.peak_bytes);
    xr_compile_resources_release(context.resources);
    CHECK(!source_program_compile_live && !source_program_compile_bytes);
}
int main(void) {
    for(size_t i=0;i<sizeof(semantic65_vectors)/sizeof(semantic65_vectors[0]);++i){fprintf(stderr,"vector index=%zu\n",i);packet_pair(&semantic65_vectors[i]);}
    puts("21 independently role-framed current65 reader positives; 2 writer-only known bytes retained for original sizing gate; 23 complete64 early/occupied zero-allocation rejects, finite owner physical0 PASS");
    return 0;
}
