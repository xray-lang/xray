/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_error_dependency_resources.h - Failure atomicity on real compiler owners
 */
#ifndef XIR_ERROR_DEPENDENCY_RESOURCES_H
#define XIR_ERROR_DEPENDENCY_RESOURCES_H
static void ed_faults(void){
    for(unsigned operation=0;operation<2;++operation){
    size_t sites=0;
    for(size_t pass=0;pass<=sites;++pass){
        XrXirCompileContext c=ed_owner(ed_caps());uint64_t baseline=ed_stats(&c).live_bytes;
        attempts=0;injected=false;fail_at=pass?pass-1:SIZE_MAX;
        XrXirStatus status=operation?ed_restart_whole(&c):ed_whole(&c,ED_GO,false);size_t actual=attempts;fail_at=SIZE_MAX;
        if(!pass){CHECK(status==XR_XIR_OK);sites=actual;CHECK(sites>0 && sites<2048);}
        else {CHECK(injected && actual>=pass && status==XR_XIR_OUT_OF_MEMORY);CHECK(ed_stats(&c).live_bytes==baseline);
            CHECK((operation?ed_restart_whole(&c):ed_whole(&c,ED_GO,false))==XR_XIR_OK);}
        ed_release(&c,baseline);
    }
    printf("dependency operation%u complete verifier/inference actual OOM sites%zu; same-owner retries physical0\n",operation,sites);
    }
}
static XrCompileResourceStats ed_measured(XrCompileResourceLimits limits,XrXirStatus expected,unsigned operation){
    XrXirCompileContext c=ed_owner(limits);uint64_t baseline=ed_stats(&c).live_bytes;
    CHECK((operation?ed_restart_whole(&c):ed_whole(&c,ED_GO,false))==expected);XrCompileResourceStats s=ed_stats(&c);ed_release(&c,baseline);return s;
}
static void ed_limits(void){
    for(unsigned operation=0;operation<2;++operation){
    XrCompileResourceStats a=ed_measured(ed_caps(),XR_XIR_OK,operation);XrCompileResourceLimits exact={a.allocated_bytes,a.peak_bytes,a.work};
    XrCompileResourceStats b=ed_measured(exact,XR_XIR_OK,operation);
    CHECK(a.allocated_bytes==b.allocated_bytes && a.peak_bytes==b.peak_bytes && a.work==b.work);
    CHECK(exact.allocated_bytes>1 && exact.live_bytes>1 && exact.work>1);
    XrCompileResourceLimits less=exact;--less.allocated_bytes;(void)ed_measured(less,XR_XIR_BUDGET,operation);
    less=exact;--less.live_bytes;(void)ed_measured(less,XR_XIR_BUDGET,operation);less=exact;--less.work;(void)ed_measured(less,XR_XIR_BUDGET,operation);
    }
}
static void ed_occupied(void){
    ErrorDependencyFixture f;ed_fixture(&f,ED_GO,false);XrXirCompileContext c=ed_owner(ed_caps());uint64_t baseline=ed_stats(&c).live_bytes;
    XrXirEffects sentinel={0},*occupied=&sentinel;memset(&sentinel,0x5a,sizeof(sentinel));
    XrXirEffects before=sentinel;XrCompileResourceStats a=ed_stats(&c);
    CHECK(xr_xir_compile_effects_infer_verified(&c,&f.module,&occupied)==XR_XIR_BAD_STRUCTURE);
    XrCompileResourceStats b=ed_stats(&c);
    CHECK(occupied==&sentinel && !memcmp(&sentinel,&before,sizeof(before)) && a.work==b.work && a.allocated_bytes==b.allocated_bytes);
    ed_release(&c,baseline);
}
#endif // XIR_ERROR_DEPENDENCY_RESOURCES_H
