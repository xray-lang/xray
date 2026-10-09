#include "xir_construction_fixture.h"
/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_error_cell_index_resources.h - Complete finite ledger and failure ownership
 */
#ifndef XIR_ERROR_CELL_INDEX_RESOURCES_H
#define XIR_ERROR_CELL_INDEX_RESOURCES_H
static void ci_faults(void){
    size_t sites=0;
    for(size_t pass=0;pass<=sites;++pass){
        XrXirCompileContext c=ci_owner(ci_caps());uint64_t baseline=ci_stats(&c).live_bytes;
        attempts=0;injected=false;fail_at=pass?pass-1:SIZE_MAX;
        XrXirStatus status=ci_whole(&c,CI_CALL,false,false);size_t actual=attempts;fail_at=SIZE_MAX;
        if(!pass){CHECK(status==XR_XIR_OK);sites=actual;CHECK(sites>0 && sites<2048);}
        else CHECK(injected && actual>=pass && status==XR_XIR_OUT_OF_MEMORY);
        ci_release(&c,baseline);
    }
    printf("Cell index complete checker+analysis OOM sites=%zu physical0\n",sites);
}
static XrCompileResourceStats ci_measured(XrCompileResourceLimits caps,XrXirStatus expected){
    XrXirCompileContext c=ci_owner(caps);uint64_t baseline=ci_stats(&c).live_bytes;
    CHECK(ci_whole(&c,CI_CALL,false,false)==expected);XrCompileResourceStats s=ci_stats(&c);ci_release(&c,baseline);return s;
}
static void ci_limits(void){
    XrCompileResourceStats cost=ci_measured(ci_caps(),XR_XIR_OK);
    XrCompileResourceLimits exact={cost.allocated_bytes,cost.peak_bytes,cost.work};
    XrCompileResourceStats second=ci_measured(exact,XR_XIR_OK);
    CHECK(cost.allocated_bytes==second.allocated_bytes && cost.peak_bytes==second.peak_bytes && cost.work==second.work);
    CHECK(exact.allocated_bytes>1 && exact.live_bytes>1 && exact.work>1);
    XrCompileResourceLimits less=exact;--less.allocated_bytes;(void)ci_measured(less,XR_XIR_BUDGET);
    less=exact;--less.live_bytes;(void)ci_measured(less,XR_XIR_BUDGET);
    less=exact;--less.work;(void)ci_measured(less,XR_XIR_BUDGET);
    printf("Cell index whole operation allocated/live/work exact-minus1=%llu/%llu/%llu physical0\n",
        (unsigned long long)cost.allocated_bytes,(unsigned long long)cost.peak_bytes,(unsigned long long)cost.work);
}
static void ci_occupied(void){
    CellIndexFixture f;ci_fixture(&f,CI_CALL,false,false);XrXirCompileContext c=ci_owner(ci_caps());uint64_t baseline=ci_stats(&c).live_bytes;
    CHECK(xir_fixture_verify(&c, &f.module, NULL)==XR_XIR_OK);
    XrXirEffects sentinel={0},*occupied=&sentinel;memset(&sentinel,0x5a,sizeof(sentinel));
    XrXirEffects before=sentinel;XrCompileResourceStats a=ci_stats(&c);
    CHECK(xr_xir_compile_effects_infer_verified(&c,&f.module,&occupied)==XR_XIR_BAD_STRUCTURE);
    XrCompileResourceStats b=ci_stats(&c);
    CHECK(occupied==&sentinel && !memcmp(&sentinel,&before,sizeof(before)) && a.work==b.work && a.allocated_bytes==b.allocated_bytes);
    ci_release(&c,baseline);
}
#endif // XIR_ERROR_CELL_INDEX_RESOURCES_H
