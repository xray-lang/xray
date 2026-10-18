/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generator_wait_resource_gates.h - Allocation histories and exact finite resource cuts
 */
#ifndef XIR_GENERATOR_WAIT_RESOURCE_GATES_H
#define XIR_GENERATOR_WAIT_RESOURCE_GATES_H
static SourceNativeAllocationEvent resource_normal[32768],resource_cancel_trace[32768];
static WaitResourceRequest resource_request(unsigned mode) {
    WaitResourceRequest request={.mode=mode,.cancel_pass=UINT32_MAX,.fault=SIZE_MAX,.cancel_prefix=UINT32_MAX};return request;
}
static bool resource_same_event(const SourceNativeAllocationEvent *a,const SourceNativeAllocationEvent *b) {
    return a->ordinal==b->ordinal && a->bytes==b->bytes && a->line==b->line && a->kind==b->kind &&
        a->phase==b->phase && a->instance==b->instance && a->poll==b->poll && a->role==b->role &&
        a->entry==b->entry && !strcmp(a->file,b->file);
}
static void resource_print_events(unsigned mode,unsigned pass,uint32_t prefix) {
    for(size_t i=0;i<source_native_event_count;++i) {
        const SourceNativeAllocationEvent *e=&source_native_events[i];
        const char *file=strrchr(e->file,'/'),*back=strrchr(e->file,'\\');
        if(back && (!file || back>file)) file=back;file=file ? file+1 : e->file;
        printf("GENERATOR_ALLOC mode=%u cancel_instance=%u prefix=%u ordinal=%zu bytes=%zu kind=%u file=%s line=%u phase=%u instance=%u poll=%u role=%u entry=%u hit=%u\n",
            mode,pass,prefix,e->ordinal,e->bytes,e->kind,file,e->line,e->phase,e->instance,e->poll,e->role,e->entry,(unsigned)e->injected);
    }
}
static void resource_print_base(unsigned mode,const WaitResourceCost *cost) {
    printf("GENERATOR_RESOURCE mode=%u sites=%zu polls=%u/%u physical=0/0\n",mode,cost->sites,cost->polls[0],cost->polls[1]);
    for(unsigned pass=0;pass<2;++pass)
        printf("GENERATOR_INSTANCE pass=%u requested_value=%llu requested_call=%llu work=%llu metadata_peak=%llu value_peak=%llu call_peak=%llu\n",pass,
            (unsigned long long)cost->budget[pass].requested_bytes,(unsigned long long)cost->budget[pass].requested_call_bytes,
            (unsigned long long)cost->budget[pass].work,(unsigned long long)cost->budget[pass].metadata_peak,
            (unsigned long long)cost->values[pass].peak_bytes,(unsigned long long)cost->budget[pass].call_peak);
}
static void resource_faults(WaitResourceFixture *fixture,unsigned mode,const WaitResourceCost *base) {
    for(size_t ordinal=0;ordinal<base->sites;++ordinal) {
        WaitResourceRequest request=resource_request(mode);request.fault=ordinal;
        WaitResourceCost cost=resource_operation(fixture,&request);
        printf("GENERATOR_FAILURE_CENSUS mode=%u ordinal=%zu attempts=%zu failure=%u\n",mode,ordinal,cost.sites,cost.failure);
        CHECK(cost.hit && cost.failure==XR_XIR_CALL_OOM);
        /* A failed history must not leave an untested allocating suffix. */
        CHECK(cost.sites==ordinal+1);
        for(size_t i=0;i<=ordinal;++i) CHECK(resource_same_event(&source_native_events[i],&resource_normal[i]));
        resource_print_events(mode,UINT32_MAX,UINT32_MAX);
    }
}
static void resource_axes(WaitResourceFixture *fixture,unsigned mode,const WaitResourceCost *base) {
    for(unsigned axis=0;axis<6;++axis) for(unsigned minus=0;minus<2;++minus) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        uint64_t exact=axis==0 ? base->budget[0].requested_call_bytes : axis==1 ? base->budget[0].metadata_peak :
            axis==2 ? base->budget[0].work : axis==3 ? base->budget[0].requested_bytes :
            axis==4 ? base->budget[0].call_peak : base->values[0].peak_bytes;
        CHECK(exact>1);
        if(!axis) config.requested_call_limit=exact-minus;
        else if(axis==1) config.metadata_limit=exact-minus;
        else if(axis==2) config.work_limit=exact-minus;
        else if(axis==3) config.requested_value_limit=exact-minus;
        else if(axis==4) config.call_limit=exact-minus;
        else config.value_limit=exact-minus;
        WaitResourceRequest request=resource_request(mode);request.limits=&config;
        WaitResourceCost cost=resource_operation(fixture,&request);
        printf("GENERATOR_AXIS mode=%u axis=%u minus=%u cut=%llu failure=%u physical=0/0\n",mode,axis,minus,(unsigned long long)(exact-minus),cost.failure);
        CHECK(cost.failure==(minus ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_READY));
    }
}
static void resource_cancel_one(WaitResourceFixture *fixture,unsigned mode,unsigned pass,uint32_t prefix,const WaitResourceCost *base) {
    WaitResourceRequest request=resource_request(mode);request.cancel_pass=pass;request.cancel_prefix=prefix;
    WaitResourceCost cost=resource_operation(fixture,&request);
    CHECK(cost.requested!=cost.rejected && !cost.hit && cost.request_before==cost.request_after);
    CHECK(cost.failure==(cost.requested ? XR_XIR_CALL_CANCELLED : XR_XIR_CALL_READY));
    CHECK(cost.request_before<=base->sites);
    for(size_t i=0;i<cost.request_before;++i) CHECK(resource_same_event(&source_native_events[i],&resource_normal[i]));
    memcpy(resource_cancel_trace,source_native_events,cost.sites*sizeof(*resource_cancel_trace));
    printf("GENERATOR_CANCEL mode=%u pass=%u prefix=%u requested=%u rejected=%u before=%zu after=%zu attempts=%zu failure=%u physical=0/0\n",
        mode,pass,prefix,(unsigned)cost.requested,(unsigned)cost.rejected,cost.request_before,cost.request_after,cost.sites,cost.failure);
    resource_print_events(mode,pass,prefix);
    if(!cost.requested) return;
    for(size_t ordinal=cost.request_after;ordinal<cost.sites;++ordinal) {
        request.fault=ordinal;WaitResourceCost failed=resource_operation(fixture,&request);
        printf("GENERATOR_CANCEL_FAULT mode=%u pass=%u prefix=%u ordinal=%zu attempts=%zu failure=%u\n",mode,pass,prefix,ordinal,failed.sites,failed.failure);
        CHECK(failed.requested && failed.hit && failed.failure==XR_XIR_CALL_OOM && failed.sites==ordinal+1);
        for(size_t i=0;i<=ordinal;++i) CHECK(resource_same_event(&source_native_events[i],&resource_cancel_trace[i]));
        resource_print_events(mode,pass,prefix);
    }
}
static void resource_cancel(WaitResourceFixture *fixture,unsigned mode,const WaitResourceCost *base) {
    for(unsigned pass=0;pass<2;++pass) for(uint32_t prefix=0;prefix<base->polls[pass];++prefix)
        resource_cancel_one(fixture,mode,pass,prefix,base);
}
#endif // XIR_GENERATOR_WAIT_RESOURCE_GATES_H
