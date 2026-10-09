#include "xir_construction_fixture.h"
/* Complete admission and inference retain genuine finite resource failures. */
#ifndef XIR_ERROR_CFG_PENDING_RESOURCES_H
#define XIR_ERROR_CFG_PENDING_RESOURCES_H
static void pc_faults(PendingCase mode) {
    size_t sites=0;
    for (size_t pass=0;pass<=sites;++pass) {
        XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
        attempts=0;injected=false;fail_at=pass ? pass-1 : SIZE_MAX;
        XrXirStatus status=pc_whole(&c,mode,false);size_t actual=attempts;fail_at=SIZE_MAX;
        if (!pass) { CHECK(status==XR_XIR_OK);sites=actual;CHECK(sites>0 && sites<4096); }
        else CHECK(injected && actual>=pass && status==XR_XIR_OUT_OF_MEMORY);
        CHECK(pc_stats(&c).live_bytes==baseline);
        CHECK(pc_whole(&c,mode,false)==XR_XIR_OK);
        pc_release(&c,baseline);
    }
    printf("pending case%u complete check/analyze OOM sites%zu same-owner retry physical0\n",(unsigned)mode,sites);
}
static XrCompileResourceStats pc_measured(XrCompileResourceLimits caps,
    XrXirStatus expected,PendingCase mode) {
    XrXirCompileContext c=pc_owner(caps);uint64_t baseline=pc_stats(&c).live_bytes;
    CHECK(pc_whole(&c,mode,false)==expected);XrCompileResourceStats s=pc_stats(&c);
    pc_release(&c,baseline);return s;
}
static void pc_limits(PendingCase mode) {
    XrCompileResourceStats a=pc_measured(pc_caps(),XR_XIR_OK,mode);
    XrCompileResourceLimits exact={a.allocated_bytes,a.peak_bytes,a.work};
    XrCompileResourceStats b=pc_measured(exact,XR_XIR_OK,mode);
    CHECK(a.allocated_bytes==b.allocated_bytes && a.peak_bytes==b.peak_bytes && a.work==b.work);
    CHECK(exact.allocated_bytes>1 && exact.live_bytes>1 && exact.work>1);
    XrCompileResourceLimits less=exact;--less.allocated_bytes;(void)pc_measured(less,XR_XIR_BUDGET,mode);
    less=exact;--less.live_bytes;(void)pc_measured(less,XR_XIR_BUDGET,mode);
    less=exact;--less.work;(void)pc_measured(less,XR_XIR_BUDGET,mode);
}
static void pc_occupied(void) {
    EdgeFixture f;pc_fixture(&f,PC_PHI_SELF,false);
    XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
    CHECK(xir_fixture_verify(&c, &f.module, NULL)==XR_XIR_OK);
    XrXirEffects sentinel={0},*occupied=&sentinel;memset(&sentinel,0x5a,sizeof(sentinel));
    XrXirEffects copy=sentinel;XrCompileResourceStats before=pc_stats(&c);
    CHECK(xr_xir_compile_effects_infer_verified(&c,&f.module,&occupied)==XR_XIR_BAD_STRUCTURE);
    XrCompileResourceStats after=pc_stats(&c);
    CHECK(occupied==&sentinel && !memcmp(&copy,&sentinel,sizeof(copy)));
    CHECK(before.work==after.work && before.allocated_bytes==after.allocated_bytes && before.live_bytes==after.live_bytes);
    pc_release(&c,baseline);
}
#endif
