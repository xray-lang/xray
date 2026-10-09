#include "xir_construction_fixture.h"
/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_error_edge_snapshot_resources.h - Complete finite operation ownership
 */
#ifndef XIR_ERROR_EDGE_SNAPSHOT_RESOURCES_H
#define XIR_ERROR_EDGE_SNAPSHOT_RESOURCES_H
static void ee_fault_case(EdgeCase mode) {
    size_t sites=0;
    for (size_t pass=0;pass<=sites;++pass) {
        XrXirCompileContext c=ee_owner(ee_caps());uint64_t baseline=ee_stats(&c).live_bytes;
        attempts=0;injected=false;fail_at=pass?pass-1:SIZE_MAX;
        XrXirStatus status=ee_whole(&c,mode,false);size_t actual=attempts;fail_at=SIZE_MAX;
        if (!pass) { CHECK(status==XR_XIR_OK);sites=actual;CHECK(sites>0 && sites<2048); }
        else { CHECK(injected && actual>=pass && status==XR_XIR_OUT_OF_MEMORY); }
        CHECK(ee_stats(&c).live_bytes==baseline);
        CHECK(ee_whole(&c,mode,false)==XR_XIR_OK);
        ee_release(&c,baseline);
    }
    printf("error edge complete checker+inference case=%u OOM sites=%zu same-owner retry physical0\n",(unsigned)mode,sites);
}
static XrCompileResourceStats ee_measured(XrCompileResourceLimits caps, XrXirStatus expected) {
    XrXirCompileContext c=ee_owner(caps);uint64_t baseline=ee_stats(&c).live_bytes;
    CHECK(ee_whole(&c,EE_PHI,false)==expected);
    XrCompileResourceStats s=ee_stats(&c);ee_release(&c,baseline);return s;
}
static void ee_limits(void) {
    XrCompileResourceStats cost=ee_measured(ee_caps(),XR_XIR_OK);
    XrCompileResourceLimits exact={cost.allocated_bytes,cost.peak_bytes,cost.work};
    XrCompileResourceStats second=ee_measured(exact,XR_XIR_OK);
    CHECK(cost.allocated_bytes==second.allocated_bytes && cost.peak_bytes==second.peak_bytes && cost.work==second.work);
    CHECK(exact.allocated_bytes>1 && exact.live_bytes>1 && exact.work>1);
    XrCompileResourceLimits less=exact;--less.allocated_bytes;(void)ee_measured(less,XR_XIR_BUDGET);
    less=exact;--less.live_bytes;(void)ee_measured(less,XR_XIR_BUDGET);
    less=exact;--less.work;(void)ee_measured(less,XR_XIR_BUDGET);
    printf("error edge whole operation allocated/live/work exact-minus1=%llu/%llu/%llu physical0\n",
        (unsigned long long)cost.allocated_bytes,(unsigned long long)cost.peak_bytes,(unsigned long long)cost.work);
}
static void ee_occupied(void) {
    EdgeFixture f;ee_fixture(&f,EE_PHI,false);XrXirCompileContext c=ee_owner(ee_caps());uint64_t baseline=ee_stats(&c).live_bytes;
    CHECK(xir_fixture_verify(&c, &f.module, NULL)==XR_XIR_OK);
    XrXirEffects sentinel={0},*occupied=&sentinel;memset(&sentinel,0x5a,sizeof(sentinel));
    XrXirEffects before=sentinel;XrCompileResourceStats a=ee_stats(&c);
    CHECK(xr_xir_compile_effects_infer_verified(&c,&f.module,&occupied)==XR_XIR_BAD_STRUCTURE);
    XrCompileResourceStats b=ee_stats(&c);
    CHECK(occupied==&sentinel && !memcmp(&sentinel,&before,sizeof(before)));
    CHECK(a.work==b.work && a.allocated_bytes==b.allocated_bytes && a.live_bytes==b.live_bytes);
    ee_release(&c,baseline);
}
#endif // XIR_ERROR_EDGE_SNAPSHOT_RESOURCES_H
