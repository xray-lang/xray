/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_retired_merge_packets.c - Reject complete retired inputs from both parents
 */
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"retired packet %d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "retired_merge_packets.h"

int main(void) {
    XrCompileResourceLimits limits={67108864,8388608,128000000};
    XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    size_t count=sizeof(retired_merge_packets)/sizeof(retired_merge_packets[0]);
    for (size_t i=0;i<count;++i) for (unsigned occupied=0;occupied<2;++occupied) {
        XrCompileResourceStats before={0},after={0};
        CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);
        XrXirArtifact *sentinel=occupied?(XrXirArtifact *)(uintptr_t)1:NULL;
        XrXirArtifact *output=sentinel;XrXirDiagnostic diagnostic={0};
        CHECK(xr_xir_compile_checked_read(&context,retired_merge_packets[i].bytes,
            retired_merge_packets[i].length,&output,&diagnostic)==XR_XIR_BAD_STRUCTURE);
        CHECK(output==sentinel && diagnostic.status==XR_XIR_BAD_STRUCTURE);
        CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(before.allocation_count==after.allocation_count && before.allocated_bytes==after.allocated_bytes);
        CHECK(before.live_bytes==after.live_bytes);
    }
    xr_compile_resources_release(resources);
    printf("Complete retired merge packets: %zu vectors, %zu preserved-output rejections PASS\n",count,count*2);
    return 0;
}
