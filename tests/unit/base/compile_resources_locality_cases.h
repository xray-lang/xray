/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * compile_resources_locality_cases.h - Actual range locality and paid relocation
 */
static void allocation_locality_cases(void) {
    for (uint32_t round=0;round<96;++round) {
        reset_observer();XrCompileResources *owner=NULL;void *payloads[27]={0};
        OK(xr_compile_resources_new(&unlimited,&owner));
        for (uint32_t n=0;n<27;++n) {
            OK(xr_compile_resources_alloc(owner,n+1,&payloads[n]));memset(payloads[n],(int)n,n+1);
        }
        XrCompileResourceStats before=snapshot(owner);size_t calls=attempts;
        OK(xr_compile_resources_extent(owner,payloads[0],1));
        CHECK(snapshot(owner).work-before.work==1+6*27+6);
        before=snapshot(owner);OK(xr_compile_resources_extent(owner,payloads[0],1));
        CHECK(snapshot(owner).work-before.work==7 && attempts==calls);
        for (uint32_t turn=0;turn<32;++turn) {
            uint32_t slot=turn%2?0:26;before=snapshot(owner);
            OK(xr_compile_resources_extent(owner,payloads[slot],slot+1));
            CHECK(snapshot(owner).work-before.work==19 && attempts==calls);
        }
        for (uint32_t n=0;n<27;++n) {
            uint32_t slot=(n*17+round)%27;
            OK(xr_compile_resources_extent(owner,(uint8_t *)payloads[slot]+slot,1));
            CHECK(((uint8_t *)payloads[slot])[slot]==slot);
            CompileAllocation *head=owner->allocations;
            CHECK(xr_compile_resources_extent(owner,payloads[slot],slot+2)==XR_COMPILE_RESOURCE_BAD_ARGUMENT);
            CHECK(owner->allocations==head);
            uintptr_t retired=(uintptr_t)payloads[slot];xr_compile_resources_free(payloads[slot]);
            CHECK(xr_compile_resources_extent(owner,(void *)retired,1)==XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        }
        CHECK(!owner->allocations && snapshot(owner).live_bytes==sizeof(*owner));
        xr_compile_resources_release(owner);CHECK(!live_count && !physical_bytes && fail_at==SIZE_MAX);
    }
    for (uint32_t minus=0;minus<2;++minus) {
        reset_observer();XrCompileResources *owner=NULL;void *first=NULL,*second=NULL;
        XrCompileResourceLimits limits={UINT64_MAX,UINT64_MAX,22-minus};
        OK(xr_compile_resources_new(&limits,&owner));OK(xr_compile_resources_alloc(owner,8,&first));
        OK(xr_compile_resources_alloc(owner,8,&second));CompileAllocation *head=owner->allocations;
        XrCompileResourceStatus status=xr_compile_resources_extent(owner,first,8);
        CHECK(status==(minus?XR_COMPILE_RESOURCE_BUDGET:XR_COMPILE_RESOURCE_OK));
        CHECK(snapshot(owner).work==(minus?16u:22u));
        CHECK(minus?owner->allocations==head:owner->allocations!=head);
        xr_compile_resources_free(first);xr_compile_resources_free(second);xr_compile_resources_release(owner);
        CHECK(!live_count && !physical_bytes);
    }
    puts("actual extent locality: 96 orders, paid first hit/repeat/alternation, failure preserves index, relocation exact/minus1 and physical0 passed");
}
