/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * compile_resources_extent_cases.h - Live extent rejection without pointer reads
 */
static void allocation_extent_cases(void) {
    reset_observer();
    XrCompileResources *owner=NULL,*foreign=NULL;
    OK(xr_compile_resources_new(&unlimited,&owner));
    OK(xr_compile_resources_new(&unlimited,&foreign));
    void *first=NULL,*second=NULL,*other=NULL;
    OK(xr_compile_resources_alloc(owner,16,&first));
    OK(xr_compile_resources_alloc(owner,8,&second));
    OK(xr_compile_resources_alloc(foreign,32,&other));
    size_t before_attempts=attempts;
    OK(xr_compile_resources_extent(owner,first,16));
    OK(xr_compile_resources_extent(owner,(uint8_t *)first+7,9));
    CHECK(xr_compile_resources_extent(owner,(uint8_t *)first+7,10)==XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_extent(owner,first,17)==XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_extent(owner,(uint8_t *)first+16,1)==XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_extent(owner,(void *)(uintptr_t)1,1)==XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_extent(owner,(void *)UINTPTR_MAX,SIZE_MAX)==XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_extent(owner,other,1)==XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_extent(foreign,first,1)==XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_extent(NULL,first,1)==XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_extent(owner,NULL,1)==XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_extent(owner,first,0)==XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(attempts==before_attempts);
    /* Plausible preceding bytes cannot introduce an unregistered payload. */
    CompileAllocation *forged=xr_malloc(sizeof(*forged)+16);
    CHECK(forged!=NULL);
    forged->info.resources=owner;forged->info.bytes=16;
    forged->info.previous=NULL;forged->info.next=NULL;
    CHECK(xr_compile_resources_extent(owner,forged+1,16)==XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    xr_free(forged);
    uintptr_t retired=(uintptr_t)second;
    xr_compile_resources_free(second);second=NULL;
    CHECK(xr_compile_resources_extent(owner,(void *)retired,1)==XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    OK(xr_compile_resources_extent(owner,first,16));
    /* Replacement exists before retirement, so the old address cannot be reused. */
    retired=(uintptr_t)first;
    memset(first,0x5a,16);
    OK(xr_compile_resources_resize(owner,&first,32));
    CHECK(xr_compile_resources_extent(owner,(void *)retired,1)==XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    OK(xr_compile_resources_extent(owner,first,32));
    CHECK(((uint8_t *)first)[15]==0x5a);
    fail_at=attempts;
    void *saved=first;
    CHECK(xr_compile_resources_resize(owner,&first,64)==XR_COMPILE_RESOURCE_OUT_OF_MEMORY && first==saved);
    fail_at=SIZE_MAX;
    OK(xr_compile_resources_extent(owner,first,32));
    OK(xr_compile_resources_resize(owner,&first,5));
    OK(xr_compile_resources_extent(owner,first,5));
    CHECK(xr_compile_resources_extent(owner,first,6)==XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    retired=(uintptr_t)first;
    OK(xr_compile_resources_resize(owner,&first,0));
    CHECK(first==NULL && xr_compile_resources_extent(owner,(void *)retired,1)==XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    xr_compile_resources_free(other);xr_compile_resources_release(foreign);
    /* The global observer includes the foreign ledger and forged raw memory.
     * Check this owner's lifetime totals against its actual allocation history. */
    XrCompileResourceStats final;
    OK(xr_compile_resources_stats(owner,&final));
    CHECK(final.live_bytes==sizeof(*owner));
    CHECK(final.peak_bytes==sizeof(*owner)+2*sizeof(CompileAllocation)+48);
    CHECK(final.allocated_bytes==sizeof(*owner)+4*sizeof(CompileAllocation)+61);
    CHECK(final.allocation_count==5 && physical_bytes==sizeof(*owner));
    xr_compile_resources_release(owner);
    CHECK(!live_count && !physical_bytes);

    for (unsigned minus=0;minus<2;++minus) {
        reset_observer();owner=NULL;first=NULL;
        XrCompileResourceLimits limits={UINT64_MAX,UINT64_MAX,9-minus};
        OK(xr_compile_resources_new(&limits,&owner));
        OK(xr_compile_resources_alloc(owner,8,&first));
        CHECK(snapshot(owner).work==2);
        XrCompileResourceStatus status=xr_compile_resources_extent(owner,first,8);
        CHECK(status==(minus?XR_COMPILE_RESOURCE_BUDGET:XR_COMPILE_RESOURCE_OK));
        CHECK(snapshot(owner).work==(minus?3u:9u));
        /* Rejected work remains consumed, and failed queries retain the payload. */
        CHECK(xr_compile_resources_extent(owner,first,8)==XR_COMPILE_RESOURCE_BUDGET);
        memset(first,0x6b,8);
        xr_compile_resources_release(owner);
        xr_compile_resources_free(first);
        CHECK(!live_count && !physical_bytes);
    }
    puts("live allocation extents: actual ranges, wild/forged/foreign/freed rejection, resize/OOM, exact/minus1 and physical0 passed");
}
